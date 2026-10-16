#!/usr/bin/env python3
"""Writes the Alpine disk for the compat suite (test/compat/alpine.txt):
Alpine Linux's root file system as a tar file (/disk/root.tar) and a local
package repository (/disk/a/repo/ARCH: .apk files and an APKINDEX; and
/disk/a/repo2/ARCH, ARCH being x86_64, or aarch64 with COMPAT_ARCH=aarch64), next to Alpine's own BusyBox at /disk/busybox. The checks unpack the root under
/disk/a, chroot into it and run apk there.

    alpinedisk.py OUT.img WORK_DIR BASE_IMAGE PKG_IMAGE... --disk NAME,... [--cache DIR] [--busybox FILE]

--busybox FILE is the BusyBox put at /disk/busybox in place of Alpine's own:
Alpine's is dynamically linked, and a kernel without the built-in musl
dynamic linker (AArch64) cannot start it outside the root.

Every package the PKG_IMAGEs hold goes in a repository WORK_DIR/disk/http,
which run.py serves over HTTP (alpine.txt's ALPINEURL): it is larger than
a disk. The disk gets the packages named after --disk, with everything they
depend on that the root lacks, so that the checks install them with no
network; and the test packages made here (SYNTH: version 1 in /repo,
version 2 in /repo2).

The images are Docker Hub's (NAME@sha256:DIGEST, pinned in the Makefile):
BASE_IMAGE is Alpine's minirootfs, and each PKG_IMAGE an image that apk
installed packages into. Alpine's own mirrors serve the packages too, but
this machine's network reaches only Docker Hub, so the packages are rebuilt
from what the images hold: every file a package installed (as listed in
/lib/apk/db/installed, with its owner and mode) goes back into an .apk with
the package's metadata. The binaries are Alpine's; the packages and the
index are not signed by Alpine, so apk needs --allow-untrusted.

The image layers are kept in the cache directory (--cache, WORK_DIR when
it is not given) under their digests, with each image's layer list, and
are checked against their digests each time they are used. Docker Hub is
asked only for what the cache lacks, so a cache kept outside the build
directory (ALPINE_CACHE in the Makefile) spares Docker Hub's rate limit
on fresh checkouts; a 429 from it is waited out and retried.
"""
import base64, zipfile, gzip, hashlib, http.client, io, json, os, re, shutil, subprocess, sys, tarfile, time, urllib.error, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
# The guest's processor (COMPAT_ARCH, as run.py's --arch): apk's name for it
# and Docker Hub's.
ARCH = os.environ.get("COMPAT_ARCH", "x86_64")
DOCKER_ARCH = {"x86_64": "amd64", "aarch64": "arm64"}[ARCH]
REGISTRY = "https://registry-1.docker.io/v2"
ACCEPT = ", ".join([
    "application/vnd.oci.image.index.v1+json",
    "application/vnd.docker.distribution.manifest.list.v2+json",
    "application/vnd.oci.image.manifest.v1+json",
    "application/vnd.docker.distribution.manifest.v2+json"])

# ---------------------------------------------------------------- Docker Hub

def opened(req):
    """urlopen, waiting out Docker Hub's rate limit (429): as long as it
    says, or a minute, up to ten times."""
    for i in range(10):
        try:
            return urllib.request.urlopen(req)
        except urllib.error.HTTPError as e:
            if e.code != 429 or i == 9:
                raise
            wait = e.headers.get("Retry-After", "60")
            wait = int(wait) if wait.isdigit() else 60
            print(f"alpinedisk: Docker Hub's rate limit; waiting {wait} s", file=sys.stderr)
            time.sleep(min(wait, 600))

def fetch(url, token, accept=None):
    h = {"Authorization": "Bearer " + token}
    if accept:
        h["Accept"] = accept
    with opened(urllib.request.Request(url, headers=h)) as r:
        return r.read()

