/* Workbench: the backdrop with the disks, drawer windows, icons, menus */
#ifndef WBDE_WB_H
#define WBDE_WB_H

#include <stdint.h>

#include "icon.h"
#include "menu.h"
#include "prefs.h"
#include "wm.h"

struct wbicon {
	struct icon ic;
	char *label;
	char *path;		/* the object: a directory, a file, a mount point */
	int isdir;
	int x, y;		/* image's top left, in the view's own coordinates */
	int labelw;
	int selected;
	int open;		/* its drawer is open: drawn ghosted, as WB does */
	int drawn;		/* selected | open << 1, as last drawn */
};

struct iconview {
	struct wbicon *icons;
	int n, cap;
	int vw, vh;		/* extent */
};

struct drawer {
	struct drawer *next;
	struct win *win;
	char *path;
	char *volname;
	struct iconview view;
	int showall;
	int byname;		/* View By Name: a text list */
	int ox, oy;		/* scroll */
	struct wbicon *opener;
	struct iconview *opener_view;
};

struct volume {
	char name[64];
	char path[256];
	int type;		/* WBDISK etc. for the default icon */
	char info[300];		/* Disk.info, if it has one */
};

struct wb {
	struct screen *s;
	struct wbenv *env;
	struct win *backdrop;
	struct iconview root;
	struct drawer *drawers;
	struct volume vols[16];
	int nvols;
	/* double click */
	uint64_t lastclick;
	struct wbicon *lasticon;
	/* dragging icons or a selection box */
	int dragging, lasso;
	struct iconview *dragview;
	struct win *dragwin;
	int dragx0, dragy0, dragmoved;
	struct bm dragbm;
	struct overlay dragov;
	int dragov0x, dragov0y;
	int quit;
	uint64_t now;
	char lastmsg[160];
	struct menustrip strip;
	int leftout_x;		/* where left-out icons without a spot go */
};

int wb_init(struct wb *wb, struct screen *s, struct wbenv *env, const char *conf);
void wb_tick(struct wb *wb, uint64_t now);
void wb_update_title(struct wb *wb);
/* a message box with one or two buttons; returns nothing, it is modeless */
void wb_request(struct wb *wb, const char *title, const char *text, const char *button);
/* run something; output (if any) appears in a console window */
int wb_launch(struct wb *wb, const char *dir, const char *const argv[], const char *title);
/* the same, for an icon: its .info and stack go into the environment */
int wb_launch_icon(struct wb *wb, const char *dir, const char *const argv[], const char *title,
		   const char *info, int stack);

#endif
