// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright © 2007-2018 ANSSI. All Rights Reserved.
/*
 * Label plugin for fbpanel
 *
 * Modified : 
 * 	Copyright (C) 2008 SGDN 
 * 	(Author: Vincent Strubel <clipos@ssi.gouv.fr>)
 * 	Copyright (C) 2009 ANSSI
 * 	(Author: Florent Chabaud <clipos@ssi.gouv.fr>)
 *

First version of CLIP label configuration
== == == == == == == == == == == == == == == == == == == == =
Plugin {
 type = label
 config {
	file = [the file with the label content]
	trigger = [the file, the presence of which makes the label modified in 
			red !label!]
	action = [the program to launch upon mouse click]
 }
}

Second version of CLIP label configuration
== == == == == == == == == == == == == == == == == == == == == 
Ascendant compatibility (a.c) with first version is guaranteed.
Plugin {
 type = label
 config {
	file = [the file with the label content. May be absent (see below).]
	trigger = [the file, the presence of which raises the flag. Content is 
		added to the label.]
	action = [the program to launch upon mouse click]
	timer = [the refreshment period in seconds. If negative, the refreshment 
		is stopped upon first event (for a.c). Defaults to -300 (for a.c)]
	noflag = [the pixmap file to display when trigger is not present. If 
		present, the label is displayed in tip upon mouse flight over.]
	flag = [the pixmap file to display when trigger is present. Ignored if 
		noflag isn't specified.]
 }

Typical configurations
----------------------
1./ New core version of CLIP availability with nice icons.

Plugin {
 type = label
 config {
	file = /etc/shared/clip-release
	trigger = /usr/local/var/core_avail
	action = /usr/local/bin/helpviewer "/etc/shared/clip-release.html" 
		"CLIP - Derniers changements"
	noflag = /usr/local/share/fbpanel/images/security-high.png
	flag = /usr/local/share/fbpanel/images/security-medium.png
 }
}

2./ USB devices presence
file begins with "type: none" (see below)
Plugin {
 type = label
 config {
	file = /usr/local/var/capacities_avail # the file with mounted devices 
		and their capacities
	trigger = /usr/local/var/usb_avail # the file with mounted usb devices 
		and their capacities
	action = /usr/local/bin/usbclt umount
	noflag = /usr/local/share/fbpanel/images/drive-removable-media-usb.png
	flag = /usr/local/share/fbpanel/images/usb_umount.png
 }
}

3./ Network presence
Plugin {
 type = label
 config {
	file = /usr/local/var/net_avail # see below
	trigger = /usr/local/var/netlevel_avail # the file with network 
		informations
	action = ssh -X -Y -p 22 _admin@127.0.0.1 /usr/local/bin/clip-config -n
	noflag = /usr/local/share/fbpanel/images/network-disconnect.png
 }
}

If file begins with a type, the messages regarding rebooting are omitted.
First line is omitted. Remaining of the file is the label, e.g:
type: none
No USB device mounted

If file begins with network type, the icons are predefined when flag is raised.
type: wired/wifi/umts
No network found

The trigger file should begin with a level indication :
level: N
UMTS F-Orange

In both case first line is omitted. Remaining of the file is the label.

wired :
level: 0 (/usr/local/share/fbpanel/images/network-disconnect.png)
level: 1 (/usr/local/share/fbpanel/images/network-wired.png)

wifi :
level: 0 (/usr/local/share/fbpanel/images/network-wireless-0.png)
level: 1 (/usr/local/share/fbpanel/images/network-wireless-1.png)
level: 2 (/usr/local/share/fbpanel/images/network-wireless-2.png)
level: 3 (/usr/local/share/fbpanel/images/network-wireless-3.png)
level: X (/usr/local/share/fbpanel/images/network-wireless.png)

umts :
level: 0 (/usr/local/share/fbpanel/images/network-umts-0.png)
level: 1 (/usr/local/share/fbpanel/images/network-umts-1.png)
level: 2 (/usr/local/share/fbpanel/images/network-umts-2.png)
level: 3 (/usr/local/share/fbpanel/images/network-umts-3.png)
level: 4 (/usr/local/share/fbpanel/images/network-umts-4.png)
level: X (/usr/local/share/fbpanel/images/network-umts.png)
*/

