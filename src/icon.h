/* Workbench .info files: struct DiskObject on disk, and the OS 3.5 FORM ICON */
#ifndef WBDE_ICON_H
#define WBDE_ICON_H

#include <stdint.h>
#include <stddef.h>
#include <limits.h>

#include "gfx.h"

enum {
	WBDISK = 1, WBDRAWER, WBTOOL, WBPROJECT, WBGARBAGE, WBDEVICE, WBKICK, WBAPPICON
};

#define ICON_NOPOS INT_MIN

/* pen for a true colour (glow icon palettes); NULL: keep indices as they are */
typedef int (*icon_penfn)(void *arg, uint8_t r, uint8_t g, uint8_t b);

struct icon {
	int type;
	int w, h;
	struct bm img[2];	/* normal, selected (selected.px NULL = complement) */
	int key;		/* pen that is see-through, -1 none */
	int glow;		/* came from a FORM ICON */
	int frameless;
	char *deftool;
	char **tooltypes;
	int ntooltypes;
	int x, y;		/* snapshot position, ICON_NOPOS if none */
	int stack;
	/* drawers and disks: where their window goes */
	int has_window;
	struct rect win;
	int dd_x, dd_y;		/* scroll offset */
	uint32_t dd_flags;	/* 1 = only icons, 2 = all files */
	int dd_viewmodes;
};

int icon_parse(const uint8_t *buf, size_t len, struct icon *ic,
	       icon_penfn pen, void *penarg);
/* a memory image whose pointers can be followed (a relocated hunk file) */
struct memimg {
	const uint8_t *p;
	uint32_t base, size;
};

static inline int memimg_in(const struct memimg *m, uint32_t a, uint32_t len)
{
	return a >= m->base && len <= m->size && a - m->base <= m->size - len;
}

static inline uint32_t memimg_l(const struct memimg *m, uint32_t a)
{
	const uint8_t *p = m->p + (a - m->base);
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static inline uint16_t memimg_w(const struct memimg *m, uint32_t a)
{
	const uint8_t *p = m->p + (a - m->base);
	return (uint16_t)(p[0] << 8 | p[1]);
}

int icon_from_memory(const struct memimg *m, uint32_t addr, struct icon *ic);
int icon_load(const char *path, struct icon *ic, icon_penfn pen, void *penarg);
void icon_free(struct icon *ic);
/* a deep copy, for the defaults that get handed out many times */
int icon_dup(struct icon *dst, const struct icon *src);
const char *icon_tooltype(const struct icon *ic, const char *name);

#endif
