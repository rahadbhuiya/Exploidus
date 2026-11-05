#include "inode.h"
#include "../../drivers/serial.h"
#include "../../crypto/blake3.h"
#include <string.h>


/*  Logical file-block -> disk-block resolution (direct/indirect/       */
/*  double-indirect), shared by read and write                         */


/*
 * exfs_resolve_block — translate a logical block index within a file
 * into a disk block number, walking direct pointers, then single
 * indirect, then double indirect (see EXFS_INDIRECT_PTRS in exfs.h for
 * the addressable range each level covers).
 */
uint64_t exfs_resolve_block(exfs_volume_t *vol, exfs_inode_t *in,
                                    uint64_t file_block, bool alloc,
                                    bool *inode_dirty)
{
    if (file_block < EXFS_DIRECT_BLOCKS) {
        if (!in->direct[file_block]) {
            if (!alloc) return 0;
            uint64_t nb = exfs_alloc_block(vol);
            if (!nb) return 0;
            in->direct[file_block] = nb;
            *inode_dirty = true;
        }
        return in->direct[file_block];
    }
    file_block -= EXFS_DIRECT_BLOCKS;

    if (file_block < EXFS_INDIRECT_PTRS) {
        if (!in->indirect) {
            if (!alloc) return 0;
            uint64_t nb = exfs_alloc_block(vol);
            if (!nb) return 0;
            static uint8_t zero_blk[EXFS_BLOCK_SIZE];
            memset(zero_blk, 0, EXFS_BLOCK_SIZE);
            exfs_meta_write_block(vol, nb, zero_blk);
            in->indirect = nb;
            *inode_dirty = true;
        }

        static uint8_t ind[EXFS_BLOCK_SIZE];
        if (!exfs_read_block(vol, in->indirect, ind)) return 0;
        uint64_t *ptrs = (uint64_t *)ind;

        if (!ptrs[file_block]) {
            if (!alloc) return 0;
            uint64_t nb = exfs_alloc_block(vol);
            if (!nb) return 0;
            ptrs[file_block] = nb;
            exfs_meta_write_block(vol, in->indirect, ind);
        }
        return ptrs[file_block];
    }
    file_block -= EXFS_INDIRECT_PTRS;

    if (file_block < EXFS_INDIRECT_PTRS * EXFS_INDIRECT_PTRS) {
        uint64_t l1_idx = file_block / EXFS_INDIRECT_PTRS;
        uint64_t l2_idx = file_block % EXFS_INDIRECT_PTRS;

        if (!in->double_indirect) {
            if (!alloc) return 0;
            uint64_t nb = exfs_alloc_block(vol);
            if (!nb) return 0;
            static uint8_t zero_blk[EXFS_BLOCK_SIZE];
            memset(zero_blk, 0, EXFS_BLOCK_SIZE);
            exfs_meta_write_block(vol, nb, zero_blk);
            in->double_indirect = nb;
            *inode_dirty = true;
        }

        static uint8_t l1[EXFS_BLOCK_SIZE];
        if (!exfs_read_block(vol, in->double_indirect, l1)) return 0;
        uint64_t *l1ptrs = (uint64_t *)l1;

        if (!l1ptrs[l1_idx]) {
            if (!alloc) return 0;
            uint64_t nb = exfs_alloc_block(vol);
            if (!nb) return 0;
            static uint8_t zero_blk2[EXFS_BLOCK_SIZE];
            memset(zero_blk2, 0, EXFS_BLOCK_SIZE);
            exfs_meta_write_block(vol, nb, zero_blk2);
            l1ptrs[l1_idx] = nb;
            exfs_meta_write_block(vol, in->double_indirect, l1);
        }

        static uint8_t l2[EXFS_BLOCK_SIZE];
        if (!exfs_read_block(vol, l1ptrs[l1_idx], l2)) return 0;
        uint64_t *l2ptrs = (uint64_t *)l2;

        if (!l2ptrs[l2_idx]) {
            if (!alloc) return 0;
            uint64_t nb = exfs_alloc_block(vol);
            if (!nb) return 0;
            l2ptrs[l2_idx] = nb;
            exfs_meta_write_block(vol, l1ptrs[l1_idx], l2);
        }
        return l2ptrs[l2_idx];
    }

    /* Beyond direct+indirect+double_indirect range. triple_indirect
     * exists in the on-disk inode but resolving through it is not
     * implemented (see EXFS_INDIRECT_PTRS comment in exfs.h). */
    return 0;
}


/*  Whole-file integrity hash                                           */


/*
 * exfs_compute_whole_file_hash — BLAKE3 over the file's entire current
 * content (offset 0 .. in->size), streamed through blake3_update() one
 * block at a time so this never needs a buffer larger than
 * EXFS_BLOCK_SIZE regardless of file size. Replaces an earlier version
 */
