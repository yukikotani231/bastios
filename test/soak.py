#!/usr/bin/env python3
"""Long-run stability: run user/msoak's mixed workload for a long time and
look for leaks, hangs and panics.

    test/soak.py [--secs N | --rounds N] [--smp N] [--mem MB] [--seed N]
                 [--kernel build/bastios.elf] [--keep DIR]

Boots BastiOS on --smp processors with a virtio disk, a virtio network card
and a disk holding /disk/msoak, and runs `msoak` in ash. msoak does rounds of
processes, memory pressure, files, pipes, threads, sockets and polling, and
prints the machine's free memory, its own descriptor count and the process
count after every round. The run fails when

  * the kernel faults or runs out of memory, or msoak reports a failed check;
  * no round finishes for --stall seconds (a hang);
  * after the first rounds (which fill caches), free memory keeps falling:
    the lowest free memory of the last rounds is more than --leak-kb below
    the lowest of the rounds just after the warm-up;
  * the descriptor or process count of a round differs from the first one's;
  * the pages free at power off differ from the pages free at boot.

--secs runs rounds until that much time has passed (default 120); --rounds
runs exactly that many. Every round's line is kept in OUT/soak.log (--keep
names the directory, default a temporary one that is removed).
"""
import argparse, os, re, select, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LINE = re.compile(rb"soak: round (\d+) ok ops=(\d+) free=(-?\d+) cached=(-?\d+) fds=(-?\d+) procs=(-?\d+)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kernel", default=os.path.join(ROOT, "build/bastios.elf"))
    ap.add_argument("--prog", default=os.path.join(ROOT, "build/user/msoak.elf"))
    ap.add_argument("--secs", type=int, default=120)
    ap.add_argument("--rounds", type=int, default=0)
    ap.add_argument("--smp", type=int, default=4)
    ap.add_argument("--mem", type=int, default=512)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--mempct", type=int, default=60)
    ap.add_argument("--warmup", type=int, default=3, help="rounds before the leak baseline is taken")
    ap.add_argument("--leak-kb", type=int, default=512)
    ap.add_argument("--stall", type=int, default=300, help="seconds without a finished round that count as a hang")
    ap.add_argument("--only", help="comma-separated steps of msoak to run (default: all)")
    ap.add_argument("--keep", help="directory for the log and disk image")
    a = ap.parse_args()

    tmp = tempfile.TemporaryDirectory()
    work = a.keep or tmp.name
    os.makedirs(work, exist_ok=True)
    disk = os.path.join(work, "soak-disk.img")
    subprocess.check_call([os.path.join(HERE, "compat/mkdisk.py"), disk, "msoak=" + a.prog], stdout=subprocess.DEVNULL)
    scratch = os.path.join(work, "soak-scratch.img")
    with open(scratch, "wb") as f:
        f.truncate(33 << 20)

    q = subprocess.Popen(
        ["qemu-system-x86_64", "-kernel", a.kernel, "-m", f"{a.mem}M", "-smp", str(a.smp),
         "-display", "none", "-no-reboot", "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
         "-serial", "stdio", "-monitor", "none",
         "-drive", f"file={disk},format=raw,if=none,id=vd", "-device", "virtio-blk-pci,drive=vd",
         "-netdev", "user,id=n0", "-device", "virtio-net-pci,netdev=n0"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = bytearray()
    log = open(os.path.join(work, "soak.log"), "wb")

    def pump(secs):
        r, _, _ = select.select([q.stdout], [], [], secs)
        if not r:
            return True
        chunk = q.stdout.read1(65536)
        if not chunk:
            return False
        out.extend(chunk)
        log.write(chunk)
        log.flush()
        return True

    def read_until(marker, start, secs):
        end = time.time() + secs
        while marker not in out[start:]:
            left = end - time.time()
            if left <= 0 or not pump(min(left, 1.0)):
                if left <= 0 or q.poll() is not None:
                    return False
        return True

    def send(b):
        q.stdin.write(b)
        q.stdin.flush()

    fails = []

    def finish():
        try:
            q.wait(timeout=10)
        except subprocess.TimeoutExpired:
            q.kill()
            q.wait()
        if fails:
            print("---- transcript (last 4000 bytes) ----")
            print(bytes(out[-4000:]).decode(errors="replace").replace("\r", ""))
        print(f"---- {len(fails)} failed ----")
        for f in fails:
            print("FAIL", f)
        sys.exit(1 if fails else 0)

    if not read_until(b"bastios> ", 0, 90):
        fails.append("boot")
        finish()
    m = re.search(rb"(\d+) pages of memory", out)
    boot_pages = int(m.group(1)) if m else None
    mark = len(out)
    send(b"ash\r")
    if not read_until(b"\n# ", mark, 30):
        fails.append("ash")
        finish()
    mark = len(out)
    send(b"mkdir -p /tmp /dev; : > /dev/null\r")
    read_until(b"\n# ", mark, 30)

    # one long run, ended by the deadline: msoak is given a round count far
    # beyond what the deadline allows and interrupted with Ctrl-C
    big = a.rounds or 1000000
    mark = len(out)
    send(((f"SOAK_ONLY={a.only} " if a.only else "") + (f"SOAK_NORMDIR=1 " if os.environ.get("SOAK_NORMDIR") else "") + f"/disk/msoak {big} {a.seed} {a.mempct}\r").encode())
    t0 = time.time()
    deadline = t0 + a.secs
    last_progress = t0
    seen = 0
    rows = []
    stopped = False
    while True:
        alive = pump(1.0)
        text = bytes(out[mark:])
        for m in LINE.finditer(text):
            if len(rows) < int(m.group(1)):
                rows.append(tuple(int(x) for x in m.groups()))
                last_progress = time.time()
        if b"/disk/msoak: not found" in text or b"ash: /disk/msoak:" in text:
            fails.append("msoak did not start: " + re.search(rb"ash: /disk/msoak:[^\r\n]*", text).group(0).decode())
            break
        if b"soak: FAIL" in text:
            fails.append("msoak: " + re.search(rb"soak: FAIL[^\r\n]*", text).group(0).decode())
            break
        if b"kernel fault" in text or b"bastio: out of memory" in text or b"panic" in text.lower():
            fails.append("the kernel stopped: " + bytes(text[-300:]).decode(errors="replace"))
            break
        if not alive or q.poll() is not None:
            fails.append("QEMU ended")
            break
        if a.rounds and len(rows) >= a.rounds:
            stopped = True
            break
        if not a.rounds and time.time() > deadline:
            stopped = True
            break
        if time.time() - last_progress > a.stall:
            fails.append(f"hang: no round finished for {a.stall} s (after round {len(rows)})")
            break
    print(f"{len(rows)} rounds in {int(time.time() - t0)} s")
    if rows:
        print("round   ops      free_kb  cached_kb  fds  procs")
        step = max(1, len(rows) // 25)
        for r in rows[::step] + ([rows[-1]] if (len(rows) - 1) % step else []):
            print(f"{r[0]:5d} {r[1]:6d} {r[2]:12d} {r[3]:9d} {r[4]:5d} {r[5]:5d}")

    if stopped and not fails:
        if not a.rounds:
            send(b"\x03")
        if not read_until(b"\n# ", len(out) - 1 if a.rounds else mark, 60):
            # the last round may still be running; give it a minute
            send(b"\x03")
            if not read_until(b"\n# ", mark, 60):
                fails.append("the shell did not come back after the run")
        # leak check over the rounds
        if len(rows) >= a.warmup + 6:
            base = rows[a.warmup:a.warmup + 3]
            tail = rows[-3:]
            lo_base = min(r[2] for r in base)
            lo_tail = min(r[2] for r in tail)
            print(f"free memory: {lo_base} kB after the warm-up, {lo_tail} kB at the end")
            if lo_base - lo_tail > a.leak_kb:
                fails.append(f"free memory fell by {lo_base - lo_tail} kB over {len(rows) - a.warmup} rounds")
            fds = {r[4] for r in rows}
            procs = {r[5] for r in rows}
            if len(fds) > 1:
                fails.append(f"descriptor count changed: {sorted(fds)}")
            if len(procs) > 1:
                fails.append(f"process count changed: {sorted(procs)}")
        elif not a.rounds:
            fails.append(f"only {len(rows)} rounds in {a.secs} s: too few to judge")
        if not fails:
            mark = len(out)
            send(b"exit\r")
            read_until(b"bastios> ", mark, 30)
            mark = len(out)
            send(b"halt\r")
            if read_until(b"pages free at power off", mark, 60):
                m = re.search(rb"(\d+) pages free at power off", bytes(out[mark:]))
                if not m:
                    fails.append("power off")
                elif boot_pages is None or int(m.group(1)) != boot_pages:
                    fails.append(f"pages: {boot_pages} at boot, {m.group(1).decode()} at power off")
                else:
                    print(f"ok every page back at power off ({boot_pages})")
            else:
                fails.append("power off")
    finish()


main()