def download(url, token, path, size):
    """Fetches url into path, resuming where a cut connection left off
    (large layers often are cut)."""
    with open(path, "wb"):
        pass
    for _ in range(20):
        have = os.path.getsize(path)
        if have >= size:
            return
        h = {"Authorization": "Bearer " + token, "Range": f"bytes={have}-"}
        try:
            with opened(urllib.request.Request(url, headers=h)) as r, open(path, "ab") as f:
                if r.status != 206 and have:
                    f.truncate(0)
                while True:
                    chunk = r.read(1 << 20)
                    if not chunk:
                        break
                    f.write(chunk)
        except (OSError, http.client.HTTPException):
            pass
    sys.exit(f"alpinedisk: {url}: the download keeps failing")

def sound(path, digest):
    """Whether the file at path is there and has sha256 digest `digest`."""
    if not os.path.exists(path):
        return False
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return "sha256:" + h.hexdigest() == digest

def layers(image, cache):
    """The layers of an image NAME@sha256:DIGEST for ARCH, as paths of gzip
    tar files in the cache, base first. Each blob is checked against its
    digest, when it is fetched and each time it comes from the cache."""
    name, digest = image.split("@")
    # Docker Hub limits how many manifests one address may fetch, so the
    # layer list is kept too, by the image's digest: the layers' digests.
    listed = os.path.join(cache, digest.replace(":", "-") + ("" if ARCH == "x86_64" else "-" + DOCKER_ARCH) + ".json")
    if os.path.exists(listed):
        # Older lists hold paths; a layer's file name is its digest either way.
        ds = [os.path.basename(p)[:-len(".tar.gz")].replace("-", ":", 1) for p in json.load(open(listed))]
        paths = [os.path.join(cache, d.replace(":", "-") + ".tar.gz") for d in ds]
        if all(sound(p, d) for p, d in zip(paths, ds)):
            return paths
    token = json.load(opened(
        "https://auth.docker.io/token?service=registry.docker.io&scope=repository:" + name + ":pull"))["token"]
    m = json.loads(fetch(f"{REGISTRY}/{name}/manifests/{digest}", token, ACCEPT))
    if "manifests" in m:
        d = [x for x in m["manifests"] if x.get("platform", {}).get("architecture") == DOCKER_ARCH
             and x.get("platform", {}).get("os") == "linux"][0]["digest"]
        m = json.loads(fetch(f"{REGISTRY}/{name}/manifests/{d}", token, ACCEPT))
    out = []
    for l in m["layers"]:
        path = os.path.join(cache, l["digest"].replace(":", "-") + ".tar.gz")
        if not sound(path, l["digest"]):
            download(f"{REGISTRY}/{name}/blobs/{l['digest']}", token, path + ".part", l["size"])
            with open(path + ".part", "rb") as f:
                if "sha256:" + hashlib.sha256(f.read()).hexdigest() != l["digest"]:
                    sys.exit(f"alpinedisk: {image}: layer {l['digest']} does not match its digest")
            os.rename(path + ".part", path)
        out.append(path)
    with open(listed + ".part", "w") as f:
        json.dump([l["digest"].replace(":", "-") + ".tar.gz" for l in m["layers"]], f)
    os.rename(listed + ".part", listed)
    return out

def merged(paths):
    """The image's files: path -> (TarInfo, contents), the layers applied
    in order with their whiteouts."""
    files = {}
    for p in paths:
        with tarfile.open(p, "r:gz") as t:
            for ti in t:
                name = ti.name.lstrip("./")
                base = os.path.basename(name)
                if base == ".wh..wh..opq":
                    d = os.path.dirname(name) + "/"
                    for k in [k for k in files if k.startswith(d)]:
                        del files[k]
                    continue
                if base.startswith(".wh."):
                    gone = os.path.join(os.path.dirname(name), base[4:])
                    for k in [k for k in files if k == gone or k.startswith(gone + "/")]:
                        del files[k]
                    continue
                body = t.extractfile(ti).read() if ti.isfile() else b""
                files[name] = (ti, body)
    return files

# ---------------------------------------------------------------- packages

