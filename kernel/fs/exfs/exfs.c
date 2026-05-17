#include "exfs.h"
#include "../../drivers/ata.h"
#include "../../drivers/serial.h"
#include "../../mm/kmalloc.h"
#include "../../audit/audit.h"
#include <string.h>


/*  Low-level block I/O                                                 */


static bool exfs_read_block(exfs_volume_t *vol, uint64_t block_idx,
                             void *buf)
{
    uint32_t base_lba = vol->dev_lba_base + (uint32_t)(block_idx * 8);
    uint8_t *dst      = (uint8_t *)buf;
    for (int s = 0; s < 8; s++) {
        if (!ata_read_sector(base_lba + (uint32_t)s, dst)) {
            serial_print("[ExFS] read_block failed at LBA ");
            serial_printhex((uint64_t)(base_lba + s));
            serial_print("\n");
            return false;
        }
        dst += ATA_SECTOR_SIZE;
    }
    return true;
}

static bool exfs_write_block(exfs_volume_t *vol, uint64_t block_idx,
                              const void *buf)
{
    uint32_t       base_lba = vol->dev_lba_base + (uint32_t)(block_idx * 8);
    const uint8_t *src      = (const uint8_t *)buf;
    for (int s = 0; s < 8; s++) {
        if (!ata_write_sector(base_lba + (uint32_t)s, src)) {
            serial_print("[ExFS] write_block failed at LBA ");
            serial_printhex((uint64_t)(base_lba + s));
            serial_print("\n");
            return false;
        }
        src += ATA_SECTOR_SIZE;
    }
    return true;
}


/*  In-memory block bitmap helpers                                      */
/*                                                                       */
/*  vol->block_bitmap is a kmalloc'd byte array. One bit per block     */
/*  (1 = used, 0 = free).  The volume superblock tracks free_blocks.   */
/*  On mount the bitmap is loaded from disk. On alloc/free it is       */
/*  updated in memory and written back to the single bitmap block.      */


/* Bits per bitmap byte and per bitmap disk-block */
#define BITS_PER_BYTE       8
#define BITS_PER_BLK        (EXFS_BLOCK_SIZE * BITS_PER_BYTE)

static inline bool bitmap_get(exfs_volume_t *vol, uint64_t blk)
{
    uint64_t byte = blk / BITS_PER_BYTE;
    uint8_t  bit  = (uint8_t)(1u << (blk % BITS_PER_BYTE));
    return (vol->block_bitmap[byte] & bit) != 0;
}

static inline void bitmap_set(exfs_volume_t *vol, uint64_t blk)
{
    uint64_t byte = blk / BITS_PER_BYTE;
    uint8_t  bit  = (uint8_t)(1u << (blk % BITS_PER_BYTE));
    vol->block_bitmap[byte] |= bit;
}

static inline void bitmap_clear(exfs_volume_t *vol, uint64_t blk)
{
    uint64_t byte = blk / BITS_PER_BYTE;
    uint8_t  bit  = (uint8_t)(1u << (blk % BITS_PER_BYTE));
    vol->block_bitmap[byte] &= (uint8_t)~bit;
}

/*
 * flush_bitmap — write the in-memory bitmap back to disk.
 * Only the first bitmap block is used for now (covers 32 768 blocks).
 */
static bool flush_bitmap(exfs_volume_t *vol)
{
    return exfs_write_block(vol, vol->sb.block_bitmap_block,
                            vol->block_bitmap);
}

/*
 * exfs_alloc_block — find a free data block, mark it used, return its index.
 * Returns 0 on failure (0 is the superblock — never a valid data block).
 */
static uint64_t exfs_alloc_block(exfs_volume_t *vol)
{
    uint64_t start = vol->sb.data_start_block;
    uint64_t total = vol->sb.total_blocks;

    for (uint64_t b = start; b < total; b++) {
        if (!bitmap_get(vol, b)) {
            bitmap_set(vol, b);
            vol->sb.free_blocks--;
            flush_bitmap(vol);
            return b;
        }
    }
    serial_print("[ExFS] alloc_block: volume full\n");
    return 0;
}

/*
 * exfs_free_block — mark a data block as free.
 */
static void __attribute__((unused)) exfs_free_block(exfs_volume_t *vol, uint64_t blk)
{
    if (blk < vol->sb.data_start_block || blk >= vol->sb.total_blocks) return;
    if (!bitmap_get(vol, blk)) return;  /* already free — ignore */
    bitmap_clear(vol, blk);
    vol->sb.free_blocks++;
    flush_bitmap(vol);
}


