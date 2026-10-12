#!/usr/bin/env python3
"""Runs the Linux Test Project's system call tests on BastiOS.

    ltp.py --kernel build/bastios.elf --ltp build/ltp --out build/compat/ltp

LTP (GPL-2.0-or-later, not part of this repository) is fetched and built by
`make compat-ltp` as static musl programs. This script reads LTP's own list
of system call tests (runtest/syscalls: a tag, then a command line), puts the
programs on disk images in BastiOS's format (the disk holds 64 files, so the
tests run in batches, one boot each), and runs every test from the kernel's
ash, with TMPDIR=/tmp.

An LTP test's exit status says how it went: 0 passed, bit 0 a failure
(TFAIL), bit 1 broken (TBROK: the test could not set itself up), bit 2 a
warning, 32 unsupported here (TCONF). A test that does not finish within
the timeout is interrupted and counted as a hang.

As in run.py, the kernel's "[pid N: Linux system call X not implemented]"
notes are charged to the test that was running, which ranks the missing
system calls by how many tests each one blocks.

Writes OUT/report.md, OUT/results.json and OUT/serial.log. Exits 1 only if a
test listed in test/compat/ltp_baseline.txt no longer passes;
--update-baseline rewrites that list. There is no --host mode: many of these
tests change the machine they run on (the clock, mounts, swap, limits), so
they are not run on the build machine. LTP's own CI runs them on Linux.
"""
import argparse, concurrent.futures, io, json, math, os, re, shlex, shutil, subprocess, sys, tempfile, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run import Guest, NOTE, esc, syscall_names  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
BATCH = 63              # programs per disk image (the disk holds 64 files, one is kconfig)
TFAIL, TBROK, TWARN, TCONF = 1, 2, 4, 32
WAITED = 20             # seconds: a failing test this slow was waiting out a timeout
# LTP's result lines: "file.c:12: TFAIL: why", or from tests on its old
# library, "name 1 TFAIL : file.c:12: why".
RESULT = re.compile(r"^(?:\S+:\d+:|\S+\s+\d+) +(TFAIL|TBROK|TWARN|TCONF|TPASS|TINFO) *: *(.*)$")


def is_elf(path):
    with open(path, "rb") as f:
        return f.read(4) == b"\x7fELF"


def load_tests(ltp, only, skip, suites=()):
    """The entries of runtest/syscalls whose program was built: (tag, argv,
    files), files being the programs it needs on the disk: its own, and the
    helper programs next to it that are not tests themselves (execve_child
    and the like, which LTP copies from the directory a test starts in)."""
    progs, dirs = {}, {}
    names = ["syscalls", *suites]
    for suite in names:
        for root, _, files in os.walk(os.path.join(ltp, "testcases", "kernel", suite)):
            for f in sorted(files):
                p = os.path.join(root, f)
                if os.access(p, os.X_OK) and "." not in f and is_elf(p) and f not in progs:
                    progs[f] = p
                    dirs.setdefault(root, []).append(f)
    entries = []
    for suite in names:
        for line in open(os.path.join(ltp, "runtest", suite)):
            line = line.strip()
            if line and not line.startswith("#"):
                tag, cmd = line.split(None, 1)
                entries.append((tag, shlex.split(cmd)))
    tested = {argv[0] for _, argv in entries}
    tests, missing = [], []
    for tag, argv in entries:
        if (only and not re.search(only, tag)) or tag in skip:
            continue
        if argv[0] not in progs:
            missing.append(tag)
            continue
        helpers = [f for f in dirs[os.path.dirname(progs[argv[0]])] if f not in tested]
        tests.append((tag, argv, {argv[0], *helpers}))
    return tests, progs, missing


def load_skip():
    """test/compat/ltp_skip.txt: tags not run, with the reason."""
    skip = {}
    path = os.path.join(HERE, "ltp_skip.txt")
    if os.path.exists(path):
        for line in open(path):
            line = line.split("#", 1)[0].strip()
            if line:
                tag, _, why = line.partition(" ")
                skip[tag] = why.strip()
    return skip


