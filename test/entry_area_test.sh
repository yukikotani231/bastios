#!/usr/bin/env bash
# Checks the entry area boot/platform.c builds (pages user-mode traps can
# reach, IDT gates) on the host: test/entry_area_check.c.
set -eu
cd "$(dirname "$0")/.."
mkdir -p build
cc -O1 -Wall -Wextra -no-pie -fno-pie -o build/entry_area test/entry_area_check.c
build/entry_area
