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

**Known limitation at the time:** no `fsck` existed yet to repair the
"leaked block" case above. `tools/fsck.py`, added in a later pass (see
below), covers exactly this.

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

## 6. Bugs found during live QEMU testing (not caught by code review)

The above was reviewed carefully but never actually run before this
point — the following were caught only once real commands were typed
into a booted kernel, and are exactly the kind of bug that code review
alone tends to miss.

### 6a. `exfs_op_create()` allowed duplicate directory entries

**Problem:** no check for an existing name before creating one. Two
callers creating the same path (e.g. a caller falling back to
`fs_create()` after an unrelated failed `open()` — which is in fact
what the shell's `write` command does) could silently leave **two**
dirents with the identical name, each pointing at a different inode.
Surfaced as `ls` showing a file listed twice.

**Fix:** `exfs_op_create()` now scans the target directory for the
name first and refuses (`NULL`, EEXIST-style) if it already exists.

### 6b. A same-block journal write could silently discard an earlier one in the same transaction

Two separate manifestations of the same root cause, both found by
running real commands, not by inspection:

- **`rename()`**: staged the source-removal edit and the destination-
  insertion edit as two separate writes. When source and destination
  land in the same physical block — always true for a same-directory
  rename, which is the common case — the journal's same-block dedup
  rule ("staged again → overwrite in place") kept only whichever edit
  was staged *second*, discarding the first. Observed as `mv` leaving
  **both** the old and new name present after the move.

- **`exfs_write_inode()`**: always read the inode-table block fresh
  from disk, never checking whether the current transaction had
  already staged an update to that same block. Since
  `EXFS_INODE_SIZE=256` and `EXFS_BLOCK_SIZE=4096`, 16 inodes share
  one on-disk block — so creating the *first* file in a freshly made
  subdirectory (which needs to allocate that directory's first data
  block, and therefore writes both the new file's own inode *and* the
  parent directory's updated inode in one transaction) could have the
  second `exfs_write_inode()` call silently erase the first one's
  staged content. The new file's own inode came out of the
  transaction blank (`mode=0`), so every subsequent `open()` on it
  failed permission checks — observed as `write` and `cat` failing
  with "not found" on a file `touch` had just reported creating.

**Fix (general, not per-callsite):** `exfs_txn_read_block()` — any
read-modify-write against a block during an active transaction now
checks the transaction's own staged (not-yet-committed) content
first, falling back to disk only if this transaction hasn't touched
that block yet. `exfs_write_inode()` and `exfs_op_create()`'s
directory-block scan were switched to use it; `rename()`'s
same-block case was additionally hand-verified to apply both its
edits to one in-memory buffer before a single write, rather than
relying solely on the general fix.

### 6c. `mv <src> <dir>/` (trailing slash) silently failed

**Problem:** the shell-level convenience of "destination ending in
`/` means keep the source's own name inside that directory" was
implemented, but checked the *already-normalized* destination path —
and the path-normalization helper (`_abs()`) always strips a trailing
slash while resolving `.`/`..`, so the check could never fire. `mv
/dir1/f /dir2/` was silently renaming `/dir1/f` to a file literally
named `dir2` inside root, which then failed because `/dir2` already
existed as a directory.

**Fix:** check the raw, pre-`_abs()` argument for a trailing slash
instead.

## Verification: deterministic crash-injection testing

Code review and interactive testing caught the bugs above, but neither
proves the journal actually survives a real crash. A `crashtest
<1|2|3>` shell command (`SYS_DEBUG_EXFS_CRASH`, test-only,
`exfs_debug_arm_crash()` in `exfs.c`) halts the kernel via an
immediate triple fault at one of the three points inside
`exfs_journal_commit()` that matter for its correctness argument:

1. after the journal's data slots are written, before the header
   (commit point) — expect no replay, operation absent after reboot
2. after the commit point, before the real blocks are applied — this
   is the case the journal exists for; expect replay, operation
   present
3. after the real blocks are applied, before the journal is cleared —
   expect a safe (idempotent) replay if it runs at all, operation
   present, no corruption

All three were run against the built kernel in QEMU and matched their
expected outcome exactly, including the `"[ExFS] Journal: replaying
committed transaction after an unclean shutdown"` boot message
appearing only for cases 2 and 3.

## Directory growth past the 12-block cap

Directories used to be hard-capped at 12 direct blocks (~180 entries)
even after files gained indirect/double-indirect addressing. Every
directory-block-scanning function (`lookup`, `readdir`, `create`,
`unlink`, `rmdir`, the empty-check, `rename`) now walks through
`EXFS_MAX_DIR_BLOCKS` via `exfs_resolve_block()` — the same helper
already crash-tested for file writes — so directories grow exactly
like files do. Also fixed in the process: `rmdir` deleting a child
directory only ever freed its `direct[]` and `indirect` blocks, with
a comment claiming directories never populate `double_indirect` —
no longer true, so it now frees that subtree too, the same way
`unlink` already did for files. Verified with a native `mkmanyfiles`
shell test command: 250 files in one directory (well past the old
cap) all created and individually resolvable via `cat`.

## `tools/fsck.py` — offline consistency checker and repairer

A standalone host-side tool (same pattern as `tools/mkexfs.py`: reads/
writes the raw disk image directly, no kernel involved) that:

- Cross-checks every in-use inode's resolved blocks (direct +
  single-indirect + double-indirect) against the block bitmap, in
  both directions: a block marked used but referenced by nothing is
  **leaked** (safe to reclaim); a block referenced by an inode but
  *not* marked used is **corruption** (something else could overwrite
  it); a block referenced by two different inodes is a **cross-link**.
- Walks the directory tree from root, flagging **dangling dirents**
  (point at an inode that isn't in use) and **duplicate names** within
  one directory (the kernel now guards against creating these, but an
  older volume could already have one).
- Flags **orphaned inodes** — in use, but unreachable from root.
- Warns (doesn't error) if the journal shows a pending replay, since
  that's expected right after a crash, not damage.

`--fix` only ever clears leaked-block bitmap bits — the one repair
that's unambiguously safe (it can only return space to the free pool,
never remove or reattach anything reachable). Everything else is
reported for a human to look at, not auto-repaired; there's no
`lost+found` to safely reattach an orphan or dangling reference to.

Validated against five synthetic corruption cases built by hand-
patching a freshly formatted image (leaked block, dangling dirent,
orphaned inode, cross-linked block, referenced-but-unmarked block) —
each was correctly detected, `--fix` correctly cleared the leaked-
block case and left a clean re-run, and exit codes matched (0 clean,
1 errors present) for use in scripts/CI.

## Explicitly out of scope for this pass

Kept as honest, documented gaps rather than silently patched over:

- `triple_indirect`
- Symlinks / hard links
- Group/other permission bits (still owner-only rwx)
- Quotas, dynamic inode-table growth past `EXFS_MAX_INODES`