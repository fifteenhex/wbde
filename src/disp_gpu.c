/*
 * The GPU display: the card composes the screen.
 *
 * Every layer wbde draws in RAM - the screen's own bitmap (title bar), each
 * window's backing store (frame and content: Workbench's backdrop with its
 * pattern and icons is one of them), each overlay (menus, a dragged icon) -
 * has an R8 texture of pens in VRAM.  What is drawn into a bitmap is marked
 * dirty there (gfx.c) and only those rectangles cross the bus, a byte a
 * pixel, before the flush draws.  A flush then draws, for each damaged
 * rectangle, the layers that show in it as textured rectangles into the one
 * 32-bit scan-out buffer, through a 256-entry palette texture
 * (smolgpu's sgpu_pal_prog): moving or uncovering a window, opening a menu
 * over something, scrolling the stacking order, all cost a few hundred
 * dwords of command stream and no pixels on the bus.
 *
 * Why 32-bit scan-out with the palette in the shader, not an 8-bit target
 * through the CRTC's LUT: the uploads are the same R8 bytes either way, and
 * the scan-out is the card reading its own VRAM, which never touches the
 * Mediator; but 32 bits lets a window hold true colour (a G3's frame, a GPU
 * client) without taking pens from Workbench, and wbde no longer owns the
 * CRTC's gamma table, so nothing is left behind in it when wbde goes.
 *
 * The pointer is the hardware cursor when the kernel takes one: moving it
 * is one ioctl and draws nothing.  WBDE_GPU_CURSOR=0 draws it as a layer.
 *
 * Windows may also show a buffer somebody else owns (disp_attach: a dma-buf
 * from a client, e.g. the frame a G3 card writes into VRAM over PCI): the
 * card draws it into the window's inner area, scaled to fit, and wbde never
 * touches its pixels.
 *
 * Everything here goes through smolgpu (sgpu.h); nothing knows which card.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sgpu.h>

#include "disp.h"
#include "wm.h"

#define MAXQUADS	1024		/* per flush; more and the flush redraws everything */
#define MAXBATCH	256
#define NEEDED_CAPS	(SGPU_CAP_TEXTURES | SGPU_CAP_RETAINED | SGPU_CAP_TEX_RECT)

/*
 * A bitmap's texture (bm->gpu), with a copy in RAM of what the texture
 * holds: a dirty rectangle is compared against it and only the longwords
 * that differ go over the bus.  Workbench repaints a whole drawer (or the
 * backdrop) to change one icon's selection, and clients repaint whole
 * windows for a few changed pixels; comparing in RAM costs a fraction of
 * storing across the Mediator.
 */
struct gtex {
	sgpu_tex t;
	int w, h;
	uint8_t *shadow;		/* bm->stride a row, like the bitmap */
};

/* a window's shared content (win->ext) */
struct ext {
	sgpu_tex t;
	int format;			/* DISP_BUF_* */
	int w, h;
};

enum { PROG_PAL, PROG_BLIT };

struct batch {
	const sgpu_tex *tex;
	const sgpu_tex *pal;		/* PROG_PAL: the palette, or the complemented one */
	int prog, key;
	unsigned int first, count;
};

struct gpriv {
	sgpu_ctx s;
	sgpu_prog prog[2];
	int u_tex, u_pal, u_key;
	sgpu_tex pal, xpal;
	uint32_t colours[256];
	int xormask;			/* what xpal was built for; -1 none yet */
	int pal_changed;		/* everything must be redrawn */
	unsigned int palgen;		/* bumped on every palette change */
	sgpu_vbuf vb[SGPU_UPDATE_SLOTS];
	sgpu_vertex_2d *verts;
	unsigned int nverts;
	struct batch batch[MAXBATCH];
	unsigned int nbatch;
	int overflow;
	int lw, lh;			/* the logical screen */
	int mw, mh;			/* the display */
	sgpu_tex pen0;			/* 1x1 XRGB black: the border round a smaller screen */
	unsigned int viewgen;		/* the view the border was drawn for */
	int clear;			/* draw the border this flush */
	int full;			/* WBDE_GPU_FULL: redraw everything every flush */
	/* the hardware cursor */
	int cursor;			/* it works and is in use */
	const struct bm *cur_bm;
	int cur_hx, cur_hy, cur_key, cur_x, cur_y, cur_shown;
	unsigned int cur_palgen;	/* the palette the image was made with */
	/* what the flush cost */
	long up_bytes, up_cmp, t_create, t_cursor;
	unsigned int up_rects, ntex, ncreate;
};

