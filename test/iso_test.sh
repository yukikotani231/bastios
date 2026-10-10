#!/usr/bin/env bash
# Boot build/bastios.iso (make iso) through GRUB under BIOS and under UEFI
# (OVMF), with the devices `make run` gives it, and check that the shell
# comes up, runs a program, uses the disk and the network, and gets every
# page back at power off. Skips what it lacks the tools for.
set -u
cd "$(dirname "$0")/.."
OVMF=${OVMF:-/usr/share/OVMF/OVMF_CODE_4M.fd}
OVMF_VARS=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}
if ! command -v grub-mkrescue >/dev/null || ! command -v xorriso >/dev/null; then
  echo "SKIP iso: grub-mkrescue or xorriso missing"
  exit 0
fi
make -s build/bastios.iso || exit 1

python3 - "$OVMF" "$OVMF_VARS" <<'PY'
import os, re, select, shutil, subprocess, sys, tempfile, time

ovmf, ovmf_vars = sys.argv[1], sys.argv[2]
tmp = tempfile.TemporaryDirectory()
disk = os.path.join(tmp.name, "disk.img")
with open(disk, "wb") as f:
    f.truncate(33 << 20)
fails = 0

def run(name, firmware):
    global fails
    devs = ["-m", "256M", "-smp", "2", "-display", "none", "-no-reboot",
            "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
            "-drive", f"file={disk},format=raw,if=none,id=vd", "-device", "virtio-blk-pci,drive=vd",
            "-netdev", "user,id=n0", "-device", "virtio-net-pci,netdev=n0",
            "-serial", "stdio", "-monitor", "none", "-cdrom", "build/bastios.iso"]
    q = subprocess.Popen(["qemu-system-x86_64"] + firmware + devs,
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = b""
    deadline = time.time() + 120
    def until(marker, start):
        nonlocal out
        while marker not in out[start:]:
            left = deadline - time.time()
            if left <= 0:
                return False
            r, _, _ = select.select([q.stdout], [], [], left)
            if not r:
                return False
            chunk = q.stdout.read1(4096)
            if not chunk:
                return False
            out += chunk
        return True
    ok = until(b"bastios> ", 0)
    print(("ok  " if ok else "FAIL"), f"({name}) GRUB boots the kernel to the shell")
    fails += not ok
    steps = [("hello", b"hello from user mode"),
             ("echo from the iso > disk/iso", b""),
             ("cat disk/iso", b"from the iso"),
             ("ifconfig", b"(DHCP)")]
    for line, want in steps:
        if not ok:
            break
        mark = len(out)
        if line == "ifconfig":
            time.sleep(1)                 # the lease may still be on its way
        q.stdin.write(line.encode() + b"\r"); q.stdin.flush()
        good = until(b"bastios> ", mark) and want in out[mark:]
        print(("ok  " if good else "FAIL"), f"({name}) {line!r}")
        fails += not good
    if ok:
        q.stdin.write(b"halt\r"); q.stdin.flush()
        until(b"pages free at power off", 0)
        booted = re.search(rb"BastiOS 0.2: (\d+) pages of memory", out)
        off = re.search(rb"BastiOS: (\d+) pages free at power off", out)
        good = booted and off and booted.group(1) == off.group(1) and int(booted.group(1)) > 10000
        print(("ok  " if good else "FAIL"), f"({name}) {booted.group(1).decode() if booted else '?'} pages, every one back at power off")
        fails += not good
    q.kill(); q.wait()
    print(f"---- {name} transcript ----")
    print(out.decode(errors="replace").replace("\r", "")[-1500:])

run("BIOS", [])
if os.path.exists(ovmf) and os.path.exists(ovmf_vars):
    vars_copy = os.path.join(tmp.name, "vars.fd")
    shutil.copy(ovmf_vars, vars_copy)
    run("UEFI", ["-drive", f"if=pflash,format=raw,readonly=on,file={ovmf}",
                 "-drive", f"if=pflash,format=raw,file={vars_copy}"])
else:
    print(f"SKIP UEFI: no OVMF firmware at {ovmf}")
print(f"---- {fails} failed ----")
sys.exit(1 if fails else 0)
PY
