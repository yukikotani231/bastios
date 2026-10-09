#!/usr/bin/env bash
# Boot BastiOS in QEMU, drive the shell over the serial port, and check its replies.
# Input is sent one line at a time, each after the prompt appears: bytes sent before
# the kernel initialises the UART are dropped.
set -u
cd "$(dirname "$0")/.."
KERNEL=${KERNEL:-build/bastios.elf}
TIMEOUT=${TIMEOUT:-60}

python3 - "$KERNEL" "$TIMEOUT" <<'PY'
import subprocess, sys, time, select

kernel, timeout = sys.argv[1], float(sys.argv[2])
qemu = subprocess.Popen(
    ["qemu-system-x86_64", "-kernel", kernel, "-m", "64M", "-display", "none",
     "-no-reboot", "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
     "-serial", "stdio", "-monitor", "none"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
deadline = time.time() + timeout
out = b""

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

# (input line, expected text in the reply)
steps = [
    ("help", b"commands:"),
    ("about", b"proved safe at compile time"),
    ("echo hello world", b"hello world"),
    ("add 2 40", b"42"),
    ("mul 3 7", b"21"),
    ("mul 4294967296 4294967296", b"overflow"),
    ("add 99999999999999999999 1", b"too large"),
    ("frobnicate", b"unknown command: frobnicate"),
]
fails = 0
if not read_until(b"bastios> "):
    print("FAIL: no prompt after boot"); fails += 1
else:
    print("ok   boot banner and prompt")
    for line, want in steps:
        mark = len(out)
        qemu.stdin.write(line.encode() + b"\r"); qemu.stdin.flush()
        ok = read_until(b"bastios> ", mark) and want in out[mark:]
        print(("ok  " if ok else "FAIL"), repr(line))
        fails += not ok
    qemu.stdin.write(b"halt\r"); qemu.stdin.flush()
    read_until(b"bye")
try:
    code = qemu.wait(timeout=max(1, deadline - time.time()))
except subprocess.TimeoutExpired:
    qemu.kill(); code = None
# isa-debug-exit: the kernel writes 0x10, QEMU exits with (0x10 << 1) | 1 = 33.
if code == 33:
    print("ok   halt exits QEMU with status 33")
else:
    print(f"FAIL halt: QEMU exit status {code}"); fails += 1
print("---- serial transcript ----")
print(out.decode(errors="replace").replace("\r", ""))
print(f"---- {fails} failed ----")
sys.exit(1 if fails else 0)
PY
