#!/usr/bin/env bash
# What TCP does about the network, one rule at a time: the guest's initial
# window, fast retransmit, the resend timer, repair of several holes in
# one window, and, as a receiver, delayed ACKs, D-SACK and SACK blocks for
# several gaps. The guest runs `mtcp send` and `mtcp sink` (Linux programs
# through the system calls) against a scripted peer on a wire this script
# controls (test/netwire.py). test/tcp_test.sh covers loss, reordering and
# window handling from the other side; test/tcpperf.sh measures speed.
#
#   NIC=name   the QEMU card (default: virtio-net-pci)
set -u
cd "$(dirname "$0")/.."
KERNEL=${KERNEL:-build/bastios.elf}
NIC=${NIC:-virtio-net-pci}

python3 - "$KERNEL" "$NIC" <<'PY'
import os, sys, time

kernel, card = sys.argv[1], sys.argv[2]
sys.path.insert(0, "test")
from netwire import *

g = Guest(kernel, card)
link, net = g.link, g.net
fails = 0
MSS = 1460

def check(ok, what):
    global fails
    print(("ok  " if ok else "FAIL"), what, flush=True)
    fails += not ok

def pat(n):
    return bytes(((i * 7 + 3) & 255) for i in range(n))

class Listener(Peer):
    """A passive peer that learns the guest's port from its SYN."""
    def take(self, s):
        if self.rport == 0:
            self.rport = s["sport"]
        super().take(s)

ports = [6000]
def next_port():
    ports[0] += 1
    return ports[0]

def tune(p):
    p.ws, p.sack_ok, p.rwin, p.mss = 6, True, 1 << 20, MSS

def ends(p):
    """The offsets in the guest's stream at which its data segments end, in
    order of first appearance, and when each was first seen."""
    first = {}
    for t, s in p.log:
        if s["data"]:
            off = (s["seq"] - p.irs - 1) % M32
            first.setdefault(off, t)
    return first

def send_run(size, in_rule=None, out_rule=None, wait=30):
    """`mtcp send` of `size` bytes to a listening peer, through the rules.
    Returns the peer, the time the command was typed and the guest's
    segments (time, offset, length) in order."""
    port = next_port()
    p = Listener(net, port, 0, b"", active=False)
    tune(p)
    p.close_when = lambda q: q.peer_fin
    net.peer = p
    link.in_rule = in_rule or (lambda f: 0)
    link.out_rule = out_rule or (lambda f: 0)
    link.seen = []
    mark = len(g.out)
    g.type_line(f"mtcp send 10.0.2.2 {port} {size}")
    t0 = time.time()
    g.until(lambda: p.done() or p.state == "RESET", wait)
    g.until(lambda: b"mtcp: sent" in g.out[mark:] and b"# " in g.out[mark:], 10)
    segs = [(t, (s["seq"] - p.irs - 1) % M32, len(s["data"])) for t, s in link.seen if s["data"]]
    net.peer = None
    link.in_rule = link.out_rule = lambda f: 0
    return p, t0, segs

g.boot()

# 1. The initial window: with no ACK for the data, the guest sends ten
#    segments and waits (RFC 6928); the timer resends the first later.
gate = {"open": False}
def hold_acks(f):
    s = tcp_of(f)
    if s and not s["flags"] & SYN and not gate["open"]:
        return None
    return 0
port = next_port()
p = Listener(net, port, 0, b"", active=False)
tune(p)
p.close_when = lambda q: q.peer_fin
net.peer = p
link.out_rule = hold_acks
link.seen = []
mark = len(g.out)
g.type_line(f"mtcp send 10.0.2.2 {port} 60000")
g.until(lambda: len([1 for _, s in link.seen if s["data"]]) >= 10, 10)
g.until(lambda: False, 0.12)
first = {}
for t, s in link.seen:
    if s["data"]:
        first.setdefault(s["seq"], t)
check(len(first) == 10, f"initial window: ten segments go before any ACK ({len(first)} seen)")
gate["open"] = True
g.until(lambda: p.done() or p.state == "RESET", 30)
g.until(lambda: b"# " in g.out[mark:], 10)
check(bytes(p.got) == pat(60000), "initial window: the rest follows once ACKs come back")
net.peer = None
link.out_rule = lambda f: 0

