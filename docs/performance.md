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

1. **Disk file writes (58x) and reads (41x).** Each `write` of up to
   4 KiB is its own copy-on-write transaction: new data, map and root
   blocks, two flushes, then the directory block (`disk_write` in
   `src/diskfs.bastio`). Reads of a file larger than the block cache
   reach the disk again. Group commit and a page cache are the fixes. The
   file-system thread owns the page cache. bastios #146 added a 1 MiB
   block cache, which an 8 MiB file does not fit in.
2. **System call entry and exit (30x).** Mostly the three CR3 loads above.
   The fix is to keep a process's table loaded across a call that returns
   to it, and to reload only when a table changed. That is a change to the
   C prelude of bastiolang (`mmu_enter`, `mmu_exit`, `cpu_run`).
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
5. **`forkbig` (2.1x).** Fork copies every page. bastios #146 moved the
   copying off the lock (plain `fork` went from 1270 to 340 us) but does
   not share pages copy-on-write.
