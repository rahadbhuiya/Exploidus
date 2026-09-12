import struct, os, sys

BLOCK_SIZE  = 4096
EXFS_MAGIC  = 0x45584653
MAX_INODES  = 4096
INODE_SIZE  = 256

def make_inode(mode, size, direct_blocks, indirect=0, double_indirect=0):
    d = list(direct_blocks) + [0]*(12-len(direct_blocks))
    return struct.pack("<QQQ"+"II"+"Q"*12+"QQQ"+"QI"+"8s84s",
        size, 0, 0, 0, mode,
        *d,
        indirect, double_indirect, 0,  # indirect, double_indirect, triple_indirect
        0, 0,
        b'\x00'*8, b'\x00'*84)

def make_dirent(inode_num, name, ftype):
    name_b = name.encode()
    return struct.pack("<QHBBs256s",
        inode_num, len(name_b), ftype, 0,
        b'\x00', name_b + b'\x00'*(256-len(name_b)))

disk_path  = sys.argv[1]
elf_paths  = sys.argv[2:]   # all remaining args are ELF files

# Journal region: 1 header block + 16 data slots, must match
# EXFS_JOURNAL_MAX_BLOCKS in kernel/fs/exfs/exfs.h.
JOURNAL_MAX_BLOCKS = 16
JOURNAL_SIZE = 1 + JOURNAL_MAX_BLOCKS

# Bits per bitmap block -- must match BITS_PER_BLK in exfs.c
# (EXFS_BLOCK_SIZE * 8).
BITS_PER_BLK = BLOCK_SIZE * 8

