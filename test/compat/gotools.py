#!/usr/bin/env python3
"""Writes a tar file of the Go toolchain of the golang image (the files
godisk.py keeps) and gopls, to unpack under the Alpine root's /usr/local
(test/compat/alpine.txt runs gopls with Neovim's LSP client). The tar holds
go/ and bin/gopls.

    gotools.py OUT.tar IMAGE GOPLS --cache DIR
"""
import os, sys, tarfile, io

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import alpinedisk, godisk

def main():
    args = sys.argv[1:]
    cache = args[args.index("--cache") + 1]
    del args[args.index("--cache"):args.index("--cache") + 2]
    out, image, gopls = args
    os.makedirs(cache, exist_ok=True)
    files = alpinedisk.merged(alpinedisk.layers(image, cache))
    with tarfile.open(out + ".part", "w") as t:
        for name, (ti, body) in sorted(files.items()):
            if not godisk.keep(name):
                continue
            ti = tarfile.TarInfo.frombuf(ti.tobuf(), "utf-8", "surrogateescape")
            ti.name = name[len("usr/local/"):]
            ti.uid = ti.gid = 0
            ti.uname = ti.gname = ""
            t.addfile(ti, io.BytesIO(body) if ti.isfile() else None)
        ti = tarfile.TarInfo("bin/gopls")
        ti.mode, ti.size = 0o755, os.path.getsize(gopls)
        with open(gopls, "rb") as f:
            t.addfile(ti, f)
    os.rename(out + ".part", out)

main()
