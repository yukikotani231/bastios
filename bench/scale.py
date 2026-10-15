#!/usr/bin/env python3
"""How BastiOS scales with processors: bench/scale.c at 1, 2, 4, ... CPUs.

    bench/scale.py [--smp 1,2,4,8] [--tests spin,fork,...] [--iters X]
                   [--kernel build/bastios.elf] [--runs N]

For each processor count, boots BastiOS once and runs every test with as many
workers as processors (the work per worker is fixed), timing it on the host
between the program's "@begin" and "@end" lines. Prints the throughput
(operations per host second) and its ratio to the one-processor run: 1.0x
per added processor would be perfect scaling. Under QEMU's TCG the host needs
at least as many cores as the guest has processors, or the numbers measure
the host.
"""
import argparse, importlib.machinery, importlib.util, os, re, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ldr = importlib.machinery.SourceFileLoader("run", os.path.join(ROOT, "bench", "run.py"))
spec = importlib.util.spec_from_loader("run", ldr)
run = importlib.util.module_from_spec(spec)
sys.argv, argv0 = sys.argv[:1], sys.argv
try:
    ldr.exec_module(run)
except SystemExit:
    pass
sys.argv = argv0

ITERS = {"spin": 4000, "syscall": 400, "fork": 100, "pipe": 40, "usock": 40, "fault": 100, "file": 400}


def build():
    os.makedirs(run.B, exist_ok=True)
    out = os.path.join(run.B, "scale")
    src = os.path.join(ROOT, "bench", "scale.c")
    if not os.path.exists(out) or os.path.getmtime(out) < os.path.getmtime(src):
        run.sh(os.environ.get("MUSLCC", "musl-gcc"), "-static-pie", "-O2", "-o", out, src)
    return out


def cpu_seconds(pid):
    """Host CPU time (user + system) QEMU has used so far: it does not move with
    other processes on the host the way wall time does."""
    try:
        f = open(f"/proc/{pid}/stat").read().rsplit(")", 1)[1].split()
        return (int(f[11]) + int(f[12])) / os.sysconf("SC_CLK_TCK")
    except OSError:
        return 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--smp", default="1,2,4")
    ap.add_argument("--tests", default=",".join(ITERS))
    ap.add_argument("--iters", type=float, default=1.0, help="scale the per-test iteration counts")
    ap.add_argument("--kernel", default=os.path.join(ROOT, "build", "bastios.elf"))
    ap.add_argument("--runs", type=int, default=1)
    ap.add_argument("--mem", default="512M")
    a = ap.parse_args()
    a.accel, a.cpu, a.linux = run.accel(), None, None
    exe = build()
    tests = a.tests.split(",")
    counts = [int(x) for x in a.smp.split(",")]
    res = {}
    for n in counts:
        a.smp = n  # run.boot reads args.smp
        with tempfile.TemporaryDirectory() as tmp:
            g = run.boot("bastios", a, exe, tmp)
            try:
                for t in tests:
                    best = None
                    for _ in range(a.runs):
                        it = max(1, int(ITERS[t] * a.iters))
                        c0 = cpu_seconds(g.q.pid)
                        at, text = g.run(f"/disk/scale {t} {n} {it}", timeout=1200)
                        cpu = cpu_seconds(g.q.pid) - c0
                        if text is None:
                            print(f"smp {n} {t}: timeout", file=sys.stderr)
                            break
                        m = re.search(rb"@begin " + t.encode() + rb"\r?\n", text)
                        e = re.search(rb"@end " + t.encode() + rb" (\d+)", text)
                        if not (m and e):
                            print(f"smp {n} {t}: bad output {text[-200:]!r}", file=sys.stderr)
                            break
                        dt = g.time_at(at + e.start()) - g.time_at(at + m.end() - 1)
                        rate = int(e.group(1)) / dt
                        if best is None or rate > best[0]:
                            best = (rate, dt, cpu / int(e.group(1)) * 1e6)
                    res[(n, t)] = best
                    if best:
                        print(f"smp {n} {t}: {best[1]:.2f} s, {best[0]:.0f} ops/s, {best[2]:.1f} us of host CPU per op", flush=True)
            finally:
                g.stop()
    print("\n| test | " + " | ".join(f"{c} CPU" for c in counts) + " |")
    print("|---|" + "---|" * len(counts))
    for t in tests:
        base = res.get((counts[0], t))
        row = []
        for c in counts:
            r = res.get((c, t))
            row.append("-" if not r or not base else f"{r[0]:.0f}/s ({r[0] / base[0]:.2f}x)")
        print(f"| {t} | " + " | ".join(row) + " |")


main()
