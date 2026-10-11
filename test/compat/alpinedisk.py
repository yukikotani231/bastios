#!/usr/bin/env python3
"""Writes the Alpine disk for the compat suite (test/compat/alpine.txt):
Alpine Linux's root file system as a tar file (/disk/root.tar) and a local
package repository (/disk/a/repo/x86_64: .apk files and an APKINDEX), next
to Alpine's own BusyBox at /disk/busybox. The checks unpack the root under
/disk/a, chroot into it and run apk there.

    alpinedisk.py OUT.img CACHE_DIR BASE_IMAGE PKG_IMAGE... [-- HTTP_IMAGE...]

The packages of the PKG_IMAGEs go on the disk; those and the HTTP_IMAGEs'
make a second repository, CACHE_DIR/disk/http, that run.py serves over
HTTP (alpine.txt's ALPINEURL): the large ones (gcc, Python) do not fit on
the disk next to the root they are installed into.

The images are Docker Hub's (NAME@sha256:DIGEST, pinned in the Makefile):
BASE_IMAGE is Alpine's minirootfs, and each PKG_IMAGE an image that apk
installed packages into. Alpine's own mirrors serve the packages too, but
this machine's network reaches only Docker Hub, so the packages are rebuilt
from what the images hold: every file a package installed (as listed in
/lib/apk/db/installed, with its owner and mode) goes back into an .apk with
the package's metadata. The binaries are Alpine's; the packages and the
index are not signed by Alpine, so apk needs --allow-untrusted.
"""
import base64, gzip, hashlib, http.client, io, json, os, subprocess, sys, tarfile, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
REGISTRY = "https://registry-1.docker.io/v2"
ACCEPT = ", ".join([
    "application/vnd.oci.image.index.v1+json",
    "application/vnd.docker.distribution.manifest.list.v2+json",
    "application/vnd.oci.image.manifest.v1+json",
    "application/vnd.docker.distribution.manifest.v2+json"])

# ---------------------------------------------------------------- Docker Hub

def fetch(url, token, accept=None):
    h = {"Authorization": "Bearer " + token}
    if accept:
        h["Accept"] = accept
    with urllib.request.urlopen(urllib.request.Request(url, headers=h)) as r:
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
            with urllib.request.urlopen(urllib.request.Request(url, headers=h)) as r, open(path, "ab") as f:
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

def layers(image, cache):
    """The layers of an amd64 image NAME@sha256:DIGEST, as paths of gzip
    tar files in the cache, base first. Each blob is checked against its
    digest."""
    name, digest = image.split("@")
    # Docker Hub limits how many manifests one address may fetch, so the
    # layer list is kept too, by the image's digest.
    listed = os.path.join(cache, digest.replace(":", "-") + ".json")
    if os.path.exists(listed):
        paths = json.load(open(listed))
        if all(os.path.exists(p) for p in paths):
            return paths
    token = json.load(urllib.request.urlopen(
        "https://auth.docker.io/token?service=registry.docker.io&scope=repository:" + name + ":pull"))["token"]
    m = json.loads(fetch(f"{REGISTRY}/{name}/manifests/{digest}", token, ACCEPT))
    if "manifests" in m:
        d = [x for x in m["manifests"] if x.get("platform", {}).get("architecture") == "amd64"
             and x.get("platform", {}).get("os") == "linux"][0]["digest"]
        m = json.loads(fetch(f"{REGISTRY}/{name}/manifests/{d}", token, ACCEPT))
    out = []
    for l in m["layers"]:
        path = os.path.join(cache, l["digest"].replace(":", "-") + ".tar.gz")
        if not os.path.exists(path):
            download(f"{REGISTRY}/{name}/blobs/{l['digest']}", token, path + ".part", l["size"])
            with open(path + ".part", "rb") as f:
                if "sha256:" + hashlib.sha256(f.read()).hexdigest() != l["digest"]:
                    sys.exit(f"alpinedisk: {image}: layer {l['digest']} does not match its digest")
            os.rename(path + ".part", path)
        out.append(path)
    with open(listed, "w") as f:
        json.dump(out, f)
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
    o = io.BytesIO()
    with gzip.GzipFile(fileobj=o, mode="wb", mtime=0) as f:
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

