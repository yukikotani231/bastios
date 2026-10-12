#!/usr/bin/env bash
# Lists and counts BastiOS's hand-written trusted base: the C, assembly and
# linker scripts that the proofs do not cover (docs/trusted-base.md says what
# each file is for). The listing is checked into test/trusted_base.txt, so
# growth shows up in a diff and gets reviewed, as bastiolang's
# `bastioc0 --trusted-base` does for the compiler's preludes.
#
# usage: test/trusted_base.sh            compare with test/trusted_base.txt
#        test/trusted_base.sh --update   rewrite test/trusted_base.txt
set -eu
cd "$(dirname "$0")/.."

# Everything under boot/ (boot/aarch64 for the AArch64 port) is trusted by
# the kernel. user/start.S, user/start-aarch64.S and user/user.ld are trusted only by the proofs of the user programs written in
# Bastio: the kernel trusts no user code. The other files under user/ are
# test programs that run as untrusted user processes.
list() {
  echo "# The trusted base of BastiOS: non-blank lines per file. Regenerate with"
  echo "# \`test/trusted_base.sh --update\` and review the diff."
  total=0
  for f in $(find boot -type f | sort) user/start.S user/start-aarch64.S user/user.ld; do
    n=$(grep -c . "$f" || true)
    total=$((total + n))
    echo "$f: $n lines"
  done
  echo "total: $total lines"
}

if [ "${1:-}" = --update ]; then
  list > test/trusted_base.txt
  echo "wrote test/trusted_base.txt"
elif list | diff -u test/trusted_base.txt -; then
  echo "PASS trusted base unchanged"
else
  echo "FAIL trusted base changed; review it, then: test/trusted_base.sh --update"
  exit 1
fi
