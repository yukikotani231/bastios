#!/usr/bin/env python3
"""Writes the Ruby disk for the compat suite (test/compat/ruby.txt): the root
of a Ruby image (the official ruby:alpine, with gem and bundler), as
scriptdisk.py writes it, plus the tests of Ruby's own source (bootstraptest/,
test/ and the helpers they load from tool/, from the release tarball). They go
under /work/r in the root, with the list of tests that pass (ruby-tests.txt)
and the script that runs a part of it (run.sh).

    rubydisk.py OUT.img IMAGE BUSYBOX TARBALL SHA256 --cache DIR

TARBALL is the source tarball (a .tar.xz), downloaded from URL into DIR when
it is not there, and checked against SHA256 each time. The tests are of the
release the image holds, or the nearest one that ships them (Ubuntu's
ruby3.3 package carries the pristine 3.3 sources).
"""
import hashlib, os, sys, tarfile, time, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import scriptdisk

URL = "https://archive.ubuntu.com/ubuntu/pool/main/r/ruby3.3/"
KEEP = ("bootstraptest/", "tool/lib/", "tool/test/", "test/lib/", "test/runner.rb", "test/ruby/")
RUN = b"""# run.sh GROUP [K N]: runs the tests of GROUP (ruby-tests.txt, one path under
# /work/r per line, GROUP being its directory), or with K and N every Nth of
# them starting at the Kth, and prints the ones that fail (and the end of
# their output), then how many ran.
# The tests that measure a leak by the memory of a subprocess are left out:
# they time out on a guest this slow, and the timeouts of the others are scaled.
cd /work/r
n=0
for f in $(grep "^$1/" ../ruby-tests.txt | awk -v k="${2:-0}" -v n="${3:-1}" 'NR % n == k'); do
  n=$((n + 1))
  case "$f" in
    bootstraptest/*) (cd bootstraptest && timeout 600 /usr/local/bin/ruby runner.rb --ruby=/usr/local/bin/ruby -q "${f#bootstraptest/}" >/tmp/rt.out 2>&1) || { echo "FAIL $f"; tail -n 15 /tmp/rt.out; };;
    *) timeout 600 /usr/local/bin/ruby -I tool/lib -I test tool/test/runner.rb --seed=1 --timeout-scale=10 -n '!/memory_leak/' "$f" >/tmp/rt.out 2>&1 || { echo "FAIL $f"; tail -n 15 /tmp/rt.out; };;
  esac
done
echo "ran $n"
"""

def tarball(path, sha):
    if not os.path.exists(path):
        urllib.request.urlretrieve(URL + os.path.basename(path), path)
    if hashlib.sha256(open(path, "rb").read()).hexdigest() != sha:
        sys.exit(f"rubydisk: {path} does not match its SHA-256")
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

    def extra(files, add):
        def put(path, body, mode=0o644):
            ti = tarfile.TarInfo(path)
            ti.size, ti.mode, ti.mtime = len(body), mode, int(time.time())
            add(ti, body)
        with tarfile.open(src) as t:
            for m in t:
                rel = m.name.split("/", 1)[1] if "/" in m.name else ""
                if m.isfile() and rel.startswith(KEEP):
                    put("work/r/" + rel, t.extractfile(m).read(), m.mode & 0o777)
        put("work/ruby-tests.txt", open(os.path.join(HERE, "ruby-tests.txt"), "rb").read())
        put("work/run.sh", RUN)
    scriptdisk.build(out, image, busybox, cache, "700", extra)

main()
