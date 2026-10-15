#!/usr/bin/env bash
# One-command setup for working on BastiOS: installs the tools the build and
# the tests need, checks out bastiolang next to this repository if it is not
# there, builds its compilers, then builds the kernel and boots it once.
#
# usage: tools/setup-dev.sh [--no-install]
#   --no-install   do not touch the system; only check for the tools and build
#
# Supported: Debian and Ubuntu (apt). On another system install z3, a C
# compiler, make, git, Python 3, QEMU (qemu-system-x86_64), musl-gcc and
# rustup yourself and run this with --no-install.
set -euo pipefail
cd "$(dirname "$0")/.."

install=1
[ "${1:-}" = "--no-install" ] && install=0

as_root() { if [ "$(id -u)" = 0 ]; then "$@"; else sudo "$@"; fi; }

if [ "$install" = 1 ]; then
  if ! command -v apt-get >/dev/null; then
    echo "setup-dev: apt-get not found; install the tools by hand and use --no-install" >&2
    exit 1
  fi
  echo "== installing packages (apt)"
  as_root apt-get update -q
  as_root apt-get install -y -q z3 build-essential qemu-system-x86 python3 musl-tools git curl ca-certificates
fi

if ! command -v cargo >/dev/null && [ -x "$HOME/.cargo/bin/cargo" ]; then
  export PATH="$HOME/.cargo/bin:$PATH"
fi
if ! command -v cargo >/dev/null; then
  if [ "$install" = 0 ]; then
    echo "setup-dev: cargo not found; install rustup from https://rustup.rs" >&2
    exit 1
  fi
  echo "== installing rustup"
  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal --default-toolchain none
  export PATH="$HOME/.cargo/bin:$PATH"
fi

missing=0
for tool in z3 cc python3 git make qemu-system-x86_64 musl-gcc; do
  command -v "$tool" >/dev/null || { echo "setup-dev: missing tool: $tool" >&2; missing=1; }
done
[ "$missing" = 0 ] || exit 1

# BastiOS is built by bastiolang's compiler, which the Makefile looks for in
# ../bastiolang (or BASTIOC0).
if [ -z "${BASTIOC0:-}" ] && [ ! -d ../bastiolang ]; then
  echo "== cloning bastiolang next to this repository"
  git clone https://github.com/yukikotani231/bastiolang ../bastiolang
fi
if [ -z "${BASTIOC0:-}" ]; then
  echo "== building bastiolang's stage0 (rustup installs the pinned Rust release on first use)"
  (cd ../bastiolang/stage0 && cargo build --release --locked)
fi

echo "== building the kernel (proves every source file; the first build also builds BusyBox)"
make -j"$(nproc)"

echo "== booting it once"
# QEMU's -display none keeps this usable on a machine without a screen.
printf 'hello\nhalt\n' | timeout 120 make run 2>&1 | tail -15 || true

cat <<'MSG'

Done. Next:
  make run          boot BastiOS in QEMU (Ctrl-A X quits)
  make test         boots the kernel and drives the shell
  docs/getting-started.md   the guided tour
  docs/architecture.md      where the kernel's parts live
  CONTRIBUTING.md   the workflow
MSG
