# BastiOS for AArch64 (QEMU virt), driven by `make ARCH=aarch64 <goal>` in
# the top-level Makefile.
#   make ARCH=aarch64          build build/aarch64/bastios.elf
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

all: $(B)/bastios.elf

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

$(B)/user/%.elf: $(B)/user/start.o $(B)/user/%.o $(B)/user/libc.o user/user.ld
	$(ALD) -nostdlib -static -z max-page-size=0x1000 -T user/user.ld --build-id=none --no-warn-rwx-segments \
	  -o $@ $(B)/user/start.o $(B)/user/$*.o $(B)/user/libc.o

$(B)/user/%.bin: $(B)/user/%.elf
	$(AOBJCOPY) -O binary $< $@

# 2. Prove the kernel and translate it to freestanding C.
KSRCS := $(wildcard src/*.bastio) $(wildcard src/aarch64/*.bastio src/aarch64/arch/*.bastio) $(wildcard files/*)

$(B)/kernel.c: $(KSRCS) $(UBINS) $(BASTIOC) | $(B)
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

QEMU_ARGS := -M virt,gic-version=3 -cpu cortex-a72 -m 256M -nographic -no-reboot

run: $(B)/bastios.elf
	$(QEMU) $(QEMU_ARGS) -kernel $< -serial mon:stdio

test: $(B)/bastios.elf
	KERNEL=$< QEMU="$(QEMU)" test/aarch64_test.sh

clean:
	rm -rf build

.PRECIOUS: $(B)/user/%.c $(B)/user/%.o $(B)/user/%.elf
.PHONY: all run test clean
