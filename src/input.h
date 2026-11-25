/* The Amiga keyboard and mouse, through evdev and smolinput */
#ifndef WBDE_INPUT_H
#define WBDE_INPUT_H

#include <poll.h>

#include "wm.h"

struct input_cb {
	void (*motion)(void *arg, int dx, int dy);
	void (*button)(void *arg, int button, int down);
	void (*key)(void *arg, const struct keyev *k);
	void *arg;
};

int input_open(void);
int input_pollfds(struct pollfd *fds, int max);
void input_service(const struct pollfd *fds, int n, const struct input_cb *cb);
unsigned int input_qual(void);
/* the keymap, for host scripts too: Linux KEY_* to a character */
int input_keychar(int code, unsigned int qual);
void input_close(void);
/* poll the mouse hardware directly (amimouse unavailable) */
int input_hwmouse(void);
/* how often input_tick wants calling, -1 never */
int input_poll_ms(void);
void input_tick(const struct input_cb *cb);

#endif
