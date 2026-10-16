#!/usr/bin/env python3
"""Mounts damaged disk images: change a few bytes of the file system's
metadata, boot BastiOS on the result, touch every file, and check that the
kernel survives.

    test/diskfuzz.py [--rounds N] [--seed S] [--drivers virtio,ata,nvme,ahci] [--keep DIR]
                     [--kernel build/bastios.elf]

The base image is made by test/compat/mkdisk.py (programs and a directory
of files). Each round copies it and changes 1 to 8 bytes in its first
--span bytes, where the header, the directory entries and the block
pointers are, to 0, 0xff or a flipped bit. A damaged image may mount, mount
read-only or be refused; what it must not do is hang, fault, run out of
kernel memory or leak pages. After the boot the script also lists the disk
and reads every file on it, which walks the damaged pointers.

Exit status 1 if any round failed; the image of the first failure is kept
under --keep.
"""
import argparse, os, random, re, select, shutil, subprocess, sys, tempfile, time

ap = argparse.ArgumentParser()
ap.add_argument("--rounds", type=int, default=20)
ap.add_argument("--seed", type=int, default=1)
ap.add_argument("--smp", type=int, default=1)
ap.add_argument("--drivers", default="virtio,ata,nvme,ahci")
ap.add_argument("--span", type=int, default=256 << 10, help="bytes at the start of the image that may be changed")
ap.add_argument("--keep", default=None, help="directory to copy the image of a failure into")
ap.add_argument("--kernel", default="build/bastios.elf")
ap.add_argument("--write", action="store_true", help="also create, grow, rename and delete files on the damaged disk")
ap.add_argument("--timeout", type=float, default=60)
args = ap.parse_args()

here = os.path.dirname(os.path.abspath(__file__))
rnd = random.Random(args.seed)
tmp = tempfile.TemporaryDirectory()
base = os.path.join(tmp.name, "base.img")
disk = os.path.join(tmp.name, "disk.img")

# Files of several sizes in a directory, so that the directory, the
# indirect blocks and the data are all there to be damaged.
files = os.path.join(tmp.name, "files")
os.makedirs(files)
for i, size in enumerate([0, 1, 100, 4096, 5000, 70000, 3 << 20]):
    with open(os.path.join(files, f"f{i}"), "wb") as f:
        f.write(bytes((j * 7 + i) % 251 for j in range(size)))
subprocess.run([sys.executable, os.path.join(here, "compat", "mkdisk.py"), base,
                "mh=build/user/mhello.elf", "d=" + files], check=True)

def drive_args(kind):
    if kind == "ata":
        return ["-drive", f"file={disk},format=raw,if=ide,index=0"]
    if kind == "virtio":
        return ["-drive", f"file={disk},format=raw,if=none,id=vd", "-device", "virtio-blk-pci,drive=vd"]
    if kind == "nvme":
        return ["-drive", f"file={disk},format=raw,if=none,id=nv", "-device", "nvme,serial=bastios,drive=nv"]
    if kind == "ahci":
        return ["-machine", "q35", "-drive", f"file={disk},format=raw,if=ide"]
    raise SystemExit(f"unknown driver {kind}")

def boot(kind):
    return subprocess.Popen(
        ["qemu-system-x86_64", "-kernel", args.kernel, "-m", "256M", "-smp", str(args.smp),
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

def damage(img):
    span = min(args.span, len(img))
    # The header and the fixed directory blocks are mostly zeros; most of
    # the changes go to the bytes that are set there, where they matter.
    live = [i for i in range(5 * 4096) if img[i]]
    changes = []
    for _ in range(rnd.randint(1, 8)):
        at = rnd.choice(live) if live and rnd.random() < 0.7 else rnd.randrange(span)
        how = rnd.randrange(3)
        old = img[at]
        img[at] = 0 if how == 0 else 0xff if how == 1 else old ^ (1 << rnd.randrange(8))
        changes.append(f"{at:#x}:{old:#04x}->{img[at]:#04x}")
    return changes

def one(n, kind):
    with open(base, "rb") as f:
        img = bytearray(f.read())
    changes = damage(img)
    with open(disk, "wb") as f:
        f.write(img)
    q = boot(kind)
    buf = [b""]
    why = None
    if not read_until(q, buf, b"bastios> "):
        why = "no prompt"
    else:
        mark = len(buf[0])
        q.stdin.write(b"ash\r"); q.stdin.flush()
        if not read_until(q, buf, b"\n# ", mark):
            why = "no ash prompt"
        else:
            mark = len(buf[0])
            # Walk the damaged file system: list it, read every regular file (a
            # damaged mode can turn a file into a pipe, which blocks a reader), stat it.
            cmd = "ls -l /disk /disk/d; for f in /disk/d/* /disk/mh; do [ -f $f ] && cat $f > /dev/null; done; stat /disk/d/f6; echo walked"
            if args.write:
                cmd = cmd.replace("echo walked", "echo a > /disk/w1; mkdir /disk/wd; echo b > /disk/wd/x; cp /disk/mh /disk/w2; head -c 300000 /disk/d/f5 > /disk/w3; rm -f /disk/w1 /disk/wd/x; rmdir /disk/wd; echo more >> /disk/d/f4; mv /disk/w2 /disk/w4; rm /disk/w3 /disk/w4; sync; echo walked")
            q.stdin.write(cmd.encode() + b"\r"); q.stdin.flush()
            if not read_until(q, buf, b"walked\r\n", mark + len(cmd)):
                why = "the walk did not finish"
            else:
                q.stdin.write(b"exit\r"); q.stdin.flush()
                if not read_until(q, buf, b"bastios> ", len(buf[0])):
                    why = "no prompt after the walk"
                else:
                    q.stdin.write(b"halt\r"); q.stdin.flush()
                    if not read_until(q, buf, b"pages free at power off"):
                        why = "no power off"
    try:
        q.wait(timeout=10)
    except subprocess.TimeoutExpired:
        q.kill(); q.wait()
    text = buf[0]
    if why is None:
        if b"kernel fault" in text or b"bastio: out of memory" in text:
            why = "the kernel reported a fault"
        else:
            booted = re.search(rb"BastiOS 0.2: (\d+) pages of memory", text)
            off = re.search(rb"BastiOS: (\d+) pages free at power off", text)
            if not (booted and off and booted.group(1) == off.group(1)):
                why = f"pages at power off: {booted and booted.group(1)} then {off and off.group(1)}"
    disk_line = next((l for l in text.split(b"\n") if b"disk:" in l), b"").strip().decode(errors="replace")
    return why, changes, disk_line, text

kinds = args.drivers.split(",")
fails = 0
for n in range(args.rounds):
    kind = kinds[n % len(kinds)]
    why, changes, disk_line, text = one(n, kind)
    print(f"{'ok  ' if why is None else 'FAIL'} round {n + 1} ({kind}): {' '.join(changes)}; {disk_line or '(no disk line)'}"
          + (f"; {why}" if why else ""), flush=True)
    if why:
        fails += 1
        print(text.decode(errors="replace")[-1500:])
        if args.keep:
            os.makedirs(args.keep, exist_ok=True)
            dst = os.path.join(args.keep, f"fail-seed{args.seed}-round{n + 1}-{kind}.img")
            shutil.copyfile(disk, dst)
            print("image kept:", dst)
        break
print(f"---- {fails} failed ----")
sys.exit(1 if fails else 0)
