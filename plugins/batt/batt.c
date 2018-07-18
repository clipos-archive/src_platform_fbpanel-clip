// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright © 2007-2018 ANSSI. All Rights Reserved.
/*
 * ACPI battery monitor plugin for LXPanel
 *
 * Copyright (C) 2007 by Greg McNew <gmcnew@gmail.com>
 * Copyright (C) 2008 by Hong Jen Yee <pcman.tw@gmail.com>
 * Modified and backported to fbpanel : 
 * 	Copyright (C) 2008 SGDN 
 * 	(Author: Vincent Strubel <clipos@ssi.gouv.fr>)
 * 	Copyright (C) 2009 ANSSI
 * 	(Author: Florent Chabaud <clipos@ssi.gouv.fr>)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 *
 * This plugin monitors battery usage on ACPI-enabled systems by reading the
 * battery information found in /proc/acpi/battery/. The update interval is
 * user-configurable and defaults to 1 second.
 *
 * The battery's remaining life is estimated from its current charge and current
 * rate of discharge. The user may configure an alarm command to be run when
 * their estimated remaining battery life reaches a certain level.
 */

/* FIXME:
 *  Here are somethings need to be improvec:
 *  1. Replace pthread stuff with gthread counterparts for portability.
 *  4. Handle failure gracefully under systems other than Linux.
*/

#include <glib.h>
#include <glib/gi18n.h>
#include <pthread.h> /* used by pthread_create() and alarmThread */
#include <semaphore.h> /* used by update() and alarmProcess() for alarms */
#include <stdlib.h>
#include <string.h>

//#define DEBUGPRN
#include "dbg.h"
#include "panel.h" /* used to determine panel orientation */
#include "plugin.h"
#include "glib-mem.h" /* compatibility macros for g_slice* */

#define BATTERY_DIRECTORY "/proc/acpi/battery/" /* must be slash-terminated */
#define AC_ADAPTER_STATE_FILE "/proc/acpi/ac_adapter/AC/state"

/* The last MAX_SAMPLES samples are averaged when charge rates are evaluated.
   This helps prevent spikes in the "time left" values the user sees. */
#define MAX_SAMPLES 10

typedef struct {
    char* name;
    int capacity,   /* unit: mWh */
        charge,     /* unit: mWh */
        is_charging,
        present,   
        last_rate,   /* unit: mW */
        rate;       /* unit: mW */
} batt_info;

typedef struct {
    plugin_instance plugin;
    char *alarmCommand,
        *backgroundColor;
    GdkColor background;
    GdkGC *bg,
        *gc1,
        *gc2;
    GdkPixmap *pixmap;
    GtkWidget *drawingArea;
    int orientation;
    int alarmTime,
        border,
        height,
        length,
        numSamples,
        requestedBorder,
        *rateSamples,
        rateSamplesSum,
        thickness,
        timer,
        state_elapsed_time,
        info_elapsed_time,
        wasCharging,
        width,
        hide_if_no_battery;
    sem_t alarmProcessLock;
    GList* batteries;
    gboolean has_ac_adapter;
} batt;


typedef struct {
    char *command;
    sem_t *lock;
} alarm_t;

static void batt_destructor(plugin_instance *p);
static void update_display(batt *b, gboolean repaint);

static void batt_info_free( batt_info* bi )
{
    g_free( bi->name );
    g_slice_free( batt_info, bi );
}

static gboolean get_batt_info( batt_info* bi )
{
    FILE *info;
    char buf[ 256 ];

    /* Open the info file */
    g_snprintf(buf, 256, "%s%s/info", BATTERY_DIRECTORY, bi->name);
    if ((info = fopen(buf, "r"))) {
        /* Read the file until the battery's capacity is found or until
           there are no more lines to be read */
        while( fgets(buf, 256, info) &&
                ! sscanf(buf, "last full capacity: %d",
                &bi->capacity) );
        fclose(info);
        return TRUE;
    }
    return FALSE;
}

