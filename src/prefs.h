/*
 * Everything Workbench gets from the machine: the ROM (fonts, the default
 * palette, the default pointer), the System disk (default icons out of
 * Libs/icon.library, and whatever Prefs/Env-Archive/Sys holds), and the pen
 * allocation for colours beyond the Workbench's own.
 */
#ifndef WBDE_PREFS_H
#define WBDE_PREFS_H

#include <stdint.h>

#include "gfx.h"
#include "icon.h"
#include "rom.h"

struct font;

#define NDEFICONS 9	/* indexed by do_Type, 1..8 */

struct wbenv {
	struct rom rom;
	int have_rom;
	char sysdir[256];		/* where the AmigaOS boot partition is mounted */
	char sysname[64];		/* its volume name */
	struct font *topaz8, *topaz9;
	struct font *screenfont, *iconfont;
	int depth;			/* from ScreenMode.prefs */
	uint8_t pal[256][3];
	uint8_t reserved[256];		/* pens nobody may allocate */
	uint8_t used[256];
	struct bm pointer;		/* in screen pens */
	int ptr_hx, ptr_hy;
	struct bm busy;			/* the stopwatch, when found */
	int busy_hx, busy_hy;
	int have_busy;
	struct icon defs[NDEFICONS];	/* by do_Type; .img[0].px NULL if missing */
	struct bm pattern[3];		/* WBPattern: 0 backdrop, 1 drawers, 2 screen */
	int paldirty;			/* a pen was allocated since the last look */
	char log[2048];			/* what was found where, for the About box */
};

int env_load(struct wbenv *env, const char *sysdir);
/* a pen for an RGB colour: an exact one, a free one, or the nearest */
int env_pen(void *env, uint8_t r, uint8_t g, uint8_t b);
/* a copy of the default icon for a type, falling back sensibly */
int env_default_icon(struct wbenv *env, int type, struct icon *out);
/* an IFF ILBM tiled behind the disks (0) or in drawers (1) */
int env_load_pattern(struct wbenv *env, int which, const char *path);
void env_logf(struct wbenv *env, const char *fmt, ...);

#endif
