/*
 * The Kickstart ROM.
 *
 * On the A4000 it sits at physical 0x00f80000, 512 KB. read() on /dev/mem
 * refuses it (the m68k kernel only translates RAM addresses for read and
 * returns EFAULT), but mmap() of the same range works and maps it uncached,
 * so that is the route. A file can stand in for it ($WBDE_ROM or the path
 * argument), which is how the extraction is developed away from the machine.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rom.h"

static int load_file(struct rom *rom, const char *path)
{
	struct stat st;
	ssize_t n;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	if (fstat(fd, &st) || (st.st_size != 0x40000 && st.st_size != 0x80000 &&
			       st.st_size != 0x100000)) {
		close(fd);
		return -EINVAL;
	}
	rom->size = (uint32_t)st.st_size;
	rom->data = malloc(rom->size);
	if (!rom->data) {
		close(fd);
		return -ENOMEM;
	}
	n = read(fd, rom->data, rom->size);
	close(fd);
	if (n != (ssize_t)rom->size) {
		free(rom->data);
		rom->data = NULL;
		return -EIO;
	}
	snprintf(rom->source, sizeof(rom->source), "%s", path);
	return 0;
}

static int load_devmem(struct rom *rom)
{
	const volatile uint32_t *src;
	uint32_t *dst;
	void *map;
	uint32_t i;
	int fd;

	fd = open("/dev/mem", O_RDONLY | O_SYNC | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	map = mmap(NULL, ROM_SIZE, PROT_READ, MAP_SHARED, fd, ROM_BASE);
	close(fd);
	if (map == MAP_FAILED)
		return -errno;

	rom->size = ROM_SIZE;
	rom->data = malloc(ROM_SIZE);
	if (!rom->data) {
		munmap(map, ROM_SIZE);
		return -ENOMEM;
	}
	/* longwords: every bus cycle costs the same, so take the widest */
	src = map;
	dst = (uint32_t *)rom->data;
	for (i = 0; i < ROM_SIZE / 4; i++)
		dst[i] = src[i];
	munmap(map, ROM_SIZE);
	snprintf(rom->source, sizeof(rom->source), "/dev/mem@0x%08x", ROM_BASE);
	return 0;
}

int rom_load(struct rom *rom, const char *path)
{
	int ret;

	memset(rom, 0, sizeof(*rom));
	if (!path)
		path = getenv("WBDE_ROM");
	ret = path ? load_file(rom, path) : load_devmem(rom);
	if (ret)
		return ret;

	/*
	 * A 512 KB Kickstart is linked at 0xf80000 and a 256 KB one at
	 * 0xfc0000; the second longword is the reset PC, which says which.
	 */
	rom->base = rom->size == 0x40000 ? 0xfc0000u : 0xf80000u;
	if (rom->size == 0x100000)
		rom->base = 0xe00000u;	/* extended, not expected here */
	rom->version = rom_w(rom, rom->base + 12);
	rom->revision = rom_w(rom, rom->base + 14);
	return 0;
}

void rom_free(struct rom *rom)
{
	free(rom->data);
	rom->data = NULL;
}

/* The sum of every longword, with the carries added back in, is ~0 */
int rom_checksum_ok(const struct rom *rom)
{
	uint32_t sum = 0, i;

	for (i = 0; i < rom->size; i += 4) {
		uint32_t v = rom_l(rom, rom->base + i);
		uint32_t s = sum + v;

		if (s < sum)
			s++;
		sum = s;
	}
	return sum == 0xffffffffu;
}

const char *rom_str(const struct rom *rom, uint32_t addr)
{
	uint32_t a;

	if (!rom_in(rom, addr, 1))
		return NULL;
	for (a = addr; rom_in(rom, a, 1); a++)
		if (!*rom_ptr(rom, a))
			return (const char *)rom_ptr(rom, addr);
	return NULL;
}

int rom_residents(const struct rom *rom, struct rom_resident *out, int max)
{
	uint32_t a;
	int n = 0;

	for (a = rom->base; rom_in(rom, a, 26); a += 2) {
		struct rom_resident r;

		if (rom_w(rom, a) != 0x4afc || rom_l(rom, a + 2) != a)
			continue;
		r.addr = a;
		r.endskip = rom_l(rom, a + 6);
		r.flags = *rom_ptr(rom, a + 10);
		r.version = *rom_ptr(rom, a + 11);
		r.type = *rom_ptr(rom, a + 12);
		r.pri = (int8_t)*rom_ptr(rom, a + 13);
		r.name = rom_str(rom, rom_l(rom, a + 14));
		r.idstring = rom_str(rom, rom_l(rom, a + 18));
		r.init = rom_l(rom, a + 22);
		if (n < max)
			out[n] = r;
		n++;
	}
	return n;
}
