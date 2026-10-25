#!/usr/bin/env python3
"""Writes the Python disk for the compat suite (test/compat/python.txt): the
root file system of the official python:alpine image (Alpine's minirootfs
with CPython in /usr/local: pip, the headers, the extension modules) as a
tar file, /disk/root.tar, next to Alpine's BusyBox at /disk/busybox. The
checks unpack the root, install gcc with apk over HTTP (the repository
alpinedisk.py makes), and develop in it: venv, pip, building a C extension,
a project's test suite.

    pythondisk.py OUT.img IMAGE BUSYBOX WHEELS --cache DIR

IMAGE is a Docker Hub image, NAME@sha256:DIGEST (the Makefile pins
python:3.14-alpine). Its layers are fetched and cached as alpinedisk.py does
it. WHEELS is the directory the Python packages of pythondeps.txt are
fetched into (checked against their SHA-256 each time), which run.py serves
over HTTP.
"""
import hashlib, io, os, sys, tarfile, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import alpinedisk

def wheelhouse(wheels):
    os.makedirs(wheels, exist_ok=True)
    for line in open(os.path.join(HERE, "pythondeps.txt")):
        if not line.strip() or line.startswith("#"):
            continue
        name, url, digest = line.split()
        path = os.path.join(wheels, name)
        if os.path.exists(path) and hashlib.sha256(open(path, "rb").read()).hexdigest() == digest:
            continue
        data = urllib.request.urlopen(url).read()
        if hashlib.sha256(data).hexdigest() != digest:
            sys.exit(f"pythondisk: {url} does not match its SHA-256")
        with open(path, "wb") as f:
            f.write(data)

def main():
    args = sys.argv[1:]
    cache = None
    if "--cache" in args:
        cache = args[args.index("--cache") + 1]
        del args[args.index("--cache"):args.index("--cache") + 2]
    out, image, busybox, wheels = args
    work = os.path.join(os.path.dirname(os.path.abspath(out)), "pythondisk")
    os.makedirs(work, exist_ok=True)
    cache = cache or work
    wheelhouse(wheels)
    files = alpinedisk.merged(alpinedisk.layers(image, cache))
    if "usr/local/bin/python3" not in files:
        sys.exit(f"pythondisk: {image} has no /usr/local/bin/python3")
    tar = os.path.join(work, "root.tar")
    with tarfile.open(tar, "w") as t:
        for name, (ti, body) in files.items():
            ti.name = name
            t.addfile(ti, io.BytesIO(body) if ti.isfile() else None)
    env = dict(os.environ, MKDISK_FREE_MB="900")
    os.execve(sys.executable, [sys.executable, os.path.join(HERE, "mkdisk.py"), out,
                               f"busybox={busybox}", tar], env)

main()