static struct gpriv *G;		/* for the free hooks */
static struct disp_view G_view;	/* this flush's view */

static long us_between(const struct timespec *a, const struct timespec *b)
{
	return (long)((b->tv_sec - a->tv_sec) * 1000000 + (b->tv_nsec - a->tv_nsec) / 1000);
}

/* ------------------------------------------------------------------ */
/* textures for bitmaps                                                */
/* ------------------------------------------------------------------ */

static void gtex_free(struct bm *bm)
{
	struct gtex *g = bm->gpu;

	if (!g || !G)
		return;
	sgpu_tex_free(&G->s, &g->t);
	free(g->shadow);
	free(g);
	bm->gpu = NULL;
	G->ntex--;
}

/* the bitmap's texture, made if need be, with everything drawn since sent */
static struct gtex *gtex_sync(struct gpriv *p, struct bm *bm)
{
	struct gtex *g = bm->gpu;
	int i;

	if (!bm->px)
		return NULL;
	if (g && (g->w != bm->w || g->h != bm->h))
		gtex_free(bm);
	g = bm->gpu;
	if (!g) {
		struct timespec a, b;

		clock_gettime(CLOCK_MONOTONIC, &a);
		g = calloc(1, sizeof(*g));
		if (!g)
			return NULL;
		g->shadow = malloc((size_t)bm->stride * bm->h);
		if (!g->shadow || sgpu_tex_create(&p->s, &g->t, (unsigned)bm->w, (unsigned)bm->h,
						  SGPU_TEX_R8)) {
			free(g->shadow);
			free(g);
			return NULL;
		}
		sgpu_tex_filter(&g->t, false, true);
		g->w = bm->w;
		g->h = bm->h;
		bm->gpu = g;
		p->ntex++;
		/* all of it once; the shadow starts as what was sent */
		sgpu_tex_write_rect(&p->s, &g->t, 0, 0, (unsigned)bm->w, (unsigned)bm->h, bm->px,
				    (unsigned)bm->stride);
		memcpy(g->shadow, bm->px, (size_t)bm->stride * bm->h);
		p->up_bytes += (long)bm->w * bm->h;
		p->up_cmp += (long)bm->w * bm->h;
		p->up_rects++;
		bm->ndirty = 0;
		clock_gettime(CLOCK_MONOTONIC, &b);
		p->t_create += us_between(&a, &b);
		p->ncreate++;
	}
	for (i = 0; i < bm->ndirty; i++) {
		const struct rect *r = &bm->dirty[i];
		size_t at = (size_t)r->y * bm->stride + r->x;
		long n;

		n = sgpu_tex_update_rect(&p->s, &g->t, (unsigned)r->x, (unsigned)r->y,
					 (unsigned)r->w, (unsigned)r->h, bm->px + at,
					 (unsigned)bm->stride, g->shadow + at, (unsigned)bm->stride);
		if (n > 0)
			p->up_bytes += n;
		p->up_cmp += (long)r->w * r->h;
		p->up_rects++;
	}
	bm->ndirty = 0;
	return g;
}

static void ext_free(struct win *w)
{
	struct ext *e = w->ext;

	if (!e || !G)
		return;
	sgpu_tex_free(&G->s, &e->t);
	free(e);
	w->ext = NULL;
}

/* ------------------------------------------------------------------ */
/* the palette and the cursor                                          */
/* ------------------------------------------------------------------ */

static void gpu_palette(struct disp *d, const uint8_t pal[256][3])
{
	struct gpriv *p = d->priv;
	uint32_t c[256];
	int i;

	for (i = 0; i < 256; i++)
		c[i] = ((uint32_t)pal[i][0] << 16) | ((uint32_t)pal[i][1] << 8) | pal[i][2];
	if (!memcmp(c, p->colours, sizeof(c)) && p->xormask >= 0)
		return;
	memcpy(p->colours, c, sizeof(c));
	sgpu_tex_write(&p->s, &p->pal, p->colours, sizeof(p->colours));
	p->xormask = -1;		/* the complemented one follows */
	p->pal_changed = 1;
	p->palgen++;			/* and the cursor's colours */
}

static void xor_palette(struct gpriv *p, int mask)
{
	uint32_t c[256];
	int i;

	for (i = 0; i < 256; i++)
		c[i] = p->colours[(i ^ mask) & 255];
	sgpu_tex_write(&p->s, &p->xpal, c, sizeof(c));
	p->xormask = mask;
}

