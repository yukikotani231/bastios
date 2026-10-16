#!/usr/bin/env bash
# Fuzzes boot/multiboot.c on the host (test/multiboot_fuzz.c): the real
# file, built with AddressSanitizer off but with a fake identity map whose
# guard pages catch any read outside the loader's information.
set -eu
cd "$(dirname "$0")/.."
mkdir -p build
cc -O1 -g -no-pie -fno-pie -Wall -Wextra -Wno-clobbered \
   -o build/multiboot_fuzz test/multiboot_fuzz.c boot/multiboot.c
for seed in 1 2 3; do build/multiboot_fuzz "$seed" 20000; done
