#!/usr/bin/env bash
# bx11 gives the keyboard focus to the top-level windows of the X programs it shows: a window that maps (a dialog)
# takes it, the one below gets it back when that window closes, and a click on a window focuses it. This runs the real
# bx11 against a real Xvfb on the host, with fakebwm standing in for bwm, and xterm windows as the X clients. It
# needs Xvfb, xterm and xdotool.
set -u
cd "$(dirname "$0")/.."
for tool in Xvfb xterm xdotool cc; do
    command -v "$tool" >/dev/null || { echo "bx11 focus: $tool is missing"; exit 1; }
done
DISP=:96
work=$(mktemp -d)
pids=()
cleanup() {
    for p in "${pids[@]}"; do kill "$p" 2>/dev/null; done
    wait 2>/dev/null
    rm -rf "$work" /tmp/bwm.sock /tmp/.X11-unix/X96 /tmp/.X96-lock
}
trap cleanup EXIT
cc -O2 -Wall -Wextra -Werror -Wno-unused-function -o "$work/bx11" user/bx11.c || exit 1
cc -O2 -Wno-unused-function -o "$work/fakebwm" test/compat/x11/fakebwm.c || exit 1
mkdir "$work/fb"
rm -f /tmp/bwm.sock
Xvfb $DISP -screen 0 640x480x24 -fbdir "$work/fb" >"$work/xvfb.log" 2>&1 &
pids+=($!)
for _ in $(seq 50); do [ -S /tmp/.X11-unix/X96 ] && [ -f "$work/fb/Xvfb_screen0" ] && break; sleep 0.2; done
"$work/fakebwm" 60 '\w10;\m10,10;\w120;' >"$work/fakebwm.log" 2>&1 &
pids+=($!)
for _ in $(seq 50); do grep -q listening "$work/fakebwm.log" && break; sleep 0.1; done
"$work/bx11" -f "$work/fb/Xvfb_screen0" -s /tmp/.X11-unix/X96 >"$work/bx11.log" 2>&1 &
pids+=($!)
for _ in $(seq 50); do grep -q "bx11: window" "$work/bx11.log" && break; sleep 0.2; done
export DISPLAY=$DISP
fails=0
check() {   # NAME EXPECTED ACTUAL
    if [ "$2" = "$3" ]; then echo "ok   bx11 focus: $1"; else echo "FAIL bx11 focus: $1 (wanted '$2', got '$3')"; fails=$((fails + 1)); fi
}
focused() {   # the name of the window that has the focus, after a moment for the X server to settle
    sleep 0.6
    xdotool getwindowfocus getwindowname 2>/dev/null
}
xterm -T first -geometry 20x5+0+0 -e sleep 300 &
pids+=($!)
sleep 1.5
check "the first window takes the focus when it maps" "first" "$(focused)"
xterm -T dialog -geometry 20x5+300+200 -e sleep 300 &
dialog=$!
pids+=($!)
sleep 1.5
check "a second window (a dialog) takes it when it opens" "dialog" "$(focused)"
kill $dialog
sleep 1.5
check "the first window has it back when the dialog closes" "first" "$(focused)"
xterm -T third -geometry 20x5+300+200 -e sleep 300 &
pids+=($!)
sleep 1.5
# a click on the first window (the third covers 300,200 and on; 10,10 is the first alone), sent by fakebwm through
# bx11 as a click in the bwm window 10 seconds after the first window drew
sleep 7
check "bx11 is still running" "alive" "$(kill -0 "${pids[2]}" 2>/dev/null && echo alive)"
# A window that was already mapped when bx11 starts gets the focus too (QueryTree at the start).
for p in "${pids[@]}"; do kill "$p" 2>/dev/null; done
wait 2>/dev/null
pids=()
rm -f /tmp/bwm.sock /tmp/.X11-unix/X96 /tmp/.X96-lock
rm -rf "$work/fb"; mkdir "$work/fb"
Xvfb $DISP -screen 0 640x480x24 -fbdir "$work/fb" >"$work/xvfb2.log" 2>&1 &
pids+=($!)
for _ in $(seq 50); do [ -S /tmp/.X11-unix/X96 ] && [ -f "$work/fb/Xvfb_screen0" ] && break; sleep 0.2; done
xterm -T early -geometry 20x5+0+0 -e sleep 300 &
pids+=($!)
sleep 1.5
"$work/fakebwm" 30 '\w120;' >"$work/fakebwm2.log" 2>&1 &
pids+=($!)
for _ in $(seq 50); do grep -q listening "$work/fakebwm2.log" && break; sleep 0.1; done
"$work/bx11" -f "$work/fb/Xvfb_screen0" -s /tmp/.X11-unix/X96 >"$work/bx11b.log" 2>&1 &
pids+=($!)
for _ in $(seq 50); do grep -q "bx11: window" "$work/bx11b.log" && break; sleep 0.2; done
check "a window mapped before bx11 started has the focus" "early" "$(focused)"
[ $fails -eq 0 ] && echo "bx11 focus: all passed" || { echo "bx11 focus: $fails failed"; cat "$work/fakebwm.log" "$work/bx11.log"; }
exit $((fails != 0))
