#!/usr/bin/env python3
"""Where the time goes while a command runs on BastiOS: samples the QEMU
process with Linux perf and maps each sample to a guest function.

    bench/prof.py [--kernel build/bastios.elf] [--smp 1] [--top 40]
                  [--perf PATH] -- COMMAND

Boots the kernel the way bench/run.py does (osbench is /disk/osbench),
starts ash, runs COMMAND once untimed so lazily built state is warm, then
again under `perf record`. QEMU runs with -perfmap, which names each block
of translated code by the guest address it came from, so a sample in
translated code is charged to the function of build/bastios64.elf holding
that address ("[user]" outside the kernel). A sample in QEMU itself (TLB
refills, helpers, device emulation) is shown as "qemu:NAME": that is where
emulation, not the guest's own instructions, costs time, and under TCG a
TLB flush (every CR3 load) shows up there.

Needs perf (linux-tools) and perf_event_paranoid <= 2.
"""
import argparse, bisect, glob, os, re, subprocess, sys, tempfile, time
from collections import Counter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "bench"))
import run as bench  # noqa: E402


def symbols(elf):
    out = subprocess.run(["nm", "-n", "--defined-only", elf], capture_output=True, text=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[1] in "tTwW":
            addrs.append(int(p[0], 16))
            names.append(p[2])
    return addrs, names


def find_perf(path):
    if path:
        return path
    for p in ["perf"] + sorted(glob.glob("/usr/lib/linux-tools/*/perf"), reverse=True):
        try:
            subprocess.run([p, "--version"], capture_output=True, check=True)
            return p
        except (OSError, subprocess.CalledProcessError):
            pass
    raise SystemExit("prof: no working perf (install linux-tools)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kernel", default=os.path.join(ROOT, "build", "bastios.elf"))
    ap.add_argument("--elf", default=os.path.join(ROOT, "build", "bastios64.elf"))
    ap.add_argument("--smp", type=int, default=1)
    ap.add_argument("--mem", default="512M")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--perf", default="")
    ap.add_argument("cmd", nargs="+")
    args = ap.parse_args()
    perf = find_perf(args.perf)
    addrs, names = symbols(args.elf)
    osbench = bench.build_osbench()
    ns = argparse.Namespace(accel="tcg", cpu="", mem=args.mem, smp=args.smp, kernel=args.kernel)
    line = " ".join(args.cmd)
    with tempfile.TemporaryDirectory() as tmp:
        bench.EXTRA_QEMU = ["-perfmap"]
        g = bench.boot("bastios", ns, osbench, tmp)
        g.run(line)
        data = os.path.join(tmp, "perf.data")
        rec = subprocess.Popen([perf, "record", "-q", "-F", "1999", "-e", "cpu-clock", "-p", str(g.q.pid),
                                "-o", data], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(0.5)
        g.run(line)
        rec.send_signal(2)
        rec.wait()
        g.stop()
        rep = subprocess.run([perf, "script", "-i", data, "-F", "ip,sym"], capture_output=True, text=True).stdout
        pmap = f"/tmp/perf-{g.q.pid}.map"
        if os.path.exists(pmap):
            os.unlink(pmap)
    hits = Counter()
    n = 0
    for l in rep.splitlines():
        p = l.split(None, 1)
        if not p:
            continue
        n += 1
        sym = p[1].strip() if len(p) > 1 else "[unknown]"
        if sym and sym != "[unknown]" and not sym.startswith("guest-"):
            hits["qemu:" + re.sub(r"\+0x.*", "", sym)] += 1
            continue
        m = re.match(r"guest-0x([0-9a-f]+)", sym)
        if not m:
            hits["qemu:[jit]"] += 1
            continue
        pc = int(m.group(1), 16)
        if addrs and addrs[0] <= pc < addrs[-1] + 0x10000:
            k = bisect.bisect_right(addrs, pc) - 1
            hits[names[k]] += 1
        else:
            hits["[user]"] += 1
    print(f"{n} samples of `{line}`")
    for name, c in hits.most_common(args.top):
        print(f"{100 * c / max(n, 1):6.2f}%  {name}")


if __name__ == "__main__":
    main()
