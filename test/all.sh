#!/usr/bin/env bash
# `make test`: every test below. Each boots its own QEMU with its own disk
# images and host ports, so tests can run side by side; they run in lanes,
# one test after another within a lane and the lanes at once:
#   a: the shell test, the longest, on 4 processors;
#   b: the TCP and IPv6 tests (the two IPv6 tests share build/ipv6, so
#      they stay in one lane) and one DHCP test;
#   c: everything else but e;
#   e: the installer boot and the crash simulation;
#   f: the ISO boots (BIOS and UEFI, with the window manager's checks), the
#      longest of the rest, a lane of its own so that a runner can take it;
#   d: the AArch64 kernel's boots (they build it first), a lane of their own
#      so that a runner can take them apart from the rest (LANES below).
# Three lanes and not more: the TCP tests time a stalled wire against the
# guest's resend timer, and a test of the shell's checks that a process is
# asleep, so a machine much busier than this makes them fail for the wrong
# reason (c, e and f count as one: they run as one lane unless LANES splits
# them, as CI does on runners of their own). LANES names the lanes to run,
# each group of letters one lane run beside the others (LANES="a b" runs a
# and b; LANES="b c" runs b and c one after the other as a single lane), so
# CI can give each part to a runner of its own; it defaults to "a b cdef".
# With fewer than 4 CPUs, or JOBS=1, every test runs one after another, as
# before. The output of each test is printed whole, in the order below,
# followed by how long it took; the run fails if any test fails. A test that
# runs longer than TEST_TIMEOUT seconds (1800 by default; the longest, iso,
# takes 15 to 25 minutes on a CI runner) is killed with its QEMU, and fails with exit 124 and the
# output it had: a hung test then names itself, where the job's own timeout
# would cancel the whole run and print nothing.
set -u
cd "$(dirname "$0")/.."
JOBS=${JOBS:-$( [ "$(nproc 2>/dev/null || echo 1)" -ge 4 ] && echo 3 || echo 1 )}
# name|lane|command, in the order they are printed
tests=(
  "trusted_base|c|test/trusted_base.sh"
  "multiboot fuzz|c|test/multiboot_test.sh"
  "acpi fuzz|c|test/acpi_test.sh"
  "sysfuzz_calls|c|test/sysfuzz_calls.sh"
  "entry area|c|test/entry_area_test.sh"
  "aarch64|d|test/aarch64_test.sh --build"
  "aarch64 64 cpus|d|BOOT_ONLY=1 SMP=64 TIMEOUT=480 test/aarch64_test.sh"
  "aarch64 gicv2|d|GIC=2 SMP=2 test/aarch64_test.sh"
  "aarch64 el2|d|VIRT=,virtualization=on SMP=2 test/aarch64_test.sh"
  "aarch64 mem|d|MEM=2500M SMP=2 test/aarch64_test.sh"
  "aarch64 nvme|d|NVME=1 SMP=2 test/aarch64_test.sh"
  "aarch64 e1000|d|NIC=e1000 SMP=2 test/aarch64_test.sh"
  "aarch64 virtio-pci|d|VIRTIO_PCI=1 SMP=2 test/aarch64_test.sh"
  "aarch64 nvme e1000|d|NVME=1 NIC=e1000 SMP=2 test/aarch64_test.sh"
  "aarch64 usb|d|USB=1 SMP=2 test/aarch64_test.sh"
  "aarch64 uefi|d|UEFI=/usr/share/qemu-efi-aarch64/QEMU_EFI.fd SMP=2 test/aarch64_test.sh"
  "aarch64 uefi acpi|d|UEFI=/usr/share/qemu-efi-aarch64/QEMU_EFI.fd ACPI=1 NVME=1 NIC=e1000 SMP=2 test/aarch64_test.sh"
  "fsck|c|test/fsck_test.py"
  "crashsim|e|test/crashsim.py --runs 1 --boots 8 --every 8"
  "shell|a|test/shell_test.sh"
  "guide|c|test/guide_test.py"
  "tcp virtio|b|test/tcp_test.sh"
  "tcp e1000|b|NIC=e1000 test/tcp_test.sh"
  "tcp igb|b|NIC=igb test/tcp_test.sh"
  "ipv6 virtio|b|test/ipv6_test.sh"
  "ipv6 e1000|b|NIC=e1000 test/ipv6_test.sh"
  "dhcp virtio|b|test/dhcp_test.sh"
  "dhcp e1000|c|NIC=e1000 test/dhcp_test.sh"
  "iso|f|test/iso_test.sh"
  "install|e|test/install_test.sh"
  "nmi|c|test/nmi_test.sh"
  "stackguard|c|test/stackguard_test.sh"
  "memory|c|test/memory_test.sh"
  "rng|c|test/rng_test.sh"
  "stackdepth|c|test/stackdepth.py"
  "fuzz|c|test/fuzz_test.sh 1 2 2000"
  "soak|c|test/soak.py --rounds 150 --smp 2 --warmup 10 --leak-kb 128"
)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
run() {  # index
  local cmd=${tests[$1]##*|} t0=$SECONDS
  timeout -k 15 "${TEST_TIMEOUT:-1800}" bash -c "$cmd" >"$tmp/$1.out" 2>&1
  echo "$? $((SECONDS - t0))" >"$tmp/$1.status"
}
LANES=${LANES:-a b cdef}
selected() {  # index: is the test in one of the lanes asked for?
  local rest=${tests[$1]#*|} g
  rest=${rest%%|*}
  for g in $LANES; do
    case $g in *"$rest"*) return 0 ;; esac
  done
  return 1
}
lane() {  # group of lane names: its tests, one after another
  local i rest
  for i in "${!tests[@]}"; do
    rest=${tests[$i]#*|}
    rest=${rest%%|*}
    case $1 in *"$rest"*) run "$i" ;; esac
  done
}
t0=$SECONDS
if [ "$JOBS" = 1 ]; then
  for i in "${!tests[@]}"; do selected "$i" && run "$i"; done
else
  for g in $LANES; do lane "$g" & done
  wait
fi
failed=()
n=0
timings=()
for i in "${!tests[@]}"; do
  selected "$i" || continue
  n=$((n + 1))
  name=${tests[$i]%%|*}
  printf '==== %s (%s)\n' "$name" "${tests[$i]##*|}"
  cat "$tmp/$i.out"
  read -r code secs <"$tmp/$i.status" || { code=lost secs=?; }
  printf '==== %s: exit %s, %s s\n\n' "$name" "$code" "$secs"
  [ "$code" = 0 ] || failed+=("$name")
  timings+=("$(printf '%6s s  %s' "$secs" "$name")")
done
echo "==== time per test, longest first"
printf '%s\n' "${timings[@]}" | sort -rn | head -8
if [ ${#failed[@]} -gt 0 ]; then
  echo "make test: FAILED: ${failed[*]} ($((SECONDS - t0)) s, JOBS=$JOBS)"
  exit 1
fi
echo "make test: all $n tests passed ($((SECONDS - t0)) s, JOBS=$JOBS)"