def classify(status, text):
    """(result, detail) from an exit status and the test's output."""
    bad = []
    for line in text.split("\n"):
        m = RESULT.match(line)
        if m and m.group(1) in ("TFAIL", "TBROK"):
            bad.append(f"{m.group(1)}: {m.group(2)}")
        elif line.startswith("[pid ") and "not implemented" not in line:
            bad.append(line.strip())     # a kernel note: a fault, a kill
    if status is None:
        return "hang", "; ".join(bad[:2]) or "did not finish"
    # A test with child processes keeps its counts in a MAP_SHARED page;
    # where the children's updates do not reach the parent, it exits 0
    # though a child printed TFAIL. Its own lines count, not just its status.
    for line in text.split("\n"):
        m = RESULT.match(line)
        if m and m.group(1) in ("TFAIL", "TBROK", "TWARN"):
            status |= {"TFAIL": TFAIL, "TBROK": TBROK, "TWARN": TWARN}[m.group(1)]
    if status == 0:
        return "pass", ""
    if status & TBROK:
        return "broken", "; ".join(bad[:2]) or f"exit {status}"
    if status & TFAIL:
        return "fail", "; ".join(bad[:2]) or f"exit {status}"
    if status == TCONF:
        conf = [RESULT.match(l).group(2) for l in text.split("\n") if RESULT.match(l) and RESULT.match(l).group(1) == "TCONF"]
        return "unsupported", conf[0] if conf else ""
    if status == TWARN or status == TWARN | TCONF:
        return "pass", "with warnings"
    return "crash", f"exit {status}" + (": " + "; ".join(bad[:2]) if bad else "")


def batches(tests, n):
    """Groups the tests, in order, so that no group needs more than n
    programs on its disk."""
    cur, files = [], set()
    for t in tests:
        if cur and len(files | t[2]) > n:
            yield cur
            cur, files = [], set()
        cur.append(t)
        files |= t[2]
    if cur:
        yield cur


def run_batch(args, group, progs, work, log, idle=False):
    """Boots a kernel with this batch's programs on its disk and runs its
    tests. Returns their results."""
    os.makedirs(work)
    files = []
    for p in sorted(set().union(*(t[2] for t in group))):
        dst = os.path.join(work, p)
        subprocess.run(["strip", "-o", dst, progs[p]], check=True)
        files.append(dst)
    disk = os.path.join(work, "disk.img")
    kconfig = os.path.join(work, "kconfig")
    shutil.copy(os.path.join(HERE, "ltp_kconfig"), kconfig)
    subprocess.run([os.path.join(HERE, "mkdisk.py"), disk, kconfig] + files, check=True)
    results = []
    g = Guest(args.kernel, disk, log, idle=idle)
    try:
        g.boot()
        for tag, argv, _ in group:
            # From /disk, where LTP finds a test's helper programs. A test
            # that wants a file system on its device is given tmpfs, which
            # needs no mkfs (there is no ext2 here). PATH has the helpers
            # and BusyBox's tools (cp for SAFE_CP), as runltp's would.
            cmd = " ".join(shlex.quote(a) for a in ["/disk/" + argv[0]] + argv[1:])
            t0 = time.time()
            text, status = g.run(f"cd /disk; PATH=/disk:/bin TMPDIR=/tmp KCONFIG_PATH=/disk/kconfig LTP_DEV_FS_TYPE=tmpfs LTP_COLORIZE_OUTPUT=0 {cmd}; "
                                 "echo @@end $?", args.timeout)
            results.append(record(tag, text, status))
            results[-1]["secs"] = round(time.time() - t0, 1)
    except SystemExit as e:
        # The kernel did not come back up after a hang: the rest of the
        # batch did not run.
        for tag, _, _ in group[len(results):]:
            results.append({"name": tag, "result": "crash", "detail": f"not run: {e}", "syscalls": []})
    finally:
        g.stop()
        shutil.rmtree(work, ignore_errors=True)
    return results


def waiting(tests, last, gated):
    """Splits the tests into the usual ones and those that, last time,
    failed after waiting out a timeout (LTP's own, or ours) and are not in
    the baseline: those run on machines of their own that get only idle
    CPU time (see run_guest). A test in the baseline (gated) always runs as usual, so a
    timing test that once failed slowly is not moved where it would fail
    again. last is the previous run's results.json, if any; without one
    every test runs as usual, and so does every test when gated is None
    (--update-baseline: a test that passes now must not be starved of the
    chance)."""
    prev = {}
    try:
        with open(last) as f:
            prev = {t["name"]: t for t in json.load(f)["tests"]}
    except (OSError, ValueError, KeyError, TypeError):
        pass
    busy, idle = [], []
    for t in tests:
        p = prev.get(t[0])
        slow = gated is not None and t[0] not in gated and p and p.get("result") != "pass" and p.get("secs", 0) >= WAITED
        (idle if slow else busy).append(t)
    return busy, idle, {k: p.get("secs", 0) for k, p in prev.items()}


