#!/usr/bin/env bash
# DHCP leases over time (src/net.bastio, dhcp_*) against a server this
# script plays. QEMU's user network hands out a day-long lease and never
# refuses one, so the card is joined to this script instead (QEMU's `dgram`
# netdev, as in ipv6_test.sh). The script gives out short leases (6 s, with
# T1 at 2 s and T2 at 4 s) and checks, from the frames the guest sends and
# whether it answers ARP for an address, that the guest:
#   - takes the address offered and answers for it,
#   - renews at T1 with its address in ciaddr, and keeps it when the server
#     extends the lease,
#   - keeps asking through T2 when nobody answers, gives the address up when
#     the lease ends, and starts over with a DISCOVER,
#   - takes a different address when that is what the server offers now,
#   - starts over at once when the server refuses a renewal (NAK).
#
#   NIC=name   the QEMU card (default: virtio-net-pci; e1000 also works)
set -u
cd "$(dirname "$0")/.."
KERNEL=${KERNEL:-build/bastios.elf}
NIC=${NIC:-virtio-net-pci}

python3 - "$KERNEL" "$NIC" <<'PY'
import select, socket, struct, subprocess, sys, time

kernel, card = sys.argv[1:3]
GUEST_MAC = bytes.fromhex("525400123456")
SERVER_MAC = bytes.fromhex("525500000202")
SERVER = socket.inet_aton("10.9.0.1")
FIRST = socket.inet_aton("10.9.0.15")
SECOND = socket.inet_aton("10.9.0.16")
LEASE, T1, T2 = 6, 2, 4

def csum(data):
    if len(data) % 2:
        data += b"\0"
    s = sum(struct.unpack(f"!{len(data) // 2}H", data))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return 0xFFFF - s

class Wire:
    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.port = self.sock.getsockname()[1]
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(("127.0.0.1", 0))
        self.guest_port = s.getsockname()[1]
        s.close()
        self.dhcp = []                  # (time, type, ciaddr, broadcast flag, requested address, xid)
        self.arp = []                   # (time, address the guest answered for)
        self.answer = None              # called with each DHCP message; returns a reply or None

    def send(self, f):
        self.sock.sendto(f, ("127.0.0.1", self.guest_port))

    def poll(self, wait):
        r, _, _ = select.select([self.sock], [], [], wait)
        if r:
            f, _ = self.sock.recvfrom(4096)
            self.handle(f)

    def handle(self, f):
        if f[12:14] == b"\x08\x06" and len(f) >= 42 and struct.unpack("!H", f[20:22])[0] == 2:
            self.arp.append((time.time(), f[28:32]))
            return
        if f[12:14] != b"\x08\x00" or len(f) < 42 or f[23] != 17:
            return
        if struct.unpack("!H", f[36:38])[0] != 67:
            return
        b = f[42:]
        if len(b) < 240 or b[236:240] != b"\x63\x82\x53\x63":
            return
        xid = b[4:8]
        flags = struct.unpack("!H", b[10:12])[0]
        ciaddr = b[12:16]
        opts, o = {}, 240
        while o < len(b) and b[o] != 255:
            if b[o] == 0:
                o += 1
                continue
            opts[b[o]] = b[o + 2:o + 2 + b[o + 1]]
            o += 2 + b[o + 1]
        kind = opts.get(53, b"\0")[0]
        self.dhcp.append((time.time(), kind, ciaddr, bool(flags & 0x8000), opts.get(50), xid))
        if self.answer:
            r = self.answer(kind, xid, ciaddr, opts)
            if r:
                self.reply(xid, *r)

    def reply(self, xid, kind, yiaddr):
        """A DHCP OFFER (2), ACK (5) or NAK (6) for yiaddr, broadcast."""
        b = struct.pack("!BBBBIHH", 2, 1, 6, 0, 0, 0, 0)[:4] + xid + b"\0" * 8
        b += yiaddr + SERVER + b"\0" * 4 + GUEST_MAC + b"\0" * 10 + b"\0" * 192
        b += b"\x63\x82\x53\x63" + bytes([53, 1, kind, 54, 4]) + SERVER
        if kind != 6:
            b += bytes([1, 4]) + socket.inet_aton("255.255.255.0") + bytes([3, 4]) + SERVER
            b += bytes([6, 4]) + SERVER
            b += bytes([51, 4]) + struct.pack("!I", LEASE)
            b += bytes([58, 4]) + struct.pack("!I", T1) + bytes([59, 4]) + struct.pack("!I", T2)
        b += b"\xff"
        u = struct.pack("!HHHH", 67, 68, 8 + len(b), 0) + b
        ip = struct.pack("!BBHHHBBH", 0x45, 0, 20 + len(u), 0, 0, 64, 17, 0) + SERVER + b"\xff" * 4
        ip = ip[:10] + struct.pack("!H", csum(ip)) + ip[12:]
        self.send(b"\xff" * 6 + SERVER_MAC + b"\x08\x00" + ip + u)

    def who_has(self, addr):
        """An ARP request for addr, from the server."""
        a = struct.pack("!HHBBH", 1, 0x0800, 6, 4, 1) + SERVER_MAC + SERVER + b"\0" * 6 + addr
        self.send(b"\xff" * 6 + SERVER_MAC + b"\x08\x06" + a)

