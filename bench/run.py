#!/usr/bin/env python3
"""Runs bench/osbench.c on BastiOS and on Linux in the same QEMU machine.

    bench/run.py [--os bastios,linux] [--smp N] [--runs N] [--tests a,b]
                 [--kernel build/bastios.elf] [--linux VMLINUZ] [--out FILE]

Both systems boot with the same CPU model, memory, processor count and a
virtio disk, under KVM when /dev/kvm is usable and TCG otherwise. Each test
runs RUNS times in a fresh process started from the shell (BastiOS's
built-in ash; BusyBox ash on Linux). The time of a run is taken on the host,
between the "@begin" and "@end" lines the program prints, so a guest clock's
calibration cannot skew it; the guest's own figure is kept beside it as a
check. The median of the runs is reported, per operation.

File tests run twice: in a RAM file system ("tmp": BastiOS's tmpfs, Linux's
tmpfs) and on the virtio disk ("disk": BastiOS's own disk format, ext4 on
Linux). Their files are small enough to stay cached, so they measure the
file system code, not the emulated disk.

Linux is Ubuntu's generic kernel (VMLINUZ, or one apt-get downloads into
build/bench/), booted from an initramfs holding static BusyBox and osbench.
Results go to OUT (default build/bench/results.json) and a Markdown table to
stdout.
"""
import argparse, glob, json, os, re, select, shutil, statistics, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
B = os.path.join(ROOT, "build", "bench")

# (test, unit the per-op figure is shown in, scale from ns/op, file test?)
TESTS = [
    ("cpu", "ns/iter", 1, False),
    ("memcpy", "us/MiB", 1e-3, False),
    ("syscall", "ns", 1, False),
    ("clock", "ns", 1, False),
    ("open", "us", 1e-3, True),
    ("stat", "us", 1e-3, True),
    ("create", "us", 1e-3, True),
    ("filemap", "ns/page", 1, True),
    ("filewrite", "MiB/s", None, True),
    ("fileread", "MiB/s", None, True),
    ("fork", "us", 1e-3, False),
    ("forkbig", "us", 1e-3, False),
    ("exec", "us", 1e-3, False),
    ("spawn", "us", 1e-3, False),
    ("fault", "ns/page", 1, False),
    ("pipelat", "us", 1e-3, False),
    ("pipebw", "MiB/s", None, False),
    ("tcplat", "us", 1e-3, False),
    ("tcpbw", "MiB/s", None, False),
    ("sleep", "us", 1e-3, False),
    ("epwait", "us", 1e-3, False),
    ("idle", "% host CPU", 1, False),
]
EXTRA_QEMU = []                     # bench/prof.py adds its gdb stub here
ICOUNT = False                      # --icount: time by instructions executed
DIRS = {"bastios": {"tmp": "/tmp", "disk": "/disk"}, "linux": {"tmp": "/tmp", "disk": "/disk"}}


def accel():
    try:
        fd = os.open("/dev/kvm", os.O_RDWR)
        os.close(fd)
        return "kvm"
    except OSError:
        return "tcg"


def sh(*cmd, **kw):
    subprocess.run(cmd, check=True, **kw)


def build_osbench():
    os.makedirs(B, exist_ok=True)
    out = os.path.join(B, "osbench")
    src = os.path.join(ROOT, "bench", "osbench.c")
    if not os.path.exists(out) or os.path.getmtime(out) < os.path.getmtime(src):
        sh(os.environ.get("MUSLCC", "musl-gcc"), "-static-pie", "-O2", "-o", out, src)
    return out


