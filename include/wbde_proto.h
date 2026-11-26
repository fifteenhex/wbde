/*
 * wbde client protocol, version 1. See doc/client-protocol.md.
 *
 * A client connects to a Unix stream socket ($WBDE_SOCKET, default
 * /tmp/wbde.sock) and exchanges messages: a fixed header, then `len` bytes
 * of payload. Both ends are on the same machine, so everything is in its
 * native byte order and alignment. A window's contents are a shared memory
 * buffer of pens (8 bits per pixel, the screen's palette) handed to the
 * client as a file descriptor; the client draws into it and says where.
 *
 * Event classes, codes and qualifiers are Intuition's own (IDCMP_*,
 * SELECTDOWN..., IEQUALIFIER_*) and keys come as Amiga raw key codes, so an
 * AmigaOS emulation can pass them through untouched.
 */
#ifndef WBDE_PROTO_H
#define WBDE_PROTO_H

#include <stdint.h>

#define WBP_VERSION		1
#define WBP_SOCKET_DEFAULT	"/tmp/wbde.sock"

enum wbp_type {
	/* client to server */
	WBP_HELLO = 1,		/* wbp_hello -> WBP_SCREEN */
	WBP_OPEN,		/* wbp_open -> WBP_WINDOW (+fd) */
	WBP_CLOSE,		/* (win) */
	WBP_DAMAGE,		/* wbp_rect: the client drew there (inner coords) */
	WBP_SET_TITLE,		/* char[] */
	WBP_MOVE_SIZE,		/* wbp_rect: outer position, inner size */
	WBP_TO_FRONT,
	WBP_TO_BACK,
	WBP_ACTIVATE,
	WBP_SET_MENUS,		/* wbp_menuitem[] */
	WBP_SET_POINTER,	/* uint32_t: 0 normal, 1 busy; or wbp_pointer + an image */
	WBP_OBTAIN_PEN,		/* uint8_t rgb[3] -> WBP_PEN */
	WBP_GET_FONT,		/* -> WBP_FONT */
	WBP_ATTACH,		/* wbp_attach + a dma-buf fd (none: detach) -> WBP_ATTACHED */
	/* screens of the client's own (see "Screens" in doc/client-protocol.md);
	   `win` carries the screen id */
	WBP_OPEN_SCREEN,	/* wbp_newscreen -> WBP_SCREEN_OPENED (+fd), win = the id */
	WBP_SCREEN_CLOSE,	/* the screen and every window on it go */
	WBP_SCREEN_DAMAGE,	/* wbp_rect: the client drew there (screen coords) */
	WBP_SCREEN_PALETTE,	/* wbp_palette + count * 3 bytes */
	WBP_SCREEN_TO_FRONT,
	WBP_SCREEN_TO_BACK,
	WBP_SCREEN_TITLE,	/* int32_t showtitle (-1: unchanged), then the title */
	WBP_SCREEN_MENUS,	/* wbp_menuitem[]: the menus of the screen's own pixels */
	WBP_SCREEN_FLAGS,	/* uint32_t WBP_SCF_* */

	/* server to client */
	WBP_SCREEN = 64,	/* wbp_screen */
	WBP_WINDOW,		/* wbp_window, with the buffer's fd */
	WBP_EVENT,		/* wbp_event */
	WBP_PEN,		/* uint32_t pen */
	WBP_FONT,		/* wbp_font, then glyphs, then chardata */
	WBP_ERROR,		/* int32_t -errno for the request with this serial */
	WBP_ATTACHED,		/* (empty) the window shows the attached buffer */
	WBP_SCREEN_OPENED,	/* wbp_screenbuf, with the screen buffer's fd */
};

struct wbp_hdr {
	uint32_t type;
	uint32_t len;		/* payload bytes after the header */
	uint32_t win;		/* window id, 0 for none */
	uint32_t serial;	/* the client's, echoed in replies */
};

struct wbp_hello {
	uint32_t version;
	char name[32];
};

/* DrawInfo pens, in Intuition's order */
enum {
	WBP_DETAILPEN, WBP_BLOCKPEN, WBP_TEXTPEN, WBP_SHINEPEN, WBP_SHADOWPEN,
	WBP_FILLPEN, WBP_FILLTEXTPEN, WBP_BACKGROUNDPEN, WBP_HIGHLIGHTTEXTPEN,
	WBP_BARDETAILPEN, WBP_BARBLOCKPEN, WBP_BARTRIMPEN, WBP_NUMPENS
};

struct wbp_screen {
	uint16_t width, height;	/* logical screen */
	uint16_t depth;
	uint16_t barheight;	/* screen title bar */
	uint16_t wbortop, wborleft, wborright, wborbottom;	/* a sizeable window's */
	uint16_t font_ysize, font_xsize, font_baseline, pad;
	char font_name[32];	/* "topaz.font" */
	uint8_t pens[16];	/* WBP_NUMPENS used */
	uint8_t palette[256][3];
};

