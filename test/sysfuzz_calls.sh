#!/usr/bin/env bash
# Checks that user/sysfuzz.c aims at every Linux system call the kernel
# implements. A call the kernel dispatches (`n == LX_NAME` in src/) must be
# in sysfuzz's `linux_calls` table or on its "left out:" line, with a reason
# in `tame`. Without this, the fuzzer reaches a new call only through its
# rare random numbers: 99 implemented calls had drifted out of its table.
set -eu
cd "$(dirname "$0")/.."

python3 - <<'PY'
import glob, re, sys
value = {}
used = set()
for f in glob.glob("src/*.bastio"):
    s = open(f).read()
    for name, v in re.findall(r"const (LX_[A-Z0-9_]+): u64 = (\d+)", s):
        value[name] = int(v)
    used |= set(re.findall(r"\bn == (LX_[A-Z0-9_]+)", s))
fz = open("user/sysfuzz.c").read()
table = {int(x) for x in re.findall(r"\d+", re.search(r"linux_calls\[\] = \{(.*?)\};", fz, re.S).group(1))}
left = {int(x) for x in re.search(r"left out:([\d ]*)", fz).group(1).split()}
bad = 0
for name in sorted(used):
    if name not in value:
        print(f"sysfuzz_calls: {name} has no `const {name}: u64 = N` in src/")
        bad += 1
    elif value[name] not in table and value[name] not in left:
        print(f"sysfuzz_calls: {name} ({value[name]}) is dispatched but sysfuzz never aims at it: "
              "add it to linux_calls in user/sysfuzz.c (tame it if it can wait), or to its \"left out:\" line")
        bad += 1
for n in sorted(left & table):
    print(f"sysfuzz_calls: {n} is both in linux_calls and left out")
    bad += 1
if bad:
    sys.exit(1)
print(f"ok   sysfuzz aims at all {len(used)} dispatched Linux calls ({len(left)} left out)")
PY
