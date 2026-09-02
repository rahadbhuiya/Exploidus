import struct, os, sys

BLOCK_SIZE  = 4096
EXFS_MAGIC  = 0x45584653
MAX_INODES  = 4096
INODE_SIZE  = 256

def make_inode(mode, size, direct_blocks, indirect=0):
    d = list(direct_blocks) + [0]*(12-len(direct_blocks))
    return struct.pack("<QQQ"+"II"+"Q"*12+"QQQ"+"QI"+"8s84s",
        size, 0, 0, 0, mode,
        *d,
        indirect,  # indirect block pointer
        0, 0, 0, 0,
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

disk_size    = os.path.getsize(disk_path)
total_blocks = disk_size // BLOCK_SIZE
inode_blocks = (MAX_INODES * INODE_SIZE + BLOCK_SIZE - 1) // BLOCK_SIZE
inode_table_block = 2
bitmap_block      = 1
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
# journal_size, reserved[3984] (padded to EXFS_BLOCK_SIZE).
sb = struct.pack("<II"+"Q"*9+"16s"+"QQ"+"3984s",
    EXFS_MAGIC, 1,
    total_blocks, free_blocks,
    MAX_INODES, MAX_INODES - 1,
    inode_table_block, bitmap_block, data_start_block,
    provenance_block, provenance_size,
    b'\x45\x58\x46\x53'+b'\x00'*12,
    journal_block, journal_size,
    b'\x00'*3984)
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
    
    direct = blocks[:12]
    indirect_blocks = blocks[12:]
    
    # indirect block
    indirect_block_num = 0
    if indirect_blocks:
        indirect_block_num = alloc_block()
        # write block numbers into indirect block
        indirect_data = bytearray(BLOCK_SIZE)
        for idx, blk in enumerate(indirect_blocks):
            indirect_data[idx*8:(idx+1)*8] = blk.to_bytes(8, 'little')
        write_block(indirect_block_num, indirect_data)
    
    inode = make_inode(0o755, len(data), direct, indirect_block_num)
    write_inode(ino, inode)
    return len(data), len(blocks)

# Root inode (inode 0) - directory
root_dir_block = alloc_block()
root_inode = make_inode(0o755, 0, [root_dir_block])
write_inode(0, root_inode)

# bin inode (inode 1) - /bin directory
bin_dir_block = alloc_block()
bin_inode = make_inode(0o755, 0, [bin_dir_block])
write_inode(1, bin_inode)

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

# Write ELF files into /bin/
next_ino = 7
bin_dir_data = bytearray(BLOCK_SIZE)
bin_dir_offset = 0

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
    bin_dir_data[bin_dir_offset:bin_dir_offset+len(de)] = de
    bin_dir_offset += len(de)

    print(f"  added /bin/{disk_name} ({size} bytes, {nblocks} blocks)")
    next_ino += 1

write_block(bin_dir_block, bin_dir_data)

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
        if len(blocks) > 12:
            # This offline tool only writes direct blocks (unlike the
            # runtime C driver, which now supports indirect/double-
            # indirect -- see exfs_resolve_block() in exfs.c). Fine for
            # a small test script; a >48 KiB embedded file would need
            # the same indirect-block-writing logic
            # write_file_to_disk() has, not yet ported here.
            raise Exception(f"{script_name}: embedded script > 48 KiB, "
                             f"this tool doesn't write indirect blocks yet")

        script_inode = make_inode(0o644, len(script_bytes), blocks)
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

# Block bitmap
bitmap = bytearray(BLOCK_SIZE)
for b in used_blocks:
    if b < BLOCK_SIZE * 8:
        bitmap[b // 8] |= (1 << (b % 8))
img[BLOCK_SIZE:BLOCK_SIZE*2] = bitmap

with open(disk_path, "wb") as f:
    f.write(img)

print(f"ExFS formatted: {total_blocks} blocks, {len(used_blocks)} used")