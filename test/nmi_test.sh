#!/usr/bin/env bash
# An NMI must land on the processor's IST1 stack (boot/core.h), never on
# whatever %rsp held: the first instructions of the `syscall` entry run in
# ring 0 on the user's stack. Boots BastiOS, injects an NMI through QEMU's
# monitor, waits for the kernel to report it, and checks that the halted
# processor's %rsp is inside the IST1 stack of its block.
#
# The NMI arrives while the kernel waits in bst_cpu_wait, not inside the
# syscall window itself (which is three instructions long and cannot be hit
# on purpose). That is enough to show the gate switches stacks: without
# IST the frame stays on the kernel stack, far above the IST1 stack.
set -u
cd "$(dirname "$0")/.."
KERNEL=${KERNEL:-build/bastios.elf}
TIMEOUT=${TIMEOUT:-30}

python3 - "$KERNEL" "$TIMEOUT" <<'PY'
import os, re, select, socket, subprocess, sys, tempfile, time

kernel, timeout = sys.argv[1], float(sys.argv[2])
CORE_BLOCK, CORE_IST_TOP, IST_SIZE = 0x20000, 0x1400, 0x1000

tmp = tempfile.TemporaryDirectory()
mon_path = os.path.join(tmp.name, "mon")
qemu = subprocess.Popen(
    ["qemu-system-x86_64", "-kernel", kernel, "-m", "64M", "-smp", "1",
     "-display", "none", "-no-reboot", "-nic", "none",
     "-serial", "stdio", "-monitor", f"unix:{mon_path},server,nowait"],
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

fails = 0
if not read_until(b"bastios> "):
    print("FAIL: no prompt after boot"); fails += 1
else:
    monitor("nmi")
    if read_until(b"kernel fault, vector 0x0000000000000002"):
        print("ok   NMI reported as a kernel fault")
        time.sleep(0.5)
        regs = monitor("info registers")
        m = re.search(r"RSP=([0-9a-f]{16})", regs)
        rsp = int(m.group(1), 16) if m else 0
        off = rsp % CORE_BLOCK
        if CORE_IST_TOP - IST_SIZE <= off < CORE_IST_TOP:
            print(f"ok   NMI handled on the IST1 stack (rsp {rsp:#x})")
        else:
            print(f"FAIL NMI stack: rsp {rsp:#x} is not in IST1"); fails += 1
    else:
        print("FAIL no kernel fault report for the NMI"); fails += 1
qemu.kill(); qemu.wait()
print(out.decode(errors="replace").replace("\r", ""))
print(f"---- {fails} failed ----")
sys.exit(1 if fails else 0)
PY
