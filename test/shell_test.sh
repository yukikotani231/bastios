#!/usr/bin/env bash
# Boot BastiOS in QEMU, drive the shell over the serial port, and check its replies.
# Input is sent one line at a time, each after the prompt appears: bytes sent before
# the kernel initialises the UART are dropped.
set -u
cd "$(dirname "$0")/.."
KERNEL=${KERNEL:-build/bastios.elf}
# Seconds each boot's steps may take in all. The longest boot's steps take
# about 35 s alone; with make test's other lanes running beside it (test/all.sh)
# they took over 60 s, and its last steps failed for want of time, on main too.
# The native job (NATIVE=1) on a loaded runner took over 110 s, and with the
# steps added since it ran past 240 s: the step being run then, and every one
# after it, failed with no output (main's native job, on whichever of the
# thread steps was running). So the limit is only there to end a hang, not to
# tell slow from broken.
TIMEOUT=${TIMEOUT:-600}
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

def boot(cpus, with_disk=False, virtio=False, nvme=False, nvme4k=False, ahci=False, net=False, subnet=None, card="virtio-net-pci", monitor=None, extra=(), qemu_bin="qemu-system-x86_64", kernel_path=None):
    drive = ["-drive", f"file={disk},format=raw,if=ide,index=0"] if with_disk else []
    if virtio:
        # The same disk as a virtio block device (src/virtio.bastio): sectors
        # move by DMA through pages the kernel lends it.
        drive = ["-drive", f"file={disk},format=raw,if=none,id=vd",
                 "-device", "virtio-blk-pci,drive=vd"]
    if nvme:
        # The same disk as an NVMe drive (src/nvme.bastio): queues and data
        # in pages lent to the controller, registers in the device window.
        # On a q35 machine it sits behind a PCIe root port, on bus 1, as on
        # a real PC.
        drive = ["-machine", "q35", "-device", "pcie-root-port,id=rp1,chassis=1",
                 "-drive", f"file={disk},format=raw,if=none,id=nv",
                 "-device", "nvme,serial=bastios,drive=nv,bus=rp1"]
    if nvme4k:
        # The same disk as an NVMe drive with 4 KiB logical blocks: the
        # driver still counts 512-byte sectors, so the bytes land where the
        # other drivers put them.
        drive = ["-drive", f"file={disk},format=raw,if=none,id=nv",
                 "-device", "nvme,serial=bastios,drive=nv,logical_block_size=4096,physical_block_size=4096"]
    if ahci:
        # The same disk on the q35 machine's own AHCI controller
        # (src/ahci.bastio), which is function 2 of device 31.
        drive = ["-machine", "q35", "-drive", f"file={disk},format=raw,if=ide"]
    # A virtio network card on QEMU's user-mode network (src/net.bastio).
    nic = ["-netdev", f"user,id=n0,hostfwd=udp:127.0.0.1:{udp_port}-:7,hostfwd=udp:127.0.0.1:{udp_port2}-:7000,hostfwd=tcp:127.0.0.1:{tcp_port}-:7001",
           "-device", f"{card},netdev=n0"] if net else ["-nic", "none"]
    # (Without -nic none QEMU adds an e1000 of its own, which the kernel
    # would now drive.)
    if subnet:
        # QEMU's DHCP server on a subnet other than its default: the
        # address the kernel ends up with can only have come from DHCP.
        nic = ["-netdev", f"user,id=n0,net={subnet}",
               "-device", "virtio-net-pci,netdev=n0"]
    # 1G: the kernel's arena is a fraction of RAM (4 MiB up to 128 MiB, 32 MiB
    # here). The per-process tables take 24 KiB a process and double with the
    # process table, so the 700 processes of mproc need several MiB of it.
    return subprocess.Popen(
        [qemu_bin, "-kernel", kernel_path or kernel, "-m", "1G", "-smp", str(cpus),
         "-display", "none", "-no-reboot", "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
         "-serial", "stdio", "-monitor", f"unix:{monitor},server,nowait" if monitor else "none"] + drive + nic + list(extra),
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
# Nor a sound card: /dev/dsp does not open (ENODEV).
mark = len(out)
qemu.stdin.write(b"mtone\r"); qemu.stdin.flush()
if read_until(b"bastios> ", mark) and b"mtone: no sound card (ENODEV)" in out[mark:]:
    print("ok   no sound card: /dev/dsp says ENODEV")
else:
    print("FAIL no sound card: mtone"); fails += 1
# A sleeping process does not run (wait_asleep): two seconds of BusyBox's
# sleep cost QEMU well under a second of host CPU. Before, nanosleep ran
# again on every pass and took the whole two seconds.
def qemu_cpu():
    try:
        f = open(f"/proc/{qemu.pid}/stat").read().rsplit(")", 1)[1].split()
        return (int(f[11]) + int(f[12])) / os.sysconf("SC_CLK_TCK")
    except (OSError, IndexError, ValueError):
        return None
mark = len(out)
qemu.stdin.write(b"ash\r"); qemu.stdin.flush()
read_until(b"# ", mark)
mark = len(out)
c0, t0 = qemu_cpu(), time.time()
qemu.stdin.write(b"sleep 2; echo slept$((1+1))\r"); qemu.stdin.flush()
ok = read_until(b"slept2", mark)
c1, t1 = qemu_cpu(), time.time()
good = ok and c0 is not None and c1 is not None and t1 - t0 >= 1.9 and c1 - c0 < 1.0
print(("ok  " if good else "FAIL"), f"sleep 2 sleeps: {t1 - t0:.2f} s, QEMU CPU {(c1 or 0) - (c0 or 0):.2f} s")
fails += not good
qemu.kill(); qemu.wait()
print("---- one-processor transcript ----")
print(out.decode(errors="replace").replace("\r", ""))

# The rest runs on `smp` processors: every processor runs the scheduler, so
# the shell and its programs run on whichever one picks them.
qemu = boot(smp, with_disk=True, net=True)
out = b""
# Each part of the test gets TIMEOUT seconds of its own: this boot and the
# steps below, then the checks after them. Sharing one budget with the
# one-processor boot left the checks at the end (bench, the parallel spins,
# halt) only what the steps had not used, and a few more steps used it up.
deadline = time.time() + timeout

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
    ("mtest", [b"mtest: malloc heap 75", b"mtest: big memory 4096 1024", b"mtest: wide 4", b"mtest: cow kept", b"mtest: direction flag spun", b"mtest: file line 42"], b"type a line"),
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
    # a descriptor far above the ones used so far survives fork, is closed
    # by exit and is not there after execve (the kernel scans descriptor
    # rows only up to the highest number ever open)
    ("exec 200>/tmp/hifd; (echo from-child >&200); exec 200>&-; cat /tmp/hifd", [b"from-child"], b"\n# "),
    ("exec 201>/tmp/hifd2; sh -c 'echo x >&201' 2>/dev/null; echo kept=$?; exec 201>&-", [b"kept="], b"\n# "),
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
    # A line longer than 256 bytes typed (or pasted) at once arrives whole:
    # the console's input ring and the terminal line hold 4096 bytes.
    ("echo " + "x" * 1000 + " | wc -c", b"\r\n1001\r\n", b"\n# "),
    # Signals: a trap handler entered and returned from (rt_sigreturn),
    # a process ending itself with kill, a writer ended by SIGPIPE once
    # head has gone, and Ctrl-C ending a busy loop (the parent ash's own
    # handler sees it too and carries on).
    ("trap 'echo got$((1+1))' USR1; kill -USR1 $$; echo after$((2+2))", [b"\r\ngot2\r\n", b"\r\nafter4\r\n"], b"\n# "),
    ("ash -c 'kill $$'; echo k=$?", b"k=143", b"\n# "),
    ("ash -c 'while :; do echo y; done' | head -n 1; echo piped", b"\r\ny\r\npiped", b"\n# "),
    ("ash -c 'while :; do :; done'", b"^C", b"\n# ", b"\x03"),
    ("echo st=$?", b"st=130", b"\n# "),
    # A script with no #! line: execve says ENOEXEC, so ash runs it itself,
    # through an execve of /proc/self/exe.
    ("echo 'echo script $1' > /sc; chmod +x /sc; /sc ran; rm /sc", b"script ran", b"\n# "),
    # execve passes up to 8192 arguments and 1024 environment strings, laid
    # out at the top of the stack (it used to stop at 32 and 16).
    ("(set --; i=0; while [ $i -lt 400 ]; do set -- \"$@\" arg$i; export V$i=x$i; i=$((i+1)); done; ash -c 'echo argc=$# last=${400} env=$V399' x \"$@\")", b"argc=400 last=arg399 env=x399", b"\n# "),
    # Time: clocks, nanosleep, poll that waits, select, alarms and pause, and
    # eventfd, timerfd and epoll (user/mtime.c); the sleep applet; ash's
    # read -t, a poll on the console that times out, then one that wakes
    # when a line is typed.
    ("mtime", [b"mtime: rang 3, selected, slept ", b"woke in time, events ok"], b"\n# "),
    # A TCP transfer to this machine's own address, many times longer than a
    # card's frame: loopback segments are longer than 1500 bytes
    # (src/net.bastio, NET_BUF), and every byte is checked on arrival. The
    # rings take their spare pages from the pool on first use (up to 512,
    # src/tcpbuf.bastio), so the later page counts may be lower than the
    # first by that much.
    ("mtcp loop 3000000", b"mtcp: loop 3000000 bytes, 0 wrong", b"\n# "),
    # 100 connections to ourselves at once is 200 in the kernel's table (it
    # held 32), and 30 UDP sockets at once (8 before).
    ("mtcp many 100", b"mtcp: 100 connections and 30 udp sockets, 16 queued", b"\n# "),
    ("sleep 1; echo slept$((1+1))", b"\r\nslept2\r\n", b"\n# "),
    # More processes at once than the 32 rows the process table starts with:
    # it grows (new_pid), and /proc lists three-digit pids. (stdin from the
    # terminal: ash gives each background job its own /dev/null otherwise,
    # and open files system-wide run out first.)
    ("i=0; while [ $i -lt 120 ]; do sleep 10 </dev/tty & i=$((i+1)); done; n=0; for d in /proc/[0-9]*; do n=$((n+1)); done; [ $n -gt 120 ] && [ -d /proc/120 ] && echo many$((1+1)); wait", b"\r\nmany2\r\n", b"\n# "),
    # /proc/PID/auxv: the vector exec built (AT_PHDR first, at least a dozen entries).
    # lseek on /proc/PID/auxv is tested by the gdb case of the compat suite.
    ("a=$(cat /proc/self/auxv | wc -c); [ $a -ge 192 ] && [ $a -le 288 ] && echo auxv$((1+1))", b"\r\nauxv2\r\n", b"\n# "),
    ("read -t 1 x; echo rt=$?", b"rt=1", b"\n# "),
    ("read -t 9 x; echo rt=$? x=$x", b"rt=0 x=late", b"\n# ", b"late\r"),
    # Directories: mkdir, a working directory (chdir, getcwd through
    # /bin/pwd), relative paths with "..", listing a subdirectory, and
    # rmdir refusing a directory that is not empty.
    ("mkdir dd; cd dd; /bin/pwd; echo deep > f; cat ../dd/f", b"\r\n/dd\r\ndeep\r\n", b"\n# "),
    ("mkdir sub; cd sub && /bin/pwd && cd ../.. && /bin/pwd; ls dd", [b"\r\n/dd/sub\r\n/\r\n", b"sub"], b"\n# "),
    ("rmdir dd; echo r=$?", [b"not empty", b"r=1"], b"\n# "),
    ("rm dd/f; rmdir dd/sub dd; test -d dd || echo gone$((1+1))", b"\r\ngone2\r\n", b"\n# "),
    # rename through BusyBox's mv: over a file, into a directory, and a
    # directory with its contents.
    ("echo one > m1; echo two > m2; mv m1 m2; cat m2; test -e m1 || echo moved$((1+1))", b"\r\none\r\nmoved2\r\n", b"\n# "),
    ("mkdir md; mv m2 md; mv md mdd; cat mdd/m2; ls md; echo l=$?", [b"\r\none\r\n", b"l=1"], b"\n# "),
    ("rm mdd/m2; rmdir mdd; echo clean$((1+1))", b"\r\nclean2\r\n", b"\n# "),
    # Removing a file that is open: the name goes, the open descriptor
    # still reads it until it is closed (in memory and on the disk).
    ("echo kept > uo; exec 3< uo; rm uo; test -e uo || cat <&3; exec 3<&-", b"\r\nkept\r\n", b"\n# "),
    ("echo dk > disk/uo; exec 4< disk/uo; rm disk/uo; ls disk/uo; cat <&4; exec 4<&-", [b"No such file", b"\r\ndk\r\n"], b"\n# "),
    # Modes and times: chmod, touch -t (utimensat), stat and ls -l, in
    # memory and on the disk.
    ("echo x > pm; chmod 750 pm; stat -c '%a %s %F' pm", b"\r\n750 2 regular file\r\n", b"\n# "),
    ("ls -l pm", b"-rwxr-x---", b"\n# "),
    ("touch -t 200001020304 pm; stat -c %Y pm", b"\r\n946782240\r\n", b"\n# "),
    ("echo y > disk/pm; chmod 600 disk/pm; touch -t 199912312359 disk/pm; stat -c '%a %Y' disk/pm", b"\r\n600 946684740\r\n", b"\n# "),
    ("mkdir pd; chmod 700 pd; stat -c '%a %F' pd", b"\r\n700 directory\r\n", b"\n# "),
    ("rm pm; rmdir pd; echo cleaned$((1+1))", b"\r\ncleaned2\r\n", b"\n# "),
    # The *at calls relative to a directory descriptor, fchdir, and mmap
    # of a file, EMFILE, and a named pipe (user/mdir.c).
    ("mdir", b"mdir: ok in /dx, mapped", b"\n# "),
    # A BastiOS file system on a disk image: mkfs.bstdisk3, mount -t bstdisk3,
    # and the file still there after an unmount and a second mount.
    ("truncate -s 1M im; mkfs.bstdisk3 im && mkdir mm && mount -t bstdisk3 im mm && echo kept > mm/f && umount mm && mount -t bstdisk3 im mm && cat mm/f; cat /proc/mounts; umount mm; rm im; rmdir mm; echo lo$((1+1))",
     [b"\r\nkept\r\n", b"bstdisk3 /mm bstdisk3 rw", b"\r\nlo2\r\n"], b"\n# "),
    # A file in memory past the 64 MiB it was once limited to: grown to
    # 300 MiB (holes cost no pages), then written at its end.
    ("truncate -s 300M bigram; stat -c %s bigram; echo z >> bigram; stat -c %s bigram; rm bigram; echo bg$((1+1))",
     [b"\r\n314572800\r\n", b"\r\n314572802\r\n", b"\r\nbg2\r\n"], b"\n# "),
    # Mount namespaces (src/lxns.bastio): a bind mount made in one is seen
    # there and not by the shell outside; pivot_root into a directory bound
    # onto itself, with /bin, a volume and proc mounted in it, as OCI
    # runtimes do it; a relative link in the volume cannot climb out of the
    # new root; a read-only bind remount; pivot_root in the first namespace
    # is refused.
    ("mkdir -p /tmp/nr/bin /tmp/nr/proc /tmp/nr/vol /tmp/nd; echo hostfile > /tmp/nd/f; echo inroot > /tmp/nr/marker; ln -s ../../../../etc /tmp/nd/esc; unshare -m sh -c 'mount --bind /tmp/nd /tmp/nr/vol && cat /tmp/nr/vol/f && cat /proc/self/mounts'; cat /proc/mounts; ls /tmp/nr/vol; echo ns1",
     [b"\r\nhostfile\r\n", b"none /tmp/nr/vol none rw 0 0", b"disk /disk bstdisk3 rw 0 0\r\nns1"], b"\n# "),
    ("unshare -m sh -c 'mount --bind /tmp/nr /tmp/nr && mount --bind /bin /tmp/nr/bin && mount --bind /tmp/nd /tmp/nr/vol && mount -t proc proc /tmp/nr/proc && cd /tmp/nr && pivot_root . . && umount -l . && cd / && cat /marker && cat /vol/f && cat /vol/esc/passwd; echo x > /vol/new; mount -o remount,bind,ro /vol && echo y > /vol/y; cat /proc/self/mounts; ls /tmp'; cat /tmp/nd/new; echo ns2",
     [b"\r\ninroot\r\nhostfile\r\n", b"can't open '/vol/esc/passwd'", b"none / none rw 0 0", b"none /vol none ro 0 0", b"proc /proc proc rw 0 0", b"/tmp: No such file", b"Read-only file system", b"\r\nx\r\nns2"], b"\n# "),
    ("pivot_root /tmp/nr /tmp/nr; echo p=$?; rm -r /tmp/nr /tmp/nd; echo ns3", [b"Invalid argument", b"p=1", b"ns3"], b"\n# "),
    # Overlay mounts (src/lxns.bastio): the layers merged as one directory,
    # the upper one and then the lower ones in the order given; a name in
    # the upper layer hides the same name below; ".wh.NAME" in a layer
    # hides NAME in the layers below it; an overlay without upperdir is
    # read-only.
    ("mkdir -p /tmp/ov/a /tmp/ov/b /tmp/ov/u /tmp/ov/w /tmp/ov/m /tmp/ov/r; echo A > /tmp/ov/a/f; echo B > /tmp/ov/b/f; echo G > /tmp/ov/b/g; echo H > /tmp/ov/b/h; mkdir /tmp/ov/b/sub; echo S > /tmp/ov/b/sub/s; touch /tmp/ov/u/.wh.h; echo U > /tmp/ov/u/n; mount -t overlay overlay -o lowerdir=/tmp/ov/a:/tmp/ov/b,upperdir=/tmp/ov/u,workdir=/tmp/ov/w /tmp/ov/m && cat /tmp/ov/m/f /tmp/ov/m/g /tmp/ov/m/n /tmp/ov/m/sub/s; cat /tmp/ov/m/h; for f in /tmp/ov/m/*; do echo $f; done; cat /proc/mounts; echo ov1",
     [b"\r\nA\r\nG\r\nU\r\nS\r\n", b"can't open '/tmp/ov/m/h'", b"/tmp/ov/m/f\r\n/tmp/ov/m/g\r\n/tmp/ov/m/n\r\n/tmp/ov/m/sub\r\n", b"\r\noverlay /tmp/ov/m overlay rw 0 0\r\n", b"\r\nov1"], b"\n# "),
    # Writing through an overlay: a file changed is copied up first (the
    # lower one stays as it was), new names and the directories above
    # them are made in the upper layer, a name removed or renamed away is
    # hidden by a whiteout, and a directory made again over one removed
    # starts empty.
    ("cd /tmp/ov/m && echo more >> f && cat f && cat /tmp/ov/a/f && mkdir -p nd/deep && echo d > nd/deep/n && chmod 600 g && echo up >> g && cat g; for x in /tmp/ov/u/*; do echo $x; done; ls -l g; cd /; echo ov3",
     [b"\r\nA\r\nmore\r\nA\r\n", b"G\r\nup\r\n", b"/tmp/ov/u/f\r\n/tmp/ov/u/g\r\n/tmp/ov/u/n\r\n/tmp/ov/u/nd\r\n", b"-rw-------", b"\r\nov3"], b"\n# "),
    ("cd /tmp/ov/m && rm g && cat g; mv nd nd2; echo mv=$?; rmdir nd2/deep; echo rd=$?; rm nd2/deep/n; rmdir nd2/deep nd2; echo rd2=$?; mkdir nd; [ -z \"$(ls nd)\" ] && echo ndempty; mv f f2 && cat f2; cat f; echo *; cd /; echo ov4",
     [b"can't open 'g'", b"mv=0", b"rd=1", b"rd2=0", b"ndempty", b"\r\nA\r\nmore\r\n", b"can't open 'f'", b"\r\nf2 n nd sub\r\n", b"\r\nov4"], b"\n# "),
    ("mount -t overlay overlay -o lowerdir=/tmp/ov/b /tmp/ov/r && cat /tmp/ov/r/g; echo x > /tmp/ov/r/new; echo w=$?; mount -t overlay overlay -o lowerdir=/tmp/ov/nope /tmp/ov/r; echo bad; umount /tmp/ov/m; umount /tmp/ov/r; rm -r /tmp/ov; echo ov2",
     [b"\r\nG\r\n", b"Read-only file system", b"w=1", b"failed: No such file", b"\r\nbad\r\n", b"\r\nov2"], b"\n# "),
    # UTS and IPC namespaces, /proc/PID/ns and setns (src/lxnsk.bastio): a
    # host name set in a new UTS namespace stays there; IPC objects made
    # in one IPC namespace are not seen from another, and setns joins one
    # through /proc/PID/ns/ipc or a pidfd (user/mns.c); nsenter joins a
    # process's UTS namespace through its /proc/PID/ns/uts link.
    ("unshare -u sh -c 'hostname box; hostname; cat /proc/sys/kernel/hostname'; hostname; echo uts1",
     b"\r\nbox\r\nbox\r\nbastios\r\nuts1", b"\n# "),
    ("mns", b"mns: ok", b"\n# "),
    # PID namespaces (src/lxnsk.bastio): the first process of a new one is 1,
    # sees itself and only its own children there, and a /proc mounted in
    # it lists them by those ids; user/mns.c (above) checks the rest.
    ("unshare -p -f --mount-proc sh -c 'echo $$; kill -0 1 && echo seen; cat /proc/1/stat; ls -d /proc/9'",
     [b"\r\n1\r\nseen\r\n1 (ash) ", b" 0 1 1 0 -1 "], b"\n# "),     # not the state letter: the shell may not have gone to sleep for cat yet
    # setns into a network namespace: nsenter -n joins the one a process
    # unshared into, so its /proc/self/ns/net link names that namespace
    # (and the caller's own stays the first one's).
    ("unshare -n sleep 5 & sleep 1; readlink /proc/$!/ns/net; nsenter -t $! -n readlink /proc/self/ns/net; readlink /proc/self/ns/net; kill $!; echo ns6",
     [b"\r\nnet:[40265320", b"\r\nnet:[40265320", b"\r\nnet:[4026531840]\r\n", b"ns6"], b"\n# "),
    ("unshare -u sh -c 'hostname inner; sleep 5' & sleep 1; nsenter -t $! -u hostname; readlink /proc/self/ns/uts; readlink /proc/$!/ns/uts; stat -L -c %i /proc/self/ns/mnt; ls /proc/self/ns; hostname; kill $!; echo ns5",
     [b"\r\ninner\r\nuts:[4026531838]\r\nuts:[40265320", b"\r\n4026531841\r\n", b"pid_for_children", b"\r\nbastios\r\n", b"ns5"], b"\n# "),
    # Owners and the umask (src/lxperm.bastio): a new file takes the umask,
    # chown keeps its owner, on the disk too.
    ("umask 077; echo x > po; chown 65534:100 po; stat -c 'p%a %u %g' po; rm po; umask 022; echo y > /disk/po; chown 7:8 /disk/po; stat -c 'd%a %u %g' /disk/po; rm /disk/po",
     [b"p600 65534 100", b"d644 7 8"], b"\n# "),
    # Users (docs/security.md): adduser, su to the new user and back,
    # /etc/shadow is root's, su and passwd are the only set-user-ID
    # programs, and su asks for root's password once root has one.
    ("adduser -D alice; id alice; stat -c '%a %U' /bin/su /bin/ls /etc/shadow /root/",
     [b"uid=1000(alice) gid=1000(alice)", b"\r\n4755 root\r\n755 root\r\n600 root\r\n700 root\r\n"], b"\n# "),
    ("su alice -c 'id -u; whoami; cd; pwd; cat /etc/shadow; echo s=$?; ls /root; echo r=$?'",
     [b"\r\n1000\r\nalice\r\n/home/alice\r\n", b"Permission denied", b"s=1", b"r=1"], b"\n# "),
    # User namespaces (src/lxuser.bastio): alice is root inside `unshare -U -r`
    # but has no more rights: root's files show as 65534 and stay unreadable,
    # and a file she makes is hers outside. Without a map she is 65534.
    ("su alice -c 'unshare -U -r sh -c \"id -u; id -g; ls -ln /etc/shadow; cat /etc/shadow; echo s=\\$?; touch /home/alice/f; ls -ln /home/alice/f; cat /proc/self/uid_map /proc/self/setgroups\"; ls -ln /home/alice/f; unshare -U id -u; unshare -U -r -u sh -c \"hostname box; hostname\"; unshare -U -r unshare -U true; echo n=$?'",
     [b"\r\n0\r\n0\r\n", b"65534    65534", b"Permission denied", b"s=1", b"0        0", b"         0       1000          1\r\ndeny", b"1000     1000", b"\r\n65534\r\nbox\r\n", b"n=1"], b"\n# "),
    # The root of a user namespace mounts in a mount namespace of its own,
    # and only there: a tmpfs it mounts is gone with the namespace, and a
    # user without a mount namespace of its own (or without any) mounts nothing.
    ("su alice -c 'mkdir -p /tmp/ur; unshare -U -r -m sh -c \"mount -t tmpfs t /tmp/ur && echo own > /tmp/ur/f && cat /tmp/ur/f; ls -ln /tmp/ur/f\"; ls -A /tmp/ur | wc -l; unshare -U -r sh -c \"mount -t tmpfs t /tmp/ur || echo nons\"; mount -t tmpfs t /tmp/ur || echo plain; rmdir /tmp/ur'",
     [b"\r\nown\r\n", b"0        0", b"\r\n0\r\n", b"nons", b"plain"], b"\n# "),
    # Control groups (src/lxcgroup.bastio): a group is made with mkdir, a
    # shell moves into it, and pids.max stops the fork that would pass it;
    # cgroup.kill empties a group so that rmdir works, and non-root users may
    # not change anything.
    ("mkdir /sys/fs/cgroup/t; echo +pids > /sys/fs/cgroup/cgroup.subtree_control; echo 3 > /sys/fs/cgroup/t/pids.max; cat /sys/fs/cgroup/t/pids.max /sys/fs/cgroup/t/cgroup.controllers; ash -c 'echo $$ > /sys/fs/cgroup/t/cgroup.procs; cat /proc/self/cgroup; sleep 30 & sleep 30 & sleep 30 & echo f=$?'",
     [b"\r\n3\r\npids\r\n", b"0::/t", b"can't fork"], b"\n# "),
    ("echo 1 > /sys/fs/cgroup/t/cgroup.kill; sleep 1; rmdir /sys/fs/cgroup/t; echo r=$?; mkdir /sys/fs/cgroup/u; rmdir /sys/fs/cgroup/u; echo r=$?; su alice -c 'mkdir /sys/fs/cgroup/x; echo pids.max > /dev/null; echo 5 > /sys/fs/cgroup/pids.max; echo w=$?'",
     [b"r=0", b"r=0", b"Permission denied", b"w=1"], b"\n# "),
    # cgroup.freeze stops a group's processes from being scheduled until it is
    # thawed; SIGKILL still ends a frozen process.
    ("mkdir /sys/fs/cgroup/fz; ash -c 'echo $$ > /sys/fs/cgroup/fz/cgroup.procs; i=0; while :; do i=$((i+1)); echo $i > /tmp/fzc; done' & sleep 1; echo 1 > /sys/fs/cgroup/fz/cgroup.freeze; cat /sys/fs/cgroup/fz/cgroup.freeze /sys/fs/cgroup/fz/cgroup.events; read a < /tmp/fzc; sleep 2; read b < /tmp/fzc; [ \"$a\" = \"$b\" ] && echo held; echo 0 > /sys/fs/cgroup/fz/cgroup.freeze; sleep 2; read c < /tmp/fzc; [ \"$c\" -gt \"$b\" ] && echo thawed",
     [b"\r\n1\r\npopulated 1\r\nfrozen 1\r\n", b"held", b"thawed"], b"\n# "),
    # cpu.max: a group that may use 20 ms of every 100 ms is not scheduled for
    # the rest of each period; cpu.stat counts what it used and the periods it
    # ran out of quota in.
    ("mkdir /sys/fs/cgroup/cq; echo +cpu > /sys/fs/cgroup/cgroup.subtree_control; echo '20000 100000' > /sys/fs/cgroup/cq/cpu.max; cat /sys/fs/cgroup/cq/cpu.max; ash -c 'echo $$ > /sys/fs/cgroup/cq/cgroup.procs; while :; do :; done' & sleep 3; while read k v; do case $k in usage_usec) u=$v;; nr_throttled) t=$v;; esac; done < /sys/fs/cgroup/cq/cpu.stat; [ \"$u\" -gt 100000 ] && [ \"$u\" -lt 1000000 ] && [ \"$t\" -gt 5 ] && echo throttled; echo 1 > /sys/fs/cgroup/cq/cgroup.kill; sleep 1; rmdir /sys/fs/cgroup/cq; echo r=$?",
     [b"\r\n20000 100000\r\n", b"throttled", b"r=0"], b"\n# "),
    # cpu.max with every processor busy running waiting calls again (readers of
    # a fifo nobody writes to): the throttled group's next period must still
    # begin, or its process is never scheduled again.
    ("mkdir /sys/fs/cgroup/rk /sys/fs/cgroup/rq; echo +cpu > /sys/fs/cgroup/cgroup.subtree_control; echo '20000 100000' > /sys/fs/cgroup/rq/cpu.max; mkfifo /tmp/rb; ash -c 'echo $$ > /sys/fs/cgroup/rk/cgroup.procs; for i in 1 2 3 4; do cat /tmp/rb > /dev/null & done; sleep 60 > /tmp/rb & wait' & sleep 1; ash -c 'echo $$ > /sys/fs/cgroup/rq/cgroup.procs; i=0; while [ $i -lt 3000 ]; do i=$((i+1)); done; echo burst > /tmp/rbd' & n=0; while [ ! -f /tmp/rbd ] && [ $n -lt 20 ]; do sleep 1; n=$((n+1)); done; read r < /tmp/rbd; echo r=$r; echo 1 > /sys/fs/cgroup/rk/cgroup.kill; echo 1 > /sys/fs/cgroup/rq/cgroup.kill; sleep 1; rmdir /sys/fs/cgroup/rk /sys/fs/cgroup/rq; echo c=$?; rm -f /tmp/rb /tmp/rbd",
     [b"r=burst", b"c=0"], b"\n# "),
    # cpu.weight: two groups that both have more busy processes than there are
    # processors share them in proportion to their weights, here 1 to 3.
    ("mkdir /sys/fs/cgroup/wa /sys/fs/cgroup/wb; echo +cpu > /sys/fs/cgroup/cgroup.subtree_control; echo 100 > /sys/fs/cgroup/wa/cpu.weight; echo 300 > /sys/fs/cgroup/wb/cpu.weight; cat /sys/fs/cgroup/wb/cpu.weight; printf 'echo $$ > /sys/fs/cgroup/$1/cgroup.procs\\nwhile :; do :; done\\n' > /tmp/wb.sh; for g in wa wb; do for i in 1 2 3 4 5 6; do ash /tmp/wb.sh $g & done; done; sleep 12; while read k v; do case $k in usage_usec) a=$v;; esac; done < /sys/fs/cgroup/wa/cpu.stat; while read k v; do case $k in usage_usec) b=$v;; esac; done < /sys/fs/cgroup/wb/cpu.stat; echo a=$a b=$b; [ $((b * 10)) -gt $((a * 20)) ] && [ $((b * 10)) -lt $((a * 45)) ] && echo weighted; echo 1 > /sys/fs/cgroup/wa/cgroup.kill; echo 1 > /sys/fs/cgroup/wb/cgroup.kill; sleep 2; rmdir /sys/fs/cgroup/wa /sys/fs/cgroup/wb; echo r=$?",
     [b"\r\n300\r\n", b"weighted", b"r=0"], b"\n# "),
    ("mkdir /sys/fs/cgroup/fk; ash -c 'echo $$ > /sys/fs/cgroup/fk/cgroup.procs; sleep 100' & sleep 1; echo 1 > /sys/fs/cgroup/fk/cgroup.freeze; echo 1 > /sys/fs/cgroup/fk/cgroup.kill; sleep 1; rmdir /sys/fs/cgroup/fk; echo r=$?; echo 1 > /sys/fs/cgroup/fz/cgroup.kill; sleep 1; rmdir /sys/fs/cgroup/fz; echo z=$?",
     [b"r=0", b"z=0"], b"\n# "),
    ("echo root:pw | chpasswd; read l < /etc/shadow; case $l in 'root:$6$'*) echo sha512;; esac; su alice -c 'echo no | su -c id root; echo w=$?; echo pw | su -c id root'",
     [b"\r\nsha512\r\n", b"w=1", b"uid=0(root) gid=0(root)"], b"\n# "),
    # (BusyBox keeps the old /etc files as NAME-: gone, so no pages stay.)
    ("deluser alice; id alice; passwd -d root; echo u=$?; rm -rf /home/alice /etc/passwd- /etc/group- /etc/shadow-",
     [b"unknown user alice", b"u=0"], b"\n# "),
    # With a disk, /etc, /root and /home are links into it (src/main.bastio,
    # install_files): a user made now is still there at the next boot.
    ("adduser -D kept; echo hi > /root/kept; ls -l /etc /root /home", [b"/etc -> /disk/etc", b"/root -> /disk/root", b"/home -> /disk/home"], b"\n# "),
    # Small calls: uname, readv, pread/pwrite, limits, sysinfo, statfs,
    # getrandom, priorities, sessions, process groups, ttyname, pipe capacity (user/mmisc.c).
    ("mmisc", b"mmisc: ok on Linux x86_64, signals, pipe 65536", b"\n# "),
    # posix_spawn: clone(CLONE_VM | CLONE_VFORK) lends the parent's
    # address space to the child until it execs or exits (user/mspawn.c).
    ("mspawn", [b"spawned 5", b"mspawn: ok, kept 1234, sh is POSIX"], b"\n# "),
    # A dynamically linked program: the kernel loads musl's dynamic linker,
    # its PT_INTERP, which links libc into it (user/mdyn.c).
    ("mdyn", b"mdyn: linked dynamically, mdyn, sum 5050, argc 1, libc apart", b"\n# "),
    # pthreads: clone(CLONE_THREAD), futex behind mutexes, condition
    # variables (one timed) and joins, and _exit from a thread ending the
    # whole process (user/mthread.c).
    ("mthread", b"mthread: ok, 8000 increments", b"\n# "),
    # Threads on several processors at once: four compute and map memory
    # in parallel (the checksum is the same as on Linux), and execve ends
    # threads that are running elsewhere.
    ("mthread par", [b"mthread: par 4 threads", b"(7f4)"], b"\n# "),
    ("mthread exec", b"mthread: exec ok, threads gone", b"\n# "),
    ("mthread unmap", b"mthread: unmap ok, threads still running", b"\n# "),
    ("mthread ids", b"mthread: ids ok", b"\n# "),
    # clock_gettime through the vDSO (user/vdso.c): it agrees with the
    # system call, and four threads read it 20000 times each.
    ("mthread clock", b"mthread: clock ok (vdso)", b"\n# "),
    # What Linux programs expect to find: /dev/null swallows, /tmp and
    # /etc/passwd exist, and every file has its own inode number.
    ("echo lost > /dev/null; wc -c < /dev/null; test -d /tmp && echo tmp$((2+2))", b"\r\n0\r\ntmp4\r\n", b"\n# "),
    ("cat /etc/passwd", b"nobody:x:65534:65534:", b"\n# "),
    ("cat /etc/group", b"nogroup:x:65534:", b"\n# "),
    ("echo a > /tmp/i1; echo b > /tmp/i2; set -- $(ls -i /tmp/i1); a=$1; set -- $(ls -i /tmp/i2); [ $a != $1 ] && echo distinct; rm /tmp/i1 /tmp/i2", b"\r\ndistinct\r\n", b"\n# "),
    # Symbolic links: made by ln -s, read by readlink, followed by
    # everything else (relative and absolute targets, a link to a
    # directory, on the disk too); a dangling link and a loop do not open.
    ("echo tgt > lt; ln -s lt ll; cat ll; readlink ll; ls -l ll", [b"\r\ntgt\r\nlt\r\n", b"ll -> lt"], b"\n# "),
    ("echo dt > disk/lt; ln -s lt disk/ll; cat disk/ll; mkdir ld; echo in > ld/f; ln -s /ld lk; cat lk/f", b"\r\ndt\r\nin\r\n", b"\n# "),
    ("ln -s nowhere dl; cat dl; ln -s l1 l2; ln -s l2 l1; cat l1; echo c=$?", [b"No such file", b"c=1"], b"\n# "),
    ("rm ll lk dl l1 l2 lt ld/f disk/ll disk/lt; rmdir ld; ls ll; echo gone$((1+1))", [b"No such file", b"gone2"], b"\n# "),
    # Hard links: ln gives a file a second name with the same inode, the
    # link count follows, and the file lives until its last name goes. Not
    # across file systems, and not for directories. disk/hk1 and disk/hk2
    # stay for the virtio and NVMe boots below.
    ("echo hl > h1; ln h1 h2; echo more >> h2; cat h1; stat -c %h h1; set -- $(ls -i h1); a=$1; set -- $(ls -i h2); [ $a = $1 ] && echo same", b"\r\nhl\r\nmore\r\n2\r\nsame\r\n", b"\n# "),
    ("rm h1; cat h2; stat -c %h h2; ls h1; rm h2; ls h2", [b"\r\nhl\r\nmore\r\n1\r\n", b"h1: No such file", b"h2: No such file"], b"\n# "),
    # Named pipes are files: listed with type p, renamed and removed like
    # any file, and kept on the disk across boots (disk/pf, for the virtio
    # boot below).
    ("mkfifo pp; ls -l pp; echo hi > pp & cat pp; wait; mv pp pq; rm pq; ls pq; mkfifo disk/pf; stat -c %F disk/pf", [b"prw", b"\r\nhi\r\n", b"pq: No such file", b"fifo"], b"\n# "),
    ("echo dh > disk/hk1; ln disk/hk1 disk/hk2; ln disk/hk1 h3; mkdir hd; ln hd hd2; rmdir hd; stat -c %h disk/hk2", [b"Cross-device link", b"not permitted", b"\r\n2\r\n"], b"\n# "),
    # A file removed while its last writes are still in a batch that was
    # never queued (the disk never sees them), and one removed after a
    # second batch over blocks the disk has: both give every block back.
    ("sync; stat -f -c %f /disk > fr0; i=0; while [ $i -lt 20 ]; do echo data$i > disk/tmpdrop; rm disk/tmpdrop; i=$((i+1)); done; echo old > disk/keepdrop; sync; echo more >> disk/keepdrop; cat disk/keepdrop; rm disk/keepdrop; sync; stat -f -c %f /disk > fr1; cmp fr0 fr1 && echo dropfree=same; ls disk/tmpdrop disk/keepdrop", [b"\r\nold\r\nmore\r\n", b"dropfree=same", b"tmpdrop: No such file"], b"\n# "),
    # Loop devices (src/lxloop.bastio): /dev/loopN are block devices and
    # /dev/loop-control a character device; test/compat/progs/t_dev.c
    # binds one to a file and reads and writes through it.
    ("stat -c %F /dev/loop0 /dev/loop-control", b"block special file\r\ncharacter special file\r\n", b"\n# "),
    # An unbound loop device's /sys/block entry: no sectors, not read-only.
    ("cat /sys/block/loop0/size /sys/block/loop0/ro", b"0\r\n0\r\n", b"\n# "),
    # The disk's directory grows past its first 64 entries (dir_grow); the
    # power cuts below check that what it grew survives. (Past 1023
    # entries: the big-disk boot.)
    # Reads fill the page cache, and the next reads of the same file come from it (src/pcache.bastio, pc_fill): they must see appends and rewrites.
    ("i=0; while [ $i -lt 3000 ]; do echo $i; i=$((i+1)); done > /disk/pc; cat /disk/pc | wc -c; cat /disk/pc | wc -c; echo more >> /disk/pc; cat /disk/pc | wc -c; wc -l < /disk/pc; echo X > /disk/pc; cat /disk/pc; rm /disk/pc", b"13890\r\n13890\r\n13895\r\n3001\r\nX\r\n", b"\n# "),
    # 55 blocks in a row: the disk takes them in batched requests (src/diskq.bastio).
    ("i=0; while [ $i -lt 600 ]; do echo line$i; i=$((i+1)); done > /tmp/mref; for k in 1 2 3 4 5 6 7 8; do cat /tmp/mref /tmp/mref /tmp/mref /tmp/mref /tmp/mref /tmp/mref; done > /tmp/mall; cp /tmp/mall /disk/multi; sync; cmp /tmp/mall /disk/multi && echo multi=same; wc -c < /disk/multi; rm /disk/multi /tmp/mall /tmp/mref", [b"multi=same", b"\r\n225120\r\n"], b"\n# "),
    ("mkdir disk/many; i=0; while [ $i -lt 150 ]; do echo $i > disk/many/f$i || break; i=$((i+1)); done; ls disk/many | wc -l; cat disk/many/f149", b"\r\n150\r\n149\r\n", b"\n# "),
    ("rm -r disk/many; ls disk/many; echo gone$((1+1))", [b"No such file", b"gone2"], b"\n# "),
    # The in-memory file system finds names through a hash index (src/ramfs.bastio): creating, removing and renaming hundreds of files
    # must leave every name findable, and enough removals fill it with tombstones, which a rebuild clears.
    ("mkdir ixr; i=0; while [ $i -lt 600 ]; do echo $i > ixr/f$i || break; i=$((i+1)); done; i=0; while [ $i -lt 600 ]; do [ $((i%3)) -eq 0 ] && rm ixr/f$i; [ $((i%3)) -eq 1 ] && mv ixr/f$i ixr/g$i; i=$((i+1)); done; ls ixr | wc -l; cat ixr/g400 ixr/f401", b"\r\n400\r\n400\r\n401\r\n", b"\n# "),
    ("i=0; while [ $i -lt 600 ]; do [ $((i%3)) -eq 0 ] && echo r$i > ixr/f$i; i=$((i+1)); done; ls ixr | wc -l; cat ixr/f300 ixr/g400; rm -r ixr; ls ixr", [b"\r\n600\r\nr300\r\n400\r\n", b"No such file"], b"\n# "),
    # /proc, generated by the kernel: the mount table, memory, uptime, a
    # directory per process with its status, and /proc/self/exe as a link.
    ("cat /proc/mounts", b"rootfs / ramfs rw 0 0\r\nproc /proc proc rw 0 0\r\n", b"\n# "),
    ("ls /proc; stat -c %F /proc/self", [b"meminfo", b"uptime", b"directory"], b"\n# "),
    ("cat /proc/meminfo; read up idle < /proc/uptime && echo up=${up%.*}", [b"MemTotal: ", b" kB\r\nMemFree: ", b"up="], b"\n# "),
    ("cat /proc/$$/status; set -- $(cat /proc/$$/stat); [ $1 = $$ ] && echo stat=$2", [b"Name:\tash", b"PPid:\t", b"stat=(ash)"], b"\n# "),
    ("ls -l /proc/self/exe; echo nope > /proc/mounts; echo w=$?", [b"-> /bin/", b"Permission denied"], b"\n# "),
    # What Linux programs look for besides: processors, the file system
    # types, a process's own mount table, and three fixed sysctls.
    ("n=0; while read k r; do [ \"$k\" = processor ] && n=$((n+1)); done < /proc/cpuinfo; echo cpus=$n; cat /proc/filesystems; cat /proc/self/mounts", [f"cpus={smp}\r\n".encode(), b"nodev\tramfs", b"nodev\ttmpfs", b"\tbstdisk3", b"proc /proc proc"], b"\n# "),
    ("cat /proc/sys/kernel/tainted /proc/sys/kernel/pid_max; ls /proc/self", [b"\r\n0\r\n1024\r\n", b"mounts", b"auxv", b"mem"], b"\n# "),
    # An 8 MiB stack, Linux's default (t_mem in make compat maps 640 MiB
    # of the 1 GiB window and grows the stack past 6 MiB).
    ("ulimit -s", b"\r\n8192\r\n", b"\n# "),
    # The kernel's command line, and a process's fixed regions.
    ("cat /proc/cmdline /proc/self/maps", [b"BOOT_IMAGE=", b"[vdso]", b"[stack]"], b"\n# "),
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
    # Directories: a file can only go in a directory that exists, and only
    # an empty directory can be removed.
    ("mkdir docs", b""),
    ("mkdir docs", b"cannot make docs"),
    ("echo inside > docs/a", b""),
    ("echo x > nodir/a", b"cannot open the file"),
    ("mkdir nodir/sub", b"cannot make nodir/sub"),
    ("mkdir docs/sub", b""),
    ("ls docs", [b"\na\r\n", b"\nsub/\r\n"]),
    ("ls /", [b"\ndocs/\r\n", b"\ndisk/\r\n", b"\nmotd\r\n"]),
    ("ls", [b"         docs/\r\n", b"       7 docs/a\r\n"]),
    ("cat docs/a", b"inside\r\n"),
    ("cat docs", b"no such file: docs"),
    ("rm docs", b"cannot remove docs"),
    ("rmdir docs", b"cannot remove docs: not an empty directory"),
    ("ls nothere", b"not a directory: nothere"),
    ("ls docs/a", b"not a directory: docs/a"),
    ("rm docs/a", b""),
    ("rmdir docs/sub", b""),
    ("rmdir docs", b""),
    ("cat docs/a", b"no such file: docs/a"),
    # Renaming: files and directories (with what is in them), never onto
    # a name that exists, into itself, or between memory and the disk.
    ("echo one > r1", b""),
    ("mv r1 r2", b""),
    ("cat r2", b"one\r\n"),
    ("cat r1", b"no such file: r1"),
    ("mkdir rd", b""),
    ("echo x > rd/f", b""),
    ("mv rd rd2", b""),
    ("cat rd2/f", b"x\r\n"),
    ("mv rd2 rd2/in", b"cannot move rd2"),
    ("echo y > r3", b""),
    ("mv r2 r3", b"cannot move r2"),
    ("mv r2 disk/r2", b"cannot move r2"),
    ("mv nothere r9", b"cannot move nothere"),
    ("mv rd2/f r2x", b""),
    ("cat r2x", b"x\r\n"),
    ("rm r2", b""),
    ("rm r3", b""),
    ("rm r2x", b""),
    ("rmdir rd2", b""),
    ("mkdir disk/md", b""),
    ("echo z > disk/md/f", b""),
    ("mv disk/md disk/md2", b""),
    ("cat disk/md2/f", b"z\r\n"),
    ("mv disk/md2 disk/md2/x", b"cannot move disk/md2"),
    ("mv disk/md2/f disk/moved", b""),
    ("cat disk/md2/f", b"no such file: disk/md2/f"),
    ("rmdir disk/md2", b""),
    # Paths longer than an old 28-byte name.
    ("mkdir a_directory_whose_name_is_long", b""),
    ("echo far > a_directory_whose_name_is_long/and_a_file_with_a_long_name_too", b""),
    ("cat a_directory_whose_name_is_long/and_a_file_with_a_long_name_too", b"far\r\n"),
    ("ls a_directory_whose_name_is_long", b"\nand_a_file_with_a_long_name_too\r\n"),
    ("rm a_directory_whose_name_is_long/and_a_file_with_a_long_name_too", b""),
    ("rmdir a_directory_whose_name_is_long", b""),
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
    # A directory on the disk, also for the later boots.
    ("mkdir disk/keep", b""),
    ("echo deep > disk/keep/f", b""),
    ("ls disk", [b"\nnote\r\n", b"\nkeep/\r\n"]),
    ("mkdir disk/long_directory_name_beyond_28_bytes", b""),
    ("echo far > disk/long_directory_name_beyond_28_bytes/and_a_file_with_a_long_name_too", b""),
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
    deadline = time.time() + timeout
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
    # The clock (src/tsc.bastio: the time-stamp counter, its rate measured
    # at boot against the HPET, whose registers are claimed through Mmio)
    # counts nanoseconds: over the same wall-clock interval it must advance
    # by about as much.
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
        print(f"ok   clock: {(c1 - c0) / 1e9:.2f} s in {w1 - w0:.1f} s")
    else:
        print("FAIL clock:", c0, c1); fails += 1
    if t0 is not None and t1 is not None and 60 * (w1 - w0) <= t1 - t0 <= 200 * (w1 - w0):
        print(f"ok   timer: {t1 - t0} ticks in {w1 - w0:.1f} s")
    else:
        print("FAIL timer ticks:", t0, t1); fails += 1
    # The shell sat idle for those two seconds, so the bootstrap processor
    # stretched its tick (src/main.bastio, stretch_p): the tick count must
    # still agree with the clock, whatever the stretches were.
    if t0 is not None and t1 is not None and c0 and c1 and 0.9 * (c1 - c0) <= (t1 - t0) * 1e7 <= 1.1 * (c1 - c0) + 3e7:
        print(f"ok   timer: {t1 - t0} ticks agree with the clock's {(c1 - c0) / 1e9:.2f} s (idle, tick stretched)")
    else:
        print("FAIL timer ticks against the clock:", t0, t1, c0, c1); fails += 1
    # UDP echo: a datagram from this machine reaches the guest through
    # QEMU's port forward (ARP, IPv4, UDP on the way in) and comes back.
    if b"net: virtio, mac 52:54:00:12:34:56" in out:
        print("ok   network card found at boot")
    else:
        print("FAIL network card at boot"); fails += 1
    # Its receive queue raises an MSI-X message (src/virtio_net.bastio),
    # for processes that sleep until a frame comes in.
    if b"net: frames received by interrupt\r\n" in out:
        print("ok   network card interrupts")
    else:
        print("FAIL network card interrupts"); fails += 1
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
    # The same to this machine: the SYN and the reset both go round the
    # loopback queue, never onto the wire.
    mark = len(out)
    qemu.stdin.write(b"tcpget 127.0.0.1 9 x\r"); qemu.stdin.flush()
    if read_until(b"bastios> ", mark) and b"connection refused" in out[mark:]:
        print("ok   TCP: loopback to a closed port is refused")
    else:
        print("FAIL TCP loopback refused"); fails += 1
    # TCP through Linux system calls (user/mtcp.c, musl): it listens,
    # accepts, reads a line naming a port on this machine and answers it;
    # then it connects to that port, writes, and reads the reply until the
    # host closes. It runs in the background so that ps can show it
    # blocked in accept: asleep (SOCKWAIT, "socket") rather than restarted
    # on every pass of the scheduler, until the frames that come in wake it.
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    srv.settimeout(10)
    ask = f"{srv.getsockname()[1]}\n".encode()
    mark = len(out)
    qemu.stdin.write(b"mtcp &\r"); qemu.stdin.flush()
    reply = b""
    line = b""
    if read_until(b"listening on tcp port 7001", mark):
        # mtcp prints that line just before it calls accept, so ps may still
        # find it ready on a loaded machine: ask again, a few times.
        asleep = False
        for _ in range(10):
            time.sleep(0.5)
            m2 = len(out)
            qemu.stdin.write(b"ps\r"); qemu.stdin.flush()
            # ps's own line names mtcp; wait for the prompt after it (the one
            # `mtcp &` printed can arrive after m2, under SMP).
            if not (read_until(b" mtcp", m2) and read_until(b"bastios> ", out.index(b" mtcp", m2))):
                break
            if re.search(rb"socket\s+\d+\s+mtcp", out[m2:]):
                asleep = True
                break
        if asleep:
            print("ok   Linux TCP: a process blocked in accept sleeps")
        else:
            print("FAIL Linux TCP accept sleep:", repr(out[m2:])); fails += 1
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
    if (read_until(b"mtcp: got", mark) and reply == b"echo: " + ask
            and b"mtcp: connection from 10.0.2.2" in out[mark:]):
        print("ok   Linux TCP: listen, accept, read, write, close")
    else:
        print("FAIL Linux TCP serve:", reply, repr(out[mark:])); fails += 1
    if line == b"linux says hi\n" and read_until(b"mtcp: got back at you", mark):
        print("ok   Linux TCP: connect, write, read until the peer closes")
    else:
        print("FAIL Linux TCP get:", line); fails += 1
    # First boot on a cloud machine (user/mcloud.c): the instance metadata
    # protocol of AWS (IMDSv2: a token from a PUT, sent back as a header),
    # served here by a fake service on this host. The address is an
    # argument; the real one, 169.254.169.254, is not reachable from QEMU's
    # user network.
    import threading
    imds_log = []
    imds_key = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIBastioTestKey test@example"
    def imds_serve(listener):
        for _ in range(4):
            try:
                c, _ = listener.accept()
            except OSError:
                return
            c.settimeout(10)
            req = b""
            try:
                while b"\r\n\r\n" not in req:
                    d = c.recv(1024)
                    if not d:
                        break
                    req += d
            except OSError:
                pass
            head = req.decode(errors="replace")
            line = head.split("\r\n")[0]
            imds_log.append(line)
            authed = "X-aws-ec2-metadata-token: TOK123\r\n" in head
            if line.startswith("PUT /latest/api/token ") and "X-aws-ec2-metadata-token-ttl-seconds: 60" in head:
                code, body = "200 OK", "TOK123"
            elif not authed:
                code, body = "401 Unauthorized", ""
            elif line.startswith("GET /latest/meta-data/hostname "):
                code, body = "200 OK", "cloudbox-7.internal"
            elif line.startswith("GET /latest/meta-data/public-keys/0/openssh-key "):
                code, body = "200 OK", imds_key
            elif line.startswith("GET /latest/user-data "):
                code, body = "200 OK", "#!/bin/sh\necho first boot ran\n"
            else:
                code, body = "404 Not Found", ""
            try:
                c.sendall(f"HTTP/1.1 {code}\r\nContent-Length: {len(body)}\r\nConnection: close\r\n\r\n{body}".encode())
            except OSError:
                pass
            c.close()
    imds = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    imds.bind(("127.0.0.1", 0))
    imds.listen(4)
    th = threading.Thread(target=imds_serve, args=(imds,), daemon=True)
    th.start()
    # The kernel's shell passes no arguments to a Linux program, so this runs
    # under BusyBox's ash.
    mark = len(out)
    qemu.stdin.write(b"ash\r"); qemu.stdin.flush()
    read_until(b"\n# ", mark)
    mark = len(out)
    qemu.stdin.write(f"mcloud 10.0.2.2:{imds.getsockname()[1]}\r".encode()); qemu.stdin.flush()
    got = read_until(b"user data saved", mark) and read_until(b"\n# ", mark)
    text = out[mark:]
    th.join(10)
    imds.close()
    want_log = ["PUT /latest/api/token HTTP/1.1", "GET /latest/meta-data/hostname HTTP/1.1",
                "GET /latest/meta-data/public-keys/0/openssh-key HTTP/1.1", "GET /latest/user-data HTTP/1.1"]
    if got and b"mcloud: hostname cloudbox-7.internal" in text and imds_log == want_log:
        print("ok   mcloud: token, host name and public key from the metadata service")
    else:
        print("FAIL mcloud:", repr(text), imds_log); fails += 1
    mark = len(out)
    qemu.stdin.write(b"hostname\r"); qemu.stdin.flush()
    if read_until(b"\n# ", mark) and b"cloudbox-7.internal" in out[mark:]:
        print("ok   mcloud: the host name is set")
    else:
        print("FAIL mcloud host name:", repr(out[mark:])); fails += 1
    mark = len(out)
    qemu.stdin.write(b"cat /root/.ssh/authorized_keys\r"); qemu.stdin.flush()
    if read_until(b"\n# ", mark) and imds_key.encode() in out[mark:]:
        print("ok   mcloud: the public key is in root's authorized_keys")
    else:
        print("FAIL mcloud authorized_keys:", repr(out[mark:])); fails += 1
    mark = len(out)
    qemu.stdin.write(b"ls -l /root/user-data; /root/user-data\r"); qemu.stdin.flush()
    if read_until(b"\n# ", mark) and b"rwx------" in out[mark:] and b"first boot ran" in out[mark:]:
        print("ok   mcloud: user data is saved as an executable only root can read, and runs")
    else:
        print("FAIL mcloud user data:", repr(out[mark:])); fails += 1
    # A Linux process killed by a page fault gets a second line saying what
    # the kernel knows of the page (the report for a crash that is hard to
    # reproduce).
    mark = len(out)
    qemu.stdin.write(b"mtest crash\r"); qemu.stdin.flush()
    if read_until(b"\n# ", mark) and re.search(rb"page fault at rip 0x[0-9a-f]+, address 0x[0-9a-f]+; killed\]\r\n\[fault diag: page 0x[0-9a-f]+ not mapped, not owned, brk 0x[0-9a-f]+, mmap 0x[0-9a-f]+, parent \d+, \w+ thread\w*, age \d+\]", out[mark:]):
        print("ok   a killed Linux process gets a fault diagnostic line")
    else:
        print("FAIL fault diagnostic:", repr(out[mark:])); fails += 1
    mark = len(out)
    qemu.stdin.write(b"mcloud 10.0.2.2:9\r"); qemu.stdin.flush()
    if read_until(b"\n# ", mark) and b"mcloud: no metadata service" in out[mark:]:
        print("ok   mcloud: no service is reported, not hung on")
    else:
        print("FAIL mcloud without a service:", repr(out[mark:])); fails += 1
    mark = len(out)
    qemu.stdin.write(b"exit\r"); qemu.stdin.flush()
    read_until(b"bastios> ", mark)
    mark = len(out)
    qemu.stdin.write(b"udpsend 10.0.2.2 0 x\r"); qemu.stdin.flush()
    if read_until(b"bastios> ", mark) and b"usage: udpsend" in out[mark:]:
        print("ok   udpsend refuses port 0")
    else:
        print("FAIL udpsend port 0"); fails += 1
    # Every page the finished processes and removed files used must be
    # back in the pool.
    mem = [l.strip() for l in out.split(b"\n") if l.strip().endswith(b"free pages")]
    free = [int(m.split()[0]) for m in mem]
    if len(mem) == 3 and free[1] == free[2] and 0 <= free[0] - free[1] <= 512:
        print("ok   no pages leaked:", mem[2].decode())
    else:
        print("FAIL page leak:", mem); fails += 1
    # Console output goes through a 64 KiB ring that the scheduler pumps to
    # the devices; a writer that finds it full waits (CONWAIT) instead of
    # losing bytes. bench writes 1 MiB of carriage returns, sixteen times
    # the ring: every one must arrive.
    mark = len(out)
    qemu.stdin.write(b"bench\r"); qemu.stdin.flush()
    read_until(b"write 1024 KiB", mark)
    crs = out[mark:].count(b"\r")
    if crs >= 1048576:
        print("ok   1 MiB through the console ring, nothing lost")
    else:
        print(f"FAIL console ring: {crs} of 1048576 bytes arrived"); fails += 1
    read_until(b"bastios> ", mark)
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
def session(name, want_banner, steps, halt=False, ready=None):
    global qemu, out, deadline, fails
    out = b""
    deadline = time.time() + timeout
    if not read_until(b"bastios> "):
        print(f"FAIL: no prompt after the {name} boot"); fails += 1
    else:
        if ready is not None:
            ready()
        if want_banner is not None:
            ok = (b"disk: virtio" in out) == want_banner
            print(("ok  " if ok else "FAIL"), f"({name} boot) disk driver: {'virtio' if want_banner else 'ATA'}")
            fails += not ok
        for step in steps:
            line, want = step[0], step[1]
            until = step[2] if len(step) > 2 else b"bastios> "
            wants = want if isinstance(want, list) else [want]
            mark = len(out)
            qemu.stdin.write(line.encode() + b"\r"); qemu.stdin.flush()
            ok = read_until(until, mark) and all(w in out[mark:] for w in wants)
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

# Every DMA disk signals completions by MSI or MSI-X (src/lapic.bastio),
# so a processor waiting for one halts instead of polling.
def irq_found(name):
    global fails
    ok = b"disk: completions by message-signalled interrupt\r\n" in out
    print(("ok  " if ok else "FAIL"), f"({name} boot) disk completions by interrupt")
    fails += not ok

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
    # The hard link made under ash: removing the first name keeps the file.
    ("cat disk/hk2", b"dh\r\n"),
    ("rm disk/hk1", b""),
    ("cat disk/hk2", b"dh\r\n"),
    ("cat disk/hk1", b"no such file"),
    ("ls disk", b"pf"),
    ("rm disk/pf", b""),
    # The user made at the first boot, kept on the disk.
    ("ash", b"built-in shell (ash)", b"\n# "),
    ("id kept; cat /root/kept; stat -c %a /etc/shadow; deluser kept; rm -rf /home/kept /root/kept /etc/*-", [b"uid=1000(kept)", b"\r\nhi\r\n600\r\n"], b"\n# "),
    # seccomp (src/lxseccomp.bastio): programs that read outside seccomp_data
    # or jump out of themselves are refused at load; filters answer with an
    # errno chosen by call number and argument, stack, are inherited, and
    # TRAP, KILL_PROCESS and strict mode end in SIGSYS or SIGKILL
    # (user/mseccomp.c). The filter pool comes from the kernel's heap at the
    # first filter and stays, so this runs in a boot that has not filled
    # memory (the one with "mkdir full" has no heap to spare).
    ("mseccomp", b"mseccomp: ok", b"\n# "),
    # What container runtimes ask for beyond namespaces (user/mcont.c):
    # close_range, the subreaper and keep-capabilities flags, the ambient
    # set, and quiet ENOSYS for the keyring and the new mount API.
    ("mcont", b"mcont: ok", b"\n# "),
    # Far more open file descriptions than the 1024 the table once held, from several processes (user/mfd.c).
    ("mfd", b"mfd: ok, 8 processes held 1856 descriptions", b"\n# "),
    # More processes at once than the 512 the process table once held (user/mproc.c).
    ("mproc", b"mproc: ok, 700 processes at once", b"\n# "),
    ("exit", b""),
], halt=True, ready=lambda: irq_found("virtio"))

