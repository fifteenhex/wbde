/*
 * wbde: the screen, the event loop, and the glue between input, the window
 * core, Workbench, console windows and clients.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <sys/stat.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "con.h"
#include "disp.h"
#include "font.h"
#include "input.h"
#include "menu.h"
#include "prefs.h"
#include "server.h"
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

/* say where it died: the PC goes through addr2line on the unstripped build */
static void on_crash(int sig, siginfo_t *si, void *ctx)
{
	char buf[128];
	unsigned long pc = 0;
	int n;

#if defined(__m68k__)
	pc = (unsigned long)((ucontext_t *)ctx)->uc_mcontext.gregs[16];
#elif defined(__x86_64__)
	pc = (unsigned long)((ucontext_t *)ctx)->uc_mcontext.gregs[REG_RIP];
#endif
	n = snprintf(buf, sizeof(buf), "wbde: signal %d at pc %08lx, address %p\n", sig, pc, si->si_addr);
	if (write(2, buf, (size_t)n) < 0)
		_exit(128 + sig);
	signal(sig, SIG_DFL);
	raise(sig);
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

/*
 * Intuition's mouse on the keyboard: Left Amiga + the cursor keys moves the
 * pointer (faster with Shift), Left Amiga + Left Alt is the left button and
 * Left Amiga + Right Alt the right one. For when the mouse cannot be
 * used, as when amimouse cannot get its interrupt.
 */
static int kbd_mouse(struct screen *s, const struct keyev *k)
{
	static int held;	/* buttons pressed from the keyboard */
	int step = (k->qual & QUAL_SHIFT) ? 32 : 4;

	if (k->code == 125 && !k->down && held) {	/* Left Amiga let go */
		if (held & 1)
			cb_button(s, 1, 0);
		if (held & 2)
			cb_button(s, 2, 0);
		held = 0;
		return 1;
	}
	if (!(k->qual & QUAL_LAMIGA))
		return 0;
	switch (k->code) {
	case 103: if (k->down) cb_motion(s, 0, -step); return 1;	/* up */
	case 108: if (k->down) cb_motion(s, 0, step); return 1;		/* down */
	case 105: if (k->down) cb_motion(s, -step, 0); return 1;	/* left */
	case 106: if (k->down) cb_motion(s, step, 0); return 1;		/* right */
	case 56:						/* Left Alt */
	case 100:						/* Right Alt */
		if (k->down == 2)
			return 1;
		{
			int b = k->code == 56 ? 1 : 2;

			if (k->down && !(held & b)) {
				held |= b;
				cb_button(s, b, 1);
			} else if (!k->down && (held & b)) {
				held &= ~b;
				cb_button(s, b, 0);
			}
		}
		return 1;
	}
	return 0;
}

static void cb_key(void *arg, const struct keyev *k)
{
	struct screen *s = arg;

	if (kbd_mouse(s, k))
		return;

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
		static int timing = -1;
		struct timespec t0, t1, t2;
		long area = 0;
		int i;

		if (timing < 0)
			timing = getenv("WBDE_TIMING") != NULL;
		for (i = 0; i < S.ndamage; i++)
			area += (long)S.damage[i].w * S.damage[i].h;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		screen_flush(&S, disp_touch, &D);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		disp_present(&D);
		clock_gettime(CLOCK_MONOTONIC, &t2);
		if (timing)
			fprintf(stderr, "wbde: flush %ld px: composite %ld us, present %ld us\n", area,
				(long)((t1.tv_sec - t0.tv_sec) * 1000000 + (t1.tv_nsec - t0.tv_nsec) / 1000),
				(long)((t2.tv_sec - t1.tv_sec) * 1000000 + (t2.tv_nsec - t1.tv_nsec) / 1000));
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
 * Commands drive wbde without a mouse: from a script file (--script, for
 * testing away from the machine, with its own clock) or written to the
 * control FIFO (/tmp/wbde-ctl or $WBDE_CTL, for poking the real thing remotely):
 *   move X Y | down B | up B | click X Y | dclick X Y | menu X Y X2 Y2 [hold] |
 *   key CODE [QUAL] | type TEXT | wait MS | shot FILE | quit
 * Coordinates are the logical screen's.
 */
static void advance(uint64_t ms)
{
	if (scripted)
		vclock += ms;
	W.now = now_ms();
}

static void command(const char *line)
{
	int a, b, c, d;
	char arg[400];

	advance(20);
	wb_tick(&W, W.now);
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
		advance(100);
		cb_button(&S, 1, 1);
		cb_button(&S, 1, 0);
	} else if (sscanf(line, "menu %d %d %d %d", &a, &b, &c, &d) == 4) {
		cb_motion(&S, a - S.mx, b - S.my);
		cb_button(&S, 2, 1);
		cb_motion(&S, c - S.mx, d - S.my);
		if (!strstr(line, "hold"))
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

			if (*p == '|')
				k.code = 28;	/* Return */
			cb_key(&S, &k);
		}
	} else if (sscanf(line, "wait %d", &a) == 1 && scripted) {
		uint64_t until = vclock + (uint64_t)a;
		struct pollfd fds[16];
		int n;

		while (vclock < until) {
			int nc;

			nc = con_pollfds(fds, 8);
			n = nc + server_pollfds(fds + nc, 8);
			poll(fds, (nfds_t)n, 10);
			con_service(&S, fds, nc);
			server_service(&S, fds + nc, n - nc);
			flush();
			if (got_chld) {
				got_chld = 0;
				con_reap(&S);
			}
			vclock += 10;
			wm_tick(&S, vclock);
		}
	} else if (sscanf(line, "shot %399s", arg) == 1) {
		shot(arg);
		return;
	} else if (!strncmp(line, "quit", 4)) {
		W.quit = 1;
	}
	flush();
}

