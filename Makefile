# BastiOS build.
#   make            build build/bastios.elf
#   make run        boot it in QEMU on this terminal (Ctrl-A X to quit)
#   make iso        build/bastios.iso: bootable from CD or USB, BIOS or UEFI
#   make run-iso    boot the ISO in QEMU (run-uefi: under UEFI firmware)
#   make test       boot it in QEMU and drive the shell over the serial line
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
UPROGS  := sh hello spin true bench fstest crashme
UASM    := fault straddle
ELFPROGS := elfexec elfpie elfbad mhello fpucheck mtest mudp mtcp mtime mdir ash
UBINS   := $(UPROGS:%=build/user/%.bin) $(UASM:%=build/user/%.bin) $(ELFPROGS:%=build/user/%.elf)

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

# A C program against musl libc, static PIE (the musl-tools package).
MUSLCC ?= musl-gcc

build/user/mhello.elf build/user/mtest.elf build/user/mudp.elf build/user/mtcp.elf build/user/mtime.elf build/user/mdir.elf: build/user/%.elf: user/%.c | build/user
	$(MUSLCC) -static-pie -O2 -o $@ $<

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
KSRCS := src/uart.bastio src/fb.bastio src/kbd.bastio src/font8x8.bin src/card.bastio src/e1000.bastio src/pic.bastio src/pit.bastio src/hpet.bastio src/console.bastio src/pages.bastio src/space.bastio src/elf.bastio src/ramfs.bastio src/ata.bastio src/pci.bastio src/virtq.bastio src/virtio_blk.bastio src/virtio_net.bastio src/net.bastio src/tcp.bastio src/blk.bastio src/diskfs.bastio src/fs.bastio \
         src/proc.bastio src/usermem.bastio src/syscall.bastio src/linux.bastio src/lxproc.bastio src/lxfd.bastio src/lxdir.bastio src/lxsock.bastio src/lxtcp.bastio src/lxsig.bastio src/lxtime.bastio src/main.bastio
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
# port 7001, for `tcpecho 7001`).
QEMU_DEVS := -m 256M -smp $(SMP) -no-reboot \
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

# Not part of `make test`: prints timings, checks nothing (see the script).
smpbench: build/bastios.elf
	test/smpbench.sh

clean:
	rm -rf build

.PRECIOUS: build/user/%.c build/user/%.o build/user/%.elf
.PHONY: all run test smpbench clean