def linux_kernel(path):
    if path:
        return path
    have = sorted(glob.glob(os.path.join(B, "linux", "boot", "vmlinuz-*")))
    if have:
        return have[-1]
    # Ubuntu's generic kernel has virtio-pci, virtio-blk, virtio-net and
    # ext4 built in, so it boots an initramfs with no modules.
    dep = subprocess.run(["apt-cache", "depends", "linux-image-virtual"], capture_output=True, text=True).stdout
    pkg = re.search(r"linux-image-\d\S*", dep).group(0)
    d = os.path.join(B, "linux")
    os.makedirs(d, exist_ok=True)
    sh("apt-get", "download", pkg, cwd=d)
    deb = glob.glob(os.path.join(d, pkg + "_*.deb"))[0]
    sh("dpkg-deb", "-x", deb, d)
    return sorted(glob.glob(os.path.join(d, "boot", "vmlinuz-*")))[-1]


LINUX_INIT = """#!/bin/busybox sh
/bin/busybox --install -s /bin
mount -t proc proc /proc
mount -t sysfs sys /sys
mount -t devtmpfs dev /dev
mount -t tmpfs tmp /tmp
mkdir -p /disk
mount -t ext4 /dev/vda /disk
ip link set lo up
echo READY
exec setsid cttyhack sh
"""


def linux_initrd(osbench, tmp):
    root = os.path.join(tmp, "root")
    shutil.rmtree(root, ignore_errors=True)         # a second boot in the same directory
    for d in ("bin", "proc", "sys", "dev", "tmp", "disk"):
        os.makedirs(os.path.join(root, d))
    bb = shutil.which("busybox") or "/bin/busybox"
    shutil.copy(bb, os.path.join(root, "bin", "busybox"))
    shutil.copy(osbench, os.path.join(root, "osbench"))
    with open(os.path.join(root, "init"), "w") as f:
        f.write(LINUX_INIT)
    os.chmod(os.path.join(root, "init"), 0o755)
    img = os.path.join(tmp, "initrd.cpio")
    files = subprocess.run("find . | sort", shell=True, cwd=root, capture_output=True).stdout
    with open(img, "wb") as f:
        subprocess.run(["cpio", "-o", "-H", "newc", "--quiet"], cwd=root, input=files, stdout=f, check=True)
    disk = os.path.join(tmp, "disk.img")
    with open(disk, "wb") as f:
        f.truncate(64 << 20)
    sh("mkfs.ext4", "-q", "-F", disk)
    return img, disk


WRAP = []


def strace_boot(args, osbench, tmp):
    """EXPERIMENT: when does QEMU open the kernel, create the vCPU and first run it?"""
    global WRAP
    subprocess.run("sudo apt-get install -y strace >/dev/null 2>&1", shell=True)
    out = os.path.join(tmp, "strace.txt")
    WRAP = ["strace", "-f", "-ttt", "-e", "trace=execve,openat,ioctl,write", "-o", out]
    try:
        a1 = argparse.Namespace(**vars(args))
        a1.smp = 1
        boot("bastios", a1, osbench, tmp).stop()
    finally:
        WRAP = []
    ev, t0 = {}, None
    for line in open(out, errors="replace"):
        f = line.split(None, 2)
        if len(f) < 3:
            continue
        try:
            t = float(f[1])
        except ValueError:
            continue
        t0 = t0 or t
        for key, pat in (("kernel", "bastios.elf"), ("kvm", "/dev/kvm"), ("vcpu", "KVM_CREATE_VCPU"), ("run", "KVM_RUN"), ("banner", "BastiOS 0.2")):
            if pat in line and key not in ev:
                ev[key] = t - t0
    print("strace (s after execve): " + ", ".join(f"{k} {v:.3f}" for k, v in ev.items()), file=sys.stderr, flush=True)


