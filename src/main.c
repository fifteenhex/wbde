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
#include "screens.h"
#include "server.h"
#include "wb.h"
#include "wm.h"

static struct disp D;
static struct screen S;			/* the Workbench screen */
static struct screen *front = &S, *stack = &S;	/* the screen stack, front first */
/* the screen under the pointer: the front one, or the one behind above it */
static struct screen *pscreen = &S;
/* display pixels a mouse count moves: on a big monitor the pointer
   crosses it as fast as it crosses a 640 pixel one */
static float motion_gain = 1;
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
	/* the display back as it was found (the gamma table, the cursor) */
	disp_emergency(&D);
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

static const struct menustrip *strip_for(struct screen *s)
{
	if (s->active && s->active->menus)
		return s->active->menus;
	return s->menus;
}

/* ---- the screen stack ---- */

/* where the front screen sits on the display: centred, or scrolled to the pointer */
static void layout(struct screen *s);

/* the screen behind that shows above the front one, if it starts lower */
static struct screen *back_of(struct screen *s)
{
	return s->top_edge > 0 ? s->next : NULL;
}

static void view_update(void)
{
	struct screen *s = front, *b = back_of(s);

	if (b) {
		/* the panel takes the scale and position of the screen behind */
		layout(b);
		s->sx = b->sx;
		s->sy = b->sy;
		s->ox = b->ox;
		s->oy = b->oy + (int)(s->top_edge * b->sy);
		disp_set_back(&D, b, b->fb, b->w, b->h, b->sx, b->sy, b->ox, b->oy);
	} else {
		layout(s);
		disp_set_back(&D, NULL, NULL, 0, 0, 0, 0, 0, 0);
	}
	disp_set_view(&D, s->fb, s->w, s->h, s->sx, s->sy, s->ox, s->oy);
}

/* where s sits on the display: centred, or scrolled to its pointer */
static void layout(struct screen *s)
{
	int vw = (int)(s->w * s->sx + 0.5f), vh = (int)(s->h * s->sy + 0.5f);

	if (vw <= D.mw)
		s->ox = (D.mw - vw) / 2;
	else {
		int px = s->ox + (int)(s->mx * s->sx);

		if (px < 0)
			s->ox -= px;
		if (px >= D.mw)
			s->ox -= px - D.mw + 1;
		if (s->ox > 0)
			s->ox = 0;
		if (s->ox < D.mw - vw)
			s->ox = D.mw - vw;
	}
	if (vh <= D.mh)
		s->oy = (D.mh - vh) / 2;
	else {
		int py = s->oy + (int)(s->my * s->sy);

		if (py < 0)
			s->oy -= py;
		if (py >= D.mh)
			s->oy -= py - D.mh + 1;
		if (s->oy > 0)
			s->oy = 0;
		if (s->oy < D.mh - vh)
			s->oy = D.mh - vh;
	}
}

/*
 * Workbench's logical size when none was given (GPU display): the native
 * mode divided by a whole scale.  From the monitor's physical size where
 * the connector reports one, for a Workbench pixel of about 0.45 mm (a
 * classic Amiga or RTG monitor's, where topaz 8 reads comfortably);
 * otherwise the largest scale that keeps 480 lines or more.  --scale (or
 * WBDE_SCALE) alone gives native / scale.
 */
#define TARGET_PITCH_MM 0.45f

static void auto_size(float asked)
{
	int k = 1, w, h;
	float pitch = 0;

	if (D.mm_w > 0 && D.mm_h > 0)
		pitch = ((float)D.mm_w / D.mw + (float)D.mm_h / D.mh) / 2;
	if (asked > 0) {
		k = (int)asked;
		env_logf(&ENV, "Workbench scale: %g, asked for", asked);
	} else if (pitch >= 0.1f && pitch <= 1.5f) {
		k = (int)(TARGET_PITCH_MM / pitch + 0.5f);
		env_logf(&ENV, "monitor: %dx%d mm, %dx%d pixels, %.3f mm a pixel: Workbench scale %d "
			 "(%.2f mm a Workbench pixel)", D.mm_w, D.mm_h, D.mw, D.mh, pitch,
			 k < 1 ? 1 : k, pitch * (k < 1 ? 1 : k));
	} else {
		for (k = 1; D.mh / (k + 1) >= 480; k++)
			;
		env_logf(&ENV, "monitor: no usable physical size (%dx%d mm): Workbench scale %d, "
			 "the largest that keeps 480 lines", D.mm_w, D.mm_h, k);
	}
	if (k < 1)
		k = 1;
	w = D.mw / k;
	h = D.mh / k;
	if (w == D.fb.w && h == D.fb.h)
		return;
	bm_free(&D.fb);
	if (bm_alloc(&D.fb, w, h)) {
		fprintf(stderr, "wbde: no memory for a %dx%d Workbench\n", w, h);
		exit(1);
	}
}

