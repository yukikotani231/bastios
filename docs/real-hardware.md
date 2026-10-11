# Trying BastiOS on a real machine

BastiOS has been run only in QEMU so far. This page is for anyone willing to boot it on a real PC and tell us what happened: what to prepare, what to check, and what to send back. A report that says "it stopped here" is as useful as one that says "everything worked".

## Is it safe for the machine?

BastiOS does not install itself and changes no firmware settings. It boots from a USB stick and writes to only two kinds of disk:

- the BastiOS partition of the USB stick it was written to with `make usb`;
- a disk that is completely blank (its first and last 34 sectors are all zeros).

Any other disk, such as the one with your own operating system on it, is left alone: it has a partition table, so BastiOS reads its first and last sectors, sees that it is someone else's, and never writes to it. The boot log then says `disk: it holds a partition table or other data; left alone`. `test/shell_test.sh` checks this with a disk image that has an MBR and one that has only a GPT backup header: not a byte of either changes.

Still, BastiOS is young software with drivers that have only met QEMU's devices. Use a machine whose data is backed up, or a spare one.

## What you need

- An x86-64 PC. It boots under BIOS (legacy/CSM) or UEFI. **Turn Secure Boot off**: the Limine on the image is not signed.
- A USB stick you can erase.
- A camera (a phone is fine) for the screen.
- Optional, and the best record if you have it: a serial port (COM1) and a null-modem or USB-serial cable to another computer. Set it to 115200 baud, 8N1, and capture everything.

