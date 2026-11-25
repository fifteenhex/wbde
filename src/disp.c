/*
 * Choosing the display, and the calls the rest of wbde makes on it.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "disp.h"
#include "wm.h"

/* the screen behind's palette (wm.h's struct screen) */
const void *disp_back_palette(const struct disp *d)
{
	return d->view.back->palette;
}

struct disp *disp_current;

static const struct disp_ops *const kinds[] = {
#ifdef HAVE_DISP_GPU
	&disp_gpu_ops,
#endif
#ifdef HAVE_DISP_SOFT
	&disp_soft_ops,
#endif
	&disp_file_ops,
};

int disp_open(struct disp *d, const char *which, const char *mode, const char *size)
{
	unsigned int i;

	if (!which)
		which = getenv("WBDE_DISPLAY");
	if (which && !*which)
		which = NULL;
	for (i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
		if (which && strcmp(which, kinds[i]->name))
			continue;
		memset(d, 0, sizeof(*d));
		d->ops = kinds[i];
		if (!d->ops->open(d, mode, size)) {
			disp_current = d;
			return 0;
		}
		fprintf(stderr, "wbde: the %s display did not open%s\n", kinds[i]->name,
			which ? "" : ", trying the next");
		if (which)
			break;
	}
	if (which && i == sizeof(kinds) / sizeof(kinds[0]))
		fprintf(stderr, "wbde: no display called %s in this build\n", which);
	return -1;
}

void disp_palette(struct disp *d, const uint8_t pal[256][3])
{
	int i, best = 0, bl = 1 << 30;

	for (i = 0; i < 256; i++) {
		int l = pal[i][0] * 3 + pal[i][1] * 6 + pal[i][2];
		if (l < bl)
			bl = l, best = i;
	}
	if (best != d->border_pen) {
		d->border_pen = best;
		d->view.gen++;          /* the border again */
	}
	if (d->ops->palette)
		d->ops->palette(d, pal);
}

void disp_touch(void *arg, const struct rect *r)
{
	struct disp *d = arg;

	if (d->ops->touch)
		d->ops->touch(d, r);
}

void disp_present(struct disp *d)
{
	if (d->ops->present)
		d->ops->present(d);
}

void disp_close(struct disp *d)
{
	if (d->ops && d->ops->close)
		d->ops->close(d);
	if (disp_current == d)
		disp_current = NULL;
}

void disp_emergency(struct disp *d)
{
	if (d && d->ops && d->ops->emergency)
		d->ops->emergency(d);
}

int disp_can_attach(const struct disp *d)
{
	return d && d->ops->attach != NULL;
}

int disp_attach(struct disp *d, struct win *w, int fd, const struct disp_buf *b)
{
	return disp_can_attach(d) ? d->ops->attach(d, w, fd, b) : -ENOTSUP;
}

int disp_share(struct disp *d, struct win *w, int format, int width, int height, int *fd,
	       struct disp_buf *b, uint32_t *size)
{
	if (!d || !d->ops->share)
		return -ENOTSUP;
	return d->ops->share(d, w, format, width, height, fd, b, size);
}

void disp_detach(struct disp *d, struct win *w)
{
	if (d && d->ops->detach)
		d->ops->detach(d, w);
}

void disp_set_view(struct disp *d, struct bm *src, int w, int h, float sx, float sy, int ox, int oy)
{
	struct disp_view *v = &d->view;

	if (v->src == src && v->w == w && v->h == h && v->sx == sx && v->sy == sy &&
	    v->ox == ox && v->oy == oy)
		return;
	v->src = src;
	v->w = w;
	v->h = h;
	v->sx = sx;
	v->sy = sy;
	v->ox = ox;
	v->oy = oy;
	v->gen++;
}

void disp_set_back(struct disp *d, struct screen *back, struct bm *src, int w, int h, float sx,
		   float sy, int ox, int oy)
{
	struct disp_view *v = &d->view;

	if (v->back == back && v->bsrc == src && v->bw == w && v->bh == h && v->bsx == sx &&
	    v->bsy == sy && v->box == ox && v->boy == oy)
		return;
	v->back = back;
	v->bsrc = src;
	v->bw = w;
	v->bh = h;
	v->bsx = sx;
	v->bsy = sy;
	v->box = ox;
	v->boy = oy;
	v->gen++;
}

int disp_view_map(const struct disp *d, int X, int Y, int *x, int *y)
{
	const struct disp_view *v = &d->view;
	float fx, fy;

	if (!v->src || v->sx <= 0 || v->sy <= 0)
		return 0;
	if (v->back && Y < v->oy) {
		fx = (X - v->box) / v->bsx;
		fy = (Y - v->boy) / v->bsy;
		if (fx < 0 || fy < 0)
			return 0;
		*x = (int)fx;
		*y = (int)fy;
		return *x < v->bw && *y < v->bh ? 2 : 0;
	}
	fx = (X - v->ox) / v->sx;
	fy = (Y - v->oy) / v->sy;
	if (fx < 0 || fy < 0)
		return 0;
	*x = (int)fx;
	*y = (int)fy;
	return *x < v->w && *y < v->h;
}

/* the screen as the display shows it, from the view's composition in RAM */
int disp_screenshot(const struct disp *d, const uint8_t pal[256][3], const char *path)
{
	const uint8_t (*backpal)[3] = d->view.back ? (const uint8_t (*)[3])disp_back_palette(d) : pal;
	FILE *f = fopen(path, "wb");
	const struct bm *src = d->view.src ? d->view.src : &d->fb;
	int x, y, W = d->mw ? d->mw : d->fb.w, H = d->mh ? d->mh : d->fb.h * d->scale;

	if (!f)
		return -errno;
	fprintf(f, "P6\n%d %d\n255\n", W, H);
	for (y = 0; y < H; y++) {
		uint8_t row[3 * 2048];

		for (x = 0; x < W && x < 2048; x++) {
			int sx, sy;

			/* outside the screen: black */
			int m = disp_view_map(d, x, y, &sx, &sy);

			if (m == 1)
				memcpy(row + 3 * x, pal[src->px[(size_t)sy * src->stride + sx]], 3);
			else if (m == 2)
				memcpy(row + 3 * x, backpal[d->view.bsrc->px[(size_t)sy *
					d->view.bsrc->stride + sx]], 3);
			else
				memset(row + 3 * x, 0, 3);
		}
		fwrite(row, 3, (size_t)x, f);
	}
	fclose(f);
	return 0;
}
