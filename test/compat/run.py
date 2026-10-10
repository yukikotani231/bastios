#!/usr/bin/env python3
"""The Linux compatibility suite: how much ordinary Linux userland BastiOS runs.

    run.py --kernel build/bastios.elf --disk build/compat/disk.img \\
           --out build/compat PROGRAM...

Boots the kernel in QEMU with a disk holding the suite's programs (static
musl C programs from test/compat/progs, and a BusyBox with a broad set of
applets), starts the kernel's built-in ash, and runs:

  * each PROGRAM as /disk/PROGRAM. A program runs a list of checks and
    prints "@@ NAME", then "ok NAME" or "FAIL NAME: why" for each
    (progs/compat.h);
  * each case in applets.txt, a one-line shell command whose output must
    match exactly.

The kernel prints "[pid N: Linux system call X not implemented]" for each
call it does not know. Those notes are charged to the check or case that
was running, which gives the list of missing system calls ranked by how
many checks each one shows up in.

A program that hangs is interrupted with Ctrl-C (or the machine rebooted)
and run again with COMPAT_SKIP set past the check that hung, so one hang
costs one check, not the rest of the program.

Writes OUT/report.md (tables), OUT/results.json, and prints the summary.
Exits 1 only if a check listed in test/compat/baseline.txt (checks that
passed when the list was last updated) no longer passes: the suite is a
measurement, so failing checks are expected, but it must not go backwards.
--update-baseline rewrites that list from this run.

--host runs the same programs and cases on this machine instead (from a
scratch directory), to check that the expectations are right for Linux.
"""
import argparse, json, os, re, select, shutil, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
PROMPT = b"\n# "
BB = "/disk/busybox"

NOTE = re.compile(r"\[pid \d+: Linux system call (\d+) not implemented\]")


def syscall_names():
    names = {}
    for line in open(os.path.join(HERE, "syscalls.txt")):
        if line.startswith("#"):
            continue
        toks = line.split()
        for i in range(0, len(toks), 2):
            names[int(toks[i])] = toks[i + 1]
    return names


def load_applets(path):
    """The cases in applets.txt: (name, command, expected lines)."""
    cases, cur = [], None
    for raw in open(path):
        line = raw.rstrip("\n")
        if line.startswith("== "):
            cur = [line[3:].strip(), None, []]
            cases.append(cur)
        elif cur is None or (cur[1] is None and (line.startswith("#") or not line.strip())):
            continue
        elif cur[1] is None and line.startswith("$ "):
            cur[1] = line[2:]
        elif cur[1] is not None:
            cur[2].append(line)
    out = []
    for name, cmd, exp in cases:
        while exp and not exp[-1].strip():
            exp.pop()
        out.append((name, cmd, exp))
    return out


def with_bb(cmd, bb):
    return re.sub(r"\bbb\b", bb, cmd)


# ---------------------------------------------------------------- the guest

