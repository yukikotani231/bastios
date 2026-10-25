#!/bin/sh
# One build-and-tooling workload, run inside the guest by bench/workloads.py:
#
#   set -- BUSYBOX PHASE; . /disk/wl.sh
#
# BUSYBOX is the BusyBox that works outside the Alpine root (BastiOS: the
# Alpine one on the disk; Linux: the one in the initramfs). Everything past
# `chroot` is Alpine's own programs, the same binaries on both systems.
# The program prints "@begin PHASE" and "@end PHASE STATUS" around the timed
# part; the host stamps the lines as they arrive on the serial port.
bb=$1
phase=$2
R=/disk/a
chr() { $bb chroot $R "$@"; }

case $phase in
untar)
    # Unpack Alpine's mini root file system: many small files created.
    cd $R || return 1
    echo @begin untar
    $bb tar -xf /disk/root.tar
    echo @end untar $?
    for d in null:1:3 zero:1:5 random:1:8 urandom:1:9 tty:5:0 console:5:1; do
        set -- $(echo $d | $bb tr : ' ')
        $bb mknod dev/$1 c $2 $3
    done
    echo /repo > etc/apk/repositories
    ;;
apk)
    # apk installs gcc, make and musl's headers from the local repository:
    # unpack, checksum and write a few thousand files.
    echo @begin apk
    chr /sbin/apk add --allow-untrusted -q --no-progress gcc musl-dev make > /dev/null 2>&1
    echo @end apk $?
    ;;
hello)
    # Compile and link one small C file at -O2 (gcc, cc1, as, ld).
    $bb tar -xf /disk/proj.tar -C $R/root 2>/dev/null
    echo @begin hello
    chr /bin/sh -c 'cd /root/proj && gcc -O2 -o hello hello.c'
    echo @end hello $?
    ;;
hello_*)
    # The stages of `hello`, ten times each so that a stage is long enough
    # to time: the preprocessor, cc1, as, the link, and a bare exec.
    $bb tar -xf /disk/proj.tar -C $R/root 2>/dev/null
    case $phase in
    hello_e) cmd='gcc -E -o /tmp/h.i hello.c' ;;
    hello_s) cmd='gcc -O2 -S -o /tmp/h.s hello.c' ;;
    hello_c) cmd='gcc -O2 -c -o /tmp/h.o hello.c' ;;
    hello_x) cmd='/bin/true' ;;
    esac
    [ $phase = hello_l ] && chr /bin/sh -c 'cd /root/proj && gcc -O2 -c -o /tmp/h.o hello.c'
    [ $phase = hello_l ] && cmd='gcc -o /tmp/h /tmp/h.o'
    echo @begin $phase
    chr /bin/sh -c "cd /root/proj && for i in 1 2 3 4 5 6 7 8 9 10; do $cmd; done"
    echo @end $phase $?
    ;;
make1|make2)
    # make builds the generated project: one gcc per file, then a link.
    j=${phase#make}
    $bb rm -rf $R/root/proj
    $bb tar -xf /disk/proj.tar -C $R/root
    echo @begin $phase
    chr /bin/sh -c "cd /root/proj && make -s -j$j"
    echo @end $phase $?
    ;;
tgz)
    # Read a large tree, compress it, write the archive: file I/O and CPU.
    echo @begin tgz
    chr /bin/sh -c 'tar -cf - /usr/lib/gcc /usr/include | gzip -1 > /tmp/x.tgz'
    echo @end tgz $?
    $bb rm -f $R/tmp/x.tgz
    ;;
git)
    # git over the generated project: init, add, commit, status, log.
    chr /sbin/apk add --allow-untrusted -q --no-progress git > /dev/null 2>&1
    $bb rm -rf $R/root/proj
    $bb tar -xf /disk/proj.tar -C $R/root
    echo @begin git
    chr /bin/sh -c 'cd /root/proj && git init -q . && git add -A && git -c user.name=b -c user.email=b@b commit -q -m x && git status -s && git log --stat | wc -l > /dev/null'
    echo @end git $?
    ;;
python)
    # CPython: five starts that import a spread of the standard library
    # (many small reads, compiling the modules once), then a compute loop.
    chr /sbin/apk add --allow-untrusted -q --no-progress python3 > /dev/null 2>&1
    echo @begin python
    chr /bin/sh -c 'for i in 1 2 3 4 5; do python3 -c "import json, re, argparse, decimal, http.client, email.parser"; done; python3 -c "print(sum(i * i % 7 for i in range(2000000)))" > /dev/null'
    echo @end python $?
    ;;
scan)
    # Walk the whole root and grep the headers: path lookup and small reads.
    echo @begin scan
    chr /bin/sh -c 'find / -xdev -type f | wc -l > /dev/null; grep -rl stdio /usr/include | wc -l > /dev/null'
    echo @end scan $?
    ;;
esac
