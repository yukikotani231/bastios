# Getting started

This guide builds BastiOS, boots it in QEMU, runs a Bastio program you write and a Linux program you compile, and makes a USB stick that boots it. It takes about as long as the first build, which proves every source file.

The programs and the terminal sessions below are tested: `make test` runs `test/guide_test.py`, which takes the programs out of this file, builds them as the guide says, boots the kernel and types each session, and fails if the replies differ from what is shown. Lines of `...` stand for output that changes from run to run.

## 1. Build

You need x86-64 Linux; Ubuntu 24.04 is what CI runs, and other systems are untested. Install the tools, and [rustup](https://rustup.rs) for the compiler:

```text
sudo apt-get install z3 build-essential qemu-system-x86 python3 musl-tools git
```

BastiOS is built by the Bastio compiler, so check out [bastiolang](https://github.com/yukikotani231/bastiolang) next to this repository and build it first:

```text
git clone https://github.com/yukikotani231/bastiolang
git clone https://github.com/yukikotani231/bastios
(cd bastiolang/stage0 && cargo build --release)
cd bastios
make -j4
```

`make` proves every kernel and user source file with Z3 (a failed proof stops the build with the line and a counterexample, as for any Bastio program), compiles the result to C and links `build/bastios.elf`. It first builds the compiler, bastiolang's bastioc1, with bastiolang's bootstrap compiler (about half a minute, once). The first build also clones BusyBox 1.36.1 and builds it against musl, for the Linux shell. Set `BASTIOC0=/path/to/bastioc0` if bastiolang is somewhere else.

## 2. Boot it in QEMU

```text
make run
```

boots the kernel on your terminal with two processors, 256 MiB of memory, a virtio disk (`build/disk.img`, kept across boots) and a virtio network card on QEMU's user network. The shell is BastiOS's own, a user program written in Bastio (`user/sh.bastio`); `help` lists its commands. Ctrl-A X quits QEMU at any time.

<!-- guide-test: session -->
```text
bastios> hello
hello from user mode, pid 2
bastios> echo hi there > disk/note
bastios> cat disk/note
hi there
bastios> mem
...
bastios> halt
bye
...
```

`hello` is a separate process in ring 3 with its own address space. `disk/note` is a file on the disk: boot again and it is still there. `halt` powers off, and the kernel checks that it got every page of memory back.

## 3. Write a program in Bastio

User programs are Bastio programs for the `user` target: they get the console as a `Serial` and reach the kernel only through system calls. Save this as `user/greet.bastio`:

<!-- guide-test: greet.bastio -->
```bastio
// Prints a greeting with this process's id, then the squares of 1 to 5.

fn puts(con: &Serial, s: Str) {
  let mut i = 0;
  while i < slen(s)
    invariant i <= slen(s)
  {
    serial_write(con, sget(s, i));
    i = i + 1;
  }
}

fn put_num(con: &Serial, n: u64) {
  let mut p = 1;
  while p <= n / 10
    invariant p >= 1
  {
    p = p * 10;
  }
  let mut q = p;
  while q > 0 {
    serial_write(con, cast[u8](48 + (n / q) % 10));
    q = q / 10;
  }
}

fn main(con: &mut Serial) -> u64 {
  puts(con, "hello from greet, pid ");
  put_num(con, sys_getpid());
  puts(con, "\r\n");
  let mut i = 1;
  while i <= 5
    invariant i <= 6
  {
    put_num(con, i * i);
    puts(con, " ");
    i = i + 1;
  }
  puts(con, "\r\n");
  return 0;
}
```

Build it, and put it on the disk:

```text
make build/user/greet.elf
make disk FILES="greet=build/user/greet.elf"
make run
```

The build proves the program first. The invariant `i <= 6` is what shows that `i * i` cannot overflow; take it out and the build stops with `could not prove that i * i does not overflow u64` and a counterexample. The [Bastio tutorial](https://github.com/yukikotani231/bastiolang/blob/main/docs/tutorial.md) explains how to read those.

`make disk` writes a new `build/disk.img` holding the files in `FILES` (`NAME=PATH`, or a plain path to keep its name), replacing what was on the disk. In the guest they are under `disk/`, or `/disk` for Linux programs. BastiOS's shell runs only the programs built into the kernel, so start a file from BusyBox's `ash`, which runs programs by path:

<!-- guide-test: session greet -->
```text
bastios> ash
...
# /disk/greet
hello from greet, pid 3
1 4 9 16 25
# exit
bastios> halt
...
```

The kernel loads `greet.elf` as an ELF executable; it talks to the kernel through BastiOS's own system calls (`int 0x80`), while `ash` uses Linux's.

## 4. Run a Linux program

BastiOS implements enough of the Linux system call interface for unmodified programs: BusyBox, Lua, QuickJS, sqlite, tcc and GNU make, Z3, and the Bastio compiler itself. Static binaries built against musl work best, and dynamically linked ones against musl work too (the kernel carries musl's dynamic linker). Save this as `hi.c`:

<!-- guide-test: hi.c -->
```c
#include <stdio.h>
#include <unistd.h>

int main(void) {
    printf("hello from Linux C, pid %d\n", getpid());
    return 0;
}
```

```text
musl-gcc -static-pie -O2 -o hi hi.c
make disk FILES="hi greet=build/user/greet.elf"
make run
```

<!-- guide-test: session hi greet -->
```text
bastios> ash
...
# /disk/hi
hello from Linux C, pid 3
# ls /disk
home   root   etc    greet  hi
# echo one two three | wc -w
3
# exit
bastios> halt
...
```

`home`, `root` and `etc` are made on the disk at its first boot: `/home`, `/root` and `/etc` link there, so users and their files are kept (see [security.md](security.md)). `ash` is BusyBox, built unmodified, and `ls`, `wc`, `cat` and its other applets in the kernel image are under `/bin`. [docs/linux.md](linux.md) lists the system calls and how each behaves, and `make compat` measures how much ordinary Linux userland runs (see [test/compat/README.md](../test/compat/README.md)).

## 5. Boot from a USB stick

`make iso` builds `build/bastios.iso`: the [Limine](https://github.com/limine-bootloader/limine) boot loader with the same kernel, as a hybrid image that boots from CD or USB, under BIOS or UEFI. It needs `xorriso` (and `ovmf` for UEFI in QEMU); the first build clones Limine's binary release at a pinned tag and builds its installer:

```text
sudo apt-get install xorriso ovmf
make iso
make run-iso     # in QEMU under BIOS
make run-uefi    # in QEMU under UEFI (OVMF)
```

Both open a QEMU window, where the kernel draws its console on the framebuffer Limine sets up and reads the keyboard; the serial console stays on your terminal. `make test` boots the image both ways (`test/iso_test.sh`, skipped when the tools are missing).

To write it to a USB stick, find the stick's device name (`lsblk`), then:

```text
sudo dd if=build/bastios.iso of=/dev/sdX bs=4M conv=fsync
```

`dd` overwrites the whole device it is given, so check the name twice.

That stick boots, but it has no room for files: they go to a disk of the machine, and only if that disk is blank (BastiOS formats a blank disk, and leaves alone any disk with a partition table or other data on it). To keep them on the stick, write the image `make usb` builds: the same ISO followed by a partition for BastiOS's file system (64 MiB; `make usb USB_MIB=1024` for more). BastiOS uses a USB disk only if it has a partition of that type, and then only that partition. `make run-usb` boots it in QEMU from a USB stick, with a USB keyboard.

```text
make usb
sudo dd if=build/bastios-usb.img of=/dev/sdX bs=4M conv=fsync
```

BastiOS has been run only in QEMU so far. To try it on a real machine and tell us what worked, see [real-hardware.md](real-hardware.md): its boot menu has an entry that prints a hardware report. On a real PC:
- **Console.** The first serial port (COM1, 115200 baud), and the screen if Limine can give it a 32-bit linear framebuffer between 2 GiB and the I/O APIC, which is where PC firmware puts it. Keys come from a PS/2 keyboard, or a USB keyboard on an xHCI (USB 3) controller, on a root port or behind up to two USB 2 hubs (a SuperSpeed device behind a USB 3 hub is not found).
- **Processor.** It needs 1 GiB pages, which every x86-64 CPU of the last decade has. Other processors are found in the ACPI tables, wherever below 4 GiB the firmware put them (above 4 GiB too, with 1 GiB pages). Local APICs in x2APIC mode work too (processors with ids of 255 and up included), though without MSI.
- **Power.** `halt` powers off and `reboot` restarts through ACPI (S5, and the FADT's reset register, a port or a memory register, else the keyboard controller or 0xCF9), and the power button powers off. The clock is the HPET, or the ACPI PM timer without one.
- **Disk.** The BastiOS partition of a USB stick or disk on an xHCI controller, as for the keyboard (`make usb`; bulk-only transport, 512-byte sectors), else an NVMe drive (namespace 1, 512-byte or 4 KiB sectors), a virtio disk, a SATA disk on an AHCI controller (the first port with a disk, 512-byte sectors), or a legacy ATA disk. A blank disk is formatted on the first boot; a disk with a partition table or other data on it (the first and last 34 sectors not all zeros) is left alone, and the boot log says so.
- **Network.** A virtio card, or an Intel card of the e1000 families: 8254x, 82574L, 82576, I350, I210 and I211 (the last three untested). Other cards are ignored.
- **Devices** are found on any PCI bus the firmware numbered, through the legacy configuration ports, not PCIe's ECAM.

## Next

- [docs/architecture.md](architecture.md): how the kernel fits together, and what is proved where.
- [docs/trusted-base.md](trusted-base.md): what is trusted instead of proved.
- [The Bastio tutorial](https://github.com/yukikotani231/bastiolang/blob/main/docs/tutorial.md) and [language reference](https://github.com/yukikotani231/bastiolang/blob/main/docs/language.md).
