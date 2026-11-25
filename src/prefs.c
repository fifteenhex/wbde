#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "font.h"
#include "hunk.h"
#include "iff.h"
#include "prefs.h"

/* sprite colours 17-19, as on the real chipset */
#define PTR_PEN0 17

void env_logf(struct wbenv *env, const char *fmt, ...)
{
	char line[256];
	size_t at = strlen(env->log);
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	fprintf(stderr, "wbde: %s\n", line);
	if (at + strlen(line) + 2 < sizeof(env->log))
		snprintf(env->log + at, sizeof(env->log) - at, "%s\n", line);
}

static void rgb4(uint8_t *out, uint16_t v)
{
	out[0] = (uint8_t)(((v >> 8) & 15) * 17);
	out[1] = (uint8_t)(((v >> 4) & 15) * 17);
	out[2] = (uint8_t)((v & 15) * 17);
}

/*
 * intuition.library carries a whole 1.x struct Preferences as its defaults.
 * Its tail is
 *   UWORD PointerMatrix[36]   (two control words, 16 rows of 2 planes, two)
 *   BYTE XOffset, YOffset
 *   UWORD color17, color18, color19, PointerTicks
 *   UWORD color0, color1, color2, color3
 * In 3.1.4 that is at 0xfec55c..0xfec5b6, colour 0 at 0xfec5ae. Find it by
 * the Workbench colours $AAA $000 $FFF $68B with a sane pointer before them.
 * A second copy of the colours (0xfec1ee) runs on to colours 4-7 and the
 * pointer colours: that one gives the 8 colour palette.
 */
static int rom_prefs(struct wbenv *env)
{
	const struct rom *rom = &env->rom;
	int found_ptr = 0, found_pal8 = 0;
	uint32_t a;

	for (a = rom->base + 82; rom_in(rom, a, 16); a += 2) {
		uint32_t pm = a - 82;
		int i, x, y;

		if (rom_l(rom, a) != 0x0aaa0000u || rom_l(rom, a + 4) != 0x0fff068bu)
			continue;
		/* the 8 colour list: 4 more colours, then 3 pointer colours */
		if (!found_pal8 && rom_w(rom, a + 8) && rom_w(rom, a + 8) <= 0xfff &&
		    rom_w(rom, a + 10) <= 0xfff && rom_w(rom, a + 12) <= 0xfff &&
		    rom_w(rom, a + 14) <= 0xfff) {
			for (i = 0; i < 8; i++)
				rgb4(env->pal[i], rom_w(rom, a + (uint32_t)i * 2));
			found_pal8 = 1;
			env_logf(env, "palette: 8 colours from ROM at %06x", a);
		}
		if (found_ptr || rom_l(rom, pm) != 0 || rom_l(rom, pm + 68) != 0 ||
		    rom_w(rom, a - 8) > 0xfff || rom_w(rom, a - 6) > 0xfff ||
		    rom_w(rom, a - 4) > 0xfff)
			continue;
		if (bm_alloc(&env->pointer, 16, 16))
			return -ENOMEM;
		for (y = 0; y < 16; y++) {
			uint16_t p0 = rom_w(rom, pm + 4 + (uint32_t)y * 4);
			uint16_t p1 = rom_w(rom, pm + 6 + (uint32_t)y * 4);

			for (x = 0; x < 16; x++) {
				int v = ((p0 >> (15 - x)) & 1) | (((p1 >> (15 - x)) & 1) << 1);

				env->pointer.px[y * env->pointer.stride + x] =
					(uint8_t)(v ? PTR_PEN0 + v - 1 : 0);
			}
		}
		env->ptr_hx = -(int8_t)*rom_ptr(rom, a - 10);
		env->ptr_hy = -(int8_t)*rom_ptr(rom, a - 9);
		for (i = 0; i < 3; i++)
			rgb4(env->pal[PTR_PEN0 + i], rom_w(rom, a - 8 + (uint32_t)i * 2));
		if (!found_pal8)
			for (i = 0; i < 4; i++)
				rgb4(env->pal[i], rom_w(rom, a + (uint32_t)i * 2));
		found_ptr = 1;
		env_logf(env, "pointer: 16x16 from ROM Preferences at %06x, hot spot %d,%d",
			 pm, env->ptr_hx, env->ptr_hy);
	}
	return found_ptr ? 0 : -ENOENT;
}

