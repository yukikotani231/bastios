# BastiOS for AArch64 (QEMU virt), driven by `make ARCH=aarch64 <goal>` in
# the top-level Makefile.
#   make ARCH=aarch64          build build/aarch64/bastios.bin (and the .elf it comes from)
#   make ARCH=aarch64 run      boot it in QEMU on this terminal (Ctrl-A X to quit)
#   make ARCH=aarch64 test     boot it and drive the shell (test/aarch64_test.sh)
#
# The kernel is src/aarch64/main.bastio, proved by bastioc1 for
# `--target kernel --arch aarch64` and compiled with the AArch64 C
# compiler; boot/aarch64 is its platform (the trusted base besides the
# compiler's AArch64 preludes). The user programs are built the same way
# with `--target user --arch aarch64`. Needs z3, qemu-system-aarch64 and
# an AArch64 C compiler with binutils (Debian/Ubuntu: gcc-aarch64-linux-gnu
# qemu-system-arm; on an AArch64 machine plain gcc).
BASTIOC0 ?= ../bastiolang/stage0/target/release/bastioc0
BASTIOC  ?= build/bastioc1
BASTIOLIB ?= ../bastiolang
export BASTIO_PROOF_CACHE ?= build/proof-cache
QEMU ?= qemu-system-aarch64
ifeq ($(shell uname -m),aarch64)
CROSS ?=
else
CROSS ?= aarch64-linux-gnu-
endif
ACC := $(CROSS)gcc
ALD := $(CROSS)ld
AOBJCOPY := $(CROSS)objcopy

B := build/aarch64
BASE_CFLAGS := -std=c11 -O2 -ffreestanding -fno-builtin -fno-stack-protector -nostdlib \
               -mgeneral-regs-only -mno-outline-atomics -fno-pic -fno-pie
TCFLAGS := $(BASE_CFLAGS) -Wall -Wextra -Werror
# Generated C (proved Bastio) is compiled without warnings: it declares
# temporaries it may not use.
GCFLAGS := $(BASE_CFLAGS) -w

all: $(B)/bastios.elf $(B)/bastios.bin

build $(B) $(B)/user:
	mkdir -p $@

