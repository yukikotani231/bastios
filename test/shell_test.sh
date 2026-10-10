#!/usr/bin/env bash
# Boot BastiOS in QEMU, drive the shell over the serial port, and check its replies.
# Input is sent one line at a time, each after the prompt appears: bytes sent before
# the kernel initialises the UART are dropped.
set -u
cd "$(dirname "$0")/.."
KERNEL=${KERNEL:-build/bastios.elf}
TIMEOUT=${TIMEOUT:-60}
SMP=${SMP:-4}

python3 - "$KERNEL" "$TIMEOUT" "$SMP" <<'PY'
import os, re, socket, subprocess, sys, tempfile, time, select

kernel, timeout, smp = sys.argv[1], float(sys.argv[2]), int(sys.argv[3])

# A blank disk for this run (sparse, so it costs nothing): BastiOS formats it
# on the first boot that sees it, and a later boot must find the files.
disk_dir = tempfile.TemporaryDirectory()
disk = os.path.join(disk_dir.name, "disk.img")
with open(disk, "wb") as f:
    f.truncate(33 << 20)

# A free UDP port on this machine, forwarded to the guest's port 7 (UDP echo).
_s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
_s.bind(("127.0.0.1", 0))
udp_port = _s.getsockname()[1]
_s.close()
# Another, forwarded to guest port 7000, where `udpecho 7000` listens on a
# socket; and a port on this machine for `udpsend` to send to.
_s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
_s.bind(("127.0.0.1", 0))
udp_port2 = _s.getsockname()[1]
_s.close()
# And a TCP port forwarded to guest port 7001, where `tcpecho 7001` listens.
_s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
_s.bind(("127.0.0.1", 0))
tcp_port = _s.getsockname()[1]
_s.close()

