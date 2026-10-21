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
#
# NATIVE=1 builds the kernel and the Bastio user programs with bastioc1's
# AArch64 backend instead (assembly, no C compiler on the proved code).
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

# Which backend built the Bastio user programs, rewritten only when it
# changes, so that switching NATIVE rebuilds them.
UBACKEND := $(if $(filter 1,$(NATIVE)),native,c)
$(B)/user.backend: FORCE | $(B)/user
	@echo $(UBACKEND) | cmp -s - $@ || echo $(UBACKEND) > $@
.PHONY: FORCE
FORCE:

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

# NATIVE=1 builds the Bastio user programs with bastioc1's AArch64 backend
# (writes assembly, no C compiler between the proved code and the machine
# code, only the assembler and linker) and links them without the C support
# functions, which nothing calls.
ifeq ($(NATIVE),1)
$(UPROGS:%=$(B)/user/%.s): $(B)/user/%.s: user/%.bastio $(wildcard user/lib/*.bastio) $(BASTIOC) $(B)/user.backend | $(B)/user
	$(BASTIOC) --lib $(BASTIOLIB) --target user --arch aarch64 --backend native --emit-asm -o $@ $<

$(UPROGS:%=$(B)/user/%.o): $(B)/user/%.o: $(B)/user/%.s
	$(CROSS)as -o $@ $<

$(UPROGS:%=$(B)/user/%.elf): $(B)/user/%.elf: $(B)/user/start.o $(B)/user/%.o user/user-aarch64.ld
	$(ALD) -nostdlib -static -z max-page-size=0x1000 -T user/user-aarch64.ld --build-id=none --no-warn-rwx-segments \
	  -o $@ $(B)/user/start.o $(B)/user/$*.o
else
$(UPROGS:%=$(B)/user/%.c): $(B)/user.backend
endif

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

# The vDSO (user/vdso-aarch64.c): a tiny shared object the kernel copies onto
# one page of every Linux process, with the clock from byte 0xF80 of it. The
# whole file must fit below the signal return trampoline at 0xF00.
$(B)/user/vdso.so: user/vdso-aarch64.c user/vdso-aarch64.ld user/vdso-aarch64.map | $(B)/user
	$(ACC) -O2 -fPIC -ffreestanding -fno-stack-protector -fno-asynchronous-unwind-tables \
	  -Wall -Wextra -Werror -nostdlib -shared -Wl,-T,user/vdso-aarch64.ld -Wl,--version-script=user/vdso-aarch64.map \
	  -Wl,--hash-style=both -Wl,-soname=linux-vdso.so.1 -Wl,--build-id=none -Wl,-z,max-page-size=0x1000 -o $@ user/vdso-aarch64.c
	@test $$(stat -c %s $@) -le 3840 || { echo "$@ does not fit below the signal trampoline"; rm -f $@; exit 1; }

# 2. Prove the kernel and translate it to freestanding C.
KSRCS := $(wildcard src/*.bastio) $(wildcard src/aarch64/*.bastio src/aarch64/arch/*.bastio) $(wildcard files/*)

# NATIVE=1 writes the kernel as assembly with bastioc1's AArch64 backend
# (no C compiler between the proved code and the machine code); the boot
# code and the platform stay C and assembly.
ifeq ($(NATIVE),1)
$(B)/kernel.s: $(KSRCS) $(UBINS) $(LXELFS) $(B)/user/vdso.so $(BASTIOC) $(B)/user.backend | $(B)
	$(BASTIOC) --lib $(BASTIOLIB) --lib src/aarch64 --lib src --target kernel --arch aarch64 --backend native --emit-asm -o $@ src/aarch64/main.bastio

$(B)/kernel.o: $(B)/kernel.s
	$(CROSS)as -o $@ $<
else
$(B)/kernel.c: $(KSRCS) $(UBINS) $(LXELFS) $(B)/user/vdso.so $(BASTIOC) $(B)/user.backend | $(B)
	$(BASTIOC) --lib $(BASTIOLIB) --lib src/aarch64 --lib src --target kernel --arch aarch64 -o $@ src/aarch64/main.bastio

$(B)/kernel.o: $(B)/kernel.c
	$(ACC) $(GCFLAGS) -c -o $@ $<
endif

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
	# musl's <net/if.h> defines IFF_DORMANT and glibc's does not, and BusyBox's `ip link` prints the operstate only under
	# #ifdef IFF_DORMANT: build it in, so the output is what the x86-64 build prints.
	sed -i 's/^#ifdef IFF_DORMANT$$/#if 1/' $(B)/compat/bb/networking/libiproute/ipaddress.c
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
MPY_GIT       ?= https://github.com/micropython/micropython
MPY_TAG       ?= v1.24.1
CPYTHON_GIT   ?= https://github.com/python/cpython
CPYTHON_TAG   ?= v3.12.7
ZLIB_GIT      ?= https://github.com/madler/zlib
ZLIB_TAG      ?= v1.3.1
GIT_GIT       ?= https://github.com/git/git
GIT_TAG       ?= v2.47.0
RIPGREP_VER   ?= 14.1.1
KILO_GIT      ?= https://github.com/antirez/kilo
KILO_REV      ?= 323d93b29bd89a2cb446de90c4ed4fea1764176e
COMPAT_APPS := $(B)/compat/lua $(B)/compat/qjs $(B)/compat/sqlite3 $(B)/compat/micropython $(B)/compat/kilo $(B)/compat/ttyepoll $(B)/compat/python $(B)/compat/git $(B)/compat/rg $(B)/compat/perl

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

# Dropbear and the keys of the ssh group (test/compat/ssh.txt) go on this disk
# too; the host client and key generator are built further down.
# MicroPython's unix port as a static glibc program, without the modules that
# need libraries the cross toolchain does not bring (ffi, btree, ssl); its
# mpy-cross, which runs while building, is the host's.
$(B)/compat/micropython: | $(B)/compat
	rm -rf $(B)/compat/src/micropython && mkdir -p $(B)/compat/src
	git clone -q --depth 1 --branch $(MPY_TAG) $(MPY_GIT) $(B)/compat/src/micropython
	git -C $(B)/compat/src/micropython submodule -q update --init --depth 1 lib/micropython-lib
	$(MAKE) -C $(B)/compat/src/micropython/mpy-cross -s ARCH= >/dev/null
	$(MAKE) -C $(B)/compat/src/micropython/ports/unix -s CC="$(ACC)" STRIP="$(CROSS)strip" LDFLAGS_EXTRA="-static -no-pie $(ULFLAGS)" \
	  MICROPY_PY_FFI=0 MICROPY_PY_BTREE=0 MICROPY_PY_SSL=0 MICROPY_SSL_AXTLS=0 >/dev/null
	$(CROSS)strip -o $@ $(B)/compat/src/micropython/ports/unix/build-standard/micropython

# CPython as a static glibc program with its C modules built in (zlib from
# source; no ctypes, sqlite3, readline, lzma, bz2, ssl), and its standard
# library in one zip for the disk as /disk/py/lib/python312.zip (run it with
# PYTHONHOME=/disk/py). It is cross-built with a host python3.12 of the same
# minor version, which also writes the zip.
$(B)/compat/python: test/compat/pyzip.py | $(B)/compat
	rm -rf $(B)/compat/src/pyzlib $(B)/compat/src/pyzlib-out $(B)/compat/src/cpython $(B)/compat/py && mkdir -p $(B)/compat/src
	git clone -q --depth 1 --branch $(ZLIB_TAG) $(ZLIB_GIT) $(B)/compat/src/pyzlib
	cd $(B)/compat/src/pyzlib && CC="$(ACC)" CFLAGS="-O2" ./configure --static --prefix=$(CURDIR)/$(B)/compat/src/pyzlib-out >/dev/null
	$(MAKE) -C $(B)/compat/src/pyzlib -s install >/dev/null
	git clone -q --depth 1 --branch $(CPYTHON_TAG) $(CPYTHON_GIT) $(B)/compat/src/cpython
	cd $(B)/compat/src/cpython && ZLIB_CFLAGS=-I$(CURDIR)/$(B)/compat/src/pyzlib-out/include \
	  ZLIB_LIBS="-L$(CURDIR)/$(B)/compat/src/pyzlib-out/lib -lz" PKG_CONFIG_LIBDIR=/nonexistent MODULE_BUILDTYPE=static \
	  py_cv_module__ctypes=n/a py_cv_module__ctypes_test=n/a py_cv_module_readline=n/a py_cv_module__uuid=n/a \
	  py_cv_module__lzma=n/a py_cv_module__bz2=n/a py_cv_module__sqlite3=n/a py_cv_module_xxlimited=n/a \
	  py_cv_module_xxlimited_35=n/a ac_cv_file__dev_ptmx=yes ac_cv_file__dev_ptc=no ac_cv_buggy_getaddrinfo=no \
	  CC="$(ACC)" CFLAGS="-O2" LDFLAGS="-static -no-pie $(ULFLAGS)" \
	  ./configure --host=aarch64-linux-gnu --build=x86_64-linux-gnu --with-build-python=python3.12 \
	  --disable-shared --disable-test-modules --without-ensurepip --prefix=/disk/py >/dev/null
	$(MAKE) -C $(B)/compat/src/cpython -s >/dev/null
	mkdir -p $(B)/compat/py/lib
	cp $(B)/compat/src/cpython/build/lib.*/_sysconfigdata_*.py $(B)/compat/src/cpython/Lib/
	python3.12 test/compat/pyzip.py $(B)/compat/src/cpython/Lib $(B)/compat/py/lib/python312.zip
	$(CROSS)strip -o $@ $(B)/compat/src/cpython/python

# git, with zlib and nothing else (no curl, OpenSSL, expat, gettext or iconv),
# as a static glibc program. It looks for its own subcommands in /disk.
$(B)/compat/git: | $(B)/compat
	rm -rf $(B)/compat/src/gitzlib $(B)/compat/src/gitzlib-out $(B)/compat/src/git && mkdir -p $(B)/compat/src
	git clone -q --depth 1 --branch $(ZLIB_TAG) $(ZLIB_GIT) $(B)/compat/src/gitzlib
	cd $(B)/compat/src/gitzlib && CC="$(ACC)" CFLAGS="-O2" ./configure --static --prefix=$(CURDIR)/$(B)/compat/src/gitzlib-out >/dev/null
	$(MAKE) -C $(B)/compat/src/gitzlib -s install >/dev/null
	git clone -q --depth 1 --branch $(GIT_TAG) $(GIT_GIT) $(B)/compat/src/git
	$(MAKE) -C $(B)/compat/src/git -s CC="$(ACC)" CFLAGS="-O2 -I$(CURDIR)/$(B)/compat/src/gitzlib-out/include" \
	  LDFLAGS="-static -no-pie $(ULFLAGS) -L$(CURDIR)/$(B)/compat/src/gitzlib-out/lib" NO_OPENSSL=1 NO_CURL=1 NO_EXPAT=1 \
	  NO_GETTEXT=1 NO_TCLTK=1 NO_PERL=1 NO_PYTHON=1 NO_ICONV=1 NO_REGEX=NeedsStartEnd \
	  prefix=/disk gitexecdir=/disk template_dir=/disk/git-templates git >/dev/null
	$(CROSS)strip -o $@ $(B)/compat/src/git/git

# perl, cross-built with perl-cross (the build machine's miniperl runs the
# generators), statically linked with its XS modules. The pure-Perl modules
# test/compat/apps.txt uses are collected into $(B)/compat/perllib, which goes
# on the disk as /disk/perllib. The re extension is left out: its static link
# duplicates the regex engine. Locale support is off: a static glibc cannot load
# locales, and perl would warn on every start when LANG names one.
PERL_GIT      ?= https://github.com/Perl/perl5
PERL_TAG      ?= v5.40.0
PERLX_URL     ?= https://github.com/arsv/perl-cross/releases/download/1.6.2/perl-cross-1.6.2.tar.gz
PERL_MODULES  := strict warnings Carp Exporter Data::Dumper JSON::PP Digest::MD5 Digest::SHA POSIX List::Util \
                 Scalar::Util Fcntl Socket IO::Handle IO::Socket::INET HTTP::Tiny Time::HiRes File::Spec \
                 File::Temp File::Path File::Basename Cwd Getopt::Long Storable Encode IO::Select
$(B)/compat/perl: test/compat/perlmods.py | $(B)/compat
	rm -rf $(B)/compat/src/perl5 $(B)/compat/perllib && mkdir -p $(B)/compat/src
	git clone -q --depth 1 --branch $(PERL_TAG) $(PERL_GIT) $(B)/compat/src/perl5
	curl -fsSL $(PERLX_URL) | tar xz -C $(B)/compat/src
	cp -a $(B)/compat/src/perl-cross-1.6.2/. $(B)/compat/src/perl5/
	cd $(B)/compat/src/perl5 && ./configure --target=aarch64-linux-gnu --prefix=/disk \
	  -Dprivlib=/disk/perllib -Darchlib=/disk/perllib -Dsitelib=/disk/perllib -Dsitearch=/disk/perllib \
	  -Dman1dir=none -Dman3dir=none -Doptimize=-O2 -Accflags=-DNO_LOCALE -Dldflags="-static -no-pie $(ULFLAGS)" -Dlibs=-lm -Uusedl -Uusethreads \
	  --disable-mod=DB_File,GDBM_File,NDBM_File,ODBM_File,SDBM_File,re >/dev/null
	$(MAKE) -C $(B)/compat/src/perl5 -s perl >/dev/null
	$(MAKE) -C $(B)/compat/src/perl5 -s >/dev/null
	python3 test/compat/perlmods.py $(B)/compat/src/perl5 $(B)/compat/perllib $(PERL_MODULES)
	$(CROSS)strip -o $@ $(B)/compat/src/perl5/perl
$(B)/compat/perllib: $(B)/compat/perl ;

# ripgrep, a static glibc build from crates.io (pinned by version and
# Cargo.lock). Needs cargo and the aarch64-unknown-linux-gnu target.
$(B)/compat/rg: | $(B)/compat
	command -v rustup >/dev/null && rustup target add aarch64-unknown-linux-gnu >/dev/null 2>&1 || true
	rm -rf $(B)/compat/src/rg-root && mkdir -p $(B)/compat/src
	RUSTFLAGS="-C target-feature=+crt-static -C relocation-model=static -C link-arg=-no-pie -C link-arg=-Wl,-z,max-page-size=0x1000 -C link-arg=-Wl,-Ttext-segment=0x400000" \
	  CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER=$(ACC) cargo install ripgrep --version $(RIPGREP_VER) --locked \
	  --target aarch64-unknown-linux-gnu --root $(B)/compat/src/rg-root --quiet
	$(CROSS)strip -o $@ $(B)/compat/src/rg-root/bin/rg

# kilo, a small editor that puts the terminal in raw mode, and a program that
# waits for keys in epoll_wait, as libuv does (test/compat/tty.txt).
$(B)/compat/kilo: | $(B)/compat
	rm -rf $(B)/compat/src/kilo && mkdir -p $(B)/compat/src/kilo
	git -C $(B)/compat/src/kilo init -q
	git -C $(B)/compat/src/kilo fetch -q --depth 1 $(KILO_GIT) $(KILO_REV)
	git -C $(B)/compat/src/kilo checkout -q FETCH_HEAD
	$(APPCC) -s -o $@ $(B)/compat/src/kilo/kilo.c

$(B)/compat/ttyepoll: test/compat/progs/ttyepoll.c | $(B)/compat
	$(APPCC) -s -o $@ $<

$(B)/compat/disk.img: $(COMPAT_BINS) $(COMPAT_APPS) $(B)/compat/perllib $(B)/compat/busybox $(B)/compat/dropbear build/compat/ssh_host_key build/compat/ssh_authorized_keys test/compat/mkdisk.py
	test/compat/mkdisk.py $@ $(COMPAT_BINS) $(COMPAT_APPS) $(B)/compat/busybox $(B)/compat/dropbear build/compat/ssh_host_key build/compat/ssh_authorized_keys perllib=$(B)/compat/perllib py=$(B)/compat/py

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
# ALPINE_PKGS pins the x86-64 manifest of heywoodlh/vim (tag 9.2); the same
# tag's arm64 manifest is pinned here, so the AArch64 repository has vim and xxd too.
VIM_ARM64 := heywoodlh/vim@sha256:7a8cdf7141a5e2ae30a603c7e6051b532fea39df819a9c29e9ee5e270dbb1e06

$(B)/compat/alpine.img: test/compat/alpinedisk.py test/compat/mkdisk.py test/compat/bastiotools.sh $(B)/compat/busybox $(B)/compat/runc tools/ocipull.sh \
    $(B)/compat/dropbear build/compat/ssh_host_key build/compat/ssh_authorized_keys \
    $(B)/compat/bastioc1 $(B)/compat/z3 $(B)/compat/tcc $(B)/compat/musl/lib/libc.a $(B)/compat/tree-sitter-c.so $(B)/compat/strace $(B)/compat/crun $(B)/compat/gdb.tar $(B)/compat/cmake.whl | $(B)/compat
	COMPAT_ARCH=aarch64 test/compat/alpinedisk.py $@ $(B)/compat/alpine $(ALPINE_BASE) $(filter-out heywoodlh/vim@%,$(ALPINE_PKGS)) $(VIM_ARM64) --cmake-wheel $(B)/compat/cmake.whl \
	  --disk $(ALPINE_DISK_PKGS) --cache $(ALPINE_CACHE) --busybox $(B)/compat/busybox \
	  --extra "$(BASTIO_TOOLS) $(B)/compat/runc tools/ocipull.sh $(B)/compat/dropbear build/compat/ssh_host_key build/compat/ssh_authorized_keys $(B)/compat/tree-sitter-c.so $(B)/compat/strace $(B)/compat/crun $(B)/compat/gdb.tar"

# The tree-sitter C parser for Neovim, as the x86-64 suite builds it (the
# top-level Makefile): the generated parser.c of the pinned npm package in a
# shared object. It links to no C library: nvim in the Alpine root (musl)
# resolves the few libc calls it makes when it loads it.
$(B)/compat/tree-sitter-c.so: | $(B)/compat
	rm -rf $(B)/compat/src/tree-sitter-c && mkdir -p $(B)/compat/src/tree-sitter-c
	curl -fsSL --retry 3 --retry-all-errors -o $(B)/compat/src/tree-sitter-c.tgz https://registry.npmjs.org/tree-sitter-c/-/tree-sitter-c-$(TREESITTER_C_VER).tgz
	echo "$(TREESITTER_C_SHA256)  $(B)/compat/src/tree-sitter-c.tgz" | sha256sum -c - >/dev/null
	tar xzf $(B)/compat/src/tree-sitter-c.tgz -C $(B)/compat/src/tree-sitter-c --strip-components=1 package/src
	$(CROSS)gcc -shared -fPIC -O2 -nostdlib -fno-stack-protector -Wl,-s -I $(B)/compat/src/tree-sitter-c/src -o $@.part $(B)/compat/src/tree-sitter-c/src/parser.c
	mv $@.part $@

# strace for the ptrace cases: nobody publishes an arm64 build to pin, so it is
# built from the release tarball (pinned in test/compat/images.mk) as a static
# executable at a fixed address, as the kernel loads those.
$(B)/compat/strace: | $(B)/compat
	rm -rf $(B)/compat/src/strace && mkdir -p $(B)/compat/src/strace
	curl -fsSL --retry 3 --retry-all-errors -o $(B)/compat/src/strace.tar.xz https://github.com/strace/strace/releases/download/v$(STRACE_VER)/strace-$(STRACE_VER).tar.xz
	echo "$(STRACE_SHA256)  $(B)/compat/src/strace.tar.xz" | sha256sum -c - >/dev/null
	tar xJf $(B)/compat/src/strace.tar.xz -C $(B)/compat/src/strace --strip-components=1
	cd $(B)/compat/src/strace && ./configure --host=aarch64-linux-gnu --enable-mpers=no CC=$(CROSS)gcc \
	  'LDFLAGS=-static -Wl,-z,max-page-size=0x1000 -Wl,-Ttext-segment=0x400000' >/dev/null
	$(MAKE) -C $(B)/compat/src/strace/src -j$$(nproc) all >/dev/null
	$(CROSS)strip -o $@.part $(B)/compat/src/strace/src/strace
	mv $@.part $@

# runc for the container cases: the static arm64 binary of the upstream
# release, checked against a pinned SHA-256 (the x86-64 suite's is the
# amd64 one, RUNC_VER in the top-level Makefile). It runs on BastiOS only.
RUNC_VER          ?= 1.2.6
RUNC_ARM64_SHA256 ?= 12c612e2ebe6ca198de676ce75ed557e79fe6109032209bb8e25166c967fe170
$(B)/compat/runc: | $(B)/compat
	curl -fsSL --retry 3 --retry-all-errors -o $@.part https://github.com/opencontainers/runc/releases/download/v$(RUNC_VER)/runc.arm64
	echo "$(RUNC_ARM64_SHA256)  $@.part" | sha256sum -c - >/dev/null
	chmod +x $@.part && mv $@.part $@

# gdb for the cases that debug a program in the Alpine root: the two arm64 layers
# of the bellsoft/alpaquita-linux-gdb:16.3-musl image, packed as the top-level
# Makefile packs the amd64 ones (see there for why): /disk/gdb.tar unpacks under
# the root's /opt. Declared with := so the CI cache key (it hashes the ?= lines)
# is left alone.
GDB_REPO := bellsoft/alpaquita-linux-gdb
GDB_LAYERS := sha256:4e37c35e973659a11a1f1ff0df2d5e4aae586089ec1d6934302355d948c191df sha256:ec3b534e1f4951e65425afb779a4b702a18b660e531f7a9338c6c16b51bc7860
GDB_LIBS := libexpat.so.1 liblzma.so.5 libreadline.so.8 libz.so.1 libzstd.so.1 libncursesw.so.6 libpython3.14.so.1.0 libmpfr.so.6 libgmp.so.10 libdebuginfod.so.1 libstdc++.so.6 libgcc_s.so.1 libcurl.so.4 libelf.so.1 libfts.so.0 libjson-c.so.5 libbrotlicommon.so.1 libbrotlidec.so.1 libcares.so.2 libidn2.so.0 libnghttp2.so.14 libpsl.so.5 libssl.so.3 libcrypto.so.3 libunistring.so.5
$(B)/compat/gdb.tar: | $(B)/compat
	rm -rf $(B)/compat/src/gdb && mkdir -p $(B)/compat/src/gdb/all $(B)/compat/src/gdb/gdb/bin $(B)/compat/src/gdb/gdb/lib
	for d in $(GDB_LAYERS); do \
	  test/compat/hubblob.sh $(GDB_REPO) $$d $(B)/compat/src/gdb/layer.tgz && \
	  tar xzf $(B)/compat/src/gdb/layer.tgz -C $(B)/compat/src/gdb/all --exclude='dev/*' --no-same-owner || exit 1; \
	done
	cd $(B)/compat/src/gdb && cp all/usr/bin/gdb gdb/bin/gdb && cp all/lib/ld-musl-aarch64.so.1 gdb/lib/ld-musl-aarch64.so.1 && ln -s ld-musl-aarch64.so.1 gdb/lib/libc.musl-aarch64.so.1 && \
	  for l in $(GDB_LIBS); do cp -L all/usr/lib/$$l gdb/lib/$$l || exit 1; done && \
	  cp -r all/usr/lib/python3.14 gdb/lib/python3.14 && cp -r all/usr/share/gdb gdb/share_gdb && mkdir gdb/share && mv gdb/share_gdb gdb/share/gdb && \
	  rm -rf gdb/lib/python3.14/test gdb/lib/python3.14/idlelib gdb/lib/python3.14/tkinter gdb/lib/python3.14/turtledemo gdb/lib/python3.14/ensurepip gdb/lib/python3.14/pydoc_data gdb/lib/python3.14/lib2to3 && \
	  find gdb/lib/python3.14 -name __pycache__ -prune -exec rm -rf {} + && \
	  $(CROSS)strip gdb/bin/gdb gdb/lib/*.so* 2>/dev/null; true
	tar cf $@.part -C $(B)/compat/src/gdb gdb
	mv $@.part $@

# cmake: the image the x86-64 suite takes it from has no arm64 build, so
# alpinedisk.py makes the package from the musllinux wheel on PyPI, pinned by
# SHA-256 (it bundles the C++ runtime it needs).
CMAKE_WHEEL_URL := https://files.pythonhosted.org/packages/4b/13/ff769c6fc4a1e1458d3e18c7989e84cc1d374bda994d01ec900d1d3bdd44/cmake-4.4.4-py3-none-musllinux_1_2_aarch64.whl
CMAKE_WHEEL_SHA256 := e2d98233f16b5d6b9d4673a0a724ce801fc413811befb8f0b008cbffb7a2ad63
$(B)/compat/cmake.whl: | $(B)/compat
	curl -fsSL --retry 3 --retry-all-errors -o $@.part $(CMAKE_WHEEL_URL)
	echo "$(CMAKE_WHEEL_SHA256)  $@.part" | sha256sum -c - >/dev/null
	mv $@.part $@

# crun, the other OCI runtime: the static arm64 binary of the upstream release
# (the x86-64 suite's CRUN_VER), checked against a pinned SHA-256.
CRUN_VER          ?= 1.19.1
CRUN_ARM64_SHA256 ?= 4f28e8912fa52d1325cb42bfdc41d8f387d44e415955a82c203251e9dc1def5c
$(B)/compat/crun: | $(B)/compat
	curl -fsSL --retry 3 --retry-all-errors -o $@.part https://github.com/containers/crun/releases/download/$(CRUN_VER)/crun-$(CRUN_VER)-linux-arm64
	echo "$(CRUN_ARM64_SHA256)  $@.part" | sha256sum -c - >/dev/null
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

# The Bastio toolchain, for the alpine-tty cases that edit a program in
# neovim and build it: bastioc1 (bastiolang's stage1, as C from bastioc0),
# Z3 to discharge its proofs, and tcc to build the C it writes, as static
# programs on the Alpine disk (outside the Alpine root, like the x86-64
# suite's). tcc makes static executables by default here (the kernel has no
# built-in musl dynamic linker) against musl's libc.a, which is built below
# from a pinned revision; its headers and C runtime go under /disk/c.
TCC_GIT       ?= https://github.com/TinyCC/tinycc
TCC_REV       ?= 43c7708b85681a2fd4451c8a541af4494a8919b2
Z3_GIT        ?= https://github.com/Z3Prover/z3
Z3_TAG        ?= z3-4.8.12
Z3_JOBS       ?= 4
MUSL_GIT      ?= https://github.com/kraj/musl
MUSL_REV      ?= 0784374d561435f7c787a555aeab8ede699ed298
TCC_SRC       := $(B)/compat/src/tcc
MUSL_ROOT     := $(CURDIR)/$(B)/compat/musl

$(B)/compat/musl/lib/libc.a: | $(B)/compat
	rm -rf $(B)/compat/src/musl $(B)/compat/musl && mkdir -p $(B)/compat/src/musl
	git -C $(B)/compat/src/musl init -q
	git -C $(B)/compat/src/musl fetch -q --depth 1 $(MUSL_GIT) $(MUSL_REV)
	git -C $(B)/compat/src/musl checkout -q FETCH_HEAD
	cd $(B)/compat/src/musl && CC="$(ACC)" ./configure --target=aarch64-linux-gnu --prefix=$(MUSL_ROOT) --disable-shared >/dev/null
	$(MAKE) -C $(B)/compat/src/musl -s -j4 >/dev/null
	$(MAKE) -C $(B)/compat/src/musl -s install >/dev/null

# tcc runs on the guest, so it is built with the AArch64 compiler; the one
# tool its build runs (c2str) is built for this machine first. ARCH is set
# on its makes because `make ARCH=aarch64` would otherwise reach them (a
# command-line variable beats config.mak) and tcc would build with no code
# generator.
$(B)/compat/tcc: test/compat/patches/tcc-auto-type.patch test/compat/patches/tcc-aarch64.patch | $(B)/compat
	rm -rf $(TCC_SRC) && mkdir -p $(TCC_SRC)
	git -C $(TCC_SRC) init -q
	git -C $(TCC_SRC) fetch -q --depth 1 $(TCC_GIT) $(TCC_REV)
	git -C $(TCC_SRC) checkout -q FETCH_HEAD
	git -C $(TCC_SRC) apply $(CURDIR)/test/compat/patches/tcc-auto-type.patch $(CURDIR)/test/compat/patches/tcc-aarch64.patch
	cd $(TCC_SRC) && ./configure --cc="$(ACC)" --cpu=arm64 --config-musl --extra-cflags="-O2 -DCONFIG_TCC_STATIC_DEFAULT" \
	  --extra-ldflags="-static -s $(ULFLAGS)" --sysincludepaths=/disk/c/include --libpaths=/disk/c/lib \
	  --crtprefix=/disk/c/lib --tccdir=/disk/c/lib --elfinterp=/lib/ld-musl-aarch64.so.1 >/dev/null
	$(MAKE) -C $(TCC_SRC) -s ARCH=arm64 CC=gcc c2str.exe tccdefs_.h
	$(MAKE) -C $(TCC_SRC) -s ARCH=arm64 tcc
	$(MAKE) -C $(TCC_SRC) -s ARCH=arm64 libtcc1.a arm64-libtcc1-usegcc=yes AR=$(CROSS)ar
	cp $(TCC_SRC)/tcc $@

# Z3 as a static glibc program, cross-built (single-threaded, as the x86-64 suite's).
$(B)/compat/z3: | $(B)/compat
	rm -rf $(B)/compat/src/z3 && mkdir -p $(B)/compat/src
	git clone -q --depth 1 --branch $(Z3_TAG) $(Z3_GIT) $(B)/compat/src/z3
	cd $(B)/compat/src/z3 && CXX="$(CROSS)g++" CC="$(ACC)" CXXFLAGS="-O2" LDFLAGS="-static -no-pie $(ULFLAGS)" \
	  python3 scripts/mk_make.py --single-threaded >/dev/null
	$(MAKE) -C $(B)/compat/src/z3/build -s -j$(Z3_JOBS) z3 >/dev/null
	$(CROSS)strip -o $@ $(B)/compat/src/z3/build/z3

$(B)/compat/bastioc1.c: $(BASTIOC0) $(wildcard $(BASTIOLIB)/stage1/*.bastio) | $(B)/compat
	$(BASTIOC0) --emit-c -o $@ $(BASTIOLIB)/stage1/bastioc1.bastio

$(B)/compat/bastioc1: $(B)/compat/bastioc1.c
	$(ACC) -O2 -w -static -no-pie -s $(ULFLAGS) -o $@ $<

# bastiolang's developer tools (tools/bastiofmt, tools/bastio-lsp), built the
# way bastioc1 is, as static executables for the Bastio disk (bastio.txt).
$(B)/compat/bastiofmt.c $(B)/compat/bastio-lsp.c: $(B)/compat/%.c: $(BASTIOC) $(wildcard $(BASTIOLIB)/tools/*/*.bastio) | $(B)/compat
	$(BASTIOC) --emit-c -o $@ $(BASTIOLIB)/tools/$*/$(if $(filter bastio-lsp,$*),lsp,$*).bastio

