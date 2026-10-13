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

$(B)/efi.o: boot/aarch64/efi.c | $(B)
	$(ACC) $(TCFLAGS) -mstrict-align -c -o $@ $<

$(B)/boot.o: boot/aarch64/boot.S | $(B)
	$(ACC) -c -o $@ $<

$(B)/libc.o: boot/libc.c | $(B)
	$(ACC) $(TCFLAGS) -mstrict-align -c -o $@ $<

$(B)/bastios.elf: $(B)/boot.o $(B)/efi.o $(B)/platform.o $(B)/kernel.o $(B)/libc.o boot/aarch64/linker.ld
	$(ALD) -nostdlib -static --no-warn-rwx-segments -T boot/aarch64/linker.ld -o $@ \
	  $(B)/boot.o $(B)/efi.o $(B)/kernel.o $(B)/platform.o $(B)/libc.o

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

# The Linux compatibility suite (test/compat) on AArch64: the t_*.c programs
# as static glibc executables at 0x400000, and a BusyBox with the suite's
# broad applet list, run from a disk image under ash.
#   make -f arch/aarch64/Makefile compat            writes build/aarch64/compat/report.md
#   make -f arch/aarch64/Makefile compat-baseline   records which checks pass now
# It fails only if a check in test/compat/baseline-aarch64.txt stops passing.
# Programs that exist only for x86-64 (none now).
COMPAT_X86ONLY :=
COMPAT_PROGS := $(filter-out $(COMPAT_X86ONLY),$(patsubst test/compat/progs/%.c,%,$(wildcard test/compat/progs/t_*.c)))
COMPAT_BINS := $(COMPAT_PROGS:%=$(B)/compat/%)
ULFLAGS := -Wl,-z,max-page-size=0x1000 -Wl,-Ttext-segment=0x400000 -Wl,--build-id=none

$(B)/compat:
	mkdir -p $@

$(COMPAT_BINS): $(B)/compat/%: test/compat/progs/%.c test/compat/progs/compat.h | $(B)/compat
	$(ACC) -static -no-pie -O2 -D_GNU_SOURCE -Wall -Wno-unused-result $(ULFLAGS) -o $@ $<

# Applets the Alpine, Debian and Node cases run from /disk/busybox (the x86-64
# suite uses Alpine's own BusyBox for them).
COMPAT_BB_MORE := CONFIG_CHROOT CONFIG_MKNOD CONFIG_MOUNT CONFIG_UMOUNT CONFIG_CHOWN CONFIG_SU CONFIG_USE_BB_CRYPT

$(B)/compat/busybox: $(B)/busybox/Makefile test/compat/busybox.config | $(B)/compat $(B)/user/ash.elf
	rm -rf $(B)/compat/bb && cp -r $(B)/busybox $(B)/compat/bb
	$(MAKE) -C $(B)/compat/bb -s clean >/dev/null
	$(MAKE) -C $(B)/compat/bb -s allnoconfig >/dev/null
	for o in $$(grep -v '^#' test/compat/busybox.config) $(COMPAT_BB_MORE); do \
	  sed -i "s/^# $$o is not set/$$o=y/" $(B)/compat/bb/.config; done
	yes "" | $(MAKE) -C $(B)/compat/bb -s oldconfig >/dev/null
	$(MAKE) -C $(B)/compat/bb -s CROSS_COMPILE=$(CROSS) CFLAGS_busybox="-static $(ULFLAGS)" busybox
	cp $(B)/compat/bb/busybox $@

# Real programs (test/compat/apps.txt), built from the sources the x86-64 suite
# uses (the Makefile's *_GIT and *_TAG), as static glibc programs at 0x400000.
# The cases of the others are left out on AArch64 (run.py, AARCH64_APPS).
LUA_GIT       ?= https://github.com/lua/lua
LUA_TAG       ?= v5.4.7
QUICKJS_GIT   ?= https://github.com/bellard/quickjs
QUICKJS_REV   ?= 535a7c250ff4a577ec36c3e103daab6dadeea650
SQLITE_GIT    ?= https://github.com/sqlite/sqlite
SQLITE_TAG    ?= version-3.52.0
APPCC := $(ACC) -static -no-pie -O2 -w $(ULFLAGS)
COMPAT_APPS := $(B)/compat/lua $(B)/compat/qjs $(B)/compat/sqlite3

