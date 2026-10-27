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

From the ISO or USB image, where the screen is a framebuffer, `bwm` starts a small window manager on it: a wallpaper with an icon for each launcher entry (a click starts the program), three windows and a pointer. Click a window to raise and focus it, drag a title bar to move it (some of the bar always stays on the screen, above the task bar), drag the edges or corners of a window that can be resized (the grip at the bottom right corner, or any side; the pointer turns into a double-headed arrow over them) to resize it, type to put text in the focused window (a key held down repeats), press Ctrl-T (or click the Term button in the task bar along the bottom, which has a button for each window as well) for a new terminal window, the Menu button at its right end (or Alt-F1, then Up, Down, Enter and Escape) for a launcher, Alt-F12 to save the desktop as a PPM in `/tmp/screenshot.ppm`, Alt-F2 to type a command that runs in a terminal window (Enter runs it, Escape cancels) (with the time of day, UTC, left of it) (Terminal, Files, System, Hello window, Quit bwm, and one entry for each line `Label=command` of `/etc/bwm.menu`, up to 8, which runs the command in a terminal window that waits for Enter when it ends; `bterm -e COMMAND` is the same thing from a shell); a right click on the empty desktop opens the same list at the pointer, Alt-Left and Alt-Right to put a resizable window on half of the screen and Alt-Up or Alt-F10 to fill it (Alt-Down, Alt-F10 again, the maximize button, or a double click on its title bar, puts it back, and Alt-Down (or Alt-F9) on a window that is not maximized minimizes it; a double click on the title bar of a resizable window also maximizes it), Alt-F4 to close the focused window, Alt-Tab to bring up the window at the bottom (Alt-Shift-Tab sends the top one to the bottom), the buttons left of a title bar's close box minimize a window (its task bar button brings it back), the clock at the right end of the task bar shows UTC unless `TZ` (or `/etc/TZ`) holds a POSIX offset such as `JST-9` (there is no zone database) and maximize or restore a resizable one, drag over text in a `bterm` window to select and copy it (a double click selects and copies the word under the pointer, a triple click the whole row) (the copy is kept in `/tmp/bwm.clip`) and press the middle button in any `bterm` window (or Shift-Insert, or Ctrl-Shift-V) to paste it (between `ESC [ 200 ~` and `ESC [ 201 ~` for a program that asked for that with `ESC [ ? 2004 h`), press Alt-Space to type Japanese (romaji becomes hiragana, `kitte` gives きって, `nn` gives ん; a "Kana" mark shows in the task bar; Alt-Space again gives katakana, marked "Kata", and a third time turns it off; Alt-Shift-Space turns on kanji conversion, marked "Kj": the kana wait in the task bar, Space shows the first candidate of a small built-in dictionary (`user/bwmdict.h`, about 170 words; the whole reading must match) and each next Space the next one, then the hiragana and the katakana, Enter puts the one shown in the window, and Backspace goes back a step), and press Ctrl-Q to leave; the text console comes back. It uses `/dev/fb0` for the screen and `/dev/input/mice` for the mouse (see [linux.md](linux.md)), so it is an ordinary Linux program (`user/bwm.c`). `bwm -n` starts it with no windows of its own, `bwm -w FILE` puts a PPM (P6, 8 bits, no comments in the header) or an uncompressed 24 or 32 bit BMP over the whole desktop, stretched to the screen (give it after `-n`, `-m` and `-s`; without it, the file named on the first line of `/etc/bwm.wallpaper` is used if there is one), `bwm -m WxH` asks the display for a smaller mode (QEMU's standard VGA only, at least 320 x 240 and no bigger than the boot mode in bytes; the boot mode comes back when `bwm`, or whatever else had `/dev/fb0` open, ends), and `bwm -s WxH` pretends the screen is smaller (at least 320 x 240; `bterm` and `bfiles` read the size from the `BWM_SCREEN` variable bwm sets, or from `/dev/fb0`, and open a window that fits; any window opens inside the screen), and `bwm -c bterm` also starts a terminal window: `bterm` is an 80 x 24 terminal at first (drag its grip and it takes as many cells as fit, up to 160 x 60) with `ash` behind it on a pseudo-terminal (a VT100 subset: cursor moves, erase, colours: the 16 of the ANSI palette and, with 38;5;N and 48;5;N, the 256 of xterm; 24-bit 38;2;R;G;B is drawn as the nearest of those), and underline, italic (the glyphs lean) and strike-through (4, 3, 9; 24, 23, 29 turn them off), so commands can be typed and run in a window (a program can set the window's title with `ESC ] 2 ; TEXT BEL`, and one that asks for the mouse with `ESC [ ? 1000 h`, `1002` or `1003`, and `1006` for the SGR encoding, gets the clicks, drags and wheel instead of the window selecting text), the mouse wheel scrolls back through the last 200 lines that went off the top (so do Shift-Page Up and Shift-Page Down, a page at a time, and Ctrl-Shift-Page Up and Ctrl-Shift-Page Down, to the oldest line and back to the live screen; typing or a click brings the live screen back), and several `bterm` windows can be open. `bfiles` is a file browser and text viewer: click a directory to go into it (".." goes up), click a file to read its first 64 KiB (the Up, Down, Home, End, Page Up and Page Down keys move a highlight along the list and Enter opens the entry under it), click again or press Backspace to go back, and j / k / Space / b scroll (so does the mouse wheel, three lines a step). In the viewer, `e` edits a file smaller than 64 KiB in place: typing, Enter (the new line starts with the indentation of the one before), Backspace, Delete, the arrow, Home, End and Page keys and a click move and change the text, Ctrl-S saves it and Ctrl-X goes back to the viewer, which shows the file as it is on disk; Ctrl-K cuts the rest of the line (it also goes to the clipboard, `/tmp/bwm.clip`) and Ctrl-U puts it back, Ctrl-V pastes the clipboard (what a `bterm` window selected), Ctrl-F finds text (Enter on an empty prompt searches for the last text again), Ctrl-R replaces every occurrence of a text (it asks for the text, then for what to put in its place; one Ctrl-Z undoes it all) and Ctrl-G goes to a line number, Ctrl-Z undoes the last edit (a run of typing, or of backspaces, is one edit; 16 are kept) and Ctrl-Y puts it back, Ctrl-A and Ctrl-E go to the start and the end of the line, Ctrl-Left and Ctrl-Right move by words, Ctrl-Home and Ctrl-End go to the start and the end of the text, Ctrl-W deletes the word before the cursor (or cuts the selection), Ctrl-D copies the line under the cursor below it, Ctrl-O opens an empty line below it, Ctrl-B swaps the two characters before the cursor, Shift-Tab takes up to four blanks off the start of the line, Shift with the arrow keys, Home or End, or dragging the mouse (a double click takes the word, a triple click the line), selects text (typing or Backspace replaces or removes it, Ctrl-C copies it), and the header shows the line the cursor is on (`bwm -c bfiles /path/to/file` opens a file directly). The list, the viewer and the editor draw UTF-8 (Japanese file names and text, a wide character in two cells) and the editor moves and deletes by character; the keyboard still types ASCII only, so other text comes in by pasting. A drag that began in a window goes on when the pointer leaves it: the editor scrolls (faster the farther the pointer is) and `bterm` selects to the edge of its window. In the list, `c` or `x` marks the entry under the cursor to copy or move (a directory is copied with the files and directories in it, down to 8 levels) and `v` puts it into the directory shown (dragging an entry onto a directory in the list moves it there; neither replaces a name that is taken; a copy gets `.copy` added), and `n` makes an empty file named `untitled` (then `untitled2` and so on) in the directory and opens it in the editor. `/` and the start of a name move the cursor to the first entry that begins with it (Backspace takes a letter back, Enter or Esc ends it). Names that start with a dot are left out of the list; `h` (or `.`) shows them and hides them again. `e` edits the file under the cursor, `~` goes to the home directory and `-` back to the directory shown before. `s` sorts the files largest first, again newest first, and again by name (the title row says which when it is not by name; newest first shows each file's date, in UTC, where the size would be), and `R` (or F5) reads the directory again. `g` and `G` jump to the top and the end of the list (or of the file in the viewer). `t` opens a terminal window in the directory shown. `N` makes an empty directory `newdir` (then `newdir2` and so on) and shows it. `r` (or F2) lets the name of the entry under the cursor be typed over, and `D` (or Delete) twice deletes it (a directory only when it is empty), as they do for a file in the viewer. In the viewer, `r` lets the name of the file be typed over (Enter renames it, Escape cancels), and `c` copies the file to `NAME.copy` next to it, and `D` asks for confirmation in the title bar and a second `D` deletes the file (any other key cancels). `bsys` (the launcher's System entry, or `bwm -c bsys`) is a window with the kernel, uptime, memory, processors, screen, root disk, time and the load averages (from `/proc/loadavg`); it reads them again every 2 seconds, and `r` (or F5) does it at once; `p` swaps them for the list of processes (with the memory each holds in KiB; newest first; `j` and `k` or the Up and Down keys move along it, `g` and `G` or Home and End go to the first and the last, Page Up and Page Down move a page, the mouse wheel moves three at a time, a click selects the process under the pointer, `n` sorts by name and back, `x` (or Delete) asks the selected one to end, `p` goes back). `bview` (`bwm -c bview FILE`, or `i` in the `bfiles` viewer, or a click on a `.ppm` or `.bmp` in the `bfiles` list) shows a PPM (P6, 8 bits) or an uncompressed 24 or 32 bit BMP in a window of its own, enlarged or sampled by a whole factor to fit 512 x 360; `+` and `-` (or the mouse wheel, up and down) double and halve the scale about the middle (four times each way), `h` `j` `k` `l` or the arrow keys move about a zoomed image, `0` or Home goes back to the first view, End zooms in all the way, Page Up and Page Down zoom like `+` and `-`, `n` and `p` show the next and the previous `.ppm` or `.bmp` of the same directory (by name, wrapping round) in the same window, and any other key closes it. `bwm -c bhello` starts a client (`bhello` opens a window, shows what is typed and clicked, and ends with the close box). Programs open windows over a UNIX socket at `/tmp/bwm.sock`: `user/bwmproto.h` describes the messages (create a window of a given size, draw rectangles of 0x00RRGGBB pixels, and receive keys, pointer events, close requests and, for a window that said it can be resized, requests for a new size that it answers with the size it takes), and `user/bwmlib.h` is a small client library. bwm keeps each window's pixels. The native shell passes no arguments to Linux programs, so run `ash` first to give `bwm` any.

## 8. On a real PC

[Getting started](getting-started.md#5-boot-from-a-usb-stick) builds a bootable ISO and a USB stick image, and lists the hardware BastiOS supports. The image boots through [Limine](https://github.com/limine-bootloader/limine), which the build fetches itself, so the only package it needs is `xorriso`. `make iso` and `make usb` build it, and `make run-iso` / `make run-uefi` try the image in QEMU first. No real PC has booted it yet; [real-hardware.md](real-hardware.md) says how to try and what to report.

## What does not work yet

- It has only run in QEMU. [real-hardware.md](real-hardware.md) says what is needed to try a real PC.
- Users, passwords and SSH host keys are kept in memory and lost at power off; only `/disk` is kept.
- There is no init system. The console starts in the native shell as root; the boot option `login` (the third entry of the ISO and USB boot menu) starts BusyBox's `login` instead.
- At most 511 processes, an 8 TiB address window each, and 256 MiB of memory in `make run`. The disk holds up to 262143 files and directories; `make disk` writes an image of at most 1 TiB, 33 MiB or the size of the files plus `MKDISK_FREE_MB` (16 by default), whichever is larger. The other limits are listed in the README's Status section.
- The graphical session is young (section 7): the screen keeps the mode the firmware set (there is no mode switch), text is drawn with one fixed bitmap font, the terminal window draws UTF-8 text with the console's font (Latin and Greek letters, box drawing, kana and the kanji of JIS X 0208; any other character is a look-alike such as `-` or a `?`) but the keyboard types ASCII, or hiragana and katakana through the romaji mode of Alt-Space (kanji conversion has a small dictionary only, Alt-Shift-Space), and only text moves through the clipboard.
- Some Linux system calls are missing; the kernel prints a note on the console when a program uses one (Dropbear's `execveat` does, harmlessly). [linux.md](linux.md) lists what is there, and `make compat` measures it.
- The system call ABI and the disk format can still change between versions.
