#!/usr/bin/env bash
# How fast TCP is, and how it behaves on a bad network: bulk transfers
# between a guest and this script over test/netwire.py's wire, which can
# add delay, random loss, reordering and a bottleneck link with a drop-tail
# queue (QEMU's own user-mode network has none of these: it never loses a
# packet, so the congestion and recovery paths of src/tcp.bastio never run
# under it).
#
#   up     the guest sends (`mtcp send`, a Linux program through the system
#          calls) and this script's peer sinks it;
#   down   the peer sends and the guest reads (`mtcp sink`).
#
# Each row: the scenario, the goodput, the time, and how many segments the
# guest sent again (up) or how many the path dropped. Not a pass/fail test
# except for the data arriving intact; like test/netbench.sh it is only
# comparable between runs on the same host.
#
#   NIC=name     the QEMU card (default: virtio-net-pci)
#   ONLY=text    run only scenarios whose name contains text
#   BYTES=n      the size of a transfer (default 4 MiB; lossy ones use a quarter)
set -u
cd "$(dirname "$0")/.."
KERNEL=${KERNEL:-build/bastios.elf}
NIC=${NIC:-virtio-net-pci}
ONLY=${ONLY:-}
BYTES=${BYTES:-4194304}

python3 - "$KERNEL" "$NIC" "$ONLY" "$BYTES" <<'PY'
import os, random, select, socket, struct, subprocess, sys, time

kernel, card, only, nbytes = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
sys.path.insert(0, "test")
from netwire import *

g = Guest(kernel, card)
link, net = g.link, g.net
fails = 0

def until(cond, secs):
    return g.until(cond, secs)

def type_line(line):
    g.type_line(line)

def pat(n):
    return bytes(((i * 7 + 3) & 255) for i in range(n))

class Listener(Peer):
    """A passive peer that learns the guest's port from its SYN."""
    def take(self, s):
        if self.rport == 0:
            self.rport = s["sport"]
        super().take(s)

g.boot()

port_next = [5000]
def next_port():
    port_next[0] += 1
    return port_next[0]

def tune(p):
    p.ws, p.sack_ok, p.rwin, p.mss = 6, True, 1 << 20, 1460

