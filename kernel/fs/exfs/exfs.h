#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "../vfs/vfs.h"
#include "../../drivers/blockdev.h"

#define EXFS_MAGIC           0x45584653   /* "EXFS" */
#define EXFS_VERSION         1
#define EXFS_BLOCK_SIZE      4096
#define EXFS_INODE_SIZE      256
#define EXFS_NAME_MAX        255
#define EXFS_DIRECT_BLOCKS   12
#define EXFS_MAX_INODES      4096

/* Pointers per indirect block: EXFS_BLOCK_SIZE / sizeof(uint64_t).
 * Single indirect reaches EXFS_INDIRECT_PTRS more blocks; double
 * indirect reaches EXFS_INDIRECT_PTRS^2 more on top of that. At 4096
 * byte blocks that's 512 and 262144 respectively -- max file size via
 * direct+indirect+double_indirect is (12 + 512 + 512*512) * 4096 =
 * ~1.03 GiB. triple_indirect exists in the on-disk inode but is not
 * wired up yet (no volume in this project is anywhere near large
 * enough for it to matter today; left as a documented gap, not
 * silently broken). */
#define EXFS_INDIRECT_PTRS   (EXFS_BLOCK_SIZE / sizeof(uint64_t))

/* Write-ahead metadata journal (crash consistency for directory
 * entries, inodes, and indirect/double-indirect pointer blocks).
 * Single-slot, single-transaction-at-a-time -- matches the fact that
 * nothing in this kernel takes a filesystem-wide lock around a
 * multi-block ExFS operation yet, so there is only ever one logical
 * transaction in flight. File *data* blocks are deliberately NOT
 * journaled (ordered-mode, like ext3's default): data is written to
 * its final location first, then the metadata that points to it is
 * journaled and committed. Worst case on a crash mid-write is a
 * dangling allocated-but-unreferenced data block (a leak, recoverable
 * by a future fsck), never a corrupted directory tree or a inode
 * pointing at half-written metadata. Block-bitmap updates are also
 * applied immediately, non-transactionally, for the same reason --
 * worst case is a leaked block, not corruption. */
#define EXFS_JOURNAL_MAGIC      0x45584a4c /* "EXJL" */
#define EXFS_JOURNAL_MAX_BLOCKS 16

/* Provenance operation codes */
#define PROV_OP_CREATE  0x01
#define PROV_OP_READ    0x02
#define PROV_OP_WRITE   0x03
#define PROV_OP_DELETE  0x04
#define PROV_OP_EXEC    0x05

/*
 * exfs_superblock_t — first block of the partition.
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint64_t total_blocks;
    uint64_t free_blocks;
    uint64_t total_inodes;
    uint64_t free_inodes;
    uint64_t inode_table_block;      /* block index of inode table */
    uint64_t block_bitmap_block;     /* block index of block bitmap */
    uint64_t data_start_block;       /* first usable data block */
    uint64_t provenance_block;       /* start of provenance region */
    uint64_t provenance_size;        /* size in blocks */
    uint8_t  fs_uuid[16];

    /* Metadata journal region: journal_block is the header block;
     * journal_block+1 .. journal_block+EXFS_JOURNAL_MAX_BLOCKS hold
     * staged block data for the (single) in-flight transaction. See
     * EXFS_JOURNAL_MAGIC above for the crash-consistency scheme.
     * Carved out of what used to be reserved[4000] rather than
     * appended, same reasoning/safety as owner_uid in exfs_inode_t
     * below: the on-disk stride is EXFS_BLOCK_SIZE for the
     * superblock (always block 0, always read/written whole), so
     * this is safe to add here. journal_size == 0 means "volume
     * formatted before journaling existed" -- exfs_mount/format
     * treat that as "journaling disabled for this volume" rather
     * than an error, so pre-existing formatted volumes keep working
     * exactly as before (just without crash-atomicity), matching
     * this codebase's established backward-compatibility pattern. */
    uint64_t journal_block;
    uint64_t journal_size;

    uint8_t  reserved[3984];         /* pad to EXFS_BLOCK_SIZE exactly:
                                       * 4+4+8*9+16+8+8+3984 = 4096. (Was
                                       * 4008, making the struct 4104 bytes —
                                       * 8 bytes larger than EXFS_BLOCK_SIZE,
                                       * which made exfs_mount's memcpy read
                                       * 8 bytes past its 4096-byte stack
                                       * buffer on every boot. journal_block/
                                       * journal_size above took 16 of those
                                       * spare bytes.) */
} exfs_superblock_t;

/*
 * exfs_inode_t — 256-byte on-disk inode.
 */
