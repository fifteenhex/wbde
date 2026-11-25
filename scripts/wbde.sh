#!/bin/sh
# Start wbde in the background, logging to /tmp/wbde.log.  The AmigaOS
# partition (sda1) is mounted read-only on /mnt/sys, the Linux one (sda3)
# on /mnt/lnx.  WBDE_BIN runs another wbde binary than the one beside this.
#   wbde.sh [wbde options]      stop: kill $(cat /tmp/wbde.pid)
D=$(dirname "$0")
mkdir -p /mnt/sys /mnt/lnx
grep -q " /mnt/sys " /proc/mounts || mount -t affs -o ro /dev/sda1 /mnt/sys
grep -q " /mnt/lnx " /proc/mounts || mount -t ext2 /dev/sda3 /mnt/lnx
if grep " /mnt/sys " /proc/mounts | grep -q " rw,"; then
	echo "wbde.sh: /mnt/sys is mounted read-write; refusing" >&2
	exit 1
fi
[ -f /tmp/wbde.pid ] && kill "$(cat /tmp/wbde.pid)" 2>/dev/null && sleep 1
rm -f /tmp/wbde-ctl
export HOME=${HOME:-/root}
setsid "${WBDE_BIN:-$D/wbde}" "$@" > /tmp/wbde.log 2>&1 < /dev/null &
echo $! > /tmp/wbde.pid
echo "wbde running, pid $(cat /tmp/wbde.pid)"
