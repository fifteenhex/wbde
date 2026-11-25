/*
 * wbclient.h: a small client for the wbde protocol, header only. Enough to
 * open windows, draw into them, get IDCMP events and give them menus.
 *
 *   struct wbc c;  wbc_connect(&c, NULL, "myprog");
 *   struct wbc_window w;  wbc_open(&c, &w, 40, 40, 300, 120, flags, idcmp, "Title");
 *   ... draw into w.pixels (w.stride bytes a row, pens) ...
 *   wbc_damage(&c, &w, 0, 0, w.width, w.height);
 *   struct wbp_event e;  while (wbc_wait(&c, &e, -1) > 0) ...
 *
 * With a GPU display, two more ways to fill a window (see client-protocol.md):
 * WBP_WF_VRAM in wbc_open's flags puts w.pixels in video memory (w.vram is
 * then set: write, never read), and wbc_attach() shows a GPU buffer the
 * client owns (a dma-buf) as the window's content.
 */
#ifndef WBCLIENT_H
#define WBCLIENT_H

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "wbde_proto.h"

#define WBC_MAXWIN 16

struct wbc_window {
	uint32_t id;
	int left, top, width, height, stride;
	uint8_t *pixels;
	size_t size;
	int changed;		/* a new buffer arrived (resize): redraw */
	int vram;		/* pixels are in video memory: writes only */
};

struct wbc {
	int fd;
	uint32_t serial;
	struct wbp_screen screen;
	struct wbc_window *wins[WBC_MAXWIN];
	struct wbp_font font;
	struct wbp_glyph glyphs[257];
	uint8_t *chardata;
	uint8_t buf[70000];
	size_t len;
	struct wbp_event pending[64];
	uint32_t pending_win[64];
	int npending;
};

static inline int wbc__send_fd(struct wbc *c, uint32_t type, uint32_t win, const void *p,
				uint32_t len, int fd)
{
	struct wbp_hdr h = { type, len, win, ++c->serial };
	struct iovec iov[2] = { { &h, sizeof(h) }, { (void *)p, len } };
	char cbuf[CMSG_SPACE(sizeof(int))];
	struct msghdr m = { 0 };

	m.msg_iov = iov;
	m.msg_iovlen = len ? 2 : 1;
	if (fd >= 0) {
		struct cmsghdr *cm;

		memset(cbuf, 0, sizeof(cbuf));
		m.msg_control = cbuf;
		m.msg_controllen = sizeof(cbuf);
		cm = CMSG_FIRSTHDR(&m);
		cm->cmsg_level = SOL_SOCKET;
		cm->cmsg_type = SCM_RIGHTS;
		cm->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(cm), &fd, sizeof(int));
	}
	return sendmsg(c->fd, &m, MSG_NOSIGNAL) == (ssize_t)(sizeof(h) + len) ? 0 : -1;
}

static inline int wbc__send(struct wbc *c, uint32_t type, uint32_t win, const void *p, uint32_t len)
{
	return wbc__send_fd(c, type, win, p, len, -1);
}

/* read one message (blocking), with an fd if one came along */
static inline int wbc__recv(struct wbc *c, struct wbp_hdr *h, void **payload, int *fd)
{
	*fd = -1;
	for (;;) {
		if (c->len >= sizeof(*h)) {
			memcpy(h, c->buf, sizeof(*h));
			if (h->len > sizeof(c->buf) - sizeof(*h))
				return -1;
			if (c->len >= sizeof(*h) + h->len) {
				*payload = c->buf + sizeof(*h);
				return 1;
			}
		}
		{
			char cbuf[CMSG_SPACE(sizeof(int))];
			struct iovec iov = { c->buf + c->len, sizeof(c->buf) - c->len };
			struct msghdr m = { 0 };
			struct cmsghdr *cm;
			ssize_t n;

			m.msg_iov = &iov;
			m.msg_iovlen = 1;
			m.msg_control = cbuf;
			m.msg_controllen = sizeof(cbuf);
			n = recvmsg(c->fd, &m, MSG_CMSG_CLOEXEC);
			if (n <= 0)
				return -1;
			for (cm = CMSG_FIRSTHDR(&m); cm; cm = CMSG_NXTHDR(&m, cm))
				if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS)
					memcpy(fd, CMSG_DATA(cm), sizeof(int));
			c->len += (size_t)n;
		}
	}
}

static inline void wbc__consume(struct wbc *c, const struct wbp_hdr *h)
{
	size_t n = sizeof(*h) + h->len;

	memmove(c->buf, c->buf + n, c->len - n);
	c->len -= n;
}

static inline struct wbc_window *wbc__find(struct wbc *c, uint32_t id)
{
	int i;

	for (i = 0; i < WBC_MAXWIN; i++)
		if (c->wins[i] && c->wins[i]->id == id)
			return c->wins[i];
	return NULL;
}

