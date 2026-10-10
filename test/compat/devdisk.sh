#!/bin/sh
# Writes the development disk for the compat suite: tcc and GNU make at
# /disk/tcc and /disk/make, and under /disk/c the C runtime tcc was
# configured to find there (see the Makefile): the headers that
# dev_headers.c pulls in, crt1.o, crti.o, crtn.o, musl's libc.so to link
# against, and tcc's libtcc1.a. Programs tcc builds are dynamic PIEs whose
# interpreter, /lib/ld-musl-x86_64.so.1, is built into the kernel.
#
#   devdisk.sh OUT.img TCC MAKE TCC_SRC MUSL_INC MUSL_LIB
set -e
out=$1 tcc=$2 make=$3 src=$4 inc=$5 lib=$6
here=$(dirname "$0")
args="tcc=$tcc make=$make"
# tcc's own headers (stdarg.h, stddef.h, ...) come first, as for tcc itself.
deps=$(mktemp)
trap 'rm -f "$deps" "$deps.o"' EXIT
"$tcc" -nostdinc -I"$src/include" -I"$inc" -MD -MF "$deps" -c -o "$deps.o" "$here/dev_headers.c"
for h in $(tr ' \\' '\n\n' < "$deps" | grep '^/' | sort -u); do
  case $h in
    "$src/include/"*) rel=${h#"$src/include/"} ;;
    "$inc/"*) rel=${h#"$inc/"} ;;
    *) continue ;;
  esac
  args="$args c/include/$rel=$h"
done
for f in crt1.o crti.o crtn.o libc.so; do
  args="$args c/lib/$f=$lib/$f"
done
args="$args c/lib/libtcc1.a=$src/libtcc1.a"
"$here/mkdisk.py" "$out" $args
