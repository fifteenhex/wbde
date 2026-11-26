/* The client interface: see doc/client-protocol.md and include/wbde_proto.h */
#ifndef WBDE_SERVER_H
#define WBDE_SERVER_H

#include <poll.h>
#include <stdint.h>

#include "wm.h"

typedef int (*server_penfn)(void *arg, uint8_t r, uint8_t g, uint8_t b);

/* path NULL: $WBDE_SOCKET, else WBP_SOCKET_DEFAULT */
int server_open(struct screen *s, const char *path, server_penfn pen, void *penarg);
int server_pollfds(struct pollfd *fds, int max);
void server_service(struct screen *s, const struct pollfd *fds, int n);
void server_close(void);
/* after every flush: presents (flips) whose buffer has been composed */
void server_flushed(void);
/* Linux KEY_* to an Amiga raw key code, 0xff if none */
int amiga_rawkey(int linuxkey);

#endif