def installed(db):
    """The packages in apk's installed database: dicts of its fields, with
    "dirs" ([path, acl]) and "files" ([dir, name, acl])."""
    pkgs, cur, last = [], None, None
    for line in db.decode("utf-8", "surrogateescape").split("\n"):
        if not line:
            cur = None
            continue
        k, v = line[0], line[2:]
        if cur is None:
            cur = {"dirs": [], "files": []}
            pkgs.append(cur)
        if k == "F":
            cur["dirs"].append([v, None])
        elif k == "M" and last == "F":
            cur["dirs"][-1][1] = v
        elif k == "R":
            cur["files"].append([cur["dirs"][-1][0] if cur["dirs"] else "", v, None])
        elif k == "a" and last == "R":
            cur["files"][-1][2] = v
        elif k not in "MaZ":
            cur.setdefault(k, v)
        last = k
    return pkgs

PKGINFO = [("P", "pkgname"), ("V", "pkgver"), ("T", "pkgdesc"), ("U", "url"),
           ("t", "builddate"), ("m", "packager"), ("I", "size"), ("A", "arch"),
           ("o", "origin"), ("c", "commit"), ("L", "license"), ("k", "provider_priority")]
LISTS = [("D", "depend"), ("p", "provides"), ("r", "replaces"), ("i", "install_if")]

def gz(b):
    # Level 6, not gzip's default 9: on the 1.7 GB the images' packages
    # hold, 9 takes about 3.5 times as long for 1% less.
    o = io.BytesIO()
    with gzip.GzipFile(fileobj=o, mode="wb", mtime=0, compresslevel=6) as f:
        f.write(b)
    return o.getvalue()

def tar(entries, cut=False):
    o = io.BytesIO()
    with tarfile.open(fileobj=o, mode="w", format=tarfile.PAX_FORMAT) as t:
        for ti, body in entries:
            t.addfile(ti, io.BytesIO(body) if body is not None else None)
    b = o.getvalue()
    if cut:                        # as abuild-tar --cut: no end-of-archive blocks
        b = b.rstrip(b"\0")
        b += b"\0" * (-len(b) % 512)
    return b

def owned(ti, acl, default):
    ti.uid, ti.gid, ti.mode = 0, 0, default
    if acl:
        uid, gid, mode = acl.split(":")[:3]
        ti.uid, ti.gid, ti.mode = int(uid), int(gid), int(mode, 8)
    return ti

def scripts(files):
    """The install scripts apk kept (lib/apk/db/scripts.tar.gz, named
    NAME-VERSION.HASH.KIND) and the trigger paths (lib/apk/db/triggers,
    by the package's C field): ({"NAME-VERSION": [(KIND, body)]},
    {C: [path]})."""
    kept, trig = {}, {}
    for name in ("lib/apk/db/scripts.tar.gz", "lib/apk/db/scripts.tar"):
        if name in files:
            with tarfile.open(fileobj=io.BytesIO(files[name][1])) as t:
                for m in t.getmembers():
                    if m.isfile():
                        pv, _, kind = m.name.rsplit(".", 2)
                        kept.setdefault(pv, []).append((kind, t.extractfile(m).read()))
    if "lib/apk/db/triggers" in files:
        for line in files["lib/apk/db/triggers"][1].decode().split("\n"):
            if line.strip():
                c, *paths = line.split()
                trig[c] = paths
    return kept, trig

# Documentation and data the checks never read, left out of the packages: a
# disk holds a bounded number of directory entries (src/diskfs.bastio's
# DSLOTS), and these are 2100 of cmake's 3400 files and 700 of
# lua-language-server's 1300.
PRUNE = ("usr/share/cmake/Help", "usr/lib/lua-language-server/meta/3rd")

def pruned(path):
    return any(path == x or path.startswith(x + "/") for x in PRUNE)