/* the defaults compiled into icon.library, by do_Type */
static void icon_library_defaults(struct wbenv *env)
{
	char path[300];
	struct hunkimg h;
	struct memimg m;
	uint8_t *buf;
	size_t len;
	uint32_t a;
	int n = 0;

	snprintf(path, sizeof(path), "%s/Libs/icon.library", env->sysdir);
	buf = file_slurp(path, &len, 1 << 20);
	if (!buf) {
		env_logf(env, "default icons: no %s", path);
		return;
	}
	if (hunk_load(buf, len, &h)) {
		free(buf);
		env_logf(env, "default icons: %s is not a load file", path);
		return;
	}
	free(buf);
	m.p = h.mem;
	m.base = h.base;
	m.size = h.size;
	for (a = h.base; hunk_in(&h, a, 78); a += 2) {
		struct icon ic;

		if (memimg_w(&m, a) != 0xe310 || icon_from_memory(&m, a, &ic))
			continue;
		if (ic.type > 0 && ic.type < NDEFICONS && !env->defs[ic.type].img[0].px) {
			ic.key = 0;
			env->defs[ic.type] = ic;
			n++;
		} else {
			icon_free(&ic);
		}
	}
	hunk_free(&h);
	env_logf(env, "default icons: %d from %s", n, path);
}

/* ENVARC:Sys/def_<x>.info overrides the built-in ones */
static void envarc_defaults(struct wbenv *env)
{
	static const char *names[NDEFICONS] = {
		NULL, "disk", "drawer", "tool", "project", "trashcan", "device", "kick", "appicon",
	};
	char path[400];
	int t;

	for (t = 1; t < NDEFICONS; t++) {
		struct icon ic;

		snprintf(path, sizeof(path), "%s/Prefs/Env-Archive/Sys/def_%s.info",
			 env->sysdir, names[t]);
		if (icon_load(path, &ic, env_pen, env))
			continue;
		icon_free(&env->defs[t]);
		env->defs[t] = ic;
		env_logf(env, "default icon: %s", path);
	}
}

static const uint8_t *prefs_chunk(struct wbenv *env, const char *name, uint32_t id,
				  uint8_t **buf, uint32_t *clen)
{
	char path[400];
	size_t len;

	snprintf(path, sizeof(path), "%s/Prefs/Env-Archive/Sys/%s", env->sysdir, name);
	*buf = file_slurp(path, &len, 1 << 20);
	if (!*buf)
		return NULL;
	return iff_chunk(*buf, len, IFF_ID('P', 'R', 'E', 'F'), id, clen);
}

static void screenmode_prefs(struct wbenv *env)
{
	uint8_t *buf;
	uint32_t len;
	const uint8_t *c = prefs_chunk(env, "ScreenMode.prefs", IFF_ID('S', 'C', 'R', 'M'), &buf, &len);

	if (c && len >= 28) {
		int depth = be16(c + 24);

		if (depth >= 1 && depth <= 8) {
			env->depth = depth;
			env_logf(env, "ScreenMode.prefs: depth %d (mode %08x)", depth, be32(c + 16));
		}
	}
	free(buf);
}

static uint8_t gun(uint16_t v, uint16_t max)
{
	if (max <= 15)
		return (uint8_t)(v * 17);
	if (max <= 255)
		return (uint8_t)v;
	return (uint8_t)(v >> 8);
}

