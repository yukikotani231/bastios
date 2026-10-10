# BastiOS build.
#   make            build build/bastios.elf
#   make run        boot it in QEMU on this terminal (Ctrl-A X to quit)
#   make iso        build/bastios.iso: bootable from CD or USB, BIOS or UEFI
#   make run-iso    boot the ISO in QEMU (run-uefi: under UEFI firmware)
#   make test       boot it in QEMU and drive the shell over the serial line
#   make fuzz       fuzz the system calls (SEED, SEEDS, CALLS, SMP)
#
# BASTIOC points at the Bastio stage0 compiler (github.com/yukikotani231/bastiolang).

BASTIOC ?= ../bastiolang/stage0/target/release/bastioc0
CC      ?= cc
QEMU    ?= qemu-system-x86_64

BASE_CFLAGS := -std=c11 -O2 -ffreestanding -fno-builtin -fno-stack-protector \
               -fno-pic -fno-pie -mno-red-zone -mgeneral-regs-only -nostdlib
# Generated C (proved Bastio) is compiled without warnings: it declares
# temporaries it may not use. The hand-written C in boot/ is the trusted
# base, so every warning there is an error. GCC takes reads of the BIOS data
# area near address 0 for null dereferences unless told pages may be that
# small; Clang has no such parameter and no such warning.
CFLAGS  := $(BASE_CFLAGS) -w
TCFLAGS := $(BASE_CFLAGS) -Wall -Wextra -Werror \
           $(shell $(CC) --param=min-pagesize=0 -Werror -E -x c /dev/null >/dev/null 2>&1 && echo --param=min-pagesize=0)
LDFLAGS := -nostdlib -static -no-pie -z max-page-size=0x1000 -T boot/linker.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments

all: build/bastios.elf

build build/user:
	mkdir -p $@

# 1. User programs: prove each one, compile it as position-independent code,
#    link it at USER_BASE behind its header, and flatten it to a raw image
#    that the kernel embeds with include_bytes.
UCFLAGS := $(CFLAGS) -fpie
UPROGS  := sh hello spin true bench fstest crashme diskbench
UASM    := fault straddle
ELFPROGS := elfexec elfpie elfbad mhello fpucheck mtest mudp mtcp mtime mdir mmisc mspawn sysfuzz mthread mdyn ash
UBINS   := $(UPROGS:%=build/user/%.bin) $(UASM:%=build/user/%.bin) $(ELFPROGS:%=build/user/%.elf) build/user/ld-musl.so

