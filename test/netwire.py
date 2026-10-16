"""The wire half of test/tcp_test.sh and test/tcpperf.sh: a QEMU `dgram`
netdev is one UDP datagram per Ethernet frame on 127.0.0.1, so a script can
sit at the other end of a guest's card, play the rest of the network (ARP,
DHCP, a TCP peer) and lose, delay, reorder and duplicate what it likes."""
import os, random, select, socket, struct, subprocess, sys, time

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
        # A burst of a full window must not overflow the socket: frames lost
        # here would look like loss on the wire. (The limit is the kernel's
        # unless we may override it.)
        for opt in (getattr(socket, "SO_RCVBUFFORCE", 33), socket.SO_RCVBUF):
            try:
                self.sock.setsockopt(socket.SOL_SOCKET, opt, 16 << 20)
                break
            except OSError:
                pass
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
                wnd=wnd, data=t[(off >> 4) * 4:], frame=frame, opts=options(t[20:(off >> 4) * 4]))

def options(raw):
    """A TCP header's options: kind -> its bytes after the length."""
    o, found = 0, {}
    while o < len(raw) and raw[o] != 0:
        if raw[o] == 1:
            o += 1
            continue
        if o + 1 >= len(raw) or raw[o + 1] < 2:
            break
        found[raw[o]] = raw[o + 2:o + raw[o + 1]]
        o += raw[o + 1]
    return found

