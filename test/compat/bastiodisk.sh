#!/bin/sh
# Writes the Bastio disk for the compat suite: the toolchain bastiotools.sh
# lists (bastioc1, Z3, tcc and the C runtime tcc finds under /disk/c), then
# the programs test/compat/bastio.txt builds (test/compat/bastio/*.bastio)
# and, under /disk/s, the compiler's own sources, so that it can verify and
# build itself.
#
#   bastiodisk.sh OUT.img BASTIOC1 BASTIOC1_C Z3 TCC TCC_SRC MUSL_INC MUSL_LIB STAGE1_DIR [BASTIOFMT BASTIO_LSP]
#
# BASTIOFMT and BASTIO_LSP, the developer tools built for BastiOS, go to
# /disk/bastiofmt and /disk/bastio-lsp (bastio.txt runs them).
set -e
out=$1 b1=$2 b1c=$3 z3=$4 tcc=$5 src=$6 inc=$7 lib=$8 stage1=$9
fmt=${10:-} lsp=${11:-}
here=$(dirname "$0")
args=$("$here/bastiotools.sh" "$b1" "$b1c" "$z3" "$tcc" "$src" "$inc" "$lib")
[ -z "$fmt" ] || args="$args bastiofmt=$fmt"
[ -z "$lsp" ] || args="$args bastio-lsp=$lsp"
for f in "$here"/bastio/*; do
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
