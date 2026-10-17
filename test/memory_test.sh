#!/usr/bin/env bash
# What BastiOS does when it runs out of memory: boots a small machine (64 MiB
# of RAM, about 30 MiB of it for programs), runs user/mhog.c on it, and checks
#   - the page cache goes back before anything is killed (reclaim),
#   - the out-of-memory killer picks the process with the highest oom_score,
#     says so on the console, and the system carries on,
#   - oom_score_adj moves the choice, and /proc/vmstat counts the kills,
#   - /proc/meminfo counts the cache as available,
#   - with a swap file on, a program bigger than the free memory runs, gets
#     back what it wrote, and swapoff brings every page home.
# ARCH=aarch64 runs the same checks on the AArch64 port (make ARCH=aarch64) on QEMU's
# virt machine, with 96 MiB of RAM.
# Input is sent a line at a time, each after the prompt, as test/shell_test.sh does.
set -u
cd "$(dirname "$0")/.."
ARCH=${ARCH:-x86_64}
if [ "$ARCH" = aarch64 ]; then
  KERNEL=${KERNEL:-build/aarch64/bastios.bin}
  MEM=${MEM:-96M}
  UPROG=build/aarch64/user/mhog.elf
  if ! command -v qemu-system-aarch64 >/dev/null; then
    echo "SKIP memory aarch64: needs qemu-system-aarch64"
    [ "${AARCH64_REQUIRED:-}" = 1 ] && exit 1
    exit 0
  fi
  make -f arch/aarch64/Makefile BASTIOC=build/bastioc1 -s "$UPROG" || exit 1
else
  KERNEL=${KERNEL:-build/bastios.elf}
  MEM=${MEM:-64M}
  UPROG=build/user/mhog.elf
fi
TIMEOUT=${TIMEOUT:-300}
SMP=${SMP:-1}

python3 - "$KERNEL" "$TIMEOUT" "$MEM" "$SMP" "$ARCH" "$UPROG" <<'PY'
import os, re, select, subprocess, sys, tempfile, time

kernel, timeout, mem, smp, arch, uprog = sys.argv[1], float(sys.argv[2]), sys.argv[3], sys.argv[4], sys.argv[5], sys.argv[6]
tmp = tempfile.TemporaryDirectory()
img = os.path.join(tmp.name, "disk.img")
subprocess.run([sys.executable, "test/compat/mkdisk.py", img, "mhog=" + uprog], check=True,
               env=dict(os.environ, MKDISK_FREE_MB="64"))

if arch == "aarch64":
    cmd = ["qemu-system-aarch64", "-M", "virt,gic-version=3", "-cpu", "cortex-a72", "-smp", smp, "-m", mem,
           "-nographic", "-no-reboot", "-kernel", kernel,
           "-drive", f"file={img},format=raw,if=none,id=vd", "-device", "virtio-blk-device,drive=vd",
           "-serial", "stdio", "-monitor", "none"]
else:
    cmd = ["qemu-system-x86_64", "-kernel", kernel, "-m", mem, "-smp", smp,
           "-display", "none", "-no-reboot", "-nic", "none",
           "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
           "-serial", "stdio", "-monitor", "none",
           "-drive", f"file={img},format=raw,if=none,id=vd", "-device", "virtio-blk-pci,drive=vd"]
qemu = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
deadline = time.time() + timeout
out = b""
fails = 0

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

def run(line):
    """Sends one line to the shell; returns what it printed."""
    mark = len(out)
    qemu.stdin.write(line.encode() + b"\r"); qemu.stdin.flush()
    read_until(b"\n# ", mark)
    return out[mark:].decode(errors="replace").replace("\r", "")

def check(name, ok, text=""):
    global fails
    if ok:
        print("ok  ", name)
    else:
        print("FAIL", name); print(text); fails += 1

def hold(line, n=1):
    """Starts holders in the background and waits until all n have their memory
    (a fixed sleep is too short on a slow machine: the holder is still faulting in)."""
    seen = out.count(b"mhog: holding")
    run(line)
    pos = 0
    for _ in range(seen + n):
        i = out.find(b"mhog: holding", pos)
        if i < 0 and read_until(b"mhog: holding", pos):
            i = out.find(b"mhog: holding", pos)
        if i < 0:
            return
        pos = i + 1

def vm(key):
    m = re.search(key + r" (\d+)", run("cat /proc/vmstat"))
    return int(m.group(1)) if m else -1

def meminfo(key):
    m = re.search(key + r": +(\d+) kB", run("cat /proc/meminfo"))
    return int(m.group(1)) if m else -1

if not read_until(b"bastios> "):
    print("FAIL no prompt"); sys.exit(1)
