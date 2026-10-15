#!/usr/bin/env python3
"""TCP over a network card on BastiOS and on Linux, driven from the host.

    bench/netcmp.py [--os bastios,linux] [--nic virtio-net-pci,e1000] [--runs N]
                    [--kernel build/bastios.elf] [--linux VMLINUZ] [--out FILE]

Each guest runs `mtcp echo PORT` (user/mtcp.c, built for BastiOS by the
Makefile and for Linux here with musl: blocking accept, read and write
through the Linux system calls, so it is the same source on both). QEMU's
user-mode network forwards a host port to it. The host connects and
measures the average round trip of 200 small messages and the rate of
echoing 2 MiB. The path through QEMU's user-mode stack is the same for both
systems, so it caps the rate; compare the systems with each other, and read
the figures as how much each guest's network stack and driver add.
Each figure is the median of RUNS runs. The first system boots, gets its
address (Linux: set by hand; BastiOS: DHCP), and runs every card in turn.
"""
import argparse, json, os, re, select, shutil, socket, statistics, subprocess, sys, tempfile, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import run as R

BULK = 2 << 20


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def echo_run(port):
    s = None
    for _ in range(50):
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=30)
            break
        except OSError:
            time.sleep(0.1)
    if s is None:
        return None
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    msg = b"x" * 32
    t0 = time.monotonic()
    for _ in range(200):
        s.sendall(msg)
        got = b""
        while len(got) < len(msg):
            chunk = s.recv(len(msg) - len(got))
            if not chunk:
                return None
            got += chunk
    rtt = (time.monotonic() - t0) / 200 * 1e6
    data = os.urandom(BULK)
    t0 = time.monotonic()
    sent, back = 0, bytearray()
    s.setblocking(False)
    while len(back) < BULK and time.monotonic() - t0 < 180:
        if sent < BULK:
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
    dt = time.monotonic() - t0
    s.close()
    if bytes(back) != data:
        return None
    return {"rtt_us": rtt, "mib_s": BULK / (1 << 20) / dt}


