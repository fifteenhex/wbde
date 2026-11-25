/*
 * The server half of the client protocol. Each client window is an
 * ordinary struct win whose content is drawn from a memfd the client
 * shares: the window core neither knows nor cares, which is the point. The
 * events a client gets are Intuition's IDCMP messages.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "disp.h"
#include "font.h"
#include "menu.h"
#include "screens.h"
#include "server.h"
#include "wbde_proto.h"

#define MAXCLIENTS 16
#define RBUF (64 * 1024)

struct client;

struct cwin {
	struct cwin *next;
	struct client *c;
	uint32_t id;
	struct screen *scr;	/* Workbench's, or one of the client's */
	struct win *w;
	int memfd;
	uint8_t *shm;
	size_t shmsize;
	struct bm view;		/* the shared buffer as a bitmap */
	int vram;		/* the buffer is the display's, in VRAM (WBP_WF_VRAM) */
	int attached;		/* the content is a buffer the client attached */
	uint32_t idcmp;
	/* its pointer, while it is the active window */
	int ptr_kind;		/* WBP_PTR_* */
	struct bm ptr;
	int ptr_hx, ptr_hy;
	/* menus the client gave */
	struct menustrip strip;
	struct menu *menus;
	struct menuitem *items, *subs;
	char *labels;
};

/* a screen of the client's: wbde's struct screen, its pixels shared */
struct cscreen {
	struct cscreen *next;
	struct client *c;
	uint32_t id;
	struct screen s;
	struct bm fb;		/* where it is composed (software display, screenshots) */
	int memfd;
	uint8_t *shm;
	size_t shmsize;
	struct bm view;		/* the shared buffer as a bitmap */
	struct cwin *root;	/* events and menus of the screen's own pixels */
};

struct client {
	int fd;
	char name[32];
	uint8_t *rbuf;
	size_t rlen;
	struct cwin *wins;
	struct cscreen *screens;
	int dead;
	int fd_in;		/* the last fd the client sent, for WBP_ATTACH */
};

static struct screen *S;
static int listen_fd = -1;
static char sock_path[108];
static struct client *clients[MAXCLIENTS];
static uint32_t next_id = 1;
static server_penfn penfn;
static void *penarg;

/* --- keys --- */

int amiga_rawkey(int k)
{
	static const struct { uint16_t linux_key; uint8_t raw; } map[] = {
		{ 41, 0x00 }, { 2, 0x01 }, { 3, 0x02 }, { 4, 0x03 }, { 5, 0x04 }, { 6, 0x05 },
		{ 7, 0x06 }, { 8, 0x07 }, { 9, 0x08 }, { 10, 0x09 }, { 11, 0x0a }, { 12, 0x0b },
		{ 13, 0x0c }, { 43, 0x0d }, { 82, 0x0f }, { 16, 0x10 }, { 17, 0x11 }, { 18, 0x12 },
		{ 19, 0x13 }, { 20, 0x14 }, { 21, 0x15 }, { 22, 0x16 }, { 23, 0x17 }, { 24, 0x18 },
		{ 25, 0x19 }, { 26, 0x1a }, { 27, 0x1b }, { 79, 0x1d }, { 80, 0x1e }, { 81, 0x1f },
		{ 30, 0x20 }, { 31, 0x21 }, { 32, 0x22 }, { 33, 0x23 }, { 34, 0x24 }, { 35, 0x25 },
		{ 36, 0x26 }, { 37, 0x27 }, { 38, 0x28 }, { 39, 0x29 }, { 40, 0x2a }, { 75, 0x2d },
		{ 76, 0x2e }, { 77, 0x2f }, { 86, 0x30 }, { 44, 0x31 }, { 45, 0x32 }, { 46, 0x33 },
		{ 47, 0x34 }, { 48, 0x35 }, { 49, 0x36 }, { 50, 0x37 }, { 51, 0x38 }, { 52, 0x39 },
		{ 53, 0x3a }, { 83, 0x3c }, { 71, 0x3d }, { 72, 0x3e }, { 73, 0x3f }, { 57, 0x40 },
		{ 14, 0x41 }, { 15, 0x42 }, { 96, 0x43 }, { 28, 0x44 }, { 1, 0x45 }, { 111, 0x46 },
		{ 74, 0x4a }, { 103, 0x4c }, { 108, 0x4d }, { 106, 0x4e }, { 105, 0x4f },
		{ 59, 0x50 }, { 60, 0x51 }, { 61, 0x52 }, { 62, 0x53 }, { 63, 0x54 }, { 64, 0x55 },
		{ 65, 0x56 }, { 66, 0x57 }, { 67, 0x58 }, { 68, 0x59 }, { 179, 0x5a }, { 180, 0x5b },
		{ 98, 0x5c }, { 55, 0x5d }, { 78, 0x5e }, { 138, 0x5f }, { 42, 0x60 }, { 54, 0x61 },
		{ 58, 0x62 }, { 29, 0x63 }, { 56, 0x64 }, { 100, 0x65 }, { 125, 0x66 }, { 126, 0x67 },
	};
	unsigned int i;

	for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if (map[i].linux_key == k)
			return map[i].raw;
	return 0xff;
}

/* --- sending --- */

static int send_msg(struct client *c, uint32_t type, uint32_t win, uint32_t serial,
		    const void *payload, uint32_t len, int fd)
{
	struct wbp_hdr h = { type, len, win, serial };
	struct iovec iov[2] = { { &h, sizeof(h) }, { (void *)payload, len } };
	char cbuf[CMSG_SPACE(sizeof(int))];
	struct msghdr m = { 0 };
	ssize_t n;

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
	/* a client that stops reading loses messages, it does not stop Workbench */
	n = sendmsg(c->fd, &m, MSG_DONTWAIT | MSG_NOSIGNAL);
	if (n < 0 && errno != EAGAIN)
		c->dead = 1;
	return n == (ssize_t)(sizeof(h) + len) ? 0 : -1;
}

