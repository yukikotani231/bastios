# The page cache

`src/pcache.bastio` keeps one shared page per (file, page of the file) that
a `MAP_SHARED` file mapping has asked for. Every process that maps the same
part of the same file maps that one page, so their stores meet, and a futex
in it wakes across processes (`lx_futex` keys it by the physical page).

## Where it lives

- The cache is a field of the file layer (`Vfs.pc` in `src/fs.bastio`), so the
  file layer keeps it current on every path, native or Linux:
  - `vfs_write` copies the written bytes into the cached pages they cover;
  - `vfs_read` and `vfs_read_at` lay the cached bytes over what the file
    holds, so a store into a mapping is seen before it is written back;
  - `vfs_truncate` and `O_TRUNC` drop the pages past the end and zero the
    tail of the last one;
  - the last close of a file drops all of its pages, and so does mounting
    it as a loop image.
- `lx_mmap` (`src/linux.bastio`) maps the cached pages for a `MAP_SHARED`
  file mapping (`lx_map_file_shared` in `src/lxmem.bastio`), reading a page
  in on first use.

## What the proof covers and what it does not

The pages are bastiolang `Shared` references held in a level-1 page table
that no processor loads. The proof shows that the cache's reference and
every mapping's reference are each released exactly once (`drop_shared`),
that the kernel touches only the bytes it means to (`bytes_to_frame` and
`frame_to_bytes` bounds), and that every page is back in the pool at power
off (`pc_free`, checked by `make test`). The reference count itself is in the
prelude (`bst_shared_refs`), which is trusted, as for anonymous `MAP_SHARED`.

The proof does not show that the cache is coherent: that a cached page holds
the file's bytes plus the stores made since. That rests on the hooks above
being on every path that changes a file, and on the compat checks in
`test/compat/progs/t_mem.c`.

## Choices and gaps

- A handle (a RAM node, or a disk or loop slot) is named again once its file
  is freed, which only happens after its last close. Pages are dropped at the
  last close, and a mapping keeps its file open (the write-back record holds
  a reference), so a cached page never outlives its file's handle.
- The cache does not replace the file's own bytes: a mapping is still
  written back at `msync`, `munmap` and exit, as before.
- It holds at most 512 pages (2 MiB). When full, the oldest slot is reused
  even if a process still maps its page. That process keeps the page, but a
  later `mmap` of that part of the file gets a fresh copy (the next
  write-back reconciles the file).
- Reads and writes go through the file system as before. The cache does not
  serve them, so it does not make them faster. With nothing mapped they cost
  one check of a flag word (`pc_on`).

## Measured cost

`make diskbench` (2 MiB written and read back in 4 KiB calls, three runs,
median ticks), on main before the change and with it, same host:

| Driver | write before | write after | read before | read after |
| --- | --- | --- | --- | --- |
| ATA | 216 | 192 | 41 | 46 |
| virtio | 141 | 120 | 16 | 15 |
| NVMe | 83 | 72 | 13 | 13 |
| AHCI | 66 | 74 | 13 | 13 |
| NVMe 4K | 76 | 75 | 13 | 17 |

The differences go both ways and are within the run-to-run spread (for
example, virtio writes on main ranged from 102 to 147 ticks), so the hooks
have no measurable cost on the native path. diskbench maps nothing, so it
never exercises the cache itself.
