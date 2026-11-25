# wbde client protocol

How a program other than wbde gets windows (and screens) on wbde and
draws into them.  amigafmt's intuition.library is such a client: it turns
`OpenWindow()` into `WBP_OPEN`, a RastPort into the shared buffer and
IDCMP messages out of `WBP_EVENT`s.  The structures are in
[`include/wbde_proto.h`](../include/wbde_proto.h); a header-only C client is
[`include/wbclient.h`](../include/wbclient.h), and
[`tools/wbclient-demo.c`](../tools/wbclient-demo.c) uses it.

## Model

- The Workbench screen is chunky, 8 bits a pixel, every pixel a pen of one
  256-entry palette.  `WBP_SCREEN` gives its size, depth, palette, the
  DrawInfo pens and the font: what `LockPubScreen("Workbench")` plus
  `GetScreenDrawInfo()` would.
- A window is a window of wbde's window core like any drawer: wbde draws
  its frame and gadgets (close, depth, zoom, size, drag bar), and moves,
  sizes, stacks and activates it.  The client owns only the inner area.
- The inner area is a shared memory buffer (a memfd passed with
  `SCM_RIGHTS`): `height` rows of `stride` bytes, each byte a pen.  The
  client draws into it and sends `WBP_DAMAGE` with the rectangle it
  changed; wbde copies that into the window's backing store and composes
  the screen again.  wbde reads the buffer at no other time, so drawing
  need not be atomic, only finished before the damage message.
- Windows are always SMART_REFRESH: wbde keeps every window's pixels, so a
  client is never asked to repaint because something moved over it.  The
  only repaint a client owes is after a resize, when it gets a new buffer.
- Events are Intuition's IDCMP messages with Intuition's numbers: classes
  (`IDCMP_MOUSEBUTTONS`, `IDCMP_CLOSEWINDOW`, ...), codes (`SELECTDOWN`,
  Amiga raw key codes, menu ids) and qualifiers (`IEQUALIFIER_*`).  A
  client asks for the classes it wants in `idcmp`, as `OpenWindow()` does.

## Transport

A Unix stream socket, `$WBDE_SOCKET` or `/tmp/wbde.sock` (wbde's
`--socket` sets it).  Every message is a `struct wbp_hdr`

| field | meaning |
|---|---|
| `type` | `WBP_*` |
| `len` | payload bytes that follow |
| `win` | window id, 0 for none |
| `serial` | chosen by the client; replies echo it |

followed by `len` bytes of payload.  Byte order and alignment are native:
both ends are on the same machine.  A request that fails is answered with
`WBP_ERROR`, carrying `-errno` and the request's serial.

## Client to server

| message | payload | answer |
|---|---|---|
| `WBP_HELLO` | `wbp_hello` (version, name) | `WBP_SCREEN` |
| `WBP_OPEN` | `wbp_open` | `WBP_WINDOW` + fd, `win` = the new id |
| `WBP_CLOSE` | - | the window goes |
| `WBP_DAMAGE` | `wbp_rect`, inner coordinates | - |
| `WBP_SET_TITLE` | the title, not terminated | - |
| `WBP_MOVE_SIZE` | `wbp_rect`: outer position, inner size | maybe `WBP_WINDOW` |
| `WBP_TO_FRONT`, `WBP_TO_BACK`, `WBP_ACTIVATE` | - | - |
| `WBP_SET_MENUS` | `wbp_menuitem[]` | - |
| `WBP_SET_POINTER` | `uint32_t` 0 normal, 1 busy, or `wbp_pointer` + an image (see Window pointers) | - |
| `WBP_OBTAIN_PEN` | `uint8_t rgb[3]` | `WBP_PEN`: `uint32_t` pen |
| `WBP_GET_FONT` | - | `WBP_FONT` |
| `WBP_ATTACH` | `wbp_attach` + a dma-buf fd (none: detach) | `WBP_ATTACHED` (see GPU buffers) |

`wbp_open` gives the outer position, the inner size and its limits, the
`WBP_WF_*` flags (the Intuition window flags a client can have: `CLOSE`,
`DEPTH`, `ZOOM`, `SIZE`, `DRAG`, `BORDERLESS`, `BACKDROP`, `RMBTRAP`, plus
`ACTIVATE` and `REPORTMOUSE`), the IDCMP mask and the title.

Menus are a flat list in order: a `WBP_MENU`, its `WBP_ITEM`s and
`WBP_BAR`s, each item followed by its `WBP_SUBITEM`s.  Items carry a
16-bit `id` that comes back as the `code` of `IDCMP_MENUPICK`, a Right
Amiga shortcut letter, and `WBP_MI_DISABLED`, `CHECKIT` and `CHECKED`.
While the client's window is active the right button shows its menus
instead of Workbench's, unless the window has `RMBTRAP`, when the button
is an event.

