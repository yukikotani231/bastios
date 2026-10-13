#!/bin/sh
# Prints the mkdisk.py arguments that put the Bastio toolchain on a disk:
# the self-hosted Bastio compiler at /disk/bastioc1, Z3 at /disk/z3, tcc at
# /disk/tcc, and under /disk/c the C runtime tcc was configured to find
# there: the headers the compiler's generated C includes, crt1.o, crti.o,
# crtn.o, musl's libc.so and tcc's libtcc1.a. Used by bastiodisk.sh and
# alpinedisk.py's disk, so that both boot with the same compiler.
#
# The headers are listed with the compiler in $BASTIO_DEPCC (default: tcc
# itself, which has to run here), and the files under /disk/c/lib are
# $BASTIO_CLIBS (default: musl's shared libc, as the x86-64 suite links
# against it; the AArch64 suite sets its static libc.a).
#
#   bastiotools.sh BASTIOC1 BASTIOC1_C Z3 TCC TCC_SRC MUSL_INC MUSL_LIB
set -e
b1=$1 b1c=$2 z3=$3 tcc=$4 src=$5 inc=$6 lib=$7
depcc=${BASTIO_DEPCC:-$tcc}
clibs=${BASTIO_CLIBS:-crt1.o crti.o crtn.o libc.so}
depmode=${BASTIO_DEPMODE:--c}      # -E for a compiler that cannot compile for the target
args="bastioc1=$b1 z3=$z3 tcc=$tcc"
# The headers: those the compiler's own C needs, which every program's C
# shares (they all start with the same prelude).
deps=$(mktemp)
trap 'rm -f "$deps" "$deps.o"' EXIT
"$depcc" -nostdinc -I"$src/include" -I"$inc" -MD -MF "$deps" $depmode -o "$deps.o" "$b1c"
for h in $(tr ' \\' '\n\n' < "$deps" | grep '^/' | sort -u); do
  case $h in
    "$src/include/"*) rel=${h#"$src/include/"} ;;
    "$inc/"*) rel=${h#"$inc/"} ;;
    *) continue ;;
  esac
  args="$args c/include/$rel=$h"
done
for f in $clibs; do
  args="$args c/lib/$f=$lib/$f"
done
args="$args c/lib/libtcc1.a=$src/libtcc1.a"
echo "$args"