/* window flags (the Intuition ones a client can ask for) */
#define WBP_WF_CLOSE		0x0001
#define WBP_WF_DEPTH		0x0002
#define WBP_WF_ZOOM		0x0004
#define WBP_WF_SIZE		0x0008
#define WBP_WF_DRAG		0x0010
#define WBP_WF_BORDERLESS	0x0020
#define WBP_WF_BACKDROP		0x0040
#define WBP_WF_RMBTRAP		0x0200
#define WBP_WF_ACTIVATE		0x10000	/* activate when opened */
#define WBP_WF_REPORTMOUSE	0x20000	/* MOUSEMOVE while active, not only while dragging */
#define WBP_WF_VRAM		0x40000	/* the buffer in video memory, if the display composes:
					   writes go straight to the card, reads are slow */
#define WBP_WF_UNSCALED		0x80000	/* the content is in display pixels, shown 1:1, not
					   scaled with Workbench (composing displays; the
					   answer's WBP_WIN_UNSCALED says it was granted) */

struct wbp_open {
	int16_t left, top;		/* outer position on the screen */
	uint16_t width, height;		/* inner size: what the client draws (with
					   WBP_WF_UNSCALED: in display pixels) */
	uint16_t minwidth, minheight, maxwidth, maxheight;	/* inner, 0 = any (likewise) */
	uint32_t flags;			/* WBP_WF_* */
	uint32_t idcmp;			/* IDCMP_* classes the client wants */
	char title[80];
	/* added for screens: a message without it (a version 1 client's) is
	   on Workbench */
	uint32_t screen;		/* 0 Workbench, else the client's screen id */
};
#define WBP_OPEN_V1_SIZE	(sizeof(struct wbp_open) - sizeof(uint32_t))

/*
 * A screen of the client's own: its size and depth, its palette and
 * DrawInfo pens, and how the display should scale it (display pixels per
 * screen pixel, as num/den: the client knows the Amiga display mode, and
 * wbde only fits the result to the monitor).  The screen's own pixels
 * (what its RastPort draws, under the windows) are a buffer of pens like
 * a window's.  Windows open on it with wbp_open.screen.
 */
#define WBP_SF_SHOWTITLE	0x0001	/* the title bar in front of backdrop windows */

struct wbp_newscreen {
	uint16_t width, height, depth;
	uint16_t flags;			/* WBP_SF_* */
	uint16_t sx_num, sx_den, sy_num, sy_den;	/* 0: wbde's choice, the largest
							   whole scale that fits */
	uint8_t pens[16];		/* WBP_NUMPENS used */
	char title[80];
	uint8_t palette[256][3];
	/* added: a message without it is a screen at the top */
	uint16_t top;			/* rows down: the screen behind shows above */
	uint16_t pad;
};
#define WBP_NEWSCREEN_V1_SIZE	(sizeof(struct wbp_newscreen) - 4)

struct wbp_screenbuf {
	uint16_t width, height, stride;
	uint16_t barheight;		/* the title bar wbde draws */
	uint16_t wbortop, wborleft, wborright, wborbottom;
	uint32_t size;			/* bytes to mmap */
};

/*
 * WBP_SET_POINTER: the window's pointer, shown while it is the active
 * window (as Intuition does).  A bare uint32_t kind is enough for the
 * first two; an image is width x height pens, 0 transparent (an Amiga
 * sprite's colours are pens 17, 18, 19), one pen a pointer pixel at the
 * Workbench pointer's own size.
 */
#define WBP_PTR_NORMAL		0
#define WBP_PTR_BUSY		1
#define WBP_PTR_IMAGE		2

struct wbp_pointer {
	uint32_t kind;			/* WBP_PTR_* */
	int16_t hx, hy;			/* the hot spot, in the image */
	uint16_t width, height;		/* then width * height pens */
};

/*
 * The screen's own pixels as a window: events where there is no window
 * (or none is active) come as WBP_EVENT with `win` = WBP_SCREEN_EVENT |
 * the screen id, in screen coordinates; WBP_SCREEN_MENUS are the menus the
 * right button shows there; WBP_SCF_RMBTRAP gives the right button to the
 * screen instead.  What an Intuition backdrop window that draws into the
 * screen's bitmap needs.
 */
#define WBP_SCREEN_EVENT	0x80000000
#define WBP_SCF_RMBTRAP		0x0001

struct wbp_palette {
	uint16_t first, count;		/* then count * 3 bytes, r g b */
};

struct wbp_window {
	int16_t left, top;		/* outer */
	uint16_t width, height;		/* inner: the buffer's size (WBP_WIN_UNSCALED: display
					   pixels, the content's; else screen pixels) */
	uint16_t stride;		/* bytes per buffer row */
	uint16_t bl, bt, br, bb;	/* borders around it (screen pixels) */
	uint16_t flags;			/* WBP_WIN_* */
	uint32_t size;			/* bytes to mmap */
	/* added with WBP_WF_UNSCALED: a message without them is from an older
	   wbde, which never grants it */
	uint32_t scalex, scaley;	/* display pixels per screen pixel, 16.16 */
};
#define WBP_WINDOW_V1_SIZE	(sizeof(struct wbp_window) - 2 * sizeof(uint32_t))

#define WBP_WIN_VRAM		0x0001	/* this buffer is in video memory (asked with WBP_WF_VRAM) */
#define WBP_WIN_UNSCALED	0x0002	/* the content is in display pixels, 1:1 (WBP_WF_UNSCALED) */

/*
 * WBP_ATTACH: the window's inner area shows a buffer the client shares as
 * a dma-buf (a GPU buffer object: its own, or one a card on the bus writes
 * into), scaled to the inner size by the display (an unscaled window's: to
 * its content size in display pixels, so a buffer that size is shown 1:1);
 * WBP_DAMAGE says it changed.  Only a composing (GPU) display can: others answer -ENOTSUP.
 * Sent with no fd, the window goes back to its own buffer.
 */
#define WBP_FMT_PENS		0	/* a byte a pixel, the screen's pens */
#define WBP_FMT_XRGB8888	1	/* native 0x00RRGGBB longwords: true colour */

struct wbp_attach {
	uint32_t format;		/* WBP_FMT_* */
	uint16_t width, height;		/* pixels */
	uint32_t pitch;			/* bytes a row (the card's alignment: 256 pixels
					   for pens, 64 for XRGB8888) */
	uint32_t offset;		/* of pixel 0,0 in the buffer, 256-aligned */
};

struct wbp_rect {
	int16_t x, y;
	uint16_t w, h;
};

/* IDCMP classes, as in intuition/intuition.h */
#define IDCMP_NEWSIZE		0x00000002
#define IDCMP_REFRESHWINDOW	0x00000004
#define IDCMP_MOUSEBUTTONS	0x00000008
#define IDCMP_MOUSEMOVE		0x00000010
#define IDCMP_MENUPICK		0x00000100
#define IDCMP_CLOSEWINDOW	0x00000200
#define IDCMP_RAWKEY		0x00000400
#define IDCMP_CHANGEWINDOW	0x02000000
#define IDCMP_ACTIVEWINDOW	0x00040000
#define IDCMP_INACTIVEWINDOW	0x00080000
#define IDCMP_VANILLAKEY	0x00200000
#define IDCMP_INTUITICKS	0x00400000

/* MOUSEBUTTONS codes */
#define SELECTDOWN	0x68
#define SELECTUP	0xe8
#define MENUDOWN	0x69
#define MENUUP		0xe9
#define MIDDLEDOWN	0x6a
#define MIDDLEUP	0xea
#define IECODE_UP_PREFIX 0x80

/* qualifiers, IEQUALIFIER_* */
#define IEQUALIFIER_LSHIFT	0x0001
#define IEQUALIFIER_RSHIFT	0x0002
#define IEQUALIFIER_CAPSLOCK	0x0004
#define IEQUALIFIER_CONTROL	0x0008
#define IEQUALIFIER_LALT	0x0010
#define IEQUALIFIER_RALT	0x0020
#define IEQUALIFIER_LCOMMAND	0x0040
#define IEQUALIFIER_RCOMMAND	0x0080
#define IEQUALIFIER_NUMERICPAD	0x0100
#define IEQUALIFIER_REPEAT	0x0200
#define IEQUALIFIER_MIDBUTTON	0x1000
#define IEQUALIFIER_RBUTTON	0x2000
#define IEQUALIFIER_LEFTBUTTON	0x4000

struct wbp_event {
	uint32_t class;		/* one IDCMP_* */
	uint16_t code;		/* button code, raw key, menu id, character */
	uint16_t qualifier;
	int16_t mousex, mousey;	/* inner coordinates (an unscaled window's: content pixels) */
	uint32_t seconds, micros;
	uint16_t linuxkey;	/* RAWKEY: the Linux KEY_* it came from */
	uint16_t pad;
};

/* menus: a flat list, menus then their items then sub-items in order */
#define WBP_MENU	1
#define WBP_ITEM	2
#define WBP_SUBITEM	3
#define WBP_BAR		4	/* separator in the current menu */

#define WBP_MI_DISABLED	0x01
#define WBP_MI_CHECKIT	0x02
#define WBP_MI_CHECKED	0x04

struct wbp_menuitem {
	uint8_t kind;
	uint8_t flags;
	char key;		/* Right Amiga shortcut, 0 none */
	uint8_t pad;
	uint16_t id;		/* MENUPICK code; 0 for menus and bars */
	uint16_t pad2;
	char label[40];
};

/* glyph i: offset/width into each chardata row, advance, kern */
struct wbp_glyph {
	uint16_t offset, width;
	int16_t space, kern;
};

struct wbp_font {
	char name[32];
	uint16_t ysize, xsize, baseline, modulo;
	uint16_t lochar, hichar;
	/* followed by 257 wbp_glyph (256 = the "no glyph" one), then modulo * ysize bytes */
};

#endif
