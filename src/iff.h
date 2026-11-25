/* EA IFF 85: FORM walking, ILBM pictures and the Prefs chunks wbde reads */
#ifndef WBDE_IFF_H
#define WBDE_IFF_H

#include <stdint.h>
#include <stddef.h>

#include "gfx.h"

#define IFF_ID(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

static inline uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static inline uint16_t be16(const uint8_t *p)
{
	return (uint16_t)(p[0] << 8 | p[1]);
}

/*
 * The first chunk `id` inside a FORM of type `type` (0 = any) at the start of
 * buf; returns the chunk's data and its length, or NULL.
 */
const uint8_t *iff_chunk(const uint8_t *buf, size_t len, uint32_t type,
			 uint32_t id, uint32_t *chunklen);

struct ilbm {
	struct bm bm;		/* pens as stored in the picture */
	struct bm mask;		/* 1 where opaque; px NULL if no mask */
	uint8_t cmap[256][3];
	int ncolours;
	int depth;
	int grabx, graby;	/* hot spot (pointers) */
	int transparent;	/* bmhd transparent colour, -1 if none */
};

int ilbm_decode(const uint8_t *buf, size_t len, struct ilbm *out);
void ilbm_free(struct ilbm *ilbm);

/* Read a whole file; returns malloc'd data or NULL */
uint8_t *file_slurp(const char *path, size_t *len, size_t max);

#endif
