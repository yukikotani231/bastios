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

    x11disk.py OUT.img SERVER_IMAGE CLIENT_IMAGE GTK_IMAGE QT_IMAGE BUSYBOX BX11 FAKEBWM X11START --cache DIR

The images are Docker Hub images, NAME@sha256:DIGEST (images.mk pins them),
fetched and cached as alpinedisk.py does it.
"""
import io, os, sys, tarfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import alpinedisk

def keep(name):
    # opt/ is the GUI base image's own tools (a web server, noVNC): the programs do not need them
    return not name.startswith("opt/") and "/share/doc/" not in name and "/share/man/" not in name and not name.startswith("usr/share/man/")

def write_tar(files, path):
    with tarfile.open(path, "w") as t:
        for name, (ti, body) in sorted(files.items()):
            if not keep(name):
                continue
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
    out, server, client, gtk, qt, qt2, java, busybox, bx11, fakebwm, x11start = args
    work = os.path.join(os.path.dirname(os.path.abspath(out)), "x11")
    cache = cache or work
    os.makedirs(cache, exist_ok=True)
    os.makedirs(work, exist_ok=True)
    for image, tar, want in ((server, "server.tar", "usr/bin/Xvfb"), (client, "client.tar", "usr/bin/xterm"), (gtk, "gtk.tar", "usr/bin/putty"), (qt, "qt.tar", "usr/bin/mediainfo-gui"), (qt2, "qt2.tar", "usr/bin/keepassxc"), (java, "java.tar", "usr/bin/java")):
        files = alpinedisk.merged(alpinedisk.layers(image, cache))
        if want not in files:
            sys.exit(f"x11disk: {image} has no {want}")
        write_tar(files, os.path.join(work, tar))
    env = dict(os.environ, MKDISK_FREE_MB="1200")
    os.execve(sys.executable, [sys.executable, os.path.join(HERE, "mkdisk.py"), out,
                               f"busybox={busybox}", f"bx11={bx11}", f"fakebwm={fakebwm}", f"x11start={x11start}",
                               f"server.tar={os.path.join(work, 'server.tar')}",
                               f"client.tar={os.path.join(work, 'client.tar')}",
                               f"gtk.tar={os.path.join(work, 'gtk.tar')}",
                               f"qt.tar={os.path.join(work, 'qt.tar')}",
                               f"qt2.tar={os.path.join(work, 'qt2.tar')}",
                               f"java.tar={os.path.join(work, 'java.tar')}"], env)

main()
