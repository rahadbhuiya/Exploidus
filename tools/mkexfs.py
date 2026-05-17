import struct, os, sys

BLOCK_SIZE  = 4096
EXFS_MAGIC  = 0x45584653
MAX_INODES  = 4096
INODE_SIZE  = 256

def make_inode(mode, size, direct_blocks):
    d = list(direct_blocks) + [0]*(12-len(direct_blocks))
    # struct: size(Q) created_at(Q) modified_at(Q) creator_pid(I) mode(I) direct[12](Q) indirect(Q) double(Q) triple(Q) prov_head(Q) prov_count(I) block_hash(8s) reserved(16s)
    return struct.pack("<QQQ"+"II"+"Q"*12+"QQQ"+"QI"+"8s84s",
        size, 0, 0, 0, mode,
        *d, 0, 0, 0, 0, 0,
        b'\x00'*8, b'\x00'*84)

def make_dirent(inode_num, name, ftype):
    name_b = name.encode()
    return struct.pack("<QHBBs256s",
        inode_num, len(name_b), ftype, 0,
        b'\x00', name_b + b'\x00'*(256-len(name_b)))

disk_path = sys.argv[1]
hello_path = sys.argv[2] if len(sys.argv) > 2 else None

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

# Root inode (inode 0) - directory
root_dir_block = alloc_block()
root_inode = make_inode(0o755, 0, [root_dir_block])
write_inode(0, root_inode)

# Root directory block (initially empty)
dir_block_data = bytearray(BLOCK_SIZE)

next_ino = 1

# Write hello.elf if provided
if hello_path and os.path.exists(hello_path):
    with open(hello_path, 'rb') as f:
        hello_data = f.read()

    # Allocate blocks for hello
    hello_blocks = []
    for i in range(0, len(hello_data), BLOCK_SIZE):
        blk = alloc_block()
        chunk = hello_data[i:i+BLOCK_SIZE]
        write_block(blk, chunk)
        hello_blocks.append(blk)

    # Hello inode (inode 1)
    hello_inode = make_inode(0o755, len(hello_data), hello_blocks[:12])
    write_inode(next_ino, hello_inode)

    # Add dirent to root
    de = struct.pack("<QHBB256s",
        next_ino, 5, 0, 0,
        b'hello'+b'\x00'*251)
    dir_block_data[0:len(de)] = de
    next_ino += 1
    print(f"  added /hello ({len(hello_data)} bytes, {len(hello_blocks)} blocks)")

write_block(root_dir_block, dir_block_data)

# Block bitmap
bitmap = bytearray(BLOCK_SIZE)
for b in used_blocks:
    if b < BLOCK_SIZE * 8:
        bitmap[b // 8] |= (1 << (b % 8))
img[BLOCK_SIZE:BLOCK_SIZE*2] = bitmap

with open(disk_path, "wb") as f:
    f.write(img)

print(f"ExFS formatted: {total_blocks} blocks, {len(used_blocks)} used")