static unsigned int ieq(unsigned int qual)
{
	/* QUAL_* are IEQUALIFIER_* for the first eight bits */
	return qual & 0xff;
}

static void send_event(struct cwin *cw, uint32_t class, uint16_t code, unsigned int qual,
		       int x, int y, uint16_t linuxkey)
{
	struct wbp_event e = { 0 };
	struct timeval tv;

	if (!(cw->idcmp & class))
		return;
	gettimeofday(&tv, NULL);
	e.class = class;
	e.code = code;
	/* Intuition's qualifiers carry the mouse buttons too (moves while
	   drawing are told from moves by them) */
	e.qualifier = (uint16_t)(ieq(qual) | screens_buttons());
	e.mousex = (int16_t)x;
	e.mousey = (int16_t)y;
	e.seconds = (uint32_t)tv.tv_sec;
	e.micros = (uint32_t)tv.tv_usec;
	e.linuxkey = linuxkey;
	send_msg(cw->c, WBP_EVENT, cw->id, 0, &e, sizeof(e), -1);
}

/* --- shared buffers --- */

/*
 * The buffer in VRAM: the display's own texture, handed to the client as a
 * dma-buf.  wbde never maps it (reading VRAM costs a microsecond and a
 * half a longword); the display draws it as the window's content.  After a
 * resize the new one starts at pen 0: copying the old would mean reading it.
 */
static int make_vram_buffer(struct cwin *cw, int w, int h)
{
	struct disp_buf b;
	uint32_t size;
	int fd, ret;

	ret = disp_share(disp_current, cw->w, DISP_BUF_PENS, w > 0 ? w : 1, h > 0 ? h : 1, &fd,
			 &b, &size);
	if (ret)
		return ret;
	if (cw->memfd >= 0)
		close(cw->memfd);
	cw->memfd = fd;
	cw->shm = NULL;
	cw->shmsize = size;
	cw->view.px = NULL;
	cw->view.w = b.width;
	cw->view.h = b.height;
	cw->view.stride = b.pitch;
	return 0;
}

static int make_buffer(struct cwin *cw, int w, int h)
{
	int stride = (w + 3) & ~3, fd;
	size_t size = (size_t)stride * (h > 0 ? h : 1);
	uint8_t *p;

	if (cw->vram)
		return make_vram_buffer(cw, w, h);
	fd = memfd_create("wbde-window", MFD_CLOEXEC);
	if (fd < 0)
		return -errno;
	if (ftruncate(fd, (off_t)size)) {
		close(fd);
		return -errno;
	}
	p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		close(fd);
		return -errno;
	}
	memset(p, cw->scr->pens[P_BACKGROUND], size);
	/* keep what was there, top left anchored */
	if (cw->shm) {
		int y, cw_ = cw->view.w < w ? cw->view.w : w;

		for (y = 0; y < h && y < cw->view.h; y++)
			memcpy(p + (size_t)y * stride, cw->shm + (size_t)y * cw->view.stride, (size_t)cw_);
		munmap(cw->shm, cw->shmsize);
		close(cw->memfd);
	}
	cw->memfd = fd;
	cw->shm = p;
	cw->shmsize = size;
	cw->view.px = p;
	cw->view.w = w;
	cw->view.h = h;
	cw->view.stride = stride;
	return 0;
}

static void send_window(struct cwin *cw, uint32_t serial)
{
	struct wbp_window m;
	struct win *w = cw->w;

	m.left = (int16_t)w->r.x;
	m.top = (int16_t)w->r.y;
	m.width = (uint16_t)cw->view.w;
	m.height = (uint16_t)cw->view.h;
	m.stride = (uint16_t)cw->view.stride;
	m.bl = (uint16_t)w->bl;
	m.bt = (uint16_t)w->bt;
	m.br = (uint16_t)w->br;
	m.bb = (uint16_t)w->bb;
	m.flags = cw->vram ? WBP_WIN_VRAM : 0;
	m.size = (uint32_t)cw->shmsize;
	send_msg(cw->c, WBP_WINDOW, cw->id, serial, &m, sizeof(m), cw->memfd);
}

/* --- the window's ops --- */

static void cw_draw(struct win *w, struct rp *rp)
{
	struct cwin *cw = w->priv;

	/* content the display draws itself (VRAM): nothing of it here */
	if (cw->vram || cw->attached || !cw->view.px) {
		rp_fill(rp, 0, 0, rp->clip.w, rp->clip.h, cw->scr->pens[P_BACKGROUND]);
		return;
	}
	rp_blit(rp, 0, 0, &cw->view, 0, 0, cw->view.w, cw->view.h, -1);
}

static void cw_button(struct win *w, int x, int y, int button, int down, unsigned int qual)
{
	static const uint16_t codes[4][2] = {
		{ 0, 0 }, { SELECTUP, SELECTDOWN }, { MENUUP, MENUDOWN }, { MIDDLEUP, MIDDLEDOWN },
	};

	if (button < 1 || button > 3)
		return;
	send_event(w->priv, IDCMP_MOUSEBUTTONS, codes[button][down ? 1 : 0], qual, x, y, 0);
}

static void cw_motion(struct win *w, int x, int y)
{
	send_event(w->priv, IDCMP_MOUSEMOVE, 0, 0, x, y, 0);
}

static void cw_key(struct win *w, const struct keyev *k)
{
	struct cwin *cw = w->priv;
	int raw = amiga_rawkey(k->code);
	int mx = cw->scr->mx - w->r.x - w->bl, my = cw->scr->my - w->r.y - w->bt;

	if (raw != 0xff)
		send_event(cw, IDCMP_RAWKEY, (uint16_t)(raw | (k->down ? 0 : IECODE_UP_PREFIX)),
			   k->qual | (k->down == 2 ? IEQUALIFIER_REPEAT : 0), mx, my, k->code);
	if (k->down && k->ch)
		send_event(cw, IDCMP_VANILLAKEY, (uint16_t)k->ch, k->qual, mx, my, k->code);
}

