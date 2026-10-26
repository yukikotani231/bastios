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
        cells = []
        for cx in range(cols):
            cell = []
            for r in range(16):
                i = ((oy + cy * 16 + r) * w + ox + cx * 8) * 3
                cell.append(sum(1 << p for p in range(8) if px[i + p * 3] > 0x60))
            cells.append(tuple(cell))
        cx = 0
        while cx < len(cells):
            pair = tuple(b for r in range(16) for b in (cells[cx][r], cells[cx + 1][r])) if cx + 1 < len(cells) else None
            if cells[cx] not in glyphs and pair in wide:      # a wide character takes two cells
                line += wide[pair]
                cx += 1
            else:
                line += glyphs.get(cells[cx], "?")
            cx += 1
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
    deadline = time.time() + 900
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
        def grab():
            if os.path.exists(dump):
                os.unlink(dump)
            mon.sendall(f"screendump {dump}\n".encode())
            for _ in range(50):
                time.sleep(0.1)
                if os.path.exists(dump) and os.path.getsize(dump) > 1024 * 768 * 3:
                    time.sleep(0.2)
                    return screen_text(open(dump, "rb").read())
            return []
        text = grab()
        want = ["screen: 1024x768 framebuffer", "bastios> echo On_screen", "On_screen",
                "日本語かな café ─│ ｱｲｳ", "bastios> echo " + "x" * 113, "日", "x" * 113 + "日"]
        good = all(w in text for w in want)
        print(("ok  " if good else "FAIL"), f"({name}) the framebuffer console shows it")
        if not good:
            print("\n".join(l for l in text if l))
        fails += not good
        # Escape sequences: `echo` writes them (`\\e` is ESC) and the screen
        # obeys them. Clear, move to row 5 column 10, write
        # kanji, step back two cells and overwrite with a letter, delete a
        # character, then draw on the alternate screen and come back.
        mark = len(out)
        q.stdin.write(b"echo \\e[2J\\e[5;10H\xe6\x97\xa5\xe6\x9c\xac\\e[2DAB\\e[1P\\e[8;3Hhere\\e[?1049hALT\\e[?1049l\r")
        q.stdin.flush()
        until(b"bastios> ", mark)
        time.sleep(0.5)
        text = grab()
        good = len(text) > 8 and text[4] == " " * 9 + "日AB" and text[7].startswith("  here") and not any("ALT" in l for l in text)
        print(("ok  " if good else "FAIL"), f"({name}) the framebuffer console obeys escape sequences")
        if not good:
            print("\n".join(f"{i}: {l}" for i, l in enumerate(text) if l))
        fails += not good
        # Editing keys: the arrows send ESC [ A to ESC [ D, Delete ESC [ 3 ~,
        # Insert ESC [ 2 ~, F1 ESC O P, F5 ESC [ 1 5 ~ and F12 ESC [ 2 4 ~;
        # the line editor drops the ESC, so the rest shows in the line.
        mark = len(out)
        keys = ["e", "c", "h", "o", "spc", "up", "down", "right", "left", "delete", "insert", "f1", "f5", "f12"]
        if usb:
            usbkeys(qmp_path, keys)
        else:
            sendkeys(mon, keys)
        time.sleep(1)
        text = grab()
        good = any(l.endswith("bastios> echo [A[B[C[D[3~[2~OP[15~[24~") for l in text)
        print(("ok  " if good else "FAIL"), f"({name}) arrow, Delete, Insert and function keys send escape sequences")
        if not good:
            print("\n".join(f"{i}: {l}" for i, l in enumerate(text) if l))
        fails += not good
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"bastios> ", mark)
        if usb:
            # /dev/input/event0 sees what the USB keyboard does as it does for
            # PS/2: Shift's press and release (96 bytes with the SYN records;
            # it makes no character, so the shell does not see it) and
            # Ctrl's (192 bytes after both).
            mark = len(out)
            q.stdin.write(b"ash\r"); q.stdin.flush()
            until(b"# ", mark)
            mark = len(out)
            q.stdin.write(b"cat /dev/input/event0 > /tmp/ev &\r"); q.stdin.flush()
            time.sleep(1)
            usbkeys(qmp_path, ["shift"])
            time.sleep(1)
            q.stdin.write(b"ls -l /tmp/ev\r"); q.stdin.flush()
            good = until(b" 96 ", mark)
            print(("ok  " if good else "FAIL"), f"({name}) /dev/input/event0 reports the press and the release of Shift on the USB keyboard")
            fails += not good
            until(b"# ", mark)
            mark = len(out)
            usbkeys(qmp_path, ["ctrl"])
            time.sleep(1)
            q.stdin.write(b"ls -l /tmp/ev\r"); q.stdin.flush()
            good = until(b" 192 ", mark)
            print(("ok  " if good else "FAIL"), f"({name}) a second modifier on the USB keyboard adds its own")
            fails += not good
            until(b"# ", mark)
            # ash gets a second between the kill of its sleeping child and
            # its exit.
            q.stdin.write(b"kill $!\r"); q.stdin.flush()
            time.sleep(1)
            mark = len(out)
            q.stdin.write(b"exit\r"); q.stdin.flush()
            until(b"bastios> ", mark)
        mon.close()
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
        def monitor(*cmds, gap=0.15):
            m3 = socket.socket(socket.AF_UNIX)
            m3.connect(mon_path)
            for c in cmds:
                m3.sendall(c.encode() + b"\n")
                time.sleep(gap)
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
            # Alt-Tab brings up the window at the bottom: the picture changes,
            # and with three windows, three of them bring it back.
            # On a loaded host bwm can take longer than monitor's pause to
            # redraw, so the picture is read again until it is the one
            # wanted (10 s at most). Only the part above the task bar is
            # compared: the bar's clock changes when a minute turns over.
            def above_bar():
                w, h, px = screendump()
                return px[:w * 3 * 740]
            def settle(want):
                end = time.time() + 10
                while True:
                    px = above_bar()
                    if want(px) or time.time() > end:
                        return px
            before = above_bar()
            for _ in range(20):                 # the picture before, once it stops changing
                px = above_bar()
                if px == before:
                    break
                before = px
            monitor("sendkey alt-tab")
            once = settle(lambda px: px != before)
            monitor("sendkey alt-tab", "sendkey alt-tab")
            thrice = settle(lambda px: px == before)
            good = once != before and thrice == before
            print(("ok  " if good else "FAIL"), f"({name}) Alt-Tab cycles the windows")
            fails += not good
            monitor("sendkey alt-tab", "sendkey alt-shift-tab")
            back = settle(lambda px: px == before)
            good = back == before
            print(("ok  " if good else "FAIL"), f"({name}) Alt-Shift-Tab goes back the other way")
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
            # Maximize (the box left of the close box) takes the whole screen
            # above the task bar, and a second click puts the window back.
            monitor("mouse_move 95 -60", "mouse_button 1", "mouse_button 0")
            good = until(b"bhello: resized 1020 722", mark)
            print(("ok  " if good else "FAIL"), f"({name}) the maximize button gives the window the whole screen above the task bar")
            fails += not good
            monitor("mouse_move 375 -35", "mouse_move 375 -35", "mouse_button 1", "mouse_button 0")
            good = until(b"bhello: resized 200 100", mark)
            time.sleep(0.5)
            w, h, px = screendump()
            good = good and at(80, 100) == (0, 255, 0) and at(260, 100) == (0, 0, 255)
            print(("ok  " if good else "FAIL"), f"({name}) maximizing again puts the window back")
            fails += not good
            # Minimize (the bar left of the maximize box) hides the window; its
            # button in the task bar brings it back.
            monitor("mouse_move -384 35", "mouse_move -384 35", "mouse_button 1", "mouse_button 0")
            time.sleep(0.7)
            w, h, px = screendump()
            good = at(80, 100) != (0, 255, 0) and at(260, 100) != (0, 0, 255)
            print(("ok  " if good else "FAIL"), f"({name}) the minimize button hides the window")
            fails += not good
            monitor("mouse_move -64 338", "mouse_move -63 338", "mouse_button 1", "mouse_button 0")
            time.sleep(0.7)
            w, h, px = screendump()
            good = at(80, 100) == (0, 255, 0) and at(260, 100) == (0, 0, 255)
            print(("ok  " if good else "FAIL"), f"({name}) its task bar button shows the window again")
            fails += not good
            monitor("mouse_move 25 -308", "mouse_move 25 -308")                          # back to where the next step starts
            # The grip at the corner of the frame: dragging it 100 right and 60
            # down asks the client for a 300 x 160 window, which it paints again.
            monitor("mouse_move 120 45", "mouse_button 1", "mouse_move 100 60", "mouse_button 0")
            good = until(b"bhello: resized 300 160", mark)
            time.sleep(0.5)
            w, h, px = screendump()
            good = good and at(100, 200) == (0, 255, 0) and at(350, 200) == (0, 0, 255)
            print(("ok  " if good else "FAIL"), f"({name}) dragging the grip resizes the window and the client paints it again")
            fails += not good
            # The left edge: dragging it 30 right takes 30 off the width and
            # the right edge stays where it was (the frame is at x 70 to 374).
            monitor("mouse_move -299 -95", "mouse_button 1", "mouse_move 30 0", "mouse_button 0")
            good = until(b"bhello: resized 270 160", mark)
            time.sleep(0.5)
            w, h, px = screendump()
            good = good and at(85, 200) != (0, 255, 0) and at(110, 200) == (0, 255, 0) and at(350, 200) == (0, 0, 255)
            print(("ok  " if good else "FAIL"), f"({name}) dragging the left edge resizes the window and keeps its right edge")
            fails += not good
            # The top edge: 20 down takes 20 off the height, the bottom stays.
            monitor("mouse_move 99 -79", "mouse_button 1", "mouse_move 0 20", "mouse_button 0")
            good = until(b"bhello: resized 270 140", mark)
            time.sleep(0.5)
            w, h, px = screendump()
            good = good and at(200, 200) == (0, 255, 0) and at(200, 245) == (0, 255, 0) and at(200, 74) != (0, 255, 0)
            print(("ok  " if good else "FAIL"), f"({name}) dragging the top edge resizes the window and keeps its bottom edge")
            fails += not good
            monitor("mouse_move 170 154")
            monitor("mouse_move -6 -145", "mouse_button 1", "mouse_button 0")
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
        # The same client with its pixels in memory shared with bwm
        # (bhello -s: a memfd sent over the socket with SCM_RIGHTS).
        mark = len(out)
        q.stdin.write(b"bwm -n -c bhello -s\r"); q.stdin.flush()
        good = until(b"bhello: shared window 1", mark)
        print(("ok  " if good else "FAIL"), f"({name}) a client opens a window whose pixels are shared memory")
        fails += not good
        if good:
            time.sleep(0.5)
            w, h, px = screendump()
            good = at(80, 100) == (255, 0, 0) and at(260, 100) == (0, 0, 255)
            print(("ok  " if good else "FAIL"), f"({name}) bwm shows what the client wrote into the shared pixels")
            fails += not good
            monitor("sendkey y")
            good = until(b"bhello: key y", mark)
            w, h, px = screendump()
            good = good and at(80, 100) == (255, 255, 0) and at(260, 100) == (0, 0, 255)
            print(("ok  " if good else "FAIL"), f"({name}) a change in the shared pixels shows after the client says where")
            fails += not good
            monitor("mouse_move -242 -199", "mouse_button 1", "mouse_move 100 60", "mouse_button 0")
            good = until(b"bhello: resized 300 160", mark)
            time.sleep(0.5)
            w, h, px = screendump()
            good = good and at(100, 200) == (255, 255, 0) and at(350, 200) == (0, 0, 255)
            print(("ok  " if good else "FAIL"), f"({name}) a shared window is resized with new shared memory")
            fails += not good
            monitor("sendkey ctrl-q")
            good = until(b"bwm: bye", mark) and until(b"# ", mark)
            fails += not good
        # A terminal in line mode keeps ESC in the line (and Tab), so the
        # escape sequences of the special keys reach a program that reads
        # it: cat takes them into a file, whose size is then counted (the
        # console drops ESC when it prints one). ESC [ A, ESC [ 1 5 ~,
        # ESC [ 3 ~, ESC [ D and ESC [ H are 18 bytes, and the line's end 1.
        mark = len(out)
        q.stdin.write(b"cat > /tmp/keys\r"); q.stdin.flush()
        time.sleep(1)
        monitor("sendkey up", "sendkey f5", "sendkey delete", "sendkey left", "sendkey home", "sendkey ret", "sendkey ctrl-d")
        until(b"# ", mark)
        mark = len(out)
        q.stdin.write(b"ls -l /tmp/keys\r"); q.stdin.flush()
        good = until(b" 19 ", mark)
        print(("ok  " if good else "FAIL"), f"({name}) a line-mode terminal keeps the escape sequences of the special keys")
        fails += not good
        until(b"# ", mark)
        # /dev/input/event0 gives the key events as Linux's evdev does:
        # a press and a release of one key are an EV_KEY and an EV_SYN
        # record each, 24 bytes a record, 96 bytes in all. Shift and Ctrl make
        # no character, so only this device sees them (192 bytes after both).
        mark = len(out)
        q.stdin.write(b"cat /dev/input/event0 > /tmp/ev &\r"); q.stdin.flush()
        time.sleep(1)
        monitor("sendkey shift")
        time.sleep(1)
        q.stdin.write(b"ls -l /tmp/ev\r"); q.stdin.flush()
        good = until(b" 96 ", mark)
        print(("ok  " if good else "FAIL"), f"({name}) /dev/input/event0 reports the press and the release of a key")
        fails += not good
        until(b"# ", mark)
        mark = len(out)
        monitor("sendkey ctrl")
        time.sleep(1)
        q.stdin.write(b"ls -l /tmp/ev\r"); q.stdin.flush()
        good = until(b" 192 ", mark)
        print(("ok  " if good else "FAIL"), f"({name}) a modifier is reported as keys of its own")
        fails += not good
        until(b"# ", mark)
        q.stdin.write(b"kill $!\r"); q.stdin.flush()
        until(b"# ", mark)
        # A process for the System window's list to end.
        mark = len(out)
        q.stdin.write(b"sleep 302 & echo sq$((1+2))=$!; sleep 301 & echo sp$((1+1))=$!; echo q$((3+4))\r"); q.stdin.flush()
        until(b"\nq7", mark)
        until(b"# ", mark)
        sleeper = re.search(rb"sp2=(\d+)", out[mark:])
        sleeper = int(sleeper.group(1)) if sleeper else 0
        sleeper2 = re.search(rb"sq3=(\d+)", out[mark:])
        sleeper2 = int(sleeper2.group(1)) if sleeper2 else 0
        # Ctrl-T starts a terminal window without a client on the command line.
        mark = len(out)
        q.stdin.write(b"bwm -n\r"); q.stdin.flush()
        good = until(b"bwm: ready", mark)
        if good:
            time.sleep(1)
            monitor("sendkey ctrl-t")
            good = until(b"bterm: window 1", mark)
        print(("ok  " if good else "FAIL"), f"({name}) Ctrl-T in bwm starts a terminal window")
        fails += not good
        # A second terminal window: bwm must still see the keys (a poll
        # asleep on the key events was not woken while the shells kept
        # retrying their waits).
        if good:
            time.sleep(1)
            mark2 = len(out)
            monitor("sendkey ctrl-t")
            good = until(b"bterm: window", mark2)
            time.sleep(1)
            monitor("sendkey ctrl-q")
            good = good and until(b"bwm: bye", mark2)
            print(("ok  " if good else "FAIL"), f"({name}) bwm still gets keys with two terminal windows open")
            fails += not good
        else:
            monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # The task bar: its Term button starts a terminal window, and a click
        # on the button of that window (the top one already) changes nothing.
        mark = len(out)
        q.stdin.write(b"bwm -n\r"); q.stdin.flush()
        good = until(b"bwm: ready", mark)
        if good:
            time.sleep(1)
            monitor("mouse_move -484 372", "mouse_button 1", "mouse_button 0")
            good = until(b"bterm: window 1", mark)
        print(("ok  " if good else "FAIL"), f"({name}) the Term button of the task bar starts a terminal window")
        fails += not good
        if good:
            time.sleep(1.5)
            w, h, before = screendump()
            monitor("mouse_move 60 0", "mouse_button 1", "mouse_button 0")
            time.sleep(0.5)
            w, h, after = screendump()
            good = before[:w * 3 * 740] == after[:w * 3 * 740]       # above the bar, where the pointer is
            print(("ok  " if good else "FAIL"), f"({name}) a click on the button of the top window changes nothing")
            fails += not good
            # Alt-Left and Alt-Right put the window on half of the screen, Alt-Up
            # on all of it (a title bar along the top shows where it reaches).
            def snapped(key):
                monitor(f"sendkey {key}")
                time.sleep(1.5)
                w, h, px = screendump()
                return lambda x: tuple(px[(3 * w + x) * 3:(3 * w + x) * 3 + 3]) == (40, 90, 200)
            # Alt-F10 maximizes the window and puts it back.
            blue = snapped("alt-f10")
            good = blue(300) and blue(800)
            blue = snapped("alt-f10")
            good = good and not blue(1000)
            print(("ok  " if good else "FAIL"), f"({name}) Alt-F10 maximizes the window and puts it back")
            fails += not good
            blue = snapped("alt-left")
            good = blue(300) and not blue(800)
            print(("ok  " if good else "FAIL"), f"({name}) Alt-Left puts the window on the left half of the screen")
            fails += not good
            blue = snapped("alt-right")
            good = blue(800) and not blue(300)
            print(("ok  " if good else "FAIL"), f"({name}) Alt-Right puts the window on the right half of the screen")
            fails += not good
            blue = snapped("alt-up")
            good = blue(300) and blue(800)
            print(("ok  " if good else "FAIL"), f"({name}) Alt-Up makes the window fill the screen")
            fails += not good
            # A double click on the title bar of the full-screen window puts it back.
            for step in ((200, -200), (12, -200), (0, -200), (0, -146)):     # from the task bar button (88, 756) onto the title bar at (300, 10)
                monitor(f"mouse_move {step[0]} {step[1]}")
            # bwm counts a second press within 400 ms of its clock as a double
            # click, so the presses go 0.1 s apart. On a loaded host the guest
            # can still see them further apart: two single clicks change
            # nothing, and the double click is tried again, three times at most.
            for _ in range(3):
                monitor("mouse_button 1", "mouse_button 0", "mouse_button 1", "mouse_button 0", gap=0.05)
                time.sleep(1.5)
                w, h, px = screendump()
                good = not (tuple(px[(3 * w + 1000) * 3:(3 * w + 1000) * 3 + 3]) == (40, 90, 200))
                if good:
                    break
            print(("ok  " if good else "FAIL"), f"({name}) a double click on the title bar restores a maximized window")
            fails += not good
            blue = snapped("alt-up")
            blue = snapped("alt-down")
            good = not blue(1000)
            print(("ok  " if good else "FAIL"), f"({name}) Alt-Down puts a maximized window back")
            fails += not good
            blue = snapped("alt-down")
            good = not any(blue(x) for x in range(0, 1000, 8))
            print(("ok  " if good else "FAIL"), f"({name}) Alt-Down minimizes a window that is not maximized")
            fails += not good
            # The task bar button brings the window back; Alt-F9 minimizes it again.
            for step in ((-200, -200),) * 4 + ((88, 200), (0, 200), (0, 200), (0, 156)):
                monitor(f"mouse_move {step[0]} {step[1]}")
            monitor("mouse_button 1", "mouse_button 0", gap=0.05)
            time.sleep(1)
            blue = snapped("alt-f9")
            good = not any(blue(x) for x in range(0, 1000, 8))
            print(("ok  " if good else "FAIL"), f"({name}) Alt-F9 minimizes the focused window")
            fails += not good
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # The launcher menu: the Menu button at the right end of the task bar
        # opens a list above it, Terminal starts a terminal window, Quit ends bwm.
        mark = len(out)
        q.stdin.write(b"bwm -n\r"); q.stdin.flush()
        good = until(b"bwm: ready", mark)
        if good:
            time.sleep(1)
            w, h, px = screendump()
            # The clock left of the Menu button: white digits on the task bar.
            clock = any(tuple(px[(y * w + x) * 3:(y * w + x) * 3 + 3]) == (255, 255, 255) for y in range(746, 766) for x in range(915, 965))
            print(("ok  " if clock else "FAIL"), f"({name}) the task bar shows a clock")
            fails += not clock
            # A right click on the empty desktop opens the same menu at the pointer; a click elsewhere closes it.
            monitor("mouse_button 2", "mouse_button 0")
            time.sleep(0.7)
            w, h, px = screendump()
            at = (444 * w + 642) * 3
            good = tuple(px[at:at + 3]) == (45, 45, 60)
            print(("ok  " if good else "FAIL"), f"({name}) a right click on the desktop opens the launcher menu at the pointer")
            fails += not good
            monitor("mouse_move 300 0")
            monitor("mouse_button 1", "mouse_button 0")
            monitor("mouse_move -300 0")
            time.sleep(0.7)
            w, h, px = screendump()
            good = tuple(px[at:at + 3]) != (45, 45, 60)
            print(("ok  " if good else "FAIL"), f"({name}) a click elsewhere closes it")
            fails += not good
            monitor("mouse_move 482 372")                          # onto the Menu button
            monitor("mouse_button 1", "mouse_button 0")
            time.sleep(0.7)
            w, h, px = screendump()
            at = (690 * w + 1000) * 3
            good = tuple(px[at:at + 3]) == (45, 45, 60)
            print(("ok  " if good else "FAIL"), f"({name}) the Menu button opens the launcher menu")
            fails += not good
            monitor("mouse_move -34 -120")                         # onto Terminal
            monitor("mouse_button 1", "mouse_button 0")
            good = until(b"bterm: window", mark)
            print(("ok  " if good else "FAIL"), f"({name}) the menu's Terminal entry starts a terminal window")
            fails += not good
            time.sleep(1)
            monitor("mouse_move 34 120")
            monitor("mouse_button 1", "mouse_button 0")
            time.sleep(0.5)
            monitor("mouse_move -34 -96")                          # onto Files
            monitor("mouse_button 1", "mouse_button 0")
            good = until(b"bfiles: dir /", mark) and until(b"bfiles: window", mark)
            print(("ok  " if good else "FAIL"), f"({name}) the menu's Files entry opens the file browser on /")
            fails += not good
            monitor("mouse_move 34 96")
            monitor("mouse_button 1", "mouse_button 0")
            time.sleep(0.5)
            monitor("mouse_move -34 -72")                          # onto System
            monitor("mouse_button 1", "mouse_button 0")
            good = until(b"bsys: window", mark)
            if good:
                time.sleep(1)
                monitor("sendkey r")
                good = until(b"bsys: refreshed", mark)
            print(("ok  " if good else "FAIL"), f"({name}) the menu's System entry opens the System window and r refreshes it")
            fails += not good
            # left alone, the window reads the numbers again by itself.
            good = good and until(b"bsys: live", mark)
            print(("ok  " if good else "FAIL"), f"({name}) the System window refreshes itself")
            fails += not good
            # F5 does what r does.
            if good:
                mark = len(out)
                monitor("sendkey f5")
                good = until(b"bsys: refreshed", mark)
            print(("ok  " if good else "FAIL"), f"({name}) F5 refreshes the System window")
            fails += not good
            # p swaps the figures for the list of processes; j walks down it to the sleeping
            # one and x ends it.
            monitor("sendkey p")
            good = until(b"bsys: processes", mark)
            found = False
            for _ in range(24):
                if not good or found:
                    break
                monitor("sendkey j")
                end = time.time() + 0.6
                while time.time() < end:
                    r, _, _ = select.select([q.stdout], [], [], 0.2)
                    if r:
                        out += q.stdout.read1(4096)
                found = (b"bsys: select %d " % sleeper) in out[mark:]
            print(("ok  " if found else "FAIL"), f"({name}) p lists the processes and j moves to one")
            fails += not found
            if found:
                monitor("sendkey x")
                good = until(b"bsys: terminated %d" % sleeper, mark)
            else:
                good = False
            print(("ok  " if good else "FAIL"), f"({name}) x ends the selected process")
            fails += not good
            # Delete does what x does.
            monitor("sendkey g")
            found = False
            for _ in range(24):
                if not good or found:
                    break
                monitor("sendkey j")
                end = time.time() + 0.6
                while time.time() < end:
                    r, _, _ = select.select([q.stdout], [], [], 0.2)
                    if r:
                        out += q.stdout.read1(4096)
                found = (b"bsys: select %d " % sleeper2) in out[mark:]
            if found:
                monitor("sendkey delete")
                good = until(b"bsys: terminated %d" % sleeper2, mark)
            else:
                good = False
            print(("ok  " if good else "FAIL"), f"({name}) Delete ends the selected process")
            fails += not good
            # G goes to the last process and g to the first.
            mark3 = len(out)
            monitor("sendkey shift-g")
            good = until(b"bsys: select", mark3)
            last = bytes(out[mark3:]).split(b"bsys: select ")[-1].split(b"\n")[0] if good else b""
            mark3 = len(out)
            monitor("sendkey g")
            good = good and until(b"bsys: select", mark3)
            first = bytes(out[mark3:]).split(b"bsys: select ")[-1].split(b"\n")[0] if good else b""
            good = good and last != first and len(first) > 2 and len(last) > 2
            print(("ok  " if good else "FAIL"), f"({name}) G and g in the process list go to the last and the first ({last!r} {first!r})")
            fails += not good
            # Page Down moves a page along the list and Page Up back to where it was.
            mark3 = len(out)
            monitor("sendkey pgdn")
            good = until(b"bsys: select", mark3)
            down = bytes(out[mark3:]).split(b"bsys: select ")[-1].split(b"\n")[0] if good else b""
            mark3 = len(out)
            monitor("sendkey pgup")
            good = good and until(b"bsys: select", mark3)
            up = bytes(out[mark3:]).split(b"bsys: select ")[-1].split(b"\n")[0] if good else b""
            good = good and down != first and up == first
            print(("ok  " if good else "FAIL"), f"({name}) Page Down and Page Up move along the process list ({first!r} {down!r} {up!r})")
            fails += not good
            # n sorts the list by name: the first name is not after the last, and n again goes back to newest first.
            mark3 = len(out)
            monitor("sendkey n")
            good = until(b"bsys: sorted by name", mark3)
            names = []
            for key in ("g", "shift-g"):
                mark3 = len(out)
                monitor("sendkey " + key)
                good = good and until(b"bsys: select", mark3)
                sel = bytes(out[mark3:]).split(b"bsys: select ")[-1].split(b"\n")[0].split() if good else []
                names.append(sel[1] if len(sel) > 1 else b"")
            mark3 = len(out)
            monitor("sendkey n")
            good = good and until(b"bsys: sorted by pid", mark3) and names[0] and names[0] <= names[1]
            print(("ok  " if good else "FAIL"), f"({name}) n sorts the process list by name ({names!r})")
            fails += not good
            monitor("sendkey p")
            good = until(b"bsys: system", mark)
            print(("ok  " if good else "FAIL"), f"({name}) p goes back to the figures")
            fails += not good
            monitor("mouse_move 34 72")
            monitor("mouse_button 1", "mouse_button 0")
            time.sleep(0.5)
            monitor("mouse_move -34 -24")                          # onto Quit bwm
            monitor("mouse_button 1", "mouse_button 0")
            good = until(b"bwm: bye", mark)
            print(("ok  " if good else "FAIL"), f"({name}) the menu's Quit entry ends bwm")
            fails += not good
        else:
            monitor("sendkey ctrl-q")
            until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        mark = len(out)
        q.stdin.write(b"kill -0 %d 2>/dev/null || echo gone$((1+1))\r" % sleeper); q.stdin.flush()
        good = until(b"gone2", mark)
        print(("ok  " if good else "FAIL"), f"({name}) the process the System window ended is gone")
        fails += not good
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # A small screen: bwm -s 640x480 pretends the screen is that big. A
        # terminal gets a grid that fits it (80 columns would not), and the
        # window opens inside the screen, not past its right and bottom edge.
        mark = len(out)
        q.stdin.write(b"bwm -n -s 640x480 -c bterm\r"); q.stdin.flush()
        good = until(b"bterm: window", mark)
        print(("ok  " if good else "FAIL"), f"({name}) a terminal opens on a 640 x 480 screen")
        fails += not good
        if good:
            time.sleep(1)
            w, h, px = screendump()
            def title_at(x, y):
                at = (y * w + x) * 3
                return tuple(px[at:at + 3]) == (40, 90, 200)
            inside = any(title_at(600, y) for y in range(0, 480))
            beyond = any(title_at(x, y) for x in (660, 700, 1000) for y in range(0, 480)) or any(title_at(x, y) for x in range(0, 640, 8) for y in range(484, 700))
            good = inside and not beyond
            print(("ok  " if good else "FAIL"), f"({name}) its window lies inside the small screen")
            fails += not good
            # Dragging its title bar far up and to the left stops with some of
            # the title bar on screen: the window cannot be lost off the edge.
            def movep(dx, dy):
                while dx or dy:
                    sx, sy = max(-200, min(200, dx)), max(-200, min(200, dy))
                    monitor(f"mouse_move {sx} {sy}")
                    dx, dy = dx - sx, dy - sy
            # Over the corner of a window the pointer is a resize cursor, a
            # white cross-hair at its centre; the arrow has a black outline there.
            xr = max(x for x in range(0, 640) if title_at(x, 53))
            movep(xr - 320, 53 - 240)                              # onto the top right corner, in the title bar
            time.sleep(0.5)
            w, h, px = screendump()
            at = (53 * w + xr) * 3
            good = tuple(px[at:at + 3]) == (255, 255, 255)
            print(("ok  " if good else "FAIL"), f"({name}) the pointer turns into a resize cursor over a window's edge")
            fails += not good
            movep(100 - xr, 7)                                     # on to the title bar, where the drag starts
            monitor("mouse_button 1")
            movep(-300, -200)
            movep(-300, -200)
            monitor("mouse_button 0")
            time.sleep(0.5)
            w, h, px = screendump()
            good = title_at(30, 10) and not title_at(30, 30)
            print(("ok  " if good else "FAIL"), f"({name}) a window dragged off the screen keeps part of its title bar on it")
            fails += not good
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # The mouse wheel scrolls a window under the pointer: bfiles on /bin
        # (more entries than rows) scrolls three entries for one step down.
        mark = len(out)
        q.stdin.write(b"bwm -n -c bfiles /bin\r"); q.stdin.flush()
        good = until(b"bfiles: window", mark)
        if good:
            time.sleep(1)
            # dz of mouse_move is the wheel (1 is up, -1 down); park the pointer inside the window first
            monitor("mouse_move -200 -200", "mouse_move -200 -200", "mouse_move -200 -200", "mouse_move -200 -200",
                    "mouse_move 200 150", "mouse_move 100 100", "mouse_move 0 0 -1")
            good = until(b"bfiles: top 3", mark)
        print(("ok  " if good else "FAIL"), f"({name}) the mouse wheel scrolls the window under the pointer")
        fails += not good
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # The wheel walks the bsys process list: down from the first process moves off it.
        mark = len(out)
        q.stdin.write(b"bwm -n -c bsys\r"); q.stdin.flush()
        good = until(b"bsys: window", mark)
        if good:
            time.sleep(1)
            monitor("sendkey p")
            good = until(b"bsys: processes", mark)
        if good:
            mark3 = len(out)
            monitor("mouse_move -200 -200", "mouse_move -200 -200", "mouse_move -200 -200", "mouse_move -200 -200",
                    "mouse_move 200 150", "mouse_move 100 100", "mouse_move 0 0 -1")
            good = until(b"bsys: select", mark3)
        print(("ok  " if good else "FAIL"), f"({name}) the wheel walks the process list")
        fails += not good
        # A click on a process in the list selects it: the fourth row is what g followed by three j reaches.
        clicked = keyed = b""
        if good:
            mark3 = len(out)
            monitor("mouse_move -200 -200", "mouse_move -200 -200", "mouse_move -200 -200", "mouse_move -200 -200",
                    "mouse_move 150 %d" % (90 + 5 * 16 + 8), "mouse_button 1", "mouse_button 0")
            good = until(b"bsys: select", mark3)
            clicked = bytes(out[mark3:]).split(b"bsys: select ")[-1].split(b"\n")[0] if good else b""
            mark3 = len(out)
            monitor("sendkey g", "sendkey j", "sendkey j", "sendkey j")
            end = time.time() + 2
            while time.time() < end:
                r, _, _ = select.select([q.stdout], [], [], 0.2)
                if r:
                    out += q.stdout.read1(4096)
            keyed = bytes(out[mark3:]).split(b"bsys: select ")[-1].split(b"\n")[0] if b"bsys: select" in out[mark3:] else b""
            good = good and clicked == keyed and len(clicked) > 2
        print(("ok  " if good else "FAIL"), f"({name}) a click on a process selects it ({clicked!r} {keyed!r})")
        fails += not good
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # Alt-F4 closes the focused window: bhello answers the close request.
        mark = len(out)
        q.stdin.write(b"bwm -n -c bhello\r"); q.stdin.flush()
        good = until(b"bhello: window", mark)
        if good:
            time.sleep(1)
            monitor("sendkey alt-f4")
            good = until(b"bhello: closed", mark)
        print(("ok  " if good else "FAIL"), f"({name}) Alt-F4 closes the focused window")
        fails += not good
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # bfiles opens a file in its viewer, e edits it: typing inserts at the
        # cursor, Ctrl-S saves, and the shell reads the new contents back.
        mark = len(out)
        q.stdin.write(b"echo hello > /tmp/e.txt\r"); q.stdin.flush()
        time.sleep(1)
        mark = len(out)
        q.stdin.write(b"bwm -n -c bfiles /tmp/e.txt\r"); q.stdin.flush()
        good = until(b"bfiles: view /tmp/e.txt", mark)
        print(("ok  " if good else "FAIL"), f"({name}) bfiles opens a file given on its command line")
        fails += not good
        if good:
            time.sleep(1)
            monitor("sendkey e")
            good = until(b"bfiles: edit /tmp/e.txt", mark)
            monitor("sendkey x", "sendkey y", "sendkey ret", "sendkey ctrl-s")
            good = good and until(b"bfiles: saved /tmp/e.txt", mark)
            print(("ok  " if good else "FAIL"), f"({name}) a file edited in bfiles is saved")
            fails += not good
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"cat /tmp/e.txt\r"); q.stdin.flush()
        good = until(b"xy\r\nhello", mark) or until(b"xy\nhello", mark)
        print(("ok  " if good else "FAIL"), f"({name}) the saved file holds the edit")
        fails += not good
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # The editor's Ctrl-G (go to a line), Ctrl-K and Ctrl-U (cut the rest of a line, put it
        # back) and Ctrl-F (find).
        mark = len(out)
        q.stdin.write(b"printf 'one\\ntwo\\nthree\\nfour\\n' > /tmp/g.txt\r"); q.stdin.flush()
        time.sleep(1)
        mark = len(out)
        q.stdin.write(b"bwm -n -c bfiles /tmp/g.txt\r"); q.stdin.flush()
        good = until(b"bfiles: view /tmp/g.txt", mark)
        if good:
            time.sleep(1)
            monitor("sendkey e")
            good = until(b"bfiles: edit /tmp/g.txt", mark)
        if good:
            monitor("sendkey ctrl-g", "sendkey 3", "sendkey ret")
            good = until(b"bfiles: line 3", mark)
        print(("ok  " if good else "FAIL"), f"({name}) Ctrl-G in the editor goes to a line")
        fails += not good
        if good:
            monitor("sendkey ctrl-k")
            good = until(b"bfiles: cut 5", mark)
        print(("ok  " if good else "FAIL"), f"({name}) Ctrl-K cuts the rest of the line")
        fails += not good
        if good:
            monitor("sendkey ctrl-g", "sendkey 1", "sendkey ret", "sendkey ctrl-u")
            monitor("sendkey ctrl-f", "sendkey f", "sendkey o", "sendkey u", "sendkey r", "sendkey ret")
            good = until(b"bfiles: found at 14", mark)
        print(("ok  " if good else "FAIL"), f"({name}) Ctrl-F finds text")
        fails += not good
        if good:
            # The text found is drawn on a different background wherever it shows in the window.
            time.sleep(1.5)
            w, h, px = screendump()
            hl = sum(1 for i in range(0, len(px) - 2, 3) if tuple(px[i:i + 3]) == (0x80, 0x60, 0x20))
            good = hl >= 100
            print(("ok  " if good else "FAIL"), f"({name}) the editor marks the text Ctrl-F looked for ({hl} pixels)")
            fails += not good
        if good:
            monitor("sendkey ctrl-s")
            good = until(b"bfiles: saved /tmp/g.txt", mark)
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"cat /tmp/g.txt\r"); q.stdin.flush()
        good = good and (until(b"threeone\r\ntwo\r\n\r\nfour", mark) or until(b"threeone\ntwo\n\nfour", mark))
        print(("ok  " if good else "FAIL"), f"({name}) Ctrl-U puts the cut text back and the file holds the edits")
        fails += not good
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # r and D in the list of bfiles rename and delete the entry under the cursor.
        mark = len(out)
        q.stdin.write(b"mkdir -p /tmp/rn/e; echo ra > /tmp/rn/a.txt; echo rb > /tmp/rn/b.txt\r"); q.stdin.flush()
        time.sleep(1)
        mark = len(out)
        q.stdin.write(b"bwm -n -c bfiles /tmp/rn\r"); q.stdin.flush()
        good = until(b"bfiles: dir /tmp/rn", mark) and until(b"bfiles: window", mark)
        if good:
            time.sleep(1)
            monitor("sendkey down", "sendkey down", "sendkey r", "sendkey x", "sendkey ret")   # a.txt
            good = until(b"bfiles: renamed /tmp/rn/a.txt to /tmp/rn/a.txtx", mark)
        print(("ok  " if good else "FAIL"), f"({name}) r in the bfiles list renames the entry under the cursor")
        fails += not good
        if good:
            monitor("sendkey home", "sendkey down", "sendkey shift-d")                        # the empty directory e
            time.sleep(0.5)
            monitor("sendkey shift-d")
            good = until(b"bfiles: deleted /tmp/rn/e", mark)
        print(("ok  " if good else "FAIL"), f"({name}) D twice in the bfiles list deletes the entry under the cursor")
        fails += not good
        # F2 does what r does.
        if good:
            monitor("sendkey home", "sendkey down", "sendkey f2", "sendkey y", "sendkey ret")
            good = until(b"bfiles: renamed /tmp/rn/a.txtx to /tmp/rn/a.txtxy", mark)
        print(("ok  " if good else "FAIL"), f"({name}) F2 in the bfiles list renames the entry under the cursor")
        fails += not good
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"[ ! -e /tmp/rn/e ] && [ -f /tmp/rn/a.txtxy ] && [ ! -e /tmp/rn/a.txt ] && [ -f /tmp/rn/b.txt ] && echo ok$((1+1))\r"); q.stdin.flush()
        good = good and until(b"ok2", mark)
        print(("ok  " if good else "FAIL"), f"({name}) the renamed file is under its new name and the directory is gone")
        fails += not good
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # Ctrl-V in the editor pastes the clipboard (what a bterm window selected).
        mark = len(out)
        q.stdin.write(b"printf 'abc\\n' > /tmp/v.txt; printf 'CLIP' > /tmp/bwm.clip\r"); q.stdin.flush()
        time.sleep(1)
        mark = len(out)
        q.stdin.write(b"bwm -n -c bfiles /tmp/v.txt\r"); q.stdin.flush()
        good = until(b"bfiles: view /tmp/v.txt", mark)
        if good:
            time.sleep(1)
            monitor("sendkey e")
            good = until(b"bfiles: edit /tmp/v.txt", mark)
        if good:
            monitor("sendkey ctrl-v")
            good = until(b"bfiles: pasted 4", mark)
        if good:
            monitor("sendkey ctrl-k")
            good = until(b"bfiles: cut 3", mark)
        if good:
            monitor("sendkey ctrl-s")
            good = until(b"bfiles: saved /tmp/v.txt", mark)
        print(("ok  " if good else "FAIL"), f"({name}) Ctrl-V in the editor pastes the clipboard")
        fails += not good
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"cat /tmp/v.txt /tmp/bwm.clip; echo end$((1+1))\r"); q.stdin.flush()
        good = good and (until(b"CLIP\r\nabcend2", mark) or until(b"CLIP\nabcend2", mark))
        print(("ok  " if good else "FAIL"), f"({name}) the clipboard text went into the file and a cut goes to the clipboard")
        fails += not good
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # Ctrl-Z in the editor undoes a run of typing.
        mark = len(out)
        q.stdin.write(b"printf 'abc\\n' > /tmp/z.txt\r"); q.stdin.flush()
        time.sleep(1)
        mark = len(out)
        q.stdin.write(b"bwm -n -c bfiles /tmp/z.txt\r"); q.stdin.flush()
        good = until(b"bfiles: view /tmp/z.txt", mark)
        if good:
            time.sleep(1)
            monitor("sendkey e")
            good = until(b"bfiles: edit /tmp/z.txt", mark)
        if good:
            monitor("sendkey x", "sendkey y", "sendkey ctrl-z")
            good = until(b"bfiles: undo", mark)
        print(("ok  " if good else "FAIL"), f"({name}) Ctrl-Z in the editor undoes the typing")
        fails += not good
        # Ctrl-Y puts it back, and a second Ctrl-Z takes it off again before the save.
        if good:
            monitor("sendkey ctrl-y")
            good = until(b"bfiles: redo", mark)
        if good:
            mark2 = len(out)
            monitor("sendkey ctrl-z")
            good = until(b"bfiles: undo", mark2)
        if good:
            monitor("sendkey ctrl-s")
            good = until(b"bfiles: saved /tmp/z.txt", mark)
        print(("ok  " if good else "FAIL"), f"({name}) Ctrl-Y in the editor redoes what Ctrl-Z took off")
        fails += not good
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"cat /tmp/z.txt; echo end$((1+1))\r"); q.stdin.flush()
        good = good and (until(b"abc\r\nend2", mark) or until(b"abc\nend2", mark))
        print(("ok  " if good else "FAIL"), f"({name}) the saved file is as it was before the typing")
        fails += not good
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # Ctrl-R in the editor replaces every occurrence of a text.
        mark = len(out)
        q.stdin.write(b"printf 'foo bar foo\\n' > /tmp/rp.txt\r"); q.stdin.flush()
        time.sleep(1)
        mark = len(out)
        q.stdin.write(b"bwm -n -c bfiles /tmp/rp.txt\r"); q.stdin.flush()
        good = until(b"bfiles: view /tmp/rp.txt", mark)
        if good:
            time.sleep(1)
            monitor("sendkey e")
            good = until(b"bfiles: edit /tmp/rp.txt", mark)
        if good:
            monitor("sendkey ctrl-r", "sendkey f", "sendkey o", "sendkey o", "sendkey ret",
                    "sendkey x", "sendkey y", "sendkey ret")
            good = until(b"bfiles: replaced 2", mark)
        if good:
            monitor("sendkey ctrl-s")
            good = until(b"bfiles: saved /tmp/rp.txt", mark)
        print(("ok  " if good else "FAIL"), f"({name}) Ctrl-R in the editor replaces every occurrence")
        fails += not good
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"cat /tmp/rp.txt; echo end$((1+1))\r"); q.stdin.flush()
        good = good and (until(b"xy bar xy\r\nend2", mark) or until(b"xy bar xy\nend2", mark))
        print(("ok  " if good else "FAIL"), f"({name}) the saved file has the text replaced")
        fails += not good
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # Ctrl-A and Ctrl-E in the editor go to the start and the end of the line.
        mark = len(out)
        q.stdin.write(b"printf 'bc\\n' > /tmp/ae.txt\r"); q.stdin.flush()
        time.sleep(1)
        mark = len(out)
        q.stdin.write(b"bwm -n -c bfiles /tmp/ae.txt\r"); q.stdin.flush()
        good = until(b"bfiles: view /tmp/ae.txt", mark)
        if good:
            time.sleep(1)
            monitor("sendkey e")
            good = until(b"bfiles: edit /tmp/ae.txt", mark)
        if good:
            monitor("sendkey ctrl-e", "sendkey d", "sendkey ctrl-a", "sendkey a", "sendkey ctrl-s")
            good = until(b"bfiles: saved /tmp/ae.txt", mark)
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"cat /tmp/ae.txt; echo end$((1+1))\r"); q.stdin.flush()
        good = good and (until(b"abcd\r\nend2", mark) or until(b"abcd\nend2", mark))
        print(("ok  " if good else "FAIL"), f"({name}) Ctrl-A and Ctrl-E in the editor go to the start and the end of the line")
        fails += not good
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # Enter in the editor keeps the indentation of the line.
        mark = len(out)
        q.stdin.write(b": > /tmp/ai.txt\r"); q.stdin.flush()
        time.sleep(1)
        mark = len(out)
        q.stdin.write(b"bwm -n -c bfiles /tmp/ai.txt\r"); q.stdin.flush()
        good = until(b"bfiles: view /tmp/ai.txt", mark)
        if good:
            time.sleep(1)
            monitor("sendkey e")
            good = until(b"bfiles: edit /tmp/ai.txt", mark)
        if good:
            monitor("sendkey spc", "sendkey spc", "sendkey a", "sendkey b", "sendkey ret", "sendkey c", "sendkey ctrl-s")
            good = until(b"bfiles: saved /tmp/ai.txt", mark)
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"cat /tmp/ai.txt; echo end$((1+1))\r"); q.stdin.flush()
        good = good and (until(b"  ab\r\n  cend2", mark) or until(b"  ab\n  cend2", mark))
        print(("ok  " if good else "FAIL"), f"({name}) Enter in the editor keeps the indentation")
        fails += not good
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # Ctrl-Left and Ctrl-Right in the editor move by words.
        mark = len(out)
        q.stdin.write(b"printf 'foo bar baz\\n' > /tmp/wd.txt\r"); q.stdin.flush()
        time.sleep(1)
        mark = len(out)
        q.stdin.write(b"bwm -n -c bfiles /tmp/wd.txt\r"); q.stdin.flush()
        good = until(b"bfiles: view /tmp/wd.txt", mark)
        if good:
            time.sleep(1)
            monitor("sendkey e")
            good = until(b"bfiles: edit /tmp/wd.txt", mark)
        if good:
            monitor("sendkey ctrl-right", "sendkey ctrl-right", "sendkey x", "sendkey ctrl-left", "sendkey y", "sendkey ctrl-s")
            good = until(b"bfiles: saved /tmp/wd.txt", mark)
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"cat /tmp/wd.txt; echo end$((1+1))\r"); q.stdin.flush()
        good = good and (until(b"foo ybarx baz\r\nend2", mark) or until(b"foo ybarx baz\nend2", mark))
        print(("ok  " if good else "FAIL"), f"({name}) Ctrl-Left and Ctrl-Right in the editor move by words")
        fails += not good
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # Ctrl-D in the editor copies the line under the cursor below it.
        mark = len(out)
        q.stdin.write(b"printf 'foo\\nbar\\n' > /tmp/dd.txt\r"); q.stdin.flush()
        time.sleep(1)
        mark = len(out)
        q.stdin.write(b"bwm -n -c bfiles /tmp/dd.txt\r"); q.stdin.flush()
        good = until(b"bfiles: view /tmp/dd.txt", mark)
        if good:
            time.sleep(1)
            monitor("sendkey e")
            good = until(b"bfiles: edit /tmp/dd.txt", mark)
        if good:
            monitor("sendkey ctrl-d", "sendkey x", "sendkey ctrl-s")
            good = until(b"bfiles: saved /tmp/dd.txt", mark)
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"cat /tmp/dd.txt; echo end$((1+1))\r"); q.stdin.flush()
        good = good and (until(b"foo\r\nxfoo\r\nbar\r\nend2", mark) or until(b"foo\nxfoo\nbar\nend2", mark))
        print(("ok  " if good else "FAIL"), f"({name}) Ctrl-D in the editor copies the line below it")
        fails += not good
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # Ctrl-O in the editor opens an empty line below the cursor.
        mark = len(out)
        q.stdin.write(b"printf 'foo\\nbar\\n' > /tmp/oo.txt\r"); q.stdin.flush()
        time.sleep(1)
        mark = len(out)
        q.stdin.write(b"bwm -n -c bfiles /tmp/oo.txt\r"); q.stdin.flush()
        good = until(b"bfiles: view /tmp/oo.txt", mark)
        if good:
            time.sleep(1)
            monitor("sendkey e")
            good = until(b"bfiles: edit /tmp/oo.txt", mark)
        if good:
            monitor("sendkey ctrl-o", "sendkey x", "sendkey ctrl-s")
            good = until(b"bfiles: saved /tmp/oo.txt", mark)
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"cat /tmp/oo.txt; echo end$((1+1))\r"); q.stdin.flush()
        good = good and (until(b"foo\r\nx\r\nbar\r\nend2", mark) or until(b"foo\nx\nbar\nend2", mark))
        print(("ok  " if good else "FAIL"), f"({name}) Ctrl-O in the editor opens a line below")
        fails += not good
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # Ctrl-B in the editor swaps the two characters before the cursor.
        mark = len(out)
        q.stdin.write(b"printf 'abc\\nbar\\n' > /tmp/tt.txt\r"); q.stdin.flush()
        time.sleep(1)
        mark = len(out)
        q.stdin.write(b"bwm -n -c bfiles /tmp/tt.txt\r"); q.stdin.flush()
        good = until(b"bfiles: view /tmp/tt.txt", mark)
        if good:
            time.sleep(1)
            monitor("sendkey e")
            good = until(b"bfiles: edit /tmp/tt.txt", mark)
        if good:
            monitor("sendkey ctrl-e", "sendkey ctrl-b", "sendkey ctrl-s")
            good = until(b"bfiles: saved /tmp/tt.txt", mark)
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"cat /tmp/tt.txt; echo end$((1+1))\r"); q.stdin.flush()
        good = good and (until(b"acb\r\nbar\r\nend2", mark) or until(b"acb\nbar\nend2", mark))
        print(("ok  " if good else "FAIL"), f"({name}) Ctrl-B in the editor swaps two characters")
        fails += not good
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # Shift-Tab in the editor takes four blanks off the start of the line.
        mark = len(out)
        q.stdin.write(b"printf '      ab\\nbar\\n' > /tmp/st.txt\r"); q.stdin.flush()
        time.sleep(1)
        mark = len(out)
        q.stdin.write(b"bwm -n -c bfiles /tmp/st.txt\r"); q.stdin.flush()
        good = until(b"bfiles: view /tmp/st.txt", mark)
        if good:
            time.sleep(1)
            monitor("sendkey e")
            good = until(b"bfiles: edit /tmp/st.txt", mark)
        if good:
            monitor("sendkey shift-tab", "sendkey ctrl-s")
            good = until(b"bfiles: saved /tmp/st.txt", mark)
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"cat /tmp/st.txt; echo end$((1+1))\r"); q.stdin.flush()
        good = good and (until(b"  ab\r\nbar\r\nend2", mark) or until(b"  ab\nbar\nend2", mark))
        print(("ok  " if good else "FAIL"), f"({name}) Shift-Tab in the editor takes four blanks off the line")
        fails += not good
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # Ctrl-W in the editor deletes the word before the cursor.
        mark = len(out)
        q.stdin.write(b"printf 'foo bar baz\\n' > /tmp/dw.txt\r"); q.stdin.flush()
        time.sleep(1)
        mark = len(out)
        q.stdin.write(b"bwm -n -c bfiles /tmp/dw.txt\r"); q.stdin.flush()
        good = until(b"bfiles: view /tmp/dw.txt", mark)
        if good:
            time.sleep(1)
            monitor("sendkey e")
            good = until(b"bfiles: edit /tmp/dw.txt", mark)
        if good:
            monitor("sendkey ctrl-e", "sendkey ctrl-w", "sendkey ctrl-w", "sendkey x", "sendkey ctrl-s")
            good = until(b"bfiles: saved /tmp/dw.txt", mark)
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"cat /tmp/dw.txt; echo end$((1+1))\r"); q.stdin.flush()
        good = good and (until(b"foo x\r\nend2", mark) or until(b"foo x\nend2", mark))
        print(("ok  " if good else "FAIL"), f"({name}) Ctrl-W in the editor deletes the word before the cursor")
        fails += not good
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # The editor shows UTF-8 and moves, deletes by character: Right then Backspace takes off one hiragana (3 bytes).
        mark = len(out)
        q.stdin.write(b"printf '\\343\\201\\202\\343\\201\\204\\343\\201\\206\\n' > /tmp/ju.txt\r"); q.stdin.flush()
        time.sleep(1)
        mark = len(out)
        q.stdin.write(b"bwm -n -c bfiles /tmp/ju.txt\r"); q.stdin.flush()
        good = until(b"bfiles: view /tmp/ju.txt", mark)
        if good:
            time.sleep(1)
            monitor("sendkey e")
            good = until(b"bfiles: edit /tmp/ju.txt", mark)
        if good:
            monitor("sendkey ctrl-e")                   # the cursor is off the text, which is drawn plain
            time.sleep(1.5)
            w, h, px = screendump()
            lines = screen_text_at(px, w, 72, 90, 60, 4)
            shown = any(l.startswith("あいう") for l in lines)
            if not shown:
                print("\n".join(l for l in lines if l))
            print(("ok  " if shown else "FAIL"), f"({name}) the editor draws Japanese text")
            fails += not shown
            monitor("sendkey ctrl-a", "sendkey right", "sendkey backspace", "sendkey ctrl-s")
            good = until(b"bfiles: saved /tmp/ju.txt", mark)
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"echo size $(wc -c < /tmp/ju.txt) end\r"); q.stdin.flush()
        good = good and until(b"size 7 end", mark)
        print(("ok  " if good else "FAIL"), f"({name}) the editor moves and deletes by character")
        fails += not good
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # Ctrl-End and Ctrl-Home in the editor go to the end and the start of the text.
        mark = len(out)
        q.stdin.write(b"printf 'ab\\ncd\\n' > /tmp/ce.txt\r"); q.stdin.flush()
        time.sleep(1)
        mark = len(out)
        q.stdin.write(b"bwm -n -c bfiles /tmp/ce.txt\r"); q.stdin.flush()
        good = until(b"bfiles: view /tmp/ce.txt", mark)
        if good:
            time.sleep(1)
            monitor("sendkey e")
            good = until(b"bfiles: edit /tmp/ce.txt", mark)
        if good:
            monitor("sendkey ctrl-end", "sendkey x", "sendkey ctrl-home", "sendkey y", "sendkey ctrl-s")
            good = until(b"bfiles: saved /tmp/ce.txt", mark)
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"cat /tmp/ce.txt; echo end$((1+1))\r"); q.stdin.flush()
        good = good and (until(b"yab\r\ncd\r\nxend2", mark) or until(b"yab\ncd\nxend2", mark))
        print(("ok  " if good else "FAIL"), f"({name}) Ctrl-Home and Ctrl-End in the editor go to the start and the end of the text")
        fails += not good
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # Shift-arrows select in the editor; typing replaces the selection, Ctrl-C copies it and Ctrl-W cuts it.
        mark = len(out)
        q.stdin.write(b"printf 'hello world\\n' > /tmp/ds.txt\r"); q.stdin.flush()
        time.sleep(1)
        mark = len(out)
        q.stdin.write(b"bwm -n -c bfiles /tmp/ds.txt\r"); q.stdin.flush()
        good = until(b"bfiles: view /tmp/ds.txt", mark)
        if good:
            time.sleep(1)
            monitor("sendkey e")
            good = until(b"bfiles: edit /tmp/ds.txt", mark)
        if good:
            monitor("sendkey ctrl-a", *["sendkey shift-right"] * 5, "sendkey ctrl-c")
            good = until(b"bfiles: copied 5", mark)
            monitor("sendkey x", "sendkey ctrl-e", "sendkey ctrl-v")
            good = good and until(b"bfiles: pasted 5", mark)
            monitor("sendkey ctrl-a", "sendkey shift-right", "sendkey ctrl-w")
            good = good and until(b"bfiles: cut 1", mark)
            monitor("sendkey ctrl-s")
            good = good and until(b"bfiles: saved /tmp/ds.txt", mark)
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"cat /tmp/ds.txt; echo end$((1+1))\r"); q.stdin.flush()
        good = good and (until(b" worldhello\r\nend2", mark) or until(b" worldhello\nend2", mark))
        print(("ok  " if good else "FAIL"), f"({name}) Shift-arrows select in the editor and typing, Ctrl-C and Ctrl-W work on the selection")
        fails += not good
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # The file list and the viewer of bfiles draw UTF-8 with the big font: a file named with a hiragana, and its text.
        mark = len(out)
        q.stdin.write(b"mkdir -p /tmp/jp; printf '\\343\\201\\202 \\343\\201\\204\\n' > \"/tmp/jp/$(printf '\\343\\201\\202').txt\"\r"); q.stdin.flush()
        time.sleep(1)
        mark = len(out)
        q.stdin.write(b"bwm -n -c bfiles /tmp/jp\r"); q.stdin.flush()
        good = until(b"bfiles: dir /tmp/jp", mark) and until(b"bfiles: window", mark)
        if good:
            time.sleep(1.5)
            w, h, px = screendump()
            lines = screen_text_at(px, w, 72, 90, 60, 4)
            good = any(l.startswith("あ.txt") for l in lines)
            if not good:
                print("\n".join(l for l in lines if l))
        print(("ok  " if good else "FAIL"), f"({name}) the file list of bfiles draws a Japanese file name")
        fails += not good
        if good:
            monitor("sendkey down", "sendkey down", "sendkey ret")
            good = until(b"bfiles: view /tmp/jp/", mark)
            time.sleep(1)
            w, h, px = screendump()
            lines = screen_text_at(px, w, 72, 90, 60, 4)
            good = good and any(l.strip() == "あ い" for l in lines)
            if not good:
                print("\n".join(l for l in lines if l))
            print(("ok  " if good else "FAIL"), f"({name}) the viewer of bfiles draws Japanese text")
            fails += not good
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # Dragging over text in the editor selects it.
        mark = len(out)
        q.stdin.write(b"printf 'alpha beta\\n' > /tmp/dm.txt\r"); q.stdin.flush()
        time.sleep(1)
        mark = len(out)
        q.stdin.write(b"bwm -n -c bfiles /tmp/dm.txt\r"); q.stdin.flush()
        good = until(b"bfiles: view /tmp/dm.txt", mark)
        if good:
            time.sleep(1)
            monitor("sendkey e")
            good = until(b"bfiles: edit /tmp/dm.txt", mark)
        if good:
            time.sleep(1.5)
            w, h, px = screendump()
            lines = screen_text_at(px, w, 72, 90, 80, 24)
            row = next((i for i, l in enumerate(lines) if l.strip() == "alpha beta"), None)
            good = row is not None
            if not good:
                print("\n".join(l for l in lines if l))
        if good:
            def movep(dx, dy):
                n = max(1, (max(abs(dx), abs(dy)) + 199) // 200)
                monitor(*[f"mouse_move {dx * (i + 1) // n - dx * i // n} {dy * (i + 1) // n - dy * i // n}" for i in range(n)])
            ty = 90 + row * 16 + 8
            movep(72 + 2 * 8 + 4 - 512, ty - 384)
            monitor("mouse_button 1")
            movep(5 * 8, 0)
            monitor("mouse_button 0")
            time.sleep(0.5)
            monitor("sendkey ctrl-c")
            good = until(b"bfiles: copied 5", mark)
        print(("ok  " if good else "FAIL"), f"({name}) dragging over text in the editor selects it")
        fails += not good
        if good:
            # The pointer is over "beta" now: a double click selects the word.
            monitor("mouse_button 1", "mouse_button 0", "mouse_button 1", "mouse_button 0", gap=0.05)
            time.sleep(0.5)
            monitor("sendkey ctrl-c")
            good = until(b"bfiles: copied 4", mark)
            print(("ok  " if good else "FAIL"), f"({name}) a double click in the editor selects the word")
            fails += not good
        if good:
            # And three presses in a row select the line with its newline.
            monitor("mouse_button 1", "mouse_button 0", "mouse_button 1", "mouse_button 0", "mouse_button 1", "mouse_button 0", gap=0.05)
            time.sleep(0.5)
            monitor("sendkey ctrl-c")
            good = until(b"bfiles: copied 11", mark)
            print(("ok  " if good else "FAIL"), f"({name}) a triple click in the editor selects the line")
            fails += not good
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # A drag that leaves the window goes on: below the text it scrolls the editor and stretches the selection.
        mark = len(out)
        q.stdin.write(b"i=10; while [ $i -le 69 ]; do echo line-$i; i=$((i+1)); done > /tmp/ds.txt\r"); q.stdin.flush()
        time.sleep(1)
        mark = len(out)
        q.stdin.write(b"bwm -n -c bfiles /tmp/ds.txt\r"); q.stdin.flush()
        good = until(b"bfiles: view /tmp/ds.txt", mark)
        if good:
            time.sleep(1)
            monitor("sendkey e")
            good = until(b"bfiles: edit /tmp/ds.txt", mark)
        if good:
            time.sleep(1.5)
            monitor("mouse_move -216 -135", "mouse_move -216 -135")     # from the screen centre to the first text row
            monitor("mouse_button 1")
            monitor(*["mouse_move 0 8"] * 70)
            monitor("mouse_button 0")
            # The header of the editor shows the number of the cursor's row: the visible rows are 23, so a row past 40
            # is there only if the motion outside the window reached the editor and scrolled it. A slow guest is still
            # working through the motion when the button is let go, so look until the number settles or 40 s pass.
            good = False
            for _ in range(40):
                time.sleep(1)
                w, h, px = screendump()
                lines = screen_text_at(px, w, 72, 90, 60, 26)
                mt = re.search(r"\^U (\d+)", lines[0])
                good = mt is not None and int(mt.group(1)) >= 40
                if good:
                    break
            if not good:
                print("\n".join(l for l in lines if l))
        print(("ok  " if good else "FAIL"), f"({name}) a drag that leaves the editor window scrolls it")
        fails += not good
        monitor("sendkey ctrl-q")
        # With the pointer resting below the window and the button still held, the editor goes on scrolling.
        time.sleep(4)                                       # bwm waits for the ^Q byte before it ends
        mark = len(out)
        q.stdin.write(b"bwm -n -c bfiles /tmp/ds.txt\r"); q.stdin.flush()
        good = until(b"bfiles: view /tmp/ds.txt", mark)
        if good:
            time.sleep(1)
            monitor("sendkey e")
            good = until(b"bfiles: edit /tmp/ds.txt", mark)
        if good:
            time.sleep(1.5)
            monitor("mouse_move -216 -135", "mouse_move -216 -135")
            monitor("mouse_button 1")
            monitor(*["mouse_move 0 60"] * 7)
            good = False
            for _ in range(40):
                time.sleep(1)
                w, h, px = screendump()
                lines = screen_text_at(px, w, 72, 90, 60, 26)
                mt = re.search(r"\^U (\d+)", lines[0])
                good = mt is not None and int(mt.group(1)) >= 60
                if good:
                    break
            monitor("mouse_button 0")
            if not good:
                print("\n".join(l for l in lines if l))
        print(("ok  " if good else "FAIL"), f"({name}) a drag held below the editor window keeps scrolling it")
        fails += not good
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # The clock in the task bar follows TZ (POSIX form): XXX-5 is five hours east of UTC.
        import datetime
        utc_h = datetime.datetime.utcnow().hour    # the guest clock comes from the host RTC
        mark = len(out)
        q.stdin.write(b"TZ=XXX-5 bwm -n -c bsys\r"); q.stdin.flush()
        good = until(b"bsys: window", mark) or until(b"bwm: ", mark)
        if good:
            time.sleep(2)
            w, h, px = screendump()
            clk = screen_text_at(px, w, w - 52 - 12 - 48 + 4, h - 24 + 4, 5, 1)[0]
            hh = clk[:2]
            good = hh.isdigit() and int(hh) in ((utc_h + 4) % 24, (utc_h + 5) % 24, (utc_h + 6) % 24)
            if not good:
                print("clock:", repr(clk), "utc hour:", utc_h)
        print(("ok  " if good else "FAIL"), f"({name}) the task bar clock follows TZ")
        fails += not good
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # c, x and v in the list of bfiles copy and move a file into another directory.
        mark = len(out)
        q.stdin.write(b"mkdir -p /tmp/pf/d; echo pa > /tmp/pf/a.txt; echo pb > /tmp/pf/b.txt\r"); q.stdin.flush()
        time.sleep(1)
        mark = len(out)
        q.stdin.write(b"bwm -n -c bfiles /tmp/pf\r"); q.stdin.flush()
        good = until(b"bfiles: dir /tmp/pf", mark) and until(b"bfiles: window", mark)
        if good:
            time.sleep(1)
            monitor("sendkey down", "sendkey down", "sendkey c")           # a.txt
            good = until(b"bfiles: copy /tmp/pf/a.txt", mark)
            monitor("sendkey up", "sendkey ret")                            # into d
            good = good and until(b"bfiles: dir /tmp/pf/d", mark)
            monitor("sendkey v")
            good = good and until(b"bfiles: copied /tmp/pf/a.txt to /tmp/pf/d/a.txt", mark)
        print(("ok  " if good else "FAIL"), f"({name}) c and v in the bfiles list copy a file into the directory shown")
        fails += not good
        if good:
            monitor("sendkey ret")                                          # ".." goes back up
            time.sleep(0.5)
            monitor("sendkey down", "sendkey down", "sendkey down", "sendkey x")   # b.txt
            good = until(b"bfiles: move /tmp/pf/b.txt", mark)
            monitor("sendkey up", "sendkey up", "sendkey ret", "sendkey v")
            good = good and until(b"bfiles: moved /tmp/pf/b.txt to /tmp/pf/d/b.txt", mark)
        print(("ok  " if good else "FAIL"), f"({name}) x and v in the bfiles list move a file into the directory shown")
        fails += not good
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"cat /tmp/pf/d/a.txt /tmp/pf/d/b.txt; [ -f /tmp/pf/a.txt ] && [ ! -e /tmp/pf/b.txt ] && echo ok$((1+1))\r"); q.stdin.flush()
        good = good and (until(b"pa\r\npb\r\nok2", mark) or until(b"pa\npb\nok2", mark))
        print(("ok  " if good else "FAIL"), f"({name}) the copy is there, the original stays, and the moved file is gone from its old place")
        fails += not good
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # c and v on a directory copy it with what is in it.
        mark = len(out)
        q.stdin.write(b"mkdir -p /tmp/pg/d /tmp/pg/s; echo pc > /tmp/pg/s/c.txt\r"); q.stdin.flush()
        time.sleep(1)
        mark = len(out)
        q.stdin.write(b"bwm -n -c bfiles /tmp/pg\r"); q.stdin.flush()
        good = until(b"bfiles: dir /tmp/pg", mark) and until(b"bfiles: window", mark)
        if good:
            time.sleep(1)
            monitor("sendkey down", "sendkey down", "sendkey c")           # s
            good = until(b"bfiles: copy /tmp/pg/s", mark)
            monitor("sendkey up", "sendkey ret")                            # into d
            good = good and until(b"bfiles: dir /tmp/pg/d", mark)
            monitor("sendkey v")
            good = good and until(b"bfiles: copied /tmp/pg/s to /tmp/pg/d/s", mark)
        print(("ok  " if good else "FAIL"), f"({name}) c and v on a directory in the bfiles list copy it into the directory shown")
        fails += not good
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"cat /tmp/pg/d/s/c.txt; ls /tmp/pg/s\r"); q.stdin.flush()
        good = good and (until(b"pc\r\nc.txt", mark) or until(b"pc\nc.txt", mark))
        print(("ok  " if good else "FAIL"), f"({name}) the copied directory holds the file and the original stays")
        fails += not good
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # n in the list of bfiles makes a new empty file and edits it.
        mark = len(out)
        q.stdin.write(b"mkdir /tmp/nf\r"); q.stdin.flush()
        time.sleep(1)
        mark = len(out)
        q.stdin.write(b"bwm -n -c bfiles /tmp/nf\r"); q.stdin.flush()
        good = until(b"bfiles: dir /tmp/nf", mark) and until(b"bfiles: window", mark)
        if good:
            time.sleep(1)
            monitor("sendkey n")
            good = until(b"bfiles: edit /tmp/nf/untitled", mark)
            monitor("sendkey h", "sendkey i", "sendkey ctrl-s")
            good = good and until(b"bfiles: saved /tmp/nf/untitled", mark)
        print(("ok  " if good else "FAIL"), f"({name}) n in bfiles makes a new file and edits it")
        fails += not good
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"cat /tmp/nf/untitled\r"); q.stdin.flush()
        good = until(b"hi", mark)
        print(("ok  " if good else "FAIL"), f"({name}) the new file holds what was typed")
        fails += not good
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # h in the list of bfiles shows and hides the names that start with a dot.
        mark = len(out)
        q.stdin.write(b"touch /tmp/nf/.dot; bwm -n -c bfiles /tmp/nf\r"); q.stdin.flush()
        good = until(b"bfiles: window", mark)
        if good:
            time.sleep(1)
            monitor("sendkey h")
            good = until(b"bfiles: dotfiles shown", mark)
            monitor("sendkey h")
            good = good and until(b"bfiles: dotfiles hidden", mark)
        print(("ok  " if good else "FAIL"), f"({name}) h in bfiles toggles the dotfiles")
        fails += not good
        # . does what h does.
        if good:
            mark2 = len(out)
            monitor("sendkey dot")
            good = until(b"bfiles: dotfiles shown", mark2)
            mark2 = len(out)
            monitor("sendkey dot")
            good = good and until(b"bfiles: dotfiles hidden", mark2)
        print(("ok  " if good else "FAIL"), f"({name}) . in bfiles toggles the dotfiles too")
        fails += not good
        # s in the list sorts the files by size (largest first), then by time (newest first), then back by name.
        if good:
            monitor("sendkey s")
            good = until(b"bfiles: sorted by size", mark)
            monitor("sendkey s")
            good = good and until(b"bfiles: sorted by time", mark)
            monitor("sendkey s")
            good = good and until(b"bfiles: sorted by name", mark)
        print(("ok  " if good else "FAIL"), f"({name}) s in bfiles cycles the sort order")
        fails += not good
        # R in the list reads the directory again (so a file made elsewhere shows).
        if good:
            monitor("sendkey shift-r")
            good = until(b"bfiles: reloaded", mark)
        print(("ok  " if good else "FAIL"), f"({name}) R in bfiles reloads the list")
        fails += not good
        # F5 does what R does.
        if good:
            mark2 = len(out)
            monitor("sendkey f5")
            good = until(b"bfiles: reloaded", mark2)
        print(("ok  " if good else "FAIL"), f"({name}) F5 in bfiles reloads the list")
        fails += not good
        # G jumps to the end of the list and g back to the top.
        if good:
            mark2 = len(out)
            monitor("sendkey shift-g")
            good = until(b"bfiles: top", mark2)
            mark2 = len(out)
            monitor("sendkey g")
            good = good and until(b"bfiles: top 0", mark2)
        print(("ok  " if good else "FAIL"), f"({name}) g and G in bfiles jump to the top and the end")
        fails += not good
        # The time order puts the file written last first.
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        mark = len(out)
        q.stdin.write(b"mkdir -p /tmp/ts; echo a > /tmp/ts/aa; sleep 2; echo z > /tmp/ts/zz; bwm -n -c bfiles /tmp/ts\r"); q.stdin.flush()
        good = until(b"bfiles: window", mark)
        if good:
            time.sleep(1)
            monitor("sendkey s", "sendkey s")
            good = until(b"bfiles: sorted by time", mark)
        if good:
            time.sleep(1.5)
            w, h, px = screendump()
            lines = screen_text_at(px, w, 72, 90, 60, 24)
            good = lines[2].startswith("zz") and lines[3].startswith("aa")
            if not good:
                print("\n".join(l for l in lines if l))
        print(("ok  " if good else "FAIL"), f"({name}) s twice in bfiles puts the newest file first")
        fails += not good
        # In that order the date of each file stands where the size was.
        if good:
            good = re.search(r"^zz +\d{4}-\d\d-\d\d$", lines[2]) is not None
            if not good:
                print(repr(lines[2]))
        print(("ok  " if good else "FAIL"), f"({name}) bfiles shows the date of each file when sorted by time")
        fails += not good
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # Dragging a file onto a directory in the list moves it there.
        mark = len(out)
        q.stdin.write(b"mkdir -p /tmp/dd/sub; echo x > /tmp/dd/f; bwm -n -c bfiles /tmp/dd\r"); q.stdin.flush()
        good = until(b"bfiles: window", mark)
        if good:
            time.sleep(1)
            mark2 = len(out)
            monitor("mouse_move -200 -200", "mouse_move -200 -200", "mouse_move -200 -200", "mouse_move -200 -200",
                    "mouse_move 150 %d" % (90 + 3 * 16 + 8), "mouse_button 1", "mouse_move 0 -16", "mouse_button 0")
            good = until(b"bfiles: moved /tmp/dd/f to /tmp/dd/sub/f", mark2)
        print(("ok  " if good else "FAIL"), f"({name}) a drag from a file onto a directory in bfiles moves the file")
        fails += not good
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        if good:
            mark = len(out)
            q.stdin.write(b"[ -f /tmp/dd/sub/f ] && [ ! -e /tmp/dd/f ] && echo moved$((1+1))\r"); q.stdin.flush()
            good = until(b"moved2", mark)
            print(("ok  " if good else "FAIL"), f"({name}) the dragged file is in the directory and gone from the old place")
            fails += not good
        # bwm -m asks the display for a smaller mode; the boot mode is back when bwm ends.
        mark = len(out)
        q.stdin.write(b"bwm -n -m 800x600\r"); q.stdin.flush()
        good = until(b"bwm: ready 800x600", mark)
        if good:
            time.sleep(1.5)
            w, h, px = screendump()
            good = (w, h) == (800, 600) and tuple(px[(3 * w + 400) * 3:(3 * w + 400) * 3 + 3]) != (0, 0, 0)
            if not good:
                print("mode seen:", w, h)
        print(("ok  " if good else "FAIL"), f"({name}) bwm -m 800x600 puts the display in that mode")
        fails += not good
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(1.5)
        w, h, px = screendump()
        good = (w, h) == (1024, 768)
        if not good:
            print("mode after:", w, h)
        print(("ok  " if good else "FAIL"), f"({name}) the boot mode is back after bwm ends")
        fails += not good
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # The desktop has an icon for each launcher entry; a click on one starts the program.
        mark = len(out)
        q.stdin.write(b"bwm -n\r"); q.stdin.flush()
        good = until(b"bwm: ready", mark)
        if good:
            time.sleep(1)
            w, h, px = screendump()
            at = (96 * w + 48) * 3                                 # the middle of the Files tile
            good = tuple(px[at:at + 3]) == (230, 180, 40)
            print(("ok  " if good else "FAIL"), f"({name}) the desktop shows an icon for each launcher entry")
            fails += not good
            monitor("mouse_move -464 -288")                        # onto the Files icon
            monitor("mouse_button 1", "mouse_button 0")
            good = until(b"bfiles: window", mark)
            print(("ok  " if good else "FAIL"), f"({name}) a click on a desktop icon starts its program")
            fails += not good
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # A line of /etc/bwm.menu adds a launcher entry that runs its command in a terminal window.
        mark = len(out)
        q.stdin.write(b"printf 'Say=echo menu-ok > /tmp/mm.out\\n' > /etc/bwm.menu; bwm -n\r"); q.stdin.flush()
        good = until(b"bwm: ready", mark)
        if good:
            time.sleep(1)
            monitor("mouse_move 482 372")                          # onto the Menu button
            monitor("mouse_button 1", "mouse_button 0")
            time.sleep(0.7)
            monitor("mouse_move -34 -72")                          # onto Say, the fourth of six entries
            monitor("mouse_button 1", "mouse_button 0")
            good = until(b"bterm: window", mark)
        if good:
            time.sleep(2)
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        mark = len(out)
        q.stdin.write(b"wc -c /tmp/mm.out; rm -f /etc/bwm.menu\r"); q.stdin.flush()
        good = good and until(b"8 /tmp/mm.out", mark)
        print(("ok  " if good else "FAIL"), f"({name}) a line of /etc/bwm.menu adds a launcher entry that runs a command")
        fails += not good
        # bview shows a PPM: a 2 x 2 red image becomes a block of pure red.
        mark = len(out)
        q.stdin.write(b"printf 'P6 2 2 255\\n\\377\\000\\000\\377\\000\\000\\377\\000\\000\\377\\000\\000' > /tmp/nf/t.ppm; bwm -n -c bview /tmp/nf/t.ppm\r"); q.stdin.flush()
        good = until(b"bview: window", mark)
        if good:
            time.sleep(1)
            w, h, px = screendump()
            red = sum(1 for i in range(0, len(px) - 2, 3) if tuple(px[i:i + 3]) == (255, 0, 0))
            good = red > 1000
        print(("ok  " if good else "FAIL"), f"({name}) bview shows an image")
        fails += not good
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        # + doubles the scale about the middle: a red middle pixel of 3 x 3 is 120 x 120 cells, then 240 x 240.
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        mark = len(out)
        pix = "".join("\\000\\000\\377" if i != 4 else "\\377\\000\\000" for i in range(9))
        q.stdin.write(f"printf 'P6 3 3 255\\n{pix}' > /tmp/nf/z.ppm; bwm -n -c bview /tmp/nf/z.ppm\r".encode()); q.stdin.flush()
        good = until(b"bview: window", mark)
        r0 = r1 = 0
        if good:
            time.sleep(1.5)
            w, h, px = screendump()
            r0 = sum(1 for i in range(0, len(px) - 2, 3) if tuple(px[i:i + 3]) == (255, 0, 0))
            monitor("sendkey equal")
            time.sleep(2)
            w, h, px = screendump()
            r1 = sum(1 for i in range(0, len(px) - 2, 3) if tuple(px[i:i + 3]) == (255, 0, 0))
            good = 10000 < r0 < 20000 and r1 > 3 * r0
        print(("ok  " if good else "FAIL"), f"({name}) + zooms the image in bview ({r0} then {r1} pixels)")
        fails += not good
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # The mouse wheel zooms the image in bview: up in, down out.
        time.sleep(4)
        mark = len(out)
        q.stdin.write(b"bwm -n -c bview /tmp/nf/z.ppm\r"); q.stdin.flush()
        good = until(b"bview: window", mark)
        r0 = r1 = r2 = 0
        if good:
            time.sleep(1.5)
            monitor("mouse_move -200 -200", "mouse_move -200 -200", "mouse_move -200 -200", "mouse_move -200 -200",
                    "mouse_move 140 150")
            time.sleep(0.5)
            def red():
                w, h, px = screendump()
                return sum(1 for i in range(0, len(px) - 2, 3) if tuple(px[i:i + 3]) == (255, 0, 0))
            r0 = red()
            monitor("mouse_move 0 0 1")
            time.sleep(2)
            r1 = red()
            monitor("mouse_move 0 0 -1", "mouse_move 0 0 -1")
            time.sleep(2)
            r2 = red()
            good = r0 > 10000 and r1 > 3 * r0 and r2 < r0
        print(("ok  " if good else "FAIL"), f"({name}) the wheel zooms the image in bview ({r0}, {r1}, {r2} pixels)")
        fails += not good
        # Home puts the zoom back to the first size.
        if good:
            monitor("mouse_move 0 0 1")
            time.sleep(1.5)
            monitor("sendkey home")
            time.sleep(2)
            r3 = red()
            good = abs(r3 - r0) < r0 // 20
        else:
            r3 = 0
        print(("ok  " if good else "FAIL"), f"({name}) Home resets the zoom in bview ({r3} against {r0} pixels)")
        fails += not good
        # End zooms in all the way.
        if good:
            monitor("sendkey end")
            time.sleep(2)
            r4 = red()
            good = r4 > r0
        else:
            r4 = 0
        print(("ok  " if good else "FAIL"), f"({name}) End zooms in all the way in bview ({r4} against {r0} pixels)")
        fails += not good
        # Page Up zooms in and Page Down out.
        if good:
            monitor("sendkey home")
            time.sleep(2)
            monitor("sendkey pgup")
            time.sleep(2)
            r5 = red()
            monitor("sendkey pgdn")
            time.sleep(2)
            r6 = red()
            good = r5 > r0 and abs(r6 - r0) < r0 // 20
        else:
            r5 = r6 = 0
        print(("ok  " if good else "FAIL"), f"({name}) Page Up and Page Down zoom in and out in bview ({r5}, {r6} against {r0} pixels)")
        fails += not good
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # A click on a .ppm in the bfiles list opens it in the image viewer.
        mark = len(out)
        q.stdin.write(b"mkdir /tmp/ni; cp /tmp/nf/t.ppm /tmp/ni/p.ppm; bwm -n -c bfiles /tmp/ni\r"); q.stdin.flush()
        good = until(b"bfiles: window", mark)
        if good:
            time.sleep(1)
            monitor("mouse_move -420 -254", "mouse_move -1 -1", "mouse_button 1", "mouse_button 0")
            good = until(b"bfiles: image /tmp/ni/p.ppm", mark) and until(b"bview: window", mark)
        print(("ok  " if good else "FAIL"), f"({name}) a click on a .ppm in bfiles opens the image viewer")
        fails += not good
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # The arrow keys move a cursor in the bfiles list and Enter opens the entry under it.
        mark = len(out)
        q.stdin.write(b"bwm -n -c bfiles /tmp/ni\r"); q.stdin.flush()
        good = until(b"bfiles: window", mark)
        if good:
            time.sleep(1)
            monitor("sendkey down")
            good = until(b"bfiles: cursor 1", mark)
        if good:
            monitor("sendkey home")
            good = until(b"bfiles: cursor 0", mark)
            mark2 = len(out)
            monitor("sendkey end")
            good = good and until(b"bfiles: cursor 1", mark2)
        if good:
            monitor("sendkey ret")
            good = until(b"bfiles: image /tmp/ni/p.ppm", mark) and until(b"bview: window", mark)
        print(("ok  " if good else "FAIL"), f"({name}) the arrow, Home and End keys and Enter work on the bfiles list")
        fails += not good
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # Delete twice in the bfiles list deletes the entry under the cursor, as D does.
        mark = len(out)
        q.stdin.write(b"mkdir -p /tmp/dk; echo x > /tmp/dk/a.txt; bwm -n -c bfiles /tmp/dk\r"); q.stdin.flush()
        good = until(b"bfiles: window", mark)
        if good:
            time.sleep(1)
            monitor("sendkey down", "sendkey delete")
            time.sleep(0.5)
            monitor("sendkey delete")
            good = until(b"bfiles: deleted /tmp/dk/a.txt", mark)
        print(("ok  " if good else "FAIL"), f"({name}) Delete twice in the bfiles list deletes the entry under the cursor")
        fails += not good
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(4)
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # e in the bfiles list edits the file under the cursor without opening the viewer first.
        mark = len(out)
        q.stdin.write(b"mkdir -p /tmp/el; echo hi > /tmp/el/a.txt; bwm -n -c bfiles /tmp/el\r"); q.stdin.flush()
        good = until(b"bfiles: window", mark)
        if good:
            time.sleep(1)
            monitor("sendkey down", "sendkey e")
            good = until(b"bfiles: edit /tmp/el/a.txt", mark)
        print(("ok  " if good else "FAIL"), f"({name}) e in the bfiles list edits the file under the cursor")
        fails += not good
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(4)
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # The title row of bfiles says when the list is not sorted by name.
        mark = len(out)
        q.stdin.write(b"mkdir -p /tmp/ff; bwm -n -c bfiles /tmp/ff\r"); q.stdin.flush()
        good = until(b"bfiles: window", mark)
        t0 = t1 = 0
        if good:
            time.sleep(1.5)
            def white():
                w, h, px = screendump()
                return sum(1 for y in range(90, 106) for x in range(72, 72 + 480) if tuple(px[(y * w + x) * 3:(y * w + x) * 3 + 3]) == (255, 255, 255))
            t0 = white()
            mark2 = len(out)
            monitor("sendkey s")
            good = until(b"bfiles: sorted by size", mark2)
            time.sleep(1)
            t1 = white()
            good = good and t0 > 20 and t1 > t0 + 40
        print(("ok  " if good else "FAIL"), f"({name}) the bfiles title row names a sort order ({t0} then {t1} pixels)")
        fails += not good
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(4)
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # - in the bfiles list goes back to the directory shown before.
        mark = len(out)
        q.stdin.write(b"mkdir -p /tmp/ff; HOME=/tmp bwm -n -c bfiles /tmp/ff\r"); q.stdin.flush()
        good = until(b"bfiles: window", mark)
        if good:
            time.sleep(1)
            mark2 = len(out)
            monitor("sendkey minus")
            good = until(b"bfiles: no earlier directory", mark2)
        if good:
            mark2 = len(out)
            monitor("sendkey shift-grave_accent")
            good = until(b"bfiles: dir /tmp\r\n", mark2)
        if good:
            mark2 = len(out)
            monitor("sendkey minus")
            good = until(b"bfiles: dir /tmp/ff\r\n", mark2)
        print(("ok  " if good else "FAIL"), f"({name}) - in the bfiles list goes back to the earlier directory")
        fails += not good
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # ~ in the bfiles list goes to the home directory.
        mark = len(out)
        q.stdin.write(b"HOME=/tmp bwm -n -c bfiles /tmp/ff\r"); q.stdin.flush()
        good = until(b"bfiles: window", mark)
        if good:
            time.sleep(1)
            mark2 = len(out)
            monitor("sendkey shift-grave_accent")
            good = until(b"bfiles: dir /tmp\r\n", mark2)
        print(("ok  " if good else "FAIL"), f"({name}) ~ in the bfiles list goes to the home directory")
        fails += not good
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # / and the start of a name moves the cursor of the bfiles list to the first entry that begins with it.
        mark = len(out)
        q.stdin.write(b"mkdir -p /tmp/ff; echo a > /tmp/ff/alpha; echo b > /tmp/ff/beta; echo g > /tmp/ff/gamma; bwm -n -c bfiles /tmp/ff\r"); q.stdin.flush()
        good = until(b"bfiles: window", mark)
        if good:
            time.sleep(1)
            monitor("sendkey slash", "sendkey g")
            good = until(b"bfiles: cursor 3", mark)
        if good:
            mark2 = len(out)
            monitor("sendkey backspace", "sendkey b")
            good = until(b"bfiles: cursor 2", mark2)
        if good:
            mark2 = len(out)
            monitor("sendkey z")
            good = until(b"bfiles: no name starts with bz", mark2)
        print(("ok  " if good else "FAIL"), f"({name}) / and the start of a name moves the bfiles cursor")
        fails += not good
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # t in the list of bfiles opens a terminal in that directory.
        mark = len(out)
        q.stdin.write(b"bwm -n -c bfiles /tmp/nf\r"); q.stdin.flush()
        good = until(b"bfiles: window", mark)
        if good:
            time.sleep(1)
            monitor("sendkey t")
            good = until(b"bfiles: terminal in /tmp/nf", mark) and until(b"bterm: window", mark)
        if good:
            time.sleep(2)
            for ch in "pwd > /tmp/nf/pwd.out":
                monitor("sendkey " + {" ": "spc", ">": "shift-dot", "/": "slash", ".": "dot"}.get(ch, ch))
            monitor("sendkey ret")
            time.sleep(1.5)
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"cat /tmp/nf/pwd.out\r"); q.stdin.flush()
        good = good and until(b"\n/tmp/nf", mark)
        print(("ok  " if good else "FAIL"), f"({name}) t in bfiles opens a terminal in the directory")
        fails += not good
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # N in the list of bfiles makes a new directory.
        mark = len(out)
        q.stdin.write(b"bwm -n -c bfiles /tmp/nf\r"); q.stdin.flush()
        good = until(b"bfiles: window", mark)
        if good:
            time.sleep(1)
            monitor("sendkey shift-n")
            good = until(b"bfiles: mkdir /tmp/nf/newdir", mark)
        print(("ok  " if good else "FAIL"), f"({name}) N in bfiles makes a new directory")
        fails += not good
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"[ -d /tmp/nf/newdir ] && echo i$((1+1))s\r"); q.stdin.flush()
        good = until(b"i2s", mark)
        print(("ok  " if good else "FAIL"), f"({name}) the new directory exists")
        fails += not good
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # r in the viewer of bfiles renames the file shown.
        mark = len(out)
        q.stdin.write(b"echo x > /tmp/nf/ab; bwm -n -c bfiles /tmp/nf/ab\r"); q.stdin.flush()
        good = until(b"bfiles: window", mark)
        if good:
            time.sleep(1)
            monitor("sendkey r", "sendkey backspace", "sendkey backspace", "sendkey c", "sendkey d", "sendkey ret")
            good = until(b"bfiles: renamed /tmp/nf/ab to /tmp/nf/cd", mark)
        print(("ok  " if good else "FAIL"), f"({name}) r in the bfiles viewer renames the file")
        fails += not good
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"[ -f /tmp/nf/cd ] && [ ! -e /tmp/nf/ab ] && echo i$((1+1))s\r"); q.stdin.flush()
        good = until(b"i2s", mark)
        print(("ok  " if good else "FAIL"), f"({name}) the renamed file is under its new name")
        fails += not good
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # c in the viewer of bfiles copies the file shown.
        mark = len(out)
        q.stdin.write(b"echo xyz > /tmp/nf/cp; bwm -n -c bfiles /tmp/nf/cp\r"); q.stdin.flush()
        good = until(b"bfiles: window", mark)
        if good:
            time.sleep(1)
            monitor("sendkey c")
            good = until(b"bfiles: copied /tmp/nf/cp to /tmp/nf/cp.copy", mark)
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"cat /tmp/nf/cp.copy\r"); q.stdin.flush()
        good = good and until(b"xyz", mark)
        print(("ok  " if good else "FAIL"), f"({name}) c in the bfiles viewer copies the file")
        fails += not good
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
        # D twice in the viewer of bfiles deletes the file it shows.
        mark = len(out)
        q.stdin.write(b"bwm -n -c bfiles /tmp/nf/untitled\r"); q.stdin.flush()
        good = until(b"bfiles: window", mark)
        if good:
            time.sleep(1)
            monitor("sendkey shift-d")
            time.sleep(0.5)
            monitor("sendkey shift-d")
            good = until(b"bfiles: deleted /tmp/nf/untitled", mark)
        print(("ok  " if good else "FAIL"), f"({name}) D twice in the bfiles viewer deletes the file")
        fails += not good
        monitor("sendkey ctrl-q")
        until(b"bwm: bye", mark)
        time.sleep(0.5)
        mark = len(out)
        q.stdin.write(b"ls /tmp/nf; echo d$((1+1))\r"); q.stdin.flush()
        until(b"d2", mark)
        good = b"untitled" not in bytes(out[mark:]).replace(b"ls /tmp/nf", b"")
        print(("ok  " if good else "FAIL"), f"({name}) the deleted file is gone")
        fails += not good
        mark = len(out)
        q.stdin.write(b"\r"); q.stdin.flush()
        until(b"# ", mark)
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
            # The alternate screen (what vi uses): alt is drawn over a blank
            # screen, and leaving it brings the shell's screen back.
            keys = {"-": "minus", " ": "spc", "'": "apostrophe", "\\": "backslash", "[": "bracket_left", "?": "shift-slash"}
            def typed(text):
                monitor(*[f"sendkey {keys.get(c, 'shift-' + c.lower() if c.isupper() else c)}" for c in text], "sendkey ret")
            # A program can set the window title (ESC ] 2 ; TEXT BEL), which bwm shows.
            keys[";"] = "semicolon"
            mark2 = len(out)
            typed("printf '\\033\\1352;Mine\\007'")
            good = until(b" Mine", mark2)
            print(("ok  " if good else "FAIL"), f"({name}) a program sets the title of its terminal window")
            fails += not good
            typed("printf '\\033[?1049halt\\n'")
            time.sleep(1.5)
            w, h, px = screendump()
            lines = screen_text_at(px, w, 72, 90, 80, 24)
            good = any(l.startswith("alt") for l in lines) and not any("echo term" in l for l in lines)
            print(("ok  " if good else "FAIL"), f"({name}) the terminal's alternate screen replaces the shell's screen")
            if not good:
                print("\n".join(l for l in lines if l))
            fails += not good
            typed("printf '\\033[?1049l'")
            time.sleep(1.5)
            w, h, px = screendump()
            lines = screen_text_at(px, w, 72, 90, 80, 24)
            good = any(l.strip() == "term" for l in lines) and not any(l.startswith("alt") for l in lines)
            print(("ok  " if good else "FAIL"), f"({name}) leaving the alternate screen brings the shell's screen back")
            if not good:
                print("\n".join(l for l in lines if l))
            fails += not good
            # UTF-8: box drawing takes one cell and Japanese two, both drawn from the console's font.
            typed("printf 'u\\342\\224\\200\\343\\201\\202v\\n'")
            time.sleep(1.5)
            w, h, px = screendump()
            lines = screen_text_at(px, w, 72, 90, 80, 24)
            good = any(l.strip() == "u\u2500\u3042v" for l in lines)
            print(("ok  " if good else "FAIL"), f"({name}) the terminal draws UTF-8 characters from the font, wide ones in two cells")
            if not good:
                print("\n".join(l for l in lines if l))
            fails += not good
            # Deleting the right half of a wide character leaves no half of it behind: "x", a hiragana, "y", then the cursor
            # on the hiragana's second cell and one character deleted gives "x" and "y" with a blank between.
            typed("printf 'x\\343\\201\\202y\\r\\033[2\\103\\033[1\\120\\n'")
            time.sleep(1.5)
            w, h, px = screendump()
            lines = screen_text_at(px, w, 72, 90, 80, 24)
            good = any(l.strip() == "x y" for l in lines)
            print(("ok  " if good else "FAIL"), f"({name}) deleting half of a wide character in the terminal blanks the other half")
            if not good:
                print("\n".join(l for l in lines if l))
            fails += not good
            # Alt-Space turns romaji into hiragana: "konnnichiha" is the greeting and "kitte" has a small tsu in it.
            monitor(*[f"sendkey {c}" for c in "echo"], "sendkey spc", "sendkey alt-spc")
            typed("konnnichihakitte")
            time.sleep(1.5)
            w, h, px = screendump()
            lines = screen_text_at(px, w, 72, 90, 80, 24)
            good = any(l.strip() == "\u3053\u3093\u306b\u3061\u306f\u304d\u3063\u3066" for l in lines)
            print(("ok  " if good else "FAIL"), f"({name}) Alt-Space types romaji as hiragana")
            if not good:
                print("\n".join(l for l in lines if l))
            fails += not good
            # The letters typed that make no kana yet show in the task bar next to the mark; Backspace takes them back.
            def bar_white():
                w, h, px = screendump()
                return sum(1 for y in range(744, 768) for x in range(836, 912) if tuple(px[(y * w + x) * 3:(y * w + x) * 3 + 3]) == (255, 255, 255))
            time.sleep(1)
            n0 = bar_white()
            monitor("sendkey k")
            time.sleep(1.5)
            n1 = bar_white()
            monitor("sendkey backspace")
            time.sleep(1.5)
            n2 = bar_white()
            good = n1 > n0 and n2 == n0
            print(("ok  " if good else "FAIL"), f"({name}) the task bar shows the romaji letters waiting for a kana ({n0}, {n1}, {n2})")
            fails += not good
            # A second Alt-Space gives katakana, a third turns the mode off.
            monitor("sendkey alt-spc", "sendkey alt-spc", *[f"sendkey {c}" for c in "echo"], "sendkey spc", "sendkey alt-spc", "sendkey alt-spc")
            typed("kitte")
            monitor("sendkey alt-spc")
            time.sleep(1.5)
            w, h, px = screendump()
            lines = screen_text_at(px, w, 72, 90, 80, 24)
            good = any(l.strip() == "\u30ad\u30c3\u30c6" for l in lines)
            print(("ok  " if good else "FAIL"), f"({name}) a second Alt-Space types katakana")
            if not good:
                print("\n".join(l for l in lines if l))
            fails += not good
            # Alt-Shift-Space turns on kanji conversion: the kana wait in the task bar, Space lists the dictionary's
            # candidates, Enter puts the one shown in the window. "nihon" is 日本; "hi" and Space twice is the second, 火.
            monitor("sendkey ctrl-u", *[f"sendkey {c}" for c in "echo"], "sendkey spc", "sendkey alt-shift-spc")
            monitor(*[f"sendkey {c}" for c in "nihon"])
            monitor("sendkey spc")
            time.sleep(1)
            monitor("sendkey ret")
            time.sleep(1.5)
            w, h, px = screendump()
            lines = screen_text_at(px, w, 72, 90, 80, 24)
            good = any("echo \u65e5\u672c" in l for l in lines)
            print(("ok  " if good else "FAIL"), f"({name}) kanji conversion turns nihon into the dictionary's word")
            if not good:
                print("\n".join(l for l in lines if l))
            fails += not good
            monitor("sendkey ctrl-u", "sendkey alt-spc", *[f"sendkey {c}" for c in "echo"], "sendkey spc", "sendkey alt-shift-spc")
            monitor("sendkey h", "sendkey i")
            monitor("sendkey spc", "sendkey spc")
            time.sleep(1)
            monitor("sendkey ret")
            time.sleep(1.5)
            w, h, px = screendump()
            lines = screen_text_at(px, w, 72, 90, 80, 24)
            good = any("echo \u706b" in l for l in lines)
            print(("ok  " if good else "FAIL"), f"({name}) a second Space shows the next candidate")
            if not good:
                print("\n".join(l for l in lines if l))
            fails += not good
            # A word from the dictionary's later entries: "settei" is 設定.
            monitor("sendkey ctrl-u", "sendkey alt-spc", *[f"sendkey {c}" for c in "echo"], "sendkey spc", "sendkey alt-shift-spc")
            monitor(*[f"sendkey {c}" for c in "settei"])
            monitor("sendkey spc")
            time.sleep(1)
            monitor("sendkey ret")
            time.sleep(1.5)
            w, h, px = screendump()
            lines = screen_text_at(px, w, 72, 90, 80, 24)
            good = any("echo \u8a2d\u5b9a" in l for l in lines)
            print(("ok  " if good else "FAIL"), f"({name}) kanji conversion knows a longer word")
            if not good:
                print("\n".join(l for l in lines if l))
            fails += not good
            monitor("sendkey ctrl-u", "sendkey alt-spc")
            # 256 colours (38;5;N and 48;5;N) and 24-bit colours (48;2;R;G;B, drawn as the nearest of them).
            keys[";"] = "semicolon"
            typed("printf '\\033[48;5;196m      \\033[48;5;21m      \\033[48;2;0;255;0m      \\033[0m'")
            time.sleep(1.5)
            w, h, px = screendump()
            cnt = {c: 0 for c in ((255, 0, 0), (0, 0, 255), (0, 255, 0))}
            for i in range(0, len(px) - 2, 3):
                t = tuple(px[i:i + 3])
                if t in cnt:
                    cnt[t] += 1
            good = all(v > 300 for v in cnt.values())
            print(("ok  " if good else "FAIL"), f"({name}) the terminal draws 256 and 24-bit colours")
            if not good:
                print(cnt)
            fails += not good
            # SGR 4 underlines the cells that follow (the two bottom pixel rows, in the foreground colour).
            typed("printf '\\033[2J\\033[H\\033[38;5;196;4m      \\033[0m'")
            time.sleep(1.5)
            w, h, px = screendump()
            red = sum(1 for i in range(0, len(px) - 2, 3) if tuple(px[i:i + 3]) == (255, 0, 0))
            good = 60 <= red <= 200
            print(("ok  " if good else "FAIL"), f"({name}) the terminal underlines text")
            if not good:
                print(red)
            fails += not good
            # SGR 9 strikes text through (two pixel rows); SGR 3 leans it: the red glyphs get wider.
            typed("printf '\\033[2J\\033[H\\033[38;5;196;9m      \\033[0m'")
            time.sleep(1.5)
            w, h, px = screendump()
            red = sum(1 for i in range(0, len(px) - 2, 3) if tuple(px[i:i + 3]) == (255, 0, 0))
            good = 60 <= red <= 200
            print(("ok  " if good else "FAIL"), f"({name}) the terminal strikes text through")
            fails += not good
            def red_width():
                w, h, px = screendump()
                xs = [(i // 3) % w for i in range(0, len(px) - 2, 3) if tuple(px[i:i + 3]) == (255, 0, 0)]
                return (max(xs) - min(xs) + 1) if xs else 0
            typed("printf '\\033[2J\\033[H\\033[38;5;196mllllll\\033[0m'")
            time.sleep(1.5)
            plain = red_width()
            typed("printf '\\033[2J\\033[H\\033[38;5;196;3mllllll\\033[0m'")
            time.sleep(1.5)
            lean = red_width()
            good = plain > 0 and lean > plain
            print(("ok  " if good else "FAIL"), f"({name}) the terminal leans italic text")
            if not good:
                print(plain, lean)
            fails += not good
            # Mouse reporting (xterm 1000 and 1006): a click at the pointer (cell 55, 18) reaches the program as
            # ESC [ < 0 ; 56 ; 19 M, 11 bytes; cat reads them with the Enter after them (12 bytes in all).
            keys[">"] = "shift-dot"; keys["/"] = "slash"; keys["."] = "dot"
            typed("printf '\\033[2J\\033[H\\033[?1000h\\033[?1006h'; cat > /tmp/ms.out; printf '\\033[?1000l\\033[?1006l'; wc -c /tmp/ms.out")
            time.sleep(1.5)
            monitor("mouse_button 1")
            time.sleep(1)
            monitor("sendkey ret")
            time.sleep(0.5)
            monitor("sendkey ctrl-d")
            time.sleep(1)
            monitor("mouse_button 0")
            time.sleep(1)
            w, h, px = screendump()
            lines = screen_text_at(px, w, 72, 90, 80, 24)
            good = any(l.strip().startswith("12 /tmp/ms.out") for l in lines)
            print(("ok  " if good else "FAIL"), f"({name}) the terminal reports a click to a program that asked for the mouse")
            if not good:
                print("\n".join(l for l in lines if l))
            fails += not good
            # A key held down repeats (bwm does it, the events have none).
            monitor("sendkey z 2500")
            time.sleep(1.2)
            w, h, px = screendump()
            lines = screen_text_at(px, w, 72, 90, 80, 24)
            good = any("zzzzzzzzzz" in l for l in lines)
            print(("ok  " if good else "FAIL"), f"({name}) a key held down repeats in the terminal window")
            if not good:
                print("\n".join(l for l in lines if l))
            fails += not good
            time.sleep(2)                                   # let go of the key before the next one
            monitor("sendkey ctrl-u")
            # Function keys give the sequences a VT100 program expects:
            # F1 (ESC O P) and F5 (ESC [ 1 5 ~) and the line's end are nine
            # bytes, which wc -c counts.
            monitor(*[f"sendkey {c}" for c in "wc"], "sendkey spc", "sendkey minus", "sendkey c")
            time.sleep(0.5)
            monitor("sendkey ret", "sendkey f1", "sendkey f5", "sendkey ret", "sendkey ctrl-d")
            time.sleep(1.5)
            w, h, px = screendump()
            lines = screen_text_at(px, w, 72, 90, 80, 24)
            good = any(l.strip() == "9" for l in lines)
            print(("ok  " if good else "FAIL"), f"({name}) function keys reach the terminal as escape sequences")
            if not good:
                print("\n".join(l for l in lines if l))
            fails += not good
            # The grip makes the terminal 60 x 20 cells: the window shrinks
            # (the wallpaper shows where it was), the shell gets SIGWINCH and still answers.
            typed("trap 'echo winched' WINCH")
            time.sleep(1)
            monitor("mouse_move 196 86", "mouse_button 1", "mouse_move -160 -64", "mouse_button 0")
            time.sleep(1.5)
            typed("echo resized")
            time.sleep(1.5)
            w, h, px = screendump()
            lines = screen_text_at(px, w, 72, 90, 60, 20)
            pxl = lambda x, y: tuple(px[(y * w + x) * 3:(y * w + x) * 3 + 3])
            good = (any(l.strip() == "resized" for l in lines) and any(l.strip() == "winched" for l in lines)
                    and pxl(600, 300) == pxl(10, 300))
            print(("ok  " if good else "FAIL"), f"({name}) dragging the grip of the terminal window resizes its grid")
            if not good:
                print("\n".join(l for l in lines if l))
            fails += not good
            # The wheel scrolls back through what has gone off the top (and
            # forward again), three lines a step.
            typed("printf '" + "".join(f"l{i:02d}\\n" for i in range(1, 31)) + "'")
            time.sleep(2)
            monitor("mouse_move -200 -200", "mouse_move -200 -200", "mouse_move -200 -200", "mouse_move -200 -200",
                    "mouse_move 200 150", "mouse_move 100 100", "mouse_move 0 0 1")
            time.sleep(1.5)
            w, h, px = screendump()
            lines = screen_text_at(px, w, 72, 90, 60, 20)
            good = any(l.strip() == "l09" for l in lines) and not any(l.strip() == "l30" for l in lines)
            print(("ok  " if good else "FAIL"), f"({name}) the mouse wheel scrolls the terminal back through its history")
            if not good:
                print("\n".join(l for l in lines if l))
            fails += not good
            monitor("mouse_move 0 0 -1")
            time.sleep(1.5)
            w, h, px = screendump()
            lines = screen_text_at(px, w, 72, 90, 60, 20)
            good = any(l.strip() == "l30" for l in lines)
            print(("ok  " if good else "FAIL"), f"({name}) the wheel scrolls forward again")
            if not good:
                print("\n".join(l for l in lines if l))
            fails += not good
            # Shift-Page Up and Shift-Page Down page through the history.
            monitor("sendkey shift-pgup")
            time.sleep(1.5)
            w, h, px = screendump()
            lines = screen_text_at(px, w, 72, 90, 60, 20)
            good = not any(l.strip() == "l30" for l in lines) and any(l.strip().startswith("l") and len(l.strip()) == 3 for l in lines)
            print(("ok  " if good else "FAIL"), f"({name}) Shift-Page Up scrolls the terminal back a page")
            if not good:
                print("\n".join(l for l in lines if l))
            fails += not good
            monitor("sendkey shift-pgdn")
            time.sleep(1.5)
            w, h, px = screendump()
            lines = screen_text_at(px, w, 72, 90, 60, 20)
            good = any(l.strip() == "l30" for l in lines)
            print(("ok  " if good else "FAIL"), f"({name}) Shift-Page Down scrolls forward a page")
            if not good:
                print("\n".join(l for l in lines if l))
            fails += not good
            monitor("sendkey ctrl-q")
            until(b"bwm: bye", mark)
            time.sleep(0.5)
            # Copy and paste, in a fresh bwm (the pointer starts at the centre):
            # dragging over text selects and copies it, the middle button types
            # the copy into the terminal.
            mark = len(out)
            q.stdin.write(b"\r"); q.stdin.flush()
            until(b"# ", mark)
            mark = len(out)
            q.stdin.write(b"bwm -n -c bterm\r"); q.stdin.flush()
            good = until(b"bterm: window", mark)
            if good:
                time.sleep(2)
                typed("echo copied")
                time.sleep(1.5)
                w, h, px = screendump()
                lines = screen_text_at(px, w, 72, 90, 80, 24)
                row = next((i for i, l in enumerate(lines) if l.strip() == "copied"), None)
                good = row is not None
            if good:
                def movep(dx, dy):
                    n = max(1, (max(abs(dx), abs(dy)) + 199) // 200)
                    monitor(*[f"mouse_move {dx * (i + 1) // n - dx * i // n} {dy * (i + 1) // n - dy * i // n}" for i in range(n)])
                ty = 90 + row * 16 + 8
                movep(74 - 512, ty - 384)
                monitor("mouse_button 1")
                movep(126 - 74, 0)
                monitor("mouse_button 0")
                time.sleep(0.7)
                monitor("mouse_button 4", "mouse_button 0", "mouse_move 200 200")   # and the pointer off the text
                time.sleep(1)
                w, h, px = screendump()
                lines2 = screen_text_at(px, w, 72, 90, 80, 24)
                good = any(l.strip() == "# copied" for l in lines2)
                if not good:
                    print("\n".join(l for l in lines2 if l))
            print(("ok  " if good else "FAIL"), f"({name}) a selection is copied and the middle button pastes it")
            fails += not good
            if good:
                # Shift-Insert pastes the copy from the keyboard.
                monitor("sendkey shift-insert")
                time.sleep(1)
                w, h, px = screendump()
                lines3 = screen_text_at(px, w, 72, 90, 80, 24)
                good = any(l.strip() == "# copiedcopied" for l in lines3)
                print(("ok  " if good else "FAIL"), f"({name}) Shift-Insert pastes the copy into the terminal")
                if not good:
                    print("\n".join(l for l in lines3 if l))
                fails += not good
            if good:
                # A double click selects the word under the pointer and copies it.
                erow = next((i for i, l in enumerate(lines3) if l.strip() == "# echo copied"), None)
                good = erow is not None
            if good:
                # the pointer is at (326, ty + 200) after the paste
                movep(100 - 326, (90 + erow * 16 + 8) - (90 + row * 16 + 8 + 200))
                monitor("mouse_button 1", "mouse_button 0", "mouse_button 1", "mouse_button 0")
                time.sleep(0.7)
                monitor("mouse_button 4", "mouse_button 0", "mouse_move 200 200")   # the middle button types the copy
                time.sleep(1)
                w, h, px = screendump()
                lines4 = screen_text_at(px, w, 72, 90, 80, 24)
                good = any(l.strip() == "# copiedcopiedecho" for l in lines4)
                print(("ok  " if good else "FAIL"), f"({name}) a double click selects a word and copies it")
                if not good:
                    print("\n".join(l for l in lines4 if l))
                fails += not good
            bracket = good
            if good:
                # A program that asked for bracketed paste (mode 2004) gets the paste between 200~ and 201~.
                monitor("sendkey ctrl-u")
                typed("printf '\\033[?2004h'; cat > /tmp/bp.txt")
                time.sleep(1)
                monitor("sendkey shift-insert", "sendkey ret", "sendkey ctrl-d")
                time.sleep(1)
            monitor("sendkey ctrl-q")
            until(b"bwm: bye", mark)
            time.sleep(0.5)
            if bracket:
                mark = len(out)
                q.stdin.write(b"\r"); q.stdin.flush()
                until(b"# ", mark)
                mark = len(out)
                q.stdin.write(b"cat /tmp/bp.txt; echo end$((1+1))\r"); q.stdin.flush()
                good = until(b"\x1b[200~echo\x1b[201~", mark)
                print(("ok  " if good else "FAIL"), f"({name}) a paste is bracketed after the program asked for mode 2004")
                fails += not good
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

# ISO_PART names the runs to make (a, b, c; any mix, default all) so that
# CI can give them runners of their own. Each run starts from a disk of its
# own except "UEFI, USB keyboard, NVMe", which reads what "UEFI" wrote
# through virtio: they stay in one part.
parts = os.environ.get("ISO_PART", "abc")
def part(x): return x in parts
stick = None
if part("a") or part("c"):
    # The image `make usb` builds: the ISO, then a BastiOS partition. The
    # keyboard is on the same controller, so it is read through the disk.
    subprocess.run(["make", "-s", "build/bastios-usb.img", "USB_MIB=33"], check=True)
    stick = os.path.join(tmp.name, "stick.img")
    shutil.copy("build/bastios-usb.img", stick)
if part("a"):
    run("BIOS", [])
    run("BIOS, from a USB stick, USB keyboard", [], usb=True, stick=stick)
if not (part("b") or part("c")):
    pass
elif os.path.exists(ovmf) and os.path.exists(ovmf_vars):
    vars_copy = os.path.join(tmp.name, "vars.fd")
    if part("b"):
        shutil.copy(ovmf_vars, vars_copy)
        run("UEFI", ["-drive", f"if=pflash,format=raw,readonly=on,file={ovmf}",
                     "-drive", f"if=pflash,format=raw,file={vars_copy}"])
        # The firmware drives the xHCI controller itself until it hands over.
        # OVMF puts both controllers' 64-bit BARs far above 4 GiB, in the upper
        # part of the device window.
        shutil.copy(ovmf_vars, vars_copy)
        run("UEFI, USB keyboard, NVMe", ["-drive", f"if=pflash,format=raw,readonly=on,file={ovmf}",
                                         "-drive", f"if=pflash,format=raw,file={vars_copy}"], usb=True, nvme=True)
    if part("c"):
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