def package(files, p, kept=({}, {})):
    """The .apk for package p: a gzip stream of its .PKGINFO and install
    scripts (from kept, as scripts() gives them), then one of its files.
    Returns (control stream, whole package)."""
    when = int(p.get("t", 0))
    data = []
    for d, acl in p["dirs"]:
        if pruned(d):
            continue
        ti = owned(tarfile.TarInfo(d), acl, 0o755)
        ti.type, ti.mtime = tarfile.DIRTYPE, when
        data.append((ti, None))
    for d, name, acl in p["files"]:
        path = f"{d}/{name}" if d else name
        if pruned(path):
            continue
        if path not in files:
            sys.exit(f"alpinedisk: {p['P']}: {path} is not in the image")
        src, body = files[path]
        ti = owned(tarfile.TarInfo(path), acl, 0o644)
        ti.mtime = when
        if src.issym():
            ti.type, ti.linkname = tarfile.SYMTYPE, src.linkname
            body = src.linkname.encode()
            ti.pax_headers = {"APK-TOOLS.checksum.SHA1": hashlib.sha1(body).hexdigest()}
            data.append((ti, None))
        elif src.islnk():
            ti.type, ti.linkname = tarfile.LNKTYPE, src.linkname.lstrip("./")
            data.append((ti, None))
        else:
            ti.size = len(body)
            ti.pax_headers = {"APK-TOOLS.checksum.SHA1": hashlib.sha1(body).hexdigest()}
            data.append((ti, body))
    dgz = gz(tar(data))
    info = "".join(f"{n} = {p[k]}\n" for k, n in PKGINFO if k in p)
    for k, n in LISTS:
        info += "".join(f"{n} = {v}\n" for v in p.get(k, "").split())
    info += "".join(f"triggers = {t}\n" for t in kept[1].get(p.get("C"), []))
    info += f"datahash = {hashlib.sha256(dgz).hexdigest()}\n"
    ti = tarfile.TarInfo(".PKGINFO")
    ti.size, ti.mode, ti.mtime = len(info.encode()), 0o644, when
    control = [(ti, info.encode())]
    # The scripts: a package's pre-install adds the users it needs, say.
    for kind, body in kept[0].get(f"{p['P']}-{p['V']}", []):
        ti = tarfile.TarInfo("." + kind)
        ti.size, ti.mode, ti.mtime = len(body), 0o755, when
        control.append((ti, body))
    cgz = gz(tar(control, cut=True))
    return cgz, cgz + dgz

# cmake for AArch64. The image the x86-64 suite takes cmake from has no arm64
# build, and Alpine's mirrors are not reachable from every machine, so the
# package is made here from the `cmake` wheel on PyPI (musllinux, aarch64): the
# binaries, their bundled C++ runtime and the Modules and Templates, with the
# Help pages left out (see PRUNE). It installs under /opt/cmake, which keeps the
# wheel's layout (the binaries find ../../cmake.libs and ../share by their own
# path), and puts wrappers for cmake and ctest in /usr/bin.
WHEEL_KEEP = ("cmake/data/bin/cmake", "cmake/data/bin/ctest", "cmake/data/share/cmake-", "cmake.libs/")

def wheel_package(path):
    """The cmake wheel at path as a package: (package dict, files, kept)."""
    z = zipfile.ZipFile(path)
    info = [n for n in z.namelist() if n.endswith(".dist-info/METADATA")][0]
    ver = info.split("/")[0][:-len(".dist-info")].split("-", 1)[1]
    files, p = {}, {"P": "cmake", "V": ver + "-r0", "T": "CMake, from the PyPI wheel", "A": ARCH,
                    "t": "1760140800", "L": "BSD-3-Clause", "U": "https://cmake.org", "o": "cmake",
                    "m": "BastiOS", "D": "", "p": "cmd:cmake=" + ver, "C": "cmake" + ver, "dirs": [], "files": []}
    dirs = set()
    def add(dest, ti, body, acl):
        d, n = dest.rsplit("/", 1)
        for i in range(1, d.count("/") + 2):
            dirs.add("/".join(d.split("/")[:i]))
        files[dest] = (ti, body)
        p["files"].append([d, n, acl])
    size = 0
    for info in z.infolist():
        n = info.filename
        if info.is_dir() or not n.startswith(WHEEL_KEEP):
            continue
        dest = "opt/cmake/" + n
        if "/Help/" in n:
            continue
        body = z.read(n)
        size += len(body)
        mode = "0:0:755" if n.startswith(("cmake/data/bin/", "cmake.libs/")) else None
        add(dest, tarfile.TarInfo(dest), body, mode)
    # Wrappers, not links: musl takes a program's $ORIGIN from the name it was
    # started by, not from where a link leads, and cmake finds its libraries by it.
    for tool in ("cmake", "ctest"):
        body = f"#!/bin/sh\nexec /opt/cmake/cmake/data/bin/{tool} \"$@\"\n".encode()
        add("usr/bin/" + tool, tarfile.TarInfo("usr/bin/" + tool), body, "0:0:755")
    p["I"] = str(size)
    p["dirs"] = [[d, None] for d in sorted(dirs)]
    return p, files, ({}, {})

