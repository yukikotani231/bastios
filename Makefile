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
LDFLAGS := -nostdlib -static -no-pie -z max-page-size=0x1000 -T boot/linker.ld -Wl,--build-id=none

all: build/bastios.elf

build:
	mkdir -p build

# 1. Prove the kernel and translate it to freestanding C.
build/kernel.c: src/main.bastio $(BASTIOC) | build
	$(BASTIOC) --target kernel -o $@ $<

build/kernel.o: build/kernel.c
	$(CC) $(CFLAGS) -c -o $@ $<

build/libc.o: boot/libc.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/boot.o: boot/boot.S | build
	$(CC) -c -o $@ $<

# 2. Link a 64-bit kernel, then repackage it as ELF32: QEMU's -kernel
#    Multiboot loader only accepts 32-bit ELF images. The code is unchanged.
build/bastios64.elf: build/boot.o build/kernel.o build/libc.o boot/linker.ld
	$(CC) $(LDFLAGS) -o $@ build/boot.o build/kernel.o build/libc.o

build/bastios.elf: build/bastios64.elf
	objcopy -O elf32-i386 $< $@

QEMU_ARGS := -kernel build/bastios.elf -m 64M -display none -no-reboot \
             -device isa-debug-exit,iobase=0xf4,iosize=0x04

run: build/bastios.elf
	$(QEMU) $(QEMU_ARGS) -serial mon:stdio

test: build/bastios.elf
	test/shell_test.sh

clean:
	rm -rf build

.PHONY: all run test clean