$(B)/compat/bastiofmt $(B)/compat/bastio-lsp: $(B)/compat/%: $(B)/compat/%.c
	$(ACC) -O2 -w -static -no-pie -s $(ULFLAGS) -o $@ $<

# GNU make as a static glibc program, cross-built, for the development disk
# (test/compat/dev.txt). Its sources come from its GitHub mirror and gnulib's;
# bootstrapping them needs autoconf, automake, autopoint and texinfo.
GMAKE_GIT     ?= https://github.com/mirror/make
GMAKE_TAG     ?= 4.4.1
GNULIB_GIT    ?= https://github.com/coreutils/gnulib
GNULIB_TAG    ?= stable-202301

$(B)/compat/make: | $(B)/compat
	rm -rf $(B)/compat/src/make $(B)/compat/src/gnulib && mkdir -p $(B)/compat/src
	git clone -q --depth 1 --branch $(GMAKE_TAG) $(GMAKE_GIT) $(B)/compat/src/make
	git clone -q --depth 1 --branch $(GNULIB_TAG) $(GNULIB_GIT) $(B)/compat/src/gnulib
	cd $(B)/compat/src/make && ./bootstrap --no-git --gnulib-srcdir=../gnulib --skip-po >/dev/null && \
	  CC="$(ACC)" CFLAGS="-O2" LDFLAGS="-static -no-pie $(ULFLAGS)" ./configure -q --host=aarch64-linux-gnu --disable-nls --without-guile
	$(MAKE) -C $(B)/compat/src/make -s >/dev/null
	$(CROSS)strip -o $@ $(B)/compat/src/make/make