// Reused deskno

#define _GNU_SOURCE
#include <glib.h>
#include <glib/gi18n.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <sys/file.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>

#include "panel.h"
#include "misc.h"
#include "plugin.h"

//#define DEBUGPRN
#include "dbg.h"

#define MAX_LEVEL 6

#define ERR_ERRNO(msg, args...) ERR(msg": %s\n", ##args, strerror(errno))

typedef enum {
	MonitorUndefined = -1,
	MonitorUpdate = 0,
	MonitorNet = 1,
	MonitorUSB = 2,
} monitor_t;

typedef enum {
	TypeUndefined = -1,
	TypeWired = 0,
	TypeWifi = 1,
	TypeUmts = 2,
} type_t;

static inline monitor_t
get_type_from_string(const char *str)
{
	if (!strcmp(str, "update"))
		return MonitorUpdate;
	if (!strcmp(str, "net"))
		return MonitorNet;
	if (!strcmp(str, "usb"))
		return MonitorUSB;
	return MonitorUndefined;
}

static inline type_t
get_subtype_from_string(const char *str)
{
	DBG("check %s\n", str);
	if (!strcmp(str, "wired"))
		return TypeWired;
	if (!strcmp(str, "wifi"))
		return TypeWifi;
	if (!strcmp(str, "umts"))
		return TypeUmts;
	return TypeUndefined;
}

static inline const char *
subtype_to_string(type_t type)
{
	switch (type) {
		case TypeWired:
			return _("Wired connection: ");
		case TypeWifi:
			return _("Wireless connection: ");
		case TypeUmts:
			return _("UMTS connection: ");
		default:
			return _("Unknown type: ");
	}
}

typedef struct {
	int level;
	char *profile;
	char *ipsec;
	char *label;
	char *complement;
	type_t type;
} labelfile;

#define INIT_LABELFILE { \
	.level = 0, \
	.profile = NULL, \
	.ipsec = NULL, \
	.label = NULL, \
	.complement = NULL, \
	.type = MonitorUndefined \
	}

static inline void
freefile(labelfile *lf)
{
	ENTER;
	if (lf->profile) 
		g_free(lf->profile);
	lf->profile = NULL;
	if (lf->ipsec) 
		g_free(lf->ipsec);
	lf->ipsec = NULL;
	if (lf->label) 
		g_free(lf->label);
	lf->label = NULL;
	if (lf->complement) 
		g_free(lf->complement);
	lf->complement = NULL;
	RET();
}

typedef struct label {
	plugin_instance plugin;

	/* widget internals */
	GtkWidget *main;
	GtkWidget *labelw;
	GdkPixmap *pixmap;
	GdkColor background;
	GdkGC *bg;
	int height, width;

	/* config */
	monitor_t type;
	gchar *flag[MAX_LEVEL];
	gchar *flagold;
	gchar *noflag;
	gchar *file, *trigger;
	gchar **action;

	int inittimer, timer;
	int wd;
	GPid pid;

	/* methods */
	gboolean (*readfile)(labelfile *, const char *);
	gboolean (*set_pixmaps)(struct label *, const labelfile *lf);
	gboolean (*update)(struct label *);
	gboolean (*tooltip)(const labelfile *, char **);
} label;



/*************************************************************/
/*                       Parsing                             */
/*************************************************************/

static inline char *
get_chomped_line(FILE *filp)
{
	char *str = NULL, *ptr;
	size_t n;

	if (getline(&str, &n, filp) == -1)
		return NULL;
	
	ptr = strrchr(str, '\n');
	/* Remove trailing newline */
	if (ptr && !*(ptr + 1))
		*ptr = '\0';

	return str;
}

static inline FILE *
open_file(const char *path, int *fd)
{
	int f;
	FILE *filp;

	f = open(path, O_RDONLY|O_NOFOLLOW);
	if (f < 0) {
		ERR_ERRNO("failed to open file %s", path);
		return NULL;
	}

	if (flock(f, LOCK_SH)) {
		ERR_ERRNO("failed to lock file %s", path);
		(void)close(f);
		return NULL;
	}

	filp = fdopen(f, "r");
	if (!filp) {
		ERR_ERRNO("failed to fdopen %s", path);
		(void)close(f);
		return NULL;
	}
	*fd = f;
	return filp;
}