static void palette_prefs(struct wbenv *env)
{
	uint8_t *buf;
	uint32_t len, i;
	const uint8_t *c = prefs_chunk(env, "palette.prefs", IFF_ID('P', 'A', 'L', 'T'), &buf, &len);
	uint16_t max = 0;

	if (c && len >= 16 + 64 + 64 + 8) {
		const uint8_t *cs = c + 16 + 64 + 64;

		for (i = 0; i + 8 <= len - 144 && (int16_t)be16(cs + i) >= 0; i += 8) {
			uint16_t r = be16(cs + i + 2), g = be16(cs + i + 4), b = be16(cs + i + 6);

			if (r > max) max = r;
			if (g > max) max = g;
			if (b > max) max = b;
		}
		for (i = 0; i + 8 <= len - 144 && (int16_t)be16(cs + i) >= 0; i += 8) {
			int idx = be16(cs + i);

			if (idx < 256) {
				env->pal[idx][0] = gun(be16(cs + i + 2), max);
				env->pal[idx][1] = gun(be16(cs + i + 4), max);
				env->pal[idx][2] = gun(be16(cs + i + 6), max);
			}
		}
		env_logf(env, "palette.prefs applied");
	}
	free(buf);
}

static void pattern_prefs(struct wbenv *env)
{
	char path[400];
	uint8_t *buf;
	size_t len, at;

	snprintf(path, sizeof(path), "%s/Prefs/Env-Archive/Sys/wbpattern.prefs", env->sysdir);
	buf = file_slurp(path, &len, 1 << 20);
	if (!buf)
		return;
	/* every PTRN chunk: 16 reserved, Which, Flags, Revision, Depth, DataLength */
	for (at = 12; at + 8 <= len;) {
		uint32_t id = be32(buf + at), clen = be32(buf + at + 4);
		const uint8_t *c = buf + at + 8;

		if (clen > len - at - 8)
			break;
		if (id == IFF_ID('P', 'T', 'R', 'N') && clen >= 24) {
			int which = be16(c + 16), flags = be16(c + 18), depth = c[21];
			int dlen = be16(c + 22);

			if (which < 3 && (flags & 1) && depth >= 1 && depth <= 8 &&
			    24 + depth * 32 <= (int)clen && dlen >= depth * 32 &&
			    !bm_alloc(&env->pattern[which], 16, 16)) {
				planar_to_chunky(&env->pattern[which], c + 24, 16, 16, depth,
						 (1 << depth) - 1, 0);
				env_logf(env, "wbpattern.prefs: pattern %d, depth %d", which, depth);
			} else if (which < 3 && !(flags & 1) && dlen > 0 && 24 + dlen <= (int)clen) {
				env_logf(env, "wbpattern.prefs: picture %.*s not supported yet",
					 dlen, (const char *)c + 24);
			}
		}
		at += 8 + clen + (clen & 1);
	}
	free(buf);
}

static void pointer_prefs(struct wbenv *env)
{
	char path[400];
	struct ilbm ilbm;
	uint8_t *buf;
	size_t len;
	int x, y, i;

	snprintf(path, sizeof(path), "%s/Prefs/Env-Archive/Sys/pointer.ilbm", env->sysdir);
	buf = file_slurp(path, &len, 1 << 20);
	if (!buf)
		return;
	if (!ilbm_decode(buf, len, &ilbm) && ilbm.ncolours >= 4) {
		bm_free(&env->pointer);
		env->pointer = ilbm.bm;
		for (y = 0; y < ilbm.bm.h; y++)
			for (x = 0; x < ilbm.bm.w; x++) {
				uint8_t *p = &env->pointer.px[y * env->pointer.stride + x];

				*p = (uint8_t)((*p & 3) ? PTR_PEN0 + (*p & 3) - 1 : 0);
			}
		for (i = 0; i < 3; i++)
			memcpy(env->pal[PTR_PEN0 + i], ilbm.cmap[i + 1], 3);
		env->ptr_hx = ilbm.grabx;
		env->ptr_hy = ilbm.graby;
		if (ilbm.mask.px)
			bm_free(&ilbm.mask);
		env_logf(env, "pointer: %s", path);
	}
	free(buf);
}

/*
 * The AFFS root block holds the volume name: a BCPL string 80 bytes before
 * the end of the block (T_HEADER = 2 at +0, ST_ROOT = 1 in the last long).
 * Its block number is the middle of the partition. The device is only read.
 */
