#!/usr/bin/env bash
# What the network costs and how fast it is: boots BastiOS on -smp 2 once
# per card (virtio-net, e1000) on QEMU's user-mode network, with host ports
# forwarded to the guest, and measures RUNS times each:
#   native: the shell's `tcpecho PORT`, driven from the host: the average
#           round trip of 200 small messages, and 256 KiB echoed, in KiB/s;
#   linux:  the same against `mtcp echo PORT` (user/mtcp.c, blocking
#           read and write through the Linux system calls), run from ash;
#   idle:   `bench` beside `mtcp &` (a Linux program blocked in accept),
#           against `bench` alone: what a process waiting for the network
#           costs the others. These runs come last: each leaves connections
#           that hold the stack's eight slots for a while.
# Not a pass/fail test, and only comparable between runs on the same host
# (see test/diskbench.sh for why).
set -u
cd "$(dirname "$0")/.."
KERNEL=${KERNEL:-build/bastios.elf}
RUNS=${RUNS:-3}

python3 - "$KERNEL" "$RUNS" <<'PY'
import os, re, select, socket, subprocess, sys, time

kernel, runs = sys.argv[1], int(sys.argv[2])

def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p

def echo_run(port):
    """Round trips and bulk echo against whatever echoes on the port."""
    s = None
    for _ in range(50):
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=30)
            break
        except OSError:
            time.sleep(0.1)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    msg = b"x" * 32
    t0 = time.time()
    for i in range(200):
        s.sendall(msg)
        got = b""
        while len(got) < len(msg):
            got += s.recv(len(msg) - len(got))
    rtt = (time.time() - t0) / 200 * 1000
    size = 256 << 10
    data = os.urandom(size)
    t0 = time.time()
    sent = 0
    back = bytearray()
    s.setblocking(False)
    while len(back) < size and time.time() - t0 < 120:
        if sent < size:
            try:
                sent += s.send(data[sent:sent + 16384])
            except BlockingIOError:
                pass
        r_, _, _ = select.select([s], [], [], 0.01)
        if r_:
            chunk = s.recv(65536)
            if not chunk:
                break
            back += chunk
    dt = time.time() - t0
    s.close()
    ok = "" if bytes(back) == data else " (data differs!)"
    return f"rtt {rtt:.2f} ms, bulk {size / 1024 / dt:.0f} KiB/s{ok}"

GP = r"getpid x\d+: (\d+) ticks"
for card in ("virtio-net-pci", "e1000"):
    port = free_port()
    # Every echo run gets its own guest port, so none waits on the last
    # one's closed connection.
    fwd = {g: free_port() for g in range(7100, 7100 + 2 * runs)}
    hostfwd = "".join(f",hostfwd=tcp:127.0.0.1:{h}-:{g}" for g, h in fwd.items())
    q = subprocess.Popen(
        ["qemu-system-x86_64", "-kernel", kernel, "-m", "256M", "-smp", "2", "-display", "none", "-no-reboot",
         "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04", "-serial", "stdio", "-monitor", "none",
         "-netdev", f"user,id=n0,hostfwd=tcp:127.0.0.1:{port}-:7001{hostfwd}", "-device", f"{card},netdev=n0"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = b""

    def until(pred, timeout=60):
        global out
        end = time.time() + timeout
        while not pred(out):
            left = end - time.time()
            if left <= 0:
                return False
            r, _, _ = select.select([q.stdout], [], [], left)
            if r:
                chunk = q.stdout.read1(65536)
                if not chunk:
                    return False
                out += chunk
        return True

    def typed(line, want, timeout=60):
        mark = len(out)
        q.stdin.write(line.encode() + b"\r"); q.stdin.flush()
        until(lambda o: want in o[mark:], timeout)
        return mark

    if not until(lambda o: b"bastios> " in o):
        print(f"{card}: no prompt"); q.kill(); continue
    time.sleep(3)                     # DHCP; an e1000's link takes a moment
    for r in range(runs):
        g = 7100 + r
        m = typed(f"tcpecho {g}", b"listening on tcp port")
        time.sleep(0.5)
        res = echo_run(fwd[g])
        until(lambda o: b"echoed" in o[m:])
        print(f"{card:15} run {r + 1}: native tcpecho: {res}")
    typed("ash", b"# ")
    for r in range(runs):
        g = 7100 + runs + r
        m = typed(f"mtcp echo {g} &", b"listening on tcp port")
        time.sleep(0.5)
        res = echo_run(fwd[g])
        until(lambda o: b"echoed" in o[m:])
        print(f"{card:15} run {r + 1}: linux mtcp echo: {res}")
    typed("exit", b"bastios> ")
    for r in range(runs):
        m = typed("bench", b"spawn+exit")
        until(lambda o: b"bastios> " in o[m:])
        alone = re.findall(GP, out[m:].decode(errors="replace"))
        typed("mtcp &", b"listening on tcp port 7001")
        m = typed("bench", b"spawn+exit")
        until(lambda o: b"bastios> " in o[m:])
        beside = re.findall(GP, out[m:].decode(errors="replace"))
        s = socket.create_connection(("127.0.0.1", port), timeout=30)
        s.sendall(b"1\n")             # mtcp then gets from host port 1: refused
        s.recv(100); s.close()
        until(lambda o: b"mtcp: connect failed" in o[m:] or b"mtcp: got" in o[m:])
        print(f"{card:15} run {r + 1}: bench getpid alone {alone}, beside a socket waiting in accept {beside}")
    q.kill(); q.wait()
PY