static void cw_close(struct win *w)
{
	/* Intuition does not close it: it tells the owner, who does */
	send_event(w->priv, IDCMP_CLOSEWINDOW, 0, 0, 0, 0, 0);
}

static void cw_resized(struct win *w)
{
	struct cwin *cw = w->priv;
	struct rect in;

	/* do_open's own win_move_size gets here before there is a buffer:
	   do_open makes it and answers WBP_OPEN with it.  Sending one now
	   as well would give the client a stale buffer behind the real one. */
	if (cw->memfd < 0)
		return;
	win_inner(w, &in);
	if (make_buffer(cw, in.w, in.h))
		return;
	send_window(cw, 0);
	send_event(cw, IDCMP_NEWSIZE, 0, 0, 0, 0, 0);
}

/* the window's own pointer goes with its being active */
static void show_pointer(struct cwin *cw, int active)
{
	if (active && cw->ptr_kind == WBP_PTR_IMAGE && cw->ptr.px)
		screen_set_pointer(cw->scr, &cw->ptr, cw->ptr_hx, cw->ptr_hy, 0);
	else
		screens_std_pointer(cw->scr, active && cw->ptr_kind == WBP_PTR_BUSY);
}

static void cw_activate(struct win *w, int active)
{
	struct cwin *cw = w->priv;

	if (active || cw->ptr_kind != WBP_PTR_NORMAL)
		show_pointer(cw, active);
	send_event(cw, active ? IDCMP_ACTIVEWINDOW : IDCMP_INACTIVEWINDOW, 0, 0, 0, 0, 0);
}

static void set_pointer(struct cwin *cw, const uint8_t *p, uint32_t len)
{
	const struct wbp_pointer *wp = (const struct wbp_pointer *)p;
	uint32_t kind;

	if (len < 4)
		return;
	memcpy(&kind, p, 4);
	bm_free(&cw->ptr);
	cw->ptr_kind = WBP_PTR_NORMAL;
	if (kind == WBP_PTR_IMAGE && len >= sizeof(*wp) && wp->width && wp->height &&
	    wp->width <= 64 && wp->height <= 64 &&
	    len >= sizeof(*wp) + (uint32_t)wp->width * wp->height &&
	    !bm_alloc(&cw->ptr, wp->width, wp->height)) {
		int y;

		for (y = 0; y < wp->height; y++)
			memcpy(cw->ptr.px + (size_t)y * cw->ptr.stride,
			       p + sizeof(*wp) + (size_t)y * wp->width, wp->width);
		cw->ptr_kind = WBP_PTR_IMAGE;
		cw->ptr_hx = wp->hx;
		cw->ptr_hy = wp->hy;
	} else if (kind == WBP_PTR_BUSY) {
		cw->ptr_kind = WBP_PTR_BUSY;
	}
	if (cw->w && cw->scr->active == cw->w)
		show_pointer(cw, 1);
}

static const struct win_ops cw_ops = {
	.draw = cw_draw,
	.button = cw_button,
	.motion = cw_motion,
	.key = cw_key,
	.close = cw_close,
	.resized = cw_resized,
	.activate = cw_activate,
};

static void cw_menupick(void *arg, int id)
{
	struct cwin *cw = arg;

	send_event(cw, IDCMP_MENUPICK, (uint16_t)id, 0, 0, 0, 0);
}

static void free_menus(struct cwin *cw)
{
	free(cw->menus);
	free(cw->items);
	free(cw->subs);
	free(cw->labels);
	cw->menus = NULL;
	cw->items = cw->subs = NULL;
	cw->labels = NULL;
	if (cw->w)
		cw->w->menus = NULL;
}

/*
 * The flat list becomes Intuition-style tables. Going through it in order,
 * the items of each menu land next to each other in one pool and the
 * sub-items of each item next to each other in another, which is all the
 * tables need.
 */
static void set_menus(struct cwin *cw, const struct wbp_menuitem *in, int n)
{
	struct menuitem *it, *last = NULL;
	int i, nm = 0, mi = -1, ni = 0, ns = 0;

	free_menus(cw);
	for (i = 0; i < n; i++)
		nm += in[i].kind == WBP_MENU;
	if (!nm)
		return;
	cw->menus = calloc((size_t)nm, sizeof(*cw->menus));
	cw->items = calloc((size_t)n, sizeof(*cw->items));
	cw->subs = calloc((size_t)n, sizeof(*cw->subs));
	cw->labels = calloc((size_t)n, 41);
	if (!cw->menus || !cw->items || !cw->subs || !cw->labels) {
		free_menus(cw);
		return;
	}
	for (i = 0; i < n; i++) {
		char *label = cw->labels + (size_t)i * 41;

		memcpy(label, in[i].label, 40);
		if (in[i].kind == WBP_MENU) {
			mi++;
			cw->menus[mi].title = label;
			cw->menus[mi].items = &cw->items[ni];
			last = NULL;
			continue;
		}
		if (mi < 0)
			continue;
		if (in[i].kind == WBP_SUBITEM) {
			if (!last)
				continue;
			it = &cw->subs[ns++];
			if (!last->sub)
				last->sub = it;
			last->nsub++;
		} else {
			it = &cw->items[ni++];
			cw->menus[mi].nitems++;
			last = in[i].kind == WBP_ITEM ? it : NULL;
		}
		it->label = label;
		it->key = in[i].key;
		it->id = in[i].id;
		it->flags = in[i].kind == WBP_BAR ? MI_BAR :
			    (in[i].flags & WBP_MI_DISABLED ? MI_DISABLED : 0) |
			    (in[i].flags & WBP_MI_CHECKIT ? MI_CHECKIT : 0) |
			    (in[i].flags & WBP_MI_CHECKED ? MI_CHECKED : 0);
	}
	cw->strip.menus = cw->menus;
	cw->strip.n = nm;
	cw->strip.arg = cw;
	cw->strip.pick = cw_menupick;
	cw->strip.state = NULL;
	if (cw->w)
		cw->w->menus = &cw->strip;
}

