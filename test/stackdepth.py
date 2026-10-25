#!/usr/bin/env python3
"""The deepest chain of kernel calls of a native kernel must fit its stack.

A kernel stack that overflows hits the guard page and halts the machine
(test/stackguard_test.sh), and the overflow is hard to see before it
happens: it needs a deep call chain and an interrupt on top of it. This
test adds up the frames along the longest call chain in build/kernel.s
(a native kernel's assembly: a frame is the return address, the saved %rbp
and the `sub $N, %rsp` of the function's prologue) and fails when the sum
passes the budget, which leaves the rest of the kernel stack (92 KiB, see
boot/core.h) to the interrupt and exception frames that arrive on top.

A kernel built through the C compiler has no such assembly; the test is
skipped for it. Bastio has no function pointers and the kernel's calls are
all direct, so the call graph is exact. A cycle in it (recursion) cannot
be bounded here and fails the test, except the ones in BOUNDED below, whose
depth the prover bounds (a requires clause on the depth argument): the
frames of such a cycle count once per level.

STACK_BUDGET (bytes) overrides the default budget; KERNEL_S the file.
"""
import os, re, sys

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
path = os.environ.get("KERNEL_S", "build/kernel.s")
budget = int(os.environ.get("STACK_BUDGET", 64 * 1024))

backend = ""
try:
    backend = open("build/kernel.backend").read().strip()
except OSError:
    pass
if "KERNEL_S" not in os.environ and backend != "native":
    print("SKIP stackdepth: the kernel is built through the C compiler")
    sys.exit(0)
if not os.path.exists(path):
    print("FAIL stackdepth: %s is missing (make build/kernel.s)" % path)
    sys.exit(1)

frame, calls, cur = {}, {}, None
for line in open(path):
    m = re.match(r"^(f_\w+|bst_\w+):", line)
    if m:
        cur = m.group(1)
        frame[cur], calls[cur] = 16, set()
        continue
    if cur is None:
        continue
    m = re.match(r"\s+sub \$(\d+), %rsp", line)
    if m and frame[cur] == 16:
        frame[cur] += int(m.group(1))
        continue
    m = re.match(r"\s+call (f_\w+|bst_\w+)\b", line)
    if m:
        calls[cur].add(m.group(1))

# Recursion that is bounded by a proved precondition: the set of functions
# in the cycle and how many times the cycle can be entered one inside the
# other. A USB hub inside a hub: `depth < XHCI_DEPTH` (2, src/xhci.bastio),
# so three levels. post_sig(SIGCONT) tells the parent through jc_tell, which
# posts SIGCHLD (not SIGCONT) to the parent: two levels.
# lx_deliver forces a SIGSEGV once when a handler has no frame room (a second
# failure kills the process): two levels. lx_open_n reopens the directory
# entry of the file that /proc/PID/fd/N names; a path of the in-memory file
# system is not a /proc path, so one level is expected: four are allowed,
# which is an assumption and not a proved bound.
# A cycle counts as bounded when all its functions are in the set: a backend
# that does not inline a small helper of the cycle (post_from, lx_open_fd_link)
# shows a longer cycle than one that does, with the same bound.
BOUNDED = [({"f_dev_attach", "f_hub_setup"}, 3), ({"f_post_sig", "f_jc_tell", "f_post_from", "f_post_w", "f_post_raw"}, 2),
           ({"f_lx_deliver"}, 2), ({"f_lx_open_n", "f_lx_open_fd_link"}, 4)]

sys.setrecursionlimit(100000)
done, onpath, cycles = {}, [], []

def deepest(f):
    """(bytes, chain) of the deepest call chain from f, frame of f included."""
    if f in done:
        return done[f]
    onpath.append(f)
    best = (0, [])
    for c in sorted(calls.get(f, ())):
        if c not in frame:
            continue
        if c in onpath:
            cyc = onpath[onpath.index(c):]
            for members, times in BOUNDED:
                if set(cyc) <= members:
                    extra = (times - 1) * sum(frame[g] for g in cyc)
                    if extra > best[0]:
                        best = (extra, ["(%s x%d)" % ("+".join(cyc), times)])
                    break
            else:
                cycles.append(cyc + [c])
            continue
        d = deepest(c)
        if d[0] > best[0]:
            best = d
    onpath.pop()
    done[f] = (frame[f] + best[0], [f] + best[1])
    return done[f]

if not frame:
    print("FAIL stackdepth: no function in %s" % path)
    sys.exit(1)
worst = max((deepest(f) for f in frame), key=lambda d: d[0])
print("deepest call chain: %d bytes of %d budget (stack 92 KiB)" % (worst[0], budget))
print("  " + " > ".join("%s(%d)" % (f[2:] if f.startswith("f_") else f, frame.get(f, 0)) if f in frame else f for f in worst[1]))
ok = True
if cycles:
    ok = False
    print("FAIL stackdepth: recursion in the kernel: " + " > ".join(cycles[0]))
if worst[0] > budget:
    ok = False
    print("FAIL stackdepth: the deepest chain passes the budget")
print("---- %d failed ----" % (0 if ok else 1))
sys.exit(0 if ok else 1)
