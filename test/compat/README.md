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
  1.24 (its unix port without ffi, btree and ssl), git 2.47 (with zlib and
  nothing else; it finds its subcommands in `/disk`), Perl 5.40 (its XS
  modules linked in; the pure-Perl modules the cases use, and what they
  load, go on the disk as `/disk/perllib`) and BusyBox's `wget`, `httpd`
  and `nc`. The Makefile clones each at a pinned tag or commit and builds
  it against musl as a static PIE (`LUA_TAG`, `QUICKJS_REV`, `SQLITE_TAG`,
  `MPY_TAG`, `GIT_TAG`, `PERL_TAG` pick the versions).
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
  `TCC_REV`; the patch modifies tcc, so it is under tcc's license,
  LGPL-2.1-or-later, not this repository's). The compiler's own sources are on the disk under `/disk/s`,
  but proving them needs more memory than a process gets.
- `alpine.txt`: Alpine Linux 3.24, from a fourth disk (`alpinedisk.py`).
  The first case unpacks its minirootfs and enters it with `chroot`. Then
  `apk` installs packages from a repository on the same disk, and they run:
  git, curl, less, openssh-keygen, bash, make, jq, rsync, openssl, file
  and xmllint (`ALPINE_DISK_PKGS`, with what they depend on). These cases
  need no network.
  - One case installs tig over HTTP from the host (`ALPINEURL`). The host
    serves every package of the images, including gcc, python3 and vim,
    which do not fit on the disk yet.
  - The `bt-*` packages, made by `alpinedisk.py`, test apk itself:
    dependency resolution, a missing dependency, `apk upgrade` with a
    version constraint (version 2 is in a second repository, `/repo2`), the
    install, upgrade and deinstall scripts, a trigger, and `apk del`
    removing dependencies no longer needed. The expected output is what the
    same commands print in a chroot on Linux.
  - The packages are rebuilt from Docker Hub images that apk installed them
    into (`ALPINE_BASE`, `ALPINE_PKGS`, pinned by digest), with the install
    scripts and triggers apk kept for them. The binaries are Alpine's. The
    packages and the index are unsigned, so apk runs with
    `--allow-untrusted`.
  - The cases change the disk, so `run.py` runs them on a copy.

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

## LTP system call tests

```sh
make compat-ltp            # writes build/compat/ltp/report.md
make compat-ltp-baseline   # record which tests pass now (ltp_baseline.txt)
```

The [Linux Test Project](https://github.com/linux-test-project/ltp)'s system
call tests are a much larger and more exacting set than the checks above:
about 1,500 tests, each written against the man pages and the kernel's own
behaviour, error cases included. LTP is GPL-2.0-or-later and is not part of
this repository: `make compat-ltp` clones it at a pinned tag (`LTP_TAG`) into
`build/ltp` and builds `testcases/kernel/syscalls` against musl as static
PIEs, as the BusyBox build does. The three files LTP's own Alpine (musl) CI
removes are removed here too.

`ltp.py` takes the tests from LTP's own list (`runtest/syscalls`), puts their
programs on disk images (the disk holds 64 files, so the tests run in batches
of up to 63 programs, each batch in its own QEMU, several at once with
`--jobs`), and runs each from ash in `/disk` with `TMPDIR=/tmp`. A test's
helper programs (`execve_child` and the like) go on the same disk, since LTP
copies them from the directory a test starts in. Tests that ask about the
kernel's configuration read `ltp_kconfig`, which lists only what BastiOS
really has; a test needing anything else reports `TCONF`. A test's exit status gives its result:
pass, fail (`TFAIL`), broken (`TBROK`: it could not set itself up),
unsupported (`TCONF`: it needs something this system does not claim to
have, such as a kernel config or a block device), hang (interrupted after
`--timeout` seconds) or crash. `--only REGEX` runs a subset by tag.

The report ranks missing system calls by how many tests they block, as
`run.py` does, and lists every test that did not pass with its first
`TFAIL`/`TBROK` line. `make compat-ltp` fails only if a test listed in
`ltp_baseline.txt` stops passing. `ltp_skip.txt` lists tests that are not
run, with the reason.

There is no `--host` mode for these: many of the tests change the machine
they run on (the clock, mounts, swap, resource limits), so they are not run
on the build machine. LTP's own CI runs them on Linux.
