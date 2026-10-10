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
#   NIC=name   the QEMU card (default: virtio-net-pci; e1000 also works)
set -u
cd "$(dirname "$0")/.."
KERNEL=${KERNEL:-build/bastios.elf}
SEED=${SEED:-1}
NIC=${NIC:-virtio-net-pci}

python3 - "$KERNEL" "$SEED" "$NIC" <<'PY'
import random, select, socket, struct, subprocess, sys, time

kernel, seed, card = sys.argv[1], int(sys.argv[2]), sys.argv[3]

GUEST_IP = (10 << 24) | (0 << 16) | (2 << 8) | 15
HOST_IP = (10 << 24) | (0 << 16) | (2 << 8) | 2
HOST_MAC = bytes.fromhex("525500000202")
BCAST = b"\xff" * 6
M32 = 1 << 32
FIN, SYN, RST, PSH, ACK = 1, 2, 4, 8, 16

def csum(data):
    if len(data) % 2:
        data += b"\0"
    s = sum(struct.unpack(f"!{len(data) // 2}H", data))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return 0xFFFF - s

def ip_packet(proto, src, dst, payload):
    h = struct.pack("!BBHHHBBHII", 0x45, 0, 20 + len(payload), 0, 0, 64, proto, 0, src, dst)
    h = h[:10] + struct.pack("!H", csum(h)) + h[12:]
    return h + payload

def l4_sum(proto, src, dst, seg):
    return csum(struct.pack("!IIBBH", src, dst, 0, proto, len(seg)) + seg)

# ------------------------------------------------------------------ the wire

class Link:
    """The other end of the card's wire. Frames to the guest go through
    `out_rule` and frames from it through `in_rule`: each returns None to
    drop the frame or a delay in seconds (0: at once)."""
    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.port = self.sock.getsockname()[1]
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(("127.0.0.1", 0))
        self.guest_port = s.getsockname()[1]
        s.close()
        self.guest_mac = None
        self.out_q = []           # (due, n, frame) to the guest
        self.in_q = []            # (due, n, frame) from it
        self.n = 0
        self.out_rule = lambda f: 0
        self.in_rule = lambda f: 0
        self.seen = []            # (time, segment) of every TCP segment from the guest

    def send(self, frame):
        d = self.out_rule(frame)
        if d is not None:
            self.n += 1
            self.out_q.append((time.time() + d, self.n, frame))

    def poll(self, wait):
        """Moves due frames and returns those from the guest now due."""
        now = time.time()
        due = [x for x in self.out_q if x[0] <= now]
        self.out_q = [x for x in self.out_q if x[0] > now]
        for _, _, f in sorted(due):
            self.sock.sendto(f, ("127.0.0.1", self.guest_port))
        nxt = min([x[0] for x in self.out_q + self.in_q], default=now + wait)
        r, _, _ = select.select([self.sock], [], [], max(0, min(wait, nxt - now)))
        if r:
            f, _ = self.sock.recvfrom(4096)
            s = tcp_of(f)
            if s:
                self.seen.append((time.time(), s))
            d = self.in_rule(f)
            if d is not None:
                self.n += 1
                self.in_q.append((time.time() + d, self.n, f))
        now = time.time()
        got = sorted(x for x in self.in_q if x[0] <= now)
        self.in_q = [x for x in self.in_q if x[0] > now]
        return [f for _, _, f in got]

def tcp_of(frame):
    """The TCP segment in a frame as a dict, or None."""
    if len(frame) < 54 or frame[12:14] != b"\x08\x00" or frame[23] != 6:
        return None
    ihl = (frame[14] & 15) * 4
    total = struct.unpack("!H", frame[16:18])[0]
    t = frame[14 + ihl:14 + total]
    sport, dport, seq, ack, off, flags, wnd = struct.unpack("!HHIIBBH", t[:16])
    return dict(sport=sport, dport=dport, seq=seq, ack=ack, flags=flags,
                wnd=wnd, data=t[(off >> 4) * 4:], frame=frame)

# ------------------------------------------------------------------ a TCP peer

