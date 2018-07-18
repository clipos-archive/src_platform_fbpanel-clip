// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright © 2007-2018 ANSSI. All Rights Reserved.
/**
 * Minimizer / maximizer plugin for fbpanel
 * Somewhat based on the launchbar plugin.
 *
 * Modified : 
 * 	Copyright (C) 2010 ANSSI
 * 	(Author: Vincent Strubel <clipos@ssi.gouv.fr>)
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License version
 *  2 as published by the Free Software Foundation.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>


#include <gdk-pixbuf/gdk-pixbuf.h>

#include "panel.h"
#include "misc.h"
#include "plugin.h"
#include "gtkbgbox.h"

//#define DEBUGPRN
#include "dbg.h"

typedef struct {
    plugin_instance plugin;
    GtkWidget *box;
    GtkWidget *button;
    GdkPixbuf *pixmin;
    GdkPixbuf *pixmin_l;
    GdkPixbuf *pixmax;
    GdkPixbuf *pixmax_l;
    gchar *tipmin;
    gchar *tipmax;
    int maxed;
    int size;
    panel *p;
} minmax;

static GdkPixbuf *
get_light_pixmap(GdkPixbuf *dark, gulong hicolor)
{
    GdkPixbuf *light;
    guchar *src, *up, extra[3];
    int i;    
    
    ENTER;
    light = gdk_pixbuf_add_alpha(dark, FALSE, 0, 0, 0);
    if (!light)
        RET(NULL);
    src = gdk_pixbuf_get_pixels (light);
    for (i = 2; i >= 0; i--, hicolor >>= 8)
        extra[i] = hicolor & 0xFF;
    for (up = src + gdk_pixbuf_get_height(light) * gdk_pixbuf_get_rowstride (light);
         src < up; src+=4) {
        if (src[3] == 0)
            continue;
        for (i = 0; i < 3; i++) {
            if (src[i] + extra[i] >= 255)
                src[i] = 255;
            else
                src[i] += extra[i];
        }
    }
    RET(light);
}

static gboolean
my_button_pressed(GtkWidget *widget, GdkEventButton *event, minmax *m )
{
    GtkWidget *image;
    gboolean ret = TRUE;

    ENTER;
    image = gtk_bin_get_child(GTK_BIN(widget));
    g_assert(m != NULL);
    if (event->type == GDK_BUTTON_RELEASE) {
        if ((event->x >=0 && event->x < widget->allocation.width)
              && (event->y >=0 && event->y < widget->allocation.height)) {
            
            if (!m->maxed) {
                panel_maximize(m->p);
    	        m->maxed = 1;
                gtk_image_set_from_pixbuf(GTK_IMAGE(image), m->pixmax_l);
                if (m->tipmax) 
		    gtk_widget_set_tooltip_text(m->button, m->tipmax);
            } else {
                panel_minimize(m->p, m->size + 2);
                m->maxed = 0;
                gtk_image_set_from_pixbuf(GTK_IMAGE(image), m->pixmin_l);
                if (m->tipmin) 
		    gtk_widget_set_tooltip_text(m->button, m->tipmin);
            }
        }
        gtk_misc_set_padding (GTK_MISC(image), 0, 0);
        
    } else if (event->type == GDK_BUTTON_PRESS) {
        gtk_misc_set_padding (GTK_MISC(image), 0, 3);
    }
    RET(ret);
}

static gboolean
my_button_enter(GtkWidget *widget, GdkEventCrossing *event, minmax *m)
{
    GtkWidget *image = gtk_bin_get_child(GTK_BIN(widget));

    ENTER;
    if (m->maxed) {
        gtk_image_set_from_pixbuf(GTK_IMAGE(image), m->pixmax_l);
    } else {
        gtk_image_set_from_pixbuf(GTK_IMAGE(image), m->pixmin_l);
    }

    RET(TRUE);
}

static gboolean
my_button_leave(GtkWidget *widget, GdkEventCrossing *event, minmax *m)
{
    GtkWidget *image = gtk_bin_get_child(GTK_BIN(widget));

    ENTER;
    if (m->maxed) {
        gtk_image_set_from_pixbuf(GTK_IMAGE(image), m->pixmax);
    } else {
        gtk_image_set_from_pixbuf(GTK_IMAGE(image), m->pixmin);
    }

    RET(TRUE);
}

static void
minmax_destructor(plugin_instance *p)
{
    minmax *m = (minmax *)p;

    ENTER;
    gtk_widget_destroy(m->box);
    RET();
}

static GtkWidget *
button_new(gchar *fname, int width, int height)
{
    GtkWidget *b, *image;

    ENTER;
    DBG("fname = %s\n", fname);
    b = gtk_bgbox_new();
    gtk_container_set_border_width(GTK_CONTAINER(b), 0);
    GTK_WIDGET_UNSET_FLAGS (b, GTK_CAN_FOCUS);
    image = fb_image_new(NULL, fname, width, height);
    if (!image)
        image = gtk_image_new_from_stock(GTK_STOCK_MISSING_IMAGE, GTK_ICON_SIZE_BUTTON);
    gtk_container_add(GTK_CONTAINER(b), image);
    gtk_misc_set_alignment(GTK_MISC(image), 0, 1);
    gtk_misc_set_padding (GTK_MISC(image), 0, 0);
    gtk_widget_show(image);
    gtk_widget_show(b);
    RET(b);
}

static int
read_minmax(plugin_instance *p)
{
    minmax *m = (minmax *)p;
    gchar *fmax = NULL, *fmin = NULL;
    GtkWidget *button;
    
    ENTER;
    XCG(p->xc, "imageMax", &fmax, str);
    XCG(p->xc, "imageMin", &fmin, str);
    XCG(p->xc, "tipMax", &m->tipmax, str);
    XCG(p->xc, "tipMin", &m->tipmin, str);
    XCG(p->xc, "initiallyMaxed", &m->maxed, enum, bool_enum);

    if (!fmax || !fmin) {
    	ERR("minmax: missing parameters\n");
        goto error;
    }
    m->pixmax = gdk_pixbuf_new_from_file_at_size(fmax, m->size, m->size, NULL);
    if (!m->pixmax) {
        ERR("mimmax: failed to create pixmax\n");
        goto error;
    }
    m->pixmax_l = get_light_pixmap(m->pixmax, 0x202020);
    if (!m->pixmax_l) {
        ERR("mimmax: failed to create pixmax_l\n");
        goto error;
    }
    m->pixmin = gdk_pixbuf_new_from_file_at_size(fmin, m->size, m->size, NULL);
    if (!m->pixmin) {
        ERR("mimmax: failed to create pixmin\n");
        goto error;
    }
    m->pixmin_l = get_light_pixmap(m->pixmin, 0x202020);
    if (!m->pixmin_l) {
        ERR("mimmax: failed to create pixmin_l\n");
        goto error;
    }

    // button
    if (m->maxed)
        button = button_new(fmax, m->size, m->size);
    else
        button = button_new(fmin, m->size, m->size);
    g_signal_connect (G_OBJECT (button), "button-release-event",
          G_CALLBACK (my_button_pressed), (gpointer) m);
    g_signal_connect (G_OBJECT (button), "enter-notify-event",
          G_CALLBACK (my_button_enter), (gpointer) m);
    g_signal_connect (G_OBJECT (button), "leave-notify-event",
          G_CALLBACK (my_button_leave), (gpointer) m);
    g_signal_connect (G_OBJECT (button), "button-press-event",
          G_CALLBACK (my_button_pressed), (gpointer) m);

    DBG("here\n");
    GTK_WIDGET_UNSET_FLAGS (button, GTK_CAN_FOCUS);
    DBG("here\n");

    gtk_box_pack_start(GTK_BOX(m->box), button, FALSE, FALSE, 0);
    m->button = button;
    gtk_widget_show(button);
    
        if (p->panel->transparent) 
        gtk_bgbox_set_background(button, BG_ROOT, p->panel->tintcolor, p->panel->alpha);

    //g_free(fmax);
    //g_free(fmin);
    // tooltip
    if (m->maxed && m->tipmax) 
        gtk_widget_set_tooltip_text(button, m->tipmax);
    else if (!m->maxed && m->tipmin)
        gtk_widget_set_tooltip_text(button, m->tipmin);

    m->p = p->panel;

    if (!m->maxed)
        panel_minimize(m->p, m->size + 2);
    
    RET(1);

 error:
    if (fmax)
        g_free(fmax);
    if (fmin)
        g_free(fmin);
    RET(0);
}

static int
minmax_constructor(plugin_instance *p)
{
    minmax *m = (minmax *)p; 
    static gchar *minmax_rc = "style 'minmax-style'\n"
        "{\n"
        "GtkWidget::focus-line-width = 0\n"
        "GtkWidget::focus-padding = 0\n"
        "GtkButton::default-border = { 0, 0, 0, 0 }\n"
        "GtkButton::default-outside-border = { 0, 0, 0, 0 }\n"
        "}\n"
        "widget '*' style 'minmax-style'";
   
    ENTER;
    gtk_widget_set_name(p->pwid, "minmax");
    gtk_rc_parse_string(minmax_rc);
    
    m->box = p->panel->my_box_new(FALSE, 1);
    gtk_container_add(GTK_CONTAINER(p->pwid), m->box);
    gtk_container_set_border_width (GTK_CONTAINER (m->box), 0);
    gtk_widget_show(m->box);
    
    if  (p->panel->orientation == ORIENT_HORIZ) 
        m->size = GTK_WIDGET(p->panel->box)->allocation.height;
    else
        m->size = GTK_WIDGET(p->panel->box)->allocation.width;
    DBG("size=%d\n", m->size);

    m->maxed = 1;

    if (!read_minmax(p))
        goto error;

    RET(1);

error:
    minmax_destructor(p);
    RET(0);
    
}



static plugin_class class = {
    .fname	= NULL,
    .count	= 0,

    .type 	= "minmax",
    .name 	= "minmax",
    .version	= "1.0",
    .description = "Button to minimize / maximize the panel",
    .priv_size	= sizeof(minmax),

    .constructor = minmax_constructor,
    .destructor  = minmax_destructor,
};

static plugin_class *class_ptr = (plugin_class *) &class;