static gboolean get_batt_state( batt_info* bi )
{
    FILE *state;
    char buf[ 512 ];

    g_snprintf( buf, 512, "%s%s/state", BATTERY_DIRECTORY, bi->name );
    state = fopen( buf, "r");
    if (!state)
    	return FALSE;

    char *pstr;
    if (fread(buf, sizeof(buf), 1, state))
    	return FALSE;

    fclose(state);

    char thisState = 'c';

    if (!((pstr = strstr(buf, "present:")) && (*(pstr+25) == 'y'))) {
        /* battery is not present */
        bi->rate = 0;
        bi->charge = 0;
        bi->is_charging = 0;
        bi->present = 0;
        return TRUE;
    }

    bi->present = 1;
    /* Read the file until the battery's charging state is found or
       until there are no more lines to be read */
    if ((pstr = strstr(buf, "charging state:")))
        thisState = *(pstr + 25);

    /* Read the file until the battery's charge/discharge rate is
       found or until there are no more lines to be read */
    if ((pstr = strstr(buf, "present rate:"))) {
        pstr += 25;
        sscanf (pstr, "%d",&bi->rate );

        if( bi->rate < 0 )
            bi->rate = 0;
    }

    /* Read the file until the battery's charge is found or until
       there are no more lines to be read */
    if ((pstr = strstr (buf, "remaining capacity"))) {
        pstr += 25;
        sscanf (pstr, "%d",&bi->charge);
    }

    /* thisState will be 'c' if the batter is charging and 'd'
       otherwise */
    bi->is_charging = !( thisState - 'c' );

    return TRUE;
}

static gboolean check_ac_adapter( batt* b )
{
    FILE *state;
    char buf[ 256 ];
    char* pstr = NULL;

    if ((state = fopen( AC_ADAPTER_STATE_FILE, "r"))) {
        gboolean has_ac_adapter = FALSE;

        while( fgets(buf, 256, state) &&
                ! ( pstr = strstr(buf, "state:") ) );
        if( pstr )
        {
            pstr += 6;
            while( *pstr && *pstr == ' ' )
                ++pstr;
            if( pstr[0] == 'o' && pstr[1] == 'n' )
                has_ac_adapter = TRUE;
        }
        fclose(state);

        /* if the state of AC adapter changed, is_charging of the batteries might change, too. */
        if( has_ac_adapter != b->has_ac_adapter )
        {
            /* g_debug( "ac_state_changed: %d", has_ac_adapter ); */
            b->has_ac_adapter = has_ac_adapter;
            /* update the state of all batteries */
            g_list_foreach( b->batteries, (GFunc)get_batt_state, NULL );
            update_display( b, TRUE );
        }
        return TRUE;
    }
    return FALSE;
}

/* alarmProcess takes the address of a dynamically allocated alarm struct (which
   it must free). It ensures that alarm commands do not run concurrently. */
static void * alarmProcess(void *arg) {
    alarm_t *a = (alarm_t *) arg;

    sem_wait(a->lock);
    if (system(a->command))
    	ERR("Failed to run alarm command %s", a->command);
    sem_post(a->lock);

    g_free(a);
    return NULL;
}


/* addRate adds a charge/discharge rate to the array of samples and returns the
   average of all the rates in the array */
static int addRate(batt *b, int isCharging, int lastRate) {

    /* Clear the rate samples array if the charge/discharge status has just
       changed */
    if (b->wasCharging != isCharging) {
        b->wasCharging = isCharging;
        b->numSamples = b->rateSamplesSum = 0;
    }

    /* The rateSamples array acts as a circular array-based queue with a fixed
       size. If it is full, there's a meaningful value at index numSamples which
       should be subtracted from the sum before the new value is added; if it
       isn't full, the value at index numSamples does not need to be
       considered. */
    int currentIndex = b->numSamples % MAX_SAMPLES;
    if (b->numSamples >= MAX_SAMPLES)
        b->rateSamplesSum -= b->rateSamples[currentIndex];
    b->rateSamples[currentIndex] = lastRate;
    b->rateSamplesSum += lastRate;

    /* Increment numSamples, but don't let it get too big. As long as it's
       greater than MAX_SAMPLES, we'll know that the next sample will be
       replacing an older one. */
    if (++b->numSamples >= MAX_SAMPLES * 2)
        b->numSamples -= MAX_SAMPLES;

    RET(b->rateSamplesSum / MIN(b->numSamples, MAX_SAMPLES));

}

