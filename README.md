# wbde

An AmigaOS Workbench lookalike for Linux, written for the Amiga 4000: the
Workbench screen with its disk icons, drawer windows with the classic
gadgets, menus on the right button, topaz, the red pointer, a Shell, and a
socket protocol through which other programs get windows and screens of
their own.  AmigaOS programs run by amigafmt (a separate project) open
their windows on it.

Nothing in this repository comes from AmigaOS.  wbde reads what it shows
from the machine it runs on: the fonts, palette and pointers from the
Kickstart ROM, the icons and preferences from the AmigaOS partition, which
it mounts read-only and never writes to.

The screen is composed by the GPU when there is a card smolgpu can drive
(the Radeon in the A4000): every window is a texture, only the pixels that
change cross the bus, and moving a window costs no pixels at all.
Otherwise a software display composes it in RAM.

## Requirements

- Linux with a DRM display, or nothing at all for the memory display used
  in testing.  wbde was written on an A4000 with a 68060 and a Radeon HD
  2400 behind a PCI bridge, but nothing in it is specific to that machine
  except the hardware mouse fallback and the launcher script.
  - The GPU display needs a card smolgpu drives: an R600-class Radeon
    (smolrdn).  The GeForce backend (smolnvda) builds, but has no textures
    yet, so wbde falls back to the software display there.
  - The software display needs a DRM device smol2d can drive in 8-bit
    indexed colour (C8).
- The Kickstart ROM.  On an Amiga wbde reads it from `/dev/mem` at
  `0xf80000`.  Anywhere else give it an image of your own ROM, dumped from
  your Amiga or from a licensed copy such as Amiga Forever, with `--rom` or
  `WBDE_ROM`.  wbde needs topaz from it at least; the palette and the
  pointers come from it too.
- The AmigaOS System partition (Workbench 3.x), mounted at `/mnt/sys` or
  given with `--sys`.  It supplies the default icons (out of
  `Libs/icon.library`), drawer icons, `Prefs/Env-Archive/Sys` and the
  volume name.  Without it the disk icons are blank.
- On the A4000 the mouse: amimouse and the serial console (amiserial)
  both want the vertical blank interrupt without sharing it, so with the
  serial console open the mouse device cannot be opened.  wbde then reads
  the mouse hardware itself through `/dev/mem`; the keyboard mouse (below)
  works either way.
- To build: meson 1.1 or later, ninja, a C compiler, and the smol*
  libraries (below).  For the Amiga, an m68k cross compiler and, to run
  configure checks and m68k binaries on the build machine, qemu-m68k.

## Building

### The smol* libraries

wbde is built against these small libraries, used straight from their
source checkouts (nothing is installed).  They are found beside this
checkout, at `../smolinput` and so on, relative to the source directory;
`-Dsmolinput=DIR` (and the same for the others) points elsewhere.  The
branches and commits are the ones this version of wbde was built and
tested with:

| library | what for | needed by | branch | commit |
|---|---|---|---|---|
| smolinput | keyboard and mouse (evdev) | every build | `intern-wbde-20261124` | `30e0c03` |
| smol2d | the software display | `-Dsoft=true` (the default) | `intern-wbde-20261124` | `fd28723` |
| smoldrm | DRM/KMS | `-Dsoft=true`, or any `-Dgpu` but `none` | `master` | `838da14` |
| smolgpu | the GPU display | `-Dgpu=rdn` (the default) or `nvda` | `intern-sgpu-flip-20261125` | `0632d0c` |
| smolrdn | Radeon (R600-class) backend | `-Dgpu=rdn` | `intern-smolrdn-flip-20261125` | `ef7087e` |
| smolnvda | GeForce backend | `-Dgpu=nvda` | `intern-snv-wbdegpu-20261124` | `532ae3a` |
| smolgl | its fake Radeon, for testing | `-Dfake_gpu=true` | any recent | |

So the full Amiga build needs smolinput, smol2d, smoldrm, smolgpu and
smolrdn; a headless build (`-Dgpu=none -Dsoft=false`) needs only
smolinput.

### For the Amiga

