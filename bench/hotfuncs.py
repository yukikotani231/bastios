#!/usr/bin/env python3
"""Regenerates bench/hot-functions.txt, the kernel functions the linker puts
together at the start of the text (see that file and build/hot.ld in the
Makefile).

    bench/hotfuncs.py [--kernel build/bastios.elf] [--elf build/bastios64.elf]

Single-steps the guest (bench/insnprof.py) through the workloads of
bench/scale.c, a system call, a file open/read/close, fork + exit, a page
fault and a pipe ping-pong, and lists the functions that ran, those used by
the most workloads first and the busiest first among them. Needs gdb.
"""
import argparse, bisect, collections, json, os, re, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORKLOADS = ["syscall 1 5000", "file 1 2000", "fork 1 100000", "fault 1 100000", "pipe 1 100", "usock 1 100"]


def symbols(elf):
    out = subprocess.run(["nm", "-n", "--defined-only", elf], capture_output=True, text=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[1] in "tTwW":
            addrs.append(int(p[0], 16))
            names.append(p[2])
    return addrs, names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kernel", default=os.path.join(ROOT, "build", "bastios.elf"))
    ap.add_argument("--elf", default=os.path.join(ROOT, "build", "bastios64.elf"))
    ap.add_argument("--steps", type=int, default=150000)
    ap.add_argument("--out", default=os.path.join(ROOT, "bench", "hot-functions.txt"))
    a = ap.parse_args()
    addrs, names = symbols(a.elf)
    used, total = collections.Counter(), collections.Counter()
    with tempfile.TemporaryDirectory() as tmp:
        for w in WORKLOADS:
            pcs_file = os.path.join(tmp, w.split()[0] + ".json")
            subprocess.run([sys.executable, os.path.join(ROOT, "bench", "insnprof.py"), "--kernel", a.kernel,
                            "--elf", a.elf, "--prog", os.path.join(ROOT, "bench", "scale.c"),
                            "--steps", str(a.steps), "--top", "1", "--save", pcs_file, "--", "/disk/scale", w],
                           check=True, stdout=subprocess.DEVNULL)
            c = collections.Counter()
            for pc in json.load(open(pcs_file)):
                if addrs[0] <= pc < addrs[-1] + 0x10000:
                    c[names[bisect.bisect_right(addrs, pc) - 1]] += 1
            for f, k in c.items():
                used[f] += 1
                total[f] += k
    order = []
    for f in sorted(total, key=lambda f: (-used[f], -total[f])):
        base = re.sub(r"\.(isra|part|constprop|cold)\.\d+", "", f)
        # Code in boot/boot.S and boot/ap.S has no function section of its own.
        if base not in order and (base.startswith("f_") or base.startswith("bst_")):
            order.append(base)
    head = [l for l in open(a.out) if l.startswith("#")] if os.path.exists(a.out) else []
    with open(a.out, "w") as f:
        f.writelines(head)
        f.write("\n".join(order) + "\n")
    print(f"{len(order)} functions written to {a.out}")


main()