static void affs_volname(struct wbenv *env, const char *dev)
{
	static const int sizes[] = { 512, 1024, 2048, 4096 };
	uint8_t blk[4096];
	off_t devsize;
	unsigned int i;
	int fd;

	fd = open(dev, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	devsize = lseek(fd, 0, SEEK_END);
	for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		int bs = sizes[i];
		off_t nblocks = devsize / bs;
		off_t root = (nblocks - 1 + 2) / 2;

		if (pread(fd, blk, (size_t)bs, root * bs) != bs)
			continue;
		if (be32(blk) == 2 && be32(blk + bs - 4) == 1) {
			int n = blk[bs - 80];

			if (n > 30)
				n = 30;
			memcpy(env->sysname, blk + bs - 79, (size_t)n);
			env->sysname[n] = 0;
			env_logf(env, "System volume: \"%s\" (%s, %d byte blocks, root %lld)",
				 env->sysname, dev, bs, (long long)root);
			break;
		}
	}
	close(fd);
}

int env_pen(void *arg, uint8_t r, uint8_t g, uint8_t b)
{
	struct wbenv *env = arg;
	int i, best = 0, free_pen = -1;
	long bestd = 1L << 30;

	for (i = 0; i < 256; i++) {
		long dr, dg, db, d;

		if (env->reserved[i] || env->used[i]) {
			if (i == 255)
				continue;
			dr = env->pal[i][0] - r;
			dg = env->pal[i][1] - g;
			db = env->pal[i][2] - b;
			d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
			if (!d)
				return i;
			if (d < bestd && (i < 4 || env->used[i])) {
				bestd = d;
				best = i;
			}
			continue;
		}
		if (free_pen < 0)
			free_pen = i;
	}
	if (free_pen >= 0) {
		env->used[free_pen] = 1;
		env->paldirty = 1;
		env->pal[free_pen][0] = r;
		env->pal[free_pen][1] = g;
		env->pal[free_pen][2] = b;
		return free_pen;
	}
	return best;
}

int env_default_icon(struct wbenv *env, int type, struct icon *out)
{
	int t = type;

	if (t <= 0 || t >= NDEFICONS || !env->defs[t].img[0].px) {
		/* a device or kick disk is still a disk; anything else a project */
		if (t == WBDEVICE || t == WBKICK)
			t = WBDISK;
		else if (t == WBAPPICON || t == WBGARBAGE)
			t = WBPROJECT;
	}
	if (t <= 0 || t >= NDEFICONS || !env->defs[t].img[0].px) {
		memset(out, 0, sizeof(*out));
		out->x = out->y = ICON_NOPOS;
		out->type = type;
		out->w = 32;
		out->h = 16;
		if (bm_alloc(&out->img[0], 32, 16))
			return -ENOMEM;
		{
			struct rp rp;

			rp_init(&rp, &out->img[0]);
			rp_fill(&rp, 0, 0, 32, 16, 2);
			rp_box(&rp, 0, 0, 32, 16, 1);
		}
		return 0;
	}
	if (icon_dup(out, &env->defs[t]))
		return -ENOMEM;
	out->type = type;
	out->x = out->y = ICON_NOPOS;
	return 0;
}

static void find_sysname_from_mounts(struct wbenv *env)
{
	char line[512];
	FILE *f = fopen("/proc/mounts", "r");

	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char dev[256], dir[256], type[32];

		if (sscanf(line, "%255s %255s %31s", dev, dir, type) != 3)
			continue;
		if (!strcmp(dir, env->sysdir)) {
			if (!strcmp(type, "affs"))
				affs_volname(env, dev);
			break;
		}
	}
	fclose(f);
}