/* the largest whole scale at which a w x h screen fits the display */
int screens_fit_scale(int w, int h)
{
	int k = w > 0 && h > 0 ? (D.mw / w < D.mh / h ? D.mw / w : D.mh / h) : 1;

	return k < 1 ? 1 : k;
}

void screens_fit(struct screen *s)
{
	(void)s;
	view_update();
}

static void set_front(struct screen *n)
{
	struct screen *old = front;
	float X, Y;

	pscreen = n;
	if (n == old) {
		view_update();          /* the screen behind it may have changed */
		screen_damage_all(n);
		return;
	}
	if (menu_is_open())
		menu_close(old);
	/* the pointer stays where it is on the display */
	X = old->ox + old->fx * old->sx;
	Y = old->oy + old->fy * old->sy;
	front = n;
	n->fx = (X - n->ox) / n->sx;
	n->fy = (Y - n->oy) / n->sy;
	if (n->fx < 0) n->fx = 0;
	if (n->fy < 0) n->fy = 0;
	if (n->fx > n->w - 1) n->fx = (float)(n->w - 1);
	if (n->fy > n->h - 1) n->fy = (float)(n->h - 1);
	n->mx = (int)n->fx;
	n->my = (int)n->fy;
	view_update();
	disp_palette(&D, n->palette);
	screen_damage_all(n);
}

static void restack(void)
{
	set_front(stack);
}

void screens_add(struct screen *s)
{
	s->next = stack;
	stack = s;
	s->hw_pointer = D.hw_pointer;
	if (!s->ptr)
		screen_set_pointer(s, S.ptr, S.ptr_hx, S.ptr_hy, S.ptr_key);
	restack();
}

void screens_remove(struct screen *s)
{
	struct screen **pp;

	if (s == &S)
		return;
	for (pp = &stack; *pp; pp = &(*pp)->next)
		if (*pp == s) {
			*pp = s->next;
			break;
		}
	s->next = NULL;
	if (pscreen == s)
		pscreen = stack;
	if (front == s) {
		front = stack;		/* without the pointer mapping: s is going */
		front->fx = (float)front->mx;
		front->fy = (float)front->my;
		view_update();
		disp_palette(&D, front->palette);
		screen_damage_all(front);
		if (menu_is_open())
			menu_close(front);
	}
}

void screens_to_front(struct screen *s)
{
	struct screen **pp;

	for (pp = &stack; *pp; pp = &(*pp)->next)
		if (*pp == s) {
			*pp = s->next;
			s->next = stack;
			stack = s;
			break;
		}
	restack();
}

void screens_to_back(struct screen *s)
{
	struct screen **pp, *last;

	if (!stack->next)
		return;
	for (pp = &stack; *pp; pp = &(*pp)->next)
		if (*pp == s) {
			*pp = s->next;
			break;
		}
	for (last = stack; last->next; last = last->next)
		;
	last->next = s;
	s->next = NULL;
	restack();
}

struct screen *screens_front(void)
{
	return front;
}

struct screen *screens_wb(void)
{
	return &S;
}

void screens_palette(struct screen *s)
{
	if (s == front)
		disp_palette(&D, s->palette);
}

void screens_std_pointer(struct screen *s, int busy)
{
	if (busy && ENV.have_busy)
		screen_set_pointer(s, &ENV.busy, ENV.busy_hx, ENV.busy_hy, 0);
	else
		screen_set_pointer(s, &ENV.pointer, ENV.ptr_hx, ENV.ptr_hy, 0);
}

/* the Workbench screen's depth gadget */
static void wb_bar_depth(struct screen *s)
{
	if (front == s)
		screens_to_back(s);
	else
		screens_to_front(s);
}

/* ---- input ---- */


static void pointer_moved(struct screen *s, uint64_t now)
{
	int ox = s->ox, oy = s->oy;
	struct screen *b = back_of(s);
	float top = b ? -(float)s->top_edge : 0;

	if (s->fx < 0) s->fx = 0;
	if (s->fy < top) s->fy = top;
	if (s->fx > s->w - 1) s->fx = (float)(s->w - 1);
	if (s->fy > s->h - 1) s->fy = (float)(s->h - 1);
	if (b && s->fy < 0 && !wm_busy(s) && !menu_is_open()) {
		/* over the screen behind: its pixels, its windows */
		pscreen = b;
		wm_motion(b, (int)s->fx, (int)(s->fy + s->top_edge), now);
		return;
	}
	if (!wm_busy(pscreen) || pscreen == s)
		pscreen = s;
	if (pscreen != s) {             /* a drag that began behind */
		wm_motion(pscreen, (int)s->fx, (int)(s->fy + s->top_edge), now);
		return;
	}
	wm_motion(s, (int)s->fx, (int)(s->fy < 0 ? 0 : s->fy), now);
	view_update();
	if (s->ox != ox || s->oy != oy)
		screen_damage_all(s);		/* it scrolled */
	if (menu_is_open())
		menu_motion(s, s->mx, s->my);
}

