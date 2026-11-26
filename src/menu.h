/* Intuition menus: a strip of titles, items, sub-items, on the right button */
#ifndef WBDE_MENU_H
#define WBDE_MENU_H

#include "wm.h"

#define MI_DISABLED	0x01
#define MI_CHECKIT	0x02
#define MI_CHECKED	0x04
#define MI_BAR		0x08	/* a separator line, not an item */

struct menuitem {
	const char *label;
	char key;			/* Right Amiga + key, 0 none */
	unsigned int flags;
	int id;
	const struct menuitem *sub;
	int nsub;
};

struct menu {
	const char *title;
	const struct menuitem *items;
	int nitems;
};

struct menustrip {
	const struct menu *menus;
	int n;
	void *arg;
	void (*pick)(void *arg, int id);
	/* the live state of an item: MI_DISABLED / MI_CHECKED, or-ed in */
	unsigned int (*state)(void *arg, int id);
};

/* open on the right button going down; returns 0 if there is no strip */
int menu_open(struct screen *s, const struct menustrip *strip);
void menu_motion(struct screen *s, int x, int y);
/* the right button came up: closes, and calls pick for a chosen item */
void menu_close(struct screen *s);
int menu_is_open(void);
/* Right Amiga + ch; returns 1 if an item took it */
int menu_shortcut(const struct menustrip *strip, int ch);
/* draw the Amiga key glyph (also used by requesters) */
void menu_amigakey(struct screen *s, struct rp *rp, int x, int y, int fg, int bg);

#endif
