#!/bin/sh
# Packs git's own test suite (t/) with the build it tests, for the gitsuite
# lane: gitsuite.sh OUT.tar GITSRC. GITSRC is a git checkout built with
# `make all t/helper/test-tool bin-wrappers/git GIT-BUILD-OPTIONS` and
# templates (see the build/compat/gitsuite.tar rule in the Makefile). The
# tar unpacks to gt/ and runs from /gt in the guest: paths in the wrappers
# and GIT-BUILD-OPTIONS are rewritten, programs git built as copies of the
# `git` binary become links to it, and tests that need a network service
# (t9*) and the performance tests are left out.
set -e
out=$1; src=$(cd "$2" && pwd)
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
gt=$stage/gt
mkdir -p "$gt/t/helper" "$gt/templates" "$gt/bin-wrappers"
cp "$src/git" "$gt/git"
for f in "$src"/git-*; do
    b=$(basename "$f")
    [ -f "$f" ] || continue
    if [ "$(stat -c %i "$f")" = "$(stat -c %i "$src/git")" ]; then ln -s git "$gt/$b"; else cp "$f" "$gt/$b"; fi
done
mkdir -p "$gt/contrib" && cp -r "$src/contrib/workdir" "$gt/contrib/workdir" && cp -r "$src/mergetools" "$gt/mergetools"
cp "$src/t/helper/test-tool" "$src/t/helper/test-fake-ssh" "$gt/t/helper/"
cp -r "$src/templates/blt" "$gt/templates/blt"
(cd "$src/t" && tar -cf - --exclude='t9*' --exclude=perf --exclude='test-results' --exclude='trash directory*' \
    --exclude='*.o' --exclude=helper --exclude=.prove . ) | (cd "$gt/t" && tar -xf -)
sed "s,$src,/gt,g" "$src/GIT-BUILD-OPTIONS" > "$gt/GIT-BUILD-OPTIONS"
for f in "$src"/bin-wrappers/*; do
    sed "s,$src,/gt,g" "$f" > "$gt/bin-wrappers/$(basename "$f")"
    chmod +x "$gt/bin-wrappers/$(basename "$f")"
done
tar -C "$stage" -cf "$out" gt