/*  Inode read / write */


static bool exfs_read_inode(exfs_volume_t *vol, uint64_t inode_num,
                             exfs_inode_t *out)
{
    /* Inode table: multiple inodes per block */
    uint64_t inodes_per_block = EXFS_BLOCK_SIZE / EXFS_INODE_SIZE;
    uint64_t block_idx = vol->sb.inode_table_block
                       + inode_num / inodes_per_block;
    uint64_t offset    = (inode_num % inodes_per_block) * EXFS_INODE_SIZE;

    uint8_t block_buf[EXFS_BLOCK_SIZE];
    if (!exfs_read_block(vol, block_idx, block_buf)) return false;

    memcpy(out, block_buf + offset, sizeof(exfs_inode_t));
    return true;
}

static bool exfs_write_inode(exfs_volume_t *vol, uint64_t inode_num,
                              const exfs_inode_t *in)
{
    uint64_t inodes_per_block = EXFS_BLOCK_SIZE / EXFS_INODE_SIZE;
    uint64_t block_idx = vol->sb.inode_table_block
                       + inode_num / inodes_per_block;
    uint64_t offset    = (inode_num % inodes_per_block) * EXFS_INODE_SIZE;

    uint8_t block_buf[EXFS_BLOCK_SIZE];
    if (!exfs_read_block(vol, block_idx, block_buf)) return false;

    memcpy(block_buf + offset, in, sizeof(exfs_inode_t));
    return exfs_write_block(vol, block_idx, block_buf);
}


/*  Provenance record append                                            */


/*
 * exfs_append_prov — append one provenance record for an inode.
 * Only callable from kernel context. User processes have no path here.
 */
static void exfs_append_prov(exfs_volume_t *vol, uint64_t inode_num,
                              exfs_inode_t *inode,
                              uint8_t op, uint32_t pid,
                              uint64_t cap_upper,
                              uint64_t file_offset, uint64_t length)
{
    exfs_prov_record_t rec;
    rec.timestamp   = audit_total();   /* use audit tick as timestamp */
    rec.pid         = pid;
    rec.operation   = op;
    rec._pad[0]     = 0;
    rec._pad[1]     = 0;
    rec._pad[2]     = 0;
    rec.cap_upper   = cap_upper;
    rec.file_offset = file_offset;
    rec.length      = length;

    /* Calculate which block and byte offset this record goes to */
    uint64_t rec_size     = sizeof(exfs_prov_record_t);
    uint64_t recs_per_blk = EXFS_BLOCK_SIZE / rec_size;
    uint64_t global_idx   = inode->prov_head + inode->prov_count;
    uint64_t blk_rel      = global_idx / recs_per_blk;
    uint64_t blk_off      = (global_idx % recs_per_blk) * rec_size;
    uint64_t abs_blk      = vol->sb.provenance_block + blk_rel;

    if (blk_rel >= vol->sb.provenance_size) return;  /* provenance region full */

    uint8_t buf[EXFS_BLOCK_SIZE];
    if (!exfs_read_block(vol, abs_blk, buf)) return;

    memcpy(buf + blk_off, &rec, rec_size);
    exfs_write_block(vol, abs_blk, buf);

    inode->prov_count++;
    exfs_write_inode(vol, inode_num, inode);

    audit_record(AUDIT_FILE_WRITE, pid, inode_num, (uint64_t)op);
}


/*  VFS operation implementations for ExFS */


typedef struct {
    exfs_volume_t *vol;
    uint64_t       inode_num;
    exfs_inode_t   inode;
} exfs_node_data_t;

static int exfs_op_open(vfs_node_t *node, uint32_t flags)
{
    (void)flags;
    exfs_node_data_t *nd = (exfs_node_data_t *)node->fs_data;
    /* Re-read inode on open to get fresh metadata */
    exfs_read_inode(nd->vol, nd->inode_num, &nd->inode);
    exfs_append_prov(nd->vol, nd->inode_num, &nd->inode,
                     PROV_OP_READ, 0, 0, 0, 0);
    return 0;
}

static int exfs_op_close(vfs_node_t *node)
{
    (void)node;
    return 0;
}