# 2. Fast retransmit: the third segment of the window is lost; the
#    duplicate ACKs the others cause make the guest send it again within a
#    few round trips, long before any timer.
lost = {"seq": None, "n": 0}
def lose_third(f):
    s = tcp_of(f)
    if s and s["data"]:
        lost["n"] += 1
        if lost["n"] == 3 and lost["seq"] is None:
            lost["seq"] = s["seq"]
            return None
    return 0
p, t0, segs = send_run(40000, in_rule=lose_third)
at = [t for t, off, ln in segs if off == 2 * MSS]
check(len(at) == 2 and at[1] - at[0] < 0.12,
      "fast retransmit: a lost segment is sent again within " +
      (f"{at[1] - at[0]:.3f} s" if len(at) == 2 else "?  (not resent)") + " of its first try")
check(bytes(p.got) == pat(40000), "fast retransmit: the data arrived intact")

# 3. The resend timer follows the round trips: with the last segment lost
#    (no duplicate ACKs to say so), the guest resends it after about
#    200 ms, not the 1 s it waits before it has measured a round trip.
size = 8 * MSS
lost = {"n": 0, "done": False, "off": None}
def lose_last(f):
    s = tcp_of(f)
    if s and s["data"] and (s["seq"] - net.peer.irs - 1) % M32 + len(s["data"]) == size and not lost["done"]:
        lost["done"] = True
        lost["off"] = (s["seq"] - net.peer.irs - 1) % M32
        return None
    return 0
p, t0, segs = send_run(size, in_rule=lose_last)
at = [t for t, off, ln in segs if off == lost["off"]]
check(len(at) == 2 and 0.12 < at[1] - at[0] < 0.7,
      "resend timer: a lost last segment is sent again after " +
      (f"{at[1] - at[0]:.2f} s" if len(at) == 2 else "?  (not resent)") + " (the first resend waited 1 s before a round trip was measured)")
check(bytes(p.got) == pat(size), "resend timer: the data arrived intact")

# 4. Several holes in one window: three segments of forty are lost; only
#    they are sent again (SACK), and the window recovers within about a
#    round trip of each.
drop = {2: 1, 9: 1, 21: 1}
def lose_some(f):
    s = tcp_of(f)
    if s and s["data"]:
        k = (s["seq"] - net.peer.irs - 1) % M32 // MSS
        if drop.get(k):
            drop[k] -= 1
            return None
    return 0
size = 40 * MSS
p, t0, segs = send_run(size, in_rule=lose_some)
offs = [off for t, off, ln in segs]
resent = len(offs) - len(set(offs))
check(bytes(p.got) == pat(size), "several holes: the data arrived intact")
check(resent == 3, f"several holes: the three lost segments, and no others, were sent again ({resent} repeats)")

# 4b. A window too small for three duplicate ACKs: three segments go, the
#     first is lost, and the two duplicate ACKs that follow are enough
#     (early retransmit, RFC 5827); the timer is not waited for.
first_lost = {"done": False}
def lose_first(f):
    s = tcp_of(f)
    if s and s["data"] and not first_lost["done"]:
        first_lost["done"] = True
        return None
    return 0.01
size = 2 * MSS + 500
p, t0, segs = send_run(size, in_rule=lose_first, out_rule=lambda f: 0.01)
at = [t for t, off, ln in segs if off == 0]
check(len(at) == 2 and at[1] - at[0] < 0.12,
      "early retransmit: with three segments out, two duplicate ACKs resend the lost first one after " +
      (f"{at[1] - at[0]:.3f} s" if len(at) == 2 else "?  (not resent)"))
check(bytes(p.got) == pat(size), "early retransmit: the data arrived intact")

# 5. As a receiver: delayed ACKs. One full segment is acknowledged within
#    40 ms (a tick or two late), not at once; two in a row at the second;
#    a short one at once.
def sink_peer(data=b""):
    port = next_port()
    mark = len(g.out)
    g.type_line(f"mtcp sink {port}")
    g.until(lambda: b"listening on tcp port %d" % port in g.out[mark:], 20)
    q = Peer(net, 41000 + port, port, data, active=True)
    tune(q)
    q.output = lambda probe=False: None        # we send what we mean to, and when
    q.sack_ok = True
    net.peer = q
    link.seen = []
    q.emit(SYN, q.iss)
    q.timer = time.time() + q.rto
    g.until(lambda: q.state == "ESTABLISHED", 10)
    g.until(lambda: False, 0.3)
    return q, mark