/* the pointer as the hardware cursor: a new image when it or the palette changed */
static void cursor_update(struct gpriv *p, struct disp *d, struct screen *s)
{
	const struct bm *b = s->ptr;
	int x, y;

	if (!b || s->ptr_hidden) {
		if (p->cur_shown)
			sgpu_cursor_hide(&p->s);
		p->cur_shown = 0;
		return;
	}
	if (b != p->cur_bm || s->ptr_hx != p->cur_hx || s->ptr_hy != p->cur_hy ||
	    s->ptr_key != p->cur_key || p->cur_palgen != p->palgen) {
		uint32_t img[SGPU_CURSOR_SIZE * SGPU_CURSOR_SIZE];
		int h = b->h * d->scale;

		memset(img, 0, sizeof(img));
		for (y = 0; y < h && y < SGPU_CURSOR_SIZE; y++)
			for (x = 0; x < b->w && x < SGPU_CURSOR_SIZE; x++) {
				int pen = b->px[(y / d->scale) * b->stride + x];

				if (pen != s->ptr_key)
					img[y * SGPU_CURSOR_SIZE + x] = 0xff000000u | p->colours[pen];
			}
		if (sgpu_cursor_set(&p->s, img, SGPU_CURSOR_SIZE, SGPU_CURSOR_SIZE, SGPU_CURSOR_SIZE)) {
			fprintf(stderr, "wbde: the hardware cursor went away\n");
			return;
		}
		p->cur_bm = b;
		p->cur_hx = s->ptr_hx;
		p->cur_hy = s->ptr_hy;
		p->cur_key = s->ptr_key;
		p->cur_palgen = p->palgen;
		p->cur_shown = 1;
		p->cur_x = p->cur_y = -10000;
	}
	x = d->view.ox + (int)(s->mx * d->view.sx) - s->ptr_hx;
	y = d->view.oy + (int)(s->my * d->view.sy) - s->ptr_hy * d->scale;
	if (x != p->cur_x || y != p->cur_y) {
		sgpu_cursor_move(&p->s, x, y);
		p->cur_x = x;
		p->cur_y = y;
	}
}

/* ------------------------------------------------------------------ */
/* rectangles                                                          */
/* ------------------------------------------------------------------ */

static void batch_begin(struct gpriv *p, int prog, const sgpu_tex *tex, const sgpu_tex *pal,
			int key)
{
	struct batch *b;

	if (p->nbatch == MAXBATCH) {
		p->overflow = 1;
		return;
	}
	b = &p->batch[p->nbatch];
	b->prog = prog;
	b->tex = tex;
	b->pal = pal;
	b->key = key;
	b->first = p->nverts;
	b->count = 0;
}

static void batch_end(struct gpriv *p)
{
	struct batch *b = &p->batch[p->nbatch];

	if (p->nbatch < MAXBATCH && p->nverts > b->first) {
		b->count = p->nverts - b->first;
		p->nbatch++;
	}
}

/*
 * The screen rectangle r showing texels from (u, v) of a tw x th texture,
 * scaled by (sx, sy) texels a pixel.
 */
static void quad(struct gpriv *p, const struct rect *r, float u, float v, float sx, float sy,
		 float tw, float th)
{
	static const float c[6][2] = { {0,0}, {1,0}, {1,1}, {0,0}, {1,1}, {0,1} };
	int i;

	if (p->nverts + 6 > MAXQUADS * 6) {
		p->overflow = 1;
		return;
	}
	for (i = 0; i < 6; i++) {
		sgpu_vertex_2d *q = &p->verts[p->nverts++];

		q->pos[0] = (float)r->x + c[i][0] * r->w;
		q->pos[1] = (float)r->y + c[i][1] * r->h;
		q->uv[0] = (u + c[i][0] * r->w * sx) / tw;
		q->uv[1] = (v + c[i][1] * r->h * sy) / th;
	}
}

/* every rectangle of `rects` that the layer at (x, y) shows in, from its texture */
static void layer(struct gpriv *p, struct bm *bm, int x, int y, const struct rect *rects,
		  int n, const signed char *skip, const sgpu_tex *pal, int key)
{
	struct gtex *g = bm->gpu;
	struct rect lr = { x, y, bm->w, bm->h }, c;
	int i;

	if (!g)
		return;
	batch_begin(p, PROG_PAL, &g->t, pal, key);
	for (i = 0; i < n; i++)
		if (!(skip && skip[i]) && rect_intersect(&c, &lr, &rects[i]))
			quad(p, &c, (float)(c.x - x), (float)(c.y - y), 1, 1, (float)g->w, (float)g->h);
	batch_end(p);
}