def run_guest(args, tests, progs, log, gated):
    """Runs the batches, args.jobs machines at a time. The tests that only
    wait (see waiting()) run besides those, on as many more machines as it
    takes to finish them within about the time the others take. A machine
    waiting in a sleep still keeps a host CPU busy (the kernel retries a
    sleeping call until its time is up), so those machines run under
    SCHED_IDLE: the others, where the timing tests are, keep the CPUs they
    had. (At nice 19 they still took enough to fail epoll_pwait03's
    timing in 4 runs of 4.)"""
    busy, idle, secs = waiting(tests, os.path.join(args.out, "results.json"), gated)
    # The batches are made from every test, as before, and then lose the
    # waiting ones, so that each test runs after the same tests as before
    # (a test can leave the kernel short of something, like pipes, that a
    # later one in its batch needs).
    moved = {t[0] for t in idle}
    groups = [g for g in ([t for t in b if t[0] not in moved] for b in batches(tests, BATCH)) if g]
    lanes = []
    if idle:
        per_lane = max(WAITED, sum(secs.get(t[0], 0) for t in busy) / args.jobs)
        n = max(1, math.ceil(sum(secs[t[0]] for t in idle) / per_lane))
        lanes = [[] for _ in range(n)]
        load = [0.0] * n
        for t in sorted(idle, key=lambda t: -secs[t[0]]):
            i = load.index(min(load))
            lanes[i].append(t)
            load[i] += secs[t[0]]
        lanes = [sorted(l, key=tests.index) for l in lanes if l]
    every = groups + lanes
    work = tempfile.mkdtemp(prefix="ltp-disk-")
    logs = [io.BytesIO() for _ in every]
    done = []

    def one(i):
        r = run_batch(args, every[i], progs, os.path.join(work, str(i)), logs[i], i >= len(groups))
        done.append(i)
        bad = sum(1 for x in r if x["result"] not in ("pass", "unsupported"))
        print(f"ltp: batch {i + 1} done ({len(done)} of {len(every)}): "
              f"{sum(1 for x in r if x['result'] == 'pass')} of {len(r)} pass, {bad} fail", flush=True)
        return r

    try:
        with concurrent.futures.ThreadPoolExecutor(max(1, len(lanes))) as side, \
             concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
            later = [side.submit(one, len(groups) + i) for i in range(len(lanes))]
            per = list(ex.map(one, range(len(groups)))) + [f.result() for f in later]
    finally:
        shutil.rmtree(work, ignore_errors=True)
        for b in logs:
            log.write(b.getvalue())
    order = {t[0]: i for i, t in enumerate(tests)}
    return sorted((r for rs in per for r in rs), key=lambda r: order[r["name"]])


def record(tag, text, status):
    result, detail = classify(status, text)
    calls = sorted({int(n) for n in NOTE.findall(text)})
    return {"name": tag, "result": result, "detail": detail[:300], "syscalls": calls}


