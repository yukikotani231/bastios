#!/usr/bin/env python3
"""The same benchmark rows on two BastiOS kernels, booted by turns.

    bench/ab.py --a build/base.elf --b build/bastios.elf [--rounds 5] [--runs 3]
                [--tests syscall,fault,...] [--smp 2] [--append pcid] [--out FILE]

A shared host moves from minute to minute, by more than the change a pull
request makes, so two tables from two jobs cannot say which of the kernels is
faster. Here each round boots A and then B (B and then A in the next round),
runs every test RUNS times on each, and the median of all the rounds is
reported per kernel with the spread (min to max) of the medians of the
rounds. B / A is the ratio of times: below 1 means B is faster, also for the
throughput tests, which are kept as time per KiB inside.
"""
import argparse, json, os, statistics, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import run as R  # noqa: E402

DEFAULT = "syscall,fault,forkbig,pipelat,pipebw,tcplat,tcpbw"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", required=True, help="the base kernel")
    ap.add_argument("--b", required=True, help="the kernel under test")
    ap.add_argument("--rounds", type=int, default=5)
    ap.add_argument("--runs", type=int, default=3, help="runs of each test in a boot")
    ap.add_argument("--tests", default=DEFAULT)
    ap.add_argument("--smp", type=int, default=2)
    ap.add_argument("--mem", default="512M")
    ap.add_argument("--cpu", default="")
    ap.add_argument("--accel", default=R.accel())
    ap.add_argument("--append", default="", help="the kernels' command line (for example pcid)")
    ap.add_argument("--out", default=os.path.join(R.B, "ab.json"))
    args = ap.parse_args()
    want = set(args.tests.split(","))
    tests = [t for t in R.TESTS if t[0] in want]
    osbench = R.build_osbench()
    kernels = {"a": args.a, "b": args.b}
    rounds = {}                                  # (test key, side) -> medians of the rounds
    with tempfile.TemporaryDirectory() as tmp:
        for rnd in range(args.rounds):
            for side in (("a", "b") if rnd % 2 == 0 else ("b", "a")):
                ns = argparse.Namespace(accel=args.accel, cpu=args.cpu, mem=args.mem, smp=args.smp,
                                        kernel=kernels[side], append=args.append)
                g = R.boot("bastios", ns, osbench, tmp)
                try:
                    for test, unit, scale, isfile in tests:
                        for where in (("tmp", "disk") if isfile else ("tmp",)):
                            key = f"{test}@{where}" if isfile else test
                            runs = [R.measure(g, test, R.DIRS["bastios"][where]) for _ in range(args.runs)]
                            ok = [r["host"] for r in runs if "host" in r]
                            if ok:
                                rounds.setdefault((key, side), []).append(statistics.median(ok))
                            print(f"round {rnd} {side} {key:16} {[round(x, 1) for x in ok]}", file=sys.stderr, flush=True)
                finally:
                    g.stop()
    lines = [f"QEMU {args.accel.upper()}, -smp {args.smp}, {args.rounds} rounds of A and B by turns, "
             f"{args.runs} runs per test and boot; A {os.path.basename(args.a)}, B {os.path.basename(args.b)}.", "",
             "| Test | Unit | A | B | B / A | A, rounds | B, rounds |", "|---|---|---:|---:|---:|---|---|"]
    result = {}
    for test, unit, scale, isfile in tests:
        for where in (("tmp", "disk") if isfile else ("tmp",)):
            key = f"{test}@{where}" if isfile else test
            a, b = rounds.get((key, "a")), rounds.get((key, "b"))
            if not a or not b:
                continue
            ma, mb = statistics.median(a), statistics.median(b)
            result[key] = {"a": a, "b": b}
            span = lambda v: f"{R.fmt(min(v), unit, scale)} to {R.fmt(max(v), unit, scale)}"
            lines.append(f"| {key} | {unit} | {R.fmt(ma, unit, scale)} | {R.fmt(mb, unit, scale)} | {mb / ma:.3f}x | {span(a)} | {span(b)} |")
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w") as f:
        json.dump(result, f, indent=1)
    print("\n".join(lines))


if __name__ == "__main__":
    main()
