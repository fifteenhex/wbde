#!/bin/sh
# qemu-headless.sh: an m68k wbde with only the memory display, under
# qemu-m68k, for m68k clients (amigafmt) to talk to on a PC in their own
# byte order.  The socket, the control FIFO (ctl) and the log go in
# $WBDE_WORK, default /tmp/wbde-work.
#   WBDE_BIN  the wbde binary (an m68k build with -Dgpu=none -Dsoft=false)
#   WBDE_ROM  a Kickstart ROM image
#   WBSYS     a copy of the AmigaOS System partition
W=${WBDE_WORK:-/tmp/wbde-work}
mkdir -p "$W"
[ -f "$W/wbde.pid" ] && kill "$(cat "$W/wbde.pid")" 2>/dev/null; sleep 1
rm -f "$W/wbde.sock"
WBDE_CTL=$W/ctl WBDE_NOCRASHHANDLER=1 setsid nohup qemu-m68k "${WBDE_BIN:?}" \
  --rom "${WBDE_ROM:?}" --sys "${WBSYS:?}" --conf /dev/null \
  --socket "$W/wbde.sock" > "$W/wbde.log" 2>&1 &
echo $! > "$W/wbde.pid"
sleep 4