static int64_t exfs_op_read(vfs_node_t *node, uint64_t offset,
                             void *buf, uint64_t len)
{
    exfs_node_data_t *nd = (exfs_node_data_t *)node->fs_data;
    exfs_inode_t     *in = &nd->inode;

    if (offset >= in->size) return 0;
    if (offset + len > in->size) len = in->size - offset;
    if (!len) return 0;

    uint64_t bytes_read = 0;
    uint8_t *dst        = (uint8_t *)buf;

    while (bytes_read < len) {
        uint64_t file_block  = (offset + bytes_read) / EXFS_BLOCK_SIZE;
        uint64_t block_off   = (offset + bytes_read) % EXFS_BLOCK_SIZE;
        uint64_t can_read    = EXFS_BLOCK_SIZE - block_off;
        if (can_read > len - bytes_read) can_read = len - bytes_read;

        if (file_block >= EXFS_DIRECT_BLOCKS) {
            /* Indirect blocks not yet implemented */
            break;
        }

        uint64_t disk_block = in->direct[file_block];
        if (!disk_block) break;

        uint8_t block_buf[EXFS_BLOCK_SIZE];
        if (!exfs_read_block(nd->vol, disk_block, block_buf)) break;

        memcpy(dst + bytes_read, block_buf + block_off, can_read);
        bytes_read += can_read;
    }

    exfs_append_prov(nd->vol, nd->inode_num, in,
                     PROV_OP_READ, 0, 0, offset, bytes_read);

    return (int64_t)bytes_read;
}

static int64_t exfs_op_write(vfs_node_t *node, uint64_t offset,
                              const void *buf, uint64_t len)
{
    exfs_node_data_t *nd = (exfs_node_data_t *)node->fs_data;
    exfs_inode_t     *in = &nd->inode;

    uint64_t       written = 0;
    const uint8_t *src     = (const uint8_t *)buf;

    while (written < len) {
        uint64_t file_block = (offset + written) / EXFS_BLOCK_SIZE;
        uint64_t block_off  = (offset + written) % EXFS_BLOCK_SIZE;
        uint64_t can_write  = EXFS_BLOCK_SIZE - block_off;
        if (can_write > len - written) can_write = len - written;

        if (file_block >= EXFS_DIRECT_BLOCKS) break;

        /* Allocate block if not present */
        if (!in->direct[file_block]) {
            uint64_t new_blk = exfs_alloc_block(nd->vol);
            if (!new_blk) {
                serial_print("[ExFS] write: no free blocks\n");
                break;
            }
            in->direct[file_block] = new_blk;
        }

        uint8_t block_buf[EXFS_BLOCK_SIZE];
        memset(block_buf, 0, EXFS_BLOCK_SIZE);

        /* Read-modify-write if we are not writing a full block */
        if (block_off != 0 || can_write != EXFS_BLOCK_SIZE)
            exfs_read_block(nd->vol, in->direct[file_block], block_buf);

        memcpy(block_buf + block_off, src + written, can_write);
        if (!exfs_write_block(nd->vol, in->direct[file_block], block_buf)) break;

        written += can_write;
    }

    if (offset + written > in->size)
        in->size = offset + written;

    in->modified_at = audit_total();
    exfs_write_inode(nd->vol, nd->inode_num, in);
    node->size = in->size;

    exfs_append_prov(nd->vol, nd->inode_num, in,
                     PROV_OP_WRITE, 0, 0, offset, written);

    return (int64_t)written;
}

static vfs_node_t *exfs_op_lookup(vfs_node_t *dir, const char *name)
{
    exfs_node_data_t *nd = (exfs_node_data_t *)dir->fs_data;
    exfs_inode_t     *in = &nd->inode;

    /* Read directory blocks and scan for the entry */
    uint8_t block_buf[EXFS_BLOCK_SIZE];

    for (int b = 0; b < EXFS_DIRECT_BLOCKS; b++) {
        if (!in->direct[b]) break;

        if (!exfs_read_block(nd->vol, in->direct[b], block_buf)) return NULL;

        uint64_t offset = 0;
        while (offset + sizeof(exfs_dirent_t) <= EXFS_BLOCK_SIZE) {
            exfs_dirent_t *de = (exfs_dirent_t *)(block_buf + offset);
            if (!de->inode_num) break;

            if (de->name_len == (uint16_t)strlen(name) &&
                memcmp(de->name, name, de->name_len) == 0)
            {
                /* Found — build a vfs_node for the entry */
                exfs_inode_t child_inode;
                if (!exfs_read_inode(nd->vol, de->inode_num, &child_inode))
                    return NULL;

                exfs_node_data_t *cnd = kzalloc(sizeof(exfs_node_data_t));
                if (!cnd) return NULL;

                cnd->vol       = nd->vol;
                cnd->inode_num = de->inode_num;
                cnd->inode     = child_inode;

                vfs_node_t *child = kzalloc(sizeof(vfs_node_t));
                if (!child) { kfree(cnd); return NULL; }

                memcpy(child->name, name, strlen(name) + 1);
                child->type    = (de->file_type == 1)
                               ? VFS_DIRECTORY : VFS_FILE;
                child->size    = child_inode.size;
                child->inode   = de->inode_num;
                child->ops     = dir->ops;
                child->fs_data = cnd;
                child->parent  = dir;
                return child;
            }

            offset += sizeof(exfs_dirent_t);
        }
    }

    return NULL;  /* not found */
}


