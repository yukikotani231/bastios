#!/usr/bin/env python3
"""Runs build and tooling workloads on BastiOS and on Linux in the same QEMU
machine: the same Alpine programs (gcc, make, apk, tar, gzip, find, grep)
on the same Alpine root, on each system's own kernel and file system.

    bench/workloads.py [--os bastios,linux] [--runs N] [--phases a,b]
                       [--disk build/bench/alpine.img] [--root-dir build/bench/alpine/disk]
                       [--files N] [--funcs N] [--out FILE]

bench/run.py times single system calls; this times whole programs. The
disk comes from test/compat/alpinedisk.py (`make bench-workloads` builds it):
Alpine's minirootfs as root.tar, a local package repository, wl.sh and the
generated C project. BastiOS reads it as its own disk format; for Linux the
same files go into an ext4 image. bench/workloads/wl.sh does the work in the
guest, entering the unpacked root with chroot, and prints "@begin" and "@end"
lines that the host stamps as they arrive on the serial port (the same
timing as bench/run.py). Phases that change the root (untar, apk) run once;
the others run RUNS times and the median is reported. The first scan run is
cold and the later ones are not, so its median is a warm one.
"""
import argparse, json, os, re, shutil, statistics, subprocess, sys, tempfile, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import run as R

# (phase, what it is, runs: None = RUNS)
PHASES = [
    ("untar", "unpack Alpine's mini root (about 500 small files)", 1),
    ("apk", "apk add gcc, make and musl-dev from a local repository", 1),
    ("hello", "gcc -O2 of one small C file, compile and link", None),
    ("make1", "make -j1 of a generated project: one gcc -O1 per file, then a link", None),
    ("make2", "the same with make -j2", None),
    ("tgz", "tar the gcc and header trees through gzip -1 into one file", None),
    ("scan", "find over the whole root, grep -rl over the headers", None),
]


def boot(osname, args, tmp):
    common = ["-cpu", args.cpu or ("qemu64,+pdpe1gb" if args.accel == "tcg" else "host"), "-accel", args.accel,
              "-m", args.mem, "-smp", str(args.smp), "-display", "none", "-no-reboot",
              "-serial", "stdio", "-monitor", "none", "-nic", "none"]
    log = os.path.join(R.B, f"wl-{osname}.log")
    if osname == "bastios":
        disk = os.path.join(tmp, "bastios.img")
        shutil.copyfile(args.disk, disk)
        cmd = ["qemu-system-x86_64", "-kernel", args.kernel,
               "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
               "-drive", f"file={disk},format=raw,if=none,id=vd", "-device", "virtio-blk-pci,drive=vd"] + common
        g = R.Guest(cmd, "# ", log)
        if not g.until(rb"bastios> ", 0, 300):
            raise SystemExit("workloads: BastiOS gave no prompt")
        at = len(g.out)
        g.send("ash")
        if not g.until(rb"# ", at, 60):
            raise SystemExit("workloads: ash did not start")
        g.bb = "/disk/busybox"
    else:
        initrd, _ = R.linux_initrd(R.build_osbench(), tmp)
        disk = os.path.join(tmp, "linux.img")
        stage = os.path.join(tmp, "stage")
        top = os.path.join(args.root_dir)
        os.makedirs(os.path.join(stage, "a", "repo", "x86_64"))
        for f in os.listdir(os.path.join(top, "repo")):
            shutil.copyfile(os.path.join(top, "repo", f), os.path.join(stage, "a", "repo", "x86_64", f))
        for f in ("root.tar",):
            shutil.copyfile(os.path.join(top, f), os.path.join(stage, f))
        for f in ("wl.sh", "proj.tar"):
            shutil.copyfile(os.path.join(os.path.dirname(args.disk), "wl", f), os.path.join(stage, f))
        with open(disk, "wb") as f:
            f.truncate(1536 << 20)
        R.sh("mke2fs", "-q", "-t", "ext4", "-F", "-d", stage, disk)
        shutil.rmtree(stage)
        cmd = ["qemu-system-x86_64", "-kernel", R.linux_kernel(args.linux), "-initrd", initrd,
               "-append", "console=ttyS0 quiet panic=-1 mitigations=off transparent_hugepage=never rdinit=/init",
               "-drive", f"file={disk},format=raw,if=none,id=vd", "-device", "virtio-blk-pci,drive=vd"] + common
        g = R.Guest(cmd, "# ", log)
        if not g.until(rb"READY", 0, 600):
            raise SystemExit("workloads: Linux did not reach its shell")
        g.until(rb"# ", len(g.out) - 1, 30)
        g.send("stty -echo 2>/dev/null; PS1='# '")
        time.sleep(1)
        g.bb = "/bin/busybox"
    return g