typedef struct __attribute__((packed)) {
    uint64_t size;
    uint64_t created_at;
    uint64_t modified_at;
    uint32_t creator_pid;
    uint32_t mode;                   /* permission bits */
    uint64_t direct[EXFS_DIRECT_BLOCKS];
    uint64_t indirect;
    uint64_t double_indirect;
    uint64_t triple_indirect;
    uint64_t prov_head;              /* offset of first provenance record */
    uint32_t prov_count;
    uint8_t  block_hash[8];          /* first 8 bytes of BLAKE3 of last write */
    /* Owning user ID -- see process_t.uid in kernel/proc/process.h for
     * the full explanation of what this does and doesn't mean yet
     * (no real login/auth exists). Carved out of what used to be
     * reserved[16] rather than appended, so this is the ONE field
     * that changes the on-disk struct layout -- safe because the
     * on-disk stride is EXFS_INODE_SIZE (256, a fixed macro), not
     * sizeof(exfs_inode_t), so pre-existing formatted volumes just
     * read this as 0 (UID_ROOT) for every existing file, which is a
     * safe default: "owned by root", not "owned by nobody, bypass
     * permission checks". */
    uint32_t owner_uid;
    uint8_t  reserved[12];
} exfs_inode_t;

/*
 * exfs_prov_record_t — one provenance entry, kernel-written only.
 * These are stored in a dedicated append-only region.
 * User processes cannot write to this region — VMM enforces it.
 */
typedef struct __attribute__((packed)) {
    uint64_t timestamp;
    uint32_t pid;
    uint8_t  operation;              /* PROV_OP_* */
    uint8_t  _pad[3];
    uint64_t cap_upper;              /* capability token used */
    uint64_t file_offset;
    uint64_t length;
} exfs_prov_record_t;

/*
 * exfs_dirent_t — directory entry.
 */
typedef struct __attribute__((packed)) {
    uint64_t inode_num;
    uint16_t name_len;
    uint8_t  file_type;              /* 0=file, 1=dir, 2=device */
    uint8_t  _pad;
    char     name[EXFS_NAME_MAX + 1];
} exfs_dirent_t;

#define EXFS_CACHE_SIZE 64  /* 64 * 4096 = 256 KiB cache */

typedef struct {
    uint64_t block_idx;
    bool     valid;
    uint8_t  data[EXFS_BLOCK_SIZE];
} exfs_cache_entry_t;

/*
 * In-memory ExFS volume descriptor.
 */
typedef struct {
    exfs_superblock_t sb;
    uint8_t          *block_bitmap;  /* one bit per block */
    struct block_device *dev;        /* backing storage (ata0, usb0, ...) */
    uint32_t          dev_lba_base;  /* LBA of first sector */
    bool              dirty;

    /* Write-through block cache — every real disk block read before
     * this was a full PIO ATA read (8 sectors) even for blocks
     * re-read moments earlier (e.g. re-walking the same directory or
     * inode-table block on every lookup). Write-through (not write-
     * back) so there's no crash-consistency risk from caching: writes
     * always hit disk immediately, the cache is purely a read
     * accelerator. See exfs_read_block/exfs_write_block. */
    exfs_cache_entry_t *cache;
    uint32_t             cache_next_evict;

    /* Metadata journal transaction staging area (see EXFS_JOURNAL_MAGIC
     * in this header for the scheme). kzalloc'd separately from the
     * rest of exfs_volume_t so a mounted-but-idle volume doesn't carry
     * this cost inline in every allocation of the struct; only touched
     * while jtxn_active is true. */
    bool      jtxn_active;
    uint32_t  jtxn_count;
    uint64_t  jtxn_blocks[EXFS_JOURNAL_MAX_BLOCKS];
    uint8_t (*jtxn_data)[EXFS_BLOCK_SIZE];
} exfs_volume_t;

/* Initialize ExFS on a block device starting at lba_base */
vfs_node_t *exfs_mount(struct block_device *dev, uint32_t lba_base);

/* Crash-injection test hook — see the g_exfs_crash_point comment in
 * exfs.c. point: 0 = disabled (normal operation), 1/2/3 = halt at one
 * of the three points inside the next exfs_journal_commit() that
 * matter for the journal's crash-consistency argument. Exposed for
 * SYS_DEBUG_EXFS_CRASH (kernel/syscall/table.c) — not meant to be
 * called from anywhere except a deliberate crash-recovery test. */
void exfs_debug_arm_crash(int point);

/* Format a block device with a fresh ExFS volume */
bool exfs_format(struct block_device *dev, uint32_t lba_base,
                  uint64_t total_blocks);