static int64_t exfs_op_readdir(vfs_node_t *dir, uint64_t offset,
                                void *buf, uint64_t max)
{
    typedef struct { uint64_t inode; uint8_t type; char name[256]; } ud_t;
    exfs_node_data_t *nd  = (exfs_node_data_t *)dir->fs_data;
    exfs_inode_t     *in  = &nd->inode;
    ud_t             *out = (ud_t *)buf;
    uint64_t count = 0, skip = offset;
    uint8_t block_buf[EXFS_BLOCK_SIZE];
    for (int b = 0; b < EXFS_DIRECT_BLOCKS && count < max; b++) {
        if (!in->direct[b]) break;
        if (!exfs_read_block(nd->vol, in->direct[b], block_buf)) break;
        uint64_t off = 0;
        while (off + sizeof(exfs_dirent_t) <= EXFS_BLOCK_SIZE && count < max) {
            exfs_dirent_t *de = (exfs_dirent_t *)(block_buf + off);
            if (!de->inode_num) break;
            if (skip > 0) { skip--; off += sizeof(exfs_dirent_t); continue; }
            out[count].inode = de->inode_num;
            out[count].type  = de->file_type;
            uint16_t nlen = de->name_len < 255 ? de->name_len : 255;
            memcpy(out[count].name, de->name, nlen);
            out[count].name[nlen] = 0;
            count++;
            off += sizeof(exfs_dirent_t);
        }
    }
    return (int64_t)count;
}

