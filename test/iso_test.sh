#!/usr/bin/env bash
# Boot build/bastios.iso (make iso) through GRUB under BIOS and under UEFI
# (OVMF), with the devices `make run` gives it, and check that the shell
# comes up, runs a program, uses the disk and the network, and gets every
# page back at power off. A line typed on the PS/2 keyboard must run and
# show on the framebuffer console: the test reads the screen back from a
# QEMU screendump, cell by cell, against the kernel's font. Skips what it
# lacks the tools for.
set -u
cd "$(dirname "$0")/.."
OVMF=${OVMF:-/usr/share/OVMF/OVMF_CODE_4M.fd}
OVMF_VARS=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}
if ! command -v grub-mkrescue >/dev/null || ! command -v xorriso >/dev/null; then
  echo "SKIP iso: grub-mkrescue or xorriso missing"
  exit 0
fi
make -s build/bastios.iso || exit 1

python3 - "$OVMF" "$OVMF_VARS" <<'PY'
import os, re, select, shutil, socket, subprocess, sys, tempfile, time

ovmf, ovmf_vars = sys.argv[1], sys.argv[2]
font = open("src/font8x8.bin", "rb").read()
glyphs = {}
for c in range(32, 127):
    g = tuple(font[c * 8 + r // 2] for r in range(16))      # 8x8 doubled to 8x16
    glyphs[g] = chr(c)
    glyphs[tuple(255 - b for b in g)] = chr(c)              # under the cursor

def screen_text(ppm):
    # The lines of text on a screendump (binary PPM), read cell by cell.
    head, rest = ppm.split(b"\n", 1)
    dims, rest = rest.split(b"\n", 1)
    _, px = rest.split(b"\n", 1)
    w, h = map(int, dims.split())
    lines = []
    for cy in range(h // 16):
        line = ""
        for cx in range(w // 8):
            cell = []
            for r in range(16):
                i = ((cy * 16 + r) * w + cx * 8) * 3
                cell.append(sum(1 << p for p in range(8) if px[i + p * 3] > 0x40))
            line += glyphs.get(tuple(cell), "?")
        lines.append(line.rstrip())
    return lines

def sendkeys(mon, keys):
    for k in keys:
        mon.sendall(b"sendkey " + k.encode() + b"\n")
        time.sleep(0.05)

tmp = tempfile.TemporaryDirectory()
disk = os.path.join(tmp.name, "disk.img")
with open(disk, "wb") as f:
    f.truncate(33 << 20)
fails = 0

def run(name, firmware):
    global fails
    mon_path = os.path.join(tmp.name, "mon.sock")
    if os.path.exists(mon_path):
        os.remove(mon_path)
    devs = ["-m", "256M", "-smp", "2", "-display", "none", "-no-reboot",
            "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
            "-drive", f"file={disk},format=raw,if=none,id=vd", "-device", "virtio-blk-pci,drive=vd",
            "-netdev", "user,id=n0", "-device", "virtio-net-pci,netdev=n0",
            "-serial", "stdio", "-monitor", f"unix:{mon_path},server,nowait",
            "-cdrom", "build/bastios.iso"]
    q = subprocess.Popen(["qemu-system-x86_64"] + firmware + devs,
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = b""
    deadline = time.time() + 120
    def until(marker, start):
        nonlocal out
        while marker not in out[start:]:
            left = deadline - time.time()
            if left <= 0:
                return False
            r, _, _ = select.select([q.stdout], [], [], left)
            if not r:
                return False
            chunk = q.stdout.read1(4096)
            if not chunk:
                return False
            out += chunk
        return True
    ok = until(b"bastios> ", 0)
    print(("ok  " if ok else "FAIL"), f"({name}) GRUB boots the kernel to the shell")
    fails += not ok
    # Both processors: under UEFI the ACPI tables are found through the
    # Multiboot memory map, not the BIOS areas.
    good = ok and b"pages of memory, 2 processors" in out
    print(("ok  " if good else "FAIL"), f"({name}) both processors started")
    fails += not good
    if ok:
        mon = socket.socket(socket.AF_UNIX)
        mon.connect(mon_path)
        mark = len(out)
        sendkeys(mon, ["e", "c", "h", "o", "spc", "shift-o", "n", "shift-minus", "s", "c", "r", "e", "e", "n", "ret"])
        good = until(b"On_screen\r\n", mark) and until(b"bastios> ", mark + 1)
        print(("ok  " if good else "FAIL"), f"({name}) a line typed on the PS/2 keyboard runs")
        fails += not good
        dump = os.path.join(tmp.name, "screen.ppm")
        mon.sendall(f"screendump {dump}\n".encode())
        text = []
        for _ in range(50):
            time.sleep(0.1)
            if os.path.exists(dump) and os.path.getsize(dump) > 1024 * 768 * 3:
                text = screen_text(open(dump, "rb").read())
                break
        mon.close()
        want = ["screen: 1024x768 framebuffer, keyboard on PS/2", "bastios> echo On_screen", "On_screen"]
        good = all(w in text for w in want)
        print(("ok  " if good else "FAIL"), f"({name}) the framebuffer console shows it")
        if not good:
            print("\n".join(l for l in text if l))
        fails += not good
    steps = [("hello", b"hello from user mode"),
             ("echo from the iso > disk/iso", b""),
             ("cat disk/iso", b"from the iso"),
             ("ifconfig", b"(DHCP)")]
    for line, want in steps:
        if not ok:
            break
        mark = len(out)
        if line == "ifconfig":
            time.sleep(1)                 # the lease may still be on its way
        q.stdin.write(line.encode() + b"\r"); q.stdin.flush()
        good = until(b"bastios> ", mark) and want in out[mark:]
        print(("ok  " if good else "FAIL"), f"({name}) {line!r}")
        fails += not good
    if ok:
        q.stdin.write(b"halt\r"); q.stdin.flush()
        until(b"pages free at power off", 0)
        booted = re.search(rb"BastiOS 0.2: (\d+) pages of memory", out)
        off = re.search(rb"BastiOS: (\d+) pages free at power off", out)
        good = booted and off and booted.group(1) == off.group(1) and int(booted.group(1)) > 10000
        print(("ok  " if good else "FAIL"), f"({name}) {booted.group(1).decode() if booted else '?'} pages, every one back at power off")
        fails += not good
    q.kill(); q.wait()
    print(f"---- {name} transcript ----")
    print(out.decode(errors="replace").replace("\r", "")[-1500:])

run("BIOS", [])
if os.path.exists(ovmf) and os.path.exists(ovmf_vars):
    vars_copy = os.path.join(tmp.name, "vars.fd")
    shutil.copy(ovmf_vars, vars_copy)
    run("UEFI", ["-drive", f"if=pflash,format=raw,readonly=on,file={ovmf}",
                 "-drive", f"if=pflash,format=raw,file={vars_copy}"])
else:
    print(f"SKIP UEFI: no OVMF firmware at {ovmf}")
print(f"---- {fails} failed ----")
sys.exit(1 if fails else 0)
PY
