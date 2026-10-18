#!/usr/bin/env python3
"""Writes a tar file of the JDK of the eclipse-temurin image (the files
jdkdisk.py keeps, without javac's companions the language server does not
need) and Eclipse JDT LS, to unpack under the Alpine root
(test/compat/alpine.txt runs jdtls with Neovim's LSP client). The tar holds
opt/java/openjdk and opt/jdtls.

    jdktools.py OUT.tar IMAGE JDTLS_TGZ --cache DIR
"""
import os, sys, tarfile, io

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import alpinedisk, jdkdisk

def main():
    args = sys.argv[1:]
    cache = args[args.index("--cache") + 1]
    del args[args.index("--cache"):args.index("--cache") + 2]
    out, image, jdtls = args
    os.makedirs(cache, exist_ok=True)
    files = alpinedisk.merged(alpinedisk.layers(image, cache))
    with tarfile.open(out + ".part", "w") as t:
        for name, (ti, body) in sorted(files.items()):
            if not name.startswith("opt/java/openjdk") or not jdkdisk.keep(name):
                continue
            ti = tarfile.TarInfo.frombuf(ti.tobuf(), "utf-8", "surrogateescape")
            ti.uid = ti.gid = 0
            ti.uname = ti.gname = ""
            t.addfile(ti, io.BytesIO(body) if ti.isfile() else None)
        with tarfile.open(jdtls, "r:gz") as src:
            for m in src:
                top = m.name.lstrip("./").split("/")[0]
                if top in ("plugins", "config_linux"):
                    m.name = "opt/jdtls/" + m.name.lstrip("./")
                    m.uid = m.gid = 0
                    m.uname = m.gname = ""
                    t.addfile(m, src.extractfile(m) if m.isfile() else None)
    os.rename(out + ".part", out)

main()