run("ash")
# Sizes follow the memory the machine has (the kernel's own use differs between backends).
F = meminfo("MemFree") // 1024
BIG, SMALL, TOUCH_BIG, TOUCH_ADJ = F * 11 // 20, F // 5, F * 2 // 5, F * 7 // 20
OVER, HOLD, SECOND, HUGE = F * 7 // 5, F * 3 // 5, F * 19 // 20, F * 4
# The swap file holds what a program needs beyond memory and a margin. It is
# no bigger than that because the native kernel's heap is small on this
# machine (the swap table takes 16 bytes of it for each slot).
SWAP_MB = max(F * 2 // 3, 8)
SWAP_KB = SWAP_MB * 1024 - 4

# A hog that fits is left alone.
t = run("/disk/mhog touch 16; echo rc=$?")
check("a program that fits gets its memory", "touched 16 MiB" in t and "rc=0" in t and "Out of memory" not in t, t)
check("no kill yet", vm("oom_kill") == 0)

# The page cache goes first. Fill it (a shared mapping of a file), then take
# more memory than is free but less than free + cached: nothing is killed.
run("/disk/mhog cache 12 /disk/f")
t = run("/disk/mhog mapfile 12 /disk/f")
cached = vm("nr_file_pages")
mi = run("cat /proc/meminfo")
free_kb, avail_kb, cache_kb = (int(re.search(k + r": +(\d+) kB", mi).group(1)) for k in ("MemFree", "MemAvailable", "Cached"))
check("the page cache holds the mapped file", cached > 100 and cache_kb > 400, t)
check("MemAvailable counts the cache", avail_kb == free_kb + cache_kb, mi)
want = (free_kb + 2 * cached) // 1024      # more than free, less than free + cached
t = run(f"/disk/mhog touch {want}; echo rc=$?")
check("cached pages are given back before anything is killed",
      f"touched {want} MiB" in t and "rc=0" in t and "Out of memory" not in t and vm("oom_kill") == 0, t)
check("the cache is empty afterwards", vm("nr_file_pages") == 0)

# drop_caches empties the cache by hand.
run("/disk/mhog cache 12 /disk/f")
check("the cache fills again", vm("nr_file_pages") > 100)
run("echo 3 > /proc/sys/vm/drop_caches")
check("drop_caches drops it", vm("nr_file_pages") == 0)

# Nothing to give back: the largest process is killed, the shell goes on.
t = run("/disk/mhog touch 200; echo rc=$?")
check("the process that wants too much is killed", "Out of memory: Killed process" in t and "(mhog)" in t and "rc=137" in t, t)
check("the console says what was killed and its score", re.search(r"oom_score \d+, \d+ kB resident", t) is not None, t)
check("the system carries on", "alive" in run("echo alive"))
check("the kill is counted", vm("oom_kill") == 1)

# The biggest holder dies, not the one that asked last.
hold(f"/disk/mhog hold {BIG} & big=$!; sleep 1; /disk/mhog hold {SMALL} & small=$!; sleep 1", 2)
t = run(f"/disk/mhog touch {TOUCH_BIG}; echo rc=$?")
m = re.search(r"Killed process \d+ \(mhog\) oom_score \d+, (\d+) kB", t)
check("the biggest process is the victim", m and 0.9 * BIG * 1024 <= int(m.group(1)) <= 1.1 * BIG * 1024 and f"touched {TOUCH_BIG} MiB" in t, t)
t = run("kill -0 $big 2>/dev/null && echo bigalive || echo bigdead; kill -0 $small && echo smallalive")
check("the others live", "bigdead" in t and "smallalive" in t, t)
run("kill -9 $small; sleep 1")

# oom_score_adj: +1000 makes a small process the first choice, -1000 exempts.
hold(f"/disk/mhog hold {BIG} & big=$!; sleep 1; /disk/mhog hold {SMALL} & small=$!; sleep 1", 2)
t = run("echo 1000 > /proc/$small/oom_score_adj; echo -1000 > /proc/$big/oom_score_adj; cat /proc/$small/oom_score_adj /proc/$big/oom_score_adj")
check("oom_score_adj is kept", "1000\n-1000" in t, t)
s_small = int(run("cat /proc/$small/oom_score").split()[-2]) if True else 0
s_big = run("cat /proc/$big/oom_score")
check("oom_score follows the adjustment", s_small > 500 and re.search(r"\n0\n", s_big) is not None, f"{s_small} {s_big}")
t = run(f"/disk/mhog touch {TOUCH_ADJ}; echo rc=$?")
t2 = run("kill -0 $big && echo bigalive; kill -0 $small 2>/dev/null || echo smalldead")
check("the process with the high adjustment is killed, the exempt one lives", "bigalive" in t2 and "smalldead" in t2 and f"touched {TOUCH_ADJ} MiB" in t, t + t2)
run("kill -9 $big; sleep 1")

check("/proc/self/oom_score reads", re.search(r"\n\d+\n", run("cat /proc/self/oom_score")) is not None)
n_kill = vm("oom_kill")
announced = len(re.findall(r"Out of memory: Killed process", out.decode(errors="replace")))
check("/proc/vmstat counts the kills", n_kill >= 3 and n_kill == announced, f"vmstat {n_kill}, console {announced}")
# Swap: a file on the disk. A program bigger than free memory runs to the end.
kills = vm("oom_kill")
t = run(f"/disk/mhog mkswap {SWAP_MB} /disk/swap && /disk/mhog swapon /disk/swap; echo rc=$?")
check("mkswap and swapon", "swap on" in t and "rc=0" in t, t)
check("/proc/meminfo has the swap area", meminfo("SwapTotal") == SWAP_KB and meminfo("SwapFree") == SWAP_KB)
check("/proc/swaps lists it", "swapfile" in run("cat /proc/swaps"))
t = run("/disk/mhog swapon /disk/swap; echo rc=$?")
check("a second swapon is refused", "failed" in t, t)
run("/disk/mhog touch 16")
t = run(f"/disk/mhog touch {OVER}; echo rc=$?")
check("a program bigger than free memory runs and reads back what it wrote",
      f"touched {OVER} MiB" in t and "rc=0" in t and "Out of memory" not in t, t)
check("pages went out and came back", vm("pswpout") > 1000 and vm("pswpin") > 1000, f"{vm('pswpout')} {vm('pswpin')}")
check("nothing was killed", vm("oom_kill") == kills)
check("every slot is free again", meminfo("SwapFree") == SWAP_KB)
# A holder is pushed out by a second program; swapoff brings it home.
hold(f"mkdir /sys/fs/cgroup/sw; echo +memory > /sys/fs/cgroup/cgroup.subtree_control; ash -c 'echo $$ > /sys/fs/cgroup/sw/cgroup.procs; exec /disk/mhog hold {HOLD}' & big=$!; sleep 1")
t = run(f"/disk/mhog touch {SECOND}; echo rc=$?")
check("a holder and a second program both fit", f"touched {SECOND} MiB" in t and "rc=0" in t and vm("oom_kill") == kills, t)
used = meminfo("SwapTotal") - meminfo("SwapFree")
check("the holder's pages are in the swap file", used > 1000, str(used))
cgswap = int(run("cat /sys/fs/cgroup/sw/memory.swap.current").split()[-2]) if True else 0
check("the holder's group counts its swapped bytes", cgswap == used * 1024 or cgswap > 1000 * 4096, str(cgswap))
t = run("/disk/mhog swapoff /disk/swap; echo rc=$?")
check("swapoff brings the pages back", "swap off" in t and "rc=0" in t and meminfo("SwapTotal") == 0, t)
check("the group's swapped bytes are back in memory", run("cat /sys/fs/cgroup/sw/memory.swap.current").split()[-2] == "0")
check("the holder is alive and whole", "bigalive" in run("kill -0 $big && echo bigalive"))
run("kill -9 $big; sleep 1; rmdir /sys/fs/cgroup/sw")
# More than memory and swap together: the killer still ends the program.
run("/disk/mhog swapon /disk/swap")
# memory.swap.max: nothing of a group at 0 goes to the swap file (the program that
# wants more memory swaps its own pages instead);
# a group with a small limit gets no more into the swap file than that.
def group_swap(g):
    return int(run(f"cat /sys/fs/cgroup/{g}/memory.swap.current").split()[-2])
run("mkdir /sys/fs/cgroup/sl /sys/fs/cgroup/sm; echo 0 > /sys/fs/cgroup/sl/memory.swap.max; echo 2097152 > /sys/fs/cgroup/sm/memory.swap.max")
hold(f"ash -c 'echo $$ > /sys/fs/cgroup/sl/cgroup.procs; exec /disk/mhog hold {HOLD}' & big=$!; sleep 1")
t = run(f"/disk/mhog touch {SECOND}; echo rc=$?")
used = meminfo("SwapTotal") - meminfo("SwapFree")
check("a group with memory.swap.max 0 keeps its pages in memory; the other program swaps its own",
      group_swap("sl") == 0 and f"touched {SECOND} MiB" in t and "rc=0" in t and "Out of memory" not in t, f"{group_swap('sl')} {t}")
run("kill -9 $big; sleep 1")
hold(f"ash -c 'echo $$ > /sys/fs/cgroup/sm/cgroup.procs; exec /disk/mhog hold {HOLD}' & big=$!; sleep 1")
t = run(f"/disk/mhog touch {SECOND}; echo rc=$?")
check("a group's swapped bytes stay under its memory.swap.max", f"touched {SECOND} MiB" in t and group_swap("sm") <= 2097152, f"{group_swap('sm')} {t}")
run("kill -9 $big; sleep 1; rmdir /sys/fs/cgroup/sl /sys/fs/cgroup/sm")
t = run(f"/disk/mhog touch {HUGE}; echo rc=$?")
check("a program bigger than memory and swap is killed", "Out of memory: Killed process" in t and "rc=137" in t, t)
check("the system carries on after it", "alive" in run("echo alive") and meminfo("SwapFree") == SWAP_KB)
qemu.kill(); qemu.wait()
print("memory: %d failed" % fails)
sys.exit(1 if fails else 0)
PY
