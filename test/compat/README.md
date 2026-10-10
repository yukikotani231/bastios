# Linux compatibility suite

How much ordinary Linux userland does BastiOS run? `make compat` measures it.

```sh
make compat            # run on BastiOS; writes build/compat/report.md
make compat-host       # the same checks on this Linux machine
make compat-baseline   # record which checks pass now (baseline.txt)
```

Needs what `make test` needs plus `musl-gcc` (the musl-tools package) and
`script` (util-linux, only for `compat-host`).

## What runs

- `progs/t_*.c`: static musl C programs, one per area (files, directories,
  processes, memory, signals, time, pipes and polling, threads, sockets,
  odds and ends). Each runs a list of small checks and prints one result
  line per check (`progs/compat.h`).
- `applets.txt`: one-line shell commands using a BusyBox built with a broad
  set of applets (`busybox.config`), each with its exact expected output.

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
