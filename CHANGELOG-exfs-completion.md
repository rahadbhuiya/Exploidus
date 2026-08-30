# ExFS completion pass — changelog

This documents one focused pass that closed the gaps identified in an
audit of `kernel/fs/exfs/exfs.c` (950 lines) before starting on a v2.
Written so it can be lifted directly into a thesis "Implementation" /
"Changes" chapter, with each item framed as problem → design → tradeoff.

## 1. Write path only reached direct blocks (max file size 48 KB)

**Problem:** `exfs_op_read()` could resolve single-indirect blocks, but
`exfs_op_write()` had `if (file_block >= EXFS_DIRECT_BLOCKS) break;` —
so files could never grow past `12 * 4096 = 48 KiB`, and
`double_indirect` (present in the on-disk inode) was never touched by
either path.

**Fix:** `exfs_resolve_block()` — one function, shared by read and
write, that walks direct → single indirect → double indirect and (when
called with `alloc = true`) allocates missing index blocks and the
final data block on demand. Max file size is now
`(12 + 512 + 512*512) * 4096 bytes ≈ 1.03 GiB`.
`triple_indirect` exists in the on-disk inode but is **not** wired up —
documented as a known gap rather than silently broken, since nothing in
this project currently needs files anywhere near 1 GiB.

## 2. No crash consistency (no journal)

**Problem:** A power loss mid-`create()`/`unlink()`/`rmdir()`/`write()`
could leave metadata half-updated: a dirent pointing at a still-zeroed
inode, an inode with a pointer to a block whose content was never
written, etc. No recovery mechanism existed.

**Fix:** A single-slot write-ahead redo journal
(`EXFS_JOURNAL_MAGIC` in `exfs.h`). Design choices, each with a
one-line reason:

- **Metadata-only journaling** (ordered mode, like ext3's default) —
  file *data* blocks are written directly, *before* the journal
  transaction opens; only inode/dirent/indirect-pointer-block writes
  are journaled. Journaling data too would need a journal region sized
  for the largest possible write, which isn't bounded.
- **Single transaction in flight** — matches the fact that nothing in
  this kernel takes a filesystem-wide lock around a multi-block ExFS
  operation, so there's only ever one logical transaction happening at
  a time anyway.
- **Block-bitmap updates are NOT journaled** — applied immediately, same
  as before this change. Worst case after a crash is a leaked block
  (marked used, unreferenced by anything) — a benign leak recoverable
  by a future `fsck`, never directory-tree corruption.
- **Backward compatible on-disk** — `journal_block`/`journal_size` were
  carved out of the superblock's `reserved[4000]` field (same pattern
  the existing `owner_uid` carve-out in `exfs_inode_t` already used).
  A volume formatted by the old `mkexfs.py`/`exfs_format()` reads those
  bytes as zero, which this code treats as "journaling disabled for
  this volume" — it still mounts and works exactly as before, just
  without crash-atomicity.

**Known limitation:** no `fsck` exists yet to repair the "leaked block"
case above, or to recover from any corruption the journal doesn't
cover.

## 3. `block_hash` field existed but was never read or written

**Problem:** `exfs_inode_t.block_hash[8]` ("first 8 bytes of BLAKE3 of
last write", per its own comment) was declared but never touched
anywhere in `exfs.c` — dead field, despite the project's README framing
ExFS as security/provenance-first.

**Fix:** `exfs_op_write()` now computes `BLAKE3(bytes just written)`
and stores the first 8 bytes in `block_hash`. Scope is intentionally
narrow: it covers only the most recent `write()` call's byte range, not
the whole file — a real whole-file or per-block checksum would need
on-disk format changes beyond the field's existing 8 bytes, which is
out of scope for this pass.

## 4. No `rename()`

**Problem:** No way to rename or move a file/directory without a
manual copy+delete (data actually moves, and a crash mid-copy leaves a
half-written destination with the source already possibly gone).

**Fix:** `exfs_op_rename()` (+ `vfs_rename()`, `SYS_RENAME` syscall 90,
libc `rename()`). Pure directory-entry move — no data ever touched —
wrapped in one journal transaction. Refuses cross-volume renames
(EXDEV-style). Overwrites an existing file at the destination (POSIX
semantics); refuses to overwrite an existing directory. The shell's
`mv` command was switched from cp+unlink to this.

## 5. `double_indirect` blocks leaked on unlink

**Problem:** `exfs_op_unlink()` freed `direct[]` and `indirect` blocks
but never touched `double_indirect` — any file using it (previously
unreachable, since write couldn't allocate past direct+indirect
anyway) would leak its entire double-indirect subtree.

**Fix:** unlink now walks and frees the double-indirect tree (leaf data
blocks → level-1 pointer blocks → the level-1 table itself) before
zeroing the inode.

## Explicitly out of scope for this pass

Kept as honest, documented gaps rather than silently patched over:

- `fsck` / offline consistency checker
- Directory growth past 12 direct blocks (~180 entries) — directories
  don't use indirect blocks even though files now can
- `triple_indirect`
- Symlinks / hard links
- Group/other permission bits (still owner-only rwx)
- Quotas, dynamic inode-table growth past `EXFS_MAX_INODES`