# BastiOS

BastiOS is an x86-64 kernel written in [Bastio](https://yukikotani231.github.io/bastiolang/),
a systems language whose memory safety is proved at compile time. The kernel
runs its own programs and unmodified Linux ones (BusyBox, Lua, Python, sqlite,
git, a C compiler, Alpine Linux's packages), on several processors, with files
on a disk that survives power cuts and a TCP/IP stack.

> **Status: experimental.** Everything has been tried in QEMU; no real PC has
> been confirmed to boot it yet. The system call ABI and the disk format can
> still change.

## Why it is different

None of the Bastio code in the kernel has runtime checks. The compiler proves,
before it emits anything, that every index is in bounds, no arithmetic
overflows, every physical page is owned by exactly one thing at a time, page
tables are empty before they are freed, and every device is driven only
through the I/O ports it claimed. Introduce an off-by-one, drop an ownership
check, or free a table that still maps a page, and the build fails with a
counterexample instead of producing a kernel.

What the proofs do not cover is listed, not hidden: the assembly and C under
`boot/`, the C compiler, Z3 and the runtime prelude are the trusted base. See
[What is trusted](trusted-base.md).

## Quick start in QEMU

On x86-64 Linux with Rust, Z3, QEMU, a C compiler and musl-gcc installed
(the [Getting started](getting-started.md) page has the full list):

```sh
git clone https://github.com/yukikotani231/bastiolang
git clone https://github.com/yukikotani231/bastios
(cd bastiolang/stage0 && cargo build --release)
cd bastios && make run
```

The first build proves every source file and fetches BusyBox, so it takes a
while. `make run` boots on your terminal and drops you at the `bastios>`
prompt; type `help`, and press Ctrl-A X to quit QEMU.

```text
bastios> hello
hello from user mode, pid 2
bastios> ash
# echo hi there > /disk/notes; cat /disk/notes | wc -c
9
```

Other targets:

| Command | What it does |
| --- | --- |
| `make test` | Boots in QEMU, types commands over the serial line and checks the replies |
| `make compat` | Runs the Linux compatibility suite and writes `build/compat/report.md` |
| `make iso` | Builds a Limine image that boots under BIOS or UEFI |
| `make usb` | Writes the same image to a USB stick |

## What works

| Area | Status |
| --- | --- |
| Processes | Ring 3 programs in their own address spaces, preemptive scheduling on every processor, threads that share an address space |
| Linux programs | Static and dynamic x86-64 ELF executables against musl, static against glibc; much of the system call interface ([details](linux.md)) |
| Files | In-memory file system, and a copy-on-write disk file system that keeps every write whole across a power cut |
| Storage | USB stick, NVMe, virtio, SATA (AHCI) and legacy ATA disks |
| Network | virtio, Intel e1000 families, Realtek RTL8111/8168 and AWS ENA cards; DHCP, DNS, IPv4, IPv6, UDP, TCP, loopback, Unix sockets |
| Users and login | Users and passwords with `login` and `su`, an SSH server (Dropbear) |
| Userland | BusyBox shell, Alpine's `apk` in a `chroot`, Lua, Python, sqlite, git, a C compiler |
| Containers | Namespaces and cgroups enough to run runc and crun |
| Architectures | x86-64; [arm64](aarch64.md) under QEMU |

Some drivers are untested against real devices; the pages for
[real hardware](real-hardware.md) and [cloud guests](cloud.md) say which.

## Not yet

A real PC confirmed to boot it, a login prompt at boot, users and SSH keys
that survive a reboot, and a graphical desktop. The compatibility suite's
current results are in `build/compat/report.md` after `make compat`, and the
[changelog](changelog.md) records what changed in each release.

## Learn more

- [Architecture](architecture.md): how the pieces fit together and what is proved where.
- [The Bastio language](https://yukikotani231.github.io/bastiolang/): the language reference, tutorial and roadmap.
- [Source on GitHub](https://github.com/yukikotani231/bastios) and the [contributing guide](contributing.md).

BastiOS is dual-licensed under Apache-2.0 or MIT.
