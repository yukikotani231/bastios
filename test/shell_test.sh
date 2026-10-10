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
import os, re, subprocess, sys, tempfile, time, select

kernel, timeout, smp = sys.argv[1], float(sys.argv[2]), int(sys.argv[3])

# A blank disk for this run (sparse, so it costs nothing): BastiOS formats it
# on the first boot that sees it, and a later boot must find the files.
disk_dir = tempfile.TemporaryDirectory()
disk = os.path.join(disk_dir.name, "disk.img")
with open(disk, "wb") as f:
    f.truncate(33 << 20)

def boot(cpus, with_disk=False, virtio=False):
    drive = ["-drive", f"file={disk},format=raw,if=ide,index=0"] if with_disk else []
    if virtio:
        # The same disk as a virtio block device (src/virtio.bastio): sectors
        # move by DMA through pages the kernel lends it.
        drive = ["-drive", f"file={disk},format=raw,if=none,id=vd",
                 "-device", "virtio-blk-pci,drive=vd"]
    return subprocess.Popen(
        ["qemu-system-x86_64", "-kernel", kernel, "-m", "64M", "-smp", str(cpus),
         "-display", "none", "-no-reboot", "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
         "-serial", "stdio", "-monitor", "none"] + drive,
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
qemu.kill(); qemu.wait()
print("---- one-processor transcript ----")
print(out.decode(errors="replace").replace("\r", ""))

# The rest runs on `smp` processors: every processor runs the scheduler, so
# the shell and its programs run on whichever one picks them.
qemu = boot(smp, with_disk=True)
out = b""

# (input line, expected text in the reply). The shell is a user process;
# the later steps exercise process creation, waiting, preemption, killing
# and fault isolation.
steps = [
    ("help", b"commands:"),
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
    ("mtest", [b"mtest: malloc heap 75", b"mtest: file line 42"], b"type a line"),
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

qemu = boot(1, virtio=True)
session("virtio", True, [
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
