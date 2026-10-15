#!/usr/bin/env python3
"""Writes the server disk for the compat suite (test/compat/server.txt):
the root file systems of the official nginx, redis and postgres images
(their Alpine variants), each as a tar file (/disk/nginx.tar, redis.tar,
postgres.tar), next to the first image's BusyBox at /disk/busybox. The checks unpack a root,
make the device nodes, chroot into it and run the server there.

    serverdisk.py OUT.img [--extra FILE]... --cache DIR IMAGE...

Each IMAGE is a Docker Hub image, NAME@sha256:DIGEST (the Makefile pins
them); its tar file is named after the image (library/nginx -> nginx.tar).
The layers are fetched and cached as alpinedisk.py does it (in DIR, checked
against their digests each time they are used). A tar file keeps the disk's
directory short: the postgres root alone has about 4000 entries.
"""
import os, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import alpinedisk

# What a check never reads, left out to keep the disk small.
SKIP = ("usr/local/lib/postgresql/bitcode/", "usr/local/include/", "usr/share/doc/",
        "usr/share/man/", "usr/local/share/doc/", "usr/local/share/man/")

def keep(name):
    return not name.startswith(SKIP)

def main():
    args = sys.argv[1:]
    cache = None
    if "--cache" in args:
        cache = args[args.index("--cache") + 1]
        del args[args.index("--cache"):args.index("--cache") + 2]
    extra = []
    while "--extra" in args:
        extra.append(args[args.index("--extra") + 1])
        del args[args.index("--extra"):args.index("--extra") + 2]
    out, images = args[0], args[1:]
    work = os.path.join(os.path.dirname(os.path.abspath(out)), "server")
    cache = cache or work
    os.makedirs(cache, exist_ok=True)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    disk = []
    for image in images:
        name = image.split("@")[0].rsplit("/", 1)[-1]
        files = alpinedisk.merged(alpinedisk.layers(image, cache))
        if not disk:
            bb = os.path.join(work, "busybox")
            with open(bb, "wb") as f:
                f.write(files["bin/busybox"][1])
            os.chmod(bb, 0o755)
            disk.append(bb)
        entries = [(ti, body if ti.isfile() else None)
                   for n, (ti, body) in sorted(files.items()) if keep(n)]
        for ti, body in entries:
            ti.name = ti.name.lstrip("./")
        path = os.path.join(work, name + ".tar")
        with open(path, "wb") as f:
            f.write(alpinedisk.tar(entries))
        disk.append(path)
        print(f"serverdisk: {name}: {len(entries)} entries, {os.path.getsize(path) >> 20} MiB",
              file=sys.stderr)
    disk += extra                  # stored under their base names
    # Room for an unpacked root and what a server writes (a database).
    env = dict(os.environ, MKDISK_FREE_MB="1200")
    subprocess.run([os.path.join(HERE, "mkdisk.py"), out] + disk, check=True, env=env)

main()
