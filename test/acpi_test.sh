#!/usr/bin/env bash
# Fuzzes the ACPI search in boot/smp.c on the host (test/acpi_fuzz.c): the
# real file against random and lying tables in a fake identity map.
set -eu
cd "$(dirname "$0")/.."
mkdir -p build
cc -O1 -g -no-pie -fno-pie -Wall -Wextra -Wno-clobbered -Iboot \
   -o build/acpi_fuzz test/acpi_fuzz.c boot/smp.c
for seed in 1 2 3; do build/acpi_fuzz "$seed" 20000; done
