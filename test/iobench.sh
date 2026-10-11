#!/usr/bin/env bash
# What disk I/O costs the rest of the machine: boots BastiOS on -smp 2 with
# an NVMe disk and runs, RUNS times each,
#   alone:   `bench` by itself (getpid and spawn+exit ticks);
#   beside:  `diskbench &` and then `bench`, so bench's system calls compete
#            with file calls for the kernel;
#   two:     two `diskbench &` at once, so the disk has two writers.
# DISK=usb uses a USB stick on an xHCI controller instead (a blank
# `tools/mkusb.py` image). Not a pass/fail test, and only comparable between runs on the same host
# (see test/diskbench.sh and test/smpbench.sh for why).
set -u
cd "$(dirname "$0")/.."
KERNEL=${KERNEL:-build/bastios.elf}
RUNS=${RUNS:-3}
DISK=${DISK:-nvme}

python3 - "$KERNEL" "$RUNS" "$DISK" <<'PY'
import os, re, select, subprocess, sys, tempfile, time

kernel, runs, kind = sys.argv[1], int(sys.argv[2]), sys.argv[3]
tmp = tempfile.TemporaryDirectory()
disk = os.path.join(tmp.name, "disk.img")
if kind == "usb":
    subprocess.run(["tools/mkusb.py", disk, "--blank", "--mib", "64"], check=True, stdout=subprocess.DEVNULL)
    drive = ["-device", "qemu-xhci,id=xh", "-drive", f"file={disk},format=raw,if=none,id=st",
             "-device", "usb-storage,bus=xh.0,drive=st"]
else:
    with open(disk, "wb") as f:
        f.truncate(64 << 20)
    drive = ["-drive", f"file={disk},format=raw,if=none,id=nv", "-device", "nvme,serial=bench,drive=nv"]
q = subprocess.Popen(
    ["qemu-system-x86_64", "-kernel", kernel, "-m", "256M", "-smp", "2", "-display", "none", "-no-reboot",
     "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04", "-nic", "none",
     "-serial", "stdio", "-monitor", "none"] + drive,
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
out = b""

def until(pred, timeout=300):
    global out
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

def run(cmds, want):
    """Types each command, waits until `want(text)` holds, returns the text."""
    mark = len(out)
    for c in cmds:
        m = len(out)
        q.stdin.write(c.encode() + b"\r"); q.stdin.flush()
        until(lambda o: b"bastios> " in o[m:])
    until(lambda o: want(o[mark:].decode(errors="replace")))
    return out[mark:].decode(errors="replace")

def nums(text, pat):
    return [int(x) for x in re.findall(pat, text)]

if not until(lambda o: b"bastios> " in o, 60):
    print("no prompt"); q.kill(); sys.exit(1)
GP, SP = r"getpid x\d+: (\d+) ticks", r"spawn\+exit x\d+: (\d+) ticks"
W, R = r"write 2 MiB: (\d+) ticks", r"read 2 MiB: (\d+) ticks"
for r in range(runs):
    t = run(["bench"], lambda s: len(re.findall(r"KiB: \d+ ticks", s)) >= 1)
    print(f"alone  run {r + 1}: bench getpid {nums(t, GP)}, spawn+exit {nums(t, SP)}")
for r in range(runs):
    t = run(["diskbench &", "bench"], lambda s: len(re.findall(r"KiB: \d+ ticks", s)) >= 1 and len(re.findall(R, s)) >= 1)
    print(f"beside run {r + 1}: bench getpid {nums(t, GP)}, spawn+exit {nums(t, SP)}; diskbench write {nums(t, W)}, read {nums(t, R)}")
for r in range(runs):
    t = run(["diskbench &", "diskbench &"], lambda s: len(re.findall(R, s)) >= 2)
    print(f"two    run {r + 1}: diskbench write {nums(t, W)}, read {nums(t, R)}")
q.kill(); q.wait()
PY
