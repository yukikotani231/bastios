#!/usr/bin/env python3
"""How many TLB refills one return to user mode costs under QEMU's TCG, and
which pages they are for.

    bench/tlbfills.py [--kernel build/bastios.elf] [--elf build/bastios64.elf]
                      [--perf PATH] [--min 0.5] [-- COMMAND]

Every CR3 load flushes QEMU's whole software TLB, and BastiOS loads CR3 twice
per trap (the user's shadow table, then the kernel's), so a system call pays
for each page the kernel touches after the flush: the translation, and for a
page-table page the walk too, goes through tlb_set_page_full. That, far more
than the number of instructions, is what a system call costs under TCG
(~100 refills for `getppid`, about a third of its time), so the number to
bring down is the number of distinct pages a call touches, kernel code and
data and the entry area alike.

The script boots the kernel the way bench/run.py does, starts COMMAND (default
the `syscall` test of /disk/osbench), puts uprobes on QEMU's
tlb_set_page_full and helper_iret_protected (one per return to user mode),
records two seconds of them with perf, and prints the refills per return and
the pages that take part in at least --min of the returns, by MMU index
(1 user, 2 kernel, 4 page-table walks) with the kernel symbol each is in.

Needs root (uprobes), perf (linux-tools) and a QEMU that keeps its symbols.
The probes are removed on exit. A uprobe costs about 25 us a hit, so the
guest runs slowly meanwhile; the counts do not depend on that.
"""
import argparse, bisect, os, re, subprocess, sys, tempfile, time
from collections import Counter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "bench"))
import run as bench  # noqa: E402
from prof import find_perf, symbols  # noqa: E402

FILL = "bastios_fill"
IRET = "bastios_iret"
MMU = {0: "kernel/smap", 1: "user", 2: "kernel", 3: "nested", 4: "walk"}


def probes(perf, qemu, add):
    names = [(FILL, "tlb_set_page_full mmu=%si:s32 addr=%dx:x64"), (IRET, "helper_iret_protected")]
    for n, spec in names:
        if add:
            r = subprocess.run([perf, "probe", "-x", qemu, "-a", f"{n}={spec}"], capture_output=True, text=True)
            if r.returncode:
                raise SystemExit(f"tlbfills: perf probe failed (root? QEMU symbols?): {r.stderr.strip()[-300:]}")
        else:
            subprocess.run([perf, "probe", "-d", f"probe_qemu:{n}"], capture_output=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kernel", default=os.path.join(ROOT, "build", "bastios.elf"))
    ap.add_argument("--elf", default=os.path.join(ROOT, "build", "bastios64.elf"))
    ap.add_argument("--perf", default="")
    ap.add_argument("--smp", type=int, default=1)
    ap.add_argument("--mem", default="512M")
    ap.add_argument("--warm", type=float, default=10.0, help="seconds before recording")
    ap.add_argument("--min", type=float, default=0.5, help="list pages seen in at least this share of the returns")
    ap.add_argument("cmd", nargs="*", default=["/disk/osbench", "-d", "/tmp", "syscall=30000000"])
    args = ap.parse_args()
    perf = find_perf(args.perf)
    qemu = subprocess.run(["which", "qemu-system-x86_64"], capture_output=True, text=True).stdout.strip()
    addrs, names = symbols(args.elf)
    osbench = bench.build_osbench()
    ns = argparse.Namespace(accel="tcg", cpu="", mem=args.mem, smp=args.smp, kernel=args.kernel)
    probes(perf, qemu, False)
    probes(perf, qemu, True)
    try:
        with tempfile.TemporaryDirectory() as tmp:
            g = bench.boot("bastios", ns, osbench, tmp)
            g.send(" ".join(args.cmd))
            time.sleep(args.warm)
            data = os.path.join(tmp, "perf.data")
            subprocess.run([perf, "record", "-q", "-e", f"probe_qemu:{FILL}", "-e", f"probe_qemu:{IRET}",
                            "-p", str(g.q.pid), "-o", data, "--", "sleep", "2"], capture_output=True)
            out = subprocess.run([perf, "script", "-i", data, "-F", "event,trace"], capture_output=True, text=True).stdout
            g.stop()
    finally:
        probes(perf, qemu, False)
    segs, cur = [], []
    for line in out.splitlines():
        if IRET in line:
            segs.append(cur)
            cur = []
            continue
        m = re.search(r"mmu=(\d+) addr=0x([0-9a-f]+)", line)
        if m:
            cur.append((int(m.group(1)), int(m.group(2), 16) >> 12 << 12))
    segs = segs[5:-5]                       # the ends of the window are partial
    if not segs:
        raise SystemExit("tlbfills: no returns recorded (is the command running?)")
    sizes = sorted(len(s) for s in segs)
    print(f"{len(segs)} returns to user mode: refills per return: mean {sum(sizes) / len(sizes):.1f}, "
          f"median {sizes[len(sizes) // 2]}")
    seen = Counter()
    for s in segs:
        for x in set(s):
            seen[x] += 1
    rows = [(k, v / len(segs)) for k, v in seen.items() if v / len(segs) >= args.min]
    by = Counter(k[0] for k, _ in rows)
    print("pages in at least %d%% of the returns: %d (%s)" % (args.min * 100, len(rows),
          ", ".join(f"{MMU.get(m, m)} {n}" for m, n in sorted(by.items()))))
    for (m, a), f in sorted(rows, key=lambda r: (r[0][0], r[0][1])):
        name = ""
        if addrs and addrs[0] <= a + 0xFFF and a <= addrs[-1] + 0x10000:
            i = bisect.bisect_right(addrs, a) - 1
            j = bisect.bisect_right(addrs, a + 0xFFF) - 1
            name = names[i] if i == j else f"{names[i]} .. {names[j]}"
        print(f"  {MMU.get(m, m):12} {a:#018x} {f:4.0%}  {name}")


if __name__ == "__main__":
    main()
