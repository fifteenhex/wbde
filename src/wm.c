/*
 * Window frames as Workbench 3.1 draws them on a hires screen with topaz 8:
 * an 11 pixel title bar (font height + 3) carrying the close gadget on the
 * left and zoom and depth on the right, 4 pixel side borders, an 18 pixel
 * right border and a 10 pixel bottom border when there is a size gadget or
 * a scroller in them. Active windows are filled with FILLPEN (blue), the
 * rest with BACKGROUNDPEN. Intuition 3.x draws the gadget glyphs with code
 * (sysiclass), not from bitmaps, so they are drawn with code here too.
 *
 * The shapes follow what a real Kickstart 3.1 Workbench shows (compared
 * side by side with one booted in an emulator):
 * every gadget is a raised box, the arrows are open chevrons, a
 * scroller's knob is a raised box in the border's own colour, the depth
 * gadget is a grey window behind a white one.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "font.h"
#include "menu.h"
#include "wm.h"

enum {
	G_NONE, G_CLOSE, G_DEPTH, G_ZOOM, G_SIZE, G_DRAG,
	G_VCONT, G_UP, G_DOWN, G_HCONT, G_LEFT, G_RIGHT, G_CONTENT,
};

enum { M_NONE, M_GADGET, M_DRAG, M_SIZE, M_VKNOB, M_HKNOB, M_CONTENT, M_BARDEPTH };

#define CLOSE_W	19
#define DEPTH_W	23
#define ZOOM_W	23
#define SIZE_W	18
#define VARROW_H 11
#define HARROW_W 16
#define REPEAT_FIRST 300
#define REPEAT_NEXT 60

int wm_title_height(const struct screen *s)
{
	return s->font ? s->font->ysize + 3 : 11;
}

#define PEN(_s, _p) ((_s)->pens[_p])

static void borders(const struct screen *s, struct win *w)
{
	if (w->flags & WF_BORDERLESS) {
		w->bl = w->br = w->bt = w->bb = 0;
		return;
	}
	w->bt = wm_title_height(s);
	w->bl = 4;
	w->br = (w->flags & (WF_SIZE | WF_VSCROLL)) ? SIZE_W : 4;
	w->bb = (w->flags & (WF_SIZE | WF_HSCROLL)) ? 10 : 2;
}

void win_inner(const struct win *w, struct rect *r)
{
	*r = rect_make(w->bl, w->bt, w->r.w - w->bl - w->br, w->r.h - w->bt - w->bb);
}

/* gadget rectangles, window coordinates */
static int gadget_rect(const struct screen *s, const struct win *w, int g, struct rect *r)
{
	int W = w->r.w, H = w->r.h, bt = w->bt;
	int right = W;

	if (w->flags & WF_BORDERLESS)
		return 0;
	switch (g) {
	case G_CLOSE:
		if (!(w->flags & WF_CLOSE))
			return 0;
		*r = rect_make(0, 0, CLOSE_W, bt);
		return 1;
	case G_DEPTH:
		if (!(w->flags & WF_DEPTH))
			return 0;
		*r = rect_make(W - DEPTH_W, 0, DEPTH_W, bt);
		return 1;
	case G_ZOOM:
		if (!(w->flags & WF_ZOOM))
			return 0;
		if (w->flags & WF_DEPTH)
			right -= DEPTH_W;
		*r = rect_make(right - ZOOM_W, 0, ZOOM_W, bt);
		return 1;
	case G_SIZE:
		if (!(w->flags & WF_SIZE))
			return 0;
		*r = rect_make(W - SIZE_W, H - w->bb, SIZE_W, w->bb);
		return 1;
	case G_DRAG: {
		int x0 = (w->flags & WF_CLOSE) ? CLOSE_W : 0;
		int x1 = W - ((w->flags & WF_DEPTH) ? DEPTH_W : 0) -
			 ((w->flags & WF_ZOOM) ? ZOOM_W : 0);

		if (!(w->flags & WF_DRAG))
			return 0;
		*r = rect_make(x0, 0, x1 - x0, bt);
		return 1;
	}
	case G_UP:
	case G_DOWN: {
		int y = H - w->bb - 2 * VARROW_H;

		if (!(w->flags & WF_VSCROLL))
			return 0;
		*r = rect_make(W - w->br, y + (g == G_DOWN ? VARROW_H : 0), w->br, VARROW_H);
		return 1;
	}
	case G_VCONT:
		if (!(w->flags & WF_VSCROLL))
			return 0;
		*r = rect_make(W - w->br + 4, bt + 2, w->br - 8,
			       H - w->bb - 2 * VARROW_H - bt - 4);
		return 1;
	case G_LEFT:
	case G_RIGHT: {
		int x = W - w->br - 2 * HARROW_W;

		if (!(w->flags & WF_HSCROLL))
			return 0;
		*r = rect_make(x + (g == G_RIGHT ? HARROW_W : 0), H - w->bb, HARROW_W, w->bb);
		return 1;
	}
	case G_HCONT:
		if (!(w->flags & WF_HSCROLL))
			return 0;
		*r = rect_make(w->bl + 2, H - w->bb + 2,
			       W - w->br - 2 * HARROW_W - w->bl - 4, w->bb - 4);
		return 1;
	}
	(void)s;
	return 0;
}

