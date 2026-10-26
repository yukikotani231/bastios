#!/usr/bin/env python3
"""Writes the Go disk for the compat suite (test/compat/go.txt): the Go
toolchain of the official golang:alpine image, as a root file system under
/disk/g, next to a BusyBox at /disk/busybox. The checks make the device
nodes, chroot into it and build and run Go programs.

    godisk.py OUT.img IMAGE BUSYBOX --cache DIR [--gopls FILE]

IMAGE is a Docker Hub image, NAME@sha256:DIGEST (the Makefile pins
golang:1.24-alpine). Its layers are fetched and cached as alpinedisk.py does
it (in DIR, checked against their digests each time they are used). The Go
tools are static programs, so the root holds only /usr/local/go (and gopls, in
/usr/local/bin, when --gopls names a build of it); what a
build never reads (the tests, the documentation, the test data of the
standard library) is left out to keep the disk small.
"""
import os, shutil, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import alpinedisk

GO = "usr/local/go/"
SKIP = ("usr/local/go/test/", "usr/local/go/doc/", "usr/local/go/misc/", "usr/local/go/api/",
        "usr/local/go/src/cmd/")
TOOLS = ("compile", "link", "asm", "buildid", "pack", "cgo")

def keep(name):
    if not name.startswith(GO) or name.startswith(SKIP):
        return False
    if "/pkg/tool/" in name and name.rsplit("/", 1)[-1] not in TOOLS + ("linux_amd64",):
        return False
    return "/testdata/" not in name and not name.endswith("_test.go")

def main():
    args = sys.argv[1:]
    cache = None
    if "--cache" in args:
        cache = args[args.index("--cache") + 1]
        del args[args.index("--cache"):args.index("--cache") + 2]
    gopls = None
    if "--gopls" in args:
        gopls = args[args.index("--gopls") + 1]
        del args[args.index("--gopls"):args.index("--gopls") + 2]
    out, image, busybox = args
    work = os.path.join(os.path.dirname(os.path.abspath(out)), "go")
    cache = cache or work
    os.makedirs(cache, exist_ok=True)
    files = alpinedisk.merged(alpinedisk.layers(image, cache))
    root = os.path.join(work, "root")
    shutil.rmtree(root, ignore_errors=True)
    n = 0
    for name, (ti, body) in sorted(files.items()):
        if not keep(name):
            continue
        path = os.path.join(root, name)
        if ti.isdir():
            os.makedirs(path, exist_ok=True)
        elif ti.isfile():
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(body)
            os.chmod(path, 0o755 if ti.mode & 0o111 else 0o644)
            n += 1
        elif ti.issym():
            os.makedirs(os.path.dirname(path), exist_ok=True)
            os.symlink(ti.linkname, path)
    if not os.path.exists(os.path.join(root, GO, "bin", "go")):
        sys.exit(f"godisk: {image} has no {GO}bin/go")
    if gopls:
        os.makedirs(os.path.join(root, "usr/local/bin"), exist_ok=True)
        shutil.copyfile(gopls, os.path.join(root, "usr/local/bin/gopls"))
        os.chmod(os.path.join(root, "usr/local/bin/gopls"), 0o755)
    for d in ("dev", "proc", "tmp", "etc"):
        os.makedirs(os.path.join(root, d), exist_ok=True)
    open(os.path.join(root, "etc", "hosts"), "w").write("127.0.0.1 localhost\n")
    env = dict(os.environ, MKDISK_FREE_MB="256")
    os.execve(sys.executable, [sys.executable, os.path.join(HERE, "mkdisk.py"), out,
                               f"busybox={busybox}", f"g={root}"], env)

if __name__ == "__main__":
    main()
