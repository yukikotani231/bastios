#!/usr/bin/env python3
"""Writes the Rust disk for the compat suite (test/compat/rust.txt): the
toolchain of the official rust:alpine image (rustc, cargo, the standard
library, and Alpine's gcc and musl, which rustc links with), as a tar file
(/disk/root.tar) next to a BusyBox at /disk/busybox. The checks unpack it
under /disk/r, make the device nodes, chroot into it and build and run Rust
programs.

    rustdisk.py OUT.img IMAGE BUSYBOX --cache DIR [--rust-analyzer FILE --rust-src TGZ]

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
    extra = {}
    for opt in ("--rust-analyzer", "--rust-src"):
        if opt in args:
            extra[opt] = args[args.index(opt) + 1]
            del args[args.index(opt):args.index(opt) + 2]
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
    disks = [f"busybox={busybox}", f"root.tar={tar}"]
    if extra:
        # rust-analyzer and the standard library's source (which it reads to
        # know the types of String, Vec and the rest) are a second tar file, to
        # unpack over the root: a file on the disk is at most 1 GiB, and the
        # root's tar is close to it.
        dest = "usr/local/rustup/toolchains/1.99.0-x86_64-unknown-linux-musl/"
        ra = os.path.join(work, "ra.tar")
        with tarfile.open(ra, "w") as t, tarfile.open(extra["--rust-src"], "r:gz") as s:
            ti = tarfile.TarInfo("usr/local/cargo/bin/rust-analyzer")
            ti.mode, ti.size = 0o755, os.path.getsize(extra["--rust-analyzer"])
            with open(extra["--rust-analyzer"], "rb") as f:
                t.addfile(ti, f)
            for ti in s:
                parts = ti.name.split("/", 2)
                if len(parts) < 3 or ti.islnk():
                    continue
                ti.name = dest + parts[2]
                t.addfile(ti, s.extractfile(ti) if ti.isfile() else None)
        disks.append(f"ra.tar={ra}")
    if RUSTC not in files:
        sys.exit(f"rustdisk: {image} has no {RUSTC}")
    env = dict(os.environ, MKDISK_FREE_MB="1300")
    os.execve(sys.executable, [sys.executable, os.path.join(HERE, "mkdisk.py"), out,
                               *disks], env)

main()
