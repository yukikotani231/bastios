# Design: disk I/O that does not stop the machine

Status (2026-10-10): stages 2 and 3 are done, in one PR. They needed each
other: the cache is what a retried read finds. The queue holds one
command at a time, and stage 1's cache arrived with them. The rest is
still planned. Numbers are in docs/architecture.md ("Waiting for the disk
without stopping the machine").

## Where we are

A file system call runs from start to end inside `turn`, under
`Lock[Sys]`. Every block command polls the device until it completes. For
the whole time the disk is busy:

- the calling processor spins on the device;
- every other processor that traps spins on `Lock[Sys]`;
- the disk has at most one command in flight.

The device's time is spent with the lock held. On real hardware that is
tens of microseconds per command for NVMe and milliseconds for a disk
that has to seek. Under TCG it is the host's fsync.

## What we want

1. A process waiting for the disk sleeps, and the others run.
2. The disk can have several commands in flight (NVMe and virtio take
   many, AHCI 32, ATA one).
3. Completion is signalled by an interrupt (MSI-X on NVMe and virtio, MSI
   on AHCI, IRQ 14 on ATA), not by polling.
4. The guarantees stay the same. A write call lands whole or not at all
   across a power cut, and memory safety is proved without runtime checks.

## The model: retry for reads, completion for writes

BastiOS has no kernel stack per process. A blocked system call either
restarts from scratch when woken (`lx_retry`, as `CONWAIT` and the console
read do), or gets its result written in by whoever completes it (as ping
and `NETWAIT` do). We use both:

- **A call that only reads the disk is retried.** If the blocks it needs
  are not in the block cache, it queues reads for all of them at once and
  sleeps in a new state, `DISKWAIT`. Once they are in, it is made `READY`
  and its call restarts from the beginning. This time it finds every block
  in the cache. Reads change nothing until they copy out, so a restart is
  harmless. A file offset only advances when the call returns.
- **A write is split into two phases.**
  - *Gather* reads every old block it needs: the partly overwritten data
    block, and the maps on the path. A miss sleeps and retries, as a read
    does.
  - *Commit* runs only once nothing can miss. It builds the new blocks in
    the cache and updates the in-memory directory. It then queues one
    transaction: the data and map blocks, a flush, the entry's sector, and
    a flush. The caller sleeps in `DISKWAIT` with its result already
    decided, and the transaction's completion writes that result into its
    `rax` and makes it `READY`.
  - Blocks that the write frees are put on the transaction's deferred list
    and only returned to the free map when it completes, so a power cut
    can never see them reused.
  - Later readers of the new blocks hit the cache, so the file reads back
    as written even before the disk has it.

Ordering on the disk is the block layer's job. Transactions run in FIFO
order, and a flush is a barrier. That keeps the current power-cut
argument word for word: the entry is written only after the blocks it
points at are on the medium.

## Stages

Each stage keeps `make test` green, including the ten power cuts, and is
measured. The baselines are taken before stage 1.

1. **Block cache.**
   - Data and map blocks go through a fixed cache of 4 KiB blocks, about
     64 blocks (256 KiB) taken from the page allocator at mount, with
     clock replacement. Writes update the cache.
   - The cache pays on its own, because `data_ptr` re-reads a map sector
     on every call, and it is the base for the retries.
   - Touches `src/diskfs.bastio` (FS thread's file; see coordination).
2. **A request queue under `Blk`.**
   - `blk_submit(req)` and `blk_poll() -> done tags`, so each driver can
     keep several commands in flight with a few lent data pages each.
     There are about 8 on NVMe and virtio, 8 AHCI slots, and 1 on ATA.
   - The synchronous `blk_read_block`/`blk_write_block` become
     submit-then-poll wrappers, so nothing above changes yet.
   - Touches `src/blk.bastio` and the drivers (mine).
3. **Sleeping on the disk.**
   - Add the `DISKWAIT` state, the read retry and the two-phase write
     (`diskfs`, `fs.bastio`, `lxfd.bastio`/`syscall.bastio` call sites).
   - Completions are found by polling `blk_poll` in `next_job` when it
     polls devices. An idle processor with I/O in flight polls instead of
     halting, since a halt would wait for the next 10 ms tick.
   - Touches `src/main.bastio`'s `next_job` and idle path (SMP thread's
     area).
4. **Splitting the lock, only if the numbers ask for it.**
   - After stage 3, nothing waits for the device with `Lock[Sys]` held.
     What is left inside it is CPU work: copying 4 KiB and building maps.
     That is in line with other system calls.
   - We measure contention with the mixed benchmark below. If file calls
     still hold others up, the disk state (the `DiskFs` with its `Blk`,
     cache and queue) moves into its own `Lock[Disk]`, taken by the
     scheduler loop after `turn` has returned "needs the disk". The order
     is `Term`, then `Sys`, then `Disk`, never the other way round.
   - That change rewires `turn`, so it waits for the SMP thread's
     page-table lease work to land, and we design it together.
5. **Interrupts.**
   - A disk vector (`V_DISK`, 0x31) is delivered to the bootstrap
     processor's local APIC: by MSI-X on NVMe and virtio-blk, MSI on
     AHCI, and IRQ 14 through the 8259 on ATA. Its handler does what the
     poll in stage 3 does.
   - Idle processors halt again.
   - In the trusted base, `boot/smp.c`'s `bst_lapic_ack` must also EOI
     `V_DISK`, and the IDT gate must exist. That is the one C change, to
     be said in that PR.
   - The legacy virtio device moves its device-specific configuration 4
     bytes further on when MSI-X is on, and the driver must follow.

## Numbers to take, before stage 1 and after each stage

- `make diskbench`: one process, 2 MiB written and read back, on each
  driver.
- `make smpbench`: N copies of `bench`, as before.
- New: a mixed run on `-smp 2` with one `diskbench` and one `bench` at
  once. `bench`'s getpid and spawn ticks show how long file calls hold the
  lock, which is the thing stages 3 and 4 should cut.
- New: two `diskbench` at once on NVMe, which shows queue depth above
  one.

## What stays trusted

- That a device touches only the pages lent to it, as now.
- That the interrupt controller delivers the vector we programmed.
- That a flush means the data is on the medium, as now.

Nothing new is trusted about ordering: the queue's FIFO-with-barriers rule
is code, and the power-cut test checks it.

## Coordination

- **FS thread** (`src/diskfs.bastio`, `src/fs.bastio`): stages 1 and 3
  change diskfs substantially: the cache, misses, two-phase write and
  deferred frees. My proposal is that I write them and the FS thread
  reviews. It would avoid diskfs edits while each of those PRs is open,
  or tell me if it would rather do the diskfs half itself.
- **SMP thread** (`src/main.bastio`'s `sched`/`turn`/`next_job`,
  `Lock[Sys]`, `boot/smp.c`):
  - Stage 3 adds a `blk_poll` next to `nic_poll` in `next_job` and a "do
    not halt while I/O is in flight" check in the idle path.
  - Stage 4, if needed, is a joint design.
  - Stage 5 adds one EOI line in `bst_lapic_ack`.
  - I'll rebase onto the lease work rather than race it.
