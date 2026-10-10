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

# (input line, expected text in the reply). The shell is a user process;
# the later steps exercise process creation, waiting, preemption, killing
# and fault isolation.
steps = [
    ("help", b"commands:"),
    ("about", b"proved safe"),
    ("echo hello world", b"hello world"),
    ("add 2 40", b"42"),
    ("mul 3 7", b"21"),
    ("mul 4294967296 4294967296", b"overflow"),
    ("add 99999999999999999999 1", b"too large"),
    ("frobnicate", b"unknown command: frobnicate"),
    ("mem", b"free pages"),
    ("hello", b"hello from user mode, pid 2"),
    ("ps", b"    1     0 ready"),
    ("spin &", b"[2]"),
    # spin never makes a system call: only the timer can give the shell
    # the CPU back, so getting a prompt at all shows preemption.
    ("echo still here", b"still here"),
    ("ps", b"spin"),
    ("kill 2", b""),
    ("kill 2", b"no such process"),
    ("fault", b"page fault at rip"),
    ("echo survived", b"survived"),
    # User buffers that cross a page boundary, and a write cut short by an
    # unmapped page (it returns 4, the exit code).
    ("straddle", [b"page-crossing write ok", b"hello from user mode", b"END![exit 4]"]),
    ("bench", b"ticks"),
    # Static Linux ELF executables: one linked at a fixed address, one a
    # static PIE. Each checks its .data, .bss and initial stack (argv, envp,
    # auxv) and says ok. A truncated ELF must be refused, not loaded.
    ("elfexec", b"elfexec: ok"),
    ("elfpie", b"elfpie: ok"),
    ("elfbad", b"unknown command: elfbad"),
    # A C program against musl libc: TLS, printf, exit status.
    ("mhello", [b"hello from musl: mhello, argc 1, 6", b"[exit 3]"]),
    # Two processes using xmm0 at once, preempting each other: each must
    # keep its own value (checked again at the end, once both are done).
    ("fpucheck &", b"]"),
    ("fpucheck", b"fpucheck: xmm0 kept"),
    # musl malloc (brk, mmap), stdio on a file, and a line read from the
    # console; the third field is what to wait for instead of the prompt.
    ("mtest", [b"mtest: malloc heap 75", b"mtest: file line 42"], b"type a line"),
    ("typed for mtest", b"mtest: got typed for mtest"),
    ("sh", b"BastiOS 0.2 -- type"),
    ("exit", b""),
    ("mem", b"free pages"),
    # Files. motd and about are built into the kernel and installed at boot.
    ("ls", [b"motd", b"about"]),
    ("cat motd", b"Welcome to BastiOS"),
    ("cat about", b"exactly\r\nonce"),
    ("cat nothere", b"no such file: nothere"),
    ("echo first line > notes", b""),
    ("echo second line >> notes", b""),
    ("cat notes", b"first line\r\nsecond line\r\n"),
    ("ls", b"      23 notes"),
    ("echo replaced>notes", b""),
    ("cat notes", b"replaced\r\n"),
    ("rm notes", b""),
    ("cat notes", b"no such file: notes"),
    ("rm notes", b"cannot remove notes"),
    # Several pages, seeking, appending, holes, descriptor limits; it exits
    # with "big" still open, so removing it shows that exit closed it.
    ("fstest", b"fstest: ok"),
    ("rm big", b""),
    ("ls", b"about"),
    # Every page the files used is back once they are removed.
    ("mem", b"free pages"),
]
fails = 0
if not read_until(b"bastios> "):
    print("FAIL: no prompt after boot"); fails += 1
else:
    print("ok   boot banner and prompt")
    for step in steps:
        line, want = step[0], step[1]
        until = step[2] if len(step) > 2 else b"bastios> "
        mark = len(out)
        qemu.stdin.write(line.encode() + b"\r"); qemu.stdin.flush()
        wants = want if isinstance(want, list) else [want]
        ok = read_until(until, mark) and all(w in out[mark:] for w in wants)
        print(("ok  " if ok else "FAIL"), repr(line))
        fails += not ok
    if out.count(b"fpucheck: xmm0 kept") == 2 and b"xmm0 changed" not in out:
        print("ok   both fpucheck processes kept xmm0")
    else:
        print("FAIL fpucheck: SSE state leaked between processes"); fails += 1
    # Every page the finished processes and removed files used must be
    # back in the pool.
    mem = [l.strip() for l in out.split(b"\n") if l.strip().endswith(b"free pages")]
    if len(mem) == 3 and mem[0] == mem[1] == mem[2]:
        print("ok   no pages leaked:", mem[2].decode())
    else:
        print("FAIL page leak:", mem); fails += 1
    qemu.stdin.write(b"halt\r"); qemu.stdin.flush()
    read_until(b"pages free at power off")
import re
boot = re.search(rb"BastiOS 0.2: (\d+) pages of memory", out)
off = re.search(rb"BastiOS: (\d+) pages free at power off", out)
if boot and off and boot.group(1) == off.group(1):
    print(f"ok   all {off.group(1).decode()} pages back in the pool at power off")
else:
    print("FAIL pages at power off:", boot and boot.group(1), off and off.group(1)); fails += 1
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
