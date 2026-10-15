# Group commit for metadata operations

Status: design, nothing implemented yet.

## Problem

`/proc/diskstats` (disk-wait counters) shows that creating a small file
costs two drive flushes. In `src/diskfs.bastio`, `disk_entry` (create,
mkdir, link) first calls `disk_drain`, so every queued write reaches the
disk; it then writes the directory block through `ent_flush` and flushes
the drive again. A loop that creates N files therefore waits for 2N
flushes, one at a time, and a flush is the slowest command on every
driver (see the table in `docs/architecture.md`).

Data writes already group: `disk_write` builds one transaction per file
(data, flush, directory entry, flush) and commits it after `BATCH_TICKS`
or when the batch is full. Metadata operations (create, mkdir, link,
unlink, rename, truncate) do not.

## What must not change

1. Ordering inside a transaction: new data and maps are durable before
   the directory entry that points at them. Group commit never moves a
   directory write ahead of the flush that precedes it.
2. `fsync`, `sync`, `O_SYNC` and unmount still return only after the
   operation is on the medium. Only the time at which an *unsynced*
   operation becomes durable moves, and POSIX allows that.
3. A crash leaves the old state or the new one for each operation, and
   operations appear in the order they were issued (no later create
   survives while an earlier one is lost). `fsck` needs no new case.

## Proposal

Keep the in-memory directory as the source of truth (it already is) and
make the directory-block write for create/mkdir/link a *queued*
operation instead of a synchronous one:

- `disk_entry` sets the entry in memory, marks its directory block dirty
  (a small bitmap or list, bounded by the existing `BATCH_MAX`), and
  returns without `disk_drain` or a flush.
- A pending-metadata set is committed by the same path that commits a
  write batch: after `BATCH_TICKS`, when full, on `fsync`/`sync`/unmount,
  and before any operation that needs the earlier state on disk
  (unlink or rename of a pending entry, `disk_take_name`, resize).
- The commit queues, in order: all pending data blocks, one flush, all
  dirty directory blocks, one flush. N creates in a tick cost 2 flushes
  instead of 2N. Directory blocks are written in ascending order, one
  command per block, which the queue already batches.

Order of entries within one directory block is by slot, and slots are
allocated in issue order, so "later create survives, earlier lost" can
only happen across blocks. To rule that out the commit writes the dirty
blocks in order of the oldest pending entry in each, and the second
flush is a barrier after all of them; a cut between two block writes
leaves a prefix of the creates, as the single-entry case does today.
This must be checked, not assumed (see Verification).

Not in scope: changing the transaction format, journaling, or the
cache (the page-cache thread's area).

## Risks

- A create that returns success may be lost by a power cut within
  `BATCH_TICKS` (about 10-30 ms) unless the caller syncs. This is the
  same contract as buffered data writes today, but it is a visible
  change for `create` followed by a cut. `crashme` currently relies on
  create being durable on return; it would need an explicit sync, and
  that is the one test change this work makes.
- Read-your-writes: lookups use the in-memory directory, so they see
  pending entries. Anything that reads directory blocks from the disk
  (fsck at mount, `disk_find` on a cold cache) runs only after a drain.
- The proofs: `disk_ok` must cover the pending set, and
  `disk_entry`'s contract must still say `blocks`/`slots` are
  preserved.

## Verification

1. `test/crashsim.py` on `user/crashme`, with a creation phase that
   makes many files and checks the prefix property above. It must pass
   with the flush between data and directory removed from nowhere: the
   existing mutation (dropping that flush) must still fail thousands of
   images.
2. A new mutation: write directory blocks in reverse order. crashsim must
   fail it. If it does not, the test is too weak and the work stops.
3. `diskbench` gains a "create 200 files" phase; the number to read is
   `/proc/diskstats` flush count per file (2 now, expected below 0.1).
4. `make test` on virtio, NVMe, AHCI and the MDTS-1 NVMe session.

## Coordination

`src/diskfs.bastio` and crash consistency belong to the file system
robustness work. This document is the proposal; the change to
`disk_entry`, `disk_commit` and `settle_batch` should be reviewed there
before it lands.