build/user/%.c: user/%.bastio $(wildcard user/lib/*.bastio) $(BASTIOC) | build/user
	$(BASTIOC) --target user -o $@ $<

build/user/%.o: build/user/%.c
	$(CC) $(UCFLAGS) -c -o $@ $<

build/user/start.o: user/start.S | build/user
	$(CC) -c -o $@ $<

$(UASM:%=build/user/%.o): build/user/%.o: user/%.S | build/user
	$(CC) -c -o $@ $<

build/user/libc.o: boot/libc.c | build/user
	$(CC) $(TCFLAGS) -fpie -c -o $@ $<

ULDFLAGS := -nostdlib -static -no-pie -z max-page-size=0x1000 -T user/user.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments

build/user/%.elf: build/user/start.o build/user/%.o build/user/libc.o user/user.ld
	$(CC) $(ULDFLAGS) -o $@ build/user/start.o build/user/$*.o build/user/libc.o

$(UASM:%=build/user/%.elf): build/user/%.elf: build/user/%.o user/user.ld
	$(CC) $(ULDFLAGS) -o $@ $<

# Static Linux ELF executables, embedded as they are (not flattened): the
# kernel's ELF loader maps their segments. elfexec is linked at USER_BASE
# (ET_EXEC), elfpie as a static PIE (ET_DYN), and elfbad is elfexec cut
# short, so its segments run past the end of the file and it must be refused.
ELFCFLAGS := $(UCFLAGS) -fno-asynchronous-unwind-tables

build/user/elfcheck.o: user/elfcheck.c | build/user
	$(CC) $(ELFCFLAGS) -c -o $@ $<

build/user/elfstart.o: user/elfstart.S | build/user
	$(CC) -c -o $@ $<

build/user/elfexec.elf: build/user/elfstart.o build/user/elfcheck.o
	$(CC) -nostdlib -static -no-pie -Wl,-Ttext-segment=0x8000000000 -Wl,--build-id=none -o $@ $^

build/user/elfpie.elf: build/user/elfstart.o build/user/elfcheck.o
	$(CC) -nostdlib -static-pie -Wl,--build-id=none -o $@ $^

build/user/fpucheck.o: user/fpucheck.c | build/user
	$(CC) $(filter-out -mgeneral-regs-only,$(ELFCFLAGS)) -c -o $@ $<

build/user/fpucheck.elf: build/user/elfstart.o build/user/fpucheck.o
	$(CC) -nostdlib -static-pie -Wl,--build-id=none -o $@ $^

# The system call fuzzer (user/sysfuzz.c, test/fuzz_test.sh): freestanding,
# so nothing between it and the kernel tidies its arguments.
build/user/sysfuzz.o: user/sysfuzz.c | build/user
	$(CC) $(ELFCFLAGS) -c -o $@ $<

# Linked at USER_BASE: its tables of pointers need no relocation.
build/user/sysfuzz.elf: build/user/elfstart.o build/user/sysfuzz.o
	$(CC) -nostdlib -static -no-pie -Wl,-Ttext-segment=0x8000000000 -Wl,--build-id=none -o $@ $^

# A C program against musl libc, static PIE (the musl-tools package).
MUSLCC ?= musl-gcc

build/user/mhello.elf build/user/mtest.elf build/user/mudp.elf build/user/mtcp.elf build/user/mtime.elf build/user/mdir.elf build/user/mmisc.elf build/user/mspawn.elf build/user/mthread.elf: build/user/%.elf: user/%.c | build/user
	$(MUSLCC) -static-pie -O2 -o $@ $<

# A dynamically linked PIE, and musl's dynamic linker (libc.so, which is
# also /lib/ld-musl-x86_64.so.1) that the kernel loads as its interpreter.
build/user/mdyn.elf: user/mdyn.c | build/user
	$(MUSLCC) -fPIE -pie -O2 -o $@ $<

MUSL_LDSO ?= /lib/ld-musl-x86_64.so.1

build/user/ld-musl.so: | build/user
	cp -L $(MUSL_LDSO) $@

# BusyBox (GPL-2.0-only, not part of this repository): fetched at a pinned
# tag, configured from allnoconfig plus the options in user/busybox.config,
# and built against musl as a static PIE. `ash` is its shell (BusyBox picks
# the applet from argv[0], which the kernel sets to the program name).
BUSYBOX_GIT ?= https://github.com/mirror/busybox
BUSYBOX_TAG ?= 1_36_1

build/busybox/Makefile: | build
	rm -rf build/busybox
	git clone -q --depth 1 --branch $(BUSYBOX_TAG) $(BUSYBOX_GIT) build/busybox

build/user/ash.elf: build/busybox/Makefile user/busybox.config | build/user
	$(MAKE) -C build/busybox -s allnoconfig >/dev/null
	for o in $$(grep -v '^#' user/busybox.config); do \
	  sed -i "s/^# $$o is not set/$$o=y/" build/busybox/.config; done
	yes "" | $(MAKE) -C build/busybox -s oldconfig >/dev/null
	$(MAKE) -C build/busybox -s CC=$(MUSLCC) EXTRA_CFLAGS=-fPIE CFLAGS_busybox=-static-pie busybox
	cp build/busybox/busybox $@

build/user/elfbad.elf: build/user/elfexec.elf
	head -c 4200 $< > $@

build/user/%.bin: build/user/%.elf
	objcopy -O binary $< $@

# 2. Prove the kernel and translate it to freestanding C.
KSRCS := $(wildcard src/*.bastio) src/font8x8.bin
KFILES := $(wildcard files/*)

# src/main.bastio imports the rest.
build/kernel.c: $(KSRCS) $(KFILES) $(UBINS) $(BASTIOC) | build
	$(BASTIOC) --target kernel -o $@ src/main.bastio

build/kernel.o: build/kernel.c
	$(CC) $(CFLAGS) -c -o $@ $<

build/libc.o: boot/libc.c | build
	$(CC) $(TCFLAGS) -c -o $@ $<

build/boot.o: boot/boot.S boot/core.h | build
	$(CC) -c -o $@ $<

build/platform.o: boot/platform.c | build
	$(CC) $(TCFLAGS) -c -o $@ $<

build/smp.o: boot/smp.c boot/core.h | build
	$(CC) $(TCFLAGS) -c -o $@ $<

build/multiboot.o: boot/multiboot.c | build
	$(CC) $(TCFLAGS) -c -o $@ $<

build/ap.o: boot/ap.S | build
	$(CC) -c -o $@ $<

# 3. Link a 64-bit kernel, then repackage it as ELF32: QEMU's -kernel
#    Multiboot loader only accepts 32-bit ELF images. The code is unchanged.
KOBJS := build/boot.o build/ap.o build/platform.o build/smp.o build/multiboot.o build/kernel.o build/libc.o

build/bastios64.elf: $(KOBJS) boot/linker.ld
	$(CC) $(LDFLAGS) -o $@ $(KOBJS)

build/bastios.elf: build/bastios64.elf
	objcopy -O elf32-i386 $< $@

# A bootable image: GRUB (BIOS and UEFI, as a hybrid ISO that can also be
# written to a USB stick) loading the same Multiboot kernel. Needs
# grub-mkrescue, xorriso, mtools, and GRUB's i386-pc and x86_64-efi files
# (grub-pc-bin and grub-efi-amd64-bin on Ubuntu).
build/bastios.iso: build/bastios.elf boot/grub.cfg
	rm -rf build/iso
	mkdir -p build/iso/boot/grub
	cp build/bastios.elf build/iso/boot/
	cp boot/grub.cfg build/iso/boot/grub/
	grub-mkrescue -o $@ build/iso 2>/dev/null

iso: build/bastios.iso

# The disk: a raw image, here as a virtio block device. BastiOS formats it
# the first time it boots, and the files under disk/ live there across
# boots.
DISK    ?= build/disk.img
SMP     ?= 2
OVMF    ?= /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS ?= /usr/share/OVMF/OVMF_VARS_4M.fd
# A virtio disk and network card (QEMU's user network: DHCP, and the host
# reachable at 10.0.2.2; TCP port 7001 on this machine reaches the guest's
# port 7001, for `tcpecho 7001`). QEMU's default CPU model lacks 1 GiB
# pages, which boot.S needs to map the device window above 4 GiB (where
# UEFI firmware puts 64-bit PCI BARs); every real x86-64 CPU of the last
# decade has them.
QEMU_DEVS := -cpu qemu64,+pdpe1gb -m 256M -smp $(SMP) -no-reboot \
             -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
             -drive file=$(DISK),format=raw,if=none,id=vd -device virtio-blk-pci,drive=vd \
             -netdev user,id=n0,hostfwd=tcp:127.0.0.1:7001-:7001 -device virtio-net-pci,netdev=n0
QEMU_ARGS := -kernel build/bastios.elf -display none $(QEMU_DEVS)
# The ISO targets open QEMU's window, for the screen and keyboard
# (ISO_DISPLAY="-display none" for serial only).
ISO_DISPLAY ?=

$(DISK): | build
	truncate -s 33M $@

# QEMU exits with 33 when the kernel powers off through isa-debug-exit
# (`halt`), which is a clean stop, not a failure.
CLEAN_EXIT := ; s=$$?; [ $$s -eq 0 ] || [ $$s -eq 33 ]

# On this terminal; Ctrl-A X quits.
run: build/bastios.elf $(DISK)
	$(QEMU) $(QEMU_ARGS) -serial mon:stdio $(CLEAN_EXIT)

# The ISO through GRUB, under BIOS and under UEFI (OVMF firmware).
run-iso: build/bastios.iso $(DISK)
	$(QEMU) -cdrom build/bastios.iso $(ISO_DISPLAY) $(QEMU_DEVS) -serial mon:stdio $(CLEAN_EXIT)

run-uefi: build/bastios.iso $(DISK)
	cp $(OVMF_VARS) build/ovmf_vars.fd
	$(QEMU) -drive if=pflash,format=raw,readonly=on,file=$(OVMF) \
	  -drive if=pflash,format=raw,file=build/ovmf_vars.fd \
	  -cdrom build/bastios.iso $(ISO_DISPLAY) $(QEMU_DEVS) -serial mon:stdio $(CLEAN_EXIT)

test: build/bastios.elf
	test/trusted_base.sh
	test/shell_test.sh
	test/tcp_test.sh
	NIC=e1000 test/tcp_test.sh
	test/iso_test.sh
	test/nmi_test.sh
	test/fuzz_test.sh 1 2 2000

# The system call fuzzer at length: SEEDS runs of CALLS calls from seed
# SEED (see test/fuzz_test.sh; `make test` runs a short one).
SEED  ?= 1
SEEDS ?= 16
CALLS ?= 5000
fuzz: build/bastios.elf
	SMP=$(SMP) test/fuzz_test.sh $(SEED) $(SEEDS) $(CALLS)

# The Linux compatibility suite (test/compat): static musl programs that
# exercise the system call surface, and a BusyBox with a broad set of
# applets, run from a disk image under ash. Writes build/compat/report.md
# (pass/fail per check, and missing system calls ranked by how many checks
# they block). Not part of `make test`: most checks are expected to fail
# today. It fails only if a check in test/compat/baseline.txt stops passing.
COMPAT_PROGS := $(patsubst test/compat/progs/%.c,%,$(wildcard test/compat/progs/t_*.c))
COMPAT_BINS  := $(COMPAT_PROGS:%=build/compat/%)

$(COMPAT_BINS): build/compat/%: test/compat/progs/%.c test/compat/progs/compat.h | build/compat
	$(MUSLCC) -static-pie -O2 -Wall -Wno-unused-result -o $@ $<

build/compat:
	mkdir -p $@

build/compat/busybox: build/busybox/Makefile test/compat/busybox.config | build/compat
	rm -rf build/compat/bb && cp -r build/busybox build/compat/bb
	$(MAKE) -C build/compat/bb -s clean >/dev/null
	$(MAKE) -C build/compat/bb -s allnoconfig >/dev/null
	for o in $$(grep -v '^#' test/compat/busybox.config); do \
	  sed -i "s/^# $$o is not set/$$o=y/" build/compat/bb/.config; done
	yes "" | $(MAKE) -C build/compat/bb -s oldconfig >/dev/null
	$(MAKE) -C build/compat/bb -s CC=$(MUSLCC) EXTRA_CFLAGS=-fPIE CFLAGS_busybox=-static-pie busybox
	cp build/compat/bb/busybox $@

build/compat/disk.img: $(COMPAT_BINS) build/compat/busybox test/compat/mkdisk.py
	test/compat/mkdisk.py $@ $(COMPAT_BINS) build/compat/busybox

compat: build/bastios.elf build/compat/disk.img
	test/compat/run.py --kernel build/bastios.elf --disk build/compat/disk.img --out build/compat $(COMPAT_PROGS)

# Records which checks pass now, so `make compat` catches regressions.
compat-baseline: build/bastios.elf build/compat/disk.img
	test/compat/run.py --update-baseline --kernel build/bastios.elf --disk build/compat/disk.img --out build/compat $(COMPAT_PROGS)

# The same checks on this Linux machine: are the expectations right?
compat-host: $(COMPAT_BINS) build/compat/busybox
	test/compat/run.py --host build/compat --out build/compat/host $(COMPAT_PROGS)

# Not part of `make test`: prints timings, checks nothing (see the script).
smpbench: build/bastios.elf
	test/smpbench.sh

# Not part of `make test` either: disk timings for each driver.
diskbench: build/bastios.elf
	test/diskbench.sh

clean:
	rm -rf build

.PRECIOUS: build/user/%.c build/user/%.o build/user/%.elf
.PHONY: all run test fuzz smpbench diskbench compat compat-baseline compat-host clean