static inline gboolean
close_file(const char *path, int fd)
{
	if (flock(fd, LOCK_UN)) {
		ERR_ERRNO("failed to unlock file %s", path);
		return FALSE;
	}
	if (close(fd)) {
		ERR_ERRNO("failed to close file %s", path);
		return FALSE;
	}
	return TRUE;
}

static gboolean
read_file_plain(labelfile *lf, const char *path)
{
	FILE *filp;
	int fd;

	if (!lf) {
		ERR("labelfile is a NULL pointer\n");
		return FALSE;
	}
	
	usleep(50000U); 

	filp = open_file(path, &fd);
	if (!filp)
		return FALSE;

	DBG("opened %s\n", path);
	
	lf->label = get_chomped_line(filp);
	DBG("read %s\n", lf->label);
	(void)close_file(path, fd); /* XXX error checking ? */

	return (lf->label) ? TRUE : FALSE;
}

static gboolean
read_file_level(labelfile *lf, const char *path)
{
	FILE *filp;
	int fd;
	char *str = NULL;
	const char *match = "level: ";
	size_t mlen = sizeof("level: ") - 1;
	gboolean ret = FALSE;

	if (!lf) {
		ERR("labelfile is a NULL pointer\n");
		return FALSE;
	}
	
	usleep(50000U); 

	filp = open_file(path, &fd);
	if (!filp)
		return FALSE;

	DBG("opened %s\n", path);
	
	str = get_chomped_line(filp);
	if (!str) {
		ERR("failed to read level from %s\n", path);
		/* Failsafe */
		lf->level = 0;
		lf->label = g_strdup("");
		ret = TRUE;
		goto out;
	}

	if (strncmp(str, match, mlen)) {
		ERR("unexpected level string: %s", str);
		goto out;
	}

	lf->level = atoi(str + mlen); /* XXX Conversion errors ? */
	DBG("level %s (%d)\n", str + mlen, lf->level);
	free(str);
	str = NULL;

	ret = TRUE;
	lf->label = get_chomped_line(filp);
	if (!lf->label) 
		goto out;
	lf->complement = get_chomped_line(filp);
	/* Fall through */
out:
	if (str)
		free(str);
	(void)close_file(path, fd);
	return ret;
}

static gboolean
read_file_type_level(labelfile *lf, const char *path)
{
	FILE *filp;
	int fd = -1; /* Shut the f. up, gcc */
	char *str = NULL;
	const char *pmatch = "profile: ", *imatch = "ipsec: ", *lmatch = "level: ", *tmatch = "type: ";
	size_t plen = sizeof("profile: ") - 1;
	size_t ilen = sizeof("ipsec: ") - 1;
	size_t llen = sizeof("level: ") - 1;
	size_t tlen = sizeof("type: ") - 1;
	gboolean ret = FALSE;

	if (!lf) {
		ERR("labelfile is a NULL pointer\n");
		return FALSE;
	}
	
	usleep(50000U); 

	filp = open_file(path, &fd);
	DBG("opened %s\n", path);

	str = get_chomped_line(filp);
	if (!str) {
		ERR("failed to read profile from %s\n", path);
		goto out;
	}

	if (strncmp(str, pmatch, plen)) {
		ERR("unexpected profile string: %s", str);
		goto out;
	}
	
	lf->profile = g_strdup(str + plen); 
	if(!lf->profile) {
		ERR("Out of memory\n");
		goto out;
	}
	DBG("profile %s (%s)\n", str + plen, lf->profile);
	free(str);
	str = NULL;

	str = get_chomped_line(filp);
	if (!str) {
		ERR("failed to read ipsec from %s\n", path);
		goto out;
	}

	if (strncmp(str, imatch, ilen)) {
		ERR("unexpected ipsec string: %s", str);
		goto out;
	}
	
	lf->ipsec = g_strdup(str + ilen); 
	if(!lf->ipsec) {
		ERR("Out of memory\n");
		goto out;
	}
	DBG("ipsec %s (%s)\n", str + ilen, lf->ipsec);
	free(str);
	str = NULL;

	str = get_chomped_line(filp);
	if (!str) {
		ERR("failed to read type from %s\n", path);
		goto out;
	}

	if (strncmp(str, tmatch, tlen)) {
		ERR("unexpected type string: %s", str);
		goto out;
	}
	
	lf->type = get_subtype_from_string(str + tlen); 
	DBG("type %s (%d)\n", str + tlen, lf->type);
	free(str);
	str = NULL;

	str = get_chomped_line(filp);
	if (!str) {
		ERR("failed to read level from %s\n", path);
		goto out;
	}

	if (strncmp(str, lmatch, llen)) {
		ERR("unexpected level string: %s", str);
		goto out;
	}

	lf->level = atoi(str + llen); /* XXX Conversion errors ? */
	DBG("level %s (%d)\n", str + llen, lf->level);
	free(str);
	str = NULL;

	ret = TRUE;
	lf->label = get_chomped_line(filp);
	if (!lf->label) 
		goto out;
	lf->complement = get_chomped_line(filp);
	/* Fall through */
out:
	if (str)
		free(str);
	(void)close_file(path, fd);
	return ret;
}
/*************************************************************/
/*                       Forkexec                            */
/*************************************************************/

