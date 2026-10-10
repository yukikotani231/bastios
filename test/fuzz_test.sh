#!/usr/bin/env bash
# Fuzz the system calls: boot BastiOS on two processors with a disk and a
# network card, run user/sysfuzz once per seed from BusyBox's ash (the
# native shell passes no arguments), and check
# that the kernel survives every run and gives every page back at power off.
#
# A run fails when the kernel reports a fault, when the shell stops
# answering (a run that is still going at its deadline is interrupted with
# Ctrl-C, and the shell must come back), or when the page count at power
# off differs from the one at boot. The fuzzer itself may fault or be
# killed: that only shows the kernel isolating it.
#
# usage: test/fuzz_test.sh [FIRST_SEED [SEEDS [CALLS]]]
#   FIRST_SEED  default 1          SEEDS  default 8 (seeds FIRST_SEED...)
#   CALLS       calls per run, default 3000
#   PAR=n       fuzzers running at once, default 2 (SMP=n: processors, default 2)
#   ELFRUNS=n   hostile executables to exec afterwards, default 100 (0: none)
# To replay one run with every call printed: `ash`, then `sysfuzz SEED CALLS v`
# (`make run`).
set -u
cd "$(dirname "$0")/.."
KERNEL=${KERNEL:-build/bastios.elf}
SMP=${SMP:-2}

python3 - "$KERNEL" "$SMP" "${1:-1}" "${2:-8}" "${3:-3000}" "${PAR:-2}" "${ELFRUNS:-100}" <<'PY'
import os, re, select, subprocess, sys, tempfile, time

kernel, smp, first, seeds, calls, par = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6])
elfruns = int(sys.argv[7])
tmp = tempfile.TemporaryDirectory()
disk = os.path.join(tmp.name, "disk.img")
with open(disk, "wb") as f:
    f.truncate(33 << 20)

qemu = subprocess.Popen(
    ["qemu-system-x86_64", "-kernel", kernel, "-m", "256M", "-smp", str(smp),
     "-display", "none", "-no-reboot", "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
     "-serial", "stdio", "-monitor", "none",
     "-drive", f"file={disk},format=raw,if=none,id=vd", "-device", "virtio-blk-pci,drive=vd",
     "-netdev", "user,id=n0", "-device", "virtio-net-pci,netdev=n0"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
out = b""

def read_until(marker, start, secs):
    global out
    end = time.time() + secs
    while marker not in out[start:]:
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
    print("FAIL", what)

if not read_until(b"bastios> ", 0, 60):
    print(out.decode(errors="replace")); print("FAIL boot"); sys.exit(1)
m = re.search(rb"(\d+) pages of memory", out)
boot_pages = int(m.group(1)) if m else None
mark = len(out)
# ash points a background job's input at /dev/null: give it an empty file.
send(b"ash\r")
if not read_until(b"\n# ", mark, 30):
    print(out.decode(errors="replace")); print("FAIL ash"); sys.exit(1)
PROMPT = b"\n# "
mark = len(out)
send(b"mkdir /dev; : > /dev/null\r")
read_until(PROMPT, mark, 30)

alive = True
# PAR fuzzers at once (seeds seed, seed+1, ...), so the processors run
# system calls against each other.
for seed in range(first, first + seeds, par):
    group = list(range(seed, min(seed + par, first + seeds)))
    mark = len(out)
    # The last one runs in the foreground. ash's `wait` needs rt_sigsuspend,
    # which BastiOS lacks, so the others are waited for by their last line.
    send(("".join(f"sysfuzz {s} {calls} & " for s in group[:-1])
          + f"sysfuzz {group[-1]} {calls}\r").encode())
    finished = read_until(PROMPT, mark, 120)
    end = time.time() + 60
    while finished and time.time() < end and len(re.findall(rb"made \d+ calls|\(sysfuzz\): [^\]]*killed\]", out[mark:])) < len(group):
        read_until(b"made ", len(out), end - time.time())
    if not finished:
        send(b"\x03")
        finished = read_until(PROMPT, mark, 20)
        if finished:
            print(f"note seeds {group}: still running at the deadline, interrupted")
    text = out[mark:]
    made = re.findall(rb"seed (\d+) made (\d+) calls", text)
    killed = re.findall(rb"\[pid \d+ \(sysfuzz\): ([^\]]*)\]", text)
    if b"kernel fault" in text or b"bastio: out of memory" in text:
        fail(f"seeds {group}: the kernel stopped")
        alive = False
        break
    if not finished:
        fail(f"seeds {group}: the shell stopped answering")
        alive = False
        break
    how = ", ".join(f"seed {a.decode()} {b.decode()} calls" for a, b in made)
    how += "".join(f"; a fuzzer was killed: {k.decode()}" for k in killed)
    print(f"ok   {how or 'every fuzzer ended early'}")

# Hostile executables (sysfuzz's ELF mode): ELFRUNS damaged images, each
# exec'd by a child that is killed if it runs on.
if alive and elfruns:
    mark = len(out)
    send(f"sysfuzz e {first} {elfruns}\r".encode())
    read_until(PROMPT, mark, 30 + elfruns)
    text = out[mark:]
    m = re.search(rb"elf seed \d+ ran \d+ images: [^\r\n]*", text)
    if b"kernel fault" in text or b"bastio: out of memory" in text:
        fail("hostile executables: the kernel stopped")
        alive = False
    elif not m or not read_until(PROMPT, mark, 20):
        fail("hostile executables: the run did not finish")
        alive = False
    else:
        print(f"ok   {m.group(0).decode()}")

if alive:
    mark = len(out)
    send(b"exit\r")
    if not read_until(b"bastios> ", mark, 30):
        fail("leaving ash")
    mark = len(out)
    send(b"halt\r")
    m = None
    if read_until(b"pages free at power off", mark, 60):
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
