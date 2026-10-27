# Users, permissions and isolation

This page says what BastiOS enforces between users and between processes
today, what it does not, and what is planned. It is an audit of the code,
not a promise: each line names the file that does the work. Keep it true
when you change any of them.

## The model

BastiOS has Linux's model of users, without capabilities: a process has
real, effective, saved and file-system user and group ids, and up to
`NGROUPS_KEPT` (16) supplementary groups (`src/lxcred.bastio`, row words
`F_UID` .. `F_GROUPS` in `src/proc.bastio`). Effective user 0 ("root") is
the only privilege; where Linux asks for a capability, BastiOS asks for
root.

Everything starts as root. The kernel's first program, the native shell,
runs with every id 0, and so does anything it starts. A process becomes
unprivileged by calling `setuid` (or `setresuid` and friends) itself, by
running a file whose set-user-ID bit names another owner, or through
BusyBox's `login` and `su`.

## Users and passwords

`/etc/passwd`, `/etc/group` and `/etc/shadow` (`files/`, built into the
kernel) name root, bin, daemon and nobody, and the group disk. `/etc/shadow`
is `0600`; root has no password until `passwd` sets one, and the others
cannot log in. `/root` is root's home (`0700`), `/home` holds everyone
else's.

With a disk, `/etc`, `/root` and `/home` are symbolic links to
`/disk/etc`, `/disk/root` and `/disk/home` (`install_files`,
`src/main.bastio`), so users, passwords and homes survive a reboot. The
first boot that finds no `/disk/etc` makes it and copies the built-in
files there; later boots keep what is on the disk (a built-in file that
is missing there is copied again). The disk file system keeps modes and
owner ids up to 2^24. Without a disk the three are directories in
memory, filled afresh at each boot.

