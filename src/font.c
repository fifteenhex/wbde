/*
 * TextFont, as graphics.library defines it:
 *
 *   0  struct Message tf_Message  (ln_Name at +10)
 *  20  UWORD tf_YSize   22 UBYTE tf_Style   23 UBYTE tf_Flags
 *  24  UWORD tf_XSize   26 UWORD tf_Baseline 28 UWORD tf_BoldSmear
 *  30  UWORD tf_Accessors
 *  32  UBYTE tf_LoChar  33 UBYTE tf_HiChar
 *  34  APTR tf_CharData 38 UWORD tf_Modulo
 *  40  APTR tf_CharLoc  (ULONG per char: bit offset << 16 | bit width)
 *  44  APTR tf_CharSpace (WORD per char, NULL for a fixed font)
 *  48  APTR tf_CharKern  (WORD per char, NULL for a fixed font)
 *
 * There are hichar - lochar + 2 entries: the last one is the image drawn
 * for any character the font has no glyph for.
 */
#include <stdlib.h>
#include <string.h>

#include "font.h"
#include "rom.h"

#define FPF_PROPORTIONAL 0x20

struct mem {
	const uint8_t *p;
	uint32_t base, size;
};

static int in(const struct mem *m, uint32_t a, uint32_t len)
{
	return a >= m->base && len <= m->size && a - m->base <= m->size - len;
}

