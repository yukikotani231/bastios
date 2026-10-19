# Using BastiOS

This guide is for trying BastiOS as a system rather than working on it: booting it, logging in, adding users, using the network, logging in over SSH, and installing Alpine Linux packages with `apk`. [Getting started](getting-started.md) covers building it and writing programs for it.

BastiOS is experimental. Everything below has been tried in QEMU on x86-64 Linux; it has not yet been run on a real PC. What does not work yet is listed [at the end](#what-does-not-work-yet).

## 1. Boot

Build it as [Getting started](getting-started.md#1-build) says, then:

```text
make run
```

QEMU boots the kernel on your terminal, with a disk image (`build/disk.img`, kept between boots) and a network card on QEMU's user network. Ctrl-A X quits QEMU at any time; `halt` powers off cleanly.

The first prompt, `bastios>`, is BastiOS's own shell, a small program written in Bastio; `help` lists its commands. For a Unix shell, type `ash`: that is BusyBox's shell, an unmodified Linux program, and the rest of this guide uses it. `exit` takes you back.

<!-- guide-test: session -->
```text
bastios> ifconfig
10.0.2.15 netmask 255.255.255.0 gateway 10.0.2.2 (DHCP)
bastios> ping
reply from 10.0.2.2
bastios> ash
...
# id
uid=0(root) gid=0(root)
# cat /etc/resolv.conf
nameserver 10.0.2.3
# exit
bastios> halt
bye
...
```

You start as root, with no password. `/` and everything outside `/disk` is kept in memory and is gone at power off; `/disk` is the disk image, and keeps what you write there.

## 2. More tools

The BusyBox built into the kernel has only a shell and the basic file tools. A fuller BusyBox (with `wget`, `vi`, `nc`, `httpd`, `tar`, `awk` and about eighty other tools) and the Dropbear SSH server are built from source by the Makefile; put them on the disk:

```text
make build/compat/busybox build/compat/dropbear
make disk FILES="build/compat/busybox build/compat/dropbear"
make run
```

`make disk` replaces what was on the disk. In `ash`, make a link for each tool once, and add them to your `PATH`:

```text
# mkdir /disk/bin && for a in $(/disk/busybox --list); do ln -s /disk/busybox /disk/bin/$a; done
# export PATH=/disk/bin:$PATH
# vi /disk/notes.txt
```

The links are on the disk, so after a reboot only the `export` is needed.

## 3. Users and logging in

Set a password for root, add a user, and log in as them:

```text
# passwd
Changing password for root
New password:
Retype password:
passwd: password for root changed by root
# adduser alice
Changing password for alice
New password:
Retype password:
passwd: password for alice changed by root
# login
bastios login: alice
Password:
$ id
uid=1000(alice) gid=1000(alice) groups=1000(alice)
$ ls /root
ls: can't open '/root': Permission denied
$ exit
```

File permissions, `su`, set-user-ID programs and the rest are described in [security.md](security.md). The user database (`/etc/passwd`, `/etc/shadow`, `/etc/group`) and home directories are in memory for now, so users and passwords last until power off.

## 4. Network

`make run` puts the machine on QEMU's user network: it gets 10.0.2.15 by DHCP, the host is 10.0.2.2, and 10.0.2.3 answers DNS queries (written to `/etc/resolv.conf`). With the tools from section 2, on the host start a web server:

```text
python3 -m http.server 8000
```

and fetch from it in BastiOS:

```text
# wget -q -O - http://10.0.2.2:8000/
```

Names resolve through QEMU's DNS proxy, so `nslookup github.com` works, and so does `wget` to the internet if your host can reach it (plain HTTP with BusyBox's `wget`; for HTTPS use `curl` from Alpine, section 6).

The other way, three TCP ports on the host (on 127.0.0.1 only) reach the guest: 2222 its port 22, 8080 its port 80 (`make run SSH_PORT=... HTTP_PORT=...` to change them) and 7001 its port 7001. Serve a page from BastiOS:

```text
# mkdir -p /disk/www && echo 'hello from BastiOS' > /disk/www/index.html
# httpd -p 80 -h /disk/www
```

and on the host:

```text
$ curl http://127.0.0.1:8080/
hello from BastiOS
```

## 5. Log in over SSH

With Dropbear on the disk (section 2) and a password set (section 3), start the server in BastiOS. `-R` makes a host key on first use; its log goes to a file instead of the console:

```text
# mkdir -p /etc/dropbear
# /disk/dropbear -R -p 22 2>/tmp/dropbear.log
```

and log in from the host:

```text
$ ssh -p 2222 alice@127.0.0.1
alice@127.0.0.1's password:
$ id
uid=1000(alice) gid=1000(alice) groups=1000(alice)
```

The session runs on a pseudo-terminal, so editors, Ctrl-C and window sizes work. Keys work too: put your public key in `~/.ssh/authorized_keys` (mode 600, in a directory of mode 700). The host key is in memory, so it changes at each boot and `ssh` will warn about that; `ssh -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no` skips the check for a test machine.

## 6. Alpine Linux and apk

BastiOS runs Alpine Linux's own binaries, which are dynamically linked against musl. Alpine's root file system goes on the disk, and `chroot` enters it; there `apk` installs packages as on Alpine. Download the "mini root filesystem" for x86_64 from [alpinelinux.org/downloads](https://alpinelinux.org/downloads/), uncompress it, and put it on the disk with the full BusyBox (`MKDISK_FREE_MB` leaves room for what you install; the disk can be at most 256 MiB):

```text
gunzip -c alpine-minirootfs-*-x86_64.tar.gz > root.tar
MKDISK_FREE_MB=200 make disk FILES="build/compat/busybox root.tar"
make run
```

In `ash`, unpack it, give it the device files it needs, and enter it (Alpine's own BusyBox, in `bin/busybox`, has `mknod` and `chroot`):

```text
# mkdir /disk/a && cd /disk/a && /disk/busybox tar -xf /disk/root.tar && rm /disk/root.tar
# for d in null:1:3 zero:1:5 random:1:8 urandom:1:9 tty:5:0 console:5:1; do set -- $(echo $d | /disk/busybox tr : ' '); bin/busybox mknod dev/$1 c $2 $3; done
# cp /etc/resolv.conf etc/
# /disk/a/bin/busybox chroot /disk/a /bin/sh -l
```

Now you are in Alpine, at its prompt. Install packages and use them:

```text
bastios:~# apk update
bastios:~# apk add git curl
bastios:~# git --version
```

`apk` fetches from the mirrors in `/etc/apk/repositories` and checks each package's signature with Alpine's keys, which the root file system carries. Leaving with `exit` and entering again with the same `chroot` keeps everything, as the root is on the disk.

What has been tested is this sequence with the mirror replaced by a copy served from the host over HTTP (`echo http://10.0.2.2:8000 > etc/apk/repositories` with the packages in `x86_64/`), since the test machines cannot reach Alpine's mirrors; `make compat` runs it with git, curl, bash, make, jq, rsync, openssl and more (`test/compat/alpine.txt`). Alpine's mirrors serve HTTPS, which has not been tried yet; if it fails, change `https://` to `http://` in `/etc/apk/repositories`: the signatures are checked either way.

To build Alpine packages on BastiOS, install `abuild`, `build-base` and `fakeroot` in the root, add a user (`adduser -D builder`, `addgroup builder abuild`) and make the devices readable by it (`chmod 666 /dev/null /dev/zero /dev/random /dev/urandom /dev/tty`; `mknod` takes the umask as on Linux). As that user, `abuild-keygen -a -n` makes a signing key (copy its `.pub` file into `/etc/apk/keys`), and `abuild checksum && abuild -r` in a directory with an `APKBUILD` fetches the sources, applies patches, runs `check()`, packages with subpackages under `fakeroot`, signs the packages and the index, and leaves them in `~/packages/<repository>/x86_64`; `apk add -X ~/packages/<repository> NAME` installs them with the signature checked. `make compat` builds a small library, an application that needs it and tinycc this way (`test/compat/aports`, `test/compat/alpine.txt`). `fakeroot` may print `libfakeroot internal error: payload not recognized!` while it works: a signal (a finished child) cut its wait for the reply of its daemon, and it asked again.

## 7. A graphical session (early)

From the ISO or USB image, where the screen is a framebuffer, `bwm` starts a small window manager on it: a wallpaper, three windows and a pointer. Click a window to raise and focus it, drag a title bar to move it, drag the edges or corners of a window that can be resized (the grip at the bottom right corner, or any side) to resize it, type to put text in the focused window (a key held down repeats), press Ctrl-T (or click the Term button in the task bar along the bottom, which has a button for each window as well) for a new terminal window, the Menu button at its right end for a launcher (Terminal, Files, Hello window, Quit bwm), Alt-Tab to bring up the window at the bottom (Alt-Shift-Tab sends the top one to the bottom), the buttons left of a title bar's close box minimize a window (its task bar button brings it back) and maximize or restore a resizable one, drag over text in a `bterm` window to select and copy it (the copy is kept in `/tmp/bwm.clip`) and press the middle button in any `bterm` window to paste it, and press Ctrl-Q to leave; the text console comes back. It uses `/dev/fb0` for the screen and `/dev/input/mice` for the mouse (see [linux.md](linux.md)), so it is an ordinary Linux program (`user/bwm.c`). `bwm -n` starts it with no windows of its own, and `bwm -c bterm` also starts a terminal window: `bterm` is an 80 x 24 terminal at first (drag its grip and it takes as many cells as fit, up to 160 x 60) with `ash` behind it on a pseudo-terminal (a VT100 subset: cursor moves, erase, colours), so commands can be typed and run in a window, and several `bterm` windows can be open. `bfiles` is a file browser and text viewer: click a directory to go into it (".." goes up), click a file to read its first 64 KiB, click again or press Backspace to go back, and j / k / Space / b scroll. `bwm -c bhello` starts a client (`bhello` opens a window, shows what is typed and clicked, and ends with the close box). Programs open windows over a UNIX socket at `/tmp/bwm.sock`: `user/bwmproto.h` describes the messages (create a window of a given size, draw rectangles of 0x00RRGGBB pixels, and receive keys, pointer events, close requests and, for a window that said it can be resized, requests for a new size that it answers with the size it takes), and `user/bwmlib.h` is a small client library. bwm keeps each window's pixels. The native shell passes no arguments to Linux programs, so run `ash` first to give `bwm` any.

## 8. On a real PC

[Getting started](getting-started.md#5-boot-from-a-usb-stick) builds a bootable ISO and a USB stick image, and lists the hardware BastiOS supports. The image boots through [Limine](https://github.com/limine-bootloader/limine), which the build fetches itself, so the only package it needs is `xorriso`. `make iso` and `make usb` build it, and `make run-iso` / `make run-uefi` try the image in QEMU first. No real PC has booted it yet; [real-hardware.md](real-hardware.md) says how to try and what to report.

## What does not work yet

- It has only run in QEMU. [real-hardware.md](real-hardware.md) says what is needed to try a real PC.
- Users, passwords and SSH host keys are kept in memory and lost at power off; only `/disk` is kept.
- There is no init system. The console starts in the native shell as root; the boot option `login` (the third entry of the ISO and USB boot menu) starts BusyBox's `login` instead.
- At most 511 processes, a 512 GiB address window each, and 256 MiB of memory in `make run`. The disk holds up to 262143 files and directories; `make disk` writes an image of at most 1 TiB, 33 MiB or the size of the files plus `MKDISK_FREE_MB` (16 by default), whichever is larger. The other limits are listed in the README's Status section.
- The graphical session is a start (section 7): windows can be opened by other programs over a socket, but a terminal window is a small VT100 subset (no scroll region, no alternate screen, so full-screen editors do not work in it), there is no shared memory for window contents, and keys come as characters (no key release or modifiers).
- Some Linux system calls are missing; the kernel prints a note on the console when a program uses one (Dropbear's `execveat` does, harmlessly). [linux.md](linux.md) lists what is there, and `make compat` measures it.
- The system call ABI and the disk format can still change between versions.