/* where the knob sits in its container, along the container's long axis */
static void knob(int contlen, int total, int vis, int top, int *pos, int *len)
{
	if (total <= vis || total <= 0) {
		*pos = 0;
		*len = contlen;
		return;
	}
	*len = (int)((long)contlen * vis / total);
	if (*len < 6)
		*len = 6;
	if (*len > contlen)
		*len = contlen;
	if (top > total - vis)
		top = total - vis;
	if (top < 0)
		top = 0;
	*pos = (int)((long)(contlen - *len) * top / (total - vis));
}

/* --- gadget glyphs --- */

static void bevel_box(struct screen *s, struct rp *rp, const struct rect *r, int in)
{
	if (in)
		rp_bevel(rp, r->x, r->y, r->w, r->h, PEN(s, P_SHADOW), PEN(s, P_SHINE));
	else
		rp_bevel(rp, r->x, r->y, r->w, r->h, PEN(s, P_SHINE), PEN(s, P_SHADOW));
}

/* the close gadget: a small white square edged in black */
static void glyph_close(struct screen *s, struct rp *rp, const struct rect *r)
{
	int x = r->x + 7, y = r->y + (r->h - 5) / 2;

	rp_fill(rp, x, y, 5, 5, PEN(s, P_SHINE));
	rp_box(rp, x, y, 5, 5, PEN(s, P_SHADOW));
}

/* the depth gadget: a grey window behind a white one, both edged in black */
static void glyph_depth(struct screen *s, struct rp *rp, const struct rect *r, int fill)
{
	int x = r->x + 4, y = r->y + 2, h = r->h - 6;

	(void)fill;
	rp_fill(rp, x, y, 11, h, PEN(s, P_BACKGROUND));
	rp_box(rp, x, y, 11, h, PEN(s, P_SHADOW));
	rp_fill(rp, x + 4, y + 2, 11, h, PEN(s, P_SHINE));
	rp_box(rp, x + 4, y + 2, 11, h, PEN(s, P_SHADOW));
}

/* the zoom gadget: a window outline with a small white one in its corner */
static void glyph_zoom(struct screen *s, struct rp *rp, const struct rect *r, int fill)
{
	int x = r->x + 5, y = r->y + 2, h = r->h - 4;

	rp_fill(rp, x, y, 13, h, fill);
	rp_box(rp, x, y, 13, h, PEN(s, P_SHADOW));
	rp_fill(rp, x, y, 7, h / 2 + 1, PEN(s, P_SHADOW));
	rp_fill(rp, x + 2, y + 1, 3, h / 2 - 1, PEN(s, P_SHINE));
}

/* the size gadget: a white wedge rising to the right, edged in black */
static void glyph_size(struct screen *s, struct rp *rp, const struct rect *r)
{
	int x0 = r->x + 3, y0 = r->y + 2, row;

	for (row = 0; row < 5; row++) {
		int from = x0 + 8 - 2 * row;	/* two pixels further left a row */

		rp_fill(rp, from, y0 + row, 2, 1, PEN(s, P_SHADOW));
		rp_fill(rp, from + 2, y0 + row, x0 + 10 - (from + 2), 1, PEN(s, P_SHINE));
	}
	rp_hline(rp, x0, y0 + 5, 11, PEN(s, P_SHADOW));
	rp_vline(rp, x0 + 10, y0, 6, PEN(s, P_SHADOW));
}

/*
 * The scroller arrows: open chevrons, two pixels thick, five rows; dir 0
 * up, 1 down, 2 left, 3 right.  Rows of an up chevron, from its point.
 */
static void glyph_arrow(struct screen *s, struct rp *rp, const struct rect *r, int dir)
{
	static const struct { signed char a, b; } up[5][2] = {
		{ { 4, 2 }, { 0, 0 } },		/* x offset, width: the point */
		{ { 3, 4 }, { 0, 0 } },
		{ { 2, 6 }, { 0, 0 } },
		{ { 1, 3 }, { 6, 3 } },		/* the two legs */
		{ { 0, 2 }, { 8, 2 } },
	};
	static const struct { signed char a, b; } left[5][2] = {
		{ { 4, 2 }, { 0, 0 } },
		{ { 2, 3 }, { 0, 0 } },
		{ { 0, 4 }, { 0, 0 } },
		{ { 2, 3 }, { 0, 0 } },
		{ { 4, 2 }, { 0, 0 } },
	};
	int pen = PEN(s, P_SHADOW), i, k;

	for (i = 0; i < 5; i++) {
		for (k = 0; k < 2; k++) {
			int a, b;

			if (dir < 2) {
				int row = dir == 0 ? i : 4 - i;

				a = up[row][k].a;
				b = up[row][k].b;
				if (b)
					rp_hline(rp, r->x + (r->w - 10) / 2 + a, r->y + (r->h - 5) / 2 + i,
						 b, pen);
			} else {
				a = left[i][k].a;
				b = left[i][k].b;
				if (!b)
					continue;
				if (dir == 3)		/* mirrored */
					a = 6 - a - b;
				rp_hline(rp, r->x + (r->w - 6) / 2 + a, r->y + (r->h - 5) / 2 + i, b, pen);
			}
		}
	}
}