/*
 * A move from the mouse, in Workbench pixels across and Workbench rows
 * down (each row D.scale display lines): the same distance on the display
 * on every screen, so a lores screen's pointer is no faster than
 * Workbench's.
 */
static void cb_motion(void *arg, int dx, int dy)
{
	struct screen *s = front;

	(void)arg;
	s->fx += dx * motion_gain / s->sx;
	s->fy += dy * D.scale * motion_gain / s->sy;
	pointer_moved(s, now_ms());
}

/* the control FIFO's coordinates are the front screen's own */
static void pointer_to(int x, int y)
{
	struct screen *s = front;

	s->fx = (float)x;
	s->fy = (float)y;
	pointer_moved(s, now_ms());
}

static unsigned int buttons;	/* IEQUALIFIER_LEFTBUTTON etc. held now */

unsigned int screens_buttons(void)
{
	return buttons;
}

static void cb_button(void *arg, int button, int down)
{
	struct screen *s = pscreen;
	unsigned int bit = button == 1 ? 0x4000 : button == 2 ? 0x2000 : 0x1000;

	buttons = down ? buttons | bit : buttons & ~bit;
	uint64_t now = now_ms();

	W.now = now;
	if (button == 2) {
		if (down && !menu_is_open() && !wm_busy(s)) {
			struct win *w = win_at(s, s->mx, s->my);

			if ((w && (w->flags & WF_RMBTRAP)) || (!w && s->root_rmbtrap)) {
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
	(void)arg;
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
	struct screen *s = pscreen;

	(void)arg;
	if (kbd_mouse(s, k))
		return;

	if (k->down == 1 && (k->qual & QUAL_RAMIGA) && k->ch && !menu_is_open()) {
		if (menu_shortcut(strip_for(s), k->ch))
			return;
	}
	if (k->down && (k->qual & QUAL_LAMIGA) && (k->ch == 'm' || k->ch == 'n')) {
		/* Intuition's: Left Amiga + M sends the front screen to the back,
		   + N brings Workbench to the front */
		if (k->down == 1) {
			if (k->ch == 'm')
				screens_to_back(front);
			else
				screens_to_front(&S);
		}
		return;
	}
	wm_key(s, k);
}

static const struct input_cb CB = { cb_motion, cb_button, cb_key, &S };

static void flush(void)
{
	static int timing = -1;

	if (timing < 0)
		timing = getenv("WBDE_TIMING") != NULL;
	if (ENV.paldirty) {
		ENV.paldirty = 0;
		memcpy(S.palette, ENV.pal, sizeof(S.palette));
		screens_palette(&S);
	}
	if (D.composes) {
		/* the display draws the damage itself (and moves its pointer) */
		D.timing[0] = 0;
		D.ops->compose(&D, front);
		if (timing && D.timing[0])
			fprintf(stderr, "wbde: %s\n", D.timing);
		return;
	}
	if (D.view.back && D.view.back->ndamage)
		screen_flush(D.view.back, NULL, NULL);  /* for screenshots; not shown */
	if (front->ndamage) {
		struct timespec t0, t1, t2;
		long area = 0;
		int i;

		for (i = 0; i < front->ndamage; i++)
			area += (long)front->damage[i].w * front->damage[i].h;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		screen_flush(front, disp_touch, &D);
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
	if (D.ops->shot ? D.ops->shot(&D, front, path) : disp_screenshot(&D, front->palette, path))
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
		pointer_to(a, b);
	} else if (sscanf(line, "down %d", &a) == 1) {
		cb_button(front, a, 1);
	} else if (sscanf(line, "up %d", &a) == 1) {
		cb_button(front, a, 0);
	} else if (sscanf(line, "click %d %d", &a, &b) == 2) {
		pointer_to(a, b);
		cb_button(front, 1, 1);
		cb_button(front, 1, 0);
	} else if (sscanf(line, "dclick %d %d", &a, &b) == 2) {
		pointer_to(a, b);
		cb_button(front, 1, 1);
		cb_button(front, 1, 0);
		advance(100);
		cb_button(front, 1, 1);
		cb_button(front, 1, 0);
	} else if (sscanf(line, "drag %d %d %d %d", &a, &b, &c, &d) == 4) {
		/* press at one place, move there in steps, let go */
		int i;

		pointer_to(a, b);
		cb_button(front, 1, 1);
		for (i = 1; i <= 8; i++)
			pointer_to(a + (c - a) * i / 8, b + (d - b) * i / 8);
		cb_button(front, 1, 0);
	} else if (sscanf(line, "menu %d %d %d %d", &a, &b, &c, &d) == 4) {
		pointer_to(a, b);
		cb_button(front, 2, 1);
		pointer_to(c, d);
		if (!strstr(line, "hold"))
			cb_button(front, 2, 0);
	} else if (sscanf(line, "key %d %d", &a, &b) >= 1) {
		struct keyev k = { (uint16_t)a, 1, 0, 0 };

		if (sscanf(line, "key %d %d", &a, &b) == 2)
			k.qual = (unsigned int)b;
		k.ch = input_keychar(a, k.qual);
		cb_key(front, &k);
		k.down = 0;
		cb_key(front, &k);
	} else if (sscanf(line, "type %399[^\n]", arg) == 1) {
		char *p;

		for (p = arg; *p; p++) {
			struct keyev k = { 0, 1, (unsigned char)*p, 0 };
			int c, q;

			if (*p == '|') {
				k.code = 28;	/* Return */
				k.ch = '\n';
			}
			/* the key that makes it, so clients get a RAWKEY as from a keyboard */
			for (c = 1; c < 58 && !k.code; c++)
				for (q = 0; q < 2; q++)
					if (input_keychar(c, q ? QUAL_LSHIFT : 0) == (unsigned char)*p) {
						k.code = (uint16_t)c;
						k.qual = q ? QUAL_LSHIFT : 0;
						break;
					}
			cb_key(front, &k);
			if (k.code) {
				k.down = 0;
				cb_key(front, &k);
			}
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
			wm_tick(front, vclock);
		}
	} else if (sscanf(line, "shot %399s", arg) == 1) {
		shot(arg);
		return;
	} else if (!strncmp(line, "quit", 4)) {
		W.quit = 1;
	} else if (!strncmp(line, "redraw", 6)) {
		screen_damage_all(front);
	} else if (!strncmp(line, "screen back", 11)) {
		screens_to_back(front);
	} else if (!strncmp(line, "screen wb", 9)) {
		screens_to_front(&S);
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
		"usage: wbde [--mode WxH] [--size WxH] [--scale N] [--sys DIR] [--conf FILE]\n"
		"            [--rom FILE] [--script FILE] [--socket PATH] [--pattern ILBM]\n"
		"            [--drawer-pattern ILBM] [--display gpu|soft|file]\n"
		"  --mode: the display mode (GPU default: the monitor's own)\n"
		"  --size: Workbench's logical size (GPU default: the mode, 1:1)\n"
		"  --scale: Workbench's scale (GPU default: the largest whole number that fits)\n");
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
		{ "display", required_argument, NULL, 'D' },
		{ "scale", required_argument, NULL, 'x' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	/* the display's own mode and, on the GPU display, Workbench as big as
	   it (1:1); the software and memory displays stay 640x480 with a
	   640x240 Workbench, rows doubled */
	const char *mode = getenv("WBDE_MODE"), *size = getenv("WBDE_SIZE"), *sys = "/mnt/sys";
	const char *conf = NULL, *script = NULL;
	float scale = getenv("WBDE_SCALE") ? (float)atof(getenv("WBDE_SCALE")) : 0;
	const char *sock = NULL, *pattern = NULL, *dpattern = NULL, *display = NULL;
	struct sigaction sa;
	int o;

	while ((o = getopt_long(argc, argv, "m:z:s:c:r:S:k:p:P:D:x:h", opts, NULL)) != -1) {
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
		case 'D': display = optarg; break;
		case 'x': scale = (float)atof(optarg); break;
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
	if (disp_open(&D, display, mode, size))
		return 1;
	env_logf(&ENV, "display: %s", D.desc);
	if (D.composes && !size)
		auto_size(scale);

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
	S.hw_pointer = D.hw_pointer;
	if (D.composes || D.uniform) {
		/* a uniform scale, square pixels, centred: the largest whole
		   number that fits unless told */
		S.sx = S.sy = scale > 0 ? scale : (float)screens_fit_scale(S.w, S.h);
		motion_gain = D.mw > 640 ? D.mw / 640.0f : 1;
	} else
		S.sy = (float)D.scale;
	env_logf(&ENV, "Workbench: %dx%d at %gx on %dx%d", S.w, S.h, S.sx, D.mw, D.mh);
	S.bar_depth = wb_bar_depth;
	memcpy(S.palette, ENV.pal, sizeof(S.palette));
	S.flush_now = flush;
	S.mx = S.w / 2;
	S.my = S.h / 2;
	S.fx = (float)S.mx;
	S.fy = (float)S.my;
	view_update();
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
		timeout = wm_busy(front) ? 20 : 500;
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
		wm_tick(front, now);
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