int env_load(struct wbenv *env, const char *sysdir)
{
	static const uint8_t pal31[8][3] = {
		{ 0xaa, 0xaa, 0xaa }, { 0, 0, 0 }, { 0xff, 0xff, 0xff }, { 0x66, 0x88, 0xbb },
		{ 0xee, 0x44, 0x44 }, { 0x55, 0xdd, 0x55 }, { 0x00, 0x44, 0xdd }, { 0xee, 0x99, 0x00 },
	};
	int ret, i;

	memset(env, 0, sizeof(*env));
	snprintf(env->sysdir, sizeof(env->sysdir), "%s", sysdir);
	env->depth = 3;
	memcpy(env->pal, pal31, sizeof(pal31));

	ret = rom_load(&env->rom, NULL);
	if (ret) {
		env_logf(env, "Kickstart ROM: cannot read %s (%s)",
			 env->rom.source[0] ? env->rom.source : "/dev/mem", strerror(-ret));
	} else {
		env->have_rom = 1;
		env_logf(env, "Kickstart %u.%u from %s, checksum %s", env->rom.version,
			 env->rom.revision, env->rom.source,
			 rom_checksum_ok(&env->rom) ? "ok" : "BAD");
		env->topaz8 = font_from_rom(&env->rom, "topaz.font", 8);
		env->topaz9 = font_from_rom(&env->rom, "topaz.font", 9);
		env_logf(env, "fonts: topaz/8 %s, topaz/9 %s", env->topaz8 ? "from ROM" : "MISSING",
			 env->topaz9 ? "from ROM" : "MISSING");
		if (rom_prefs(env))
			env_logf(env, "pointer: none found in ROM");
	}
	if (!env->topaz8)
		return -ENOENT;
	env->screenfont = env->topaz8;
	env->iconfont = env->topaz8;

	find_sysname_from_mounts(env);
	screenmode_prefs(env);
	for (i = 0; i < 8; i++)
		env->reserved[i] = 1;
	for (i = 0; i < 3; i++)
		env->reserved[PTR_PEN0 + i] = 1;
	/* the top four pens mirror the first four, complemented, as Intuition does */
	for (i = 0; i < 4; i++) {
		env->pal[255 - i][0] = (uint8_t)~env->pal[i][0];
		env->pal[255 - i][1] = (uint8_t)~env->pal[i][1];
		env->pal[255 - i][2] = (uint8_t)~env->pal[i][2];
		env->reserved[255 - i] = 1;
	}
	palette_prefs(env);
	pointer_prefs(env);
	pattern_prefs(env);
	icon_library_defaults(env);
	envarc_defaults(env);
	if (!env->pointer.px && !bm_alloc(&env->pointer, 8, 8)) {
		/* nothing found anywhere: a plain arrow so the mouse can be seen */
		int y;

		for (y = 0; y < 8; y++)
			memset(env->pointer.px + y * env->pointer.stride, PTR_PEN0 + 1,
			       (size_t)(8 - y));
	}
	return 0;
}

/* an IFF ILBM as a backdrop tile, its colours mapped onto pens */
int env_load_pattern(struct wbenv *env, int which, const char *path)
{
	struct ilbm ilbm;
	uint8_t map[256], *buf;
	size_t len;
	int i, x, y, ret;

	if (which < 0 || which > 2)
		return -EINVAL;
	buf = file_slurp(path, &len, 4 << 20);
	if (!buf)
		return -errno;
	ret = ilbm_decode(buf, len, &ilbm);
	free(buf);
	if (ret)
		return ret;
	for (i = 0; i < 256; i++)
		map[i] = i < ilbm.ncolours ?
			 (uint8_t)env_pen(env, ilbm.cmap[i][0], ilbm.cmap[i][1], ilbm.cmap[i][2]) : 0;
	for (y = 0; y < ilbm.bm.h; y++)
		for (x = 0; x < ilbm.bm.w; x++) {
			uint8_t *p = &ilbm.bm.px[y * ilbm.bm.stride + x];

			*p = map[*p];
		}
	if (ilbm.mask.px)
		bm_free(&ilbm.mask);
	if (env->pattern[which].px)
		bm_free(&env->pattern[which]);
	env->pattern[which] = ilbm.bm;
	env_logf(env, "backdrop: %s, %dx%d, %d colours", path, ilbm.bm.w, ilbm.bm.h, ilbm.ncolours);
	return 0;
}