/* a scroller: no container of its own, the knob a raised box in the border's colour */
static void draw_prop(struct screen *s, struct rp *rp, const struct rect *c,
		      int vertical, int total, int vis, int top, int fill)
{
	int pos, len;
	struct rect k;

	rp_fill(rp, c->x, c->y, c->w, c->h, fill);
	if (c->w <= 2 || c->h <= 2)
		return;
	if (vertical) {
		knob(c->h, total, vis, top, &pos, &len);
		k = rect_make(c->x, c->y + pos, c->w, len);
	} else {
		knob(c->w, total, vis, top, &pos, &len);
		k = rect_make(c->x + pos, c->y, len, c->h);
	}
	bevel_box(s, rp, &k, 0);
}

static void draw_frame(struct screen *s, struct win *w)
{
	int active = (s->active == w);
	int fill = active ? PEN(s, P_FILL) : PEN(s, P_BACKGROUND);
	int W = w->r.w, H = w->r.h;
	struct rp rp;
	struct rect g;

	if (w->flags & WF_BORDERLESS)
		return;
	rp_init(&rp, &w->bm);

	rp_fill(&rp, 0, 0, W, w->bt, fill);
	rp_fill(&rp, 0, w->bt, w->bl, H - w->bt, fill);
	rp_fill(&rp, W - w->br, w->bt, w->br, H - w->bt, fill);
	rp_fill(&rp, 0, H - w->bb, W, w->bb, fill);
	rp_bevel(&rp, 0, 0, W, H, PEN(s, P_SHINE), PEN(s, P_SHADOW));
	/* the content sits in a recess */
	rp_bevel(&rp, w->bl - 1, w->bt - 1, W - w->bl - w->br + 2, H - w->bt - w->bb + 2,
		 PEN(s, P_SHADOW), PEN(s, P_SHINE));

	if (gadget_rect(s, w, G_DRAG, &g) || !(w->flags & WF_BORDERLESS)) {
		/* the drag bar is a raised box too; the title sits 11 pixels into it */
		int x0 = (w->flags & WF_CLOSE) ? CLOSE_W + 11 : 4;
		int x1 = W - ((w->flags & WF_DEPTH) ? DEPTH_W : 0) -
			 ((w->flags & WF_ZOOM) ? ZOOM_W : 0) - 2;
		struct rp t = rp;
		struct rect clip = rect_make(x0, 0, x1 - x0, w->bt);

		if (w->flags & WF_DRAG) {
			struct rect d;

			gadget_rect(s, w, G_DRAG, &d);
			if (w->flags & WF_CLOSE)
				bevel_box(s, &rp, &d, 0);
		}
		rp_clip(&t, &clip);
		rp_text(&t, s->font, x0, 1, w->title, -1,
			active ? PEN(s, P_FILLTEXT) : PEN(s, P_TEXT), -1);
	}
	if (gadget_rect(s, w, G_CLOSE, &g)) {
		int in = w->pressed == G_CLOSE && w->pressed_in;

		bevel_box(s, &rp, &g, in);
		glyph_close(s, &rp, &g);
	}
	if (gadget_rect(s, w, G_ZOOM, &g)) {
		bevel_box(s, &rp, &g, w->pressed == G_ZOOM && w->pressed_in);
		glyph_zoom(s, &rp, &g, fill);
	}
	if (gadget_rect(s, w, G_DEPTH, &g)) {
		bevel_box(s, &rp, &g, w->pressed == G_DEPTH && w->pressed_in);
		glyph_depth(s, &rp, &g, fill);
	}
	if (gadget_rect(s, w, G_SIZE, &g)) {
		bevel_box(s, &rp, &g, 0);
		glyph_size(s, &rp, &g);
	}
	if (gadget_rect(s, w, G_VCONT, &g))
		draw_prop(s, &rp, &g, 1, w->vtotal, w->vvis, w->vtop, fill);
	if (gadget_rect(s, w, G_UP, &g)) {
		bevel_box(s, &rp, &g, w->pressed == G_UP && w->pressed_in);
		glyph_arrow(s, &rp, &g, 0);
	}
	if (gadget_rect(s, w, G_DOWN, &g)) {
		bevel_box(s, &rp, &g, w->pressed == G_DOWN && w->pressed_in);
		glyph_arrow(s, &rp, &g, 1);
	}
	if (gadget_rect(s, w, G_HCONT, &g))
		draw_prop(s, &rp, &g, 0, w->htotal, w->hvis, w->htop, fill);
	if (gadget_rect(s, w, G_LEFT, &g)) {
		bevel_box(s, &rp, &g, w->pressed == G_LEFT && w->pressed_in);
		glyph_arrow(s, &rp, &g, 2);
	}
	if (gadget_rect(s, w, G_RIGHT, &g)) {
		bevel_box(s, &rp, &g, w->pressed == G_RIGHT && w->pressed_in);
		glyph_arrow(s, &rp, &g, 3);
	}
}

