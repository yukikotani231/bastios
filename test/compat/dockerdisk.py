#!/usr/bin/env python3
"""Writes the Docker disk for the compat suite (test/compat/docker.txt): the
root of the docker:dind image (dockerd with its built-in containerd, the
docker client, runc and their configuration), as a tar file
(/disk/root.tar) next to a BusyBox at /disk/busybox. The checks unpack it
under /disk/p, set up the device nodes and mounts, start dockerd and run
containers there.

    dockerdisk.py OUT.img IMAGE BUSYBOX --cache DIR

IMAGE is a Docker Hub image, NAME@sha256:DIGEST (the Makefile pins it). Its
layers are fetched and cached as alpinedisk.py does it. The whole image root
is kept except documentation and manual pages.
"""
import io, os, sys, tarfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import alpinedisk

DOCKERD = "usr/local/bin/dockerd"

def keep(name):
    return "/share/doc/" not in name and "/share/man/" not in name and not name.startswith("usr/share/man/")

def main():
    args = sys.argv[1:]
    cache = None
    if "--cache" in args:
        cache = args[args.index("--cache") + 1]
        del args[args.index("--cache"):args.index("--cache") + 2]
    out, image, busybox = args
    work = os.path.join(os.path.dirname(os.path.abspath(out)), "docker")
    cache = cache or work
    os.makedirs(cache, exist_ok=True)
    files = alpinedisk.merged(alpinedisk.layers(image, cache))
    if DOCKERD not in files:
        sys.exit(f"dockerdisk: {image} has no {DOCKERD}")
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
    env = dict(os.environ, MKDISK_FREE_MB="600")
    os.execve(sys.executable, [sys.executable, os.path.join(HERE, "mkdisk.py"), out,
                               f"busybox={busybox}", f"root.tar={tar}"], env)

main()
