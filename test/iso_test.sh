#!/usr/bin/env bash
# Boot build/bastios.iso (make iso) through Limine under BIOS and under UEFI
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
if ! command -v xorriso >/dev/null; then
  echo "SKIP iso: xorriso missing"
  exit 0
fi
make -s build/bastios.iso || exit 1

python3 - "$OVMF" "$OVMF_VARS" <<'PY'
import json, os, re, select, shutil, socket, subprocess, sys, tempfile, time

ovmf, ovmf_vars = sys.argv[1], sys.argv[2]
font = open("src/font8x8.bin", "rb").read()
glyphs = {}
for c in range(32, 127):
    g = tuple(font[c * 8 + r // 2] for r in range(16))      # 8x8 doubled to 8x16
    glyphs[g] = chr(c)
    glyphs[tuple(255 - b for b in g)] = chr(c)              # under the cursor

# src/font16.bin (layout in tools/mkfont.py): one-cell glyphs of 16 rows, and
# two-cell ones of 16 rows of two bytes, by code point.
f16 = open("src/font16.bin", "rb").read()
nw, nn = int.from_bytes(f16[0:4], "little"), int.from_bytes(f16[4:8], "little")
wcp = [int.from_bytes(f16[8 + 2 * i:10 + 2 * i], "little") for i in range(nw)]
ncp = [int.from_bytes(f16[8 + 2 * nw + 2 * i:10 + 2 * nw + 2 * i], "little") for i in range(nn)]
wbase, nbase = 8 + 2 * nw + 2 * nn, 8 + 2 * nw + 2 * nn + 32 * nw
wide = {tuple(f16[wbase + 32 * i:wbase + 32 * i + 32]): chr(c) for i, c in enumerate(wcp)}
for i, c in enumerate(ncp):
    g = tuple(f16[nbase + 16 * i:nbase + 16 * i + 16])
    if g not in glyphs:                                     # Greek capitals look like Latin ones
        glyphs[g] = chr(c)
        glyphs[tuple(255 - b for b in g)] = chr(c)

def screen_text(ppm):
    # The lines of text on a screendump (binary PPM), read cell by cell.
    head, rest = ppm.split(b"\n", 1)
    dims, rest = rest.split(b"\n", 1)
    _, px = rest.split(b"\n", 1)
    w, h = map(int, dims.split())
    lines = []
    for cy in range(h // 16):
        line, cells = "", []
        for cx in range(w // 8):
            cell = []
            for r in range(16):
                i = ((cy * 16 + r) * w + cx * 8) * 3
                cell.append(sum(1 << p for p in range(8) if px[i + p * 3] > 0x40))
            cells.append(tuple(cell))
        i = 0
        while i < len(cells):
            if cells[i] in glyphs:
                line += glyphs[cells[i]]
            elif i + 1 < len(cells) and tuple(b for r in range(16) for b in (cells[i][r], cells[i + 1][r])) in wide:
                line += wide[tuple(b for r in range(16) for b in (cells[i][r], cells[i + 1][r]))]
                i += 1
            else:
                line += "?"
            i += 1
        lines.append(line.rstrip())
    return lines

def screen_text_at(px, w, ox, oy, cols, rows):
    # Lines of text in a block of cells at pixel (ox, oy), read from the pixels of a screendump.
    lines = []
    for cy in range(rows):
        line = ""
        for cx in range(cols):
            cell = []
            for r in range(16):
                i = ((oy + cy * 16 + r) * w + ox + cx * 8) * 3
                cell.append(sum(1 << p for p in range(8) if px[i + p * 3] > 0x60))
            line += glyphs.get(tuple(cell), "?")
        lines.append(line.rstrip())
    return lines

def usbkeys(path, keys):
    # Key presses through QMP, which QEMU hands to the USB keyboard (the
    # machine has no other).
    qmp = socket.socket(socket.AF_UNIX)
    qmp.connect(path)
    f = qmp.makefile("rw")
    f.readline()
    def cmd(c):
        f.write(json.dumps(c) + "\n"); f.flush()
        f.readline()
    cmd({"execute": "qmp_capabilities"})
    for k in keys:
        parts = k.split("-")
        for down in (True, False):
            for p in (parts if down else parts[::-1]):
                cmd({"execute": "input-send-event", "arguments": {"events": [
                    {"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": p}}}]}})
            time.sleep(0.03)
    qmp.close()

def sendkeys(mon, keys):
    for k in keys:
        mon.sendall(b"sendkey " + k.encode() + b"\n")
        time.sleep(0.05)

tmp = tempfile.TemporaryDirectory()
disk = os.path.join(tmp.name, "disk.img")
with open(disk, "wb") as f:
    f.truncate(33 << 20)
fails = 0

def run(name, firmware, usb=False, nvme=False, stick=None, diag=False):
    # usb: a machine without PS/2 (i8042=off) and a USB keyboard on an
    # xHCI controller, typed on through QMP; only the keyboard, the screen
    # and the page count are checked. nvme: the disk is an NVMe drive.
    # stick: no CD; the machine boots from this USB stick image (make usb)
    # on the xHCI controller, whose BastiOS partition must then be the
    # disk, and the virtio disk is left untouched. diag: Limine's second entry,
    # the hardware report (Down on the menu, then Enter), paged through on
    # the serial line; only the report and the page count are checked.
    global fails
    qmp_path = os.path.join(tmp.name, "qmp.sock")
    if os.path.exists(qmp_path):
        os.remove(qmp_path)
    mon_path = os.path.join(tmp.name, "mon.sock")
    if os.path.exists(mon_path):
        os.remove(mon_path)
    devs = ["-cpu", "qemu64,+pdpe1gb", "-m", "256M", "-smp", "2", "-display", "none", "-no-reboot",
            "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
            "-drive", f"file={disk},format=raw,if=none,id=vd",
            "-device", "nvme,serial=bastios,drive=vd" if nvme else "virtio-blk-pci,drive=vd",
            "-netdev", "user,id=n0", "-device", "virtio-net-pci,netdev=n0",
            "-serial", "stdio", "-monitor", f"unix:{mon_path},server,nowait"]
    if stick:
        devs += ["-drive", f"file={stick},format=raw,if=none,id=st",
                 "-device", "qemu-xhci,id=xh", "-device", "usb-storage,bus=xh.0,drive=st,bootindex=0"]
        before = open(disk, "rb").read()
    else:
        devs += ["-cdrom", "build/bastios.iso"]
    if usb:
        if not stick:
            devs += ["-device", "qemu-xhci,id=xh"]
        devs += ["-machine", "pc,i8042=off", "-device", "usb-kbd,bus=xh.0",
                 "-qmp", f"unix:{qmp_path},server,nowait"]
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
    if diag:
        # Limine draws its menu with a cursor move before every character,
        # so the countdown line is waited for with the escapes taken out.
        while b"Booting automatically" not in re.sub(rb"\x1b\[[0-9;?]*[A-Za-z]", b"", out):
            if not until(b"\x1b", len(out)):
                break
        q.stdin.write(b"\x1b[B"); q.stdin.flush()
        time.sleep(0.3)
        q.stdin.write(b"\r"); q.stdin.flush()
        for _ in range(3):
            mark = len(out)
            if not until(b"(or wait a minute) --", mark):
                break
            q.stdin.write(b" "); q.stdin.flush()
        ok = until(b"bastios> ", 0)
        text = out.decode(errors="replace").replace("\r", "")
        for want, what in (("---- hardware report", "the menu's second entry gives the report (boot option diag)"),
                           ("acpi: RSDP from the boot loader (Multiboot 2)", "the RSDP came through Multiboot 2"),
                           ("pci 00:", "the PCI devices are listed"),
                           ("bytes a line; in use", "the report shows the framebuffer the console draws on"),
                           ("---- end of the hardware report", "the report ends")):
            good = ok and want in text
            print(("ok  " if good else "FAIL"), f"({name}) {what}")
            fails += not good
    ok = until(b"bastios> ", 0)
    print(("ok  " if ok else "FAIL"), f"({name}) Limine boots the kernel to the shell")
    fails += not ok
    # Both processors: under UEFI the ACPI tables are found through the
    # Multiboot memory map, not the BIOS areas.
    good = ok and b"pages of memory, 2 processors" in out
    print(("ok  " if good else "FAIL"), f"({name}) both processors started")
    fails += not good
    if ok and not diag:
        mon = socket.socket(socket.AF_UNIX)
        mon.connect(mon_path)
        mark = len(out)
        keys = ["e", "c", "h", "o", "spc", "shift-o", "n", "shift-minus", "s", "c", "r", "e", "e", "n", "ret"]
        if usb:
            usbkeys(qmp_path, keys)
        else:
            sendkeys(mon, keys)
        good = until(b"On_screen\r\n", mark) and until(b"bastios> ", mark + 1)
        if usb:
            good = good and b"keyboard: USB (xHCI)" in out
        print(("ok  " if good else "FAIL"), f"({name}) a line typed on the {'USB' if usb else 'PS/2'} keyboard runs")
        fails += not good
        if nvme:
            good = b"disk: NVMe" in out
            print(("ok  " if good else "FAIL"), f"({name}) disk driver: NVMe")
            fails += not good
        if stick:
            good = b"disk: USB, the BastiOS partition (" in out
            print(("ok  " if good else "FAIL"), f"({name}) the disk is the stick's BastiOS partition")
            fails += not good
        if usb or nvme or stick:
            # Where the firmware put the 64-bit BARs: OVMF puts them far
            # above 4 GiB, so the drivers above ran in the upper device window.
            mon.sendall(b"info pci\n")
            info = b""
            t0 = time.time()
            while time.time() - t0 < 3:
                r, _, _ = select.select([mon], [], [], 0.2)
                if r:
                    info += mon.recv(65536)
            bars = [int(b, 16) for b in re.findall(rb"BAR0: 64 bit memory at 0x([0-9a-f]+)", info)]
            high = firmware != [] and len(bars) == (usb or bool(stick)) + nvme and all(b >= 1 << 32 for b in bars)
            print(f"     ({name}) 64-bit BAR0s at {', '.join(hex(b) for b in bars) or 'none'}")
            if firmware != []:
                print(("ok  " if high else "FAIL"), f"({name}) the 64-bit BARs lie above 4 GiB")
                fails += not high
        # UTF-8 on the screen: the serial line takes the bytes, the screen
        # draws them (kana and kanji in two cells, accented letters, box
        # drawing and half-width katakana in one; a two-cell character that
        # would straddle the right edge wraps whole: the typed line is 113 letters
        # after the prompt, and the kanji after them starts the next line).
        mark = len(out)
        q.stdin.write("echo 日本語かな café ─│ ｱｲｳ\r".encode()); q.stdin.flush()
        until(b"bastios> ", mark)
        mark = len(out)
        q.stdin.write(("echo " + "x" * 113 + "日\r").encode()); q.stdin.flush()
        until(b"bastios> ", mark)
        dump = os.path.join(tmp.name, "screen.ppm")
        mon.sendall(f"screendump {dump}\n".encode())
        text = []
        for _ in range(50):
            time.sleep(0.1)
            if os.path.exists(dump) and os.path.getsize(dump) > 1024 * 768 * 3:
                text = screen_text(open(dump, "rb").read())
                break
        mon.close()
        want = ["screen: 1024x768 framebuffer", "bastios> echo On_screen", "On_screen",
                "日本語かな café ─│ ｱｲｳ", "bastios> echo " + "x" * 113, "日", "x" * 113 + "日"]
        good = all(w in text for w in want)
        print(("ok  " if good else "FAIL"), f"({name}) the framebuffer console shows it")
        if not good:
            print("\n".join(l for l in text if l))
        fails += not good
    if ok and not diag and not usb and not nvme and not stick:
        # /dev/fb0 (user/mfb.c): a program draws on the screen, and the
        # console stops drawing while it holds the device and comes back
        # when it closes it. The pixels are read back from screendumps.
        def screendump():
            m2 = socket.socket(socket.AF_UNIX)
            m2.connect(mon_path)
            path = os.path.join(tmp.name, "fb.ppm")
            if os.path.exists(path):
                os.remove(path)
            m2.sendall(f"screendump {path}\n".encode())
            for _ in range(50):
                time.sleep(0.1)
                if os.path.exists(path) and os.path.getsize(path) > 1024 * 768 * 3:
                    time.sleep(0.2)
                    break
            m2.close()
            d = open(path, "rb").read()
            _, rest = d.split(b"\n", 1)
            dims, rest = rest.split(b"\n", 1)
            _, px = rest.split(b"\n", 1)
            w, h = map(int, dims.split())
            return w, h, px
        mark = len(out)
        q.stdin.write(b"mfb\r"); q.stdin.flush()
        good = until(b"mfb: drawn 1024x768", mark)
        print(("ok  " if good else "FAIL"), f"({name}) mfb opens /dev/fb0, reads its mode and draws")
        fails += not good
        if good:
            w, h, px = screendump()
            def at(x, y):
                i = (y * w + x) * 3
                return tuple(px[i:i + 3])
            good = (at(10, 10) == (255, 0, 0) and at(900, 380) == (255, 0, 0) and at(10, 700) == (0, 0, 255)
                    and at(100, 100) == (0, 255, 0) and at(163, 163) == (0, 255, 0)
                    and at(164, 164) == (255, 0, 0) and at(99, 99) == (255, 0, 0))
            print(("ok  " if good else "FAIL"), f"({name}) the program's pixels are on the screen")
            fails += not good
            q.stdin.write(b"\r"); q.stdin.flush()
            good = until(b"mfb: done", mark) and until(b"bastios> ", mark)
            print(("ok  " if good else "FAIL"), f"({name}) mfb ends (a write at the end of the screen is ENOSPC)")
            fails += not good
            time.sleep(0.5)
            w, h, px = screendump()
            lines = screen_text(b"P6\n%d %d\n255\n" % (w, h) + px)
            good = any("mfb: drawn 1024x768" in l for l in lines) and any("mfb: done" in l for l in lines)
            print(("ok  " if good else "FAIL"), f"({name}) the console is drawn again, its text from before and after, once the device is closed")
            if not good:
                print("\n".join(l for l in lines if l))
            fails += not good
        # bwm (user/bwm.c), the window manager: windows, a pointer, focus,
        # dragging and typing, driven through QEMU's monitor.
        def monitor(*cmds):
            m3 = socket.socket(socket.AF_UNIX)
            m3.connect(mon_path)
            for c in cmds:
                m3.sendall(c.encode() + b"\n")
                time.sleep(0.15)
            time.sleep(0.4)
            m3.close()
        mark = len(out)
        q.stdin.write(b"bwm\r"); q.stdin.flush()
        good = until(b"bwm: ready 1024x768", mark)
        print(("ok  " if good else "FAIL"), f"({name}) bwm starts on /dev/fb0")
        fails += not good
        if good:
            time.sleep(0.5)
            w, h, px = screendump()
            def at(x, y):
                i = (y * w + x) * 3
                return tuple(px[i:i + 3])
            blue, grey, wall = (40, 90, 200), (110, 110, 120), at(10, 10)
            good = (at(400, 125) == blue and at(300, 70) == grey and at(400, 300) == (235, 235, 235)
                    and wall[2] > 70 and wall != blue and at(650, 300) == (220, 235, 250))
            print(("ok  " if good else "FAIL"), f"({name}) bwm draws the wallpaper and three windows, the top one focused")
            fails += not good
            # Click the title bar of the Notes window (under the others'
            # corners), drag it 100 right and 100 down: it comes to the front.
            monitor("mouse_move -212 -314", "mouse_button 1", "mouse_move 50 50", "mouse_move 50 50", "mouse_button 0")
            w, h, px = screendump()
            good = at(300, 170) == blue and at(300, 70) == at(10, 70) and at(300, 70) != grey
            print(("ok  " if good else "FAIL"), f"({name}) dragging a title bar moves the window and focuses it")
            fails += not good
            # Typed text goes into the focused window, after "for BastiOS." on its second line.
            monitor("sendkey h")
            w, h, px = screendump()
            cell = [at(x, y) for y in range(200, 216) for x in range(282, 290)]
            good = (10, 10, 10) in cell
            print(("ok  " if good else "FAIL"), f"({name}) a typed key appears in the focused window")
            fails += not good
            monitor("sendkey ctrl-q")
            good = until(b"bwm: bye", mark) and until(b"bastios> ", mark)
            print(("ok  " if good else "FAIL"), f"({name}) Ctrl-Q ends bwm and the console comes back")
            fails += not good
        # A client over the socket (user/bhello.c): a window of its own
        # pixels, keys and clicks sent back to it, and its close box.
        # The native shell passes no arguments, so this runs from BusyBox's ash.
        mark = len(out)
        q.stdin.write(b"ash\r"); q.stdin.flush()
        until(b"# ", mark)
        mark = len(out)
        q.stdin.write(b"bwm -n -c bhello\r"); q.stdin.flush()
        good = until(b"bhello: window 1", mark)
        print(("ok  " if good else "FAIL"), f"({name}) a client opens a window on bwm over its socket")
        fails += not good
        if good:
            time.sleep(0.5)
            w, h, px = screendump()
            wall = lambda y: at(10, y)
            good = (at(80, 100) == (255, 0, 0) and at(260, 100) == (0, 0, 255) and at(150, 75) == blue
                    and at(10, 100) == wall(100) and at(80, 100) != wall(100))
            print(("ok  " if good else "FAIL"), f"({name}) the client's pixels are in its window")
            fails += not good
            monitor("sendkey g")
            good = until(b"bhello: key g", mark)
            w, h, px = screendump()
            good = good and at(80, 100) == (0, 255, 0) and at(260, 100) == (0, 0, 255)
            print(("ok  " if good else "FAIL"), f"({name}) a key typed goes to the client, which redraws")
            fails += not good
            monitor("mouse_move -362 -244", "mouse_button 1", "mouse_button 0")
            good = until(b"bhello: mouse 78 50 1", mark)
            print(("ok  " if good else "FAIL"), f"({name}) a click inside the window reaches the client, in its own coordinates")
            fails += not good
            monitor("mouse_move 114 -60", "mouse_button 1", "mouse_button 0")
            good = until(b"bhello: closed", mark)
            time.sleep(0.5)
            w, h, px = screendump()
            good = good and at(80, 100) == wall(100) and at(150, 75) == wall(75)
            print(("ok  " if good else "FAIL"), f"({name}) the close box tells the client, whose window goes when it ends")
            fails += not good
            monitor("sendkey ctrl-q")
            good = until(b"bwm: bye", mark) and until(b"# ", mark)
            print(("ok  " if good else "FAIL"), f"({name}) Ctrl-Q ends bwm again")
            fails += not good
        # bterm (user/bterm.c): a terminal window with ash behind it. A line
        # typed is run and what it prints is read back off the screen, cell
        # by cell, from the window's contents.
        mark = len(out)
        q.stdin.write(b"bwm -n -c bterm\r"); q.stdin.flush()
        good = until(b"bterm: window 1", mark)
        print(("ok  " if good else "FAIL"), f"({name}) bterm opens a terminal window")
        fails += not good
        if good:
            time.sleep(1.5)
            monitor("sendkey e", "sendkey c", "sendkey h", "sendkey o", "sendkey spc", "sendkey t", "sendkey e", "sendkey r", "sendkey m", "sendkey ret")
            time.sleep(1.5)
            w, h, px = screendump()
            lines = screen_text_at(px, w, 72, 90, 80, 24)
            good = any(l.strip() == "term" for l in lines) and any("echo term" in l for l in lines)
            print(("ok  " if good else "FAIL"), f"({name}) a line typed into the terminal window runs in ash and its output is drawn")
            if not good:
                print("\n".join(l for l in lines if l))
            fails += not good
            monitor("sendkey ctrl-q")
            until(b"bwm: bye", mark)
            time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"exit\r"); q.stdin.flush()
        until(b"bastios> ", mark)
    steps = [("hello", b"hello from user mode"),
             ("echo from the iso > disk/iso", b""),
             ("cat disk/iso", b"from the iso"),
             ("ifconfig", b"(DHCP)")]
    if nvme:
        # What the first UEFI run wrote through virtio, read through NVMe.
        steps = [("cat disk/iso", b"from the iso")]
    for line, want in steps:
        if not ok or diag or (usb and not nvme and not stick):
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
    if stick:
        q.kill(); q.wait()
        good = open(disk, "rb").read() == before
        print(("ok  " if good else "FAIL"), f"({name}) the virtio disk is untouched")
        fails += not good
    q.kill(); q.wait()
    print(f"---- {name} transcript ----")
    print(out.decode(errors="replace").replace("\r", "")[-1500:])

run("BIOS", [])
# The image `make usb` builds: the ISO, then a BastiOS partition. The
# keyboard is on the same controller, so it is read through the disk.
subprocess.run(["make", "-s", "build/bastios-usb.img", "USB_MIB=33"], check=True)
stick = os.path.join(tmp.name, "stick.img")
shutil.copy("build/bastios-usb.img", stick)
run("BIOS, from a USB stick, USB keyboard", [], usb=True, stick=stick)
if os.path.exists(ovmf) and os.path.exists(ovmf_vars):
    vars_copy = os.path.join(tmp.name, "vars.fd")
    shutil.copy(ovmf_vars, vars_copy)
    run("UEFI", ["-drive", f"if=pflash,format=raw,readonly=on,file={ovmf}",
                 "-drive", f"if=pflash,format=raw,file={vars_copy}"])
    # The firmware drives the xHCI controller itself until it hands over.
    # OVMF puts both controllers' 64-bit BARs far above 4 GiB, in the upper
    # part of the device window.
    shutil.copy(ovmf_vars, vars_copy)
    run("UEFI, USB keyboard, NVMe", ["-drive", f"if=pflash,format=raw,readonly=on,file={ovmf}",
                                     "-drive", f"if=pflash,format=raw,file={vars_copy}"], usb=True, nvme=True)
    shutil.copy(ovmf_vars, vars_copy)
    run("UEFI, from a USB stick", ["-drive", f"if=pflash,format=raw,readonly=on,file={ovmf}",
                                   "-drive", f"if=pflash,format=raw,file={vars_copy}"], stick=stick)
    shutil.copy(ovmf_vars, vars_copy)
    run("UEFI, hardware report", ["-drive", f"if=pflash,format=raw,readonly=on,file={ovmf}",
                                  "-drive", f"if=pflash,format=raw,file={vars_copy}"], diag=True)
else:
    print(f"SKIP UEFI: no OVMF firmware at {ovmf}")
print(f"---- {fails} failed ----")
sys.exit(1 if fails else 0)
PY
