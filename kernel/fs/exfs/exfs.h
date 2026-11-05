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
#define BITS_PER_BYTE        8
#define BITS_PER_BLK         (EXFS_BLOCK_SIZE * BITS_PER_BYTE)

/* Indirect block pointers */
#define EXFS_INDIRECT_PTRS   (EXFS_BLOCK_SIZE / sizeof(uint64_t))

/* Maximum directory logical blocks */
#define EXFS_MAX_DIR_BLOCKS  (EXFS_DIRECT_BLOCKS + EXFS_INDIRECT_PTRS \
                             + EXFS_INDIRECT_PTRS * EXFS_INDIRECT_PTRS)

/* Metadata journal (ordered mode) */
#define EXFS_JOURNAL_MAGIC      0x45584a4c /* "EXJL" */
#define EXFS_JOURNAL_MAX_BLOCKS 16
/* Crash test point 4 */

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

    /* Journal location and size */
    uint64_t journal_block;
    uint64_t journal_size;

    /* Block bitmap blocks */
    uint64_t bitmap_size;

    uint8_t  reserved[3976];         /* Padding to EXFS_BLOCK_SIZE */
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
    /* Owner UID */
    uint32_t owner_uid;
    /* Owning group ID -- see process_t.gid in kernel/proc/process.h.
     * Same carve-out reasoning as owner_uid just above: carved from
     * what was reserved[12], so a pre-existing file just reads this
     * as 0 (GID_ROOT) -- "owned by the root group", a safe default,
     * not "bypass group checks". */
    uint32_t group_gid;
    uint8_t  reserved[8];
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

/* TEST ONLY -- see the function's own comment in exfs.c. Exposed for
 * SYS_DEBUG_EXFS_CORRUPT (kernel/syscall/table.c). */
int exfs_debug_corrupt_file(vfs_node_t *node);

/* Format a block device with a fresh ExFS volume */
bool exfs_format(struct block_device *dev, uint32_t lba_base,
                  uint64_t total_blocks);