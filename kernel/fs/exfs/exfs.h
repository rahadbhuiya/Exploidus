#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "../vfs/vfs.h"

#define EXFS_MAGIC           0x45584653   /* "EXFS" */
#define EXFS_VERSION         1
#define EXFS_BLOCK_SIZE      4096
#define EXFS_INODE_SIZE      256
#define EXFS_NAME_MAX        255
#define EXFS_DIRECT_BLOCKS   12
#define EXFS_MAX_INODES      4096

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
    uint8_t  reserved[4008];         /* pad to EXFS_BLOCK_SIZE */
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
    uint8_t  reserved[16];
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

/*
 * In-memory ExFS volume descriptor.
 */
typedef struct {
    exfs_superblock_t sb;
    uint8_t          *block_bitmap;  /* one bit per block */
    uint32_t          dev_lba_base;  /* LBA of first sector */
    bool              dirty;
} exfs_volume_t;

/* Initialize ExFS on a block device starting at lba_base */
vfs_node_t *exfs_mount(uint32_t lba_base);

/* Format a block device with a fresh ExFS volume */
bool exfs_format(uint32_t lba_base, uint64_t total_blocks);
