#!/usr/bin/env python3
"""Installs BastiOS from build/bastios-installer.iso (make installer) onto a
blank disk in QEMU, then boots the disk alone, and checks the new system.

    install_test.py [bios|uefi] [virtio|nvme|ahci] [KEEP_DISK.img]

The installer is driven over the serial line as a person would drive it:
the Limine menu starts it by default, it asks for `erase`, and at the end
for Enter, which restarts the machine (QEMU exits: -no-reboot). The new
disk is then checked on the host (a GPT with the three partitions, BastiOS's
type on the last, and an EFI system partition holding Limine and the kernel,
which are the files on the installer's payload) and booted by itself, BIOS or
UEFI as given, with no CD: Limine's menu, the kernel finding its partition,
a file the installer wrote, and Alpine Linux's shell through /disk/alpine.

Skips (exit 0) when QEMU, xorriso or OVMF are missing.
"""
import os, re, select, shutil, struct, subprocess, sys, tempfile, time, uuid

OVMF = os.environ.get("OVMF", "/usr/share/OVMF/OVMF_CODE_4M.fd")
OVMF_VARS = os.environ.get("OVMF_VARS", "/usr/share/OVMF/OVMF_VARS_4M.fd")
ISO = os.environ.get("INSTALLER_ISO", "build/bastios-installer.iso")
DISK_MIB = 1024
LOG = open(os.environ["INSTALL_LOG"], "ab") if os.environ.get("INSTALL_LOG") else None

args = sys.argv[1:]
firmware = args.pop(0) if args and args[0] in ("bios", "uefi") else "bios"
bus = args.pop(0) if args and args[0] in ("virtio", "nvme", "ahci") else "virtio"
keep = args.pop(0) if args else None

if not shutil.which("qemu-system-x86_64"):
    print("SKIP install: qemu-system-x86_64 missing")
    sys.exit(0)
if firmware == "uefi" and not os.path.exists(OVMF):
    print("SKIP install: OVMF missing")
    sys.exit(0)
if not os.path.exists(ISO):
    sys.exit(f"install: {ISO} is missing (make installer)")

tmp = tempfile.TemporaryDirectory()
disk = keep or os.path.join(tmp.name, "target.img")
with open(disk, "wb") as f:
    f.truncate(DISK_MIB << 20)
fails = 0


def check(name, ok, detail=""):
    global fails
    print(("ok   " if ok else "FAIL ") + name + ("" if ok else ": " + detail))
    if not ok:
        fails += 1


def machine(cd):
    fw = []
    if firmware == "uefi":
        vars_ = os.path.join(tmp.name, "vars.fd")
        shutil.copy(OVMF_VARS, vars_)
        fw = ["-drive", f"if=pflash,format=raw,readonly=on,file={OVMF}", "-drive", f"if=pflash,format=raw,file={vars_}"]
    devs = ["-cpu", "qemu64,+pdpe1gb", "-m", "512M", "-smp", "2", "-display", "none", "-no-reboot",
            "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
            "-drive", f"file={disk},format=raw,if=none,id=vd"]
    if bus == "nvme":
        devs += ["-device", "nvme,serial=bastios,drive=vd,bootindex=1"]
    elif bus == "ahci":
        devs += ["-device", "ahci,id=ahci", "-device", "ide-hd,drive=vd,bus=ahci.0,bootindex=1"]
    else:
        devs += ["-device", "virtio-blk-pci,drive=vd,bootindex=1"]
    if cd:
        devs += ["-drive", f"file={ISO},format=raw,media=cdrom,if=none,id=cd,readonly=on",
                 "-device", "ide-cd,drive=cd,bootindex=0"]
    devs += ["-netdev", "user,id=n0", "-device", "virtio-net-pci,netdev=n0", "-serial", "stdio"]
    return subprocess.Popen(["qemu-system-x86_64"] + fw + devs, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)


class Session:
    def __init__(self, q, limit):
        self.q, self.out, self.deadline = q, b"", time.time() + limit

    def read(self, t=0.2):
        r, _, _ = select.select([self.q.stdout], [], [], t)
        if r:
            d = os.read(self.q.stdout.fileno(), 65536)
            self.out += d
            if LOG:
                LOG.write(d)
                LOG.flush()
            return bool(d)
        return True

    def until(self, marker, start=0):
        """Waits for marker in the output after `start`; returns where it ends."""
        m = marker.encode()
        while True:
            i = self.out.find(m, start)
            if i >= 0:
                return i + len(m)
            if time.time() > self.deadline or not self.read():
                if m in self.out[start:]:
                    continue
                return -1

    def send(self, s):
        self.q.stdin.write(s.encode())
        self.q.stdin.flush()

    def text(self):
        t = self.out.decode("latin-1")
        t = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", t)
        return t.replace("\r", "")

    def finish(self):
        while time.time() < self.deadline and self.q.poll() is None:
            self.read()
        if self.q.poll() is None:
            self.q.kill()
            self.q.wait()
            return None                     # it never ended by itself
        return self.q.returncode


