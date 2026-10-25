#!/usr/bin/env python3
"""Writes the JDK disk for the compat suite (test/compat/jdk.txt): the JDK of
the official eclipse-temurin alpine image (java, javac, jar and the modules
file lib/modules), as a tar file (/disk/root.tar) next to a BusyBox at
/disk/busybox. The checks unpack it under /disk/r, make the device nodes,
chroot into it and compile and run Java programs.

    jdkdisk.py OUT.img IMAGE BUSYBOX --cache DIR [--jdtls TGZ]

IMAGE is a Docker Hub image, NAME@sha256:DIGEST (the Makefile pins
eclipse-temurin). Its layers are fetched and cached as alpinedisk.py does it
(in DIR, checked against their digests each time they are used). The whole
image root is kept except the documentation, manual pages, jmods and src.zip.
With --jdtls, the Eclipse JDT LS release tarball is added as /disk/jdtls.tar
(its plugins and the Linux configuration, under opt/jdtls).
"""
import io, os, sys, tarfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import alpinedisk

JAVAC = "opt/java/openjdk/bin/javac"

def keep(name):
    # The JDK's jmods, legal notices, manual pages and src.zip are not needed
    # to compile or run: they would only fill the disk.
    return ("/share/doc/" not in name and "/share/man/" not in name and not name.startswith("usr/share/man/")
            and not name.startswith("opt/java/openjdk/jmods/") and not name.startswith("opt/java/openjdk/legal/")
            and not name.startswith("opt/java/openjdk/man/") and name != "opt/java/openjdk/lib/src.zip")

def main():
    args = sys.argv[1:]
    cache = None
    if "--cache" in args:
        cache = args[args.index("--cache") + 1]
        del args[args.index("--cache"):args.index("--cache") + 2]
    jdtls = None
    if "--jdtls" in args:
        jdtls = args[args.index("--jdtls") + 1]
        del args[args.index("--jdtls"):args.index("--jdtls") + 2]
    out, image, busybox = args
    work = os.path.join(os.path.dirname(os.path.abspath(out)), "jdk")
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
    if JAVAC not in files:
        sys.exit(f"jdkdisk: {image} has no {JAVAC}")
    extra = []
    if jdtls:
        # Eclipse JDT LS (the language server): the launcher and its bundles
        # and the Linux configuration, as /opt/jdtls once unpacked in the root.
        jt = os.path.join(work, "jdtls.tar")
        with tarfile.open(jdtls, "r:gz") as src, tarfile.open(jt, "w") as dst:
            for m in src:
                top = m.name.lstrip("./").split("/")[0]
                if top in ("plugins", "config_linux"):
                    m.name = "opt/jdtls/" + m.name.lstrip("./")
                    dst.addfile(m, src.extractfile(m) if m.isfile() else None)
        extra = [f"jdtls.tar={jt}"]
    env = dict(os.environ, MKDISK_FREE_MB="1300")
    os.execve(sys.executable, [sys.executable, os.path.join(HERE, "mkdisk.py"), out,
                               f"busybox={busybox}", f"root.tar={tar}"] + extra, env)

main()
