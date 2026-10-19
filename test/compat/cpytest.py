"""Runs modules of CPython's own regression tests (Lib/test) and prints one
line for each: "NAME ok", or "NAME FAILED TEST..." naming each test that
failed and is not in KNOWN. It runs on the compat disk with the test modules
in /disk/cpytests (the Makefile's build/compat/cpytests), as

    PYTHONHOME=/disk/py PYTHONPATH=/disk/cpytests /disk/python /disk/cpytests/run.py select os ...

A test that fails because the interpreter has no _testcapi (it is built with
--disable-test-modules) is not a failure of the system under test and
counts as skipped.
"""
import io, sys, unittest

# Failures that are not the kernel's: test id -> why.
KNOWN = {
    "test.test_os.TestInvalidFD.test_fpathconf": "musl's fpathconf does not check the descriptor",
    "test.test_posix.PosixTester.test_makedev": "CPython rejects major(-1) with musl's unsigned dev_t",
    "test.test_threading.SubinterpThreadingTests.test_daemon_threads_fatal_error": "its child process imports _testcapi",
    "test.test_fcntl.TestFcntl.test_fcntl_64_bit": "F_NOTIFY (dnotify) is not implemented",
}

def run(name):
    suite = unittest.defaultTestLoader.loadTestsFromName("test.test_" + name)
    res = unittest.TextTestRunner(stream=io.StringIO(), verbosity=0).run(suite)
    bad = []
    for t, tb in res.failures + res.errors:
        tid = t.id()
        if tid in KNOWN or "No module named '_testcapi'" in tb:
            continue
        if tid.startswith("unittest.loader."):
            # The module did not import: say why, on one line.
            tid += " " + tb.strip().splitlines()[-1]
        bad.append(tid.replace("test.test_" + name + ".", "", 1))
    print(name, "FAILED " + " ".join(sorted(bad)) if bad else "ok", flush=True)
    return not bad

# Some tests start processes with multiprocessing's "spawn", which imports
# this file again in the child.
if __name__ == "__main__":
    ok = True
    for n in sys.argv[1:]:
        ok = run(n) and ok
    sys.exit(0 if ok else 1)