/* a window's shared content over its inner area, scaled to fit */
static void ext_layer(struct gpriv *p, struct win *w, const struct rect *rects, int n,
		      const signed char *skip)
{
	struct ext *e = w->ext;
	struct rect in, c;
	float sx, sy;
	int i;

	win_inner(w, &in);
	in.x += w->r.x;
	in.y += w->r.y;
	if (in.w <= 0 || in.h <= 0)
		return;
	sx = (float)e->w / in.w;
	sy = (float)e->h / in.h;
	if (e->format == DISP_BUF_PENS)
		batch_begin(p, PROG_PAL, &e->t, &p->pal, -1);
	else
		batch_begin(p, PROG_BLIT, &e->t, NULL, -1);
	for (i = 0; i < n; i++)
		if (!(skip && skip[i]) && rect_intersect(&c, &in, &rects[i]))
			quad(p, &c, (c.x - in.x) * sx, (c.y - in.y) * sy, sx, sy, (float)e->w,
			     (float)e->h);
	batch_end(p);
}

/*
 * All the layers for rectangles r[0..n), bottom up, as composite() in wm.c
 * does them: nothing under the topmost window that covers a rectangle
 * whole.  `pal` is the palette or the complemented one (the XOR outline).
 */
static void layers(struct gpriv *p, struct screen *s, const struct rect *r, int n,
		   const sgpu_tex *pal, int with_ext)
{
	signed char skip[DAMAGE_MAX * 4];
	struct win *cover[DAMAGE_MAX * 4], *w;
	struct overlay *o;
	int i, any_root = 0;

	for (i = 0; i < n; i++) {
		cover[i] = screen_cover(s, &r[i]);
		skip[i] = cover[i] != NULL;
		any_root |= !cover[i];
	}
	if (any_root)
		layer(p, &s->root, 0, 0, r, n, skip, pal, -1);
	for (w = s->bottom; w; w = w->above) {
		if (w->dead)
			continue;
		/* below its cover a rectangle shows nothing; from it up, everything */
		for (i = 0; i < n; i++)
			if (cover[i] == w)
				skip[i] = 0;
		layer(p, &w->bm, w->r.x, w->r.y, r, n, skip, pal, -1);
		if (w->ext && with_ext)
			ext_layer(p, w, r, n, skip);
	}
	for (o = s->overlays; o; o = o->next)
		layer(p, o->bm, o->x, o->y, r, n, NULL, pal, o->key);
}

/* the rubber band's four edges where they cross the damage, as composite() has them */
static int outline_pieces(const struct screen *s, const struct rect *d, int nd, struct rect *out)
{
	const struct rect *b = &s->outline;
	struct rect e[4], c;
	int i, k, n = 0;

	e[0] = rect_make(b->x, b->y, b->w, 1);
	e[1] = rect_make(b->x, b->y + b->h - 1, b->w, 1);
	e[2] = rect_make(b->x, b->y + 1, 1, b->h - 2);
	e[3] = rect_make(b->x + b->w - 1, b->y + 1, 1, b->h - 2);
	for (k = 0; k < 4; k++)
		for (i = 0; i < nd && n < DAMAGE_MAX * 4; i++)
			if (rect_intersect(&c, &e[k], &d[i]))
				out[n++] = c;
	return n;
}

/* the whole display in screen coordinates: the border (pen 0) under everything */
static void border(struct gpriv *p, const struct disp_view *v)
{
	static const float c[6][2] = { {0,0}, {1,0}, {1,1}, {0,0}, {1,1}, {0,1} };
	float x0 = -v->ox / v->sx, y0 = -v->oy / v->sy;
	float w = p->mw / v->sx, h = p->mh / v->sy;
	int i;

	batch_begin(p, PROG_BLIT, &p->pen0, NULL, -1);
	for (i = 0; i < 6 && p->nverts + 6 <= MAXQUADS * 6; i++) {
		sgpu_vertex_2d *q = &p->verts[p->nverts++];

		q->pos[0] = x0 + c[i][0] * w;
		q->pos[1] = y0 + c[i][1] * h;
		q->uv[0] = 0.5f;
		q->uv[1] = 0.5f;
	}
	batch_end(p);
}

static void build(struct gpriv *p, struct screen *s, const struct rect *d, int nd)
{
	struct rect x[DAMAGE_MAX * 4];
	int nx;

	p->nverts = p->nbatch = 0;
	p->overflow = 0;
	if (p->clear)
		border(p, &G_view);
	layers(p, s, d, nd, &p->pal, 1);
	if (s->outline_on && (nx = outline_pieces(s, d, nd, x)) > 0)
		layers(p, s, x, nx, &p->xpal, 0);
	if (!p->cursor && s->ptr && !s->ptr_hidden)
		layer(p, s->ptr, s->mx - s->ptr_hx, s->my - s->ptr_hy, d, nd, NULL, &p->pal,
		      s->ptr_key);
}

