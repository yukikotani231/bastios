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
mk cjson https://github.com/DaveGamble/cJSON v1.7.18

# A release tarball (its generated configure script included) made into the
# same kind of repository.
rel() {  # NAME URL
  [ -d "$out/$1.git" ] && return 0
  mkdir "$work/$1"
  curl -fsSL "$2" | tar xz -C "$work/$1" --strip-components=1
  git -C "$work/$1" -c init.defaultBranch=main init -q
  git -C "$work/$1" add -A
  git -C "$work/$1" -c user.name=compat -c user.email=compat@example.invalid commit -q -m "$1 release"
  git clone -q --bare "$work/$1" "$out/$1.git"
  git -C "$out/$1.git" update-server-info
}

rel libyaml https://github.com/yaml/libyaml/releases/download/0.2.5/yaml-0.2.5.tar.gz

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
  mkdir -p "$work/rs-$1/.cargo"
  (cd "$work/rs-$1" && cargo vendor --versioned-dirs vendor > .cargo/config.toml 2>/dev/null)
  sed -i "s#directory = \".*\"#directory = \"vendor\"#" "$work/rs-$1/.cargo/config.toml"
  # The Windows-only crates are the bulk of a vendor directory and never built
  # here: keep their manifests (cargo resolves every target) and empty them.
  for d in "$work/rs-$1"/vendor/windows* "$work/rs-$1"/vendor/winapi*; do
    [ -d "$d" ] || continue
    pkg=$(sed -n 's/.*"package":"\([0-9a-f]*\)".*/\1/p' "$d/.cargo-checksum.json")
    find "$d" -mindepth 1 ! -name Cargo.toml -delete 2>/dev/null || true
    mkdir -p "$d/src" && : > "$d/src/lib.rs"
    printf '{"files":{},"package":"%s"}' "$pkg" > "$d/.cargo-checksum.json"
  done
  mkdir "$work/rs-top-$1"
  mv "$work/rs-$1" "$work/rs-top-$1/$1"
  tar -C "$work/rs-top-$1" -cf - "$1" | gzip -n -c > "$out/$1.tar.gz"
}

rs semver https://github.com/dtolnay/semver 1.0.23
rs fastrand https://github.com/smol-rs/fastrand v2.3.0
rs itoa https://github.com/dtolnay/itoa 1.0.11
rs either https://github.com/rayon-rs/either 1.13.0
rs smallvec https://github.com/servo/rust-smallvec v1.13.2

# A Go module with its dependencies vendored beside it (go build -mod=vendor).
gv() {  # NAME URL TAG
  [ -f "$out/$1.tar.gz" ] && return 0
  mkdir "$work/gv-$1"
  git -C "$work/gv-$1" init -q
  git -C "$work/gv-$1" fetch -q --depth 1 "$2" "$3"
  git -C "$work/gv-$1" checkout -q FETCH_HEAD
  rm -rf "$work/gv-$1/.git"
  (cd "$work/gv-$1" && go mod vendor)
  mkdir "$work/gv-top-$1"
  mv "$work/gv-$1" "$work/gv-top-$1/$1"
  tar -C "$work/gv-top-$1" -cf - "$1" | gzip -n -c > "$out/$1.tar.gz"
}

gv testify https://github.com/stretchr/testify v1.9.0
gv fzf https://github.com/junegunn/fzf v0.58.0
gv websocket https://github.com/gorilla/websocket v1.5.3
gv uuid https://github.com/google/uuid v1.6.0
gv pflag https://github.com/spf13/pflag v1.0.5
gv mux https://github.com/gorilla/mux v1.8.1
gv gjson https://github.com/tidwall/gjson v1.18.0

# An npm package built on the host and shipped with its dependencies and one
# test runner (or several, separated by spaces) installed beside it (node_modules), nothing else of its tooling.
nm() {  # NAME URL TAG RUNNER
  [ -f "$out/$1.tar.gz" ] && return 0
  mkdir "$work/nm-$1"
  git -C "$work/nm-$1" init -q
  git -C "$work/nm-$1" fetch -q --depth 1 "$2" "$3"
  git -C "$work/nm-$1" checkout -q FETCH_HEAD
  rm -rf "$work/nm-$1/.git"
  (cd "$work/nm-$1" && npm install --ignore-scripts --no-audit --no-fund >/dev/null 2>&1 && npm run build --if-present >/dev/null 2>&1 \
    && npm pkg delete devDependencies scripts >/dev/null && rm -rf node_modules package-lock.json \
    && npm install --ignore-scripts --omit=dev --no-audit --no-fund >/dev/null 2>&1 \
    && npm install --no-save --ignore-scripts --no-audit --no-fund $4 >/dev/null 2>&1)
  mkdir "$work/nm-top-$1"
  mv "$work/nm-$1" "$work/nm-top-$1/$1"
  tar -C "$work/nm-top-$1" -cf - "$1" | gzip -n -c > "$out/$1.tar.gz"
}

nm mri https://github.com/lukeed/mri v1.2.0 tape@4.13.3
nm debug https://github.com/debug-js/debug 4.3.7 "mocha@10.8.2 sinon@14.0.2"
nm express https://github.com/expressjs/express 4.21.1 "mocha@10.2.0 supertest@6.3.0 after@0.8.2 connect-redis@3.4.2 cookie-parser@1.4.6 cookie-session@2.0.0 ejs@3.1.9 express-session@1.17.2 hbs@4.2.0 marked@0.7.0 method-override@3.0.0 morgan@1.10.0 pbkdf2-password@1.2.1 vhost@3.0.2"
nm ws https://github.com/websockets/ws 8.18.0 "mocha@10.2.0"
nm minimist https://github.com/minimistjs/minimist v1.2.8 tape@5.9.0
nm qs https://github.com/ljharb/qs v6.13.1 "tape@5.9.0 es-value-fixtures@1.5.0 for-each@0.3.3 has-bigints@1.0.2 has-override-mistake@1.0.1 has-property-descriptors@1.0.2 has-proto@1.0.3 has-symbols@1.0.3 iconv-lite@0.5.1 mock-property@1.1.0 object-inspect@1.13.3 qs-iconv@1.0.4 safer-buffer@2.1.2 mkdirp@0.5.5 glob@10.3.7"
nm cookie https://github.com/jshttp/cookie v0.7.2 "mocha@10.2.0 safe-buffer@5.2.1"
nm send https://github.com/pillarjs/send 0.19.0 "mocha@9.2.2 supertest@6.2.2 after@0.8.2"
nm body-parser https://github.com/expressjs/body-parser 1.20.3 "mocha@10.2.0 supertest@6.3.3 methods@1.1.2 safe-buffer@5.2.1"
nm async https://github.com/caolan/async v3.2.6 "mocha@6.2.3 babel-register@6.26.0 babel-plugin-transform-es2015-modules-commonjs@6.26.2 babel-plugin-syntax-async-generators@6.13.0 chai@4.3.10 bluebird@3.7.2 es6-promise@4.2.8 lodash@4.17.21 rsvp@4.8.5 native-promise-only@0.8.1"

tb nanoid https://github.com/ai/nanoid 5.0.7
