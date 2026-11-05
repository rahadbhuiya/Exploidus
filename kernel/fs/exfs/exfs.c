#include "exfs.h"
#include "journal.h"
#include "inode.h"
#include "../../drivers/serial.h"
#include "../../mm/kmalloc.h"
#include "../../audit/audit.h"
#include "../../proc/process.h"
#include "../../crypto/blake3.h"
#include <string.h>




static int64_t exfs_op_write(vfs_node_t *node, uint64_t offset,
                              const void *buf, uint64_t len)
{
    exfs_node_data_t *nd  = (exfs_node_data_t *)node->fs_data;
    exfs_volume_t    *vol = nd->vol;
    exfs_inode_t     *in  = &nd->inode;

    uint64_t       written = 0;
    const uint8_t *src     = (const uint8_t *)buf;
    bool           inode_dirty = false;

    /* Metadata (inode + any newly allocated indirect/double-indirect
     * pointer blocks) written during this call is journaled as one
     * transaction, so a crash mid-write never leaves a half-updated
     * inode or a pointer block referencing garbage. The data blocks
     * themselves are written directly, below, before the transaction
     * even opens — ordered-mode, see EXFS_JOURNAL_MAGIC in exfs.h. */
    while (written < len) {
        uint64_t file_block = (offset + written) / EXFS_BLOCK_SIZE;
        uint64_t block_off  = (offset + written) % EXFS_BLOCK_SIZE;
        uint64_t can_write  = EXFS_BLOCK_SIZE - block_off;
        if (can_write > len - written) can_write = len - written;

        uint64_t disk_block = exfs_resolve_block(vol, in, file_block,
                                                   true, &inode_dirty);
        if (!disk_block) {
            serial_print("[ExFS] write: allocation failed (volume full "
                         "or file exceeds max addressable size)\n");
            break;
        }

        static uint8_t block_buf[EXFS_BLOCK_SIZE];
        memset(block_buf, 0, EXFS_BLOCK_SIZE);

        /* Read-modify-write if we are not writing a full block */
        if (block_off != 0 || can_write != EXFS_BLOCK_SIZE)
            exfs_read_block(vol, disk_block, block_buf);

        memcpy(block_buf + block_off, src + written, can_write);
        if (!exfs_write_block(vol, disk_block, block_buf)) break;

        written += can_write;
    }

    if (offset + written > in->size) {
        in->size = offset + written;
        inode_dirty = true;
    }

    if (written > 0) {
        in->modified_at = audit_total();
        inode_dirty = true;
    }

    if (inode_dirty) {
        /* Whole-file integrity hash, recomputed after size/content
         * settle -- see exfs_compute_whole_file_hash()'s comment for
         * what this covers and its cost. Only worth doing when the
         * inode is actually dirty (i.e. this write changed anything);
         * a zero-length write() that touched nothing leaves the
         * existing hash (and file) untouched. */
        uint8_t digest[8];
        exfs_compute_whole_file_hash(vol, in, digest);
        memcpy(in->block_hash, digest, sizeof(in->block_hash));

        exfs_journal_begin(vol);
        exfs_write_inode(vol, nd->inode_num, in);
        exfs_journal_commit(vol);
    }
    node->size = in->size;

    exfs_append_prov(vol, nd->inode_num, in,
                     PROV_OP_WRITE, 0, 0, offset, written);

    return (int64_t)written;
}

