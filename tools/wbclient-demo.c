/*
 * wbclient-demo: a client of wbde's window protocol. Opens a window, draws
 * in the Workbench pens with the screen's own topaz, follows the mouse,
 * redraws when sized, has a menu, and goes when its close gadget is hit.
 * amigafmt's intuition.library talks to wbde the same way.
 */
#include <stdio.h>
#include <string.h>

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
	int x, y;

	fill(0, 0, W.width, W.height, pens[WBP_BACKGROUNDPEN]);
	for (y = 0; y < W.height; y += 8)
		for (x = (y / 8 + pattern) % 2 * 8; x < W.width; x += 16)
			fill(x, y, 8, 8, pattern ? pens[WBP_FILLPEN] : pens[WBP_SHINEPEN]);
	fill(4, 4, W.width - 8, C.font.ysize * 3 + 6, pens[WBP_BACKGROUNDPEN]);
	wbc_text(&C, &W, 8, 6, "Hello from a wbde client", pens[WBP_TEXTPEN]);
	snprintf(line, sizeof(line), "%dx%d, %d clicks", W.width, W.height, clicks);
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
	uint32_t win;

	if (wbc_connect(&C, argc > 1 ? argv[1] : NULL, "wbclient-demo")) {
		fprintf(stderr, "wbclient-demo: is wbde running?\n");
		return 1;
	}
	wbc_get_font(&C);
	if (wbc_open(&C, &W, 120, 60, 260, 80,
		     WBP_WF_CLOSE | WBP_WF_DEPTH | WBP_WF_ZOOM | WBP_WF_SIZE | WBP_WF_DRAG |
		     WBP_WF_ACTIVATE,
		     IDCMP_CLOSEWINDOW | IDCMP_MOUSEBUTTONS | IDCMP_NEWSIZE | IDCMP_MENUPICK |
		     IDCMP_VANILLAKEY, "wbclient demo")) {
		fprintf(stderr, "wbclient-demo: no window\n");
		return 1;
	}
	wbc_set_menus(&C, &W, menus, 4);
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
