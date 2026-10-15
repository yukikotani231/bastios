#!/usr/bin/env python3
"""Where the instructions go while a command runs on BastiOS: single-steps the
guest from gdb and charges each executed instruction to a kernel function.

    bench/insnprof.py [--kernel build/bastios.elf] [--steps 20000] [--top 30]
                      -- COMMAND

Boots the kernel the way bench/run.py does with -icount (so the guest keeps
running while gdb is not attached and nothing depends on host speed), starts
COMMAND (e.g. `/disk/osbench syscall`) in the background, lets it warm up, then
attaches gdb and steps STEPS instructions. The result is exact for those
instructions, unlike bench/prof.py's samples of host time, and is the same
every run. Code outside the kernel's symbols is "[user]"; the syscall
trampoline, which has its own alias address, is "[tramp]".

Needs gdb. The second column is instructions per return from the kernel to the
program (one per system call for a loop of calls): compare two kernels by it.
"""
import argparse, bisect, json, os, subprocess, sys, tempfile, time
from collections import Counter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "bench"))
import run as bench  # noqa: E402
from prof import symbols  # noqa: E402

GDB = r'''
import gdb, json
gdb.execute("set pagination off")
gdb.execute("set confirm off")
gdb.execute("target remote :%(port)d")
if %(brk)d:
    gdb.execute("hbreak *%(brk)d")
    gdb.execute("continue", to_string=True)
    gdb.execute("delete", to_string=True)
pcs = []
n = %(steps)d
for i in range(n):
    gdb.execute("stepi", to_string=True)
    pcs.append(int(gdb.parse_and_eval("$pc")) & 0xFFFFFFFFFFFFFFFF)
open(%(out)r, "w").write(json.dumps(pcs))
gdb.execute("detach")
gdb.execute("quit")
'''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kernel", default=os.path.join(ROOT, "build", "bastios.elf"))
    ap.add_argument("--elf", default=os.path.join(ROOT, "build", "bastios64.elf"))
    ap.add_argument("--smp", type=int, default=1)
    ap.add_argument("--mem", default="512M")
    ap.add_argument("--steps", type=int, default=20000)
    ap.add_argument("--warm", type=float, default=3.0, help="seconds of host time before gdb attaches")
    ap.add_argument("--top", type=int, default=30)
    ap.add_argument("--break", dest="brk", default="", help="start the window when this kernel function is entered (counts every instruction of the window, not whole round trips)")
    ap.add_argument("--save", default="", help="also write the executed addresses to this JSON file")
    ap.add_argument("cmd", nargs="+")
    args = ap.parse_args()
    addrs, names = symbols(args.elf)
    osbench = bench.build_osbench()
    ns = argparse.Namespace(accel="tcg", cpu="", mem=args.mem, smp=args.smp, kernel=args.kernel)
    line = " ".join(args.cmd)
    port = 20000 + os.getpid() % 20000
    brk = 0
    if args.brk:
        for a, nm in zip(addrs, names):
            if nm == args.brk or nm.startswith(args.brk + "."):
                brk = a
                break
        if not brk:
            raise SystemExit(f"insnprof: no function {args.brk} in {args.elf}")
    bench.ICOUNT = True
    with tempfile.TemporaryDirectory() as tmp:
        bench.EXTRA_QEMU = ["-gdb", f"tcp::{port}"]
        g = bench.boot("bastios", ns, osbench, tmp)
        g.send(line)
        time.sleep(args.warm)
        out = os.path.join(tmp, "pcs.json")
        script = os.path.join(tmp, "step.py")
        with open(script, "w") as f:
            f.write(GDB % {"port": port, "steps": args.steps, "out": out, "brk": brk})
        subprocess.run(["gdb", "--batch", "-nx", "-x", script], capture_output=True, check=False)
        g.stop()
        if not os.path.exists(out):
            raise SystemExit("insnprof: gdb did not write the trace")
        pcs = json.load(open(out))
        if args.save:
            json.dump(pcs, open(args.save, "w"))
    hits = Counter()
    # Only whole round trips are counted: from the first return to the program
    # to the last one, so the partial calls at the ends of the window do not
    # skew the per-call figures.
    entry_at = []                           # steps at which the guest went from the kernel to its program
    was_user = True
    for i, pc in enumerate(pcs):
        user = not (addrs and addrs[0] <= pc < addrs[-1] + 0x10000) and pc < 0x7F0000000000
        if user and not was_user:
            entry_at.append(i)
        was_user = user
    if brk:
        entries = 1
    else:
        if len(entry_at) >= 2:
            pcs = pcs[entry_at[0]:entry_at[-1]]
        entries = max(len(entry_at) - 1, 0)
    for pc in pcs:
        if addrs and addrs[0] <= pc < addrs[-1] + 0x10000:
            hits[names[bisect.bisect_right(addrs, pc) - 1]] += 1
        elif pc >= 0xFFFF000000000000 or pc >= 0x7F0000000000:
            hits["[tramp]"] += 1
        else:
            hits["[user]"] += 1
    n = len(pcs)
    per = max(entries, 1)
    print(f"{n} instructions of `{line}` over {entries} whole round trips")
    print(f"per return: {n / per:.0f} instructions")
    for name, c in hits.most_common(args.top):
        print(f"{100 * c / n:6.2f}%  {c / per:8.1f}  {name}")


if __name__ == "__main__":
    main()
