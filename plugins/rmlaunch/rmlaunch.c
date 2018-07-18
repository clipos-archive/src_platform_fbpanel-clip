// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright © 2007-2018 ANSSI. All Rights Reserved.
/**
 * RM jail launcher plugin for fbpanel
 * Based on the launchbar plugin.
 *
 * Modified : 
 * 	Copyright (C) 2008 SGDN 
 * 	(Author: Vincent Strubel <clipos@ssi.gouv.fr>)
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

typedef struct rmlaunch {
    plugin_instance plugin;
    GtkWidget *box;
    int iconsize;

    gchar *action;
    GPid pid;

    panel *p;
    int timer;
} rmlaunch;


static void 
child_reaper(GPid pid, gint status, gpointer data)
{
    rmlaunch *rl = data;

    ENTER;
    if (pid == rl->pid) {
        rl->pid = 0;
    } else {
        ERR("rmlaunch child reaper error: %d != %d\n", pid, rl->pid);
    }
    RET();
}

#define BEGIN_CHILD_PROTECT() do {\
  sigemptyset(&set); \
  sigaddset(&set, SIGCHLD); \
  sigprocmask(SIG_BLOCK, &set, &oldset); \
} while (0)

#define END_CHILD_PROTECT() do {\
  sigemptyset(&set); \
  sigaddset(&set, SIGCHLD); \
  sigprocmask(SIG_UNBLOCK, &set, &oldset); \
} while (0)

static gboolean
my_spawn_action(rmlaunch *rl)
{
    GPid pid;
    sigset_t set, oldset;
    gboolean ret = FALSE;
    GError *err = NULL;

    char *my_argv[] = {
        rl->action,
        NULL,
    };

    ENTER;
    BEGIN_CHILD_PROTECT();
    if (rl->pid)
        goto out;

    ret = g_spawn_async(NULL, my_argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD,
                NULL, NULL, &pid, &err);
    if (ret == TRUE) {
        rl->pid = pid;
        (void)g_child_watch_add(pid, child_reaper, rl);
    } else {
        if (err != NULL) 
            ERR("Spawn error: %s\n", err->message);
        else
            ERR("Spawn error, no message\n");
    }

out:
    END_CHILD_PROTECT();
    RET(ret);
}

static gboolean
my_button_pressed(GtkWidget *widget, GdkEventButton *event, rmlaunch *rl )
{
    GtkWidget *image;
    gboolean ret = TRUE;

    ENTER;
    image = gtk_bin_get_child(GTK_BIN(widget));
    g_assert(rl != NULL);
    if (event->type == GDK_BUTTON_RELEASE) {
        if ((event->x >=0 && event->x < widget->allocation.width)
              && (event->y >=0 && event->y < widget->allocation.height)) {
            
            ret = my_spawn_action(rl);
        }
        gtk_misc_set_padding (GTK_MISC(image), 0, 0);
        
    } else if (event->type == GDK_BUTTON_PRESS) {
      
        gtk_misc_set_padding (GTK_MISC(image), 0, 3);
    }
    RET(ret);
}

static void
rmlaunch_destructor(plugin_instance *p)
{
    rmlaunch *rl = (rmlaunch *)p;

    ENTER;
    if (rl->box)
        gtk_widget_destroy(rl->box);
    g_free(rl->action);     
    RET();
}

static int
init_widget(plugin_instance *p, gchar *fname, gchar *tooltip)
{
    rmlaunch *rl = (rmlaunch *)p;
    GtkWidget *button;
    int w, h;

    // button
    ENTER;
    if (p->panel->orientation == ORIENT_HORIZ) {
        w = -1;
        h = p->panel->ah;
    } else {
        w = p->panel->aw;
        h = -1;
    }
    w = h = rl->iconsize;

    rl->box = p->panel->my_box_new(FALSE, 0);

    button = fb_button_new(NULL, fname, w, h, 0x202020, NULL);
    g_signal_connect (G_OBJECT (button), "button-release-event",
          G_CALLBACK (my_button_pressed), (gpointer) rl);
    g_signal_connect (G_OBJECT (button), "button-press-event",
          G_CALLBACK (my_button_pressed), (gpointer) rl);

    DBG("here\n");
    
    GTK_WIDGET_UNSET_FLAGS (button, GTK_CAN_FOCUS);

    DBG("here\n");

    gtk_box_pack_start(GTK_BOX(rl->box), button, FALSE, FALSE, 0);
    gtk_widget_show(button);

    if (p->panel->transparent) 
        gtk_bgbox_set_background(button, BG_ROOT, p->panel->tintcolor, p->panel->alpha);

    if (tooltip)
        gtk_widget_set_tooltip_text(button, tooltip);

    gtk_container_add(GTK_CONTAINER(p->pwid), rl->box);
    gtk_container_set_border_width (GTK_CONTAINER (rl->box), 0);
    gtk_widget_show(rl->box);

    RET(1);
}

static int
read_rmlaunch(plugin_instance *p)
{
    rmlaunch *rl = (rmlaunch *)p;
    gchar *tooltip, *fname, *action;
    int ret;
    
    ENTER;
    tooltip = fname = action = NULL;

    XCG(p->xc, "image", &fname, str);
    XCG(p->xc, "tooltip", &tooltip, str);
    XCG(p->xc, "action", &action, str);

    if (!action) {
        ERR("rmlaunch: missing action\n");
        goto error;
    }
    rl->action = expand_tilda(action);
    DBG("action=%s\n", rl->action);
    fname = expand_tilda(fname);
    rl->pid = 0;

    ret = init_widget(p, fname, tooltip);

    if (fname)
        g_free(fname);
 
    RET(ret);

 error:
    if (fname)
        g_free(fname);
    if (tooltip)
        g_free(tooltip);
    if (rl->action) {
        g_free(rl->action);
        rl->action = NULL;
    }
    RET(ret);
}

static int
rmlaunch_constructor(plugin_instance *p)
{
    rmlaunch *rl = (rmlaunch *)p; 
    static gchar *rmlaunch_rc = "style 'rmlaunch-style'\n"
        "{\n"
        "GtkWidget::focus-line-width = 0\n"
        "GtkWidget::focus-padding = 0\n"
        "GtkButton::default-border = { 0, 0, 0, 0 }\n"
        "GtkButton::default-outside-border = { 0, 0, 0, 0 }\n"
        "}\n"
        "widget '*' style 'rmlaunch-style'";
   
    ENTER;
    gtk_widget_set_name(p->pwid, "rmlaunch");
    gtk_rc_parse_string(rmlaunch_rc);
    
    
    if  (p->panel->orientation == ORIENT_HORIZ) 
        rl->iconsize = GTK_WIDGET(p->panel->box)->allocation.height;
    else
        rl->iconsize = GTK_WIDGET(p->panel->box)->allocation.width;
    DBG("iconsize=%d\n", rl->iconsize);

    rl->box = NULL;
    rl->p = p->panel;

    if (!read_rmlaunch(p)) {
        ERR( "rmlaunch: failed to parse config\n");
        goto error;
    }

    RET(1);

error:
    RET(0);
}



static plugin_class class = {
    .count	= 0,
    .type 	= "rmlaunch",
    .name 	= "rmlaunch",
    .version	= "1.1",
    .description = "Single button widget to launch an application",
    .priv_size 	= sizeof(rmlaunch),

    .constructor = rmlaunch_constructor,
    .destructor  = rmlaunch_destructor,
};

static plugin_class *class_ptr = (plugin_class *) &class;