BusyBox's `adduser`, `addgroup`, `deluser`, `delgroup`, `passwd`,
`chpasswd`, `su`, `login`, `id` and `whoami` are built in (`/bin/NAME`).
New users get ids from 1000, and passwords are hashed with SHA-512 by
BusyBox's own `crypt`. From the native shell, `ash` then `login` (or `su
NAME`) starts a session as another user.

With the boot option `login` (the third boot menu entry, or `-append
login` under QEMU's own loader), the console runs BusyBox's `login`
instead of the native shell, and the kernel starts a new one whenever the
last has ended, as init restarts a getty (`relogin`, `src/main.bastio`):
logging out brings the prompt back. As BusyBox's `login` does, an
unanswered prompt gives up after 60 seconds and a new one appears.

`/bin/su` and `/bin/passwd` are set-user-ID root, as on a system where
BusyBox is installed set-user-ID: running either starts BusyBox with
effective user 0 (`exec_ids`, `src/lxperm.bastio`; `bin_suid`,
`src/lxproc.bastio`), and BusyBox, built with `CONFIG_FEATURE_SUID`, drops
root at once unless the applet its `argv[0]` names needs it. So `/bin/su`
run under another name gets nothing, and every other `/bin/NAME` runs with
the caller's ids. That puts BusyBox's `su`, `passwd` and its start-up code
in the trusted base for this.

Memory is isolated by construction: each process has its own address
space (`src/space.bastio`), and the proofs show the kernel reads and
writes a process's memory only through that process's own page tables
(`src/usermem.bastio`). Pages are shared only where a process asked for
it: `MAP_SHARED` memory with its own children, a `MAP_SHARED` file
mapping with everyone who maps the same file, and SysV shared memory with
whoever its mode admits.

## What is enforced

| Area | Rule | Where |
|---|---|---|
| Changing ids | `setuid`, `setgid`, `setreuid`, `setregid`, `setresuid`, `setresgid`, `setfsuid`, `setfsgid`: root sets anything, anyone else moves only between the ids it holds. `setgroups` is root's. | `src/lxcred.bastio` |
| Files | open, `execve`, `truncate` and `access` check the mode bits; every directory on a path needs search permission; making, removing and renaming names need write and search permission on the directory; a sticky directory keeps others' names; `chmod` and `utime` with times are the owner's; `chown` is root's, or the owner's to one of its groups, and clears the set-ID bits; new files belong to their maker and take the umask; a set-group-ID directory passes its group on. Root passes every check, but runs only files with an execute bit. | `src/lxperm.bastio` |
| Set-ID programs | `execve` of a file with the set-user-ID (set-group-ID) bit runs it with its owner's user (group) id, except in a loop-mounted image, which is `nosuid`: its maker chose its owners and modes. | `exec_ids`, `src/lxperm.bastio` |
| Signals | `kill`, `tkill`, `tgkill` and `rt_sigqueueinfo` need root, or the sender's real or effective user id to match the target's real or saved one; `SIGCONT` reaches any process in the sender's session. | `may_signal`, `src/lxsig.bastio` |
| Waiting | `wait4` and `waitid` reach only the caller's own children. | `src/lxproc.bastio` |
| Other processes | `prlimit64` and `setpriority` change another user's process only as root; `setpgid` moves only a child in the caller's session that has not run `execve`; `readlink` of `/proc/PID/exe` and `/proc/PID/fd/N` needs root or the same real, effective and saved user and group ids (`EACCES`). | `lx_prlimit`, `src/lxrlimit.bastio`; `lx_setpriority`, `src/lxprio.bastio`; `lx_setpgid`, `src/lxmisc.bastio`; `proc_peek`, `src/lxprocfs.bastio` |
| SysV IPC | message queues, semaphores and shared memory check their mode bits, the group bits for a process whose file-system group or a supplementary group is the set's; `IPC_SET` and `IPC_RMID` are for the owner, the creator or root; `SHM_LOCK` is root's, or the owner's under a nonzero `RLIMIT_MEMLOCK`. | `ipc_may`, `ipc_owner`, `src/lxipc.bastio` |
| The system | `reboot`, `mount`, `umount2`, `sethostname`, `setdomainname`, `settimeofday`, `clock_settime`, `adjtimex` and `clock_adjtime` (when they change something) are root's; device nodes cannot be made at all. | `src/linux.bastio`, `src/lxmount.bastio`, `src/lxsys.bastio`, `src/lxevent.bastio` |
| Shared file mappings | `mmap` refuses a writable `MAP_SHARED` mapping of a file opened without write permission, and `mprotect` refuses to make one writable later (`EACCES`). Only a file opened for writing is mapped through the page cache's own pages; from a read-only descriptor, a shared mapping is a copy, so even a mapping `mprotect` cannot see (past the `MAP_N` recorded per process) changes no one else's view of the file. The cost: such a mapping does not see later writes to the file. | `lx_mmap`, `src/linux.bastio`; `map_no_write`, `src/lxmem.bastio` |
| Unix sockets | `bind` to a path makes a socket file there, with the checks for making any file (write and search permission on the directory) and the umask; `connect` needs write permission on that file, so its mode, owner and group say who may connect. `connect` finds the socket by the file itself, so a renamed socket file or another hard link to it still leads there. | `ux_mkfile`, `ux_may_connect`, `src/lxunix.bastio` |
| Devices | `/dev/loop0`..`7` and `/dev/loop-control` are `rw-rw----` for root and group disk (6), and open checks it: a loop device reads and writes whatever file it is bound to. `/dev/console` is `rw--w----` for root, and open checks it. `/dev/null`, `zero`, `random`, `urandom` and `tty` are open to everyone; there are no raw disk or memory devices. | `lx_open`, `src/lxfd.bastio`; `dev_meta`, `src/lxdev.bastio` |
| Address space layout | Each `execve` draws 128 bits from the `/dev/urandom` generator. They become the 16 bytes `AT_RANDOM` points at (what libc takes its stack-protector canary and pointer guard from), and they move the layout, as Linux's `kernel.randomize_va_space` does. Level 1: the stack (up to 512 KiB down, in steps of 16 bytes), the top of the mmap area and the dynamic linker (up to 64 GiB down), and a position-independent program (up to 16 GiB up). Level 2, the default: the program break too (up to 32 MiB above the image). Level 0, or `personality(ADDR_NO_RANDOMIZE)` (kept across `execve`), turns it off. A fixed-address program (`ET_EXEC`) cannot move. | `layout_rnd`, `src/lxdev.bastio`; `load_image`, `src/proc.bastio`; `init_stack`, `src/elf.bastio` |
| Other users' layout | `/proc/PID/maps`, `auxv`, `mem` and `io` of a process open (`EACCES`) only for a process that could ptrace it: root, or the same real, effective, saved and file-system ids. They would give the layout away. | `proc_secret`, `src/lxprocfs.bastio`; `lx_open_tail`, `src/lxfd.bastio` |
| Ports | binding a UDP or TCP port below 1024 is root's (`EACCES`). | `may_bind`, `src/lxsock.bastio` |
| Native calls | Any program can raise `int 0x80`, the native ABI (`src/syscall.bastio`), a Linux program included. Its file, network, power and reboot calls check no permissions, so a process that is not root may make only the calls that touch nothing of anyone else's: console I/O, starting and waiting for its own children (which keep its ids), yielding, its pid, `SYS_INFO`'s counters, the process list, and `SYS_KILL` of a process `may_signal` allows. Everything else gives `NO_RESULT`. | `native_allowed`, `src/syscall.bastio`; `spawn`, `src/proc.bastio` |

The proofs state some of these as properties of the functions that decide
them: `native_allowed` proves that a process that is not root gets no
call past `SYS_KILL`, and `may_bind` that a port below 1024 is bound only
by root. The rest are checked by tests, not proved: `test/compat/progs/t_cred.c`,
`t_perm.c`, `t_sysv.c` and `t_isol.c` drop to nobody and try each rule.

## Not done

Known gaps, most serious first.

1. **Login is a boot option.** By default the console starts in the
   native shell as root (the tests drive it). With the boot option `login`
   it starts BusyBox's `login` instead (see "Users and passwords"), as
   the third boot menu entry does.
2. **Native processes.** The native shell and the programs it starts run
   as root, and `SYS_SPAWN` starts any built-in program; that is safe only
   because a process that is not root cannot gain anything by it (the
   child keeps its ids). Native sockets are global indices with no owner,
   so root processes can reach one another's.
3. Smaller ones: `/dev/tty` in a process with no pty for a terminal
   opens the console, as the console has no session of its own yet (on
   Linux it is `ENXIO` without a controlling terminal); names of unix
   sockets in the abstract namespace (a leading 0 byte) have no
   permissions, as on Linux.
4. Not implemented, so not a hole: `/proc/PID/environ`, `iopl` and
   `ioperm`, and `swapon` all fail (`ENOSYS` or missing); `syslog` answers but the kernel keeps no log. (`chroot` and
   `pivot_root` exist: `chroot` needs root, `pivot_root` works on a mount
   namespace's own mounts.) `process_vm_readv`,
   `process_vm_writev`, `pidfd_getfd`, `kcmp` and `ptrace` (`PTRACE_ATTACH`
   and `PTRACE_SEIZE`; `PTRACE_TRACEME` lets a process's parent trace it,
   as on Linux) reach another process only
   with the same user and group ids (real, effective, saved and
   file-system), or as root
   (`src/lxpidfd.bastio`, `src/lxptrace.bastio`), as Linux's
   `PTRACE_MODE_ATTACH_REALCREDS` asks; there is no Yama ptrace scope on
   top of that, and no `CAP_SYS_PTRACE` or dumpable flag. A tracer cannot
   give a tracee a kernel address, a segment or the trap flag: the entry to
   user mode (`boot/boot.S`) takes only the general registers, rip, rsp,
   the arithmetic flags, the trap flag (which a user program may set itself)
   and the fs base from the saved context, and checks rip, rsp and the fs
   base are user addresses.

## Memory protections: where they stand

What the page tables enforce for a user process, and what they do not.

- **Layout.** Randomized as the "Address space layout" row says, with
  fewer bits than Linux has: the stack moves by 15 bits, the mmap area by
  24 bits of pages (the window is 8 TiB, Linux's is 128 TiB). The vDSO
  page still lies at a fixed place just under the stack, and so does the
  first part of the window (a non-PIE program).
- **No execute (NX) and W^X, on x86-64.** `EFER.NXE` is set on every
  processor (`boot/boot.S`, `boot/ap.S`). Whether a page can run is decided
  when its page-table entry is made, in bastiolang's prelude: a page mapped
  writable (`pt_put_frame`, `pt_put_shared` with `w`) gets the no-execute
  bit, a read-only one does not, and `pt_set_access` leaves the bit alone.
  So the stack, the heap, anonymous mappings, a program's data and bss, and
  every page fork shares copy-on-write cannot run code. `PROT_EXEC` is
  honoured: `mmap` and `mprotect` map the page again with the bit its new
  protection wants (`l2_protect`, `src/lxmem.bastio`), so a JIT that writes
  code and then `mprotect`s it executable works, and `PROT_READ|PROT_WRITE`
  after that stops the code again. The pages of an executable segment are
  made executable after the image is copied in (`exec_pages`,
  `src/elf.bastio`). What is not enforced yet:
  - A page that is both writable and executable (a program asking for
    `PROT_WRITE|PROT_EXEC`, the flat images of the built-in programs) is
    marked in the page bitmap (`XPAGE`, `src/pages.bastio`), because the
    entry cannot be asked; fork (copy-on-write or copying) and the copy a
    write makes keep it. `mremap` of such a page drops the execute right.
  - A program's text is not made read-only (only the pages shared from the
    page cache are), and read-only data in shared pages can run code.
  - The AArch64 port and the native backends do not set the bit.
- **SMEP and SMAP: not needed.** The kernel runs on its own page table
  and loads it on every entry from user mode (`trap_alias`); the user
  pages are not in it. A kernel jump to user code, or a kernel read of a
  user address the kernel was not asked to read, finds nothing mapped.
  The kernel reads and writes a process's memory only through that
  process's own tables (`src/usermem.bastio`, proved).
- **The kernel's own pages.** The kernel image and RAM are mapped by
  1 GiB pages, read-write and executable. A write through a kernel bug
  could change kernel code. The proofs say the kernel makes no such write;
  this would be a second line.

## Randomness

`/dev/urandom`, `/dev/random` and `getrandom` come from one ChaCha20 generator
(`src/lxdev.bastio`) with fast key erasure. Its pool takes the time-stamp
counter on every scheduler pass, the clocks at boot, a virtio-rng's 64 bytes
at boot if the machine has one, and, if the processor has RDSEED or RDRAND, 64
bytes at boot and one word every 4096 scheduler passes (`rng_hw_boot`,
`rng_stir`; `hw_random` in `boot/smp.c`). Without one of those two
hardware sources (a QEMU CPU model without them, an old processor, a virtual
machine without virtio-rng) the pool's early state rests on timing jitter and
the clock, which is an estimate of entropy and not a measured one. `test/rng_test.sh` boots with and without the instructions.
