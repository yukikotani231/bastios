#!/usr/bin/env bash
# How fast the disk drivers are: boots BastiOS once per driver (legacy ATA,
# virtio, NVMe, AHCI, and NVMe with 4 KiB sectors) on a fresh 64 MiB disk
# image, runs `diskbench` (a 2 MiB file written and read back in 4 KiB
# calls) RUNS times, and prints each run's numbers. Not a pass/fail test, and only comparable between
# runs on the same host: under QEMU's TCG every register access and every
# sector the host writes cost far more than on real hardware.
set -u
cd "$(dirname "$0")/.."
KERNEL=${KERNEL:-build/bastios.elf}
RUNS=${RUNS:-3}

python3 - "$KERNEL" "$RUNS" <<'PY'
import os, re, select, subprocess, sys, tempfile, time

kernel, runs = sys.argv[1], int(sys.argv[2])
tmp = tempfile.TemporaryDirectory()
drivers = {
    "ATA": lambda d: ["-drive", f"file={d},format=raw,if=ide,index=0"],
    "virtio": lambda d: ["-drive", f"file={d},format=raw,if=none,id=vd", "-device", "virtio-blk-pci,drive=vd"],
    "NVMe": lambda d: ["-drive", f"file={d},format=raw,if=none,id=nv", "-device", "nvme,serial=bench,drive=nv"],
    "AHCI": lambda d: ["-machine", "q35", "-drive", f"file={d},format=raw,if=ide"],
    "NVMe 4K": lambda d: ["-drive", f"file={d},format=raw,if=none,id=nv", "-device",
                          "nvme,serial=bench,drive=nv,logical_block_size=4096,physical_block_size=4096"],

}
for name, args in drivers.items():
    disk = os.path.join(tmp.name, f"{name}.img")
    with open(disk, "wb") as f:
        f.truncate(64 << 20)
    q = subprocess.Popen(
        ["qemu-system-x86_64", "-kernel", kernel, "-m", "256M", "-display", "none", "-no-reboot",
         "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04", "-nic", "none",
         "-serial", "stdio", "-monitor", "none"] + args(disk),
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = b""
    def until(marker, start, timeout=300):
        global out
        end = time.time() + timeout
        while marker not in out[start:]:
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
    if not until(b"bastios> ", 0):
        print(f"{name}: no prompt")
        q.kill(); q.wait()
        continue
    for r in range(runs):
        mark = len(out)
        q.stdin.write(b"diskbench\r"); q.stdin.flush()
        until(b"bastios> ", mark)
        lines = [l.strip().decode(errors="replace") for l in out[mark:].split(b"\n") if b"MiB" in l or b"diskbench" in l]
        print(f"{name:7s} run {r + 1}: " + "; ".join(l for l in lines if not l.endswith("diskbench")))
    q.kill(); q.wait()
PY