$(B)/compat/lua: | $(B)/compat
	rm -rf $(B)/compat/src/lua && mkdir -p $(B)/compat/src
	git clone -q --depth 1 --branch $(LUA_TAG) $(LUA_GIT) $(B)/compat/src/lua
	$(APPCC) -DLUA_USE_LINUX -DMAKE_LUA -o $@ $(B)/compat/src/lua/onelua.c -lm

$(B)/compat/qjs: | $(B)/compat
	rm -rf $(B)/compat/src/quickjs && mkdir -p $(B)/compat/src/quickjs
	git -C $(B)/compat/src/quickjs init -q
	git -C $(B)/compat/src/quickjs fetch -q --depth 1 $(QUICKJS_GIT) $(QUICKJS_REV)
	git -C $(B)/compat/src/quickjs checkout -q FETCH_HEAD
	$(MAKE) -C $(B)/compat/src/quickjs -s CROSS_PREFIX=aarch64-linux-gnu- CONFIG_LTO= qjs >/dev/null
	cd $(B)/compat/src/quickjs && $(APPCC) -s -o $(CURDIR)/$@ .obj/qjs.o .obj/repl.o .obj/quickjs.o \
	  .obj/dtoa.o .obj/libregexp.o .obj/libunicode.o .obj/cutils.o .obj/quickjs-libc.o -lm -lpthread

$(B)/compat/sqlite3: | $(B)/compat
	rm -rf $(B)/compat/src/sqlite && mkdir -p $(B)/compat/src
	git clone -q --depth 1 --branch $(SQLITE_TAG) $(SQLITE_GIT) $(B)/compat/src/sqlite
	cd $(B)/compat/src/sqlite && ./configure --disable-tcl >/dev/null && $(MAKE) -s sqlite3.c shell.c >/dev/null
	$(APPCC) -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -o $@ \
	  $(B)/compat/src/sqlite/shell.c $(B)/compat/src/sqlite/sqlite3.c -lm

$(B)/compat/disk.img: $(COMPAT_BINS) $(COMPAT_APPS) $(B)/compat/busybox test/compat/mkdisk.py
	test/compat/mkdisk.py $@ $(COMPAT_BINS) $(COMPAT_APPS) $(B)/compat/busybox

# Alpine Linux for AArch64 (test/compat/alpine.txt), from the same pinned
# Docker Hub images as the x86-64 suite's, their arm64 layers: the minirootfs
# entered with chroot and apk installing packages from a local repository.
# Without the Bastio toolchain and Dropbear, which are built for x86-64 only,
# so the checks that use them fail and stay out of the baseline.
include test/compat/images.mk
ALPINE_CACHE ?= $(B)/compat/alpine
ALPINE_BASE_DIGEST = $(word 2,$(subst @sha256:, ,$(ALPINE_BASE)))

# The shell commands of these disks run the suite's static BusyBox: the
# AArch64 kernel has no built-in musl dynamic linker, so Alpine's own
# (dynamic) BusyBox starts only inside the unpacked root.
$(B)/compat/alpine.img: test/compat/alpinedisk.py test/compat/mkdisk.py $(B)/compat/busybox $(B)/compat/runc tools/ocipull.sh \
    $(B)/compat/dropbear build/compat/ssh_host_key build/compat/ssh_authorized_keys | $(B)/compat
	COMPAT_ARCH=aarch64 test/compat/alpinedisk.py $@ $(B)/compat/alpine $(ALPINE_BASE) $(ALPINE_PKGS) \
	  --disk $(ALPINE_DISK_PKGS) --cache $(ALPINE_CACHE) --busybox $(B)/compat/busybox \
	  --extra "$(B)/compat/runc tools/ocipull.sh $(B)/compat/dropbear build/compat/ssh_host_key build/compat/ssh_authorized_keys"

# runc for the container cases: the static arm64 binary of the upstream
# release, checked against a pinned SHA-256 (the x86-64 suite's is the
# amd64 one, RUNC_VER in the top-level Makefile). It runs on BastiOS only.
RUNC_VER          ?= 1.2.6
RUNC_ARM64_SHA256 ?= 12c612e2ebe6ca198de676ce75ed557e79fe6109032209bb8e25166c967fe170
$(B)/compat/runc: | $(B)/compat
	curl -fsSL -o $@.part https://github.com/opencontainers/runc/releases/download/v$(RUNC_VER)/runc.arm64
	echo "$(RUNC_ARM64_SHA256)  $@.part" | sha256sum -c - >/dev/null
	chmod +x $@.part && mv $@.part $@

