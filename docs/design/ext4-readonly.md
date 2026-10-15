# Design: reading ext4

Status: proposal. Nothing here is built yet.

## Goal

`mount -t ext4 -o ro IMAGE DIR` shows the files of an ext4 file system:
regular files, directories, symbolic links, hard links, device nodes,
modes, owners and times. Reading is all it does; a write to the mount
fails with `EROFS`, and the ext4 image is never changed.

Why: a disk or image made on Linux (a root file system built with
`mke2fs -d`, a cloud image's root, a USB stick) can be read without
converting it first.

## What constrains the design

1. **The handle space is fixed.** `src/fs.bastio` names a file by a
   handle below `HFILES = NFILES + 2 * DSLOTS`: the in-memory nodes
   (32768), the boot disk's entries and the loop mount's entries
   (262144 each). A descriptor row stores `(fs, node)`, `nopen` has one
   counter per handle, and the page cache and the Linux layer key their
   state by handle. ext4 inode numbers run to millions, so they cannot be
   handles, and a new `FS_EXT` would need its own range, a handle table
   and a place in `nopen`, the page cache and every `st_ino` user.
2. **Every operation dispatches on the file system.** `grep -c "FS_LO\|lo_pfx"
   src/fs.bastio` gives about 60 places (open, read, write, truncate,
   unlink, rename, link, mkdir, rmdir, symlink, mknod, meta, owner,
   readdir, dir lookup, stat, sync, orphan handling). A new file system
   type that is a peer of `FS_DISK` and `FS_LO` branches in all of them,
   and each branch needs a proof that the write paths leave it alone.
3. **The write-path branches are the expensive part.** Reading needs
   about 15 of them (open, close, read, read_at, size, stat, dirent,
   readdir, dir_lookup, child, meta, node_type, is_link, target, nlink).
   Everything else must be refused for the mount, in every case, which is
   the part that is easy to get wrong.
4. **The kernel drives one disk.** `blk_find` (src/main.bastio) picks one
   device, and `diskfs` owns it whole. An external ext4 disk therefore
   cannot be mounted today: there is no second `Blk`. The loop mount
   (`Blk::Loop`) is the only second block source.
5. **Names are whole paths.** A RAM node stores its full path, at most
   255 bytes (`FNAME_MAX`), not a directory entry.

## The choice: import into a read-only tmpfs mount

Instead of a new file system next to `FS_DISK` and `FS_LO`, mounting
walks the ext4 tree once and makes ordinary in-memory nodes (`FS_RAM`)
under the mount point, then marks the mount read-only
(`vfs_mount(..., ro)`, `vfs_set_ro`, which tmpfs mounts already have).

- No new handle space, no new `nopen` rows, no new dispatch branches:
  open, read, mmap, stat, readdir, `st_ino`, symlinks, hard links,
  device nodes, modes and owners all work as they do for a tmpfs, and the
  existing read-only mount check refuses every write path in one place.
- The page cache, `MAP_SHARED` and `execve` of a file on the mount need
  nothing new, because the files are RAM files.
- Unmount frees the nodes with `vfs_umount`, as for a tmpfs.

The importer is `src/ext4.bastio`: a reader of the superblock, group
descriptors, inodes, extent trees (and the older block maps), directory
blocks (linear and htree: an htree directory is read linearly), inline
symlinks and "fast" symlinks. It takes its bytes from one function,
`ext4_read(src, off, buf, n)`, so the source can change without touching
the parser:

| Stage | Source | What it adds |
| --- | --- | --- |
| 1 | an image that is a file (`vfs_read_at`) | `mount -t ext4 -o ro,loop`-style mount of an image on the boot disk or in tmpfs |
| 2 | a second block device | needs `blk_find` to return a list; the same importer |

Features it accepts: `extents`, `64bit`, `flex_bg`, `huge_file` (reads),
`dir_nlink`, `extra_isize`, `metadata_csum` (checksums are not verified),
`filetype`, `sparse_super`. It refuses with `EINVAL` an image that needs
`journal_needs_recovery` (a dirty journal: the image may be stale),
`inline_data`, `encrypt`, `casefold`, `bigalloc`, `meta_bg` or a block
size other than 1, 2 or 4 KiB, instead of showing wrong data.

### Limits (documented, not hidden)

- The mounted tree and its file data live in memory. Mounting an image
  needs about its used size in free memory, and at most 32768 nodes in
  all, shared with every other tmpfs. A tree with more files, or a path
  over 255 bytes, makes the mount fail with `ENOSPC` / `ENAMETOOLONG`
  and leaves nothing mounted (the importer unwinds what it made).
- Files over 1 GiB (`FILE_MAX`) are refused the same way.
- It is a snapshot: later changes to the image file are not seen, and
  nothing is written back.
- Sparse files stay sparse (holes read as zeros and take no pages).

### Why not the alternatives

- **A peer `FS_EXT` with its own handle table**: the faithful design (lazy,
  no memory copy, any size), but it is the 60-branch change from
  constraint 2, with a handle table that needs eviction while descriptors
  and mappings hold handles. It is worth doing once the importer shows the
  parser is right, because the parser (`ext4_read`, inode, extent, and
  directory readers) is reused as is. Demand-reading file data would then
  replace the copy in the importer.
- **Reusing `diskfs` for the on-disk format**: ext4 is not BSTDISK; there
  is nothing to share but the block reads.

## Proof and trust

The importer is Bastio, so memory safety is proved like the rest of the
kernel: every read from the image goes through bounds-checked `Bytes`, and
every loop over an image-controlled count (extent entries, directory
entries, depth) has a bound taken from the constants above and a
`decreases`/invariant, so a hostile image cannot make the kernel read out
of bounds or loop forever. What is *not* proved: that the data shown is
what Linux would show. That is checked by tests against `e2fsprogs` images
(`test/ext4.sh`: build a tree with `mke2fs -d`, mount, and compare a
listing and checksums with what the host made), and malformed images
(truncated, loops in the directory tree, extent depth bombs) must be
refused with an error, not crash. The trusted base does not grow.

## Steps

1. This note.
2. `src/ext4.bastio`: superblock and feature checks, inode and extent
   reader, directory walk; a `shell` command `ext4ls IMG` printing a tree,
   with its case in `test/shell_test.sh`.
3. The mount: `lx_mount` for `ext4`, the import, read-only. Tests:
   `test/compat/progs` mount and read back an image built by `mke2fs -d`
   (files, holes, long names, symlinks, hard links, 0-byte, 1 GiB sparse).
4. Fuzzed/malformed images in `tests/`.
5. Stage 2 (second device) and the peer `FS_EXT`, if wanted.