# The same disk as an NVMe drive: what virtio wrote is there, and what is
# written here must be on the medium for the ATA boot below.
qemu = boot(1, nvme=True)
def nvme_found():
    global fails
    ok = b"disk: NVMe\r\n" in out
    print(("ok  " if ok else "FAIL"), "(NVMe boot) disk driver: NVMe")
    fails += not ok
    irq_found("NVMe")
session("NVMe", None, [
    ("cat disk/note", b"kept across boots\r\nappended\r\n"),
    ("echo written by nvme > disk/nv", b""),
    ("cat disk/nv", b"written by nvme\r\n"),
    ("fstest", b"fstest: disk ok"),
    ("rm disk/big", b""),
    ("cat disk/hk2", b"dh\r\n"),
    ("rm disk/hk2", b""),
], halt=True, ready=nvme_found)

# And as a SATA disk on AHCI: what NVMe wrote is there.
qemu = boot(1, ahci=True)
def ahci_found():
    global fails
    ok = b"disk: AHCI\r\n" in out
    print(("ok  " if ok else "FAIL"), "(AHCI boot) disk driver: AHCI")
    fails += not ok
    irq_found("AHCI")
session("AHCI", None, [
    ("cat disk/nv", b"written by nvme\r\n"),
    ("echo and ahci >> disk/nv", b""),
    ("fstest", b"fstest: disk ok"),
    ("rm disk/big", b""),
], halt=True, ready=ahci_found)