# The development disk (test/compat/dev.txt): tcc, make and the C runtime tcc
# finds under /disk/c, listed the way the Bastio disk's is.
$(B)/compat/dev.img: $(B)/compat/tcc $(B)/compat/make $(B)/compat/musl/lib/libc.a test/compat/devdisk.sh test/compat/dev_headers.c test/compat/mkdisk.py
	DEV_DEPCC=gcc DEV_DEPMODE=-E DEV_CLIBS="crt1.o crti.o crtn.o libc.a" test/compat/devdisk.sh $@ $(B)/compat/tcc $(B)/compat/make \
	  $(CURDIR)/$(TCC_SRC) $(MUSL_ROOT)/include $(MUSL_ROOT)/lib

# The mkdisk.py arguments for the toolchain: the headers are listed by this
# machine's compiler (tcc cannot run here), the C runtime is musl's static one.
BASTIO_TOOLS = $(shell BASTIO_DEPCC=gcc BASTIO_DEPMODE=-E BASTIO_CLIBS="crt1.o crti.o crtn.o libc.a" test/compat/bastiotools.sh \
  $(B)/compat/bastioc1 $(B)/compat/bastioc1.c $(B)/compat/z3 $(B)/compat/tcc $(CURDIR)/$(TCC_SRC) $(MUSL_ROOT)/include $(MUSL_ROOT)/lib)