static void draw_content(struct screen *s, struct win *w)
{
	struct rp rp;
	struct rect in;

	rp_init(&rp, &w->bm);
	win_inner(w, &in);
	rp_clip(&rp, &in);
	rp.ox = in.x;
	rp.oy = in.y;
	if (w->ops && w->ops->draw)
		w->ops->draw(w, &rp);
	else
		rp_fill(&rp, 0, 0, in.w, in.h, PEN(s, P_BACKGROUND));
}

static void damage_win(struct screen *s, struct win *w)
{
	screen_damage(s, &w->r);
}

void win_refresh_frame(struct screen *s, struct win *w)
{
	draw_frame(s, w);
	damage_win(s, w);
}

void win_refresh(struct screen *s, struct win *w)
{
	draw_frame(s, w);
	draw_content(s, w);
	damage_win(s, w);
}

/* --- the screen --- */

void screen_init(struct screen *s, struct bm *fb, int depth, struct font *font)
{
	static const uint8_t pens31[NPENS] = {
		[P_DETAIL] = 0, [P_BLOCK] = 1, [P_TEXT] = 1, [P_SHINE] = 2,
		[P_SHADOW] = 1, [P_FILL] = 3, [P_FILLTEXT] = 1, [P_BACKGROUND] = 0,
		[P_HIGHLIGHTTEXT] = 2, [P_BARDETAIL] = 1, [P_BARBLOCK] = 2,
		[P_BARTRIM] = 1,
	};

	memset(s, 0, sizeof(*s));
	s->fb = fb;
	s->w = fb->w;
	s->h = fb->h;
	s->depth = depth;
	s->xormask = (1 << depth) - 1;
	memcpy(s->pens, pens31, sizeof(s->pens));
	s->font = font;
	s->barh = font->ysize + 3;
	s->ptr_key = -1;
	bm_alloc(&s->root, s->w, s->h);
	memset(s->root.px, PEN(s, P_BACKGROUND), (size_t)s->root.stride * s->root.h);
	screen_draw_bar(s);
}

void screen_draw_bar(struct screen *s)
{
	struct rp rp;
	/* the screen's depth gadget: a grey raised box down to the bar's bottom line */
	struct rect g = rect_make(s->w - DEPTH_W, 0, DEPTH_W, s->barh);

	rp_init(&rp, &s->root);
	rp_fill(&rp, 0, 0, s->w, s->barh - 1, PEN(s, P_BARBLOCK));
	rp_hline(&rp, 0, s->barh - 1, s->w, PEN(s, P_BARTRIM));
	rp_text(&rp, s->font, 4, 1, s->title, -1, PEN(s, P_BARDETAIL), -1);
	rp_fill(&rp, g.x, g.y, g.w, g.h, PEN(s, s->bar_depth_pressed ? P_FILL : P_BACKGROUND));
	bevel_box(s, &rp, &g, s->bar_depth_pressed);
	glyph_depth(s, &rp, &g, PEN(s, P_BARBLOCK));
	g = rect_make(0, 0, s->w, s->barh);
	screen_damage(s, &g);
}

void screen_set_title(struct screen *s, const char *title)
{
	if (!strcmp(s->title, title))
		return;
	snprintf(s->title, sizeof(s->title), "%s", title);
	screen_draw_bar(s);
}

static long area(const struct rect *r)
{
	return (long)r->w * r->h;
}

void screen_damage(struct screen *s, const struct rect *in)
{
	struct rect r, scr = rect_make(0, 0, s->w, s->h);
	int i, best = -1;
	long bestgrow = 0;

	if (!rect_intersect(&r, in, &scr))
		return;
	for (i = 0; i < s->ndamage; i++) {
		struct rect u = s->damage[i], x;
		long grow;

		rect_union(&u, &r);
		/* merge when it costs little: overlapping or close neighbours */
		grow = area(&u) - area(&s->damage[i]) - area(&r);
		if (rect_intersect(&x, &s->damage[i], &r))
			grow += area(&x);
		if (grow <= 512 || (rect_contains(&s->damage[i], r.x, r.y) &&
		    rect_contains(&s->damage[i], r.x + r.w - 1, r.y + r.h - 1))) {
			s->damage[i] = u;
			return;
		}
		if (best < 0 || grow < bestgrow) {
			best = i;
			bestgrow = grow;
		}
	}
	if (s->ndamage < DAMAGE_MAX) {
		s->damage[s->ndamage++] = r;
		return;
	}
	rect_union(&s->damage[best], &r);
}

void screen_damage_all(struct screen *s)
{
	s->ndamage = 1;
	s->damage[0] = rect_make(0, 0, s->w, s->h);
}

void (*win_ext_free)(struct win *w);

struct win *screen_cover(struct screen *s, const struct rect *r)
{
	struct win *w;

	for (w = s->top; w; w = w->below)
		if (!w->dead && rect_contains(&w->r, r->x, r->y) &&
		    rect_contains(&w->r, r->x + r->w - 1, r->y + r->h - 1))
			return w;
	return NULL;
}

