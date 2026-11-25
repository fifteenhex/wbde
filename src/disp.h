/*
 * The display.  Three kinds, chosen at run time (--display, $WBDE_DISPLAY):
 *
 *   gpu   the GPU composes the screen (disp_gpu.c, smolgpu): every layer is
 *         a texture, the damaged parts of the scan-out are drawn by the card
 *         each flush, the pointer is the hardware cursor
 *   soft  wbde composes the screen in RAM and smol2d copies the damage to a
 *         C8 scan-out (disp_soft.c)
 *   file  memory only, for running on the build machine (disp_file.c)
 *
 * The default tries gpu, then soft, then file, so wbde runs without a card
 * smolgpu knows.  A software display composites into d->fb (the logical
 * screen) and is told which rectangles changed; a composing one is handed
 * the screen and its damage instead.
 */
#ifndef WBDE_DISP_H
#define WBDE_DISP_H

#include <stdint.h>

#include "gfx.h"

struct disp;
struct screen;
struct win;

/* a buffer somebody else owns, shown as a window's content (dma-buf) */
enum { DISP_BUF_PENS, DISP_BUF_XRGB8888 };

struct disp_buf {
	int format;			/* DISP_BUF_* */
	int width, height;		/* in pixels */
	int pitch;			/* bytes per row */
	uint32_t offset;		/* of pixel 0,0 in the buffer */
};

struct disp_ops {
	const char *name;
	int (*open)(struct disp *d, const char *mode, const char *size);
	void (*palette)(struct disp *d, const uint8_t pal[256][3]);
	/* software composition: d->fb has the new pixels of r */
	void (*touch)(struct disp *d, const struct rect *r);
	void (*present)(struct disp *d);
	/* composing displays: draw the screen's damage (and clear it) */
	void (*compose)(struct disp *d, struct screen *s);
	/* the screen as a PPM, as the display shows it; NULL: from d->fb */
	int (*shot)(struct disp *d, struct screen *s, const char *path);
	void (*close)(struct disp *d);
	/* from a signal handler: put the display back as it was found */
	void (*emergency)(struct disp *d);
	/* a window whose content is a shared buffer (composing displays) */
	int (*attach)(struct disp *d, struct win *w, int fd, const struct disp_buf *b);
	void (*detach)(struct disp *d, struct win *w);
	/* a new buffer of the display's own as the window's content, for the
	 * client to write into: its dma-buf in *fd, layout in *b, bytes in *size */
	int (*share)(struct disp *d, struct win *w, int format, int width, int height, int *fd,
		     struct disp_buf *b, uint32_t *size);
};

/*
 * What the display shows: the front screen's composition (src: d->fb for
 * the Workbench screen, a client screen's own), screen pixel (x, y) at
 * display pixel (ox + x * sx, oy + y * sy).  Outside it, black (the
 * software display, whose palette is the screen's, takes its darkest pen).
 */
struct disp_view {
	struct bm *src;
	int w, h;		/* the screen */
	float sx, sy;
	int ox, oy;
	unsigned int gen;	/* bumped on every change */
	/*
	 * A screen behind the front one shows above it when the front one
	 * starts lower down (its top_edge): display rows above oy are that
	 * screen's, through its own view.
	 */
	struct screen *back;
	struct bm *bsrc;
	int bw, bh;
	float bsx, bsy;
	int box, boy;
};

struct disp {
	struct bm fb;		/* the logical screen, what wbde draws */
	int scale;		/* rows are shown this many times over */
	int mw, mh;		/* the display mode */
	int border_pen;		/* the software display's darkest pen, for the border */
	int mm_w, mm_h;		/* the monitor's size in mm (EDID), 0 unknown */
	struct disp_view view;
	int composes;		/* ops->compose, not software composition */
	int uniform;		/* Workbench at any size, scaled uniformly (as composing ones) */
	int hw_pointer;		/* the pointer is not drawn into the screen */
	const struct disp_ops *ops;
	void *priv;
	char desc[128];
	/* what the last flush cost, for WBDE_TIMING (composing displays) */
	char timing[160];
};

extern const struct disp_ops disp_gpu_ops, disp_soft_ops, disp_file_ops;

/*
 * which: "gpu", "soft", "file" or NULL for the first that opens.
 * mode: the display mode, "640x480" (NULL: the backend's choice).
 * size: the logical screen, "640x240" (NULL: the mode). When the mode is a
 * whole multiple of it in height every row is shown that many times, which
 * is how a hires non-interlaced Workbench, with its tall pixels, looks right
 * on a monitor with square ones.
 */
int disp_open(struct disp *d, const char *which, const char *mode, const char *size);
void disp_palette(struct disp *d, const uint8_t pal[256][3]);
void disp_touch(void *d, const struct rect *r);
void disp_present(struct disp *d);
void disp_close(struct disp *d);
void disp_emergency(struct disp *d);
int disp_attach(struct disp *d, struct win *w, int fd, const struct disp_buf *b);
void disp_detach(struct disp *d, struct win *w);
int disp_can_attach(const struct disp *d);
int disp_share(struct disp *d, struct win *w, int format, int width, int height, int *fd,
	       struct disp_buf *b, uint32_t *size);

/* the display wbde runs on, for the client server */
extern struct disp *disp_current;

/* the composed screen as a PPM from d->fb (RAM: reading VRAM back is slow) */
int disp_screenshot(const struct disp *d, const uint8_t pal[256][3], const char *path);

/* show src (a w x h screen) at that scale and offset */
void disp_set_view(struct disp *d, struct bm *src, int w, int h, float sx, float sy, int ox, int oy);
/* the screen behind that shows above the front one (NULL: none) */
void disp_set_back(struct disp *d, struct screen *back, struct bm *src, int w, int h, float sx,
		   float sy, int ox, int oy);
/* display pixel to screen pixel under the view: 1 the front screen's,
   2 the screen behind's, 0 neither */
int disp_view_map(const struct disp *d, int X, int Y, int *x, int *y);

#endif
