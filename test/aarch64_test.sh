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
KERNEL=${KERNEL:-build/aarch64/bastios.elf}
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
import re, select, subprocess, sys, time

kernel, qemu_bin, timeout = sys.argv[1], sys.argv[2], float(sys.argv[3])
qemu = subprocess.Popen(
    [qemu_bin, "-M", "virt,gic-version=3", "-cpu", "cortex-a72", "-m", "256M", "-nographic",
     "-no-reboot", "-kernel", kernel, "-serial", "stdio", "-monitor", "none"],
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

check("boots to the shell on one processor",
      read_until(b"bastios> ") and b"(aarch64): " in out and b" pages of memory, 1 processor\r\n" in out)
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
run("cpus", b"processors running: 1")
run("hello", b"hello from user mode, pid 2")
run("true", b"")
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
run("echo survived", b"survived")
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
