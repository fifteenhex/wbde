/* Amiga TextFonts, as graphics.library lays them out, made chunky-friendly */
#ifndef WBDE_FONT_H
#define WBDE_FONT_H

#include <stdint.h>

struct rom;

struct glyph {
	uint16_t offset;	/* bit offset into each row of chardata */
	uint16_t width;		/* bits of image */
	int16_t space;		/* advance */
	int16_t kern;		/* offset before drawing */
};

struct font {
	char name[32];
	int ysize, xsize, baseline;
	int lochar, hichar;
	int modulo;
	int bold;		/* algorithmic emboldening, for the menus */
	uint8_t *chardata;	/* modulo * ysize bytes */
	struct glyph glyphs[257];	/* 256 = the "no glyph" image */
};

/*
 * Read a TextFont whose pointers are absolute addresses in a memory image
 * that starts at `base` (the ROM, or a relocated disk font).
 */
struct font *font_from_textfont(const uint8_t *mem, uint32_t base, uint32_t size,
				uint32_t tf);
/* topaz.font of the given height, out of the Kickstart ROM */
struct font *font_from_rom(const struct rom *rom, const char *name, int ysize);
/* every TextFont the ROM holds, for wbrom */
struct romfont {
	uint32_t at;		/* the InitStruct table */
	uint8_t tf[64];		/* the TextFont it builds */
};
int font_rom_list(const struct rom *rom, struct romfont *out, int max);
struct font *font_from_romfont(const struct rom *rom, const struct romfont *rf);
void font_free(struct font *f);

static inline const struct glyph *font_glyph(const struct font *f, uint8_t c)
{
	if (c < f->lochar || c > f->hichar)
		return &f->glyphs[256];
	return &f->glyphs[c];
}

int font_textlen(const struct font *f, const char *s, int len);

#endif
