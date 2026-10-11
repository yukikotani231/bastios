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
unprivileged only by calling `setuid` (or `setresuid` and friends) itself,
or by running a file whose set-user-ID bit names another owner. There is
no login, `su` or `passwd` yet (see "Not done" below). `/etc/passwd` and
`/etc/group` (`files/`) name root, bin, daemon and nobody, for programs
that look names up.

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
| Set-ID programs | `execve` of a file with the set-user-ID (set-group-ID) bit runs it with its owner's user (group) id. | `exec_ids`, `src/lxperm.bastio` |
| Signals | `kill`, `tkill`, `tgkill` and `rt_sigqueueinfo` need root, or the sender's real or effective user id to match the target's real or saved one; `SIGCONT` reaches any process in the sender's session. | `may_signal`, `src/lxsig.bastio` |
| Waiting | `wait4` and `waitid` reach only the caller's own children. | `src/lxproc.bastio` |
| SysV IPC | message queues, semaphores and shared memory check their mode bits; `IPC_SET` and `IPC_RMID` are for the owner, the creator or root. | `ipc_may`, `ipc_owner`, `src/lxipc.bastio` |
| The system | `reboot`, `mount`, `umount2`, `sethostname`, `setdomainname`, `settimeofday`, `clock_settime`, `adjtimex` and `clock_adjtime` (when they change something) are root's; device nodes cannot be made at all. | `src/linux.bastio`, `src/lxmount.bastio`, `src/lxsys.bastio`, `src/lxevent.bastio` |
| Ports | binding a UDP or TCP port below 1024 is root's (`EACCES`). | `may_bind`, `src/lxsock.bastio` |
| Native calls | Any program can raise `int 0x80`, the native ABI (`src/syscall.bastio`), a Linux program included. Its file, network, power and reboot calls check no permissions, so a process that is not root may make only the calls that touch nothing of anyone else's: console I/O, starting and waiting for its own children (which keep its ids), yielding, its pid, `SYS_INFO`'s counters, the process list, and `SYS_KILL` of a process `may_signal` allows. Everything else gives `NO_RESULT`. | `native_allowed`, `src/syscall.bastio`; `spawn`, `src/proc.bastio` |

The proofs state some of these as properties of the functions that decide
them: `native_allowed` proves that a process that is not root gets no
call past `SYS_KILL`, and `may_bind` that a port below 1024 is bound only
by root. The rest are checked by tests, not proved: `test/compat/progs/t_cred.c`,
`t_perm.c`, `t_sysv.c` and `t_isol.c` drop to nobody and try each rule.

## Not done

Known gaps, most serious first.

1. **`mprotect` can make a read-only shared file mapping writable.** `mmap`
   refuses a writable `MAP_SHARED` mapping of a file opened read-only, but
   a later `mprotect(PROT_WRITE)` is not checked, and such a mapping maps
   the page cache's own pages (`src/lxmem.bastio`), so the writes reach
   every other reader of the file (they are not written back to the disk:
   `map_flush` writes only for a writable descriptor).
2. **`AF_UNIX` sockets have no permissions.** Their names live in a table
   of their own (`src/lxunix.bastio`), not in the file system, so any user
   can connect to any name, or take one first.
3. **No login.** There is no `login`, `su`, `passwd`, `/etc/shadow` or
   password hashing; BusyBox is built without those applets. A system
   with several users needs them, and a way to start the console as
   someone other than root.
4. **Native processes.** The native shell and the programs it starts run
   as root, and `SYS_SPAWN` starts any built-in program; that is safe only
   because a process that is not root cannot gain anything by it (the
   child keeps its ids). Native sockets are global indices with no owner,
   so root processes can reach one another's.
5. **Loop mounts are not `nosuid`.** A `bstdisk3` image is mounted with
   `LO_NODEV` only. Mounting is root's, so this matters only when root
   mounts an image someone else made.
6. Smaller ones: `prlimit64` and `setpriority` ignore their target pid
   (neither changes anything); `setpgid` does not check that the child is
   in the caller's session or has not run `execve`; SysV IPC checks only a
   process's effective group, not its supplementary groups; `SHM_LOCK`
   needs only ownership; `/proc/PID/exe` of anyone's process can be read;
   `/dev/console` is open to all.
7. Not implemented, so not a hole: `ptrace`, `process_vm_readv`,
   `/proc/PID/mem`, `environ` and `maps`, `chroot`, `iopl` and `ioperm`,
   `syslog`, `swapon` and `pivot_root` all fail (`ENOSYS` or missing).