void update_display(batt *b, gboolean repaint) {
    GList* l;
    char tooltip[256];

    const gchar *icon[]={
      "battery-missing.png", /* 0 */
      "battery-low.png",
      "battery-caution.png",
      "battery-040.png",
      "battery-060.png",
      "battery-080.png",	/* 5 */
      "battery-100.png",
      "battery-charging-low.png",
      "battery-charging-caution.png",
      "battery-charging-040.png",
      "battery-charging-060.png",	/* 10 */
      "battery-charging-080.png",
      "battery-charging.png",
      "no-battery.png"
    };
#define LEN_FN 250
    gchar filename[LEN_FN],prevfn[LEN_FN];
    static int mem=0;
    
    if (! b->pixmap)
        return;

    int capacity = 0,   /* unit: mWh */
        charge = 0,     /* unit: mWh */
        isCharging = 0,
        lastRate = 0,   /* unit: mW */
        rate = 0;       /* unit: mW */

    if( b->batteries )
      {
        int percent, hours, mins;
        /* Calculate the total capacity, charge, and charge/discharge rate */
        for( l = b->batteries; l; l = l->next )
	  {
            batt_info* bi = (batt_info*)l->data;
            capacity += bi->capacity;
            charge += bi->charge;
            lastRate += bi->rate;
            if( bi->is_charging )
                isCharging = TRUE;
	  }

        /* Add the last rate to the array of recent samples and get the average
           rate */
        rate = addRate(b, isCharging, lastRate);
	
        /* Consider running the alarm command */
        if (! isCharging && rate && charge * 60 / rate <= b->alarmTime) {
	  
	  /* Alarms should not run concurrently; determine whether an alarm is
	     already running */
	  int alarmCanRun;
	  sem_getvalue(&(b->alarmProcessLock), &alarmCanRun);
	  
	  /* Run the alarm command if it isn't already running */
	  if (alarmCanRun) {
	    
	    alarm_t *a = (alarm_t *) malloc(sizeof(alarm_t));
	    a->command = b->alarmCommand;
	    a->lock = &(b->alarmProcessLock);
	    
	    /* Manage the alarm process in a new thread, which which will be
	       responsible for freeing the alarm struct it's given */
	    pthread_t alarmThread;
	    pthread_create(&alarmThread, NULL, alarmProcess, a);
	    
	  }
        }

        /* Make a tooltip string, and display remaining charge time if the battery
           is charging or remaining life if it's discharging */
        percent = capacity ? charge * 100 / capacity : 0;
        if (percent > 100)
	  percent = 100;
        if (isCharging) {
	  if (rate) {
	    hours = (capacity > charge) ? (capacity - charge) / rate : 0;
	    mins = (capacity > charge) ? 
	      (((capacity - charge) * 60 )/ rate) % 60 : 0;
	    snprintf(tooltip, 256,
		     _("Battery: %d%% charged, %d:%02d until full"),
		     percent, hours, mins);
	    
	  } else {
	    /* A battery will sometimes have a charge rate of 0, even if it isn't
	       finished charging */
	    snprintf(tooltip, 256,
		     _("Battery: %d%% charged, %s"),
		     percent,
		     (charge >= capacity) ? _("charging finished") : _("not charging") );
	  }
        } else {
	  snprintf(tooltip, 256,
		   _("Battery: %d%% charged, %d:%02d left"),
		   percent, rate ? charge / rate : 0,
		   rate ? (charge * 60 / rate) % 60 : 0);
        }


        gtk_widget_set_tooltip_text(b->plugin.pwid, tooltip);

        /* Choose the right colors for the charge bar */
        if (isCharging) {
	  if(percent >= 95) 
	    g_snprintf( filename, LEN_FN, IMGPREFIX "/%s",icon[12]);
	  else if(percent >= 75) 
	    g_snprintf( filename, LEN_FN, IMGPREFIX "/%s",icon[11]); 
	  else if(percent >= 50) 
	    g_snprintf( filename, LEN_FN, IMGPREFIX "/%s",icon[10]); 
	  else if(percent >= 25) 
	    g_snprintf( filename, LEN_FN, IMGPREFIX "/%s",icon[9]); 
	  else if(percent >= 15) 
	    g_snprintf( filename, LEN_FN, IMGPREFIX "/%s",icon[8]);
	  else
	    g_snprintf( filename, LEN_FN, IMGPREFIX "/%s",icon[7]);
        }
        else {			/* discharging */
	  if(percent >= 95) 
	    g_snprintf( filename, LEN_FN, IMGPREFIX "/%s",icon[6]);
	  else if(percent >= 75) 
	    g_snprintf( filename, LEN_FN, IMGPREFIX "/%s",icon[5]); 
	  else if(percent >= 50) 
	    g_snprintf( filename, LEN_FN, IMGPREFIX "/%s",icon[4]); 
	  else if(percent >= 25) 
	    g_snprintf( filename, LEN_FN, IMGPREFIX "/%s",icon[3]); 
	  else if(percent >= 15) 
	    g_snprintf( filename, LEN_FN, IMGPREFIX "/%s",icon[2]);
	  else if(percent >= 8) 
	    g_snprintf( filename, LEN_FN, IMGPREFIX "/%s",icon[1]);
	  else
	    g_snprintf( filename, LEN_FN, IMGPREFIX "/%s",icon[0]);
        }	
      }
    else    /* no battery is found */
      {
        g_snprintf( tooltip, 256, _("No batteries found") );
        gtk_widget_set_tooltip_text(b->drawingArea, tooltip);

	if (b->hide_if_no_battery)
	  g_snprintf( filename, LEN_FN, IMGPREFIX "/%s",icon[13]);        
	else
	  g_snprintf( filename, LEN_FN, IMGPREFIX "/%s",icon[0]);        
      }

    if((mem==0)||(strcmp(filename,prevfn)!=0))
    {
      GError *p_gerror=NULL;
      GdkPixbuf *p_pixbuf;

      if(!(p_pixbuf=gdk_pixbuf_new_from_file_at_size(filename,b->width, b->height, &p_gerror))) {

        g_snprintf( tooltip, 256, _("Failed to load file: %s\n"), p_gerror->message);
        gtk_widget_set_tooltip_text(b->drawingArea, tooltip);
	g_error_free(p_gerror);
	p_gerror=NULL;
      }
      gdk_draw_rectangle(b->pixmap, b->bg, TRUE, 0, 0, b->width, b->height);	
      gdk_draw_pixbuf(b->pixmap,b->gc1, p_pixbuf, 0, 0, b->border, b->border, -1, -1, GDK_RGB_DITHER_MAX, 0, 0);
      g_object_unref(p_pixbuf);
      if( repaint ) {
	gtk_widget_queue_draw( b->drawingArea );
	mem=1;
	g_snprintf(prevfn, LEN_FN, "%s",filename);
      }
    }
}