static void 
child_reaper(GPid pid, gint status, gpointer data)
{
	label *l = data;
	if (!l)
		return;

	if (pid == l->pid) {
		l->pid = 0;
	} else {
		ERR("label child reaper error: %d ! = %d\n", pid, l->pid);
	}
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
my_spawn_action(label *l)
{
	GPid pid;
	sigset_t set, oldset;
	gboolean ret = FALSE;
	GError *err = NULL;
	
	if (!l->action)
		return TRUE;

	BEGIN_CHILD_PROTECT();
	if (l->pid)
		goto out;

	ret = g_spawn_async(NULL, l->action, NULL, G_SPAWN_DO_NOT_REAP_CHILD,
						NULL, NULL, &pid, &err);
	if (ret == TRUE) {
		l->pid = pid;
		(void)g_child_watch_add(pid, child_reaper, l);
	} else {
		if (err != NULL) 
			ERR("Spawn error: %s\n", err->message);
		else
			ERR("Spawn error, no message\n");
	}

out:
	END_CHILD_PROTECT();
	return ret;
}

/*************************************************************/
/*                       Update                              */
/*************************************************************/

static gboolean
set_pixmaps_nolevels(label *l, const labelfile *lf __attribute__((unused))) 
{
	unsigned int i;
	ENTER;
	if (l->noflag) {
		if (l->flag[0])
			g_free(l->flag[0]);
		l->flag[0] = l->noflag;
		l->noflag = NULL;
	}
	if (l->flagold) {
		if (l->flag[1])
			g_free(l->flag[1]);
		l->flag[1] = l->flagold;
		l->flagold = NULL;
	}
	for(i = 2; i < MAX_LEVEL; i++) {
		if (l->flag[i])
			g_free(l->flag[i]);
		l->flag[i] = g_strdup(l->flag[1]);
	}
	RET(TRUE);
}

static gboolean
set_pixmaps_net(label *l, const labelfile *lf)
{
	unsigned int i;
	char *str;

	ENTER;
	if(!l->noflag) {
		l->noflag = l->flag[0];
		l->flag[0] = NULL;
		l->flagold = l->flag[1];
		l->flag[1] = NULL;
	}
	if (!lf)
		return TRUE;

	switch (lf->type) {
		case TypeWired:
			if (l->flag[0])
				g_free(l->flag[0]);
			l->flag[0] = g_strdup(IMGPREFIX "/network-wired.png");
			for (i = 1; i < MAX_LEVEL; i++) {
				if (l->flag[i])
					g_free(l->flag[i]);
				l->flag[i] = g_strdup(IMGPREFIX 
						"/network-wired-connect.png");
			}
			break;
		case TypeWifi:
			for(i = 0; i <= 3; i++) {
				if (l->flag[i])
					g_free(l->flag[i]);
				if (asprintf(&str, 
					IMGPREFIX "/network-wireless-%d.png", 
						i) < 0) {
					ERR("Out of memory");
					return FALSE;
				}
				l->flag[i] = str;
			}
			for (; i < MAX_LEVEL; i++) {
				if (l->flag[i])
					g_free(l->flag[i]);
				l->flag[i] = g_strdup(IMGPREFIX 
						"/network-wireless.png");
			}
			break;
		case TypeUmts:	
			for(i = 0; i <= 4; i++) {
				if (l->flag[i])
					g_free(l->flag[i]);
				if (asprintf(&str, IMGPREFIX 
					"/network-umts-%d.png", i) < 0) {
					ERR("Out of memory");
					return FALSE;
				}
				l->flag[i] = str;
			}
			for (; i < MAX_LEVEL; i++) {
				if (l->flag[i])
					g_free(l->flag[i]);
				l->flag[i] = g_strdup(IMGPREFIX 
							"/network-umts.png");
			}
			break;
		default:
			ERR("Unsupported net type : %d\n", lf->type);
			return FALSE;
	}
	RET(TRUE);
}

static void
do_update_widgets(label *l, labelfile *lf, const char *tip)
{
	GError *gerror = NULL;
	GdkPixbuf *pixbuf;
	
	gtk_widget_set_tooltip_text(l->plugin.pwid, tip);

	DBG("lf.level %d\n", lf->level);
	if ((lf->level < 0) || (lf->level >= MAX_LEVEL))
		lf->level = MAX_LEVEL-1;
	if (l->flag[lf->level]) {
		DBG("file: %s\n", l->flag[lf->level]);
		pixbuf = gdk_pixbuf_new_from_file_at_size(l->flag[lf->level], 
						l->width, l->height, &gerror);
		if (!pixbuf) {
			ERR( _("Failed to load file: %s\n"), gerror->message);
			g_error_free(gerror);
			gerror = NULL;
		}
		DBG("draw\n");
		
		gdk_draw_rectangle(l->pixmap, l->bg, TRUE, 0, 0, 
							l->width, l->height);	
		gdk_draw_pixbuf(l->pixmap, l->bg, pixbuf, 
			0, 0, 1, 1, -1, -1, GDK_RGB_DITHER_NORMAL, 0, 0);
		g_object_unref(pixbuf);
		DBG("queue\n");		
		gtk_widget_queue_draw(l->labelw);
	}
}

static gboolean
tooltip_update(const labelfile *lf, char **tip)
{
	if (lf->level) {
		if (asprintf(tip, "%s\n%s", lf->label, 
				_("Please reboot to apply updates")) < 0) {
			ERR("Out of memory");
			return FALSE;
		}
	} else {
		if (asprintf(tip, "%s\n%s", lf->label, 
				_("CLIP core is up-to-date")) < 0) {
			ERR("Out of memory");
			return FALSE;
		}
	}
	return TRUE;
}

static gboolean
update_update(label *l)
{
	labelfile lf = INIT_LABELFILE;
	char *tip = NULL;
	struct stat buf;
	gboolean ret = FALSE;

	if (!l->readfile(&lf, l->file)) {
		if (asprintf(&tip, _("No file %s"), l->file) < 0) {
			ERR("Out of memory");
			goto out;
		}
		goto update;
	} 
	if (stat(l->trigger, &buf)) {
		if (errno != ENOENT) {
			ERR_ERRNO("label: failed to stat trigger %s", 
								l->trigger);
			goto out;
		}
		lf.level = 0;
	} else {
		lf.level = 1;
	}

	if (!l->tooltip(&lf, &tip))
		goto out;

update:
	DBG("updating...\n");
	do_update_widgets(l, &lf, tip);
	ret = TRUE;
	/* Fall through */
out:
	if (tip)
		free(tip);
	freefile(&lf);
	return ret;
}

static gboolean
tooltip_net(const labelfile *lf, char **tip)
{
	gboolean profile = (gboolean)lf->profile;
	gboolean ipsec = lf->ipsec && strlen(lf->ipsec) != 0;

	if (asprintf(tip, "%s%s%s%s%s%s%s%s%s%s", 
			(profile) ? _("Profile: ") : "", 
			(profile) ? lf->profile : "", 
			(profile) ? "\n" : "", 
			(ipsec) ? _("IPsec: ") : "", 
			(ipsec) ? lf->ipsec : "", 
			(ipsec) ? "\n" : "", 
			(lf->type != TypeUndefined) ? 
				subtype_to_string(lf->type) : "",
			(lf->label) ? lf->label : "", 
			(lf->complement) ? "\n" : "",
			(lf->complement) ? lf->complement : "") < 0) {
		ERR("Out of memory");
		return FALSE;
	}
	return TRUE;
}

static gboolean
tooltip_usb(const labelfile *lf, char **tip)
{
	int ret;
	if (!lf->level) {
		ret = asprintf(tip, _("No mounted USB token%s%s"), 
			(lf->label) ? "\n" : "",
			(lf->label) ? lf->label : "");
	} else if (lf->level == 1) {
		ret = asprintf(tip, _("1 mounted USB token%s%s"), 
			(lf->label) ? "\n" : "",
			(lf->label) ? lf->label : "");
	} else {
		ret = asprintf(tip, _("%d mounted USB tokens%s%s"), 
			lf->level,
			(lf->label) ? "\n" : "",
			(lf->label) ? lf->label : "");
	}
	if (ret < 0) {
		ERR("Out of memory");
		return FALSE;
	}
	return TRUE;
}

static gboolean 
update_from_file(label *l, labelfile *lf, const char *path, char **tip)
{
	if (!l->readfile(lf, path)) {
		if (asprintf(tip, _("No file %s"), path) < 0) {
			ERR("Out of memory");
			return FALSE;
		} else {
			return TRUE;
		}
	}
	return l->tooltip(lf, tip);
}

static gboolean
update_level(label *l)
{
	labelfile lf = INIT_LABELFILE;
	char *tip = NULL;
	struct stat buf;
	gboolean ret = FALSE;

 
	if (l->trigger) {
		if (stat(l->trigger, &buf)) {
			if (errno != ENOENT) {
				ERR_ERRNO("label: failed to stat trigger %s", 
								l->trigger);
				goto out;
			}
			update_from_file(l, &lf, l->file, &tip);
		} else {
			lf.level = 1;
			update_from_file(l, &lf, l->trigger, &tip);
		}
	} else {
		update_from_file(l, &lf, l->file, &tip);
	}

	if (lf.type != TypeUndefined) 
		l->set_pixmaps(l, &lf);

	do_update_widgets(l, &lf, tip);
	ret = TRUE;
	/* Fall through */
out:
	if (tip)
		free(tip);
	freefile(&lf);
	return ret;
}

/*************************************************************/
/*                       Inotify                             */
/*************************************************************/

static gboolean
inotify_watch(GIOChannel *src, GIOCondition cond, gpointer data)
{
	struct inotify_event event;
	ssize_t rret;
	label *l = data;
	int fd = g_io_channel_unix_get_fd(src);
	gboolean done = FALSE;

	DBG("inotify_watch: %s\n", l->file);
	for (;;) {
		/* XXX we don't deal with the variable length 
		 * 'name' field.
		 */
		rret = read(fd, &event, sizeof(event));
		if (rret < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN)
				break;
			ERR_ERRNO("read inotify");
			break;
		}
		if (!rret)
			break;

		if (event.wd != l->wd) {
			ERR("Wrong wd for watch : %d != %d\n", 
				event.wd, l->wd);
			return FALSE;
		}

		/* No point updating several times */
		if (done) 
			continue;

		DBG("calling update on %s\n", l->file);
		(void)l->update(l);
		done = TRUE;
	}

	l->wd = inotify_add_watch(fd, l->file, IN_MODIFY|IN_ONESHOT);
	if (l->wd < 0) {
		ERR_ERRNO("inotify_add_watch %s failed", l->file);
		return FALSE;
	}
	return TRUE;
}