/* --- requests --- */

static struct cwin *find_win(struct client *c, uint32_t id)
{
	struct cwin *cw;

	for (cw = c->wins; cw; cw = cw->next)
		if (cw->id == id)
			return cw;
	return NULL;
}

static void destroy_win(struct cwin *cw)
{
	struct cwin **p;

	for (p = &cw->c->wins; *p; p = &(*p)->next)
		if (*p == cw) {
			*p = cw->next;
			break;
		}
	if (cw->w && cw->scr->active == cw->w && cw->ptr_kind != WBP_PTR_NORMAL)
		screens_std_pointer(cw->scr, 0);
	if (cw->w)
		win_close(cw->scr, cw->w);	/* and the display's content with it */
	cw->w = NULL;
	bm_free(&cw->ptr);
	free_menus(cw);
	if (cw->shm)
		munmap(cw->shm, cw->shmsize);
	if (cw->memfd >= 0)
		close(cw->memfd);
	free(cw);
}

static void send_screen(struct client *c, uint32_t serial)
{
	struct wbp_screen m;
	int i;

	memset(&m, 0, sizeof(m));
	m.width = (uint16_t)S->w;
	m.height = (uint16_t)S->h;
	m.depth = (uint16_t)S->depth;
	m.barheight = (uint16_t)S->barh;
	m.wbortop = (uint16_t)wm_title_height(S);
	m.wborleft = 4;
	m.wborright = 18;
	m.wborbottom = 10;
	m.font_ysize = (uint16_t)S->font->ysize;
	m.font_xsize = (uint16_t)S->font->xsize;
	m.font_baseline = (uint16_t)S->font->baseline;
	snprintf(m.font_name, sizeof(m.font_name), "%s", S->font->name);
	for (i = 0; i < WBP_NUMPENS && i < NPENS; i++)
		m.pens[i] = S->pens[i];
	memcpy(m.palette, S->palette, sizeof(m.palette));
	send_msg(c, WBP_SCREEN, 0, serial, &m, sizeof(m), -1);
}

static void send_font(struct client *c, uint32_t serial)
{
	const struct font *f = S->font;
	size_t len = sizeof(struct wbp_font) + 257 * sizeof(struct wbp_glyph) +
		     (size_t)f->modulo * f->ysize;
	uint8_t *buf = calloc(1, len);
	struct wbp_font *h = (struct wbp_font *)buf;
	struct wbp_glyph *g = (struct wbp_glyph *)(h + 1);
	int i;

	if (!buf)
		return;
	snprintf(h->name, sizeof(h->name), "%s", f->name);
	h->ysize = (uint16_t)f->ysize;
	h->xsize = (uint16_t)f->xsize;
	h->baseline = (uint16_t)f->baseline;
	h->modulo = (uint16_t)f->modulo;
	h->lochar = (uint16_t)f->lochar;
	h->hichar = (uint16_t)f->hichar;
	for (i = 0; i < 257; i++) {
		g[i].offset = f->glyphs[i].offset;
		g[i].width = f->glyphs[i].width;
		g[i].space = f->glyphs[i].space;
		g[i].kern = f->glyphs[i].kern;
	}
	memcpy(g + 257, f->chardata, (size_t)f->modulo * f->ysize);
	send_msg(c, WBP_FONT, 0, serial, buf, (uint32_t)len, -1);
	free(buf);
}

static void send_error(struct client *c, const struct wbp_hdr *h, int err)
{
	int32_t e = -err;

	send_msg(c, WBP_ERROR, h->win, h->serial, &e, sizeof(e), -1);
}

static struct cscreen *find_screen(struct client *c, uint32_t id)
{
	struct cscreen *cs;

	for (cs = c->screens; cs; cs = cs->next)
		if (cs->id == id)
			return cs;
	return NULL;
}

/* --- screens --- */

/* a client screen's title bar is a borderless window of wbde's own */
static void bar_draw(struct win *w, struct rp *rp)
{
	screen_paint_bar(w->priv, rp);
}

static void bar_button(struct win *w, int x, int y, int button, int down, unsigned int qual)
{
	struct screen *s = w->priv;

	(void)y;
	(void)qual;
	/* the screen depth gadget (the right 23 pixels): this screen to the back */
	if (button == 1 && !down && x >= s->w - 23)
		screens_to_back(s);
}

static const struct win_ops bar_ops = {
	.draw = bar_draw,
	.button = bar_button,
};

static void cs_bar_depth(struct screen *s)
{
	screens_to_back(s);
}

static void root_button(struct screen *s, int x, int y, int button, int down, unsigned int qual)
{
	static const uint16_t codes[4][2] = {
		{ 0, 0 }, { SELECTUP, SELECTDOWN }, { MENUUP, MENUDOWN }, { MIDDLEUP, MIDDLEDOWN },
	};
	struct cscreen *cs = s->owner;

	if (button >= 1 && button <= 3)
		send_event(cs->root, IDCMP_MOUSEBUTTONS, codes[button][down ? 1 : 0], qual, x, y, 0);
}

static void root_motion(struct screen *s, int x, int y)
{
	struct cscreen *cs = s->owner;

	send_event(cs->root, IDCMP_MOUSEMOVE, 0, 0, x, y, 0);
}

