/*
 * wbclient-demo: a client of wbde's window protocol. Opens a window, draws
 * in the Workbench pens with the screen's own topaz, follows the mouse,
 * redraws when sized, has a menu, and goes when its close gadget is hit.
 * amigafmt's intuition.library talks to wbde the same way.
 *
 *   wbclient-demo [--vram] [--unscaled] [--bench N] [socket]
 *
 * --vram asks for the window buffer in video memory (a GPU display only:
 * the drawing below goes straight to the card), --bench N redraws the
 * window N times, 20 ms apart, and says what drawing a frame cost here.
 * --unscaled asks for the content in display pixels, 1:1 (WBP_WF_UNSCALED):
 * the checks are then one pixel each, which shows at a glance whether
 * anything resampled them, and clicks report content pixels.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "wbclient.h"

static struct wbc C;
static struct wbc_window W;
static int clicks, lastx = -1, lasty = -1, pattern;

static void fill(int x, int y, int w, int h, int pen)
{
	int r, i;

	for (r = y; r < y + h && r < W.height; r++)
		for (i = x; i < x + w && i < W.width; i++)
			if (r >= 0 && i >= 0)
				W.pixels[r * W.stride + i] = (uint8_t)pen;
}

static void draw(void)
{
	const uint8_t *pens = C.screen.pens;
	char line[80];
	int x, y, k = W.unscaled ? 1 : 8;

	fill(0, 0, W.width, W.height, pens[WBP_BACKGROUNDPEN]);
	for (y = 0; y < W.height; y += k)
		for (x = (y / k + pattern) % 2 * k; x < W.width; x += 2 * k)
			fill(x, y, k, k, pattern ? pens[WBP_FILLPEN] : pens[WBP_SHINEPEN]);
	fill(4, 4, W.width - 8, C.font.ysize * 3 + 6, pens[WBP_BACKGROUNDPEN]);
	wbc_text(&C, &W, 8, 6, "Hello from a wbde client", pens[WBP_TEXTPEN]);
	snprintf(line, sizeof(line), "%dx%d%s, %d clicks", W.width, W.height,
		 W.unscaled ? " display pixels" : "", clicks);
	wbc_text(&C, &W, 8, 6 + C.font.ysize + 1, line, pens[WBP_TEXTPEN]);
	if (lastx >= 0) {
		snprintf(line, sizeof(line), "last click at %d,%d", lastx, lasty);
		wbc_text(&C, &W, 8, 6 + 2 * (C.font.ysize + 1), line, pens[WBP_HIGHLIGHTTEXTPEN]);
	}
	wbc_damage(&C, &W, 0, 0, W.width, W.height);
}

int main(int argc, char **argv)
{
	static const struct wbp_menuitem menus[] = {
		{ WBP_MENU, 0, 0, 0, 0, 0, "Project" },
		{ WBP_ITEM, 0, 'P', 0, 1, 0, "Pattern" },
		{ WBP_BAR, 0, 0, 0, 0, 0, "" },
		{ WBP_ITEM, 0, 'Q', 0, 2, 0, "Quit" },
	};
	struct wbp_event e;
	uint32_t win, vram = 0, unscaled = 0;
	const char *sock = NULL;
	int a, bench = 0;

	for (a = 1; a < argc; a++) {
		if (!strcmp(argv[a], "--vram"))
			vram = WBP_WF_VRAM;
		else if (!strcmp(argv[a], "--unscaled"))
			unscaled = WBP_WF_UNSCALED;
		else if (!strcmp(argv[a], "--bench") && a + 1 < argc)
			bench = atoi(argv[++a]);
		else
			sock = argv[a];
	}
	if (wbc_connect(&C, sock, "wbclient-demo")) {
		fprintf(stderr, "wbclient-demo: is wbde running?\n");
		return 1;
	}
	wbc_get_font(&C);
	if (wbc_open(&C, &W, 120, 60, bench ? 400 : unscaled ? 401 : 260,
		     bench ? 150 : unscaled ? 121 : 80,
		     WBP_WF_CLOSE | WBP_WF_DEPTH | WBP_WF_ZOOM | WBP_WF_SIZE | WBP_WF_DRAG |
		     WBP_WF_ACTIVATE | vram | unscaled,
		     IDCMP_CLOSEWINDOW | IDCMP_MOUSEBUTTONS | IDCMP_NEWSIZE | IDCMP_MENUPICK |
		     IDCMP_VANILLAKEY, "wbclient demo")) {
		fprintf(stderr, "wbclient-demo: no window\n");
		return 1;
	}
	wbc_set_menus(&C, &W, menus, 4);
	if (unscaled)
		printf("wbclient-demo: %s, %dx%d content, a screen pixel %gx%g display pixels\n",
		       W.unscaled ? "unscaled granted" : "unscaled NOT granted", W.width, W.height,
		       W.scalex, W.scaley);
	if (bench) {
		struct timespec t0, t1;
		double sum = 0;
		int i;

		for (i = 0; i < bench; i++) {
			clock_gettime(CLOCK_MONOTONIC, &t0);
			pattern ^= 1;
			draw();
			clock_gettime(CLOCK_MONOTONIC, &t1);
			sum += (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
			while (wbc_wait(&C, &e, &win, 20) > 0)
				;
		}
		printf("wbclient-demo: %dx%d buffer %s, %d frames, %.2f ms a frame to draw\n",
		       W.width, W.height, W.vram ? "in VRAM" : "in RAM (memfd)", bench, sum / bench);
		wbc_close(&C, &W);
		return 0;
	}
	draw();
	while (wbc_wait(&C, &e, &win, -1) > 0) {
		switch (e.class) {
		case IDCMP_CLOSEWINDOW:
			wbc_close(&C, &W);
			return 0;
		case IDCMP_MOUSEBUTTONS:
			if (e.code == SELECTDOWN) {
				clicks++;
				lastx = e.mousex;
				lasty = e.mousey;
				draw();
			}
			break;
		case IDCMP_NEWSIZE:
			draw();
			break;
		case IDCMP_VANILLAKEY:
			if (e.code == 'q')
				goto out;
			pattern ^= 1;
			draw();
			break;
		case IDCMP_MENUPICK:
			if (e.code == 2)
				goto out;
			pattern ^= 1;
			draw();
			break;
		}
	}
out:
	wbc_close(&C, &W);
	return 0;
}