class Peer:
    """One TCP connection at 10.0.2.2, with offsets counted from the
    initial sequence numbers. It holds segments that arrive out of order,
    as Linux does, and resends with go-back-N."""
    def __init__(self, net, lport, rport, data, active, mss=536):
        self.net, self.lport, self.rport = net, lport, rport
        self.data, self.mss = data, mss
        self.iss = 0x7FFFF000           # near a wrap of the 32-bit space
        self.state = "SYN_SENT" if active else "LISTEN"
        self.una = 0                    # offsets into data; len(data) is our FIN
        self.nxt = 0
        self.max = 0
        self.irs = None
        self.got = bytearray()
        self.held = {}                  # offset -> bytes, out of order
        self.peer_fin_at = None         # the offset of the guest's FIN
        self.peer_fin = False
        self.peer_wnd = 0
        self.rwin = 8192                # the window we offer
        self.closing = False            # our FIN may go once the data is out
        self.timer = None
        self.recover = 0
        self.rto = 0.25
        self.log = []                   # (time, segment) of every guest segment seen
        self.close_when = lambda p: False
        if active:
            self.timer = time.time()

    def abs_seq(self, off):
        return (self.iss + 1 + off) % M32

    def rcv_abs(self):
        return (self.irs + 1 + len(self.got) + (1 if self.peer_fin else 0)) % M32

    def emit(self, flags, seq, data=b"", window=None):
        win = self.rwin if window is None else window
        ack = self.rcv_abs() if self.irs is not None else 0
        hl = 24 if flags & SYN else 20
        seg = struct.pack("!HHIIBBHHH", self.lport, self.rport, seq, ack, hl // 4 << 4,
                          flags | (ACK if self.irs is not None else 0), win, 0, 0)
        if flags & SYN:
            seg += struct.pack("!BBH", 2, 4, self.mss)
        seg += data
        seg = seg[:16] + struct.pack("!H", l4_sum(6, HOST_IP, GUEST_IP, seg)) + seg[18:]
        self.net.send_ip(6, seg)

    def ack_now(self):
        self.emit(0, self.abs_seq(self.nxt))

    def output(self, probe=False):
        if self.state not in ("ESTABLISHED", "CLOSE_WAIT"):
            return
        wnd = max(self.peer_wnd, 1 if probe else 0)
        while self.nxt < len(self.data) and self.nxt < self.una + wnd:
            k = min(self.mss, len(self.data) - self.nxt, self.una + wnd - self.nxt)
            self.emit(PSH, self.abs_seq(self.nxt), self.data[self.nxt:self.nxt + k])
            self.nxt += k
        if self.closing and self.nxt == len(self.data):
            self.emit(FIN, self.abs_seq(self.nxt))
            self.nxt += 1
        self.max = max(self.max, self.nxt)
        if self.timer is None and (self.max > self.una or
                                   (self.peer_wnd == 0 and self.una < len(self.data))):
            self.timer = time.time() + self.rto

    def resend_head(self):
        """Sends the oldest unacknowledged segment again (or one byte into
        a shut window, or our FIN)."""
        if self.una < len(self.data):
            k = min(self.mss, len(self.data) - self.una)
            if self.peer_wnd == 0:
                k = 1
            elif self.max > self.una:
                k = min(k, self.max - self.una)
            self.emit(PSH, self.abs_seq(self.una), self.data[self.una:self.una + k])
            self.nxt = max(self.nxt, self.una + k)
            self.max = max(self.max, self.nxt)
        elif self.closing and self.una == len(self.data):
            self.emit(FIN, self.abs_seq(self.una))
            self.nxt = self.max = len(self.data) + 1
        self.timer = time.time() + self.rto

    def tick(self):
        if self.timer is None or time.time() < self.timer:
            return
        self.timer = None
        if self.state in ("SYN_SENT", "SYN_RCVD"):
            self.emit(SYN, self.iss)
            self.timer = time.time() + self.rto
        elif self.state in ("ESTABLISHED", "CLOSE_WAIT"):
            # Only the head goes again (no go-back-N): a receiver that
            # dropped what came after the gap is told so by its next ACK.
            self.recover = self.max
            self.resend_head()

    def take(self, s):
        self.log.append((time.time(), s))
        if s["flags"] & RST:
            self.state = "RESET"
            return
        if self.state == "LISTEN":
            if s["flags"] & SYN:
                self.irs = s["seq"]
                self.peer_wnd = s["wnd"]
                self.state = "SYN_RCVD"
                self.emit(SYN, self.iss)
                self.timer = time.time() + self.rto
            return
        if self.state == "SYN_SENT":
            if s["flags"] & SYN and s["flags"] & ACK and s["ack"] == (self.iss + 1) % M32:
                self.irs = s["seq"]
                self.state = "ESTABLISHED"
                self.timer = None
                self.peer_wnd = s["wnd"]
                self.ack_now()
                self.output()
            return
        if s["flags"] & SYN:
            self.ack_now()                  # our ACK of its SYN was lost
            return
        if self.state == "SYN_RCVD":
            if not s["flags"] & ACK or s["ack"] != (self.iss + 1) % M32:
                return
            self.state = "ESTABLISHED"
            self.timer = None
        if s["flags"] & ACK:
            a = (s["ack"] - self.iss - 1) % M32
            self.peer_wnd = s["wnd"]
            if self.una < a <= self.max:
                self.una = a
                self.nxt = max(self.nxt, a)
                self.timer = None
                if a < self.recover:
                    self.resend_head()      # a partial ACK while recovering
        # Data, held if it is out of order, and only within our window.
        off = (s["seq"] - self.irs - 1) % M32
        if off >= M32 // 2:
            off -= M32
        d = s["data"]
        room = len(self.got) + self.rwin
        if d:
            if off + len(d) > room:
                d = d[:max(0, room - off)]
            if d and off + len(d) > len(self.got):
                self.held[off] = d
        if s["flags"] & FIN and off + len(s["data"]) <= room:
            self.peer_fin_at = off + len(s["data"])
        while True:
            moved = False
            for o in sorted(self.held):
                b = self.held[o]
                if o <= len(self.got):
                    del self.held[o]
                    if o + len(b) > len(self.got):
                        self.got += b[len(self.got) - o:]
                    moved = True
                    break
            if not moved:
                break
        if self.peer_fin_at is not None and self.peer_fin_at == len(self.got):
            self.peer_fin = True
        if s["data"] or s["flags"] & FIN:
            self.ack_now()
        if not self.closing and self.close_when(self):
            self.closing = True
        self.output()

    def done(self):
        return self.state == "RESET" or (self.peer_fin and self.closing and self.una == len(self.data) + 1)

