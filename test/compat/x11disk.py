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

    x11disk.py OUT.img SERVER_IMAGE CLIENT_IMAGE GTK_IMAGE QT_IMAGE ... PY_IMAGE TAG_IMAGE DIRSTAT_IMAGE FILEZILLA_IMAGE AVIDEMUX_IMAGE HANDBRAKE_IMAGE MKV_IMAGE BUSYBOX BX11 FAKEBWM X11START --cache DIR

The images are Docker Hub images, NAME@sha256:DIGEST (images.mk pins them),
fetched and cached as alpinedisk.py does it.
"""
import copy, io, os, sys, tarfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import alpinedisk

def keep(name):
    # opt/ is the GUI base image's own tools (a web server, noVNC): the programs do not need them
    # cache/ is a package manager's download cache: the files of the image that
    # were hard-linked to it are written out as files (see write_tar)
    return name != "cache" and not name.startswith(("opt/", "cache/")) and "/share/doc/" not in name and "/share/man/" not in name and not name.startswith("usr/share/man/")

def write_tar(files, path, only=None):
    """Writes the files that keep() allows to the tar file path; only, if given,
    says which of them (by name) go into this file."""
    with tarfile.open(path, "w") as t:
        for name, (ti, body) in sorted(files.items()):
            if not keep(name) or (only is not None and not only(name)):
                continue
            if ti.islnk() and ti.linkname.lstrip("./") in files:
                # a hard link to a file that is left off: write a copy
                body = files[ti.linkname.lstrip("./")][1]
                ti = copy.copy(ti)
                ti.type, ti.linkname, ti.size = tarfile.REGTYPE, "", len(body)
            if ti.isfile():
                t.addfile(ti, io.BytesIO(body))
            else:
                t.addfile(ti)

def main():
    args = sys.argv[1:]
    cache = None
    if "--cache" in args:
        cache = args[args.index("--cache") + 1]
        del args[args.index("--cache"):args.index("--cache") + 2]
    out, server, client, gtk, qt, qt2, java, wx, py, tag, dirstat, filezilla, avidemux, handbrake, mkv, busybox, bx11, fakebwm, x11start = args
    work = os.path.join(os.path.dirname(os.path.abspath(out)), "x11")
    cache = cache or work
    os.makedirs(cache, exist_ok=True)
    os.makedirs(work, exist_ok=True)
    for image, tar, want in ((server, "server.tar", "usr/bin/Xvfb"), (client, "client.tar", "usr/bin/xterm"), (gtk, "gtk.tar", "usr/bin/putty"), (qt, "qt.tar", "usr/bin/mediainfo-gui"), (qt2, "qt2.tar", "usr/bin/keepassxc"), (java, "java.tar", "usr/bin/java"), (wx, "wx.tar", "usr/bin/audacity"), (py, "py.tar", "usr/bin/dupeguru"), (tag, "tag.tar", "usr/bin/puddletag"), (dirstat, "dirstat.tar", "usr/bin/qdirstat"), (filezilla, "filezilla.tar", "usr/bin/filezilla"), (avidemux, "avidemux.tar", "usr/bin/avidemux3_qt5"), (handbrake, "handbrake.tar", "usr/bin/ghb"), (mkv, "mkv.tar", "usr/bin/mkvtoolnix-gui")):
        files = alpinedisk.merged(alpinedisk.layers(image, cache))
        if want not in files:
            sys.exit(f"x11disk: {image} has no {want}")
        if tar == "tag.tar":
            # puddletag's root is 640 MB; as one file it ended in "tar: short read"
            # in CI, so its biggest libraries are a file of their own (tag2.tar)
            big = lambda n: n.startswith(("usr/lib/libLLVM", "usr/lib/libgallium"))
            write_tar(files, os.path.join(work, tar), lambda n: not big(n))
            write_tar(files, os.path.join(work, "tag2.tar"), big)
            continue
        write_tar(files, os.path.join(work, tar))
    env = dict(os.environ, MKDISK_FREE_MB="1400")
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
                               f"mkv.tar={os.path.join(work, 'mkv.tar')}"], env)

main()