def sack_blocks(s):
    """The (left, right) sequence numbers of a segment's SACK blocks."""
    b = s["opts"].get(5, b"")
    return [struct.unpack("!II", b[i:i + 8]) for i in range(0, len(b) - 7, 8)]

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
        self.ws = None                  # the window scale we offer in our SYN, if any
        self.sack_ok = False            # whether our SYN says we take SACK blocks
        self.peer_ws = None             # the guest's, once both sides scale
        self.sack_both = False          # both SYNs carried SACK-permitted
        self.last_off = None            # where the segment that arrived last starts
        self.dsack = None               # a duplicate to report in the next ACK (RFC 2883)
        self.dup_data = 0               # segments that arrived with nothing new in them
        self.dup_acks = 0               # ACKs we sent that acknowledged nothing new
        self.last_ack_sent = None
        self.cc = False                 # congestion control and SACK recovery as a sender (like a modern stack)
        self.cwnd = None
        self.ssthresh = 1 << 30
        self.dups = 0
        self.rec_end = None             # in recovery until this offset is acknowledged
        self.sacked = []                # [(left, right)] offsets the guest reports holding
        self.rtx_next = 0
        if active:
            self.timer = time.time()

    def abs_seq(self, off):
        return (self.iss + 1 + off) % M32

    def rcv_abs(self):
        return (self.irs + 1 + len(self.got) + (1 if self.peer_fin else 0)) % M32

    def emit(self, flags, seq, data=b"", window=None):
        win = self.rwin if window is None else window
        if not flags & SYN and self.peer_ws is not None:
            win >>= self.ws                 # scaled, except in a SYN
        win = min(win, 65535)
        ack = self.rcv_abs() if self.irs is not None else 0
        opts = b""
        if flags & SYN:
            opts = struct.pack("!BBH", 2, 4, self.mss)
            if self.ws is not None:
                opts += bytes([1, 3, 3, self.ws])
            if self.sack_ok:
                opts += bytes([1, 1, 4, 2])
        if not flags & SYN and self.sack_both and (self.held or self.dsack):
            blocks = ([self.dsack] if self.dsack else []) + self.sack_ranges()[:3]
            self.dsack = None
            opts = bytes([1, 1, 5, 2 + 8 * len(blocks)]) + b"".join(
                struct.pack("!II", (self.irs + 1 + l) % M32, (self.irs + 1 + r) % M32) for l, r in blocks)
        hl = 20 + len(opts)
        seg = struct.pack("!HHIIBBHHH", self.lport, self.rport, seq, ack, hl // 4 << 4,
                          flags | (ACK if self.irs is not None else 0), win, 0, 0)
        if not flags & SYN and not data:
            if ack == self.last_ack_sent:
                self.dup_acks += 1
            self.last_ack_sent = ack
        seg += opts
        seg += data
        seg = seg[:16] + struct.pack("!H", l4_sum(6, HOST_IP, GUEST_IP, seg)) + seg[18:]
        self.net.send_ip(6, seg)

    def sack_ranges(self):
        """What is held past the gap, as merged [left, right) offsets, the
        block with the segment that arrived last first (RFC 2018)."""
        spans = []
        for o in sorted(self.held):
            r = o + len(self.held[o])
            if spans and o <= spans[-1][1]:
                spans[-1][1] = max(spans[-1][1], r)
            else:
                spans.append([o, r])
        spans = [tuple(x) for x in spans]
        if self.last_off is not None:
            spans.sort(key=lambda x: not (x[0] <= self.last_off < x[1]))
        return spans

    def ack_now(self):
        self.emit(0, self.abs_seq(self.nxt))

    def output(self, probe=False):
        if self.state not in ("ESTABLISHED", "CLOSE_WAIT"):
            return
        wnd = max(self.peer_wnd, 1 if probe else 0)
        if self.cc:
            if self.cwnd is None:
                self.cwnd = 10 * self.mss
            wnd = min(wnd, self.cwnd)
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

    def cc_ack(self, s, a):
        """A sender with slow start, congestion avoidance, fast retransmit and
        SACK-driven recovery (RFC 5681, 6675), for peers that send to the guest."""
        if self.cwnd is None:
            self.cwnd = 10 * self.mss
        for l, r in sack_blocks(s):
            l, r = (l - self.iss - 1) % M32, (r - self.iss - 1) % M32
            if self.una < l < r <= self.max:
                self.sacked = [x for x in self.sacked if x[1] < l or x[0] > r] + [
                    (min([l] + [x[0] for x in self.sacked if not (x[1] < l or x[0] > r)]),
                     max([r] + [x[1] for x in self.sacked if not (x[1] < l or x[0] > r)]))]
        if self.una < a <= self.max:
            acked = a - self.una
            self.una = a
            self.nxt = max(self.nxt, a)
            self.sacked = [(max(l, a), r) for l, r in self.sacked if r > a]
            self.dups = 0
            self.timer = None
            if self.rec_end is not None:
                if a >= self.rec_end:
                    self.rec_end = None
                    self.cwnd = self.ssthresh
                    self.sacked = []
                else:
                    self.rtx_hole()
            elif self.cwnd < self.ssthresh:
                self.cwnd += min(acked, 2 * self.mss)
            else:
                self.cwnd += max(1, self.mss * self.mss // self.cwnd)
        elif a == self.una and self.max > self.una and not s["data"]:
            self.dups += 1
            hi = max([r for _, r in self.sacked] + [self.una])
            if self.rec_end is None and (self.dups >= 3 or sum(r - l for l, r in self.sacked) >= 3 * self.mss):
                flight = self.max - self.una
                self.ssthresh = self.cwnd = max(flight // 2, 2 * self.mss)
                self.rec_end = self.max
                self.rtx_next = self.una
                self.rtx_hole()
            elif self.rec_end is not None:
                self.rtx_hole()
        if self.timer is None and self.max > self.una:
            self.timer = time.time() + self.rto

    def rtx_hole(self):
        """Sends the next segment the guest is not known to hold (below the
        furthest it reports), if the window allows."""
        hi = max([r for _, r in self.sacked] + [self.una + 1])
        o = max(self.rtx_next, self.una)
        for l, r in sorted(self.sacked):
            if l <= o < r:
                o = r
        if o >= min(hi, len(self.data)) and not (self.sacked == [] and o == self.una):
            return
        k = min(self.mss, len(self.data) - o)
        for l, r in sorted(self.sacked):
            if l > o:
                k = min(k, l - o)
        if k > 0:
            self.emit(PSH, self.abs_seq(o), self.data[o:o + k])
            self.rtx_next = o + k

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
            if self.cc:
                self.ssthresh = max((self.max - self.una) // 2, 2 * self.mss)
                self.cwnd = self.mss
                self.rec_end = None
                self.dups = 0
                self.sacked = []
                self.nxt = self.una
            self.resend_head()

    def scaled(self, s):
        """The window a segment from the guest gives, scaled as agreed."""
        if s["flags"] & SYN or self.peer_ws is None:
            return s["wnd"]
        return s["wnd"] << self.peer_ws

    def agree(self, s):
        """Window scaling is on if both SYNs offered it."""
        if self.ws is not None and 3 in s["opts"]:
            self.peer_ws = min(s["opts"][3][0], 14)
        self.sack_both = self.sack_ok and 4 in s["opts"]

    def take(self, s):
        self.log.append((time.time(), s))
        if s["flags"] & RST:
            self.state = "RESET"
            return
        if self.state == "LISTEN":
            if s["flags"] & SYN:
                self.irs = s["seq"]
                self.peer_wnd = s["wnd"]
                self.agree(s)
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
                self.agree(s)
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
            self.peer_wnd = self.scaled(s)
            if self.cc:
                self.cc_ack(s, a)
            elif self.una < a <= self.max:
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
        if d and off + len(d) <= len(self.got):
            self.dup_data += 1
            self.dsack = (off, off + len(d))
        if d:
            if off + len(d) > room:
                d = d[:max(0, room - off)]
            if d and off + len(d) > len(self.got):
                self.held[off] = d
                self.last_off = off
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
        self.arp_asked = []

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
            elif op == 1 and HOST_IP < tip <= HOST_IP + 12:
                self.arp_asked.append(tip)
                mac = bytes([2, 0, 0, 0, 1, tip - HOST_IP])
                arp = struct.pack("!HHBBH", 1, 0x0800, 6, 4, 2) + mac + f[38:42] + f[22:28] + f[28:32]
                self.link.send(f[6:12] + mac + b"\x08\x06" + arp)
            return
        if f[12:14] != b"\x08\x00":
            return
        dst = struct.unpack("!I", f[30:34])[0]
        if f[23] == 1 and f[34] == 8 and HOST_IP < dst <= HOST_IP + 12:
            # An echo request for one of the twelve neighbors: answer as it.
            body = b"\0\0\0\0" + f[38:14 + struct.unpack("!H", f[16:18])[0]]
            body = b"\0\0" + struct.pack("!H", csum(body)) + body[4:]
            mac = bytes([2, 0, 0, 0, 1, dst - HOST_IP])
            self.link.send(f[6:12] + mac + b"\x08\x00" + ip_packet(1, dst, GUEST_IP, body))
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



# ------------------------------------------------------------------ a path

class Path:
    """One direction of a network path for Link.out_rule / Link.in_rule: a
    one-way delay with jitter, random loss, reordering (a frame held back
    `reorder_by` seconds, so the ones sent after it overtake it) and, with `rate` (bytes per second), a
    bottleneck link with a drop-tail queue of `queue` bytes. Only TCP
    segments are touched. `drops` and `sent` count what happened."""
    def __init__(self, delay=0.0, jitter=0.0, loss=0.0, reorder=0.0, reorder_by=0.003, rate=None, queue=60000, seed=1):
        self.delay, self.jitter, self.loss, self.reorder, self.reorder_by = delay, jitter, loss, reorder, reorder_by
        self.rate, self.queue = rate, queue
        self.rng = random.Random(seed)
        self.free_at = 0.0              # when the bottleneck has sent what it holds
        self.sent = self.drops = 0

    def __call__(self, frame):
        if tcp_of(frame) is None:
            return 0
        self.sent += 1
        if self.loss and self.rng.random() < self.loss:
            self.drops += 1
            return None
        now = time.time()
        wait = 0.0
        if self.rate:
            backlog = max(0.0, self.free_at - now)
            if backlog * self.rate > self.queue:
                self.drops += 1
                return None
            self.free_at = max(now, self.free_at) + len(frame) / self.rate
            wait = self.free_at - now
        d = self.delay + wait
        if self.jitter:
            d += self.rng.random() * self.jitter
        if self.reorder and self.rng.random() < self.reorder:
            d += self.reorder_by
        return d


# ------------------------------------------------------------------ a guest

class Guest:
    """A BastiOS guest in QEMU whose card is joined to this script's wire,
    with its serial line in `out`. `boot()` waits for the shell, the DHCP
    lease and (with ash=True) the Linux shell, where `mtcp` runs."""
    def __init__(self, kernel, card="virtio-net-pci", mem="256M"):
        self.link = Link()
        self.net = Net(self.link)
        self.out = b""
        self.qemu = subprocess.Popen(
            ["qemu-system-x86_64", "-kernel", kernel, "-m", mem, "-smp", "2",
             "-display", "none", "-no-reboot", "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
             "-serial", "stdio", "-monitor", "none",
             "-netdev", f"dgram,id=n0,local.type=inet,local.host=127.0.0.1,local.port={self.link.guest_port},"
                        f"remote.type=inet,remote.host=127.0.0.1,remote.port={self.link.port}",
             "-device", f"{card},netdev=n0"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    def pump(self, wait=0.01):
        for f in self.link.poll(wait):
            self.net.handle(f)
        if self.net.peer:
            self.net.peer.tick()
        while True:
            r, _, _ = select.select([self.qemu.stdout], [], [], 0)
            if not r:
                break
            chunk = self.qemu.stdout.read1(65536)
            if not chunk:
                break
            self.out += chunk

    def until(self, cond, secs):
        end = time.time() + secs
        while time.time() < end:
            if cond():
                return True
            self.pump()
        return cond()

    def type_line(self, line):
        self.qemu.stdin.write(line.encode() + b"\r")
        self.qemu.stdin.flush()

    def boot(self, ash=True):
        if not self.until(lambda: b"bastios> " in self.out, 90):
            raise SystemExit("no prompt")
        t0 = time.time()
        leased = False
        while not leased and time.time() - t0 < 15:
            mark = len(self.out)
            self.type_line("ifconfig")
            self.until(lambda: b"bastios> " in self.out[mark:], 10)
            leased = b"10.0.2.15" in self.out[mark:]
            if not leased:
                self.until(lambda: False, 0.5)
        if not leased:
            raise SystemExit("no lease")
        if ash:
            mark = len(self.out)
            self.type_line("ash")
            self.until(lambda: b"# " in self.out[mark:], 20)

    def kill(self):
        self.qemu.kill()
        self.qemu.wait()
