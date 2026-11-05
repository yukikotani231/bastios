#!/usr/bin/env python3
"""Writes the X11 disk for the compat suite (test/compat/x11.txt): the root of
an Alpine image with the Xvfb X server, the root of one with xterm and the
root of one with PuTTY, a GTK 3 program, and the root of one with MediaInfo, a
Qt 5 program, each as a tar file (/disk/server.tar, /disk/client.tar,
/disk/gtk.tar, /disk/qt.tar), a BusyBox at /disk/busybox and
bx11 and fakebwm (the program that connects X to bwm, and a stand-in for bwm)
and x11start (the script that starts them all).
The checks unpack the roots under /disk/x, /disk/t, /disk/g and /disk/q, start Xvfb in the first
and xterm in the second (which share the X socket through a bind mount).

    x11disk.py OUT.img SERVER_IMAGE CLIENT_IMAGE GTK_IMAGE QT_IMAGE ... PY_IMAGE TAG_IMAGE DIRSTAT_IMAGE FILEZILLA_IMAGE AVIDEMUX_IMAGE HANDBRAKE_IMAGE MKV_IMAGE CZKAWKA_IMAGE TSMUXER_IMAGE FILEBOT_IMAGE FFS_IMAGE KRUSADER_IMAGE EVINCE_IMAGE VLC_IMAGE BUSYBOX BX11 FAKEBWM X11START --cache DIR

The images are Docker Hub images, NAME@sha256:DIGEST (images.mk pins them),
fetched and cached as alpinedisk.py does it.
"""
import copy, io, os, sys, tarfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import alpinedisk

def keep(name):
    # opt/ is the GUI base image's own tools (a web server, noVNC): the programs do not need them,
    # but FileBot and FreeFileSync are installed in opt/filebot and opt/FreeFileSync
    # cache/ is a package manager's download cache: the files of the image that
    # were hard-linked to it are written out as files (see write_tar)
    return name != "cache" and (name == "opt" or name.startswith(("opt/filebot", "opt/FreeFileSync")) or not name.startswith("opt/")) and not name.startswith("cache/") and "/share/doc/" not in name and "/share/man/" not in name and not name.startswith("usr/share/man/")

BLOCK, PTRS, DENT = 4096, 512, 256   # src/diskfs.bastio

