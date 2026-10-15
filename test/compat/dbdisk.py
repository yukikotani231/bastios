#!/usr/bin/env python3
"""Writes the database disk for the compat suite (test/compat/db.txt): the
servers of the official postgres:alpine, redis:alpine and valkey:alpine
images, each as a tar file (/disk/postgres.tar, /disk/redis.tar,
/disk/valkey.tar), next to Alpine's BusyBox (the redis image's own) at
/disk/busybox. The checks unpack a tar file, make the device nodes, chroot
into the root and run the server.

    dbdisk.py OUT.img POSTGRES_IMAGE REDIS_IMAGE VALKEY_IMAGE --cache DIR

The images are Docker Hub images, NAME@sha256:DIGEST (images.mk pins them).
Their layers are fetched and cached as alpinedisk.py does it. What a server
never reads at run time is left out to keep the disk small: headers, manuals,
and LLVM with the JIT module that loads it (the checks run with jit=off).
"""
import io, os, sys, tarfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import alpinedisk

SKIP = ("usr/local/include/", "usr/include/", "usr/share/man/", "usr/local/share/man/",
        "usr/local/share/doc/", "usr/share/doc/", "usr/lib/libLLVM", "usr/local/lib/postgresql/bitcode/",
        "usr/local/lib/postgresql/llvmjit", "usr/lib/libapk", "sbin/apk", "usr/share/apk/", "lib/apk/")

def pack(image, cache, out):
    """The image's files, less SKIP, as a tar file (symbolic links and modes kept)."""
    files = alpinedisk.merged(alpinedisk.layers(image, cache))
    with tarfile.open(out, "w") as tf:
        for name, (ti, body) in sorted(files.items()):
            if name.startswith(SKIP) or "/bitcode/" in name:
                continue
            ti.uid = ti.gid = 0
            ti.uname = ti.gname = ""
            tf.addfile(ti, io.BytesIO(body) if ti.isfile() else None)

def main():
    args = sys.argv[1:]
    cache = None
    if "--cache" in args:
        cache = args[args.index("--cache") + 1]
        del args[args.index("--cache"):args.index("--cache") + 2]
    out, pg, redis, valkey = args
    work = os.path.join(os.path.dirname(os.path.abspath(out)), "db")
    cache = cache or work
    os.makedirs(work, exist_ok=True)
    os.makedirs(cache, exist_ok=True)
    p, r = os.path.join(work, "postgres.tar"), os.path.join(work, "redis.tar")
    pack(pg, cache, p)
    pack(redis, cache, r)
    v = os.path.join(work, "valkey.tar")
    pack(valkey, cache, v)
    busybox = os.path.join(work, "busybox")
    with tarfile.open(r) as tf, open(busybox, "wb") as f:
        f.write(tf.extractfile("bin/busybox").read())
    env = dict(os.environ, MKDISK_FREE_MB="512")
    os.execve(sys.executable, [sys.executable, os.path.join(HERE, "mkdisk.py"), out,
                               f"busybox={busybox}", p, r, v], env)

main()