static vfs_node_t *exfs_op_lookup(vfs_node_t *dir, const char *name)
{
    exfs_node_data_t *nd = (exfs_node_data_t *)dir->fs_data;
    exfs_inode_t     *in = &nd->inode;

    /* Read directory blocks and scan for the entry */
    static uint8_t block_buf[EXFS_BLOCK_SIZE];

    for (uint64_t b = 0; b < EXFS_MAX_DIR_BLOCKS; b++) {
        bool unused_dirty = false;
        uint64_t blk = exfs_resolve_block(nd->vol, in, b, false, &unused_dirty);
        if (!blk) break;

        if (!exfs_read_block(nd->vol, blk, block_buf)) return NULL;

        uint64_t offset = 0;
        while (offset + sizeof(exfs_dirent_t) <= EXFS_BLOCK_SIZE) {
            exfs_dirent_t *de = (exfs_dirent_t *)(block_buf + offset);
            if (!de->inode_num) {
                /* Deleted entry (tombstone left by unlink) — skip it,
                 * do NOT treat it as end-of-directory. Real entries
                 * can exist after a hole once files have been removed. */
                offset += sizeof(exfs_dirent_t);
                continue;
            }

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
                child->type    = (de->file_type == 1) ? VFS_DIRECTORY
                               : (de->file_type == 2) ? VFS_SYMLINK
                               : VFS_FILE;
                child->size    = child_inode.size;
                child->inode   = de->inode_num;
                child->ops     = dir->ops;
                child->fs_data = cnd;
                child->parent  = dir;
                child->mode    = child_inode.mode;
                child->owner_uid = child_inode.owner_uid;
                child->group_gid = child_inode.group_gid;
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
    static uint8_t block_buf[EXFS_BLOCK_SIZE];
    for (uint64_t b = 0; b < EXFS_MAX_DIR_BLOCKS && count < max; b++) {
        bool unused_dirty = false;
        uint64_t blk = exfs_resolve_block(nd->vol, in, b, false, &unused_dirty);
        if (!blk) break;
        if (!exfs_read_block(nd->vol, blk, block_buf)) break;
        uint64_t off = 0;
        while (off + sizeof(exfs_dirent_t) <= EXFS_BLOCK_SIZE && count < max) {
            exfs_dirent_t *de = (exfs_dirent_t *)(block_buf + off);
            if (!de->inode_num) { off += sizeof(exfs_dirent_t); continue; }
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
    static uint8_t block_buf[EXFS_BLOCK_SIZE];

    /*
 * Without this check, two dirents with the identical name could
 * both exist (each pointing at a different inode) — e.g. any
 * caller that falls back to create() after a failed open() for an
 * unrelated reason would silently duplicate the file instead of
 */
    {
        exfs_inode_t *din = &nd->inode;
        static uint8_t dup_check_buf[EXFS_BLOCK_SIZE];
        for (uint64_t b = 0; b < EXFS_MAX_DIR_BLOCKS; b++) {
            bool unused_dirty = false;
            uint64_t blk = exfs_resolve_block(vol, din, b, false, &unused_dirty);
            if (!blk) break;
            if (!exfs_read_block(vol, blk, dup_check_buf)) break;
            uint64_t off = 0;
            while (off + sizeof(exfs_dirent_t) <= EXFS_BLOCK_SIZE) {
                exfs_dirent_t *de = (exfs_dirent_t *)(dup_check_buf + off);
                if (de->inode_num && de->name_len == (uint16_t)name_len &&
                    memcmp(de->name, name, name_len) == 0)
                    return NULL; /* EEXIST */
                off += sizeof(exfs_dirent_t);
            }
        }
    }

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

    /* Everything below (new inode, possibly a newly allocated dir
     * block, and the dirent insertion) is one logical operation --
     * journaled as a single transaction so a crash partway through
     * never leaves e.g. a written inode with no dirent pointing at it
     * yet visible, or vice versa, in a way that corrupts traversal. */
    exfs_journal_begin(vol);

    /* Write new inode */
    exfs_inode_t new_inode;
    memset(&new_inode, 0, sizeof(new_inode));
    new_inode.mode        = (ftype == 1) ? 0755 : 0644;
    new_inode.creator_pid = 1;
    /* Real owner, unlike creator_pid above (a pre-existing hardcoded
     * placeholder, not this change's concern). g_current_proc is
     * already correctly set to the calling process by the time this
     * runs (vfs_create() -> here happens synchronously inside
     * whatever syscall handler is servicing the calling process),
     * same pattern vfs.c already uses for fd owner_pid. */
    new_inode.owner_uid = g_current_proc ? g_current_proc->uid : UID_ROOT;
    new_inode.group_gid = g_current_proc ? g_current_proc->gid : GID_ROOT;
    exfs_write_inode(vol, free_ino, &new_inode);

    /* Add dirent to parent directory */
    exfs_inode_t *din = &nd->inode;
    uint64_t dir_block = 0;
    memset(block_buf, 0, EXFS_BLOCK_SIZE);

    for (uint64_t b = 0; b < EXFS_MAX_DIR_BLOCKS; b++) {
        bool dirty = false;
        uint64_t blk = exfs_resolve_block(vol, din, b, false, &dirty);
        if (!blk) {
            /* First hole: extend the directory by one block here
             * (walks through indirect/double-indirect allocation the
             * same way a file's write() does, once b >=
             * EXFS_DIRECT_BLOCKS). */
            blk = exfs_resolve_block(vol, din, b, true, &dirty);
            if (!blk) { exfs_journal_commit(vol); return NULL; }
            if (dirty) exfs_write_inode(vol, nd->inode_num, din);
            memset(block_buf, 0, EXFS_BLOCK_SIZE);
            dir_block = blk;
            break;
        }
        if (!exfs_txn_read_block(vol, blk, block_buf)) {
            exfs_journal_commit(vol); return NULL;
        }
        uint64_t off = 0;
        while (off + sizeof(exfs_dirent_t) <= EXFS_BLOCK_SIZE) {
            exfs_dirent_t *de = (exfs_dirent_t *)(block_buf + off);
            if (!de->inode_num) { dir_block = blk; break; }
            off += sizeof(exfs_dirent_t);
        }
        if (dir_block) break;
    }
    if (!dir_block) { exfs_journal_commit(vol); return NULL; }

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
            exfs_meta_write_block(vol, dir_block, block_buf);
            inserted = true;
            break;
        }
        off2 += sizeof(exfs_dirent_t);
    }
    exfs_journal_commit(vol);
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
    child->mode    = new_inode.mode;
    child->owner_uid = new_inode.owner_uid;
    child->group_gid = new_inode.group_gid;
    return child;
}

/*
 * exfs_op_symlink — create a symlink dirent (file_type == 2) whose
 * stored content is the raw target path string. Mirrors
 * exfs_op_create()'s structure (duplicate-name guard, free-inode
 * search, one journaled transaction covering the new inode + its one
 */
static int exfs_op_symlink(vfs_node_t *dir, const char *name, const char *target)
{
    uint64_t name_len = strlen(name);
    if (!name_len || name_len > EXFS_NAME_MAX) return -1;
    uint64_t target_len = strlen(target);
    if (!target_len || target_len > EXFS_NAME_MAX) return -1;

    exfs_node_data_t *nd  = (exfs_node_data_t *)dir->fs_data;
    exfs_volume_t    *vol = nd->vol;
    uint64_t ipb = EXFS_BLOCK_SIZE / EXFS_INODE_SIZE;
    static uint8_t block_buf[EXFS_BLOCK_SIZE];

    /* Duplicate-name guard -- see the identical block in
     * exfs_op_create() for why this matters. */
    {
        exfs_inode_t *din = &nd->inode;
        static uint8_t dup_check_buf[EXFS_BLOCK_SIZE];
        for (uint64_t b = 0; b < EXFS_MAX_DIR_BLOCKS; b++) {
            bool unused_dirty = false;
            uint64_t blk = exfs_resolve_block(vol, din, b, false, &unused_dirty);
            if (!blk) break;
            if (!exfs_read_block(vol, blk, dup_check_buf)) break;
            uint64_t off = 0;
            while (off + sizeof(exfs_dirent_t) <= EXFS_BLOCK_SIZE) {
                exfs_dirent_t *de = (exfs_dirent_t *)(dup_check_buf + off);
                if (de->inode_num && de->name_len == (uint16_t)name_len &&
                    memcmp(de->name, name, name_len) == 0)
                    return -1; /* EEXIST */
                off += sizeof(exfs_dirent_t);
            }
        }
    }

    uint64_t free_ino = 0;
    for (uint64_t b = 0; b < (vol->sb.total_inodes / ipb) && !free_ino; b++) {
        if (!exfs_read_block(vol, vol->sb.inode_table_block + b, block_buf))
            return -1;
        for (uint64_t i = 0; i < ipb; i++) {
            uint64_t ino = b * ipb + i;
            if (ino == 0) continue;
            exfs_inode_t *in = (exfs_inode_t *)(block_buf + i * EXFS_INODE_SIZE);
            if (!in->mode && !in->size) { free_ino = ino; break; }
        }
    }
    if (!free_ino) return -1;

    exfs_journal_begin(vol);

    uint64_t data_blk = exfs_alloc_block(vol);
    if (!data_blk) { exfs_journal_commit(vol); return -1; }
    memset(block_buf, 0, EXFS_BLOCK_SIZE);
    memcpy(block_buf, target, target_len);
    exfs_meta_write_block(vol, data_blk, block_buf);

    exfs_inode_t new_inode;
    memset(&new_inode, 0, sizeof(new_inode));
    new_inode.mode        = 0777; /* symlinks are traditionally
                                    * always "rwxrwxrwx"; permission
                                    * checks apply to the target once
                                    * resolved, not the link itself */
    new_inode.size         = target_len;
    new_inode.direct[0]    = data_blk;
    new_inode.creator_pid  = 1;
    new_inode.owner_uid    = g_current_proc ? g_current_proc->uid : UID_ROOT;
    new_inode.group_gid    = g_current_proc ? g_current_proc->gid : GID_ROOT;
    /* Whole-file integrity hash covers this too (see
     * exfs_compute_whole_file_hash()'s comment on exfs_op_write) --
     * computed directly from `target`, already in memory, rather than
     * reading the just-written data block back for a one-block file. */
    {
        uint8_t digest[32];
        blake3_hash((const uint8_t *)target, target_len, digest);
        memcpy(new_inode.block_hash, digest, sizeof(new_inode.block_hash));
    }
    exfs_write_inode(vol, free_ino, &new_inode);

    exfs_inode_t *din = &nd->inode;
    uint64_t dir_block = 0;
    memset(block_buf, 0, EXFS_BLOCK_SIZE);

    for (uint64_t b = 0; b < EXFS_MAX_DIR_BLOCKS; b++) {
        bool dirty = false;
        uint64_t blk = exfs_resolve_block(vol, din, b, false, &dirty);
        if (!blk) {
            blk = exfs_resolve_block(vol, din, b, true, &dirty);
            if (!blk) { exfs_journal_commit(vol); return -1; }
            if (dirty) exfs_write_inode(vol, nd->inode_num, din);
            memset(block_buf, 0, EXFS_BLOCK_SIZE);
            dir_block = blk;
            break;
        }
        if (!exfs_txn_read_block(vol, blk, block_buf)) {
            exfs_journal_commit(vol); return -1;
        }
        uint64_t off = 0;
        while (off + sizeof(exfs_dirent_t) <= EXFS_BLOCK_SIZE) {
            exfs_dirent_t *de = (exfs_dirent_t *)(block_buf + off);
            if (!de->inode_num) { dir_block = blk; break; }
            off += sizeof(exfs_dirent_t);
        }
        if (dir_block) break;
    }
    if (!dir_block) { exfs_journal_commit(vol); return -1; }

    uint64_t off2 = 0;
    bool inserted = false;
    while (off2 + sizeof(exfs_dirent_t) <= EXFS_BLOCK_SIZE) {
        exfs_dirent_t *de = (exfs_dirent_t *)(block_buf + off2);
        if (!de->inode_num) {
            memset(de, 0, sizeof(exfs_dirent_t));
            de->inode_num = free_ino;
            de->file_type = 2; /* symlink */
            de->name_len  = (uint16_t)name_len;
            memcpy(de->name, name, name_len);
            exfs_meta_write_block(vol, dir_block, block_buf);
            inserted = true;
            break;
        }
        off2 += sizeof(exfs_dirent_t);
    }
    exfs_journal_commit(vol);
    return inserted ? 0 : -1;
}



/*
 * exfs_op_unlink — remove a file from a directory.
 * Frees the file's direct blocks and (if present) its indirect block
 * and everything it points to, zeroes the inode so exfs_op_create's
 * free-inode scan (mode==0 && size==0) can reuse it, then clears the
 */
static int exfs_op_unlink(vfs_node_t *dir, const char *name)
{
    exfs_node_data_t *nd  = (exfs_node_data_t *)dir->fs_data;
    exfs_volume_t    *vol = nd->vol;
    exfs_inode_t     *din = &nd->inode;

    uint64_t name_len = strlen(name);
    if (!name_len || name_len > EXFS_NAME_MAX) return -1;

    static uint8_t block_buf[EXFS_BLOCK_SIZE];

    for (uint64_t b = 0; b < EXFS_MAX_DIR_BLOCKS; b++) {
        bool unused_dirty = false;
        uint64_t blk = exfs_resolve_block(vol, din, b, false, &unused_dirty);
        if (!blk) break;
        if (!exfs_read_block(vol, blk, block_buf)) return -1;

        uint64_t off = 0;
        while (off + sizeof(exfs_dirent_t) <= EXFS_BLOCK_SIZE) {
            exfs_dirent_t *de = (exfs_dirent_t *)(block_buf + off);
            if (!de->inode_num) { off += sizeof(exfs_dirent_t); continue; }

            if (de->name_len == (uint16_t)name_len &&
                memcmp(de->name, name, name_len) == 0)
            {
                if (de->file_type == 1) return -2;  /* it's a directory */

                uint64_t child_ino = de->inode_num;

                exfs_inode_t cin;
                if (!exfs_read_inode(vol, child_ino, &cin)) return -1;

                /*
 * + inode zeroing happen FIRST, atomically (one
 * journal transaction), and the child's data blocks
 * are only freed AFTER that transaction durably
 * commits. Freeing blocks before removing the dirent
 */
                exfs_journal_begin(vol);

                exfs_inode_t blank;
                memset(&blank, 0, sizeof(blank));
                exfs_write_inode(vol, child_ino, &blank);

                memset(de, 0, sizeof(exfs_dirent_t));
                exfs_meta_write_block(vol, blk, block_buf);

                exfs_journal_commit(vol);
                if (g_exfs_crash_point == 4) exfs_debug_crash_now();

                for (int db = 0; db < EXFS_DIRECT_BLOCKS; db++) {
                    if (cin.direct[db]) exfs_free_block(vol, cin.direct[db]);
                }
                if (cin.indirect) {
                    static uint8_t ind_buf[EXFS_BLOCK_SIZE];
                    if (exfs_read_block(vol, cin.indirect, ind_buf)) {
                        uint64_t *ptrs = (uint64_t *)ind_buf;
                        for (uint64_t pi = 0; pi < EXFS_INDIRECT_PTRS; pi++) {
                            if (ptrs[pi]) exfs_free_block(vol, ptrs[pi]);
                        }
                    }
                    exfs_free_block(vol, cin.indirect);
                }
                /* double_indirect: free every leaf data block, then
                 * every level-1 pointer block, then the level-1 table
                 * itself. This was previously missing entirely — any
                 * file that used double_indirect (unreachable before
                 * this change added write support for it) would have
                 * leaked its whole double-indirect subtree on unlink. */
                if (cin.double_indirect) {
                    static uint8_t l1_buf[EXFS_BLOCK_SIZE];
                    if (exfs_read_block(vol, cin.double_indirect, l1_buf)) {
                        uint64_t *l1ptrs = (uint64_t *)l1_buf;
                        for (uint64_t i1 = 0; i1 < EXFS_INDIRECT_PTRS; i1++) {
                            if (!l1ptrs[i1]) continue;
                            static uint8_t l2_buf[EXFS_BLOCK_SIZE];
                            if (exfs_read_block(vol, l1ptrs[i1], l2_buf)) {
                                uint64_t *l2ptrs = (uint64_t *)l2_buf;
                                for (uint64_t i2 = 0; i2 < EXFS_INDIRECT_PTRS; i2++) {
                                    if (l2ptrs[i2]) exfs_free_block(vol, l2ptrs[i2]);
                                }
                            }
                            exfs_free_block(vol, l1ptrs[i1]);
                        }
                    }
                    exfs_free_block(vol, cin.double_indirect);
                }

                return 0;
            }

            off += sizeof(exfs_dirent_t);
        }
    }

    return -1;  /* not found */
}

/* Returns true if a directory inode has zero live entries in any of
 * its allocated blocks (direct, indirect, or double-indirect) — used
 * by rmdir to refuse removing a non-empty directory. */
static bool exfs_dir_is_empty(exfs_volume_t *vol, exfs_inode_t *in)
{
    static uint8_t buf[EXFS_BLOCK_SIZE];

    for (uint64_t b = 0; b < EXFS_MAX_DIR_BLOCKS; b++) {
        bool unused_dirty = false;
        uint64_t blk = exfs_resolve_block(vol, in, b, false, &unused_dirty);
        if (!blk) break;
        if (!exfs_read_block(vol, blk, buf)) return false; /* be safe: treat unreadable as non-empty */
        uint64_t off = 0;
        while (off + sizeof(exfs_dirent_t) <= EXFS_BLOCK_SIZE) {
            exfs_dirent_t *de = (exfs_dirent_t *)(buf + off);
            if (de->inode_num) return false;
            off += sizeof(exfs_dirent_t);
        }
    }

    return true;
}

/*
 * exfs_op_rmdir — removes an EMPTY directory. Refuses (-2) if it has
 * any live entries, matching standard rmdir() semantics. Mirrors
 * exfs_op_unlink's block-freeing/dirent-removal pattern, but for the
 * file_type==1 (directory) case that unlink deliberately rejects.
 */
static int exfs_op_rmdir(vfs_node_t *dir, const char *name)
{
    exfs_node_data_t *nd  = (exfs_node_data_t *)dir->fs_data;
    exfs_volume_t    *vol = nd->vol;
    exfs_inode_t     *din = &nd->inode;

    uint64_t name_len = strlen(name);
    if (!name_len || name_len > EXFS_NAME_MAX) return -1;
    if (name_len == 1 && name[0] == '.') return -1;
    if (name_len == 2 && name[0] == '.' && name[1] == '.') return -1;

    static uint8_t block_buf[EXFS_BLOCK_SIZE];

    for (uint64_t b = 0; b < EXFS_MAX_DIR_BLOCKS; b++) {
        bool unused_dirty = false;
        uint64_t blk = exfs_resolve_block(vol, din, b, false, &unused_dirty);
        if (!blk) break;
        if (!exfs_read_block(vol, blk, block_buf)) return -1;

        uint64_t off = 0;
        while (off + sizeof(exfs_dirent_t) <= EXFS_BLOCK_SIZE) {
            exfs_dirent_t *de = (exfs_dirent_t *)(block_buf + off);
            if (!de->inode_num) { off += sizeof(exfs_dirent_t); continue; }

            if (de->name_len == (uint16_t)name_len &&
                memcmp(de->name, name, name_len) == 0)
            {
                if (de->file_type != 1) return -3; /* not a directory */

                uint64_t child_ino = de->inode_num;
                exfs_inode_t cin;
                if (!exfs_read_inode(vol, child_ino, &cin)) return -1;

                if (!exfs_dir_is_empty(vol, &cin)) return -2; /* not empty */

                /* Same crash-safety reordering as exfs_op_unlink:
                 * remove the dirent + zero the inode in one journaled
                 * transaction FIRST, and only free the (now provably
                 * unreachable) blocks after that commits. See the
                 * comment in exfs_op_unlink for the failure mode this
                 * avoids. */
                exfs_journal_begin(vol);

                exfs_inode_t blank;
                memset(&blank, 0, sizeof(blank));
                exfs_write_inode(vol, child_ino, &blank);

                memset(de, 0, sizeof(exfs_dirent_t));
                exfs_meta_write_block(vol, blk, block_buf);

                exfs_journal_commit(vol);
                if (g_exfs_crash_point == 4) exfs_debug_crash_now();

                for (int db = 0; db < EXFS_DIRECT_BLOCKS; db++) {
                    if (cin.direct[db]) exfs_free_block(vol, cin.direct[db]);
                }
                if (cin.indirect) {
                    static uint8_t ind_buf[EXFS_BLOCK_SIZE];
                    if (exfs_read_block(vol, cin.indirect, ind_buf)) {
                        uint64_t *ptrs = (uint64_t *)ind_buf;
                        for (uint64_t pi = 0; pi < EXFS_INDIRECT_PTRS; pi++) {
                            if (ptrs[pi]) exfs_free_block(vol, ptrs[pi]);
                        }
                    }
                    exfs_free_block(vol, cin.indirect);
                }
                /* double_indirect: directories can now grow past 12
                 * blocks the same way files do (see EXFS_MAX_DIR_BLOCKS
                 * above), so a directory inode genuinely can have this
                 * populated -- free every leaf pointer block, then the
                 * level-1 table itself, same as exfs_op_unlink does
                 * for files. */
                if (cin.double_indirect) {
                    static uint8_t l1_buf[EXFS_BLOCK_SIZE];
                    if (exfs_read_block(vol, cin.double_indirect, l1_buf)) {
                        uint64_t *l1ptrs = (uint64_t *)l1_buf;
                        for (uint64_t i1 = 0; i1 < EXFS_INDIRECT_PTRS; i1++) {
                            if (!l1ptrs[i1]) continue;
                            static uint8_t l2_buf[EXFS_BLOCK_SIZE];
                            if (exfs_read_block(vol, l1ptrs[i1], l2_buf)) {
                                uint64_t *l2ptrs = (uint64_t *)l2_buf;
                                for (uint64_t i2 = 0; i2 < EXFS_INDIRECT_PTRS; i2++) {
                                    if (l2ptrs[i2]) exfs_free_block(vol, l2ptrs[i2]);
                                }
                            }
                            exfs_free_block(vol, l1ptrs[i1]);
                        }
                    }
                    exfs_free_block(vol, cin.double_indirect);
                }

                return 0;
            }

            off += sizeof(exfs_dirent_t);
        }
    }

    return -1; /* not found */
}

/*
 * exfs_op_rename — move/rename a directory entry from (old_dir,
 * old_name) to (new_dir, new_name). old_dir and new_dir may be the
 * same directory (plain rename) or different ones (move) — vfs_rename()
 * has already confirmed both live on this same volume before calling
 */
static int exfs_op_rename(vfs_node_t *old_dir, const char *old_name,
                           vfs_node_t *new_dir, const char *new_name)
{
    exfs_node_data_t *ond = (exfs_node_data_t *)old_dir->fs_data;
    exfs_node_data_t *nnd = (exfs_node_data_t *)new_dir->fs_data;
    exfs_volume_t    *vol = ond->vol;
    if (vol != nnd->vol) return -1; /* cross-volume — vfs_rename() should
                                      * already have refused this via the
                                      * ops-table check, this is a
                                      * defense-in-depth backstop */

    uint64_t old_len = strlen(old_name), new_len = strlen(new_name);
    if (!old_len || old_len > EXFS_NAME_MAX) return -1;
    if (!new_len || new_len > EXFS_NAME_MAX) return -1;

    /* Locate the source entry */
    exfs_inode_t *odin = &ond->inode;
    static uint8_t old_buf[EXFS_BLOCK_SIZE];
    uint64_t src_phys_block = 0; /* physical block number, not a
                                   * direct[] array index -- once
                                   * directories can span indirect/
                                   * double-indirect blocks (see
                                   * EXFS_MAX_DIR_BLOCKS) there's no
                                   * single array to index into. */
    uint64_t src_off      = 0;
    exfs_dirent_t src_de;
    bool found = false;

    for (uint64_t b = 0; b < EXFS_MAX_DIR_BLOCKS && !found; b++) {
        bool unused_dirty = false;
        uint64_t blk = exfs_resolve_block(vol, odin, b, false, &unused_dirty);
        if (!blk) break;
        if (!exfs_read_block(vol, blk, old_buf)) return -1;
        uint64_t off = 0;
        while (off + sizeof(exfs_dirent_t) <= EXFS_BLOCK_SIZE) {
            exfs_dirent_t *de = (exfs_dirent_t *)(old_buf + off);
            if (de->inode_num && de->name_len == (uint16_t)old_len &&
                memcmp(de->name, old_name, old_len) == 0)
            {
                src_phys_block = blk;
                src_off        = off;
                src_de         = *de;
                found          = true;
                break;
            }
            off += sizeof(exfs_dirent_t);
        }
    }
    if (!found) return -1; /* ENOENT */

    /* Bail out early (no-op success) if this is a rename onto the
     * exact same (dir, name) pair -- if we let it fall through, the
     * "does the destination already exist" check below would find
     * the source's own entry and try to unlink it out from under us. */
    if (old_dir == new_dir && old_len == new_len &&
        memcmp(old_name, new_name, old_len) == 0)
        return 0;

    /* If the destination name already exists: refuse for a directory,
     * remove-then-continue for a file (matches POSIX rename()). Doing
     * this lookup/unlink *before* opening our own journal transaction
     * keeps it as its own independent, already-atomic operation
     * (exfs_op_unlink journals itself) rather than nesting transactions
     * (the journal is single-slot/single-transaction — see exfs.h). */
    vfs_node_t *existing = exfs_op_lookup(new_dir, new_name);
    if (existing) {
        bool is_dir = (existing->type == VFS_DIRECTORY);
        kfree(existing->fs_data);
        kfree(existing);
        if (is_dir) return -1;
        if (exfs_op_unlink(new_dir, new_name) != 0) return -1;
    }

    /* Find an empty dirent slot in the destination directory. Read-only
     * pass — nothing is mutated or staged yet. */
    exfs_inode_t *ndin = &nnd->inode;
    uint64_t dst_block = 0, dst_off = 0;
    bool      need_new_block = false;
    uint64_t  new_block_logical = 0;
    static uint8_t dst_buf[EXFS_BLOCK_SIZE];

    for (uint64_t b = 0; b < EXFS_MAX_DIR_BLOCKS; b++) {
        bool unused_dirty = false;
        uint64_t blk = exfs_resolve_block(vol, ndin, b, false, &unused_dirty);
        if (!blk) { need_new_block = true; new_block_logical = b; break; }
        if (!exfs_read_block(vol, blk, dst_buf)) return -1;
        uint64_t off = 0;
        bool slot_found = false;
        while (off + sizeof(exfs_dirent_t) <= EXFS_BLOCK_SIZE) {
            exfs_dirent_t *de = (exfs_dirent_t *)(dst_buf + off);
            if (!de->inode_num) { dst_block = blk; dst_off = off; slot_found = true; break; }
            off += sizeof(exfs_dirent_t);
        }
        if (slot_found) break;
    }
    if (!dst_block && !need_new_block) return -1; /* directory full */

    /*
 * destination dirent. One transaction — a crash between the two
 * must never leave the entry visible in neither directory (lost)
 * or in both (duplicated).
 * When source and destination land in the *same* physical block
 */
    exfs_journal_begin(vol);

    if (need_new_block) {
        bool dirty = false;
        uint64_t nb = exfs_resolve_block(vol, ndin, new_block_logical, true, &dirty);
        if (!nb) { exfs_journal_commit(vol); return -1; }
        if (dirty) exfs_write_inode(vol, nnd->inode_num, ndin);
        memset(dst_buf, 0, EXFS_BLOCK_SIZE);
        dst_block = nb;
        dst_off   = 0;
    } else if (dst_block != src_phys_block) {
        /* Different block from the source -- re-read fresh now that
         * we're inside the transaction, in case anything upstream
         * (the destination-overwrite unlink above) touched it. */
        if (!exfs_read_block(vol, dst_block, dst_buf)) {
            exfs_journal_commit(vol); return -1;
        }
    }
    /* else: dst_block == the source's own block, and dst_buf already
     * holds that exact block's current content from the scan above --
     * reuse it as-is so the source-removal edit below and the
     * insertion edit both land in this one copy. */

    if (dst_block == src_phys_block) {
        /* Same block: clear the source slot and write the new entry
         * in this single buffer, then persist it once. */
        memset(dst_buf + src_off, 0, sizeof(exfs_dirent_t));
    } else {
        /* Different block: clear the source slot in its own buffer
         * and stage that separately. */
        static uint8_t src_buf[EXFS_BLOCK_SIZE];
        if (!exfs_read_block(vol, src_phys_block, src_buf)) {
            exfs_journal_commit(vol); return -1;
        }
        memset(src_buf + src_off, 0, sizeof(exfs_dirent_t));
        exfs_meta_write_block(vol, src_phys_block, src_buf);
    }

    exfs_dirent_t *de = (exfs_dirent_t *)(dst_buf + dst_off);
    memset(de, 0, sizeof(exfs_dirent_t));
    de->inode_num = src_de.inode_num;
    de->file_type = src_de.file_type;
    de->name_len  = (uint16_t)new_len;
    memcpy(de->name, new_name, new_len);
    exfs_meta_write_block(vol, dst_block, dst_buf);

    exfs_journal_commit(vol);
    return 0;
}


static const vfs_ops_t g_exfs_ops = {
    .open   = exfs_op_open,
    .close  = exfs_op_close,
    .read   = exfs_op_read,
    .write  = exfs_op_write,
    .lookup  = exfs_op_lookup,
    .readdir = exfs_op_readdir,
    .create  = exfs_op_create,
    .unlink  = exfs_op_unlink,
    .rmdir   = exfs_op_rmdir,
    .chmod   = exfs_op_chmod,
    .rename  = exfs_op_rename,
    .symlink = exfs_op_symlink,
    .chgrp   = exfs_op_chgrp,
};






/*  Mount   */


vfs_node_t *exfs_mount(block_device_t *dev, uint32_t lba_base)
{
    if (!dev || !dev->read_sector || !dev->write_sector) {
        serial_print("[ExFS] mount: invalid block device\n");
        return NULL;
    }

    exfs_volume_t *vol = kzalloc(sizeof(exfs_volume_t));
    if (!vol) return NULL;

    vol->dev          = dev;
    vol->dev_lba_base = lba_base;

    /* Cache allocation failure is non-fatal — exfs_read_block/
     * exfs_write_block both check vol->cache for NULL and just skip
     * caching (falling back to always hitting disk) if it's not
     * available, so a low-memory boot can still mount successfully,
     * just without the speedup. */
    vol->cache = kzalloc(sizeof(exfs_cache_entry_t) * EXFS_CACHE_SIZE);
    vol->cache_next_evict = 0;

    /* Read superblock */
    static uint8_t sb_buf[EXFS_BLOCK_SIZE];
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

    /* Allocate and load the in-memory block bitmap. Sized in whole
     * EXFS_BLOCK_SIZE blocks (not just the minimal ceil(total_blocks/8)
     * bytes a bit count would need) because flush_bitmap()/the load
     * below do whole-block I/O across vol->sb.bitmap_size blocks --
     * an allocation smaller than bitmap_size*EXFS_BLOCK_SIZE would
     * let that read/write past the end of this buffer. */
    uint64_t bitmap_size = vol->sb.bitmap_size ? vol->sb.bitmap_size : 1;
    uint64_t bitmap_bytes = bitmap_size * EXFS_BLOCK_SIZE;
    vol->block_bitmap = kzalloc(bitmap_bytes);
    if (!vol->block_bitmap) {
        serial_print("[ExFS] mount: bitmap alloc failed\n");
        kfree(vol);
        return NULL;
    }
    /* Load the on-disk bitmap (bitmap_size blocks) into memory */
    bool bitmap_ok = true;
    for (uint64_t i = 0; i < bitmap_size; i++) {
        bitmap_ok &= exfs_read_block(vol, vol->sb.block_bitmap_block + i,
                                     vol->block_bitmap + i * EXFS_BLOCK_SIZE);
    }
    if (!bitmap_ok) {
        serial_print("[ExFS] mount: bitmap read failed\n");
        kfree(vol->block_bitmap);
        kfree(vol);
        return NULL;
    }
    /* Ensure all metadata blocks are marked used in the bitmap */
    for (uint64_t b = 0; b < vol->sb.data_start_block; b++)
        bitmap_set(vol, b);

    /* Journal transaction staging buffer. Allocated separately from
     * exfs_volume_t (see the struct comment in exfs.h) — failure here
     * is non-fatal, same reasoning as the block cache above: every
     * journal call site falls back to a direct (non-atomic) write when
     * vol->jtxn_data is NULL, so a low-memory boot still mounts. */
    vol->jtxn_data = kzalloc(EXFS_JOURNAL_MAX_BLOCKS * EXFS_BLOCK_SIZE);
    vol->jtxn_active = false;
    vol->jtxn_count  = 0;

    /* Replay any committed-but-not-fully-applied transaction left by
     * an unclean shutdown, before this volume is handed to the VFS. */
    exfs_journal_replay(vol);

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
    root->mode    = rnd->inode.mode;
    root->owner_uid = rnd->inode.owner_uid;

    serial_print("[ExFS] Root mounted\n");
    return root;
}


/*  Format  */


bool exfs_format(block_device_t *dev, uint32_t lba_base, uint64_t total_blocks)
{
    if (!dev || !dev->read_sector || !dev->write_sector) {
        serial_print("[ExFS] format: invalid block device\n");
        return false;
    }

    uint64_t inode_blocks = (EXFS_MAX_INODES * EXFS_INODE_SIZE
                           + EXFS_BLOCK_SIZE - 1) / EXFS_BLOCK_SIZE;
    uint64_t block_bitmap_block = 1;
    /*
 * the volume, BITS_PER_BLK (32768) bits per block. A volume of
 * up to 32768 blocks (128 MiB at 4 KiB blocks) still fits in the
 * original single block; past that, more are reserved here so
 * every block on the volume has a bit to represent it -- this is
 */
    uint64_t bitmap_size = (total_blocks + BITS_PER_BLK - 1) / BITS_PER_BLK;
    if (bitmap_size < 1) bitmap_size = 1;
    uint64_t inode_table_block = block_bitmap_block + bitmap_size;
    uint64_t provenance_size = 16;
    /* Journal region: 1 header block + EXFS_JOURNAL_MAX_BLOCKS data
     * slots (see EXFS_JOURNAL_MAGIC in exfs.h). Placed right before
     * the provenance region, at the high end of the volume, same as
     * provenance already was. */
    uint64_t journal_size = 1 + EXFS_JOURNAL_MAX_BLOCKS;
    if (total_blocks <= inode_table_block + inode_blocks
                       + provenance_size + journal_size)
        return false;

    uint64_t data_start_block = inode_table_block + inode_blocks;
    uint64_t provenance_block = total_blocks - provenance_size;
    uint64_t journal_block    = provenance_block - journal_size;

    exfs_volume_t vol;
    memset(&vol, 0, sizeof(vol));
    vol.dev          = dev;
    vol.dev_lba_base = lba_base;

    static uint8_t zero_block[EXFS_BLOCK_SIZE];
    memset(zero_block, 0, EXFS_BLOCK_SIZE);

    /* Superblock at block 0 */
    exfs_superblock_t *sb = &vol.sb;
    sb->magic              = EXFS_MAGIC;
    sb->version            = EXFS_VERSION;
    sb->total_blocks       = total_blocks;
    sb->free_blocks        = total_blocks - data_start_block
                            - provenance_size - journal_size;
    sb->total_inodes       = EXFS_MAX_INODES;
    sb->free_inodes        = EXFS_MAX_INODES - 1;
    sb->inode_table_block  = inode_table_block;
    sb->block_bitmap_block = block_bitmap_block;
    sb->bitmap_size        = bitmap_size;
    sb->data_start_block   = data_start_block;
    sb->provenance_block   = provenance_block;
    sb->provenance_size    = provenance_size;
    sb->journal_block      = journal_block;
    sb->journal_size       = journal_size;

    static uint8_t sb_buf[EXFS_BLOCK_SIZE];
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

    /* Block bitmap — mark metadata blocks as used, one output block
     * at a time (bitmap_size blocks total, computed above). Building
     * it this way rather than into one big buffer keeps this
     * bounded regardless of volume size. */
    for (uint64_t bb = 0; bb < bitmap_size; bb++) {
        static uint8_t bitmap_buf[EXFS_BLOCK_SIZE];
        memset(bitmap_buf, 0, EXFS_BLOCK_SIZE);
        uint64_t base = bb * BITS_PER_BLK;
        uint64_t lim  = base + BITS_PER_BLK;
        if (lim > sb->total_blocks) lim = sb->total_blocks;

        for (uint64_t b = base; b < lim; b++) {
            bool used = (b < sb->data_start_block) ||
                        (b >= sb->provenance_block && b < sb->total_blocks) ||
                        (b >= sb->journal_block && b < sb->journal_block + sb->journal_size);
            if (used) {
                uint64_t local = b - base;
                bitmap_buf[local / 8] |= (uint8_t)(1u << (local % 8));
            }
        }
        if (!exfs_write_block(&vol, block_bitmap_block + bb, bitmap_buf))
            return false;
    }

    /* Zero the journal region (header block's magic == 0 means "no
     * committed transaction to replay" — see exfs_journal_replay). */
    for (uint64_t b = 0; b < journal_size; b++) {
        if (!exfs_write_block(&vol, journal_block + b, zero_block))
            return false;
    }

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