static void check_batteries( batt* b )
{
    GDir *batteryDirectory;
    const char *battery_name;
    GList* l;
    gboolean need_update_display = FALSE;

    if (! (batteryDirectory = g_dir_open(BATTERY_DIRECTORY, 0, NULL)))
    {
        g_list_foreach( b->batteries, (GFunc)batt_info_free, NULL );
        g_list_free( b->batteries );
        b->batteries = NULL;
        return;
    }

    /* Remove dead entries */
    for( l = b->batteries; l; )
    {
        GList* next = l->next;
        batt_info* bi = (batt_info*)l->data;
        char* path;
        path = g_build_filename( BATTERY_DIRECTORY, bi->name, NULL );
        if( ! g_file_test( path, G_FILE_TEST_EXISTS ) ) /* file no more exists */
        {
            b->batteries = g_list_remove_link( b->batteries, l );   /* remove from the list */
            need_update_display = TRUE;
        }
        g_free( path );
        l = next;
    }

    /* Scan the battery directory for available batteries */
    while ((battery_name = g_dir_read_name(batteryDirectory))) {
        if (battery_name[0] != '.') {
            /* find the battery in our list */
            for( l = b->batteries; l; l = l->next )
            {
                batt_info* bi = (batt_info*)l->data;
                if( 0 == strcmp( bi->name, battery_name ) )
                    break;
            }
            if( ! l ) /* not found, this is a new battery */
            {
                batt_info* bi = g_slice_new0( batt_info );
                bi->name = g_strdup( battery_name );
                /* get battery info & state for the newly added entry */
                get_batt_info( bi );
                get_batt_state( bi );
                if (bi->present) {
                    b->batteries = g_list_prepend( b->batteries, bi );  /* add to our list */
                    need_update_display = TRUE;
                }
            }
        }
    }
    g_dir_close(batteryDirectory);

    if( need_update_display )
        update_display( b, TRUE );
}

