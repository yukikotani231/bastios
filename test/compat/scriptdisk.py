#!/usr/bin/env python3
"""Writes the disk of one scripting-language lane of the compat suite
(perl.txt, ruby.txt, lua.txt): the root file system of a Docker Hub image
that holds the language (Alpine-based, so musl's programs), as a tar file
(/disk/root.tar) next to a BusyBox at /disk/busybox. The checks unpack it under /disk/s, make the device nodes, chroot into it
and run the interpreter and its package tools.

    scriptdisk.py OUT.img IMAGE BUSYBOX --cache DIR [--free MB]

IMAGE is NAME@sha256:DIGEST (the Makefile pins them). Its layers are fetched
and cached as alpinedisk.py does it (in DIR, checked against their digests
each time they are used). What a run never reads (manual pages, the
documentation and the ri data) is left out to keep the disk small.
"""
import io, os, sys, tarfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import alpinedisk

SKIP = ("usr/share/man/", "usr/share/doc/", "usr/share/info/", "usr/share/ri/", "usr/local/share/ri/",
        "usr/local/share/man/", "usr/local/share/doc/", "var/cache/", "lib/apk/", "usr/share/perl5/pod/",
        "usr/share/gtk-doc/")

def keep(name):
    return not name.startswith(SKIP)

def build(out, image, busybox, cache, free="256", extra=None):
    """Writes the disk OUT. extra(files, add) may add files to the root's
    tar: add(tarinfo, body) for each."""
    work = os.path.join(os.path.dirname(os.path.abspath(out)), "script-" + os.path.basename(out).split(".")[0])
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
        if extra:
            extra(files, lambda ti, body=None: t.addfile(ti, io.BytesIO(body)) if body is not None else t.addfile(ti))
    env = dict(os.environ, MKDISK_FREE_MB=free)
    os.execve(sys.executable, [sys.executable, os.path.join(HERE, "mkdisk.py"), out,
                               f"busybox={busybox}", f"root.tar={tar}"], env)

def main():
    args = sys.argv[1:]
    cache = free = None
    if "--cache" in args:
        cache = args[args.index("--cache") + 1]
        del args[args.index("--cache"):args.index("--cache") + 2]
    free = "256"
    if "--free" in args:
        free = args[args.index("--free") + 1]
        del args[args.index("--free"):args.index("--free") + 2]
    out, image, busybox = args
    build(out, image, busybox, cache, free)

if __name__ == "__main__":
    main()