# Ports the firmware did not bring up (src/ahci.bastio, port_wake): with
# the boot option ahci_reset every port with a device gets a COMRESET, after
# which a device sends its signature only once FIS receiving is on. A CD
# on port 0 is tried first and let go (an ATAPI signature); the disk on
# port 3 is found and holds what was written above.
cd_img = os.path.join(disk_dir.name, "cd.iso")
with open(cd_img, "wb") as f:
    f.truncate(1 << 20)
qemu = boot(1, extra=["-machine", "q35", "-drive", f"file={disk},format=raw,if=ide,index=3",
                      "-drive", f"file={cd_img},format=raw,media=cdrom,if=ide,index=0", "-append", "ahci_reset"])
def ahci_reset_found():
    global fails
    ok = b"disk: AHCI\r\n" in out
    print(("ok  " if ok else "FAIL"), "(AHCI after COMRESET) disk driver: AHCI, past a CD")
    fails += not ok
session("AHCI after COMRESET", None, [
    ("cat disk/nv", b"written by nvme\r\nand ahci\r\n"),
], halt=True, ready=ahci_reset_found)

# An NVMe drive whose only namespace is 2 (src/nvme.bastio takes the first
# active one; it used to ask for namespace 1 and find no disk).
qemu = boot(1, extra=["-machine", "q35", "-drive", f"file={disk},format=raw,if=none,id=nv2",
                      "-device", "nvme,serial=bastios,id=nvc", "-device", "nvme-ns,drive=nv2,bus=nvc,nsid=2"])
