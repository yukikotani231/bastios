#!/usr/bin/env bash
# How fast processes are made: boots BastiOS on 1 and on SMP processors and,
# from BusyBox's ash, runs COPIES loops at once, each starting PROG ITERS
# times (a fork of ash and an exec of PROG, both under the kernel lock),
# printing the seconds the host saw them take. Not a pass/fail test; only
# comparable on the same host.
set -u
cd "$(dirname "$0")/.."
KERNEL=${KERNEL:-build/bastios.elf}
SMP=${SMP:-4}
COPIES=${COPIES:-4}
ITERS=${ITERS:-25}
PROG=${PROG:-/bin/mhello}

python3 - "$KERNEL" "$SMP" "$COPIES" "$ITERS" "$PROG" <<'PY'
import re, subprocess, sys, time, select

kernel, smp, copies, iters, prog = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), sys.argv[5]

def run(cpus):
    q = subprocess.Popen(
        ["qemu-system-x86_64", "-kernel", kernel, "-m", "128M", "-smp", str(cpus),
         "-display", "none", "-no-reboot", "-nic", "none",
         "-serial", "stdio", "-monitor", "none"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = b""
    def until(pat, timeout):
        nonlocal out
        end = time.time() + timeout
        while not re.search(pat, out):
            left = end - time.time()
            if left <= 0:
                return None
            r, _, _ = select.select([q.stdout], [], [], left)
            if not r:
                return None
            chunk = q.stdout.read1(65536)
            if not chunk:
                return None
            out += chunk
        return re.search(pat, out)
    try:
        if not until(rb"bastios> ", 60):
            return None
        q.stdin.write(b"ash\r"); q.stdin.flush()
        if not until(rb"\n# ", 60):
            return None
        loop = f"i=0; while [ $i -lt {iters} ]; do {prog} >/dev/null; i=$((i+1)); done"
        cmd = " ".join(f"({loop}) &" for _ in range(copies)) + " wait; echo fork''bench done"
        start = time.time()
        q.stdin.write(cmd.encode() + b"\r"); q.stdin.flush()
        if not until(rb"forkbench done", 900):
            return None
        return time.time() - start
    finally:
        q.kill(); q.wait()

for cpus in sorted({1, smp}):
    t = run(cpus)
    print(f"-smp {cpus}: {copies} x {iters} x {prog}: " + (f"{t:.1f} s" if t is not None else "did not finish"))
PY