def step(s, marker, what, start=0):
    at = s.until(marker, start)
    if at < 0:
        check(what, False, f"never saw {marker!r}; the last output:\n" + s.text()[-1500:])
        s.q.kill()
        sys.exit(1)
    return at


# ------------------------------------------------------------ the installation
t0 = time.time()
s = Session(machine(True), 900)
at = step(s, "Type erase and Enter", "the installer starts")
check("the disk is seen", re.search(r"The disk: \d+ sectors, %d MiB, blank" % DISK_MIB, s.text()) is not None, s.text()[-600:])
s.send("erase\r")
at = step(s, "BastiOS is installed", "the installation ends", at)
text = s.text()
check("the disk is checked after writing", "the disk is partitioned and the boot loader is in place" in text)
check("the partition is mounted", re.search(r"disk/ is the new BastiOS partition: \d+ blocks", text) is not None, text[-800:])
s.send("\r")
rc = s.finish()
check("the machine restarts", rc is not None and "pages free at restart" in s.text(), s.text()[-500:])
print(f"     installed in {time.time() - t0:.0f} s")

# ------------------------------------------------------------ the disk, on the host
buf = open(disk, "rb").read(1 << 20)
check("a protective MBR", buf[450] == 0xEE and buf[510:512] == b"\x55\xaa")
check("Limine's boot code in sector 0", any(buf[:440]))
check("a GPT", buf[512:520] == b"EFI PART")
ents = buf[1024:1024 + 3 * 128]
types = [uuid.UUID(bytes_le=ents[i * 128:i * 128 + 16]) for i in range(3)]
check("the BIOS boot, EFI system and BastiOS partitions",
      types == [uuid.UUID("21686148-6449-6e6f-744e-656564454649"), uuid.UUID("c12a7328-f81f-11d2-ba4b-00a0c93ec93b"),
                uuid.UUID("e0c9ea2a-d5f1-45b3-a799-0a7bec83d701")], str(types))
first, last = struct.unpack_from("<QQ", ents, 2 * 128 + 32)
check("the BastiOS partition runs to the end of the disk", last == DISK_MIB * 2048 - 34, f"{first} {last}")
if shutil.which("fsck.fat") and shutil.which("mdir"):
    esp_first, esp_last = struct.unpack_from("<QQ", ents, 128 + 32)
    esp = os.path.join(tmp.name, "esp.img")
    with open(disk, "rb") as f, open(esp, "wb") as o:
        f.seek(esp_first * 512)
        o.write(f.read((esp_last - esp_first + 1) * 512))
    ls = subprocess.run(["mdir", "-i", esp, "-/", "-b", "::"], capture_output=True, text=True).stdout
    for want in ("::/EFI/BOOT/BOOTX64.EFI", "::/boot/bastios.elf", "::/boot/limine/limine.conf"):
        check(f"the EFI system partition has {want[2:]}", want in ls, ls)

# ------------------------------------------------------------ the new system, alone
q = machine(False)
s = Session(q, 600)
at = step(s, "bastios> ", "the new system starts")
text = s.text()
check("the kernel finds its partition", re.search(r"disk: the BastiOS partition at sector %d" % first, text) is not None, text[-900:])
s.send("cat disk/a/etc/alpine-release\r")
at = step(s, "\nbastios> ", "the shell answers", at)
check("Alpine Linux is on the disk", re.search(r"\n\d+\.\d+\.\d+\n", s.text()[-80:]) is not None, s.text()[-400:])
s.send("ash\r")
s.send("/disk/alpine\r")
at = step(s, "# ", "Alpine's shell starts", at)
s.send("cat /etc/os-release | head -n 1; echo sum$((6 * 7))\r")
at = step(s, "sum42", "Alpine's shell runs commands", at)
check("it is Alpine Linux", 'NAME="Alpine Linux"' in s.text(), s.text()[-500:])
s.send("exit\r")
s.send("exit\r")
step(s, "bastios> ", "back at the BastiOS prompt", at)
s.send("halt\r")
rc = s.finish()
check("the new system halts cleanly", rc == 33, f"exit status {rc}")
check("every page is back at power off", re.search(r"BastiOS: \d+ pages free at power off", s.text()) is not None)

if fails:
    print(f"install ({firmware}, {bus}): {fails} FAILED")
    sys.exit(1)
print(f"install ({firmware}, {bus}): ok")