## Server to client

| message | payload |
|---|---|
| `WBP_SCREEN` | `wbp_screen`: size, depth, title bar and border sizes, font, pens, palette |
| `WBP_WINDOW` | `wbp_window` + the buffer's fd: outer position, inner size, stride, borders, bytes to map |
| `WBP_EVENT` | `wbp_event`: class, code, qualifier, inner mouse position, time, Linux key |
| `WBP_PEN` | `uint32_t` |
| `WBP_FONT` | `wbp_font`, 257 `wbp_glyph`, then `modulo * ysize` bytes of glyph rows |
| `WBP_ERROR` | `int32_t -errno` |

`WBP_WINDOW` is the reply to `WBP_OPEN` (it never comes before it), and
comes again, unasked, after every resize: the old buffer is finished with
(unmap it), the new one starts as a copy of what fitted, and an
`IDCMP_NEWSIZE` event follows.

Events sent, when the client asked for the class:

| class | when | code |
|---|---|---|
| `IDCMP_MOUSEBUTTONS` | a button in the inner area | `SELECTDOWN/UP`, `MIDDLEDOWN/UP`, `MENUDOWN/UP` (with `RMBTRAP`) |
| `IDCMP_MOUSEMOVE` | moves while a button is held in it, or always with `REPORTMOUSE` | 0 |
| `IDCMP_RAWKEY` | every key while active | Amiga raw key, `0x80` set on release |
| `IDCMP_VANILLAKEY` | keys that make a character | the character (Latin-1) |
| `IDCMP_CLOSEWINDOW` | close gadget released over | 0; the window stays until `WBP_CLOSE` |
| `IDCMP_NEWSIZE` | after a resize, after the new `WBP_WINDOW` | 0 |
| `IDCMP_MENUPICK` | a menu item chosen | the item's id |
| `IDCMP_ACTIVEWINDOW`, `IDCMP_INACTIVEWINDOW` | activation | 0 |

## Pens and colour

The buffer holds pens, not colours.  Pens 0-7 are the Workbench's (the
DrawInfo pens in `WBP_SCREEN` say which is which: draw text in
`pens[WBP_TEXTPEN]`, not in 1), 17-19 the pointer's, and 252-255 the
complements of 0-3 that Intuition keeps for XOR drawing.
`WBP_OBTAIN_PEN` is `ObtainBestPen()`: an exact match, a free pen set to
the colour, or the nearest.  A palette change reaches clients as a fresh
`WBP_SCREEN`.

## Intuition on the protocol

- `LockPubScreen()`: `WBP_HELLO`, and a Screen built from `WBP_SCREEN`.
- `OpenWindow()`: `WBP_OPEN`, with the buffer mapped as the window's
  pixels.  It is one plane of chunky pixels, not an Amiga BitMap, so the
  client either draws into it with chunky routines or keeps a planar
  BitMap and converts on the way to `WBP_DAMAGE`.
- Drawing into the window's RastPort: draw, then `WBP_DAMAGE` the bounding
  box.  One damage per `WaitPort()` is enough.
- The window's UserPort: `WBP_EVENT`s turned into IntuiMessages.
- `SetMenuStrip()`: `WBP_SET_MENUS`, with `FULLMENUNUM` as the id.

## Programs started from Workbench

A double click runs the program (through binfmt_misc, for an AmigaOS one)
on a pseudo terminal whose output, if there is any, opens a console
window.  Its environment says how it was started, so that amigafmt can
build a `WBStartup` message instead of a CLI start:

| variable | value |
|---|---|
| `WBDE_SOCKET` | the protocol socket (always) |
| `WBDE_ICON` | the `.info` of the icon that was opened (tool or project) |
| `WBDE_STACK` | the icon's `do_StackSize`, if it has one |

For a project, argv is `{ default tool, project }`, with the default
tool's `SYS:` path mapped onto the System partition's mount point.  The
tool types are in the `.info`; the current directory is the icon's
drawer.

## GPU buffers

With the GPU display a window's content can live in video memory, so that
the CPU never copies it.  Clients that do not ask see none of this.