static vfs_node_t *exfs_op_create(vfs_node_t *dir, const char *name,
                                   uint8_t ftype)
{
    uint64_t name_len = strlen(name);
    if (!name_len || name_len > EXFS_NAME_MAX) return NULL;

    exfs_node_data_t *nd  = (exfs_node_data_t *)dir->fs_data;
    exfs_volume_t    *vol = nd->vol;
    uint64_t ipb = EXFS_BLOCK_SIZE / EXFS_INODE_SIZE;
    uint8_t block_buf[EXFS_BLOCK_SIZE];

    /* Find free inode (skip 0 = root) */
    uint64_t free_ino = 0;
    for (uint64_t b = 0; b < (vol->sb.total_inodes / ipb) && !free_ino; b++) {
        if (!exfs_read_block(vol, vol->sb.inode_table_block + b, block_buf))
            return NULL;
        for (uint64_t i = 0; i < ipb; i++) {
            uint64_t ino = b * ipb + i;
            if (ino == 0) continue;
            exfs_inode_t *in = (exfs_inode_t *)(block_buf + i * EXFS_INODE_SIZE);
            if (!in->mode && !in->size) { free_ino = ino; break; }
        }
    }
    if (!free_ino) return NULL;

    /* Write new inode */
    exfs_inode_t new_inode;
    memset(&new_inode, 0, sizeof(new_inode));
    new_inode.mode        = (ftype == 1) ? 0755 : 0644;
    new_inode.creator_pid = 1;
    exfs_write_inode(vol, free_ino, &new_inode);

    /* Add dirent to parent directory */
    exfs_inode_t *din = &nd->inode;
    uint64_t dir_block = 0;
    memset(block_buf, 0, EXFS_BLOCK_SIZE);

    for (int b = 0; b < EXFS_DIRECT_BLOCKS; b++) {
        if (!din->direct[b]) {
            dir_block = exfs_alloc_block(vol);
            if (!dir_block) return NULL;
            memset(block_buf, 0, EXFS_BLOCK_SIZE);
            din->direct[b] = dir_block;
            exfs_write_inode(vol, nd->inode_num, din);
            break;
        }
        if (!exfs_read_block(vol, din->direct[b], block_buf)) return NULL;
        uint64_t off = 0;
        while (off + sizeof(exfs_dirent_t) <= EXFS_BLOCK_SIZE) {
            exfs_dirent_t *de = (exfs_dirent_t *)(block_buf + off);
            if (!de->inode_num) { dir_block = din->direct[b]; break; }
            off += sizeof(exfs_dirent_t);
        }
        if (dir_block) break;
    }
    if (!dir_block) return NULL;

    /* Find empty slot and write dirent */
    uint64_t off2 = 0;
    bool inserted = false;
    while (off2 + sizeof(exfs_dirent_t) <= EXFS_BLOCK_SIZE) {
        exfs_dirent_t *de = (exfs_dirent_t *)(block_buf + off2);
        if (!de->inode_num) {
            memset(de, 0, sizeof(exfs_dirent_t));
            de->inode_num = free_ino;
            de->file_type = ftype;
            de->name_len  = (uint16_t)name_len;
            memcpy(de->name, name, name_len);
            exfs_write_block(vol, dir_block, block_buf);
            inserted = true;
            break;
        }
        off2 += sizeof(exfs_dirent_t);
    }
    if (!inserted) return NULL;

    exfs_node_data_t *cnd = kzalloc(sizeof(exfs_node_data_t));
    if (!cnd) return NULL;
    cnd->vol = vol; cnd->inode_num = free_ino; cnd->inode = new_inode;

    vfs_node_t *child = kzalloc(sizeof(vfs_node_t));
    if (!child) { kfree(cnd); return NULL; }
    memcpy(child->name, name, name_len + 1);
    child->type    = (ftype == 1) ? VFS_DIRECTORY : VFS_FILE;
    child->size    = 0;
    child->inode   = free_ino;
    child->ops     = dir->ops;
    child->fs_data = cnd;
    child->parent  = dir;
    return child;
}



static const vfs_ops_t g_exfs_ops = {
    .open   = exfs_op_open,
    .close  = exfs_op_close,
    .read   = exfs_op_read,
    .write  = exfs_op_write,
    .lookup  = exfs_op_lookup,
    .readdir = exfs_op_readdir,
    .create  = exfs_op_create,
};






/*  Mount   */


vfs_node_t *exfs_mount(uint32_t lba_base)
{
    exfs_volume_t *vol = kzalloc(sizeof(exfs_volume_t));
    if (!vol) return NULL;

    vol->dev_lba_base = lba_base;

    /* Read superblock */
    uint8_t sb_buf[EXFS_BLOCK_SIZE];
    if (!exfs_read_block(vol, 0, sb_buf)) {
        kfree(vol);
        return NULL;
    }

    memcpy(&vol->sb, sb_buf, sizeof(exfs_superblock_t));

    if (vol->sb.magic != EXFS_MAGIC) {
        serial_print("[ExFS] Bad magic — not an ExFS volume\n");
        kfree(vol);
        return NULL;
    }

    /* Allocate and load the in-memory block bitmap */
    uint64_t bitmap_bytes = (vol->sb.total_blocks + 7) / 8;
    if (bitmap_bytes < EXFS_BLOCK_SIZE) bitmap_bytes = EXFS_BLOCK_SIZE;
    vol->block_bitmap = kzalloc(bitmap_bytes);
    if (!vol->block_bitmap) {
        serial_print("[ExFS] mount: bitmap alloc failed\n");
        kfree(vol);
        return NULL;
    }
    /* Load the on-disk bitmap block into memory */
    if (!exfs_read_block(vol, vol->sb.block_bitmap_block, vol->block_bitmap)) {
        serial_print("[ExFS] mount: bitmap read failed\n");
        kfree(vol->block_bitmap);
        kfree(vol);
        return NULL;
    }
    /* Ensure all metadata blocks are marked used in the bitmap */
    for (uint64_t b = 0; b < vol->sb.data_start_block; b++)
        bitmap_set(vol, b);

    serial_print("[ExFS] Superblock OK. Total blocks: ");
    serial_printhex(vol->sb.total_blocks);
    serial_print(" Free: ");
    serial_printhex(vol->sb.free_blocks);
    serial_print("\n");

    /* Build root vfs_node (inode 0 = root directory) */
    exfs_node_data_t *rnd = kzalloc(sizeof(exfs_node_data_t));
    if (!rnd) { kfree(vol); return NULL; }

    rnd->vol       = vol;
    rnd->inode_num = 0;
    exfs_read_inode(vol, 0, &rnd->inode);

    vfs_node_t *root = kzalloc(sizeof(vfs_node_t));
    if (!root) { kfree(rnd); kfree(vol); return NULL; }

    memcpy(root->name, "/", 2);
    root->type    = VFS_DIRECTORY;
    root->size    = rnd->inode.size;
    root->inode   = 0;
    root->ops     = &g_exfs_ops;
    root->fs_data = rnd;
    root->parent  = NULL;

    serial_print("[ExFS] Root mounted\n");
    return root;
}


