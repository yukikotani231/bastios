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

BastiOS main (b3aa0d4), with the `disk_links` fix from this change.
QEMU 8.2 TCG, `-smp 2`, `-m 512M`, median of 3 runs, on a 4-processor
cloud container with no KVM. The ratio column is BastiOS's time divided by
Linux's for the same work, so a figure above 1 means BastiOS is slower.

| Test | Unit | BastiOS | Linux | BastiOS / Linux |
|---|---|---:|---:|---:|
| cpu | ns/iter | 1.39 | 1.84 | 0.75x |
| memcpy | us/MiB | 77.8 | 75.0 | 1.04x |
| syscall | ns | 19604 | 644 | 30.43x |
| clock | ns | 135 | 1349 | 0.10x |
| open@tmp | us | 91.7 | 8.81 | 10.40x |
| open@disk | us | 80.2 | 9.58 | 8.37x |
| stat@tmp | us | 47.4 | 5.48 | 8.64x |
| stat@disk | us | 67.4 | 6.87 | 9.80x |
| create@tmp | us | 233 | 171 | 1.36x |
| create@disk | us | 4299 | 593 | 7.25x |
| filewrite@tmp | MiB/s | 252.8 | 290.7 | 1.15x |
| filewrite@disk | MiB/s | 2.2 | 128.4 | 57.92x |
| fileread@tmp | MiB/s | 205.5 | 1758.9 | 8.56x |
| fileread@disk | MiB/s | 27.3 | 1113.9 | 40.87x |
| fork | us | 340 | 1680 | 0.20x |
| forkbig | us | 4996 | 2398 | 2.08x |
| exec | us | 1268 | 5399 | 0.23x |
| spawn | us | 1171 | 4524 | 0.26x |
| fault | ns/page | 1993 | 10594 | 0.19x |
| pipelat | us | 95.6 | 121 | 0.79x |
| pipebw | MiB/s | 306.9 | 376.0 | 1.23x |
| tcplat | us | 167 | 290 | 0.58x |
| tcpbw | MiB/s | 24.1 | 328.5 | 13.62x |

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

1. **Disk file reads (41x) and writes (about 17x).** Consecutive writes to
   one file used to be a copy-on-write transaction each: new data, map and
   root blocks, two flushes, then the directory block. They now share a
   transaction (a batch, `src/diskfs.bastio`): the blocks stay in the cache
   until the batch commits, on `fsync`, `sync`, `msync`, a write to another
   file, any other change to the disk, 32 blocks, or 0.1 s. `filewrite@disk`
   went from 1.9 to 6.6 MiB/s (median of 3, TCG, same host). The cost is
   that a write is on the disk only after one of those, not when the call
   returns. Reads of a file larger than the block cache reach the disk
   again; a page cache is the fix. bastios #146 added a 1 MiB block cache,
   which an 8 MiB file does not fit in.
2. **System call entry and exit (about 14x; the table's 30x predates CR3
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
3. **`stat`, `open` (8 to 10x).** Partly (2). `stat` also counted hard
   links by scanning all 1024 directory slots of the disk. It now scans
   only the slots the directory holds, which made `stat@disk` 4.9 times
   faster (351 to 72 us, measured on e7b211b). On tmpfs, `ram_links` still scans all 128
   nodes.
4. **TCP bulk (14x before).** Each connection had 4 KiB buffers and no
   window scaling, loopback held eight frames, and a `read` or `write`
   moved at most 4 KiB. Connections now have 256 KiB rings of pages taken
   as data arrives, window scaling, a 64-frame loopback queue that TCP
   does not overflow, no TCP checksum round the loopback, and calls that
   move up to the ring's size (`src/tcpbuf.bastio`). `tcpbw` went from
   14.7 to 73.8 MiB/s on one host under TCG (`-smp 2`, median of 5,
   main and this change run back to back); `tcplat` did not change
   beyond the noise (276-336 us before, 313-338 after).
   What is left is per segment: every 1460 bytes is a frame built, queued,
   copied and taken in again, where Linux's loopback carries 64 KiB
   segments.
5. **`forkbig` (2.1x in the table).** Fork used to copy every page. bastios
   #146 moved the copying off the lock (plain `fork` went from 1270 to
   340 us). Fork now shares a single-threaded process's pages
   copy-on-write (src/lxcow.bastio): measured A/B on the same host, two
   rounds of three runs each, `forkbig` went from 4383-4783 us to
   561-572 us, about 4x faster than Linux's 2398. Plain `fork` went from
   351-411 us to 438-489 us: the parent's first write to each page after
   a fork is now a fault and a copy, even after the child is gone, since
   a page whose other reference was dropped is still copied (no way yet
   to take back a sole reference to a shared page as a private one).