To capture the serial log on the other computer (`/dev/ttyUSB0` is a USB-serial adapter's usual name on Linux; on macOS it is `/dev/tty.usbserial-...`):

```text
picocom -b 115200 --logfile bastios.log /dev/ttyUSB0      # Linux; quit with Ctrl-A Ctrl-X
screen -L -Logfile bastios.log /dev/tty.usbserial-XXXX 115200   # macOS; quit with Ctrl-A K
```

On Windows, PuTTY does it: Connection type Serial, speed 115200, and under Session > Logging choose "All session output". Start the capture before powering the PC on. The serial line also takes input, so you can type the commands below there instead of on the PC.

## Make the stick

You need the USB image, `bastios-usb.img`. Either build it (on x86-64 Linux, set up as in [getting-started.md](getting-started.md), then `make usb`; it lands in `build/bastios-usb.img`) or use one the maintainers give you, with its SHA-256 checksum to compare against. The image is the ISO followed by a 64 MiB partition where BastiOS keeps its files. A release's ISO (`bastios-vX.Y.Z.iso`) can be written the same way if you would rather not build anything, but it has no partition, so step 7 below cannot work with it.

Writing the image erases the whole stick. Whatever tool you use, check twice that it is about to write the stick and not another disk.

**Linux**

```text
sha256sum bastios-usb.img            # compare with the checksum you were given
lsblk                                # find the stick: /dev/sdX
sudo dd if=bastios-usb.img of=/dev/sdX bs=4M conv=fsync
```

**macOS**

```text
shasum -a 256 bastios-usb.img
diskutil list                        # find the stick: /dev/diskN (external, physical)
diskutil unmountDisk /dev/diskN
sudo dd if=bastios-usb.img of=/dev/rdiskN bs=4m
diskutil eject /dev/diskN
```

`rdiskN` (with the `r`) is the same stick, written much faster. If macOS then says the disk is not readable, choose **Ignore**: it cannot read BastiOS's partition, which is expected.

**Windows**

Use [Rufus](https://rufus.ie): pick the stick under Device, choose the image with SELECT (set the file filter to "All files" to see a `.img`), and press START. If Rufus asks how to write it, choose **DD image** mode. [balenaEtcher](https://etcher.balena.io) also works and needs no choices. Check the checksum first with `certutil -hashfile bastios-usb.img SHA256` in a command prompt.

## Boot it

1. Plug the stick in and open the firmware's boot menu. The key is usually F12, F11, F10, F8 or Esc, pressed right after power on. Pick the USB stick: under UEFI it may be listed as "UEFI: (the stick's name)".
2. The Limine menu waits three seconds. Choose **BastiOS, with a hardware report** with the arrow keys and Enter.
3. The kernel prints its boot log and then the hardware report, one screenful at a time. After each part it waits for a key, or a minute: **photograph the screen each time it stops**, then press a key. There are three parts.
4. The shell prompt `bastios>` appears.

## What to check

Note for each step whether it worked. If something goes wrong, note what it did instead, because that is what we need to know.

| # | Do | Expected |
|---|---|---|
| 1 | Boot from the stick | The Limine menu, with two BastiOS entries |
| 2 | Pick the hardware report entry | `BastiOS 0.2: N pages of memory, P processors`, then the report |
| 3 | Look at the screen | Text on the screen, not only on the serial line. If the screen stays black but the machine has a serial port, the log is there |
| 4 | Type `help` and Enter | The list of commands. Try both a PS/2 keyboard (or the laptop's own) and a USB one if you have them |
| 5 | Count the processors | `P processors` matches the machine's threads (cores times threads per core) |
| 6 | Type `clock`, wait about ten seconds by a watch, type `clock` again | The two numbers differ by about 10,000,000,000 (nanoseconds) |
| 7 | `echo hello > disk/note`, then `cat disk/note` | `hello`. Then type `reboot`: the machine restarts. Boot BastiOS again and `cat disk/note` still prints `hello` |
| 8 | `ash`, then `ls /`, then `exit` | BusyBox's shell runs Linux programs |
| 9 | `ifconfig` | An address, but only with a supported network card (an Intel 8254x, 82574L, 82576, I350, I210 or I211, or virtio): most real cards have no driver yet, and then it has none |
| 10 | `halt` | `bye`, and the machine switches off |
| 11 | Boot again, then press the power button briefly | `power button: powering off`, and the machine switches off |

If the machine stops responding at any point, photograph the screen. Any line that starts with `bastio: kernel fault` matters most: please copy it exactly.

## If it stops early

Each of these is worth a report on its own; try the next step only if you have time.

- **The machine does not offer the stick, or goes straight to its own system.** Check that Secure Boot is off, and that USB boot is allowed in the firmware settings. Under UEFI, also try the stick's entry without "UEFI:" (that boots it through the BIOS compatibility mode, if the machine has one), and the other way round.
- **The Limine menu appears, then the screen goes black or freezes.** Photograph the last screen. If the machine has a serial port, the log is there even when the screen shows nothing: capture it at 115200 baud. Then try the first entry, **BastiOS**, which skips the report.
- **The boot log stops partway.** Photograph it: the last line says how far the kernel got.
- **The prompt appears but typing does nothing.** Try another keyboard: the laptop's own (PS/2 inside), a USB keyboard on another port, or one plugged in directly rather than through a hub or dock.
- **A SATA disk is plugged in, but the log does not say `disk: AHCI`.** Reboot, select the first entry, **BastiOS**, press `e`, add a line `cmdline: ahci_reset` at the end of the entry, and press F10 to boot.

## What to send back

Open an issue at <https://github.com/yukikotani231/bastios/issues> titled `Real hardware: <maker and model>` with:

- the machine's maker and model, and whether it booted under BIOS or UEFI;
- the photos of the boot log and the three parts of the report, or the serial log;
- the table above with ok or what happened for each step;
- anything else you noticed.

The report holds the processor's model, the memory map, the ACPI tables and PCI device ids: nothing that identifies you or what is on your disks. The network card's MAC address is in the boot log only if BastiOS has a driver for it; cover it in the photo if you like.

## What the report says

| Part | Lines | What we learn |
|---|---|---|
| Before the report | `screen:`, `keyboard:`, `disk:`, `net:` | What was found and used, and what was not |
| 1 | `cpu:` | Who made the processor, its name, family and model, and whether it has the features the kernel needs (`1gb-pages`: without it, devices above 4 GiB are out of reach) |
| 1 | `processors:` | How many processors the ACPI MADT lists and how many started, and if not all, why (no MADT, a processor that did not answer) |
| 1 | `local APIC:` | xAPIC or x2APIC mode, and the mode the firmware left it in |
| 1 | `screen:` | Where the boot loader's framebuffer is, its size, and whether the console draws on it |
| 1 | `timer:`, `tick:`, `clock:` | What the local APIC timer was calibrated against (HPET, ACPI PM timer, PIT, RTC), what gives the 10 ms tick (the PIT, or the local APIC timer when the PIT does not count) and what keeps time (the HPET, the PM timer, or the time-stamp counter at a rate measured at boot) |
| 2 | `memory:` | The firmware's memory map, region by region |
| 3 | `acpi:` | Where the ACPI tables were found, each table and its address, the power-management registers, the S5 sleep type (needed for power off) and the reset register |
| 3 | `pci` | Every PCI device: bus:slot.function, vendor:device, class, and what BastiOS does with it ("no driver" is common and fine) |

## Known limits

These are expected on real machines today, so there is no need to report them unless something else goes wrong too:

- Network: only the Intel e1000 families (8254x, 82574L, 82576, I350, I210, I211: the I350, I210 and I211 are untested, so a report on one of them is especially welcome) and virtio cards. Most desktop and laptop cards (Intel I219 and I225/I226, Realtek RTL8111/8168, and Wi-Fi) have no driver.
- USB: only xHCI (USB 3) controllers, with keyboards, mice and BastiOS-partitioned sticks, on a root port or behind up to two USB 2 hubs. With several controllers, only the first that has any of these is used, so keep the keyboard and the stick on the same kind of port (both USB-A, say). On Intel 7- and 8-series chipsets (2012-2013), whose USB 2 ports start out routed to the older EHCI controller, USB devices are not found.
- Disks: NVMe, AHCI (SATA), legacy ATA and USB sticks. A disk is used only if it is blank or already BastiOS's. If a SATA disk is plugged in but the boot log does not say `disk: AHCI`, try again with the boot option `ahci_reset` (press `e` on the first menu entry and add a line `cmdline: ahci_reset`): it resets every SATA link before looking for the disk.
- Screen: a 32-bit framebuffer that Limine sets up, in the device window below 4 GiB or anywhere above 4 GiB (where many PCs put the graphics memory; this case is untested, because no QEMU display has its framebuffer there). There is no GPU driver.
- Processors: when the local APICs have to run in x2APIC mode (the report's `local APIC:` line says so: the firmware locked that mode, or a processor has an id of 255 or more), the kernel does without MSI and wake-up interrupts, so disks and the network are polled more.
- ACPI: only the PM1a registers; PM1b is not used. The only AML read is `\_S5`. A reset register in memory has not been tested, because QEMU has none.
- Memory: the kernel uses every run of RAM the firmware reports, up to 512 GiB (above 4 GiB only on processors with 1 GiB pages).
