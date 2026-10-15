#!/usr/bin/env bash
# IPv6 (src/net6.bastio) against a network this script plays. QEMU's user
# network cannot be used here: it needs IPv6 on the host. The card is joined
# to this script instead (QEMU's `dgram` netdev, as in tcp_test.sh), which
# is a router (it answers router solicitations with the prefix
# 2001:db8:1::/64), a host on the link at fe80::2 and 2001:db8:1::2 (UDP
# echo on port 7, a small TCP server on port 80), and a host behind the
# router at 2001:db8:99::5 (UDP echo). A Linux program on a disk
# (test/ipv6probe.c) uses AF_INET6 sockets in the guest; the script pings
# the guest and asks for its MAC.
#
#   NIC=name   the QEMU card (default: virtio-net-pci; e1000 also works)
set -u
cd "$(dirname "$0")/.."
KERNEL=${KERNEL:-build/bastios.elf}
NIC=${NIC:-virtio-net-pci}
MUSLCC=${MUSLCC:-musl-gcc}
mkdir -p build/ipv6
$MUSLCC -O2 -fPIE -static-pie -o build/ipv6/ipv6probe test/ipv6probe.c || exit 1
test/compat/mkdisk.py build/ipv6/disk.img build/ipv6/ipv6probe >/dev/null || exit 1

python3 - "$KERNEL" "$NIC" build/ipv6/disk.img <<'PY'
import ipaddress, os, select, socket, struct, subprocess, sys, time

kernel, card, disk = sys.argv[1:4]
GUEST_MAC = bytes.fromhex("525400123456")
HOST_MAC = bytes.fromhex("525500000202")
ROUTER_LL = ipaddress.IPv6Address("fe80::2").packed
ON_LINK = ipaddress.IPv6Address("2001:db8:1::2").packed
OFF_LINK = ipaddress.IPv6Address("2001:db8:99::5").packed
OURS = {ROUTER_LL, ON_LINK, OFF_LINK}
GUEST_LL = ipaddress.IPv6Address("fe80::5054:ff:fe12:3456").packed
GUEST_GL = ipaddress.IPv6Address("2001:db8:1::5054:ff:fe12:3456").packed
ALL_NODES = ipaddress.IPv6Address("ff02::1").packed

def csum(data):
    if len(data) % 2:
        data += b"\0"
    s = sum(struct.unpack(f"!{len(data) // 2}H", data))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return 0xFFFF - s

def ip6(nh, src, dst, payload, hops=64, at=None):
    """An Ethernet frame to the guest carrying payload (its checksum, at
    offset `at`, filled in)."""
    if at is not None:
        p = bytearray(payload)
        p[at:at + 2] = b"\0\0"
        c = csum(src + dst + struct.pack("!IxxxB", len(p), nh) + bytes(p))
        if c == 0 and nh == 17:
            c = 0xFFFF
        p[at:at + 2] = struct.pack("!H", c)
        payload = bytes(p)
    h = struct.pack("!IHBB", 0x60000000, len(payload), nh, hops) + src + dst
    dmac = GUEST_MAC if dst[0] != 0xFF else b"\x33\x33" + dst[12:]
    return dmac + HOST_MAC + b"\x86\xdd" + h + payload

def ok_sum(nh, src, dst, payload):
    return csum(src + dst + struct.pack("!IxxxB", len(payload), nh) + payload) == 0