# Small packages made here, to test what apk does around the files: a
# dependency, an upgrade with a version constraint, the install, upgrade
# and deinstall scripts (each appends to /var/log/bt.log), a trigger on a
# directory, and a dependency that nothing provides. Version 1 of each
# goes in the disk repository, version 2 in a second one.
APP = "#!/bin/sh\ncat /usr/share/bt/lib\necho app {v}\n"
LOG = "#!/bin/sh\necho {kind} \"$@\" >> /var/log/bt.log\n"
SYNTH = [
    # (name, version, depends, files {path: body}, scripts, triggers)
    ("bt-lib", "1.0-r0", "", {"usr/share/bt/lib": "lib 1.0\n"}, [], []),
    ("bt-app", "1.0-r0", "bt-lib", {"usr/bin/bt-app": APP.format(v="1.0")},
     ["post-install", "post-upgrade", "pre-deinstall", "post-deinstall"], []),
    ("bt-trig", "1.0-r0", "", {"usr/share/doc/bt-trig": "watches /usr/share/bt\n"},
     ["trigger"], ["/usr/share/bt"]),
    ("bt-broken", "1.0-r0", "bt-missing", {"usr/share/doc/bt-broken": "never installed\n"}, [], []),
]
SYNTH2 = [
    ("bt-lib", "2.0-r0", "", {"usr/share/bt/lib": "lib 2.0\n"}, [], []),
    ("bt-app", "2.0-r0", "bt-lib>=2.0", {"usr/bin/bt-app": APP.format(v="2.0")},
     ["post-install", "post-upgrade", "pre-deinstall", "post-deinstall"], []),
]

def synthetic(pkgs):
    """The packages of SYNTH or SYNTH2, as (package dict, files, kept) for
    package()."""
    out = []
    for name, ver, dep, body, kinds, trig in pkgs:
        files, p = {}, {"P": name, "V": ver, "T": "BastiOS compat test package", "A": ARCH,
                        "t": "1760140800", "L": "MIT", "U": "https://github.com/yukikotani231/bastios",
                        "o": name, "m": "BastiOS", "I": str(sum(len(b) for b in body.values())),
                        "D": dep, "C": name + ver, "dirs": [], "files": []}
        for path, b in body.items():
            d, n = path.rsplit("/", 1)
            for i in range(1, d.count("/") + 2):
                sub = "/".join(d.split("/")[:i])
                if [sub, None] not in p["dirs"]:
                    p["dirs"].append([sub, None])
            ti = tarfile.TarInfo(path)
            files[path] = (ti, b.encode())
            p["files"].append([d, n, "0:0:755" if d.endswith("bin") else None])
        kept = ({f"{name}-{ver}": [(k, LOG.format(kind=k).encode()) for k in kinds]},
                {name + ver: trig})
        if "trigger" in kinds:
            kept[0][f"{name}-{ver}"] = [("trigger", b"#!/bin/sh\necho trigger \"$@\" >> /var/log/bt.log\n")]
        out.append((p, files, kept))
    return out