disk_size    = os.path.getsize(disk_path)
total_blocks = disk_size // BLOCK_SIZE
inode_blocks = (MAX_INODES * INODE_SIZE + BLOCK_SIZE - 1) // BLOCK_SIZE
bitmap_block      = 1
# How many blocks the bitmap itself needs -- one bit per block on the
# volume. A volume up to BITS_PER_BLK (32768) blocks still fits in a
# single bitmap block (the original hardcoded layout); past that,
# more are reserved so every block has a bit to represent it. This
# mirrors exfs_format()'s bitmap_size computation in exfs.c exactly --
# see bitmap_size's comment in exfs.h for why a single fixed block was
# a real correctness bug above that size, not just a scale limit.
bitmap_size       = max(1, (total_blocks + BITS_PER_BLK - 1) // BITS_PER_BLK)
inode_table_block = bitmap_block + bitmap_size
data_start_block  = inode_table_block + inode_blocks
provenance_block  = total_blocks - 16
provenance_size   = 16
journal_block     = provenance_block - JOURNAL_SIZE
journal_size      = JOURNAL_SIZE
free_blocks       = total_blocks - data_start_block - provenance_size - journal_size

img = bytearray(disk_size)

# Superblock. Layout must match exfs_superblock_t in exfs.h exactly:
# magic, version, 9 x uint64 (total/free_blocks, total/free_inodes,
# inode_table_block, block_bitmap_block, data_start_block,
# provenance_block, provenance_size), fs_uuid[16], journal_block,
# journal_size, bitmap_size, reserved[3976] (padded to EXFS_BLOCK_SIZE).
sb = struct.pack("<II"+"Q"*9+"16s"+"QQQ"+"3976s",
    EXFS_MAGIC, 1,
    total_blocks, free_blocks,
    MAX_INODES, MAX_INODES - 1,
    inode_table_block, bitmap_block, data_start_block,
    provenance_block, provenance_size,
    b'\x45\x58\x46\x53'+b'\x00'*12,
    journal_block, journal_size, bitmap_size,
    b'\x00'*3976)
img[0:BLOCK_SIZE] = sb[:BLOCK_SIZE]

# Block allocator
used_blocks = set(range(data_start_block))
for b in range(journal_block, total_blocks):
    used_blocks.add(b)

def alloc_block():
    for b in range(data_start_block, provenance_block):
        if b not in used_blocks:
            used_blocks.add(b)
            return b
    raise Exception("disk full")

def write_block(blk, data):
    off = blk * BLOCK_SIZE
    img[off:off+BLOCK_SIZE] = bytes(data)[:BLOCK_SIZE].ljust(BLOCK_SIZE, b'\x00')

def write_inode(ino, inode_data):
    ipb = BLOCK_SIZE // INODE_SIZE
    blk = inode_table_block + ino // ipb
    off = blk * BLOCK_SIZE + (ino % ipb) * INODE_SIZE
    img[off:off+INODE_SIZE] = bytes(inode_data)[:INODE_SIZE]

def write_file_to_disk(ino, filepath):
    with open(filepath, 'rb') as f:
        data = f.read()
    blocks = []
    for i in range(0, len(data), BLOCK_SIZE):
        blk = alloc_block()
        write_block(blk, data[i:i+BLOCK_SIZE])
        blocks.append(blk)

    direct, indirect_block_num, double_indirect_block_num = \
        _place_blocks(blocks)

    inode = make_inode(0o755, len(data), direct,
                        indirect_block_num, double_indirect_block_num)
    write_inode(ino, inode)
    return len(data), len(blocks)

# Pointers per indirect block -- must match EXFS_INDIRECT_PTRS in
# kernel/fs/exfs/exfs.h (EXFS_BLOCK_SIZE / sizeof(uint64_t)).
INDIRECT_PTRS = BLOCK_SIZE // 8   # 512

def _write_ptr_block(block_nums):
    """Allocate one block and fill it with up to INDIRECT_PTRS 8-byte
    little-endian block numbers (zero-padded). Returns that block's
    number."""
    blk_num = alloc_block()
    buf = bytearray(BLOCK_SIZE)
    for idx, b in enumerate(block_nums):
        buf[idx*8:(idx+1)*8] = b.to_bytes(8, 'little')
    write_block(blk_num, buf)
    return blk_num

def _place_blocks(blocks):
    """Split an already-allocated, in-order list of data block numbers
    into (direct[], indirect_block_num, double_indirect_block_num),
    building whatever pointer blocks are needed -- mirrors
    exfs_resolve_block()'s addressing exactly (direct 12, then single
    indirect up to INDIRECT_PTRS more, then double indirect up to
    INDIRECT_PTRS*INDIRECT_PTRS more). Raises if the file is bigger
    than double-indirect can address (matches the runtime driver's own
    ceiling -- see the EXFS_INDIRECT_PTRS comment in exfs.h;
    triple_indirect is unimplemented there too).
    """
    max_addressable = 12 + INDIRECT_PTRS + INDIRECT_PTRS * INDIRECT_PTRS
    if len(blocks) > max_addressable:
        raise Exception(
            f"file needs {len(blocks)} blocks, more than this "
            f"filesystem can address ({max_addressable} via direct + "
            f"single + double indirect -- ~{max_addressable*BLOCK_SIZE//(1024*1024)} MiB)")

    direct = blocks[:12]
    rest = blocks[12:]

    indirect_block_num = 0
    double_indirect_block_num = 0

    if not rest:
        return direct, indirect_block_num, double_indirect_block_num

    single_ptrs = rest[:INDIRECT_PTRS]
    indirect_block_num = _write_ptr_block(single_ptrs)
    rest = rest[INDIRECT_PTRS:]

    if not rest:
        return direct, indirect_block_num, double_indirect_block_num

    # Double indirect: chunk the remainder into groups of
    # INDIRECT_PTRS, write each group as its own pointer block (a
    # "level 2" block), then write a "level 1" block whose entries
    # point at each level-2 block in order.
    level1_ptrs = []
    for i in range(0, len(rest), INDIRECT_PTRS):
        group = rest[i:i+INDIRECT_PTRS]
        level1_ptrs.append(_write_ptr_block(group))
    double_indirect_block_num = _write_ptr_block(level1_ptrs)

    return direct, indirect_block_num, double_indirect_block_num

# Root inode (inode 0) - directory
root_dir_block = alloc_block()
root_inode = make_inode(0o755, 0, [root_dir_block])
write_inode(0, root_inode)

# bin inode (inode 1) - /bin directory. The actual inode write
# happens further down, after all ELF files are placed (see
# bin_dir_block_nums) -- its block list depends on how many
# directory blocks /bin ends up needing.

# var/log directory inodes
var_dir_block = alloc_block()
var_inode = make_inode(0o755, 0, [var_dir_block])
write_inode(2, var_inode)

log_dir_block = alloc_block()
log_inode = make_inode(0o755, 0, [log_dir_block])
write_inode(3, log_inode)

# var/rahu directory inode — rahu package manager state (index.json etc.)
rahu_dir_block = alloc_block()
rahu_inode = make_inode(0o755, 0, [rahu_dir_block])
write_inode(4, rahu_inode)

# tmp directory inode — runtime temp files (e.g. compositor.pid)
tmp_dir_block = alloc_block()
tmp_inode = make_inode(0o777, 0, [tmp_dir_block])
write_inode(5, tmp_inode)
write_block(tmp_dir_block, bytearray(BLOCK_SIZE))  # empty dir

# bigdir inode (inode 6) — pre-created, empty. Exists purely so
# embedded test scripts (see EMBEDDED_SCRIPTS below) can write files
# into it directly without needing mkdir at runtime. mkdir itself
# works fine from the shell; this exists only because os.execute()
# (which the Lua port routes to our libc's system(), an honest stub
# returning -1 — see userspace/libc/stdio.c) is not a safe way to
# invoke mkdir from a *script*, and there's no other Lua binding for
# creating a directory.
bigdir_block = alloc_block()
bigdir_inode = make_inode(0o755, 0, [bigdir_block])
write_inode(6, bigdir_inode)
write_block(bigdir_block, bytearray(BLOCK_SIZE))  # empty dir

# Root directory — add bin/, var/, tmp/, bigdir/
root_dir_data = bytearray(BLOCK_SIZE)
# bin entry
de_bin = struct.pack("<QHBB256s", 1, 3, 1, 0, b'bin'+b'\x00'*253)
root_dir_data[0:len(de_bin)] = de_bin
# var entry
de_var = struct.pack("<QHBB256s", 2, 3, 1, 0, b'var'+b'\x00'*253)
root_dir_data[len(de_bin):len(de_bin)+len(de_var)] = de_var
# tmp entry
de_tmp = struct.pack("<QHBB256s", 5, 3, 1, 0, b'tmp'+b'\x00'*253)
root_dir_data[len(de_bin)+len(de_var):len(de_bin)+len(de_var)+len(de_tmp)] = de_tmp
# bigdir entry
de_bigdir = struct.pack("<QHBB256s", 6, 6, 1, 0, b'bigdir'+b'\x00'*250)
_off = len(de_bin)+len(de_var)+len(de_tmp)
root_dir_data[_off:_off+len(de_bigdir)] = de_bigdir
write_block(root_dir_block, root_dir_data)

# var directory — add log/ and rahu/
var_dir_data = bytearray(BLOCK_SIZE)
de_log = struct.pack("<QHBB256s", 3, 3, 1, 0, b'log'+b'\x00'*253)
var_dir_data[0:len(de_log)] = de_log
de_rahu = struct.pack("<QHBB256s", 4, 4, 1, 0, b'rahu'+b'\x00'*252)
var_dir_data[len(de_log):len(de_log)+len(de_rahu)] = de_rahu
write_block(var_dir_block, var_dir_data)

# Write ELF files into /bin/. bin_dir_blocks holds one bytearray
# per directory block, each capped at floor(BLOCK_SIZE/dirent_size)
# = 15 whole dirents (268-byte dirents don't divide 4096 evenly) --
# starting a new block rather than letting a partial dirent spill
# past the block boundary. A single fixed block silently lost any
# 16th+ entry here before this: write_block() truncates to exactly
# BLOCK_SIZE, and the kernel's own dirent scan only ever considers
# offsets where a FULL dirent fits (off + sizeof(dirent) <=
# BLOCK_SIZE), so a 16th entry crammed in past that boundary was
# neither loadable nor visible to any lookup/readdir -- its data and
# inode were still written correctly, just permanently unreachable
# (a real, confirmed-in-testing orphaned-inode bug, not hypothetical:
# adding a 16th binary here was what first exposed it).
next_ino = 7
DIRENT_SIZE = 268
DIRENTS_PER_BLOCK = BLOCK_SIZE // DIRENT_SIZE  # 15

bin_dir_blocks = [bytearray(BLOCK_SIZE)]
bin_dir_count_in_block = 0

def _bin_dir_add(de):
    global bin_dir_count_in_block
    if bin_dir_count_in_block >= DIRENTS_PER_BLOCK:
        bin_dir_blocks.append(bytearray(BLOCK_SIZE))
        bin_dir_count_in_block = 0
    off = bin_dir_count_in_block * DIRENT_SIZE
    bin_dir_blocks[-1][off:off+DIRENT_SIZE] = de
    bin_dir_count_in_block += 1

# Map of ELF filename → disk name
NAME_MAP = {
    'exploish.elf':   'exploish',
    'auditd.elf':     'auditd',
    'init.elf':       'init',
    'hello.elf':      'hello',
    'httpd.elf':      'httpd',
    'ys.elf':         'ys',
    'compositor.elf': 'compositor',
    'gui_demo.elf':   'gui_demo',
    'terminal.elf':   'terminal',
}

for elf_path in elf_paths:
    if not elf_path or not os.path.exists(elf_path):
        print(f"  skipping {elf_path} (not found)")
        continue

    basename = os.path.basename(elf_path)
    disk_name = NAME_MAP.get(basename, basename.replace('.elf', ''))

    size, nblocks = write_file_to_disk(next_ino, elf_path)

    name_b = disk_name.encode()
    de = struct.pack("<QHBB256s",
        next_ino, len(name_b), 0, 0,
        name_b + b'\x00'*(256-len(name_b)))
    _bin_dir_add(de)

    print(f"  added /bin/{disk_name} ({size} bytes, {nblocks} blocks)")
    next_ino += 1

bin_dir_block_nums = []
for blk_data in bin_dir_blocks:
    blk_num = alloc_block()
    write_block(blk_num, blk_data)
    bin_dir_block_nums.append(blk_num)

if len(bin_dir_block_nums) > 12:
    raise Exception(f"/bin needs {len(bin_dir_block_nums)} directory "
                     f"blocks (more than {len(elf_paths)} entries fit "
                     f"in 12 direct blocks at {DIRENTS_PER_BLOCK} "
                     f"entries/block) -- this offline tool only "
                     f"writes direct blocks for directories, unlike "
                     f"the runtime driver (see EXFS_MAX_DIR_BLOCKS in "
                     f"exfs.h)")

bin_inode = make_inode(0o755, 0, bin_dir_block_nums)
write_inode(1, bin_inode)

# Optional: bake an embedded test script (e.g. a directory-growth
# stress test for exercising EXFS_MAX_DIR_BLOCKS -- see
# CHANGELOG-exfs-completion.md) directly into / at build time. There's
# no way to get a new text file onto the disk image from within a
# running Exploidus shell yet (no in-shell text editor), so this is
# the practical way to seed one: define EMBEDDED_SCRIPTS below and
# rerun this tool to rebuild the image with them included.
#
# Example:
#   EMBEDDED_SCRIPTS = {
#       "bigdir_test.lua": '''
#           -- /bigdir is pre-created by this tool (see bigdir_inode
#           -- above) -- no mkdir/os.execute() needed from the script.
#           for i = 0, 250 do
#               local fh = io.open("/bigdir/f" .. i, "w")
#               fh:write("x")
#               fh:close()
#           end
#           print("done")
#       ''',
#   }
EMBEDDED_SCRIPTS = {}

if EMBEDDED_SCRIPTS:
    dirent_size = struct.calcsize("<QHBB256s")
    root_dir_offset = dirent_size * 4  # bin, var, tmp, bigdir already packed above

    for script_name, script_text in EMBEDDED_SCRIPTS.items():
        script_bytes = script_text.encode()
        blocks = []
        for i in range(0, len(script_bytes), BLOCK_SIZE):
            blk = alloc_block()
            chunk = script_bytes[i:i+BLOCK_SIZE]
            write_block(blk, chunk + b'\x00' * (BLOCK_SIZE - len(chunk)))
            blocks.append(blk)

        # _place_blocks() raises if this exceeds what direct + single
        # + double indirect can address (~1 GiB) -- same ceiling the
        # runtime C driver has (see exfs_resolve_block() in exfs.c).
        direct, indirect_blk, double_indirect_blk = _place_blocks(blocks)
        script_inode = make_inode(0o644, len(script_bytes), direct,
                                   indirect_blk, double_indirect_blk)
        write_inode(next_ino, script_inode)

        name_b = script_name.encode()
        de = struct.pack("<QHBB256s",
            next_ino, len(name_b), 0, 0,
            name_b + b'\x00'*(256-len(name_b)))
        root_dir_data[root_dir_offset:root_dir_offset+dirent_size] = de
        root_dir_offset += dirent_size

        print(f"  added /{script_name} ({len(script_bytes)} bytes)")
        next_ino += 1

    # Rewrite root_dir_block once with bin/var/tmp plus the new entries.
    write_block(root_dir_block, root_dir_data)

# Block bitmap -- write bitmap_size blocks, one bit per volume block
# (see bitmap_size's computation above). Was previously hardcoded to
# exactly one block with any bit beyond BITS_PER_BLK silently
# dropped -- correctness bug above that block count (every such block
# would read back as "free" and could be handed out while genuinely
# in use), not just a scale limit.
for bb in range(bitmap_size):
    bitmap = bytearray(BLOCK_SIZE)
    base = bb * BITS_PER_BLK
    lim = min(base + BITS_PER_BLK, total_blocks)
    for b in used_blocks:
        if base <= b < lim:
            local = b - base
            bitmap[local // 8] |= (1 << (local % 8))
    img[(bitmap_block + bb) * BLOCK_SIZE:(bitmap_block + bb + 1) * BLOCK_SIZE] = bitmap

with open(disk_path, "wb") as f:
    f.write(img)

print(f"ExFS formatted: {total_blocks} blocks, {len(used_blocks)} used")