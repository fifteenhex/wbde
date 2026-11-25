/*
 * The window system: one screen, a stack of windows each with its own
 * backing store (Intuition's SMART_REFRESH, always), overlays for menus and
 * dragged icons, a rubber band outline and the pointer. Everything is drawn
 * into RAM and composited into the display's backbuffer one damaged
 * rectangle at a time, which is the only thing that crosses the slow bus.
 *
 * The window core knows nothing about Workbench: drawers, requesters and
 * client windows are all a struct win with a struct win_ops.
 */
#ifndef WBDE_WM_H
#define WBDE_WM_H

#include <stdint.h>

#include "gfx.h"

enum {
	P_DETAIL, P_BLOCK, P_TEXT, P_SHINE, P_SHADOW, P_FILL, P_FILLTEXT,
	P_BACKGROUND, P_HIGHLIGHTTEXT, P_BARDETAIL, P_BARBLOCK, P_BARTRIM,
	NPENS
};

/* window flags */
#define WF_CLOSE	0x0001
#define WF_DEPTH	0x0002
#define WF_ZOOM		0x0004
#define WF_SIZE		0x0008
#define WF_DRAG		0x0010
#define WF_BORDERLESS	0x0020
#define WF_BACKDROP	0x0040
#define WF_VSCROLL	0x0080
#define WF_HSCROLL	0x0100
#define WF_RMBTRAP	0x0200	/* right button goes to the window, not menus */
#define WF_NOACTIVATE	0x0400
#define WF_STD		(WF_CLOSE | WF_DEPTH | WF_ZOOM | WF_SIZE | WF_DRAG)

struct win;
struct menustrip;

struct keyev {
	uint16_t code;		/* Linux KEY_* */
	int down;		/* 1 press, 2 repeat, 0 release */
	int ch;			/* character after the keymap, 0 if none */
	unsigned int qual;	/* QUAL_* */
};

#define QUAL_LSHIFT	0x0001
#define QUAL_RSHIFT	0x0002
#define QUAL_CAPS	0x0004
#define QUAL_CTRL	0x0008
#define QUAL_LALT	0x0010
#define QUAL_RALT	0x0020
#define QUAL_LAMIGA	0x0040
#define QUAL_RAMIGA	0x0080
#define QUAL_SHIFT	(QUAL_LSHIFT | QUAL_RSHIFT)

struct win_ops {
	/* draw the content; rp's origin is the inner top left, clipped to it */
	void (*draw)(struct win *w, struct rp *rp);
	/* inner coordinates; button 1 left, 2 right, 3 middle */
	void (*button)(struct win *w, int x, int y, int button, int down, unsigned int qual);
	/* while a button is held in the content, and for every move if wants_motion */
	void (*motion)(struct win *w, int x, int y);
	void (*key)(struct win *w, const struct keyev *k);
	/* the close gadget was released over */
	void (*close)(struct win *w);
	/* after a size or zoom; the content has not been drawn yet */
	void (*resized)(struct win *w);
	/* a scroller moved: w->vtop / w->htop changed */
	void (*scrolled)(struct win *w);
	void (*activate)(struct win *w, int active);
};

struct win {
	struct win *above, *below;
	struct rect r;			/* on the screen, frame included */
	struct rect zoom;		/* the other size the zoom gadget swaps to */
	unsigned int flags;
	char title[128];
	struct bm bm;			/* backing store: frame and content */
	int bl, br, bt, bb;		/* border widths */
	int minw, minh, maxw, maxh;
	const struct win_ops *ops;
	void *priv;
	const struct menustrip *menus;	/* NULL: the screen's */
	int wants_motion;
	/* proportional scrollers, in content pixels */
	int vtotal, vvis, vtop;
	int htotal, hvis, htop;
	int pressed;			/* gadget held down, G_* */
	int pressed_in;			/* and the pointer is still over it */
	int dead;			/* closed, freed when it is safe */
	/*
	 * A composing display's own content for the inner area (a buffer a
	 * client shares, in VRAM): drawn over bm's inner part, which then
	 * only matters to the software composition used for screenshots.
	 */
	void *ext;
};

/* a composing display frees a window's ext here when the window goes */
extern void (*win_ext_free)(struct win *w);

struct overlay {
	struct overlay *next;
	struct bm *bm;
	int x, y;
	int key;
};

#define DAMAGE_MAX 32

