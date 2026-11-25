/*
 * wbde: the screen, the event loop, and the glue between input, the window
 * core, Workbench, console windows and clients.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "con.h"
#include "disp.h"
#include "font.h"
#include "input.h"
#include "menu.h"
#include "prefs.h"
#include "wb.h"
#include "wm.h"

static struct disp D;
static struct screen S;
static struct wbenv ENV;
static struct wb W;
static volatile sig_atomic_t got_chld, got_quit, got_shot;
static uint64_t vclock;		/* scripted runs keep their own time */
static int scripted;

static uint64_t now_ms(void)
{
	struct timespec ts;

	if (scripted)
		return vclock;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void on_signal(int sig)
{
	if (sig == SIGCHLD)
		got_chld = 1;
	else if (sig == SIGUSR1)
		got_shot = 1;
	else
		got_quit = 1;
}

int disp_screenshot(const struct disp *d, const uint8_t pal[256][3], const char *path)
{
	FILE *f = fopen(path, "wb");
	int x, y;

	if (!f)
		return -errno;
	fprintf(f, "P6\n%d %d\n255\n", d->fb.w, d->fb.h * d->scale);
	for (y = 0; y < d->fb.h * d->scale; y++)
		for (x = 0; x < d->fb.w; x++)
			fwrite(pal[d->fb.px[(y / d->scale) * d->fb.stride + x]], 3, 1, f);
	fclose(f);
	return 0;
}

static const struct menustrip *strip_for(struct screen *s)
{
	if (s->active && s->active->menus)
		return s->active->menus;
	return s->menus;
}

static void cb_motion(void *arg, int dx, int dy)
{
	struct screen *s = arg;
	uint64_t now = now_ms();

	wm_motion(s, s->mx + dx, s->my + dy, now);
	if (menu_is_open())
		menu_motion(s, s->mx, s->my);
}

static void cb_button(void *arg, int button, int down)
{
	struct screen *s = arg;
	uint64_t now = now_ms();

	W.now = now;
	if (button == 2) {
		if (down && !menu_is_open() && !wm_busy(s)) {
			struct win *w = win_at(s, s->mx, s->my);

			if (w && (w->flags & WF_RMBTRAP)) {
				wm_button(s, button, down, input_qual(), now);
				return;
			}
			menu_open(s, strip_for(s));
		} else if (!down && menu_is_open()) {
			menu_close(s);
		} else if (!menu_is_open()) {
			wm_button(s, button, down, input_qual(), now);
		}
		return;
	}
	if (menu_is_open())
		return;
	wm_button(s, button, down, input_qual(), now);
}

static void cb_key(void *arg, const struct keyev *k)
{
	struct screen *s = arg;

	if (k->down == 1 && (k->qual & QUAL_RAMIGA) && k->ch && !menu_is_open()) {
		if (menu_shortcut(strip_for(s), k->ch))
			return;
	}
	if (k->down && (k->qual & QUAL_LAMIGA) && (k->ch == 'm' || k->ch == 'n')) {
		/* LAmiga-M/N flip screens; there is only one */
		return;
	}
	wm_key(s, k);
}

static const struct input_cb CB = { cb_motion, cb_button, cb_key, &S };

static void flush(void)
{
	if (ENV.paldirty) {
		ENV.paldirty = 0;
		memcpy(S.palette, ENV.pal, sizeof(S.palette));
		disp_palette(&D, ENV.pal);
	}
	if (S.ndamage) {
		screen_flush(&S, disp_touch, &D);
		disp_present(&D);
	}
}

static void shot(const char *path)
{
	flush();
	if (disp_screenshot(&D, ENV.pal, path))
		fprintf(stderr, "wbde: cannot write %s\n", path);
	else
		fprintf(stderr, "wbde: screenshot %s\n", path);
}

/*
 * Scripts drive wbde without a mouse, for testing away from the machine:
 *   move X Y | down B | up B | click X Y | dclick X Y | menu X Y X2 Y2 |
 *   key CODE [QUAL] | type TEXT | wait MS | shot FILE | quit
 */
static int run_script(const char *path)
{
	char line[512];
	FILE *f = fopen(path, "r");

	if (!f) {
		perror(path);
		return -1;
	}
	while (fgets(line, sizeof(line), f) && !W.quit) {
		int a, b, c, d;
		char arg[400];

		vclock += 20;
		W.now = vclock;
		wb_tick(&W, vclock);
		if (sscanf(line, "move %d %d", &a, &b) == 2) {
			cb_motion(&S, a - S.mx, b - S.my);
		} else if (sscanf(line, "down %d", &a) == 1) {
			cb_button(&S, a, 1);
		} else if (sscanf(line, "up %d", &a) == 1) {
			cb_button(&S, a, 0);
		} else if (sscanf(line, "click %d %d", &a, &b) == 2) {
			cb_motion(&S, a - S.mx, b - S.my);
			cb_button(&S, 1, 1);
			cb_button(&S, 1, 0);
		} else if (sscanf(line, "dclick %d %d", &a, &b) == 2) {
			cb_motion(&S, a - S.mx, b - S.my);
			cb_button(&S, 1, 1);
			cb_button(&S, 1, 0);
			vclock += 100;
			W.now = vclock;
			cb_button(&S, 1, 1);
			cb_button(&S, 1, 0);
		} else if (sscanf(line, "menu %d %d %d %d", &a, &b, &c, &d) == 4) {
			cb_motion(&S, a - S.mx, b - S.my);
			cb_button(&S, 2, 1);
			cb_motion(&S, c - S.mx, d - S.my);
			if (strstr(line, "hold")) {
				flush();
				continue;
			}
			cb_button(&S, 2, 0);
		} else if (sscanf(line, "key %d %d", &a, &b) >= 1) {
			struct keyev k = { (uint16_t)a, 1, 0, 0 };

			if (sscanf(line, "key %d %d", &a, &b) == 2)
				k.qual = (unsigned int)b;
			k.ch = input_keychar(a, k.qual);
			cb_key(&S, &k);
			k.down = 0;
			cb_key(&S, &k);
		} else if (sscanf(line, "type %399[^\n]", arg) == 1) {
			char *p;

			for (p = arg; *p; p++) {
				struct keyev k = { 0, 1, (unsigned char)*p, 0 };

				cb_key(&S, &k);
			}
		} else if (sscanf(line, "wait %d", &a) == 1) {
			uint64_t until = vclock + (uint64_t)a;
			struct pollfd fds[16];
			int n;

			while (vclock < until) {
				n = con_pollfds(fds, 16);
				poll(fds, (nfds_t)n, 10);
				con_service(&S, fds, n);
				if (got_chld) {
					got_chld = 0;
					con_reap(&S);
				}
				vclock += 10;
				wm_tick(&S, vclock);
			}
		} else if (sscanf(line, "shot %399s", arg) == 1) {
			shot(arg);
			continue;
		} else if (!strncmp(line, "quit", 4)) {
			break;
		}
		flush();
	}
	fclose(f);
	return 0;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: wbde [--mode WxH] [--size WxH] [--sys DIR] [--conf FILE] [--rom FILE]\n"
		"            [--script FILE] [--pattern ILBM] [--drawer-pattern ILBM]\n");
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "mode", required_argument, NULL, 'm' },
		{ "size", required_argument, NULL, 'z' },
		{ "sys", required_argument, NULL, 's' },
		{ "conf", required_argument, NULL, 'c' },
		{ "rom", required_argument, NULL, 'r' },
		{ "script", required_argument, NULL, 'S' },
		{ "pattern", required_argument, NULL, 'p' },
		{ "drawer-pattern", required_argument, NULL, 'P' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	const char *mode = "640x480", *size = "640x240", *sys = "/mnt/sys", *conf = NULL, *script = NULL;
	const char *pattern = NULL, *dpattern = NULL;
	struct sigaction sa;
	int o;

	while ((o = getopt_long(argc, argv, "m:z:s:c:r:S:p:P:h", opts, NULL)) != -1) {
		switch (o) {
		case 'm': mode = optarg; break;
		case 'z': size = optarg; break;
		case 's': sys = optarg; break;
		case 'c': conf = optarg; break;
		case 'r': setenv("WBDE_ROM", optarg, 1); break;
		case 'S': script = optarg; break;
		case 'p': pattern = optarg; break;
		case 'P': dpattern = optarg; break;
		default: usage(); return o == 'h' ? 0 : 1;
		}
	}
	if (!conf && access("/mnt/lnx/wbde/wbde.conf", R_OK) == 0)
		conf = "/mnt/lnx/wbde/wbde.conf";
	scripted = script != NULL;

	if (env_load(&ENV, sys)) {
		fprintf(stderr, "wbde: no topaz: the Kickstart ROM could not be read\n");
		return 1;
	}
	if (pattern && env_load_pattern(&ENV, 0, pattern))
		env_logf(&ENV, "backdrop: cannot load %s", pattern);
	if (dpattern && env_load_pattern(&ENV, 1, dpattern))
		env_logf(&ENV, "drawer pattern: cannot load %s", dpattern);
	if (disp_open(&D, mode, size))
		return 1;
	env_logf(&ENV, "display: %s", D.desc);

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGCHLD, &sa, NULL);
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	screen_init(&S, &D.fb, ENV.depth, ENV.screenfont);
	memcpy(S.palette, ENV.pal, sizeof(S.palette));
	S.flush_now = flush;
	S.mx = S.w / 2;
	S.my = S.h / 2;
	screen_set_pointer(&S, &ENV.pointer, ENV.ptr_hx, ENV.ptr_hy, 0);
	if (wb_init(&W, &S, &ENV, conf)) {
		disp_close(&D);
		return 1;
	}
	ENV.paldirty = 1;
	screen_damage_all(&S);
	flush();

	if (script) {
		run_script(script);
		disp_close(&D);
		return 0;
	}

	if (input_open())
		fprintf(stderr, "wbde: no keyboard or mouse\n");

	while (!W.quit && !got_quit) {
		struct pollfd fds[64];
		int ni, nc, timeout;
		uint64_t now;

		ni = input_pollfds(fds, 4);
		nc = con_pollfds(fds + ni, 32);
		timeout = wm_busy(&S) ? 20 : 500;
		if (input_poll_ms() > 0 && input_poll_ms() < timeout)
			timeout = input_poll_ms();
		if (poll(fds, (nfds_t)(ni + nc), timeout) < 0 && errno != EINTR)
			break;
		now = now_ms();
		W.now = now;
		input_service(fds, ni, &CB);
		input_tick(&CB);
		con_service(&S, fds + ni, nc);
		if (got_chld) {
			got_chld = 0;
			con_reap(&S);
		}
		wm_tick(&S, now);
		wb_tick(&W, now);
		flush();
		if (got_shot) {
			got_shot = 0;
			shot("/tmp/wbde-shot.ppm");
		}
	}
	input_close();
	disp_close(&D);
	return 0;
}
