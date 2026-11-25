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
| `WBP_SET_POINTER` | `uint32_t` 0 normal, 1 busy (accepted, not shown) | - |
| `WBP_OBTAIN_PEN` | `uint8_t rgb[3]` | `WBP_PEN`: `uint32_t` pen |
| `WBP_GET_FONT` | - | `WBP_FONT` |

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

## Not provided

Screens other than Workbench's, busy pointer images, requesters drawn by
wbde for a client (`EasyRequest` can be a client window), gadgets drawn by
wbde inside the inner area (gadtools can be done client side), the
clipboard, and AppIcons and AppWindows (dropping Workbench icons onto a
window).
