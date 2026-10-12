#!/usr/bin/env bash
# Boot the AArch64 port (make ARCH=aarch64) on QEMU's virt machine, drive the
# shell over the serial line and check its replies: the timer interrupt
# (uptime, and a program that never makes a system call being preempted),
# user programs at EL0, process creation, waiting and killing, a fault in
# a user program, and a power off through PSCI that finds every page back
# in the pool. Input is sent one line at a time, each after the prompt
# appears: bytes sent before the console is read are dropped.
#
# usage: test/aarch64_test.sh            test the kernel `make ARCH=aarch64` built
#        test/aarch64_test.sh --build    build it first (what `make test` runs):
#                                        skipped, with a notice, on a machine
#                                        without qemu-system-aarch64 or an
#                                        AArch64 C compiler (AARCH64_REQUIRED=1
#                                        makes that a failure, as in CI)
set -u
cd "$(dirname "$0")/.."
KERNEL=${KERNEL:-build/aarch64/bastios.bin}
QEMU=${QEMU:-qemu-system-aarch64}
TIMEOUT=${TIMEOUT:-60}
if [ "${1:-}" = --build ]; then
  cc=aarch64-linux-gnu-gcc
  [ "$(uname -m)" = aarch64 ] && cc=gcc
  if ! command -v "$QEMU" >/dev/null || ! command -v "$cc" >/dev/null; then
    echo "SKIP aarch64: needs $QEMU and $cc (Debian/Ubuntu: qemu-system-arm gcc-aarch64-linux-gnu)"
    [ "${AARCH64_REQUIRED:-}" = 1 ] && exit 1
    exit 0
  fi
  # bastioc1 is the only compiler with the AArch64 targets: use the one
  # the Makefile builds with stage0 (BASTIOC may name bastioc0 for the x86-64 tests).
  make ARCH=aarch64 BASTIOC=build/bastioc1 -s all || exit 1
fi

python3 - "$KERNEL" "$QEMU" "$TIMEOUT" <<'PY'
import os, re, select, subprocess, sys, tempfile, time

# A blank disk (sparse): BastiOS formats it on first use.
disk_dir = tempfile.TemporaryDirectory()
disk = os.path.join(disk_dir.name, "disk.img")
with open(disk, "wb") as f:
    f.truncate(32 * 1024 * 1024)