def measure(g, phase, timeout):
    """Seconds one phase took by the host's clock, or an error text."""
    at = len(g.out)
    g.send(f"set -- {g.bb} {phase}; . /disk/wl.sh; echo @@do''ne")
    if not g.until(rb"@@done", at, timeout):
        return {"error": "timeout"}
    text = g.out[at:]
    b = re.search(rb"@begin " + phase.encode() + rb"\r?\n", text)
    e = re.search(rb"@end " + phase.encode() + rb" (\d+)", text)
    if not b or not e:
        return {"error": text[-400:].decode(errors="replace")}
    if int(e.group(1)) != 0:
        return {"error": f"status {int(e.group(1))}: " + text[:e.start()][-300:].decode(errors="replace")}
    g.until(rb"# ", at + e.end(), 30)
    t0 = g.time_at(at + b.end() - 1)
    t1 = g.time_at(at + e.start())
    return {"seconds": t1 - t0}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--os", default="bastios,linux")
    ap.add_argument("--smp", type=int, default=2)
    ap.add_argument("--mem", default="512M")
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--phases", default="")
    ap.add_argument("--kernel", default=os.path.join(R.ROOT, "build", "bastios.elf"))
    ap.add_argument("--linux", default=os.environ.get("LINUX_KERNEL", ""))
    ap.add_argument("--accel", default=R.accel())
    ap.add_argument("--cpu", default="")
    ap.add_argument("--disk", default=os.path.join(R.B, "alpine.img"))
    ap.add_argument("--root-dir", default=os.path.join(R.B, "alpine", "disk"))
    ap.add_argument("--timeout", type=int, default=3600)
    ap.add_argument("--out", default=os.path.join(R.B, "workloads.json"))
    args = ap.parse_args()
    want = set(args.phases.split(",")) if args.phases else None
    os.makedirs(R.B, exist_ok=True)
    results = {"accel": args.accel, "smp": args.smp, "mem": args.mem, "runs": args.runs,
               "host_cpus": os.cpu_count(), "date": time.strftime("%Y-%m-%d %H:%M:%S"),
               "phases": {p: d for p, d, _ in PHASES}, "os": {}}
    with tempfile.TemporaryDirectory() as tmp:
        for osname in args.os.split(","):
            g = boot(osname, args, tmp)
            res = results["os"][osname] = {}
            try:
                for phase, _, n in PHASES:
                    if want and phase not in want and phase not in ("untar", "apk"):
                        continue
                    runs = [measure(g, phase, args.timeout) for _ in range(n or args.runs)]
                    ok = [r["seconds"] for r in runs if "seconds" in r]
                    res[phase] = ({"seconds": statistics.median(ok), "runs": [round(x, 2) for x in ok]}
                                  if ok else runs[0])
                    print(f"{osname:8} {phase:8} {res[phase]}", file=sys.stderr, flush=True)
            finally:
                g.stop()
    with open(args.out, "w") as f:
        json.dump(results, f, indent=1)
    print(table(results))


def table(results):
    oses = list(results["os"])
    lines = [f"QEMU {results['accel'].upper()}, -smp {results['smp']}, -m {results['mem']}, "
             f"median of {results['runs']} runs ({results['date']}, {results['host_cpus']} host CPUs).", "",
             "| Workload | What it does | " + " | ".join(f"{o} (s)" for o in oses) + (" | BastiOS / Linux |" if len(oses) == 2 else " |"),
             "|---|---|" + "---:|" * len(oses) + ("---:|" if len(oses) == 2 else "")]
    for phase, desc in results["phases"].items():
        cells, vals = [], []
        for o in oses:
            r = results["os"][o].get(phase)
            if r is None:
                break
            cells.append(f"{r['seconds']:.2f}" if "seconds" in r else "error")
            vals.append(r.get("seconds"))
        if len(cells) != len(oses):
            continue
        row = f"| {phase} | {desc} | " + " | ".join(cells) + " |"
        if len(oses) == 2:
            row += f" {vals[0] / vals[1]:.2f}x |" if None not in vals else " |"
        lines.append(row)
    return "\n".join(lines)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--table":
        print(table(json.load(open(sys.argv[2]))))
    else:
        main()
