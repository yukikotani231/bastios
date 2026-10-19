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
  and `nc`, and ripgrep 14.1 (built by cargo from crates.io, `RIPGREP_VER`,
  as a static musl binary: it searches on several threads). The Makefile clones each at a pinned tag or commit and builds
  it against musl as a static PIE (`LUA_TAG`, `QUICKJS_REV`, `SQLITE_TAG`,
  `MPY_TAG`, `GIT_TAG`, `PERL_TAG` pick the versions).
- `tty.txt`: programs driven by keystrokes on the console: MicroPython's
  REPL (line editing, history, Ctrl-C), BusyBox `vi` and kilo
  (`KILO_REV`), each of which puts the terminal in raw mode. `run.py`
  starts the program, waits for it to draw, types the keys, then checks
  what it wrote with a shell command. On the host (`--host`) the program
  gets a pseudo-terminal.
- On AArch64 (`make ARCH=aarch64 compat`) the `apps` group runs only Lua,
  QuickJS and the sqlite3 shell, built by `arch/aarch64/Makefile` as static
  glibc programs (`AARCH64_APPS` in `run.py`).
- `HOSTURL` and `HOSTADDR` in a command name a web server `run.py` runs on
  the host, which the guest reaches through QEMU's user network; the
  `wget` and `nc` cases in `apps.txt` fetch from it.
- `dev.txt`: building C on BastiOS, in the same format. tcc and GNU make
  run from a second disk (`devdisk.sh`, booted after everything else)
  with the headers `dev_headers.c` pulls in, the C runtime objects and
  musl's `libc.so`, on a disk of its own. tcc (pinned by `TCC_REV`) is configured to find
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
  bastiolang's developer tools (`tools/bastiofmt`, `tools/bastio-lsp`, built
  here as static PIEs against musl) are on the disk too: the cases format a
  line and the compiler's own parser, and drive the language server over
  its standard input and output with the requests an editor sends (hover,
  go to definition, formatting, a diagnostic from `bastioc1`).
- `alpine.txt`: Alpine Linux 3.24, from a fourth disk (`alpinedisk.py`).
  The first case unpacks its minirootfs and enters it with `chroot`. Then
  `apk` installs packages from a repository on the same disk, and they run:
  git, curl, less, openssh-keygen, bash, make, jq, rsync, openssl, file
  and xmllint (`ALPINE_DISK_PKGS`, with what they depend on). These cases
  need no network.
  - Other cases install packages over HTTP from the host (`ALPINEURL`):
    tig, neovim, gcc and musl-dev, ripgrep, fzf, tmux, cmake, python3. The
    host serves every package of the images; these are the ones the cases
    ask for.
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
  - `make ARCH=aarch64 compat` runs this group, `debian.txt` and `node.txt` on
    the AArch64 kernel from the images' arm64 layers (`COMPAT_ARCH=aarch64`;
    the pins are in `images.mk`). The Bastio toolchain and Dropbear are built
    for x86-64 only, so the checks that use them do not pass there. Two
    packages come from elsewhere for lack of an arm64 image: gdb is the
    arm64 layers of the same Alpaquita image (`arch/aarch64/Makefile`), and
    cmake is made from the `cmake` wheel on PyPI (`--cmake-wheel`, pinned by
    SHA-256), which bundles its C++ runtime.
- `debian.txt`: Debian 13, from a fifth disk (`debiandisk.py`). The first
  case unpacks Debian's root file system (the `debian:stable-slim` image,
  `DEBIAN_IMAGE`, without `/usr/share`, dpkg's file lists, gconv and Perl's
  modules, so that it fits in the disk's 1024 entries) and enters it with
  `chroot`. Its programs are linked against glibc and start in glibc's
  dynamic linker: bash, coreutils, sed, grep, awk, find, GNU tar and gzip,
  perl and dpkg-query run. The expected output is what the same commands
  print in a chroot on Linux. The cases change the disk, so `run.py` runs
  them on a copy.
- `node.txt`: Node.js, from a sixth disk (`nodedisk.py`): the `node` binary of
  the official `node:22-alpine` image (`NODE_IMAGE`) and the libraries it
  needs, entered with `chroot`. V8 reserves a large range of address space,
  starts threads and writes machine code at run time, so this lane boots the
  guest with 1 GiB of memory, and the cases change the disk, so `run.py` runs
  them on a copy.
- `python.txt`: Python development, from a seventh disk (`pythondisk.py`): the
  root of the official `python:3.14-alpine` image (`PYTHON_IMAGE`: CPython,
  pip and the headers in `/usr/local`), entered with `chroot`, with gcc
  installed by apk from the Alpine repository over HTTP. The cases make a
  venv, `pip install` packages from the host's web server (pinned by SHA-256
  in `pythondeps.txt`, served at `HOSTURL/wheels/`; pip finds them through
  `/etc/pip.conf`), build a C extension from source (MarkupSafe), run the
  whole test suite of a real project (Jinja2's 784 tests, spread over two xdist workers), load binary musl
  wheels (numpy, cryptography, lxml, Pillow, PyYAML, greenlet, psutil, orjson,
  cffi) and run `pyprogs/stdlib.py`, which exercises the standard library on
  top of the system calls: fork, threads, signals, sockets, mmap, pty,
  sqlite3, ctypes, multiprocessing (fork, spawn and forkserver), asyncio and
  TLS. `pyprogs/` is served at `HOSTURL/pyprogs/`, and the same script runs on
  Linux (`python3 pyprogs/stdlib.py`). x86-64 only; the guest has 1 GiB.
- `go.txt`: the Go toolchain of the official `golang` image (`GO_IMAGE`), from
  a seventh disk (`godisk.py`), entered with `chroot`: Go programs are
  static, so the root holds only `/usr/local/go`. x86-64 only.
- `ssh.txt`: Dropbear in the guest, with the host's Dropbear client logging
  in over QEMU's port forward. `alpine_tty.txt`: programs driven by
  keystrokes inside the Alpine root (tmux, Neovim, editing and building C).
- The container cases (`runc` and `crun`) are in `alpine.txt`; the
  per-area summary of what they check is in
  [docs/linux.md](../../docs/linux.md#container-runtimes).

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

The suite is a measurement: it counts what passes and ranks what is missing.
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

`make compat-ltp` also runs LTP's file system stress tests (`runtest/fs`: growfiles,
ftest, stream), the memory tests (`runtest/mm`: mmap stress, shared memory), its pseudo-terminal, namespace (container) and
CVE regression tests, and the Open POSIX Test Suite's conformance tests
(`--suite posix`; about 1,600 programs named after an interface and an
assertion, such as `posix-sigaction_1-1`). Those exit 0 for a pass, 1 for a
failure, 2 when they could not set themselves up, 4 or 5 for something not
supported here.

The report ranks missing system calls by how many tests they block, as
`run.py` does, and lists every test that did not pass with its first
`TFAIL`/`TBROK` line. `make compat-ltp` fails only if a test listed in
`ltp_baseline.txt` stops passing. `ltp_skip.txt` lists tests that are not
run, with the reason.

There is no `--host` mode for these: many of the tests change the machine
they run on (the clock, mounts, swap, resource limits), so they are not run
on the build machine. LTP's own CI runs them on Linux.
