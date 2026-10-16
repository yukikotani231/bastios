# BastiOS build.
#   make            build build/bastios.elf
#   make run        boot it in QEMU on this terminal (Ctrl-A X to quit)
#   make disk       put files from this machine on the disk (FILES="...")
#   make iso        build/bastios.iso: bootable from CD or USB, BIOS or UEFI
#   make usb        build/bastios-usb.img: the ISO plus a partition for the files
#   make run-usb    boot that image from a USB stick in QEMU
#   make run-iso    boot the ISO in QEMU (run-uefi: under UEFI firmware)
#   make test       boot it in QEMU and drive the shell over the serial line
#   make fuzz       fuzz the system calls (SEED, SEEDS, CALLS, SMP)
#   make soak       run a mixed workload for a long time (SECS, ALPINE)
#
# The kernel and its programs are built with bastioc1, bastiolang's compiler
# (stage1, written in Bastio), which stage0's bastioc0 builds once into
# build/bastioc1. BASTIOC names another compiler (bastioc0 still works);
# BASTIOC0 names stage0.
#
# NATIVE=1 builds the kernel and the Bastio user programs with bastioc1's own
# x86-64 backend instead of translating them to C for $(CC): no C compiler
# between the proved code and the machine code (only the assembler). Opt-in
# while its code is about ten times the size of gcc's.

# ARCH=aarch64 builds the AArch64 port (arch/aarch64/Makefile): QEMU's virt
# machine, with its own platform in boot/aarch64 and kernel in
# src/aarch64. Every goal is handed to that makefile. (ARCH is x86_64 unless
# given.)
ifeq ($(ARCH),aarch64)

.DEFAULT_GOAL := all
%:
	@$(MAKE) -f arch/aarch64/Makefile $@

else

include test/compat/images.mk

BASTIOC0 ?= ../bastiolang/stage0/target/release/bastioc0
BASTIOC  ?= build/bastioc1
# Where `import "core/..."` finds the Bastio core library.
BASTIOLIB ?= ../bastiolang
# Functions bastioc1 has proved are recorded here and not sent to the solver
# again, so a rebuild proves only what changed (`make clean` empties it;
# BASTIO_PROOF_CACHE= turns it off). The cache is trusted like the solver:
# it lives in build/, which only this checkout writes.
export BASTIO_PROOF_CACHE ?= build/proof-cache
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
ELFPROGS := elfexec elfpie elfbad mhello fpucheck mtest mudp mtcp mtime mdir mmisc mspawn sysfuzz mthread mdyn ash mkfs mtone mmouse mns mseccomp mcont mcloud mfd mproc mbig mfb bwm bhello
UBINS   := $(UPROGS:%=build/user/%.bin) $(UASM:%=build/user/%.bin) $(ELFPROGS:%=build/user/%.elf) build/user/ld-musl.so build/user/vdso.so

# Which backend built the kernel and the user programs, rewritten only when it
# changes, so that switching NATIVE rebuilds them.
KBACKEND := $(if $(filter 1,$(NATIVE)),native,c)
build/kernel.backend: FORCE | build
	@echo $(KBACKEND) | cmp -s - $@ || echo $(KBACKEND) > $@

