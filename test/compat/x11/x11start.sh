#!/bin/sh
# Runs an X11 program on the bwm desktop, from Alpine roots that hold Xvfb and
# the program:
#
#     x11start [-r SERVER_ROOT] [-c CLIENT_ROOT] [-g WxH] [-d N] [-b BX11] COMMAND [ARGS...]
#
# SERVER_ROOT (default /alpine) has Xvfb; CLIENT_ROOT (default SERVER_ROOT) has
# COMMAND. It starts Xvfb as display N (default 0) with a WxH screen (default
# 640x480), opens a bwm window for it with bx11 (default /disk/bx11) and runs
# COMMAND in CLIENT_ROOT with DISPLAY set. When COMMAND ends, so do the others.
# BusyBox (BB, default /disk/busybox) supplies mount, chroot and the rest.
# Run it from `bwm -c` or a terminal window; `bwm` must be running.
BB=${BB:-/disk/busybox}
SR=/alpine CR= G=640x480 N=0 BX=/disk/bx11
while [ $# -gt 0 ]; do
    case $1 in
    -r) SR=$2 ;; -c) CR=$2 ;; -g) G=$2 ;; -d) N=$2 ;; -b) BX=$2 ;;
    -*) echo "x11start: unknown option $1" >&2; exit 2 ;;
    *) break ;;
    esac
    shift 2
done
[ $# -gt 0 ] || { echo "usage: x11start [-r SERVER_ROOT] [-c CLIENT_ROOT] [-g WxH] [-d N] [-b BX11] COMMAND..." >&2; exit 2; }
CR=${CR:-$SR}
for r in "$SR" "$CR"; do
    $BB mkdir -p "$r/dev" "$r/proc" "$r/tmp/.X11-unix"
    [ -e "$r/dev/null" ] || $BB mount --bind /dev "$r/dev"
    [ -e "$r/proc/self" ] || $BB mount -t proc proc "$r/proc"
done
$BB mkdir -p "$SR/tmp/fb$N"
$BB chroot "$SR" /usr/bin/Xvfb ":$N" -screen 0 "${G}x24" -ac -fbdir "/tmp/fb$N" >/dev/null 2>&1 &
XP=$!
i=0
while [ ! -S "$SR/tmp/.X11-unix/X$N" ]; do
    i=$((i + 1))
    [ $i -le 60 ] || { echo "x11start: Xvfb did not start" >&2; $BB kill $XP; exit 1; }
    $BB sleep 1
done
if [ "$CR" != "$SR" ] && [ ! -S "$CR/tmp/.X11-unix/X$N" ]; then
    $BB mount --bind "$SR/tmp/.X11-unix" "$CR/tmp/.X11-unix"
fi
$BB mkdir -p /tmp/.X11-unix
[ -S "/tmp/.X11-unix/X$N" ] || $BB mount --bind "$SR/tmp/.X11-unix" /tmp/.X11-unix
"$BX" -f "$SR/tmp/fb$N/Xvfb_screen0" -s "/tmp/.X11-unix/X$N" -t "X11 :$N" &
BP=$!
$BB chroot "$CR" /usr/bin/env DISPLAY=":$N" "$@"
R=$?
$BB kill $BP $XP 2>/dev/null
exit $R
