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

def boot(cpus, with_disk=False, virtio=False, nvme=False, nvme4k=False, ahci=False, net=False, subnet=None, card="virtio-net-pci", monitor=None, extra=()):
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
    return subprocess.Popen(
        ["qemu-system-x86_64", "-kernel", kernel, "-m", "64M", "-smp", str(cpus),
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
    # Time: clocks, nanosleep, poll that waits, select, alarms and pause, and
    # eventfd, timerfd and epoll (user/mtime.c); the sleep applet; ash's
    # read -t, a poll on the console that times out, then one that wakes
    # when a line is typed.
    ("mtime", [b"mtime: rang 3, selected, slept ", b"woke in time, events ok"], b"\n# "),
    ("sleep 1; echo slept$((1+1))", b"\r\nslept2\r\n", b"\n# "),
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
    # Owners and the umask (src/lxperm.bastio): a new file takes the umask,
    # chown keeps its owner, on the disk too.
    ("umask 077; echo x > po; chown 65534:100 po; stat -c 'p%a %u %g' po; rm po; umask 022; echo y > /disk/po; chown 7:8 /disk/po; stat -c 'd%a %u %g' /disk/po; rm /disk/po",
     [b"p600 65534 100", b"d644 7 8"], b"\n# "),
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
    ("cat /etc/passwd", b"root:x:0:0", b"\n# "),
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
    # The disk's directory grows past its first 64 entries (dir_grow); the
    # power cuts below check that what it grew survives.
    ("mkdir disk/many; i=0; while [ $i -lt 150 ]; do echo $i > disk/many/f$i || break; i=$((i+1)); done; ls disk/many | wc -l; cat disk/many/f149", b"\r\n150\r\n149\r\n", b"\n# "),
    ("rm -r disk/many; ls disk/many; echo gone$((1+1))", [b"No such file", b"gone2"], b"\n# "),
    # /proc, generated by the kernel: the mount table, memory, uptime, a
    # directory per process with its status, and /proc/self/exe as a link.
    ("cat /proc/mounts", b"rootfs / ramfs rw 0 0\r\nproc /proc proc rw 0 0\r\n", b"\n# "),
    ("ls /proc; stat -c %F /proc/self", [b"meminfo", b"uptime", b"directory"], b"\n# "),
    ("cat /proc/meminfo; read up idle < /proc/uptime && echo up=${up%.*}", [b"MemTotal: 262144 kB", b"MemFree: ", b"up="], b"\n# "),
    ("cat /proc/$$/status; set -- $(cat /proc/$$/stat); [ $1 = $$ ] && echo stat=$2", [b"Name:\tash", b"PPid:\t", b"stat=(ash)"], b"\n# "),
    ("ls -l /proc/self/exe; echo nope > /proc/mounts; echo w=$?", [b"-> /bin/", b"Permission denied"], b"\n# "),
    # What Linux programs look for besides: processors, the file system
    # types, a process's own mount table, and three fixed sysctls.
    ("n=0; while read k r; do [ \"$k\" = processor ] && n=$((n+1)); done < /proc/cpuinfo; echo cpus=$n; cat /proc/filesystems; cat /proc/self/mounts", [f"cpus={smp}\r\n".encode(), b"nodev\tramfs", b"nodev\ttmpfs", b"\tbstdisk3", b"proc /proc proc"], b"\n# "),
    ("cat /proc/sys/kernel/tainted /proc/sys/kernel/pid_max; ls /proc/self", [b"\r\n0\r\n32\r\n", b"mounts"], b"\n# "),
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
        time.sleep(0.5)
        m2 = len(out)
        qemu.stdin.write(b"ps\r"); qemu.stdin.flush()
        # ps's own line names mtcp; wait for the prompt after it (the one
        # `mtcp &` printed can arrive after m2, under SMP).
        if (read_until(b" mtcp", m2) and read_until(b"bastios> ", out.index(b" mtcp", m2))
                and re.search(rb"socket\s+\d+\s+mtcp", out[m2:])):
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
    time.sleep(8)                     # the boot, which nothing shows
    sendkeys(mon, ["h", "a", "l", "t", "ret"])
    code = q.wait(timeout=30)
    mon.close()
except (OSError, subprocess.TimeoutExpired):
    q.kill(); q.wait()
good = code == 0
print(("ok  " if good else "FAIL"), f"(no COM1) `halt` typed on the keyboard powers off (status {code})")
fails += not good

# A USB keyboard on an xHCI controller (src/xhci.bastio), on a machine with
# no PS/2 controller at all (i8042=off), so the keys can only come over
# USB. They are sent through QMP; at power off the controller is reset and
# its fourteen pages are back.
import json
qmp_path = os.path.join(disk_dir.name, "qmp.sock")
qemu = boot(1, extra=["-machine", "pc,i8042=off", "-device", "qemu-xhci", "-device", "usb-kbd",
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
# front panel often is); test/fsck.py then checks the partition.
stick = os.path.join(disk_dir.name, "stick.img")
subprocess.run(["tools/mkusb.py", stick, "--blank", "--mib", "33"], check=True, stdout=subprocess.DEVNULL)
before = open(disk, "rb").read()
def stick_boot(name, steps, keys=False, image=None, hubs=False):
    global qemu, out, deadline, fails
    usb = ["-device", "usb-storage,bus=xh.0,drive=st", "-device", "usb-kbd,bus=xh.0"]
    if hubs:
        usb = ["-device", "usb-hub,bus=xh.0,port=1", "-device", "usb-hub,bus=xh.0,port=1.2",
               "-device", "usb-storage,bus=xh.0,port=1.2.3,drive=st", "-device", "usb-kbd,bus=xh.0,port=1.4"]
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

# No i8042 at all (many recent PCs, with only USB keyboards): its ports
# read 0xFF, which src/kbd.bastio took as a mouse byte always waiting, and
# polled for on every turn. Now the keyboard is left alone, and the report
# says so; the serial line still works.
ok, code = report_boot(["-machine", "pc,i8042=off", "-m", "64M", "-append", "diag"], "no i8042", 3)
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
                ("local APIC: x2APIC, for ids of 255 and up", "the local APICs stay in x2APIC mode")))):
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

print(f"---- {fails} failed ----")
sys.exit(1 if fails else 0)
PY