static inline void wbc__window(struct wbc *c, struct wbc_window *w, const struct wbp_window *m, int fd)
{
	(void)c;
	if (w->pixels)
		munmap(w->pixels, w->size);
	w->left = m->left;
	w->top = m->top;
	w->width = m->width;
	w->height = m->height;
	w->stride = m->stride;
	w->size = m->size;
	w->vram = (m->flags & WBP_WIN_VRAM) != 0;
	w->pixels = mmap(NULL, w->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (w->pixels == MAP_FAILED)
		w->pixels = NULL;
	w->changed = 1;
	close(fd);
}

/* handle whatever is not the reply being waited for; events are queued */
static inline void wbc__other(struct wbc *c, const struct wbp_hdr *h, void *p, int fd)
{
	struct wbc_window *w;

	switch (h->type) {
	case WBP_EVENT:
		if (c->npending < 64) {
			memcpy(&c->pending[c->npending], p, sizeof(struct wbp_event));
			c->pending_win[c->npending++] = h->win;
		}
		break;
	case WBP_WINDOW:
		w = wbc__find(c, h->win);
		if (w && fd >= 0)
			wbc__window(c, w, p, fd);
		else if (fd >= 0)
			close(fd);
		break;
	case WBP_SCREEN:
		memcpy(&c->screen, p, sizeof(c->screen));
		break;
	default:
		if (fd >= 0)
			close(fd);
	}
}

/* send a request and wait for the reply of `type` with the same serial */
static inline int wbc__call(struct wbc *c, uint32_t type, uint32_t win, const void *p, uint32_t len,
			    uint32_t reply, void *out, size_t outlen, int *outfd, uint32_t *outwin)
{
	struct wbp_hdr h;
	void *pl;
	int fd;

	if (wbc__send(c, type, win, p, len))
		return -1;
	for (;;) {
		if (wbc__recv(c, &h, &pl, &fd) <= 0)
			return -1;
		if (h.serial == c->serial && (h.type == reply || h.type == WBP_ERROR)) {
			int ret = h.type == WBP_ERROR ? *(int32_t *)pl : 0;

			if (!ret && out)
				memcpy(out, pl, h.len < outlen ? h.len : outlen);
			if (outwin)
				*outwin = h.win;
			if (outfd)
				*outfd = fd;
			else if (fd >= 0)
				close(fd);
			wbc__consume(c, &h);
			return ret;
		}
		wbc__other(c, &h, pl, fd);
		wbc__consume(c, &h);
	}
}

static inline int wbc_connect(struct wbc *c, const char *path, const char *name)
{
	struct sockaddr_un a = { 0 };
	struct wbp_hello hello = { WBP_VERSION, { 0 } };

	memset(c, 0, sizeof(*c));
	if (!path)
		path = getenv("WBDE_SOCKET");
	if (!path)
		path = WBP_SOCKET_DEFAULT;
	c->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (c->fd < 0)
		return -1;
	a.sun_family = AF_UNIX;
	strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
	if (connect(c->fd, (struct sockaddr *)&a, sizeof(a))) {
		close(c->fd);
		return -1;
	}
	strncpy(hello.name, name ? name : "client", sizeof(hello.name) - 1);
	return wbc__call(c, WBP_HELLO, 0, &hello, sizeof(hello), WBP_SCREEN, &c->screen,
			 sizeof(c->screen), NULL, NULL);
}

static inline int wbc_open(struct wbc *c, struct wbc_window *w, int left, int top, int width,
			   int height, uint32_t flags, uint32_t idcmp, const char *title)
{
	struct wbp_open o;
	struct wbp_window m;
	int fd = -1, i, ret;

	memset(&o, 0, sizeof(o));
	memset(w, 0, sizeof(*w));
	o.left = (int16_t)left;
	o.top = (int16_t)top;
	o.width = (uint16_t)width;
	o.height = (uint16_t)height;
	o.flags = flags;
	o.idcmp = idcmp;
	strncpy(o.title, title, sizeof(o.title) - 1);
	ret = wbc__call(c, WBP_OPEN, 0, &o, sizeof(o), WBP_WINDOW, &m, sizeof(m), &fd, &w->id);
	if (ret || fd < 0)
		return ret ? ret : -1;
	for (i = 0; i < WBC_MAXWIN && c->wins[i]; i++)
		;
	if (i == WBC_MAXWIN) {
		close(fd);
		return -1;
	}
	c->wins[i] = w;
	wbc__window(c, w, &m, fd);
	return 0;
}

/*
 * Show a GPU buffer (a dma-buf: `fd`, which the caller keeps) as w's content,
 * width x height pixels of WBP_FMT_*, `pitch` bytes a row from `offset`,
 * scaled to the inner area.  wbc_damage then says it changed.  -ENOTSUP
 * without a GPU display.  fd < 0 goes back to the window's own buffer.
 */
static inline int wbc_attach(struct wbc *c, struct wbc_window *w, int fd, uint32_t format,
			     int width, int height, uint32_t pitch, uint32_t offset)
{
	struct wbp_attach a = { format, (uint16_t)width, (uint16_t)height, pitch, offset };
	struct wbp_hdr h;
	void *pl;
	int rfd;

	if (wbc__send_fd(c, WBP_ATTACH, w->id, &a, sizeof(a), fd))
		return -1;
	for (;;) {
		if (wbc__recv(c, &h, &pl, &rfd) <= 0)
			return -1;
		if (h.serial == c->serial && (h.type == WBP_ATTACHED || h.type == WBP_ERROR)) {
			int ret = h.type == WBP_ERROR ? *(int32_t *)pl : 0;

			wbc__consume(c, &h);
			return ret;
		}
		wbc__other(c, &h, pl, rfd);
		wbc__consume(c, &h);
	}
}

static inline int wbc_damage(struct wbc *c, struct wbc_window *w, int x, int y, int width, int height)
{
	struct wbp_rect r = { (int16_t)x, (int16_t)y, (uint16_t)width, (uint16_t)height };

	return wbc__send(c, WBP_DAMAGE, w->id, &r, sizeof(r));
}

static inline int wbc_close(struct wbc *c, struct wbc_window *w)
{
	int i;

	for (i = 0; i < WBC_MAXWIN; i++)
		if (c->wins[i] == w)
			c->wins[i] = NULL;
	if (w->pixels)
		munmap(w->pixels, w->size);
	w->pixels = NULL;
	return wbc__send(c, WBP_CLOSE, w->id, NULL, 0);
}

static inline int wbc_set_menus(struct wbc *c, struct wbc_window *w, const struct wbp_menuitem *m, int n)
{
	return wbc__send(c, WBP_SET_MENUS, w->id, m, (uint32_t)(n * (int)sizeof(*m)));
}

static inline int wbc_set_title(struct wbc *c, struct wbc_window *w, const char *t)
{
	return wbc__send(c, WBP_SET_TITLE, w->id, t, (uint32_t)strlen(t));
}

/* the screen font, to draw text in the Workbench's own topaz */
static inline int wbc_get_font(struct wbc *c)
{
	struct wbp_hdr h;
	void *pl;
	int fd;

	if (wbc__send(c, WBP_GET_FONT, 0, NULL, 0))
		return -1;
	for (;;) {
		if (wbc__recv(c, &h, &pl, &fd) <= 0)
			return -1;
		if (h.type == WBP_FONT && h.serial == c->serial) {
			size_t data;

			memcpy(&c->font, pl, sizeof(c->font));
			memcpy(c->glyphs, (uint8_t *)pl + sizeof(c->font), sizeof(c->glyphs));
			data = (size_t)c->font.modulo * c->font.ysize;
			c->chardata = malloc(data);
			if (c->chardata)
				memcpy(c->chardata, (uint8_t *)pl + sizeof(c->font) + sizeof(c->glyphs), data);
			wbc__consume(c, &h);
			return c->chardata ? 0 : -1;
		}
		wbc__other(c, &h, pl, fd);
		wbc__consume(c, &h);
	}
}

static inline int wbc_text(struct wbc *c, struct wbc_window *w, int x, int y, const char *s, int pen)
{
	int start = x;

	if (!c->chardata)
		return 0;
	for (; *s; s++) {
		unsigned char ch = (unsigned char)*s;
		const struct wbp_glyph *g = (ch < c->font.lochar || ch > c->font.hichar) ?
					    &c->glyphs[256] : &c->glyphs[ch];
		int row, col;

		for (row = 0; row < c->font.ysize; row++)
			for (col = 0; col < g->width; col++) {
				int bit = g->offset + col, px = x + g->kern + col, py = y + row;

				if (!(c->chardata[row * c->font.modulo + bit / 8] & (0x80 >> (bit & 7))))
					continue;
				if (px >= 0 && py >= 0 && px < w->width && py < w->height)
					w->pixels[py * w->stride + px] = (uint8_t)pen;
			}
		x += g->space;
	}
	return x - start;
}

/*
 * The next event, for any window (*win gets its id). timeout in ms, -1 to
 * wait. 1 event, 0 timeout, -1 disconnected.
 */
static inline int wbc_wait(struct wbc *c, struct wbp_event *e, uint32_t *win, int timeout)
{
	for (;;) {
		struct wbp_hdr h;
		struct pollfd p = { c->fd, POLLIN, 0 };
		void *pl;
		int fd;

		if (c->npending) {
			*e = c->pending[0];
			if (win)
				*win = c->pending_win[0];
			memmove(c->pending, c->pending + 1, sizeof(c->pending[0]) * (size_t)--c->npending);
			memmove(c->pending_win, c->pending_win + 1, sizeof(c->pending_win[0]) * (size_t)c->npending);
			return 1;
		}
		if (c->len < sizeof(h) && poll(&p, 1, timeout) <= 0)
			return 0;
		if (wbc__recv(c, &h, &pl, &fd) <= 0)
			return -1;
		wbc__other(c, &h, pl, fd);
		wbc__consume(c, &h);
	}
}

#endif