kernel, qemu_bin, timeout = sys.argv[1], sys.argv[2], float(sys.argv[3])
qemu = subprocess.Popen(
    [qemu_bin, "-M", "virt,gic-version=" + os.environ.get("GIC", "3") + os.environ.get("VIRT", ""), "-cpu", os.environ.get("CPU", "cortex-a72"), "-smp", os.environ.get("SMP", "4"), "-m", "256M", "-nographic",
     "-no-reboot", "-kernel", kernel,
     "-drive", f"file={disk},format=raw,if=none,id=vd", "-device", "virtio-blk-device,drive=vd",
     "-netdev", "user,id=n0,net=10.0.2.0/24", "-device", "virtio-net-device,netdev=n0", "-serial", "stdio", "-monitor", "none"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
deadline = time.time() + timeout
out = b""
fails = 0

def read_until(marker, start=0):
    global out
    while marker not in out[start:]:
        left = deadline - time.time()
        if left <= 0:
            return False
        r, _, _ = select.select([qemu.stdout], [], [], left)
        if not r:
            return False
        chunk = qemu.stdout.read1(4096)
        if not chunk:
            return False
        out += chunk
    return True

def check(name, good):
    global fails
    print(("ok   " if good else "FAIL ") + name)
    fails += not good

ncpu = int(os.environ.get("SMP", "4"))
check(f"boots to the shell on {ncpu} processor(s)",
      read_until(b"bastios> ") and b"(aarch64): " in out
      and (b" pages of memory, %d processor%s\r\n" % (ncpu, b"" if ncpu == 1 else b"s")) in out)
m = re.search(rb"\(aarch64\): (\d+) pages of memory", out)
pages = int(m.group(1)) if m else -1

def run(line, want, wait=b"bastios> "):
    """Send a line; the reply must contain every string in `want` before the next prompt."""
    global out
    mark = len(out)
    qemu.stdin.write(line.encode() + b"\r"); qemu.stdin.flush()
    ok = read_until(wait, mark)
    reply = out[mark:]
    want = want if isinstance(want, list) else [want]
    good = ok and all(w in reply for w in want)
    check(f"{line}", good)
    return reply

run("help", b"commands:")
run("echo hello world", b"hello world")
run("add 2 40", b"42")
run("mul 3 7", b"21")
run("mul 4294967296 4294967296", b"overflow")
run("frobnicate", b"unknown command: frobnicate")
run("mem", b"free pages")
run("cpus", b"processors running: %d" % ncpu)
run("hello", b"hello from user mode, pid 2")
run("true", b"")
# A Linux arm64 program (an ELF executable with no libc; svc numbers from
# the asm-generic table, turned into the Linux layer's own numbers).
run("mhello", [b"hello from linux arm64, pid ", b"stdout mode 0x2", b"read 9: 127.0.0.1", b"[exit 3]"])
# A static glibc program: malloc, files, fork, signals, threads, pipes,
# epoll, sockets (every line must read "ok" and none "FAIL").
reply = run("mtest", b"mtest done")
check("mtest: no check failed", b"FAIL" not in reply)
for want in [b"malloc ok 1", b"file abc", b"child exit 5", b"signal 10", b"thread 7",
             b"ok   epoll_wait", b"ok   socket read", b"machine aarch64",
             b"ok   undefined instruction is SIGILL", b"ok   brk is SIGTRAP", b"ok   ptrace: five single steps", b"ok   ptrace: SETREGSET",
             b"ok   ptrace: stops before and after getpid", b"ok   ptrace: child runs on to exit 7"]:
    check("mtest: " + want.decode(), want in reply)
# BusyBox's ash, built as a static glibc executable at 0x400000 (a non-PIE
# program, which loads where it was linked): arithmetic, a pipe between two
# forked applets, a file written and read back.
run("ash", b"BusyBox", wait=b"# ")
run("echo ash $((6*7))", b"ash 42", wait=b"# ")
run("echo abc | wc -c", b"4", wait=b"# ")
run("echo $((1 << 40))", b"1099511627776", wait=b"# ")
run("echo data > /tmp/f; cat /tmp/f; wc -c < /tmp/f", [b"data", b"5"], wait=b"# ")
run("exit", b"")
# The timer interrupt: ticks count up, 100 a second.
reply = run("uptime", b"ticks")
time.sleep(1.0)
reply2 = run("uptime", b"ticks")
t1 = re.search(rb"(\d+)", reply); t2 = re.search(rb"(\d+)", reply2)
check("the timer ticks about 100 times a second",
      bool(t1 and t2) and 60 <= int(t2.group(1)) - int(t1.group(1)) <= 160)
run("ps", b"    1     0 ready")
run("spin &", b"[2]")
# spin never makes a system call: only the timer can give the shell
# the CPU back, so getting a prompt at all shows preemption.
run("echo still here", b"still here")
run("ps", b"spin")
run("kill 2", b"")
run("kill 2", b"no such process")
if ncpu >= 3:
    # Programs that never stop run at once on different processors: two of
    # three spins are RUNNING in the same `ps`.
    for _ in range(3):
        run("spin &", b"[")
    reply = run("ps", b"spin")
    check("spins run on several processors at once", reply.count(b" running ") >= 2)
    for pid in re.findall(rb"^\s*(\d+)\s+1 \w+\s+\d+ spin", reply, re.M):
        run("kill " + pid.decode(), b"")
run("echo survived", b"survived")
# The file system (src/fs.bastio, shared with x86-64): files in memory.
run("echo hi there > f1", b"")
run("cat f1", b"hi there")
run("echo and more >> f1", b"")
run("cat f1", [b"hi there", b"and more"])
run("mkdir d", b"")
run("mv f1 d/f2", b"")
run("ls d", b"f2")
run("cat d/f2", b"hi there")
run("cat disk/etc/hosts", b"localhost")
run("rm d/f2", b"")
run("ls d", b"")
run("cat d/f2", b"no such file")
# The virtio-mmio disk and network card (src/virtio_blk.bastio and
# src/virtio_net.bastio over src/virtio_mmio.bastio, shared with x86-64).
check("the device tree lists QEMU virt's 32 virtio-mmio slots", b"devicetree: 32 virtio-mmio slots from 0x000000000a000000\r\n" in out)
check("the virtio disk is found", b"disk: virtio\r\n" in out)
run("echo on disk > disk/a", b"")
run("cat disk/a", b"on disk")
run("ls disk", b"a")
run("ping", b"reply from 10.0.2.2")
run("halt", b"pages free at power off", wait=b"power off\r\n")
try:
    qemu.wait(timeout=10)
    check("QEMU exits on halt (PSCI SYSTEM_OFF)", True)
except subprocess.TimeoutExpired:
    qemu.kill()
    check("QEMU exits on halt (PSCI SYSTEM_OFF)", False)
m = re.search(rb"BastiOS: (\d+) pages free at power off", out)
check(f"every page is back in the pool at power off ({pages} at boot)", bool(m) and int(m.group(1)) == pages)
print("---- transcript ----")
print(out.decode(errors="replace").replace("\r", ""))
sys.exit(1 if fails else 0)
PY