void screen_compose(struct screen *s, struct bm *into, const struct rect *r, int pointer)
{
	struct rp rp;
	struct win *w;
	struct overlay *o;
	struct rect x;

	rp_init(&rp, into);
	rp_clip(&rp, r);

	/* the topmost opaque window covering the whole rectangle hides all below */
	w = screen_cover(s, r);
	if (!w) {
		rp_blit(&rp, r->x, r->y, &s->root, r->x, r->y, r->w, r->h, -1);
		w = s->bottom;
	}
	for (; w; w = w->above) {
		if (w->dead || !rect_intersect(&x, &w->r, r))
			continue;
		rp_blit(&rp, x.x, x.y, &w->bm, x.x - w->r.x, x.y - w->r.y, x.w, x.h, -1);
	}
	for (o = s->overlays; o; o = o->next)
		rp_blit(&rp, o->x, o->y, o->bm, 0, 0, o->bm->w, o->bm->h, o->key);
	if (s->outline_on) {
		const struct rect *b = &s->outline;

		rp_xor_rect(&rp, b->x, b->y, b->w, 1, s->xormask);
		rp_xor_rect(&rp, b->x, b->y + b->h - 1, b->w, 1, s->xormask);
		rp_xor_rect(&rp, b->x, b->y + 1, 1, b->h - 2, s->xormask);
		rp_xor_rect(&rp, b->x + b->w - 1, b->y + 1, 1, b->h - 2, s->xormask);
	}
	if (pointer && s->ptr && !s->ptr_hidden)
		rp_blit(&rp, s->mx - s->ptr_hx, s->my - s->ptr_hy, s->ptr, 0, 0,
			s->ptr->w, s->ptr->h, s->ptr_key);
}

static void composite(struct screen *s, const struct rect *r)
{
	screen_compose(s, s->fb, r, !s->hw_pointer);
}

void screen_flush(struct screen *s, void (*touch)(void *arg, const struct rect *r), void *arg)
{
	int i, n = s->ndamage;
	struct rect d[DAMAGE_MAX];

	memcpy(d, s->damage, sizeof(d[0]) * (size_t)n);
	s->ndamage = 0;
	for (i = 0; i < n; i++) {
		composite(s, &d[i]);
		if (touch)
			touch(arg, &d[i]);
	}
}

static void damage_pointer(struct screen *s)
{
	struct rect r;

	if (!s->ptr || s->hw_pointer)
		return;
	r = rect_make(s->mx - s->ptr_hx, s->my - s->ptr_hy, s->ptr->w, s->ptr->h);
	screen_damage(s, &r);
}

void screen_set_pointer(struct screen *s, struct bm *ptr, int hx, int hy, int key)
{
	damage_pointer(s);
	s->ptr = ptr;
	s->ptr_hx = hx;
	s->ptr_hy = hy;
	s->ptr_key = key;
	damage_pointer(s);
}

static void damage_overlay(struct screen *s, struct overlay *o)
{
	struct rect r = rect_make(o->x, o->y, o->bm->w, o->bm->h);

	screen_damage(s, &r);
}

void screen_add_overlay(struct screen *s, struct overlay *o)
{
	struct overlay **p = &s->overlays;

	while (*p)
		p = &(*p)->next;
	o->next = NULL;
	*p = o;
	damage_overlay(s, o);
}

void screen_remove_overlay(struct screen *s, struct overlay *o)
{
	struct overlay **p;

	for (p = &s->overlays; *p; p = &(*p)->next) {
		if (*p == o) {
			*p = o->next;
			damage_overlay(s, o);
			return;
		}
	}
}

void screen_move_overlay(struct screen *s, struct overlay *o, int x, int y)
{
	damage_overlay(s, o);
	o->x = x;
	o->y = y;
	damage_overlay(s, o);
}

static void damage_outline(struct screen *s)
{
	struct rect b = s->outline, e;

	if (!s->outline_on)
		return;
	e = rect_make(b.x, b.y, b.w, 1);
	screen_damage(s, &e);
	e = rect_make(b.x, b.y + b.h - 1, b.w, 1);
	screen_damage(s, &e);
	e = rect_make(b.x, b.y, 1, b.h);
	screen_damage(s, &e);
	e = rect_make(b.x + b.w - 1, b.y, 1, b.h);
	screen_damage(s, &e);
}

static void set_outline(struct screen *s, const struct rect *r)
{
	damage_outline(s);
	if (r) {
		s->outline = *r;
		s->outline_on = 1;
		damage_outline(s);
	} else {
		s->outline_on = 0;
	}
}

/* --- windows --- */

static void unlink_win(struct screen *s, struct win *w)
{
	if (w->below)
		w->below->above = w->above;
	else
		s->bottom = w->above;
	if (w->above)
		w->above->below = w->below;
	else
		s->top = w->below;
	w->above = w->below = NULL;
}

/* above `after`, or at the bottom when after is NULL */
static void link_above(struct screen *s, struct win *w, struct win *after)
{
	w->below = after;
	w->above = after ? after->above : s->bottom;
	if (w->above)
		w->above->below = w;
	else
		s->top = w;
	if (after)
		after->above = w;
	else
		s->bottom = w;
}

static struct win *highest_backdrop(struct screen *s)
{
	struct win *w, *b = NULL;

	for (w = s->bottom; w && (w->flags & WF_BACKDROP); w = w->above)
		b = w;
	return b;
}

