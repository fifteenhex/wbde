/* The display: a chunky backbuffer the screen composites into, and a flush */
#ifndef WBDE_DISP_H
#define WBDE_DISP_H

#include <stdint.h>

#include "gfx.h"

struct disp {
	struct bm fb;		/* the logical screen, what wbde draws */
	int scale;		/* rows are shown this many times over */
	void *priv;
	char desc[96];
};

/*
 * mode: the display mode, "640x480" (NULL: the backend's choice).
 * size: the logical screen, "640x240" (NULL: the mode). When the mode is a
 * whole multiple of it in height every row is shown that many times, which
 * is how a hires non-interlaced Workbench, with its tall pixels, looks right
 * on a monitor with square ones.
 */
int disp_open(struct disp *d, const char *mode, const char *size);
void disp_palette(struct disp *d, const uint8_t pal[256][3]);
void disp_touch(void *d, const struct rect *r);
void disp_present(struct disp *d);
void disp_close(struct disp *d);

/* the composed screen as a PPM (from RAM: reading VRAM back is slow) */
int disp_screenshot(const struct disp *d, const uint8_t pal[256][3], const char *path);

#endif
