#!/usr/bin/env python3
"""Writes the Debian disk for the compat suite (test/compat/debian.txt):
Debian's root file system as a tar file (/disk/root.tar), next to a
BusyBox at /disk/busybox. The checks unpack the root under /disk/d,
chroot into it and run Debian's programs, which are linked against glibc
and started by its dynamic linker, /lib64/ld-linux-x86-64.so.2 (on AArch64, with
COMPAT_ARCH=aarch64, /lib/ld-linux-aarch64.so.1).

    debiandisk.py OUT.img IMAGE BUSYBOX --cache DIR

IMAGE is a Docker Hub image, NAME@sha256:DIGEST (the Makefile pins
debian:stable-slim). Its layers are fetched and cached as alpinedisk.py
does it (in DIR, checked against their digests each time they are used),
so Docker Hub is asked only for what the cache lacks.

The disk holds at most 1024 entries, so the root leaves out what the
checks never touch: /usr/share (documentation, locales, terminfo), dpkg's
per-package file lists, glibc's character set converters (gconv) and
Perl's modules (the perl binary itself stays). What is left, about 850
entries, is every program and library of the image, and three of the
time zone files (ZONES) for the cases that read tzdata.
"""
import gzip, io, os, sys, tarfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import alpinedisk

LEFT_OUT = ("usr/share/", "var/lib/dpkg/info/", f"usr/lib/{alpinedisk.ARCH}-linux-gnu/gconv/",
            f"usr/lib/{alpinedisk.ARCH}-linux-gnu/perl")

ZONES = ("Asia/Tokyo", "America/New_York", "Europe/London")
ZONE_DIRS = ("usr/share/zoneinfo",) + tuple("usr/share/zoneinfo/" + z.split("/")[0] for z in ZONES)

def kept(name):
    n = name[2:] if name.startswith("./") else name
    if n.rstrip("/") in ZONE_DIRS or n in ["usr/share/zoneinfo/" + z for z in ZONES]:
        return True
    return not any(n.startswith(p) for p in LEFT_OUT) or n.rstrip("/") + "/" in LEFT_OUT   # the directories stay

def main():
    args = sys.argv[1:]
    cache = None
    if "--cache" in args:
        cache = args[args.index("--cache") + 1]
        del args[args.index("--cache"):args.index("--cache") + 2]
    out, image, busybox = args
    cache = cache or os.path.join(os.path.dirname(os.path.abspath(out)), "debian")
    os.makedirs(cache, exist_ok=True)
    paths = alpinedisk.layers(image, cache)
    if len(paths) != 1:
        sys.exit(f"debiandisk: {image} should be one layer, a root file system")
    work = os.path.join(os.path.dirname(os.path.abspath(out)), "debian")
    os.makedirs(work, exist_ok=True)
    root = os.path.join(work, "root.tar")
    n = 0
    with tarfile.open(paths[0], "r:gz") as src, tarfile.open(root + ".part", "w", format=tarfile.PAX_FORMAT) as dst:
        for ti in src:
            if not kept(ti.name):
                continue
            dst.addfile(ti, src.extractfile(ti) if ti.isfile() else None)
            n += 1
    os.rename(root + ".part", root)
    print(f"debiandisk: {n} entries of {image} in the root", file=sys.stderr)
    env = dict(os.environ, MKDISK_FREE_MB="120")   # room for the unpacked root
    os.execve(sys.executable, [sys.executable, os.path.join(HERE, "mkdisk.py"), out,
                               f"busybox={busybox}", f"root.tar={root}"], env)

if __name__ == "__main__":
    main()