def write_repo(o, pkgs, index=""):
    """A repository in directory o: each (p, files, kept) of pkgs as an
    .apk, and the APKINDEX.tar.gz, which lists index's entries too."""
    os.makedirs(o, exist_ok=True)
    for p, files, kept in pkgs:
        cgz, apk = package(files, p, kept)
        e = dict(p)
        e["C"] = "Q1" + base64.b64encode(hashlib.sha1(cgz).digest()).decode()
        e["S"] = str(len(apk))
        with open(os.path.join(o, f"{p['P']}-{p['V']}.apk"), "wb") as f:
            f.write(apk)
        index += "".join(f"{k}:{e[k]}\n" for k in "CPVASITULomtckDpri" if e.get(k)) + "\n"
    ti = tarfile.TarInfo("APKINDEX")
    ti.size, ti.mode = len(index.encode()), 0o644
    with open(os.path.join(o, "APKINDEX.tar.gz"), "wb") as f:
        f.write(gz(tar([(ti, index.encode())])))

def repository(images, cache, o, extra=()):
    """Writes every package the images hold to directory o, with an
    APKINDEX.tar.gz. Returns how many packages it got."""
    index, seen = "", set()
    os.makedirs(o, exist_ok=True)
    for image in images:
        files = merged(layers(image, cache))   # one image at a time: they are large
        kept = scripts(files)
        for p in installed(files["lib/apk/db/installed"][1]):
            if (p["P"], p["V"]) in seen:
                continue                   # images built at different times may hold two versions
            seen.add((p["P"], p["V"]))
            cgz, apk = package(files, p, kept)
            e = dict(p)
            e["C"] = "Q1" + base64.b64encode(hashlib.sha1(cgz).digest()).decode()
            e["S"] = str(len(apk))
            with open(os.path.join(o, f"{p['P']}-{p['V']}.apk"), "wb") as f:
                f.write(apk)
            index += "".join(f"{k}:{e[k]}\n" for k in "CPVASITULomtckDpri" if k in e) + "\n"
        del files
    for p, files, kept in extra:
        cgz, apk = package(files, p, kept)
        e = dict(p)
        e["C"] = "Q1" + base64.b64encode(hashlib.sha1(cgz).digest()).decode()
        e["S"] = str(len(apk))
        with open(os.path.join(o, f"{p['P']}-{p['V']}.apk"), "wb") as f:
            f.write(apk)
        index += "".join(f"{k}:{e[k]}\n" for k in "CPVASITULomtckDpri" if e.get(k)) + "\n"
        seen.add((p["P"], p["V"]))
    ti = tarfile.TarInfo("APKINDEX")
    ti.size, ti.mode = len(index.encode()), 0o644
    with open(os.path.join(o, "APKINDEX.tar.gz"), "wb") as f:
        f.write(gz(tar([(ti, index.encode())])))
    return len(seen)

def closure(repo, want, have):
    """The .apk files of repository directory repo that installing the
    packages named in want takes, with what they depend on, leaving out
    what the names in have (the root's own packages) provide."""
    with tarfile.open(os.path.join(repo, "APKINDEX.tar.gz")) as t:
        text = t.extractfile("APKINDEX").read().decode()
    entries = [dict(l.split(":", 1) for l in e.split("\n") if ":" in l) for e in text.split("\n\n") if e.strip()]
    def name(d):                   # "so:libz.so.1", "cmd:sh=1.2", "bt-lib>=2.0" -> the name
        return re.split(r"[<>=~]", d, maxsplit=1)[0]
    who = {}
    for e in entries:
        for n in [e["P"]] + [name(x) for x in e.get("p", "").split()]:
            who.setdefault(n, []).append(e)
    # A dependency on a version ("libcrypto3=3.5.9-r0") is followed even
    # when the root has the package: the root's may be older.
    done, todo, out = set(), list(want), []
    while todo:
        n = todo.pop()
        if n in done or (n in have and not re.search(r"[<>=~]", n)):
            continue
        n = name(n)
        if n in done:
            continue
        done.add(n)
        if n not in who:
            sys.exit(f"alpinedisk: nothing in the repository provides {n}")
        for e in who[n]:
            f = f"{e['P']}-{e['V']}.apk"
            if f not in out:
                out.append(f)
                todo += [d for d in e.get("D", "").split() if not d.startswith("!")]
    return out

