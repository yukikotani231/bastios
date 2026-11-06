#!/usr/bin/env bash
# bx11 shares the clipboard with the X programs it shows: the text in /tmp/bwm.clip becomes the X CLIPBOARD and
# PRIMARY selections, and a selection an X client takes goes into /tmp/bwm.clip. This runs the real bx11 against a
# real Xvfb on the host, with fakebwm standing in for bwm, and xclip as the X client. It needs Xvfb and xclip.
set -u
cd "$(dirname "$0")/.."
for tool in Xvfb xclip cc; do
    command -v "$tool" >/dev/null || { echo "bx11 clipboard: $tool is missing"; exit 1; }
done
DISP=:97
work=$(mktemp -d)
pids=()
cleanup() {
    for p in "${pids[@]}"; do kill "$p" 2>/dev/null; done
    wait 2>/dev/null
    rm -rf "$work" /tmp/bwm.sock /tmp/bwm.clip /tmp/.X11-unix/X97 /tmp/.X97-lock
}
trap cleanup EXIT
cc -O2 -Wall -Wextra -Werror -Wno-unused-function -o "$work/bx11" user/bx11.c || exit 1
cc -O2 -Wno-unused-function -o "$work/fakebwm" test/compat/x11/fakebwm.c || exit 1
mkdir "$work/fb"
rm -f /tmp/bwm.clip /tmp/bwm.sock
Xvfb $DISP -screen 0 320x240x24 -fbdir "$work/fb" >"$work/xvfb.log" 2>&1 &
pids+=($!)
for _ in $(seq 50); do [ -S /tmp/.X11-unix/X97 ] && [ -f "$work/fb/Xvfb_screen0" ] && break; sleep 0.2; done
"$work/fakebwm" 60 "" >"$work/fakebwm.log" 2>&1 &
pids+=($!)
for _ in $(seq 50); do grep -q listening "$work/fakebwm.log" && break; sleep 0.1; done
"$work/bx11" -f "$work/fb/Xvfb_screen0" -s /tmp/.X11-unix/X97 >"$work/bx11.log" 2>&1 &
pids+=($!)
for _ in $(seq 50); do grep -q "bx11: window" "$work/bx11.log" && break; sleep 0.2; done
export DISPLAY=$DISP
fails=0
check() {   # NAME EXPECTED ACTUAL
    if [ "$2" = "$3" ]; then echo "ok   bx11 clipboard: $1"; else echo "FAIL bx11 clipboard: $1 (wanted '$2', got '$3')"; fails=$((fails + 1)); fi
}
# bwm to X: the file becomes both selections, and TARGETS lists the text types
printf 'from bwm' >/tmp/bwm.clip
sleep 1.5
check "the CLIPBOARD holds the text of the file" "from bwm" "$(timeout 5 xclip -o -selection clipboard)"
check "the PRIMARY selection holds it too" "from bwm" "$(timeout 5 xclip -o -selection primary)"
check "TARGETS names UTF8_STRING" "UTF8_STRING" "$(timeout 5 xclip -o -selection clipboard -t TARGETS | grep UTF8_STRING)"
printf 'caf\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac' >/tmp/bwm.clip
sleep 1.5
check "UTF-8 text arrives whole" "$(printf 'caf\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac')" "$(timeout 5 xclip -o -selection clipboard)"
# X to bwm: a copy in the X program ends up in the file
printf 'copied in X' | xclip -i -selection clipboard
sleep 1.5
check "a copy in X lands in the file" "copied in X" "$(cat /tmp/bwm.clip)"
printf 'selected in X' | xclip -i -selection primary
sleep 1.5
check "a PRIMARY selection in X lands in the file too" "selected in X" "$(cat /tmp/bwm.clip)"
# and back again: the file wins when it is written last, and X still reads it
printf 'second from bwm' >/tmp/bwm.clip
sleep 1.5
check "a later write of the file replaces the X owner" "second from bwm" "$(timeout 5 xclip -o -selection clipboard)"
check "bx11 is still running" "alive" "$(kill -0 "${pids[2]}" 2>/dev/null && echo alive)"
[ $fails -eq 0 ] && echo "bx11 clipboard: all passed" || { echo "bx11 clipboard: $fails failed"; cat "$work/bx11.log"; }
exit $((fails != 0))
