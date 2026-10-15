#!/usr/bin/env python3
"""Writes the Rust disk for the compat suite (test/compat/rust.txt): the
toolchain of the official rust:alpine image (rustc, cargo, the standard
library, and Alpine's gcc and musl, which rustc links with), as a tar file
(/disk/root.tar) next to a BusyBox at /disk/busybox. The checks unpack it
under /disk/r, make the device nodes, chroot into it and build and run Rust
programs.

    rustdisk.py OUT.img IMAGE BUSYBOX --cache DIR

IMAGE is a Docker Hub image, NAME@sha256:DIGEST (the Makefile pins
rust:alpine). Its layers are fetched and cached as alpinedisk.py does it (in
DIR, checked against their digests each time they are used). The whole
image root is kept except the documentation and manual pages.
"""
import io, os, sys, tarfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import alpinedisk

RUSTC = "usr/local/cargo/bin/rustc"

def keep(name):
    return "/share/doc/" not in name and "/share/man/" not in name and not name.startswith("usr/share/man/")

def main():
    args = sys.argv[1:]
    cache = None
    if "--cache" in args:
        cache = args[args.index("--cache") + 1]
        del args[args.index("--cache"):args.index("--cache") + 2]
    out, image, busybox = args
    work = os.path.join(os.path.dirname(os.path.abspath(out)), "rust")
    cache = cache or work
    os.makedirs(cache, exist_ok=True)
    files = alpinedisk.merged(alpinedisk.layers(image, cache))
    os.makedirs(work, exist_ok=True)
    tar = os.path.join(work, "root.tar")
    with tarfile.open(tar, "w") as t:
        for name, (ti, body) in sorted(files.items()):
            if not keep(name):
                continue
            if ti.isfile():
                t.addfile(ti, io.BytesIO(body))
            else:
                t.addfile(ti)
    if RUSTC not in files:
        sys.exit(f"rustdisk: {image} has no {RUSTC}")
    env = dict(os.environ, MKDISK_FREE_MB="1300")
    os.execve(sys.executable, [sys.executable, os.path.join(HERE, "mkdisk.py"), out,
                               f"busybox={busybox}", f"root.tar={tar}"], env)

main()
