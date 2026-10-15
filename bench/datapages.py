#!/usr/bin/env python3
"""Which data pages the kernel touches per system call: single-steps the guest
from gdb, decodes each instruction's memory operand and counts the distinct
4 KiB pages (kernel addresses only) touched per return to the program.

    bench/datapages.py [--kernel build/bastios.elf] [--steps 20000] [--top 40]
                       --prog bench/scale.c -- /disk/scale syscall 1 400

Under QEMU's TCG every trap starts with an empty software TLB, so the time
under the kernel lock is mostly one refill per page touched (see
docs/performance.md). Each line is a page, with the data symbol it falls in,
the offset into it and how many of the round trips touched it. Accesses are
decoded from the AT&T disassembly (base, index*scale, displacement; push, pop,
call, ret and string moves through rsp, rsi and rdi), so a handful of
exotic instructions are missed; the page count is a good estimate, not exact.
Needs gdb. Compare two kernels by the "pages per return" line.
"""
import argparse, bisect, json, os, re, subprocess, sys, tempfile, time
from collections import Counter, defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "bench"))
import run as bench  # noqa: E402
from prof import symbols  # noqa: E402

GDB = r'''
import gdb, json, re
gdb.execute("set pagination off")
gdb.execute("set confirm off")
gdb.execute("target remote :%(port)d")
arch = gdb.selected_frame().architecture()
MEM = re.compile(r"(-?0x[0-9a-f]+|-?\d+)?\((%%\w+)?(?:,(%%\w+)(?:,(\d))?)?\)")
ABS = re.compile(r"(?:^|[ ,])(0x[0-9a-f]+|\d+)(?=$|,| )")
names = {}
def reg(n):
    n = n[1:]
    if n == "rip":
        return pc
    v = gdb.parse_and_eval("$" + n)
    return int(v) & 0xFFFFFFFFFFFFFFFF
out = []
lockpc = %(lockpc)d
locks = {}
for i in range(%(steps)d):
    pc = int(gdb.parse_and_eval("$pc")) & 0xFFFFFFFFFFFFFFFF
    ins = arch.disassemble(pc)[0]
    txt = ins["asm"]
    addrs = []
    for m in MEM.finditer(txt):
        d = int(m.group(1), 0) if m.group(1) else 0
        a = d
        if m.group(2):
            a += reg(m.group(2))
        if m.group(3):
            a += reg(m.group(3)) * int(m.group(4) or 1)
        if "lea" in txt.split()[0]:
            continue
        addrs.append(a & 0xFFFFFFFFFFFFFFFF)
    op = txt.split()[0]
    if op.startswith(("push", "call")):
        addrs.append((reg("%%rsp") - 8) & 0xFFFFFFFFFFFFFFFF)
    elif op.startswith(("pop", "ret", "leave")):
        addrs.append(reg("%%rsp"))
    if op.startswith(("movs", "rep", "stos")) and ("movs" in txt or "stos" in txt):
        addrs.append(reg("%%rdi"))
        if "movs" in txt:
            addrs.append(reg("%%rsi"))
    out.append([pc, addrs])
    if pc == lockpc:
        l = reg("%%rdi")
        locks[l] = locks.get(l, 0) + 1
    gdb.execute("stepi", to_string=True)
base = max(locks, key=locks.get) + 8 if locks else 0
mem = bytes(gdb.selected_inferior().read_memory(base, %(syslen)d)).hex() if base else ""
json.dump({"base": base, "mem": mem}, open(%(out)r + ".sys", "w"))
open(%(out)r, "w").write(json.dumps(out))
gdb.execute("detach")
gdb.execute("quit")
'''


