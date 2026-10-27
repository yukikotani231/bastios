# Changelog

Notable changes to BastiOS. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/). BastiOS is
pre-release: before 1.0, any minor version may change the system call ABI,
the disk format or the build.

The version is the one the kernel prints at boot (`BastiOS X.Y` in
`src/main.bastio` and `user/sh.bastio`). A release is a tag `vX.Y.Z` on
`main`; `.github/workflows/release.yml` builds it (see
[docs/releasing.md](docs/releasing.md)).

## [Unreleased]

What will be 0.3.0. It needs bastiolang 0.2.0.

### Added

- `/dev/input/mice` has the ImExPS/2 mode (sample rates 200, 200, 80, id 4): the fourth byte carries the wheel in its low four bits and the back and forward buttons in bits 4 and 5, fed from a USB mouse's side and extra buttons and from a PS/2 mouse that reports five buttons (`mmouse` and the USB and PS/2 mouse tests check it).
- The `SIGSYS` of a seccomp `SECCOMP_RET_TRAP` filter carries `si_code` `SYS_SECCOMP`, `si_syscall`, `si_arch` and `si_call_addr`, as Linux's does, so a sandbox's handler can see which call it refused.
- A click on a process in the `bsys` list selects it.
- Page Up and Page Down zoom the image in `bview` in and out, as `+` and `-` do.
- The `bfiles` list shows the date of each file in place of its size while it is sorted by time.
- In the `bsys` process list Delete ends the selected process, as `x` does.
- Alt-F10 in `bwm` maximizes the focused window and puts it back.
- F5 in `bsys` reads the numbers again, as `r` does.
- In the `bfiles` list F2 renames the entry under the cursor, as `r` does.
- End zooms the image in `bview` in all the way.
- Kanji conversion in `bwm`: Alt-Shift-Space (marked "Kj" in the task bar) keeps the typed kana in the task bar, Space cycles the candidates of a built-in dictionary of about 170 words (`user/bwmdict.h`) then the hiragana and the katakana, Enter puts the one shown in the window. The glyphs come from the console's font through `tools/imefont.py`.
- `bwm -m WxH` (FBIOPUT_VSCREENINFO on `/dev/fb0`) puts QEMU's standard VGA in a smaller mode; the boot mode comes back when the last description of `/dev/fb0` closes. The I/O ports 0x1CE to 0x1D0 are claimed for the mode registers (`src/vbe.bastio`); no C or assembly was added.
- In `bfiles`, a drag from an entry onto a directory entry moves it there (a click now acts when the button is released).
- Alt-F9 in `bwm` minimizes the focused window.
- Alt-F1 in `bwm` opens the launcher menu from the keyboard: Up and Down move the lit entry, Enter chooses it and Escape closes the menu.
- Alt-F2 in `bwm` asks for a command in the task bar and runs it in a terminal window (Enter runs it, Escape cancels).
- `bwm -w FILE` puts a PPM (P6, 8 bits) or an uncompressed 24 or 32 bit BMP over the whole desktop, stretched to the screen. Without `-w`, the file named on the first line of `/etc/bwm.wallpaper` is used if there is one.
- Ctrl-Shift-Page Up in `bterm` goes to the oldest line kept in the history, Ctrl-Shift-Page Down back to the live screen.
- Alt-F12 in `bwm` saves the desktop as a PPM to `/tmp/screenshot.ppm` (open it with `bview` or from `bfiles`).
- The `bsys` System window shows the load averages (read from `/proc/loadavg`) in a new Load row.
- `u` in the `bfiles` list shows file sizes as 4.8K, 1.2M and 3.0G instead of bytes (and back).
- Alt-F5 in `bwm` minimizes every window; pressing it again brings back the ones it hid.
- The `bsys` System window shows the first network interface that has an address (or `none`) in a new Network row.
- The process list of `bsys` has a Mem(K) column with the resident size of each process.
- `m` in the `bsys` process list sorts by memory, largest first (and back to newest first).
- `n` and `p` in `bview` show the next and the previous image (`.ppm` or `.bmp`) of the same directory, by name, in the same window.
- Alt-Down in `bwm` puts a snapped or maximized window back, and minimizes any other.
- In the `bsys` process list `g` and `G` (or Home and End) go to the first and the last process.
- The mouse wheel zooms the image in `bview`: up in, down out.
- In the `bfiles` list the Delete key does what `D` does: twice deletes the entry under the cursor.
- In the `bfiles` list `e` edits the file under the cursor without opening the viewer first.
- In the `bfiles` list `.` shows and hides the names that start with a dot, as `h` does.
- In the `bsys` process list Page Up and Page Down move a page at a time.
- In the `bfiles` list F5 reads the directory again, as `R` does.
- Shift-Tab in the `bfiles` editor takes up to four blanks off the start of the line.
- In the `bsys` process list `n` sorts by name and back to newest first.
- Home puts the zoom in `bview` back to the first size.
- The mouse wheel walks the `bsys` process list, three processes a step.
- The `bfiles` editor swaps the two characters before the cursor with Ctrl-B.
- The `bfiles` editor opens an empty line below the cursor with Ctrl-O.
- The `bfiles` editor copies the line under the cursor below it with Ctrl-D.
- The title row of `bfiles` adds `[by size]` or `[by time]` when the list is not sorted by name.
- In the `bfiles` list `-` goes back to the directory shown before the last change.
- In the `bfiles` list `~` goes to the home directory (`$HOME`, or `/` when it is not set).
- In the `bfiles` list `/` and the start of a name move the cursor to the first entry that begins with it (Backspace takes a letter back, Enter or Esc ends it).
- `bview` zooms: `+` and `-` double and halve the scale about the middle of the window, `h j k l` or the arrow keys move about a zoomed image, `0` goes back.
- The `bfiles` editor marks the text of the last Ctrl-F wherever it shows in the window.
- A drag in a `bfiles` editor that rests above or below the window goes on scrolling: `bwm` repeats the motion every 60 ms while the button is held.
- `bwm` types Japanese: Alt-Space turns on a romaji to hiragana mode, again a romaji to katakana mode, and a third time turns it off (a "Kana" or "Kata" mark shows in the task bar, with the letters that wait for a kana). There is no conversion to kanji.
- `bwm` opens its launcher menu at the pointer on a right click on the empty desktop.
- The `bsys` System window lists the processes (`p`), moves along the list (`j`, `k` or the arrow keys) and ends the selected one (`x`).
- The `bfiles` editor cuts and pastes a line (Ctrl-K, Ctrl-U), finds text (Ctrl-F) and goes to a line (Ctrl-G), and shows the cursor's line number.
- `bfiles` copies and moves files between directories: `c` or `x` marks the entry under the cursor and `v` puts it into the directory shown.
- The `bfiles` editor pastes the clipboard (Ctrl-V, the text a `bterm` window selected), and what Ctrl-K cuts goes to the clipboard too.
- `bfiles` renames (`r`) and deletes (`D` twice) the entry under the cursor in the list, as it did for the file in the viewer; an empty directory can be deleted too.
- The `bfiles` editor undoes edits with Ctrl-Z (a run of typing is one edit; the last 16 are kept) and puts them back with Ctrl-Y.
- The `bfiles` editor replaces text: Ctrl-R asks for the text to find and the text to put in its place, replaces every occurrence and says how many; one Ctrl-Z undoes it.
- The `bfiles` editor keeps the indentation of a line when Enter starts a new one.
- The `bfiles` editor goes to the start and the end of the line with Ctrl-A and Ctrl-E.
- The `bfiles` editor moves by words with Ctrl-Left and Ctrl-Right, and `bwm` sends `ESC [ 1 ; 5 A` to `D` (and `H`, `F`) for an arrow key held with Ctrl, as xterm does; `bfiles` also skips such modifier numbers in a key sequence instead of typing them.
- The `bfiles` editor deletes the word before the cursor with Ctrl-W (one Ctrl-Z puts it back).
- The `bfiles` file list and viewer draw UTF-8 (a Japanese file name, text in a file) with the same font, a wide character in two cells.
- `bterm` blanks what is left of a wide character when erase, insert or delete characters (`CSI X`, `@`, `P`, `K`, `J`) cut it in half, instead of drawing half a glyph.
- A drag that began in a window's contents goes on when the pointer leaves it: `bwm` keeps sending the motion and the release to that window, with signed coordinates; the `bfiles` editor scrolls and stretches the selection, `bterm` stretches it to the edge.
- The `bfiles` editor draws UTF-8 too, and the cursor, Left and Right, Up and Down, Backspace, Delete and the mouse move by character and by cell column, not by byte; a byte that is not UTF-8 is a `?`.
- `bterm` draws UTF-8 text from the console's font (`src/font16.bin`): Latin and Greek letters, box drawing, blocks, kana, the kanji of JIS X 0208 and half-width katakana, a wide character in two cells; copying a selection writes UTF-8. Any other character is still a look-alike or `?`.
- `bterm` reads UTF-8: a character takes one cell, drawn as a look-alike for box drawing, blocks, arrows, bullets and quotes (`-`, `|`, `+`, `#`, `*` and so on) and as `?` for anything else the 128-glyph font lacks; combining and zero-width characters take none. Before, every byte of such a character was dropped, so tmux borders and non-ASCII file names vanished or shifted the line.
- The `bfiles` editor goes to the start and the end of the text with Ctrl-Home and Ctrl-End.
- A triple click in the `bfiles` editor selects the line under the pointer, with its newline.
- A double click in the `bfiles` editor selects the word under the pointer.
- The `bfiles` editor selects text by dragging the mouse over it (a click puts the cursor and starts a selection there).
- The `bfiles` editor selects text with Shift and the arrow keys, Home or End; typing, Backspace or Delete replaces or removes the selection, Ctrl-C copies it and Ctrl-W cuts it (into the clipboard other windows paste from). `bwm` now sends the Shift-modified arrow sequences.
- The task bar clock of `bwm` follows `TZ` (or `/etc/TZ`) in POSIX form such as `JST-9`; there is still no zone database, so it shows UTC when neither is set.
- `bfiles` copies a directory with everything in it (down to 8 levels), not only a file.
- `bwm` maximizes a resizable window, or puts a maximized one back, on a double click on its title bar.
- `bwm` snaps the focused window to the left or right half of the screen with Alt-Left and Alt-Right, and maximizes it with Alt-Up.
- `bterm` sets the window title when a program prints `ESC ] 0 ; TEXT` or `ESC ] 2 ; TEXT` ended by BEL or `ESC \`.
- `bwm` draws a desktop icon for each launcher entry but Hello window and Quit bwm, and starts the program on a click.
- `bwm`'s launcher menu gains an entry for each `Label=command` line of `/etc/bwm.menu`, which runs the command in a terminal window; `bterm -e COMMAND` runs a command in place of the shell prompt.
- `bfiles` moves a cursor along the list with the arrow, Home, End and Page keys and opens the entry under it on Enter.
- `bsys` reads its numbers again every 2 seconds.
- `bfiles` opens a `.ppm` or `.bmp` in the image viewer on a click in the list.
- `bfiles` makes a new empty file on `n` in the directory list and opens it in the editor.
- `bterm` selects and copies the word under the pointer on a double click and the whole row on a triple click.
- `bterm` scrolls back and forward a page on Shift-Page Up and Shift-Page Down (`bwm` sends the keys `BWM_KEY_SCROLL_UP` and `BWM_KEY_SCROLL_DOWN`, 0x101 and 0x102, to the focused client).
- `bterm` draws the 256-colour palette (`38;5;N`, `48;5;N`), the bright backgrounds (`100`-`107`) and 24-bit colours (`38;2;R;G;B`, as the nearest of the 256).
- `bterm` brackets a paste in `ESC [ 200 ~` and `ESC [ 201 ~` for a program that asked for it with `ESC [ ? 2004 h`, as shells and editors do to tell typed text from pasted text.
- `bterm` reports the mouse to programs that ask for it (xterm modes 1000, 1002, 1003 and 1006), for example `mouse=a` in vim.
- `bterm` draws italic (`ESC [ 3 m`, leaning glyphs) and strike-through (`9`) text.
- `bterm` underlines text after `ESC [ 4 m` (until `24` or `0`).
- `bview` shows a PPM or BMP image in a window (`bwm -c bview FILE`, or `i` in the `bfiles` viewer). It is part of `bterm`. `bterm` is now linked with `--gc-sections`, which saves about 4 KiB of the kernel image.
- `bsys` is a System window (kernel, uptime, memory, processors, screen, root disk, time) that the launcher menu opens; `r` refreshes it. It is part of `bterm`, so it adds no image.
- `g` and `G` in `bfiles` jump to the top and the end of the list or the file.
- `R` in the `bfiles` list reads the directory again.
- `s` in the `bfiles` list sorts the files by size (largest first), then by time (newest first), then back by name.
- `bfiles` leaves out names that start with a dot, and `h` in the list shows or hides them.
- `bfiles` copies the file it is showing to `NAME.copy` on `c`.
- `bfiles` opens a terminal window in the directory shown on `t` in the list.
- `bfiles` makes a new empty directory on `N` in the directory list.
- `bfiles` renames the file it is showing on `r`: the new name is typed in the title bar, Enter renames and Escape cancels.
- `bfiles` deletes the file it is showing on `D` pressed twice (the title bar asks first; any other key cancels).
- `bterm` pastes the copy on Shift-Insert and Ctrl-Shift-V, as the middle button does (`bwm` sends a key `BWM_KEY_PASTE`, 0x100, to the focused client).
- `bterm` keeps the last 200 lines that scrolled off the top and shows them again when the mouse wheel is turned up (turned down, it comes forward); typing or a click returns to the live screen. The alternate screen has no history.
- `bwm` closes the focused window on Alt-F4, as the close box does.
- `bwm` turns the mouse wheel on (ImPS/2) and hands wheel steps to the window under the pointer; `bfiles` scrolls three lines a step in the viewer and the editor.
- `bwm` shows the time of day (UTC, `HH:MM`) in the task bar.
- `bwm` draws a resize cursor over the edges and corners of a resizable window.
- `bfiles` edits a text file in place (`e` in its viewer; Ctrl-S saves, Ctrl-X returns) and opens a file given on its command line.
- `bwm` keeps new windows inside the screen and above the task bar, `bterm` and `bfiles` open a grid that fits a screen smaller than 80 x 24 cells (a 640 x 480 screen used to refuse the terminal), and `bwm -s WxH` pretends the screen is smaller so `test/iso_test.sh` can check it.
- `bwm` keeps part of a dragged window's title bar on the screen and above the task bar, so a window cannot be thrown out of reach.
- `bfiles` (`user/bfiles.h`, run by `bterm` when it is started under that name), a file browser and text viewer for `bwm`, started from the Files entry of the launcher menu: click a directory to enter it, a file to read its first 64 KiB, and scroll with j, k, Space and b. `test/iso_test.sh` opens it from the menu.
- `/dev/input/event0` (`src/lxkeyev.bastio`) gives the PS/2 keyboard's key events as Linux's evdev does: 24-byte `input_event` records, an `EV_KEY` for each press (value 1) and release (value 0) with the key's Linux code, each followed by an `EV_SYN`. Shift, Ctrl and Alt are reported as keys of their own. There is no auto-repeat, no `EVIOCG*` ioctl answer and no USB keyboard yet.
- `mount -t ext4 IMAGE DIR` (also `ext2` and `ext3` images without a journal to replay) reads an ext4 image, a file anywhere (on the disk too) or a loop device, into a read-only mount on an empty in-memory directory (`src/ext4.bastio`, `docs/design/ext4-readonly.md`). Files, directories, symbolic links, hard links, device nodes, pipes, modes, owners and times are shown; extents and classic block maps are read; holes stay holes. It is a snapshot held in memory: at most 32768 nodes, paths up to 255 bytes and files up to 1 GiB (the mount fails with `ENOSPC`, `ENAMETOOLONG` or `EFBIG` otherwise), and an image with a journal to replay, inline data or encryption is refused with `EINVAL`. `test/ext4img.py` makes images with `mke2fs -d` and `user/mext4.c` checks them in `make test`.

### Documentation

- A documentation website for BastiOS at <https://yukikotani231.github.io/bastios/>, built from `docs/` with mdBook (`site/build.sh`) and published by GitHub Actions on every push to `main`.

### Boot

- The ARP cache holds 64 neighbors (4 before): a machine on a network with more than four neighbors no longer asks again for the ones it just forgot.
- The system runs 1023 processes and threads at once (511 before). `NPROC` is 1024; the per-process tables follow it, and `/proc/PID` names, `dec_out` and `proc_pid` take four-digit ids. `mproc` holds 700 children at once and checks ids past 511. A machine with a small kernel heap still stops sooner (about 250 at 256 MiB), because the per-process tables double at 257 rows.
- The kernel looks at UHCI (USB 1.1) controllers and says in the boot log how many there are and how many devices are on their ports. There is no driver for them yet; this shows whether a USB keyboard on an older PC is out of reach.
- The hardware report's `pci` lines say when a SATA controller is in RAID mode (Intel RST) or Intel VMD is on, which hide the disks from BastiOS, and what to change in the firmware.
- Swap to a file: `swapon(path)` and `swapoff(path)` (root only) take a swap file made by `mkswap` that lives on the disk (up to 65536 pages, 256 MiB; one area at a time). When a page fault finds no free page and the page cache has nothing to give back, the kernel writes pages of the process that holds the most memory to the file (a clock hand per process, 4 KiB each, synchronously) before the out-of-memory killer is asked; a page comes back at its next fault, so the program sees the bytes it wrote. `/proc/swaps`, `SwapTotal`/`SwapFree` in /proc/meminfo, `pswpin`/`pswpout` in /proc/vmstat and `oom_score` (swapped pages do not count) follow. `swapoff` reads every page home (ENOMEM if they do not fit). Tested by `test/memory_test.sh` at 64 MiB of RAM: a 44 MiB program reads back what it wrote, a holder pushed out by a second program is whole after `swapoff`, and a program bigger than memory and swap is still killed. How it works: an evicted page's slot maps a marker page (no access) and a hash from (process, page) to the file slot lives in a kernel table, so the fault finds it; system calls bring back the pages their arguments name before they run (up to 512 pages from each), fork and `mprotect`/`munmap`/`mremap`/`madvise` do the same or drop the slots. Not done: pages that are shared, in the page cache or part of a program are never swapped; a page reached through a pointer inside a structure the call reads (an iovec's buffers) must be resident; ptrace, `process_vm_*` and /proc/PID/mem do not swap in; the disk I/O runs under the kernel lock, so a swapping program is slow; a swap partition or a second area is not supported; priorities and `swapon` flags are ignored.
- A USB serial port on the xHCI controller is `/dev/ttyUSB0` (an FTDI adapter, set to 115200 baud with DTR and RTS up) or `/dev/ttyACM0` (a CDC-ACM modem or board; the same device under either name). The driver (`src/xhci.bastio`) takes a sixth device slot and two more pages for it (`XHCI_PAGES` is 13; the sound card's start moved to page 45), and keeps one packet queued on the bulk IN endpoint and one write at a time on the OUT endpoint. The bytes pass the console pump into two rings of 1 KiB in the kernel (`src/lxserial.bastio`); a Linux program reads, writes, polls and sets termios on it (kept, not acted on), and a write waits for room when the ring is full. Bytes that arrive while the receive ring is full are dropped. `user/mserial` and `shell_test.sh` exercise it against QEMU's `usb-serial` (an FTDI adapter on a socket) on the console controller and on the disk's, beside a stick, a keyboard, a mouse and two hubs. Untested: CDC-ACM (QEMU has no such device) and real adapters. Not done: `O_NONBLOCK` set by `fcntl` after the open, flow control, other speeds.
- On Intel 7-, 8- and 9-series chipsets the xHCI driver routes the USB 2 ports from the EHCI controller to the xHCI controller (and enables its SuperSpeed ports) before starting it, so keyboards and sticks behind those ports are found. Untested on hardware: QEMU's xHCI has no such registers.
- A System V shared memory segment can be 32 MiB (2 MiB before; `shmmax` reads 33554432): its pages hang from up to 16 level-1 tables of the shared store instead of one. Pages are still all taken when the segment is made, so a small VM refuses a big one with ENOMEM.
- The in-memory file system holds 32768 files and directories (8192 before); the console shell's `ls` looks at the first 32768 disk entries after them. The node table now grows as nodes are made instead of being filled at boot, so a small machine pays only for the nodes it uses; the name index (1 MiB) and the open-description table are still allocated at boot. `ram_create` starts its search at a hint, so making many names is not quadratic. `rmdir` and the rename of a directory no longer walk the whole table when nothing lies under the directory: a count per hash of each ancestor directory name, kept where names enter and leave the index, answers that first.
- The network stack holds 240 TCP connections (32 before: two for each connection between programs on this machine, plus listeners and connections in TIME_WAIT), 32 UDP sockets (8) and 256 masqueraded flows (64) for network namespaces.
- The network stack holds 64 network namespaces (16 before), 128 interfaces across them (32), 32 netlink sockets (8) and 64 IPv6 neighbors (16). The netlink answer queues are the one eager cost, 256 KiB in all.
- Files on a disk can be up to 512 GiB (1 GiB before): a file past 1 GiB hangs from a super map of root maps, one per GiB (disk format `BSTDISK5`, written when the first such file is made; older kernels do not mount it). `user/mbig` writes pieces at offsets up to 40 GiB on a sparse 40 GiB disk, across GiB boundaries, cuts and regrows the file, and checks that every block comes back; `test/fsck.py` and `test/crashsim.py` read the new layout.
- A file in memory (tmpfs, and the image behind a loop device) can reach 1 GiB (64 MiB before), and a loop device 1 GiB (64 MiB before). Pages are still taken only where a file is written, so a sparse file costs a table of 16 bytes a page. `shell_test.sh` grows a file to 300 MiB and appends to it.
- The disk holds 262143 files and directories (32767 before): the directory takes up to 16384 blocks, named by up to 32 extension blocks, and the in-memory directory and name index grow to 64 MiB and 2 MiB. Disk formats are unchanged. `shell_test.sh` creates 33000 files in one directory (the old limit would have refused the 32768th), reboots, and finds them; `test/fsck.py` reads the larger directory. Filling the whole table is no longer tested (it is eight times longer than the old fill).
- Tested: 255 processors (the most xAPIC ids can address) start in QEMU without KVM, reach the shell in about ten seconds, and every page comes back at power off. Beyond that the kernel already runs processors in x2APIC mode, but the test for it needs QEMU 9.
- Executable holds: 512 programs at once can be loaded lazily (128 before), one for each possible process; past the limit a program is loaded whole, as before.
- System V semaphores: 1024 SEM_UNDO adjustments in the whole system (256 before), so one process can hold an undo for each of 512 semaphores and still have the rest of the machine room. `t_sysv` checks 512 in one process, all undone at exit.
- Pseudo-terminals: 64 pairs at once (16 before); the 65th `openpty` fails. The master and slave device numbers moved to 128 and 192, and the slaves have inode numbers of their own.
- Epoll, timerfd, inotify and named pipes: 32 epoll instances (16 before), 64 timerfds (32), 32 inotify instances (16) and 32 named pipes open at once (16). The sets of epoll instances that loop detection walks are 32-bit masks now. `t_inotify` checks each.
- AF_UNIX: 64 socketpairs (128 stream or datagram channels) and 32 named sockets at once in the whole system (16 channels and 4 names before). A datagram socket keeps its own and its peer's slot in base 64, not base 8.
- System V shared memory: 32 attachments in one process (16 before); the 33rd `shmat` is EMFILE. `shminfo.shmseg` reports 32.
- Seccomp: 64 filters at once in the whole system (8 before), sharing room for 65536 instructions (8192). Container runtimes load a filter per container and runc stacks several; the eighth filter used to fail with `ENOMEM`. The pool is still taken from the kernel heap at the first filter (512 KiB now). `t_misc` stacks forty filters on one thread.
- virtio devices over MMIO (QEMU `microvm`, Firecracker's transport): the block and network drivers go through a transport that is either I/O ports (PCI) or a block of memory-mapped registers, version 1 or 2. Found at QEMU microvm's slots on a machine with no PCI. The 12-byte modern network header is handled. The devices named by `virtio_mmio.device=SIZE@ADDRESS:IRQ` words on the command line (Firecracker) are looked for instead of microvm's slots.
- A PVH entry (Xen's direct boot protocol, used by Firecracker, Cloud Hypervisor and QEMU's `microvm`): `boot/boot.S` carries the ELF note, `boot/multiboot.c` converts the `hvm_start_info`. Tested on QEMU by booting a copy without the Multiboot header.
- `mcloud` (`user/mcloud.c`, a Linux program): first-boot setup from an instance metadata service speaking AWS's IMDSv2 protocol (a token from a PUT, then GETs with it): it sets the host name and installs the first public key in root's `.ssh/authorized_keys`. The address is an argument (default 169.254.169.254). Tested against a fake service on the host, from BusyBox's `ash`; not run on a real cloud. `mcloud` also saves `user-data` as `~root/user-data` (mode 700) without running it.
- Boot option `mcloud`: runs the first-boot setup at boot (waiting for the network), beside the console shell.
- A virtio entropy source (virtio-rng, on PCI or virtio-mmio) is read at boot, before any other driver looks for a device: 64 bytes go into the pool behind `/dev/urandom` and `getrandom`, then the device is reset and everything it held is given back. `rng: virtio-rng, 64 bytes of entropy` is printed when there is one. A guest's first host key no longer depends on the time-stamp counter alone. Trusted: that the host's bytes are random (they are only mixed in). x86-64 only so far.
- Tested: a disk made larger since the last boot keeps its files and offers the new space (no resize step is needed on a cloud volume that was enlarged).
- A Linux process killed by a page fault gets a second console line saying what the kernel knows of the page (mapped or not, owned or not, heap and mmap limits, parent, thread count, age), so a crash that is hard to reproduce can be read from its log. The line is printed only for Linux processes; the native `fault` demo is unchanged.
- Ctrl-Alt-Delete on the PS/2 keyboard restarts the machine (what Firecracker's `SendCtrlAltDel` sends), so a guest without ACPI has a way to end. The console prints `ctrl-alt-del: restarting` and the machine goes through the same path as `reboot`.
- The local APIC timer is calibrated against PIT channel 0 when the speaker channel (2) is missing, as in QEMU's `microvm`, before falling back to two seconds of the RTC. A `microvm` boots in 0.3 s instead of 2.1 s under TCG. Trusted: about 30 more lines of C in `boot/smp.c`.

### Memory

- The page cache that shares program code between runs holds 32 MiB (16 MiB before). A warm gcc run needs more than 16 MiB of code, so each run used to load it into different pages, which an emulating host (QEMU TCG) translates again every time: a second `gcc -O2` of one file took 3.65 s and takes about 0.7 s now on the same host (the first takes about 1.5 s). The cache's arrays take about 0.13 MiB more kernel heap.
- Running out of memory no longer ends the faulting process with SIGSEGV. A page fault that finds no free page first gives the page cache back (a cached page nobody maps is freed) and runs the fault again; if that frees nothing the out-of-memory killer (`src/oom.bastio`) sends SIGKILL to the process with the highest `oom_score`, Linux's rule: the share of all pages it holds in thousandths, moved by its `oom_score_adj` (-1000 never kills it, 1000 makes it the first choice; process 1 is never chosen). The console says `Out of memory: Killed process N (name) oom_score S, K kB resident`, the faulting process waits while the victim dies, and /proc/vmstat counts `oom_kill`. `/proc/PID/oom_score` reports the score (it was always 0), `/proc/sys/vm/drop_caches` (1 or 3) drops the page cache, and `MemAvailable` counts the cached pages. Tested at 64 MiB of RAM by `test/memory_test.sh` (user/mhog.c) on one and four processors: reclaim before any kill, the biggest process chosen over the one that asked last, `oom_score_adj` moving the choice. Not done: a shortage of the kernel's own heap still halts, a system call that finds no page still fails with ENOMEM, and a stack that cannot grow still gets its SIGSEGV.
### Tests

- `test/compat/bastio.txt` runs bastiolang's `bastiofmt` (the formatter) and `bastio-lsp` (the language server) on BastiOS: the formatter on a line and on the compiler's parser, the language server over its standard input and output for hover, go to definition, formatting and a diagnostic from `bastioc1`.

### AArch64

- BastiOS builds for AArch64 and boots to the shell on QEMU's `virt` machine
  (`make ARCH=aarch64 run`; [docs/aarch64.md](docs/aarch64.md)): exception
  vectors, MMU, GICv3, the generic timer as the scheduler tick, the PL011
  console and PSCI power off, with the native-ABI programs (`sh`, `hello`,
  `spin`, `true`) running at EL0. `test/aarch64_test.sh` is part of `make test`.
  The Linux layer, the file system and the network are still x86-64 only.
- Programs get AT_HWCAP and AT_HWCAP2 in their auxiliary vector (`HWCAP_FP | HWCAP_ASIMD | HWCAP_CPUID` on AArch64, 0 on x86-64). A musl built with optimised string routines reads them before it has a thread pointer and crashed without them. `/proc/PID/auxv` reads the longer vector, and core files carry an AArch64 `NT_PRSTATUS`, so gdb 16 debugs programs on AArch64 (breakpoints in a PIE program, attach, threads, a core file).
- Vim and xxd are in the AArch64 Alpine repository (the arm64 manifest of the same Docker Hub tag the x86-64 suite pins).
- `mprotect` that makes never-touched pages of a PROT_NONE reservation writable records them to be mapped when touched (as a writable anonymous `mmap` does) instead of mapping them all at once, so V8's 4 GiB-wide code range no longer fails with ENOMEM on a small machine: Alpine's Node 24 runs its JIT.

### Network

- A driver for the AWS Elastic Network Adapter (`src/ena.bastio`, PCI
  `1d0f:ec20` and relatives), polled, one queue pair in host memory. It has
  been run only against a model of the device for QEMU
  (`tools/qemu-ena`), which the DHCP, TCP and IPv6 tests and three boots of
  the shell test use (`NIC=ena`); see `docs/cloud.md` for what that leaves open.
- A network card's registers may now lie anywhere in the 32-bit device
  window (they had to be above 3 GiB).

### Disk

- A whole path can be 4095 bytes long (`PATH_MAX` 4096 with the NUL), as on Linux; it was 255. A component is still limited to 255 bytes (`NAME_MAX`), so a path past 4095 bytes fails with `ENAMETOOLONG` and so does a component past 255. The transfer buffer and the lexer's work buffer grew to hold two paths, and local path buffers are 4096 bytes. Working directories, roots and the names of open directories keep their first 256 bytes in the small rows they had (eight bytes to a word, 33 words a row, which is less than the 256-word rows before) and the rest in a side table that grows only when a path is that long, so the memory a machine spends is the same when no path is. A `fork` or `clone` whose parent has a long working directory fails with `ENOMEM` if that table cannot grow, and so do `chdir` and `chroot`. The key and source of a mount (`mount`, overlay layers) are still limited to 255 bytes each (`ENAMETOOLONG`). `test/compat/progs/t_path.c` builds a path of 3000 bytes of nested directories and checks `mkdir`, `open`, `stat`, `chdir`, `getcwd` and `unlink` on it, and that a 256-byte component is `ENAMETOOLONG`. Proved: the whole kernel is discharged; the trusted base is unchanged.
- File names on the disk file system can be 255 bytes long, as on Linux (`NAME_MAX`; the limit was 200, so a name of 201 to 255 bytes failed). A name past 200 bytes goes on into the entry after the one that holds it, which is of a new kind (5) and the other half of the same 512-byte sector, so a power cut leaves the whole name or none. The first such name rewrites the magic to `BSTDISK6` in one sector, so an older kernel leaves that disk alone instead of reading a cut name; a disk with no long name keeps its magic. `test/fsck.py` checks the new kind, and `mkdisk.py` can make such names.
- NVMe and AHCI take a run of consecutive blocks in one command, as virtio already did: up to 32 blocks (128 KiB), through a PRP list (NVMe, bounded by the drive's MDTS) or one PRD entry per block (AHCI). `diskbench` writes 2 MiB in 3-4 ticks on both, 8-10 and 6-7 before.
- The disk file system holds 5119 files and directories (was 4095): the
  directory has up to 320 blocks, all named by the superblock's block, so
  existing disks mount unchanged. The two directory tables (the disk's and
  the loop mount's) now take 2.5 MiB of the kernel's arena, which is 4 MiB
  up to 128 MiB of RAM; `test/shell_test.sh` boots with 256 MiB for that
  reason. Neovim, git, gcc and musl together are
  about 4300 files.

### Terminal

- Insert and F1-F12 on a PS/2 or USB keyboard send the VT220 sequences too (`ESC [ 2 ~`, `ESC O P`, `ESC [ 1 5 ~` and so on). A terminal in line mode keeps Tab and ESC in the line instead of dropping them, as Linux does, and echoes ESC as `^[`. `test/iso_test.sh` presses the keys on the PS/2 and the USB keyboard and counts the bytes a program reads.
- Programs the kernel starts get `LANG=C.UTF-8` in their environment, and a new `/etc/profile` (read by login shells, including SSH logins) exports it, so musl's locale functions and programs that check for a UTF-8 locale (BusyBox with Unicode support, Neovim's `ambiwidth` and `fileencoding` defaults) agree with the terminal. `t_time` now also checks `TZ`: a POSIX string, a DST rule and a zone file. A zone file under `/usr/share/zoneinfo` works as on any musl system (Alpine's `tzdata` package); there is no zone by default, so the time is UTC. The Alpine tty group types Japanese into Neovim and checks the bytes it saves and the cells and characters it counts.
- The framebuffer console decodes UTF-8 and draws Japanese: kana, kanji (JIS X 0208) and fullwidth forms in two cells, Latin-1, Greek, box drawing and half-width katakana in one, from `src/font16.bin` (about 230 KiB, built by `tools/mkfont.py` from the public-domain Shinonome fonts and font8x8). Before, every byte of a multi-byte character drew as a blank. A character with no glyph shows as `?`. `test/iso_test.sh` types a Japanese line and reads the screen back.
- The console and pseudo-terminals start with `IUTF8` set, and Backspace takes back a whole UTF-8 character instead of one byte: typing a Japanese word and correcting it no longer leaves half a character in the line. The echo backs over as many columns as the character filled (two for East Asian wide characters; Linux backs over one), so the cursor ends where the erased character began. `IUTF8` can be cleared with `tcsetattr`, which restores erasing a byte at a time. Table of cell widths: `src/utf8.bastio`.
- The framebuffer console obeys ANSI escape sequences (cursor movement, erase, insert and delete of lines and characters, scroll regions, save and restore of the cursor, reverse video and the alternate screen), so a full-screen program such as Neovim draws properly on the local screen, Japanese text included; before, they were skipped. The shell's `echo` writes `\e` as ESC, which `test/iso_test.sh` uses to drive the screen and read it back.
- The arrow keys, Home, End, Delete, Page Up and Page Down on a PS/2 or USB keyboard send the escape sequences a terminal sends (`ESC [ A` and so on, `ESC [ 3 ~`); before they were dropped, so Neovim and line editors could not be driven by them on the local console. `test/iso_test.sh` presses them under QEMU.
- Keys typed at the console reach a program that waits in `epoll_wait`,
  `select` or `pselect6` (libuv, and so Neovim); before, they stayed queued
  for the native shell unless the program was in `poll`, `ppoll` or `read`.

### Processors

- `nanosleep` and `clock_nanosleep` sleep until the next tick between
  looks (`wait_asleep`, as `poll` and `pause` do) instead of running again
  on every scheduler pass: BusyBox's `sleep 5` went from 5.0 s of host CPU
  under QEMU to about 0.2 s. A sleep may now end up to a tick (10 ms) late.
- An idle application processor stops its 10 ms timer (a one-second
  one-shot backstop instead); the kernel wakes it with `V_WAKE` on the
  bootstrap processor's tick while a process is left ready, which is when
  its own tick would have found it. An idle 4-processor guest takes half the
  host CPU under QEMU (7.3% to 3.6% of a core). The hardware report says
  whether it applies (`idle:`); with x2APIC every timer keeps ticking.
- The code pages a program reads when it first runs them (see below) come
  from the page cache, so every process running the program maps the same
  read-only page and the disk is read once; before, each process read and
  copied its own. A fault whose blocks are not in the disk's cache sleeps
  until they are read, instead of the kernel polling the disk with its
  lock held. Four compiler builds at once went from 7.2-9.1 s to 5.5-5.6 s
  on four processors and from 8.5-8.9 s to 4.8-5.3 s on one.
- virtio-blk reads a run of up to eight consecutive blocks in one request
  (the blocks a read queues ahead), instead of one request per 4 KiB
  block: under QEMU each request is a notify and a trip through its I/O
  thread, made under the kernel lock. Four Z3 starts in a row went from
  1.3 s to 0.7 s, and four compiler builds at once from 9.4-13.8 s to
  6.8-7.8 s on four processors (10.2-10.8 s to 9.1-9.2 s on one).
- A program from a file system is read when it runs, not all at exec: its
  largest code segment's pages are read from the file the first time the
  program runs them (`src/lazy.bastio`). Z3 has 24 MiB of code; four Z3
  starts in a row went from 4.2 s to 1.5 s, and four compiler builds
  (bastioc1 with Z3 and tcc) at once from 14.8 s to 10.6 s on four
  processors (15.8 s to 12.9 s on one). The process holds its file, so
  removing the file's name while it runs changes nothing. The kernel
  reading such a page for a system call before the program ran it (a
  `write` straight from the program's code) gets `EFAULT`.
- Programs on a file system share their code: exec maps each whole page
  that only read-only segments cover from the page cache, read-only,
  instead of copying it into a fresh page, as built-in programs already
  share theirs. Four shells running BusyBox pipelines from the disk at once
  went from 9.0 s on one processor and 6.6 s on four to 1.3 s on either
  (QEMU, which also no longer translates BusyBox's code again for every
  process). A process that mprotects such a page writable gets its own
  copy first.
- Four shells running BusyBox pipelines from the disk at once went from
  17.0 s on four processors (15.4 s on one) to 6.4 s (8.6 s). The block
  cache, at 256 KiB, was smaller than BusyBox, so every exec read the whole
  program from the disk again, a block at a time, under the kernel lock.
  It is now 1 MiB.
- The read-only pages of the built-in programs (code and constants) are
  made once and shared, read-only, by every process running the program
  (`src/text.bastio`, bastiolang's `Shared`), so `execve` maps them instead
  of copying them out of the image and `fork` maps them again instead of
  copying them. `fork` copies the rest into pages it does not zero first
  (bastiolang's `Dirty`). A process that makes such a page writable with
  `mprotect` gets its own copy. `make forkbench` (4 shells starting a
  program 100 times each): 3.8 s → 0.6 s on one processor, 5.4 s → 0.7 s
  on four. `mem` counts the cached pages as free.
- A vDSO (`user/vdso.c`): `clock_gettime` for the monotonic and real-time
  clocks runs in user mode, with no system call and no kernel lock. The
  kernel now keeps time with the time-stamp counter, its rate measured
  against the HPET at boot (`src/tsc.bastio`), and copies the clock's
  parameters into each Linux process's vDSO page, so both read one clock.
  Four threads reading the clock 20000 times each went from about 2.3 s on
  one processor and 4.1 s on four to under 30 ms (`MODE=clock
  test/threadbench.sh`). Needs bastiolang's `cpu_tsc`.
- The threads of a Linux process run on several processors at once. They
  used to take turns on one: four threads of pure computation now finish
  about twice as fast on four processors under QEMU.
- `getpid`, `gettid`, `getuid` (and the other id calls) and `sched_yield`
  with nothing else waiting are answered without the kernel lock: 4 copies
  of `bench` at once on 4 processors make 20000 `getpid` calls in about
  25 ticks instead of about 250.
- x2APIC. A firmware that leaves the local APICs in x2APIC mode (SeaBIOS
  and OVMF do when a machine may have APIC ids of 255 and up; many real
  PCs do with VT-d on) used to hang the kernel at boot. It now takes them
  back to xAPIC, unless the firmware locked x2APIC mode or a processor has
  an id of 255 or more: then it runs them in x2APIC mode, and processors
  the MADT lists only as x2APIC entries are started too. In x2APIC mode
  the kernel does without MSI and wake-up interrupts for now (its own APIC
  code, src/lapic.bastio, reaches the registers only as memory). Tested
  with QEMU 9.2 (`QEMU_X2APIC=... make test`; QEMU 8 cannot emulate
  x2APIC without KVM).
- A PIT that does not count. Many recent chipsets gate the 8254's clock
  off by default, and the bootstrap processor took its 10 ms tick only from
  the PIT: no tick ever came, `uptime` stayed at 0 and nothing that waits
  on time ran. The kernel now checks at boot whether PIT channel 0 counts
  and, if not, has the local APIC timer raise the same interrupt every
  10 ms (`boot/smp.c`). The hardware report's `tick:` line says which.
  Tested with QEMU's `-machine q35,pit=off`.
- A clock without the HPET or the ACPI PM timer. Many recent PCs turn both
  off, and the clock then stood still at 0 ns. `boot/smp.c` now measures
  the time-stamp counter's rate against PIT channel 2, or, when that is
  gated too, against one second of the RTC (between two of its updates),
  and the clock runs on that; the local APIC timer is counted against the
  same. Where neither the PIT nor the RTC answers, the PIT channel 2 wait
  is bounded by the time-stamp counter (it used to poll up to 10^8 port
  reads). The PIT-less tick works without a MADT too. Tested with QEMU's
  `-machine pc,acpi=off,hpet=off` and `...,pit=off`.
- SATA ports the firmware did not bring up. A controller with staggered
  spin-up keeps each disk off until its port's SUD bit is set, and firmware
  that boots fast from another disk may leave a link down. The AHCI driver
  now sets SUD (and POD) on every port, gives a port whose link is still
  down a COMRESET, and tries a port whose device has not yet sent its
  signature once FIS receiving is on, letting it go again if it is not a
  SATA disk. The boot option `ahci_reset` resets every port with a device,
  for disks that are still not found. Tested with that option in QEMU (a CD
  on port 0, the disk on port 3); staggered spin-up itself is untested, as
  QEMU's AHCI does not have it.
- PCs with no PS/2 controller at all. Its ports read 0xFF, which the
  keyboard driver took for a mouse byte always waiting and polled for on
  every turn of the console. The driver now notices at boot and leaves
  the ports alone; the hardware report says `keyboard: none found` when
  there is no USB keyboard either. Tested with `-machine pc,i8042=off`.
- NVMe drives whose namespace 1 is not active. The driver used namespace 1
  unconditionally; it now asks for the active namespace list and takes the
  first one. Tested with a QEMU NVMe controller whose only namespace is 2.

### Linux programs

- Python development works in a chroot of the official `python:alpine` image: `python3 -m venv`, `pip install` of pure-Python, source and binary musl packages (a C extension built by gcc; numpy, cryptography, lxml, Pillow, PyYAML, greenlet, psutil), the whole test suite of a real project (Jinja2's 784 tests) and `pyprogs/stdlib.py` run in the compatibility suite (`python.txt`). Gaps it found, now closed: a unix socket bound by a process in a chroot read back (`getsockname`, `recvfrom`) with the chroot's own path in front, so `multiprocessing` managers could not connect to their own address; a `/dev/ptmx` made by `mknod 5:2` in a chroot did not open as a terminal master; `getrusage` reported `ru_maxrss` 0; `/proc/PID/status` lacked `VmSize`, `VmRSS` and the context switch counts; `mbind`, `set_mempolicy` and `get_mempolicy` (numpy) answered ENOSYS; `ioctl(SIOCETHTOOL)` answered ENOTTY where psutil expects EOPNOTSUPP.
- `clone` and `clone3` with `CLONE_FILES` make a process that shares its parent's descriptor table: what one opens, closes or `dup`s the other sees, until one of them calls `unshare(CLONE_FILES)`, `close_range` with `CLOSE_RANGE_UNSHARE` or `execve`; `kcmp(KCMP_FILES)` tells which processes share. Threads use the same mechanism (a ring of processes through `F_FDNEXT`, the table in the row of the member whose call is being handled) in place of moving it with the address space. `/proc/sys/fs/nr_open` is writable, and `unshare(CLONE_FILES)` fails with `EMFILE` when a descriptor is open past it. `clone` with `CLONE_FS` shares the working directory, root and umask the same way (`kcmp(KCMP_FS)`, `unshare(CLONE_FS)`, and `unshare(CLONE_NEWNS)` leave the sharing); a process in a thread group cannot clone with it, nor create threads while it shares.
- `pidfd_open` takes `PIDFD_THREAD`, `pidfd_send_signal` takes `PIDFD_SIGNAL_THREAD`, `PIDFD_SIGNAL_THREAD_GROUP` and `PIDFD_SIGNAL_PROCESS_GROUP` (more than one is `EINVAL`), and `clone3` with `CLONE_PIDFD` works together with `CLONE_THREAD`, so the new thread gets a thread pidfd. A forked child no longer inherits its parent's `CLONE_FS` ring links. `t_pidapi` covers `clone3`, `waitid(P_PIDFD)`, `pidfd_getfd` and the error cases. `clone3` accepts `CLONE_PIDFD` together with `CLONE_PARENT_SETTID` when they name different words, and a `wait4` interrupted by the exit signal of a child that has already ended reaps it instead of failing with `EINTR` (clone301). `clone(CLONE_VM|CLONE_VFORK|CLONE_PIDFD)`, the call Go's os/exec makes, stores a pidfd as well.
- A symbolic link or a device node made by `symlink` or `mknod` now belongs to the user who made it (it always belonged to root), so its owner can set its times (`touch -h`), and a device node takes the umask. `abuild` builds Alpine packages as an unprivileged user under `fakeroot`, signs them and installs them with `apk` (libraries with `-dev` and `-doc` subpackages, patches, `check()`, install scripts, tinycc from its source; `docs/using.md`, `test/compat/alpine.txt`).
- A `bwm` client can give its window's pixels as shared memory: `BWM_CREATE_SHM` carries a `memfd` over the socket with `SCM_RIGHTS`, the client writes into it and a `BWM_DRAW` with no pixels names the rectangle that changed (`bwm_create_shm` and `bwm_damage` in `user/bwmlib.h`). `bterm` uses it and `bhello -s` shows it; `test/iso_test.sh` checks the pixels on the screendump.
- `bterm` (`user/bterm.c`), a terminal window for `bwm`: an 80 x 24 grid drawn with the console font, with BusyBox's `ash` on a pseudo-terminal behind it. It reads a VT100 subset (CR, LF, backspace, tab, cursor moves, erase, insert and delete of characters and lines, scroll regions, the alternate screen, colours) and sends keys to the shell. `test/iso_test.sh` types a command into it and reads the output back cell by cell from the screendump.

- `bwm` takes windows from other programs over a UNIX socket at `/tmp/bwm.sock` (`user/bwmproto.h`, with a header-only client library in `user/bwmlib.h`): a client opens a window, draws rectangles of pixels, and gets key, pointer and close events back; the pixels are kept by bwm and the close box tells the client. `bwm -n` starts with no windows of its own and `bwm -c PROGRAM` starts a client. `user/bhello.c` is the test client; `test/iso_test.sh` checks its pixels on a screendump and the key, click and close round trips.

- `bwm` (`user/bwm.c`), a window manager for `/dev/fb0` and `/dev/input/mice`: a wallpaper, three windows with title bars, a pointer, click to raise and focus, drag a title bar to move a window, typing into the focused window, Ctrl-Q to leave. Frames are composed in memory and only the changed rectangles are written. `test/iso_test.sh` drives it through QEMU's monitor (mouse and keys) and reads the screen back. The windows are its own; there is no protocol for other programs yet.

- `/dev/fb0`: the screen for programs that draw their own pixels (`src/lxfb.bastio`). `FBIOGET_VSCREENINFO` and `FBIOGET_FSCREENINFO` give the 32-bit mode; `write` and `lseek` store pixels. The text console stops drawing while a description is open and is drawn again when the last one closes. `user/mfb.c` draws a pattern, and `test/iso_test.sh` reads it back from a QEMU screendump under BIOS and UEFI. The framebuffer stays inside the proved device window; the pixels reach it through a ring the console pump empties. There is no `mmap` of it yet.

- A read of a regular file on the disk is no longer cut short when some of its blocks are not cached yet: `read`, `pread64` and the calls built on them wait for the disk and return the whole count (they returned the cached part first, and a program that takes the count for granted, such as the JVM reading its modules file, got zeros for the rest). The JDK of the eclipse-temurin image (`java`, `javac`, `jar`; records, streams, threads, files, exit status) now runs in the compatibility suite; the lane unpacks the image's root (about 400 MB) from a tar file on its disk.
- Real-time signals queue: a signal from 32 up sent while one is pending waits behind it, with its own sender and `sigqueue` value, up to 32 instances per thread (`sigtimedwait`, `signalfd` and delivery take them first in, first out; standard signals still merge, and `RLIMIT_SIGPENDING` counts the queue). A signal sent to a process goes to a thread that does not block it, also from `kill(-pgrp)`.
- Signal information: a fault's `si_code` is `SEGV_ACCERR` for a protection fault or a `PROT_NONE` reservation, `ILL_ILLOPN` for a bad opcode and `SI_KERNEL` for a general protection fault; `SIGCHLD` carries `si_pid`, `si_status` and `si_code` (`CLD_EXITED`, `CLD_KILLED`, `CLD_DUMPED`, `CLD_STOPPED`, `CLD_TRAPPED`, `CLD_CONTINUED`); a POSIX timer's signal carries its `sigev_value` with `SI_TIMER`. `SIGCHLD` set to `SIG_IGN`, or with `SA_NOCLDWAIT`, reaps children as they end, so `wait` fails with `ECHILD` once none is left. `t_sig3` checks these and the rest of signal delivery (`SA_RESTART`, delivery order, `SA_NODEFER`, `sigaltstack`, job control, `signalfd`).
- ptrace: `PTRACE_O_TRACEEXIT` stops the tracee before `exit` and `exit_group` finish; `PTRACE_O_TRACEVFORKDONE` follows the vfork event; a stop signal stops a seized tracee with `PTRACE_EVENT_STOP`, `PTRACE_LISTEN` keeps it stopped until `SIGCONT`; `PTRACE_GETSIGINFO` returns the signal's real siginfo; a single step taken while `SIGTRAP` is blocked (inside musl's `raise`) stops the tracee instead of killing it; the parent of a zombie that its tracer reaped hears of the end then. `t_ptrace2` checks these and the event, `EXITKILL` and attach corners.
- A writable `MAP_SHARED` mapping of a file is written back even after a program has mapped twenty files read-only (a writable mapping of a file takes the place of a read-only file's mapping when a process's 16 write-back records are all in use), and `munmap` or `msync` of a range of a disk file larger than the disk's write queue now finishes (it waited for the disk and started over from the first page, for ever). Go's linker does both; the Go toolchain of the golang image (`go run`, `go build`, `go test`, `gofmt`; goroutines and the collector) now runs in the compatibility suite.
- The Rust toolchain of the rust:alpine image (`rustc`, `cargo build`, `cargo run`, `cargo test`, threads, files, panics) runs in the compatibility suite, with no kernel change; the lane unpacks the image's root (about 1 GB) from a tar file on its disk.
- ptrace for debuggers: a tracer can write into the read-only code of a program it traces (a breakpoint), read code the program has not run yet, and read and write its x87/SSE registers (`PTRACE_GETFPREGS`, `SETFPREGS`); a breakpoint trap says `SI_KERNEL` and a single step `TRAP_TRACE` in `PTRACE_GETSIGINFO`; `strace -p` hears of the death of a process whose parent is elsewhere. `/proc/PID/auxv` exists (gdb finds where a PIE program was loaded), and lseek on it moves its offset (lseek on the other /proc files is still ignored). Tested: `strace -p`, and gdb 16 stops at a breakpoint of a dynamically linked PIE program, prints its arguments and backtrace and finishes the function, and attaches to a running one (`gdb -p`), stops it at a breakpoint and detaches (x86-64; it needs Alpaquita's musl loader for ifunc relocations and an x86-64-v2 CPU, which the compat lane's QEMU now asks for).
- `/proc/PID/mem`: read and write another process's memory (or the program's own) at an address, with `read`, `write`, `pread64`, `pwrite64` and `lseek`. A write goes into read-only code too, as ptrace's poke does, and a read finds code the program has not run yet. Allowed for the process itself, root, and a process with the same ids. gdb no longer warns that it cannot open it. Tested in `t_ptrace` (own memory, a child's).
- Core files: a process that dies of a signal that dumps core (SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGQUIT, SIGTRAP, SIGSYS, SIGXCPU, SIGXFSZ) with `RLIMIT_CORE` above 0 leaves an ELF core file called `core` in its working directory, up to the limit: the registers of the thread that took the signal, the auxiliary vector, and the pages the process had touched. `gdb PROGRAM core` shows the call chain and the arguments. x86-64 only for now; the default limit is still 0, as on Linux. The wait status of such a process carries the core flag (`WCOREDUMP`, and `CLD_DUMPED` in `waitid`). Tested in `t_core` and with gdb 16 in the Alpine lane.
- Files have owners and permissions (`src/lxperm.bastio`). Each file keeps
  its owner's user and group ids, in memory and on the disk; `chown`,
  `fchown`, `lchown` and `fchownat` change them, and `stat` reports them.
  `open`, `execve`, `truncate` and `access` check the mode bits against
  the caller's ids. A path needs search permission on its directories, and
  making or removing a name needs write permission on its directory. A
  sticky directory keeps others' names. `chmod` and `utime` are for the
  owner. A new file belongs to its maker and takes the process's `umask`,
  which is kept per process now (it was always 022). `/tmp` is 01777, and
  BusyBox's `chown` is built in.
- Anonymous `MAP_SHARED` memory is shared with children: `fork` maps the
  same pages in the child instead of copying them, so a write on one side
  is seen on the other. The pages are counted references (bastiolang's
  `Shared`), given back with the last mapping; `munmap`, `mprotect`,
  `mremap` and exit handle them.
- A futex in a `MAP_SHARED` page reaches every process that maps the page:
  a futex without `FUTEX_PRIVATE_FLAG` on such a page is known by the
  physical page and offset, not by the caller's address. Futexes on private
  memory stay per process.
- A `MAP_SHARED` file mapping is shared with `fork`'s child the same way:
  both see each other's writes at once. It is still written back to the
  file at `msync`, `munmap` and exit.
- A page cache (`src/pcache.bastio`): a `MAP_SHARED` file mapping maps one
  shared page per page of the file, so separate processes that map the same
  file share it, and `read` and `write` agree with the mapping before it is
  written back. See [docs/design/page-cache.md](docs/design/page-cache.md).
- `FUTEX_REQUEUE` and `FUTEX_CMP_REQUEUE` move waiters with a timeout too:
  they used to go back to the old word on their next check of the clock.
  Negative counts give `EINVAL`, and a futex that is not private on an
  unmapped word gives `EFAULT`, as on Linux.
- `mmap` of a file gives `EACCES` when the descriptor was not opened for
  reading, or for a writable `MAP_SHARED` mapping when it was not opened
  for writing (or was opened `O_APPEND`).
- Dynamically linked programs: the kernel loads a PIE's interpreter, and
  carries musl's `libc.so` for `/lib/ld-musl-x86_64.so.1`. `/bin/sh` is
  BusyBox's `ash`, so `system` and `popen` work; `waitid`, `times`.
- Threads: `clone(CLONE_THREAD)` as `pthread_create` makes it, `futex`
  and `set_tid_address`, so musl's mutexes, condition variables and
  joins work.
- `eventfd`, `timerfd` and `epoll`; `select`, `pselect6`, `alarm`,
  `setitimer` and `pause`; `rt_sigsuspend`, `rt_sigtimedwait`,
  `rt_sigpending`, and `SIGCHLD` to a Linux parent.
- `signalfd` and `signalfd4`: read, poll and epoll on the signals in its
  set. A blocked signal is now kept pending even when its action ignores
  it, as on Linux, so a blocked `SIGCHLD` reaches a signalfd or `sigwait`.
- POSIX timers: `timer_create` (SIGEV_SIGNAL and SIGEV_NONE), `timer_settime`
  (relative or absolute), `timer_gettime`, `timer_getoverrun` and
  `timer_delete`, four per process.
- `AF_UNIX` stream sockets: `socketpair`, and `bind`, `listen`,
  `connect` and `accept` on a name.
- `sendmsg` and `recvmsg`, with `SCM_RIGHTS` passing a descriptor over an
  `AF_UNIX` socket (one per message, one in flight per direction);
  `MSG_PEEK` on TCP and unix sockets; `shutdown` on TCP (sends the FIN)
  and unix sockets; `MSG_NOSIGNAL`; `SO_SNDBUF` and `SO_RCVBUF`;
  `AF_UNIX` `SOCK_DGRAM` and `SOCK_SEQPACKET` socket pairs, which keep
  message boundaries; and `rt_sigqueueinfo` (`sigqueue`), whose value
  reaches the handler's siginfo. A new compat program, `t_sock2`, checks
  these.
- `AF_INET6` sockets, TCP and UDP, over the IPv4 stack: `::`, `::1` and
  `::ffff:a.b.c.d` work, and names come back as `sockaddr_in6`. There is
  no IPv6 on the wire.
- `posix_spawn` (`clone(CLONE_VM | CLONE_VFORK)`), process groups and
  sessions, `tcgetpgrp` and `ttyname`, and `execve` of `/proc/PID/exe`, so
  ash runs a script with no `#!` line.
- The small calls programs make: `uname`, `readv`, `pread64`/`pwrite64`,
  limits, `sysinfo`, `statfs`, `getrandom`, priorities, `umask`; `mmap` of
  files and `MAP_FIXED`; the `*at` calls relative to a directory
  descriptor, `fchdir` and `faccessat`; `lseek` with `SEEK_END`,
  nonblocking pipes, a wall clock, `/dev`, `/tmp` and `/etc/passwd`.
- 16 open files per process (8 before), and `EMFILE` past them; a pipe
  holds 64 KiB, and a write of up to `PIPE_BUF` bytes goes in whole.
- `munmap`, `mprotect` and `mremap` work: unmapped pages fault, read-only
  pages fault on a write (and stay read-only across `fork`), and `mremap`
  grows a mapping by moving its pages. A user fault now kills the process
  with `SIGSEGV` (or `SIGFPE`, `SIGILL`, `SIGTRAP`), as Linux does, so
  `wait4` and a shell report the signal.
- `msync` writes a `MAP_SHARED` file mapping back to the file, and so do
  `munmap`, exit and `execve` (up to four such mappings per process; the
  mapping is still a copy, so others see the bytes only after a
  write-back).
- A generated, read-only `/proc` for Linux programs: `mounts`,
  `meminfo`, `uptime`, a directory per process with `stat`, `status`,
  `cmdline` and `exe` (a link `readlink` reads), and `/proc/self`.
  BusyBox's `ps`, `free`, `df` and `uptime` work on it.
- `/proc/cpuinfo`, `/proc/filesystems`, `/proc/PID/mounts` (so
  `/proc/self/mounts`) and three read-only sysctls:
  `/proc/sys/kernel/tainted`, `pid_max` and `printk`.
- Symbolic links: `symlink`/`symlinkat` make them in memory or on the
  disk, every path a Linux program passes is followed through them (at
  most 8 links), and `lstat`, `readlink`, `unlink` and `rename` act on
  the link itself. BusyBox's `ln` and `readlink` are built in.
- Hard links: `link`/`linkat` give a file another name on the same file
  system (not a directory), `st_nlink` counts the names, and the file
  lives until its last name goes. On the disk a link is a new entry kind
  (4) naming the file's entry; a mount drops any link whose file is gone.
- Named pipes live in the file system: `mknod`/`mkfifo` make a file with
  the FIFO type bits, so a FIFO is listed (type `p`), renamed, linked and
  removed like any file, and on the disk it survives a reboot. Only four
  can be open at once; any number can exist. BusyBox's `mkfifo` is built
  in.
- File locks are kept (`src/lxlock.bastio`): `fcntl` record locks
  (`F_GETLK`, `F_SETLK`, `F_SETLKW`), open file description locks
  (`F_OFD_*`) and `flock`, with Linux's owners and release rules. A
  conflicting `F_SETLKW` or blocking `flock` waits, and an `F_SETLKW` that
  would close a cycle of waits fails with `EDEADLK`. Checked by the new
  compat program `t_lock`.
- `sendfile`, `copy_file_range`, `preadv`, `pwritev`, `statx`,
  `fallocate` (growing), `fadvise64`, `memfd_create`, `getcpu`, and
  `FIONREAD` on a pipe. A new compat program, `t_more`, checks these and other calls
  real programs make.
- `mount` and `umount2` for tmpfs (`src/lxmount.bastio`): a tmpfs mounts
  on an empty in-memory directory, `MS_RDONLY` makes it read-only
  (`EROFS`) until an `MS_REMOUNT`, and unmounting removes what is in it.
  Mounts are listed in `/proc/mounts` and `tmpfs` in
  `/proc/filesystems`. Other file system types say `ENODEV`.
- Fixes from the LTP file-system tests: a path through a file fails with
  `ENOTDIR`, a loop of symbolic links with `ELOOP`, a too-long name with
  `ENAMETOOLONG`, and a bad directory descriptor with `EBADF`/`ENOTDIR`
  (they all said `ENOENT`). `renameat2` takes `RENAME_EXCHANGE`, `mknod`
  makes regular files, one `write` moves more than 4096 bytes, `"link/"`
  names what the link points to, `linkat` across file systems says
  `EXDEV`, `newfstatat` rejects unknown flags, a full in-memory table
  makes `mkdir` say `ENOSPC`, and `O_TMPFILE` is refused with
  `EOPNOTSUPP` (it opened the directory).
- Fixes from LTP's signal tests: waits a `SA_RESTART` handler interrupts,
  `epoll_pwait`'s mask, `SA_RESETHAND`, `SA_ONSTACK`; the user stack grows
  to 512 KiB.
- `ppoll` and `pselect6` wait with the signal mask they are given (GNU
  make `-j` hung waiting for SIGCHLD).
- `FUTEX_CLOCK_REALTIME` deadlines are read on the wall clock.
- `set_robust_list`, `rseq` and `clone3`, which glibc asks for, fail with
  `ENOSYS` quietly; glibc carries on without them (static glibc programs
  run).
- Reading a `/proc/PID` file of a process that ended after the file was
  opened fails with `ESRCH`, as on Linux, instead of giving an empty file.
  BusyBox's `ps` wrote through NULL when a process ended while it ran.

### Users and permissions

- `docs/security.md` audits what BastiOS enforces between users and
  processes, and lists what it does not yet.
- The native system calls (`int 0x80`) are root's, except console I/O,
  starting and waiting for children, yielding, the pid, `SYS_INFO`, the
  process list and a `SYS_KILL` that `kill`'s rule allows. Any program can
  raise `int 0x80`, so before this an unprivileged Linux process could
  open, unlink, kill and reboot through it with no check. A native
  `SYS_SPAWN` child keeps its parent's ids instead of starting as root.
- `reboot`, `mount` and `umount2` are root's (`EPERM`), `rt_sigqueueinfo`
  follows `kill`'s rule, and binding a port below 1024 is root's
  (`EACCES`). `test/compat/progs/t_isol.c` tries each as nobody.
- A `MAP_SHARED` mapping of a file opened read-only is a copy rather than
  the page cache's pages: before, `mprotect(PROT_WRITE)` on it let a
  process that may only read a file change what every other reader of
  it saw.
- `bind` of an `AF_UNIX` socket to a path makes a socket file there, and
  `connect` needs write permission on it, as on Linux. Before, the name
  lived only in a table of its own, so any user could connect to any
  socket, or take its name first. `open` of a socket file is `ENXIO`.
- The loop devices are `rw-rw----` for root and the new group disk (6),
  and open checks it.
- `readlink` of another user's `/proc/PID/exe` or `/proc/PID/fd/N` is
  `EACCES`, as Linux's ptrace read check gives; before, anyone could see
  which program and which files any process had open. `/dev/console` is
  root's, as its `rw--w----` mode already said: before, any user could
  open the console and read what was typed on it. SysV IPC's group bits
  count supplementary groups, and `SHM_LOCK` by an owner who is not root
  needs a nonzero `RLIMIT_MEMLOCK`, both as on Linux.
- Users and passwords: `/etc/shadow` (`0600`), `/root` and `/home`, and
  BusyBox's `adduser`, `addgroup`, `deluser`, `delgroup`, `passwd`,
  `chpasswd`, `su`, `login`, `id` and `whoami` as `/bin/NAME`. `/bin/su`
  and `/bin/passwd` are set-user-ID root, and BusyBox
  (`CONFIG_FEATURE_SUID`) drops root for every other applet. Passwords
  are SHA-512.
- With a disk, `/etc`, `/root` and `/home` live on it (symbolic links to
  `/disk/etc`, `/disk/root` and `/disk/home`), so users and their files
  survive a reboot. The first boot seeds `/disk/etc` from the built-in files.
- A loop-mounted `bstdisk3` image is `nosuid`: its set-user-ID and
  set-group-ID files run with the caller's ids.
- The boot option `login` starts BusyBox's `login` on the console instead
  of the native shell, and starts a new one after each logout.
- `mprotect(PROT_WRITE)` on a `MAP_SHARED` mapping of a file opened
  read-only fails with `EACCES`, as on Linux (it used to succeed on a
  private copy).
- `connect` finds a unix socket by its socket file, so a renamed socket
  file, or another hard link to it, still leads to the socket.

### Programs that run

- Pseudo-terminals (`src/lxpty.bastio`): `/dev/ptmx` and `/dev/pts/N`
  (four pairs), so `openpty`, `ttyname` and SSH servers work. The line
  discipline between the ends is the console's: echo, canonical lines with
  erase and kill, Ctrl-D, `ICRNL`, `ONLCR`, raw mode, and Ctrl-C and
  Ctrl-\\ sending SIGINT and SIGQUIT to the foreground group. `TIOCSCTTY`
  makes one a session's terminal (`/dev/tty` then opens it),
  `TIOCSPGRP`/`TIOCGPGRP` move the foreground group, `TIOCSWINSZ` sends
  SIGWINCH, closing the master hangs up the session (SIGHUP), and a master
  read fails with `EIO` once the slave is closed. `test/compat/progs/t_pty.c`
  checks each. No `SIGTTIN`/`SIGTTOU`, `VSUSP`, flow control or packet mode.

- Real programs in `make compat` (`test/compat/apps.txt`): Lua 5.4.7,
  QuickJS and the sqlite3 3.52 shell, built from source as static PIEs,
  run their files, pipes, timers, WAL databases and a 20000-row index.
- C can be built on BastiOS: tcc and GNU make 4.4.1, built as static PIEs,
  compile and link programs (dynamic PIEs against musl's `libc.so`) and
  run makefiles, `-j` included. `make compat` boots a second disk with
  them and a small C runtime and runs `test/compat/dev.txt`.
- The Bastio compiler runs on BastiOS: bastiolang's self-hosted `bastioc1`
  (built with musl), Z3 4.8.12 (a static glibc build) and tcc prove a
  Bastio program, report a failed proof with its counterexample, and build
  and run the program, all inside BastiOS. tcc gains GNU `__auto_type`
  (`test/compat/patches/tcc-auto-type.patch`), which the generated C uses.
  `make compat` boots a third disk with them and runs
  `test/compat/bastio.txt`. Not yet: `bastioc1` proving itself, which needs
  about 320 MiB, more than a process's 64 MiB window and the kernel's
  256 MiB page pool.
- More real programs in `make compat`: MicroPython 1.24 (scripts, threads,
  its REPL), BusyBox `vi`, the kilo editor, and BusyBox `wget`, `httpd` and
  `nc` talking HTTP with a web server on the host (`run.py` runs one) and
  over loopback. A new group, `test/compat/tty.txt`, types keys at
  full-screen programs on the console and checks what they wrote.
- git 2.47 and Perl 5.40 in `make compat`: git commits, branches, merges
  300 files, packs with `gc`, clones through `upload-pack` and runs
  `bisect run`; Perl runs its core modules, fork, signals, sockets and
  HTTP::Tiny from the host. They found three gaps, fixed here: `execve`
  passed at most 32 arguments, 16 environment strings and 1280 bytes of
  them, now 8192 arguments, 1024 environment strings and 128 KiB, as on
  Linux (`sed -i ... f*` over 300 files failed); `munmap` never gave the
  address space back, so a program that maps and unmaps as it runs, as
  musl's `malloc` does for every large block, used up its window (`git
  add` of 300 files ran out); and the in-memory file system held 128
  files, and a create past that failed with `ENOENT` instead of `ENOSPC`.

### Build and test

- For contributors: `tools/setup-dev.sh` sets up a Debian or Ubuntu machine in one command (and a dev container runs it), CONTRIBUTING.md explains the issue labels and where to find a first issue, and a feature-request issue template and `.github/labels.yml` (synced by a workflow) were added.
- `test/stackdepth.py` checks that the deepest chain of kernel calls in a native kernel fits the kernel stack (it fails above 64 KiB of the 92 KiB; today 41 KiB). It was written after a native kernel built with an older compiler overflowed its stack in the UDP path under a loaded test run: its functions had frames up to 19 KiB where the current compiler's are under 4.5 KiB (the boot function aside).

### Files and disks

- The system holds 4096 open file descriptions at once (1024 before), and 2048 pipes (512). Eight processes with about 230 each are tested (`user/mfd.c`). The table of open directories' names (`dpath`) took 2 MiB at boot whatever happened; it starts empty and grows as directories are opened, so a small machine has that memory back. Proved: unchanged (the same table bounds, now larger; a directory whose name could not be stored for lack of memory reads as having an empty name).
- A process may hold 512 open files (256 before): a server with a few hundred connections, or a build that opens many files at once, no longer meets `EMFILE`. The tables of descriptors grow with the process table as before. `pipe2` and `socketpair` wrote a descriptor number into one byte of the result, which only worked below 256; they now write all four bytes. Tested in `user/mdir.c` (a pipe made from the last two of 512 descriptors) and `user/fstest.bastio` (exactly 512, then none). Proved: the bounds hold at the larger sizes; the arithmetic on a descriptor number written to a byte had to be split into two bytes for the proof to go through.
- A process may hold 1024 open descriptors (512 before), the usual soft limit. The files they name now take rows from one pool of 4096 (as many as there are open file descriptions) instead of a block of 512 rows in every process, so a process costs 8 KiB for its descriptors, not 40 KiB: the 120-process fork case no longer runs the kernel's arena out at 1024. The native calls keep 64 rows per process. Proved: the whole kernel is discharged; the trusted base is unchanged. Tested in `user/mdir.c` (opens until `EMFILE`, a pipe among the last descriptors).
- The disk holds 8191 files and directories (5119 before), the most block 0 can name. The directory table is no longer one 1.25 MiB buffer taken at mount (twice, with the loop mount): it starts at 32 KiB and doubles as directory blocks are added, so a disk with few files costs little heap. The on-disk format is unchanged. Proved: the whole kernel is discharged; the trusted base is unchanged. Tested by the disk-full case in `test/shell_test.sh`.
- The loop mount that no file backs takes 16 block-cache slots (64 KiB), not 256 (1 MiB). A disk with no file system has nothing to cache, so the cache is sized when a disk is mounted: the full 256 slots for a disk with a file system, 16 for none; attaching a file to the loop device mounts it again, at the full size. Proved: the whole kernel is discharged (the slot count became a field of the cache, checked by the same bounds); the trusted base is unchanged. Tested by the existing loop-mount and disk cases in `make test`.
- The system runs 511 processes at once (255 before). `NPROC` is 512 and the sizes of `k.sys`, `k.ipc`, `k.alarms` and `k.holes` follow it (they were numbers worked out for 256). Four places had an id in a byte: the sender of a pending signal (`si_pid` and the value of `sigqueue`, in `k.sigsave` and `signalfd`), the numbers of `/proc/PID/ns`, `fd` and `task` (they were 256 apart), `sysinfo`'s process count, and the rows of the file layer's table. `/proc/sys/kernel/pid_max` says 512. Proved: the whole kernel is discharged; the trusted base is unchanged. Tested by `user/mproc.c` (300 children at once, a signal sent from one with an id past 255, `/proc/PID/fd` of the last), which needs the shell test's 1 GiB of memory: the per-process tables are 24 KiB a process and take a fraction of RAM.
- The disk holds 32767 files and directories (8191 before). The directory is named past 512 blocks by extension blocks (`BSTDISK4`: byte 8 of block 0 names the first, word 0 of each names the next, the other 511 words name directory blocks); see the header of `src/diskfs.bastio`. A disk keeps its old format until its directory outgrows 512 blocks, so `BSTDISK2` and `BSTDISK3` disks mount unchanged, and an older kernel leaves a `BSTDISK4` disk alone as one it does not know. `test/fsck.py` and `test/crashsim.py` read the new format. `/proc` inode numbers start past the file inodes, which the larger directory had made overlap them.
- inotify: 16 instances of 64 watches each (8 of 16 before), as `/proc/sys/fs/inotify` reports. A file watcher over a source tree no longer runs out of watches after sixteen files; `t_inotify` adds sixty watches on one instance.
- Namespaces and cgroups: 32 UTS and 32 user namespaces besides the first (8 before), 64 PID namespaces (16), 128 cgroups (32) and 256 mounts of mount namespaces in all (64). A host running a few dozen containers no longer runs out of namespace slots; `t_nsmany` holds twenty-four UTS, PID and IPC namespaces at once. The tables are still scanned linearly; the namespace state costs about 40 KiB more.
- tmpfs mounts: 64 at once (8 before), so a host running a container per mount point no longer runs out of mount slots. `t_nsmany` mounts 60 and checks each keeps its own files.
- System V IPC: 32 message queues, 32 semaphore sets and 32 shared memory segments at once (16 before). `t_sysv` creates 32 of each, uses every one and checks the 33rd gives `ENOSPC`. The IPC tables take about 0.3 MiB more kernel heap, allocated at boot.
- Memory: 16 MAP_SHARED file mappings per process (4 before; one beyond the limit was never written back), and the tables of address-space holes and of anonymous ranges not yet touched hold 32 records per process (16 before). `t_mem` writes through twelve shared mappings of one file.
- POSIX message queues: 32 at once (8 before). `t_ltp2` creates 32, sends and receives on each, and checks that a 33rd is `ENOSPC`. The queues' memory (names, messages, notification cookies) is allocated at boot and grows by about 0.4 MiB.
- Loop devices: 32 at once (8 before), `/dev/loop0` .. `/dev/loop31` and the same under `/sys/block`. `t_dev` binds loop8 .. loop31 together and checks that loop32 and loop08 do not exist.
- Pseudo-terminals: 16 pairs at once (4 before), so a server can hold sixteen logins. `t_pty` opens sixteen and checks each carries its own bytes.
- epoll, timerfd and named pipes: 16 epoll instances of 256 descriptors each (8 of 64 before), 32 timerfds (8 before) and 16 named pipes open at once (4 before). `t_inotify` adds sixteen instances, two hundred descriptors on one, thirty timerfds and twelve open FIFOs.
- The AArch64 port starts up to 64 processors (16 before), from the device tree or the ACPI MADT. A new test boots QEMU with `-smp 64` and checks the processor count and the power off (`BOOT_ONLY=1`); the full shell test is too slow on 64 emulated processors for a host with few cores. More than 123 processors would need the GICv3's second redistributor region.
- The in-memory file system holds 8192 files and directories (1024 before): a build tree or an unpacked package no longer fills `/tmp`. The name index doubles with it; `ls` in the console shell walks the larger table. Tested by `mdir`, which makes directories until the table is full (thousands); `mdir` fills the table up to 20000 names. What is proved is unchanged (the table has a fixed size and every page is owned by one file); each node still costs kernel memory whether used or not.
- A disk that is not blank and has no BastiOS file system (a partition
  table, another system's data) is left alone. It used to be formatted on
  the first boot, which on a real machine would have destroyed the disk
  with its owner's system on it.
- USB sticks and disks as the disk (`src/xhci.bastio`): SCSI over the
  bulk-only transport, on the same xHCI controller as the USB keyboard.
  Only a GPT partition of the BastiOS type is used, and it comes before
  the machine's own disks, which are then left alone. `make usb` builds
  `build/bastios-usb.img`, the ISO plus that partition, so the stick
  BastiOS boots from keeps its files (`tools/mkusb.py`); `make run-usb`
  boots it in QEMU. Checked by `test/shell_test.sh` and, booting from
  the stick under BIOS and UEFI, `test/iso_test.sh`.
- USB hubs: the keyboard and the stick are found behind up to two tiers of
  USB 2 hubs, as on a PC's front panel or a monitor's ports.
- The USB stick finishes its commands by interrupt (MSI-X on the xHCI
  event ring) instead of being polled, so a process waiting for it sleeps
  and an idle processor halts. `DISK=usb make iobench` measures it.
- An NVMe drive as the disk (`src/nvme.bastio`): polled, one I/O queue,
  namespace 1 with 512-byte or 4 KiB blocks, each write flushed. It is picked
  before virtio and ATA.
- A SATA disk on an AHCI controller (`src/ahci.bastio`): polled, command
  slot 0 of the first port with a disk, each write flushed. It is picked
  after virtio, before legacy ATA.
- NVMe namespaces with 4 KiB logical blocks are used; they were skipped.
  AHCI disks with sectors longer than 512 bytes are now recognized and
  left alone instead of being misdriven.
- PCI devices are found on every bus and function, not only the
  function-0 devices of bus 0 (NVMe drives sit behind root ports, and
  q35's AHCI controller is function 2).
- Every disk driver moves a 4 KiB file system block with one command and
  one cache flush instead of eight: writing a 2 MiB file is 3.4 to 3.9
  times faster on NVMe, virtio and AHCI, 1.5 times on ATA (numbers in
  docs/architecture.md; `make diskbench` measures them).
- A process waiting for the disk sleeps, and the others run. File data
  goes through a 64-block cache and an ordered queue of disk commands
  (`src/diskq.bastio`). A read that misses queues its blocks and read-ahead
  and restarts when they arrive. A write queues its blocks, a flush, its
  directory block and a flush, and returns once they are done. While a
  disk writer runs, `bench`'s getpid takes 120-183 ticks instead of
  240-260 (`make iobench`).
- Disk completions arrive by interrupt: MSI-X on NVMe and virtio, MSI on
  AHCI, to the bootstrap processor's local APIC, which the kernel now
  turns on. A processor with nothing to run halts instead of polling the
  disk, so `bench`'s getpid beside a disk writer takes 97-137 ticks
  instead of 122-191. Under QEMU's TCG, disk throughput drops 10-20%
  (numbers in docs/architecture.md).
- Blocks are copied in bulk (bastiolang's `bcopy` and friends) instead of
  a byte at a time: between user memory, the disk cache, the devices'
  pages, the network buffers and the pipe and AF_UNIX rings. Under QEMU,
  `diskbench` reads three to four times faster and writes up to a third
  faster.
- Files on the disk can grow to 1 GiB (as far as the disk allows) and in
  memory to 64 MiB; they were capped at 1 MiB.
- Paths can be up to 255 bytes; they were capped at 28.
- The in-memory file system holds 1024 files and directories (it held 32).
- The disk's directory grows: up to 1023 files and directories under
  `disk/` (it held 64). The format is now `BSTDISK3`, which names the
  directory's extra blocks in the superblock; a `BSTDISK2` disk is mounted
  as it is and upgraded in place. The power-cut test now grows the
  directory while the power is cut, and `test/fsck.py` checks the new
  blocks.
- A write to a file on the disk lands whole or not at all across a power
  cut, even one that overwrites the middle of a file across two blocks.
- Each file and directory keeps permission bits and a modification time,
  in memory and on the disk. Linux `chmod`, `fchmod`, `fchmodat`,
  `utime`, `utimes`, `futimesat` and `utimensat` change them, and `stat`
  reports them with an inode number per file. BusyBox's `chmod`, `touch`
  and `stat` applets are built in.
- Removing a file that is open works as on Linux: the name goes at once
  and the data stays readable through the open descriptors until the
  last one is closed. On the disk such a file is an orphan entry, and a
  mount frees any a power cut left behind.
- Renaming files and directories: `vfs_rename`, system call 22, and the
  shell's `mv`. On the disk a rename is one sector write, so a power cut
  leaves the old name or the new one.
- Setting an open file's length (`vfs_truncate`, system call 23):
  shrinking frees what lies past the new end, growing reads as zeros.
  On the disk it is copy-on-write like a write, so a power cut leaves the
  old length or the new one.
- Removing the name a disk file was made under while a hard link remains
  is committed by one sector write (`disk_take_name`); a rename between
  two names of one file does nothing, as on Linux.
- The power-cut test cuts power fifteen times and also truncates,
  renames and links; `test/fsck.py` checks the disk image after every
  cut (`test/fsck_test.py` shows it catches damage). Native call 24
  (`SYS_LINK`, `f_link`) makes a hard link.
- The ATA driver waits longer for the drive (a cache flush waits on the
  host's fsync).
- A BastiOS file system on a disk image: `mkfs.bstdisk3 IMAGE` formats an
  in-memory file, and `mount -t bstdisk3 IMAGE DIR` mounts it on an empty
  in-memory directory as a second disk file system (`vfs_lo_mount`). It
  is the same code and format as `disk/`, over a driver that reads and
  writes the image's pages (`src/loopblk.bastio`). One image at a time;
  it can hold more files than the in-memory file system. BusyBox's
  `mount`, `umount` and `truncate` are built in.

### Network

- Network namespaces for containers: `unshare(CLONE_NEWNET)` gives a
  process its own interfaces, addresses, routes, sockets and ports,
  starting with only `lo`. rtnetlink and the interface ioctls are enough
  for BusyBox's `ip link`, `ip addr` and `ip route`. They make veth pairs
  and bridges, move a link to another namespace, rename it, set it up or
  down and give it an address and a default route. Namespace 0 forwards
  what comes in on a virtual interface and masquerades it out of the
  card. Raw ICMP sockets make `ping` work. See "Network namespaces" in
  [docs/architecture.md](docs/architecture.md). The compatibility suite's
  BusyBox now has `ip`, `unshare` and `ping`. A new case pings from a
  namespace across a veth pair on a bridge, and fetches a page through
  the masquerade.
- TCP buffers are no longer 4 KiB: each connection has a receive and a
  send ring of 256 KiB whose pages are taken as data arrives and given
  back once it is read or acknowledged, so idle connections cost nothing.
  Windows are scaled (RFC 7323) when the peer offers it, and ACKs carry a
  SACK block (RFC 2018) for data held past a gap. A `read` or `write` on
  a connection moves up to the ring's size instead of 4 KiB, the loopback
  queue holds 64 frames and TCP waits for room in it instead of losing
  segments, and segments round the loopback carry no checksum. `make
  bench`'s `tcpbw` went from 14.7 to 73.8 MiB/s under TCG.
- The virtio network card sends without the kernel lock waiting for it:
  a frame goes out through one of two transmit buffers, and the buffers
  come back when the next frame is sent. Received buffers go back to the
  card with one notice per poll instead of one per frame. Four hosts each
  echoing 1 MiB through `mtcp echo` at once went from 4.2 s to 2.0 s on
  four processors and from 0.9 s to 0.6 s on one (QEMU TCG; four
  processors stay slower there because the test host has four cores, and
  QEMU's network thread shares them with the spinning processors).
- Up to eight frames can wait for an ARP answer, not one. Two connections
  opened at once before the kernel knew the gateway's MAC lost the first
  SYN+ACK, and that connection waited a second for the host to try again.
- More Intel network cards on the e1000 driver: the 82544GC and 82545EM,
  the 82574L (QEMU's `e1000e`), and the igb family: the 82576 (QEMU's
  `igb`), and the I350, I210 and I211 found on many server and desktop
  boards (untested: QEMU has none). The igb chips need their queue
  enabled on its own, advanced descriptors and the PHY told to
  negotiate. `test/shell_test.sh` gets a lease and a ping through
  `e1000e` and `igb`, and `make test` runs the TCP test on `igb` too.

- A Linux program blocked on a network socket (`accept`, `read`,
  `recvfrom`, a full TCP send buffer) sleeps instead of being restarted on
  every pass of the scheduler, so it no longer keeps a processor busy
  (`ps` shows it as `socket`). The network card wakes it by interrupt:
  MSI-X on virtio-net, the PCI interrupt line through the 8259 on the
  e1000, asked for only while someone sleeps. `make netbench` measures it.
- Loopback: a packet for 127.0.0.0/8 or the machine's own address never
  reaches the wire.
- Name lookups: DHCP asks for the name server and the kernel writes
  `/etc/resolv.conf` from it (10.0.2.3 with the static address), so musl's
  `getaddrinfo` resolves names through QEMU's resolver. `t_dns` in the
  compatibility suite checks the resolver against a name server of its own.
- `/dev/urandom` and `getrandom` come from ChaCha20 over an entropy pool
  fed by the time-stamp counter, instead of splitmix64 over a boot-time
  seed. BusyBox's `wget` fetches over HTTPS with its own TLS.
- IPv6 (`src/net6.bastio`): a link-local address and one from the prefix a
  router advertises (EUI-64 interface ids), neighbor discovery, echo
  replies, and UDP and TCP to IPv6 peers through `AF_INET6` sockets. The
  packets are translated to and from the IPv4 stack, with each peer stood
  in for by an address in 240.0.0.0/8, so the UDP and TCP code is shared.
  No extension headers, fragments, duplicate address detection or path MTU
  discovery; 16 peers at a time. `test/ipv6_test.sh` (in `make test`)
  plays a router and hosts on the card's wire, as QEMU's user network needs
  IPv6 on the host. The e1000 now takes all multicast frames.
- An SSH server: Dropbear 2024.86 (built as a static PIE by the Makefile)
  runs in BastiOS, and a host logs in with a key through QEMU's port
  forwarding: commands, piped input and exit status, 1 MiB transfers, and
  an interactive shell on a terminal, where Ctrl-C stops the foreground
  command. `test/compat/ssh.txt` (in `make compat`) does all of it from
  the host with Dropbear's client; a key that is not authorized is
  refused. `/etc/shells` lists `/bin/sh` and `/bin/ash`, and `/etc/group`
  has a `tty` group.
- DHCP leases are renewed: at T1 the kernel asks its server to extend the
  lease, at T2 any server, and when the lease runs out the address is
  dropped and the search starts again; a NAK starts it at once. Before,
  the first lease was kept forever. `test/dhcp_test.sh` (in `make test`,
  on virtio and e1000) plays a server with a 6-second lease through all
  of it.
- Servers that wait for events: `ioctl(FIONBIO)` makes any descriptor
  nonblocking (it worked on pseudo-terminals only), a nonblocking send to
  a full connection gives `EAGAIN` instead of waiting, `sendfile` writes to TCP
  connections, `FIOASYNC`, `FIOCLEX` and `FIONCLEX` work on any
  descriptor, and `prctl(PR_SET_DUMPABLE)` is accepted. With these, nginx
  installed with `apk` serves pages to the host. `poll`, `select`,
  `epoll_wait`, `pause` and `rt_sigsuspend` sleep until a frame, a clock
  tick or a signal instead of running on every scheduler pass (see
  [docs/linux.md](docs/linux.md)): a 16 MiB download from nginx went from
  0.86 MB/s to 7 MB/s. `t_server` in the compatibility suite checks the
  calls.
- Reads of `/dev/zero`, `/dev/urandom` and the other character devices
  return up to 256 KiB at once, not 4 KiB, so `dd bs=64k` gets whole
  blocks.

### Mouse

- A PS/2 mouse (`src/kbd.bastio`) for Linux programs as `/dev/input/mice`
  and `/dev/psaux` (`src/lxmouse.bastio`): 3-byte PS/2 packets, blocking
  or `O_NONBLOCK` reads, and `poll`. The hardware report says
  `mouse: PS/2` or `mouse: none found`.
- The mouse wheel: the PS/2 mouse is asked for it with the IntelliMouse
  sequence, a USB mouse gives it as the fourth byte of its boot report,
  and `/dev/input/mice` answers mouse commands as Linux's mousedev does,
  switching to ImPS/2 (4-byte packets with the wheel) when a program sends
  the sequence. The hardware report says `mouse: PS/2, with a wheel`.
- A USB mouse on the xHCI controller (`src/xhci.bastio`), through the
  same device: HID boot reports become PS/2 packets. A wireless receiver
  that is both keyboard and mouse serves as both. The controller now has a
  fifth device slot. The hardware report says
  `mouse: USB (xHCI)`.


### Real hardware

- xHCI controllers that ask for more than four scratchpad pages, as
  Intel's do (34 or so), were given up on, so a PC with only USB ports had
  no keyboard. The driver now takes up to 256 scratchpad pages from the
  pool. The controller's fixed pages are down to eleven.
- The xHCI driver powers every root port and waits for devices to
  connect (at least 300 ms, at most a second) before looking at them: a
  real controller's devices reconnect after the reset at start, which
  QEMU's do at once.
- A boot with no keyboard, no disk or no network card says so on the
  screen, not only with the boot option `diag`.
- With several xHCI controllers (recent Intel PCs have one for the USB-C
  ports ahead of the chipset's; AMD ones two or three), each is tried in
  turn until one has a keyboard, a mouse or a BastiOS stick on it. Only
  the first was looked at before.

### Sound

- An Intel HD Audio driver (`src/hda.bastio`) and `/dev/dsp`
  (`src/lxsnd.bastio`), the Open Sound System's playback device: 48 kHz
  16-bit stereo through every output the first codec has wired up
  (speakers, headphones, line out), found by walking the codec's widgets.
  Polled on the timer tick, no interrupts; no mixer, recording or other
  formats. Without a card `/dev/dsp` fails with `ENODEV`. Tested on QEMU's
  `intel-hda` with `hda-output` and its WAV backend: `mtone`'s 1 kHz square
  wave comes out whole. Real codecs (Realtek and others) are untested.

### Console and boot

- A framebuffer above 4 GiB is used. Many PCs put the graphics memory
  there (recent Intel graphics at 0x4000000000), and the console then had
  no screen at all. It needs 1 GiB pages, as the rest of the device window
  above 4 GiB does. Untested: no QEMU display puts its framebuffer there.
  The hardware report gains a `screen:` line with the framebuffer's
  address, size and whether it is in use.

- ACPI (`src/acpi.bastio`): `halt` powers a PC off through S5 instead of
  only halting it, `reboot` (new, and BusyBox's `reboot -f` and
  `poweroff -f` through Linux's reboot(2)) restarts it through the FADT's
  reset register, the keyboard controller or 0xCF9, and the power button
  powers off. The kernel parses the FADT and the DSDT's `\_S5` itself, in
  Bastio, through bastiolang's new `boot_acpi`. Checked on QEMU's i440fx
  and q35 machines without the isa-debug-exit device.
- The clock is the HPET where the ACPI HPET table puts it, else the ACPI PM
  timer (`-machine hpet=off` used to leave the clock at 0), and the
  processors' local APIC timers are calibrated against the same instead of
  the PIT, which newer PCs may gate off.
- A hardware report for trying BastiOS on a real machine: the ISO's GRUB
  menu has a second entry, "BastiOS, with a hardware report" (boot option
  `diag`), which prints the processor, the memory map, the ACPI tables and
  registers and every PCI device with its driver, a screenful at a time.
  [docs/real-hardware.md](docs/real-hardware.md) says what to check and
  what to send back. Uses bastiolang's new `boot_info`.
- The ACPI tables are found wherever the firmware put them: between 1 and
  2 GiB (where they are on a machine with 2 GiB or more below 4 GiB, which
  used to leave it on one processor and without power off) and above
  4 GiB. A reset register in memory is used too.

- The console obeys `tcsetattr`: a program can turn off canonical mode
  and echo and read each key as it is typed, with `VMIN` and `VTIME`, and
  turn off `ISIG`, `ICRNL` and output processing. Settings used to be
  accepted and ignored, so editors and REPLs got whole echoed lines.

- A console on the screen and a PS/2 keyboard when booted from the ISO:
  text drawn on the framebuffer GRUB sets up under BIOS or UEFI, keys read
  from the i8042, alongside the serial console.
- A USB keyboard on an xHCI controller's root port, for PCs without PS/2
  (no hubs).
- The ISO's GRUB loads the kernel with Multiboot 2 (QEMU's `-kernel` keeps
  Multiboot 1), so under UEFI the kernel gets the ACPI RSDP and starts
  every processor; it ran on one before.
- Under UEFI the kernel finds the ACPI tables through the RSDP GRUB
  passes, so every processor starts (it ran on one).
- `make run-iso` and `make run-uefi` open a QEMU window.

### Build

- The kernel and its programs are built with bastioc1, bastiolang's
  compiler written in Bastio, which the Makefile builds once with stage0's
  bastioc0 (`BASTIOC0`). `BASTIOC` still names another compiler.
- `make NATIVE=1` builds the kernel with bastioc1's x86-64 backend instead
  of through C (#151). It passes `make test`, but its code is about ten
  times larger than gcc's, so it is opt-in.

### Tests and documentation

- A documentation index (`docs/README.md`) and a page on developing on BastiOS (`docs/development.md`); the README's status, `docs/using.md`'s limits, `docs/security.md`'s list of missing calls and `docs/aarch64.md`'s open items now match the code. Issue and pull request templates.

- A getting-started guide (`docs/getting-started.md`): build, boot in
  QEMU, write a Bastio program, run a Linux program, boot from a USB
  stick. `make test` checks its programs and terminal sessions, and the
  README's, against the real system (`test/guide_test.py`).
- `make disk FILES="..."` puts files from the build machine on the disk
  that `make run` boots with.
- The README describes what BastiOS does now; the list of Linux system
  calls moved to `docs/linux.md`.
- The Linux compatibility suite (`make compat`, `test/compat/`): check
  programs, BusyBox applets and real programs, with a baseline of the
  checks that pass.
- A system call fuzzer (`user/sysfuzz.c`, `make fuzz`, and a short run in
  `make test`): hostile arguments through both entry points, on several
  processors. It aims at every Linux call the kernel implements (it had
  missed 99 of them, among them System V IPC, epoll, select, splice,
  timers, signalfd, links and renames, mount, clone3 and rseq), with the
  calls that could wait made not to: zero timeouts, IPC_NOWAIT, LOCK_NB,
  SPLICE_F_NONBLOCK, WNOHANG, and timers only disarmed. It opens a
  pseudo-terminal pair, a pipe, a socket pair, an eventfd, an epoll and a
  file first, so the small descriptor numbers it picks reach them; it
  sends half its ioctls to requests the kernel knows; it runs execve and
  execveat in a child, on programs that end at once and on `#!` scripts;
  and it reaps its children as it goes, so forks keep succeeding.
  `test/sysfuzz_calls.sh` (in `make test`) fails when the kernel
  dispatches a call the fuzzer does not aim at.
- The system call fuzzer damages executables too (`sysfuzz e SEED RUNS`,
  `ELFRUNS=` in `test/fuzz_test.sh`, 100 in `make test`): it breaks the
  headers of three real programs (fixed-address, PIE, and one with an
  interpreter), points the interpreter at other programs, itself or
  nothing, cuts the file short, and has a child exec each one.
- `make test` counts the trusted base (`test/trusted_base.sh`), and
  warnings in `boot/`'s C are errors.

### Changed (incompatible)

- The disk format is now `BSTDISK2`: 4 KiB blocks, 64 directory entries
  that each hold one path component and point at their parent, two levels
  of block maps per file, and copy-on-write for every write. A disk in the
  old `BSTDISK1` format is formatted afresh at boot (its files are lost).
- The device window is mapped from 2 GiB (it was 3 GiB), where UEFI
  firmware puts the framebuffer. Needs bastiolang with `boot_fb`.
- The device window now reaches 2^46: boot.S maps 4 GiB to 2^46 uncached
  with 1 GiB pages, so the xHCI and NVMe drivers take 64-bit BARs above
  4 GiB, where UEFI firmware puts them. Needs bastiolang with the wider
  window. QEMU targets pass `-cpu qemu64,+pdpe1gb`, since QEMU's default
  CPU model has no 1 GiB pages (on such a CPU the window stops at 4 GiB).
- The ISO boots through Limine (BSD-2-Clause) instead of GRUB
  (GPL-3.0), so the image ships no GPL-3.0 code. `make iso` fetches
  Limine's binary release at a pinned tag and needs only `xorriso`. The
  kernel is now linked at 16 MiB (it was 1 MiB): Limine loads a Multiboot
  kernel only into RAM the firmware marks free, and under OVMF the range the
  kernel used to take (1 to 14 MiB) is not all free. The 15 MiB below the
  kernel go unused. The menu keeps the hardware report entry and talks on
  the serial line too. `make usb` keeps the stick's GPT: it builds the ISO
  again with a GPT, adds a BIOS boot partition for Limine's BIOS stage and
  installs Limine into the image. The ISO itself has an MBR, which Limine's
  installer makes from the GPT for older firmware.

### Fixed

- The copy of the Multiboot 1 boot information keeps only the flags whose
  fields point into the copy (memory size, memory map, framebuffer), and
  the memory map walks no longer stall on an entry size that wraps 32
  bits. QEMU's loader triggers neither.
- `mprotect(PROT_NONE)` and a `PROT_NONE` file or `MAP_SHARED` mapping made
  the pages read-only, not inaccessible: the program and the system calls it
  made could still read them. Such a page is now kept mapped but hidden
  (bastiolang's `pt_set_access`), so any touch faults, a system call given
  it fails with `EFAULT`, `fork` keeps it hidden, and a hidden `MAP_SHARED`
  page stays shared when `mprotect` uncovers it.
- A kernel stack that overflowed wrote over its processor's IST1 stack,
  task state segment and saved user context, with no fault. Each kernel
  stack now has an unmapped guard page under it: an overflow becomes a
  double fault that halts the processor (`test/stackguard_test.sh`). The
  kernel stack grew from 91 to 92 KiB; the trap stack, which holds one
  trap frame, shrank from 32 to 24 KiB.
- A TCP listener could not start on a port while a connection on it that
  its program had already closed was still finishing (waiting in
  `LAST_ACK` for the peer's last ACK, say): `listen` failed with
  `EADDRINUSE` and the shell's `tcpecho` said `cannot listen`, until the
  connection gave up. Now only a listener or a connection a program still
  holds keeps the port, as on Linux with `SO_REUSEADDR`; segments still
  go to the connection whose addresses they carry first. Closing a
  listener also closes the connections that came in on it and were never
  accepted, so a new listener on the port does not hand them out.
  `test/tcp_test.sh` loses the guest's last ACK and checks that `tcpecho`
  listens again at once.
- mmap without an address, shmat without one, and brk could hand out
  pages a mapping made with MAP_FIXED (or shmat at an address, or mremap
  with MREMAP_FIXED) already held, with that mapping's bytes in them, and
  a munmap or shmdt of the new range, or a shrinking brk, then unmapped
  the old mapping too. Such a mapping in the free space between the break
  and the mmap area now starts the mmap area, and what was free above it
  becomes a hole that mmap reuses; mremap with MREMAP_FIXED also stops
  leaving its new pages listed as a hole. The system call fuzzer found it;
  `mdir` checks it.
- The clock went wrong a few seconds after boot under QEMU, which broke
  about 30 `make compat` checks (sleeps took 0 ms, poll and socket
  timeouts fired at once, `date` said 2554). There were two bugs. First,
  the TSC rate was measured from a stale first HPET read, about 12 ms
  behind the next one, so the clock ran 50 to 200 times too fast. The
  measurement now starts when the clock next moves. Second, `tsc_scale`,
  and its copy in the vDSO, dropped the `hi * mh * 2^32` term and
  returned the maximum instead. With a rate under 1 GHz, which the bad
  measurement produced, that happened once the counter had moved 2^32
  ticks, and the clock stuck at the end of time. Both now compute the
  term.

- A PC without a serial port (most of those of the last decade) took no
  keyboard input: COM1's ports read as 0xFF there, which said a byte was
  always waiting, so the console read 0xFF forever and never looked at
  the keyboard. The UART is now detected (its scratch register must keep
  what is written to it) and left alone when it is missing.

- `readv` on a disk file whose blocks were not cached yet returned the
  kernel's internal retry code to the program instead of waiting for the
  disk (stdio's `fread` failed on `/disk` files).
- A `futex` wake missed a waiter with a timeout that was on a processor at
  that moment (a timed wait is retried each time it is scheduled), so it
  slept to its deadline: glibc's condition variables waited out their
  whole timeout, which made each Z3 query take 10 s.
- Modification times were seconds since boot; they are wall-clock times
  now, as `stat` reports them.
- A file created by `open` with `O_CREAT` gets the mode asked for, less a
  umask of 022 (it was always 0644, so programs tcc linked were not
  executable by mode).
- The console takes lines of up to 4095 bytes (it cut them at 255, which
  left a long pasted command hanging in ash).
- `#DB`, NMI, `#DF` and `#MC` get their own stack (IST), since they can
  arrive while the `syscall` entry still runs on the user's stack.
- Interrupts stay off after the interrupt that ends an idle wait (a
  second one could overwrite the first's vector).

## [0.2.0] - not yet tagged

The first version: everything up to the bootable ISO.

### Kernel

- Boot from QEMU's Multiboot loader or from GRUB, as a hybrid BIOS and UEFI
  ISO (`make iso`).
- A physical page allocator whose bitmap is proved to agree with the pool,
  page tables with each page owned by exactly one slot, and user address
  spaces whose teardown returns every page (the tests compare the free page
  count at power off with the count at boot).
- User processes in ring 3 with preemptive round-robin scheduling, system
  calls, fault isolation, and a 64 MiB user window per process, mapped as
  programs ask for it. The scheduler runs on every processor, under one
  kernel lock (`Lock[Sys]`).
- Drivers written in Bastio, each reaching only the I/O ports or registers
  it claimed: the UART, the 8259 PICs, the PIT, the HPET, a legacy ATA disk,
  virtio-blk, virtio-net and Intel e1000 network cards.
- File systems: one in memory, whose data pages are owned by its files, and
  one on disk, under `disk/`, tested against power cuts during writes.
- Networking: Ethernet, ARP, IPv4, ICMP echo, UDP with sockets, a DHCP
  client and TCP. Every read of a frame is proved to stay inside it.
- A shell written in Bastio, with commands for processes, files and the
  network.

### Linux compatibility

- Static x86-64 Linux ELF executables (fixed-address and static PIE), with
  every header field treated as untrusted and every read proved in bounds.
- A Linux system call layer, written in Bastio: files, pipes, `dup`,
  `fork`/`execve`/`wait4`, `brk`/`mmap`, `poll` that waits, sleeps and
  clocks, signals (handlers, `kill`, Ctrl-C, SIGPIPE), and UDP and TCP
  sockets. The README lists every call.
- BusyBox 1.36.1 `ash`, built unmodified against musl, runs interactively,
  with `ls`, `cat`, `wc`, `head`, `rm`, `cp`, `kill` and `sleep`.

### Known limits

- Tried only in QEMU. Serial console only (COM1). One processor under UEFI.
- The README's "Status" section lists the rest.

[Unreleased]: https://github.com/yukikotani231/bastios/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/yukikotani231/bastios/releases/tag/v0.2.0
