#!/usr/bin/env python3
"""Randomised power cuts: boot BastiOS on a disk, run `crashme`, kill QEMU
after a random delay, run test/fsck.py on the image, and boot again.

    test/powercut.py [--rounds N] [--seed S] [--smp N] [--drivers ata,virtio,nvme,ahci,nvme4k]
                     [--keep DIR] [--kernel build/bastios.elf]

test/shell_test.sh does the same with a fixed list of delays; this runs
many more cuts, at random delays, on any driver and processor count, and
keeps the image of the first failure. Killing QEMU keeps every sector write
that completed, so the cut falls between two sector writes (test/crashsim.py
covers reordered and torn writes). Exit status 1 if any cut left the image
inconsistent or the next boot found a damaged file system.
"""
import argparse, os, random, select, shutil, subprocess, sys, tempfile, time

ap = argparse.ArgumentParser()
ap.add_argument("--rounds", type=int, default=20)
ap.add_argument("--seed", type=int, default=1)
ap.add_argument("--smp", type=int, default=1)
ap.add_argument("--drivers", default="ata,virtio,nvme,ahci,nvme4k")
ap.add_argument("--max-delay", type=float, default=1.5)
ap.add_argument("--keep", default=None, help="directory to copy the image of a failure into")
ap.add_argument("--workload", default="crashme", choices=["crashme", "mpc", "grow", "big"],
                help="crashme: the built-in sequential workload; mpc: user/mpc.c, concurrent workers on a disk with programs")
ap.add_argument("--disk-mb", type=int, default=33)
ap.add_argument("--workers", type=int, default=4)
ap.add_argument("--hog", action="store_true", help="mpc: one more process fills the disk and frees it again")
ap.add_argument("--kernel", default="build/bastios.elf")
ap.add_argument("--timeout", type=float, default=120)
args = ap.parse_args()

here = os.path.dirname(os.path.abspath(__file__))
fsck_py = os.path.join(here, "fsck.py")
rnd = random.Random(args.seed)
tmp = tempfile.TemporaryDirectory()
disk = os.path.join(tmp.name, "disk.img")
if args.workload in ("mpc", "grow", "big"):
    # A disk holding the program (the same file system code, formatted by
    # test/compat/mkdisk.py); it is mounted at /disk, where mpc works.
    subprocess.run([sys.executable, os.path.join(here, "compat", "mkdisk.py"), disk, "mpc=build/user/mpc.elf"], check=True)
    if os.path.getsize(disk) < args.disk_mb << 20:
        with open(disk, "r+b") as f:
            f.truncate(args.disk_mb << 20)     # the file system takes the size at mount
else:
    with open(disk, "wb") as f:
        f.truncate(args.disk_mb << 20)

def drive_args(kind):
    if kind == "ata":
        return ["-drive", f"file={disk},format=raw,if=ide,index=0"]
    if kind == "virtio":
        return ["-drive", f"file={disk},format=raw,if=none,id=vd", "-device", "virtio-blk-pci,drive=vd"]
    if kind == "nvme":
        return ["-machine", "q35", "-device", "pcie-root-port,id=rp1,chassis=1",
                "-drive", f"file={disk},format=raw,if=none,id=nv",
                "-device", "nvme,serial=bastios,drive=nv,bus=rp1"]
    if kind == "nvme4k":
        return ["-drive", f"file={disk},format=raw,if=none,id=nv",
                "-device", "nvme,serial=bastios,drive=nv,logical_block_size=4096,physical_block_size=4096"]
    if kind == "ahci":
        return ["-machine", "q35", "-drive", f"file={disk},format=raw,if=ide"]
    raise SystemExit(f"unknown driver {kind}")

def boot(kind):
    return subprocess.Popen(
        ["qemu-system-x86_64", "-kernel", args.kernel, "-m", "1G", "-smp", str(args.smp),
         "-display", "none", "-no-reboot", "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
         "-serial", "stdio", "-monitor", "none", "-nic", "none"] + drive_args(kind),
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

def read_until(q, buf, marker, start=0):
    deadline = time.time() + args.timeout
    while marker not in buf[0][start:]:
        left = deadline - time.time()
        if left <= 0:
            return False
        r, _, _ = select.select([q.stdout], [], [], left)
        if not r:
            return False
        chunk = q.stdout.read1(4096)
        if not chunk:
            return False
        buf[0] += chunk
    return True

def fsck():
    r = subprocess.run([sys.executable, fsck_py, disk], capture_output=True, text=True)
    return r.returncode == 0, r.stdout + r.stderr

kinds = args.drivers.split(",")
fails = 0
for n in range(args.rounds):
    # (a boot on the fresh image has nothing to check)
    kind = kinds[n % len(kinds)]
    delay = rnd.uniform(0.02, args.max_delay)
    q = boot(kind)
    buf = [b""]
    ok = read_until(q, buf, b"bastios> ")
    mark = len(buf[0])
    if args.workload in ("mpc", "grow", "big"):
        if ok:
            q.stdin.write(b"ash\r"); q.stdin.flush()
            ok = read_until(q, buf, b"\n# ", mark)
            mark = len(buf[0])
        if ok:
            cmd = "/disk/mpc grow /disk/pc" if args.workload == "grow" else "/disk/mpc big /disk/pc run" if args.workload == "big" else f"{'MPC_HOG=1 ' if args.hog else ''}/disk/mpc run /disk/pc {args.workers}"
            q.stdin.write(cmd.encode() + b"\r"); q.stdin.flush()
            ok = read_until(q, buf, b"mpc: running", mark) and b"mpc: clean" in buf[0][mark:]
        time.sleep(delay)
        q.kill(); q.wait()
        line = next((l for l in buf[0][mark:].split(b"\n") if b"mpc:" in l and b"usage" not in l), b"(nothing)").strip().decode(errors="replace")
        if b"mpc: BAD" in buf[0][mark:]:
            ok = False
            print(buf[0][mark:].decode(errors="replace"))
    else:
        if ok:
            q.stdin.write(b"crashme\r"); q.stdin.flush()
            ok = read_until(q, buf, b" records\r\n", mark) and b"crashme: clean" in buf[0][mark:]
        time.sleep(delay)
        q.kill(); q.wait()
        line = next((l for l in buf[0][mark:].split(b"\n") if b"crashme:" in l), b"(nothing)").strip().decode(errors="replace")
    good, report = fsck()
    status = "ok  " if ok and good else "FAIL"
    print(f"{status} cut {n + 1} ({kind}, smp {args.smp}) after {delay:.2f} s: {line}; fsck {report.strip().splitlines()[-1] if report.strip() else '(no output)'}", flush=True)
    if not (ok and good):
        fails += 1
        print(report)
        if args.keep:
            os.makedirs(args.keep, exist_ok=True)
            dst = os.path.join(args.keep, f"fail-seed{args.seed}-cut{n + 1}-{kind}.img")
            shutil.copyfile(disk, dst)
            print("image kept:", dst)
            args.keep = None
        break
print(f"---- {fails} failed ----")
sys.exit(1 if fails else 0)
