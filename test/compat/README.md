# Linux compatibility suite

How much ordinary Linux userland does BastiOS run? `make compat` measures it.

```sh
make compat            # run on BastiOS; writes build/compat/report.md
make compat-host       # the same checks on this Linux machine
make compat-baseline   # record which checks pass now (baseline.txt)
```

Needs what `make test` needs plus `musl-gcc` (the musl-tools package),
`git` and `tclsh` (the tcl package, to generate sqlite's amalgamation) for
the real programs, and `script` (util-linux, only for `compat-host`).

## What runs

- `progs/t_*.c`: static musl C programs, one per area (files, directories,
  processes, memory, signals, time, pipes and polling, threads, sockets,
  odds and ends). Each runs a list of small checks and prints one result
  line per check (`progs/compat.h`).
- `applets.txt`: one-line shell commands using a BusyBox built with a broad
  set of applets (`busybox.config`), each with its exact expected output.
- `apps.txt`: real programs, in the same format: Lua 5.4.7, QuickJS, the
  sqlite3 3.52 shell (files, WAL, an index over 20000 rows), MicroPython
  1.24 (its unix port without ffi, btree and ssl) and BusyBox's `wget`,
  `httpd` and `nc`. The Makefile clones each at a pinned tag or commit and
  builds it against musl as a static PIE (`LUA_TAG`, `QUICKJS_REV`,
  `SQLITE_TAG`, `MPY_TAG` pick the versions).
- `tty.txt`: programs driven by keystrokes on the console: MicroPython's
  REPL (line editing, history, Ctrl-C), BusyBox `vi` and kilo
  (`KILO_REV`), each of which puts the terminal in raw mode. `run.py`
  starts the program, waits for it to draw, types the keys, then checks
  what it wrote with a shell command. On the host (`--host`) the program
  gets a pseudo-terminal.
- `HOSTURL` and `HOSTADDR` in a command name a web server `run.py` runs on
  the host, which the guest reaches through QEMU's user network; the
  `wget` and `nc` cases in `apps.txt` fetch from it.
- `dev.txt`: building C on BastiOS, in the same format. tcc and GNU make
  run from a second disk (`devdisk.sh`, booted after everything else)
  with the headers `dev_headers.c` pulls in, the C runtime objects and
  musl's `libc.so`, on a disk of its own (split out when the disk's
  directory held only 64 entries; it now holds 1023). tcc (pinned by `TCC_REV`) is configured to find
  them under `/disk/c` and builds dynamic PIEs, which the kernel runs
  with its built-in musl dynamic linker. Building make needs autoconf,
  automake, autopoint and texinfo for its bootstrap.
- `bastio.txt`: the Bastio compiler on BastiOS, from a third disk
  (`bastiodisk.sh`): bastiolang's `bastioc1` (C from `bastioc0`, built
  against musl), Z3 (`Z3_TAG`, a static PIE against glibc, as there is no
  musl C++ toolchain; its build takes a while, once) and tcc, with the
  headers the generated C includes, and the programs in `bastio/`.
  `bastioc1` proves each program with Z3, then builds it with tcc, which
  needs GNU `__auto_type` (`patches/tcc-auto-type.patch`, applied to
  `TCC_REV`). The compiler's own sources are on the disk under `/disk/s`,
  but proving them needs more memory than a process gets.

`mkdisk.py` puts the programs and that BusyBox on a disk image in BastiOS's
own disk format, so they appear as `/disk/NAME`. `run.py` boots the kernel
in QEMU with that disk, starts the built-in ash, and runs everything over the
serial line.

## What it reports

`build/compat/report.md`:

- pass, fail, hang and crash counts per area;
- **missing system calls ranked by how many checks they block**. The kernel
  prints `[pid N: Linux system call X not implemented]` for every call it
  does not know; the harness charges each note to the check that was
  running;
- failures where no call was missing (wrong results, missing `/dev`,
  `/proc` or `/etc` files, ignored flags);
- every check with its detail.

`build/compat/results.json` has the same data, and `serial.log` the whole
console transcript.

A check that hangs is interrupted (Ctrl-C, or a reboot if the shell is gone)
and its program is run again with `COMPAT_SKIP` set past it, so one hang
costs one check. Later checks that needed state from a skipped one can fail
for that reason alone; the report shows them as ordinary failures.

## Pass or fail

Most checks fail today, and that is the point: the suite is a measurement.
`make compat` fails only when a check listed in `baseline.txt` (the checks
that passed when it was last recorded) stops passing. After a change that
makes more checks pass, run `make compat-baseline` and commit the new list.

`make compat-host` runs the same programs and cases directly on Linux, to
check that the expectations themselves are right. On a normal Linux machine
everything passes, except where the machine itself lacks something (no IPv6,
a pseudo-terminal with no window size).

## Adding checks

Add an `EXPECT("name", condition)` line to a program, or a case to
`applets.txt`, run `make compat-host` to see it pass on Linux, then
`make compat`. Keep checks independent where you can: create what a check
needs outside it (as `t_ipc.c` does with its pipe), so a rerun past a hang
still has it. A check must not leave a child behind that could signal
someone later; reap every child it forks.
