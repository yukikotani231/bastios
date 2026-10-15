#!/bin/sh
# Source tarballs for the "build from source" cases of alpine.txt: fetch each
# release from Ubuntu's archive (a mirror of the upstream tarballs), check
# its SHA-256, and put it in DIR (default build/compat/srcpkg), which run.py
# serves at HOSTURL/src. SQLite is shipped as its amalgamation (sqlite3.c,
# shell.c), as upstream does for its "autoconf" release: generating it needs
# tclsh, which the Alpine guest does not have.
set -eu
dir=${1:-build/compat/srcpkg}
base=https://archive.ubuntu.com/ubuntu/pool/main
mkdir -p "$dir"
fetch() { # file sha256 path
    if [ -f "$dir/$1" ] && echo "$2  $dir/$1" | sha256sum -c - >/dev/null 2>&1; then return; fi
    curl -fsSL --retry 3 -o "$dir/$1.part" "$base/$3"
    echo "$2  $dir/$1.part" | sha256sum -c - >/dev/null
    mv "$dir/$1.part" "$dir/$1"
}
fetch busybox-1.37.0.tar.bz2 3311dff32e746499f4df0d5df04d7eb396382d7e108bb9250e7b519b837043a4 b/busybox/busybox_1.37.0.orig.tar.bz2
fetch zlib-1.3.1.tar.gz 60dd315c07f616887caa029408308a018ace66e3d142726a97db164b3b8f69fb z/zlib/zlib_1.3.dfsg+really1.3.1.orig.tar.gz
fetch lua-5.4.7.tar.gz 9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30 l/lua5.4/lua5.4_5.4.7.orig.tar.gz
fetch git-2.53.0.tar.xz 5818bd7d80b061bbbdfec8a433d609dc8818a05991f731ffc4a561e2ca18c653 g/git/git_2.53.0.orig.tar.xz
fetch curl-8.18.0.tar.gz e9274a5f8ab5271c0e0e6762d2fce194d5f98acc568e4ce816845b2dcc0cf88f c/curl/curl_8.18.0.orig.tar.gz
if [ ! -f "$dir/sqlite-amalgamation.tar.gz" ]; then
    fetch sqlite3-3.53.4.tar.xz 05366adc8ee80c6b1faa71285b49d133c2121238ca58934b077fdeb95934abd4 s/sqlite3/sqlite3_3.53.4.orig.tar.xz
    tmp=$(mktemp -d)
    tar xJf "$dir/sqlite3-3.53.4.tar.xz" -C "$tmp" --strip-components=1
    (cd "$tmp" && ./configure --disable-tcl >/dev/null && make -s sqlite3.c shell.c >/dev/null)
    mkdir "$tmp/out" && cp "$tmp/sqlite3.c" "$tmp/shell.c" "$tmp/sqlite3.h" "$tmp/sqlite3ext.h" "$tmp/out/"
    tar czf "$dir/sqlite-amalgamation.tar.gz" -C "$tmp/out" .
    rm -rf "$tmp"
fi
