/*
 * Chunky 8-bit bitmaps and a RastPort-like drawing context over them.
 * Every pixel is a pen number; the palette lives with the screen.
 */
#ifndef WBDE_GFX_H
#define WBDE_GFX_H

#include <stdint.h>

struct rect {
	int x, y, w, h;
};

#define BM_DIRTY_MAX 8

struct bm {
	int w, h, stride;
	uint8_t *px;
	/*
	 * What has been drawn since a display that keeps its own copy of this
	 * bitmap (a GPU texture) last took it: rectangles in bitmap
	 * coordinates, merged when that costs little.  Every rp_* drawing call
	 * adds what it touched; bm_alloc marks the whole bitmap.
	 */
	int ndirty;
	struct rect dirty[BM_DIRTY_MAX];
	void *gpu;			/* the display's copy, freed with the bitmap */
};

/* a display that keeps copies of bitmaps frees its copy here */
extern void (*bm_free_hook)(struct bm *bm);
void bm_dirty(struct bm *bm, const struct rect *r);
void bm_dirty_all(struct bm *bm);

/* Where drawing goes: a bitmap, a clip in bitmap coords, an origin */
struct rp {
	struct bm *bm;
	struct rect clip;
	int ox, oy;
};

struct font;

int bm_alloc(struct bm *bm, int w, int h);
void bm_free(struct bm *bm);

static inline int rect_empty(const struct rect *r)
{
	return r->w <= 0 || r->h <= 0;
}

static inline struct rect rect_make(int x, int y, int w, int h)
{
	struct rect r = { x, y, w, h };
	return r;
}

int rect_intersect(struct rect *out, const struct rect *a, const struct rect *b);
void rect_union(struct rect *into, const struct rect *with);
static inline int rect_contains(const struct rect *r, int x, int y)
{
	return x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h;
}

void rp_init(struct rp *rp, struct bm *bm);
/* narrow the clip to r (in rp coordinates) */
void rp_clip(struct rp *rp, const struct rect *r);

void rp_fill(struct rp *rp, int x, int y, int w, int h, int pen);
void rp_pixel(struct rp *rp, int x, int y, int pen);
void rp_hline(struct rp *rp, int x, int y, int w, int pen);
void rp_vline(struct rp *rp, int x, int y, int h, int pen);
void rp_box(struct rp *rp, int x, int y, int w, int h, int pen);
/* raised (shine top/left, shadow bottom/right) or recessed bevel */
void rp_bevel(struct rp *rp, int x, int y, int w, int h, int shine, int shadow);
void rp_xor_rect(struct rp *rp, int x, int y, int w, int h, int mask);
/*
 * Intuition 3.x's frames (frameiclass) on a hires screen, where vertical
 * lines are drawn two pixels wide to look as thick as horizontal ones:
 * FRAME_BUTTON (raised, or recessed with shine and shadow swapped) and
 * FRAME_RIDGE (a raised edge round a recessed one: string gadgets).
 */
void rp_frame_button(struct rp *rp, int x, int y, int w, int h, int shine, int shadow);
void rp_frame_ridge(struct rp *rp, int x, int y, int w, int h, int shine, int shadow);
/* key < 0: opaque */
void rp_blit(struct rp *rp, int x, int y, const struct bm *src,
	     int sx, int sy, int w, int h, int key);
/* tile src over the area, aligned to the bitmap's own origin */
void rp_pattern(struct rp *rp, int x, int y, int w, int h, const struct bm *pat);
/* ghosting: a pen every other pixel in a checkerboard */
void rp_ghost(struct rp *rp, int x, int y, int w, int h, int pen);

/* text: y is the top; bg < 0 is transparent (JAM1); returns the advance */
int rp_text(struct rp *rp, const struct font *f, int x, int y,
	    const char *s, int len, int fg, int bg);

/* Amiga planar image data (word aligned rows, plane after plane) to chunky */
void planar_to_chunky(struct bm *dst, const uint8_t *planes, int w, int h,
		      int depth, int planepick, int planeonoff);

#endif