class Guest:
    def __init__(self, cmd, prompt, logpath):
        self.prompt = prompt
        self.log = open(logpath, "wb")
        self.t0 = time.monotonic()
        self.q = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.out = b""
        self.marks = []                     # (offset in out, host time)

    def read(self, timeout):
        r, _, _ = select.select([self.q.stdout], [], [], timeout)
        if not r:
            return False
        chunk = self.q.stdout.read1(65536)
        if not chunk:
            raise SystemExit("bench: QEMU exited:\n" + self.out[-2000:].decode(errors="replace"))
        t = time.monotonic()
        self.log.write(chunk)
        self.log.flush()
        self.marks.append((len(self.out), t))
        self.out += chunk
        return True

    def until(self, pat, start, timeout):
        end = time.monotonic() + timeout
        while True:
            m = re.compile(pat).search(self.out, start)
            if m:
                return m
            left = end - time.monotonic()
            if left <= 0 or (not self.read(left) and time.monotonic() >= end):
                return None

    def time_at(self, off):
        """Host time the byte at offset off arrived."""
        t = None
        for o, tt in self.marks:
            if o > off:
                break
            t = tt
        return t

    def send(self, line):
        self.q.stdin.write(line.encode() + b"\r")
        self.q.stdin.flush()

    def run(self, line, timeout=600):
        at = len(self.out)
        self.send(line)
        m = self.until(rb"@done|@skip [^\n]*\n|osbench: [^\n]*\n", at, timeout)
        if not m:
            return None, None
        self.until(re.escape(self.prompt.encode()), m.end(), 30)
        return at, self.out[at:]

    def stop(self):
        self.q.kill()
        self.q.wait()
        self.log.close()


def boot(osname, args, osbench, tmp):
    common = ["-cpu", args.cpu or ("qemu64,+pdpe1gb" if args.accel == "tcg" else "host"), "-accel", args.accel,
              "-m", args.mem, "-smp", str(args.smp), "-display", "none", "-no-reboot",
              "-serial", "stdio", "-monitor", "none",
              # a card on no network: BastiOS has loopback only with one
              "-netdev", "user,id=n0,restrict=on", "-device", "virtio-net-pci,netdev=n0"]
    common += EXTRA_QEMU
    if ICOUNT:
        common += ["-icount", "shift=0"]
    log = os.path.join(B, f"{osname}.log")
    if osname == "bastios":
        disk = os.path.join(tmp, "bastios-disk.img")
        sh(os.path.join(ROOT, "test", "compat", "mkdisk.py"), disk, osbench, stdout=subprocess.DEVNULL)
        cmd = ["qemu-system-x86_64", "-kernel", args.kernel,
               "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
               "-drive", f"file={disk},format=raw,if=none,id=vd", "-device", "virtio-blk-pci,drive=vd"] + common
        if getattr(args, "append", ""):
            cmd += ["-append", args.append]
        g = Guest(WRAP + cmd, "# ", log)
        m = g.until(rb"bastios> ", 0, 300)
        if not m:
            raise SystemExit("bench: BastiOS gave no prompt")
        g.boot_ns = (g.time_at(m.start()) - g.t0) * 1e9
        for _m in re.finditer(rb"exp-(?:tsc|stages): [^\r\n]*", g.out):
            print(_m.group(0).decode(), file=sys.stderr, flush=True)
        print("boot stages (s after starting QEMU): " + ", ".join(
            f"{name} {g.time_at(g.out.find(pat)) - g.t0:.3f}"
            for name, pat in (("banner", b"BastiOS 0.2:"), ("acpi", b"acpi:"), ("disk", b"disk:"), ("net", b"net:"), ("prompt", b"bastios> "))
            if g.out.find(pat) >= 0), file=sys.stderr, flush=True)
        at = len(g.out)
        g.send("ash")
        if not g.until(rb"# ", at, 60):
            raise SystemExit("bench: ash did not start")
        g.exe = "/disk/osbench"
    else:
        initrd, disk = linux_initrd(osbench, tmp)
        cmd = ["qemu-system-x86_64", "-kernel", linux_kernel(args.linux), "-initrd", initrd,
               "-append", "console=ttyS0 quiet panic=-1 mitigations=off transparent_hugepage=never rdinit=/init",
               "-drive", f"file={disk},format=raw,if=none,id=vd", "-device", "virtio-blk-pci,drive=vd"] + common
        g = Guest(cmd, "# ", log)
        m = g.until(rb"READY", 0, 600)
        if not m:
            raise SystemExit("bench: Linux did not reach its shell")
        g.boot_ns = (g.time_at(m.start()) - g.t0) * 1e9
        g.until(rb"# ", len(g.out) - 1, 30)
        g.send("stty -echo 2>/dev/null; PS1='# '")
        time.sleep(1)
        g.exe = "/osbench"
    return g