static void clamp_size(struct win *w, struct rect *r)
{
	if (r->w < w->minw)
		r->w = w->minw;
	if (r->h < w->minh)
		r->h = w->minh;
	if (w->maxw && r->w > w->maxw)
		r->w = w->maxw;
	if (w->maxh && r->h > w->maxh)
		r->h = w->maxh;
}

struct win *win_open(struct screen *s, const struct rect *r, unsigned int flags,
		     const char *title, const struct win_ops *ops, void *priv)
{
	struct win *w = calloc(1, sizeof(*w));

	if (!w)
		return NULL;
	w->flags = flags;
	w->ops = ops;
	w->priv = priv;
	snprintf(w->title, sizeof(w->title), "%s", title ? title : "");
	borders(s, w);
	w->minw = CLOSE_W + DEPTH_W + ZOOM_W + 40;
	w->minh = w->bt + w->bb + ((flags & WF_VSCROLL) ? 2 * VARROW_H + 16 : 8);
	if (flags & WF_BORDERLESS)
		w->minw = w->minh = 1;
	w->r = *r;
	clamp_size(w, &w->r);
	if (w->r.w > s->w)
		w->r.w = s->w;
	if (w->r.h > s->h - s->barh && !(flags & WF_BORDERLESS))
		w->r.h = s->h - s->barh;
	if (w->r.x + w->r.w > s->w)
		w->r.x = s->w - w->r.w;
	if (w->r.y + w->r.h > s->h)
		w->r.y = s->h - w->r.h;
	if (w->r.x < 0)
		w->r.x = 0;
	if (w->r.y < 0)
		w->r.y = 0;
	w->zoom = rect_make(w->r.x, w->r.y, w->minw, w->minh);
	if (bm_alloc(&w->bm, w->r.w, w->r.h)) {
		free(w);
		return NULL;
	}
	if (flags & WF_BACKDROP)
		link_above(s, w, highest_backdrop(s));
	else
		link_above(s, w, s->top);
	return w;
}

void win_close(struct screen *s, struct win *w)
{
	if (s->active == w)
		s->active = NULL;
	if (s->grab == w) {
		s->grab = NULL;
		s->mode = M_NONE;
		set_outline(s, NULL);
	}
	damage_win(s, w);
	unlink_win(s, w);
	if (w->ext && win_ext_free)
		win_ext_free(w);
	bm_free(&w->bm);
	free(w);
}

void win_set_title(struct screen *s, struct win *w, const char *title)
{
	snprintf(w->title, sizeof(w->title), "%s", title);
	win_refresh_frame(s, w);
}

void win_to_front(struct screen *s, struct win *w)
{
	if (s->top == w || (w->flags & WF_BACKDROP))
		return;
	unlink_win(s, w);
	link_above(s, w, s->top);
	damage_win(s, w);
}

void win_to_back(struct screen *s, struct win *w)
{
	if (w->flags & WF_BACKDROP)
		return;
	unlink_win(s, w);
	link_above(s, w, highest_backdrop(s));
	damage_win(s, w);
}

void win_activate(struct screen *s, struct win *w)
{
	struct win *old = s->active;

	if (old == w || (w && (w->flags & WF_NOACTIVATE)))
		return;
	s->active = w;
	if (old) {
		win_refresh_frame(s, old);
		if (old->ops && old->ops->activate)
			old->ops->activate(old, 0);
	}
	if (w) {
		win_refresh_frame(s, w);
		if (w->ops && w->ops->activate)
			w->ops->activate(w, 1);
	}
}

void win_move_size(struct screen *s, struct win *w, const struct rect *nr)
{
	struct rect r = *nr;
	int resized = r.w != w->r.w || r.h != w->r.h;

	clamp_size(w, &r);
	damage_win(s, w);
	if (resized) {
		struct bm nb;

		if (bm_alloc(&nb, r.w, r.h))
			return;
		bm_free(&w->bm);
		w->bm = nb;
	}
	w->r = r;
	if (resized) {
		if (w->ops && w->ops->resized)
			w->ops->resized(w);
		win_refresh(s, w);
	}
	damage_win(s, w);
}

void win_set_scroll(struct screen *s, struct win *w, int vtotal, int vvis, int vtop,
		    int htotal, int hvis, int htop)
{
	w->vtotal = vtotal;
	w->vvis = vvis;
	w->vtop = vtop;
	w->htotal = htotal;
	w->hvis = hvis;
	w->htop = htop;
	(void)s;
}

struct win *win_at(struct screen *s, int x, int y)
{
	struct win *w;

	for (w = s->top; w; w = w->below)
		if (!w->dead && rect_contains(&w->r, x, y))
			return w;
	return NULL;
}

static int gadget_at(struct screen *s, struct win *w, int x, int y)
{
	static const int order[] = {
		G_CLOSE, G_DEPTH, G_ZOOM, G_SIZE, G_UP, G_DOWN, G_LEFT, G_RIGHT,
		G_VCONT, G_HCONT, G_DRAG,
	};
	struct rect r;
	unsigned int i;

	for (i = 0; i < sizeof(order) / sizeof(order[0]); i++)
		if (gadget_rect(s, w, order[i], &r) && rect_contains(&r, x, y))
			return order[i];
	win_inner(w, &r);
	if (rect_contains(&r, x, y))
		return G_CONTENT;
	return G_NONE;
}