/* ------------------------------------------------------------------ */
/* the flush                                                           */
/* ------------------------------------------------------------------ */

static void draw(struct gpriv *p, const struct disp_view *v)
{
	sgpu_vbuf *vb = &p->vb[sgpu_update_slot(&p->s)];
	float m[4][4];
	int prog = -1;
	unsigned int i;

	sgpu_update_begin(&p->s);
	sgpu_vbuf_write(&p->s, vb, 0, p->verts, p->nverts);
	/* screen pixels to the display through the view: (ox + x sx, oy + y sy) */
	sgpu_m_ortho_px(m, (float)p->mw, (float)p->mh);
	m[0][0] = 2.0f * v->sx / p->mw;
	m[0][3] = 2.0f * v->ox / p->mw - 1.0f;
	m[1][1] = -2.0f * v->sy / p->mh;
	m[1][3] = 1.0f - 2.0f * v->oy / p->mh;
	for (i = 0; i < p->nbatch; i++) {
		const struct batch *b = &p->batch[i];

		if (b->prog != prog) {
			prog = b->prog;
			sgpu_use(&p->s, &p->prog[prog]);
			sgpu_set_mat4(&p->s, p->s.u_mvp, m);
		}
		sgpu_set_tex(&p->s, p->u_tex, b->tex);
		if (prog == PROG_PAL) {
			sgpu_set_tex(&p->s, p->u_pal, b->pal);
			sgpu_set_float4(&p->s, p->u_key, b->key < 0 ? -1.0f : b->key / 255.0f, 0, 0, 0);
		}
		sgpu_draw_range(&p->s, vb, SGPU_PRIM_TRIANGLES, b->first, b->count);
	}
	sgpu_update_end(&p->s);
}

