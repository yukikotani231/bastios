#!/usr/bin/env bash
# `make test`: every test below. Each boots its own QEMU with its own disk
# images and host ports, so tests can run side by side; they run in lanes,
# one test after another within a lane and the lanes at once:
#   a: the shell test, the longest, on 4 processors;
#   b: the TCP tests;
#   c: everything else (the two IPv6 tests share build/ipv6, so they stay
#      in one lane).
# Three lanes and not more: the TCP tests time a stalled wire against the
# guest's resend timer, and a test of the shell's checks that a process is
# asleep, so a machine much busier than this makes them fail for the wrong
# reason. With fewer than 4 CPUs, or JOBS=1, every test runs one after
# another, as before. The output of each test is printed whole, in the
# order below, followed by how long it took; the run fails if any test
# fails.
set -u
cd "$(dirname "$0")/.."
JOBS=${JOBS:-$( [ "$(nproc 2>/dev/null || echo 1)" -ge 4 ] && echo 3 || echo 1 )}
# name|lane|command, in the order they are printed
tests=(
  "trusted_base|c|test/trusted_base.sh"
  "fsck|c|test/fsck_test.py"
  "crashsim|c|test/crashsim.py --runs 1 --boots 8 --every 8"
  "shell|a|test/shell_test.sh"
  "guide|c|test/guide_test.py"
  "tcp virtio|b|test/tcp_test.sh"
  "tcp e1000|b|NIC=e1000 test/tcp_test.sh"
  "tcp igb|b|NIC=igb test/tcp_test.sh"
  "ipv6 virtio|c|test/ipv6_test.sh"
  "ipv6 e1000|c|NIC=e1000 test/ipv6_test.sh"
  "iso|c|test/iso_test.sh"
  "nmi|c|test/nmi_test.sh"
  "fuzz|c|test/fuzz_test.sh 1 2 2000"
)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
run() {  # index
  local cmd=${tests[$1]##*|} t0=$SECONDS
  bash -c "$cmd" >"$tmp/$1.out" 2>&1
  echo "$? $((SECONDS - t0))" >"$tmp/$1.status"
}
lane() {  # lane name: its tests, one after another
  local i rest
  for i in "${!tests[@]}"; do
    rest=${tests[$i]#*|}
    [ "${rest%%|*}" = "$1" ] && run "$i"
  done
}
t0=$SECONDS
if [ "$JOBS" = 1 ]; then
  for i in "${!tests[@]}"; do run "$i"; done
else
  lane a & lane b & lane c &
  wait
fi
failed=()
for i in "${!tests[@]}"; do
  name=${tests[$i]%%|*}
  printf '==== %s (%s)\n' "$name" "${tests[$i]##*|}"
  cat "$tmp/$i.out"
  read -r code secs <"$tmp/$i.status" || { code=lost secs=?; }
  printf '==== %s: exit %s, %s s\n\n' "$name" "$code" "$secs"
  [ "$code" = 0 ] || failed+=("$name")
done
if [ ${#failed[@]} -gt 0 ]; then
  echo "make test: FAILED: ${failed[*]} ($((SECONDS - t0)) s, JOBS=$JOBS)"
  exit 1
fi
echo "make test: all ${#tests[@]} tests passed ($((SECONDS - t0)) s, JOBS=$JOBS)"
