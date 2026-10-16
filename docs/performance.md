# Performance

How fast BastiOS is, measured against Linux on the same emulated machine.
`make bench` reproduces the table below. Numbers in this file are from one
run and only compare within that run; rerun before relying on one.

## How it is measured

`bench/osbench.c` is a static musl program with one micro-benchmark per
test. The same binary runs on both systems. `bench/run.py` boots each
system in QEMU with the same CPU model, memory, processor count, a
virtio-blk disk and a virtio-net card. It uses KVM when `/dev/kvm` can be
opened and TCG otherwise. Each test runs in a fresh process started from
ash: BastiOS's built-in ash, or BusyBox ash on Linux. The program prints a
line before and after the timed part. The host stamps both lines as they
arrive on the serial port, so the guest clock cannot skew the result. The
guest's own figure is kept in `build/bench/results.json` as a check. Each
result is the median of the runs.

Linux is Ubuntu's generic 6.8 kernel, booted with `mitigations=off`
because BastiOS has no page-table isolation either. Its initramfs holds
static BusyBox and osbench, `/tmp` is tmpfs and `/disk` is ext4 on the
virtio disk. On BastiOS, `/tmp` is tmpfs and `/disk` is the disk file
system. Without `/dev/kvm`, `bench/run.py` downloads the kernel with
`apt-get download`, or takes `--linux VMLINUZ`.

| Test | What one operation is |
|---|---|
| cpu | one step of a 64-bit LCG, no system calls: the emulated CPU's speed |
| memcpy | copying 1 MiB between two 256 KiB buffers that are already mapped |
| syscall | `getppid` |
| clock | `clock_gettime(CLOCK_MONOTONIC)` as musl calls it |
| open, stat | `open`+`close`, or `stat`, of an existing file |
| create | create a file, write 1 KiB, close it and unlink it |
| filewrite, fileread | 8 MiB in 64 KiB calls, to a new file and then read back |
| fork | `fork`, the child exits, the parent waits |
| forkbig | the same with 8 MiB of touched heap |
| exec, spawn | `fork`+`execve`, or `posix_spawn`, of a program that exits at once, and a wait |
| fault | map 32 MiB of anonymous memory, touch every page, unmap it; per page |
| pipelat | one byte to another process and back through two pipes |
| pipebw | 64 KiB writes through a pipe to a reader |
| tcplat, tcpbw | the same over TCP on 127.0.0.1 |

`make bench-workloads` adds whole-program workloads and `make bench-net` a TCP comparison over a network card (see Results).

`bench/prof.py -- COMMAND` shows where the time goes for one command on
BastiOS. It samples QEMU with Linux `perf` and charges each sample to a
kernel function through QEMU's `-perfmap`, or to QEMU itself.

## Results

BastiOS main (ed8a934). QEMU 8.2 TCG, `-smp 2`, `-m 512M`, median of 3
runs, on a 4-processor cloud container with no KVM. The ratio column is
BastiOS's time divided by Linux's for the same work, so a figure above 1
means BastiOS is slower. Compare the ratios, not the absolute figures:
TCG time depends on the host.

| Test | Unit | BastiOS | Linux | BastiOS / Linux |
|---|---|---:|---:|---:|
| cpu | ns/iter | 1.09 | 1.27 | 0.86x |
| memcpy | us/MiB | 54.5 | 61.3 | 0.89x |
| syscall | ns | 16009 | 459 | 34.90x |
| clock | ns | 104 | 999 | 0.10x |
| open@tmp | us | 46.6 | 7.52 | 6.19x |
| open@disk | us | 65.1 | 7.73 | 8.42x |
| stat@tmp | us | 28.6 | 4.83 | 5.91x |
| stat@disk | us | 31.7 | 4.29 | 7.38x |
| create@tmp | us | 108 | 151 | 0.72x |
| create@disk | us | 1326 | 444 | 2.99x |
| filemap@tmp | ns/page | 3800 | 1704 | 2.23x |
| filemap@disk | ns/page | 4246 | 1984 | 2.14x |
| filewrite@tmp | MiB/s | 486.6 | 352.9 | 0.73x |
| filewrite@disk | MiB/s | 76.2 | 202.4 | 2.66x |
| fileread@tmp | MiB/s | 961.3 | 2371.0 | 2.47x |
| fileread@disk | MiB/s | 930.0 | 2595.9 | 2.79x |
| fork | us | 281 | 1259 | 0.22x |
| forkbig | us | 412 | 2177 | 0.19x |
| exec | us | 746 | 4239 | 0.18x |
| spawn | us | 733 | 3645 | 0.20x |
| fault | ns/page | 13612 | 9815 | 1.39x |
| pipelat | us | 62.5 | 97.2 | 0.64x |
| pipebw | MiB/s | 508.8 | 485.1 | 0.95x |
| tcplat | us | 165 | 249 | 0.66x |
| tcpbw | MiB/s | 189.1 | 413.4 | 2.19x |
| sleep | us | 1031 | 1171 | 0.88x |
| epwait | us | 1097 | 1164 | 0.94x |
| idle | % host CPU | 11.3 | 1.00 | 11.34x |

