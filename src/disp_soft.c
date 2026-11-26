/*
 * The software display: smol2d's DRM backend in C8, which on the Radeon is
 * true indexed scan-out with the CRTC's LUT as the palette. smol2d keeps the
 * backbuffer in RAM and copies only the damaged rectangles to VRAM at
 * present time, which matters here: every VRAM store is ~0.5 us across the
 * Mediator, and reading VRAM back is slower still.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <smol2d.h>

#include "disp.h"

struct priv {
	void *cntx;
	struct bm phys;		/* smol2d's backbuffer */
	unsigned int gen;	/* the view phys was last cleared for */
};

static int soft_open(struct disp *d, const char *mode, const char *size)
{
	struct smol2d_tex *bb;
	unsigned int stride;
	struct priv *p;
	void *cntx;
	int w = 0, h = 0;

	/* the software display stays small: scaling a big one by the CPU
	   is what it cannot afford */
	if (!mode)
		mode = "640x480";
	if (!size)
		size = "640x240";
	setenv("SMOL2D_MODE", mode, 1);
	if (smol2d_init(&cntx, SMOL2D_CS_C8)) {
		fprintf(stderr, "wbde: smol2d could not open the display in C8\n");
		return -1;
	}
	p = calloc(1, sizeof(*p));
	if (!p) {
		smol2d_close(cntx);
		return -1;
	}
	bb = smol2d_getbackbuffer(cntx);
	p->cntx = cntx;
	p->phys.px = smol2d_tex_map(cntx, bb, &stride);
	p->phys.w = (int)bb->w;
	p->phys.h = (int)bb->h;
	p->phys.stride = (int)stride;
	d->priv = p;
	d->scale = 1;
	if (size && sscanf(size, "%dx%d", &w, &h) == 2 && w == p->phys.w && h > 0 &&
	    p->phys.h % h == 0 && p->phys.h / h > 1) {
		d->scale = p->phys.h / h;
		if (bm_alloc(&d->fb, w, h)) {
			smol2d_close(cntx);
			free(p);
			return -1;
		}
	} else {
		d->fb = p->phys;
	}
	d->mw = p->phys.w;
	d->mh = p->phys.h;
	snprintf(d->desc, sizeof(d->desc), "smol2d DRM, %dx%d C8, screen %dx%d",
		 p->phys.w, p->phys.h, d->fb.w, d->fb.h);
	return 0;
}

static void soft_palette(struct disp *d, const uint8_t pal[256][3])
{
	struct smol2d_palette p;
	int i;

	memset(&p, 0, sizeof(p));
	for (i = 0; i < 256; i++) {
		p.colours[i].r = pal[i][0];
		p.colours[i].g = pal[i][1];
		p.colours[i].b = pal[i][2];
	}
	smol2d_setpalette(((struct priv *)d->priv)->cntx, &p);
}

/*
 * r of the view's screen to the backbuffer, scaled.  Scaling happens RAM
 * to RAM; only the result crosses the bus.  The Workbench screen (sx 1,
 * whole rows doubled) is a memcpy a row.
 */
static void soft_touch(struct disp *d, const struct rect *r)
{
	struct priv *p = d->priv;
	const struct disp_view *v = &d->view;
	const struct bm *src = v->src ? v->src : &d->fb;
	float sx = v->src ? v->sx : 1, sy = v->src ? v->sy : (float)d->scale;
	int ox = v->src ? v->ox : 0, oy = v->src ? v->oy : 0;
	int X0, X1, Y0, Y1, X, Y;
	struct smol2d_rect sr;

	if (v->gen != p->gen) {
		/* a new view: the border is black, the darkest pen there is (the
		   CRTC's palette is the screen's) */
		int dark = d->border_pen;

		for (Y = 0; Y < p->phys.h; Y++)
			memset(p->phys.px + (size_t)Y * p->phys.stride, dark, (size_t)p->phys.w);
		p->gen = v->gen;
		sr.x = 0;
		sr.y = 0;
		sr.w = (unsigned int)p->phys.w;
		sr.h = (unsigned int)p->phys.h;
		smol2d_tex_touch(p->cntx, smol2d_getbackbuffer(p->cntx), &sr);
	}
	X0 = ox + (int)(r->x * sx);
	X1 = ox + (int)((r->x + r->w) * sx + 0.999f);
	Y0 = oy + (int)(r->y * sy);
	Y1 = oy + (int)((r->y + r->h) * sy + 0.999f);
	if (X0 < 0) X0 = 0;
	if (Y0 < 0) Y0 = 0;
	if (X1 > p->phys.w) X1 = p->phys.w;
	if (Y1 > p->phys.h) Y1 = p->phys.h;
	if (X0 >= X1 || Y0 >= Y1)
		return;
	for (Y = Y0; Y < Y1; Y++) {
		int y = (int)((Y - oy) / sy);
		const uint8_t *s;
		uint8_t *o = p->phys.px + (size_t)Y * p->phys.stride;

		if (y < 0 || y >= src->h)
			continue;
		s = src->px + (size_t)y * src->stride;
		if (s == o)
			continue;		/* the screen is the backbuffer itself */
		if (sx == 1.0f) {
			int x0 = X0 - ox, n = X1 - X0;

			if (x0 + n > src->w)
				n = src->w - x0;
			if (n > 0)
				memcpy(o + X0, s + x0, (size_t)n);
		} else {
			for (X = X0; X < X1; X++) {
				int x = (int)((X - ox) / sx);

				if (x >= 0 && x < src->w)
					o[X] = s[x];
			}
		}
	}
	sr.x = X0;
	sr.y = Y0;
	sr.w = (unsigned int)(X1 - X0);
	sr.h = (unsigned int)(Y1 - Y0);
	smol2d_tex_touch(p->cntx, smol2d_getbackbuffer(p->cntx), &sr);
}

static void soft_present(struct disp *d)
{
	smol2d_present(((struct priv *)d->priv)->cntx);
}

/*
 * The palette is the CRTC's gamma table, which the kernel keeps after we
 * are gone, so that every true-colour program after wbde would show in
 * Workbench colours.  So whatever way wbde leaves, the table goes back to a
 * straight ramp (the table it found is not readable through smol2d; a
 * ramp is what every true-colour program expects, and what the kernel
 * starts with).
 */
static void lut_ramp(struct priv *p)
{
	struct smol2d_palette ramp;
	int i;

	for (i = 0; i < 256; i++)
		ramp.colours[i].r = ramp.colours[i].g = ramp.colours[i].b = (uint8_t)i;
	smol2d_setpalette(p->cntx, &ramp);
}

/* signal handler: two ioctls, nothing allocated */
static void soft_emergency(struct disp *d)
{
	if (d->priv)
		lut_ramp(d->priv);
}

static void soft_close(struct disp *d)
{
	struct priv *p = d->priv;

	if (!p)
		return;
	lut_ramp(p);
	if (d->scale > 1)
		bm_free(&d->fb);
	smol2d_close(p->cntx);
	free(p);
	d->priv = NULL;
}

const struct disp_ops disp_soft_ops = {
	.name = "soft",
	.open = soft_open,
	.palette = soft_palette,
	.touch = soft_touch,
	.present = soft_present,
	.close = soft_close,
	.emergency = soft_emergency,
};