def nvme_ns2_found():
    global fails
    ok = b"disk: NVMe\r\n" in out
    print(("ok  " if ok else "FAIL"), "(NVMe namespace 2) disk driver: NVMe")
    fails += not ok
session("NVMe namespace 2", None, [
    ("cat disk/nv", b"written by nvme\r\nand ahci\r\n"),
], halt=True, ready=nvme_ns2_found)

# And on an NVMe drive with 4 KiB sectors: what AHCI wrote is there, and
# what is written here is where the ATA boot below looks for it.
qemu = boot(1, nvme4k=True)
session("NVMe 4K", None, [
    ("cat disk/nv", b"written by nvme\r\nand ahci\r\n"),
    ("echo and 4k >> disk/nv", b""),
    ("fstest", b"fstest: disk ok"),
    ("diskbench", b"read 2 MiB: "),
    ("rm disk/big", b""),
], halt=True, ready=nvme_found)

# Several processes on the disk at once, on two processors: two
# diskbench (each writes, reads back and checks its own 2 MiB file) and
# fstest. Each sleeps while the disk works for another (src/diskq.bastio);
# all must finish with their data intact, and every page come back.
qemu = boot(2, nvme=True)
out = b""
deadline = time.time() + timeout
ok = read_until(b"bastios> ")
if ok:
    for cmd in (b"diskbench &", b"diskbench &", b"fstest"):
        mark = len(out)
        qemu.stdin.write(cmd + b"\r"); qemu.stdin.flush()
        ok = ok and read_until(b"bastios> ", mark)
    ok = ok and read_until(b"fstest: ok")
    ok = ok and read_until(b"read 2 MiB")
    ok = ok and read_until(b"read 2 MiB", out.find(b"read 2 MiB") + 1)
    ok = ok and b"failed" not in out and b"fstest: disk ok" in out
print(("ok  " if ok else "FAIL"), "(disk at once) two diskbench and fstest together")
fails += not ok
qemu.stdin.write(b"halt\r"); qemu.stdin.flush()
read_until(b"pages free at power off")
booted = re.search(rb"BastiOS 0.2: (\d+) pages of memory", out)
off = re.search(rb"BastiOS: (\d+) pages free at power off", out)
good = booted and off and booted.group(1) == off.group(1)
print(("ok  " if good else "FAIL"), "(disk at once) every page back at power off")
fails += not good
qemu.kill(); qemu.wait()
print("---- disk at once transcript ----")
print(out.decode(errors="replace").replace("\r", "")[-1500:])

# An e1000 card instead of virtio (src/e1000.bastio), on QEMU's network.
# Its link takes a moment to come up, so DHCP may need a retry: ask until
# the lease is there. test/tcp_test.sh runs its TCP cases on it too.
def e1000_ready():
    global fails
    ok = b"net: e1000, mac 52:54:00:12:34:56" in out
    print(("ok  " if ok else "FAIL"), "(e1000 boot) card found")
    fails += not ok
    ok = b"net: frames received by interrupt\r\n" in out
    print(("ok  " if ok else "FAIL"), "(e1000 boot) card interrupts, through the 8259")
    fails += not ok
    t0 = time.time()
    leased = False
    while not leased and time.time() - t0 < 10:
        mark = len(out)
        qemu.stdin.write(b"ifconfig\r"); qemu.stdin.flush()
        leased = read_until(b"bastios> ", mark) and b"10.0.2.15 netmask 255.255.255.0 gateway 10.0.2.2 (DHCP)" in out[mark:]
        if not leased:
            time.sleep(0.5)
    print(("ok  " if leased else "FAIL"), f"(e1000 boot) address by DHCP ({time.time() - t0:.1f}s)")
    fails += not leased
    us = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    us.settimeout(10)
    try:
        us.sendto(b"through the e1000", ("127.0.0.1", udp_port))
        got, _ = us.recvfrom(100)
    except OSError:
        got = None
    us.close()
    print(("ok  " if got == b"through the e1000" else "FAIL"), "(e1000 boot) UDP echo on port 7")
    fails += got != b"through the e1000"
