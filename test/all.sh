#!/usr/bin/env bash
# `make test`: every test below. Each boots its own QEMU with its own disk
# images and host ports, so tests can run side by side; they run in lanes,
# one test after another within a lane and the lanes at once:
#   a: the shell test, the longest, on 4 processors;
#   b: the TCP and IPv6 tests (the two IPv6 tests share build/ipv6, so
#      they stay in one lane) and one DHCP test;
#   c: everything else.
# Three lanes and not more: the TCP tests time a stalled wire against the
# guest's resend timer, and a test of the shell's checks that a process is
# asleep, so a machine much busier than this makes them fail for the wrong
# reason. With fewer than 4 CPUs, or JOBS=1, every test runs one after
# another, as before. The output of each test is printed whole, in the
# order below, followed by how long it took; the run fails if any test
# fails.
set -u
cd "$(dirname "$0")/.."
JOBS=${JOBS:-$( [ "$(nproc 2>/dev/null || echo 1)" -ge 4 ] && echo 3 || echo 1 )}
# name|lane|command, in the order they are printed
tests=(
  "trusted_base|c|test/trusted_base.sh"
  "sysfuzz_calls|c|test/sysfuzz_calls.sh"
  "aarch64|c|test/aarch64_test.sh --build"
  "aarch64 64 cpus|c|BOOT_ONLY=1 SMP=64 TIMEOUT=240 test/aarch64_test.sh"
  "aarch64 gicv2|c|GIC=2 SMP=2 test/aarch64_test.sh"
  "aarch64 el2|c|VIRT=,virtualization=on SMP=2 test/aarch64_test.sh"
  "aarch64 mem|c|MEM=2500M SMP=2 test/aarch64_test.sh"
  "aarch64 nvme|c|NVME=1 SMP=2 test/aarch64_test.sh"
  "aarch64 e1000|c|NIC=e1000 SMP=2 test/aarch64_test.sh"
  "aarch64 virtio-pci|c|VIRTIO_PCI=1 SMP=2 test/aarch64_test.sh"
  "aarch64 nvme e1000|c|NVME=1 NIC=e1000 SMP=2 test/aarch64_test.sh"
  "aarch64 usb|c|USB=1 SMP=2 test/aarch64_test.sh"
  "aarch64 uefi|c|UEFI=/usr/share/qemu-efi-aarch64/QEMU_EFI.fd SMP=2 test/aarch64_test.sh"
  "aarch64 uefi acpi|c|UEFI=/usr/share/qemu-efi-aarch64/QEMU_EFI.fd ACPI=1 NVME=1 NIC=e1000 SMP=2 test/aarch64_test.sh"
  "fsck|c|test/fsck_test.py"
  "crashsim|c|test/crashsim.py --runs 1 --boots 8 --every 8"
  "shell|a|test/shell_test.sh"
  "guide|c|test/guide_test.py"
  "tcp virtio|b|test/tcp_test.sh"
  "tcp e1000|b|NIC=e1000 test/tcp_test.sh"
  "tcp igb|b|NIC=igb test/tcp_test.sh"
  "ipv6 virtio|b|test/ipv6_test.sh"
  "ipv6 e1000|b|NIC=e1000 test/ipv6_test.sh"
  "dhcp virtio|b|test/dhcp_test.sh"
  "dhcp e1000|c|NIC=e1000 test/dhcp_test.sh"
  "iso|c|test/iso_test.sh"
  "nmi|c|test/nmi_test.sh"
  "stackguard|c|test/stackguard_test.sh"
  "stackdepth|c|test/stackdepth.py"
  "fuzz|c|test/fuzz_test.sh 1 2 2000"
)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
run() {  # index
  local cmd=${tests[$1]##*|} t0=$SECONDS
  bash -c "$cmd" >"$tmp/$1.out" 2>&1
  echo "$? $((SECONDS - t0))" >"$tmp/$1.status"
}
lane() {  # lane name: its tests, one after another
  local i rest
  for i in "${!tests[@]}"; do
    rest=${tests[$i]#*|}
    [ "${rest%%|*}" = "$1" ] && run "$i"
  done
}
t0=$SECONDS
if [ "$JOBS" = 1 ]; then
  for i in "${!tests[@]}"; do run "$i"; done
else
  lane a & lane b & lane c &
  wait
fi
failed=()
for i in "${!tests[@]}"; do
  name=${tests[$i]%%|*}
  printf '==== %s (%s)\n' "$name" "${tests[$i]##*|}"
  cat "$tmp/$i.out"
  read -r code secs <"$tmp/$i.status" || { code=lost secs=?; }
  printf '==== %s: exit %s, %s s\n\n' "$name" "$code" "$secs"
  [ "$code" = 0 ] || failed+=("$name")
done
if [ ${#failed[@]} -gt 0 ]; then
  echo "make test: FAILED: ${failed[*]} ($((SECONDS - t0)) s, JOBS=$JOBS)"
  exit 1
fi
echo "make test: all ${#tests[@]} tests passed ($((SECONDS - t0)) s, JOBS=$JOBS)"
