#!/usr/bin/env python3
"""Compares two `make bench` result files, so a regression shows next to the
change that caused it.

    bench/compare.py OLD.json NEW.json [--os bastios] [--threshold 10]

Both files come from bench/run.py (its --out, default build/bench/results.json;
bench/baseline.json is the one kept in the repository). Every figure in them is
a cost (nanoseconds per operation, per KiB, or percent of a host CPU), so a
lower number is better. The table shows the change from OLD to NEW; a test that
got slower by more than the threshold (percent, default 10) is marked "worse",
one that got faster by more than that "better". Timing under QEMU TCG is noisy
(a run to run spread of 10% is normal on a busy host), so run both files on the
same host, back to back, before trusting a small change.

Exit status is 1 if any test got worse, so a script can gate on it.
"""
import argparse, json, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import run as bench  # noqa: E402


def load(path):
    with open(path) as f:
        return json.load(f)


def rows(old, new, osname, threshold):
    a, b = old["os"].get(osname, {}), new["os"].get(osname, {})
    out = []
    for test, unit, scale, isfile in bench.TESTS:
        for where in (("tmp", "disk") if isfile else ("tmp",)):
            key = f"{test}@{where}" if isfile else test
            x, y = a.get(key), b.get(key)
            if x is None or y is None or "host" not in x or "host" not in y or x["host"] <= 0:
                continue
            ratio = y["host"] / x["host"]
            mark = ""
            if ratio > 1 + threshold / 100:
                mark = "worse"
            elif ratio < 1 - threshold / 100:
                mark = "better"
            out.append((key, unit, bench.fmt(x["host"], unit, scale), bench.fmt(y["host"], unit, scale), ratio, mark))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("old")
    ap.add_argument("new")
    ap.add_argument("--os", default="bastios")
    ap.add_argument("--threshold", type=float, default=10.0, help="percent change that counts (default 10)")
    args = ap.parse_args()
    old, new = load(args.old), load(args.new)
    lines = ["| Test | Unit | Old | New | Change | |", "|---|---|---:|---:|---:|---|"]
    worse = 0
    for key, unit, x, y, ratio, mark in rows(old, new, args.os, args.threshold):
        lines.append(f"| {key} | {unit} | {x} | {y} | {(ratio - 1) * 100:+.0f}% | {mark} |")
        worse += mark == "worse"
    print(f"{args.os}: {args.old} ({old.get('date', '?')}) -> {args.new} ({new.get('date', '?')})")
    print()
    print("\n".join(lines))
    sys.exit(1 if worse else 0)


if __name__ == "__main__":
    main()