/*  Format  */


bool exfs_format(uint32_t lba_base, uint64_t total_blocks)
{
    uint64_t inode_blocks = (EXFS_MAX_INODES * EXFS_INODE_SIZE
                           + EXFS_BLOCK_SIZE - 1) / EXFS_BLOCK_SIZE;
    uint64_t inode_table_block = 2;
    uint64_t block_bitmap_block = 1;
    uint64_t provenance_size = 16;
    if (total_blocks <= inode_table_block + inode_blocks + provenance_size)
        return false;

    uint64_t data_start_block = inode_table_block + inode_blocks;
    uint64_t provenance_block = total_blocks - provenance_size;

    exfs_volume_t vol;
    memset(&vol, 0, sizeof(vol));
    vol.dev_lba_base = lba_base;

    uint8_t zero_block[EXFS_BLOCK_SIZE];
    memset(zero_block, 0, EXFS_BLOCK_SIZE);

    /* Superblock at block 0 */
    exfs_superblock_t *sb = &vol.sb;
    sb->magic              = EXFS_MAGIC;
    sb->version            = EXFS_VERSION;
    sb->total_blocks       = total_blocks;
    sb->free_blocks        = total_blocks - data_start_block - provenance_size;
    sb->total_inodes       = EXFS_MAX_INODES;
    sb->free_inodes        = EXFS_MAX_INODES - 1;
    sb->inode_table_block  = inode_table_block;
    sb->block_bitmap_block = block_bitmap_block;
    sb->data_start_block   = data_start_block;
    sb->provenance_block   = provenance_block;
    sb->provenance_size    = provenance_size;

    uint8_t sb_buf[EXFS_BLOCK_SIZE];
    memset(sb_buf, 0, EXFS_BLOCK_SIZE);
    memcpy(sb_buf, sb, sizeof(exfs_superblock_t));

    if (!exfs_write_block(&vol, 0, sb_buf)) {
        serial_print("[ExFS] format: failed to write superblock\n");
        return false;
    }

    /* Zero inode table */
    for (uint64_t b = 0; b < inode_blocks; b++) {
        if (!exfs_write_block(&vol, inode_table_block + b, zero_block))
            return false;
    }

    /* Block bitmap (block 2) — mark metadata blocks as used */
    uint8_t bitmap_buf[EXFS_BLOCK_SIZE];
    memset(bitmap_buf, 0, EXFS_BLOCK_SIZE);
    /* Blocks 0 .. data_start_block-1 are reserved for metadata */
    for (uint64_t b = 0; b < sb->data_start_block && b < BITS_PER_BLK; b++) {
        bitmap_buf[b / 8] |= (uint8_t)(1u << (b % 8));
    }
    for (uint64_t b = sb->provenance_block;
         b < sb->total_blocks && b < BITS_PER_BLK; b++) {
        bitmap_buf[b / 8] |= (uint8_t)(1u << (b % 8));
    }
    if (!exfs_write_block(&vol, block_bitmap_block, bitmap_buf)) return false;

    /* Write root directory inode (inode 0) */
    exfs_inode_t root_inode;
    memset(&root_inode, 0, sizeof(root_inode));
    root_inode.size        = 0;
    root_inode.created_at  = 0;
    root_inode.creator_pid = 0;
    root_inode.mode        = 0755;
    root_inode.prov_head   = 0;
    root_inode.prov_count  = 0;

    if (!exfs_write_inode(&vol, 0, &root_inode)) {
        serial_print("[ExFS] format: failed to write root inode\n");
        return false;
    }

    serial_print("[ExFS] Format complete. Volume ready.\n");
    return true;
}