def disk_bytes(n):
    """What a file of n bytes takes on a BastiOS disk once unpacked: its data
    blocks, its root map and a map per 512 blocks (a small file takes three
    blocks, so a root of many small files takes much more than its tar)."""
    b = (n + BLOCK - 1) // BLOCK
    return 0 if b == 0 else (b + 1 + (b + PTRS - 1) // PTRS) * BLOCK

def write_tar(files, path, only=None):
    """Writes the files that keep() allows to the tar file path; only, if given,
    says which of them (by name) go into this file. Returns how many bytes of
    the disk the files take once the tar is unpacked there (disk_bytes, and
    a directory entry each)."""
    unpacked = 0
    with tarfile.open(path, "w") as t:
        for name, (ti, body) in sorted(files.items()):
            if not keep(name) or (only is not None and not only(name)):
                continue
            if ti.islnk() and ti.linkname.lstrip("./") in files:
                # a hard link to a file that is left off: write a copy
                body = files[ti.linkname.lstrip("./")][1]
                ti = copy.copy(ti)
                ti.type, ti.linkname, ti.size = tarfile.REGTYPE, "", len(body)
            unpacked += DENT
            if ti.isfile():
                t.addfile(ti, io.BytesIO(body))
                unpacked += disk_bytes(len(body))
            else:
                t.addfile(ti)
                if ti.issym():
                    unpacked += disk_bytes(len(ti.linkname))
    return unpacked

def main():
    args = sys.argv[1:]
    cache = None
    if "--cache" in args:
        cache = args[args.index("--cache") + 1]
        del args[args.index("--cache"):args.index("--cache") + 2]
    out, server, client, gtk, qt, qt2, java, wx, py, tag, dirstat, filezilla, avidemux, handbrake, mkv, czkawka, tsmuxer, filebot, ffs, krusader, evince, vlc, busybox, bx11, fakebwm, x11start = args
    work = os.path.join(os.path.dirname(os.path.abspath(out)), "x11")
    cache = cache or work
    os.makedirs(cache, exist_ok=True)
    os.makedirs(work, exist_ok=True)
    unpacked = {}                  # tar -> the bytes its files take on the disk once unpacked
    for image, tar, want in ((server, "server.tar", "usr/bin/Xvfb"), (client, "client.tar", "usr/bin/xterm"), (gtk, "gtk.tar", "usr/bin/putty"), (qt, "qt.tar", "usr/bin/mediainfo-gui"), (qt2, "qt2.tar", "usr/bin/keepassxc"), (java, "java.tar", "usr/bin/java"), (wx, "wx.tar", "usr/bin/audacity"), (py, "py.tar", "usr/bin/dupeguru"), (tag, "tag.tar", "usr/bin/puddletag"), (dirstat, "dirstat.tar", "usr/bin/qdirstat"), (filezilla, "filezilla.tar", "usr/bin/filezilla"), (avidemux, "avidemux.tar", "usr/bin/avidemux3_qt5"), (handbrake, "handbrake.tar", "usr/bin/ghb"), (mkv, "mkv.tar", "usr/bin/mkvtoolnix-gui"), (czkawka, "czkawka.tar", "usr/bin/czkawka_gui"), (tsmuxer, "tsmuxer.tar", "usr/bin/tsMuxerGUI"), (filebot, "filebot.tar", "opt/filebot/filebot"), (ffs, "ffs.tar", "opt/FreeFileSync/Bin/FreeFileSync"), (krusader, "krusader.tar", "usr/bin/krusader"), (evince, "evince.tar", "usr/bin/evince"), (vlc, "vlc.tar", "usr/bin/vlc")):
        files = alpinedisk.merged(alpinedisk.layers(image, cache))
        if want not in files:
            sys.exit(f"x11disk: {image} has no {want}")
        if tar == "krusader.tar":
            # A BastiOS disk holds at most 262144 entries (files, directories
            # and links; DSLOTS in src/diskfs.bastio) and the lane's roots
            # use most of them. Krusader's icon themes and message catalogues
            # are tens of thousands of small files it can start without.
            unpacked[tar] = write_tar(files, os.path.join(work, tar), lambda n: not n.startswith(("usr/share/icons/breeze", "usr/share/locale/")))
            continue
        if tar == "evince.tar":
            # the Adwaita icon theme is 6000 of its 9000 entries (see krusader.tar)
            unpacked[tar] = write_tar(files, os.path.join(work, tar), lambda n: not n.startswith("usr/share/icons/Adwaita"))
            continue
        if tar == "tag.tar":
            # puddletag's root is 640 MB: its biggest libraries are a file of
            # their own (tag2.tar), unpacked after tag.tar is removed, so the
            # case needs less free space at once
            big = lambda n: n.startswith(("usr/lib/libLLVM", "usr/lib/libgallium"))
            unpacked[tar] = write_tar(files, os.path.join(work, tar), lambda n: not big(n))
            unpacked["tag2.tar"] = write_tar(files, os.path.join(work, "tag2.tar"), big)
            continue
        unpacked[tar] = write_tar(files, os.path.join(work, tar))
    # The free space the lane needs: the cases unpack every root and keep it,
    # removing each tar after it is unpacked but server.tar and client.tar.
    # Unpacked, a root takes more than its tar (disk_bytes), and that much
    # more stays taken, case after case. With 900 MB, a fixed figure, the
    # disk filled partway through the puddletag root, and BusyBox tar said
    # "short read". So: what the kept roots take, what every root takes past
    # its tar, the largest root once more (the case unpacking it), and 768 MiB
    # for what the programs write (FileBot and JDownloader keep caches and logs in theirs).
    size = lambda tar: os.path.getsize(os.path.join(work, tar))
    kept = ("server.tar", "client.tar")
    past = sum(max(0, unpacked[t] - size(t)) for t in unpacked if t not in kept)
    free = sum(unpacked[t] for t in kept) + past + max(unpacked[t] for t in unpacked if t not in kept) + (768 << 20)
    env = dict(os.environ, MKDISK_FREE_MB=str((free >> 20) + 1))
    print(f"x11disk: {free >> 20} MiB free for the cases", file=sys.stderr)
    os.execve(sys.executable, [sys.executable, os.path.join(HERE, "mkdisk.py"), out,
                               f"busybox={busybox}", f"bx11={bx11}", f"fakebwm={fakebwm}", f"x11start={x11start}",
                               f"server.tar={os.path.join(work, 'server.tar')}",
                               f"client.tar={os.path.join(work, 'client.tar')}",
                               f"gtk.tar={os.path.join(work, 'gtk.tar')}",
                               f"qt.tar={os.path.join(work, 'qt.tar')}",
                               f"qt2.tar={os.path.join(work, 'qt2.tar')}",
                               f"java.tar={os.path.join(work, 'java.tar')}",
                               f"wx.tar={os.path.join(work, 'wx.tar')}",
                               f"py.tar={os.path.join(work, 'py.tar')}",
                               f"tag.tar={os.path.join(work, 'tag.tar')}",
                               f"tag2.tar={os.path.join(work, 'tag2.tar')}",
                               f"dirstat.tar={os.path.join(work, 'dirstat.tar')}",
                               f"filezilla.tar={os.path.join(work, 'filezilla.tar')}",
                               f"avidemux.tar={os.path.join(work, 'avidemux.tar')}",
                               f"handbrake.tar={os.path.join(work, 'handbrake.tar')}",
                               f"mkv.tar={os.path.join(work, 'mkv.tar')}",
                               f"czkawka.tar={os.path.join(work, 'czkawka.tar')}",
                               f"tsmuxer.tar={os.path.join(work, 'tsmuxer.tar')}",
                               f"filebot.tar={os.path.join(work, 'filebot.tar')}",
                               f"ffs.tar={os.path.join(work, 'ffs.tar')}",
                               f"krusader.tar={os.path.join(work, 'krusader.tar')}",
                               f"evince.tar={os.path.join(work, 'evince.tar')}",
                               f"vlc.tar={os.path.join(work, 'vlc.tar')}"], env)

main()