def qemu_cpu(g):
    """Host CPU seconds QEMU has used so far."""
    with open(f"/proc/{g.q.pid}/stat") as f:
        st = f.read().rsplit(")", 1)[1].split()
    return (int(st[11]) + int(st[12])) / os.sysconf("SC_CLK_TCK")


def measure(g, test, d):
    """One run: (host ns per op, guest ns per op, ops) or a skip reason.
    For "idle", "host" is instead the host CPU QEMU used, in percent of
    the time the guest slept, less what starting the program cost."""
    base = 0.0
    if test == "idle":
        # The program's own start (reading it from the disk) costs CPU too,
        # more on some systems than the sleep: a run that sleeps 1 ms is
        # that cost alone, and is taken off.
        c = qemu_cpu(g)
        g.run(f"{g.exe} -d {d} idle=1")
        base = qemu_cpu(g) - c
    cpu0 = qemu_cpu(g)
    at, text = g.run(f"{g.exe} -d {d} {'filewrite fileread' if test == 'fileread' else test}")
    if text is None:
        return {"error": "timeout"}
    s = re.search(rb"@skip (\S+) ([^\r\n]*)", text)
    if s:
        return {"skip": s.group(2).decode()}
    m = None
    for m in re.finditer(rb"@begin " + test.encode() + rb"\r?\n", text):
        pass
    e = re.search(rb"@end " + test.encode() + rb" (\d+) (\d+)", text)
    if not m or not e:
        return {"error": text[-300:].decode(errors="replace")}
    t0 = g.time_at(at + m.end() - 1)
    t1 = g.time_at(at + e.start())
    ops, guest = int(e.group(1)), int(e.group(2))
    host = (t1 - t0) * 1e9
    if test == "idle":
        # Taken over the whole command, less the start-up cost measured above.
        return {"host": 100 * max(qemu_cpu(g) - cpu0 - base, 0) * 1e9 / guest, "guest": guest, "ops": ops}
    # Lines that came in one read have one host time; a run that short is
    # timed by the guest.
    if host < 50e6 or ICOUNT:
        host = guest
    return {"host": host / ops, "guest": guest / ops, "ops": ops}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--os", default="bastios,linux")
    ap.add_argument("--smp", type=int, default=2)
    ap.add_argument("--mem", default="512M")
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--tests", default="")
    ap.add_argument("--kernel", default=os.path.join(ROOT, "build", "bastios.elf"))
    ap.add_argument("--linux", default=os.environ.get("LINUX_KERNEL", ""))
    ap.add_argument("--accel", default=accel())
    ap.add_argument("--cpu", default="")
    ap.add_argument("--append", default="", help="the BastiOS kernel's command line (for example pcid)")
    ap.add_argument("--icount", action="store_true",
                    help="TCG with -icount shift=0: the guest clock counts instructions (one per ns), so a figure does not depend on the host's load")
    ap.add_argument("--out", default=os.path.join(B, "results.json"))
    args = ap.parse_args()
    global ICOUNT
    ICOUNT = args.icount
    if ICOUNT and args.accel != "tcg":
        sys.exit("--icount needs TCG (--accel tcg)")
    want = set(args.tests.split(",")) if args.tests else None
    osbench = build_osbench()
    results = {"accel": args.accel, "smp": args.smp, "mem": args.mem, "runs": args.runs,
               "host_cpus": os.cpu_count(), "date": time.strftime("%Y-%m-%d %H:%M:%S"), "os": {}}
    with tempfile.TemporaryDirectory() as tmp:
        if "bastios" in args.os.split(",") and args.accel == "kvm":
            strace_boot(args, osbench, tmp)
        for osname in args.os.split(","):
            res = results["os"][osname] = {}
            if args.smp > 1:
                # One processor: what the extra ones cost to start (the delays of boot/smp.c)
                a1 = argparse.Namespace(**vars(args))
                a1.smp = 1
                boot(osname, a1, osbench, tmp).stop()     # the first boot of a system reads its files cold
                g1 = boot(osname, a1, osbench, tmp)
                res["boot1"] = {"host": g1.boot_ns, "guest": g1.boot_ns, "runs": [round(g1.boot_ns / 1e6, 1)]}
                g1.stop()
            g = boot(osname, args, osbench, tmp)
            res["boot"] = {"host": g.boot_ns, "guest": g.boot_ns, "runs": [round(g.boot_ns / 1e6, 1)]}
            try:
                for test, unit, scale, isfile in TESTS:
                    if want and test not in want:
                        continue
                    for where in (("tmp", "disk") if isfile else ("tmp",)):
                        key = f"{test}@{where}" if isfile else test
                        runs = [measure(g, test, DIRS[osname][where]) for _ in range(args.runs)]
                        ok = [r for r in runs if "host" in r]
                        if ok:
                            res[key] = {"host": statistics.median(r["host"] for r in ok),
                                        "guest": statistics.median(r["guest"] for r in ok),
                                        "runs": [round(r["host"], 1) for r in ok]}
                        else:
                            res[key] = runs[0]
                        print(f"{osname:8} {key:16} {res[key]}", file=sys.stderr, flush=True)
            finally:
                g.stop()
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w") as f:
        json.dump(results, f, indent=1)
    print(table(results))


