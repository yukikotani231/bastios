#!/usr/bin/env bash
# A kernel stack that runs past its bottom must hit the unmapped guard page
# under it (boot/core.h, guard_block in boot/smp.c) and halt, not write over
# the IST1 stack, the TSS and the saved user context below.
#
# Boots BastiOS, stops the processor while it waits at the shell prompt,
# moves its %rsp to 16 bytes above the guard through QEMU's gdb stub, and
# lets it run. The next interrupt pushes its frame into the guard: that
# page fault finds no stack either and becomes a double fault, which runs
# on IST1 and is reported with cr2 inside the guard. Without the guard the
# frame lands on mapped memory and nothing is reported.
#
# The application processor's block comes from bst_boot_take, not from the
# kernel image, so the test also reads its guard through the gdb stub:
# the guard must not be mapped and the page above it must be.
#
# Nothing in Bastio recurses this deep today; the test makes the stack
# overflow on purpose instead of finding a program that does.
set -u
cd "$(dirname "$0")/.."
KERNEL=${KERNEL:-build/bastios.elf}
TIMEOUT=${TIMEOUT:-30}

python3 - "$KERNEL" "$TIMEOUT" <<'PY'
import os, re, select, socket, subprocess, sys, tempfile, time

kernel, timeout = sys.argv[1], float(sys.argv[2])
CORE_BLOCK, CORE_GUARD, PAGE = 0x20000, 0x2000, 0x1000   # boot/core.h

tmp = tempfile.TemporaryDirectory()
mon_path = os.path.join(tmp.name, "mon")
gdb_path = os.path.join(tmp.name, "gdb")
qemu = subprocess.Popen(
    ["qemu-system-x86_64", "-kernel", kernel, "-m", "64M", "-smp", "2",
     "-display", "none", "-no-reboot", "-nic", "none",
     "-serial", "stdio", "-monitor", f"unix:{mon_path},server,nowait",
     "-gdb", f"unix:{gdb_path},server,nowait"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
deadline = time.time() + timeout
out = b""

def read_until(marker):
    global out
    while marker not in out:
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

def monitor(cmd):
    s = socket.socket(socket.AF_UNIX)
    s.connect(mon_path)
    s.settimeout(1.0)
    got = b""
    def drain():
        nonlocal got
        try:
            while True:
                d = s.recv(65536)
                if not d:
                    break
                got += d
        except socket.timeout:
            pass
    drain()
    got = b""
    s.sendall(cmd.encode() + b"\n")
    drain()
    s.close()
    return got.decode(errors="replace")

class Gdb:
    """Just enough of the gdb remote protocol: send a packet, read its reply."""
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(path)
        self.s.settimeout(5.0)
        self.buf = b""
    def packet(self, body):
        self.s.sendall(b"$%s#%02x" % (body, sum(body) % 256))
        while True:
            m = re.search(rb"\$([^#]*)#[0-9a-f]{2}", self.buf)
            if m:
                self.buf = self.buf[m.end():]
                self.s.sendall(b"+")
                return m.group(1)
            self.buf += self.s.recv(4096)

fails = 0
if not read_until(b"bastios> "):
    print("FAIL: no prompt after boot"); fails += 1
else:
    time.sleep(0.5)                       # let it settle in bst_cpu_wait
    g = Gdb(gdb_path)
    g.packet(b"?")                        # attaching stops the processors
    if g.packet(b"Hg2") != b"OK":
        print("FAIL could not select the application processor"); fails += 1
    else:
        ap = int.from_bytes(bytes.fromhex(g.packet(b"p7").decode()), "little")
        ap_guard = ap - ap % CORE_BLOCK + CORE_GUARD
        under, above = g.packet(b"m%x,8" % ap_guard), g.packet(b"m%x,8" % (ap_guard + PAGE))
        if under.startswith(b"E") and not above.startswith(b"E"):
            print(f"ok   the application processor's guard page {ap_guard:#x} is unmapped")
        else:
            print(f"FAIL application processor guard {ap_guard:#x}: {under!r}, above it {above!r}"); fails += 1
    g.packet(b"Hg1")
    rsp = int.from_bytes(bytes.fromhex(g.packet(b"p7").decode()), "little")
    block = rsp - rsp % CORE_BLOCK
    guard = block + CORE_GUARD
    new = guard + PAGE + 16
    print(f"     rsp {rsp:#x}, guard page {guard:#x}; moving rsp to {new:#x}")
    if g.packet(b"P7=" + new.to_bytes(8, "little").hex().encode()) != b"OK":
        print("FAIL could not set rsp"); fails += 1
    g.s.sendall(b"$c#63")
    if read_until(b"kernel fault, vector 0x0000000000000008"):
        print("ok   stack overflow reported as a double fault")
        read_until(b" err ")
        m = re.search(rb"vector 0x0000000000000008 rip 0x[0-9a-f]+ cr2 0x([0-9a-f]+)", out)
        cr2 = int(m.group(1), 16) if m else 0
        if guard <= cr2 < guard + PAGE:
            print(f"ok   the fault address is in the guard page (cr2 {cr2:#x})")
        else:
            print(f"FAIL fault address {cr2:#x} is not in the guard page"); fails += 1
    else:
        print("FAIL no double fault: the overflow went unnoticed"); fails += 1
qemu.kill(); qemu.wait()
print(out.decode(errors="replace").replace("\r", ""))
print(f"---- {fails} failed ----")
sys.exit(1 if fails else 0)
PY
