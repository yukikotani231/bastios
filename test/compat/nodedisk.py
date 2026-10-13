#!/usr/bin/env python3
"""Writes the Node.js disk for the compat suite (test/compat/node.txt):
the Node binary of the official node:alpine image with the few libraries
it needs (musl, libstdc++, libgcc), as a root file system under /disk/n,
next to a BusyBox at /disk/busybox. The checks make the device nodes, chroot
into it and run Node.

    nodedisk.py OUT.img IMAGE BUSYBOX --cache DIR

IMAGE is a Docker Hub image, NAME@sha256:DIGEST (the Makefile pins
node:22-alpine). Its layers are fetched and cached as alpinedisk.py does it
(in DIR, checked against their digests each time they are used). Node
reserves a large range of address space at start (V8's heap cage, code
range and thread stacks), so it also checks how the kernel's user window
copes with big mappings.
"""
import os, shutil, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import alpinedisk

# What the image holds that Node needs, and where it goes: (path in the image, path in the root).
A = alpinedisk.ARCH
FILES = [("usr/local/bin/node", "usr/local/bin/node"),
         (f"lib/ld-musl-{A}.so.1", f"lib/ld-musl-{A}.so.1"),
         (f"lib/ld-musl-{A}.so.1", f"lib/libc.musl-{A}.so.1"),
         ("usr/lib/libgcc_s.so.1", "usr/lib/libgcc_s.so.1"),
         ("usr/lib/libstdc++.so.6.0.34", "usr/lib/libstdc++.so.6")]

def main():
    args = sys.argv[1:]
    cache = None
    if "--cache" in args:
        cache = args[args.index("--cache") + 1]
        del args[args.index("--cache"):args.index("--cache") + 2]
    out, image, busybox = args
    work = os.path.join(os.path.dirname(os.path.abspath(out)), "node")
    cache = cache or work
    os.makedirs(cache, exist_ok=True)
    files = alpinedisk.merged(alpinedisk.layers(image, cache))
    root = os.path.join(work, "root")
    shutil.rmtree(root, ignore_errors=True)
    for src, dst in FILES:
        if src not in files:
            sys.exit(f"nodedisk: {image} has no {src}")
        path = os.path.join(root, dst)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(files[src][1])
        os.chmod(path, 0o755)
    for d in ("dev", "proc", "tmp", "etc"):
        os.makedirs(os.path.join(root, d), exist_ok=True)
    keep = os.path.join(root, "etc", "hosts")
    open(keep, "w").write("127.0.0.1 localhost\n")
    env = dict(os.environ, MKDISK_FREE_MB="64")
    os.execve(sys.executable, [sys.executable, os.path.join(HERE, "mkdisk.py"), out,
                               f"busybox={busybox}", f"n={root}"], env)

main()
