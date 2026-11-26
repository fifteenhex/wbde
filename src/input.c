/*
 * The keyboard is the device smolinput scores best as a keyboard, the mouse
 * the best relative pointer with a left button (event0 amiga-keyboard and
 * event1 amiga-mouse on the A4000). The keyboard is grabbed so that what is
 * typed into Workbench does not also land on the console underneath.
 *
 * The keymap is the US one, which is what the Linux amikbd driver's keycodes
 * mean; the Amiga keys are KEY_LEFTMETA / KEY_RIGHTMETA.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include <linux/input.h>
#include <smolinput.h>

#include "input.h"

static struct smolinput_keyboard kbd, mouse;
static int haskbd, hasmouse;

/*
 * The mouse straight from the hardware, for when amimouse cannot be opened:
 * on the A4000 it shares the vertical blank interrupt with amiserial's
 * modem status poll, neither asks for IRQF_SHARED, and with the serial
 * console open the mouse loses ("genirq: Flags mismatch irq 13"). So read
 * what amimouse would read in its interrupt -- the JOY0DAT counters, the
 * left button on CIA-A PRA bit 6 and the right and middle ones on POTINP
 * bits 10 and 8, all active low -- through /dev/mem, polled.
 */
static volatile uint16_t *custom;	/* 0xdff000 */
static volatile uint8_t *ciaa;		/* 0xbfe000 */
static int hwmouse, hw_lastx, hw_lasty, hw_buttons;
static unsigned int qual;
static int accum_dx, accum_dy;

int input_open(void)
{
	const char *k = getenv("WBDE_KEYBOARD"), *m = getenv("WBDE_MOUSE");

	haskbd = !smolinput_open_scored(&kbd, k, smolinput_score, NULL);
	hasmouse = !smolinput_open_scored(&mouse, m, smolinput_score_mouse, NULL);
	if (haskbd) {
		smolinput_grab(&kbd, 1);
		fprintf(stderr, "wbde: keyboard: %s\n", kbd.name);
	}
	if (hasmouse && m && !strcmp(m, "hw")) {
		smolinput_close(&mouse);
		hasmouse = 0;
	}
	if (hasmouse) {
		/* opening it is what claims the interrupt: see that it worked */
		struct pollfd p = { mouse.fd, POLLIN, 0 };
		struct smolinput_event e;

		if (poll(&p, 1, 0) < 0 || (read(mouse.fd, &e, sizeof(e)) < 0 && errno != EAGAIN)) {
			smolinput_close(&mouse);
			hasmouse = 0;
		}
	}
	if (hasmouse)
		fprintf(stderr, "wbde: mouse: %s\n", mouse.name);
	if (!hasmouse || (m && !strcmp(m, "hw")))
		input_hwmouse();
	return haskbd || hasmouse || hwmouse ? 0 : -ENODEV;
}

int input_hwmouse(void)
{
	int fd;
	void *c, *a;
	uint16_t j;

	if (hasmouse) {
		smolinput_close(&mouse);
		hasmouse = 0;
	}
	if (hwmouse)
		return 0;
	fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	c = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0xdff000);
	a = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0xbfe000);
	close(fd);
	if (c == MAP_FAILED || a == MAP_FAILED) {
		fprintf(stderr, "wbde: no mouse: cannot map the custom chips\n");
		return -ENODEV;
	}
	custom = c;
	ciaa = a;
	j = custom[0x00a / 2];
	hw_lastx = j & 0xff;
	hw_lasty = j >> 8;
	hwmouse = 1;
	fprintf(stderr, "wbde: mouse: read from JOY0DAT/CIA-A/POTINP, polled\n");
	return 0;
}

int input_poll_ms(void)
{
	return hwmouse ? 15 : -1;
}

static int wrap(int d)
{
	if (d < -127)
		d += 256;
	if (d > 127)
		d -= 256;
	return d;
}

static int accel(int d);

void input_tick(const struct input_cb *cb)
{
	uint16_t j, pot;
	int nx, ny, dx, dy, b, changed;

	if (!hwmouse)
		return;
	j = custom[0x00a / 2];
	pot = custom[0x016 / 2];
	nx = j & 0xff;
	ny = j >> 8;
	dx = wrap(nx - hw_lastx);
	dy = wrap(ny - hw_lasty);
	hw_lastx = nx;
	hw_lasty = ny;
	if ((dx || dy) && cb->motion)
		cb->motion(cb->arg, accel(dx), accel(dy));
	b = (!(ciaa[1] & 0x40) ? 1 : 0) | (!(pot & 0x0400) ? 2 : 0) | (!(pot & 0x0100) ? 4 : 0);
	changed = b ^ hw_buttons;
	hw_buttons = b;
	if (changed & 1)
		cb->button(cb->arg, 1, b & 1);
	if (changed & 2)
		cb->button(cb->arg, 2, (b >> 1) & 1);
	if (changed & 4)
		cb->button(cb->arg, 3, (b >> 2) & 1);
}

