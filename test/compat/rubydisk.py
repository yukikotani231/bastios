#!/usr/bin/env python3
"""Writes the Ruby disk for the compat suite (test/compat/ruby.txt): the root
of the official ruby:alpine image (ruby, its standard library and bundled
gems, and the libraries they load) as a tar file (/disk/root.tar) next to a
BusyBox at /disk/busybox. The checks unpack it under /disk/r, make the device
nodes, chroot into it and run Ruby.

    rubydisk.py OUT.img IMAGE BUSYBOX --cache DIR

IMAGE is a Docker Hub image, NAME@sha256:DIGEST (images.mk pins ruby:alpine).
Its layers are fetched and cached as alpinedisk.py does it (in DIR, checked
against their digests each time they are used). The whole image root is kept
except the manual pages and the documentation.
"""
import io, os, sys, tarfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import alpinedisk

RUBY = "usr/local/bin/ruby"

def keep(name):
    return ("/share/doc/" not in name and "/share/man/" not in name and not name.startswith("usr/share/man/")
            and "/share/ri/" not in name)

def main():
    args = sys.argv[1:]
    cache = None
    if "--cache" in args:
        cache = args[args.index("--cache") + 1]
        del args[args.index("--cache"):args.index("--cache") + 2]
    out, image, busybox = args
    work = os.path.join(os.path.dirname(os.path.abspath(out)), "ruby")
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
    if RUBY not in files:
        sys.exit(f"rubydisk: {image} has no {RUBY}")
    env = dict(os.environ, MKDISK_FREE_MB="600")
    os.execve(sys.executable, [sys.executable, os.path.join(HERE, "mkdisk.py"), out,
                               f"busybox={busybox}", f"root.tar={tar}"], env)

main()
