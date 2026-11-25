#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "font.h"

int bm_alloc(struct bm *bm, int w, int h)
{
	if (w < 1)
		w = 1;
	if (h < 1)
		h = 1;
	bm->w = w;
	bm->h = h;
	bm->stride = (w + 3) & ~3;
	bm->px = calloc((size_t)bm->stride, (size_t)h);
	return bm->px ? 0 : -1;
}

void bm_free(struct bm *bm)
{
	free(bm->px);
	bm->px = NULL;
	bm->w = bm->h = 0;
}

int rect_intersect(struct rect *out, const struct rect *a, const struct rect *b)
{
	int x0 = a->x > b->x ? a->x : b->x;
	int y0 = a->y > b->y ? a->y : b->y;
	int x1 = a->x + a->w < b->x + b->w ? a->x + a->w : b->x + b->w;
	int y1 = a->y + a->h < b->y + b->h ? a->y + a->h : b->y + b->h;

	out->x = x0;
	out->y = y0;
	out->w = x1 - x0;
	out->h = y1 - y0;
	if (out->w <= 0 || out->h <= 0) {
		out->w = out->h = 0;
		return 0;
	}
	return 1;
}

void rect_union(struct rect *into, const struct rect *with)
{
	int x0, y0, x1, y1;

	if (rect_empty(with))
		return;
	if (rect_empty(into)) {
		*into = *with;
		return;
	}
	x0 = into->x < with->x ? into->x : with->x;
	y0 = into->y < with->y ? into->y : with->y;
	x1 = into->x + into->w > with->x + with->w ? into->x + into->w : with->x + with->w;
	y1 = into->y + into->h > with->y + with->h ? into->y + into->h : with->y + with->h;
	into->x = x0;
	into->y = y0;
	into->w = x1 - x0;
	into->h = y1 - y0;
}

void rp_init(struct rp *rp, struct bm *bm)
{
	rp->bm = bm;
	rp->clip = rect_make(0, 0, bm->w, bm->h);
	rp->ox = rp->oy = 0;
}

void rp_clip(struct rp *rp, const struct rect *r)
{
	struct rect abs = { r->x + rp->ox, r->y + rp->oy, r->w, r->h };

	rect_intersect(&rp->clip, &rp->clip, &abs);
}

/* to bitmap coordinates, clipped; 0 if nothing is left */
static int clipped(const struct rp *rp, int *x, int *y, int *w, int *h)
{
	struct rect r = { *x + rp->ox, *y + rp->oy, *w, *h };

	if (!rect_intersect(&r, &r, &rp->clip))
		return 0;
	*x = r.x;
	*y = r.y;
	*w = r.w;
	*h = r.h;
	return 1;
}

void rp_fill(struct rp *rp, int x, int y, int w, int h, int pen)
{
	uint8_t *p;

	if (!clipped(rp, &x, &y, &w, &h))
		return;
	p = rp->bm->px + (size_t)y * rp->bm->stride + x;
	while (h--) {
		memset(p, pen, (size_t)w);
		p += rp->bm->stride;
	}
}

void rp_pixel(struct rp *rp, int x, int y, int pen)
{
	rp_fill(rp, x, y, 1, 1, pen);
}

void rp_hline(struct rp *rp, int x, int y, int w, int pen)
{
	rp_fill(rp, x, y, w, 1, pen);
}

void rp_vline(struct rp *rp, int x, int y, int h, int pen)
{
	rp_fill(rp, x, y, 1, h, pen);
}

void rp_box(struct rp *rp, int x, int y, int w, int h, int pen)
{
	rp_hline(rp, x, y, w, pen);
	rp_hline(rp, x, y + h - 1, w, pen);
	rp_vline(rp, x, y, h, pen);
	rp_vline(rp, x + w - 1, y, h, pen);
}

void rp_bevel(struct rp *rp, int x, int y, int w, int h, int shine, int shadow)
{
	rp_hline(rp, x, y, w - 1, shine);
	rp_vline(rp, x, y, h - 1, shine);
	rp_hline(rp, x + 1, y + h - 1, w - 1, shadow);
	rp_vline(rp, x + w - 1, y + 1, h - 1, shadow);
}

void rp_frame_button(struct rp *rp, int x, int y, int w, int h, int shine, int shadow)
{
	rp_hline(rp, x, y, w - 1, shine);
	rp_fill(rp, x, y, 2, h - 1, shine);
	rp_hline(rp, x + 1, y + h - 1, w - 1, shadow);
	rp_fill(rp, x + w - 2, y + 1, 2, h - 1, shadow);
	rp_pixel(rp, x + w - 1, y, shadow);
	rp_pixel(rp, x, y + h - 1, shine);
}

void rp_frame_ridge(struct rp *rp, int x, int y, int w, int h, int shine, int shadow)
{
	rp_frame_button(rp, x, y, w, h, shine, shadow);
	rp_frame_button(rp, x + 2, y + 1, w - 4, h - 2, shadow, shine);
}