class Guest:
    def __init__(self, kernel, disk, log):
        self.kernel, self.disk, self.log = kernel, disk, log
        self.q = None

    def boot(self):
        self.stop()
        self.out = b""
        self.q = subprocess.Popen(
            ["qemu-system-x86_64", "-kernel", self.kernel, "-m", "256M", "-smp", "2",
             "-display", "none", "-no-reboot", "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
             "-serial", "stdio", "-monitor", "none",
             "-drive", f"file={self.disk},format=raw,if=none,id=vd", "-device", "virtio-blk-pci,drive=vd",
             "-netdev", "user,id=n0", "-device", "virtio-net-pci,netdev=n0"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if not self.read_until(b"bastios> ", 0, 60):
            raise SystemExit("compat: the kernel did not reach its shell prompt")
        mark = len(self.out)
        self.send(b"ash\r")
        if not self.read_until(PROMPT, mark, 30):
            raise SystemExit("compat: ash did not start")

    def stop(self):
        if self.q:
            self.q.kill()
            self.q.wait()
            self.log.write(self.out)
            self.q = None

    def send(self, data):
        self.q.stdin.write(data)
        self.q.stdin.flush()

    def read_until(self, marker, start, timeout):
        deadline = time.time() + timeout
        while marker not in self.out[start:]:
            left = deadline - time.time()
            if left <= 0:
                return False
            r, _, _ = select.select([self.q.stdout], [], [], left)
            if not r:
                return False
            chunk = self.q.stdout.read1(65536)
            if not chunk:
                return False
            self.out += chunk
        return True

    def run(self, line, timeout):
        """Runs a shell line ending in `echo @@end $?`. Returns (text, status),
        status None if it never finished (the guest is then made usable
        again, by Ctrl-C or a reboot)."""
        mark = len(self.out)
        self.send(line.encode() + b"\r")
        done = self.read_until(b"\n@@end ", mark, timeout) and self.read_until(PROMPT, mark, 10)
        text = self.out[mark:].decode(errors="replace").replace("\r", "")
        if done:
            m = re.search(r"\n@@end (\d+)", text)
            return text, int(m.group(1)) if m else None
        mark2 = len(self.out)
        self.send(b"\x03")
        if not self.read_until(PROMPT, mark2, 10):
            self.boot()
        else:
            # The Ctrl-C ended the program; let the trailing echo run, so the
            # next line starts on a clean prompt.
            self.read_until(PROMPT, len(self.out), 2)
        return text, None


class Host:
    """The same interface, on this machine, for checking expectations."""

    def __init__(self, bindir):
        self.bindir = bindir
        self.cwd = tempfile.mkdtemp(prefix="compat-host-")

    def boot(self):
        pass

    def stop(self):
        shutil.rmtree(self.cwd, ignore_errors=True)

    def run(self, line, timeout):
        line = line.replace("/disk/", self.bindir + "/")
        try:
            # Under `script`, so the programs have a terminal as in the guest.
            p = subprocess.run(["script", "-qec", line, "/dev/null"], cwd=self.cwd, capture_output=True,
                               timeout=timeout, stdin=subprocess.DEVNULL)
        except subprocess.TimeoutExpired as e:
            return (e.stdout or b"").decode(errors="replace").replace("\r", ""), None
        text = p.stdout.decode(errors="replace").replace("\r", "")
        m = re.search(r"(?:^|\n)@@end (\d+)", text)
        return text, int(m.group(1)) if m else None


# ---------------------------------------------------------------- running

def run_program(env, prog, timeout):
    """Runs /disk/PROG, rerunning past hangs. Returns its checks, in order:
    dicts with name, result (pass / fail / hang / crash), detail, syscalls."""
    checks, skip, rounds = [], 0, 0
    while rounds < 20:
        rounds += 1
        pre = f"COMPAT_SKIP={skip} " if skip else ""
        text, status = env.run(f"{pre}/disk/{prog}; echo @@end $?", timeout)
        cur, idx = None, skip
        for line in text.split("\n"):
            m = NOTE.search(line)
            if m and cur is not None:
                cur["syscalls"].append(int(m.group(1)))
            if line.startswith("@@ "):
                cur = {"name": line[3:], "result": None, "detail": "", "syscalls": [], "index": idx}
                checks.append(cur)
                idx += 1
            elif cur is not None and line.startswith("ok ") and line[3:] == cur["name"]:
                cur["result"] = "pass"
            elif cur is not None and line.startswith("FAIL " + cur["name"] + ": "):
                cur["result"] = "fail"
                cur["detail"] = line[len("FAIL " + cur["name"] + ": "):]
            elif cur is not None and cur["result"] is None and line.startswith("[") and "not implemented" not in line:
                cur["detail"] = (cur["detail"] + " " + line).strip()    # a kernel note: a fault, a kill
        if cur is not None and cur["result"] is None:
            # The program stopped inside this check: hung, or died.
            cur["result"] = "hang" if status is None else "crash"
            if status is not None and not cur["detail"]:
                cur["detail"] = f"program exited with status {status}"
            skip = cur["index"] + 1
            continue
        if status is None and cur is None:
            checks.append({"name": "(start-up)", "result": "hang", "detail": "no output", "syscalls": [], "index": skip})
        elif status is not None and cur is None and skip == 0:
            checks.append({"name": "(start-up)", "result": "crash", "detail": f"exited with {status} before any check",
                           "syscalls": [int(n) for n in NOTE.findall(text)], "index": 0})
        break
    for c in checks:
        if c["result"] is None:
            # Its result line never came, though the program went on (say,
            # the shell running it was killed).
            c["result"] = "crash"
            c["detail"] = (c["detail"] + " no result line").strip()
        c["syscalls"] = sorted(set(c["syscalls"]))
        c.pop("index", None)
    return checks


def run_applet(env, name, cmd, expected, timeout, bb):
    text, status = env.run(f"echo @@begin; {with_bb(cmd, bb)}; echo @@end $?", timeout)
    lines = text.split("\n")
    try:
        start = lines.index("@@begin") + 1
    except ValueError:
        start = len(lines)
    body, calls, notes = [], [], []
    for line in lines[start:]:
        if line.startswith("@@end"):
            break
        m = NOTE.search(line)
        if m:
            calls.append(int(m.group(1)))
            line = NOTE.sub("", line)
            if not line:
                continue
        body.append(line)
    if status is None:
        return {"name": name, "result": "hang", "detail": "did not finish", "syscalls": sorted(set(calls))}
    ok = body == expected
    detail = "" if ok else "got " + json.dumps("\n".join(body)[:200]) + ", expected " + json.dumps("\n".join(expected))
    return {"name": name, "result": "pass" if ok else "fail", "detail": detail, "syscalls": sorted(set(calls))}


# ---------------------------------------------------------------- reporting

def report(results, names, out_dir, elapsed):
    rows = [(g, c) for g, cs in results.items() for c in cs]
    total = len(rows)
    passed = sum(1 for _, c in rows if c["result"] == "pass")
    rank = {}
    for g, c in rows:
        if c["result"] == "pass":
            continue
        for n in c["syscalls"]:
            rank.setdefault(n, []).append(f"{g}: {c['name']}")
    ranked = sorted(rank.items(), key=lambda kv: (-len(kv[1]), kv[0]))
    md = ["# BastiOS Linux compatibility report", "",
          f"{passed} of {total} checks pass ({elapsed:.0f} s). Generated by `make compat` (test/compat/run.py).", "",
          "| Group | Checks | Pass | Fail | Hang | Crash |", "|---|---:|---:|---:|---:|---:|"]
    for g, cs in results.items():
        cnt = {k: sum(1 for c in cs if c["result"] == k) for k in ("pass", "fail", "hang", "crash")}
        md.append(f"| {g} | {len(cs)} | {cnt['pass']} | {cnt['fail']} | {cnt['hang']} | {cnt['crash']} |")
    md += ["", "## Missing system calls, by checks blocked", "",
           "A system call counts against a check when the kernel reported it as not implemented while that "
           "check was running and the check did not pass.", "",
           "| Rank | System call | Nr | Checks | Examples |", "|---:|---|---:|---:|---|"]
    for i, (n, who) in enumerate(ranked, 1):
        ex = "; ".join(who[:3]) + (" ..." if len(who) > 3 else "")
        md.append(f"| {i} | {names.get(n, '?')} | {n} | {len(who)} | {ex} |")
    nosys = [(g, c) for g, c in rows if c["result"] != "pass" and not c["syscalls"]]
    md += ["", "## Failures with no missing system call", "",
           "These checks fail although every call they made exists: wrong results, missing files "
           "(/dev, /proc, /etc), unsupported flags, hangs, or crashes.", "",
           "| Group | Check | Result | Detail |", "|---|---|---|---|"]
    for g, c in nosys:
        md.append(f"| {g} | {c['name']} | {c['result']} | {esc(c['detail'])} |")
    md += ["", "## Every check", "", "| Group | Check | Result | Missing calls | Detail |", "|---|---|---|---|---|"]
    for g, c in rows:
        sc = ", ".join(names.get(n, str(n)) for n in c["syscalls"])
        md.append(f"| {g} | {c['name']} | {c['result']} | {sc} | {esc(c['detail'])} |")
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "report.md"), "w") as f:
        f.write("\n".join(md) + "\n")
    with open(os.path.join(out_dir, "results.json"), "w") as f:
        json.dump({"passed": passed, "total": total, "groups": results,
                   "missing": [{"nr": n, "name": names.get(n, "?"), "checks": who} for n, who in ranked]}, f, indent=1)
    print(f"compat: {passed} of {total} checks pass")
    for g, cs in results.items():
        print(f"  {g:10} {sum(1 for c in cs if c['result'] == 'pass'):3} / {len(cs)}")
    print("compat: missing system calls, by checks blocked:")
    for n, who in ranked[:15]:
        print(f"  {names.get(n, '?'):20} {n:4} {len(who)}")
    print(f"compat: full report in {os.path.join(out_dir, 'report.md')}")


