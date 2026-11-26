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
| `WBP_ATTACH_BUFFER`, `WBP_PRESENT` | several buffers and which shows | see Flips |
| `WBP_OPEN_CON` | `wbp_opencon` + an fd | `WBP_CON_OPENED`, `win` = its id (see Console windows) |

`wbp_open` gives the outer position, the inner size and its limits, the
`WBP_WF_*` flags (the Intuition window flags a client can have: `CLOSE`,
`DEPTH`, `ZOOM`, `SIZE`, `DRAG`, `BORDERLESS`, `BACKDROP`, `RMBTRAP`, plus
`ACTIVATE` and `REPORTMOUSE`, and wbde's own `VRAM` and `UNSCALED`, below),
the IDCMP mask and the title.

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
| `WBP_WINDOW` | `wbp_window` + the buffer's fd: outer position, inner size, stride, borders, flags, bytes to map, the screen's scale |
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
| `IDCMP_CHANGEWINDOW` | after the window moved or was sized (by the user, or `WBP_MOVE_SIZE`) | 0; `mousex/mousey` are the new outer left and top in screen pixels |

A client has no `Window->LeftEdge` to read, so `IDCMP_CHANGEWINDOW` carries
the position itself; `wbclient.h` keeps `w->left` and `w->top` up to date from it,
which is what a client needs to resize with `WBP_MOVE_SIZE` (it takes the
outer position too) without moving the window back to where it opened.

As in Intuition, the qualifier carries the mouse buttons held.

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

## Flips

`WBP_ATTACH` shows one buffer, which the client (or a card on the bus)
overwrites while the GPU may be reading it, so frames can tear and wbde
cannot tell a whole frame from a half drawn one.  With flips a window has
several buffers and shows one at a time.  GPU display only.

| message | payload | answer |
|---|---|---|
| `WBP_ATTACH_BUFFER` | `wbp_attach_buffer` (`wbp_attach` and a `slot`) + a dma-buf fd | `WBP_ATTACHED` |
| `WBP_PRESENT` | `wbp_present { slot, cookie }` | later, `WBP_PRESENTED` |

- There are up to `WBP_MAX_BUFFERS` (8) slots, each with its own dma-buf,
  format, size, pitch and offset; several may be one buffer object at
  different offsets.  Attaching a slot again replaces it, and `WBP_ATTACH`
  with no fd drops every slot.
- `WBP_PRESENT` shows `slot` from the next composite on (the whole inner
  area is redrawn).
- `WBP_PRESENTED { slot, cookie, released, usecs }` answers it once a
  composite showing `slot` has been sent to the GPU and the GPU has
  finished reading the buffer shown before (`released`, -1 if there was
  none or it was the same slot).  The client may draw into `released` from
  then on, so a client that draws only into released buffers never has a
  half drawn frame on the screen.  `usecs` is the time from the present to
  the answer.  Answers come in order, so each also releases every buffer
  before it.
- wbde keeps a duplicate of each slot's dma-buf and polls it for
  `POLLOUT`, which a dma-buf reports once every fence on it (the command
  streams that read it) has signalled.  Slots that are one buffer object
  share its fences, so the answer can come a composite later than it
  needs to.
- With three buffers neither side waits for the other in the steady
  state; with two, the client waits for the answer before drawing.
- `wbclient.h` has `wbc_attach_buffer()`, `wbc_present()`, and
  `wbc_presented()` to take the answers (`wbc_wait()` returns 0 when one
  arrives).

## Unscaled content

Workbench is shown at a scale (3x on a 1920x1200 panel, for a 640x400
Workbench), and so is every window's content.  A client whose pixels are
already display-sized, such as a camera, another computer's framebuffer or
a video, can ask for them not to be.

`WBP_WF_UNSCALED` (`0x80000`) in `wbp_open.flags`: the content is in
display pixels and is shown 1:1.

- `wbp_open`'s width and height, and the size limits, are display pixels.
  wbde makes the inner area that many display pixels, rounded up to whole
  screen pixels (one more at a scale that is not a whole number), so the
  frame, the title bar and the gadgets stay at Workbench's scale around it.
  The content starts at the inner area's top left, on a whole display
  pixel; what the rounding leaves at the right and the bottom (under one
  screen pixel) is the background pen.
- The answer's `wbp_window.flags` has `WBP_WIN_UNSCALED` when the flag
  was granted.  Then `width`, `height` and `stride` are the content's, in
  display pixels, and the shared buffer holds the content pixel for pixel.
  The borders are still screen pixels.  Without it (a display that does
  not compose, see below) the window is an ordinary one and the sizes are
  screen pixels; the client can open again without the flag at a size of
  its choice.
- `wbp_window.scalex` and `scaley` (16.16, display pixels a screen pixel)
  come with every `WBP_WINDOW`, unscaled or not, so that a client can pick
  a size that is a whole multiple.  They are at the end of `wbp_window`: a
  message `WBP_WINDOW_V1_SIZE` long is from a wbde without them, which
  never grants the flag.
- `WBP_DAMAGE` rectangles, `WBP_MOVE_SIZE`'s inner size and every event's
  `mousex` and `mousey` are content pixels.  The mouse position is where
  the pointer's hot spot is drawn, measured from the content's top left;
  the pointer moves display width / 640 display pixels a mouse count, so
  that is the resolution of positions, not one pixel.
- The window may still be sizeable: after the user sizes it, the new
  buffer is as many display pixels as the inner area holds.
- It combines with the other two kinds of content: with `WBP_WF_VRAM` the
  VRAM buffer is display-sized, and with `WBP_ATTACH` the attached buffer
  is shown over the content size (scaled from its own size, so a buffer
  the content's size is 1:1 and one half its size an exact 2x).
- Only the GPU display grants it.  The software display composes the
  logical Workbench in RAM and scales the whole screen on the way out, so
  there is no display pixel to put the content on.

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
| `WBP_SCREEN_MENUS` | `wbp_menuitem[]` | - |
| `WBP_SCREEN_FLAGS` | `uint32_t` `WBP_SCF_*` | - |

`win` in the header is the screen id for all of these.

- `wbp_newscreen` has the width, height, depth (1-8), `WBP_SF_SHOWTITLE`,
  the DrawInfo pens, the title, the first palette and the scale: display
  pixels per screen pixel across and down, as num/den.  The client knows
  the display mode (an Amiga ModeID) and so how big a pixel is; wbde only
  fits the result to the monitor, centred, and scrolled with the pointer
  when it is bigger than the monitor (Intuition's autoscroll).
  A scale of 0 is wbde's choice, the largest whole scale that fits, as
  for Workbench.  Outside the screen the display is black.
  amigafmt asks for whole numbers from the ModeID: low resolution 2
  across, high 1, super-high 1/2; 2 down unless interlaced.
- `wbp_newscreen.top` opens the screen lower down: the screen behind it
  shows above it, with its own palette, and the new screen takes the
  scale and position of the one behind.  Only the GPU display shows
  this; the software display shows the front screen alone.
- The screen's own pixels (what its RastPort draws, under every window)
  are a buffer of pens like a window's (`wbp_screenbuf` has the size,
  stride and bytes to map), and `WBP_SCREEN_DAMAGE` says where it
  changed.
- The screen's own pixels take input as an Intuition backdrop window
  drawing into the screen's bitmap would: buttons, moves and keys where
  there is no window, or none is active, come as `WBP_EVENT` with `win`
  = `WBP_SCREEN_EVENT` | the screen id, in screen coordinates.  There
  the right button shows the `WBP_SCREEN_MENUS`, or with
  `WBP_SCF_RMBTRAP` is an event itself.
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

## Console windows

wbde's Shell and CON: windows are a terminal it draws itself: VT100
enough for a shell (line editing, clearing, colours as Workbench pens), in
the screen font, with keys typed into it sent as a terminal sends them
(Return is `\r`, the cursor keys `ESC [ A` and so on).  A client can have
one on an fd of its own, for whatever it wants to talk to (a serial line,
a program it runs, a card's console) without drawing text itself.

| message | payload | answer |
|---|---|---|
| `WBP_OPEN_CON` | `wbp_opencon { left, top, width, height, flags, title }` + an fd | `WBP_CON_OPENED`, `win` = the console's id |

- The fd is a pty master or one end of a `socketpair()`: wbde reads what
  to show from it and writes the keys to it.  Close your copy of the end
  you sent and keep the other.
- The rectangle is outer, in screen pixels, on Workbench; a width or
  height of 0 is wbde's choice (with all four 0: the screen's width and a
  third of its height, under the title bar).  `flags` is 0.
- `\n` only moves down, as on a terminal: send `\r\n` for a new line (a
  pty's `ONLCR` does that for a program; a client writing to a socket does
  it itself).
- It goes when either side lets go: the client closing its end, or
  `WBP_CLOSE` with the id, closes the window, and the user closing the
  window closes wbde's end, so that the client reads end of file.  It
  goes too when the client disconnects.
- `WBP_SET_TITLE`, `WBP_TO_FRONT`, `WBP_TO_BACK` and `WBP_ACTIVATE` take
  the id as they take a window's.  It sends no events: the keys are on
  the fd, and while it is active the menus are Workbench's.
- There is no scrollback: what scrolls off the top is gone, as in wbde's
  Shell.
- `wbclient.h` has `wbc_open_con()` and `wbc_close_con()`.

## Not provided

Requesters drawn by wbde for a client (`EasyRequest` can be a client
window), gadgets drawn by wbde inside the inner area (gadtools can be done
client side), the clipboard, and AppIcons and AppWindows (dropping
Workbench icons onto a window).