def run_os(osname, card, args, tmp, mtcp_linux):
    if osname == "linux" and card == "e1000":
        return {"skip": "the Ubuntu kernel builds e1000 as a module"}
    hosts = {g: free_port() for g in range(7100, 7100 + args.runs)}
    fwd = "".join(f",hostfwd=tcp:127.0.0.1:{h}-:{g}" for g, h in hosts.items())
    common = ["-cpu", "qemu64,+pdpe1gb" if args.accel == "tcg" else "host", "-accel", args.accel,
              "-m", args.mem, "-smp", str(args.smp), "-display", "none", "-no-reboot",
              "-serial", "stdio", "-monitor", "none",
              "-netdev", f"user,id=n0{fwd}", "-device", f"{card},netdev=n0"] + args.qemu_extra.split()
    log = os.path.join(R.B, f"net-{osname}-{card}.log")
    if osname == "bastios":
        cmd = ["qemu-system-x86_64", "-kernel", args.kernel,
               "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"] + common
        g = R.Guest(cmd, "# ", log)
        if not g.until(rb"bastios> ", 0, 300):
            raise SystemExit("netcmp: BastiOS gave no prompt")
        time.sleep(4)                       # DHCP
        at = len(g.out)
        g.send("ash")
        g.until(rb"# ", at, 60)
        exe = "mtcp"
    else:
        sub = tempfile.mkdtemp(dir=tmp)
        initrd, _ = R.linux_initrd(mtcp_linux, sub)
        cmd = ["qemu-system-x86_64", "-kernel", R.linux_kernel(args.linux), "-initrd", initrd,
               "-append", "console=ttyS0 quiet panic=-1 mitigations=off transparent_hugepage=never rdinit=/init"] + common
        g = R.Guest(cmd, "# ", log)
        if not g.until(rb"READY", 0, 600):
            raise SystemExit("netcmp: Linux did not reach its shell")
        g.until(rb"# ", len(g.out) - 1, 30)
        g.send("stty -echo 2>/dev/null; PS1='# '")
        time.sleep(1)
        g.send("ip addr add 10.0.2.15/24 dev eth0; ip link set eth0 up; ip route add default via 10.0.2.2")
        time.sleep(2)
        exe = "/osbench"
    res = []
    try:
        for gport, hport in hosts.items():
            at = len(g.out)
            g.send(f"{exe} echo {gport} &")
            if not g.until(rb"listening on tcp port", at, 60):
                raise SystemExit(f"netcmp: {osname}: no listener")
            time.sleep(0.5)
            r = echo_run(hport)
            print(f"{osname:8} {card:15} {r}", file=sys.stderr, flush=True)
            if r:
                res.append(r)
            time.sleep(1)
    finally:
        g.stop()
    if not res:
        return {"error": "no run finished"}
    return {"rtt_us": statistics.median(r["rtt_us"] for r in res),
            "mib_s": statistics.median(r["mib_s"] for r in res),
            "runs": [{k: round(v, 1) for k, v in r.items()} for r in res]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--os", default="bastios,linux")
    ap.add_argument("--nic", default="virtio-net-pci,e1000")
    ap.add_argument("--smp", type=int, default=2)
    ap.add_argument("--mem", default="512M")
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--kernel", default=os.path.join(R.ROOT, "build", "bastios.elf"))
    ap.add_argument("--linux", default=os.environ.get("LINUX_KERNEL", ""))
    ap.add_argument("--accel", default=R.accel())
    ap.add_argument("--qemu-extra", default="", help="more QEMU options, e.g. a -object filter-dump to capture the traffic")
    ap.add_argument("--out", default=os.path.join(R.B, "net.json"))
    args = ap.parse_args()
    os.makedirs(R.B, exist_ok=True)
    mtcp = os.path.join(R.B, "mtcp-linux")
    src = os.path.join(R.ROOT, "user", "mtcp.c")
    if not os.path.exists(mtcp) or os.path.getmtime(mtcp) < os.path.getmtime(src):
        R.sh(os.environ.get("MUSLCC", "musl-gcc"), "-static", "-O2", "-o", mtcp, src)
    results = {"accel": args.accel, "smp": args.smp, "mem": args.mem, "runs": args.runs,
               "host_cpus": os.cpu_count(), "date": time.strftime("%Y-%m-%d %H:%M:%S"), "os": {}}
    with tempfile.TemporaryDirectory() as tmp:
        for osname in args.os.split(","):
            res = results["os"][osname] = {}
            for card in args.nic.split(","):
                res[card] = run_os(osname, card, args, tmp, mtcp)
    with open(args.out, "w") as f:
        json.dump(results, f, indent=1)
    print(table(results))


def table(results):
    oses = list(results["os"])
    lines = [f"QEMU {results['accel'].upper()}, -smp {results['smp']}, user-mode network, "
             f"median of {results['runs']} runs ({results['date']}).", "",
             "| Card | Test | Unit | " + " | ".join(oses) + (" | BastiOS / Linux |" if len(oses) == 2 else " |"),
             "|---|---|---|" + "---:|" * len(oses) + ("---:|" if len(oses) == 2 else "")]
    for card in results["os"][oses[0]]:
        for key, name, unit in (("rtt_us", "round trip, 32 bytes", "us"), ("mib_s", "echo 2 MiB", "MiB/s")):
            vals = [results["os"][o].get(card, {}).get(key) for o in oses]
            skip = ["skip" in results["os"][o].get(card, {}) for o in oses]
            row = f"| {card} | {name} | {unit} | " + " | ".join("n/a" if k else "error" if v is None else f"{v:.1f}" for v, k in zip(vals, skip)) + " |"
            if len(oses) == 2:
                row += f" {vals[0] / vals[1]:.2f}x |" if None not in vals else " |"
            lines.append(row)
    return "\n".join(lines)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--table":
        print(table(json.load(open(sys.argv[2]))))
    else:
        main()