static void gpu_compose(struct disp *d, struct screen *s)
{
	struct gpriv *p = d->priv;
	struct timespec t0, t1, t2, t3;
	struct win *w;
	struct overlay *o;
	struct rect all = { 0, 0, s->w, s->h };
	long area = 0;
	int i;

	if (sgpu_error(&p->s))
		return;
	G_view = d->view;
	if (!G_view.src || G_view.sx <= 0) {
		G_view.sx = 1;
		G_view.sy = (float)d->scale;
		G_view.ox = G_view.oy = 0;
	}
	if (d->view.gen != p->viewgen) {
		p->viewgen = d->view.gen;
		p->clear = 1;
		screen_damage_all(s);
	}
	clock_gettime(CLOCK_MONOTONIC, &t0);
	p->up_bytes = 0;
	p->up_cmp = 0;
	p->up_rects = 0;
	p->t_create = p->t_cursor = 0;
	p->ncreate = 0;
	/* what was drawn into the layers, to their textures */
	gtex_sync(p, &s->root);
	for (w = s->bottom; w; w = w->above)
		if (!w->dead)
			gtex_sync(p, &w->bm);
	for (o = s->overlays; o; o = o->next)
		gtex_sync(p, o->bm);
	if (!p->cursor && s->ptr)
		gtex_sync(p, s->ptr);
	if (p->xormask != s->xormask)
		xor_palette(p, s->xormask);
	if (p->pal_changed) {
		p->pal_changed = 0;
		p->clear = 1;
		screen_damage_all(s);
	}
	if (p->cursor) {
		struct timespec a, b;

		clock_gettime(CLOCK_MONOTONIC, &a);
		cursor_update(p, d, s);
		clock_gettime(CLOCK_MONOTONIC, &b);
		p->t_cursor = us_between(&a, &b);
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	if (!s->ndamage)
		return;
	if (p->full)
		screen_damage_all(s);
	for (i = 0; i < s->ndamage; i++)
		area += (long)s->damage[i].w * s->damage[i].h;
	build(p, s, s->damage, s->ndamage);
	if (p->overflow)		/* too many pieces: the whole screen, once a layer */
		build(p, s, &all, 1);
	p->clear = 0;
	clock_gettime(CLOCK_MONOTONIC, &t2);
	draw(p, &G_view);
	clock_gettime(CLOCK_MONOTONIC, &t3);
	s->ndamage = 0;
	if (sgpu_error(&p->s))
		fprintf(stderr, "wbde: the GPU display failed: %s (%d)\n", p->s.err_what, p->s.err);
	snprintf(d->timing, sizeof(d->timing),
		 "flush %ld px: upload %ld B of %ld in %u rects %ld us, %u quads %u draws %ld us, "
		 "submit %ld us, gpu %.0f us (new textures %u %ld us, cursor %ld us)", area,
		 p->up_bytes, p->up_cmp, p->up_rects, us_between(&t0, &t1),
		 p->nverts / 6, p->nbatch,
		 us_between(&t1, &t2), us_between(&t2, &t3), sgpu_update_gpu_time(&p->s) * 1e6,
		 p->ncreate, p->t_create, p->t_cursor);
}

/* ------------------------------------------------------------------ */
/* screenshots: what the card shows, checked against the CPU's picture  */
/* ------------------------------------------------------------------ */

static int in_ext(struct screen *s, int x, int y)
{
	struct win *w, *top = NULL;

	for (w = s->bottom; w; w = w->above)
		if (!w->dead && rect_contains(&w->r, x, y))
			top = w;
	if (top && top->ext) {
		struct rect in;

		win_inner(top, &in);
		return rect_contains(&in, x - top->r.x, y - top->r.y);
	}
	return 0;
}

static int gpu_shot(struct disp *d, struct screen *s, const char *path)
{
	struct gpriv *p = d->priv;
	struct rect all = { 0, 0, s->w, s->h };
	struct bm *ref = d->view.src ? d->view.src : &d->fb;
	unsigned int stride;
	const uint32_t *px;
	uint32_t *img;
	long bad = 0, total = 0;
	int W = (int)sgpu_width(&p->s), H = (int)sgpu_height(&p->s), x, y;
	FILE *f;
	struct timespec t0, t1;

	img = malloc((size_t)W * H * 4);
	if (!img)
		return -ENOMEM;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	px = sgpu_readback_begin(&p->s, &stride);
	for (y = 0; y < H; y++)
		memcpy(img + (size_t)y * W, px + (size_t)y * stride, (size_t)W * 4);
	sgpu_readback_end(&p->s);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	/* the CPU's composition of the same screen, through the same view, as the reference */
	screen_compose(s, ref, &all, !p->cursor);
	for (y = 0; y < H; y++)
		for (x = 0; x < W; x++) {
			int sx, sy, in = disp_view_map(d, x, y, &sx, &sy);
			uint32_t want = in ? p->colours[ref->px[(size_t)sy * ref->stride + sx]] : 0;
			uint32_t got = img[(size_t)y * W + x] & 0xffffff;

			if (in && in_ext(s, sx, sy))
				continue;
			total++;
			if (got != want && bad++ < 5)
				fprintf(stderr, "wbde: shot: (%d,%d) is %06x, the CPU says %06x\n", x, y,
					got, want);
		}
	fprintf(stderr, "wbde: shot: read %dx%d back from VRAM in %ld ms; %ld of %ld pixels differ "
		"from the CPU's composition%s\n", W, H, us_between(&t0, &t1) / 1000, bad, total,
		p->cursor ? " (pointer: hardware cursor, painted in)" : "");
	/* the hardware cursor is not in the scan-out: paint it in for the eye */
	if (p->cursor && s->ptr && !s->ptr_hidden) {
		const struct bm *b = s->ptr;
		int cx = d->view.ox + (int)(s->mx * d->view.sx) - s->ptr_hx;
		int cy = d->view.oy + (int)(s->my * d->view.sy) - s->ptr_hy * d->scale;

		for (y = 0; y < b->h * d->scale; y++)
			for (x = 0; x < b->w; x++) {
				int X = cx + x, Y = cy + y;
				int pen = b->px[(y / d->scale) * b->stride + x];

				if (pen != s->ptr_key && X >= 0 && Y >= 0 && X < W && Y < H)
					img[(size_t)Y * W + X] = p->colours[pen];
			}
	}
	f = fopen(path, "wb");
	if (!f) {
		free(img);
		return -errno;
	}
	fprintf(f, "P6\n%d %d\n255\n", W, H);
	for (y = 0; y < H; y++) {
		uint8_t row[3 * 2048];

		for (x = 0; x < W && x < 2048; x++) {
			uint32_t c = img[(size_t)y * W + x];

			row[3 * x] = (uint8_t)(c >> 16);
			row[3 * x + 1] = (uint8_t)(c >> 8);
			row[3 * x + 2] = (uint8_t)c;
		}
		fwrite(row, 3, (size_t)x, f);
	}
	fclose(f);
	free(img);
	return 0;
}

/* ------------------------------------------------------------------ */
/* shared buffers as window content                                    */
/* ------------------------------------------------------------------ */

static int gpu_attach(struct disp *d, struct win *w, int fd, const struct disp_buf *b)
{
	struct gpriv *p = d->priv;
	struct ext *e;
	unsigned int fmt, bpp;
	int ret;

	if (b->format == DISP_BUF_PENS) {
		fmt = SGPU_TEX_R8;
		bpp = 1;
	} else if (b->format == DISP_BUF_XRGB8888) {
		fmt = SGPU_TEX_XRGB8;
		bpp = 4;
	} else {
		return -EINVAL;
	}
	if (b->pitch % bpp || b->width <= 0 || b->height <= 0)
		return -EINVAL;
	e = calloc(1, sizeof(*e));
	if (!e)
		return -ENOMEM;
	ret = sgpu_tex_import(&p->s, &e->t, fd, (unsigned)b->width, (unsigned)b->height, fmt,
			      (unsigned)b->pitch / bpp, b->offset);
	if (ret) {
		free(e);
		return ret;
	}
	sgpu_tex_filter(&e->t, getenv("WBDE_GPU_SMOOTH") != NULL, true);
	e->format = b->format;
	e->w = b->width;
	e->h = b->height;
	if (w->ext)
		ext_free(w);
	w->ext = e;
	return 0;
}

static void gpu_detach(struct disp *d, struct win *w)
{
	(void)d;
	ext_free(w);
}

/*
 * A pens texture for the client to draw into itself: its stores cross the
 * bus once, straight into VRAM, and wbde copies nothing.  It starts as pen
 * 0 (Workbench's background), written here once a row at a time.
 */
static int gpu_share(struct disp *d, struct win *w, int format, int width, int height, int *fd,
		     struct disp_buf *b, uint32_t *size)
{
	struct gpriv *p = d->priv;
	struct ext *e;
	int ret;

	if (format != DISP_BUF_PENS || width <= 0 || height <= 0)
		return -EINVAL;
	e = calloc(1, sizeof(*e));
	if (!e)
		return -ENOMEM;
	if (sgpu_tex_create(&p->s, &e->t, (unsigned)width, (unsigned)height, SGPU_TEX_R8)) {
		fprintf(stderr, "wbde: smolgpu: %s (%d)\n", p->s.err_what, p->s.err);
		free(e);
		return -ENOMEM;
	}
	ret = sgpu_tex_export(&p->s, &e->t, fd);
	if (ret) {
		sgpu_tex_free(&p->s, &e->t);
		free(e);
		return ret;
	}
	sgpu_tex_filter(&e->t, false, true);
	{
		uint8_t *zero = calloc(1, (size_t)width);
		int y;

		for (y = 0; zero && y < height; y++)
			sgpu_tex_write_rect(&p->s, &e->t, 0, (unsigned)y, (unsigned)width, 1, zero,
					    (unsigned)width);
		free(zero);
	}
	e->format = DISP_BUF_PENS;
	e->w = width;
	e->h = height;
	b->format = DISP_BUF_PENS;
	b->width = width;
	b->height = height;
	b->pitch = (int)sgpu_tex_pitch_bytes(&e->t);
	b->offset = 0;
	*size = (uint32_t)b->pitch * (uint32_t)height;
	if (w->ext)
		ext_free(w);
	w->ext = e;
	return 0;
}

/* ------------------------------------------------------------------ */
/* open and close                                                      */
/* ------------------------------------------------------------------ */

static void gpu_close(struct disp *d)
{
	struct gpriv *p = d->priv;
	unsigned int i;

	if (!p)
		return;
	if (p->cursor)
		sgpu_cursor_hide(&p->s);
	for (i = 0; i < SGPU_UPDATE_SLOTS; i++)
		sgpu_vbuf_free(&p->s, &p->vb[i]);
	sgpu_close(&p->s);	/* every texture goes with the card's file */
	bm_free_hook = NULL;
	win_ext_free = NULL;
	G = NULL;
	free(p->verts);
	free(p);
	d->priv = NULL;
	bm_free(&d->fb);
}

static void gpu_emergency(struct disp *d)
{
	struct gpriv *p = d->priv;

	if (p && p->cursor)
		sgpu_cursor_hide(&p->s);
}

static int gpu_open(struct disp *d, const char *mode, const char *size)
{
	sgpu_config cfg = SGPU_CONFIG_DEFAULT;
	struct gpriv *p;
	unsigned int i;
	int mw = 0, mh = 0, w = 0, h = 0;
	const char *cur = getenv("WBDE_GPU_CURSOR");

	if ((sgpu_caps() & NEEDED_CAPS) != NEEDED_CAPS || !SGPU_HAVE_2D) {
		fprintf(stderr, "wbde: smolgpu's %s backend cannot compose (no textures or "
			"retained display)\n", SGPU_BACKEND);
		return -1;
	}
	p = calloc(1, sizeof(*p));
	if (!p)
		return -1;
	p->verts = malloc(sizeof(*p->verts) * MAXQUADS * 6);
	if (!p->verts) {
		free(p);
		return -1;
	}
	if (mode && sscanf(mode, "%dx%d", &mw, &mh) == 2) {
		cfg.w = (unsigned)mw;
		cfg.h = (unsigned)mh;
	}
	cfg.retained = true;
	cfg.quiet = true;
	cfg.name = "wbde";
	if (sgpu_open(&p->s, &cfg)) {
		fprintf(stderr, "wbde: smolgpu: %s (%d)\n", p->s.err_what ? p->s.err_what : "open",
			p->s.err);
		free(p->verts);
		free(p);
		return -1;
	}
	mw = (int)sgpu_width(&p->s);
	mh = (int)sgpu_height(&p->s);
	p->mw = d->mw = mw;
	p->mh = d->mh = mh;
	d->scale = 1;
	p->lw = mw;
	p->lh = mh;
	if (size && sscanf(size, "%dx%d", &w, &h) == 2 && w == mw && h > 0 && mh % h == 0) {
		d->scale = mh / h;
		p->lh = h;
	}
	d->priv = p;
	G = p;
	bm_free_hook = gtex_free;
	win_ext_free = ext_free;
	if (bm_alloc(&d->fb, p->lw, p->lh))
		goto fail;
	p->u_tex = sgpu_uniform(&p->s, "u_tex");
	p->u_pal = sgpu_uniform(&p->s, "u_pal");
	p->u_key = sgpu_uniform(&p->s, "u_key");
#if SGPU_HAVE_2D
	sgpu_prog_load(&p->s, &p->prog[PROG_PAL], &sgpu_pal_prog);
	sgpu_prog_load(&p->s, &p->prog[PROG_BLIT], &sgpu_blit_prog);
#endif
	sgpu_tex_create(&p->s, &p->pal, 256, 1, SGPU_TEX_XRGB8);
	sgpu_tex_create(&p->s, &p->xpal, 256, 1, SGPU_TEX_XRGB8);
	sgpu_tex_filter(&p->pal, false, true);
	sgpu_tex_filter(&p->xpal, false, true);
	{
		uint32_t black = 0;

		sgpu_tex_create(&p->s, &p->pen0, 1, 1, SGPU_TEX_XRGB8);
		sgpu_tex_write(&p->s, &p->pen0, &black, sizeof(black));
		sgpu_tex_filter(&p->pen0, false, true);
	}
	p->clear = 1;
	for (i = 0; i < SGPU_UPDATE_SLOTS; i++)
		sgpu_vbuf_create_own(&p->s, &p->vb[i], &sgpu_layout_2d, NULL, MAXQUADS * 6);
	p->xormask = -1;
	p->palgen = 1;
	p->full = getenv("WBDE_GPU_FULL") != NULL;
	/* an empty update shows the buffer (the mode set); then the cursor can be tried */
	sgpu_update_begin(&p->s);
	sgpu_update_end(&p->s);
	if (sgpu_error(&p->s)) {
		fprintf(stderr, "wbde: smolgpu: %s (%d)\n", p->s.err_what, p->s.err);
		goto fail;
	}
	if ((sgpu_caps() & SGPU_CAP_CURSOR) && !(cur && *cur == '0')) {
		uint32_t none = 0;

		p->cursor = sgpu_cursor_set(&p->s, &none, 1, 1, 1) == 0;
		if (!p->cursor)
			fprintf(stderr, "wbde: no hardware cursor; the pointer is a layer\n");
	}
	d->composes = 1;
	d->hw_pointer = p->cursor;
	snprintf(d->desc, sizeof(d->desc), "GPU (smolgpu %s, %s), %dx%d XRGB8888, screen %dx%d, %s",
		 SGPU_BACKEND, SGPU_CARD_NAME, mw, mh, p->lw, p->lh,
		 p->cursor ? "hardware cursor" : "pointer as a layer");
	return 0;
fail:
	gpu_close(d);
	return -1;
}

const struct disp_ops disp_gpu_ops = {
	.name = "gpu",
	.open = gpu_open,
	.palette = gpu_palette,
	.compose = gpu_compose,
	.shot = gpu_shot,
	.close = gpu_close,
	.emergency = gpu_emergency,
	.attach = gpu_attach,
	.detach = gpu_detach,
	.share = gpu_share,
};