qemu = boot(1, net=True, card="e1000")
session("e1000", None, [
    ("ping", b"reply from 10.0.2.2"),
    ("ping 10.0.2.3", b"reply from 10.0.2.3"),
], halt=True, ready=e1000_ready)

# The same driver on the other families it knows (src/e1000.bastio): the
# 82574L, and the 82576 of the igb family, which takes advanced
# descriptors and its PHY told to negotiate. Each must get a DHCP lease
# and a ping answered.
def family_ready(card):
    def ready():
        global fails
        ok = b"net: e1000, mac 52:54:00:12:34:56" in out
        print(("ok  " if ok else "FAIL"), f"({card} boot) card found")
        fails += not ok
        t0 = time.time()
        leased = False
        while not leased and time.time() - t0 < 10:
            mark = len(out)
            qemu.stdin.write(b"ifconfig\r"); qemu.stdin.flush()
            read_until(b"bastios> ", mark)
            leased = b"(DHCP)" in out[mark:]
            if not leased:
                time.sleep(0.5)
        print(("ok  " if leased else "FAIL"), f"({card} boot) DHCP lease")
        fails += not leased
    return ready
for card in ("e1000e", "igb"):
    qemu = boot(1, net=True, card=card)
    session(card, None, [
        ("ping", b"reply from 10.0.2.2"),
    ], halt=True, ready=family_ready(card))

# An AWS ENA instead (src/ena.bastio). QEMU has no such card: the model in
# tools/qemu-ena stands for it, in the QEMU build that script makes (QEMU_ENA
# names it). The three variants of the model are the two ways registers
# are read (the PCI revision says which) and a device that only has the
# older maximum-queues feature. Each must get a DHCP lease, answer a ping,
# and give every page back at power off.
ena_bin = os.environ.get("QEMU_ENA", "build/qemu-ena/bin/qemu-system-x86_64")
def ena_ready(variant):
    def ready():
        global fails
        ok = b"net: ena, mac 52:54:00:12:34:56" in out
        print(("ok  " if ok else "FAIL"), f"(ena{variant} boot) card found")
        fails += not ok
        t0 = time.time()
        leased = False
        while not leased and time.time() - t0 < 10:
            mark = len(out)
            qemu.stdin.write(b"ifconfig\r"); qemu.stdin.flush()
            read_until(b"bastios> ", mark)
            leased = b"(DHCP)" in out[mark:]
            if not leased:
                time.sleep(0.5)
        print(("ok  " if leased else "FAIL"), f"(ena{variant} boot) DHCP lease")
        fails += not leased
    return ready
if os.path.exists(ena_bin):
    for variant in ("", ",readless=off", ",queues-ext=off"):
        qemu = boot(1, net=True, card="ena" + variant, qemu_bin=ena_bin)
        session("ena" + variant, None, [
            ("ping", b"reply from 10.0.2.2"),
        ], halt=True, ready=ena_ready(variant))
else:
    print("skip (ena) no QEMU with the ENA model at", ena_bin, "(tools/qemu-ena/build.sh builds it)")

# virtio over MMIO (src/virtio_mmio.bastio), as on QEMU's microvm machine
# and Firecracker: no PCI at all. Version 1 is the legacy register layout,
# version 2 the modern one (QEMU offers both). The disk and the network card
# are found at fixed slots; the disk keeps what the earlier boots wrote.
def mmio_ready(version):
    def ready():
        global fails
        ok = b"disk: virtio" in out and b"net: virtio, mac 52:54:00:12:34:56" in out
        print(("ok  " if ok else "FAIL"), f"(virtio-mmio v{version} boot) disk and card found")
        fails += not ok
        # The entropy source is read first and given back, so the disk and
        # the card above still find their slots.
        ok = b"rng: virtio-rng, 64 bytes of entropy" in out
        print(("ok  " if ok else "FAIL"), f"(virtio-mmio v{version} boot) virtio-rng read")
        fails += not ok
        t0 = time.time()
        leased = False
        while not leased and time.time() - t0 < 10:
            mark = len(out)
            qemu.stdin.write(b"ifconfig\r"); qemu.stdin.flush()
            read_until(b"bastios> ", mark)
            leased = b"(DHCP)" in out[mark:]
            if not leased:
                time.sleep(0.5)
        print(("ok  " if leased else "FAIL"), f"(virtio-mmio v{version} boot) DHCP lease")
        fails += not leased
    return ready
for version, legacy in ((1, "on"), (2, "off")):
    qemu = boot(1, extra=["-machine", "microvm", "-global", "virtio-mmio.force-legacy=" + legacy,
                          "-drive", f"file={disk},format=raw,if=none,id=mv",
                          "-device", "virtio-blk-device,drive=mv",
                          "-netdev", "user,id=m0", "-device", "virtio-net-device,netdev=m0",
                          "-object", "rng-random,filename=/dev/urandom,id=r0", "-device", "virtio-rng-device,rng=r0"])
    session(f"virtio-mmio v{version}", None, [
        (f"echo mmio{version} > disk/mm", b""),
        ("cat disk/mm", f"mmio{version}\r\n".encode()),
        ("fstest", b"fstest: disk ok"),
        ("rm disk/mm", b""),
        ("ping", b"reply from 10.0.2.2"),
    ], halt=True, ready=mmio_ready(version))

# A virtio entropy source on PCI beside a virtio disk and card: the kernel
# reads 64 bytes of it before any other driver starts, resets it and gives
# its ports and pages back, so the disk and the card come up as without it.
# A source that never answers only costs the wait for it.
silent = os.path.join(disk_dir.name, "silent")
os.mkfifo(silent)
silent_fd = os.open(silent, os.O_RDWR)    # a writer that writes nothing
for name, source, want in (("answering", "/dev/urandom", True), ("silent", silent, False)):
    qemu = boot(1, virtio=True, net=True, extra=["-object", f"rng-random,filename={source},id=r0",
                                                 "-device", "virtio-rng-pci,rng=r0"])
    def rng_pci(name=name, want=want):
        global fails
        ok = (b"rng: virtio-rng, 64 bytes of entropy" in out) == want and b"disk: virtio" in out and b"net: virtio, mac" in out
        print(("ok  " if ok else "FAIL"), f"(virtio-rng on PCI, {name}) entropy {'read' if want else 'not read'}, disk and card found")
        fails += not ok
    session("virtio-rng " + name, None, [("echo rng_" + "ok", b"rng_ok")], halt=True, ready=rng_pci)
os.close(silent_fd)

# The same device named on the command line, as Firecracker does
# (virtio_mmio.device=SIZE@ADDRESS:IRQ): only the named blocks are looked at.
# QEMU's microvm puts a device added alone in the slot at 0xfeb02e00.
for where, want in (("4K@0xfeb02e00:5", True), ("512@0xfeb00000:5", False)):
    qemu = boot(1, extra=["-machine", "microvm", "-append", "quiet virtio_mmio.device=" + where,
                          "-drive", f"file={disk},format=raw,if=none,id=mv",
                          "-device", "virtio-blk-device,drive=mv"])
    def named(want=want, where=where):
        global fails
        ok = (b"disk: virtio" in out) == want
        print(("ok  " if ok else "FAIL"), f"(virtio-mmio {where}) disk {'found' if want else 'not looked for'}")
        fails += not ok
    session("virtio-mmio " + where, None, [("echo named", b"named")], halt=True, ready=named)

# The PVH entry (boot.S), which Firecracker and QEMU's microvm machine use
# when the image has no Multiboot header: QEMU prefers Multiboot when both
# are there, so the header's magic is zeroed in a copy of the kernel.
pvh_kernel = os.path.join(disk_dir.name, "pvh.elf")
with open(kernel, "rb") as f:
    image = bytearray(f.read())
at = image.find(bytes([0x02, 0xB0, 0xAD, 0x1B]), 0, 8192)
for machine in ("pc", "microvm"):
    if at < 0:
        print("FAIL: (pvh) no Multiboot header in", kernel); fails += 1
        break
    image[at:at + 4] = bytes(4)
    with open(pvh_kernel, "wb") as f:
        f.write(image)
    qemu = boot(1, extra=["-machine", machine], kernel_path=pvh_kernel)
    session("PVH " + machine, None, [
        ("echo pvh_" + "ok", b"pvh_ok"),
    ], halt=True)

# The PS/2 keyboard (src/kbd.bastio): keys pressed through QEMU's monitor
# reach the shell as serial input would, Shift and Backspace included.
# Booted with -kernel there is no framebuffer, so the console stays on the
# serial port alone; test/iso_test.sh checks the screen.
def sendkeys(mon, keys):
    for k in keys:
        mon.sendall(b"sendkey " + k.encode() + b"\n")
        time.sleep(0.05)
mon_path = os.path.join(disk_dir.name, "mon.sock")
qemu = boot(1, monitor=mon_path)
out = b""
deadline = time.time() + timeout
ok = read_until(b"bastios> ")
good = ok and b"screen:" not in out
print(("ok  " if good else "FAIL"), "(keyboard boot) no framebuffer: serial console only")
fails += not good
if ok:
    mon = socket.socket(socket.AF_UNIX)
    mon.connect(mon_path)
    mark = len(out)
    # "echo Typed_abx", Backspace, "c", Enter.
    sendkeys(mon, ["e", "c", "h", "o", "spc", "shift-t", "y", "p", "e", "d", "shift-minus",
                   "a", "b", "x", "backspace", "c", "ret"])
    good = read_until(b"Typed_abc\r\n", mark) and read_until(b"bastios> ", mark + 1)
    print(("ok  " if good else "FAIL"), "(keyboard boot) a line typed on the keyboard runs")
    fails += not good
    mon.close()
qemu.kill(); qemu.wait()
print("---- keyboard boot transcript ----")
print(out.decode(errors="replace").replace("\r", "")[-400:])

# Ctrl-Alt-Delete on the PS/2 keyboard restarts the machine, which is how a
# host (Firecracker's SendCtrlAltDel) asks a guest with no ACPI to stop: with
# -no-reboot, QEMU exits when the guest resets. The kernel says so first.
mon_path = os.path.join(disk_dir.name, "mon_cad.sock")
qemu = boot(1, monitor=mon_path, extra=["-no-reboot"])
out = b""
deadline = time.time() + timeout
ok = read_until(b"bastios> ")
if ok:
    mon = socket.socket(socket.AF_UNIX)
    mon.connect(mon_path)
    mark = len(out)
    sendkeys(mon, ["ctrl-alt-delete"])
    good = read_until(b"ctrl-alt-del: restarting", mark)
    try:
        code = qemu.wait(timeout=15)
    except subprocess.TimeoutExpired:
        code = None
    good = good and code is not None
    print(("ok  " if good else "FAIL"), f"(keyboard boot) Ctrl-Alt-Delete restarts the machine (QEMU status {code})")
    fails += not good
    mon.close()
else:
    print("FAIL (keyboard boot) no prompt before Ctrl-Alt-Delete"); fails += 1
qemu.kill(); qemu.wait()

# No COM1, as on most PCs of the last decade (src/uart.bastio): its ports
# read as 0xFF, which used to look like a byte always waiting, so the
# console read 0xFF forever and the keyboard was never looked at. Now the
# UART is found missing and `halt` typed on the keyboard powers off (S5,
# without isa-debug-exit: QEMU exits 0).
mon_path = os.path.join(disk_dir.name, "mon3.sock")
q = subprocess.Popen(["qemu-system-x86_64", "-kernel", kernel, "-m", "64M", "-display", "none", "-nic", "none",
                      "-serial", "none", "-monitor", f"unix:{mon_path},server,nowait"],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
code = None
for _ in range(100):
    if os.path.exists(mon_path):
        break
    time.sleep(0.1)
try:
    mon = socket.socket(socket.AF_UNIX)
    mon.connect(mon_path)
    # Nothing shows when the shell is up, so type `halt` again every 1.5 s
    # until QEMU exits. The leading Enter ends any line that a copy typed
    # during the boot left half done.
    end = time.time() + 30
    while code is None:
        try:
            sendkeys(mon, ["ret", "h", "a", "l", "t", "ret"])
        except OSError:               # QEMU exited while we typed
            code = q.wait(timeout=5)
            break
        try:
            code = q.wait(timeout=1.5)
        except subprocess.TimeoutExpired:
            if time.time() > end:
                raise
    mon.close()
except (OSError, subprocess.TimeoutExpired):
    q.kill(); q.wait()
good = code == 0
print(("ok  " if good else "FAIL"), f"(no COM1) `halt` typed on the keyboard powers off (status {code})")
fails += not good

# A USB keyboard and a USB mouse on an xHCI controller (src/xhci.bastio),
# on a machine with no PS/2 controller at all (i8042=off), so the keys and
# the moves can only come over USB. They are sent through QMP; at power
# off the controller is reset and its pages are back.
import json
qmp_path = os.path.join(disk_dir.name, "qmp.sock")
def usbmouse(name, qmp_cmd):
    # mmouse reads two packets: a move 10 right and 5 up, then the left
    # button. A USB mouse says y down positive; the PS/2 packet, up. Then
    # it switches to ImPS/2 and reads the wheel turned up one step, which
    # a USB mouse says as +1 and ImPS/2 as -1.
    global fails
    mark = len(out)
    qemu.stdin.write(b"mmouse\r"); qemu.stdin.flush()
    if read_until(b"mmouse: ready", mark):
        qmp_cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "rel", "data": {"axis": "x", "value": 10}}, {"type": "rel", "data": {"axis": "y", "value": -5}}]}})
        time.sleep(0.3)
        for down in (True, False):
            qmp_cmd({"execute": "input-send-event", "arguments": {"events": [
                {"type": "btn", "data": {"down": down, "button": "left"}}]}})
            time.sleep(0.3)
        if read_until(b"mmouse: wheel ready", mark):
            for down in (True, False):
                qmp_cmd({"execute": "input-send-event", "arguments": {"events": [
                    {"type": "btn", "data": {"down": down, "button": "wheel-up"}}]}})
                time.sleep(0.3)
    read_until(b"bastios> ", mark)
    good = b"mmouse: buttons 0 dx 10 dy 5\r\nmmouse: buttons 1 dx 0 dy 0\r\n" in out[mark:]
    print(("ok  " if good else "FAIL"), f"({name} boot) the USB mouse's move and button come through /dev/psaux")
    fails += not good
    good = b"mmouse: wheel ready\r\nmmouse: wheel -1\r\n" in out[mark:]
    print(("ok  " if good else "FAIL"), f"({name} boot) ImPS/2 mode, and the USB mouse's wheel turned up reads -1")
    fails += not good
# A first controller with nothing on it stands for a PC's USB-C one: the
# keyboard and mouse are on the second, which must be found.
qemu = boot(1, extra=["-machine", "pc,i8042=off", "-device", "qemu-xhci,id=empty", "-device", "qemu-xhci,id=xu",
                      "-device", "usb-kbd,bus=xu.0", "-device", "usb-mouse,bus=xu.0",
                      "-qmp", f"unix:{qmp_path},server,nowait"])
out = b""
deadline = time.time() + timeout
ok = read_until(b"bastios> ")
good = ok and b"keyboard: USB (xHCI)\r\n" in out
print(("ok  " if good else "FAIL"), "(USB boot) keyboard found on the xHCI controller")
fails += not good
if ok:
    qmp = socket.socket(socket.AF_UNIX)
    qmp.connect(qmp_path)
    qf = qmp.makefile("rw")
    qf.readline()
    def qmp_cmd(c):
        qf.write(json.dumps(c) + "\n"); qf.flush()
        return qf.readline()
    qmp_cmd({"execute": "qmp_capabilities"})
    def usbkeys(keys):
        for k in keys:
            parts = k.split("-")
            for down in (True, False):
                for q in (parts if down else parts[::-1]):
                    qmp_cmd({"execute": "input-send-event", "arguments": {"events": [
                        {"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": q}}}]}})
                time.sleep(0.03)
    mark = len(out)
    usbkeys(["e", "c", "h", "o", "spc", "shift-u", "s", "b", "shift-minus", "o", "k", "x", "backspace", "ret"])
    good = read_until(b"Usb_ok\r\n", mark) and read_until(b"bastios> ", mark + 1)
    print(("ok  " if good else "FAIL"), "(USB boot) a line typed on the USB keyboard runs")
    fails += not good
    usbmouse("USB", qmp_cmd)
    qemu.stdin.write(b"halt\r"); qemu.stdin.flush()
    read_until(b"pages free at power off")
    booted = re.search(rb"BastiOS 0.2: (\d+) pages of memory", out)
    off = re.search(rb"BastiOS: (\d+) pages free at power off", out)
    good = booted and off and booted.group(1) == off.group(1)
    print(("ok  " if good else "FAIL"), "(USB boot) every page back at power off")
    fails += not good
    qmp.close()
qemu.kill(); qemu.wait()
print("---- USB boot transcript ----")
print(out.decode(errors="replace").replace("\r", "")[-400:])