def struct_layout(elf):
    """Field offsets of the kernel's Sys and Kernel structs, from the debug
    info of the generated C compiled by itself (gdb's ptype /o)."""
    src = os.path.join(ROOT, "build", "kernel.c")
    obj = os.path.join(ROOT, "build", "kernel_g.o")
    if not os.path.exists(obj) or os.path.getmtime(obj) < os.path.getmtime(src):
        subprocess.run(["cc", "-g", "-O0", "-w", "-c", "-o", obj, src], check=True)
    res = {}
    for st in ("Sys", "Kernel"):
        txt = subprocess.run(["gdb", "-batch", "-nx", "-ex", f"ptype /o struct bst_s_{st}", obj], capture_output=True, text=True).stdout
        fields = []
        for ln in txt.splitlines():
            m = re.match(r"/\*\s+(\d+)\s+\|\s+(\d+)\s*\*/\s+(.+?)\s+(\w+);", ln)
            if m:
                fields.append((int(m.group(1)), int(m.group(2)), m.group(3), m.group(4)))
        tot = int(re.search(r"total size \(bytes\):\s*(\d+)", txt).group(1))
        res[st] = (fields, tot)
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kernel", default=os.path.join(ROOT, "build", "bastios.elf"))
    ap.add_argument("--elf", default=os.path.join(ROOT, "build", "bastios64.elf"))
    ap.add_argument("--smp", type=int, default=1)
    ap.add_argument("--mem", default="512M")
    ap.add_argument("--steps", type=int, default=20000)
    ap.add_argument("--warm", type=float, default=3.0, help="seconds of host time before gdb attaches")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--prog", default="", help="run this C file (built as /disk/<name>) instead of osbench")
    ap.add_argument("cmd", nargs="+")
    args = ap.parse_args()
    layout = struct_layout(args.elf)
    syslen = layout["Sys"][1]
    osbench = bench.build_osbench()
    if args.prog:
        osbench = os.path.join(bench.B, os.path.splitext(os.path.basename(args.prog))[0])
        bench.sh(os.environ.get("MUSLCC", "musl-gcc"), "-static-pie", "-O2", "-o", osbench, args.prog)
    ta, tn = symbols(args.elf)
    lockpc = ta[tn.index("bst_lock_acquire")]
    ns = argparse.Namespace(accel="tcg", cpu="", mem=args.mem, smp=args.smp, kernel=args.kernel)
    line = " ".join(args.cmd)
    port = 20000 + os.getpid() % 20000
    bench.ICOUNT = True
    with tempfile.TemporaryDirectory() as tmp:
        bench.EXTRA_QEMU = ["-gdb", f"tcp::{port}"]
        g = bench.boot("bastios", ns, osbench, tmp)
        g.send(line)
        time.sleep(args.warm)
        out = os.path.join(tmp, "trace.json")
        script = os.path.join(tmp, "step.py")
        with open(script, "w") as f:
            f.write(GDB % {"port": port, "steps": args.steps, "out": out, "lockpc": lockpc, "syslen": syslen})
        r = subprocess.run(["gdb", "--batch", "-nx", "-x", script], capture_output=True, text=True, check=False)
        if os.environ.get("DP_DEBUG"):
            print(r.stdout[-2000:], r.stderr[-2000:])
        g.stop()
        if not os.path.exists(out):
            raise SystemExit("datapages: gdb did not write the trace")
        trace = json.load(open(out))
        sysd = json.load(open(out + ".sys"))
    nm = subprocess.run(["nm", "-n", "-S", "--defined-only", args.elf], capture_output=True, text=True).stdout
    sa, sn = [], []
    for ln in nm.splitlines():
        p = ln.split()
        if len(p) == 4:
            sa.append(int(p[0], 16))
            sn.append(p[3])
        elif len(p) == 3:
            sa.append(int(p[0], 16))
            sn.append(p[2])
    addrs, _ = symbols(args.elf)
    lo, hi = addrs[0], addrs[-1] + 0x10000
    # Round trips: from one return to the program to the next.
    taddrs, tnames = symbols(args.elf)
    keep = [i for i, n in enumerate(tnames) if n != 'mb2_end']
    taddrs, tnames = [taddrs[i] for i in keep], [tnames[i] for i in keep]
    words = defaultdict(set)
    who = defaultdict(Counter)              # page -> accessing functions
    trips, cur, was_user = [], None, True
    for pc, data in trace:
        user = not (lo <= pc < hi) and pc < 0x7F0000000000
        if user and not was_user:
            if cur is not None:
                trips.append(cur)
            cur = set()
        elif not user and cur is not None:
            for a in data:
                if a >= 0x100000 and (a < 0x7F0000000000 or a >= 0xFFFF000000000000):
                    cur.add(a >> 12)
                    words[a >> 12].add(a)
                    if pc >= lo and pc < hi:
                        who[a >> 12][tnames[bisect.bisect_right(taddrs, pc) - 1]] += 1
        was_user = user
    if os.environ.get("DP_DEBUG"):
        print(len(trace), trace[:3], trace[-3:], hex(lo), hex(hi))
    if not trips:
        raise SystemExit("datapages: no whole round trip in the trace")
    # Label pages: the Sys box, and what each of Kernel's buffers points at.
    mem = bytes.fromhex(sysd["mem"]) if sysd["mem"] else b""
    base = sysd["base"]
    ranges = []                             # (lo, hi, name)
    if mem:
        ranges.append((base - 8, base + len(mem), "Sys"))
        koff = [o for o, sz, t, n in layout["Sys"][0] if n == "k"][0]
        for off, size, ty, nm in layout["Kernel"][0]:
            off += koff
            if ty in ("bst_buf", "bst_vec", "bst_bytes") and off + 16 <= len(mem):
                ln = int.from_bytes(mem[off:off + 8], "little")
                ptr = int.from_bytes(mem[off + 8:off + 16], "little")
                if ptr and ln:
                    ranges.append((ptr, ptr + ln * (1 if ty == "bst_bytes" else 8), "k." + nm))
    def label(a, hi):
        out = []
        for lo_, hi_, nm in ranges:
            if lo_ < hi and hi_ > a:
                if nm == "Sys":
                    fs = [f"{n}" for o, sz, t, n in layout["Sys"][0] if base + o < hi and base + o + sz > a]
                    out.append("Sys{" + ",".join(fs[:8]) + "}")
                else:
                    out.append(f"{nm}[{max(a, lo_) - lo_:#x}..]")
        return " ".join(out)
    hits = Counter()
    for t in trips:
        for pg in t:
            hits[pg] += 1
    print(f"{len(trips)} whole round trips of `{line}`")
    print(f"pages per return: {sum(len(t) for t in trips) / len(trips):.1f}")
    for pg, c in hits.most_common(args.top):
        a = pg << 12
        i = bisect.bisect_right(sa, a + 0xFFF) - 1
        where = f"{sn[i]}+{a - sa[i]:#x}" if i >= 0 else "?"
        fns = ",".join(f for f, _ in who[pg].most_common(6))
        if os.environ.get("DP_WORDS"):
            fns += "  @" + " ".join(f"{w:#x}" for w in sorted(words[pg])[:12])
        print(f"{c:4d}/{len(trips)}  {a:#x}  {label(a, a + 4096) or where}  {fns}")


if __name__ == "__main__":
    main()
