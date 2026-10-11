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
| SysV IPC | message queues, semaphores and shared memory check their mode bits; `IPC_SET` and `IPC_RMID` are for the owner, the creator or root. | `ipc_may`, `ipc_owner`, `src/lxipc.bastio` |
| The system | `reboot`, `mount`, `umount2`, `sethostname`, `setdomainname`, `settimeofday`, `clock_settime`, `adjtimex` and `clock_adjtime` (when they change something) are root's; device nodes cannot be made at all. | `src/linux.bastio`, `src/lxmount.bastio`, `src/lxsys.bastio`, `src/lxevent.bastio` |
| Shared file mappings | `mmap` refuses a writable `MAP_SHARED` mapping of a file opened without write permission, and `mprotect` refuses to make one writable later (`EACCES`). Only a file opened for writing is mapped through the page cache's own pages; from a read-only descriptor, a shared mapping is a copy, so even a mapping `mprotect` cannot see (past the `MAP_N` recorded per process) changes no one else's view of the file. The cost: such a mapping does not see later writes to the file. | `lx_mmap`, `src/linux.bastio`; `map_ro_shared`, `src/lxmem.bastio` |
| Unix sockets | `bind` to a path makes a socket file there, with the checks for making any file (write and search permission on the directory) and the umask; `connect` needs write permission on that file, so its mode, owner and group say who may connect. `connect` finds the socket by the file itself, so a renamed socket file or another hard link to it still leads there. | `ux_mkfile`, `ux_may_connect`, `src/lxunix.bastio` |
| Devices | `/dev/loop0`..`7` and `/dev/loop-control` are `rw-rw----` for root and group disk (6), and open checks it: a loop device reads and writes whatever file it is bound to. `/dev/null`, `zero`, `random`, `urandom` and `tty` are open to everyone; there are no raw disk or memory devices. | `lx_open`, `src/lxfd.bastio`; `dev_meta`, `src/lxdev.bastio` |
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
3. Smaller ones: `prlimit64` and `setpriority` ignore their target pid
   (neither changes anything); `setpgid` does not check that the child is
   in the caller's session or has not run `execve`; SysV IPC checks only a
   process's effective group, not its supplementary groups; `SHM_LOCK`
   needs only ownership; `/proc/PID/exe` of anyone's process can be read;
   `/dev/console` is open to all; names of unix sockets in the abstract
   namespace (a leading 0 byte) have no permissions, as on Linux.
4. Not implemented, so not a hole: `ptrace`, `/proc/PID/mem`, `environ`
   and `maps`, `chroot`, `iopl` and `ioperm`, `syslog`, `swapon` and
   `pivot_root` all fail (`ENOSYS` or missing). `process_vm_readv`,
   `process_vm_writev`, `pidfd_getfd` and `kcmp` reach another process only
   with the same user and group ids (real, effective, saved and
   file-system), or as root
   (`src/lxpidfd.bastio`), as Linux's `PTRACE_MODE_ATTACH_REALCREDS` asks;
   there is no Yama ptrace scope on top of that.
