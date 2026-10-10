#!/usr/bin/env bash
# How the kernel scales across processors: boots BastiOS on 1 and on SMP
# processors and runs N copies of `bench` at once on each, printing the
# wall-clock time and each copy's timings. Not a pass/fail test, and only
# comparable between runs on the same host: under QEMU's TCG (no KVM) the
# numbers include emulator costs real hardware does not have, notably a
# slow `pause` in the lock's spin loop.
set -u
cd "$(dirname "$0")/.."
KERNEL=${KERNEL:-build/bastios.elf}
SMP=${SMP:-4}
N=${N:-4}

python3 - "$KERNEL" "$SMP" "$N" <<'PY'
import re, subprocess, sys, time, select

kernel, smp, n = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])

def run(cpus):
    q = subprocess.Popen(
        ["qemu-system-x86_64", "-kernel", kernel, "-m", "64M", "-smp", str(cpus),
         "-display", "none", "-no-reboot", "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
         "-serial", "stdio", "-monitor", "none"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = b""
    def until(pred, timeout):
        nonlocal out
        end = time.time() + timeout
        while not pred(out):
            left = end - time.time()
            if left <= 0:
                return False
            r, _, _ = select.select([q.stdout], [], [], left)
            if not r:
                return False
            chunk = q.stdout.read1(65536)
            if not chunk:
                return False
            out += chunk
        return True
    try:
        if not until(lambda o: b"bastios> " in o, 60):
            return None, b""
        mark = len(out)
        t0 = time.time()
        for i in range(n):
            q.stdin.write(b"bench &\r"); q.stdin.flush()
            until(lambda o: o[mark:].count(b"bastios> ") >= i + 1, 60)
        # Each copy prints three lines ending in "ticks".
        ok = until(lambda o: o[mark:].count(b"ticks") >= 3 * n, 900)
        return (time.time() - t0 if ok else None), out[mark:]
    finally:
        q.kill(); q.wait()

for cpus in sorted({1, smp}):
    wall, out = run(cpus)
    if wall is None:
        print(f"-smp {cpus}: bench did not finish")
        continue
    lines = re.findall(rb"(getpid x\d+|spawn\+exit x\d+|write \d+ KiB): (\d+) ticks", out)
    per = {}
    for what, t in lines:
        per.setdefault(what.decode(), []).append(int(t))
    detail = "; ".join(f"{w} {'/'.join(map(str, ts))}" for w, ts in per.items())
    print(f"-smp {cpus}, {n} x bench at once: {wall:.1f} s wall ({detail} ticks)")
PY
