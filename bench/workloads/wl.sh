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
scan)
    # Walk the whole root and grep the headers: path lookup and small reads.
    echo @begin scan
    chr /bin/sh -c 'find / -xdev -type f | wc -l > /dev/null; grep -rl stdio /usr/include | wc -l > /dev/null'
    echo @end scan $?
    ;;
esac