wire = Wire()
qemu = subprocess.Popen(
    ["qemu-system-x86_64", "-kernel", kernel, "-m", "128M", "-smp", "2",
     "-display", "none", "-no-reboot", "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
     "-serial", "stdio", "-monitor", "none",
     "-netdev", f"dgram,id=n0,local.type=inet,local.host=127.0.0.1,local.port={wire.guest_port},"
                f"remote.type=inet,remote.host=127.0.0.1,remote.port={wire.port}",
     "-device", f"{card},netdev=n0,mac=52:54:00:12:34:56"],
    stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
out = b""
fails = 0

def pump(wait=0.02):
    global out
    wire.poll(wait)
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

def check(ok, what):
    global fails
    print(("ok  " if ok else "FAIL"), what, flush=True)
    fails += not ok

def answers_for(addr, secs=2):
    """Whether the guest answers an ARP request for addr."""
    mark = len(wire.arp)
    wire.who_has(addr)
    return until(lambda: any(a == addr for _, a in wire.arp[mark:]), secs)

def since(mark, kind):
    return [d for d in wire.dhcp[mark:] if d[1] == kind]

# 1. The first lease: DISCOVER -> OFFER -> REQUEST -> ACK.
def first(kind, xid, ciaddr, opts):
    if kind == 1:
        return (2, FIRST)
    if kind == 3 and opts.get(50) == FIRST:
        return (5, FIRST)
wire.answer = first
check(until(lambda: since(0, 1), 60), f"sends a DISCOVER at boot (card {card})")
d = since(0, 1)
check(d and d[0][3] and d[0][2] == b"\0" * 4, "the DISCOVER asks for a broadcast answer and names no address")
check(until(lambda: since(0, 3), 10), "answers the OFFER with a REQUEST")
r = since(0, 3)
check(r and r[0][4] == FIRST and r[0][2] == b"\0" * 4, "the REQUEST names the address offered in option 50, not in ciaddr")
until(lambda: False, 0.5)
bound = time.time()
check(answers_for(FIRST), "answers ARP for the address leased")

# 2. Renewal at T1: a REQUEST with ciaddr set; the ACK extends the lease.
mark = len(wire.dhcp)
renewed = []
def renew(kind, xid, ciaddr, opts):
    if kind == 3 and ciaddr == FIRST and opts.get(50) is None:
        renewed.append(time.time())
        return (5, FIRST)
wire.answer = renew
check(until(lambda: renewed, 10), "renews the lease: a REQUEST with its address in ciaddr")
if renewed:
    t = renewed[0] - bound
    check(T1 - 1 <= t <= T1 + 2.5, f"at T1 ({t:.1f} s after the lease, T1 is {T1} s)")
check(not since(mark, 1), "no DISCOVER while the lease is being renewed")
until(lambda: False, LEASE - T1 + 1)       # past where the first lease would have ended
check(answers_for(FIRST), "still has the address after the lease would have ended: the renewal extended it")

# 3. The server goes quiet: renewing and rebinding go unanswered, the
# lease ends, the address goes, and the search starts over.
mark = len(wire.dhcp)
last = renewed[-1] if renewed else time.time()
wire.answer = None
check(until(lambda: since(mark, 1), LEASE + 6), "starts over with a DISCOVER when no renewal is answered")
asked = since(mark, 3)
gone = since(mark, 1)
if gone:
    t = gone[0][0] - last
    check(LEASE - 1 <= t <= LEASE + 3, f"when the lease ends ({t:.1f} s after the last ACK, the lease is {LEASE} s)")
    late = [a for a in asked if a[0] - last >= T2]
    check(len(asked) >= 3 and late, f"kept asking until then, after T2 too ({len(asked)} REQUESTs, {len(late)} after T2)")
    check(all(a[2] == FIRST for a in asked), "every REQUEST while renewing or rebinding carried its address in ciaddr")
check(not answers_for(FIRST, 1), "no longer answers ARP for the address that ran out")

# 4. A new lease, for another address.
mark = len(wire.dhcp)
def second(kind, xid, ciaddr, opts):
    if kind == 1:
        return (2, SECOND)
    if kind == 3 and opts.get(50) == SECOND:
        return (5, SECOND)
wire.answer = second
check(until(lambda: since(mark, 3) and since(mark, 3)[-1][4] == SECOND, 10), "takes the address now offered")
until(lambda: False, 0.5)
check(answers_for(SECOND), "answers ARP for the new address")

# 5. A NAK to the renewal: the address goes at once and the search starts over.
mark = len(wire.dhcp)
naked = []
def nak(kind, xid, ciaddr, opts):
    if kind == 3 and ciaddr == SECOND:
        naked.append(time.time())
        return (6, b"\0" * 4)
wire.answer = nak
check(until(lambda: naked, 10), "renews the new lease too")
check(until(lambda: since(mark, 1), 3), "a NAK sends it back to DISCOVER")
if naked and since(mark, 1):
    t = since(mark, 1)[0][0] - naked[0]
    check(t < 1.5, f"at once ({t:.1f} s after the NAK)")
wire.answer = None
check(not answers_for(SECOND, 1), "no longer answers ARP for the refused address")

qemu.kill()
qemu.wait()
print(f"---- {fails} failed ----")
sys.exit(1 if fails else 0)
PY