# ---------------------------------------------------------------- the disk

def main():
    args = sys.argv[1:]
    want = args[args.index("--disk") + 1].split(",") if "--disk" in args else []
    if "--disk" in args:
        del args[args.index("--disk"):args.index("--disk") + 2]
    cache = None
    if "--cache" in args:
        cache = args[args.index("--cache") + 1]
        del args[args.index("--cache"):args.index("--cache") + 2]
    busybox = None
    if "--busybox" in args:
        busybox = args[args.index("--busybox") + 1]
        del args[args.index("--busybox"):args.index("--busybox") + 2]
    wheel = []
    if "--cmake-wheel" in args:
        wheel = [wheel_package(args[args.index("--cmake-wheel") + 1])]
        del args[args.index("--cmake-wheel"):args.index("--cmake-wheel") + 2]
    extra = []
    if "--extra" in args:
        extra = args[args.index("--extra") + 1].split()
        del args[args.index("--extra"):args.index("--extra") + 2]
    out, top, base, images = args[0], args[1], args[2], args[3:]
    cache = cache or top
    os.makedirs(cache, exist_ok=True)
    os.makedirs(top, exist_ok=True)
    root = layers(base, cache)
    if len(root) != 1:
        sys.exit(f"alpinedisk: {base} should be one layer, the minirootfs")
    work = os.path.join(top, "disk")
    subprocess.run(["rm", "-rf", work], check=True)
    os.makedirs(work)
    with gzip.open(root[0]) as f, open(os.path.join(work, "root.tar"), "wb") as o:
        o.write(f.read())
    rootfs = merged(root)
    with open(os.path.join(work, "busybox"), "wb") as f:
        f.write(open(busybox, "rb").read() if busybox else rootfs["bin/busybox"][1])
    http = os.path.join(work, "http")
    n = repository(images, cache, http, wheel)
    have = set()
    for p in installed(rootfs["lib/apk/db/installed"][1]):
        have.update([p["P"]] + [re.split(r"[<>=~]", x, maxsplit=1)[0] for x in p.get("p", "").split()])
    # The disk's repository: what the named packages take, and the test
    # packages; its index lists both.
    repo, repo2 = os.path.join(work, "repo"), os.path.join(work, "repo2")
    pkgs = closure(http, want, have)
    with tarfile.open(os.path.join(http, "APKINDEX.tar.gz")) as t:
        text = t.extractfile("APKINDEX").read().decode()
    keep = [e for e in text.split("\n\n") if e.strip() and
            "{P}-{V}.apk".format(**dict(l.split(":", 1) for l in e.split("\n") if ":" in l)) in pkgs]
    write_repo(repo, synthetic(SYNTH), "".join(e + "\n\n" for e in keep))
    write_repo(repo2, synthetic(SYNTH2))
    for f in pkgs:
        shutil.copyfile(os.path.join(http, f), os.path.join(repo, f))
    print(f"alpinedisk: {len(pkgs)} packages and {len(SYNTH)} test packages on the disk, {n} over HTTP",
          file=sys.stderr)
    args = [os.path.join(work, "busybox"), os.path.join(work, "root.tar")] + extra
    for r in (repo, repo2):
        args += [f"a/{os.path.basename(r)}/{ARCH}/{f}={os.path.join(r, f)}" for f in sorted(os.listdir(r))]
    # Room for the unpacked root and what apk installs (neovim, gcc and git
    # together are about 350 MB; cmake, tmux and lua-language-server add about 60 MB).
    env = dict(os.environ, MKDISK_FREE_MB="700")
    subprocess.run([os.path.join(HERE, "mkdisk.py"), out] + args, check=True, env=env)

if __name__ == "__main__":
    main()