def esc(s):
    return s.replace("|", "\\|").replace("\n", " ")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kernel", default="build/bastios.elf")
    ap.add_argument("--disk", default="build/compat/disk.img")
    ap.add_argument("--out", default="build/compat")
    ap.add_argument("--host", metavar="BINDIR", help="run on this machine, programs from BINDIR")
    ap.add_argument("--timeout", type=float, default=60)
    ap.add_argument("--update-baseline", action="store_true")
    ap.add_argument("programs", nargs="*")
    args = ap.parse_args()

    names = syscall_names()
    applets = load_applets(os.path.join(HERE, "applets.txt"))
    os.makedirs(args.out, exist_ok=True)
    log = open(os.path.join(args.out, "serial.log"), "wb")
    env = Host(os.path.abspath(args.host)) if args.host else Guest(args.kernel, args.disk, log)
    bb = os.path.join(os.path.abspath(args.host), "busybox") if args.host else BB
    t0 = time.time()
    results = {}
    try:
        env.boot()
        for prog in args.programs:
            print(f"compat: {prog}", flush=True)
            results[prog] = run_program(env, prog, args.timeout)
        print("compat: busybox applets", flush=True)
        results["busybox"] = [run_applet(env, n, c, e, 30, bb) for n, c, e in applets]
    finally:
        env.stop()
        log.close()
    report(results, names, args.out, time.time() - t0)

    ids = [f"{g}: {c['name']}" for g, cs in results.items() for c in cs if c["result"] == "pass"]
    base = os.path.join(HERE, "baseline.txt")
    if args.host:
        return 0
    if args.update_baseline:
        with open(base, "w") as f:
            f.write("# Checks that pass on BastiOS (make compat-baseline rewrites this).\n"
                    "# make compat fails if one of them stops passing.\n")
            f.write("".join(i + "\n" for i in sorted(ids)))
        print(f"compat: baseline rewritten, {len(ids)} checks")
        return 0
    if os.path.exists(base):
        want = [l.strip() for l in open(base) if l.strip() and not l.startswith("#")]
        lost = sorted(set(want) - set(ids))
        if lost:
            print("compat: REGRESSION, these checks passed before:")
            for i in lost:
                print("  " + i)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
