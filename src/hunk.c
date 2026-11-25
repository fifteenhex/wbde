/*
 * HUNK_HEADER, then per hunk HUNK_CODE / HUNK_DATA / HUNK_BSS followed by
 * HUNK_RELOC32 (and friends) and HUNK_END. Every hunk is laid out in one
 * buffer at HUNK_BASE upwards, longword aligned, and the 32-bit relocations
 * are applied, so pointers in the data can simply be followed.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "hunk.h"

#define HUNK_UNIT	0x3e7
#define HUNK_NAME	0x3e8
#define HUNK_CODE	0x3e9
#define HUNK_DATA	0x3ea
#define HUNK_BSS	0x3eb
#define HUNK_RELOC32	0x3ec
#define HUNK_RELOC16	0x3ed
#define HUNK_RELOC8	0x3ee
#define HUNK_EXT	0x3ef
#define HUNK_SYMBOL	0x3f0
#define HUNK_DEBUG	0x3f1
#define HUNK_END	0x3f2
#define HUNK_HEADER	0x3f3
#define HUNK_RELOC32SHORT 0x3fc
#define HUNK_DREL32	0x3f7

struct rd {
	const uint8_t *p;
	size_t len, at;
	int bad;
};

static uint32_t rl(struct rd *r)
{
	const uint8_t *p;

	if (r->bad || r->len - r->at < 4) {
		r->bad = 1;
		return 0;
	}
	p = r->p + r->at;
	r->at += 4;
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint16_t rw(struct rd *r)
{
	const uint8_t *p;

	if (r->bad || r->len - r->at < 2) {
		r->bad = 1;
		return 0;
	}
	p = r->p + r->at;
	r->at += 2;
	return (uint16_t)(p[0] << 8 | p[1]);
}

static void reloc(struct hunkimg *h, int cur, int target, uint32_t off, int *bad)
{
	uint8_t *p;
	uint32_t v;

	if (target >= h->nhunks || off + 4 > h->len[cur]) {
		*bad = 1;
		return;
	}
	p = h->mem + (h->addr[cur] - h->base) + off;
	v = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
	v += h->addr[target];
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

int hunk_load(const uint8_t *file, size_t len, struct hunkimg *h)
{
	struct rd r = { file, len, 0, 0 };
	uint32_t first, last, total = 0, i;
	int cur = -1;

	memset(h, 0, sizeof(*h));
	if (rl(&r) != HUNK_HEADER)
		return -EINVAL;
	while (!r.bad) {
		uint32_t n = rl(&r);

		if (!n)
			break;
		r.at += (size_t)n * 4;
	}
	rl(&r);			/* table size */
	first = rl(&r);
	last = rl(&r);
	if (r.bad || last < first || last - first + 1 > HUNK_MAX)
		return -EINVAL;
	h->nhunks = (int)(last - first + 1);
	h->base = HUNK_BASE;
	for (i = 0; i < (uint32_t)h->nhunks; i++) {
		uint32_t sz = rl(&r);

		if ((sz >> 30) == 3)
			rl(&r);	/* memory attributes follow */
		h->len[i] = (sz & 0x3fffffff) * 4;
		h->addr[i] = h->base + total;
		total += h->len[i];
		if (total > (16u << 20))
			return -EINVAL;
	}
	if (r.bad)
		return -EINVAL;
	h->size = total;
	h->mem = calloc(1, total ? total : 4);
	if (!h->mem)
		return -ENOMEM;

	while (!r.bad && r.at < len) {
		uint32_t type = rl(&r) & 0x3fffffff, n, k;

		switch (type) {
		case HUNK_CODE:
		case HUNK_DATA:
			n = rl(&r) * 4;
			cur++;
			if (cur >= h->nhunks || n > h->len[cur] || n > len - r.at) {
				r.bad = 1;
				break;
			}
			memcpy(h->mem + (h->addr[cur] - h->base), file + r.at, n);
			r.at += n;
			break;
		case HUNK_BSS:
			rl(&r);
			cur++;
			break;
		case HUNK_RELOC32:
		case HUNK_DREL32:
		case HUNK_RELOC32SHORT:
			for (;;) {
				int shortform = type != HUNK_RELOC32;
				uint32_t target;

				n = shortform ? rw(&r) : rl(&r);
				if (!n || r.bad)
					break;
				target = shortform ? rw(&r) : rl(&r);
				for (k = 0; k < n && !r.bad; k++) {
					uint32_t off = shortform ? rw(&r) : rl(&r);
					int bad = 0;

					if (cur >= 0)
						reloc(h, cur, (int)target, off, &bad);
					if (bad)
						r.bad = 1;
				}
			}
			if (type != HUNK_RELOC32 && (r.at & 2))
				r.at += 2;
			break;
		case HUNK_RELOC16:
		case HUNK_RELOC8:
			for (;;) {
				n = rl(&r);
				if (!n || r.bad)
					break;
				r.at += 4 + (size_t)n * 4;
			}
			break;
		case HUNK_SYMBOL:
			for (;;) {
				n = rl(&r);
				if (!n || r.bad)
					break;
				r.at += (size_t)(n & 0xffffff) * 4 + 4;
			}
			break;
		case HUNK_DEBUG:
		case HUNK_NAME:
		case HUNK_UNIT:
			n = rl(&r);
			r.at += (size_t)n * 4;
			break;
		case HUNK_END:
			break;
		default:
			r.bad = 1;
			break;
		}
	}
	if (cur < 0) {
		hunk_free(h);
		return -EINVAL;
	}
	return 0;
}

void hunk_free(struct hunkimg *h)
{
	free(h->mem);
	memset(h, 0, sizeof(*h));
}
