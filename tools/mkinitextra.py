#!/usr/bin/env python3
"""Writes the installer image's extra/initextra.bastio (see
src/x86_64/arch/initfiles.bastio): the files it adds to the file system.

    mkinitextra.py OUT.bastio NAME=FILE[:MODE] ...

NAME is the path in the guest (16 bytes at most); MODE is octal, default 644.
"""
import os, sys

if len(sys.argv) < 3:
    sys.exit(__doc__)
out = sys.argv[1]
files = []
for a in sys.argv[2:]:
    name, src = a.split("=", 1)
    mode = 0o644
    if ":" in src:
        src, m = src.rsplit(":", 1)
        mode = int(m, 8)
    if len(name) > 16:
        sys.exit(f"mkinitextra: {name} is longer than 16 bytes")
    rel = os.path.relpath(os.path.abspath(src), os.path.dirname(os.path.abspath(out)))
    files.append((name, rel, mode))

def chain(fn, body, default):
    s = ""
    for i, v in enumerate(body):
        s += f"  if i == {i} {{\n    return {v};\n  }}\n"
    return s + f"  return {default};\n"

text = "// Written by tools/mkinitextra.py: the files the installer image adds.\n\n"
text += f"const NEXTRA: u64 = {len(files)};\n\n"
text += "fn extra_name(i: u64) -> Str\n  ensures slen(result) <= 16\n{\n"
text += chain("name", [f'"{n}"' for n, _, _ in files], '""') + "}\n\n"
text += "fn extra_data(i: u64) -> Str {\n"
text += chain("data", [f'include_bytes("{r}")' for _, r, _ in files], '""') + "}\n\n"
text += "fn extra_mode(i: u64) -> u64 {\n"
text += chain("mode", [str(m) for _, _, m in files], "420") + "}\n"
os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
with open(out, "w") as f:
    f.write(text)