build/bastioc1.c: $(BASTIOC0) $(wildcard $(BASTIOLIB)/stage1/*.bastio) | build
	$(BASTIOC0) --emit-c -o $@ $(BASTIOLIB)/stage1/bastioc1.bastio

build/bastioc1: build/bastioc1.c
	cc -O2 -o $@ $<

# 1. User programs: prove each one, compile it as position-independent
#    code, link it at USER_BASE behind its header, and flatten it to a raw
#    image that the kernel embeds with include_bytes.
UPROGS := sh hello spin true
UBINS := $(UPROGS:%=$(B)/user/%.bin)

$(B)/user/%.c: user/%.bastio $(wildcard user/lib/*.bastio) $(BASTIOC) | $(B)/user
	$(BASTIOC) --lib $(BASTIOLIB) --target user --arch aarch64 -o $@ $<

$(B)/user/%.o: $(B)/user/%.c
	$(ACC) $(GCFLAGS) -fpie -c -o $@ $<

$(B)/user/start.o: user/start-aarch64.S | $(B)/user
	$(ACC) -c -o $@ $<

$(B)/user/libc.o: boot/libc.c | $(B)/user
	$(ACC) $(TCFLAGS) -fpie -c -o $@ $<

$(B)/user/%.elf: $(B)/user/start.o $(B)/user/%.o $(B)/user/libc.o user/user-aarch64.ld
	$(ALD) -nostdlib -static -z max-page-size=0x1000 -T user/user-aarch64.ld --build-id=none --no-warn-rwx-segments \
	  -o $@ $(B)/user/start.o $(B)/user/$*.o $(B)/user/libc.o

$(B)/user/%.bin: $(B)/user/%.elf
	$(AOBJCOPY) -O binary $< $@

# Linux arm64 programs: ordinary ELF executables, linked at the start of the
# user window with no libc (mhello is the table's name for the first).
LXELFS := $(B)/user/mhello.elf $(B)/user/mtest.elf $(B)/user/ash.elf

# BusyBox's ash (the same source and applets as x86-64's, user/busybox.config)
# as an ordinary static glibc executable at 0x400000, where Linux links one.
BUSYBOX_GIT ?= https://github.com/mirror/busybox
BUSYBOX_TAG ?= 1_36_1

$(B)/busybox/Makefile: | $(B)
	rm -rf $(B)/busybox
	git clone -q --depth 1 --branch $(BUSYBOX_TAG) $(BUSYBOX_GIT) $(B)/busybox

$(B)/user/ash.elf: $(B)/busybox/Makefile user/busybox.config | $(B)/user
	$(MAKE) -C $(B)/busybox -s allnoconfig >/dev/null
	for o in $$(grep -v '^#' user/busybox.config); do \
	  sed -i "s/^# $$o is not set/$$o=y/" $(B)/busybox/.config; done
	yes "" | $(MAKE) -C $(B)/busybox -s oldconfig >/dev/null
	sed -i -e 's/^CONFIG_FEATURE_DEFAULT_PASSWD_ALGO=.*/CONFIG_FEATURE_DEFAULT_PASSWD_ALGO="sha512"/' \
	  -e 's/^CONFIG_LAST_ID=.*/CONFIG_LAST_ID=60000/' -e 's/^CONFIG_FIRST_SYSTEM_ID=.*/CONFIG_FIRST_SYSTEM_ID=100/' \
	  -e 's/^CONFIG_LAST_SYSTEM_ID=.*/CONFIG_LAST_SYSTEM_ID=999/' $(B)/busybox/.config
	$(MAKE) -C $(B)/busybox -s CROSS_COMPILE=$(CROSS) \
	  CFLAGS_busybox="-static -Wl,-z,max-page-size=0x1000 -Wl,-Ttext-segment=0x400000 -Wl,--build-id=none" busybox
	cp $(B)/busybox/busybox $@

$(B)/user/mhello.elf: user/lxhello-aarch64.c | $(B)/user
	$(ACC) -O2 -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -no-pie -nostdlib -static \
	  -Wl,-z,max-page-size=0x1000 -Wl,-Ttext-segment=0x400000 -Wl,--build-id=none -Wl,--no-warn-rwx-segments \
	  -o $@ $<

# mtest is an ordinary glibc program, linked statically at the same address.
$(B)/user/mtest.elf: user/lxtest-aarch64.c | $(B)/user
	$(ACC) -O1 -fno-pic -no-pie -static -Wl,-z,max-page-size=0x1000 -Wl,-Ttext-segment=0x400000 \
	  -Wl,--build-id=none -Wl,--no-warn-rwx-segments -o $@ $<

# 2. Prove the kernel and translate it to freestanding C.
KSRCS := $(wildcard src/*.bastio) $(wildcard src/aarch64/*.bastio src/aarch64/arch/*.bastio) $(wildcard files/*)

$(B)/kernel.c: $(KSRCS) $(UBINS) $(LXELFS) $(BASTIOC) | $(B)
	$(BASTIOC) --lib $(BASTIOLIB) --lib src/aarch64 --lib src --target kernel --arch aarch64 -o $@ src/aarch64/main.bastio

$(B)/kernel.o: $(B)/kernel.c
	$(ACC) $(GCFLAGS) -c -o $@ $<

# The platform runs with the MMU off at first, where unaligned accesses fault.
$(B)/platform.o: boot/aarch64/platform.c | $(B)
	$(ACC) $(TCFLAGS) -mstrict-align -c -o $@ $<

$(B)/boot.o: boot/aarch64/boot.S | $(B)
	$(ACC) -c -o $@ $<

$(B)/libc.o: boot/libc.c | $(B)
	$(ACC) $(TCFLAGS) -mstrict-align -c -o $@ $<

$(B)/bastios.elf: $(B)/boot.o $(B)/platform.o $(B)/kernel.o $(B)/libc.o boot/aarch64/linker.ld
	$(ALD) -nostdlib -static --no-warn-rwx-segments -T boot/aarch64/linker.ld -o $@ \
	  $(B)/boot.o $(B)/kernel.o $(B)/platform.o $(B)/libc.o

# QEMU hands a device tree only to an Image, which is the ELF's contents with
# the arm64 header boot.S begins with.
$(B)/bastios.bin: $(B)/bastios.elf
	$(AOBJCOPY) -O binary $< $@

QEMU_ARGS := -M virt,gic-version=3 -cpu cortex-a72 -m 256M -nographic -no-reboot

run: $(B)/bastios.bin
	$(QEMU) $(QEMU_ARGS) -kernel $< -serial mon:stdio

test: $(B)/bastios.bin
	KERNEL=$< QEMU="$(QEMU)" test/aarch64_test.sh

clean:
	rm -rf build

.PRECIOUS: $(B)/user/%.c $(B)/user/%.o $(B)/user/%.elf
.PHONY: all run test clean