void exfs_compute_whole_file_hash(exfs_volume_t *vol,
                                          const exfs_inode_t *in,
                                          uint8_t out_hash[8])
{
    blake3_ctx_t ctx;
    blake3_init(&ctx);

    static uint8_t buf[EXFS_BLOCK_SIZE];
    uint64_t remaining = in->size;
    uint64_t file_block = 0;

    while (remaining > 0) {
        bool unused_dirty = false;
        uint64_t blk = exfs_resolve_block(vol, (exfs_inode_t *)in,
                                            file_block, false, &unused_dirty);
        if (!blk) break; /* hole -- shouldn't happen for a properly
                           * sized file, but don't hang on one */
        if (!exfs_read_block(vol, blk, buf)) break;

        uint64_t chunk = remaining < EXFS_BLOCK_SIZE ? remaining : EXFS_BLOCK_SIZE;
        blake3_update(&ctx, buf, chunk);
        remaining -= chunk;
        file_block++;
    }

    uint8_t digest[32];
    blake3_final(&ctx, digest);
    memcpy(out_hash, digest, 8);
}


/*  Provenance record append                                            */


/*
 * exfs_append_prov — append one provenance record for an inode.
 * Only callable from kernel context. User processes have no path here.
 */
void exfs_append_prov(exfs_volume_t *vol, uint64_t inode_num,
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

    static uint8_t buf[EXFS_BLOCK_SIZE];
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

/*
 * exfs_debug_corrupt_file — TEST ONLY. Flips every bit of the first
 * byte of a file's first data block, writing it back through
 * exfs_write_block() directly -- bypassing exfs_op_write() (and
 * therefore block_hash) entirely. This simulates genuine silent
 */
int exfs_debug_corrupt_file(vfs_node_t *node)
{
    if (!node || !node->fs_data) return -1;
    exfs_node_data_t *nd = (exfs_node_data_t *)node->fs_data;
    if (nd->inode.size == 0) return -1;

    bool unused_dirty = false;
    uint64_t blk = exfs_resolve_block(nd->vol, &nd->inode, 0, false,
                                        &unused_dirty);
    if (!blk) return -1;

    static uint8_t buf[EXFS_BLOCK_SIZE];
    if (!exfs_read_block(nd->vol, blk, buf)) return -1;
    buf[0] ^= 0xFF;
    bool ok = exfs_write_block(nd->vol, blk, buf);

    serial_print("[ExFS] CORRUPT TEST: flipped first byte of inode ");
    serial_printhex(nd->inode_num);
    serial_print(ok ? " (write ok)\n" : " (write FAILED)\n");
    return ok ? 0 : -1;
}


static int exfs_op_open(vfs_node_t *node, uint32_t flags)
{
    (void)flags;
    exfs_node_data_t *nd = (exfs_node_data_t *)node->fs_data;
    /* Re-read inode on open to get fresh metadata */
    exfs_read_inode(nd->vol, nd->inode_num, &nd->inode);

    /*
 * (a populated, non-zero stored block_hash -- see
 * exfs_compute_whole_file_hash()), recompute its whole-file hash
 * now and compare. A mismatch means the on-disk content changed
 * since the hash was last set without going through a normal
 */
    {
        static const uint8_t zero8[8] = {0};
        if (memcmp(nd->inode.block_hash, zero8, 8) != 0) {
            uint8_t fresh[8];
            exfs_compute_whole_file_hash(nd->vol, &nd->inode, fresh);
            if (memcmp(fresh, nd->inode.block_hash, 8) != 0) {
                serial_print("[ExFS] INTEGRITY WARNING: on-disk content for "
                             "inode ");
                serial_printhex(nd->inode_num);
                serial_print(" does not match its stored hash -- possible "
                             "silent corruption\n");
            }
        }
    }

    exfs_append_prov(nd->vol, nd->inode_num, &nd->inode,
                     PROV_OP_READ, 0, 0, 0, 0);
    return 0;
}

static int exfs_op_close(vfs_node_t *node)
{
    (void)node;
    return 0;
}

static int exfs_op_chmod(vfs_node_t *node, uint32_t mode)
{
    exfs_node_data_t *nd = (exfs_node_data_t *)node->fs_data;
    if (!nd) return -1;
    nd->inode.mode = mode;
    exfs_write_inode(nd->vol, nd->inode_num, &nd->inode);
    return 0;
}

static int exfs_op_chgrp(vfs_node_t *node, uint32_t gid)
{
    exfs_node_data_t *nd = (exfs_node_data_t *)node->fs_data;
    if (!nd) return -1;
    nd->inode.group_gid = gid;
    exfs_write_inode(nd->vol, nd->inode_num, &nd->inode);
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

        bool unused_dirty = false;
        uint64_t disk_block = exfs_resolve_block(nd->vol, in, file_block,
                                                   false, &unused_dirty);
        if (!disk_block) break; /* hole / EOF for this block */
