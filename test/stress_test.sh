#!/usr/bin/env bash
# Concurrency stress: boot BastiOS on several processors with user/stress
# on the disk, run it once per seed from BusyBox's ash, and check that every
# run passes its own checks (it prints "stress: ok"), that the kernel
# survives (no kernel fault, the shell answers), and that every page comes
# back at power off.
#
# usage: test/stress_test.sh [FIRST_SEED [SEEDS [SECONDS]]]
#   FIRST_SEED default 1, SEEDS default 4 (seeds FIRST_SEED...), SECONDS per
#   run default 60. PROCS (default 4) processes of THREADS (default 3)
#   threads each; SMP processors (default 4); MEM (default 512M).
# A failing seed is replayed with `/disk/stress SEED SECONDS PROCS THREADS`
# in ash (make run with that disk: make disk FILES=stress=build/user/stress.elf).
set -u
cd "$(dirname "$0")/.."
KERNEL=${KERNEL:-build/bastios.elf}
PROG=${PROG:-build/user/stress.elf}

python3 - "$KERNEL" "$PROG" "${SMP:-4}" "${MEM:-512M}" "${1:-1}" "${2:-4}" "${3:-60}" "${PROCS:-4}" "${THREADS:-3}" <<'PY'
import os, re, select, subprocess, sys, tempfile, time

kernel, prog, smp, mem = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
first, seeds, secs, procs, threads = (int(x) for x in sys.argv[5:10])
tmp = tempfile.TemporaryDirectory()
disk = os.path.join(tmp.name, "disk.img")
subprocess.run([sys.executable, "test/compat/mkdisk.py", disk, f"stress={prog}"], check=True)
with open(disk, "r+b") as f:
    f.truncate(64 << 20)

qemu = subprocess.Popen(
    ["qemu-system-x86_64", "-kernel", kernel, "-m", mem, "-smp", smp,
     "-display", "none", "-no-reboot", "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
     "-serial", "stdio", "-monitor", "none",
     "-drive", f"file={disk},format=raw,if=none,id=vd", "-device", "virtio-blk-pci,drive=vd"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
out = b""

def read_until(pattern, start, secs):
    global out
    end = time.time() + secs
    while not re.search(pattern, out[start:]):
        left = end - time.time()
        if left <= 0:
            return False
        r, _, _ = select.select([qemu.stdout], [], [], left)
        if not r:
            return False
        chunk = qemu.stdout.read1(65536)
        if not chunk:
            return False
        out += chunk
    return True

def send(b):
    qemu.stdin.write(b); qemu.stdin.flush()

fails = 0
def fail(what):
    global fails
    fails += 1
    print("FAIL", what, flush=True)

if not read_until(rb"bastios> ", 0, 120):
    print(out.decode(errors="replace")); print("FAIL boot"); sys.exit(1)
m = re.search(rb"(\d+) pages of memory", out)
boot_pages = int(m.group(1)) if m else None
mark = len(out)
send(b"ash\r")
if not read_until(rb"\n# ", mark, 60):
    print(out.decode(errors="replace")); print("FAIL ash"); sys.exit(1)
PROMPT = rb"\n# "

alive = True
for seed in range(first, first + seeds):
    mark = len(out)
    send(f"/disk/stress {seed} {secs} {procs} {threads}\r".encode())
    # Under emulation a run ends well after its SECONDS: the last operations
    # finish, and every worker is waited for.
    finished = read_until(rb"stress: per operation[^\n]*\n(?:[\s\S]*\n)?# ", mark, secs + 300)
    if not finished:
        send(b"\x03")
        finished = read_until(PROMPT, len(out), 30)
    text = out[mark:]
    if b"kernel fault" in text or b"bastio: out of memory" in text:
        fail(f"seed {seed}: the kernel stopped")
        alive = False
        break
    if not finished:
        fail(f"seed {seed}: the shell stopped answering")
        alive = False
        break
    lines = [l.decode(errors="replace").strip() for l in text.split(b"\n") if b"stress:" in l]
    ok = [l for l in lines if l.startswith("stress: ok")]
    if not ok or any("FAIL" in l for l in lines):
        fail(f"seed {seed}: " + " | ".join(lines or ["no result"]))
    else:
        print("ok  ", ok[0][len("stress: ok "):], flush=True)

if alive:
    mark = len(out)
    send(b"exit\r")
    if not read_until(rb"bastios> ", mark, 30):
        fail("leaving ash")
    mark = len(out)
    send(b"halt\r")
    m = None
    if read_until(rb"pages free at power off", mark, 120):
        m = re.search(rb"(\d+) pages free at power off", out[mark:])
    if not m:
        fail("power off")
    elif boot_pages is None or int(m.group(1)) != boot_pages:
        fail(f"pages: {boot_pages} at boot, {m.group(1).decode()} at power off")
    else:
        print(f"ok   every page back at power off ({boot_pages})")
try:
    qemu.wait(timeout=10)
except subprocess.TimeoutExpired:
    qemu.kill(); qemu.wait()
if fails:
    print("---- transcript (last 6000 bytes) ----")
    print(out[-6000:].decode(errors="replace").replace("\r", ""))
print(f"---- {fails} failed ----")
sys.exit(1 if fails else 0)
PY
