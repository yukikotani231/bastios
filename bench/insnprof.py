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

Needs gdb. Divide a function's count by the number of loop iterations in the
window (the line `iterations` is the number of times the first function of the
user loop was seen entering the kernel, if any) or compare two kernels over
the same window.
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
    ap.add_argument("cmd", nargs="+")
    args = ap.parse_args()
    addrs, names = symbols(args.elf)
    osbench = bench.build_osbench()
    ns = argparse.Namespace(accel="tcg", cpu="", mem=args.mem, smp=args.smp, kernel=args.kernel)
    line = " ".join(args.cmd)
    port = 20000 + os.getpid() % 20000
    bench.ICOUNT = True
    with tempfile.TemporaryDirectory() as tmp:
        bench.EXTRA_QEMU = ["-gdb", f"tcp::{port}"]
        g = bench.boot("bastios", ns, osbench, tmp)
        g.send(line)
        time.sleep(args.warm)
        out = os.path.join(tmp, "pcs.json")
        script = os.path.join(tmp, "step.py")
        with open(script, "w") as f:
            f.write(GDB % {"port": port, "steps": args.steps, "out": out})
        subprocess.run(["gdb", "--batch", "-nx", "-x", script], capture_output=True, check=False)
        g.stop()
        if not os.path.exists(out):
            raise SystemExit("insnprof: gdb did not write the trace")
        pcs = json.load(open(out))
    hits = Counter()
    for pc in pcs:
        if addrs and addrs[0] <= pc < addrs[-1] + 0x10000:
            hits[names[bisect.bisect_right(addrs, pc) - 1]] += 1
        elif pc >= 0xFFFF000000000000 or pc >= 0x7F0000000000:
            hits["[tramp]"] += 1
        else:
            hits["[user]"] += 1
    n = len(pcs)
    print(f"{n} instructions of `{line}`")
    for name, c in hits.most_common(args.top):
        print(f"{100 * c / n:6.2f}%  {c:7d}  {name}")


if __name__ == "__main__":
    main()
