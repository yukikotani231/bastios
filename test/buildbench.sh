#!/usr/bin/env bash
# An end-to-end number: how long `make` takes to build a small C project on
# BastiOS with tcc, one job at a time and SMP at a time. Boots the kernel on
# -smp SMP (default 2) with the development disk (tcc and GNU make; `make build/compat/dev.img`
# is test/compat/devdisk.sh), generates FILES small sources, and times each
# `make` from the host, RUNS times, as a fresh copy of the project each time.
# Not part of `make test` and not a pass/fail test: it prints the median time
# of each and the speed-up of -j2 over -j1, and is only comparable between
# runs on the same host (see test/smpbench.sh).
#   KERNEL=build/bastios.elf DEV=build/compat/dev.img FILES=100 RUNS=3 SMP=2 test/buildbench.sh
set -u
cd "$(dirname "$0")/.."
KERNEL=${KERNEL:-build/bastios.elf}
DEV=${DEV:-build/compat/dev.img}
FILES=${FILES:-100}
SMP=${SMP:-2}
RUNS=${RUNS:-3}

python3 - "$KERNEL" "$DEV" "$FILES" "$RUNS" "$SMP" <<'PY'
import os, select, shutil, statistics, subprocess, sys, tempfile, time

kernel, dev, files, runs, smp = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
tmp = tempfile.TemporaryDirectory()
disk = os.path.join(tmp.name, "dev.img")
shutil.copyfile(dev, disk)
q = subprocess.Popen(
    ["qemu-system-x86_64", "-kernel", kernel, "-m", "512M", "-smp", smp, "-display", "none", "-no-reboot",
     "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04", "-nic", "none", "-serial", "stdio", "-monitor", "none",
     "-drive", f"file={disk},format=raw,if=none,id=vd", "-device", "virtio-blk-pci,drive=vd"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
out = b""

def until(marker, start, timeout):
    global out
    end = time.time() + timeout
    while marker not in out[start:]:
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

def sh(cmd, timeout=600):
    """Runs one ash line; returns (seconds, its output)."""
    mark = len(out)
    t0 = time.time()
    q.stdin.write(cmd.encode() + b"; echo @@do''ne $?\r"); q.stdin.flush()
    if not until(b"@@done ", mark, timeout):
        sys.exit("buildbench: timed out on: " + cmd[:60])
    t = time.time() - t0
    text = out[mark:].decode(errors="replace")
    return t, text

if not until(b"bastios> ", 0, 120):
    sys.exit("buildbench: no shell prompt")
mark = len(out)
q.stdin.write(b"ash\r"); q.stdin.flush()
if not until(b"# ", mark, 60) and not until(b"$ ", mark, 5):
    sys.exit("buildbench: ash did not start")

# The project: FILES sources of 40 small functions each, one header, a main.
# Short lines, built by a loop in ash (the console's line length is limited).
setup = [
    "mkdir /bb && cd /bb && printf 'int f0(int);\\n' > all.h",
    "i=0; while [ $i -lt %d ]; do j=0; { echo '#include \"all.h\"'; while [ $j -lt 40 ]; do echo \"int f${i}_$j(int x) { return x * $i + $j; }\"; j=$((j+1)); done; } > s$i.c; i=$((i+1)); done" % files,
    "printf '#include \"all.h\"\\nint main(void) { return 0; }\\n' > main.c",
    "O=; i=0; while [ $i -lt %d ]; do O=\"$O s$i.o\"; i=$((i+1)); done" % files,
    "printf \"OBJS =$O main.o\\nprog: \\$(OBJS)\\n\\t/disk/tcc -o \\$@ \\$(OBJS)\\n%%.o: %%.c all.h\\n\\t/disk/tcc -c -o \\$@ \\$<\\nclean:\\n\\trm -f \\$(OBJS) prog\\n\" > Makefile",
]
for line in setup:
    t, text = sh(line)
    if "@@done 0" not in text:
        sys.exit("buildbench: could not set up the project:\n" + text[-600:])

res = {}
for jobs in sorted({1, int(smp)}):
    ts = []
    for _ in range(runs):
        sh("/disk/make clean >/dev/null")
        t, text = sh("/disk/make -j%d >/dev/null" % jobs)
        if "@@done 0" not in text:
            sys.exit("buildbench: make -j%d failed:\n%s" % (jobs, text[-600:]))
        ts.append(t)
    res[jobs] = statistics.median(ts)
    print("make -j%d: %.2f s (runs: %s)" % (jobs, res[jobs], " ".join("%.2f" % x for x in ts)))
print("%d sources; -j%s is %.2fx as fast as -j1" % (files + 1, smp, res[1] / res[int(smp)]))
q.kill()
PY
