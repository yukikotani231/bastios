"""Packs CPython's standard library into one zip for the BastiOS disk.

    python pyzip.py LIBDIR OUT.zip

Every module under LIBDIR goes in as its source and as a compiled .pyc
beside it (unchecked-hash, so zipimport never recompiles). Tests, Tk, IDLE
and the like stay out. One file, because the disk's directory holds only
1024 entries and the library has more files than that.
"""
import os, py_compile, sys, tempfile, zipfile

SKIP = {"test", "idlelib", "tkinter", "turtledemo", "ensurepip", "lib2to3", "pydoc_data",
        "site-packages", "__pycache__", "venv"}

def main(lib, out):
    tmp = os.path.join(tempfile.mkdtemp(), "m.pyc")
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for root, dirs, files in os.walk(lib):
            dirs[:] = sorted(d for d in dirs if d not in SKIP and not d.startswith("config-"))
            for f in sorted(files):
                if not f.endswith(".py"):
                    continue
                path = os.path.join(root, f)
                name = os.path.relpath(path, lib)
                z.write(path, name)
                try:
                    py_compile.compile(path, cfile=tmp, dfile=name, doraise=True,
                                       invalidation_mode=py_compile.PycInvalidationMode.UNCHECKED_HASH)
                except py_compile.PyCompileError:
                    continue                # a few files are deliberately bad syntax
                z.write(tmp, name + "c")

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
