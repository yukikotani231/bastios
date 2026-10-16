#!/usr/bin/env bash
# TCP over a network this script controls. QEMU's user-mode network never
# loses or reorders a packet and acknowledges everything at once, so the
# paths of src/tcp.bastio that matter on a real network never run under it.
# Here the virtio card is joined to this script instead (QEMU's `dgram`
# netdev: every Ethernet frame is one UDP datagram on 127.0.0.1), and the
# script plays the rest of the network: ARP, a DHCP server, and a small TCP
# peer of its own that can drop, delay, reorder and duplicate segments and
# close its window, by rule or on a seed.
#
#   SEED=n     the seed for the random-loss run (default: 1)
#   NIC=name   the QEMU card (default: virtio-net-pci; e1000, e1000e, igb and ena also work)
set -u
cd "$(dirname "$0")/.."
KERNEL=${KERNEL:-build/bastios.elf}
SEED=${SEED:-1}
NIC=${NIC:-virtio-net-pci}

python3 - "$KERNEL" "$SEED" "$NIC" <<'PY'
import os, random, select, socket, struct, subprocess, sys, time

kernel, seed, card = sys.argv[1], int(sys.argv[2]), sys.argv[3]

sys.path.insert(0, "test")
from netwire import *

# ------------------------------------------------------------------ the guest

# The ENA card is not in QEMU: it needs the build of tools/qemu-ena.
qemu_bin = "qemu-system-x86_64"
if card == "ena":
    qemu_bin = os.environ.get("QEMU_ENA", "build/qemu-ena/bin/qemu-system-x86_64")

