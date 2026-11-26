/*
 * Console windows: a program on a pty, drawn in topaz. What AmigaOS calls a
 * CON: window, opened on the first output (CON:...AUTO) or straight away for
 * a Shell. VT100 enough for busybox ash and ls --color.
 */
#ifndef WBDE_CON_H
#define WBDE_CON_H

#include <poll.h>
#include <stdint.h>

#include "wm.h"

struct con;

/*
 * argv NULL: an interactive /bin/sh. auto_open: no window until the program
 * says something. err (if not NULL) gets the exec errno when exec failed.
 */
struct con *con_spawn(struct screen *s, const struct rect *r, const char *title,
		      const char *dir, const char *const argv[], int auto_open, int *err);
/*
 * A client's console window (WBP_OPEN_CON) on its fd, opened at once; owner
 * and id are the client's and the id it was told.  con_destroy closes it
 * (and the fd), con_drop_owner every one a client has.
 */
struct con *con_attach(struct screen *s, const struct rect *r, const char *title, int fd,
		       void *owner, uint32_t id);
struct con *con_find(const void *owner, uint32_t id);
struct win *con_win(struct con *c);
void con_destroy(struct con *c);
void con_drop_owner(const void *owner);
int con_pollfds(struct pollfd *fds, int max);
void con_service(struct screen *s, const struct pollfd *fds, int n);
void con_reap(struct screen *s);	/* after SIGCHLD */

#endif
