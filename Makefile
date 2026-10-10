# BastiOS build.
#   make            build build/bastios.elf
#   make run        boot it in QEMU on this terminal (Ctrl-A X to quit)
#   make test       boot it in QEMU and drive the shell over the serial line
#
# BASTIOC points at the Bastio stage0 compiler (github.com/yukikotani231/bastiolang).

BASTIOC ?= ../bastiolang/stage0/target/release/bastioc0
CC      ?= cc
QEMU    ?= qemu-system-x86_64

CFLAGS  := -std=c11 -O2 -ffreestanding -fno-builtin -fno-stack-protector \
           -fno-pic -fno-pie -mno-red-zone -mgeneral-regs-only -nostdlib -w
LDFLAGS := -nostdlib -static -no-pie -z max-page-size=0x1000 -T boot/linker.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments

all: build/bastios.elf

build build/user:
	mkdir -p $@

# 1. User programs: prove each one, compile it as position-independent code,
#    link it at USER_BASE behind its header, and flatten it to a raw image
#    that the kernel embeds with include_bytes.
UCFLAGS := $(CFLAGS) -fpie
UPROGS  := sh hello spin true bench fstest
UASM    := fault straddle
ELFPROGS := elfexec elfpie elfbad mhello fpucheck mtest ash
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
	$(CC) $(UCFLAGS) -c -o $@ $<

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

build/user/mhello.elf build/user/mtest.elf: build/user/%.elf: user/%.c | build/user
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
KSRCS := src/uart.bastio src/pic.bastio src/pit.bastio src/hpet.bastio src/console.bastio src/pages.bastio src/space.bastio src/elf.bastio src/ramfs.bastio src/ata.bastio src/diskfs.bastio src/fs.bastio \
         src/proc.bastio src/usermem.bastio src/syscall.bastio src/linux.bastio src/main.bastio
KFILES := $(wildcard files/*)

# src/main.bastio imports the rest.
build/kernel.c: $(KSRCS) $(KFILES) $(UBINS) $(BASTIOC) | build
	$(BASTIOC) --target kernel -o $@ src/main.bastio

build/kernel.o: build/kernel.c
	$(CC) $(CFLAGS) -c -o $@ $<

build/libc.o: boot/libc.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/boot.o: boot/boot.S boot/core.h | build
	$(CC) -c -o $@ $<

build/platform.o: boot/platform.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/smp.o: boot/smp.c boot/core.h | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/ap.o: boot/ap.S | build
	$(CC) -c -o $@ $<

# 3. Link a 64-bit kernel, then repackage it as ELF32: QEMU's -kernel
#    Multiboot loader only accepts 32-bit ELF images. The code is unchanged.
KOBJS := build/boot.o build/ap.o build/platform.o build/smp.o build/kernel.o build/libc.o

build/bastios64.elf: $(KOBJS) boot/linker.ld
	$(CC) $(LDFLAGS) -o $@ $(KOBJS)

build/bastios.elf: build/bastios64.elf
	objcopy -O elf32-i386 $< $@

# The disk: a raw image on the first ATA drive. BastiOS formats it the first
# time it boots, and the files under disk/ live there across boots.
DISK    ?= build/disk.img
QEMU_ARGS := -kernel build/bastios.elf -m 64M -display none -no-reboot \
             -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
             -drive file=$(DISK),format=raw,if=ide,index=0

$(DISK): | build
	truncate -s 33M $@

run: build/bastios.elf $(DISK)
	$(QEMU) $(QEMU_ARGS) -serial mon:stdio

test: build/bastios.elf
	test/shell_test.sh

clean:
	rm -rf build

.PRECIOUS: build/user/%.c build/user/%.o build/user/%.elf
.PHONY: all run test clean