static gboolean
add_inotify(label *l, const char *path)
{
	int fd, wd = -1;
	GIOChannel *gio;

	fd = inotify_init();
	if (fd < 0) {
		ERR_ERRNO("inotify_init failed");
		return FALSE;
	}

	/* XXX Don't know why, but I have to re-add a watch 
	 * every time, so I might as well make it ONESHOT...
	 */
	wd = inotify_add_watch(fd, path, IN_MODIFY|IN_ONESHOT);
	if (wd < 0) {
		ERR_ERRNO("inotify_add_watch %s failed", path);
		return FALSE;
	}
	l->wd = wd;

	gio = g_io_channel_unix_new(fd);
	g_io_add_watch(gio, G_IO_IN, inotify_watch, l);
	g_io_channel_set_flags(gio, G_IO_FLAG_NONBLOCK, NULL);

	return TRUE;
}


/*************************************************************/
/*                       Config                            */
/*************************************************************/


#define SKIP_WHITESPACE(ptr) do {\
	while (*ptr == ' ' || *ptr == '\t') { \
		*ptr = '\0'; \
		ptr++; \
	} \
} while (0)

static gboolean
set_action(label *l, const char *act)
{
	char *copy = NULL, *token, *next, *delim;
	char **args = NULL;
	size_t count = 0, len;
	unsigned int i, in_quote = 0;
	
	if (l->action) {
		ERR("label: action already defined to %s [...], "
			"won't set it to %s\n", *(l->action), act);
		return FALSE;
	}
	
	copy = strdup(act);
	if (!copy) {
		ERR("Out of memory\n");
		return FALSE;
	}
	DBG("action? %s\n", copy);
	next = copy;
	delim = " \t\n";
	token = strsep(&next, delim);
	while (next) {
		count++;
		if (in_quote) {
			in_quote = 0;
			SKIP_WHITESPACE(next);
			delim = " \t\n";
		} else if (next && (*next == '"' || *next == '\'')) {
			switch(*next) {
			case '"':
				delim = "\"";
				break;
			case '\'':
				delim = "'";
				break;
			}
			*next = '\0';
			next++;
			in_quote = 1;
		}
		token = strsep(&next, delim);
	}
	
	args = calloc(count + 2, sizeof(char *));
	if (!args) {
		ERR("Out of memory\n");
		free(copy);
		return FALSE;
	}
	
	next = copy;
	if (!count) {
		args[0] = copy;
		args[1] = NULL;
	} else {
		for (i = 0; i < count; i++) {
			token = next;
			if (*token == '"' || *token == '\'')
				token++;
			len = strlen(token);
			next += len + 1;
			if (token[len - 1] == '"' || token[len - 1] == '\'')
				token[len - 1] = '\0';
			/* Multiple trailing NULLS are possible with quotes */
			if (i != count - 1) {
				while (!(*next))
					next++;
			}
			args[i] = token;
			DBG("args[%d] = %s\n", i, token);
		}
		args[i] = NULL;		/* last one MUST be NULL */
	}
	l->action = args;
	
	return TRUE;
}