static uint32_t L(const struct mem *m, uint32_t a)
{
	const uint8_t *p = m->p + (a - m->base);
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint16_t W(const struct mem *m, uint32_t a)
{
	const uint8_t *p = m->p + (a - m->base);
	return (uint16_t)(p[0] << 8 | p[1]);
}

static uint8_t B(const struct mem *m, uint32_t a)
{
	return m->p[a - m->base];
}

static uint32_t sL(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint16_t sW(const uint8_t *p)
{
	return (uint16_t)(p[0] << 8 | p[1]);
}

/* does this look like a TextFont? t is the struct, pointers go into m */
static int plausible(const struct mem *m, const uint8_t *t)
{
	unsigned int ysize, xsize, lo, hi, modulo, n;
	uint32_t data, loc;

	ysize = sW(t + 20);
	xsize = sW(t + 24);
	lo = t[32];
	hi = t[33];
	data = sL(t + 34);
	modulo = sW(t + 38);
	loc = sL(t + 40);
	if (ysize < 4 || ysize > 64 || xsize < 2 || xsize > 64 || lo > hi)
		return 0;
	if (sW(t + 26) >= ysize || modulo < 2 || modulo > 1024)
		return 0;
	n = hi - lo + 2;
	if (!in(m, data, modulo * ysize) || !in(m, loc, n * 4))
		return 0;
	return 1;
}

static struct font *font_build(const struct mem *mm, const uint8_t *t)
{
	const struct mem m = *mm;
	const uint8_t *p = m.p;
	uint32_t base = m.base;
	uint32_t data, loc, space, kern, name;
	struct font *f;
	unsigned int n, i;

	if (!plausible(&m, t))
		return NULL;
	f = calloc(1, sizeof(*f));
	if (!f)
		return NULL;

	name = sL(t + 10);
	if (in(&m, name, 1)) {
		for (i = 0; i < sizeof(f->name) - 1 && in(&m, name + i, 1) &&
		     B(&m, name + i); i++)
			f->name[i] = (char)B(&m, name + i);
	}
	f->ysize = sW(t + 20);
	f->xsize = sW(t + 24);
	f->baseline = sW(t + 26);
	f->lochar = t[32];
	f->hichar = t[33];
	data = sL(t + 34);
	f->modulo = sW(t + 38);
	loc = sL(t + 40);
	space = sL(t + 44);
	kern = sL(t + 48);
	n = (unsigned int)(f->hichar - f->lochar + 2);
	if (!(t[23] & FPF_PROPORTIONAL) || !in(&m, space, n * 2))
		space = 0;
	if (!in(&m, kern, n * 2))
		kern = 0;

	f->chardata = malloc((size_t)f->modulo * f->ysize);
	if (!f->chardata) {
		free(f);
		return NULL;
	}
	memcpy(f->chardata, p + (data - base), (size_t)f->modulo * f->ysize);

	for (i = 0; i < 257; i++) {
		unsigned int idx;
		uint32_t cl;
		struct glyph *g = &f->glyphs[i];

		if (i == 256)
			idx = n - 1;
		else if ((int)i < f->lochar || (int)i > f->hichar)
			continue;
		else
			idx = i - (unsigned int)f->lochar;
		cl = L(&m, loc + idx * 4);
		g->offset = (uint16_t)(cl >> 16);
		g->width = (uint16_t)cl;
		if ((g->offset + g->width + 7) / 8 > (unsigned int)f->modulo)
			g->width = 0;
		g->space = space ? (int16_t)W(&m, space + idx * 2) : (int16_t)f->xsize;
		g->kern = kern ? (int16_t)W(&m, kern + idx * 2) : 0;
	}
	for (i = 0; i < 256; i++)
		if ((int)i < f->lochar || (int)i > f->hichar)
			f->glyphs[i] = f->glyphs[256];
	return f;
}

struct font *font_from_textfont(const uint8_t *p, uint32_t base, uint32_t size,
				uint32_t tf)
{
	struct mem m = { p, base, size };

	if (!in(&m, tf, 52))
		return NULL;
	return font_build(&m, p + (tf - base));
}

void font_free(struct font *f)
{
	if (!f)
		return;
	free(f->chardata);
	free(f);
}

int font_textlen(const struct font *f, const char *s, int len)
{
	int x = 0, i;

	if (len < 0)
		len = (int)strlen(s);
	for (i = 0; i < len; i++)
		x += font_glyph(f, (uint8_t)s[i])->space + (f->bold ? 1 : 0);
	return x;
}

/*
 * graphics.library does not keep its ROM fonts as TextFonts: it keeps an
 * exec InitStruct() table for each and builds the TextFont in RAM at boot.
 * In Kickstart 3.1.4 topaz/8 is at 0xfbc7cc and topaz/9 at 0xfbbcd8, both
 * starting with the command 0x80 0x0a <ln_Name> (byte offset 10, one long:
 * the node's name). So: find "x.font" strings, find InitStruct commands that
 * store one as ln_Name, run the table into a scratch TextFont and keep it if
 * it makes sense.
 *
 * InitStruct: a command byte, bits 7-6 the form (0 data follows, 1 one item
 * repeated, 2 byte offset then data, 3 24-bit offset then data), bits 5-4
 * the size (long, word, byte), bits 3-0 the count - 1; 0 ends the table.
 * Word and long data, and every command, start on an even address.
 */
static int initstruct(const struct mem *m, uint32_t a, uint8_t *out, unsigned int outlen)
{
	unsigned int off = 0, guard;

	memset(out, 0, outlen);
	for (guard = 0; guard < 64; guard++) {
		unsigned int cmd, form, size, count, sz, i;

		a = (a + 1) & ~1u;
		if (!in(m, a, 1))
			return -1;
		cmd = B(m, a++);
		if (!cmd)
			return 0;
		form = cmd >> 6;
		size = (cmd >> 4) & 3;
		count = (cmd & 15) + 1;
		if (size == 3)
			return -1;
		sz = size == 0 ? 4 : size == 1 ? 2 : 1;
		if (form == 2) {
			if (!in(m, a, 1))
				return -1;
			off = B(m, a++);
		} else if (form == 3) {
			if (!in(m, a, 3))
				return -1;
			off = (unsigned int)B(m, a) << 16 | (unsigned int)B(m, a + 1) << 8 | B(m, a + 2);
			a += 3;
		}
		if (sz > 1)
			a = (a + 1) & ~1u;
		for (i = 0; i < count; i++) {
			if (off + sz > outlen || !in(m, a, sz))
				return -1;
			memcpy(out + off, m->p + (a - m->base), sz);
			off += sz;
			if (form != 1)
				a += sz;
		}
		if (form == 1)
			a += sz;
	}
	return -1;
}

int font_rom_list(const struct rom *rom, struct romfont *out, int max)
{
	struct mem m = { rom->data, rom->base, rom->size };
	uint32_t a, s;
	int n = 0;

	for (a = rom->base; rom_in(rom, a, 6); a += 2) {
		const char *str;
		size_t len;

		if (rom_w(rom, a) != 0x800a)
			continue;
		s = rom_l(rom, a + 2);
		if (!rom_in(rom, s, 6))
			continue;
		str = rom_str(rom, s);
		if (!str || (len = strlen(str)) < 6 || strcmp(str + len - 5, ".font"))
			continue;
		if (n < max) {
			out[n].at = a;
			if (initstruct(&m, a, out[n].tf, sizeof(out[n].tf)) ||
			    !plausible(&m, out[n].tf))
				continue;
		}
		n++;
	}
	return n;
}

struct font *font_from_romfont(const struct rom *rom, const struct romfont *rf)
{
	struct mem m = { rom->data, rom->base, rom->size };

	return font_build(&m, rf->tf);
}

struct font *font_from_rom(const struct rom *rom, const char *name, int ysize)
{
	struct romfont rf[16];
	int n, i;

	n = font_rom_list(rom, rf, 16);
	for (i = 0; i < n && i < 16; i++) {
		struct font *f = font_from_romfont(rom, &rf[i]);

		if (f && !strcmp(f->name, name) && f->ysize == ysize)
			return f;
		font_free(f);
	}
	return NULL;
}
