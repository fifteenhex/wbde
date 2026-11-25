/* AmigaOS load files (hunks): enough to read data out of a library or font */
#ifndef WBDE_HUNK_H
#define WBDE_HUNK_H

#include <stdint.h>
#include <stddef.h>

#define HUNK_MAX 64
#define HUNK_BASE 0x00100000u	/* where the first hunk is pretended to be */

struct hunkimg {
	uint8_t *mem;		/* every hunk, one after the other, relocated */
	uint32_t base, size;
	int nhunks;
	uint32_t addr[HUNK_MAX];	/* each hunk's address */
	uint32_t len[HUNK_MAX];
};

int hunk_load(const uint8_t *file, size_t len, struct hunkimg *out);
void hunk_free(struct hunkimg *h);

static inline int hunk_in(const struct hunkimg *h, uint32_t a, uint32_t len)
{
	return a >= h->base && len <= h->size && a - h->base <= h->size - len;
}

static inline uint32_t hunk_l(const struct hunkimg *h, uint32_t a)
{
	const uint8_t *p = h->mem + (a - h->base);
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

#endif