# A USB stick with a BastiOS partition (tools/mkusb.py --blank: a GPT and
# the partition, no boot image) on the same xHCI controller as the USB
# keyboard: the partition is the disk, ahead of the virtio disk, which must
# be left untouched, and the keyboard is read through the disk's driver.
# The controller's event ring raises V_DISK by MSI-X, so a process waiting
# for the stick sleeps. A second boot finds the file, with the stick and
# the keyboard behind two tiers of USB hubs (as a stick in a real PC's
# front panel often is), and a USB mouse too, which takes the fifth and
# last device slot and is read through the disk's driver as well;
# test/fsck.py then checks the partition.
stick = os.path.join(disk_dir.name, "stick.img")
subprocess.run(["tools/mkusb.py", stick, "--blank", "--mib", "33"], check=True, stdout=subprocess.DEVNULL)
before = open(disk, "rb").read()
def stick_boot(name, steps, keys=False, image=None, hubs=False):
    global qemu, out, deadline, fails
    usb = ["-device", "usb-storage,bus=xh.0,drive=st", "-device", "usb-kbd,bus=xh.0"]
    if hubs:
        usb = ["-device", "usb-hub,bus=xh.0,port=1", "-device", "usb-hub,bus=xh.0,port=1.2",
               "-device", "usb-storage,bus=xh.0,port=1.2.3,drive=st", "-device", "usb-mouse,bus=xh.0,port=1.2.4",
               "-device", "usb-kbd,bus=xh.0,port=1.4"]
    qemu = boot(1, virtio=True, extra=["-machine", "pc,i8042=off", "-device", "qemu-xhci,id=xh",
                                       "-drive", f"file={image or stick},format=raw,if=none,id=st"] + usb +
                                      ["-qmp", f"unix:{qmp_path},server,nowait"])
    out = b""
    deadline = time.time() + timeout
    ok = read_until(b"bastios> ")
    if image:
        good = ok and b"usb: a USB disk without a BastiOS partition; not used\r\n" in out and b"disk: virtio\r\n" in out
    else:
        good = ok and b"disk: USB, the BastiOS partition (67584 sectors)\r\n" in out
        good = good and b"disk: completions by message-signalled interrupt\r\n" in out
    print(("ok  " if good else "FAIL"), f"({name} boot) {'the stick is not used' if image else 'the disk is the stick'}")
    fails += not good
    if ok and keys:
        qmp = socket.socket(socket.AF_UNIX)
        qmp.connect(qmp_path)
        qf = qmp.makefile("rw")
        qf.readline()
        def qmp_cmd(c):
            qf.write(json.dumps(c) + "\n"); qf.flush()
            return qf.readline()
        qmp_cmd({"execute": "qmp_capabilities"})
        mark = len(out)
        for k in ["e", "c", "h", "o", "spc", "shift-s", "t", "i", "c", "k", "ret"]:
            parts = k.split("-")
            for down in (True, False):
                for q in (parts if down else parts[::-1]):
                    qmp_cmd({"execute": "input-send-event", "arguments": {"events": [
                        {"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": q}}}]}})
                time.sleep(0.03)
        good = read_until(b"Stick\r\n", mark) and read_until(b"bastios> ", mark + 1)
        print(("ok  " if good else "FAIL"), f"({name} boot) a line typed on the USB keyboard runs, read through the disk")
        fails += not good
        if hubs:
            usbmouse(name, qmp_cmd)
        qmp.close()
    for line, w in steps:
        if not ok:
            break
        mark = len(out)
        qemu.stdin.write(line.encode() + b"\r"); qemu.stdin.flush()
        good = read_until(b"bastios> ", mark) and w in out[mark:]
        print(("ok  " if good else "FAIL"), f"({name} boot)", repr(line))
        fails += not good
    if ok:
        qemu.stdin.write(b"halt\r"); qemu.stdin.flush()
        read_until(b"pages free at power off")
        booted = re.search(rb"BastiOS 0.2: (\d+) pages of memory", out)
        off = re.search(rb"BastiOS: (\d+) pages free at power off", out)
        good = booted and off and booted.group(1) == off.group(1)
        print(("ok  " if good else "FAIL"), f"({name} boot) every page back at power off")
        fails += not good
    qemu.kill(); qemu.wait()
    if not image:
        good = open(disk, "rb").read() == before
        print(("ok  " if good else "FAIL"), f"({name} boot) the virtio disk is untouched")
        fails += not good
    print(f"---- {name} boot transcript ----")
    print(out.decode(errors="replace").replace("\r", "")[-600:])
stick_boot("USB disk", [
    ("echo on the stick > disk/s", b""),
    ("cat disk/s", b"on the stick\r\n"),
    ("fstest", b"fstest: disk ok"),
    ("rm disk/big", b""),
    ("diskbench", b"read 2 MiB: "),
], keys=True)
stick_boot("USB disk behind hubs", [("cat disk/s", b"on the stick\r\n"), ("fstest", b"fstest: disk ok")],
           keys=True, hubs=True)
f, n = map(int, subprocess.check_output(["tools/mkusb.py", "--find", stick]).split())
part = os.path.join(disk_dir.name, "part.img")
with open(stick, "rb") as src, open(part, "wb") as dst:
    src.seek(f * 512)
    dst.write(src.read(n * 512))