def package(files, p, kept=({}, {})):
    """The .apk for package p: a gzip stream of its .PKGINFO and install
    scripts (from kept, as scripts() gives them), then one of its files.
    Returns (control stream, whole package)."""
    when = int(p.get("t", 0))
    data = []
    for d, acl in p["dirs"]:
        ti = owned(tarfile.TarInfo(d), acl, 0o755)
        ti.type, ti.mtime = tarfile.DIRTYPE, when
        data.append((ti, None))
    for d, name, acl in p["files"]:
        path = f"{d}/{name}" if d else name
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

def repository(images, cache, outs):
    """Writes every package the images hold to the directories in outs, with
    an APKINDEX.tar.gz in each: outs[0] gets all of them, outs[1] those of
    the first `first` images. images is [(image, first?)]. Returns how many
    packages each got."""
    index = {o: "" for o in outs}
    seen = {o: set() for o in outs}
    for o in outs:
        os.makedirs(o, exist_ok=True)
    for image, first in images:
        files = merged(layers(image, cache))   # one image at a time: they are large
        kept = scripts(files)
        for p in installed(files["lib/apk/db/installed"][1]):
            to = [o for o in (outs if first else outs[:1]) if (p["P"], p["V"]) not in seen[o]]
            if not to:
                continue                   # images built at different times may hold two versions
            cgz, apk = package(files, p, kept)
            e = dict(p)
            e["C"] = "Q1" + base64.b64encode(hashlib.sha1(cgz).digest()).decode()
            e["S"] = str(len(apk))
            for o in to:
                seen[o].add((p["P"], p["V"]))
                with open(os.path.join(o, f"{p['P']}-{p['V']}.apk"), "wb") as f:
                    f.write(apk)
                index[o] += "".join(f"{k}:{e[k]}\n" for k in "CPVASITULomtckDpri" if k in e) + "\n"
        del files
    for o in outs:
        ti = tarfile.TarInfo("APKINDEX")
        ti.size, ti.mode = len(index[o].encode()), 0o644
        with open(os.path.join(o, "APKINDEX.tar.gz"), "wb") as f:
            f.write(gz(tar([(ti, index[o].encode())])))
    return [len(seen[o]) for o in outs]

# ---------------------------------------------------------------- the disk

def main():
    out, cache, base, rest = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
    disk_pkgs = rest[:rest.index("--")] if "--" in rest else rest
    http_pkgs = rest[rest.index("--") + 1:] if "--" in rest else []
    os.makedirs(cache, exist_ok=True)
    root = layers(base, cache)
    if len(root) != 1:
        sys.exit(f"alpinedisk: {base} should be one layer, the minirootfs")
    work = os.path.join(cache, "disk")
    subprocess.run(["rm", "-rf", work], check=True)
    os.makedirs(work)
    with gzip.open(root[0]) as f, open(os.path.join(work, "root.tar"), "wb") as o:
        o.write(f.read())
    rootfs = merged(root)
    with open(os.path.join(work, "busybox"), "wb") as f:
        f.write(rootfs["bin/busybox"][1])
    repo, http = os.path.join(work, "repo"), os.path.join(work, "http")
    n = repository([(i, True) for i in disk_pkgs] + [(i, False) for i in http_pkgs], cache, [http, repo])
    print(f"alpinedisk: {n[1]} packages on the disk, {n[0]} over HTTP", file=sys.stderr)
    args = [os.path.join(work, "busybox"), os.path.join(work, "root.tar")]
    args += [f"a/repo/x86_64/{n}={os.path.join(repo, n)}" for n in sorted(os.listdir(repo))]
    # Room for the unpacked root and what apk installs.
    env = dict(os.environ, MKDISK_FREE_MB="160")
    subprocess.run([os.path.join(HERE, "mkdisk.py"), out] + args, check=True, env=env)

if __name__ == "__main__":
    main()
