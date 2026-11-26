/*
 * A display that is only memory, for running wbde on the build machine:
 * screenshots are the way to look at it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "disp.h"

static int file_open(struct disp *d, const char *mode, const char *size)
{
	int w = 640, h = 480, sw, sh;

	if (!size && !mode)
		size = "640x240";
	if (mode)
		sscanf(mode, "%dx%d", &w, &h);
	d->scale = 1;
	d->mw = w;
	d->mh = h;
	if (size && sscanf(size, "%dx%d", &sw, &sh) == 2 && sw == w && sh > 0 && h % sh == 0) {
		/* the old Workbench: the mode's width, rows doubled */
		d->scale = h / sh;
		h = sh;
	} else if (size && sscanf(size, "%dx%d", &sw, &sh) == 2 && sw > 0 && sh > 0) {
		/* any logical size, scaled uniformly like the GPU display does */
		w = sw;
		h = sh;
		d->uniform = 1;
	}
	if (bm_alloc(&d->fb, w, h))
		return -1;
	snprintf(d->desc, sizeof(d->desc), "memory, %dx%d, screen %dx%d%s", d->mw, d->mh, w, h,
		 d->scale > 1 ? ", rows doubled" : "");
	return 0;
}

static void file_close(struct disp *d)
{
	bm_free(&d->fb);
}

const struct disp_ops disp_file_ops = {
	.name = "file",
	.open = file_open,
	.close = file_close,
};