`WBP_WF_VRAM` (`0x40000`) in `wbp_open.flags`: the window buffer is a pens
texture in VRAM and the fd that comes with `WBP_WINDOW` is its dma-buf
(map it as before; `wbp_window.flags` has `WBP_WIN_VRAM`).  The client's
stores are the upload, and wbde copies nothing on `WBP_DAMAGE`, it only
redraws.  Reading the buffer is slow (about 1.4 us a longword on an
A4000's Radeon), so draw and never read back: no ScrollRaster in it.
After a resize the new buffer starts as pen 0, not as a copy of the old
one.  Without the GPU display, or when VRAM cannot be had, the client
gets the ordinary memfd and no `WBP_WIN_VRAM`.  Each store crosses the
bus, so this is worth it for a client that stores longwords or writes
each pixel once, and not for one that draws byte by byte.

`WBP_ATTACH` with a dma-buf fd and a `wbp_attach { format, width, height,
pitch, offset }`: the inner area shows that buffer, scaled to fit, until
the window closes or another `WBP_ATTACH` comes without an fd.  The
formats are `WBP_FMT_PENS` (a byte a pixel, the screen's pens) and
`WBP_FMT_XRGB8888` (native `0x00RRGGBB` longwords: true colour, no pens
taken).  The pitch is in bytes, aligned as the card wants (256 pixels for
pens, 64 for XRGB8888), and the offset is 256-aligned.  The answer is
`WBP_ATTACHED`, or `WBP_ERROR` (`-ENOTSUP` without the GPU display).
`WBP_DAMAGE`, with any rectangle, says the buffer changed, and the whole
inner area is redrawn from it.  The buffer must be on the same card: a
GEM object exported with `DRM_IOCTL_PRIME_HANDLE_TO_FD`, for example from
the render node, or a frame that another card writes into VRAM over PCI.
`wbclient.h` has `wbc_attach()`.

## Screens

A client may open screens of its own, as an AmigaOS program opens a custom
screen, each with its own size, depth, palette and windows.  The screens
are a stack, the front one is on the display, and Workbench's is one of
them.

| message | payload | answer |
|---|---|---|
| `WBP_OPEN_SCREEN` | `wbp_newscreen` | `WBP_SCREEN_OPENED` (`wbp_screenbuf`) + the buffer's fd, `win` = the screen id |
| `WBP_SCREEN_DAMAGE` | `wbp_rect`, screen coordinates | - |
| `WBP_SCREEN_PALETTE` | `wbp_palette` (first, count) + count x r g b | - |
| `WBP_SCREEN_TITLE` | `int32_t` showtitle (1, 0, or -1 unchanged), then the title | - |
| `WBP_SCREEN_TO_FRONT`, `WBP_SCREEN_TO_BACK` | - | - |
| `WBP_SCREEN_CLOSE` | - | its windows go with it |

`win` in the header is the screen id for all of these.

- `wbp_newscreen` has the width, height, depth (1-8), `WBP_SF_SHOWTITLE`,
  the DrawInfo pens, the title, the first palette and the scale: display
  pixels per screen pixel across and down, as num/den.  The client knows
  the display mode (an Amiga ModeID) and so how big a pixel is; wbde only
  fits the result to the monitor, centred, and scrolled with the pointer
  when it is bigger than the monitor (Intuition's autoscroll).
  Outside the screen the display shows the screen's colour 0, as an
  Amiga shows its border.
  amigafmt asks for whole numbers from the ModeID: low resolution 2
  across, high 1, super-high 1/2; 2 down unless interlaced.
- The screen's own pixels (what its RastPort draws, under every window)
  are a buffer of pens like a window's (`wbp_screenbuf` has the size,
  stride and bytes to map), and `WBP_SCREEN_DAMAGE` says where it
  changed.
- Windows open on it with `wbp_open.screen` = its id (a `wbp_open` without
  that field, as version 1 clients send it, is on Workbench).  They are
  ordinary wbde windows with wbde's frames.
- The title bar is wbde's, drawn in the screen's pens with its depth
  gadget, in front of backdrop windows with `WBP_SF_SHOWTITLE` (or
  `WBP_SCREEN_TITLE` 1) and behind them otherwise.
- The user flips screens the Amiga way: Left Amiga + M sends the front
  screen to the back, Left Amiga + N brings Workbench to the front, and a
  click on a screen's depth gadget sends that screen to the back.
- A client that goes takes its screens with it.

## Window pointers

`WBP_SET_POINTER` with a `wbp_pointer { kind, hx, hy, width, height }`
followed by width x height pens (0 transparent; an Amiga sprite's colours
are pens 17, 18 and 19) gives the window its own pointer, shown while it
is the active window, as Intuition's `SetPointer()` does.  A bare
`uint32_t` kind is enough for `WBP_PTR_NORMAL` and `WBP_PTR_BUSY`.

## Not provided

Requesters drawn by wbde for a client (`EasyRequest` can be a client
window), gadgets drawn by wbde inside the inner area (gadtools can be done
client side), the clipboard, and AppIcons and AppWindows (dropping
Workbench icons onto a window).
