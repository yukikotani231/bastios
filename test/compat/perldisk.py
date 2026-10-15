#!/usr/bin/env python3
"""Writes the Perl disk for the compat suite (test/compat/perl.txt): the root
of a Perl image (Alpine's perl), as scriptdisk.py writes it, plus the
test suite of Perl's own source (t/, from the release tarball) and a copy of
the installed core library as lib/, which the tests expect next to t/. They go
under /work/p in the root, with the list of tests that pass (perl-tests.txt)
and the script that runs a part of it (run.sh).

    perldisk.py OUT.img IMAGE BUSYBOX TARBALL SHA256 --cache DIR

TARBALL is the release tarball (perl-X.Y.Z.tar.gz): downloaded from cpan.org
into DIR when it is not there, and checked against SHA256 each time.
"""
import hashlib, io, os, sys, tarfile, time, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import scriptdisk

LIBS = ("usr/share/perl5/core_perl/", "usr/lib/perl5/core_perl/")
RUN = b"""# run.sh GROUP [K N]: runs the tests of GROUP (perl-tests.txt, one path under t/
# per line), or with K and N every Nth of them starting at the Kth, and prints
# the ones that fail, then how many ran.
cd /work/p/t
n=0
for f in $(grep "^$1/" ../perl-tests.txt | awk -v k="${2:-0}" -v n="${3:-1}" 'NR % n == k'); do
  n=$((n + 1))
  out=$(timeout 300 /usr/bin/perl -I../lib $f 2>&1) || { echo "FAIL $f"; continue; }
  case "$out" in *"
not ok "*|"not ok "*) echo "FAIL $f";; esac
done
echo "ran $n"
"""

def tarball(path, sha):
    if not os.path.exists(path):
        name = os.path.basename(path)
        urllib.request.urlretrieve("https://www.cpan.org/src/5.0/" + name, path)
    if hashlib.sha256(open(path, "rb").read()).hexdigest() != sha:
        sys.exit(f"perldisk: {path} does not match its SHA-256")
    return path

def main():
    args = sys.argv[1:]
    cache = None
    if "--cache" in args:
        cache = args[args.index("--cache") + 1]
        del args[args.index("--cache"):args.index("--cache") + 2]
    out, image, busybox, name, sha = args
    cache = cache or os.path.dirname(os.path.abspath(out))
    os.makedirs(cache, exist_ok=True)
    src = tarball(os.path.join(cache, name), sha)
    top = name[:-len(".tar.gz")]

    def extra(files, add):
        def put(path, body, mode=0o644):
            ti = tarfile.TarInfo(path)
            ti.size, ti.mode, ti.mtime = len(body), mode, int(time.time())
            add(ti, body)
        with tarfile.open(src) as t:
            for m in t:
                if m.isfile() and m.name.startswith(top + "/t/"):
                    put("work/p/t/" + m.name[len(top) + 3:], t.extractfile(m).read(), m.mode & 0o777)
        for name, (ti, body) in sorted(files.items()):
            for lib in LIBS:
                if name.startswith(lib) and ti.isfile():
                    put("work/p/lib/" + name[len(lib):], body, ti.mode & 0o777)
        put("work/p/perl-tests.txt", open(os.path.join(HERE, "perl-tests.txt"), "rb").read())
        put("work/p/run.sh", RUN)
    scriptdisk.build(out, image, busybox, cache, "700", extra)

main()
