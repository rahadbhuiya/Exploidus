#!/usr/bin/env python3
"""
fsck.py -- offline consistency checker (and limited repairer) for an
ExFS disk image.

This mirrors tools/mkexfs.py's approach: a standalone host-side Python
tool that reads/writes the raw image directly, rather than an in-kernel
repair pass. All struct layouts below MUST match kernel/fs/exfs/exfs.h
exactly -- if that file's on-disk structs change, update the format
strings here too (search for "FMT" below).

What it checks:
  1. Superblock sanity (magic, version, region ordering).
  2. Every in-use inode's blocks (direct + single-indirect +
     double-indirect; triple_indirect is unimplemented on this
     filesystem, same as the kernel driver -- see EXFS_INDIRECT_PTRS
     in exfs.h) are resolved and cross-checked against the block
     bitmap:
       - a block marked used in the bitmap but referenced by nothing
         and not part of the reserved metadata region -> LEAKED
         (the kind of thing EXFS_JOURNAL_MAGIC's design doc says can
         happen after a crash mid-write -- see exfs.h/exfs.c) --
         safe to reclaim, and the only thing --fix touches.
       - a block referenced by an inode but NOT marked used in the
         bitmap -> CORRUPTION (something else could overwrite live
         data) -- reported only, not auto-fixed.
       - a block referenced by more than one inode -> CORRUPTION
         (cross-linked file) -- reported only.
  3. The directory tree, walked from root (inode 0): every dirent's
     inode_num must point at an in-use inode (else DANGLING dirent),
     and every in-use inode except root must be reachable from root
     (else ORPHANED inode) -- reported only, no lost+found exists
     to safely reattach an orphan to.
  4. Duplicate names within one directory (the kernel guards against
     creating these now, but a volume formatted/written before that
     fix could already have one).
  5. The metadata journal header: if it shows an armed/committed
     transaction, that means either an unclean shutdown whose replay
     hasn't run yet (boot the kernel once first -- exfs_mount()
     replays it automatically) or, if this fsck is itself being run
     right after a crash instead of booting, that's expected and not
     an error -- just a heads-up.

Usage:
    python3 fsck.py <disk_image> [--fix]

Without --fix: read-only, reports findings, exits 1 if anything is
wrong (0 if clean) -- suitable for scripting/CI.
With --fix: additionally clears leaked-block bitmap bits (the one
unambiguously safe repair -- it only ever gives blocks back to the
free pool, never removes or reattaches anything reachable). Nothing
else is auto-repaired; orphaned inodes and dangling dirents are
reported for a human to look at, matching how conservative real fsck
tools are by default about anything that could destroy data.
"""
import struct, sys

BLOCK_SIZE   = 4096
EXFS_MAGIC   = 0x45584653
INODE_SIZE   = 256
NAME_MAX     = 255
DIRECT_BLOCKS = 12
INDIRECT_PTRS = BLOCK_SIZE // 8   # 512
JOURNAL_MAGIC = 0x45584a4c

# --- Struct formats -- MUST match kernel/fs/exfs/exfs.h exactly ---
# Superblock: magic,version,9xQ,fs_uuid[16],journal_block,journal_size,
# bitmap_size,reserved[3976]
SB_FMT = "<II" + "Q"*9 + "16s" + "QQQ" + "3976s"
SB_SIZE = struct.calcsize(SB_FMT)
assert SB_SIZE == BLOCK_SIZE, f"superblock format is {SB_SIZE} bytes, expected {BLOCK_SIZE}"

# Inode (256-byte on-disk stride; sizeof(exfs_inode_t) itself is less
# than that -- see the write_inode() comment in exfs.c -- so this
# format's tail includes the unused slack the C struct doesn't
# explicitly name):
#   size,created_at,modified_at (3xQ), creator_pid,mode (2xI),
#   direct[12] (12xQ), indirect,double_indirect,triple_indirect (3xQ),
#   prov_head,prov_count (Q,I), block_hash (8s), owner_uid (I),
#   then whatever's left over to reach the 256-byte stride.
INODE_FMT = "<QQQII" + "Q"*12 + "QQQ" + "QI" + "8s" + "I" + "80s"
INODE_FMT_SIZE = struct.calcsize(INODE_FMT)
assert INODE_FMT_SIZE == INODE_SIZE, f"inode format is {INODE_FMT_SIZE} bytes, expected {INODE_SIZE}"