Debian's cross compiler (`gcc-m68k-linux-gnu`) is enough:

    meson setup --cross-file cross/m68k-linux-gnu.ini build-m68k
    ninja -C build-m68k

A buildroot SDK works too, and is what to use for a 68060: its C library
and libgcc can be built for the 68060, whereas Debian's are built for the
68020 and use the 64-bit multiplies and divides that a 68060 emulates in
software.  Name the SDK in a machine file of your own, read first:

    printf "[constants]\nsdk = '%s'\n" /path/to/m68k-buildroot-linux-gnu_sdk-buildroot > sdk.ini
    meson setup --cross-file sdk.ini --cross-file cross/m68k-buildroot.ini build-m68k

The binaries are static and built for a 68040, which also runs on a 68060;
`-Dcpu=68060` builds for the 68060 instead.  Nothing in wbde needs the
060.

### On a PC

    meson setup build -Dgpu=none -Dsoft=false
    ninja -C build

gives wbde with only the memory display, for running scripts and taking
screenshots (`-Db_sanitize=address,undefined` is worth adding).  An m68k
build with the same options runs under qemu-m68k beside m68k clients
such as amigafmt; see Testing, below.

    meson setup build-fake -Dsoft=false -Dfake_gpu=true

runs the GPU display on smolgl's fake Radeon: there is no picture, but
every command stream is checked as the kernel would.

### Options

| option | default | |
|---|---|---|
| `cpu` | `68040` | m68k CPU to build for: `68040` or `68060` |
| `gpu` | `rdn` | the GPU display: `rdn` (Radeon), `nvda` (GeForce) or `none` |
| `soft` | `true` | the software display (smol2d) |
| `fake_gpu` | `false` | the GPU display on smolgl's fake Radeon (PC testing) |
| `smol2d`, `smoldrm`, `smolinput`, `smolgpu`, `smolrdn`, `smolnvda`, `smolgl` | `../<name>` | where each checkout is |

Debian's gcc 16 for m68k miscompiles a float that is live across a call
(it is spilled as 80-bit extended and reloaded as a 4-byte single).
`tools/floatcheck.c` reproduces it at configure time, and when it fails,
or cannot be run, the whole build gets `-ffloat-store`.

### Installing

    meson install -C build-m68k --destdir /some/staging

installs `wbde` and `wbde.sh` in the bindir, the client headers
(`wbde_proto.h`, `wbclient.h`) under `include/wbde`, and the documents.
`wbrom` (shows what wbde finds in a ROM) and `wbclient-demo` (an example
client) are built but not installed.

### Continuous integration

`.github/workflows/build.yml` builds the headless wbde for the PC and for
m68k on every push, and the full Amiga build for the 68040 and the 68060.
It checks the smol* libraries out from repository variables:
`SMOLINPUT_URL` for the headless build, and `SMOL2D_URL`, `SMOLDRM_URL`,
`SMOLGPU_URL` and `SMOLRDN_URL` as well for the Amiga build, which runs
only once `SMOLGPU_URL` is set.  `<NAME>_REF` overrides the commit in the
table above.

## Running

    wbde.sh [wbde options]          # stop with Workbench > Quit, or kill $(cat /tmp/wbde.pid)

`wbde.sh` is the launcher for the A4000 layout: it mounts the AmigaOS
partition (`/dev/sda1`) read-only on `/mnt/sys` and the Linux one
(`/dev/sda3`) on `/mnt/lnx`, registers amigafmt with binfmt_misc if it is
installed as `/mnt/lnx/amigafmt/amigafmt/amigafmt`, and starts wbde in the
background with its log in `/tmp/wbde.log`.  `WBDE_BIN` runs another wbde
binary.  Elsewhere run `wbde` directly.

### Options