def fmt(v, unit, scale):
    if scale is None:                       # ns per KiB -> MiB/s
        return f"{1e9 / v / 1024:.1f}"
    x = v * scale
    return f"{x:.0f}" if x >= 100 else f"{x:.2f}" if x < 10 else f"{x:.1f}"


def table(results):
    oses = list(results["os"])
    lines = [f"QEMU {results['accel'].upper()}, -smp {results['smp']}, -m {results['mem']}, "
             f"median of {results['runs']} runs, {'instruction count (--icount)' if ICOUNT else 'host time'} ({results['date']}, {results['host_cpus']} host CPUs).", "",
             "| Test | Unit | " + " | ".join(oses) + (" | BastiOS / Linux |" if len(oses) == 2 else " |"),
             "|---|---|" + "---:|" * len(oses) + ("---:|" if len(oses) == 2 else "")]
    for test, unit, scale, isfile in [("boot", "ms", 1e-6, False), ("boot1", "ms", 1e-6, False)] + TESTS:
        for where in (("tmp", "disk") if isfile else ("tmp",)):
            key = f"{test}@{where}" if isfile else test
            cells, vals = [], []
            for o in oses:
                r = results["os"][o].get(key)
                if r is None:
                    break
                if "host" in r:
                    cells.append(fmt(r["host"], unit, scale))
                    vals.append(r["host"])
                else:
                    cells.append("skip" if "skip" in r else "error")
                    vals.append(None)
            if len(cells) != len(oses):
                continue
            row = f"| {key} | {unit} | " + " | ".join(cells) + " |"
            if len(oses) == 2:
                # A slowdown factor either way: higher is worse for BastiOS.
                row += f" {vals[0] / vals[1]:.2f}x |" if None not in vals and vals[1] else " |"
            lines.append(row)
    return "\n".join(lines)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--table":
        print(table(json.load(open(sys.argv[2]))))
    else:
        main()