r = subprocess.run(["test/fsck.py", part], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
print(("ok  " if r.returncode == 0 else "FAIL"), "(USB disk) fsck of the partition:", r.stdout.decode().strip())
fails += r.returncode != 0
# A stick with no BastiOS partition is never written to: the virtio disk
# is the disk, and the stick stays all zeros.
bare = os.path.join(disk_dir.name, "bare.img")
with open(bare, "wb") as fb:
    fb.truncate(8 << 20)
stick_boot("bare stick", [], image=bare)
good = not any(open(bare, "rb").read())
print(("ok  " if good else "FAIL"), "(bare stick boot) the stick is untouched")
fails += not good

# And back on ATA: what virtio wrote is on the disk.
qemu = boot(1, with_disk=True)
session("third", False, [
    ("cat disk/note", b"kept across boots\r\nappended\r\n"),
    ("rm disk/note", b""),
    ("cat disk/nv", b"written by nvme\r\nand ahci\r\nand 4k\r\n"),
    ("rm disk/nv", b""),
    ("cat disk/moved", b"z\r\n"),
    ("rm disk/pm", b""),
    ("rm disk/moved", b""),
    ("ls disk/keep", b"\nf\r\n"),
    ("cat disk/keep/f", b"deep\r\n"),
    ("rmdir disk/keep", b"not an empty directory"),
    ("rm disk/keep/f", b""),
    ("rmdir disk/keep", b""),
    ("ls disk/keep", b"not a directory: disk/keep"),
    ("cat disk/long_directory_name_beyond_28_bytes/and_a_file_with_a_long_name_too", b"far\r\n"),
    ("ls", b"       4 disk/long_directory_name_beyond_28_bytes/and_a_file_with_a_long_name_too\r\n"),
    ("rm disk/long_directory_name_beyond_28_bytes/and_a_file_with_a_long_name_too", b""),
    ("rmdir disk/long_directory_name_beyond_28_bytes", b""),
    ("cat disk/note", b"no such file: disk/note"),
])
# Power cuts. user/crashme.bastio checks what the last run left on the disk
# (disk/log a whole number of correct records, disk/snap empty or whole,
# disk/t absent or empty, disk/ow not torn, disk/tr zeros past its cut,
# one of disk/ra and disk/rb, disk/la and disk/lb whole), then keeps
# appending, rewriting, creating, truncating, renaming and linking.
# After each cut, before the next boot mounts it, test/fsck.py checks the
# image itself: every entry, name, parent and block map.
# QEMU is killed part-way through each run, on ATA, virtio, NVMe, AHCI and
# NVMe with 4 KiB sectors in turn; every boot after a cut must find the file system consistent. Killing
# QEMU keeps every sector write that completed (it is in the host's page
# cache), so this cuts power between two sector writes; it cannot tear a
# sector in half.
cuts = [0.05, 0.4, 0.15, 0.9, 0.6, 1.2, 0.3, 0.75, 0.5, 1.0, 0.2, 0.45, 0.7, 0.95, 1.1]
def fsck(when, clean=False):
    global fails
    r = subprocess.run([sys.executable, "test/fsck.py", disk], capture_output=True, text=True)
    last = (r.stdout.strip().splitlines() or ["(no output)"])[-1]
    ok = r.returncode == 0 and (not clean or "fsck: note" not in r.stdout)
    print(("ok  " if ok else "FAIL"), f"fsck {when}:", last)
    if not ok:
        print(r.stdout + r.stderr)
    fails += not ok
for n, delay in enumerate(cuts):
    qemu = boot(1, with_disk=n % 5 == 0, virtio=n % 5 == 1, nvme=n % 5 == 2, ahci=n % 5 == 3, nvme4k=n % 5 == 4)
    out = b""
    deadline = time.time() + timeout
    mark = 0
    ok = read_until(b"bastios> ")
    if ok:
        mark = len(out)
        qemu.stdin.write(b"crashme\r"); qemu.stdin.flush()
        ok = read_until(b" records\r\n", mark) and b"crashme: clean" in out[mark:]
    time.sleep(delay)
    qemu.kill(); qemu.wait()
    line = next((l for l in out[mark:].split(b"\n") if b"crashme:" in l), b"(nothing)")
    print(("ok  " if ok else "FAIL"), f"power cut {n + 1} after {delay} s:", line.strip().decode(errors="replace"))
    fails += not ok
    fsck(f"after power cut {n + 1}")
# A last run that is left to finish, then the files are removed.
qemu = boot(1, with_disk=True)
session("after the power cuts", False, [
    ("crashme", b"crashme: done"),
    ("rm disk/log", b""),
    ("rm disk/snap", b""),
    ("rm disk/ow", b""),
    ("rm disk/tr", b""),
    ("rm disk/ra", b""),
    ("rm disk/rb", b""),
    ("rm disk/la", b""),
    ("rm disk/lb", b""),
    ("ls", b"about"),
], halt=True)
fsck("at the end", clean=True)
# What a power cut inside disk_take_name leaves: a file under the name its
# hard link still has (disk/b), and a link naming a free entry (disk/c).
# The mount drops both links; the file stays, and the image is clean. The
# image is in the older BSTDISK2 format, which the mount upgrades in place.
src = os.path.join(disk_dir.name, "b")
with open(src, "wb") as f:
    f.write(b"kept\n")
subprocess.run([sys.executable, "test/compat/mkdisk.py", disk, src], check=True)
with open(disk, "r+b") as f:
    for slot, name, target in ((1, b"b", 0), (2, b"c", 9)):
        e = bytearray(256)
        e[0], e[1] = 4, len(name)
        e[8:16] = (64).to_bytes(8, "little")
        e[48:56] = target.to_bytes(8, "little")
        e[56:56 + len(name)] = name
        f.seek(4096 + slot * 256)
        f.write(e)
    f.seek(0)
    f.write(b"BSTDISK2")
fsck("of the cut-off link image")
qemu = boot(1, with_disk=True)
session("cut-off link", False, [
    ("cat disk/b", b"kept\r\n"),
    ("ls disk", b"\nb\r\n"),
    ("cat disk/c", b"no such file"),
], halt=True)
fsck("after mounting it", clean=True)
with open(disk, "rb") as f:
    ok = f.read(8) == b"BSTDISK3"
print(("ok  " if ok else "FAIL"), "a BSTDISK2 disk is upgraded to BSTDISK3 at mount")
fails += not ok
# A program on the disk shares its read-only pages through the page cache
# (share_text, src/elf.bastio): the second run maps the pages the first
# read in. mmisc mprotects a page of its constants writable in a child and
# writes it; that must give the child a copy, so the next run (and the
# parent) still read the original. Its code is read from the file only
# when first run (src/lazy.bastio); given its own path, it removes the file
# first and still runs code it had not touched (one page after mprotect),
# which a later exec of the same name then cannot find.
subprocess.run([sys.executable, "test/compat/mkdisk.py", disk, "mx=build/user/mmisc.elf", "mh=build/user/mhello.elf"], check=True)
qemu = boot(2, with_disk=True)
out = b""
deadline = time.time() + timeout
for line, want, until in [
    ("ash", b"built-in shell (ash)", b"\n# "),
    ("/disk/mx", b"mmisc: ok on Linux", b"\n# "),
    ("/disk/mx && /disk/mx && echo twice", b"mmisc: ok on Linux x86_64, signals, pipe 65536\r\ntwice", b"\n# "),
    # Two at once: their code pages come from one page cache entry each,
    # and a fault that waits for the disk sleeps while the other runs.
    # (mmisc makes a file "mm" in its working directory, so each has its own;
    # ash gives a background job /dev/null as stdin, and mmisc checks the tty.)
    ("mkdir /tmp/a /tmp/b; (cd /tmp/a; /disk/mx </dev/tty) & (cd /tmp/b; /disk/mx); wait; echo both", b"pipe 65536\r\nmmisc: ok on Linux x86_64, signals, pipe 65536\r\nboth", b"\n# "),
    ("/disk/mx /disk/mx && echo gone; /disk/mx", [b"mmisc: ok on Linux", b"\r\ngone\r\n", b"not found"], b"\n# "),
    # A new program in the entry mx left: it runs its own code, and its
    # second run maps the pages the first left in the page cache after
    # its last close.
    ("cp /disk/mh /disk/my; /disk/my; /disk/my; echo rc=$?", b"hello from musl: /disk/my, argc 1, 8\r\nhello from musl: /disk/my, argc 1, 8\r\nrc=3", b"\n# "),
    # A new file takes the removed program's entry, and with it the handle
    # its cached code pages are filed under (src/fs.bastio, pc_forget at
    # create): made by extending it, so nothing writes over those pages,
    # it must read as zeros: no newline byte, which the code has.
    ("s=$(stat -c %s /disk/my); rm /disk/my; truncate -s $s /disk/mz; echo nl=$(wc -l < /disk/mz); rm /disk/mz", b"nl=0\r\n", b"\n# "),
]:
    mark = len(out)
    if mark == 0 and not read_until(b"bastios> "):
        print("FAIL: no prompt after the disk-program boot"); fails += 1
        break
    mark = len(out)
    qemu.stdin.write(line.encode() + b"\r"); qemu.stdin.flush()
    ok = read_until(until, mark) and all(w in out[mark:] for w in (want if isinstance(want, list) else [want]))
    print(("ok  " if ok else "FAIL"), "(disk-program boot)", repr(line)); ok or print(repr(out[mark:]))
    fails += not ok
qemu.kill(); qemu.wait()
# A 40 GiB disk (sparse): the file system takes its size from the
# device's block count, so statfs under disk/ reports all of it (40 GiB
# is 10485760 blocks, less the 5 at the start), and the used map, a bit
# per block, fits the kernel's arena. Its directory grows past the 1023
# entries the superblock's first sector has room to name, and a second
# boot finds them.
big = os.path.join(disk_dir.name, "big.img")
# (formatted with user/mbig on it, then made 40 GiB: the file system takes
# the volume's size at mount)
subprocess.run([sys.executable, "test/compat/mkdisk.py", big, "mbig=build/user/mbig.elf"], check=True)
with open(big, "r+b") as f:
    f.truncate(40 << 30)
qemu = boot(1, extra=["-drive", f"file={big},format=raw,if=none,id=bg", "-device", "virtio-blk-pci,drive=bg"])
out = b""
deadline = time.time() + timeout
# The last write waits in a batch for up to 0.1 s before it is committed
# (src/diskfs.bastio), and the image has no `sync` applet, so the line ends
# with a sleep before QEMU is killed.
for line, want, until in [
    ("ash", b"built-in shell (ash)", b"\n# "),
    ("stat -f -c '%b %c' /disk; echo big > /disk/b; cat /disk/b; stat -f -c %f /disk; rm /disk/b", b"10485755 262143\r\nbig\r\n1048", b"\n# "),
    # Files past the 1 GiB a root map reaches: pieces at offsets up to 40 GiB
    # (a super map of root maps, one per GiB), a write across a GiB boundary,
    # cuts and regrowth, and every block freed at the end (user/mbig.c).
    ("/disk/mbig /disk", b"mbig: ok, files past 1 GiB in /disk", b"\n# "),
    ("mkdir disk/many; i=0; while [ $i -lt 1100 ]; do echo $i > disk/many/f$i || break; i=$((i+1)); done; ls disk/many | wc -l; sleep 1", b"\r\n1100\r\n", b"\n# "),
    # One directory takes more entries than the 32767 a disk held before the
    # directory table grew (it holds 262143 now): the creates past that
    # all succeed. (Filling the table would take ten minutes of disk writes.)
    ("mkdir disk/full; i=0; while [ $i -lt 33000 ]; do echo x > disk/full/f$i || break; i=$((i+1)); done; echo x > disk/full/z; echo n$((i == 33000))", b"\r\nn1\r\n", b"\n# "),
]:
    mark = len(out)
    if mark == 0 and not read_until(b"bastios> "):
        print("FAIL: no prompt after the big-disk boot"); fails += 1
        break
    mark = len(out)
    if "disk/full" in line:
        deadline = time.time() + timeout      # 33000 creates, each a disk write: a minute or two
    qemu.stdin.write(line.encode() + b"\r"); qemu.stdin.flush()
    ok = read_until(until, mark) and want in out[mark:]
    print(("ok  " if ok else "FAIL"), "(big-disk boot)", repr(line)); ok or print(repr(out[mark:]))
    fails += not ok
qemu.kill(); qemu.wait()
# The full directory (left for the reboot below) takes more than 512
# directory blocks, which the superblock cannot name: the disk is BSTDISK4,
# or BSTDISK5 here, where mbig made a file with a super map first (the
# extension blocks keep the newer magic).
with open(big, "rb") as f:
    ok = f.read(8) == b"BSTDISK5"
print(("ok  " if ok else "FAIL"), "(big-disk boot) a directory past 512 blocks and a file past 1 GiB make the disk BSTDISK5"); fails += not ok
# A volume that was made larger since the last boot (a cloud volume resized
# while the machine was off): the file system takes the new size at mount, with
# its files in place.
grow = os.path.join(disk_dir.name, "grow.img")
with open(grow, "wb") as f:
    f.truncate(64 << 20)
sizes = []
deadline = time.time() + timeout
for size, line, want in [
    (64 << 20, "ash", b"built-in shell (ash)"),
    (None, "echo kept > /disk/g; stat -f -c %b /disk; sleep 1", b"\r\n"),
    (128 << 20, "ash", b"built-in shell (ash)"),
    (None, "cat /disk/g; stat -f -c %b /disk", b"kept\r\n"),
]:
    if size is not None:
        if size != 64 << 20:
            with open(grow, "r+b") as f:
                f.truncate(size)
        qemu = boot(1, extra=["-drive", f"file={grow},format=raw,if=none,id=gr", "-device", "virtio-blk-pci,drive=gr"])
        out = b""
        if not read_until(b"bastios> "):
            print("FAIL: no prompt after the grown-disk boot"); fails += 1
            break
    mark = len(out)
    qemu.stdin.write(line.encode() + b"\r"); qemu.stdin.flush()
    ok = read_until(b"\n# " if line != "ash" else want, mark) and want in out[mark:]
    if line == "ash":
        read_until(b"\n# ", mark)
    else:
        sizes.append([int(w) for w in out[mark:].split() if w.isdigit()][-1:])
    fails += not ok
    if not ok:
        print("FAIL (grown disk)", repr(line), repr(out[mark:]))
    if line.startswith("cat") or (line.startswith("echo") and False):
        qemu.kill(); qemu.wait()
    elif line.startswith("echo"):
        qemu.kill(); qemu.wait()
if len(sizes) == 2 and sizes[0] and sizes[1] and sizes[1][0] > sizes[0][0]:
    print(f"ok   a disk made larger since the last boot: {sizes[0][0]} blocks became {sizes[1][0]}, with its file")
else:
    print("FAIL a grown disk:", sizes); fails += 1
if smp >= 4:   # (the fake service above is set up on the four-processor run)
    # The boot option `mcloud` runs the same setup at boot, with no
    # argument: against QEMU's address for the metadata service, which a
    # guest forward points at the fake server (the real address is
    # 169.254.169.254:80; QEMU takes a forward only inside its network, so
    # that is made 169.254.0.0/16). It must wait for DHCP, then do its work
    # beside the shell.
    bridge = os.path.join(disk_dir.name, "bridge.py")
    with open(bridge, "w") as f:
        f.write("import os, socket, sys, threading, time\n"
                "s = socket.create_connection(('127.0.0.1', int(sys.argv[1])))\n"
                "def up():\n"
                "    while True:\n"
                "        d = os.read(0, 4096)\n"
                "        if not d: break\n"
                "        s.sendall(d)\n"
                "threading.Thread(target=up, daemon=True).start()\n"
                "while True:\n"
                "    d = s.recv(4096)\n"
                "    if not d: break\n"
                "    os.write(1, d)\n"
                "time.sleep(0.2)\n"
                "os._exit(0)\n")
    imds_log.clear()
    imds = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    imds.bind(("127.0.0.1", 0))
    imds.listen(4)
    th = threading.Thread(target=imds_serve, args=(imds,), daemon=True)
    th.start()
    qemu = boot(1, extra=["-netdev", f"user,id=n1,net=169.254.0.0/16,guestfwd=tcp:169.254.169.254:80-cmd:{sys.executable} {bridge} {imds.getsockname()[1]}",
                          "-device", "virtio-net-pci,netdev=n1", "-append", "mcloud"])
    out = b""
    deadline = time.time() + timeout
    ok = read_until(b"mcloud: user data saved", 0)
    th.join(10)
    imds.close()
    if ok and imds_log == want_log:
        print("ok   boot option mcloud: setup ran at boot, once the network was up")
    else:
        print("FAIL boot option mcloud:", repr(out[-600:]), imds_log); fails += 1
    mark = len(out)
    qemu.stdin.write(b"ash\r"); qemu.stdin.flush()
    read_until(b"\n# ", mark)
    mark = len(out)
    qemu.stdin.write(b"hostname; cat /root/.ssh/authorized_keys\r"); qemu.stdin.flush()
    if read_until(b"\n# ", mark) and b"cloudbox-7.internal" in out[mark:] and imds_key.encode() in out[mark:]:
        print("ok   boot option mcloud: the host name and key are in place")
    else:
        print("FAIL boot option mcloud state:", repr(out[mark:])); fails += 1
    qemu.kill(); qemu.wait()

def reset_deadline():
    global deadline
    deadline = time.time() + timeout

def send_line_ok(line, until):
    mark = len(out)
    qemu.stdin.write(line + b"\r"); qemu.stdin.flush()
    return read_until(until, mark)
qemu = boot(1, extra=["-drive", f"file={big},format=raw,if=none,id=bg", "-device", "virtio-blk-pci,drive=bg"])
out = b""
deadline = time.time() + timeout
ok = (read_until(b"bastios> ") and send_line_ok(b"ash", b"\n# ")
      and send_line_ok(b"ls disk/many | wc -l; cat disk/many/f1099; stat -f -c %c /disk", b"\n# ")
      and b"\r\n1100\r\n1099\r\n262143\r\n" in out
      and (reset_deadline() or True)
      and send_line_ok(b"n=$(ls disk/full | wc -l); echo c$((n > 30000)); cat disk/full/f30000; rm -r disk/full; echo x > disk/after; cat disk/after; rm disk/after; sleep 1", b"\n# ")
      and b"\r\nc1\r\nx\r\nx\r\n" in out)
print(("ok  " if ok else "FAIL"), "(big-disk boot) 1100 entries in one directory after a reboot"); ok or print(repr(out[-500:]))
fails += not ok
qemu.kill(); qemu.wait()
os.unlink(big)
# Two connections opened at once, before the kernel knows the gateway's
# MAC: both SYN+ACKs wait for one ARP answer (send_via holds up to
# HELD_N frames). With only one frame held, the first was dropped and its
# connection waited a second for the host to send its SYN again.
ports = []
for _ in range(2):
    _s = socket.socket(); _s.bind(("127.0.0.1", 0)); ports.append(_s.getsockname()[1]); _s.close()
qemu = boot(2, extra=["-netdev", f"user,id=n1,hostfwd=tcp:127.0.0.1:{ports[0]}-:7101,hostfwd=tcp:127.0.0.1:{ports[1]}-:7102",
                      "-device", "virtio-net-pci,netdev=n1"])
out = b""
deadline = time.time() + timeout
ok = read_until(b"bastios> ")
time.sleep(2)                           # DHCP
for line, want in [("ash", b"\n# "), ("mtcp echo 7101 &", b"listening on tcp port 7101"), ("mtcp echo 7102 &", b"listening on tcp port 7102")]:
    mark = len(out)
    qemu.stdin.write(line.encode() + b"\r"); qemu.stdin.flush()
    ok = ok and read_until(want, mark)
took = []
if ok:
    import threading
    def echo_once(port):
        try:
            t0 = time.time()
            c = socket.create_connection(("127.0.0.1", port), timeout=20)
            c.sendall(b"ping")
            got = c.recv(10)
            took.append((time.time() - t0, got))
            c.close()
        except OSError as e:
            took.append((99, repr(e).encode()))
    ts = [threading.Thread(target=echo_once, args=(p,)) for p in ports]
    [t.start() for t in ts]; [t.join(30) for t in ts]
ok = ok and len(took) == 2 and all(g == b"ping" and t < 0.9 for t, g in took)
print(("ok  " if ok else "FAIL"), "two TCP connections at once before ARP: both answered", [f"{t:.2f} s" for t, _ in took])
fails += not ok
qemu.kill(); qemu.wait()
# ACPI (src/acpi.bastio) on both of QEMU's PCs, the i440fx (pc) and the
# q35, without the isa-debug-exit device and without -no-reboot, so only
# the kernel's own ACPI code can end QEMU: `reboot` restarts the machine
# (a second boot), the power button (QEMU's system_powerdown) powers it off
# through S5, and QEMU exits by itself with status 0. With the HPET turned
# off the clock is the PM timer, and BusyBox's `reboot -f` and
# `poweroff -f` reach the kernel through Linux's reboot(2).
def acpi_boot(machine, name, steps, end):
    global qemu, out, deadline, fails
    mon_path = os.path.join(disk_dir.name, "acpi-mon.sock")
    if os.path.exists(mon_path):
        os.unlink(mon_path)
    qemu = subprocess.Popen(
        ["qemu-system-x86_64", "-kernel", kernel, "-m", "64M", "-smp", "2", "-display", "none",
         "-machine", machine, "-nic", "none", "-serial", "stdio", "-monitor", f"unix:{mon_path},server,nowait"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = b""
    deadline = time.time() + timeout
    ok = read_until(b"bastios> ")
    for line, want in steps:
        if not ok:
            break
        mark = len(out)
        if line == "reboot":
            qemu.stdin.write(b"reboot\r"); qemu.stdin.flush()
            ok = read_until(b"pages free at restart", mark) and read_until(b"bastios> ", mark)
            booted = re.findall(rb"BastiOS 0.2: (\d+) pages of memory", out)
            restart = re.search(rb"BastiOS: (\d+) pages free at restart", out)
            ok = ok and len(booted) == 2 and restart and booted[0] == restart.group(1)
            print(("ok  " if ok else "FAIL"), f"({name}) reboot: every page back, then a second boot")
        else:
            qemu.stdin.write(line.encode() + b"\r"); qemu.stdin.flush()
            ok = read_until(b"# " if line == "ash" else b"bastios> ", mark) and want in out[mark:]
            print(("ok  " if ok else "FAIL"), f"({name})", repr(line))
        fails += not ok
    if ok:
        mark = len(out)
        if end == "button":
            m = socket.socket(socket.AF_UNIX)
            m.connect(mon_path)
            m.sendall(b"system_powerdown\n")
            ok = read_until(b"power button: powering off", mark)
            m.close()
            print(("ok  " if ok else "FAIL"), f"({name}) the power button powers off")
            fails += not ok
        else:
            qemu.stdin.write(end.encode() + b"\r"); qemu.stdin.flush()
        read_until(b"pages free at power off", mark)
    try:
        code = qemu.wait(timeout=max(1, deadline - time.time()))
    except subprocess.TimeoutExpired:
        qemu.kill(); qemu.wait(); code = None
    booted = re.findall(rb"BastiOS 0.2: (\d+) pages of memory", out)
    off = re.search(rb"BastiOS: (\d+) pages free at power off", out)
    good = code == 0 and booted and off and booted[-1] == off.group(1)
    print(("ok  " if good else "FAIL"), f"({name}) every page back, and QEMU powered off by S5 (status {code})")
    fails += not good
    print(f"---- {name} transcript ----")
    print(out.decode(errors="replace").replace("\r", "")[-500:])

def clock_ns_now():
    global out
    mark = len(out)
    qemu.stdin.write(b"clock\r"); qemu.stdin.flush()
    read_until(b"bastios> ", mark)
    m = re.search(rb"(\d+) ns since boot", out[mark:])
    return int(m.group(1)) if m else None

for machine, name in (("pc", "i440fx"), ("q35", "q35")):
    acpi_boot(machine, f"{name} ACPI", [
        ("help", b"  reboot           restart the machine"),
        ("reboot", b""),
    ], "button")
    good = b"acpi: soft power off, power button\r\n" in out
    print(("ok  " if good else "FAIL"), f"({name} ACPI) the boot log has soft power off and the power button")
    fails += not good

# The PM timer as the clock: two readings a second apart (by the host's
# clock; TCG is slow, so the window is wide), on each machine.
for machine, name in (("pc,hpet=off", "i440fx, no HPET"), ("q35,hpet=off", "q35, no HPET")):
    qemu = subprocess.Popen(
        ["qemu-system-x86_64", "-kernel", kernel, "-m", "64M", "-smp", "2", "-display", "none",
         "-machine", machine, "-nic", "none", "-serial", "stdio", "-monitor", "none"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = b""
    deadline = time.time() + timeout
    ok = read_until(b"bastios> ") and b"clock: the ACPI PM timer (no HPET)\r\n" in out
    print(("ok  " if ok else "FAIL"), f"({name}) the clock is the ACPI PM timer")
    fails += not ok
    if ok:
        t0 = clock_ns_now()
        time.sleep(1)
        t1 = clock_ns_now()
        good = t0 is not None and t1 is not None and 500000000 <= t1 - t0 <= 5000000000
        print(("ok  " if good else "FAIL"), f"({name}) a second passes on it: {t0} then {t1} ns")
        fails += not good
        # BusyBox's reboot and poweroff, with -f: reboot(2) straight away.
        mark = len(out)
        qemu.stdin.write(b"ash\r"); qemu.stdin.flush()
        read_until(b"# ", mark)
        mark = len(out)
        qemu.stdin.write(b"reboot -f\r"); qemu.stdin.flush()
        good = read_until(b"pages free at restart", mark) and read_until(b"bastios> ", mark)
        print(("ok  " if good else "FAIL"), f"({name}) reboot -f in ash restarts the machine")
        fails += not good
        mark = len(out)
        qemu.stdin.write(b"ash\r"); qemu.stdin.flush()
        read_until(b"# ", mark)
        qemu.stdin.write(b"poweroff -f\r"); qemu.stdin.flush()
        read_until(b"pages free at power off", mark)
    try:
        code = qemu.wait(timeout=max(1, deadline - time.time()))
    except subprocess.TimeoutExpired:
        qemu.kill(); qemu.wait(); code = None
    print(("ok  " if code == 0 else "FAIL"), f"({name}) poweroff -f powers QEMU off (status {code})")
    fails += code != 0
    print(f"---- {name} transcript ----")
    print(out.decode(errors="replace").replace("\r", "")[-500:])

# The hardware report (boot option diag, src/info.bastio), on a q35 with
# 2 GiB, whose firmware puts the ACPI tables just below 2 GiB: the kernel
# must still find them (the processors, power off). Each pause in the report
# waits for a key, which the serial line gives.
def report_boot(args, name, keys, qemu_bin="qemu-system-x86_64", between=None):
    global qemu, out, deadline
    qemu = subprocess.Popen(
        [qemu_bin, "-kernel", kernel, "-display", "none", "-nic", "none",
         "-serial", "stdio", "-monitor", "none"] + args,
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = b""
    deadline = time.time() + timeout
    for _ in range(keys):
        mark = len(out)
        if not read_until(b"(or wait a minute) --", mark):
            break
        qemu.stdin.write(b" "); qemu.stdin.flush()
    ok = read_until(b"bastios> ")
    if ok and between:
        between()
    mark = len(out)
    qemu.stdin.write(b"halt\r"); qemu.stdin.flush()
    read_until(b"pages free at power off", mark)
    try:
        code = qemu.wait(timeout=max(1, deadline - time.time()))
    except subprocess.TimeoutExpired:
        qemu.kill(); qemu.wait(); code = None
    print(f"---- {name} transcript ----")
    print(out.decode(errors="replace").replace("\r", "")[-1500:])
    return ok, code

ok, code = report_boot(["-machine", "q35", "-m", "2G", "-smp", "2", "-append", "diag"], "hardware report", 3)
text = out.decode(errors="replace").replace("\r", "")
for want, what in (
        ("---- hardware report", "the report starts"),
        ("processors: the MADT lists 2, 2 running", "the MADT, below 2 GiB, lists both processors"),
        ("acpi: soft power off, power button", "the FADT below 2 GiB gives power off"),
        ("acpi: tables FACP@0x7f", "the tables are listed where they are"),
        ("timer: the local APIC timer, counted against the HPET", "the timer's calibration"),
        ("tick: the PIT\n", "the PIT ticks"),
        ("idle: hlt; the other processors' timers stop while they idle", "an idle processor stops its timer"),
        ("keyboard: PS/2 (no USB keyboard)", "the PS/2 keyboard"),
        ("MiB usable in a map of", "the memory map"),
        ("pci 00:1f.2 8086:2922 class 010601 AHCI disk", "the PCI devices, with their drivers"),
        ("acpi: reset register: port 0xcf9", "the reset register"),
        ("---- end of the hardware report", "the report ends")):
    good = ok and want in text
    print(("ok  " if good else "FAIL"), f"(hardware report) {what}")
    fails += not good
booted = re.findall(r"BastiOS 0.2: (\d+) pages of memory", text)
off = re.search(r"BastiOS: (\d+) pages free at power off", text)
good = code == 0 and booted and off and booted[-1] == off.group(1)
print(("ok  " if good else "FAIL"), f"(hardware report) every page back, and S5 (status {code})")
fails += not good
ok, code = report_boot(["-machine", "pc", "-m", "64M"], "no report", 0)
good = ok and b"hardware report" not in out
print(("ok  " if good else "FAIL"), "(hardware report) only with diag")
fails += not good
good = ok and b"disk: none found" in out and b"net: none" in out
print(("ok  " if good else "FAIL"), "(hardware report) a boot without a disk or network card says so, even without diag")
fails += not good

# Sound (src/hda.bastio, src/lxsnd.bastio): an HD Audio controller with
# QEMU's output codec, whose sound QEMU writes to a WAV file. mtone plays
# half a second of a 1 kHz square wave through /dev/dsp: the file must hold
# exactly that, 500 periods of 24 frames up and 24 down, nothing lost or
# played twice.
import wave
wav = os.path.join(disk_dir.name, "sound.wav")
def play():
    global out
    mark = len(out)
    qemu.stdin.write(b"mtone\r"); qemu.stdin.flush()
    read_until(b"bastios> ", mark)
ok, code = report_boot(["-machine", "pc", "-m", "64M",
                        "-audiodev", f"wav,id=snd0,path={wav},out.frequency=48000,out.channels=2,out.format=s16",
                        "-device", "intel-hda", "-device", "hda-output,audiodev=snd0"], "sound", 0, between=play)
text = out.decode(errors="replace").replace("\r", "")
good = ok and "sound: HD Audio, 1 output\n" in text
print(("ok  " if good else "FAIL"), "(sound) the HD Audio controller and its output are found")
fails += not good
good = ok and "mtone: ok, 96000 bytes" in text
print(("ok  " if good else "FAIL"), "(sound) mtone writes half a second to /dev/dsp")
fails += not good
runs = []
try:
    w = wave.open(wav)
    frames = w.readframes(w.getnframes())
    left = [int.from_bytes(frames[i:i + 2], "little", signed=True) for i in range(0, len(frames), 4)]
    loud = [i for i, v in enumerate(left) if v != 0]
    if loud:
        for v in left[loud[0]:loud[-1] + 1]:
            if runs and runs[-1][0] == v:
                runs[-1][1] += 1
            else:
                runs.append([v, 1])
except (OSError, EOFError, wave.Error):
    pass
good = len(runs) == 1000 and all(n == 24 and v == (8000 if i % 2 == 0 else -8000) for i, (v, n) in enumerate(runs))
print(("ok  " if good else "FAIL"), f"(sound) QEMU played the square wave whole ({len(runs)} half periods)")
fails += not good
booted = re.findall(r"BastiOS 0.2: (\d+) pages of memory", text)
off = re.search(r"BastiOS: (\d+) pages free at power off", text)
good = code == 0 and booted and off and booted[-1] == off.group(1)
print(("ok  " if good else "FAIL"), f"(sound) every page back at power off (status {code})")
fails += not good

# The PS/2 mouse (src/kbd.bastio, src/lxmouse.bastio): QEMU's monitor
# moves it 10 right and 5 up, then presses the left button, while mmouse
# reads /dev/psaux; before that it checks that /dev/input/mice opened
# O_NONBLOCK has nothing to read and polls empty. The kernel finds the
# wheel QEMU's PS/2 mouse has (the IntelliMouse sequence); mmouse turns
# ImPS/2 on and reads the wheel turned up (`mouse_move 0 0 1`).
mon = os.path.join(disk_dir.name, "mouse.sock")
def move():
    global out
    mark = len(out)
    qemu.stdin.write(b"mmouse\r"); qemu.stdin.flush()
    if not read_until(b"mmouse: ready", mark):
        return
    try:
        m = socket.socket(socket.AF_UNIX)
        m.connect(mon)
        m.sendall(b"mouse_move 10 -5\n")
        time.sleep(0.3)
        m.sendall(b"mouse_button 1\n")
        time.sleep(0.3)
        m.sendall(b"mouse_button 0\n")
        read_until(b"mmouse: wheel ready", mark)
        m.sendall(b"mouse_move 0 0 1\n")       # the monitor's dz 1 is the wheel turned up
        time.sleep(0.3)
        m.close()
    except OSError:
        pass
    read_until(b"bastios> ", mark)
ok, code = report_boot(["-machine", "pc", "-m", "64M", "-append", "diag", "-monitor", f"unix:{mon},server,nowait"],
                       "mouse", 3, between=move)
text = out.decode(errors="replace").replace("\r", "")
for want, what in (
        ("mouse: PS/2, with a wheel\n", "the report finds the PS/2 mouse, and its wheel"),
        ("mmouse: ready", "nothing to read before the mouse moves, O_NONBLOCK and poll"),
        ("mmouse: buttons 0 dx 10 dy 5\n", "a move comes through as one packet, y up positive"),
        ("mmouse: buttons 1 dx 0 dy 0\n", "the left button comes through"),
        ("mmouse: wheel ready\nmmouse: wheel -1\n", "ImPS/2 mode, and the wheel turned up reads -1")):
    good = ok and want in text
    print(("ok  " if good else "FAIL"), f"(mouse) {what}")
    fails += not good

# No i8042 at all (many recent PCs, with only USB keyboards): its ports
# read 0xFF, which src/kbd.bastio took as a mouse byte always waiting, and
# polled for on every turn. Now the keyboard is left alone, and the report
# says so; the serial line still works.
def no_mouse():
    global out
    mark = len(out)
    qemu.stdin.write(b"mmouse\r"); qemu.stdin.flush()
    read_until(b"bastios> ", mark)
ok, code = report_boot(["-machine", "pc,i8042=off", "-m", "64M", "-append", "diag"], "no i8042", 3, between=no_mouse)
good = ok and b"mouse: none found" in out and b"mmouse: no mouse (ENODEV)" in out
print(("ok  " if good else "FAIL"), "(no i8042) no mouse: the report says so, /dev/input/mice says ENODEV")
fails += not good
good = ok and b"keyboard: none found (no USB keyboard, no PS/2 controller)" in out and code == 0
print(("ok  " if good else "FAIL"), f"(no i8042) no keyboard found, the shell works, S5 (status {code})")
fails += not good

# Time without the usual timers. A PIT whose clock is gated off (many
# recent chipsets: Intel's "8254 clock gating"; QEMU's pit=off has none at
# all): the bootstrap processor ticks on the local APIC timer instead
# (boot/smp.c); before, no tick ever came and uptime stayed at 0. No HPET
# and no ACPI PM timer (many recent PCs turn both off; here acpi=off, which
# also takes the MADT, and with it the other processor, and S5 away, so the
# kernel ends through isa-debug-exit, status 33): boot/smp.c measures the
# time-stamp counter against PIT channel 2, else against a second of the
# RTC, and the clock runs on that; before, it stayed at 0 ns.
exit_dev = ["-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"]
for machine, name, extra, status, wants in (
        ("q35,pit=off", "no PIT", [], 0, (
            ("timer: the local APIC timer, counted against the HPET", "the timer is counted against the HPET"),
            ("tick: the local APIC timer (the PIT does not count)", "the report says the local APIC timer ticks"))),
        ("pc,acpi=off,hpet=off", "no HPET or PM timer", exit_dev, 33, (
            ("timer: the local APIC timer, counted against the PIT", "the timer is counted against the PIT"),
            ("tick: the PIT\n", "the PIT ticks"),
            ("clock: the time-stamp counter at", "the clock is the TSC"),
            ("kHz, as measured against the PIT", "its rate measured against the PIT"))),
        # No ACPI: halting only stops the processor, so QEMU is killed (status None).
        ("microvm", "microvm", [], None, (
            ("timer: the local APIC timer, counted against the PIT", "the timer is counted against the PIT, not the RTC"),)),
        ("pc,acpi=off,hpet=off,pit=off", "no HPET, PM timer or PIT", exit_dev, 33, (
            ("timer: the local APIC timer, counted against the RTC", "the timer is counted against the RTC"),
            ("tick: the local APIC timer (the PIT does not count)", "the local APIC timer ticks"),
            ("kHz, as measured against the RTC", "the TSC's rate measured against the RTC")))):
    marks = []
    def measure():
        t0, c0, w0 = uptime(), clock(), time.time()
        time.sleep(2)
        t1, c1, w1 = uptime(), clock(), time.time()
        marks.extend([t0, t1, c0, c1, w1 - w0])
    ok, code = report_boot(["-machine", machine, "-m", "64M", "-append", "diag"] + extra, name, 3, between=measure)
    text = out.decode(errors="replace").replace("\r", "")
    for want, what in wants:
        good = ok and want in text
        print(("ok  " if good else "FAIL"), f"({name}) {what}")
        fails += not good
    good = len(marks) == 5 and None not in marks and 60 * marks[4] <= marks[1] - marks[0] <= 200 * marks[4]
    print(("ok  " if good else "FAIL"), f"({name}) the timer ticks: {marks[:2]}")
    fails += not good
    good = len(marks) == 5 and None not in marks and 0.6e9 * marks[4] <= marks[3] - marks[2] <= 1.5e9 * marks[4]
    print(("ok  " if good else "FAIL"), f"({name}) the clock runs: {marks[2:4]}")
    fails += not good
    good = code == status
    print(("ok  " if good else "FAIL"), f"({name}) halt ends QEMU (status {code})")
    fails += not good

# x2APIC (boot/smp.c). SeaBIOS leaves the local APICs in x2APIC mode when
# the machine may have APIC ids of 255 and up (sockets of 256 cores): with
# only low ids present the kernel goes back to xAPIC, which its own APIC
# code drives; with a processor of id 256 (socket 1) it stays in x2APIC
# mode and starts that one too. Before, the kernel hung at boot in both.
# QEMU emulates x2APIC without KVM only from 9.0: QEMU_X2APIC names such a
# qemu-system-x86_64, else the one on the path is used if it is new enough.
def qemu_major(q):
    try:
        v = subprocess.run([q, "--version"], capture_output=True, text=True).stdout
    except OSError:
        return 0
    m = re.search(r"version (\d+)\.", v)
    return int(m.group(1)) if m else 0
qemu9 = os.environ.get("QEMU_X2APIC") or "qemu-system-x86_64"
if qemu_major(qemu9) < 9:
    print("skip (x2APIC) needs QEMU 9.0 or later: set QEMU_X2APIC")
else:
    many = ["-machine", "q35", "-m", "64M", "-cpu", "qemu64,+x2apic", "-smp", "2,sockets=2,cores=256,maxcpus=512", "-append", "diag"]
    for extra, name, checks in (
            ([], "x2APIC firmware", (
                ("pages of memory, 2 processors", "both processors start"),
                ("local APIC: xAPIC; the firmware left it in x2APIC mode", "the kernel takes the local APIC back to xAPIC"))),
            (["-device", "qemu64-x86_64-cpu,socket-id=1,core-id=0,thread-id=0"], "x2APIC id 256", (
                ("pages of memory, 3 processors", "the processor with APIC id 256 starts too"),
                ("the MADT lists 2 (and 1 x2APIC entries), 3 running", "the MADT's x2APIC entry is counted"),
                ("local APIC: x2APIC, for ids of 255 and up", "the local APICs stay in x2APIC mode"),
                ("idle: hlt; every processor's timer keeps ticking", "without wake-up interrupts the timers keep ticking")))):
        ok, code = report_boot(many + extra, name, 3, qemu9)
        text = out.decode(errors="replace").replace("\r", "")
        for want, what in checks:
            good = ok and want in text
            print(("ok  " if good else "FAIL"), f"({name}) {what}")
            fails += not good
        booted = re.findall(r"BastiOS 0.2: (\d+) pages of memory", text)
        off = re.search(r"BastiOS: (\d+) pages free at power off", text)
        good = code == 0 and booted and off and booted[-1] == off.group(1)
        print(("ok  " if good else "FAIL"), f"({name}) every page back, and S5 (status {code})")
        fails += not good

# More than 4 GiB of RAM: the page pool takes every run of the memory map
# (3 GiB below QEMU's PCI hole, 3 GiB above 4 GiB), boot/multiboot.c maps
# the RAM above 2 GiB write-back, and the kernel writes and reads back the
# last page of RAM at boot. A processor without 1 GiB pages has nothing
# mapped above 4 GiB, so there the pool stops at the hole.
for name, cpu, high in (("6 GiB", "qemu64,+pdpe1gb", True), ("6 GiB, no 1 GiB pages", "qemu64", False)):
    ok, code = report_boot(["-m", "6G", "-cpu", cpu], name, 0)
    m = re.search(rb"BastiOS 0.2: (\d+) pages of memory", out)
    pages = int(m.group(1)) if m else 0
    good = ok and pages > (2 << 18) and (pages > (5 << 18)) == high and b"does not read back" not in out
    print(("ok  " if good else "FAIL"), f"({name}) the pool has {pages} pages ({pages >> 8} MiB)")
    fails += not good
    off = re.search(rb"BastiOS: (\d+) pages free at power off", out)
    good = code == 0 and off and int(off.group(1)) == pages
    print(("ok  " if good else "FAIL"), f"({name}) every page back, and S5 (status {code})")
    fails += not good

# More processors than the 16 the kernel once had room for: each
# application processor's block comes out of the memory map
# (boot/multiboot.c's bst_boot_take), so the pool shrinks by 128 KiB for
# each one, and every page of it still comes back at power off.
ok, code = report_boot(["-m", "256M", "-smp", "24"], "24 processors", 0)
good = ok and b"pages of memory, 24 processors" in out
print(("ok  " if good else "FAIL"), "(24 processors) all 24 start")
fails += not good
m = re.search(rb"BastiOS 0.2: (\d+) pages of memory", out)
off = re.search(rb"BastiOS: (\d+) pages free at power off", out)
good = code == 0 and m and off and m.group(1) == off.group(1)
print(("ok  " if good else "FAIL"), f"(24 processors) every page back, and S5 (status {code})")
fails += not good

# 255 processors, the most the xAPIC's eight-bit ids can address (a
# processor beyond that needs x2APIC mode, which QEMU emulates without KVM
# only from 9.0, so the 256th is out of reach of this test): every one
# starts and reports itself, and every page comes back at power off.
ok, code = report_boot(["-m", "512M", "-smp", "255"], "255 processors", 0)
good = ok and b"pages of memory, 255 processors" in out
print(("ok  " if good else "FAIL"), "(255 processors) all 255 start")
fails += not good
m = re.search(rb"BastiOS 0.2: (\d+) pages of memory", out)
off = re.search(rb"BastiOS: (\d+) pages free at power off", out)
good = code == 0 and m and off and m.group(1) == off.group(1)
print(("ok  " if good else "FAIL"), f"(255 processors) every page back, and S5 (status {code})")
fails += not good

# A disk that is not blank and has no BastiOS file system (an MBR's
# signature; or only a GPT's backup header at the end) is someone else's:
# it is not formatted, and not a byte of it changes.
for name, at, data in (("an MBR", 510, b"\x55\xaa"), ("a backup GPT header", (33 << 20) - 512, b"EFI PART")):
    other = os.path.join(disk_dir.name, "other.img")
    with open(other, "wb") as f:
        f.truncate(33 << 20)
        f.seek(at)
        f.write(data)
    before = open(other, "rb").read()
    ok, code = report_boot(["-m", "64M", "-drive", f"file={other},format=raw,if=none,id=od",
                            "-device", "virtio-blk-pci,drive=od"], f"disk with {name}", 0)
    good = ok and b"disk: it holds a partition table or other data; left alone" in out
    print(("ok  " if good else "FAIL"), f"(foreign disk) a disk with {name} is left alone")
    fails += not good
    good = open(other, "rb").read() == before
    print(("ok  " if good else "FAIL"), f"(foreign disk) not a byte of the disk with {name} changed")
    fails += not good

# With the boot option `login`, the console is BusyBox's login instead of
# the native shell, and a new login starts whenever the last one ends
# (relogin, src/main.bastio). Root has no password here (passwd -d above).
qemu = boot(1, with_disk=True, extra=["-append", "login"])
out = b""
deadline = time.time() + timeout
def login_step(line, want, until):
    global fails
    mark = len(out)
    qemu.stdin.write(line.encode() + b"\r"); qemu.stdin.flush()
    wants = want if isinstance(want, list) else [want]
    ok = read_until(until, mark + len(line)) and all(w in out[mark:] for w in wants)
    print(("ok  " if ok else "FAIL"), "(login boot)", repr(line)); ok or print(repr(out[mark:]))
    fails += not ok
if not read_until(b"login: "):
    print("FAIL (login boot) no login prompt"); fails += 1
else:
    print("ok   (login boot) a login prompt at boot")
    login_step("root", b"", b"\n# ")
    login_step("id -u; adduser -D carol; echo carol:pw | chpasswd; exit", b"\r\n0\r\n", b"login: ")
    login_step("carol", b"", b"Password: ")
    login_step("wrong", b"Login incorrect", b"login: ")
    login_step("carol", b"", b"Password: ")
    login_step("pw", b"", b"\n$ ")
    login_step("id -u; cat /etc/shadow; exit", [b"\r\n1000\r\n", b"Permission denied"], b"login: ")
    login_step("root", b"", b"\n# ")
    login_step("deluser carol; rm -rf /home/carol /etc/passwd- /etc/group- /etc/shadow-; id carol", b"unknown user carol", b"\n# ")
qemu.kill(); qemu.wait()

print(f"---- {fails} failed ----")
sys.exit(1 if fails else 0)
PY