def run(name, direction, size, fwd, rev, secs=240):
    """One transfer. `fwd` is the path from the peer to the guest and `rev`
    the path from the guest back."""
    global fails
    if only and only not in f"{name} {direction}":
        return
    port = next_port()
    link.out_rule, link.in_rule = fwd, rev
    link.seen = []
    mark = len(g.out)
    data = pat(size)
    if direction == "up":
        p = Listener(net, port, 0, b"", active=False)
        tune(p)
        p.close_when = lambda q: q.peer_fin
        net.peer = p
        trace = bool(os.environ.get("TRACE"))
        type_line(f"mtcp send 10.0.2.2 {port} {size}" + (" &" if trace else ""))
        t0 = time.time()
        done_at = [None]
        last_poll = [time.time()]
        def fin():
            if done_at[0] is None and len(p.got) >= size:
                done_at[0] = time.time()
            if trace and time.time() - last_poll[0] > 0.4:
                last_poll[0] = time.time()
                type_line("cat /proc/net/tcp")
            return p.done() or p.state == "RESET"
        until(fin, secs)
        ok = bytes(p.got) == data
        dt = (done_at[0] or time.time()) - t0
        seqs = [s["seq"] for _, s in link.seen if s["data"]]
        extra = len(seqs) - len(set(seqs))
        note = f"{extra} segments sent again"
        if os.environ.get("TRACE"):
            ts = [t for t, sg in link.seen if sg["data"]]
            acks = [sg["ack"] for _, sg in p.log] if False else []
            order = [sg["seq"] for _, sg in link.seen if sg["data"]]
            back = sum(1 for a, b in zip(order, order[1:]) if (b - a) % M32 > M32 // 2)
            for line in g.out[mark:].decode(errors="replace").replace("\r", "").split("\n"):
                if f":{port:04X} " in line or f":{port:04x} " in line:
                    print("    ", line.strip())
            print(f"     guest segments {len(order)}, backwards jumps {back}, peer dup acks {p.dup_acks}, duplicate data {p.dup_data}")
            print("     segments per 100 ms:", [sum(1 for t in ts if int((t - ts[0]) * 10) == b) for b in range(int((ts[-1] - ts[0]) * 10) + 1)] if ts else [])
    else:
        type_line(f"mtcp sink {port}")
        until(lambda: b"listening on tcp port %d" % port in g.out[mark:], 20)
        p = Peer(net, 41000 + port, port, data, active=True)
        tune(p)
        p.cc = True
        p.close_when = lambda q: len(q.got) >= 0 and q.una >= len(data)
        net.peer = p
        p.emit(SYN, p.iss)
        p.timer = time.time() + p.rto
        t0 = time.time()
        until(lambda: b"sunk" in g.out[mark:], secs)
        ok = f"sunk {size} bytes, 0 wrong".encode() in g.out[mark:]
        dt = time.time() - t0
        note = ""
    until(lambda: b"# " in g.out[mark:], 30)
    dropped = getattr(fwd, "drops", 0) + getattr(rev, "drops", 0)
    rate = size / 1024 / dt if ok and dt > 0 else 0
    print(f"{'ok  ' if ok else 'FAIL'} {name:34} {direction:5} {rate:8.0f} KiB/s {dt:7.2f} s  "
          f"{note}{' ' if note else ''}(path dropped {dropped})", flush=True)
    fails += not ok
    net.peer = None
    link.out_rule = link.in_rule = lambda f: 0
    until(lambda: False, 0.3)

q = max(nbytes // 4, 65536)
D = 0.01       # one-way delay of the long paths: 20 ms round trip
# name, direction, size, forward path, reverse path
run("lan", "up", nbytes, Path(), Path())
run("lan", "down", nbytes, Path(), Path())
run("rtt 20ms", "up", nbytes, Path(delay=D), Path(delay=D))
run("rtt 20ms", "down", nbytes, Path(delay=D), Path(delay=D))
run("rtt 20ms, 10 Mbit bottleneck", "up", nbytes, Path(delay=D), Path(delay=D, rate=1250000, queue=30000))
run("lan, 0.5% loss", "up", q, Path(seed=2), Path(loss=0.005, seed=3))
run("lan, 1% loss", "up", q, Path(seed=2), Path(loss=0.01, seed=3))
run("rtt 20ms, 1% loss", "up", q, Path(delay=D, seed=2), Path(delay=D, loss=0.01, seed=3))
run("rtt 20ms, 1% loss", "down", q, Path(delay=D, loss=0.01, seed=2), Path(delay=D, seed=3))
run("rtt 20ms, 5% loss", "up", q // 2, Path(delay=D, seed=2), Path(delay=D, loss=0.05, seed=3))
run("rtt 20ms, 10% reordered 3ms", "up", q, Path(delay=D, seed=2), Path(delay=D, reorder=0.1, seed=3))
run("rtt 20ms, 10% reordered 3ms", "down", q, Path(delay=D, reorder=0.1, seed=2), Path(delay=D, seed=3))
run("rtt 20ms, 10% reordered 30ms", "up", q, Path(delay=D, seed=2), Path(delay=D, reorder=0.1, reorder_by=0.03, seed=3))
run("rtt 20ms, 10% reordered 30ms", "down", q, Path(delay=D, reorder=0.1, reorder_by=0.03, seed=2), Path(delay=D, seed=3))
run("rtt 100ms, 1% loss", "up", q // 2, Path(delay=0.05, seed=2), Path(delay=0.05, loss=0.01, seed=3), secs=400)

g.kill()
print(f"---- {fails} failed ----")
sys.exit(1 if fails else 0)
PY
