#!/bin/sh
# Writes the Bastio disk for the compat suite: the self-hosted Bastio
# compiler at /disk/bastioc1, Z3 at /disk/z3, tcc at /disk/tcc, and under
# /disk/c the C runtime tcc was configured to find there: the headers the
# compiler's generated C includes, crt1.o, crti.o, crtn.o, musl's libc.so
# and tcc's libtcc1.a. Then the programs test/compat/bastio.txt builds
# (test/compat/bastio/*.bastio) and, under /disk/s, the compiler's own
# sources, so that it can verify and build itself.
#
#   bastiodisk.sh OUT.img BASTIOC1 BASTIOC1_C Z3 TCC TCC_SRC MUSL_INC MUSL_LIB STAGE1_DIR
set -e
out=$1 b1=$2 b1c=$3 z3=$4 tcc=$5 src=$6 inc=$7 lib=$8 stage1=$9
here=$(dirname "$0")
args="bastioc1=$b1 z3=$z3 tcc=$tcc"
# The headers: those the compiler's own C needs, which every program's C
# shares (they all start with the same prelude).
deps=$(mktemp)
trap 'rm -f "$deps" "$deps.o"' EXIT
"$tcc" -nostdinc -I"$src/include" -I"$inc" -MD -MF "$deps" -c -o "$deps.o" "$b1c"
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
for f in "$here"/bastio/*.bastio; do
  args="$args $f"
done
# The compiler's sources: bastioc1.bastio and every file it imports, by
# name from its own directory (as stage1's files import each other).
srcs=" "
walk() {
  case $srcs in *" $1 "*) return ;; esac
  srcs="$srcs$1 "
  for i in $(sed -n 's/^import "\(.*\)";$/\1/p' "$stage1/$1"); do
    walk "$i"
  done
}
walk bastioc1.bastio
for n in $srcs; do
  args="$args s/$n=$stage1/$n"
done
"$here/mkdisk.py" "$out" $args
