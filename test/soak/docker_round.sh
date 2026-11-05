# Run by test/soak.py in BastiOS's ash after dockerd is up in /disk/p (see
# test/compat/docker.txt): rounds of containers, in the format msoak prints.
# $1: the number of rounds.
BB=/disk/busybox
fail() { echo "soak: FAIL $*"; exit 1; }
dc() { $BB chroot /disk/p /usr/local/bin/docker "$@"; }
sample() {
  free=$($BB sed -n 's/^MemFree: *\([0-9]*\).*/\1/p' /proc/meminfo)
  cached=$($BB sed -n 's/^Cached: *\([0-9]*\).*/\1/p' /proc/meminfo)
  fds=$($BB ls /proc/self/fd | $BB wc -l)
  procs=$($BB ls /proc | $BB grep -c '^[0-9]')
}
$BB tar -cf /disk/p/tmp/ctr.tar -C /disk/p bin lib || fail "packing the image"
dc import /tmp/ctr.tar ctr >/dev/null 2>&1 || fail "docker import"
n=1
while [ $n -le $1 ]; do
  out=$(dc run --rm --network=none ctr echo hello 2>&1) || fail "run (round $n): $out"
  [ "$out" = hello ] || fail "a container printed '$out' (round $n)"
  dc run --rm --network=none ctr sh -c 'exit 3' >/dev/null 2>&1; [ $? -eq 3 ] || fail "exit status (round $n)"
  # a detached container, its log, its removal
  o=$(dc run -d --network=none --name lg$n ctr sh -c 'echo logged' 2>&1) || fail "run -d (round $n): $(echo "$o" | $BB tr '\n' '|' | $BB cut -c1-300)"
  dc wait lg$n >/dev/null 2>&1
  out=$(dc logs lg$n 2>&1); [ "$out" = logged ] || fail "log '$out' (round $n)"
  dc rm lg$n >/dev/null 2>&1 || fail "rm (round $n)"
  # a memory limit and several containers at once
  out=$(dc run --rm --network=none --memory 64m ctr cat /sys/fs/cgroup/memory.max 2>&1)
  [ "$out" = 67108864 ] || fail "memory.max '$out' (round $n)"
  # Three at once: run them one after another unless DSOAK_PAR=1 (under
  # emulation without KVM the client's wait for a container then sometimes
  # fails with "unexpected EOF"; judge that on a KVM host).
  for i in 1 2 3; do
    $BB rm -f /tmp/par$i.out
    if [ "$DSOAK_PAR" = 1 ]; then dc run --rm --network=none ctr echo par$i >/tmp/par$i.out 2>&1 &
    else dc run --rm --network=none ctr echo par$i >/tmp/par$i.out 2>&1; fi
  done
  wait
  t=0
  while [ $t -lt 60 ] && [ "$($BB cat /tmp/par1.out /tmp/par2.out /tmp/par3.out | $BB wc -l)" -lt 3 ]; do $BB sleep 1; t=$((t + 1)); done
  for i in 1 2 3; do o=$($BB cat /tmp/par$i.out); [ "$o" = par$i ] || fail "parallel $i printed '$(echo "$o" | $BB tr "\n" "|" | $BB cut -c1-300)' after wait + ${t}s (round $n); ps: $($BB ps | $BB grep -c docker)"; done
  # on the bridge
  raw=$(dc run --rm ctr /bin/busybox ping -c 1 -W 5 172.17.0.1 2>&1)
  out=$(echo "$raw" | $BB grep -c '1 packets received')
  [ "$out" = 1 ] || fail "bridge ping (round $n): $(echo "$raw" | $BB tr '\n' '|' | $BB cut -c1-300)"
  [ -z "$(dc ps -aq 2>/dev/null)" ] || fail "containers left over (round $n)"
  sample
  echo "soak: round $n ok ops=$((n * 8)) free=$free cached=$cached fds=$fds procs=$procs"
  n=$((n + 1))
done
