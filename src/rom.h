/* Kickstart ROM access: read it out of the machine, find what is in it. */
#ifndef WBDE_ROM_H
#define WBDE_ROM_H

#include <stdint.h>
#include <stddef.h>

#define ROM_BASE	0x00f80000u
#define ROM_SIZE	0x00080000u

struct rom {
	uint8_t *data;
	uint32_t base;		/* address the ROM is linked at */
	uint32_t size;
	char source[64];	/* where it came from */
	unsigned int version, revision;	/* exec's, from the header */
};

struct rom_resident {
	uint32_t addr;
	uint8_t flags, version, type;
	int8_t pri;
	const char *name;
	const char *idstring;
	uint32_t init;
	uint32_t endskip;
};

/*
 * path NULL: $WBDE_ROM if set, else the live ROM through /dev/mem.
 * Returns 0 or -errno; fills in source either way.
 */
int rom_load(struct rom *rom, const char *path);
void rom_free(struct rom *rom);
int rom_checksum_ok(const struct rom *rom);

static inline int rom_in(const struct rom *rom, uint32_t addr, uint32_t len)
{
	return addr >= rom->base && len <= rom->size &&
	       addr - rom->base <= rom->size - len;
}

static inline const uint8_t *rom_ptr(const struct rom *rom, uint32_t addr)
{
	return rom->data + (addr - rom->base);
}

static inline uint16_t rom_w(const struct rom *rom, uint32_t addr)
{
	const uint8_t *p = rom_ptr(rom, addr);
	return (uint16_t)(p[0] << 8 | p[1]);
}

static inline uint32_t rom_l(const struct rom *rom, uint32_t addr)
{
	const uint8_t *p = rom_ptr(rom, addr);
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* A NUL terminated string at addr, or NULL if it runs off the end */
const char *rom_str(const struct rom *rom, uint32_t addr);

/* Every RomTag in the image; returns the count, fills up to max */
int rom_residents(const struct rom *rom, struct rom_resident *out, int max);

#endif
