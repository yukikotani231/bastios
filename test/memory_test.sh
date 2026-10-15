#!/usr/bin/env bash
# What BastiOS does when it runs out of memory: boots a small machine (64 MiB
# of RAM, about 30 MiB of it for programs), runs user/mhog.c on it, and checks
#   - the page cache goes back before anything is killed (reclaim),
#   - the out-of-memory killer picks the process with the highest oom_score,
#     says so on the console, and the system carries on,
#   - oom_score_adj moves the choice, and /proc/vmstat counts the kills,
#   - /proc/meminfo counts the cache as available.
# Input is sent a line at a time, each after the prompt, as test/shell_test.sh does.
set -u
cd "$(dirname "$0")/.."
KERNEL=${KERNEL:-build/bastios.elf}
TIMEOUT=${TIMEOUT:-300}
MEM=${MEM:-48M}
SMP=${SMP:-1}

python3 - "$KERNEL" "$TIMEOUT" "$MEM" "$SMP" <<'PY'
import os, re, select, subprocess, sys, tempfile, time

kernel, timeout, mem, smp = sys.argv[1], float(sys.argv[2]), sys.argv[3], sys.argv[4]
tmp = tempfile.TemporaryDirectory()
img = os.path.join(tmp.name, "disk.img")
subprocess.run([sys.executable, "test/compat/mkdisk.py", img, "mhog=build/user/mhog.elf"], check=True)

qemu = subprocess.Popen(
    ["qemu-system-x86_64", "-kernel", kernel, "-m", mem, "-smp", smp,
     "-display", "none", "-no-reboot", "-nic", "none",
     "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
     "-serial", "stdio", "-monitor", "none",
     "-drive", f"file={img},format=raw,if=none,id=vd", "-device", "virtio-blk-pci,drive=vd"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
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

def vm(key):
    m = re.search(key + r" (\d+)", run("cat /proc/vmstat"))
    return int(m.group(1)) if m else -1

def meminfo(key):
    m = re.search(key + r": +(\d+) kB", run("cat /proc/meminfo"))
    return int(m.group(1)) if m else -1

if not read_until(b"bastios> "):
    print("FAIL no prompt"); sys.exit(1)
run("ash")

# A hog that fits is left alone.
t = run("/disk/mhog touch 16; echo rc=$?")
check("a program that fits gets its memory", "touched 16 MiB" in t and "rc=0" in t and "Out of memory" not in t, t)
check("no kill yet", vm("oom_kill") == 0)

# The page cache goes first. Fill it (a shared mapping of a file), then take
# more memory than is free but less than free + cached: nothing is killed.
run("/disk/mhog cache 12 /disk/f")
t = run("/disk/mhog mapfile 12 /disk/f")
cached = vm("nr_file_pages")
free_kb = meminfo("MemFree")
avail_kb = meminfo("MemAvailable")
check("the page cache holds the mapped file", cached > 100, t)
check("MemAvailable counts the cache", avail_kb >= free_kb + 4 * cached - 16, f"{free_kb} {avail_kb} {cached}")
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
run("/disk/mhog hold 18 & big=$!; sleep 1; /disk/mhog hold 6 & small=$!; sleep 1")
t = run("/disk/mhog touch 12; echo rc=$?")
check("the biggest process is the victim", re.search(r"Killed process \d+ \(mhog\) oom_score \d+, 1[789]\d{3} kB", t) and "touched 12 MiB" in t, t)
t = run("kill -0 $big 2>/dev/null && echo bigalive || echo bigdead; kill -0 $small && echo smallalive")
check("the others live", "bigdead" in t and "smallalive" in t, t)
run("kill -9 $small; sleep 1")

# oom_score_adj: +1000 makes a small process the first choice, -1000 exempts.
run("/disk/mhog hold 18 & big=$!; sleep 1; /disk/mhog hold 6 & small=$!; sleep 1")
t = run("echo 1000 > /proc/$small/oom_score_adj; echo -1000 > /proc/$big/oom_score_adj; cat /proc/$small/oom_score_adj /proc/$big/oom_score_adj")
check("oom_score_adj is kept", "1000\n-1000" in t, t)
s_small = int(run("cat /proc/$small/oom_score").split()[-2]) if True else 0
s_big = run("cat /proc/$big/oom_score")
check("oom_score follows the adjustment", s_small > 500 and re.search(r"\n0\n", s_big) is not None, f"{s_small} {s_big}")
t = run("/disk/mhog touch 8; echo rc=$?")
t2 = run("kill -0 $big && echo bigalive; kill -0 $small 2>/dev/null || echo smalldead")
check("the process with the high adjustment is killed, the exempt one lives", "bigalive" in t2 and "smalldead" in t2 and "touched 8 MiB" in t, t + t2)
run("kill -9 $big; sleep 1")

check("/proc/self/oom_score reads", re.search(r"\n\d+\n", run("cat /proc/self/oom_score")) is not None)
check("/proc/vmstat counts the kills", vm("oom_kill") == 3)
qemu.kill(); qemu.wait()
print("memory: %d failed" % fails)
sys.exit(1 if fails else 0)
PY