static gboolean
set_type(label *l, const char *type)
{
	monitor_t t = get_type_from_string(type);
	switch (t) {
		case MonitorUpdate:
			l->readfile = read_file_plain;
			l->set_pixmaps = set_pixmaps_nolevels;
			l->update = update_update;
			l->tooltip = tooltip_update;
			break;
		case MonitorNet:
			l->readfile = read_file_type_level;
			l->set_pixmaps = set_pixmaps_net;
			l->update = update_level;
			l->tooltip = tooltip_net;
			break;
		case MonitorUSB:
			l->readfile = read_file_level;
			l->set_pixmaps = set_pixmaps_nolevels;
			l->update = update_level;
			l->tooltip = tooltip_usb;
			break;
		default:
			ERR("label: unknown type %s\n", type);
			return FALSE;
	}
	l->type = t;
	return TRUE;
}

/*************************************************************/
/*                       Events                              */
/*************************************************************/

static gint 
configure_event(GtkWidget *widget, GdkEventConfigure *event, label *l) 
{
	ENTER;

	if (l->pixmap)
		g_object_unref(l->pixmap);

	/* Update the plugin's dimensions */
	l->width = widget->allocation.width;
	l->height = widget->allocation.height;

	l->pixmap = gdk_pixmap_new (widget->window, 
				widget->allocation.width,
				widget->allocation.height, -1);

	l->update(l);
	RET(TRUE);
}


