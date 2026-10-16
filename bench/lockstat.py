#!/usr/bin/env python3
"""Lock contention while a command runs: reads the per-lock counters of a
kernel built with -DBST_LOCKSTAT (see the lock prelude in bastiolang) through
QEMU's gdb stub and prints, per lock, how often it was taken, how often the
taker had to wait, and the share of the run spent waiting and holding.

    make build/bastios.elf CFLAGS="<the usual flags> -DBST_LOCKSTAT"
    bench/lockstat.py --kernel build/bastios.elf --smp 4 -- /disk/scale syscall 4 400

Needs gdb. The program is built from --prog (default bench/scale.c).
"""
import argparse, os, re, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "bench"))
import run as bench  # noqa: E402

GDB = r'''
import gdb
gdb.execute("set pagination off")
gdb.execute("target remote :%(port)d")
def snap():
    inf = gdb.selected_inferior()
    base = int(gdb.parse_and_eval("(unsigned long)&bst_lockstat"))
    m = bytes(inf.read_memory(base, 8 * 48))
    return [int.from_bytes(m[i * 48 + j * 8:i * 48 + j * 8 + 8], "little") for i in range(8) for j in range(6)]
open(%(out)r, "w").write(" ".join(map(str, snap())))
gdb.execute("detach")
gdb.execute("quit")
'''


def read(port, out):
    with tempfile.NamedTemporaryFile("w", suffix=".py", delete=False) as f:
        f.write(GDB % {"port": port, "out": out})
    subprocess.run(["gdb", "--batch", "-nx", "-x", f.name, os.path.join(ROOT, "build", "bastios64.elf")],
                   capture_output=True, check=False)
    os.unlink(f.name)
    if not os.path.exists(out):
        raise SystemExit("lockstat: gdb read nothing")
    v = list(map(int, open(out).read().split()))
    os.unlink(out)
    return [v[i * 6:i * 6 + 6] for i in range(8)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kernel", default=os.path.join(ROOT, "build", "bastios.elf"))
    ap.add_argument("--smp", type=int, default=4)
    ap.add_argument("--mem", default="512M")
    ap.add_argument("--prog", default=os.path.join(ROOT, "bench", "scale.c"))
    ap.add_argument("cmd", nargs="+")
    a = ap.parse_args()
    exe = os.path.join(bench.B, os.path.splitext(os.path.basename(a.prog))[0])
    os.makedirs(bench.B, exist_ok=True)
    bench.sh(os.environ.get("MUSLCC", "musl-gcc"), "-static-pie", "-O2", "-o", exe, a.prog)
    ns = argparse.Namespace(accel="tcg", cpu="", mem=a.mem, smp=a.smp, kernel=a.kernel)
    line = " ".join(a.cmd)
    port = 20000 + os.getpid() % 20000
    with tempfile.TemporaryDirectory() as tmp:
        bench.EXTRA_QEMU = ["-gdb", f"tcp::{port}"]
        g = bench.boot("bastios", ns, exe, tmp)
        out = os.path.join(tmp, "ls.txt")
        before = read(port, out)
        t0 = time.time()
        at, text = g.run(line)
        wall = time.time() - t0
        after = read(port, out)
        g.stop()
    print(f"`{line}` on -smp {a.smp}: {wall:.2f} s wall")
    print("lock word        taken  contended  wait/hold(ticks)  avg hold  avg wait(contended)")
    for b, e in zip(before, after):
        if e[1] == b[1]:
            continue
        acq, con, wait, hold = e[1] - b[1], e[2] - b[2], e[3] - b[3], e[4] - b[4]
        print(f"{e[0]:#012x} {acq:8d} {con:9d} ({100 * con / acq:3.0f}%)  wait {wait:>12d}  hold {hold:>12d}  "
              f"{hold / acq:9.0f}  {wait / max(con, 1):9.0f}")


main()
