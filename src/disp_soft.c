/*
 * The real display: smol2d's DRM backend in C8, which on the Radeon is true
 * indexed scan-out with the CRTC's LUT as the palette. smol2d keeps the
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
};

int disp_open(struct disp *d, const char *mode, const char *size)
{
	struct smol2d_tex *bb;
	unsigned int stride;
	struct priv *p;
	void *cntx;
	int w = 0, h = 0;

	memset(d, 0, sizeof(*d));
	if (mode)
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
	snprintf(d->desc, sizeof(d->desc), "smol2d DRM, %dx%d C8, screen %dx%d",
		 p->phys.w, p->phys.h, d->fb.w, d->fb.h);
	return 0;
}

void disp_palette(struct disp *d, const uint8_t pal[256][3])
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

void disp_touch(void *arg, const struct rect *r)
{
	struct disp *d = arg;
	struct priv *p = d->priv;
	struct smol2d_rect sr = { r->x, r->y * d->scale, (unsigned int)r->w,
				  (unsigned int)(r->h * d->scale) };
	int y, k;

	/* doubling happens RAM to RAM; only the result crosses the bus */
	if (d->scale > 1)
		for (y = r->y; y < r->y + r->h; y++)
			for (k = 0; k < d->scale; k++)
				memcpy(p->phys.px + (size_t)(y * d->scale + k) * p->phys.stride + r->x,
				       d->fb.px + (size_t)y * d->fb.stride + r->x, (size_t)r->w);
	smol2d_tex_touch(p->cntx, smol2d_getbackbuffer(p->cntx), &sr);
}

void disp_present(struct disp *d)
{
	smol2d_present(((struct priv *)d->priv)->cntx);
}

void disp_close(struct disp *d)
{
	struct priv *p = d->priv;

	if (!p)
		return;
	if (d->scale > 1)
		bm_free(&d->fb);
	smol2d_close(p->cntx);
	free(p);
	d->priv = NULL;
}
