"""Runs Tornado's own test suite from its source tree (the current directory),
without the autoreload tests: each starts Python twice within a 4 s limit,
which emulated CPUs miss. A few of the SSL tests are racy when the CPU is
slow (the handshake finishes after the test has moved on, or logs one line
more than the test expects), so each test that fails is run once more on its
own; the suite passes if those pass. The two SSL variants of
test_inline_read_error depend on whether the server's handshake has finished
by the time the test closes its socket, which it does about half the time
here, so their results are not counted.

Prints "Ran N tests" and then "OK", or the tests that still fail."""
import os
import re
import subprocess
import sys

# The default 5 s per async test and the 0.1 s request timeouts are short for
# emulated CPUs; Tornado's tests take EMULATION=1 for the latter.
os.environ.setdefault("ASYNC_TEST_TIMEOUT", "120")
os.environ.setdefault("EMULATION", "1")

sys.path.insert(0, os.getcwd())

from tornado.test.runtests import TEST_MODULES

RACY = {
    "tornado.test.iostream_test.TestIOStreamSSL.test_inline_read_error",
    "tornado.test.iostream_test.TestIOStreamSSLContext.test_inline_read_error",
}
RUN = [sys.executable, "-m", "tornado.test.runtests"]


def run(names):
    p = subprocess.run(RUN + names, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    failed = []
    for line in p.stdout.splitlines():
        if line.startswith(("FAIL:", "ERROR:")):
            m = re.search(r"\((tornado\.test\.[\w.]+)\)", line)
            failed.append(m.group(1) if m else line)
    ran = re.search(r"^Ran (\d+) tests?", p.stdout, re.M)
    return int(ran.group(1)) if ran else 0, sorted(set(failed))


ran, failed = run([m for m in TEST_MODULES if "autoreload" not in m])
for name in failed[:]:
    if name in RACY or run([name])[1] == []:
        failed.remove(name)
print("Ran %d tests" % ran)
if failed:
    for name in failed:
        print("FAIL:", name)
else:
    print("OK")
