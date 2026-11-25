/*
 * Choosing the display, and the calls the rest of wbde makes on it.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "disp.h"

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

int disp_screenshot(const struct disp *d, const uint8_t pal[256][3], const char *path)
{
	FILE *f = fopen(path, "wb");
	int x, y;

	if (!f)
		return -errno;
	fprintf(f, "P6\n%d %d\n255\n", d->fb.w, d->fb.h * d->scale);
	for (y = 0; y < d->fb.h * d->scale; y++) {
		uint8_t row[3 * 2048];

		for (x = 0; x < d->fb.w && x < 2048; x++)
			memcpy(row + 3 * x, pal[d->fb.px[(y / d->scale) * d->fb.stride + x]], 3);
		fwrite(row, 3, (size_t)x, f);
	}
	fclose(f);
	return 0;
}