# Dirent: inode_num(Q), name_len(H), file_type(B), _pad(B), name[256]
DIRENT_FMT = "<QHBB256s"
DIRENT_SIZE = struct.calcsize(DIRENT_FMT)
assert DIRENT_SIZE == 268

JOURNAL_HDR_FMT = "<II" + "Q"*16   # magic, block_count, block_idx[16]


class Superblock:
    def __init__(self, raw):
        (self.magic, self.version, self.total_blocks, self.free_blocks,
         self.total_inodes, self.free_inodes, self.inode_table_block,
         self.block_bitmap_block, self.data_start_block,
         self.provenance_block, self.provenance_size, self.fs_uuid,
         self.journal_block, self.journal_size, self.bitmap_size,
         _reserved) = struct.unpack(SB_FMT, raw)


class Inode:
    def __init__(self, raw):
        vals = struct.unpack(INODE_FMT, raw)
        self.size = vals[0]
        self.created_at = vals[1]
        self.modified_at = vals[2]
        self.creator_pid = vals[3]
        self.mode = vals[4]
        self.direct = list(vals[5:17])
        self.indirect = vals[17]
        self.double_indirect = vals[18]
        self.triple_indirect = vals[19]
        self.prov_head = vals[20]
        self.prov_count = vals[21]
        self.block_hash = vals[22]
        self.owner_uid = vals[23]

    @property
    def in_use(self):
        return self.mode != 0 or self.size != 0