static void scroll_to(struct screen *s, struct win *w, int vtop, int htop)
{
	if (vtop > w->vtotal - w->vvis)
		vtop = w->vtotal - w->vvis;
	if (vtop < 0)
		vtop = 0;
	if (htop > w->htotal - w->hvis)
		htop = w->htotal - w->hvis;
	if (htop < 0)
		htop = 0;
	if (vtop == w->vtop && htop == w->htop)
		return;
	w->vtop = vtop;
	w->htop = htop;
	if (w->ops && w->ops->scrolled)
		w->ops->scrolled(w);
	win_refresh(s, w);
}

static void step(struct screen *s, struct win *w, int g)
{
	int line = s->font ? s->font->ysize * 2 : 16;

	switch (g) {
	case G_UP: scroll_to(s, w, w->vtop - line, w->htop); break;
	case G_DOWN: scroll_to(s, w, w->vtop + line, w->htop); break;
	case G_LEFT: scroll_to(s, w, w->vtop, w->htop - line); break;
	case G_RIGHT: scroll_to(s, w, w->vtop, w->htop + line); break;
	}
}

/* a click in the container but not on the knob pages by what is visible */
static void page(struct screen *s, struct win *w, int g, int x, int y)
{
	struct rect c;
	int pos, len;

	gadget_rect(s, w, g, &c);
	if (g == G_VCONT) {
		knob(c.h, w->vtotal, w->vvis, w->vtop, &pos, &len);
		if (y < c.y + pos)
			scroll_to(s, w, w->vtop - w->vvis, w->htop);
		else if (y >= c.y + pos + len)
			scroll_to(s, w, w->vtop + w->vvis, w->htop);
		else {
			s->mode = M_VKNOB;
			s->knob_off = y - (c.y + pos);
		}
	} else {
		knob(c.w, w->htotal, w->hvis, w->htop, &pos, &len);
		if (x < c.x + pos)
			scroll_to(s, w, w->vtop, w->htop - w->hvis);
		else if (x >= c.x + pos + len)
			scroll_to(s, w, w->vtop, w->htop + w->hvis);
		else {
			s->mode = M_HKNOB;
			s->knob_off = x - (c.x + pos);
		}
	}
}

static void drag_knob(struct screen *s, struct win *w, int x, int y)
{
	struct rect c;
	int pos, len;

	if (s->mode == M_VKNOB) {
		gadget_rect(s, w, G_VCONT, &c);
		knob(c.h, w->vtotal, w->vvis, w->vtop, &pos, &len);
		pos = y - c.y - s->knob_off;
		if (c.h > len)
			scroll_to(s, w, (int)((long)pos * (w->vtotal - w->vvis) / (c.h - len)),
				  w->htop);
	} else {
		gadget_rect(s, w, G_HCONT, &c);
		knob(c.w, w->htotal, w->hvis, w->htop, &pos, &len);
		pos = x - c.x - s->knob_off;
		if (c.w > len)
			scroll_to(s, w, w->vtop,
				  (int)((long)pos * (w->htotal - w->hvis) / (c.w - len)));
	}
}

int wm_busy(const struct screen *s)
{
	return s->mode != M_NONE;
}

void wm_motion(struct screen *s, int x, int y, uint64_t now)
{
	struct win *w = s->grab;

	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;
	if (x >= s->w)
		x = s->w - 1;
	if (y >= s->h)
		y = s->h - 1;
	if (x == s->mx && y == s->my)
		return;
	damage_pointer(s);
	s->mx = x;
	s->my = y;
	damage_pointer(s);
	(void)now;

	switch (s->mode) {
	case M_DRAG: {
		struct rect r = s->grab_start;

		r.x = x - s->grab_dx;
		r.y = y - s->grab_dy;
		if (r.y < 0)
			r.y = 0;
		if (r.x + r.w > s->w)
			r.x = s->w - r.w;
		if (r.y + r.h > s->h)
			r.y = s->h - r.h;
		if (r.x < 0)
			r.x = 0;
		set_outline(s, &r);
		break;
	}
	case M_SIZE: {
		struct rect r = s->grab_start;

		r.w = x - r.x + s->grab_dx;
		r.h = y - r.y + s->grab_dy;
		clamp_size(w, &r);
		if (r.x + r.w > s->w)
			r.w = s->w - r.x;
		if (r.y + r.h > s->h)
			r.h = s->h - r.y;
		set_outline(s, &r);
		break;
	}
	case M_VKNOB:
	case M_HKNOB:
		drag_knob(s, w, x - w->r.x, y - w->r.y);
		break;
	case M_GADGET: {
		struct rect g;
		int in = gadget_rect(s, w, w->pressed, &g) &&
			 rect_contains(&g, x - w->r.x, y - w->r.y);

		if (in != w->pressed_in) {
			w->pressed_in = in;
			win_refresh_frame(s, w);
		}
		break;
	}
	case M_BARDEPTH: {
		struct rect g = rect_make(s->w - DEPTH_W, 0, DEPTH_W, s->barh - 1);
		int in = rect_contains(&g, x, y);

		if (in != s->bar_depth_pressed) {
			s->bar_depth_pressed = in;
			screen_draw_bar(s);
		}
		break;
	}
	case M_CONTENT:
		if (w->ops && w->ops->motion)
			w->ops->motion(w, x - w->r.x - w->bl, y - w->r.y - w->bt);
		break;
	default:
		w = s->active;
		if (w && w->wants_motion && w->ops && w->ops->motion)
			w->ops->motion(w, x - w->r.x - w->bl, y - w->r.y - w->bt);
		break;
	}
}