# The Bastio disk (test/compat/bastio.txt): the same toolchain and the programs
# it builds, on a disk of its own, as on x86-64 (bastiodisk.sh).
$(B)/compat/bastio.img: $(B)/compat/bastioc1 $(B)/compat/z3 $(B)/compat/tcc $(B)/compat/musl/lib/libc.a $(B)/compat/bastiofmt $(B)/compat/bastio-lsp test/compat/bastiodisk.sh \
    test/compat/bastiotools.sh $(wildcard test/compat/bastio/*.bastio) test/compat/mkdisk.py | $(B)/compat
	BASTIO_DEPCC=gcc BASTIO_DEPMODE=-E BASTIO_CLIBS="crt1.o crti.o crtn.o libc.a" test/compat/bastiodisk.sh $@ \
	  $(B)/compat/bastioc1 $(B)/compat/bastioc1.c $(B)/compat/z3 $(B)/compat/tcc $(CURDIR)/$(TCC_SRC) \
	  $(MUSL_ROOT)/include $(MUSL_ROOT)/lib $(BASTIOLIB)/stage1 $(B)/compat/bastiofmt $(B)/compat/bastio-lsp

# Debian (glibc) and Node (musl, V8) the same way, from the arm64 layers.
$(B)/compat/debian.img: test/compat/debiandisk.py test/compat/alpinedisk.py test/compat/mkdisk.py $(B)/compat/busybox
	COMPAT_ARCH=aarch64 test/compat/debiandisk.py $@ $(DEBIAN_IMAGE) $(B)/compat/busybox --cache $(ALPINE_CACHE)

$(B)/compat/node.img: test/compat/nodedisk.py test/compat/alpinedisk.py test/compat/mkdisk.py $(B)/compat/busybox
	COMPAT_ARCH=aarch64 test/compat/nodedisk.py $@ $(NODE_IMAGE) $(B)/compat/busybox --cache $(ALPINE_CACHE)

# COMPAT_DISKS, COMPAT_LANES and COMPAT_SKIP_LANES (run.py --lanes and
# --skip-lanes) let a CI runner build and run a part of the suite: the
# images of its lanes, and those lanes.
COMPAT_DISKS ?= $(B)/compat/disk.img $(B)/compat/dev.img $(B)/compat/bastio.img $(B)/compat/alpine.img $(B)/compat/debian.img $(B)/compat/node.img
COMPAT_RUN := test/compat/run.py --arch aarch64 --groups progs,busybox,apps,ssh,tty --kernel $(B)/bastios.bin \
              --disk $(B)/compat/disk.img --dev-disk $(B)/compat/dev.img --bastio-disk $(B)/compat/bastio.img --alpine-disk $(B)/compat/alpine.img \
              --alpine-repo $(B)/compat/alpine/disk/http \
              --registry-image $(ALPINE_CACHE)/sha256-$(ALPINE_BASE_DIGEST)-arm64.json --debian-disk $(B)/compat/debian.img \
              --node-disk $(B)/compat/node.img --out $(B)/compat \
              $(if $(COMPAT_LANES),--lanes $(COMPAT_LANES)) $(if $(COMPAT_SKIP_LANES),--skip-lanes $(COMPAT_SKIP_LANES))

compat-disks: $(B)/bastios.bin $(COMPAT_DISKS)

compat: $(B)/bastios.bin $(COMPAT_DISKS)
	$(COMPAT_RUN) $(COMPAT_PROGS)

compat-baseline: $(B)/bastios.bin $(COMPAT_DISKS)
	$(COMPAT_RUN) --update-baseline $(COMPAT_PROGS)