def report(results, missing, skip, names, out_dir, elapsed):
    kinds = ("pass", "fail", "broken", "unsupported", "hang", "crash")
    cnt = {k: sum(1 for r in results if r["result"] == k) for k in kinds}
    rank = {}
    for r in results:
        if r["result"] not in ("pass", "unsupported"):
            for n in r["syscalls"]:
                rank.setdefault(n, []).append(r["name"])
    ranked = sorted(rank.items(), key=lambda kv: (-len(kv[1]), kv[0]))
    md = ["# LTP system call tests on BastiOS", "",
          f"{cnt['pass']} of {len(results)} tests pass ({elapsed:.0f} s). Generated by `make compat-ltp` "
          "(test/compat/ltp.py) from LTP's runtest/syscalls.", "",
          "| " + " | ".join(k.capitalize() for k in kinds) + " |", "|" + "---:|" * len(kinds),
          "| " + " | ".join(str(cnt[k]) for k in kinds) + " |", "",
          f"Not run: {len(missing)} tests whose program did not build against musl, and {len(skip)} listed in "
          "test/compat/ltp_skip.txt.", "",
          "## Missing system calls, by tests blocked", "",
          "| Rank | System call | Nr | Tests | Examples |", "|---:|---|---:|---:|---|"]
    for i, (n, who) in enumerate(ranked, 1):
        ex = ", ".join(who[:5]) + (" ..." if len(who) > 5 else "")
        md.append(f"| {i} | {names.get(n, '?')} | {n} | {len(who)} | {ex} |")
    why = {}
    for r in results:
        if r["result"] not in ("pass", "unsupported"):
            key = re.sub(r"\d+", "N", r["detail"].split("; ")[0])[:120] or "(no message)"
            why.setdefault(key, []).append(r["name"])
    md += ["", "## Most common reasons for not passing", "",
           "The first TFAIL or TBROK line of each test that failed, broke, hung or crashed, numbers "
           "replaced by N.", "", "| Tests | Reason | Examples |", "|---:|---|---|"]
    for key, who in sorted(why.items(), key=lambda kv: -len(kv[1]))[:40]:
        md.append(f"| {len(who)} | {esc(key)} | {', '.join(who[:5])}{' ...' if len(who) > 5 else ''} |")
    md += ["", "## Every test that did not pass", "", "| Test | Result | Missing calls | Detail |", "|---|---|---|---|"]
    for r in results:
        if r["result"] != "pass":
            sc = ", ".join(names.get(n, str(n)) for n in r["syscalls"])
            md.append(f"| {r['name']} | {r['result']} | {sc} | {esc(r['detail'])} |")
    md += ["", "## Passing tests", "", ", ".join(r["name"] for r in results if r["result"] == "pass")]
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "report.md"), "w") as f:
        f.write("\n".join(md) + "\n")
    with open(os.path.join(out_dir, "results.json"), "w") as f:
        json.dump({"counts": cnt, "total": len(results), "tests": results, "not_built": missing,
                   "missing": [{"nr": n, "name": names.get(n, "?"), "tests": who} for n, who in ranked]}, f, indent=1)
    print(f"ltp: {cnt['pass']} of {len(results)} tests pass; " + ", ".join(f"{cnt[k]} {k}" for k in kinds[1:]))
    print("ltp: missing system calls, by tests blocked:")
    for n, who in ranked[:15]:
        print(f"  {names.get(n, '?'):20} {n:4} {len(who)}")
    print(f"ltp: full report in {os.path.join(out_dir, 'report.md')}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kernel", default="build/bastios.elf")
    ap.add_argument("--ltp", default="build/ltp", help="the built LTP tree")
    ap.add_argument("--out", default="build/compat/ltp")
    ap.add_argument("--only", help="run only tags matching this regular expression")
    ap.add_argument("--timeout", type=float, default=60)
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2),
                    help="machines run at once (each has two processors)")
    ap.add_argument("--update-baseline", action="store_true")
    ap.add_argument("--suite", action="append", default=[],
                    help="also run runtest/NAME with the programs in testcases/kernel/NAME (pty, sched, ...)")
    args = ap.parse_args()

    base = os.path.join(HERE, "ltp_baseline.txt")
    gated = None
    if os.path.exists(base) and not args.update_baseline:
        gated = {l.strip() for l in open(base) if l.strip() and not l.startswith("#")}
    skip = load_skip()
    tests, progs, missing = load_tests(args.ltp, args.only, skip, args.suite)
    os.makedirs(args.out, exist_ok=True)
    t0 = time.time()
    with open(os.path.join(args.out, "serial.log"), "wb") as log:
        results = run_guest(args, tests, progs, log, gated)
    report(results, missing, skip, syscall_names(), args.out, time.time() - t0)

    ids = sorted(r["name"] for r in results if r["result"] == "pass")
    if args.only:
        return 0
    if args.update_baseline:
        with open(base, "w") as f:
            f.write("# LTP tests that pass on BastiOS (make compat-ltp-baseline rewrites this).\n"
                    "# make compat-ltp fails if one of them stops passing.\n")
            f.write("".join(i + "\n" for i in ids))
        print(f"ltp: baseline rewritten, {len(ids)} tests")
        return 0
    if os.path.exists(base):
        want = [l.strip() for l in open(base) if l.strip() and not l.startswith("#")]
        lost = sorted(set(want) - set(ids))
        # A few tests depend on timing and fail now and then (clock_gettime04,
        # shmctl01). A test that stopped passing runs again, twice at most,
        # alone on a fresh kernel; one that passes then is reported as flaky,
        # not as a regression. A real regression fails every time.
        for attempt in (1, 2):
            if not lost or args.only:
                break
            again = [t for t in tests if t[0] in set(lost)]
            print(f"ltp: running {len(lost)} lost test(s) again (attempt {attempt}): {', '.join(lost)}")
            with open(os.path.join(args.out, "serial.log"), "ab") as log:
                rerun = run_guest(args, again, progs, log, None)
            fixed = sorted(r["name"] for r in rerun if r["result"] == "pass")
            for n in fixed:
                print(f"ltp: FLAKY, {n} failed once and passed on a second run")
            lost = sorted(set(lost) - set(fixed))
        if lost:
            print("ltp: REGRESSION, these tests passed before:")
            for i in lost:
                print("  " + i)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
