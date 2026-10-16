#!/usr/bin/env bash
# The processor's own random number generator joins the kernel's pool when
# the processor has one (boot/smp.c, boot_info 12): a CPU model with RDSEED
# and RDRAND makes the kernel say so at boot, and the default model, which
# has neither, makes it say nothing.
set -u
cd "$(dirname "$0")/.."
KERNEL=${KERNEL:-build/bastios.elf}
TIMEOUT=${TIMEOUT:-60}

boot() {
    # $1: the CPU model. Prints the boot output up to the first prompt.
    local out
    out=$(mktemp)
    timeout "$TIMEOUT" qemu-system-x86_64 -cpu "$1" -kernel "$KERNEL" -m 128M -smp 1 \
        -display none -no-reboot -nic none -serial stdio -monitor none < /dev/null > "$out" 2>&1
    cat "$out"
    rm -f "$out"
}

fail=0
with=$(boot max)
without=$(boot qemu64)
if ! grep -q "bastios> " <<< "$with"; then
    echo "rng_test: the kernel did not reach its prompt on -cpu max"; fail=1
elif ! grep -q "^rng: the processor's RDSEED/RDRAND, 64 bytes of entropy" <<< "$with"; then
    echo "rng_test: -cpu max (RDSEED, RDRAND) gave the pool no words"; fail=1
else
    echo "rng_test: ok, -cpu max feeds the pool 64 bytes from the processor"
fi
if ! grep -q "bastios> " <<< "$without"; then
    echo "rng_test: the kernel did not reach its prompt on -cpu qemu64"; fail=1
elif grep -q "RDSEED/RDRAND" <<< "$without"; then
    echo "rng_test: -cpu qemu64 has no RDSEED, yet the kernel claims it"; fail=1
else
    echo "rng_test: ok, -cpu qemu64 has no hardware generator and the kernel does not use one"
fi
exit $fail