/* This callback is called every 3 seconds */
static int update_timout(batt *b) {
    GDK_THREADS_ENTER();

    ++b->state_elapsed_time;
    ++b->info_elapsed_time;


    /* check the existance of batteries every 3 seconds */
    check_batteries( b );

    /* check the existance of AC adapter every 3 seconds,
     * and update charging state of batteries if needed. */
    check_ac_adapter( b );

    /* check state of batteries every 30 seconds */
    if( b->state_elapsed_time == 30/3 )  /* 30 sec */
    {
        /* update state of batteries */
        g_list_foreach( b->batteries, (GFunc)get_batt_state, NULL );
        b->state_elapsed_time = 0;
    }
    /* check the capacity of batteries every 1 hour */
    if( b->info_elapsed_time == 3600/3 )  /* 1 hour */
    {
        /* update info of batteries */
        g_list_foreach( b->batteries, (GFunc)get_batt_info, NULL );
        b->info_elapsed_time = 0;
    }

    update_display( b, TRUE );

    GDK_THREADS_LEAVE();
    return TRUE;
}

/* An update will be performed whenever the user clicks on the charge bar */
static gint buttonPressEvent(GtkWidget *widget, GdkEventButton *event,
        plugin_instance* plugin) {

    batt *b = (batt*)plugin;

    update_display(b, TRUE);

    return FALSE;
}


static gint configure_event(GtkWidget *widget, GdkEventConfigure *event,
        batt *b) {

    ENTER;

    if (b->pixmap)
        g_object_unref(b->pixmap);

    /* Update the plugin's dimensions */
    b->width = widget->allocation.width;
    b->height = widget->allocation.height;
    if (b->orientation != ORIENT_HORIZ) {
        b->length = b->height;
        b->thickness = b->width;
    }
    else {
        b->length = b->width;
        b->thickness = b->height;
    }

    b->pixmap = gdk_pixmap_new (widget->window, widget->allocation.width,
          widget->allocation.height, -1);

    /* Perform an update so the bar will look right in its new orientation */
    update_display(b, FALSE);

    RET(TRUE);

}


static gint expose_event(GtkWidget *widget, GdkEventExpose *event, batt *b) {

    ENTER;

    gdk_draw_drawable (widget->window, b->drawingArea->style->black_gc,
            b->pixmap, event->area.x, event->area.y, event->area.x,
            event->area.y, event->area.width, event->area.height);

    RET(FALSE);

}


