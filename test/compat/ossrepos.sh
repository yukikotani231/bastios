#!/bin/sh
# Real open-source projects for the compat suite to clone, build and test on
# BastiOS: each is a pinned release, fetched once and made into a git
# repository of one commit that the host web server serves for git's dumb
# HTTP protocol (OSS/NAME.git; alpine.txt's HOSTURL/oss/NAME.git).
# Usage: ossrepos.sh OUTDIR
set -e
out=$1
mkdir -p "$out"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

mk() {  # NAME URL TAG-OR-COMMIT
  [ -d "$out/$1.git" ] && return 0
  mkdir "$work/$1"
  git -C "$work/$1" init -q
  git -C "$work/$1" fetch -q --depth 1 "$2" "$3"
  git -C "$work/$1" checkout -q FETCH_HEAD
  rm -rf "$work/$1/.git"
  git -C "$work/$1" -c init.defaultBranch=main init -q
  git -C "$work/$1" add -A
  git -C "$work/$1" -c user.name=compat -c user.email=compat@example.invalid commit -q -m "$1 $3"
  git clone -q --bare "$work/$1" "$out/$1.git"
  git -C "$out/$1.git" update-server-info
}

mk zlib https://github.com/madler/zlib v1.3.1
mk lua https://github.com/lua/lua v5.4.7
mk lz4 https://github.com/lz4/lz4 v1.10.0

# The same for the toolchain disks that have no git: a tarball of a release
# (OSS/NAME.tar.gz, with the files under NAME/), fetched with wget.
tb() {  # NAME URL TAG
  [ -f "$out/$1.tar.gz" ] && return 0
  mkdir "$work/tb-$1"
  git -C "$work/tb-$1" init -q
  git -C "$work/tb-$1" fetch -q --depth 1 "$2" "$3"
  git -C "$work/tb-$1" archive --prefix="$1/" -o "$work/$1.tar" FETCH_HEAD
  gzip -n -c "$work/$1.tar" > "$out/$1.tar.gz"
}

tb toml https://github.com/BurntSushi/toml v1.4.0

# A Rust crate with what cargo needs to build and test it offline: its
# dependencies vendored beside it.
rs() {  # NAME URL TAG
  [ -f "$out/$1.tar.gz" ] && return 0
  mkdir "$work/rs-$1"
  git -C "$work/rs-$1" init -q
  git -C "$work/rs-$1" fetch -q --depth 1 "$2" "$3"
  git -C "$work/rs-$1" checkout -q FETCH_HEAD
  rm -rf "$work/rs-$1/.git"
  mkdir "$work/rs-$1/.cargo"
  (cd "$work/rs-$1" && cargo vendor --versioned-dirs vendor > .cargo/config.toml 2>/dev/null)
  sed -i "s#directory = \".*\"#directory = \"vendor\"#" "$work/rs-$1/.cargo/config.toml"
  mkdir "$work/rs-top-$1"
  mv "$work/rs-$1" "$work/rs-top-$1/$1"
  tar -C "$work/rs-top-$1" -cf - "$1" | gzip -n -c > "$out/$1.tar.gz"
}

rs semver https://github.com/dtolnay/semver 1.0.23

tb nanoid https://github.com/ai/nanoid 5.0.7