| option | |
|---|---|
| `--display gpu\|soft\|file` | the display; default: the first of these that opens (`WBDE_DISPLAY`) |
| `--mode WxH` | the display mode; default the monitor's own (`WBDE_MODE`) |
| `--size WxH` | Workbench's size (`WBDE_SIZE`) |
| `--scale N` | Workbench's scale, any number on the GPU display (`WBDE_SCALE`) |
| `--rom FILE` | a Kickstart ROM image instead of the machine's (`WBDE_ROM`) |
| `--sys DIR` | where the AmigaOS System partition is mounted (`/mnt/sys`) |
| `--conf FILE` | volumes to show, lines of `volume NAME PATH` (default `/mnt/lnx/wbde/wbde.conf` if it exists); without it, the System partition, every mounted ext, FAT, ISO 9660 or AFFS volume, Ram Disk (`/tmp`) and Linux (`/`) |
| `--pattern ILBM` | an IFF picture tiled behind the disks |
| `--drawer-pattern ILBM` | the same, in drawer windows |
| `--socket PATH` | the client socket (`WBDE_SOCKET`, default `/tmp/wbde.sock`) |
| `--script FILE` | run control commands from a file, then exit |

`wbpattern.prefs`, `palette.prefs`, `pointer.ilbm` and `def_*.info` in
`Prefs/Env-Archive/Sys` are used when present.

### The display and its size

The GPU display drives the monitor at its preferred mode, and each screen
has a logical size of its own, shown at a uniform scale (square pixels,
nearest neighbour), centred with black round it.

Without `--size`, Workbench's size is the native mode divided by a whole
scale chosen from the monitor's physical size (EDID), so that a Workbench
pixel is about 0.45 mm, as on a classic Amiga or RTG monitor.  When the
monitor reports no usable size, the scale is the largest that keeps 480
lines or more.  The log says what it measured and chose.

    wbde.sh                          # automatic (a 14.1" 1920x1200 panel: 640x400 at 3x)
    wbde.sh --scale 2                # native / 2, e.g. 960x600 at 2x
    wbde.sh --size 640x480           # 640x480 at the largest whole scale, centred
    wbde.sh --size 960x600 --scale 1.5
    wbde.sh --mode 1280x1024         # another display mode

The software display does not scale: by default it runs at 640x480 with a
640x240 Workbench, each row shown twice, since scaling a large screen is
beyond the CPU.  `--mode` picks another mode there too, and a `--size` as
wide as the mode, with a whole fraction of its height, has its rows
repeated.  The memory display defaults to the same and takes `--mode` and
`--size` like the GPU display, for testing.

### Using it

It works as Workbench 3.1 does: double click a disk or drawer to open it,
drag icons about, lasso them, double click a tool or a project (through
its default tool, with `SYS:` mapped onto the System partition).  Menus
are on the right button, with Right Amiga shortcuts.  Workbench >
Execute Command runs a shell command, Icons > Information shows an icon's
tool types, Tools > Shell (Right Amiga + Z) opens a Linux shell in a
console window with AmigaShell's prompt, and a program's output opens a
console window of its own.  Nothing is ever written to a disk: icon
positions are not saved and the menu items that would write are ghosted.

The keyboard is grabbed while wbde runs, with the US keymap.  Intuition's
keyboard shortcuts:

| keys | |
|---|---|
| Left Amiga + cursor keys | move the pointer (with Shift, faster) |
| Left Amiga + Left Alt / Right Alt | the left / right mouse button |
| Left Amiga + M | the front screen to the back |
| Left Amiga + N | Workbench to the front |

### The control FIFO

wbde reads commands from the FIFO `/tmp/wbde-ctl` (`WBDE_CTL`), one a
line, and `--script` runs the same commands from a file with a clock of its
own.  Coordinates are the front screen's.

| command | |
|---|---|
| `move X Y` | move the pointer |
| `down B`, `up B` | press, release button B (1 left, 2 right, 3 middle) |
| `click X Y`, `dclick X Y` | click, double click the left button |
| `drag X Y X2 Y2` | drag with the left button |
| `menu X Y X2 Y2 [hold]` | open the menus at X Y and choose at X2 Y2 (`hold`: leave them open) |
| `key CODE [QUAL]` | press and release a Linux key code, with `QUAL_*` qualifier bits |
| `type TEXT` | type the text; `\|` is Return |
| `wait MS` | let time pass (scripts only) |
| `shot FILE` | write a screenshot (PPM) |
| `redraw` | redraw the whole screen |
| `screen back`, `screen wb` | the front screen to the back, Workbench to the front |
| `quit` | quit |

    echo 'dclick 30 50' > /tmp/wbde-ctl