static int
batt_constructor(plugin_instance *p)
{
    ENTER;

    batt *b = (batt *)p;
    gtk_container_set_border_width( GTK_CONTAINER(p->pwid), 1 );

    b->drawingArea = gtk_drawing_area_new();
    gtk_widget_add_events( b->drawingArea, GDK_EXPOSURE_MASK|GDK_BUTTON_PRESS_MASK );

    gtk_container_add( (GtkContainer*)p->pwid, b->drawingArea );

    if ((b->orientation = p->panel->orientation) == ORIENT_HORIZ) {
        b->height = b->thickness = 20;
        b->length = b->width = 8;
    }
    else {
        b->height = b->length = 8;
        b->thickness = b->width = 20;
    }
    gtk_widget_set_size_request(b->drawingArea, b->width, b->height);

    b->bg = gdk_gc_new(p->panel->topgwin->window);
    b->gc1 = gdk_gc_new(p->panel->topgwin->window);
    b->gc2 = gdk_gc_new(p->panel->topgwin->window);

    g_signal_connect (G_OBJECT (b->drawingArea), "button_press_event",
            G_CALLBACK(buttonPressEvent), (gpointer) p);
    g_signal_connect (G_OBJECT (b->drawingArea),"configure_event",
          G_CALLBACK (configure_event), (gpointer) b);
    g_signal_connect (G_OBJECT (b->drawingArea), "expose_event",
          G_CALLBACK (expose_event), (gpointer) b);
    gtk_widget_show_all(b->drawingArea);

    sem_init(&(b->alarmProcessLock), 0, 1);

    b->alarmCommand = b->backgroundColor = NULL;

    /* Set default values for integers */
    b->alarmTime = 10;
    b->requestedBorder = 1;
    b->numSamples = b->rateSamplesSum = b->wasCharging = 0;

    b->rateSamples = malloc(sizeof(int) * MAX_SAMPLES);

    XCG(p->xc, "HideIfNoBattery", &b->hide_if_no_battery, int);
    XCG(p->xc, "AlarmCommand", &b->alarmCommand, str);
    XCG(p->xc, "BackgroundColor", &b->backgroundColor, str);
    XCG(p->xc, "AlarmTime", &b->alarmTime, int);
    XCG(p->xc, "BorderWidth", &b->requestedBorder, int);
    XCG(p->xc, "Size", &b->thickness, int);

    if (b->orientation == ORIENT_HORIZ)
	b->width = b->thickness;
    else
	b->height = b->thickness;
    gtk_widget_set_size_request(b->drawingArea, b->width, b->height);

    /* Make sure the border value is acceptable */
    b->border = MIN(MAX(0, b->requestedBorder),
            (MIN(b->length, b->thickness) - 1) / 2);

    /* Apply more default options */
    if (!b->backgroundColor)
        b->backgroundColor = "#ebebeb";

    gdk_color_parse(b->backgroundColor, &b->background);
    gdk_colormap_alloc_color(gdk_drawable_get_colormap(
            p->panel->topgwin->window), &b->background, FALSE, TRUE);
    gdk_gc_set_foreground(b->bg, &b->background);

    check_batteries( b );   /* get available batteries */

    /* Start the update loop */
#if GTK_CHECK_VERSION( 2, 14, 0 )
    b->timer = g_timeout_add_seconds( 3, (GSourceFunc) update_timout, (gpointer) b);
#else
    b->timer = g_timeout_add( 3000,
            (GSourceFunc) update_timout, (gpointer) b);
#endif
    RET(TRUE);
}


static void
batt_destructor(plugin_instance *p)
{
    ENTER;

    batt *b = (batt *) p;

    if (b->pixmap)
        g_object_unref(b->pixmap);

    g_object_unref(b->gc1);
    g_object_unref(b->gc2);

    g_free(b->rateSamples);
    sem_destroy(&(b->alarmProcessLock));
    g_source_remove(b->timer);

    RET();

}

static plugin_class class = {
    .fname	= NULL,
    .count	= 0,

    .type	= "batt",
    .name	= "Battery Monitor",
    .version 	= "1.1",
    .description = "Display battery status using ACPI",
    .priv_size	= sizeof(batt),

    .constructor = batt_constructor,
    .destructor  = batt_destructor,
};

static plugin_class *class_ptr = (plugin_class *) &class;
