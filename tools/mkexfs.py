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

disk_size    = os.path.getsize(disk_path)
total_blocks = disk_size // BLOCK_SIZE
inode_blocks = (MAX_INODES * INODE_SIZE + BLOCK_SIZE - 1) // BLOCK_SIZE
inode_table_block = 2
bitmap_block      = 1
data_start_block  = inode_table_block + inode_blocks
provenance_block  = total_blocks - 16
provenance_size   = 16
free_blocks       = total_blocks - data_start_block - provenance_size

img = bytearray(disk_size)

# Superblock
sb = struct.pack("<II"+"Q"*9+"16s4008s",
    EXFS_MAGIC, 1,
    total_blocks, free_blocks,
    MAX_INODES, MAX_INODES - 1,
    inode_table_block, bitmap_block, data_start_block,
    provenance_block, provenance_size,
    b'\x45\x58\x46\x53'+b'\x00'*12,
    b'\x00'*4008)
img[0:BLOCK_SIZE] = sb[:BLOCK_SIZE]

# Block allocator
used_blocks = set(range(data_start_block))
for b in range(provenance_block, total_blocks):
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

# Root directory — add bin/ and var/ entries
root_dir_data = bytearray(BLOCK_SIZE)
# bin entry
de_bin = struct.pack("<QHBB256s", 1, 3, 1, 0, b'bin'+b'\x00'*253)
root_dir_data[0:len(de_bin)] = de_bin
# var entry
de_var = struct.pack("<QHBB256s", 2, 3, 1, 0, b'var'+b'\x00'*253)
root_dir_data[len(de_bin):len(de_bin)+len(de_var)] = de_var
write_block(root_dir_block, root_dir_data)

# var directory — add log/
var_dir_data = bytearray(BLOCK_SIZE)
de_log = struct.pack("<QHBB256s", 3, 3, 1, 0, b'log'+b'\x00'*253)
var_dir_data[0:len(de_log)] = de_log
write_block(var_dir_block, var_dir_data)

# Write ELF files into /bin/
next_ino = 4
bin_dir_data = bytearray(BLOCK_SIZE)
bin_dir_offset = 0

# Map of ELF filename → disk name
NAME_MAP = {
    'exploish.elf':  'exploish',
    'auditd.elf':    'auditd',
    'init.elf':      'init',
    'hello.elf':     'hello',
    'httpd.elf':     'httpd',
    'ys.elf':        'ys',

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

# Block bitmap
bitmap = bytearray(BLOCK_SIZE)
for b in used_blocks:
    if b < BLOCK_SIZE * 8:
        bitmap[b // 8] |= (1 << (b % 8))
img[BLOCK_SIZE:BLOCK_SIZE*2] = bitmap

with open(disk_path, "wb") as f:
    f.write(img)

print(f"ExFS formatted: {total_blocks} blocks, {len(used_blocks)} used")