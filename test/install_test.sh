#!/usr/bin/env bash
# Builds the installer image (make installer) and runs the installation in
# QEMU: BIOS on a virtio disk, UEFI on an NVMe drive (test/install_test.py).
# Skips what it lacks the tools for.
set -u
cd "$(dirname "$0")/.."
for t in xorriso mkfs.fat mcopy qemu-system-x86_64; do
  if ! command -v "$t" >/dev/null; then
    echo "SKIP install: $t missing"
    exit 0
  fi
done
make -s installer || exit 1
rc=0
python3 test/install_test.py bios virtio || rc=1
python3 test/install_test.py uefi nvme || rc=1
exit $rc
