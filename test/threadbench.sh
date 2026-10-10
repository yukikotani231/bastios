#!/usr/bin/env bash
# Whether the threads of one process run in parallel: boots BastiOS on 1
# and on SMP processors and runs `mthread par` (four threads of pure
# computation, user/mthread.c) from BusyBox's ash, printing the time it
# reports. Not a pass/fail test; only comparable on the same host.
set -u
cd "$(dirname "$0")/.."
KERNEL=${KERNEL:-build/bastios.elf}
SMP=${SMP:-4}
# MODE=clock times `mthread clock` (four threads reading the clock) instead.
MODE=${MODE:-par}

python3 - "$KERNEL" "$SMP" "$MODE" <<'PY'
import re, subprocess, sys, time, select

kernel, smp, mode = sys.argv[1], int(sys.argv[2]), sys.argv[3]

def run(cpus):
    q = subprocess.Popen(
        ["qemu-system-x86_64", "-kernel", kernel, "-m", "64M", "-smp", str(cpus),
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
        q.stdin.write(b"mthread " + mode.encode() + b"\r"); q.stdin.flush()
        m = until(rb"4 threads (\d+) ms", 600)
        return int(m.group(1)) if m else None
    finally:
        q.kill(); q.wait()

for cpus in sorted({1, smp}):
    ms = run(cpus)
    print(f"-smp {cpus}: mthread {mode}: " + (f"{ms} ms" if ms is not None else "did not finish"))
PY