# The programs written in Bastio (user/NAME.bastio) follow the backend; any
# other name (test/guide_test.py writes its own C) is compiled from its .c.
BASTIO_USER := $(patsubst user/%.bastio,%,$(wildcard user/*.bastio))

build/user/%.c: user/%.bastio $(wildcard user/lib/*.bastio) $(BASTIOC) | build/user
	$(BASTIOC) --lib $(BASTIOLIB) --target user -o $@ $<

build/user/%.s: user/%.bastio $(wildcard user/lib/*.bastio) $(BASTIOC) | build/user
	$(BASTIOC) --lib $(BASTIOLIB) --target user --backend native --emit-asm -o $@ $<

build/user/%.o: build/user/%.c
	$(CC) $(UCFLAGS) -c -o $@ $<

ifeq ($(KBACKEND),native)
$(BASTIO_USER:%=build/user/%.o): build/user/%.o: build/user/%.s build/kernel.backend
	$(CC) -c -o $@ $<
else
$(BASTIO_USER:%=build/user/%.o): build/user/%.o: build/user/%.c build/kernel.backend
	$(CC) $(UCFLAGS) -c -o $@ $<
endif

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
# kernel's ELF loader maps their segments. elfexec is linked at 0x400000, where a non-PIE executable lives
# (ET_EXEC), elfpie as a static PIE (ET_DYN), and elfbad is elfexec cut
# short, so its segments run past the end of the file and it must be refused.
ELFCFLAGS := $(UCFLAGS) -fno-asynchronous-unwind-tables

build/user/elfcheck.o: user/elfcheck.c | build/user
	$(CC) $(ELFCFLAGS) -c -o $@ $<

build/user/elfstart.o: user/elfstart.S | build/user
	$(CC) -c -o $@ $<

build/user/elfexec.elf: build/user/elfstart.o build/user/elfcheck.o
	$(CC) -nostdlib -static -no-pie -Wl,-Ttext-segment=0x400000 -Wl,--build-id=none -o $@ $^

build/user/elfpie.elf: build/user/elfstart.o build/user/elfcheck.o
	$(CC) -nostdlib -static-pie -Wl,--build-id=none -o $@ $^

build/user/fpucheck.o: user/fpucheck.c | build/user
	$(CC) $(filter-out -mgeneral-regs-only,$(ELFCFLAGS)) -c -o $@ $<

build/user/fpucheck.elf: build/user/elfstart.o build/user/fpucheck.o
	$(CC) -nostdlib -static-pie -Wl,--build-id=none -o $@ $^

# The system call fuzzer (user/sysfuzz.c, test/fuzz_test.sh): freestanding,
# so nothing between it and the kernel tidies its arguments.
# Its ELF mode carries three real programs to damage (.incbin), since /bin's
# programs cannot be opened as files.
build/user/sysfuzz.o: user/sysfuzz.c build/user/elfexec.elf build/user/elfpie.elf build/user/mdyn.elf | build/user
	$(CC) $(ELFCFLAGS) -c -o $@ $<

# Linked at a fixed address: its tables of pointers need no relocation.
build/user/sysfuzz.elf: build/user/elfstart.o build/user/sysfuzz.o
	$(CC) -nostdlib -static -no-pie -Wl,-Ttext-segment=0x400000 -Wl,--build-id=none -o $@ $^

# The vDSO (user/vdso.c): a tiny shared object the kernel copies onto one
# page of every Linux process, with the clock from byte 0xF80 of it. The
# whole file must fit below that (user/vdso.ld fails the link otherwise,
# and the size check catches the section headers at its end).
build/user/vdso.so: user/vdso.c user/vdso.ld user/vdso.map | build/user
	$(CC) -O2 -fPIC -ffreestanding -fno-stack-protector -fno-asynchronous-unwind-tables -mgeneral-regs-only \
	  -Wall -Wextra -Werror -nostdlib -shared -Wl,-T,user/vdso.ld -Wl,--version-script=user/vdso.map \
	  -Wl,--hash-style=both -Wl,-soname=linux-vdso.so.1 -Wl,--build-id=none -o $@ user/vdso.c
	@test $$(stat -c %s $@) -le 3968 || { echo "$@ does not fit below the vDSO's clock"; rm -f $@; exit 1; }

# A C program against musl libc, static PIE (the musl-tools package).
MUSLCC ?= musl-gcc

build/user/mhello.elf build/user/mtest.elf build/user/mudp.elf build/user/mtcp.elf build/user/mtime.elf build/user/mdir.elf build/user/mmisc.elf build/user/mspawn.elf build/user/mthread.elf build/user/mkfs.elf build/user/mtone.elf build/user/mmouse.elf build/user/mns.elf build/user/mseccomp.elf build/user/mcont.elf build/user/mcloud.elf build/user/mfd.elf build/user/mproc.elf build/user/mbig.elf build/user/mfb.elf: build/user/%.elf: user/%.c | build/user
	$(MUSLCC) -static-pie -O2 -o $@ $<

# bwm draws text with the console's font.
build/user/font8x8.h: src/font8x8.bin | build/user
	(echo 'static const unsigned char font8x8[] = {'; od -An -v -tu1 -w16 $< | sed 's/ \+/,/g; s/^,//; s/$$/,/'; echo '};') > $@

build/user/bwm.elf: user/bwm.c user/bwmproto.h build/user/font8x8.h | build/user
	$(MUSLCC) -static-pie -O2 -Ibuild/user -o $@ $<

build/user/bhello.elf: user/bhello.c user/bwmlib.h user/bwmproto.h | build/user
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
	# The non-boolean options for users: SHA-512 passwords, and ids for
	# new users from 1000 (BusyBox's defaults, which allnoconfig zeroes).
	sed -i -e 's/^CONFIG_FEATURE_DEFAULT_PASSWD_ALGO=.*/CONFIG_FEATURE_DEFAULT_PASSWD_ALGO="sha512"/' \
	  -e 's/^CONFIG_LAST_ID=.*/CONFIG_LAST_ID=60000/' -e 's/^CONFIG_FIRST_SYSTEM_ID=.*/CONFIG_FIRST_SYSTEM_ID=100/' \
	  -e 's/^CONFIG_LAST_SYSTEM_ID=.*/CONFIG_LAST_SYSTEM_ID=999/' build/busybox/.config
	$(MAKE) -C build/busybox -s CC=$(MUSLCC) EXTRA_CFLAGS=-fPIE CFLAGS_busybox=-static-pie busybox
	cp build/busybox/busybox $@

build/user/elfbad.elf: build/user/elfexec.elf
	head -c 4200 $< > $@

build/user/%.bin: build/user/%.elf
	objcopy -O binary $< $@

# 2. Prove the kernel and translate it to freestanding C.
KSRCS := $(wildcard src/*.bastio src/x86_64/arch/*.bastio) src/font8x8.bin 
KFILES := $(wildcard files/*)

# src/main.bastio imports the rest.
build/kernel.c: $(KSRCS) $(KFILES) $(UBINS) $(BASTIOC) | build
	$(BASTIOC) --lib $(BASTIOLIB) --lib src/x86_64 --lib src --target kernel -o $@ src/main.bastio

build/bastioc1.c: $(BASTIOC0) $(wildcard $(BASTIOLIB)/stage1/*.bastio) | build
	$(BASTIOC0) --emit-c -o $@ $(BASTIOLIB)/stage1/bastioc1.bastio

build/bastioc1: build/bastioc1.c
	cc -O2 -o $@ $<

build/kernel.s: $(KSRCS) $(KFILES) $(UBINS) $(BASTIOC) | build
	$(BASTIOC) --lib $(BASTIOLIB) --lib src/x86_64 --lib src --target kernel --backend native --emit-asm -o $@ src/main.bastio

# build/kernel.backend is defined with the user programs' rules (the kernel and
# the user programs are built the same way).

ifeq ($(KBACKEND),native)
build/kernel.o: build/kernel.s build/kernel.backend
	$(CC) -c -o $@ $<
else
build/kernel.o: build/kernel.c build/kernel.backend
	$(CC) $(CFLAGS) -c -o $@ $<
endif

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

# A bootable image: Limine (BSD-2-Clause; BIOS and UEFI, as a hybrid ISO
# that can also be written to a USB stick) loading the same Multiboot
# kernel. Limine is not part of this repository: its binary release is
# fetched at a pinned tag, and its small installer (limine.c, which writes
# the BIOS boot code into the image) is built here. Needs xorriso.
LIMINE_GIT ?= https://github.com/limine-bootloader/limine.git
LIMINE_TAG ?= v11.4.1-binary

build/limine/limine: | build
	rm -rf build/limine
	git -c advice.detachedHead=false clone -q --depth 1 --branch $(LIMINE_TAG) $(LIMINE_GIT) build/limine
	$(MAKE) -C build/limine -s CC=$(CC)

build/bastios.iso: build/bastios.elf boot/limine.conf build/limine/limine
	rm -rf build/iso
	mkdir -p build/iso/boot/limine build/iso/EFI/BOOT
	cp build/bastios.elf build/iso/boot/
	cp boot/limine.conf build/limine/limine-bios.sys build/limine/limine-bios-cd.bin \
	  build/limine/limine-uefi-cd.bin build/limine/LICENSE build/iso/boot/limine/
	cp build/limine/BOOTX64.EFI build/iso/EFI/BOOT/
	xorriso -as mkisofs -quiet -R -r -J -b boot/limine/limine-bios-cd.bin \
	  -no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus -apm-block-size 2048 \
	  --efi-boot boot/limine/limine-uefi-cd.bin -efi-boot-part --efi-boot-image \
	  --protective-msdos-label build/iso -o $@ 2>/dev/null
	build/limine/limine bios-install --quiet $@

iso: build/bastios.iso

# A USB stick image: the ISO, then a GPT partition of USB_MIB MiB for the
# file system (tools/mkusb.py). Booted from, the stick is also the disk,
# and the machine's own disks are left alone. Limine's installer turns the
# ISO's GPT into an MBR (better for CDs and old firmware), but the stick
# needs its GPT, so the stick starts from the same files made into an ISO
# with a GPT and no Apple map, gets a BIOS boot partition for Limine's BIOS
# stage, and Limine is installed into it with the GPT kept.
USB_MIB ?= 64
build/bastios-gpt.iso: build/bastios.iso
	xorriso -as mkisofs -quiet -R -r -J -b boot/limine/limine-bios-cd.bin \
	  -no-emul-boot -boot-load-size 4 -boot-info-table \
	  --efi-boot boot/limine/limine-uefi-cd.bin -efi-boot-part --efi-boot-image \
	  --protective-msdos-label build/iso -o $@ 2>/dev/null

build/bastios-usb.img: build/bastios-gpt.iso tools/mkusb.py
	tools/mkusb.py $@ --iso build/bastios-gpt.iso --mib $(USB_MIB) --bios-boot
	build/limine/limine bios-install --quiet --no-gpt-to-mbr-isohybrid-conversion $@

usb: build/bastios-usb.img

# The disk: a raw image, here as a virtio block device. BastiOS formats it
# the first time it boots, and the files under disk/ live there across
# boots.
DISK    ?= build/disk.img
SMP     ?= 2
OVMF    ?= /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS ?= /usr/share/OVMF/OVMF_VARS_4M.fd
# A virtio disk and network card (QEMU's user network: DHCP, and the host
# reachable at 10.0.2.2). TCP ports on this machine reach the guest's:
# 7001 for `tcpecho 7001`, SSH_PORT its 22 and HTTP_PORT its 80, for an SSH
# server and a web server (docs/using.md). QEMU's default CPU model lacks 1 GiB
# pages, which boot.S needs to map the device window above 4 GiB (where
# UEFI firmware puts 64-bit PCI BARs); every real x86-64 CPU of the last
# decade has them.
SSH_PORT  ?= 2222
HTTP_PORT ?= 8080
QEMU_DEVS := -cpu qemu64,+pdpe1gb -m 256M -smp $(SMP) -no-reboot \
             -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
             -drive file=$(DISK),format=raw,if=none,id=vd -device virtio-blk-pci,drive=vd \
             -netdev user,id=n0,hostfwd=tcp:127.0.0.1:7001-:7001,hostfwd=tcp:127.0.0.1:$(SSH_PORT)-:22,hostfwd=tcp:127.0.0.1:$(HTTP_PORT)-:80 \
             -device virtio-net-pci,netdev=n0
QEMU_ARGS := -kernel build/bastios.elf -display none $(QEMU_DEVS)
# The ISO targets open QEMU's window, for the screen and keyboard
# (ISO_DISPLAY="-display none" for serial only).
ISO_DISPLAY ?=

$(DISK): | build
	truncate -s 33M $@

# A disk holding files from this machine, which then show up under disk/
# (in ash: /disk/NAME). FILES lists them: PATH stores PATH under its base
# name, NAME=PATH under NAME. This replaces $(DISK) and what was on it.
#   make disk FILES="hi greet=build/user/greet.elf"
disk: | build
	test/compat/mkdisk.py $(DISK) $(FILES)

# QEMU exits with 33 when the kernel powers off through isa-debug-exit
# (`halt`), which is a clean stop, not a failure.
CLEAN_EXIT := ; s=$$?; [ $$s -eq 0 ] || [ $$s -eq 33 ]

# On this terminal; Ctrl-A X quits.
run: build/bastios.elf $(DISK)
	$(QEMU) $(QEMU_ARGS) -serial mon:stdio $(CLEAN_EXIT)

# The ISO through Limine, under BIOS and under UEFI (OVMF firmware).
run-iso: build/bastios.iso $(DISK)
	$(QEMU) -cdrom build/bastios.iso $(ISO_DISPLAY) $(QEMU_DEVS) -serial mon:stdio $(CLEAN_EXIT)

# From the USB stick image, on an xHCI controller, with a USB keyboard.
run-usb: build/bastios-usb.img
	$(QEMU) -drive file=build/bastios-usb.img,format=raw,if=none,id=st -device qemu-xhci,id=xh \
	  -device usb-storage,bus=xh.0,drive=st,bootindex=0 -device usb-kbd,bus=xh.0 \
	  $(ISO_DISPLAY) -cpu qemu64,+pdpe1gb -m 256M -smp $(SMP) -no-reboot \
	  -device isa-debug-exit,iobase=0xf4,iosize=0x04 -nic user,model=virtio-net-pci \
	  -serial mon:stdio $(CLEAN_EXIT)

run-uefi: build/bastios.iso $(DISK)
	cp $(OVMF_VARS) build/ovmf_vars.fd
	$(QEMU) -drive if=pflash,format=raw,readonly=on,file=$(OVMF) \
	  -drive if=pflash,format=raw,file=build/ovmf_vars.fd \
	  -cdrom build/bastios.iso $(ISO_DISPLAY) $(QEMU_DEVS) -serial mon:stdio $(CLEAN_EXIT)

# The tests are listed in test/all.sh, which runs JOBS of them at a time.
test: $(if $(filter d,$(LANES)),build/bastioc1,build/bastios.elf)
	BASTIOC=$(BASTIOC) BASTIOC0=$(BASTIOC0) BASTIOLIB=$(BASTIOLIB) MUSLCC=$(MUSLCC) test/all.sh

# The long-run soak (test/soak.py): rounds of a mixed workload for SECS
# seconds, looking for leaks, hangs and panics. `make soak SECS=3600`;
# `make soak ALPINE=build/compat/soak-alpine.img` runs gcc builds in Alpine
# instead (see test/soak.py).
SECS ?= 600
soak: build/bastios.elf
	MUSLCC=$(MUSLCC) test/soak.py --secs $(SECS) $(if $(ALPINE),--alpine $(ALPINE))

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

$(filter-out build/compat/t_nopie,$(COMPAT_BINS)): build/compat/%: test/compat/progs/%.c test/compat/progs/compat.h | build/compat
	$(MUSLCC) -static-pie -O2 -Wall -Wno-unused-result -o $@ $<

# Linked at a fixed address (musl's default for -static -no-pie is 0x400000).
build/compat/t_nopie: test/compat/progs/t_nopie.c test/compat/progs/compat.h | build/compat
	$(MUSLCC) -static -no-pie -O2 -Wall -Wno-unused-result -o $@ $<

build/compat:
	mkdir -p $@

# Copy the tree only after build/user/ash.elf has finished configuring and
# building it in place: under make -j the copy otherwise races that build.
build/compat/busybox: build/busybox/Makefile test/compat/busybox.config | build/compat build/user/ash.elf
	rm -rf build/compat/bb && cp -r build/busybox build/compat/bb
	$(MAKE) -C build/compat/bb -s clean >/dev/null
	$(MAKE) -C build/compat/bb -s allnoconfig >/dev/null
	for o in $$(grep -v '^#' test/compat/busybox.config); do \
	  sed -i "s/^# $$o is not set/$$o=y/" build/compat/bb/.config; done
	yes "" | $(MAKE) -C build/compat/bb -s oldconfig >/dev/null
	# `ip` needs the kernel's headers (linux/rtnetlink.h), which musl has
	# not: the host's, as for LTP below.
	rm -rf build/compat/khdrs && mkdir -p build/compat/khdrs
	ln -s $(LTP_HDRS)/linux $(LTP_HDRS)/asm-generic build/compat/khdrs/
	ln -s $$(dirname $$(find $(LTP_HDRS) -path '*/asm/unistd_64.h' | head -1)) build/compat/khdrs/asm
	$(MAKE) -C build/compat/bb -s CC=$(MUSLCC) EXTRA_CFLAGS="-fPIE -isystem $(CURDIR)/build/compat/khdrs" CFLAGS_busybox=-static-pie busybox
	cp build/compat/bb/busybox $@

# Real programs, built from their own sources against musl as static PIEs
# and checked by test/compat/apps.txt: Lua, QuickJS, the sqlite3 shell, MicroPython,
# kilo, git and Perl.
# sqlite3's sources come from its GitHub mirror, which has no amalgamation:
# configuring and generating it needs tclsh (the tcl package).
LUA_GIT       ?= https://github.com/lua/lua
LUA_TAG       ?= v5.4.7
QUICKJS_GIT   ?= https://github.com/bellard/quickjs
QUICKJS_REV   ?= 535a7c250ff4a577ec36c3e103daab6dadeea650
SQLITE_GIT    ?= https://github.com/sqlite/sqlite
SQLITE_TAG    ?= version-3.52.0
MPY_GIT       ?= https://github.com/micropython/micropython
MPY_TAG       ?= v1.24.1
KILO_GIT      ?= https://github.com/antirez/kilo
KILO_REV      ?= 323d93b29bd89a2cb446de90c4ed4fea1764176e
ZLIB_GIT      ?= https://github.com/madler/zlib
ZLIB_TAG      ?= v1.3.1
GIT_GIT       ?= https://github.com/git/git
GIT_TAG       ?= v2.47.0
PERL_GIT      ?= https://github.com/Perl/perl5
PERL_TAG      ?= v5.40.0
CPYTHON_GIT   ?= https://github.com/python/cpython
CPYTHON_TAG   ?= v3.12.7
RIPGREP_VER   ?= 14.1.1
RUNC_VER      ?= 1.2.6
RUNC_SHA256   ?= 0774f49d1b1eebb5849e644db5e4dc6f2b06cee05f13b3d17d5d6ba62d6f2ebc
CRUN_VER      ?= 1.19.1
CRUN_SHA256   ?= eaa2408dee92432d142d58317127b5974d39a8ad48689ba8b2eff8f74614f4b8
DROPBEAR_GIT  ?= https://github.com/mkj/dropbear
DROPBEAR_TAG  ?= DROPBEAR_2024.86
COMPAT_APPS   := build/compat/lua build/compat/qjs build/compat/sqlite3 build/compat/micropython build/compat/kilo \
                 build/compat/git build/compat/perl build/compat/python build/compat/ttyepoll \
                 build/compat/dropbear build/compat/ssh_host_key build/compat/ssh_authorized_keys build/compat/rg
APP_CFLAGS    := -static-pie -fPIE -O2 -w -s

build/compat/src:
	mkdir -p $@

# ripgrep (what Neovim's pickers and :grep use), a static musl build from
# crates.io, pinned by version and Cargo.lock (--locked). Needs cargo, the
# musl target (rustup adds it) and musl-gcc.
build/compat/rg: | build/compat/src
	command -v rustup >/dev/null && rustup target add x86_64-unknown-linux-musl >/dev/null 2>&1 || true
	CARGO_TARGET_X86_64_UNKNOWN_LINUX_MUSL_LINKER=$(MUSLCC) cargo install ripgrep --version $(RIPGREP_VER) --locked \
	  --target x86_64-unknown-linux-musl --root build/compat/src/rg-root --quiet
	strip -o $@ build/compat/src/rg-root/bin/rg

# runc, the OCI container runtime, for the container cases in alpine.txt:
# the static amd64 binary of the upstream release, checked against a pinned
# SHA-256. It runs on BastiOS only; nothing here links against it.
build/compat/runc: | build/compat/src
	curl -fsSL -o $@.part https://github.com/opencontainers/runc/releases/download/v$(RUNC_VER)/runc.amd64
	echo "$(RUNC_SHA256)  $@.part" | sha256sum -c - >/dev/null
	chmod +x $@.part && mv $@.part $@

# crun, the other common OCI runtime (written in C), for the cases that run
# the same containers with it: the static amd64 build of the upstream
# release, checked against a pinned SHA-256. Like runc it runs on BastiOS
# only.
build/compat/crun: | build/compat/src
	curl -fsSL -o $@.part https://github.com/containers/crun/releases/download/$(CRUN_VER)/crun-$(CRUN_VER)-linux-amd64
	echo "$(CRUN_SHA256)  $@.part" | sha256sum -c - >/dev/null
	chmod +x $@.part && mv $@.part $@

# The tree-sitter C parser for Neovim: Alpine's neovim package ships only the
# vimdoc parser, so the grammar's generated parser.c, from the npm package at a
# pinned version and SHA-256, is built into a shared object (nvim loads
# parser/c.so; the C queries come with neovim). It runs on BastiOS only.
# (The version is in test/compat/images.mk, which arch/aarch64/Makefile shares.)
build/compat/tree-sitter-c.so: | build/compat/src
	curl -fsSL -o build/compat/src/tree-sitter-c.tgz https://registry.npmjs.org/tree-sitter-c/-/tree-sitter-c-$(TREESITTER_C_VER).tgz
	echo "$(TREESITTER_C_SHA256)  build/compat/src/tree-sitter-c.tgz" | sha256sum -c - >/dev/null
	rm -rf build/compat/src/tree-sitter-c && mkdir build/compat/src/tree-sitter-c
	tar xzf build/compat/src/tree-sitter-c.tgz -C build/compat/src/tree-sitter-c --strip-components=1 package/src
	$(MUSLCC) -shared -fPIC -O2 -Wl,-s -I build/compat/src/tree-sitter-c/src -o $@.part build/compat/src/tree-sitter-c/src/parser.c
	mv $@.part $@

# python-lsp-server and the pure-Python packages it needs, as a tar file for
# the case that runs it under Alpine's python3 with Neovim's LSP client. The
# wheels come from PyPI, each pinned by SHA-256 in test/compat/pylsp.py, and are
# unpacked without pip. x86-64 only.
build/compat/pylsp.tar: test/compat/pylsp.py | build/compat/src
	test/compat/pylsp.py $@ build/compat/src/pylsp-wheels

# strace, for the cases that trace a program on the Alpine disk (ptrace). Alpine's
# package is not on the disk's repository, so this is the layer of the stagex/strace
# image on Docker Hub (musl, linked to Alpine's libc), pinned by the layer's SHA-256.
STRACE_LAYER := sha256:1380bf046ab307e9340c2273fdcd2b1f224d019544a8db326d5776eeac5c5525
build/compat/strace: | build/compat/src
	tok=$$(curl -fsSL "https://auth.docker.io/token?service=registry.docker.io&scope=repository:stagex/strace:pull" | jq -r .token) && \
	  curl -fsSL -H "Authorization: Bearer $$tok" -o build/compat/src/strace.tgz https://registry-1.docker.io/v2/stagex/strace/blobs/$(STRACE_LAYER)
	echo "$(patsubst sha256:%,%,$(STRACE_LAYER))  build/compat/src/strace.tgz" | sha256sum -c - >/dev/null
	rm -rf build/compat/src/strace && mkdir build/compat/src/strace
	tar xzf build/compat/src/strace.tgz -C build/compat/src/strace usr/bin/strace
	strip -o $@.part build/compat/src/strace/usr/bin/strace
	mv $@.part $@

# gdb, for the cases that debug a program on the Alpine disk (ptrace, breakpoints).
# Alpine's package is not on the disk's repository, so this is the two layers of
# the bellsoft/alpaquita-linux-gdb:16.3-musl image on Docker Hub (musl, amd64),
# pinned by SHA-256. gdb links against more libraries than the disk's root has
# (Python 3.14 among them) and Alpaquita's musl, which runs the ifunc relocations
# some of them use (Alpine's does not), so everything it needs goes in one tar,
# /disk/gdb.tar, to unpack under the root's /opt: gdb/bin/gdb, gdb/lib (the
# libraries, Alpaquita's loader, and Python's standard library) and gdb/share/gdb.
# gdb runs through that loader.
GDB_REPO := bellsoft/alpaquita-linux-gdb
GDB_LAYERS := sha256:0b9b7727f90001f8d1eaeff1db88899575254b37875d1ab6fd10af2703ef09cb sha256:a0a525c37b15c2d2ed38a49cdcfb5732d0f58624520f6175518770db3dbecdb8
GDB_LIBS := libexpat.so.1 liblzma.so.5 libreadline.so.8 libz.so.1 libzstd.so.1 libncursesw.so.6 libpython3.14.so.1.0 libmpfr.so.6 libgmp.so.10 libdebuginfod.so.1 libstdc++.so.6 libgcc_s.so.1 libcurl.so.4 libelf.so.1 libfts.so.0 libjson-c.so.5 libbrotlicommon.so.1 libbrotlidec.so.1 libcares.so.2 libidn2.so.0 libnghttp2.so.14 libpsl.so.5 libssl.so.3 libcrypto.so.3 libunistring.so.5
build/compat/gdb.tar: | build/compat/src
	rm -rf build/compat/src/gdb && mkdir -p build/compat/src/gdb/all build/compat/src/gdb/gdb/bin build/compat/src/gdb/gdb/lib
	tok=$$(curl -fsSL "https://auth.docker.io/token?service=registry.docker.io&scope=repository:$(GDB_REPO):pull" | jq -r .token) && \
	for d in $(GDB_LAYERS); do \
	  curl -fsSL -H "Authorization: Bearer $$tok" -o build/compat/src/gdb/layer.tgz https://registry-1.docker.io/v2/$(GDB_REPO)/blobs/$$d && \
	  echo "$${d#sha256:}  build/compat/src/gdb/layer.tgz" | sha256sum -c - >/dev/null && \
	  tar xzf build/compat/src/gdb/layer.tgz -C build/compat/src/gdb/all --exclude='dev/*' --no-same-owner || exit 1; \
	done
	cd build/compat/src/gdb && cp all/usr/bin/gdb gdb/bin/gdb && cp all/lib/ld-musl-x86_64.so.1 gdb/lib/ld-musl-x86_64.so.1 && ln -s ld-musl-x86_64.so.1 gdb/lib/libc.musl-x86_64.so.1 && \
	  for l in $(GDB_LIBS); do cp -L all/usr/lib/$$l gdb/lib/$$l || exit 1; done && \
	  cp -r all/usr/lib/python3.14 gdb/lib/python3.14 && cp -r all/usr/share/gdb gdb/share_gdb && mkdir gdb/share && mv gdb/share_gdb gdb/share/gdb && \
	  rm -rf gdb/lib/python3.14/test gdb/lib/python3.14/idlelib gdb/lib/python3.14/tkinter gdb/lib/python3.14/turtledemo gdb/lib/python3.14/ensurepip gdb/lib/python3.14/pydoc_data gdb/lib/python3.14/lib2to3 && \
	  find gdb/lib/python3.14 -name __pycache__ -prune -exec rm -rf {} + && \
	  strip gdb/bin/gdb gdb/lib/*.so* 2>/dev/null; true
	tar cf $@.part -C build/compat/src/gdb gdb
	mv $@.part $@

# Dropbear, the SSH server (and its client and key tool, which also run on
# the host: ssh.txt logs in from here). The keys are made for the test:
# the server's host key and a client key whose public half goes in
# root's authorized_keys on the disk.
build/compat/dropbear: | build/compat/src
	rm -rf build/compat/src/dropbear
	git clone -q --depth 1 --branch $(DROPBEAR_TAG) $(DROPBEAR_GIT) build/compat/src/dropbear
	cd build/compat/src/dropbear && CC=$(MUSLCC) CFLAGS="-O2 -fPIE" LDFLAGS="-static-pie -s" ./configure -q \
	  --disable-zlib --disable-lastlog --disable-utmp --disable-utmpx --disable-wtmp --disable-wtmpx \
	  --disable-pututline --disable-pututxline --disable-syslog >/dev/null
	$(MAKE) -C build/compat/src/dropbear -s PROGRAMS="dropbear dbclient dropbearkey" >/dev/null
	cp build/compat/src/dropbear/dbclient build/compat/src/dropbear/dropbearkey build/compat/
	cp build/compat/src/dropbear/dropbear $@

build/compat/ssh_host_key build/compat/ssh_authorized_keys &: build/compat/dropbear
	rm -f build/compat/ssh_host_key build/compat/ssh_id
	build/compat/dropbearkey -t ed25519 -f build/compat/ssh_host_key >/dev/null
	build/compat/dropbearkey -t ed25519 -f build/compat/ssh_id | grep '^ssh-' > build/compat/ssh_authorized_keys

build/compat/lua: | build/compat/src
	rm -rf build/compat/src/lua
	git clone -q --depth 1 --branch $(LUA_TAG) $(LUA_GIT) build/compat/src/lua
	$(MUSLCC) $(APP_CFLAGS) -DLUA_USE_LINUX -DMAKE_LUA -o $@ build/compat/src/lua/onelua.c -lm

build/compat/qjs: | build/compat/src
	rm -rf build/compat/src/quickjs && mkdir build/compat/src/quickjs
	git -C build/compat/src/quickjs init -q
	git -C build/compat/src/quickjs fetch -q --depth 1 $(QUICKJS_GIT) $(QUICKJS_REV)
	git -C build/compat/src/quickjs checkout -q FETCH_HEAD
	$(MAKE) -C build/compat/src/quickjs -s CC=$(MUSLCC) HOST_CC=$(CC) CONFIG_LTO= qjs >/dev/null
	cd build/compat/src/quickjs && $(MUSLCC) -static-pie -s -o ../../qjs .obj/qjs.o .obj/repl.o .obj/quickjs.o \
	  .obj/dtoa.o .obj/libregexp.o .obj/libunicode.o .obj/cutils.o .obj/quickjs-libc.o -lm

build/compat/sqlite3: | build/compat/src
	rm -rf build/compat/src/sqlite
	git clone -q --depth 1 --branch $(SQLITE_TAG) $(SQLITE_GIT) build/compat/src/sqlite
	cd build/compat/src/sqlite && ./configure --disable-tcl >/dev/null && $(MAKE) -s sqlite3.c shell.c >/dev/null
	$(MUSLCC) $(APP_CFLAGS) -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -o $@ \
	  build/compat/src/sqlite/shell.c build/compat/src/sqlite/sqlite3.c -lm

# MicroPython's unix port, without the modules that need libraries musl
# does not bring (ffi, btree, ssl).
build/compat/micropython: | build/compat/src
	rm -rf build/compat/src/micropython
	git clone -q --depth 1 --branch $(MPY_TAG) $(MPY_GIT) build/compat/src/micropython
	git -C build/compat/src/micropython submodule -q update --init --depth 1 lib/micropython-lib
	$(MAKE) -C build/compat/src/micropython/mpy-cross -s >/dev/null
	$(MAKE) -C build/compat/src/micropython/ports/unix -s CC=$(MUSLCC) CFLAGS_EXTRA=-fPIE LDFLAGS_EXTRA=-static-pie \
	  MICROPY_PY_FFI=0 MICROPY_PY_BTREE=0 MICROPY_PY_SSL=0 MICROPY_SSL_AXTLS=0 >/dev/null
	cp build/compat/src/micropython/ports/unix/build-standard/micropython $@

# A program that waits for keys in epoll_wait, as libuv does (test/compat/tty.txt).
build/compat/ttyepoll: test/compat/progs/ttyepoll.c | build/compat
	$(MUSLCC) $(APP_CFLAGS) -o $@ $<

# kilo, a small editor that puts the terminal in raw mode (test/compat/tty.txt).
build/compat/kilo: | build/compat/src
	rm -rf build/compat/src/kilo && mkdir build/compat/src/kilo
	git -C build/compat/src/kilo init -q
	git -C build/compat/src/kilo fetch -q --depth 1 $(KILO_GIT) $(KILO_REV)
	git -C build/compat/src/kilo checkout -q FETCH_HEAD
	$(MUSLCC) $(APP_CFLAGS) -o $@ build/compat/src/kilo/kilo.c

# git, with zlib and nothing else (no curl, OpenSSL, expat, gettext or
# iconv). It looks for its own subcommands in /disk, where it is.
build/compat/git: | build/compat/src
	rm -rf build/compat/src/zlib build/compat/src/zlib-out build/compat/src/git
	git clone -q --depth 1 --branch $(ZLIB_TAG) $(ZLIB_GIT) build/compat/src/zlib
	cd build/compat/src/zlib && CC=$(MUSLCC) CFLAGS="-O2 -fPIE" ./configure --static --prefix=$(CURDIR)/build/compat/src/zlib-out >/dev/null
	$(MAKE) -C build/compat/src/zlib -s install >/dev/null
	git clone -q --depth 1 --branch $(GIT_TAG) $(GIT_GIT) build/compat/src/git
	$(MAKE) -C build/compat/src/git -s CC=$(MUSLCC) CFLAGS="-O2 -fPIE -I$(CURDIR)/build/compat/src/zlib-out/include" \
	  LDFLAGS="-static-pie -s -L$(CURDIR)/build/compat/src/zlib-out/lib" NO_OPENSSL=1 NO_CURL=1 NO_EXPAT=1 \
	  NO_GETTEXT=1 NO_TCLTK=1 NO_PERL=1 NO_PYTHON=1 NO_ICONV=1 NO_REGEX=NeedsStartEnd \
	  prefix=/disk gitexecdir=/disk template_dir=/disk/git-templates git >/dev/null
	cp build/compat/src/git/git $@

# perl, statically linked with its XS modules, and the pure-Perl modules
# test/compat/apps.txt uses (with what they load) in build/compat/perllib,
# which goes on the disk as /disk/perllib.
PERL_MODULES  := strict warnings Carp Exporter Data::Dumper JSON::PP Digest::MD5 Digest::SHA POSIX List::Util \
                 Scalar::Util Fcntl Socket IO::Handle IO::Socket::INET HTTP::Tiny Time::HiRes File::Spec \
                 File::Temp File::Path File::Basename Cwd Getopt::Long Storable Encode
build/compat/perl: | build/compat/src
	rm -rf build/compat/src/perl5 build/compat/perllib
	git clone -q --depth 1 --branch $(PERL_TAG) $(PERL_GIT) build/compat/src/perl5
	cd build/compat/src/perl5 && sh Configure -des -Dcc=$(MUSLCC) -Doptimize=-O2 -Dccflags=-fPIE -Dldflags="-static-pie -s" \
	  -Dlibs=-lm -Dlibc=$(MUSL_LIB)/libc.a -Dlibpth=$(MUSL_LIB) -Dlocincpth= -Dloclibpth= -Uusedl -Uusethreads \
	  -Dprefix=/disk -Dprivlib=/disk/perllib -Darchlib=/disk/perllib -Dsitelib=/disk/perllib -Dsitearch=/disk/perllib \
	  -Dman1dir=none -Dman3dir=none -Dnoextensions="DB_File GDBM_File NDBM_File ODBM_File" >/dev/null
	$(MAKE) -C build/compat/src/perl5 -s perl >/dev/null
	cd build/compat/src/perl5 && ./perl -Ilib -e 'for (@ARGV) { eval "require $$_; 1" or die $$@ } print "$$_\n" for values %INC' \
	  $(PERL_MODULES) | sed -n 's|^lib/||p' | while read f; do \
	  mkdir -p $(CURDIR)/build/compat/perllib/$$(dirname $$f) && cp lib/$$f $(CURDIR)/build/compat/perllib/$$f; done
	cp build/compat/src/perl5/perl $@

# CPython, statically linked with its C modules (zlib from source; no
# ctypes, sqlite3, readline, lzma, bz2, ssl: their libraries are not built
# for musl here), and its standard library in one zip, which goes on the
# disk as /disk/py/lib/python312.zip: run it with PYTHONHOME=/disk/py.
# configure compares the compiler's multiarch with its triplet, which
# musl-gcc on a glibc host gets wrong, hence the wrapper.
build/compat/python: test/compat/pyzip.py | build/compat/src
	rm -rf build/compat/src/pyzlib build/compat/src/pyzlib-out build/compat/src/cpython build/compat/py
	git clone -q --depth 1 --branch $(ZLIB_TAG) $(ZLIB_GIT) build/compat/src/pyzlib
	cd build/compat/src/pyzlib && CC=$(MUSLCC) CFLAGS="-O2 -fPIE" ./configure --static --prefix=$(CURDIR)/build/compat/src/pyzlib-out >/dev/null
	$(MAKE) -C build/compat/src/pyzlib -s install >/dev/null
	git clone -q --depth 1 --branch $(CPYTHON_TAG) $(CPYTHON_GIT) build/compat/src/cpython
	printf '#!/bin/sh\nfor a; do [ "$$a" = --print-multiarch ] && { echo x86_64-linux-musl; exit 0; }; done\nexec $(MUSLCC) "$$@"\n' \
	  > build/compat/src/cpython/musl-cc && chmod +x build/compat/src/cpython/musl-cc
	cd build/compat/src/cpython && ZLIB_CFLAGS=-I$(CURDIR)/build/compat/src/pyzlib-out/include \
	  ZLIB_LIBS="-L$(CURDIR)/build/compat/src/pyzlib-out/lib -lz" PKG_CONFIG_LIBDIR=/nonexistent MODULE_BUILDTYPE=static \
	  py_cv_module__ctypes=n/a py_cv_module__ctypes_test=n/a py_cv_module_readline=n/a py_cv_module__uuid=n/a \
	  py_cv_module__lzma=n/a py_cv_module__bz2=n/a py_cv_module__sqlite3=n/a py_cv_module_xxlimited=n/a \
	  py_cv_module_xxlimited_35=n/a CC=$(CURDIR)/build/compat/src/cpython/musl-cc CFLAGS=-fPIE LDFLAGS=-static-pie \
	  ./configure --disable-shared --disable-test-modules --without-ensurepip --prefix=/disk/py >/dev/null
	$(MAKE) -C build/compat/src/cpython -s >/dev/null
	mkdir -p build/compat/py/lib
	cp build/compat/src/cpython/build/lib.*/_sysconfigdata_*.py build/compat/src/cpython/Lib/
	build/compat/src/cpython/python test/compat/pyzip.py build/compat/src/cpython/Lib build/compat/py/lib/python312.zip
	strip -o $@ build/compat/src/cpython/python

# Building C on BastiOS: tcc and GNU make on a second disk (test/compat/
# devdisk.sh) with a small C runtime under /disk/c, which tcc is configured
# to look in; test/compat/dev.txt compiles and runs programs with them.
# make's sources come from its GitHub mirror and gnulib's, and bootstrapping
# them needs autoconf, automake, autopoint and texinfo.
TCC_GIT       ?= https://github.com/TinyCC/tinycc
TCC_REV       ?= 43c7708b85681a2fd4451c8a541af4494a8919b2
GMAKE_GIT     ?= https://github.com/mirror/make
GMAKE_TAG     ?= 4.4.1
GNULIB_GIT    ?= https://github.com/coreutils/gnulib
GNULIB_TAG    ?= stable-202301
MUSL_INC      ?= /usr/include/x86_64-linux-musl
MUSL_LIB      ?= /usr/lib/x86_64-linux-musl
TCC_SRC       := build/compat/src/tcc

build/compat/tcc: test/compat/patches/tcc-auto-type.patch | build/compat/src
	rm -rf $(TCC_SRC) && mkdir $(TCC_SRC)
	git -C $(TCC_SRC) init -q
	git -C $(TCC_SRC) fetch -q --depth 1 $(TCC_GIT) $(TCC_REV)
	git -C $(TCC_SRC) checkout -q FETCH_HEAD
	git -C $(TCC_SRC) apply $(CURDIR)/test/compat/patches/tcc-auto-type.patch
	cd $(TCC_SRC) && ./configure --cc=$(MUSLCC) --config-musl --config-pie --extra-cflags="-O2 -fPIE" \
	  --extra-ldflags="-static-pie -s" --sysincludepaths=/disk/c/include --libpaths=/disk/c/lib \
	  --crtprefix=/disk/c/lib --tccdir=/disk/c/lib --elfinterp=/lib/ld-musl-x86_64.so.1 >/dev/null
	$(MAKE) -C $(TCC_SRC) -s tcc
	$(MAKE) -C $(TCC_SRC) -s libtcc1.a XFLAGS-unx="-B$(CURDIR)/$(TCC_SRC) -I$(CURDIR)/$(TCC_SRC)/include -I$(MUSL_INC)"
	cp $(TCC_SRC)/tcc $@

build/compat/make: | build/compat/src
	rm -rf build/compat/src/make build/compat/src/gnulib
	git clone -q --depth 1 --branch $(GMAKE_TAG) $(GMAKE_GIT) build/compat/src/make
	git clone -q --depth 1 --branch $(GNULIB_TAG) $(GNULIB_GIT) build/compat/src/gnulib
	cd build/compat/src/make && ./bootstrap --no-git --gnulib-srcdir=../gnulib --skip-po >/dev/null && \
	  CC=$(MUSLCC) CFLAGS="-O2 -fPIE" LDFLAGS="-static-pie -s" ./configure -q --disable-nls --without-guile
	$(MAKE) -C build/compat/src/make -s >/dev/null
	cp build/compat/src/make/make $@

build/compat/dev.img: build/compat/tcc build/compat/make test/compat/devdisk.sh test/compat/dev_headers.c test/compat/mkdisk.py
	test/compat/devdisk.sh $@ build/compat/tcc build/compat/make $(CURDIR)/$(TCC_SRC) $(MUSL_INC) $(MUSL_LIB)

# The Bastio compiler on BastiOS: bastioc1 (bastiolang's stage1, as C from
# bastioc0, built here with musl), Z3 to discharge its proofs, and tcc to
# build the C it writes, on a third disk (test/compat/bastiodisk.sh) for
# test/compat/bastio.txt. Z3 is built against glibc, statically: there is
# no C++ compiler for musl here. Its build takes a while, once.
BASTIOLANG    ?= $(patsubst %/stage0/target/release/bastioc0,%,$(BASTIOC0))
Z3_GIT        ?= https://github.com/Z3Prover/z3
Z3_TAG        ?= z3-4.8.12
Z3_JOBS       ?= 4

build/compat/z3: | build/compat/src
	rm -rf build/compat/src/z3
	git clone -q --depth 1 --branch $(Z3_TAG) $(Z3_GIT) build/compat/src/z3
	cd build/compat/src/z3 && CXX=g++ CC=gcc CXXFLAGS=-fPIE LDFLAGS=-static-pie \
	  python3 scripts/mk_make.py --single-threaded >/dev/null
	$(MAKE) -C build/compat/src/z3/build -s -j$(Z3_JOBS) z3 >/dev/null
	strip -o $@ build/compat/src/z3/build/z3

build/compat/bastioc1.c: $(BASTIOC0) $(wildcard $(BASTIOLANG)/stage1/*.bastio) | build/compat
	$(BASTIOC0) --emit-c -o $@ $(BASTIOLANG)/stage1/bastioc1.bastio

build/compat/bastioc1: build/compat/bastioc1.c
	$(MUSLCC) $(APP_CFLAGS) -o $@ $<

build/compat/bastio.img: build/compat/bastioc1 build/compat/z3 build/compat/tcc test/compat/bastiodisk.sh \
    $(wildcard test/compat/bastio/*.bastio) test/compat/mkdisk.py
	test/compat/bastiodisk.sh $@ build/compat/bastioc1 build/compat/bastioc1.c build/compat/z3 build/compat/tcc \
	  $(CURDIR)/$(TCC_SRC) $(MUSL_INC) $(MUSL_LIB) $(BASTIOLANG)/stage1

# Alpine Linux on BastiOS, on a fourth disk (test/compat/alpinedisk.py) for
# test/compat/alpine.txt: Alpine's root file system, which the checks enter
# with chroot, and local repositories apk installs packages from. They come
# from Docker Hub images, pinned by digest: the minirootfs, and images that
# apk installed packages into, which alpinedisk.py packs again. All their
# packages are served over HTTP; ALPINE_DISK_PKGS, with what they depend
# on, also go on the disk, so the checks that use them need no network.
# The images: git and curl; Alpine's own CI image (alpine-sdk: gcc, g++,
# make, binutils, musl-dev); python3 (the ansible image); jq and libxml2's
# tools; neovim (serh/nvim: Alpine 3.23's neovim 0.11 with git, gcc and make); tmux; cmake and ninja; lua-language-server, a real language server for neovim's LSP client; and vim from edge (its runtime is about 2100 files: it waits for a
# disk directory bigger than 1024 entries, as gcc and python3 do).
# (The pinned images are in test/compat/images.mk.)
# Where the images' layers are kept, checked against their digests on each
# use. Point it outside build/ to keep them across fresh checkouts: Docker
# Hub rate-limits anonymous pulls, and only what is missing is fetched.
ALPINE_CACHE ?= build/compat/alpine
# The base image's layer list in the cache: the web server serves it as a registry (tools/ocipull.sh).
ALPINE_BASE_DIGEST = $(word 2,$(subst @sha256:, ,$(ALPINE_BASE)))

# The Bastio toolchain goes on this disk too (outside the Alpine root, like
# the bastio disk's), for the cases that edit a program in Alpine's neovim
# and build it with bastioc1 and tcc, and Dropbear, to log in and run it
# over SSH.
build/compat/alpine.img: test/compat/alpinedisk.py test/compat/mkdisk.py test/compat/bastiotools.sh \
    build/compat/bastioc1 build/compat/z3 build/compat/tcc \
    build/compat/dropbear build/compat/ssh_host_key build/compat/ssh_authorized_keys build/compat/runc build/compat/crun build/compat/tree-sitter-c.so build/compat/strace build/compat/gdb.tar build/compat/pylsp.tar tools/ocipull.sh | build/compat
	test/compat/alpinedisk.py $@ build/compat/alpine $(ALPINE_BASE) $(ALPINE_PKGS) $(ALPINE_X86_PKGS) --disk $(ALPINE_DISK_PKGS) --cache $(ALPINE_CACHE) \
	  --extra "$$(test/compat/bastiotools.sh build/compat/bastioc1 build/compat/bastioc1.c build/compat/z3 build/compat/tcc $(CURDIR)/$(TCC_SRC) $(MUSL_INC) $(MUSL_LIB)) \
	  build/compat/dropbear build/compat/ssh_host_key build/compat/ssh_authorized_keys build/compat/runc build/compat/crun build/compat/tree-sitter-c.so build/compat/strace build/compat/gdb.tar build/compat/pylsp.tar tools/ocipull.sh"

# Debian on BastiOS, on a fifth disk (test/compat/debiandisk.py) for
# test/compat/debian.txt: Debian's root file system, whose programs are
# linked against glibc, entered with chroot. From the debian:stable-slim
# image on Docker Hub, pinned by digest, with its layer kept in
# ALPINE_CACHE like the Alpine images'; the BusyBox is Alpine's.

build/compat/debian.img: test/compat/debiandisk.py test/compat/alpinedisk.py test/compat/mkdisk.py build/compat/alpine.img
	test/compat/debiandisk.py $@ $(DEBIAN_IMAGE) build/compat/alpine/disk/busybox --cache $(ALPINE_CACHE)

build/compat/disk.img: $(COMPAT_BINS) build/compat/busybox $(COMPAT_APPS) test/compat/mkdisk.py
	test/compat/mkdisk.py $@ $(COMPAT_BINS) build/compat/busybox $(COMPAT_APPS) perllib=build/compat/perllib py=build/compat/py

# Node.js, on a sixth disk (test/compat/nodedisk.py) for test/compat/node.txt:
# the node binary of the official node:22-alpine image and the libraries it
# links against, entered with chroot. Pinned by digest, its layers kept in
# ALPINE_CACHE like the others'.

build/compat/node.img: test/compat/nodedisk.py test/compat/alpinedisk.py test/compat/mkdisk.py build/compat/alpine.img
	test/compat/nodedisk.py $@ $(NODE_IMAGE) build/compat/alpine/disk/busybox --cache $(ALPINE_CACHE)

# The Go toolchain of the official golang:alpine image (static programs), as a
# root under /disk/g, for go.txt. Pinned by digest, its layers kept in
# ALPINE_CACHE like the others'. x86-64 only.
build/compat/go.img: test/compat/godisk.py test/compat/alpinedisk.py test/compat/mkdisk.py build/compat/alpine.img
	test/compat/godisk.py $@ $(GO_IMAGE) build/compat/alpine/disk/busybox --cache $(ALPINE_CACHE)

# The JDK of the official eclipse-temurin alpine image, as a tar file the cases
# unpack, for java.txt. Pinned by digest, its layers kept in ALPINE_CACHE like
# the others'. x86-64 only.
build/compat/jdk.img: test/compat/jdkdisk.py test/compat/alpinedisk.py test/compat/mkdisk.py build/compat/alpine.img
	test/compat/jdkdisk.py $@ $(JDK_IMAGE) build/compat/alpine/disk/busybox --cache $(ALPINE_CACHE)

# The Rust toolchain of the official rust:alpine image (rustc, cargo, Alpine's
# gcc and musl), as a tar file the cases unpack, for rust.txt. Pinned by
# digest, its layers kept in ALPINE_CACHE like the others'. x86-64 only.
build/compat/rust.img: test/compat/rustdisk.py test/compat/alpinedisk.py test/compat/mkdisk.py build/compat/alpine.img
	test/compat/rustdisk.py $@ $(RUST_IMAGE) build/compat/alpine/disk/busybox --cache $(ALPINE_CACHE)

compat: build/bastios.elf build/compat/disk.img build/compat/dev.img build/compat/bastio.img build/compat/alpine.img build/compat/debian.img build/compat/node.img build/compat/go.img build/compat/jdk.img build/compat/rust.img
	test/compat/run.py --kernel build/bastios.elf --disk build/compat/disk.img --dev-disk build/compat/dev.img --bastio-disk build/compat/bastio.img --alpine-disk build/compat/alpine.img --alpine-repo build/compat/alpine/disk/http --registry-image $(ALPINE_CACHE)/sha256-$(ALPINE_BASE_DIGEST).json --debian-disk build/compat/debian.img --node-disk build/compat/node.img --go-disk build/compat/go.img --jdk-disk build/compat/jdk.img --rust-disk build/compat/rust.img --out build/compat $(COMPAT_PROGS)

# Records which checks pass now, so `make compat` catches regressions.
compat-baseline: build/bastios.elf build/compat/disk.img build/compat/dev.img build/compat/bastio.img build/compat/alpine.img build/compat/debian.img build/compat/node.img build/compat/go.img build/compat/jdk.img build/compat/rust.img
	test/compat/run.py --update-baseline --kernel build/bastios.elf --disk build/compat/disk.img --dev-disk build/compat/dev.img --bastio-disk build/compat/bastio.img --alpine-disk build/compat/alpine.img --alpine-repo build/compat/alpine/disk/http --registry-image $(ALPINE_CACHE)/sha256-$(ALPINE_BASE_DIGEST).json --debian-disk build/compat/debian.img --node-disk build/compat/node.img --go-disk build/compat/go.img --jdk-disk build/compat/jdk.img --rust-disk build/compat/rust.img --out build/compat $(COMPAT_PROGS)

# The same checks on this Linux machine: are the expectations right?
compat-host: $(COMPAT_BINS) build/compat/busybox $(COMPAT_APPS)
	test/compat/run.py --host build/compat --out build/compat/host $(COMPAT_PROGS)

# Power cuts on a disk with a write cache, at every point of two crashme
# runs (see test/crashsim.py; `make test` runs a short pass).
crashsim: build/bastios.elf
	test/crashsim.py

# The Linux Test Project's system call tests (test/compat/ltp.py). LTP
# (GPL-2.0-or-later, not part of this repository) is fetched at a pinned tag
# and built against musl as static PIEs; the three files LTP's own Alpine CI
# removes as unsupported on musl are removed here too. musl-gcc has no Linux
# headers of its own, so the system's are put on its include path.
LTP_GIT ?= https://github.com/linux-test-project/ltp
LTP_TAG ?= 20260930
LTP_HDRS ?= /usr/include

build/ltp/configure: | build
	rm -rf build/ltp
	git clone -q --depth 1 --branch $(LTP_TAG) $(LTP_GIT) build/ltp
	rm -f build/ltp/testcases/kernel/syscalls/fmtmsg/fmtmsg01.c \
	  build/ltp/testcases/kernel/syscalls/timer_create/timer_create01.c \
	  build/ltp/testcases/kernel/syscalls/timer_create/timer_create03.c
	# LTP images a 300 MB file for a test's block device; a RAM file holds
	# 64 MiB here (FILE_MAX, src/ramfs.bastio), so ask for 32 MB.
	sed -i 's/^#define DEV_SIZE_MB 300u$$/#define DEV_SIZE_MB 32u/' build/ltp/lib/tst_device.c
	$(MAKE) -C build/ltp -s autotools >/dev/null

build/ltp/.built: build/ltp/configure
	rm -rf build/ltp-hdrs && mkdir -p build/ltp-hdrs
	ln -s $(LTP_HDRS)/linux $(LTP_HDRS)/asm-generic build/ltp-hdrs/
	ln -s $$(dirname $$(find $(LTP_HDRS) -path '*/asm/unistd_64.h' | head -1)) build/ltp-hdrs/asm
	cd build/ltp && ./configure -q CC="$(MUSLCC) -isystem $(CURDIR)/build/ltp-hdrs" \
	  CFLAGS="-O2 -fPIE" LDFLAGS=-static-pie --without-tirpc >/dev/null
	$(MAKE) -C build/ltp/lib -s
	$(MAKE) -C build/ltp/libs -s
	$(MAKE) -C build/ltp/testcases/kernel/syscalls -s -k || true
	touch $@

# Not part of `make test`: a full run boots the kernel once per 63 programs
# (25 boots, three at a time; about 10 minutes on four processors). Fails
# only if a test in ltp_baseline.txt stops passing.
compat-ltp: build/bastios.elf build/ltp/.built
	test/compat/ltp.py --kernel build/bastios.elf --ltp build/ltp --out build/compat/ltp

compat-ltp-baseline: build/bastios.elf build/ltp/.built
	test/compat/ltp.py --update-baseline --kernel build/bastios.elf --ltp build/ltp --out build/compat/ltp

# Not part of `make test`: the same micro-benchmarks (bench/osbench.c) on
# BastiOS and on Linux in the same QEMU machine, side by side; see
# docs/performance.md. BENCH_ARGS passes options to bench/run.py
# (--os bastios, --runs 5, --tests syscall,fork, --smp 4).
bench: build/bastios.elf
	bench/run.py $(BENCH_ARGS)

# Not part of `make test`: whole-program workloads (bench/workloads.py), the
# same Alpine gcc, make, apk, tar and gzip on BastiOS and on Linux. The disk
# is the compat suite's Alpine disk cut down to gcc, make and musl-dev, plus
# the guest script and a generated C project (bench/workloads/).
BENCH_GCC_IMAGE ?= alpinelinux/alpine-gitlab-ci@sha256:4bce54b0b246967f6ab51476d5511245ba35a49f30b2e63102ca294ab488add9
build/bench/wl/proj.tar: bench/workloads/mkproj.py | build/bench/wl
	bench/workloads/mkproj.py $@
build/bench/wl/wl.sh: bench/workloads/wl.sh | build/bench/wl
	cp $< $@
build/bench/wl:
	mkdir -p $@
build/bench/alpine.img: test/compat/alpinedisk.py test/compat/mkdisk.py build/bench/wl/proj.tar build/bench/wl/wl.sh
	MKDISK_FREE_MB=700 test/compat/alpinedisk.py $@ build/bench/alpine $(ALPINE_BASE) $(BENCH_GCC_IMAGE) \
	  --disk gcc,musl-dev,make --cache $(ALPINE_CACHE) --extra "wl.sh=build/bench/wl/wl.sh proj.tar=build/bench/wl/proj.tar"
bench-workloads: build/bastios.elf build/bench/alpine.img
	bench/workloads.py $(WORKLOAD_ARGS)

# TCP over virtio-net and e1000 against Linux's, driven from the host (bench/netcmp.py).
bench-net: build/bastios.elf
	bench/netcmp.py $(NET_ARGS)

# Not part of `make test`: the change in each benchmark from bench/baseline.json
# (or COMPARE_TO=other.json) to the last `make bench` (build/bench/results.json).
# Exits 1 if a test got more than 10% slower; see bench/compare.py.
COMPARE_TO ?= bench/baseline.json
bench-compare:
	bench/compare.py $(COMPARE_TO) build/bench/results.json

# Not part of `make test`: prints timings, checks nothing (see the script).
smpbench: build/bastios.elf
	test/smpbench.sh

# Not part of `make test` either: an end-to-end number, `make -j1` and `make -j2`
# of a project of small C files built with tcc on the development disk.
buildbench: build/bastios.elf build/compat/dev.img
	test/buildbench.sh

# Not part of `make test` either: do one process's threads run in parallel?
threadbench: build/bastios.elf
	test/threadbench.sh

# Not part of `make test` either: how fast ash forks and execs, 4 at a time.
forkbench: build/bastios.elf
	test/forkbench.sh

# Not part of `make test` either: disk timings for each driver.
diskbench: build/bastios.elf
	test/diskbench.sh

iobench: build/bastios.elf
	test/iobench.sh

# Network timings on virtio-net and e1000.
netbench: build/bastios.elf
	test/netbench.sh

clean:
	rm -rf build

.PRECIOUS: build/user/%.c build/user/%.s build/user/%.o build/user/%.elf
FORCE:

.PHONY: FORCE all run disk usb run-usb test crashsim fuzz bench bench-workloads bench-net bench-compare smpbench buildbench threadbench forkbench diskbench iobench netbench compat compat-baseline compat-host compat-ltp compat-ltp-baseline clean

endif