static void button_down(struct screen *s, int button, unsigned int qual, uint64_t now)
{
	struct win *w = win_at(s, s->mx, s->my);
	int x, y, g;

	if (!w) {
		struct rect gd = rect_make(s->w - DEPTH_W, 0, DEPTH_W, s->barh - 1);

		if (button == 1 && rect_contains(&gd, s->mx, s->my)) {
			s->mode = M_BARDEPTH;
			s->bar_depth_pressed = 1;
			screen_draw_bar(s);
		}
		return;
	}
	if (button == 1)
		win_activate(s, w);
	x = s->mx - w->r.x;
	y = s->my - w->r.y;
	g = gadget_at(s, w, x, y);
	if (button != 1) {
		if (g == G_CONTENT && w->ops && w->ops->button)
			w->ops->button(w, x - w->bl, y - w->bt, button, 1, qual);
		return;
	}
	s->grab = w;
	switch (g) {
	case G_CLOSE:
	case G_DEPTH:
	case G_ZOOM:
	case G_UP:
	case G_DOWN:
	case G_LEFT:
	case G_RIGHT:
		s->mode = M_GADGET;
		w->pressed = g;
		w->pressed_in = 1;
		win_refresh_frame(s, w);
		if (g >= G_UP) {
			step(s, w, g);
			s->repeat_at = now + REPEAT_FIRST;
		}
		break;
	case G_DRAG:
		s->mode = M_DRAG;
		s->grab_start = w->r;
		s->grab_dx = x;
		s->grab_dy = y;
		set_outline(s, &w->r);
		break;
	case G_SIZE:
		s->mode = M_SIZE;
		s->grab_start = w->r;
		s->grab_dx = w->r.w - x;
		s->grab_dy = w->r.h - y;
		set_outline(s, &w->r);
		break;
	case G_VCONT:
	case G_HCONT:
		page(s, w, g, x, y);
		if (s->mode == M_NONE)
			s->grab = NULL;
		break;
	case G_CONTENT:
		s->mode = M_CONTENT;
		if (w->ops && w->ops->button)
			w->ops->button(w, x - w->bl, y - w->bt, 1, 1, qual);
		break;
	default:
		s->grab = NULL;
		break;
	}
}

static void button_up(struct screen *s, int button, unsigned int qual)
{
	struct win *w = s->grab;
	int mode = s->mode;

	if (button != 1) {
		w = win_at(s, s->mx, s->my);
		if (w && w->ops && w->ops->button)
			w->ops->button(w, s->mx - w->r.x - w->bl, s->my - w->r.y - w->bt,
				       button, 0, qual);
		return;
	}
	s->mode = M_NONE;
	s->grab = NULL;
	switch (mode) {
	case M_BARDEPTH:
		if (s->bar_depth_pressed) {
			s->bar_depth_pressed = 0;
			screen_draw_bar(s);
			if (s->bar_depth)
				s->bar_depth(s);
		}
		break;
	case M_GADGET: {
		int g = w->pressed, in = w->pressed_in;

		w->pressed = G_NONE;
		w->pressed_in = 0;
		win_refresh_frame(s, w);
		if (!in)
			break;
		if (g == G_CLOSE) {
			if (w->ops && w->ops->close)
				w->ops->close(w);
		} else if (g == G_DEPTH) {
			if (s->top == w || (qual & QUAL_SHIFT))
				win_to_back(s, w);
			else
				win_to_front(s, w);
		} else if (g == G_ZOOM) {
			struct rect z = w->zoom;

			w->zoom = w->r;
			win_move_size(s, w, &z);
		}
		break;
	}
	case M_DRAG:
	case M_SIZE: {
		struct rect r = s->outline;

		set_outline(s, NULL);
		if (r.x != w->r.x || r.y != w->r.y || r.w != w->r.w || r.h != w->r.h)
			win_move_size(s, w, &r);
		break;
	}
	case M_CONTENT:
		if (w->ops && w->ops->button)
			w->ops->button(w, s->mx - w->r.x - w->bl, s->my - w->r.y - w->bt,
				       1, 0, qual);
		break;
	}
}

int wm_button(struct screen *s, int button, int down, unsigned int qual, uint64_t now)
{
	if (down)
		button_down(s, button, qual, now);
	else
		button_up(s, button, qual);
	return 1;
}

void wm_key(struct screen *s, const struct keyev *k)
{
	struct win *w = s->active;

	if (w && w->ops && w->ops->key)
		w->ops->key(w, k);
}

void wm_tick(struct screen *s, uint64_t now)
{
	struct win *w = s->grab;

	if (s->mode != M_GADGET || !w || w->pressed < G_UP || !w->pressed_in)
		return;
	if (now >= s->repeat_at) {
		step(s, w, w->pressed);
		s->repeat_at = now + REPEAT_NEXT;
	}
}
