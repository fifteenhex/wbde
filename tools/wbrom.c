/* wbrom: look at the Kickstart ROM the way wbde does. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/rom.h"
#include "../src/font.h"
#include "../src/gfx.h"
#include "../src/icon.h"
#include "../src/iff.h"
#include "../src/hunk.h"

int main(int argc, char **argv)
{
	struct rom_resident res[128];
	struct rom rom;
	const char *cmd = argc > 1 ? argv[1] : "info";
	int ret, n, i;

	if (!strcmp(cmd, "defaults") && argc > 2) {
		struct hunkimg h;
		size_t len;
		uint8_t *buf = file_slurp(argv[2], &len, 4 << 20);
		uint32_t a;

		if (!buf || hunk_load(buf, len, &h)) {
			fprintf(stderr, "%s: not a load file\n", argv[2]);
			return 1;
		}
		for (a = h.base; hunk_in(&h, a, 78); a += 2) {
			struct memimg m = { h.mem, h.base, h.size };
			struct icon ic;
			int x, y;

			if (icon_from_memory(&m, a, &ic))
				continue;
			printf("%06x: type %d %dx%d%s deftool %s\n", a, ic.type, ic.w, ic.h,
			       ic.img[1].px ? " alt" : "", ic.deftool ? ic.deftool : "-");
			for (y = 0; y < ic.h; y++) {
				for (x = 0; x < ic.w; x++)
					putchar(".123456789abcdef"[ic.img[0].px[y * ic.img[0].stride + x] & 15]);
				putchar('\n');
			}
			icon_free(&ic);
		}
		return 0;
	}

	if (!strcmp(cmd, "icon") || !strcmp(cmd, "scan")) {
		for (i = 2; i < argc; i++) {
			size_t len, at;
			uint8_t *buf = file_slurp(argv[i], &len, 4 << 20);

			if (!buf) {
				perror(argv[i]);
				continue;
			}
			for (at = 0; at + 78 <= len; at += 2) {
				struct icon ic;
				int x, y, k;

				if (be16(buf + at) != 0xe310 || be16(buf + at + 2) != 1)
					continue;
				if (icon_parse(buf + at, len - at, &ic, NULL, NULL)) {
					printf("%s+%zx: bad icon\n", argv[i], at);
					continue;
				}
				printf("%s+%zx: type %d, %dx%d%s%s, pos %d,%d, deftool %s, %d tooltypes",
				       argv[i], at, ic.type, ic.w, ic.h, ic.img[1].px ? ", alt image" : "",
				       ic.glow ? ", glow" : "", ic.x, ic.y,
				       ic.deftool ? ic.deftool : "-", ic.ntooltypes);
				if (ic.has_window)
					printf(", window %d,%d %dx%d flags %x view %d",
					       ic.win.x, ic.win.y, ic.win.w, ic.win.h,
					       ic.dd_flags, ic.dd_viewmodes);
				printf("\n");
				for (k = 0; k < ic.ntooltypes; k++)
					printf("    %s\n", ic.tooltypes[k]);
				for (y = 0; y < ic.h && argc > 0; y++) {
					for (x = 0; x < ic.w; x++) {
						int v = ic.img[0].px[y * ic.img[0].stride + x];
						putchar(v == ic.key ? '.' : "0123456789abcdef"[v & 15]);
					}
					putchar('\n');
				}
				icon_free(&ic);
				if (!strcmp(cmd, "icon"))
					break;
			}
			free(buf);
		}
		return 0;
	}

	ret = rom_load(&rom, getenv("WBDE_ROM"));
	if (ret) {
		fprintf(stderr, "wbrom: cannot read the ROM (%s): %s\n",
			rom.source[0] ? rom.source : "/dev/mem", strerror(-ret));
		return 1;
	}

	if (!strcmp(cmd, "dump") && argc > 2) {
		FILE *f = fopen(argv[2], "wb");

		if (!f || fwrite(rom.data, 1, rom.size, f) != rom.size) {
			perror(argv[2]);
			return 1;
		}
		fclose(f);
		return 0;
	}

	printf("source %s, %u KB at 0x%06x, exec %u.%u, checksum %s\n",
	       rom.source, rom.size / 1024, rom.base, rom.version, rom.revision,
	       rom_checksum_ok(&rom) ? "ok" : "BAD");

	if (!strcmp(cmd, "residents")) {
		n = rom_residents(&rom, res, 128);
		for (i = 0; i < n && i < 128; i++)
			printf("%06x %-24s v%-3u t%-3u pri %4d  %s",
			       res[i].addr, res[i].name ? res[i].name : "?",
			       res[i].version, res[i].type, res[i].pri,
			       res[i].idstring ? res[i].idstring : "\n");
	}
	if (!strcmp(cmd, "fonts")) {
		struct romfont rf[16];

		n = font_rom_list(&rom, rf, 16);
		for (i = 0; i < n && i < 16; i++) {
			struct font *f = font_from_romfont(&rom, &rf[i]);
			const char *sample = argc > 2 ? argv[2] : "Workbench";
			int row, c;

			if (!f)
				continue;
			printf("TextFont InitStruct at %06x: %s/%d, xsize %d, baseline %d, chars %d-%d, modulo %d\n",
			       rf[i].at, f->name, f->ysize, f->xsize, f->baseline,
			       f->lochar, f->hichar, f->modulo);
			for (row = 0; row < f->ysize; row++) {
				for (c = 0; sample[c]; c++) {
					const struct glyph *g = font_glyph(f, (uint8_t)sample[c]);
					int x;

					for (x = 0; x < g->space; x++) {
						int b = g->offset + x - g->kern;
						int on = x - g->kern >= 0 && x - g->kern < g->width &&
							 (f->chardata[row * f->modulo + b / 8] & (0x80 >> (b & 7)));
						putchar(on ? '#' : '.');
					}
				}
				putchar('\n');
			}
			font_free(f);
		}
	}
	return 0;
}