def boot(cpus, with_disk=False, virtio=False, net=False, subnet=None):
    drive = ["-drive", f"file={disk},format=raw,if=ide,index=0"] if with_disk else []
    if virtio:
        # The same disk as a virtio block device (src/virtio.bastio): sectors
        # move by DMA through pages the kernel lends it.
        drive = ["-drive", f"file={disk},format=raw,if=none,id=vd",
                 "-device", "virtio-blk-pci,drive=vd"]
    # A virtio network card on QEMU's user-mode network (src/net.bastio).
    nic = ["-netdev", f"user,id=n0,hostfwd=udp:127.0.0.1:{udp_port}-:7,hostfwd=udp:127.0.0.1:{udp_port2}-:7000,hostfwd=tcp:127.0.0.1:{tcp_port}-:7001",
           "-device", "virtio-net-pci,netdev=n0"] if net else []
    if subnet:
        # QEMU's DHCP server on a subnet other than its default: the
        # address the kernel ends up with can only have come from DHCP.
        nic = ["-netdev", f"user,id=n0,net={subnet}",
               "-device", "virtio-net-pci,netdev=n0"]
    return subprocess.Popen(
        ["qemu-system-x86_64", "-kernel", kernel, "-m", "64M", "-smp", str(cpus),
         "-display", "none", "-no-reboot", "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
         "-serial", "stdio", "-monitor", "none"] + drive + nic,
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

deadline = time.time() + timeout
out = b""
fails = 0

# A single-processor machine first: no application processors to start, and
# the kernel must say so and still reach the prompt.
qemu = boot(1)

def read_until(marker, start=0):
    global out
    while marker not in out[start:]:
        left = deadline - time.time()
        if left <= 0:
            return False
        r, _, _ = select.select([qemu.stdout], [], [], left)
        if not r:
            return False
        chunk = qemu.stdout.read1(4096)
        if not chunk:
            return False
        out += chunk
    return True

if read_until(b"bastios> ") and b"pages of memory, 1 processor\r\n" in out:
    print("ok   boots on one processor")
else:
    print("FAIL boot on one processor"); fails += 1
# This machine has no disk: names under disk/ cannot be created, and nothing
# else breaks.
mark = len(out)
qemu.stdin.write(b"echo x > disk/a\r"); qemu.stdin.flush()
if read_until(b"bastios> ", mark) and b"cannot open the file" in out[mark:]:
    print("ok   no disk: disk/ files are refused")
else:
    print("FAIL no disk: disk/a"); fails += 1
# Nor a network card: ping says so.
mark = len(out)
qemu.stdin.write(b"ping\r"); qemu.stdin.flush()
if read_until(b"bastios> ", mark) and b"no network" in out[mark:]:
    print("ok   no network card: ping says so")
else:
    print("FAIL no network card: ping"); fails += 1
mark = len(out)
qemu.stdin.write(b"udpsend 10.0.2.2 9 x\r"); qemu.stdin.flush()
if read_until(b"bastios> ", mark) and b"no network" in out[mark:]:
    print("ok   no network card: no sockets either")
else:
    print("FAIL no network card: udpsend"); fails += 1
qemu.kill(); qemu.wait()
print("---- one-processor transcript ----")
print(out.decode(errors="replace").replace("\r", ""))

# The rest runs on `smp` processors: every processor runs the scheduler, so
# the shell and its programs run on whichever one picks them.
qemu = boot(smp, with_disk=True, net=True)
out = b""

# (input line, expected text in the reply). The shell is a user process;
# the later steps exercise process creation, waiting, preemption, killing
# and fault isolation.
steps = [
    ("help", b"commands:"),
    # The network: ICMP echo to QEMU's gateway and to its DNS address, both
    # answered by QEMU itself; a malformed address is refused.
    ("ifconfig", b"10.0.2.15 netmask 255.255.255.0 gateway 10.0.2.2 (DHCP)"),
    ("ping", b"reply from 10.0.2.2"),
    ("ping 10.0.2.3", b"reply from 10.0.2.3"),
    ("ping 10.0.2.300", b"usage: ping"),
    # Nothing answers ARP for this address: the shell waits off the kernel
    # lock (its process is in NETWAIT) until the timer ends the ping.
    ("ping 10.0.2.99", b"no answer to ARP for 10.0.2.99"),
    ("cpus", f"processors running: {smp}\r\n".encode()),
    ("about", b"proved safe"),
    ("echo hello world", b"hello world"),
    ("add 2 40", b"42"),
    ("mul 3 7", b"21"),
    ("mul 4294967296 4294967296", b"overflow"),
    ("add 99999999999999999999 1", b"too large"),
    ("frobnicate", b"unknown command: frobnicate"),
    ("mem", b"free pages"),
    ("hello", b"hello from user mode, pid 2"),
    ("ps", b"    1     0 ready"),
    ("spin &", b"[2]"),
    # spin never makes a system call: only the timer can give the shell
    # the CPU back, so getting a prompt at all shows preemption.
    ("echo still here", b"still here"),
    ("ps", b"spin"),
    ("kill 2", b""),
    ("kill 2", b"no such process"),
    ("fault", b"page fault at rip"),
    ("echo survived", b"survived"),
    # User buffers that cross a page boundary, and a write cut short by an
    # unmapped page (it returns 4, the exit code).
    ("straddle", [b"page-crossing write ok", b"hello from user mode", b"END![exit 4]"]),
    ("bench", b"ticks"),
    # Static Linux ELF executables: one linked at a fixed address, one a
    # static PIE. Each checks its .data, .bss and initial stack (argv, envp,
    # auxv) and says ok. A truncated ELF must be refused, not loaded.
    ("elfexec", b"elfexec: ok"),
    ("elfpie", b"elfpie: ok"),
    ("elfbad", b"unknown command: elfbad"),
    # A C program against musl libc: TLS, printf, exit status.
    ("mhello", [b"hello from musl: mhello, argc 1, 6", b"[exit 3]"]),
    # Two processes using xmm0 at once, preempting each other: each must
    # keep its own value (checked again at the end, once both are done).
    ("fpucheck &", b"]"),
    ("fpucheck", b"fpucheck: xmm0 kept"),
    # musl malloc (brk, mmap), stdio on a file, and a line read from the
    # console; the third field is what to wait for instead of the prompt.
    ("mtest", [b"mtest: malloc heap 75", b"mtest: big memory 4096 1024", b"mtest: file line 42"], b"type a line"),
    ("typed for mtest", b"mtest: got typed for mtest"),
    # BusyBox's shell, unmodified, reading commands from the console. The
    # console is a terminal (ioctl TCGETS works), so ash is interactive and
    # prompts with "# "; the kernel echoes what is typed and handles
    # Backspace (DEL) itself, as a Linux tty in canonical mode does.
    ("ash", b"built-in shell (ash)", b"\n# "),
    ("echo $((6 * 7)) from ash", b"42 from ash", b"\n# "),
    ("for i in 1 2 3; do echo n$i; done; test -z \"\" && echo empty", [b"n1\r\nn2\r\nn3\r\nempty"], b"\n# "),
    ("echo abX\x7fc", [b"echo abX\x08 \x08c", b"\nabc\r\n"], b"\n# "),
    # fork, execve and wait4: a subshell, programs found on PATH as
    # /bin/NAME (stat), with their arguments and exit status, and a
    # nested ash whose exit status the outer one sees.
    ("(echo in subshell; exit 4); echo sub=$?", [b"in subshell", b"sub=4"], b"\n# "),
    ("mhello a b; echo st=$?", [b"hello from musl: mhello, argc 3, 6", b"st=3"], b"\n# "),
    ("hello", b"hello from user mode, pid ", b"\n# "),
    ("nothere", b"nothere: not found", b"\n# "),
    ("ash", b"built-in shell (ash)", b"\n# "),
    ("exit 7", b"", b"\n# "),
    ("echo outer got $?", b"outer got 7", b"\n# "),
    # Pipes, redirection and dup2 (built-ins and programs on both sides),
    # the environment passed by execve, and argv/envp checked by elfpie.
    ("echo piped | (read x; echo got=$x)", b"got=piped", b"\n# "),
    ("mhello p | (read l; echo [$l])", b"[hello from musl: mhello, argc 2, 6]", b"\n# "),
    ("mhello > lxout; read y < lxout; echo y=$y", b"y=hello from musl: mhello, argc 1, 6", b"\n# "),
    ("export GREETING=hey; ash", b"built-in shell (ash)", b"\n# "),
    ("echo env $GREETING", b"env hey", b"\n# "),
    ("exit", b"", b"\n# "),
    ("elfpie a b", b"elfpie: ok", b"\n# "),
    # execve of a file: elfdemo is the elfpie program installed at boot.
    ("/elfdemo a b", b"/elfdemo: ok", b"\n# "),
    ("./elfdemo; echo st=$?", [b"./elfdemo: ok", b"st=0"], b"\n# "),
    # BusyBox applets, run as /bin/NAME: getdents64, fstat, stat.
    ("ls", [b"elfdemo", b"motd", b"bin"], b"\n# "),
    ("ls /bin", [b"mhello", b"cat"], b"\n# "),
    ("cat /motd", b"Welcome to BastiOS", b"\n# "),
    ("echo one > t; echo two >> t; cat t | wc -l", b"\r\n2\r\n", b"\n# "),
    ("cp t t2; head -n 1 t2; rm t t2; ls", [b"\r\none\r\n", b"elfdemo"], b"\n# "),
    # Signals: a trap handler entered and returned from (rt_sigreturn),
    # a process ending itself with kill, a writer ended by SIGPIPE once
    # head has gone, and Ctrl-C ending a busy loop (the parent ash's own
    # handler sees it too and carries on).
    ("trap 'echo got$((1+1))' USR1; kill -USR1 $$; echo after$((2+2))", [b"\r\ngot2\r\n", b"\r\nafter4\r\n"], b"\n# "),
    ("ash -c 'kill $$'; echo k=$?", b"k=143", b"\n# "),
    ("ash -c 'while :; do echo y; done' | head -n 1; echo piped", b"\r\ny\r\npiped", b"\n# "),
    ("ash -c 'while :; do :; done'", b"^C", b"\n# ", b"\x03"),
    ("echo st=$?", b"st=130", b"\n# "),
    ("exit 5", b"[exit 5]"),
    ("rm lxout", b""),
    ("sh", b"BastiOS 0.2 -- type"),
    ("exit", b""),
    ("mem", b"free pages"),
    # Files. motd and about are built into the kernel and installed at boot.
    ("ls", [b"motd", b"about"]),
    ("cat motd", b"Welcome to BastiOS"),
    ("cat about", b"exactly\r\nonce"),
    ("cat nothere", b"no such file: nothere"),
    ("echo first line > notes", b""),
    ("echo second line >> notes", b""),
    ("cat notes", b"first line\r\nsecond line\r\n"),
    ("ls", b"      23 notes"),
    ("echo replaced>notes", b""),
    ("cat notes", b"replaced\r\n"),
    ("rm notes", b""),
    ("cat notes", b"no such file: notes"),
    ("rm notes", b"cannot remove notes"),
    # Several pages, seeking, appending, holes, descriptor limits, first on
    # the disk and then in memory; it exits with "big" still open, so
    # removing it shows that exit closed it.
    ("fstest", [b"fstest: disk ok", b"fstest: ok"]),
    ("rm big", b""),
    ("rm disk/big", b""),
    ("ls", b"about"),
    # A file on the disk; the next boot must still have it.
    ("echo kept across boots > disk/note", b""),
    ("ls", b"      18 disk/note"),
    ("cat disk/note", b"kept across boots\r\n"),
    # Every page the files used is back once they are removed.
    ("mem", b"free pages"),
]
if not read_until(b"bastios> "):
    print("FAIL: no prompt after boot"); fails += 1
else:
    print("ok   boot banner and prompt")
    for step in steps:
        line, want = step[0], step[1]
        until = step[2] if len(step) > 2 else b"bastios> "
        mark = len(out)
        qemu.stdin.write(line.encode() + b"\r"); qemu.stdin.flush()
        if len(step) > 3:
            time.sleep(1)                   # then keys typed while it runs
            qemu.stdin.write(step[3]); qemu.stdin.flush()
        wants = want if isinstance(want, list) else [want]
        ok = read_until(until, mark) and all(w in out[mark:] for w in wants)
        print(("ok  " if ok else "FAIL"), repr(line)); ok or print(repr(out[mark:]))
        fails += not ok
    if out.count(b"fpucheck: xmm0 kept") == 2 and b"xmm0 changed" not in out:
        print("ok   both fpucheck processes kept xmm0")
    else:
        print("FAIL fpucheck: SSE state leaked between processes"); fails += 1
    # The timer (src/pit.bastio) runs at 100 Hz and every tick is
    # acknowledged at the interrupt controller (src/pic.bastio): ticks keep
    # coming, about 100 a second. The window is wide because QEMU's clock
    # under load is not exact.
    def uptime():
        mark = len(out)
        qemu.stdin.write(b"uptime\r"); qemu.stdin.flush()
        if not read_until(b"bastios> ", mark):
            return None
        m = re.search(rb"(\d+) ticks, \d+ s", out[mark:])
        return int(m.group(1)) if m else None
    # The HPET (src/hpet.bastio, memory-mapped registers claimed through
    # Mmio) counts nanoseconds: over the same wall-clock interval it must
    # advance by about as much.
    def clock():
        mark = len(out)
        qemu.stdin.write(b"clock\r"); qemu.stdin.flush()
        if not read_until(b"bastios> ", mark):
            return None
        m = re.search(rb"(\d+) ns since boot", out[mark:])
        return int(m.group(1)) if m else None
    t0, c0, w0 = uptime(), clock(), time.time()
    time.sleep(2)
    t1, c1, w1 = uptime(), clock(), time.time()
    if c0 and c1 and 0.6e9 * (w1 - w0) <= c1 - c0 <= 1.5e9 * (w1 - w0):
        print(f"ok   HPET clock: {(c1 - c0) / 1e9:.2f} s in {w1 - w0:.1f} s")
    else:
        print("FAIL HPET clock:", c0, c1); fails += 1
    if t0 is not None and t1 is not None and 60 * (w1 - w0) <= t1 - t0 <= 200 * (w1 - w0):
        print(f"ok   timer: {t1 - t0} ticks in {w1 - w0:.1f} s")
    else:
        print("FAIL timer ticks:", t0, t1); fails += 1
    # UDP echo: a datagram from this machine reaches the guest through
    # QEMU's port forward (ARP, IPv4, UDP on the way in) and comes back.
    if b"net: virtio, mac 52:54:00:12:34:56" in out:
        print("ok   network card found at boot")
    else:
        print("FAIL network card at boot"); fails += 1
    us = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    us.settimeout(10)
    try:
        us.sendto(b"proved packets", ("127.0.0.1", udp_port))
        got, _ = us.recvfrom(100)
    except OSError:
        got = None
    us.close()
    if got == b"proved packets":
        print("ok   UDP echo on port 7")
    else:
        print("FAIL UDP echo:", got); fails += 1
    # UDP sockets (the system calls in src/syscall.bastio): the shell binds
    # port 7000, gets a datagram from this machine and sends it back.
    mark = len(out)
    qemu.stdin.write(b"udpecho 7000\r"); qemu.stdin.flush()
    got = None
    if read_until(b"listening on udp port 7000", mark):
        us = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        us.settimeout(10)
        try:
            us.sendto(b"hello socket", ("127.0.0.1", udp_port2))
            got, _ = us.recvfrom(100)
        except OSError:
            pass
        us.close()
    if (read_until(b"bastios> ", mark) and got == b"hello socket"
            and re.search(rb"from 10\.0\.2\.2:\d+: hello socket", out[mark:])
            and b"sent it back" in out[mark:]):
        print("ok   UDP socket: bind, recvfrom, sendto back")
    else:
        print("FAIL UDP socket echo:", got); fails += 1
    # And one sent from an ephemeral port to this machine (QEMU's 10.0.2.2
    # is the host's loopback), after ARP for the gateway.
    us = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    us.bind(("127.0.0.1", 0))
    us.settimeout(10)
    mark = len(out)
    qemu.stdin.write(f"udpsend 10.0.2.2 {us.getsockname()[1]} sent by sendto\r".encode()); qemu.stdin.flush()
    try:
        got, _ = us.recvfrom(100)
    except OSError:
        got = None
    us.close()
    if read_until(b"bastios> ", mark) and got == b"sent by sendto" and b"sent 14 bytes" in out[mark:]:
        print("ok   UDP socket: sendto from an ephemeral port")
    else:
        print("FAIL UDP sendto:", got); fails += 1
    # Three datagrams sent while the shell sleeps queue on the socket and
    # come out in order.
    mark = len(out)
    qemu.stdin.write(b"udpecho 7000 3\r"); qemu.stdin.flush()
    got = []
    if read_until(b"listening on udp port 7000", mark):
        us = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        us.settimeout(10)
        try:
            for w in (b"one", b"two", b"three"):
                us.sendto(w, ("127.0.0.1", udp_port2))
            for _ in range(3):
                got.append(us.recvfrom(100)[0])
        except OSError:
            pass
        us.close()
    if (read_until(b"bastios> ", mark) and got == [b"one", b"two", b"three"]
            and re.search(rb": one\r\n.*: two\r\n.*: three\r\n", out[mark:], re.S)):
        print("ok   UDP socket: three datagrams queue and come out in order")
    else:
        print("FAIL UDP socket queue:", got); fails += 1
    # The same through Linux system calls: a musl program (user/mudp.c)
    # binds port 7000, blocks in recvfrom, sends the datagram back with
    # sendto, then connects and writes "bye".
    mark = len(out)
    qemu.stdin.write(b"mudp 7000\r"); qemu.stdin.flush()
    got = []
    if read_until(b"listening on udp port 7000", mark):
        us = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        us.settimeout(10)
        try:
            us.sendto(b"hello linux", ("127.0.0.1", udp_port2))
            got.append(us.recvfrom(100)[0])
            got.append(us.recvfrom(100)[0])
        except OSError:
            pass
        us.close()
    if (read_until(b"bastios> ", mark) and got == [b"hello linux", b"bye"]
            and b"mudp: 11 bytes from 10.0.2.2: hello linux" in out[mark:]
            and b"mudp: sent it back" in out[mark:]):
        print("ok   Linux UDP socket: bind, recvfrom, sendto, connect, write")
    else:
        print("FAIL Linux UDP socket:", got, repr(out[mark:])); fails += 1
    # TCP, accepting: 20000 bytes through a 4 KiB window and back, then the
    # host closes its side and the guest's echo loop sees the end.
    mark = len(out)
    qemu.stdin.write(b"tcpecho 7001\r"); qemu.stdin.flush()
    sent = bytes((i * 7 + i // 251) % 256 for i in range(20000))
    back = b""
    if read_until(b"listening on tcp port 7001", mark):
        # The echo is read slowly through a small buffer on another thread,
        # so QEMU acknowledges the guest's data a piece at a time.
        import threading
        got_back = []
        def slow_reader(sock):
            try:
                while True:
                    chunk = sock.recv(700)
                    if not chunk:
                        break
                    got_back.append(chunk)
                    time.sleep(0.002)
            except OSError as e:
                print("     tcp echo read:", e)
        try:
            ts = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            ts.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2048)
            ts.settimeout(20)
            ts.connect(("127.0.0.1", tcp_port))
            rd = threading.Thread(target=slow_reader, args=(ts,))
            rd.start()
            ts.sendall(sent)
            ts.shutdown(socket.SHUT_WR)
            rd.join(30)
            ts.close()
            back = b"".join(got_back)
        except OSError as e:
            print("     tcp echo:", e)
    if read_until(b"bastios> ", mark) and back == sent and b"echoed 20000 bytes" in out[mark:]:
        print("ok   TCP: accept, 20000 bytes echoed in order, close")
    else:
        print("FAIL TCP echo:", len(back), back == sent[:len(back)]); fails += 1
    # TCP, connecting: to a server on this machine (10.0.2.2), which reads
    # a line, answers and closes.
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    srv.settimeout(10)
    mark = len(out)
    qemu.stdin.write(f"tcpget 10.0.2.2 {srv.getsockname()[1]} hello over tcp\r".encode()); qemu.stdin.flush()
    line = b""
    try:
        cs, _ = srv.accept()
        cs.settimeout(10)
        while not line.endswith(b"\n"):
            chunk = cs.recv(100)
            if not chunk:
                break
            line += chunk
        cs.sendall(b"pong from the host\n")
        cs.close()
    except OSError as e:
        print("     tcp get:", e)
    srv.close()
    if (read_until(b"bastios> ", mark) and line == b"hello over tcp\n"
            and b"pong from the host" in out[mark:] and b"closed" in out[mark:]):
        print("ok   TCP: connect, send, receive until the peer closes")
    else:
        print("FAIL TCP connect:", line); fails += 1
    # A closed port on the host: QEMU resets the connection.
    mark = len(out)
    qemu.stdin.write(f"tcpget 10.0.2.2 {udp_port} x\r".encode()); qemu.stdin.flush()
    if read_until(b"bastios> ", mark) and b"connection refused" in out[mark:]:
        print("ok   TCP: a refused connection says so")
    else:
        print("FAIL TCP refused"); fails += 1
    # TCP through Linux system calls (user/mtcp.c, musl): it listens,
    # accepts, reads a line naming a port on this machine and answers it;
    # then it connects to that port, writes, and reads the reply until the
    # host closes.
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    srv.settimeout(10)
    ask = f"{srv.getsockname()[1]}\n".encode()
    mark = len(out)
    qemu.stdin.write(b"mtcp\r"); qemu.stdin.flush()
    reply = b""
    line = b""
    if read_until(b"listening on tcp port 7001", mark):
        try:
            ts = socket.create_connection(("127.0.0.1", tcp_port), timeout=10)
            ts.sendall(ask)
            while True:
                chunk = ts.recv(100)
                if not chunk:
                    break
                reply += chunk
            ts.close()
            cs, _ = srv.accept()
            cs.settimeout(10)
            while not line.endswith(b"\n"):
                chunk = cs.recv(100)
                if not chunk:
                    break
                line += chunk
            cs.sendall(b"back at you\n")
            cs.close()
        except OSError as e:
            print("     mtcp:", e)
    srv.close()
    if (read_until(b"bastios> ", mark) and reply == b"echo: " + ask
            and b"mtcp: connection from 10.0.2.2" in out[mark:]):
        print("ok   Linux TCP: listen, accept, read, write, close")
    else:
        print("FAIL Linux TCP serve:", reply, repr(out[mark:])); fails += 1
    if line == b"linux says hi\n" and b"mtcp: got back at you" in out[mark:]:
        print("ok   Linux TCP: connect, write, read until the peer closes")
    else:
        print("FAIL Linux TCP get:", line); fails += 1
    mark = len(out)
    qemu.stdin.write(b"udpsend 10.0.2.2 0 x\r"); qemu.stdin.flush()
    if read_until(b"bastios> ", mark) and b"usage: udpsend" in out[mark:]:
        print("ok   udpsend refuses port 0")
    else:
        print("FAIL udpsend port 0"); fails += 1
    # Every page the finished processes and removed files used must be
    # back in the pool.
    mem = [l.strip() for l in out.split(b"\n") if l.strip().endswith(b"free pages")]
    if len(mem) == 3 and mem[0] == mem[1] == mem[2]:
        print("ok   no pages leaked:", mem[2].decode())
    else:
        print("FAIL page leak:", mem); fails += 1
    # Several processors at once: processes that never make a system call
    # run on the other processors while the shell stays responsive, and
    # `ps` (which runs with the kernel's lock held) catches at least two of
    # them (on four processors, usually all three) on a processor at the
    # same moment. One is left running for
    # `halt`, which must wait for its processor to bring it back.
    if smp >= 4:
        pids = []
        for _ in range(3):
            mark = len(out)
            qemu.stdin.write(b"spin &\r"); qemu.stdin.flush()
            read_until(b"bastios> ", mark)
            m = re.search(rb"\[(\d+)\]", out[mark:])
            pids.append(m.group(1).decode() if m else "0")
        time.sleep(0.5)
        # A process caught between two runs (back under the lock, about to
        # be picked again) shows as ready, so look a few times.
        running = 0
        for _ in range(5):
            mark = len(out)
            qemu.stdin.write(b"ps\r"); qemu.stdin.flush()
            read_until(b"bastios> ", mark)
            running = max(running, len(re.findall(rb"running +\d+ spin", out[mark:])))
            if running == 3:
                break
        if running >= 2:
            print(f"ok   {running} processes running on other processors at once")
        else:
            print(f"FAIL parallel: {running} spin processes running"); fails += 1
        for p in pids[:2]:
            mark = len(out)
            qemu.stdin.write(f"kill {p}\r".encode()); qemu.stdin.flush()
            ok = read_until(b"bastios> ", mark) and b"no such" not in out[mark:]
            print(("ok  " if ok else "FAIL"), f"kill {p} (running on another processor)")
            fails += not ok
    qemu.stdin.write(b"halt\r"); qemu.stdin.flush()
    read_until(b"pages free at power off")
booted = re.search(rb"BastiOS 0.2: (\d+) pages of memory", out)
off = re.search(rb"BastiOS: (\d+) pages free at power off", out)
if booted and off and booted.group(1) == off.group(1):
    print(f"ok   all {off.group(1).decode()} pages back in the pool at power off")
else:
    print("FAIL pages at power off:", booted and booted.group(1), off and off.group(1)); fails += 1
try:
    code = qemu.wait(timeout=max(1, deadline - time.time()))
except subprocess.TimeoutExpired:
    qemu.kill(); code = None
# isa-debug-exit: the kernel writes 0x10, QEMU exits with (0x10 << 1) | 1 = 33.
if code == 33:
    print("ok   halt exits QEMU with status 33")
else:
    print(f"FAIL halt: QEMU exit status {code}"); fails += 1
print("---- serial transcript ----")
print(out.decode(errors="replace").replace("\r", ""))

# Boot again on the same disk, now as a virtio device: the file written
# through ATA is still there, and virtio can read, extend and keep it. At
# power off the four pages the device was lent are back in the pool.
def session(name, want_banner, steps, halt=False):
    global qemu, out, deadline, fails
    out = b""
    deadline = time.time() + timeout
    if not read_until(b"bastios> "):
        print(f"FAIL: no prompt after the {name} boot"); fails += 1
    else:
        if want_banner is not None:
            ok = (b"disk: virtio" in out) == want_banner
            print(("ok  " if ok else "FAIL"), f"({name} boot) disk driver: {'virtio' if want_banner else 'ATA'}")
            fails += not ok
        for line, want in steps:
            mark = len(out)
            qemu.stdin.write(line.encode() + b"\r"); qemu.stdin.flush()
            ok = read_until(b"bastios> ", mark) and want in out[mark:]
            print(("ok  " if ok else "FAIL"), f"({name} boot)", repr(line))
            fails += not ok
        if halt:
            qemu.stdin.write(b"halt\r"); qemu.stdin.flush()
            read_until(b"pages free at power off")
            booted = re.search(rb"BastiOS 0.2: (\d+) pages of memory", out)
            off = re.search(rb"BastiOS: (\d+) pages free at power off", out)
            ok = booted and off and booted.group(1) == off.group(1)
            print(("ok  " if ok else "FAIL"), f"({name} boot) every page back at power off")
            fails += not ok
    qemu.kill(); qemu.wait()
    print(f"---- {name} boot transcript ----")
    print(out.decode(errors="replace").replace("\r", ""))

qemu = boot(1, virtio=True, subnet="192.168.76.0/24")
session("virtio", True, [
    ("ifconfig", b"192.168.76.15 netmask 255.255.255.0 gateway 192.168.76.2 (DHCP)"),
    ("ping", b"reply from 192.168.76.2"),
    ("ping 192.168.76.3", b"reply from 192.168.76.3"),
    ("ls", b"      18 disk/note"),
    ("cat disk/note", b"kept across boots\r\n"),
    ("echo appended >> disk/note", b""),
    ("cat disk/note", b"kept across boots\r\nappended\r\n"),
    ("fstest", b"fstest: disk ok"),
    ("rm disk/big", b""),
], halt=True)

# And back on ATA: what virtio wrote is on the disk.
qemu = boot(1, with_disk=True)
session("third", False, [
    ("cat disk/note", b"kept across boots\r\nappended\r\n"),
    ("rm disk/note", b""),
    ("cat disk/note", b"no such file: disk/note"),
])
print(f"---- {fails} failed ----")
sys.exit(1 if fails else 0)
PY
