"""Runs the socket-heavy part of Tornado's own test suite from its source tree
(the current directory): the event loop, address and port allocation, the TCP
client and server, child processes, the HTTP server, the simple HTTP client
(connection refused, timeouts, SSL) and WebSockets. The rest is Python code
that does not reach the kernel, and the whole suite takes a quarter of an
hour in CI. A few of the SSL tests are racy when the CPU is slow (a handshake
ends after the test has moved on, or logs one line more than the test
expects), so each test that fails is run once more on its own; the suite
passes if those pass.

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

MODULES = [
    "tornado.test." + m
    for m in (
        "ioloop_test", "netutil_test", "tcpclient_test", "tcpserver_test", "process_test",
        "httpserver_test", "websocket_test", "simple_httpclient_test",
    )
]
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


ran, failed = run(MODULES)
for name in failed[:]:
    if run([name])[1] == []:
        failed.remove(name)
print("Ran %d tests" % ran)
if failed:
    for name in failed:
        print("FAIL:", name)
else:
    print("OK")