### Whole programs

`bench/workloads.py` (`make bench-workloads`) runs the same Alpine programs
on both systems, on the same Alpine root (the same binaries: gcc 15, GNU
make, apk, BusyBox tar and gzip), each on its own kernel and file system
(BastiOS's disk file system, ext4 on Linux). The host times each phase from
the serial line, as `bench/run.py` does; `apk` and `untar` change the root
and run once, the rest are medians of 3.

| Workload | What it does | BastiOS (s) | Linux (s) | BastiOS / Linux |
|---|---|---:|---:|---:|
| untar | unpack Alpine's mini root (about 500 small files) | 1.95 | 0.39 | 5.05x |
| apk | apk add gcc, make and musl-dev from a local repository | 16.77 | 9.56 | 1.76x |
| hello | gcc -O2 of one small C file, compile and link | 3.45 | 1.19 | 2.91x |
| make1 | make -j1 of a generated project: one gcc -O1 per file, then a link | 125.53 | 130.59 | 0.96x |
| make2 | the same with make -j2 | 71.19 | 66.86 | 1.06x |
| tgz | tar the gcc and header trees through gzip -1 into one file | 2.98 | 2.02 | 1.47x |
| scan | find over the whole root, grep -rl over the headers | 0.97 | 0.41 | 2.37x |

(2 GiB machine, one processor, QEMU TCG on a shared host with nothing else
running; the second table below splits the first run from the later ones.)

| Workload | BastiOS cold / warm (s) | Linux cold / warm (s) | BastiOS / Linux, cold / warm |
|---|---:|---:|---:|
| hello | 3.06 / 3.65 | 2.12 / 1.02 | 1.44x / 3.57x |
| make1 | 124.27 / 129.67 | 126.66 / 133.81 | 0.98x / 0.97x |
| make2 | 71.19 / 70.69 | 69.19 / 66.03 | 1.03x / 1.07x |
| tgz | 3.37 / 2.89 | 4.20 / 1.92 | 0.80x / 1.51x |
| scan | 0.97 / 1.05 | 0.41 / 0.41 | 2.37x / 2.57x |

How to read it: a build of many files is CPU-bound, and `make1` and `make2`
run at the speed of Linux's (0.96x to 1.06x); user-side work is the same
instructions on both. Where BastiOS is behind:

- `untar` (5x) is the file system: each file created costs about four
  synchronous metadata writes (`disk_set_owner` and `disk_set_meta` write the
  entry's sector and wait for it), where Linux's ext4 batches them in its
  journal. Deferring those writes would save at most about 18% and changes
  what survives a power cut, so it is not done. Counting the disk's own
  statistics (`/proc/diskstats`) for the 500-file root: 1082 write commands, about
  2.2 a file and a little over a millisecond each as the host sees them, and a
  `perf` profile of the run puts 40% of the host's samples in the guest's
  submit and poll loops (`virtio_submit`, `blk_poll`). The time is the number
  of round trips to the emulated disk, not the kernel's work between them.
  Most of a round trip is the host's `fsync`: the virtio-blk device does not
  take the cache feature, so QEMU makes every write durable before it answers,
  and the same run with `cache=unsafe` on the drive takes 0.71 s instead of
  1.16 s. Accepting the feature and flushing only at the queue's barriers does
  not help while the metadata sectors are written synchronously: each of those
  writes must then be followed by its own flush, and the run got slower (1.33 s).
  The gain needs those writes to go through the queue's group commit.
- `scan` (2.4x in the table) was profiled on a different machine from the
  table's, where a warm run took 0.54 s: the guest's own time is mostly
  `vfs_readdir` (14% of the samples), then `disk_find` (3%) and `vfs_nlink`
  (2%); about a third of the samples are QEMU's own code, which the build
  here has no symbols for. Not compared with Linux on that machine.
  Listing a directory on the disk asked `vfs_readdir` about every slot of the
  disk in turn, since a directory's entries are not stored together; `getdents`
  now skips over the slots of other directories in one tight loop, which took the
  same run on this host from 0.16-0.18 s to 0.14 s.
- `hello` was 3.6x off Linux when run again (3.65 s against 1.02 s), and
  slower the second time than the first. The time was in the emulator, not
  the disk: the page cache that shares program code between runs
  (`src/pcache.bastio`) held 16 MiB, less than the code gcc, cc1, as and ld
  run, so each run found its code gone, loaded it into different pages, and
  QEMU's translator (TCG, which keys translated blocks by physical address)
  translated cc1 again (about 80,000 translated blocks a run, against about
  13,000 when the pages are reused). The cache holds 32 MiB now and a warm
  `hello` takes about 0.7 s on the machine the first run takes about 1.5 s on.
- The disk's block cache stops at 32 MiB because doubling it makes Z3 give up
  on `lx_syscall`; it is a sixteenth of memory below that (`dq_slots_for` in
  `src/diskq.bastio`; the kernel heap extension, which it takes half of, is
  an eighth of the page pool). The page cache stops at 32 MiB because its
  arrays are fixed in size and so take kernel heap on a small machine too: 64
  MiB (16384 slots) took enough of a 30 MiB machine's heap that crashsim and
  the memory test failed. Sizing the arrays to the machine would fix that
  but gave Z3 more than it could prove in `lx_syscall`.

### Over a network card

`bench/netcmp.py` (`make bench-net`) runs `mtcp echo` (`user/mtcp.c`, the same
source on both systems, built with musl) in the guest, forwards a host port
to it through QEMU's user-mode network and drives it from the host: the
average round trip of 200 messages of 32 bytes, and the rate of echoing 2 MiB.
QEMU's user-mode stack is on the path of both systems and caps the rate, so
compare the two systems, not the absolute figures. Ubuntu's kernel builds
e1000 as a module, which the bench's initramfs does not carry, so only
BastiOS has an e1000 row.

| Card | Test | Unit | BastiOS | Linux | BastiOS / Linux |
|---|---|---|---:|---:|---:|
| virtio-net | round trip, 32 bytes | us | 263.6 | 316.3 | 0.83x |
| virtio-net | echo 2 MiB | MiB/s | 13.3 | 11.9 | 1.12x |
| e1000 | round trip, 32 bytes | us | 143.8 | n/a | |
| e1000 | echo 2 MiB | MiB/s | 0.2 | n/a | |

The e1000 driver moves bulk data at a fiftieth of virtio-net's rate (0.2
against 13.3 MiB/s) although its round trip is shorter; the cause is not
known yet. It is the gap that matters for real network cards and for
clouds that offer an e1000.

## Boot time and memory

Boot to the `bastios>` prompt, QEMU 8.2 TCG, one CPU, `-kernel`, on the same
no-KVM container (so these are emulator figures; the kernel's share of a
KVM boot has not been measured, and neither has a comparison with a minimal
Linux guest):

| Machine | Boot | Note |
|---|---:|---|
| `microvm` | 2.1 s | before the PIT channel 0 calibration: the local APIC timer was counted against two seconds of the RTC |
| `microvm` | 0.29 s | after it |
| `pc` | 0.2 to 0.4 s | includes QEMU's start-up |

The smallest RAM that boots is 36 MiB (1863 free 4 KiB pages at the prompt).
`-m 16M` prints nothing because the image is linked at 16 MiB. Details and
the failure modes of the other sizes are in [cloud.md](cloud.md).

## Tracking it

`bench/baseline.json` is the result file of one `make bench --os bastios`
run (see its `date`), kept in the repository so a change can be compared
with what came before. `make bench BENCH_ARGS="--os bastios"` followed by
`make bench-compare` prints the change in every test against it, marks any
that is more than 10% slower or faster, and exits 1 if one got slower.
`bench/compare.py OLD.json NEW.json` compares any two files. Update the
baseline in the commit that changes the numbers on purpose, and put the
table (and its change from the previous baseline) in the notes of every
release. TCG timing is noisy, and a baseline from another host proves
nothing: rerun both on the same host back to back before trusting a small
difference. The numbers are not hardware timing; once BastiOS runs on a
physical machine, record that table next to Linux's on the same machine.

`bench/run.py --icount` times by instructions executed instead of host time
(TCG with `-icount shift=0`: the guest clock advances one nanosecond per
instruction). Three runs of one test agree to 0.02%, where wall time moves by
20% with the host's load, so it is the way to see a change of a few percent.
It counts instructions, not cycles: a TLB flush or a cache miss costs nothing
in it, so compare two kernels with it, and do not read it against Linux's
times. `bench/insnprof.py -- COMMAND` says where the instructions go: it
single-steps the guest from gdb and charges each to a kernel function, per
call. On 2026-10-13 a `getppid` call took 2640 instructions (after the
`sock_wake` fix; the FPU page's copy in and out by loops in the compiler's
runtime is about 600 of them, the 21 registers' save and 18 restores about
500, `sched`, `finish` and `next_job` the rest), and a `stat` of a tmpfs file
12500: the 2600 of any call plus path lookup (`ns_find` 1400, `ram_find` 1200,
`take_node` and `put_node` 1800, `put_stat` 850, an allocation 800).
Two scans that cost the same on every call were cut with a high-water mark
(`Kernel.ofd_hi`, `Kernel.mt_hi`): `ns_find` looked at all 64 mount slots, and
`pidfd_reuse` at all 4096 open file descriptions on each fork. After them a
`stat` of a tmpfs file is 11200 instructions (`ns_find` is out of the
profile), and `fork` + `exit` + `wait` 160600, down from 202200; a fork of
a group with no `memory.max` also stopped counting the parent's pages. What
is left in a fork is table walking: `clone_l1`, `free_l1` and their kin read
all 512 entries of every page table the process has, about 3000 instructions
a table, whatever it holds. The other fixed scan is the 1024-slot descriptor
table, walked on every process exit and fork: `lx_fd_close_all` called
`lx_fd_close` for each slot (5700 instructions) and `lx_fd_fork` took a
reference for each; both now look at the slot first (the exit walk is 1500).
By `bench/run.py --icount` that is `fork` 196 to 168 us and `exec` 655 to
629 us.
`execve` of a file asked `file_writing` about all 36864 descriptor rows
(ETXTBSY) although the table grows with the process table; it now stops at
the table's end, and `terminate`, `wake_readers`, `disk_wake`,
`sleepers_due` and the thread-group check stop at the process table's end
too. `exec` goes from 625 to 439 us and `spawn` from 567 to 380 us.
Closing a file dropped the process's record locks by walking all 128 lock
entries; the table now keeps a high-water mark (`LK_HI`, one word after the
wait slots), which takes `open` + `close` from 16.8 to 14.9 us and `create`
on tmpfs from 35.3 to 33.4 us.

`make buildbench` (`test/buildbench.sh`) is an end-to-end number the table
does not have: `make -j1` and `make -jN` of 100 small C files with tcc, on the
development disk, on `-smp N` (`SMP=`, default 2). It prints the median seconds
of each and the speed-up. Measured on one shared 4-core host under QEMU TCG:
`-j1` 1.8 s (the same on one processor and on two), `-j2` 1.4 s (1.3 times as
fast), `-j4` on `-smp 4` 1.2 s (1.5 times). Those fit a fixed 1.0 s that does
not shrink plus 0.8 s that divides by the job count, so about half of a build
is serial. Most of that is probably system call time: every call of every
processor takes the kernel's one lock (`Lock[Sys]`, see "How it scales" in
docs/architecture.md), and a compile is mostly `exec`, file creation and
reads. That is a guess from the fit, not a profile. It is not part of
`make bench`, which runs the same program on Linux; this one is BastiOS alone.

