#!/bin/sh
# usage: boot.sh ROOT
# Boots ROOT's init (OpenRC) as PID 1 of a new PID namespace, as a machine
# would: pivot_root into it, init runs the runlevels and then check.sh. Prints
# the lines of the console that check.sh and the shutdown wrote.
B=/disk/busybox
R=$1
$B unshare --pid --fork --mount-proc=$R/proc $B sh -c "mount --bind $R $R && cd $R && mkdir -p mnt && pivot_root . mnt && exec /sbin/init" > /tmp/boot.log 2>&1
$B grep -E '^check: |Requesting system' /tmp/boot.log