def main():
    if len(sys.argv) < 2:
        print("Usage: fsck.py <disk_image> [--fix]")
        sys.exit(2)
    disk_path = sys.argv[1]
    do_fix = "--fix" in sys.argv[2:]

    with open(disk_path, "rb") as f:
        img = bytearray(f.read())

    def read_block(idx):
        off = idx * BLOCK_SIZE
        return bytes(img[off:off + BLOCK_SIZE])

    def write_block(idx, data):
        assert len(data) == BLOCK_SIZE
        off = idx * BLOCK_SIZE
        img[off:off + BLOCK_SIZE] = data

    errors = []
    warnings = []
    fixed = []

    # --- 1. Superblock ---
    sb = Superblock(read_block(0))
    if sb.magic != EXFS_MAGIC:
        print(f"FATAL: bad superblock magic 0x{sb.magic:08x}, "
              f"expected 0x{EXFS_MAGIC:08x} -- not an ExFS image (or "
              f"wrong offset/partition)")
        sys.exit(2)
    print(f"Superblock OK: {sb.total_blocks} blocks total, "
          f"{sb.free_blocks} free (per superblock), "
          f"{sb.total_inodes} inodes, journal_size={sb.journal_size} "
          f"({'enabled' if sb.journal_size else 'disabled/legacy volume'})")

    if not (sb.data_start_block < sb.provenance_block <= sb.total_blocks):
        errors.append("superblock region ordering is inconsistent "
                       "(data_start_block / provenance_block / total_blocks)")
    if sb.journal_size and not (sb.journal_block + sb.journal_size <= sb.provenance_block):
        errors.append("journal region overlaps the provenance region")

    # --- 2. Journal header ---
    if sb.journal_size:
        jhdr_raw = read_block(sb.journal_block)
        jmagic = struct.unpack("<I", jhdr_raw[:4])[0]
        if jmagic == JOURNAL_MAGIC:
            warnings.append(
                "journal has a committed transaction pending replay -- "
                "this is expected right after a crash and is NOT a "
                "problem by itself (boot the kernel once and it will "
                "replay automatically -- see exfs_journal_replay() in "
                "exfs.c). If you're running fsck --fix on an image you "
                "don't intend to boot again first, note that this tool "
                "does NOT replay the journal for you.")

    # --- 3. Block bitmap (may span multiple blocks -- see bitmap_size
    # in exfs.h; older images without it default to 1 block) ---
    bmap_size = sb.bitmap_size if sb.bitmap_size else 1
    bitmap = bytearray()
    for i in range(bmap_size):
        bitmap += bytearray(read_block(sb.block_bitmap_block + i))
    def bit_get(b):
        return (bitmap[b // 8] >> (b % 8)) & 1
    def bit_clear(b):
        bitmap[b // 8] &= ~(1 << (b % 8)) & 0xFF

    # --- 4. Walk every inode, resolve its blocks ---
    inodes_per_block = BLOCK_SIZE // INODE_SIZE
    referenced = {}   # block_idx -> [inode_num, ...] (len>1 == cross-linked)
    in_use_inodes = {}  # inode_num -> Inode

    def resolve_indirect(ptr_block, out_list, ino_for_report):
        if not ptr_block:
            return
        if ptr_block >= sb.total_blocks:
            errors.append(f"inode {ino_for_report}: indirect pointer "
                           f"block {ptr_block} is out of range")
            return
        data = read_block(ptr_block)
        ptrs = struct.unpack(f"<{INDIRECT_PTRS}Q", data)
        for p in ptrs:
            if p:
                out_list.append(p)

    for b in range(sb.total_inodes // inodes_per_block):
        blk = read_block(sb.inode_table_block + b)
        for i in range(inodes_per_block):
            ino_num = b * inodes_per_block + i
            in_raw = blk[i * INODE_SIZE:(i + 1) * INODE_SIZE]
            inode = Inode(in_raw)
            if ino_num != 0 and not inode.in_use:
                continue
            in_use_inodes[ino_num] = inode

            blocks = [d for d in inode.direct if d]
            resolve_indirect(inode.indirect, blocks, ino_num)
            if inode.double_indirect:
                if inode.double_indirect >= sb.total_blocks:
                    errors.append(f"inode {ino_num}: double_indirect "
                                   f"block {inode.double_indirect} out of range")
                else:
                    l1_data = read_block(inode.double_indirect)
                    l1ptrs = struct.unpack(f"<{INDIRECT_PTRS}Q", l1_data)
                    blocks.append(inode.double_indirect)
                    for l1 in l1ptrs:
                        if l1:
                            blocks.append(l1)
                            resolve_indirect(l1, blocks, ino_num)
            if inode.indirect:
                blocks.append(inode.indirect)

            for blk_idx in blocks:
                if blk_idx == 0 or blk_idx >= sb.total_blocks:
                    errors.append(f"inode {ino_num}: block pointer "
                                   f"{blk_idx} is out of range")
                    continue
                referenced.setdefault(blk_idx, []).append(ino_num)

    for blk_idx, owners in referenced.items():
        if len(owners) > 1:
            errors.append(f"block {blk_idx} is referenced by multiple "
                           f"inodes (cross-linked): {owners}")

    # --- 5. Reserved metadata region ---
    reserved_blocks = set(range(0, sb.data_start_block))
    if sb.journal_size:
        reserved_blocks |= set(range(sb.journal_block,
                                      sb.journal_block + sb.journal_size))
    reserved_blocks |= set(range(sb.provenance_block, sb.total_blocks))

    # --- 6. Cross-check bitmap vs. reality ---
    leaked = []
    unmarked_used = []
    for b in range(sb.total_blocks):
        used_bit = bit_get(b)
        is_referenced = b in referenced
        is_reserved = b in reserved_blocks
        if is_reserved:
            if not used_bit:
                errors.append(f"metadata block {b} is not marked used "
                               f"in the bitmap")
        elif is_referenced:
            if not used_bit:
                unmarked_used.append(b)
        else:
            if used_bit:
                leaked.append(b)

    if unmarked_used:
        errors.append(f"{len(unmarked_used)} block(s) are referenced by "
                       f"an inode but NOT marked used in the bitmap "
                       f"(at risk of being overwritten): "
                       f"{unmarked_used[:10]}{'...' if len(unmarked_used) > 10 else ''}")
    if leaked:
        msg = (f"{len(leaked)} leaked block(s) (marked used, referenced "
               f"by nothing): "
               f"{leaked[:10]}{'...' if len(leaked) > 10 else ''}")
        if do_fix:
            for b in leaked:
                bit_clear(b)
            fixed.append(msg + " -- cleared")
        else:
            warnings.append(msg + " -- rerun with --fix to reclaim")

    # --- 7. Directory tree walk from root ---
    reachable = set()
    dangling_dirents = []
    duplicate_names = []

    def walk_dir(ino_num, path, depth=0):
        if depth > 64:
            errors.append(f"directory tree too deep at {path} "
                           f"(possible cycle)")
            return
        inode = in_use_inodes.get(ino_num)
        if inode is None:
            return
        reachable.add(ino_num)
        blocks = [d for d in inode.direct if d]
        resolve_indirect(inode.indirect, blocks, ino_num)
        if inode.double_indirect and inode.double_indirect < sb.total_blocks:
            l1_data = read_block(inode.double_indirect)
            l1ptrs = struct.unpack(f"<{INDIRECT_PTRS}Q", l1_data)
            for l1 in l1ptrs:
                if l1:
                    resolve_indirect(l1, blocks, ino_num)

        seen_names = {}
        for blk_idx in blocks:
            if blk_idx == 0 or blk_idx >= sb.total_blocks:
                continue
            data = read_block(blk_idx)
            for off in range(0, BLOCK_SIZE - DIRENT_SIZE + 1, DIRENT_SIZE):
                child_ino, name_len, ftype, _pad, name_raw = struct.unpack(
                    DIRENT_FMT, data[off:off + DIRENT_SIZE])
                if child_ino == 0:
                    continue
                name = name_raw[:name_len].decode(errors="replace")
                if name in seen_names:
                    duplicate_names.append(f"{path}: '{name}' appears "
                                            f"more than once")
                seen_names[name] = True

                if child_ino not in in_use_inodes:
                    dangling_dirents.append(
                        f"{path}/{name}: points at inode {child_ino}, "
                        f"which is not in use")
                    continue
                if ftype == 1:  # directory
                    walk_dir(child_ino, f"{path}/{name}".replace("//", "/"),
                             depth + 1)
                else:
                    reachable.add(child_ino)

    walk_dir(0, "")

    orphaned = [i for i in in_use_inodes if i != 0 and i not in reachable]

    if dangling_dirents:
        errors.extend(dangling_dirents)
    if duplicate_names:
        errors.extend(duplicate_names)
    if orphaned:
        warnings.append(f"{len(orphaned)} orphaned inode(s) (in use, "
                         f"unreachable from root, no lost+found to "
                         f"reattach them to): {orphaned[:10]}"
                         f"{'...' if len(orphaned) > 10 else ''}")

    # --- Report ---
    print()
    print(f"Inodes in use: {len(in_use_inodes)} / {sb.total_inodes}")
    print(f"Blocks referenced by inodes: {len(referenced)}")
    print()

    if fixed:
        print("Fixed:")
        for m in fixed:
            print(f"  - {m}")
        print()
    if warnings:
        print("Warnings:")
        for w in warnings:
            print(f"  - {w}")
        print()
    if errors:
        print("ERRORS:")
        for e in errors:
            print(f"  - {e}")
        print()

    if not errors and not warnings:
        print("Filesystem is clean.")
    elif not errors:
        print("No errors found (see warnings above).")
    else:
        print(f"{len(errors)} error(s) found.")

    if do_fix and fixed:
        with open(disk_path, "r+b") as f:
            f.seek(sb.block_bitmap_block * BLOCK_SIZE)
            f.write(bytes(bitmap))
        print(f"\nWrote repaired bitmap back to {disk_path}.")

    sys.exit(1 if errors else 0)


if __name__ == "__main__":
    main()