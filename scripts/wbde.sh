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
# AmigaOS programs: amigafmt through binfmt_misc, with SYS: on /mnt/sys
AF=/mnt/lnx/amigafmt/amigafmt/amigafmt
if [ -x "$AF" ]; then
	B=/proc/sys/fs/binfmt_misc
	[ -e $B/register ] || mount -t binfmt_misc binfmt_misc $B 2>/dev/null
	if [ -e $B/register ] && [ ! -e $B/amigahunk ]; then
		printf '%s' ":amigahunk:M::\\x00\\x00\\x03\\xf3::$AF:F" > $B/register
	fi
	[ -f /mnt/lnx/amigafmt/amigafmt.conf ] &&
		export AMIGAFMT_CONF=/mnt/lnx/amigafmt/amigafmt.conf
fi
[ -f /tmp/wbde.pid ] && kill "$(cat /tmp/wbde.pid)" 2>/dev/null && sleep 1
rm -f /tmp/wbde-ctl
export HOME=${HOME:-/root}
setsid "${WBDE_BIN:-$D/wbde}" "$@" > /tmp/wbde.log 2>&1 < /dev/null &
echo $! > /tmp/wbde.pid
echo "wbde running, pid $(cat /tmp/wbde.pid)"