static void root_key(struct screen *s, const struct keyev *k)
{
	struct cscreen *cs = s->owner;
	int raw = amiga_rawkey(k->code);

	if (raw != 0xff)
		send_event(cs->root, IDCMP_RAWKEY, (uint16_t)(raw | (k->down ? 0 : IECODE_UP_PREFIX)),
			   k->qual | (k->down == 2 ? IEQUALIFIER_REPEAT : 0), s->mx, s->my, k->code);
	if (k->down && k->ch)
		send_event(cs->root, IDCMP_VANILLAKEY, (uint16_t)k->ch, k->qual, s->mx, s->my, k->code);
}

static void do_open_screen(struct client *c, const struct wbp_hdr *h, const struct wbp_newscreen *n)
{
	struct cscreen *cs;
	struct wbp_screenbuf m;
	struct rect r;
	int w = n->width, ht = n->height, stride, i;
	char title[81];

	if (w < 16 || ht < 16 || w > 4096 || ht > 4096 || n->depth < 1 || n->depth > 8 ||
	    (n->sx_num && (!n->sx_den || !n->sy_num || !n->sy_den))) {
		send_error(c, h, EINVAL);
		return;
	}
	cs = calloc(1, sizeof(*cs));
	if (!cs || bm_alloc(&cs->fb, w, ht)) {
		free(cs);
		send_error(c, h, ENOMEM);
		return;
	}
	cs->c = c;
	cs->id = next_id++;
	stride = (w + 3) & ~3;
	cs->shmsize = (size_t)stride * ht;
	cs->memfd = memfd_create("wbde-screen", MFD_CLOEXEC);
	if (cs->memfd < 0 || ftruncate(cs->memfd, (off_t)cs->shmsize) ||
	    (cs->shm = mmap(NULL, cs->shmsize, PROT_READ | PROT_WRITE, MAP_SHARED, cs->memfd, 0)) ==
	    MAP_FAILED) {
		if (cs->memfd >= 0)
			close(cs->memfd);
		bm_free(&cs->fb);
		free(cs);
		send_error(c, h, ENOMEM);
		return;
	}
	memset(cs->shm, 0, cs->shmsize);
	cs->view.px = cs->shm;
	cs->view.w = w;
	cs->view.h = ht;
	cs->view.stride = stride;

	screen_init(&cs->s, &cs->fb, n->depth, S->font);
	for (i = 0; i < WBP_NUMPENS && i < NPENS; i++)
		cs->s.pens[i] = n->pens[i];
	memcpy(cs->s.palette, n->palette, sizeof(cs->s.palette));
	cs->s.ncolours = 1 << n->depth;
	if (n->sx_num) {                /* the client's own scale */
		cs->s.sx = (float)n->sx_num / n->sx_den;
		cs->s.sy = (float)n->sy_num / n->sy_den;
	} else                          /* the largest whole one that fits */
		cs->s.sx = cs->s.sy = (float)screens_fit_scale(w, ht);
	cs->s.id = (int)cs->id;
	cs->s.owner = cs;
	cs->s.bar_depth = cs_bar_depth;
	cs->s.showtitle = (n->flags & WBP_SF_SHOWTITLE) != 0;
	if (h->len >= sizeof(struct wbp_newscreen) && n->top < 4096)
		cs->s.top_edge = n->top;
	cs->s.flush_now = S->flush_now;
	cs->s.root_button = root_button;
	cs->s.root_motion = root_motion;
	cs->s.root_key = root_key;
	cs->root = calloc(1, sizeof(*cs->root));
	cs->root->c = c;
	cs->root->id = WBP_SCREEN_EVENT | cs->id;
	cs->root->scr = &cs->s;
	cs->root->memfd = -1;
	cs->root->idcmp = 0xffffffff;
	cs->s.mx = w / 2;
	cs->s.my = ht / 2;
	cs->s.fx = (float)cs->s.mx;
	cs->s.fy = (float)cs->s.my;
	memcpy(title, n->title, 80);
	title[80] = 0;
	snprintf(cs->s.title, sizeof(cs->s.title), "%s", title);
	/* the screen's own pixels start as the client's (all pen 0) */
	memset(cs->s.root.px, 0, (size_t)cs->s.root.stride * cs->s.root.h);
	bm_dirty_all(&cs->s.root);
	r = rect_make(0, 0, w, cs->s.barh);
	cs->s.barwin = win_open(&cs->s, &r, WF_BORDERLESS | WF_BACKDROP | WF_NOACTIVATE, "",
				&bar_ops, &cs->s);
	screen_place_bar(&cs->s);
	if (cs->s.barwin)
		win_refresh(&cs->s, cs->s.barwin);
	cs->next = c->screens;
	c->screens = cs;
	screens_add(&cs->s);
	fprintf(stderr, "wbde: screen %u for %s: %dx%dx%d at %gx, top %d\n", cs->id, c->name, w, ht,
		n->depth, cs->s.sx, cs->s.top_edge);

	memset(&m, 0, sizeof(m));
	m.width = (uint16_t)w;
	m.height = (uint16_t)ht;
	m.stride = (uint16_t)stride;
	m.barheight = (uint16_t)cs->s.barh;
	m.wbortop = (uint16_t)wm_title_height(&cs->s);
	m.wborleft = 4;
	m.wborright = 18;
	m.wborbottom = 10;
	m.size = (uint32_t)cs->shmsize;
	send_msg(c, WBP_SCREEN_OPENED, cs->id, h->serial, &m, sizeof(m), cs->memfd);
}

static void destroy_win(struct cwin *cw);