static int run_script(const char *path)
{
	char line[512];
	FILE *f = fopen(path, "r");

	if (!f) {
		perror(path);
		return -1;
	}
	while (fgets(line, sizeof(line), f) && !W.quit)
		command(line);
	fclose(f);
	return 0;
}

#define CTL_PATH "/tmp/wbde-ctl"
static int ctl_fd = -1, ctl_keep = -1;
static char ctl_buf[1024];
static size_t ctl_len;

static void ctl_open(void)
{
	const char *path = getenv("WBDE_CTL") ? getenv("WBDE_CTL") : CTL_PATH;

	unlink(path);
	if (mkfifo(path, 0600))
		return;
	ctl_fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	/* a writer of our own, so the FIFO never reads as closed */
	ctl_keep = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
}

static void ctl_service(void)
{
	ssize_t n;
	char *nl;

	n = read(ctl_fd, ctl_buf + ctl_len, sizeof(ctl_buf) - 1 - ctl_len);
	if (n <= 0)
		return;
	ctl_len += (size_t)n;
	ctl_buf[ctl_len] = 0;
	while ((nl = strchr(ctl_buf, '\n'))) {
		*nl = 0;
		command(ctl_buf);
		ctl_len -= (size_t)(nl + 1 - ctl_buf);
		memmove(ctl_buf, nl + 1, ctl_len + 1);
	}
	if (ctl_len == sizeof(ctl_buf) - 1)
		ctl_len = 0;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: wbde [--mode WxH] [--size WxH] [--sys DIR] [--conf FILE] [--rom FILE]\n"
		"            [--script FILE] [--socket PATH] [--pattern ILBM] [--drawer-pattern ILBM]\n");
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
		{ "socket", required_argument, NULL, 'k' },
		{ "pattern", required_argument, NULL, 'p' },
		{ "drawer-pattern", required_argument, NULL, 'P' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	const char *mode = "640x480", *size = "640x240", *sys = "/mnt/sys", *conf = NULL, *script = NULL;
	const char *sock = NULL, *pattern = NULL, *dpattern = NULL;
	struct sigaction sa;
	int o;

	while ((o = getopt_long(argc, argv, "m:z:s:c:r:S:k:p:P:h", opts, NULL)) != -1) {
		switch (o) {
		case 'm': mode = optarg; break;
		case 'z': size = optarg; break;
		case 's': sys = optarg; break;
		case 'c': conf = optarg; break;
		case 'r': setenv("WBDE_ROM", optarg, 1); break;
		case 'S': script = optarg; break;
		case 'k': sock = optarg; break;
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
	if (!getenv("WBDE_NOCRASHHANDLER")) {
		struct sigaction cs;

		memset(&cs, 0, sizeof(cs));
		cs.sa_sigaction = on_crash;
		cs.sa_flags = SA_SIGINFO | SA_RESETHAND;
		sigaction(SIGSEGV, &cs, NULL);
		sigaction(SIGBUS, &cs, NULL);
		sigaction(SIGILL, &cs, NULL);
		sigaction(SIGFPE, &cs, NULL);
		sigaction(SIGABRT, &cs, NULL);
	}

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
	server_open(&S, sock, env_pen, &ENV);
	ENV.paldirty = 1;
	screen_damage_all(&S);
	flush();

	if (script) {
		run_script(script);
		server_close();
		disp_close(&D);
		return 0;
	}

	if (input_open())
		fprintf(stderr, "wbde: no keyboard or mouse\n");
	ctl_open();

	while (!W.quit && !got_quit) {
		struct pollfd fds[64];
		int ni, nc, ns, timeout;
		uint64_t now;

		ni = input_pollfds(fds, 4);
		nc = con_pollfds(fds + ni, 32);
		ns = server_pollfds(fds + ni + nc, 63 - ni - nc);
		if (ctl_fd >= 0) {
			fds[ni + nc + ns].fd = ctl_fd;
			fds[ni + nc + ns].events = POLLIN;
			fds[ni + nc + ns].revents = 0;
		}
		timeout = wm_busy(&S) ? 20 : 500;
		if (input_poll_ms() > 0 && input_poll_ms() < timeout)
			timeout = input_poll_ms();
		if (poll(fds, (nfds_t)(ni + nc + ns + (ctl_fd >= 0)), timeout) < 0 && errno != EINTR)
			break;
		now = now_ms();
		W.now = now;
		input_service(fds, ni, &CB);
		input_tick(&CB);
		con_service(&S, fds + ni, nc);
		server_service(&S, fds + ni + nc, ns);
		if (ctl_fd >= 0 && (fds[ni + nc + ns].revents & POLLIN))
			ctl_service();
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
	server_close();
	disp_close(&D);
	if (ctl_fd >= 0)
		unlink(CTL_PATH);
	return 0;
}