link = Link()
net = Net(link)
qemu = subprocess.Popen(
    [qemu_bin, "-kernel", kernel, "-m", "64M", "-smp", "2",
     "-display", "none", "-no-reboot", "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
     "-serial", "stdio", "-monitor", "none",
     "-netdev", f"dgram,id=n0,local.type=inet,local.host=127.0.0.1,local.port={link.guest_port},"
                f"remote.type=inet,remote.host=127.0.0.1,remote.port={link.port}",
     "-device", f"{card},netdev=n0"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
out = b""
fails = 0

def pump(wait=0.02):
    """Serial output in, frames both ways, timers."""
    global out
    for f in link.poll(wait):
        net.handle(f)
    if net.peer:
        net.peer.tick()
    while True:
        r, _, _ = select.select([qemu.stdout], [], [], 0)
        if not r:
            break
        chunk = qemu.stdout.read1(4096)
        if not chunk:
            break
        out += chunk

def until(cond, secs):
    end = time.time() + secs
    while time.time() < end:
        if cond():
            return True
        pump()
    return cond()

def type_line(line):
    qemu.stdin.write(line.encode() + b"\r")
    qemu.stdin.flush()

def check(ok, what):
    global fails
    print(("ok  " if ok else "FAIL"), what, flush=True)
    fails += not ok

def pattern(n, salt):
    r = random.Random(salt)
    return bytes(r.randrange(256) for _ in range(n))

def listen_7001():
    """`tcpecho 7001` in the guest, typed again if the port is still taken
    (the last connection on it can still be waiting for our final ACK when
    the shell prompt comes back). Returns where its output starts;
    `listen_tries` says how many times it was typed."""
    global listen_tries
    listen_tries = 0
    for _ in range(5):
        listen_tries += 1
        mark = len(out)
        type_line("tcpecho 7001")
        until(lambda: b"listening on tcp port 7001" in out[mark:] or b"cannot listen" in out[mark:], 10)
        if b"listening on tcp port 7001" in out[mark:]:
            break
        until(lambda: b"bastios> " in out[mark:], 5)
        until(lambda: False, 0.5)
    return mark

def echo_run(name, data, setup, secs=60):
    """`tcpecho 7001` in the guest and a connection to it from here that
    sends `data` and closes once it all came back. `setup(peer)` sets the
    rules. Returns the peer."""
    mark = listen_7001()
    p = Peer(net, 40000 + (len(out) % 20000), 7001, data, active=True)
    p.close_when = lambda q: len(q.got) >= len(data)
    net.peer = p
    link.seen = []
    setup(p)
    p.emit(SYN, p.iss)
    p.timer = time.time() + p.rto
    finished = until(lambda: p.done() and b"bastios> " in out[mark:], secs)
    link.out_rule = lambda f: 0
    link.in_rule = lambda f: 0
    check(finished and p.state != "RESET" and bytes(p.got) == data
          and f"echoed {len(data)} bytes".encode() in out[mark:],
          f"{name}: {len(data)} bytes echoed intact")
    if not finished:
        print(f"     state {p.state}, got {len(p.got)} of {len(data)}, una {p.una} max {p.max}")
        until(lambda: b"bastios> " in out[mark:], 20)
    net.peer = None
    return p

def guest_data(p):
    return [s for _, s in p.log if s["data"]]

ok = until(lambda: b"bastios> " in out, 60)
check(ok, f"boots with its card ({card}) on this script's wire")
# A card's link can take a moment to come up (an e1000's does), so DHCP
# may need a retry or two: ask until the lease is there.
leased = False
t_boot = time.time()
while not leased and time.time() - t_boot < 10:
    mark = len(out)
    type_line("ifconfig")
    until(lambda: b"bastios> " in out[mark:], 10)
    leased = b"10.0.2.15 netmask 255.255.255.0 gateway 10.0.2.2 (DHCP)" in out[mark:]
    if not leased:
        until(lambda: False, 0.5)
check(leased and net.dhcp_done, f"takes its address from this script's DHCP server ({time.time() - t_boot:.1f}s)")

# 1. Lost segments from the guest: its SYN+ACK once and its first data
#    segment twice. It must send each again when its timer runs out.
lost = {"synack": 1, "data": 2}
def lose_guest(f):
    s = tcp_of(f)
    if s and s["flags"] & SYN and lost["synack"]:
        lost["synack"] -= 1
        return None
    if s and s["data"] and s["seq"] == (net.peer.irs + 1) % M32 and lost["data"]:
        lost["data"] -= 1
        return None
    return 0
p = echo_run("loss from the guest", pattern(6000, 1), lambda p: setattr(link, "in_rule", lose_guest))
synacks = [s for _, s in link.seen if s["flags"] & SYN]
first = [s for _, s in link.seen if s["data"] and s["seq"] == (p.irs + 1) % M32]
biggest = max(len(s["data"]) for _, s in link.seen)
check(biggest <= 536, f"the guest keeps to the MSS we asked for (largest segment {biggest})")
check(len(synacks) >= 2, f"loss from the guest: SYN+ACK sent again ({len(synacks)} seen)")
check(len(first) >= 3, f"loss from the guest: first segment sent {len(first)} times")
# The resends back off: each wait is longer than the one before.
times = [t for t, s in link.seen if s["data"] and s["seq"] == (p.irs + 1) % M32][:3]
gaps = [b - a for a, b in zip(times, times[1:])]
check(len(gaps) == 2 and gaps[1] > gaps[0] * 1.5,
      "loss from the guest: the resend timer backs off (" + ", ".join(f"{g:.2f}s" for g in gaps) + ")")

# 2. Lost and reordered segments to the guest: our second and third
#    segments overtake the first, which is lost once.
order = {"n": 0, "first": None, "sent": set(), "again": 0}
def reorder_to_guest(f):
    s = tcp_of(f)
    if s and s["data"]:
        order["n"] += 1
        if order["n"] == 1:
            order["first"] = s["seq"]
        elif s["seq"] in order["sent"] and s["seq"] != order["first"]:
            order["again"] += 1
        order["sent"].add(s["seq"])
        if order["n"] == 1:
            return None                 # lost; our timer sends it again
        if order["n"] == 2:
            return 0.05
    return 0
p = echo_run("reordered to the guest", pattern(8000, 2), lambda p: setattr(link, "out_rule", reorder_to_guest))
# The guest holds what came early: once the gap is filled its ACK covers
# the segments that overtook it, so none but the lost one went twice.
check(order["again"] == 0, f"reordered to the guest: early segments kept ({order['again']} resent besides the lost one)")

# 3. The wire to the guest stalls and then delivers everything at once,
#    late: the guest's timer runs out first, so it goes back and resends
#    what we already have, and then takes the late ACKs and our duplicates.
stall = {"from": None}
def stall_acks(f):
    if tcp_of(f) and net.peer and len(net.peer.got) > 2000:
        now = time.time()
        if stall["from"] is None:
            stall["from"] = now
        if now < stall["from"] + 1.5:
            return stall["from"] + 1.5 - now
    return 0
p = echo_run("late ACKs", pattern(12000, 3), lambda p: setattr(link, "out_rule", stall_acks))
seqs = [s["seq"] for _, s in link.seen if s["data"]]
seqs = [s["seq"] for _, s in link.seen if s["data"]]
check(len(seqs) > len(set(seqs)), f"late ACKs: the guest timed out and went back ({len(seqs) - len(set(seqs))} segments resent)")

# 4. Our window is shut from the start and stays shut for longer than the
#    guest's resend limit: the guest must probe it, and not give up while
#    we answer. Meanwhile its own buffers fill and its window closes, and
#    we probe it in turn. Then ours reopens. (The data is more than the
#    guest's send and receive rings hold together.)
def zero_window_scenario():
    data = pattern(700000, 4)
    mark = listen_7001()
    q = Peer(net, 45000, 7001, data, active=True)
    q.close_when = lambda r: len(r.got) >= len(data)
    q.rwin = 0
    net.peer = q
    q.emit(SYN, q.iss)
    q.timer = time.time() + q.rto
    t0 = time.time()
    shut = 12.0
    until(lambda: time.time() - t0 > shut or q.state == "RESET", shut + 1)
    probes = [s for t, s in q.log if s["data"] and t - t0 < shut]
    guest_shut = any(s["wnd"] == 0 for _, s in q.log)
    alive = q.state == "ESTABLISHED"
    q.rwin = 8192
    q.ack_now()                         # the window reopens
    # We dropped the probe byte (no room), so the guest must go back to it
    # at once, not leave a hole for its backed-off resend timer to fill.
    prompt = until(lambda: len(q.got) > 0, 2)
    finished = until(lambda: q.done() and b"bastios> " in out[mark:], 60)
    check(prompt, "zero window: the guest resends the dropped probe byte as soon as the window reopens")
    check(alive, "zero window: the guest keeps the connection while we answer its probes")
    check(len(probes) >= 3, f"zero window: the guest probed our closed window ({len(probes)} probes)")
    check(guest_shut, "zero window: the guest closed its own window when its buffers filled")
    check(finished and bytes(q.got) == data and f"echoed {len(data)} bytes".encode() in out[mark:],
          f"zero window: {len(data)} bytes echoed intact once it reopened")
    if not finished:
        print(f"     state {q.state}, got {len(q.got)}, una {q.una} max {q.max}")
        until(lambda: b"bastios> " in out[mark:], 20)
    net.peer = None
zero_window_scenario()

# 5. Random loss, delay, reordering and duplication both ways, on a seed.
rng = random.Random(seed)
def chaos(f):
    if tcp_of(f) is None:
        return 0
    x = rng.random()
    if x < 0.08:
        return None
    return rng.random() * 0.06 if rng.random() < 0.3 else 0
def chaos_out(f):
    d = chaos(f)
    if d is not None and rng.random() < 0.04:
        link.n += 1
        link.out_q.append((time.time() + d + 0.02, link.n, f))   # a duplicate
    return d
def chaos_setup(p):
    link.out_rule = chaos_out
    link.in_rule = chaos
p = echo_run(f"random loss (SEED={seed})", pattern(30000, 5), chaos_setup, secs=120)

# 5b. A connection the guest has let go of but not finished closing (our
#     ACK of its FIN is lost, so it waits in LAST_ACK, sending the FIN
#     again to nobody) does not keep a new listener off its port.
def lose_last_ack(p):
    link.out_rule = lambda f: None if tcp_of(f) and p.peer_fin else 0
p = echo_run("our last ACK lost", pattern(3000, 7), lose_last_ack)
p = echo_run("a new listener beside a closing connection", pattern(3000, 8), lambda p: None)
check(listen_tries == 1, f"a new listener beside a closing connection: tcpecho listens at once ({listen_tries} tries)")

# 6. Window scaling and SACK (RFC 7323, RFC 2018): we offer both, so the
#    guest offers a window past 64 KiB, and when one of our segments is
#    lost its ACKs say with a SACK block what came after the gap.
lost3 = {"n": 0, "end": None}
def lose_third(f):
    s = tcp_of(f)
    if s and s["data"]:
        lost3["n"] += 1
        if lost3["n"] == 3:
            lost3["end"] = (s["seq"] + len(s["data"])) % M32
            return None
    return 0
def scale_setup(p):
    p.ws, p.sack_ok, p.rwin, p.mss = 3, True, 200000, 1460
    link.out_rule = lose_third
p = echo_run("window scaling and SACK", pattern(300000, 6), scale_setup)
synacks = [s for _, s in p.log if s["flags"] & SYN]
check(synacks and 3 in synacks[0]["opts"] and 4 in synacks[0]["opts"],
      "window scaling and SACK: the guest's SYN+ACK takes up both")
wide = max([p.scaled(s) for _, s in p.log if not s["flags"] & SYN] + [0])
check(p.peer_ws is not None and wide > 65535, f"window scaling: the guest offers a window past 64 KiB ({wide} bytes)")
sacked = [b for _, s in p.log for b in sack_blocks(s)]
check(lost3["end"] is not None and any(l == lost3["end"] and r != l for l, r in sacked),
      f"SACK: the guest's ACKs name what it holds past the lost segment ({len(sacked)} blocks)")

# 7. The guest connects out: its first SYN is lost.
mark = len(out)
srv = Peer(net, 8080, 0, b"hello from the host\n", active=False)
srv.close_when = lambda q: b"\n" in q.got
lost_syn = {"n": 1}
def lose_syn(f):
    s = tcp_of(f)
    if s and s["flags"] & SYN and lost_syn["n"]:
        lost_syn["n"] -= 1
        return None
    return 0
def take_port(s):
    if srv.rport == 0:
        srv.rport = s["sport"]
link.in_rule = lose_syn
orig_take = srv.take
srv.take = lambda s: (take_port(s), orig_take(s))
net.peer = srv
type_line("tcpget 10.0.2.2 8080 hi")
finished = until(lambda: b"closed\r\n" in out[mark:] and b"bastios> " in out[mark:], 30)
link.in_rule = lambda f: 0
syns = [s for _, s in srv.log if s["flags"] & SYN]
check(finished and b"hello from the host" in out[mark:] and bytes(srv.got) == b"hi\n",
      f"connecting out: SYN sent again after a loss ({len(syns)} seen), reply read")
check(syns and all(k in syns[0]["opts"] for k in (2, 3, 4)),
      "connecting out: the SYN offers an MSS, window scaling and SACK (and nothing is scaled when we do not)")
net.peer = None

# 7. The ARP cache holds more than four neighbors: the guest pings twelve
#    hosts that this script answers for, then the same twelve again. The
#    second round needs no ARP request.
def ping_round():
    for i in range(12):
        mark = len(out)
        type_line(f"ping 10.0.2.{3 + i}")
        until(lambda: b"bastios> " in out[mark:], 10)
        if f"reply from 10.0.2.{3 + i}".encode() not in out[mark:]:
            return False
    return True

net.arp_asked = []
first = ping_round()
asked_first = len(net.arp_asked)
second = ping_round()
asked_second = len(net.arp_asked) - asked_first
check(first and second, "ARP cache: twelve neighbors answer a ping, twice")
check(asked_first >= 12 and asked_second == 0, f"ARP cache: {asked_first} requests for twelve neighbors, {asked_second} the second time")

qemu.kill(); qemu.wait()
print("---- transcript ----")
print(out.decode(errors="replace").replace("\r", ""))
print(f"---- {fails} failed ----")
sys.exit(1 if fails else 0)
PY