## Reading the numbers

TCG is not a processor. It inflates some costs and hides others, and it
treats the two kernels differently:

- **Every CR3 load empties QEMU's whole software TLB.** That also loses its
  cache of translated-code lookups. BastiOS loads CR3 three times per
  system call (`mmu_exit`, `mmu_enter`, and `cpu_run` again), and Linux
  loads it none. `bench/prof.py` on the syscall test puts about 85% of the
  samples inside QEMU, in TLB refills and translation-block lookups,
  rather than in translated guest code. An experiment that skipped the
  redundant loads (it was unsafe and is not merged) brought `syscall` from
  22.8 to 5.8 us, `pipelat` from 97 to 26 us and `open@tmp` from 95 to 27
  us. On real hardware the share is smaller but real, since a CR3 load
  without PCID flushes the TLB there too.
- **QEMU's software TLB grows only when the guest flushes it.** Linux
  rarely flushes, so a working set over about 1 MiB thrashes it.
  `memcpy` uses 256 KiB buffers for this reason (1 MiB buffers cost Linux
  6.8 ms/MiB against BastiOS's 0.37). The same effect probably weighs on
  Linux's `fault` and `forkbig`, so BastiOS's lead there is not a real
  lead.
- **Linux under TCG has no stable TSC.** Its `clock_gettime` makes a
  system call to the HPET, while BastiOS's vDSO reads the TSC. On real
  hardware both use the vDSO.

Only run the comparison under KVM or on real hardware to get absolute
numbers. The ratios that are far above 1 under TCG are worth fixing on any
machine, because they come from algorithms, not from emulation.

## The gaps, largest first

1. **Disk file reads (28x) and writes (23x).** Consecutive writes to
   one file used to be a copy-on-write transaction each: new data, map and
   root blocks, two flushes, then the directory block. They now share a
   transaction (a batch, `src/diskfs.bastio`): the blocks stay in the cache
   until the batch commits, on `fsync`, `sync`, `msync`, a write to another
   file, any other change to the disk, 32 blocks, or 0.1 s. `filewrite@disk`
   went from 1.9 to 6.6 MiB/s (median of 3, TCG, same host). The cost is
   that a write is on the disk only after one of those, not when the call
   returns. A `read` of a disk file now leaves the pages it read in the page
   cache (`pc_fill`, `src/pcache.bastio`), up to an eighth of memory and
   at most 16 MiB, and the next read of those pages is answered from
   there without the disk (`pc_have` in `vfs_read`, `src/fs.bastio`);
   writes, truncation and removal keep them current as for mapped pages.
   `fileread@disk` went from 47 to 157 MiB/s (median of 3, TCG, same
   host; the first of the benchmark's eight passes over the 8 MiB file
   still reads the disk). What is left: a miss reads through the 1 MiB
   block cache with 8 blocks of read-ahead, a write does not fill the
   cache, and nothing gives cached pages back when memory runs low (the
   cap is what bounds them).
   Writes are limited by the host, not by the kernel's code: a
   `bench/prof.py` profile of `filewrite@disk` charges about 40% of the
   samples to the host kernel's file system and scheduler inside the QEMU
   process (the image file's writes and the flushes BastiOS sends at each
   commit) and under 10% to BastiOS's own functions, and raising the batch
   from 32 to 64 blocks left the rate unchanged (4.7 and 4.0 MiB/s). Linux
   writes to its page cache and sends no flush during the run, so the
   comparison measures durability as much as speed; closing it means
   giving up the commit-on-timer guarantee, which this file system keeps on
   purpose.
2. **System call entry and exit (9x; an older table had 30x, before CR3
   parking).** A process's table now stays in CR3 across a call that
   returns to it, so `getppid` takes about 9 us (`bench/baseline.json`)
   against Linux's 0.64 us, where the table above measured 19.6 us. A
   `bench/prof.py` profile of the `syscall` test now charges the time
   mostly to emulation of the x87/SSE state, not to the kernel's own work:
   leaving out `fxrstor` and `fxsave` around every run (an experiment, not
   merged, since the registers would be lost) took `getppid` from about
   9.5 to 5.9 us, and `fxrstor` alone is 2.2 us of that. QEMU's TCG
   implements both with helper calls over 16 vector and 8 x87 registers,
   where hardware takes tens of cycles, so this part should shrink a lot on
   KVM or a real processor. The rest is the kernel's own: `rdtsc` six times
   per turn (about 1.4 us under TCG, measured by leaving them out), the scan for a ready process over all
   256 slots (`lx_pick`, about 1.5 us; `NPROC=32` saves that much) and
   copying 39 registers in and out. A lazy FPU switch (CR0.TS and a device
   not available handler) would remove the first part, and a ready queue
   the second; neither is done, and neither is worth doing before the
   numbers can be taken under KVM or on hardware.
3. **`stat`, `open` (3 to 8x).** Partly (2). `stat` also counted hard
   links by scanning all 1024 directory slots of the disk. It now scans
   only the slots the directory holds, which made `stat@disk` 4.9 times
   faster (351 to 72 us, measured on e7b211b). On tmpfs, `ram_links` still scans all 128
   nodes.
4. **TCP bulk (1.8x now, 14x before).** Each connection had 4 KiB buffers and no
   window scaling, loopback held eight frames, and a `read` or `write`
   moved at most 4 KiB. Connections now have 256 KiB rings of pages taken
   as data arrives, window scaling, a 64-frame loopback queue that TCP
   does not overflow, no TCP checksum round the loopback, and calls that
   move up to the ring's size (`src/tcpbuf.bastio`). `tcpbw` went from
   14.7 to 73.8 MiB/s on one host under TCG (`-smp 2`, median of 5,
   main and this change run back to back); `tcplat` did not change
   beyond the noise (276-336 us before, 313-338 after). A later change
   (frames of up to 15000 bytes on loopback, a 96 KiB variable-length queue)
   took the table's `tcpbw` to 105.8 MiB/s, 1.8x Linux's 194 on the same run.
   What is left is per segment: every 1460 bytes is a frame built, queued,
   copied and taken in again, where Linux's loopback carries 64 KiB
   segments.
5. **`forkbig` (0.33x in the table; it was 2.1x).** Fork used to copy every page. bastios
   #146 moved the copying off the lock (plain `fork` went from 1270 to
   340 us). Fork now shares a single-threaded process's pages
   copy-on-write (src/lxcow.bastio): measured A/B on the same host, two
   rounds of three runs each, `forkbig` went from 4383-4783 us to
   561-572 us, about 4x faster than Linux's 2398. Plain `fork` went from
   351-411 us to 438-489 us: the parent's first write to each page after
   a fork is now a fault and a copy, even after the child is gone, since
   a page whose other reference was dropped is still copied (no way yet
   to take back a sole reference to a shared page as a private one).

