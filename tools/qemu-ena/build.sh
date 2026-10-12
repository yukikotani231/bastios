#!/bin/sh
# Builds qemu-system-x86_64 with the ENA test model (ena.c) into
# $OUT/bin/qemu-system-x86_64 (default build/qemu-ena). QEMU's sources are
# fetched at a pinned tag; nothing here is part of BastiOS or its trusted
# base. Needs the usual QEMU build dependencies (ninja, glib, pixman, slirp).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$here/../../build/qemu-ena}
TAG=${QEMU_TAG:-v8.2.2}
SRC=${QEMU_SRC:-$OUT/src}
mkdir -p "$OUT"
if [ ! -d "$SRC/.git" ]; then
  git clone -q --depth 1 --branch "$TAG" https://gitlab.com/qemu-project/qemu.git "$SRC"
fi
cp "$here/ena.c" "$SRC/hw/net/ena.c"
grep -q 'ena.c' "$SRC/hw/net/meson.build" || \
  echo "system_ss.add(when: 'CONFIG_ENA', if_true: files('ena.c'))" >> "$SRC/hw/net/meson.build"
grep -q '^config ENA$' "$SRC/hw/net/Kconfig" || cat >> "$SRC/hw/net/Kconfig" <<'K'

config ENA
    bool
    default y if PCI_DEVICES
    depends on PCI
K
mkdir -p "$OUT/build"
cd "$OUT/build"
[ -f build.ninja ] || "$SRC/configure" --prefix="$OUT" --target-list=x86_64-softmmu \
  --without-default-features --enable-slirp --disable-docs --disable-werror
ninja qemu-system-x86_64
mkdir -p "$OUT/bin" "$OUT/share/qemu"
cp qemu-system-x86_64 "$OUT/bin/"
# The firmware and option ROMs the binary looks for next to itself.
cp -rL "$SRC/pc-bios/." "$OUT/share/qemu/" 2>/dev/null || true
echo "$OUT/bin/qemu-system-x86_64"