struct screen {
	int w, h, depth;
	int xormask;			/* complement: (1 << depth) - 1 */
	uint8_t pens[NPENS];
	uint8_t palette[256][3];
	int ncolours;
	struct font *font;
	int barh;			/* screen title bar, including its bottom line */
	char title[160];
	const struct menustrip *menus;	/* the screen's own (Workbench's) */
	struct bm root;			/* the screen's own layer: title bar, colour 0 */
	struct bm *fb;			/* where composition lands */
	struct win *bottom, *top;
	struct win *active;
	struct overlay *overlays;
	struct rect damage[DAMAGE_MAX];
	int ndamage;
	/* rubber band */
	int outline_on;
	struct rect outline;
	/* pointer */
	struct bm *ptr;
	int ptr_hx, ptr_hy, ptr_key;
	int mx, my;
	int ptr_hidden;
	int hw_pointer;			/* the display shows it: moving it damages nothing */
	/* what the window core is doing with the mouse */
	int mode;
	struct win *grab;
	int grab_dx, grab_dy;
	struct rect grab_start;
	int knob_off;
	uint64_t repeat_at;
	/* the screen depth gadget was pressed */
	int bar_depth_pressed;
	void (*palette_changed)(struct screen *s);
	void (*flush_now)(void);	/* get what is drawn onto the display, now */
	void (*bar_depth)(struct screen *s);	/* the screen depth gadget */
	/*
	 * Several screens, as Intuition has them: each its own size, depth,
	 * palette and windows; the display shows the front one.  Screen pixel
	 * (x, y) is at display pixel (ox + x * sx, oy + y * sy); the scale is
	 * asked for by whoever opened the screen, the offset centres it (or,
	 * for a screen bigger than the display, scrolls with the pointer).
	 */
	float sx, sy;
	int ox, oy;
	float fx, fy;			/* the pointer, with what is left over of a move */
	struct screen *next;		/* the screen stack, front first */
	struct win *barwin;		/* a client screen's title bar, a window of its own */
	int showtitle;			/* the bar in front of backdrop windows */
	int id;				/* a client screen's */
	void *owner;
};

void screen_init(struct screen *s, struct bm *fb, int depth, struct font *font);
void screen_set_title(struct screen *s, const char *title);
void screen_draw_bar(struct screen *s);
void screen_paint_bar(struct screen *s, struct rp *rp);
void screen_damage(struct screen *s, const struct rect *r);
void screen_damage_all(struct screen *s);
/* composite every damaged rectangle into fb; calls out for each one */
void screen_flush(struct screen *s, void (*touch)(void *arg, const struct rect *r), void *arg);
/* r of the screen composed into `into` (the screen's size), by the CPU */
void screen_compose(struct screen *s, struct bm *into, const struct rect *r, int pointer);
/* the topmost live window covering all of r, or NULL: nothing below it shows */
struct win *screen_cover(struct screen *s, const struct rect *r);
void screen_set_pointer(struct screen *s, struct bm *ptr, int hx, int hy, int key);
/* a client screen's title bar sits above the backdrop windows, or below them */
void screen_place_bar(struct screen *s);
void screen_add_overlay(struct screen *s, struct overlay *o);
void screen_remove_overlay(struct screen *s, struct overlay *o);
void screen_move_overlay(struct screen *s, struct overlay *o, int x, int y);

struct win *win_open(struct screen *s, const struct rect *r, unsigned int flags,
		     const char *title, const struct win_ops *ops, void *priv);
void win_close(struct screen *s, struct win *w);
void win_set_title(struct screen *s, struct win *w, const char *title);
void win_to_front(struct screen *s, struct win *w);
void win_to_back(struct screen *s, struct win *w);
void win_activate(struct screen *s, struct win *w);
void win_move_size(struct screen *s, struct win *w, const struct rect *r);
/* redraw content (and frame) into the backing store and damage it */
void win_refresh(struct screen *s, struct win *w);
void win_refresh_frame(struct screen *s, struct win *w);
/* redraw only r of the content (inner coordinates), and damage only that */
void win_refresh_rect(struct screen *s, struct win *w, const struct rect *r);
void win_inner(const struct win *w, struct rect *r);	/* in window coords */
void win_set_scroll(struct screen *s, struct win *w, int vtotal, int vvis, int vtop,
		    int htotal, int hvis, int htop);
struct win *win_at(struct screen *s, int x, int y);

/* input from main; time in ms */
void wm_motion(struct screen *s, int x, int y, uint64_t now);
/* returns 1 if the event was used by the window core */
int wm_button(struct screen *s, int button, int down, unsigned int qual, uint64_t now);
void wm_key(struct screen *s, const struct keyev *k);
void wm_tick(struct screen *s, uint64_t now);
int wm_busy(const struct screen *s);	/* in the middle of a drag or size */

/* sizes, from the font: everything else is derived */
int wm_title_height(const struct screen *s);

#endif
