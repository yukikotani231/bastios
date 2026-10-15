#!/usr/bin/env python3
"""Writes the Rust disk for the compat suite (test/compat/rust.txt): the
toolchain of the official rust:alpine image (rustc, cargo, the standard
library, and Alpine's gcc and musl, which rustc links with), as a tar file
(/disk/root.tar) next to a BusyBox at /disk/busybox. The checks unpack it
under /disk/r, make the device nodes, chroot into it and build and run Rust
programs.

    rustdisk.py OUT.img IMAGE BUSYBOX --cache DIR

IMAGE is a Docker Hub image, NAME@sha256:DIGEST (the Makefile pins
rust:alpine). Its layers are fetched and cached as alpinedisk.py does it (in
DIR, checked against their digests each time they are used). The whole
image root is kept except the documentation and manual pages.
"""
import hashlib, io, os, re, sys, tarfile, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import alpinedisk

RUSTC = "usr/local/cargo/bin/rustc"

def crates(lock, cache):
    """The registry crates of a Cargo.lock as a tar of a vendor directory
    (what `cargo vendor` writes), every crate checked against the checksum
    in the lock and cached in CACHE. The guest has no network."""
    out = io.BytesIO()
    with tarfile.open(fileobj=out, mode="w") as t:
        for block in open(lock).read().split("[[package]]")[1:]:
            f = dict(re.findall(r'^(\w+) = "(.*)"$', block, re.M))
            if "checksum" not in f:
                continue
            name, ver, want = f["name"], f["version"], f["checksum"]
            path = os.path.join(cache, f"crate-{want}.crate")
            if not os.path.exists(path):
                url = f"https://static.crates.io/crates/{name}/{name}-{ver}.crate"
                with urllib.request.urlopen(url, timeout=120) as r:
                    open(path, "wb").write(r.read())
            data = open(path, "rb").read()
            if hashlib.sha256(data).hexdigest() != want:
                sys.exit(f"rustdisk: {name} {ver} does not match the checksum in {lock}")
            with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as c:
                for m in c.getmembers():
                    m.uid = m.gid = 0
                    m.uname = m.gname = ""
                    m.name = "vendor/" + m.name
                    t.addfile(m, c.extractfile(m) if m.isfile() else None)
            body = ('{"files":{},"package":"%s"}' % want).encode()
            ti = tarfile.TarInfo(f"vendor/{name}-{ver}/.cargo-checksum.json")
            ti.size = len(body)
            t.addfile(ti, io.BytesIO(body))
    return out.getvalue()

def keep(name):
    return "/share/doc/" not in name and "/share/man/" not in name and not name.startswith("usr/share/man/")

def main():
    args = sys.argv[1:]
    cache = None
    if "--cache" in args:
        cache = args[args.index("--cache") + 1]
        del args[args.index("--cache"):args.index("--cache") + 2]
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
    vendor = os.path.join(work, "vendor.tar")
    open(vendor, "wb").write(crates(os.path.join(HERE, "rustcrate", "Cargo.lock"), cache))
    if RUSTC not in files:
        sys.exit(f"rustdisk: {image} has no {RUSTC}")
    env = dict(os.environ, MKDISK_FREE_MB="1300")
    os.execve(sys.executable, [sys.executable, os.path.join(HERE, "mkdisk.py"), out,
                               f"busybox={busybox}", f"root.tar={tar}", f"vendor.tar={vendor}",
                               f"crate={HERE}/rustcrate"], env)

main()
