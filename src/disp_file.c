/*
 * A display that is only memory, for running wbde on the build machine:
 * screenshots are the way to look at it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "disp.h"

int disp_open(struct disp *d, const char *mode, const char *size)
{
	int w = 640, h = 480, sw, sh;

	memset(d, 0, sizeof(*d));
	if (mode)
		sscanf(mode, "%dx%d", &w, &h);
	d->scale = 1;
	if (size && sscanf(size, "%dx%d", &sw, &sh) == 2 && sw == w && sh > 0 && h % sh == 0) {
		d->scale = h / sh;
		h = sh;
	}
	if (bm_alloc(&d->fb, w, h))
		return -1;
	snprintf(d->desc, sizeof(d->desc), "memory, %dx%d, rows x%d", w, h, d->scale);
	return 0;
}

void disp_palette(struct disp *d, const uint8_t pal[256][3])
{
	(void)d;
	(void)pal;
}

void disp_touch(void *d, const struct rect *r)
{
	(void)d;
	(void)r;
}

void disp_present(struct disp *d)
{
	(void)d;
}

void disp_close(struct disp *d)
{
	bm_free(&d->fb);
}
