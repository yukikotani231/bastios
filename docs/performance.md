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

`bench/prof.py -- COMMAND` shows where the time goes for one command on
BastiOS. It samples QEMU with Linux `perf` and charges each sample to a
kernel function through QEMU's `-perfmap`, or to QEMU itself.

## Results

BastiOS main (f71b0a8), after the loopback TCP ring and the COW fork work.
This host ran slower than the one behind earlier tables (`cpu` 2.9 ns
against 1.4), so compare the ratios, not the absolute figures.
QEMU 8.2 TCG, `-smp 2`, `-m 512M`, median of 3 runs, on a 4-processor
cloud container with no KVM. The ratio column is BastiOS's time divided by
Linux's for the same work, so a figure above 1 means BastiOS is slower.

| Test | Unit | BastiOS | Linux | BastiOS / Linux |
|---|---|---:|---:|---:|
| cpu | ns/iter | 2.89 | 3.00 | 0.96x |
| memcpy | us/MiB | 110 | 112 | 0.99x |
| syscall | ns | 9364 | 1026 | 9.13x |
| clock | ns | 145 | 2623 | 0.06x |
| open@tmp | us | 43.4 | 15.2 | 2.86x |
| open@disk | us | 54.8 | 18.8 | 2.91x |
| stat@tmp | us | 26.2 | 9.09 | 2.88x |
| stat@disk | us | 64.3 | 8.08 | 7.95x |
| create@tmp | us | 203 | 252 | 0.81x |
| create@disk | us | 5638 | 734 | 7.68x |
| filemap@tmp | ns/page | 7080 | 3927 | 1.80x |
| filemap@disk | ns/page | 62277 | 4213 | 14.78x |
| filewrite@tmp | MiB/s | 560.8 | 180.0 | 0.32x |
| filewrite@disk | MiB/s | 5.6 | 126.9 | 22.75x |
| fileread@tmp | MiB/s | 663.4 | 1147.4 | 1.73x |
| fileread@disk | MiB/s | 47.2 | 1322.5 | 28.05x |
| fork | us | 768 | 2313 | 0.33x |
| forkbig | us | 1236 | 3722 | 0.33x |
| exec | us | 1896 | 8446 | 0.22x |
| spawn | us | 1555 | 7092 | 0.22x |
| fault | ns/page | 66480 | 22687 | 2.93x |
| pipelat | us | 165 | 250 | 0.66x |
| pipebw | MiB/s | 281.4 | 167.8 | 0.60x |
| tcplat | us | 278 | 482 | 0.58x |
| tcpbw | MiB/s | 105.8 | 194.0 | 1.83x |
| sleep | us | 1024 | 1329 | 0.77x |
| epwait | us | 1054 | 1307 | 0.81x |
| idle | % host CPU | 15.0 | 2.00 | 7.51x |

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