Writing to the FIFO blocks while wbde is not running.  `kill -USR1` takes
a screenshot to `/tmp/wbde-shot.ppm`.  On the GPU display a screenshot is
read back from VRAM and compared with the CPU's composition of the same
screen, and the log says how many pixels differ.

### Environment

| variable | |
|---|---|
| `WBDE_KEYBOARD`, `WBDE_MOUSE` | the input devices to use; `WBDE_MOUSE=hw` reads the Amiga mouse hardware |
| `WBDE_GPU_CURSOR=0` | draw the pointer as a layer instead of the hardware cursor |
| `WBDE_GPU_FULL=1` | redraw the whole screen at every flush |
| `WBDE_GPU_SMOOTH=1` | filter buffers that clients attach, instead of nearest neighbour |
| `WBDE_GPU_FLIP=0` | compose into the scanned-out buffer instead of flipping between two at vblank |
| `WBDE_SCANLINE=ADDR` | log where the beam was during each GPU update; ADDR is the physical address of the Radeon's registers |
| `WBDE_TIMING=1` | log what every flush and drawer read costs |
| `WBDE_NOCRASHHANDLER=1` | no crash report with the program counter |
| `WBDE_NOQUIT=1` | Workbench > Quit does nothing (for fuzzing) |

## Amiga programs

amigafmt runs OS-friendly AmigaOS executables on Linux
through binfmt_misc, and opens their windows and screens on wbde through
the client protocol (`WBDE_SOCKET`).  With amigafmt registered, a double
click on an AmigaOS tool runs it: wbde passes the icon's `.info` and stack
size in `WBDE_ICON` and `WBDE_STACK`, from which amigafmt builds a
`WBStartup` message.  From a shell:

    WBDE_SOCKET=/tmp/wbde.sock amigafmt /mnt/sys/Utilities/Clock

Programs that open their own screens get them at the scale their display
mode asks for, fitted to the monitor; Left Amiga + M and N flip between
them.

## The client protocol

Programs of your own can use the protocol directly: windows with menus and
IDCMP-style events, screens of their own, pens, the ROM font, pointers,
GPU buffers shown as a window's content (flipped between, or unscaled at
the display's resolution), and console windows on an fd.  See
[doc/client-protocol.md](doc/client-protocol.md), `include/wbclient.h`
(a small client library in one header) and `tools/wbclient-demo.c`.

## Testing

- `--script FILE` runs control commands with a clock of their own and
  exits; `shot FILE` writes a PPM.  On a PC the memory display needs only
  a ROM image and, for icons, a copy of the System partition.
- `tools/fuzz.py SEED N` writes N random commands.  Run them on a host
  build with `-Db_sanitize=address,undefined` and `WBDE_NOQUIT=1`.
- An m68k client such as amigafmt speaks the protocol in its own byte
  order, so the server has to be m68k too: build with `-Dgpu=none
  -Dsoft=false` for m68k and start it with `tools/qemu-headless.sh`
  (`WBDE_BIN`, `WBDE_ROM`, `WBSYS`; socket, FIFO and log in
  `$WBDE_WORK`), then take screenshots with `tools/qemu-shot.sh NAME`.
- `-Dfake_gpu=true` runs the GPU display on smolgl's fake Radeon, which
  checks every command stream as the kernel's checker would and prints a
  summary on exit.
- On the machine, the control FIFO drives the running wbde, and a GPU
  screenshot reports how many pixels differ from the CPU's composition.

## Limits

- Nothing is written to disk: no Snapshot, Leave Out, Rename, Copy,
  Delete or New Drawer, and View By Date and Size are missing.
- No disk fonts: everything is in the ROM's topaz.  WBPattern pictures
  (as opposed to patterns) are not shown.
- The software display shows only the front screen, at 640x480; a screen
  opened lower down shows the screen behind only on the GPU display.
  Screens cannot be dragged down.
- The client protocol has no wbde-drawn requesters or gadgets, no
  clipboard and no AppWindows.
- On the GPU display every flush costs one command stream submission,
  about 3 ms of kernel time on the A4000 whatever it holds.