static void destroy_screen(struct cscreen *cs)
{
	struct cscreen **p;
	struct cwin *cw, *next;

	for (cw = cs->c->wins; cw; cw = next) {
		next = cw->next;
		if (cw->scr == &cs->s)
			destroy_win(cw);
	}
	for (p = &cs->c->screens; *p; p = &(*p)->next)
		if (*p == cs) {
			*p = cs->next;
			break;
		}
	screens_remove(&cs->s);
	cs->s.menus = NULL;
	free_menus(cs->root);
	free(cs->root);
	while (cs->s.top)
		win_close(&cs->s, cs->s.top);
	bm_free(&cs->s.root);
	bm_free(&cs->fb);
	munmap(cs->shm, cs->shmsize);
	close(cs->memfd);
	free(cs);
}

static void screen_damage_from(struct cscreen *cs, const struct wbp_rect *r)
{
	struct rp rp;
	struct rect d = rect_make(r->x, r->y, r->w, r->h);

	rp_init(&rp, &cs->s.root);
	rp_blit(&rp, r->x, r->y, &cs->view, r->x, r->y, r->w, r->h, -1);
	screen_damage(&cs->s, &d);
}

static void screen_palette_from(struct cscreen *cs, const uint8_t *p, uint32_t len)
{
	const struct wbp_palette *wp = (const struct wbp_palette *)p;
	unsigned int i;

	if (len < sizeof(*wp))
		return;
	for (i = 0; i < wp->count && wp->first + i < 256 && sizeof(*wp) + 3 * (i + 1) <= len; i++)
		memcpy(cs->s.palette[wp->first + i], p + sizeof(*wp) + 3 * i, 3);
	screens_palette(&cs->s);
}

static void do_open(struct client *c, const struct wbp_hdr *h, const struct wbp_open *o)
{
	unsigned int flags = o->flags & (WF_CLOSE | WF_DEPTH | WF_ZOOM | WF_SIZE | WF_DRAG |
					 WF_BORDERLESS | WF_BACKDROP | WF_RMBTRAP);
	struct cwin *cw = calloc(1, sizeof(*cw));
	char title[81];
	struct rect r, in;
	int ret;

	if (!cw) {
		send_error(c, h, ENOMEM);
		return;
	}
	memcpy(title, o->title, 80);
	title[80] = 0;
	cw->c = c;
	cw->id = next_id++;
	cw->memfd = -1;
	cw->scr = S;
	if (h->len >= sizeof(struct wbp_open) && o->screen) {
		struct cscreen *cs = find_screen(c, o->screen);

		if (!cs) {
			free(cw);
			send_error(c, h, ENOENT);
			return;
		}
		cw->scr = &cs->s;
	}
	cw->idcmp = o->idcmp;
	/* in VRAM only if asked and the display composes; else the usual memfd */
	cw->vram = (o->flags & WBP_WF_VRAM) && disp_current && disp_current->ops->share;
	/* the client gives the inner size; borders are the window core's business */
	r = rect_make(o->left, o->top, o->width, o->height);
	cw->w = win_open(cw->scr, &r, flags, title, &cw_ops, cw);
	if (!cw->w) {
		free(cw);
		send_error(c, h, ENOMEM);
		return;
	}
	r.w = o->width + cw->w->bl + cw->w->br;
	r.h = o->height + cw->w->bt + cw->w->bb;
	if (o->minwidth)
		cw->w->minw = o->minwidth + cw->w->bl + cw->w->br;
	if (o->minheight)
		cw->w->minh = o->minheight + cw->w->bt + cw->w->bb;
	if (o->maxwidth)
		cw->w->maxw = o->maxwidth + cw->w->bl + cw->w->br;
	if (o->maxheight)
		cw->w->maxh = o->maxheight + cw->w->bt + cw->w->bb;
	cw->w->wants_motion = (o->flags & WBP_WF_REPORTMOUSE) != 0;
	win_move_size(cw->scr, cw->w, &r);
	win_inner(cw->w, &in);
	ret = make_buffer(cw, in.w, in.h);
	if (ret && cw->vram) {		/* no VRAM to be had: the ordinary buffer */
		cw->vram = 0;
		ret = make_buffer(cw, in.w, in.h);
	}
	if (ret) {
		win_close(cw->scr, cw->w);
		free(cw);
		send_error(c, h, -ret);
		return;
	}
	cw->next = c->wins;
	c->wins = cw;
	if (o->flags & WBP_WF_ACTIVATE)
		win_activate(cw->scr, cw->w);
	win_refresh(cw->scr, cw->w);
	send_window(cw, h->serial);
}

static void do_damage(struct cwin *cw, const struct wbp_rect *r)
{
	struct rp rp;
	struct rect in, d;

	win_inner(cw->w, &in);
	if (cw->w->ext) {
		/* the display draws the client's buffer itself: no copy, just where */
		if (cw->attached) {
			/* an attached buffer is scaled to the inner area: all of it */
			d = rect_make(cw->w->r.x + in.x, cw->w->r.y + in.y, in.w, in.h);
			screen_damage(cw->scr, &d);
			return;
		}
	} else {
		rp_init(&rp, &cw->w->bm);
		rp_clip(&rp, &in);
		rp.ox = in.x;
		rp.oy = in.y;
		rp_blit(&rp, r->x, r->y, &cw->view, r->x, r->y, r->w, r->h, -1);
	}
	d = rect_make(cw->w->r.x + in.x + r->x, cw->w->r.y + in.y + r->y, r->w, r->h);
	if (!rect_intersect(&d, &d, &cw->w->r))
		return;
	screen_damage(cw->scr, &d);
}

/*
 * A buffer the client shares becomes the window's content (a GPU buffer
 * object: a G3 card's frame in VRAM, a GPU client's picture).  The fd came
 * with the message, or before it.
 */