static gint 
expose_event(GtkWidget *widget, GdkEventExpose *event, label *l) 
{
	ENTER;

	if (l->pixmap)
		gdk_draw_drawable (widget->window, l->bg,
			 l->pixmap, event->area.x, event->area.y, 
			 event->area.x, event->area.y, 
			 event->area.width, event->area.height);

	RET(FALSE);
}

static gboolean
clicked(GtkWidget *widget, gpointer dummy, label *l)
{
	gboolean ret;
	ENTER;

	ret = my_spawn_action(l);
	RET(ret);
}

/*************************************************************/
/*                       Constructor                         */
/*************************************************************/


static int
label_constructor(plugin_instance *p)
{
	label *l = (label *)p;
	gchar *action = NULL, *type = NULL, *flag0 = NULL, *flag1 = NULL;
	
	ENTER;
	l->width = 30;
	l->inittimer = -300;
	l->height = 10;
	l->main = gtk_event_box_new();
	l->bg = gdk_gc_new(p->panel->topgwin->window);
	{
		unsigned int i;
		for (i = 0; i < MAX_LEVEL; i++)
			l->flag[i] = NULL;
	}
	l->trigger = NULL;
	l->noflag = l->flagold = NULL;

	XCG(p->xc, "file", &l->file, str);
	XCG(p->xc, "timer", &l->inittimer, int);
	XCG(p->xc, "trigger", &l->trigger, str);
	XCG(p->xc, "noflag", &flag0, str);
	XCG(p->xc, "flag", &flag1, str);
	XCG(p->xc, "action", &action, str);
	XCG(p->xc, "type", &type, str);
	XCG(p->xc, "Size", &l->width, int);
	{
		char *backgroundColor = NULL;
		XCG(p->xc, "BackgroundColor", &backgroundColor, str);
		if (!backgroundColor)
			backgroundColor = "#ebebeb";
		gdk_color_parse(backgroundColor, &l->background);
		gdk_colormap_alloc_color(
			gdk_drawable_get_colormap(p->panel->topgwin->window), 
			&l->background, FALSE, TRUE);
		gdk_gc_set_foreground(l->bg, &l->background);
	}

	if (flag0)
		l->flag[0] = g_strdup(flag0);
	if (flag1)
		l->flag[1] = g_strdup(flag1);
	if (action && !set_action(l, action))
		goto error;
	if (type && !set_type(l, type))
		goto error;

	if (!l->file) {
		ERR("label: file is not defined\n");
		goto error;
	}

	l->labelw = gtk_drawing_area_new();
	gtk_widget_add_events(l->labelw, 
		GDK_EXPOSURE_MASK|GDK_BUTTON_PRESS_MASK );

	DBG("pixmap\n");
	gtk_container_add(GTK_CONTAINER(p->pwid), l->labelw);
	gtk_widget_set_size_request(l->labelw, l->width, l->height);

	l->set_pixmaps(l, NULL);

	(void)l->update(l);
	if (l->inittimer > 0) {
		l->timer = g_timeout_add(ABS(l->inittimer) * 1000, 
				(GSourceFunc) l->update, (gpointer)l);
	} else if (!l->inittimer) {
		add_inotify(l, l->file);
	}
	if (l->action) {
		g_signal_connect(G_OBJECT(l->plugin.pwid), "button_press_event",
					G_CALLBACK(clicked), (gpointer)l);
	}
	g_signal_connect (G_OBJECT (l->labelw), "configure_event",
		G_CALLBACK (configure_event), (gpointer) l);
	g_signal_connect (G_OBJECT (l->labelw), "expose_event",
		G_CALLBACK (expose_event), (gpointer) l);
	gtk_widget_show_all(l->labelw);
	RET(1);

error:
	RET(0);
}


static void
label_destructor(plugin_instance *p)
{
	label *l = (label *)p;
	ENTER;
	if (l->timer)
		g_source_remove(l->timer);

	if (l->action)
		free(l->action);

	{
		unsigned int i;
		for(i = 0; i<MAX_LEVEL; i++)
			if (l->flag[i]) {
				g_free(l->flag[i]);
			}
	}
	if (l->pixmap)
		g_object_unref(l->pixmap);
	RET();
}

static plugin_class class = {
	.fname		= NULL,
	.count		= 0,

	.type 		= "label",
	.name 		= "Label",
	.version	= "1.2",
	.description 	= "Display contents of a file as a label",
	.priv_size	= sizeof(label),

	.constructor 	= label_constructor,
	.destructor 	= label_destructor,
};

static plugin_class *class_ptr = (plugin_class *) &class;