## Scaling with processors

`bench/scale.py` runs `bench/scale.c` on 1, 2, 4, .. processors: n workers
on n processors each do a fixed amount of one kind of work (user-mode spin
as the control, a system call, fork + exit, a pipe and a socketpair
ping-pong, anonymous page faults, open/read/close), and it prints
operations per host second and the ratio to one processor. The kernel runs
every system call under one lock, so the ceiling is one over the time that
lock is held. A kernel built with `-DBST_LOCKSTAT` counts it:
`bench/lockstat.py --smp 4 -- /disk/scale syscall 4 400` prints, per lock,
how often it was taken and waited for, and the time spent holding and
waiting.

Under QEMU's TCG the time in the lock is mostly not instructions. Every
trap writes CR3 twice (the kernel's tables and the user's shadow tables
are separate) and TCG has no PCID, so each system call starts with an empty
software TLB and the kernel pays a refill for every page it touches while
it holds the lock: a `read` of `/dev/zero` runs about 1500 instructions
under the lock, spread over 27 pages of code. `bench/hot-functions.txt` lists
the functions the system call, file, fork, fault and pipe paths run, and the
linker puts them first in the text (`build/hot.ld`), so the same paths touch
16 to 33 pages instead of 27 to 66. `bench/hotfuncs.py` regenerates the list.

Four host cores, one run each, work per worker fixed (operations per second):

| test | before, 1 CPU | before, 4 CPU | after, 1 CPU | after, 4 CPU |
|---|---|---|---|---|
| syscall | 61.9k | 87.8k | 67.7k | 118k |
| pipe | 17.0k | 21.2k | 18.3k | 29.2k |
| file | 10.5k | 12.8k | 12.6k | 15.7k |
| fork | 3.2k | 4.0k | 3.6k | 4.8k |
| fault | 68.0k | 131k | 80.2k | 215k |

Two thirds of the gain is the hot-function layout and the rest is the
lock's bounded plain spin (bastiolang). The numbers move by about 10% from
run to run. Four processors give less than four times one because the
lock is held about 8 us of each 16 us system call and is saturated at
four; the next step is shortening what runs under it, not more cores.