static void do_attach(struct client *c, struct cwin *cw, const struct wbp_hdr *h,
		      const struct wbp_attach *a)
{
	struct disp_buf b;
	int fd = c->fd_in, ret;

	c->fd_in = -1;
	if (!cw || !a) {
		if (fd >= 0)
			close(fd);
		send_error(c, h, EINVAL);
		return;
	}
	if (fd < 0) {			/* detach: back to the window's own buffer */
		disp_detach(disp_current, cw->w);
		cw->attached = 0;
		win_refresh(cw->scr, cw->w);
		send_msg(c, WBP_ATTACHED, h->win, h->serial, NULL, 0, -1);
		return;
	}
	b.format = a->format == WBP_FMT_XRGB8888 ? DISP_BUF_XRGB8888 :
		   a->format == WBP_FMT_PENS ? DISP_BUF_PENS : -1;
	b.width = a->width;
	b.height = a->height;
	b.pitch = (int)a->pitch;
	b.offset = a->offset;
	ret = b.format < 0 ? -EINVAL : disp_attach(disp_current, cw->w, fd, &b);
	close(fd);			/* the display holds its own reference */
	if (ret) {
		send_error(c, h, -ret);
		return;
	}
	cw->attached = 1;
	win_refresh(cw->scr, cw->w);
	send_msg(c, WBP_ATTACHED, h->win, h->serial, NULL, 0, -1);
}

static void handle(struct client *c, const struct wbp_hdr *h, const uint8_t *p)
{
	int screen_msg = h->type >= WBP_OPEN_SCREEN && h->type <= WBP_SCREEN_FLAGS;
	struct cwin *cw = h->win && !screen_msg ? find_win(c, h->win) : NULL;
	struct cscreen *cs = h->win && screen_msg ? find_screen(c, h->win) : NULL;

	if (h->win && !cw && !cs && h->type != WBP_HELLO && h->type != WBP_OPEN_SCREEN) {
		send_error(c, h, ENOENT);
		return;
	}
	switch (h->type) {
	case WBP_OPEN_SCREEN:
		if (h->len >= WBP_NEWSCREEN_V1_SIZE)
			do_open_screen(c, h, (const struct wbp_newscreen *)p);
		else
			send_error(c, h, EINVAL);
		break;
	case WBP_SCREEN_CLOSE:
		if (cs)
			destroy_screen(cs);
		break;
	case WBP_SCREEN_DAMAGE:
		if (cs && h->len >= sizeof(struct wbp_rect))
			screen_damage_from(cs, (const struct wbp_rect *)p);
		break;
	case WBP_SCREEN_PALETTE:
		if (cs)
			screen_palette_from(cs, p, h->len);
		break;
	case WBP_SCREEN_TO_FRONT:
		if (cs)
			screens_to_front(&cs->s);
		break;
	case WBP_SCREEN_TO_BACK:
		if (cs)
			screens_to_back(&cs->s);
		break;
	case WBP_SCREEN_MENUS:
		if (cs) {
			set_menus(cs->root, (const struct wbp_menuitem *)p,
				  (int)(h->len / sizeof(struct wbp_menuitem)));
			cs->s.menus = cs->root->menus ? &cs->root->strip : NULL;
		}
		break;
	case WBP_SCREEN_FLAGS:
		if (cs && h->len >= 4) {
			uint32_t fl;

			memcpy(&fl, p, 4);
			cs->s.root_rmbtrap = (fl & WBP_SCF_RMBTRAP) != 0;
		}
		break;
	case WBP_SCREEN_TITLE:
		if (cs && h->len >= 4) {
			int32_t show;
			size_t n = h->len - 4 < sizeof(cs->s.title) - 1 ? h->len - 4 :
				   sizeof(cs->s.title) - 1;

			memcpy(&show, p, 4);
			memcpy(cs->s.title, p + 4, n);
			cs->s.title[n] = 0;
			if (show >= 0 && show != cs->s.showtitle) {
				cs->s.showtitle = show;
				screen_place_bar(&cs->s);
			}
			screen_draw_bar(&cs->s);
		}
		break;
	case WBP_HELLO:
		if (h->len >= sizeof(struct wbp_hello))
			snprintf(c->name, sizeof(c->name), "%.31s", ((const struct wbp_hello *)p)->name);
		send_screen(c, h->serial);
		break;
	case WBP_OPEN:
		if (h->len >= WBP_OPEN_V1_SIZE)
			do_open(c, h, (const struct wbp_open *)p);
		else
			send_error(c, h, EINVAL);
		break;
	case WBP_CLOSE:
		if (cw)
			destroy_win(cw);
		break;
	case WBP_DAMAGE:
		if (cw && h->len >= sizeof(struct wbp_rect))
			do_damage(cw, (const struct wbp_rect *)p);
		break;
	case WBP_SET_TITLE:
		if (cw) {
			char t[128];
			size_t n = h->len < sizeof(t) - 1 ? h->len : sizeof(t) - 1;

			memcpy(t, p, n);
			t[n] = 0;
			win_set_title(cw->scr, cw->w, t);
		}
		break;
	case WBP_MOVE_SIZE:
		if (cw && h->len >= sizeof(struct wbp_rect)) {
			const struct wbp_rect *r = (const struct wbp_rect *)p;
			struct rect nr = rect_make(r->x, r->y, r->w + cw->w->bl + cw->w->br,
						   r->h + cw->w->bt + cw->w->bb);

			win_move_size(cw->scr, cw->w, &nr);
		}
		break;
	case WBP_TO_FRONT:
		if (cw)
			win_to_front(cw->scr, cw->w);
		break;
	case WBP_TO_BACK:
		if (cw)
			win_to_back(cw->scr, cw->w);
		break;
	case WBP_ACTIVATE:
		if (cw)
			win_activate(cw->scr, cw->w);
		break;
	case WBP_SET_MENUS:
		if (cw)
			set_menus(cw, (const struct wbp_menuitem *)p,
				  (int)(h->len / sizeof(struct wbp_menuitem)));
		break;
	case WBP_SET_POINTER:
		if (cw)
			set_pointer(cw, p, h->len);
		break;
	case WBP_OBTAIN_PEN:
		if (h->len >= 3) {
			uint32_t pen = penfn ? (uint32_t)penfn(penarg, p[0], p[1], p[2]) : 1;

			send_msg(c, WBP_PEN, h->win, h->serial, &pen, sizeof(pen), -1);
		}
		break;
	case WBP_GET_FONT:
		send_font(c, h->serial);
		break;
	case WBP_ATTACH:
		do_attach(c, cw, h, h->len >= sizeof(struct wbp_attach) ?
				   (const struct wbp_attach *)p : NULL);
		break;
	default:
		send_error(c, h, ENOSYS);
		break;
	}
}

