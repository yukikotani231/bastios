#!/usr/bin/env python3
"""Writes the ALSA disk for the compat suite (test/compat/alsa.txt): the
sound utilities of Ubuntu 24.04 (alsa-utils 1.2.9, alsa-lib 1.2.11), with
the glibc and the libraries they link against, as a tar file
(/disk/root.tar) next to a static BusyBox at /disk/busybox. The checks
unpack the root under /disk/s, chroot into it and play sound on the HD
Audio card the lane's guest has.

    alsadisk.py OUT.img --cache DIR

Every package is a .deb of Ubuntu's release pocket, which the archive keeps,
pinned below by its SHA-256 and kept in DIR (checked each time it is used),
so the archive is asked only for what the cache lacks. The Ubuntu snapshot
service is the fallback. The packages are unpacked with dpkg-deb.
"""
import hashlib, os, shutil, subprocess, sys, tarfile, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
MIRRORS = ("https://archive.ubuntu.com/ubuntu/", "https://snapshot.ubuntu.com/ubuntu/20240425T000000Z/")
PACKAGES = (
    ("pool/main/b/busybox/busybox-static_1.36.1-6ubuntu3_amd64.deb",
     "9936a597976a42088f977bfb204b84f02615feb42bb732ff1fdb8bef5b5c13f4"),
    ("pool/main/a/alsa-utils/alsa-utils_1.2.9-1ubuntu5_amd64.deb",
     "964331366e162595a595be0d4789e30fac939688006e0f0f569b53b0f6568716"),
    ("pool/main/n/ncurses/libtinfo6_6.4+20240113-1ubuntu2_amd64.deb",
     "53e1e1753729d04cf65b05e6e58abe06e2bb76cc07eff0e1b2a638a638ca209b"),
    ("pool/main/g/glibc/libc6_2.39-0ubuntu8_amd64.deb",
     "af36c7ac770770fe3d3c10e85d6bc538e76e57570ba7db7d397fb9f654783ef3"),
    ("pool/main/g/gcc-14/libgcc-s1_14-20240412-0ubuntu1_amd64.deb",
     "a39efdcaa2245f026dc3254ce14fcff255fc430a17064632b6ba7c5da974a912"),
    ("pool/main/g/gcc-14/gcc-14-base_14-20240412-0ubuntu1_amd64.deb",
     "bc5a682c694a1ce5af50772b7c737a21e9a74f0f0edaae98e97bc9c4af583fef"),
    ("pool/main/libs/libsamplerate/libsamplerate0_0.2.2-4build1_amd64.deb",
     "8a389d1cae81860c9fb7cd9641c3f9d38815e80c0379b3181b4317489126f738"),
    ("pool/main/n/ncurses/libncursesw6_6.4+20240113-1ubuntu2_amd64.deb",
     "96d1e64bb1a80f2005f41c4112d10e4756a222a6c245e9383fc8da924bd40bf9"),
    ("pool/main/f/fftw3/libfftw3-single3_3.3.10-1ubuntu3_amd64.deb",
     "07d67e30d98e41e00ce9edcfd885e0ba7f4fe99fcf390f84726a69144b17cf2b"),
    ("pool/main/g/gcc-14/libgomp1_14-20240412-0ubuntu1_amd64.deb",
     "cc2ca1193d2c11d7383bdb03dd333cc621d128237b82dafeb8d7f2bd8302f3dd"),
    ("pool/main/a/alsa-lib/libatopology2t64_1.2.11-1build2_amd64.deb",
     "bc11e5adaafafffd4b35a925678b2cd46286e47fb5e51c641d854f91c5b1ccc6"),
    ("pool/main/a/alsa-lib/libasound2t64_1.2.11-1build2_amd64.deb",
     "c2f0caa30869876791ba349bef4907d5cfec47b8cfd1ae9889a05dce0feb7c39"),
    ("pool/main/a/alsa-lib/libasound2-data_1.2.11-1build2_all.deb",
     "a9e6326591ad5867f0783367f4708f145627ec95edfd8124d70d78b7516db4f1"),
    ("pool/main/k/kmod/kmod_31+20240202-2ubuntu7_amd64.deb",
     "91451843605387ed51c53dcd86816027399a03fc7173d85cab51a0f035877a63"),
    ("pool/main/k/kmod/libkmod2_31+20240202-2ubuntu7_amd64.deb",
     "43858c80a67c3a8a5a10d8e6769e7d92614d9eb19cb14e159b4311a16b9cf54b"),
    ("pool/main/libz/libzstd/libzstd1_1.5.5+dfsg2-2build1_amd64.deb",
     "4dcc3ba9606a5837a55248d08162a5b426dd621f75c0eb24d86409e0d0e71d52"),
    ("pool/main/o/openssl/libssl3t64_3.0.13-0ubuntu3_amd64.deb",
     "6cde53feabd28f26586a4db2ffb9fc84e7b0598aee15b33bee907b6392a92d24"),
    ("pool/main/x/xz-utils/liblzma5_5.6.1+really5.4.5-1_amd64.deb",
     "28851b3997e4ba74bb3197bd58796bd3041c204c21df85fe0ddafdcaced647f0"),
)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def fetch(path, digest, cache):
    dest = os.path.join(cache, digest + ".deb")
    if os.path.exists(dest) and sha256(dest) == digest:
        return dest
    for mirror in MIRRORS:
        try:
            with urllib.request.urlopen(mirror + path, timeout=120) as r, open(dest + ".part", "wb") as f:
                shutil.copyfileobj(r, f)
        except OSError as e:
            print(f"alsadisk: {mirror}{path}: {e}", file=sys.stderr)
            continue
        if sha256(dest + ".part") == digest:
            os.rename(dest + ".part", dest)
            return dest
        print(f"alsadisk: {mirror}{path}: wrong checksum", file=sys.stderr)
    sys.exit(f"alsadisk: cannot get {path}")


def main():
    args = sys.argv[1:]
    cache = None
    if "--cache" in args:
        cache = args[args.index("--cache") + 1]
        del args[args.index("--cache"):args.index("--cache") + 2]
    out, = args
    work = os.path.join(os.path.dirname(os.path.abspath(out)), "alsa")
    cache = cache or work
    os.makedirs(cache, exist_ok=True)
    shutil.rmtree(work + "/root", ignore_errors=True)
    shutil.rmtree(work + "/bb", ignore_errors=True)
    os.makedirs(work + "/root")
    os.makedirs(work + "/bb")
    for path, digest in PACKAGES:
        deb = fetch(path, digest, cache)
        subprocess.check_call(["dpkg-deb", "-x", deb, work + ("/bb" if "busybox" in path else "/root")])
    # Ubuntu merges /usr: /lib and /lib64 are links into it, which the debs do not carry.
    for link, to in (("lib", "usr/lib"), ("lib64", "usr/lib64"), ("bin", "usr/bin"), ("sbin", "usr/sbin")):
        if not os.path.lexists(f"{work}/root/{link}"):
            os.symlink(to, f"{work}/root/{link}")
    for d in ("dev", "proc", "tmp", "etc"):
        os.makedirs(f"{work}/root/{d}", exist_ok=True)
    root = work + "/root.tar"
    with tarfile.open(root, "w", format=tarfile.PAX_FORMAT) as t:
        t.add(work + "/root", arcname=".")
    env = dict(os.environ, MKDISK_FREE_MB="150")   # room for the unpacked root
    os.execve(sys.executable, [sys.executable, os.path.join(HERE, "mkdisk.py"), out,
                               f"busybox={work}/bb/usr/bin/busybox", f"root.tar={root}"], env)


if __name__ == "__main__":
    main()