void rp_xor_rect(struct rp *rp, int x, int y, int w, int h, int mask)
{
	uint8_t *p;
	int i;

	if (!clipped(rp, &x, &y, &w, &h))
		return;
	p = rp->bm->px + (size_t)y * rp->bm->stride + x;
	while (h--) {
		for (i = 0; i < w; i++)
			p[i] ^= (uint8_t)mask;
		p += rp->bm->stride;
	}
}

void rp_blit(struct rp *rp, int x, int y, const struct bm *src,
	     int sx, int sy, int w, int h, int key)
{
	int dx, dy, cw, ch, i;
	const uint8_t *s;
	uint8_t *d;

	/* clip against the source first */
	if (sx < 0) { x -= sx; w += sx; sx = 0; }
	if (sy < 0) { y -= sy; h += sy; sy = 0; }
	if (sx + w > src->w)
		w = src->w - sx;
	if (sy + h > src->h)
		h = src->h - sy;
	dx = x;
	dy = y;
	cw = w;
	ch = h;
	if (w <= 0 || h <= 0 || !clipped(rp, &dx, &dy, &cw, &ch))
		return;
	sx += dx - (x + rp->ox);
	sy += dy - (y + rp->oy);
	s = src->px + (size_t)sy * src->stride + sx;
	d = rp->bm->px + (size_t)dy * rp->bm->stride + dx;
	while (ch--) {
		if (key < 0) {
			memcpy(d, s, (size_t)cw);
		} else {
			for (i = 0; i < cw; i++)
				if (s[i] != key)
					d[i] = s[i];
		}
		s += src->stride;
		d += rp->bm->stride;
	}
}

void rp_pattern(struct rp *rp, int x, int y, int w, int h, const struct bm *pat)
{
	int row, col;
	uint8_t *d;

	if (!clipped(rp, &x, &y, &w, &h))
		return;
	for (row = 0; row < h; row++) {
		const uint8_t *s = pat->px + (size_t)((y + row) % pat->h) * pat->stride;
		int sx = x % pat->w;

		d = rp->bm->px + (size_t)(y + row) * rp->bm->stride + x;
		for (col = 0; col < w; col++) {
			d[col] = s[sx];
			if (++sx == pat->w)
				sx = 0;
		}
	}
}

void rp_ghost(struct rp *rp, int x, int y, int w, int h, int pen)
{
	int row, col;
	uint8_t *d;

	if (!clipped(rp, &x, &y, &w, &h))
		return;
	for (row = 0; row < h; row++) {
		d = rp->bm->px + (size_t)(y + row) * rp->bm->stride;
		for (col = x + ((x + y + row) & 1); col < x + w; col += 2)
			d[col] = (uint8_t)pen;
	}
}

int rp_text(struct rp *rp, const struct font *f, int x, int y,
	    const char *s, int len, int fg, int bg)
{
	int start = x, i;

	if (!f)
		return 0;
	if (len < 0)
		len = (int)strlen(s);
	for (i = 0; i < len; i++) {
		const struct glyph *g = font_glyph(f, (uint8_t)s[i]);
		int gx = x + g->kern, row, col;

		if (bg >= 0)
			rp_fill(rp, x, y, g->space, f->ysize, bg);
		for (row = 0; row < f->ysize; row++) {
			const uint8_t *bits = f->chardata + (size_t)row * f->modulo;
			int py = y + row + rp->oy;

			if (py < rp->clip.y || py >= rp->clip.y + rp->clip.h)
				continue;
			for (col = 0; col < g->width; col++) {
				int bit = g->offset + col;
				int px = gx + col + rp->ox;

				if (!(bits[bit >> 3] & (0x80 >> (bit & 7))))
					continue;
				if (px < rp->clip.x || px >= rp->clip.x + rp->clip.w)
					continue;
				rp->bm->px[(size_t)py * rp->bm->stride + px] = (uint8_t)fg;
				if (f->bold && px + 1 < rp->clip.x + rp->clip.w)
					rp->bm->px[(size_t)py * rp->bm->stride + px + 1] = (uint8_t)fg;
			}
		}
		x += g->space + (f->bold ? 1 : 0);
	}
	return x - start;
}

void planar_to_chunky(struct bm *dst, const uint8_t *planes, int w, int h,
		      int depth, int planepick, int planeonoff)
{
	int rowbytes = ((w + 15) / 16) * 2;
	int plane, stored = 0, x, y;

	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
			dst->px[(size_t)y * dst->stride + x] = 0;

	/*
	 * Only the planes picked are stored; the others are filled from
	 * PlaneOnOff. An icon has PlanePick = all of its planes.
	 */
	for (plane = 0; plane < 8; plane++) {
		uint8_t bit = (uint8_t)(1 << plane);

		if (planepick & bit) {
			const uint8_t *p;

			if (stored >= depth)
				break;
			p = planes + (size_t)stored * rowbytes * h;
			stored++;
			for (y = 0; y < h; y++)
				for (x = 0; x < w; x++)
					if (p[y * rowbytes + (x >> 3)] & (0x80 >> (x & 7)))
						dst->px[(size_t)y * dst->stride + x] |= bit;
		} else if (planeonoff & bit) {
			for (y = 0; y < h; y++)
				for (x = 0; x < w; x++)
					dst->px[(size_t)y * dst->stride + x] |= bit;
		}
	}
}