def acks_since(q, t, upto=None):
    return [(ts, s) for ts, s in q.log if ts >= t and (upto is None or ts <= upto) and not s["data"] and not s["flags"] & (SYN | FIN)]

data = pat(12 * MSS)
q, mark = sink_peer(data)
q.nxt = 0
def seg(k, n=MSS):
    q.emit(PSH, q.abs_seq(k * MSS), data[k * MSS:k * MSS + n])
t = time.time()
seg(0)
g.until(lambda: acks_since(q, t), 1)
a = acks_since(q, t)
d1 = a[0][0] - t if a else None
check(d1 is not None and 0.015 < d1 < 0.12,
      "delayed ACK: one full segment is acknowledged after " + (f"{d1 * 1000:.0f} ms" if d1 else "?") + ", not at once")
g.until(lambda: False, 0.2)
t = time.time()
seg(1)
seg(2)
g.until(lambda: len(acks_since(q, t)) >= 1, 1)
g.until(lambda: False, 0.15)
a = acks_since(q, t)
check(len(a) == 1 and a[0][0] - t < 0.03,
      f"delayed ACK: two segments in a row get one ACK, at the second ({len(a)} ACKs, first after {(a[0][0] - t) * 1000:.0f} ms)" if a else "delayed ACK: two segments got no ACK")
t = time.time()
q.emit(PSH, q.abs_seq(3 * MSS), data[3 * MSS:3 * MSS + 100])
g.until(lambda: acks_since(q, t), 1)
a = acks_since(q, t)
check(a and a[0][0] - t < 0.03, "delayed ACK: a short segment (someone waits for the answer) is acknowledged at once")

# 6. D-SACK: the same segment again is reported, below the ACK number.
t = time.time()
q.emit(PSH, q.abs_seq(3 * MSS), data[3 * MSS:3 * MSS + 100])
g.until(lambda: any(sack_blocks(s) for _, s in acks_since(q, t)), 1)
a = [s for _, s in acks_since(q, t) if sack_blocks(s)]
want = (q.abs_seq(3 * MSS), q.abs_seq(3 * MSS + 100))
check(a and sack_blocks(a[0])[0] == want and (a[0]["ack"] - want[1]) % M32 < M32 // 2,
      "D-SACK: a repeated segment is reported in the first SACK block, below the ACK number")

# 7. Three gaps at once: segments 5, 7 and 9 are missing; the ACKs name
#    all three stretches held (RFC 2018), most recent first, and the guest
#    keeps them: once the gaps are filled, everything it read is intact.
q.emit(PSH, q.abs_seq(3 * MSS + 100), data[3 * MSS + 100:4 * MSS])   # the rest of segment 3
for k in (4, 6, 8, 10):
    seg(k)
g.until(lambda: False, 0.3)
last = [s for _, s in q.log if not s["data"] and not s["flags"] & SYN][-1]
blocks = sack_blocks(last)
want3 = {(q.abs_seq(10 * MSS), q.abs_seq(11 * MSS)), (q.abs_seq(8 * MSS), q.abs_seq(9 * MSS)), (q.abs_seq(6 * MSS), q.abs_seq(7 * MSS))}
check(set(blocks) >= want3 and len(blocks) >= 3,
      f"SACK: the ACK names the stretches held past three gaps ({len(blocks)} blocks)")
for k in (5, 7, 9, 11):
    seg(k)
q.emit(FIN + PSH, q.abs_seq(12 * MSS))
g.until(lambda: b"sunk" in g.out[mark:], 10)
check(f"sunk {12 * MSS} bytes, 0 wrong".encode() in g.out[mark:],
      "SACK: all stretches were kept, the stream arrived intact")
net.peer = None

g.kill()
print(f"---- {fails} failed ----")
sys.exit(1 if fails else 0)
PY
