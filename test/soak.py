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

--alpine IMG runs a different workload instead: Alpine's gcc and make build a
C project over and over inside a chroot on the disk image IMG (see
test/compat/alpinedisk.py; make one with `--disk gcc,make,musl-dev,binutils`),
with tar, gzip, a file system walk, and a download of a 16 MiB file from a web
server on this machine, between the builds. A round is one build and its
companions; the same checks apply.

--secs runs rounds until that much time has passed (default 120); --rounds
runs exactly that many. Every round's line is kept in OUT/soak.log (--keep
names the directory, default a temporary one that is removed).
"""
import argparse, base64, hashlib, http.server, os, re, select, shutil, subprocess, sys, tempfile, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LINE = re.compile(rb"soak: round (\d+) ok ops=(\d+) free=(-?\d+) cached=(-?\d+) fds=(-?\d+) procs=(-?\d+)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kernel", default=os.path.join(ROOT, "build/bastios.elf"))
    ap.add_argument("--prog", help="a built msoak (default: user/msoak.c compiled with $MUSLCC)")
    ap.add_argument("--secs", type=int, default=120)
    ap.add_argument("--rounds", type=int, default=0)
    ap.add_argument("--smp", type=int, default=4)
    ap.add_argument("--mem", type=int, default=512)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--mempct", type=int, default=60)
    ap.add_argument("--warmup", type=int, default=3, help="rounds before the leak baseline is taken")
    ap.add_argument("--leak-kb", type=int, default=512)
    ap.add_argument("--stall", type=int, default=300, help="seconds without a finished round that count as a hang")
    ap.add_argument("--env", action="append", default=[], help="NAME=VALUE for msoak (e.g. SOAK_IPC=m)")
    ap.add_argument("--par", type=int, default=1, help="msoak instances at once (they stop on their own after --secs)")
    ap.add_argument("--alpine", help="run the Alpine build workload on this disk image instead of msoak")
    ap.add_argument("--only", help="comma-separated steps of msoak to run (default: all)")
    ap.add_argument("--keep", help="directory for the log and disk image")
    a = ap.parse_args()

    tmp = tempfile.TemporaryDirectory()
    work = a.keep or tmp.name
    os.makedirs(work, exist_ok=True)
    disk = os.path.join(work, "soak-disk.img")
    if a.alpine:
        shutil.copyfile(a.alpine, disk)
        if a.mem < 1024:
            a.mem = 1024
    else:
        prog = a.prog
        if not prog:
            prog = os.path.join(work, "msoak.elf")
            subprocess.check_call([os.environ.get("MUSLCC", "musl-gcc"), "-static-pie", "-O2", "-o", prog,
                                   os.path.join(ROOT, "user/msoak.c")])
        subprocess.check_call([os.path.join(HERE, "compat/mkdisk.py"), disk, "msoak=" + prog], stdout=subprocess.DEVNULL)

    blob = os.urandom(16 << 20)
    blob_md5 = hashlib.md5(blob).hexdigest()
    port = 0
    if a.alpine:
        class H(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                self.send_response(200)
                self.send_header("Content-Length", str(len(blob)))
                self.end_headers()
                try:
                    self.wfile.write(blob)
                except OSError:
                    pass
            def log_message(self, *x):
                pass
        srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
        port = srv.server_address[1]
        threading.Thread(target=srv.serve_forever, daemon=True).start()

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
    if a.alpine:
        bb = "/disk/busybox"
        send(f"mkdir -p /disk/a && cd /disk/a && {bb} tar -xf /disk/root.tar && for d in null:1:3 zero:1:5 random:1:8 urandom:1:9 tty:5:0 console:5:1; do set -- $(echo $d | {bb} tr : ' '); {bb} mknod dev/$1 c $2 $3; done; echo /repo > etc/apk/repositories; cd /; echo setup-tar-done\r".encode())
        if not read_until(b"setup-tar-done", mark, 300):
            fails.append("unpacking the Alpine root")
            finish()
        mark = len(out)
        send(f"{bb} chroot /disk/a /sbin/apk add --allow-untrusted -q --no-progress gcc musl-dev make binutils; echo apk-status=$?\r".encode())
        if not read_until(b"apk-status=0", mark, 600):
            fails.append("apk add gcc")
            finish()
        script = open(os.path.join(HERE, "soak/alpine_round.sh"), "rb").read()
        enc = base64.b64encode(script).decode()
        mark = len(out)
        send(b": > /tmp/asoak.b64\r")
        for i in range(0, len(enc), 800):
            send(f"printf '%s' '{enc[i:i + 800]}' >> /tmp/asoak.b64\r".encode())
        send(f"{bb} base64 -d /tmp/asoak.b64 > /tmp/asoak.sh; echo script-md5=$({bb} md5sum < /tmp/asoak.sh)\r".encode())
        if not read_until(f"script-md5={hashlib.md5(script).hexdigest()}".encode(), mark, 120):
            fails.append("sending the workload script")
            finish()
        mark = len(out)
        send(f"{bb} sh /tmp/asoak.sh {big} 10.0.2.2:{port} {blob_md5}\r".encode())
    else:
        pre = (f"SOAK_ONLY={a.only} " if a.only else "") + "".join(e + " " for e in a.env)
        if a.par > 1:
            cmd = "".join(f"{pre}/disk/msoak {big} {a.seed + i} {a.mempct} {a.secs} & " for i in range(a.par - 1))
            cmd += f"{pre}/disk/msoak {big} {a.seed + a.par - 1} {a.mempct} {a.secs}"
            send((cmd + "\r").encode())
        else:
            send((f"{pre}/disk/msoak {big} {a.seed} {a.mempct}\r").encode())
    t0 = time.time()
    deadline = t0 + a.secs
    last_progress = t0
    seen = 0
    rows = []
    pos = 0
    stopped = False
    while True:
        alive = pump(1.0)
        text = bytes(out[mark:])
        for m in LINE.finditer(text, pos):
            rows.append(tuple(int(x) for x in m.groups()))
            pos = m.end()
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
        if a.par > 1 and text.count(b"soak: done") >= a.par:
            stopped = True
            break
        if not a.rounds and a.par == 1 and time.time() > deadline:
            stopped = True
            break
        if a.par > 1 and time.time() > deadline + 600:
            fails.append("the instances did not end after their time")
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
        if not a.rounds and a.par == 1:
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
            if a.par == 1 and lo_base - lo_tail > a.leak_kb:   # with --par, others' pressure moves free memory
                fails.append(f"free memory fell by {lo_base - lo_tail} kB over {len(rows) - a.warmup} rounds")
            fds = {r[4] for r in rows}
            procs = {r[5] for r in rows}
            if a.par == 1 and len(fds) > 1:      # instances see one another's descriptors and processes
                fails.append(f"descriptor count changed: {sorted(fds)}")
            if a.par == 1 and len(procs) > 1:
                fails.append(f"process count changed: {sorted(procs)}")
        elif not a.rounds:
            fails.append(f"only {len(rows)} rounds in {a.secs} s: too few to judge")
        if not fails:
            # Leave ash for the kernel's shell. A line typed while ash
            # resets the terminal after Ctrl-C can be lost, so `exit` is
            # typed again until the kernel's prompt shows.
            for _ in range(3):
                mark = len(out)
                send(b"exit\r")
                if read_until(b"bastios> ", mark, 20):
                    break
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