# Dropbear, the SSH server of the "nvim over ssh" case, built from the same
# source as the x86-64 suite's (DROPBEAR_GIT, DROPBEAR_TAG) as a static glibc
# program at 0x400000. The client, the key tool and the keys the case uses
# run on this machine: they are built natively into build/compat, where the
# case looks for them (the x86-64 suite's rule makes the same files).
DROPBEAR_GIT ?= https://github.com/mkj/dropbear
DROPBEAR_TAG ?= DROPBEAR_2024.86
DROPBEAR_CONF := --disable-zlib --disable-lastlog --disable-utmp --disable-utmpx --disable-wtmp --disable-wtmpx \
                 --disable-pututline --disable-pututxline --disable-syslog

$(B)/compat/dropbear: | $(B)/compat
	rm -rf $(B)/compat/src/dropbear && mkdir -p $(B)/compat/src
	git clone -q --depth 1 --branch $(DROPBEAR_TAG) $(DROPBEAR_GIT) $(B)/compat/src/dropbear
	cd $(B)/compat/src/dropbear && CC="$(ACC)" CFLAGS="-O2" LDFLAGS="-static -no-pie $(ULFLAGS) -s" ./configure -q \
	  --host=aarch64-linux-gnu --disable-harden $(DROPBEAR_CONF) >/dev/null
	echo '#define DROPBEAR_SVR_PASSWORD_AUTH 0' > $(B)/compat/src/dropbear/localoptions.h   # no static crypt() here; the case logs in by key
	$(MAKE) -C $(B)/compat/src/dropbear -s STATIC=1 PROGRAMS=dropbear >/dev/null
	cp $(B)/compat/src/dropbear/dropbear $@

build/compat/dbclient build/compat/dropbearkey &: | $(B)/compat
	rm -rf $(B)/compat/src/dropbear-host && mkdir -p $(B)/compat/src build/compat
	git clone -q --depth 1 --branch $(DROPBEAR_TAG) $(DROPBEAR_GIT) $(B)/compat/src/dropbear-host
	cd $(B)/compat/src/dropbear-host && ./configure -q $(DROPBEAR_CONF) >/dev/null
	$(MAKE) -C $(B)/compat/src/dropbear-host -s PROGRAMS="dbclient dropbearkey" >/dev/null
	cp $(B)/compat/src/dropbear-host/dbclient $(B)/compat/src/dropbear-host/dropbearkey build/compat/

build/compat/ssh_host_key build/compat/ssh_authorized_keys &: build/compat/dropbearkey
	rm -f build/compat/ssh_host_key build/compat/ssh_id
	build/compat/dropbearkey -t ed25519 -f build/compat/ssh_host_key >/dev/null
	build/compat/dropbearkey -t ed25519 -f build/compat/ssh_id | grep '^ssh-' > build/compat/ssh_authorized_keys

# Debian (glibc) and Node (musl, V8) the same way, from the arm64 layers.
$(B)/compat/debian.img: test/compat/debiandisk.py test/compat/alpinedisk.py test/compat/mkdisk.py $(B)/compat/busybox
	COMPAT_ARCH=aarch64 test/compat/debiandisk.py $@ $(DEBIAN_IMAGE) $(B)/compat/busybox --cache $(ALPINE_CACHE)

$(B)/compat/node.img: test/compat/nodedisk.py test/compat/alpinedisk.py test/compat/mkdisk.py $(B)/compat/busybox
	COMPAT_ARCH=aarch64 test/compat/nodedisk.py $@ $(NODE_IMAGE) $(B)/compat/busybox --cache $(ALPINE_CACHE)

COMPAT_DISKS := $(B)/compat/disk.img $(B)/compat/alpine.img $(B)/compat/debian.img $(B)/compat/node.img
COMPAT_RUN := test/compat/run.py --arch aarch64 --groups progs,busybox,apps --kernel $(B)/bastios.bin \
              --disk $(B)/compat/disk.img --alpine-disk $(B)/compat/alpine.img \
              --alpine-repo $(B)/compat/alpine/disk/http \
              --registry-image $(ALPINE_CACHE)/sha256-$(ALPINE_BASE_DIGEST)-arm64.json --debian-disk $(B)/compat/debian.img \
              --node-disk $(B)/compat/node.img --out $(B)/compat

compat: $(B)/bastios.bin $(COMPAT_DISKS)
	$(COMPAT_RUN) $(COMPAT_PROGS)

compat-baseline: $(B)/bastios.bin $(COMPAT_DISKS)
	$(COMPAT_RUN) --update-baseline $(COMPAT_PROGS)
