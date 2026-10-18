#!/usr/bin/env python3
"""Collects the pure-Perl files that a list of modules loads from a perl-cross build tree.

    perlmods.py SRC OUT MODULE...

SRC is a perl source tree after "make" (perl-cross builds only perl for the
target, so the miniperl that runs on the build machine does the loading, with
XSLoader::load stubbed out: only the files a module requires matter here).
Each file the loading asks for is copied from the tree into OUT, which becomes
/disk/perllib. The x86-64 build does the same with the freshly built perl.
"""
import glob
import os
import re
import shutil
import subprocess
import sys

CODE = r'''BEGIN { require XSLoader; no warnings;
  *XSLoader::load = sub { my $p = caller; no strict "refs"; *{"${p}::AUTOLOAD"} = sub { 0 }; 1 };
  $INC{"DynaLoader.pm"} = 1; *DynaLoader::bootstrap = sub { 1 };
  *Encode::onBOOT = sub { };
  require Socket; *Socket::AUTOLOAD = sub { 0 }; *Socket::inet_aton = sub { 0 }; *Socket::pack_sockaddr_un = sub { 0 } }
for (@ARGV) { eval "require $_; 1" or die "$_: $@" }'''


def find(src, rel):
    d, b = os.path.split(rel)
    dirn = d.replace("/", "-")
    cands = [f"{src}/lib/{rel}"] + glob.glob(f"{src}/*/*/lib/{rel}")
    if dirn:
        # XS modules keep Data::Dumper as dist/Data-Dumper/Dumper.pm
        cands += glob.glob(f"{src}/*/{dirn}-{b[:-3]}/{b}") + glob.glob(f"{src}/*/{dirn}-{b[:-3]}/lib/{b}")
        cands += glob.glob(f"{src}/*/{dirn}/{b}") + glob.glob(f"{src}/*/{dirn}/lib/{b}")
    else:
        cands += glob.glob(f"{src}/*/{b[:-3]}/{b}")
    cands += glob.glob(f"{src}/*/*/{rel}")      # dist/PathTools/Cwd.pm, ext/POSIX/lib/POSIX.pm
    return next((c for c in cands if os.path.isfile(c)), None)


def main():
    src, out, mods = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2]), sys.argv[3:]
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    xsl = f"{src}/dist/XSLoader/XSLoader.pm"
    if not os.path.isfile(xsl):
        subprocess.run([f"{src}/miniperl", f"-I{src}/lib", "XSLoader_pm.PL"], cwd=f"{src}/dist/XSLoader", check=True)
    shutil.copy(xsl, f"{out}/XSLoader.pm")
    for _ in range(1000):
        r = subprocess.run([f"{src}/miniperl", f"-I{out}", "-e", CODE] + mods, capture_output=True, text=True, cwd=out)
        if r.returncode == 0:
            return
        m = re.search(r"Can't locate (\S+\.p[mh]) in", r.stderr)
        if not m:
            sys.exit(f"perlmods: {r.stderr[:400]}")
        f = find(src, m.group(1))
        if not f:
            sys.exit(f"perlmods: no source for {m.group(1)}")
        os.makedirs(os.path.dirname(f"{out}/{m.group(1)}"), exist_ok=True)
        shutil.copy(f, f"{out}/{m.group(1)}")
    sys.exit("perlmods: gave up")


main()