class Wire:
    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.port = self.sock.getsockname()[1]
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(("127.0.0.1", 0))
        self.guest_port = s.getsockname()[1]
        s.close()
        self.got = []                   # (kind, details) seen from the guest
        self.tcp = {}                   # guest port -> state of a connection to :80
        self.bad = []                   # frames from the guest with a wrong checksum
        self.maxframe = 0               # the longest TCP frame from the guest
        self.syn_mss = None             # the MSS its SYN asks for

    def send(self, f):
        self.sock.sendto(f, ("127.0.0.1", self.guest_port))

    def poll(self, wait):
        r, _, _ = select.select([self.sock], [], [], wait)
        if r:
            f, _ = self.sock.recvfrom(4096)
            self.handle(f)

    def handle(self, f):
        if len(f) < 54 or f[12:14] != b"\x86\xdd":
            return
        plen, nh, hops = struct.unpack("!HBB", f[18:22])
        src, dst, p = f[22:38], f[38:54], f[54:54 + plen]
        if len(p) != plen:
            self.bad.append("short")
            return
        if not ok_sum(nh, src, dst, p):
            self.bad.append(f"checksum nh={nh}")
            return
        if nh == 58:
            self.icmp(f, src, dst, p, hops)
        elif nh == 17 and dst in OURS:
            sport, dport = struct.unpack("!HH", p[:4])
            self.got.append(("udp", dst, f[0:6]))
            if dport == 7:
                u = struct.pack("!HHHH", 7, sport, len(p), 0) + p[8:]
                self.send(ip6(17, dst, src, u, at=6))
        elif nh == 6 and dst == ON_LINK:
            self.maxframe = max(self.maxframe, len(f))
            if p[13] & 2:
                o = p[20:(p[12] >> 4) * 4]
                while len(o) >= 2 and o[0]:
                    if o[0] == 2 and len(o) >= 4:
                        self.syn_mss = struct.unpack("!H", o[2:4])[0]
                    o = o[max(o[1], 2) if o[0] != 1 else 1:]
            self.tcp_in(src, p)

    def icmp(self, f, src, dst, p, hops):
        kind = p[0]
        if kind == 133:                 # router solicitation
            self.got.append(("rs", src, hops))
            self.ra()
        elif kind == 135 and hops == 255:   # neighbor solicitation
            target = p[8:24]
            self.got.append(("ns", target, f[0:6]))
            if target in (ROUTER_LL, ON_LINK):
                na = struct.pack("!BBHI", 136, 0, 0, 0x60000000) + target + bytes([2, 1]) + HOST_MAC
                self.send(ip6(58, target, src, na, hops=255, at=2))
        elif kind == 136:
            self.got.append(("na", p[8:24], p[26:32] if len(p) >= 32 else b"", hops))
        elif kind == 129:
            self.got.append(("echo-reply", src, dst, p[4:]))

    def ra(self):
        ra = struct.pack("!BBHBBHII", 134, 0, 0, 64, 0, 1800, 0, 0)
        ra += bytes([1, 1]) + HOST_MAC
        ra += struct.pack("!BBBBIII", 3, 4, 64, 0xC0, 86400, 14400, 0) + ipaddress.IPv6Address("2001:db8:1::").packed
        self.send(ip6(58, ROUTER_LL, ALL_NODES, ra, hops=255, at=2))

    def tcp_in(self, src, p):
        sport, dport, seq, ack, off, flags, wnd = struct.unpack("!HHIIBBH", p[:16])
        data = p[(off >> 4) * 4:]
        if dport != 80:
            return
        c = self.tcp.get(sport)
        def seg(fl, s, a, body=b"", opts=b""):
            hl = 20 + len(opts)
            t = struct.pack("!HHIIBBHHH", 80, sport, s, a, (hl // 4) << 4, fl, 65535, 0, 0) + opts + body
            self.send(ip6(6, ON_LINK, src, t, at=16))
        if flags & 2:                   # SYN
            c = self.tcp[sport] = dict(rcv=seq + 1, snd=5000, buf=b"", sent=False, closed=False)
            seg(0x12, 5000, seq + 1, opts=struct.pack("!BBH", 2, 4, 1460))
            c["snd"] = 5001
            return
        if not c:
            return
        if data and seq == c["rcv"]:
            c["buf"] += data
            c["rcv"] += len(data)
        if flags & 1 and seq + len(data) == c["rcv"]:
            c["rcv"] += 1
            c["closed"] = True
        if data or flags & 1:
            seg(0x10, c["snd"], c["rcv"])
        if b"\n" in c["buf"] and not c["sent"]:
            c["sent"] = True
            body = bytes(b"0123456789abcdef"[i % 16] for i in range(3000))
            for i in range(0, 3000, 1000):
                seg(0x18, c["snd"] + i, c["rcv"], body[i:i + 1000])
            seg(0x11, c["snd"] + 3000, c["rcv"])        # FIN
            c["snd"] += 3001
            self.got.append(("tcp-served", sport))

# The ENA card is not in QEMU: it needs the build of tools/qemu-ena.
qemu_bin = "qemu-system-x86_64"
if card == "ena":
    qemu_bin = os.environ.get("QEMU_ENA", "build/qemu-ena/bin/qemu-system-x86_64")

wire = Wire()
qemu = subprocess.Popen(
    [qemu_bin, "-kernel", kernel, "-m", "128M", "-smp", "2",
     "-display", "none", "-no-reboot", "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
     "-serial", "stdio", "-monitor", "none",
     "-drive", f"file={disk},format=raw,if=none,id=vd", "-device", "virtio-blk-pci,drive=vd",
     "-netdev", f"dgram,id=n0,local.type=inet,local.host=127.0.0.1,local.port={wire.guest_port},"
                f"remote.type=inet,remote.host=127.0.0.1,remote.port={wire.port}",
     "-device", f"{card},netdev=n0,mac=52:54:00:12:34:56"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
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

def type_line(line):
    qemu.stdin.write(line.encode() + b"\r")
    qemu.stdin.flush()

def check(ok, what):
    global fails
    print(("ok  " if ok else "FAIL"), what, flush=True)
    fails += not ok

check(until(lambda: b"bastios> " in out, 60), f"boots with its card ({card}) on this script's wire")
check(until(lambda: any(g[0] == "rs" for g in wire.got), 10), "sends a router solicitation")
rs = [g for g in wire.got if g[0] == "rs"]
check(rs and rs[0][1] == GUEST_LL and rs[0][2] == 255, "from its link-local address (EUI-64 of its MAC), hop limit 255")

# The script asks for the guest's MAC, then pings both its addresses.
until(lambda: False, 0.5)
ns = struct.pack("!BBHI", 135, 0, 0, 0) + GUEST_GL + bytes([1, 1]) + HOST_MAC
snm = ipaddress.IPv6Address("ff02::1:ff12:3456").packed
wire.send(ip6(58, ROUTER_LL, snm, ns, hops=255, at=2))
check(until(lambda: any(g[0] == "na" and g[1] == GUEST_GL and g[2] == GUEST_MAC and g[3] == 255 for g in wire.got), 5),
      "answers a neighbor solicitation for its global address with its MAC")
for name, src, dst in (("global", ON_LINK, GUEST_GL), ("link-local", ROUTER_LL, GUEST_LL)):
    echo = struct.pack("!BBHHH", 128, 0, 0, 0x6262, 1) + b"bastios ipv6 ping"
    wire.send(ip6(58, src, dst, echo, at=2))
    check(until(lambda: any(g[0] == "echo-reply" and g[1] == dst and g[2] == src and g[3] == b"\x62\x62\x00\x01bastios ipv6 ping"
                            for g in wire.got), 5), f"answers a ping to its {name} address")

mark = len(out)
type_line("ash")
until(lambda: b"# " in out[mark:], 20)
mark = len(out)
type_line("/disk/ipv6probe")
done = until(lambda: b"done " in out[mark:], 90)
text = out[mark:].decode(errors="replace").replace("\r", "")
for line in text.split("\n"):
    if line.startswith("ok ") or line.startswith("FAIL "):
        check(line.startswith("ok "), "guest: " + line.split(" ", 1)[1])
check(done and "done 0" in text, "the guest program finished with every check passing")
if not done:
    print(text[-2000:])
check(any(g[0] == "ns" and g[1] == ON_LINK for g in wire.got), "asked for a neighbor's MAC before sending to it")
off = [g for g in wire.got if g[0] == "udp" and g[1] == OFF_LINK]
check(off and all(g[2] == HOST_MAC for g in off), "sent off-link traffic to the router's MAC")
check(any(g[0] == "tcp-served" for g in wire.got), "TCP reached the server over IPv6")
check(wire.syn_mss is not None and wire.syn_mss <= 1440, f"the SYN asks for an MSS that fits an IPv6 frame ({wire.syn_mss})")
check(0 < wire.maxframe <= 1514, f"no frame past the 1500-byte link MTU although the server offered 1460 ({wire.maxframe} bytes)")
check(not wire.bad, f"every frame from the guest had a right checksum {wire.bad[:3]}")

qemu.kill()
qemu.wait()
print(f"---- {fails} failed ----")
sys.exit(1 if fails else 0)
PY
