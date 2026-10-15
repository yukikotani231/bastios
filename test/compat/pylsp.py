#!/usr/bin/env python3
"""Writes a tar file of python-lsp-server and the pure-Python packages it
needs (test/compat/alpine.txt runs it under Alpine's python3 with Neovim's
LSP client). Every wheel is pinned by SHA-256; the wheels are unpacked
without pip, so the tree can be put on PYTHONPATH as it is.

usage: pylsp.py OUT.tar CACHE_DIR
"""
import hashlib, io, os, sys, tarfile, urllib.request, zipfile

PYPI = "https://files.pythonhosted.org/packages/"
WHEELS = [
    ("python_lsp_server-1.15.0-py3-none-any.whl", "c2/4a/b4abc04dcc65c985fa66d6486393ad3051ff485ccabd9727aea43a242f88", "d6ac11b467021310498f2dffb2edf09629bbcfe0d3073156db2337334a60463f"),
    ("jedi-0.20.1-py2.py3-none-any.whl", "be/c7/71eb0d51cdcbb63b857c931aa4e9218b078b5d9f8ecd383b234f28fe7b0f", "0fb16d86c4a4c73c37ba518c77419975e30fcc620658a8d14fbb5720cdd34142"),
    ("parso-0.8.7-py2.py3-none-any.whl", "99/5d/8268b644392ee874ee82a635cd0df1773de230bde356c38de28e298392cc", "a8926eb2a1b915486941fdbd31e86a4baf88fe8c210f25f2f35ecec5b574ca1c"),
    ("pluggy-1.7.0-py3-none-any.whl", "40/9e/2b38731e0fc536806f16490e1a12d7f0dc2a1235aa8cc07bcc75416a7daa", "7dd7b0d8832ba3cb632c306926ded123429211b83641b35dc5c41ad2d34f9bec"),
    ("python_lsp_jsonrpc-1.1.2-py3-none-any.whl", "cb/d9/656659d5b5d5f402b2b174cd0ba9bc827e07ce3c0bf88da65424baf64af8", "7339c2e9630ae98903fdaea1ace8c47fba0484983794d6aafd0bd8989be2b03c"),
    ("docstring_to_markdown-0.17-py3-none-any.whl", "56/7b/af3d0da15bed3a8665419bb3a630585756920f4ad67abfdfef26240ebcc0", "fd7d5094aa83943bf5f9e1a13701866b7c452eac19765380dead666e36d3711c"),
    ("typing_extensions-4.16.0-py3-none-any.whl", "49/d3/b8441a820a491ddfc024b0b0cf0393375b75ea13866d9c66727e54c2fc80", "481caa481374e813c1b176ada14e97f1f67a4539ce9cfeb3f350d78d6370c2e8"),
    ("importlib_metadata-9.0.1-py3-none-any.whl", "b3/55/ecca97ae19075f1fac62def77731e7f535e6c1fb8f92ff08160c5e6dade8", "bba5600596a7e21f3eef53281cf28d6a5195634d2f2b78ff9501a3272c6eaab0"),
    ("zipp-4.1.1-py3-none-any.whl", "b5/98/df615823cd9419131ce19fba00de53a663794369e198aade064a244b385d", "8979f52d874162f485ff2981e3891f3a3317b7a3dd43ff1e1775b9304f307a9c"),
]


def fetch(name, path, sha, cache):
    f = os.path.join(cache, name)
    if not os.path.exists(f):
        with urllib.request.urlopen(PYPI + path + "/" + name, timeout=120) as r:
            data = r.read()
        if hashlib.sha256(data).hexdigest() != sha:
            sys.exit(f"pylsp.py: {name} does not match its SHA-256")
        os.makedirs(cache, exist_ok=True)
        open(f + ".part", "wb").write(data)
        os.replace(f + ".part", f)
    data = open(f, "rb").read()
    if hashlib.sha256(data).hexdigest() != sha:
        sys.exit(f"pylsp.py: {name} does not match its SHA-256")
    return data


def main():
    out, cache = sys.argv[1], sys.argv[2]
    with tarfile.open(out + ".part", "w") as tar:
        for name, path, sha in WHEELS:
            z = zipfile.ZipFile(io.BytesIO(fetch(name, path, sha, cache)))
            for info in z.infolist():
                if info.is_dir() or ".." in info.filename.split("/") or info.filename.startswith("/"):
                    continue
                t = tarfile.TarInfo("pylsp/" + info.filename)
                data = z.read(info)
                t.size, t.mode, t.mtime = len(data), 0o644, 0
                tar.addfile(t, io.BytesIO(data))
    os.replace(out + ".part", out)


main()