void input_close(void)
{
	if (haskbd) {
		smolinput_grab(&kbd, 0);
		smolinput_close(&kbd);
	}
	if (hasmouse)
		smolinput_close(&mouse);
	haskbd = hasmouse = 0;
}

int input_pollfds(struct pollfd *fds, int max)
{
	int n = 0;

	if (haskbd && n < max) {
		fds[n].fd = kbd.fd;
		fds[n].events = POLLIN;
		n++;
	}
	if (hasmouse && n < max) {
		fds[n].fd = mouse.fd;
		fds[n].events = POLLIN;
		n++;
	}
	return n;
}

unsigned int input_qual(void)
{
	return qual;
}

int input_keychar(int code, unsigned int q)
{
	static const char lower[] =
		"\0\0331234567890-=\b\tqwertyuiop[]\n\0asdfghjkl;'`\0\\zxcvbnm,./\0*\0 ";
	static const char upper[] =
		"\0\033!@#$%^&*()_+\b\tQWERTYUIOP{}\n\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 ";
	int shift = (q & QUAL_SHIFT) != 0;
	int ch;

	if (code >= KEY_KP7 && code <= KEY_KPDOT) {
		static const char kp[] = "789-456+1230.";

		return kp[code - KEY_KP7];
	}
	if (code == KEY_KPSLASH)
		return '/';
	if (code == KEY_KPENTER)
		return '\n';
	if (code < 0 || code >= (int)sizeof(lower) - 1)
		return 0;
	ch = (unsigned char)(shift ? upper[code] : lower[code]);
	if ((q & QUAL_CAPS) && ch >= 'a' && ch <= 'z')
		ch -= 32;
	return ch;
}

static unsigned int qual_for(int code)
{
	switch (code) {
	case KEY_LEFTSHIFT: return QUAL_LSHIFT;
	case KEY_RIGHTSHIFT: return QUAL_RSHIFT;
	case KEY_LEFTCTRL: case KEY_RIGHTCTRL: return QUAL_CTRL;
	case KEY_LEFTALT: return QUAL_LALT;
	case KEY_RIGHTALT: return QUAL_RALT;
	case KEY_LEFTMETA: return QUAL_LAMIGA;
	case KEY_RIGHTMETA: return QUAL_RAMIGA;
	}
	return 0;
}

static void key_event(const struct smolinput_event *e, const struct input_cb *cb)
{
	struct keyev k;
	unsigned int q;

	if (e->code == BTN_LEFT || e->code == BTN_RIGHT || e->code == BTN_MIDDLE) {
		if (e->value != 2 && cb->button)
			cb->button(cb->arg, e->code == BTN_LEFT ? 1 : e->code == BTN_RIGHT ? 2 : 3,
				   e->value != 0);
		return;
	}
	q = qual_for(e->code);
	if (q) {
		if (e->value)
			qual |= q;
		else
			qual &= ~q;
	}
	if (e->code == KEY_CAPSLOCK && e->value == 1)
		qual ^= QUAL_CAPS;
	k.code = e->code;
	k.down = e->value;
	k.qual = qual;
	k.ch = input_keychar(e->code, qual);
	if (cb->key)
		cb->key(cb->arg, &k);
}

/* Amiga mouse acceleration: fast moves count double */
static int accel(int d)
{
	return (d > 3 || d < -3) ? d * 2 : d;
}

static void service(struct smolinput_keyboard *dev, const struct input_cb *cb)
{
	struct smolinput_event e;

	while (smolinput_readevent(dev, &e) > 0) {
		switch (e.type) {
		case EV_KEY:
			key_event(&e, cb);
			break;
		case EV_REL:
			if (e.code == REL_X)
				accum_dx += e.value;
			else if (e.code == REL_Y)
				accum_dy += e.value;
			break;
		case EV_SYN:
			if ((accum_dx || accum_dy) && cb->motion)
				cb->motion(cb->arg, accel(accum_dx), accel(accum_dy));
			accum_dx = accum_dy = 0;
			break;
		}
	}
}

void input_service(const struct pollfd *fds, int n, const struct input_cb *cb)
{
	int i;

	for (i = 0; i < n; i++) {
		if (!(fds[i].revents & POLLIN))
			continue;
		if (haskbd && fds[i].fd == kbd.fd)
			service(&kbd, cb);
		else if (hasmouse && fds[i].fd == mouse.fd)
			service(&mouse, cb);
	}
}