static void drop_client(int i)
{
	struct client *c = clients[i];

	while (c->wins)
		destroy_win(c->wins);
	while (c->screens)
		destroy_screen(c->screens);
	if (c->fd_in >= 0)
		close(c->fd_in);
	close(c->fd);
	free(c->rbuf);
	free(c);
	clients[i] = NULL;
}

int server_open(struct screen *s, const char *path, server_penfn pen, void *parg)
{
	struct sockaddr_un a = { 0 };

	S = s;
	penfn = pen;
	penarg = parg;
	if (!path)
		path = getenv("WBDE_SOCKET");
	if (!path)
		path = WBP_SOCKET_DEFAULT;
	snprintf(sock_path, sizeof(sock_path), "%s", path);
	listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (listen_fd < 0)
		return -errno;
	a.sun_family = AF_UNIX;
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", sock_path);
	unlink(sock_path);
	if (bind(listen_fd, (struct sockaddr *)&a, sizeof(a)) || listen(listen_fd, 8)) {
		close(listen_fd);
		listen_fd = -1;
		return -errno;
	}
	chmod(sock_path, 0666);
	/* everything started from Workbench knows where to find it */
	setenv("WBDE_SOCKET", sock_path, 1);
	fprintf(stderr, "wbde: clients on %s\n", sock_path);
	return 0;
}

int server_pollfds(struct pollfd *fds, int max)
{
	int n = 0, i;

	if (listen_fd < 0)
		return 0;
	if (n < max) {
		fds[n].fd = listen_fd;
		fds[n].events = POLLIN;
		fds[n].revents = 0;
		n++;
	}
	for (i = 0; i < MAXCLIENTS && n < max; i++) {
		if (!clients[i])
			continue;
		fds[n].fd = clients[i]->fd;
		fds[n].events = POLLIN;
		fds[n].revents = 0;
		n++;
	}
	return n;
}

static void read_client(int i)
{
	struct client *c = clients[i];
	ssize_t got;

	{
		/* with an fd now and then (WBP_ATTACH): the latest is kept for it */
		char cbuf[CMSG_SPACE(sizeof(int))];
		struct iovec iov = { c->rbuf + c->rlen, RBUF - c->rlen };
		struct msghdr m = { 0 };
		struct cmsghdr *cm;

		m.msg_iov = &iov;
		m.msg_iovlen = 1;
		m.msg_control = cbuf;
		m.msg_controllen = sizeof(cbuf);
		got = recvmsg(c->fd, &m, MSG_CMSG_CLOEXEC);
		for (cm = got > 0 ? CMSG_FIRSTHDR(&m) : NULL; cm; cm = CMSG_NXTHDR(&m, cm)) {
			if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
				if (c->fd_in >= 0)
					close(c->fd_in);
				memcpy(&c->fd_in, CMSG_DATA(cm), sizeof(int));
			}
		}
	}
	if (got <= 0) {
		if (got < 0 && errno == EAGAIN)
			return;
		c->dead = 1;
		return;
	}
	c->rlen += (size_t)got;
	while (c->rlen >= sizeof(struct wbp_hdr) && !c->dead) {
		struct wbp_hdr h;

		memcpy(&h, c->rbuf, sizeof(h));
		if (h.len > RBUF - sizeof(h)) {
			c->dead = 1;
			break;
		}
		if (c->rlen < sizeof(h) + h.len)
			break;
		handle(c, &h, c->rbuf + sizeof(h));
		/* the client may have gone with its windows; c itself stays until below */
		memmove(c->rbuf, c->rbuf + sizeof(h) + h.len, c->rlen - sizeof(h) - h.len);
		c->rlen -= sizeof(h) + h.len;
	}
}

void server_service(struct screen *s, const struct pollfd *fds, int n)
{
	int i, j;

	(void)s;
	for (j = 0; j < n; j++) {
		if (!fds[j].revents)
			continue;
		if (fds[j].fd == listen_fd) {
			int fd = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);

			if (fd < 0)
				continue;
			for (i = 0; i < MAXCLIENTS && clients[i]; i++)
				;
			if (i == MAXCLIENTS) {
				close(fd);
				continue;
			}
			clients[i] = calloc(1, sizeof(struct client));
			if (clients[i])
				clients[i]->rbuf = malloc(RBUF);
			if (!clients[i] || !clients[i]->rbuf) {
				free(clients[i]);
				clients[i] = NULL;
				close(fd);
				continue;
			}
			clients[i]->fd = fd;
			clients[i]->fd_in = -1;
			continue;
		}
		for (i = 0; i < MAXCLIENTS; i++)
			if (clients[i] && clients[i]->fd == fds[j].fd)
				read_client(i);
	}
	for (i = 0; i < MAXCLIENTS; i++)
		if (clients[i] && clients[i]->dead)
			drop_client(i);
}

void server_close(void)
{
	int i;

	for (i = 0; i < MAXCLIENTS; i++)
		if (clients[i])
			drop_client(i);
	if (listen_fd >= 0) {
		close(listen_fd);
		unlink(sock_path);
	}
	listen_fd = -1;
}