class Net:
    """ARP, DHCP and TCP for one peer at a time."""
    def __init__(self, link):
        self.link = link
        self.peer = None
        self.dhcp_done = False

    def send_ip(self, proto, payload):
        mac = self.link.guest_mac or BCAST
        self.link.send(mac + HOST_MAC + b"\x08\x00" + ip_packet(proto, HOST_IP, GUEST_IP, payload))

    def handle(self, f):
        if len(f) < 14:
            return
        if self.link.guest_mac is None:
            self.link.guest_mac = f[6:12]
        if f[12:14] == b"\x08\x06" and len(f) >= 42:
            op = struct.unpack("!H", f[20:22])[0]
            tip = struct.unpack("!I", f[38:42])[0]
            if op == 1 and tip == HOST_IP:
                arp = struct.pack("!HHBBH", 1, 0x0800, 6, 4, 2) + HOST_MAC + f[38:42] + f[22:28] + f[28:32]
                self.link.send(f[6:12] + HOST_MAC + b"\x08\x06" + arp)
            return
        if f[12:14] != b"\x08\x00":
            return
        if f[23] == 17 and len(f) >= 282 + 4 and struct.unpack("!H", f[36:38])[0] == 67:
            self.dhcp(f)
            return
        s = tcp_of(f)
        if s and self.peer and s["dport"] == self.peer.lport:
            self.peer.take(s)

    def dhcp(self, f):
        xid = f[46:50]
        kind = None
        o = 282
        while o + 1 < len(f) and f[o] != 255:
            if f[o] == 0:
                o += 1
                continue
            if f[o] == 53:
                kind = f[o + 2]
            o += 2 + f[o + 1]
        reply = {1: 2, 3: 5}.get(kind)
        if reply is None:
            return
        b = bytearray(240)
        b[0], b[1], b[2] = 2, 1, 6
        b[4:8] = xid
        b[16:20] = struct.pack("!I", GUEST_IP)
        b[28:34] = f[6:12]
        b[236:240] = bytes.fromhex("63825363")
        b += bytes([53, 1, reply, 54, 4]) + struct.pack("!I", HOST_IP)
        b += bytes([1, 4, 255, 255, 255, 0, 3, 4]) + struct.pack("!I", HOST_IP)
        b += bytes([51, 4, 0, 0, 14, 16, 255])
        udp = struct.pack("!HHHH", 67, 68, 8 + len(b), 0) + bytes(b)
        udp = udp[:6] + struct.pack("!H", l4_sum(17, HOST_IP, 0xFFFFFFFF, udp)) + udp[8:]
        # The guest takes the frame before it has an address: send to all.
        self.link.send(BCAST + HOST_MAC + b"\x08\x00" +
                       ip_packet(17, HOST_IP, 0xFFFFFFFF, udp))
        if reply == 5:
            self.dhcp_done = True

# ------------------------------------------------------------------ the guest

link = Link()
net = Net(link)
qemu = subprocess.Popen(
    ["qemu-system-x86_64", "-kernel", kernel, "-m", "64M", "-smp", "2",
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

def echo_run(name, data, setup, secs=60):
    """`tcpecho 7001` in the guest and a connection to it from here that
    sends `data` and closes once it all came back. `setup(peer)` sets the
    rules. Returns the peer."""
    mark = len(out)
    type_line("tcpecho 7001")
    until(lambda: b"listening on tcp port 7001" in out[mark:], 10)
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
#    we probe it in turn. Then ours reopens.
def zero_window_scenario():
    mark = len(out)
    data = pattern(20000, 4)
    type_line("tcpecho 7001")
    until(lambda: b"listening on tcp port 7001" in out[mark:], 10)
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

# 6. The guest connects out: its first SYN is lost.
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
net.peer = None

qemu.kill(); qemu.wait()
print("---- transcript ----")
print(out.decode(errors="replace").replace("\r", ""))
print(f"---- {fails} failed ----")
sys.exit(1 if fails else 0)
PY
