#include "journal.h"
#include "../../drivers/serial.h"
#include "../../mm/kmalloc.h"
#include <string.h>


/*
 * Crash injection hook for testing journal replay idempotency.
 * Armed via SYS_DEBUG_EXFS_CRASH:
 *   1 = pre-commit (slots written, before header commit)
 *   2 = post-commit (header written, before block update)
 *   3 = post-apply (blocks written, before header clear)
 */
static volatile int g_exfs_crash_point = 0;

void exfs_debug_arm_crash(int point)
{
    g_exfs_crash_point = point;
    serial_print("[ExFS] CRASH TEST: armed, crash point=");
    serial_printhex((uint64_t)point);
    serial_print("\n");
}

static void exfs_debug_crash_now(void)
{
    serial_print("[ExFS] CRASH TEST: halting now (armed crash point "
                 "reached)\n");
    /* Immediate triple fault: no cleanup, no delay, no cli-then-wait
     * like a graceful reboot -- this is meant to look exactly like
     * a power loss, i.e. nothing else gets to run after this. */
    static const struct __attribute__((packed)) {
        uint16_t limit; uint64_t base;
    } null_idt = { 0, 0 };
    __asm__ volatile ("lidt %0\n int $0x3\n" :: "m"(null_idt));
    for (;;) { __asm__ volatile ("hlt"); }
}

/* Forward declarations: exfs_write_inode() needs to route through the
 * journal-aware metadata writer, but the journal machinery itself
 * (defined further down, right after exfs_write_inode) also calls
 * exfs_write_inode() indirectly via callers throughout this file — so
 * neither can textually come first without one of them forward-
 * declaring the other. */
bool exfs_meta_write_block(exfs_volume_t *vol, uint64_t block_idx,
                                   const void *data);
bool exfs_txn_read_block(exfs_volume_t *vol, uint64_t block_idx,
                                 void *buf);


/*  Low-level block I/O                                                 */


bool exfs_read_block(exfs_volume_t *vol, uint64_t block_idx,
                             void *buf)
{
    /* Cache check first — avoids a full PIO ATA read (8 sectors) for
     * blocks that were recently read or written. */
    if (vol->cache) {
        for (uint32_t i = 0; i < EXFS_CACHE_SIZE; i++) {
            if (vol->cache[i].valid && vol->cache[i].block_idx == block_idx) {
                memcpy(buf, vol->cache[i].data, EXFS_BLOCK_SIZE);
                return true;
            }
        }
    }

    uint32_t base_lba = vol->dev_lba_base + (uint32_t)(block_idx * 8);
    uint8_t *dst      = (uint8_t *)buf;
    for (int s = 0; s < 8; s++) {
        if (!vol->dev->read_sector(vol->dev, base_lba + (uint32_t)s, dst)) {
            serial_print("[ExFS] read_block failed at LBA ");
            serial_printhex((uint64_t)(base_lba + s));
            serial_print("\n");
            return false;
        }
        dst += BLOCKDEV_SECTOR_SIZE;
    }

    /* Populate the cache (simple round-robin eviction — good enough
     * for a 64-entry cache; no access-frequency tracking needed). */
    if (vol->cache) {
        uint32_t slot = vol->cache_next_evict;
        vol->cache_next_evict = (slot + 1) % EXFS_CACHE_SIZE;
        vol->cache[slot].block_idx = block_idx;
        vol->cache[slot].valid     = true;
        memcpy(vol->cache[slot].data, buf, EXFS_BLOCK_SIZE);
    }

    return true;
}

bool exfs_write_block(exfs_volume_t *vol, uint64_t block_idx,
                              const void *buf)
{
    uint32_t       base_lba = vol->dev_lba_base + (uint32_t)(block_idx * 8);
    const uint8_t *src      = (const uint8_t *)buf;
    for (int s = 0; s < 8; s++) {
        if (!vol->dev->write_sector(vol->dev, base_lba + (uint32_t)s, src)) {
            serial_print("[ExFS] write_block failed at LBA ");
            serial_printhex((uint64_t)(base_lba + s));
            serial_print("\n");
            return false;
        }
        src += BLOCKDEV_SECTOR_SIZE;
    }

    /* Write-through: keep any cached copy of this block in sync (or
     * insert it) so a subsequent read doesn't return stale data. */
    if (vol->cache) {
        int slot = -1;
        for (uint32_t i = 0; i < EXFS_CACHE_SIZE; i++) {
            if (vol->cache[i].valid && vol->cache[i].block_idx == block_idx) {
                slot = (int)i;
                break;
            }
        }
        if (slot < 0) {
            slot = (int)vol->cache_next_evict;
            vol->cache_next_evict = ((uint32_t)slot + 1) % EXFS_CACHE_SIZE;
        }
        vol->cache[slot].block_idx = block_idx;
        vol->cache[slot].valid     = true;
        memcpy(vol->cache[slot].data, buf, EXFS_BLOCK_SIZE);
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
 * flush_bitmap — write the in-memory bitmap back to disk, across
 * however many blocks it spans (vol->sb.bitmap_size, defaulting to 1
 * for a legacy volume formatted before multi-block bitmaps existed
 * -- see bitmap_size's comment in exfs.h).
 */
static bool flush_bitmap(exfs_volume_t *vol)
{
    uint64_t nblocks = vol->sb.bitmap_size ? vol->sb.bitmap_size : 1;
    bool ok = true;
    for (uint64_t i = 0; i < nblocks; i++) {
        ok &= exfs_write_block(vol, vol->sb.block_bitmap_block + i,
                               vol->block_bitmap + i * EXFS_BLOCK_SIZE);
    }
    return ok;
}

/*
 * exfs_alloc_block — find a free data block, mark it used, return its index.
 * Returns 0 on failure (0 is the superblock — never a valid data block).
 */
uint64_t exfs_alloc_block(exfs_volume_t *vol)
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
void exfs_free_block(exfs_volume_t *vol, uint64_t blk)
{
    if (blk < vol->sb.data_start_block || blk >= vol->sb.total_blocks) return;
    if (!bitmap_get(vol, blk)) return;  /* already free — ignore */
    bitmap_clear(vol, blk);
    vol->sb.free_blocks++;
    flush_bitmap(vol);
}


/*  Inode read / write */


bool exfs_read_inode(exfs_volume_t *vol, uint64_t inode_num,
                             exfs_inode_t *out)
{
    /* Inode table: multiple inodes per block */
    uint64_t inodes_per_block = EXFS_BLOCK_SIZE / EXFS_INODE_SIZE;
    uint64_t block_idx = vol->sb.inode_table_block
                       + inode_num / inodes_per_block;
    uint64_t offset    = (inode_num % inodes_per_block) * EXFS_INODE_SIZE;

    static uint8_t block_buf[EXFS_BLOCK_SIZE];
    if (!exfs_read_block(vol, block_idx, block_buf)) return false;

    memcpy(out, block_buf + offset, sizeof(exfs_inode_t));
    return true;
}

bool exfs_write_inode(exfs_volume_t *vol, uint64_t inode_num,
                              const exfs_inode_t *in)
{
    uint64_t inodes_per_block = EXFS_BLOCK_SIZE / EXFS_INODE_SIZE;
    uint64_t block_idx = vol->sb.inode_table_block
                       + inode_num / inodes_per_block;
    uint64_t offset    = (inode_num % inodes_per_block) * EXFS_INODE_SIZE;

    static uint8_t block_buf[EXFS_BLOCK_SIZE];
    if (!exfs_txn_read_block(vol, block_idx, block_buf)) return false;

    memcpy(block_buf + offset, in, sizeof(exfs_inode_t));
    return exfs_meta_write_block(vol, block_idx, block_buf);
}


/*  Metadata journal — crash-consistent redo log                        */
/*                                                                       */
/*  See EXFS_JOURNAL_MAGIC in exfs.h for the design/scope. Usage        */
/*  pattern throughout this file:                                       */
/*                                                                       */
/*      exfs_journal_begin(vol);                                        */
/*      ... exfs_meta_write_block()/exfs_write_inode() calls, which     */
/*          transparently stage into the transaction while it's active */
/*      exfs_journal_commit(vol);   // atomically applies everything    */


/*
 * exfs_journal_stage — record a block's new content as part of the
 * current transaction. If this exact block was already staged earlier
 * in the same transaction, overwrite that staged copy (last write
 * wins) instead of consuming another slot — a single operation can
 */
static bool exfs_journal_stage(exfs_volume_t *vol, uint64_t block_idx,
                                const void *data)
{
    if (!vol->jtxn_data) {
        /* Journal staging buffer failed to allocate at mount time (low
         * memory) — fall back to writing straight through, same as a
         * legacy (journal_size == 0) volume. Loses crash-atomicity but
         * keeps the filesystem usable rather than failing every write. */
        return exfs_write_block(vol, block_idx, data);
    }

    for (uint32_t i = 0; i < vol->jtxn_count; i++) {
        if (vol->jtxn_blocks[i] == block_idx) {
            memcpy(vol->jtxn_data[i], data, EXFS_BLOCK_SIZE);
            return true;
        }
    }

    if (vol->jtxn_count >= EXFS_JOURNAL_MAX_BLOCKS) {
        /* Transaction grew past what the journal can hold — extremely
         * unlikely for any single ExFS operation (create/unlink/rmdir/
         * rename/write each touch only a handful of metadata blocks),
         * but if it ever happens, write straight through rather than
         * silently drop the update. Documented, not fixed: a bigger
         * journal region or a growable one would remove this cap. */
        serial_print("[ExFS] journal: transaction exceeded capacity, "
                      "writing block directly (crash-atomicity not "
                      "guaranteed for this block)\n");
        return exfs_write_block(vol, block_idx, data);
    }

    vol->jtxn_blocks[vol->jtxn_count] = block_idx;
    memcpy(vol->jtxn_data[vol->jtxn_count], data, EXFS_BLOCK_SIZE);
    vol->jtxn_count++;
    return true;
}

/*
 * exfs_meta_write_block — write a metadata block (inode table block,
 * directory block, indirect/double-indirect pointer block). Routes
 * through the journal while a transaction is active; writes straight
 * through otherwise (matches old behavior for any call site outside a
 * begin/commit pair, and for legacy/no-journal volumes).
 */
bool exfs_meta_write_block(exfs_volume_t *vol, uint64_t block_idx,
                                   const void *data)
{
    if (vol->jtxn_active) return exfs_journal_stage(vol, block_idx, data);
    return exfs_write_block(vol, block_idx, data);
}

/*
 * exfs_txn_read_block — read a block's *current logical* content: if
 * it's already been staged earlier in the active transaction, that
 * staged (not-yet-committed-to-disk) copy is authoritative and is
 * returned instead of the on-disk version. Falls back to a normal
 */
bool exfs_txn_read_block(exfs_volume_t *vol, uint64_t block_idx,
                                 void *buf)
{
    if (vol->jtxn_active && vol->jtxn_data) {
        for (uint32_t i = 0; i < vol->jtxn_count; i++) {
            if (vol->jtxn_blocks[i] == block_idx) {
                memcpy(buf, vol->jtxn_data[i], EXFS_BLOCK_SIZE);
                return true;
            }
        }
    }
    return exfs_read_block(vol, block_idx, buf);
}

void exfs_journal_begin(exfs_volume_t *vol)
{
    vol->jtxn_active = true;
    vol->jtxn_count  = 0;
}

/*
 * exfs_journal_commit — atomically apply everything staged since the
 * matching exfs_journal_begin(). Sequence: (1) write every staged
 * block's new content into the journal's data slots, (2) write the
 * journal header last, with a valid magic and the block count/index
 */
bool exfs_journal_commit(exfs_volume_t *vol)
{
    if (!vol->jtxn_active) return true;
    vol->jtxn_active = false;

    if (vol->jtxn_count == 0) return true;

    if (!vol->sb.journal_size || !vol->jtxn_data) {
        /* Legacy volume (formatted before journaling existed) or
         * staging buffer unavailable — apply directly, same as
         * pre-journal behavior. No crash-atomicity, by design for
         * these two cases (see exfs_journal_stage/exfs_mount). */
        bool ok = true;
        for (uint32_t i = 0; i < vol->jtxn_count; i++)
            ok &= exfs_write_block(vol, vol->jtxn_blocks[i], vol->jtxn_data[i]);
        vol->jtxn_count = 0;
        return ok;
    }

    if (vol->jtxn_count > vol->sb.journal_size - 1) {
        serial_print("[ExFS] journal: transaction larger than journal "
                      "region, applying directly (crash-atomicity not "
                      "guaranteed)\n");
        bool ok = true;
        for (uint32_t i = 0; i < vol->jtxn_count; i++)
            ok &= exfs_write_block(vol, vol->jtxn_blocks[i], vol->jtxn_data[i]);
        vol->jtxn_count = 0;
        return ok;
    }

    /* (1) durably record the new content in the journal region */
    for (uint32_t i = 0; i < vol->jtxn_count; i++) {
        if (!exfs_write_block(vol, vol->sb.journal_block + 1 + i,
                               vol->jtxn_data[i])) {
            vol->jtxn_count = 0;
            return false; /* journal write itself failed — nothing was
                            * ever applied to real locations, so the
                            * volume is still in its pre-transaction
                            * (consistent) state. */
        }
    }

    if (g_exfs_crash_point == 1) exfs_debug_crash_now();

    /* (2) commit point */
    typedef struct __attribute__((packed)) {
        uint32_t magic;
        uint32_t block_count;
        uint64_t block_idx[EXFS_JOURNAL_MAX_BLOCKS];
    } jhdr_t;

    static uint8_t hdr_buf[EXFS_BLOCK_SIZE];
    memset(hdr_buf, 0, EXFS_BLOCK_SIZE);
    jhdr_t *hdr = (jhdr_t *)hdr_buf;
    hdr->magic       = EXFS_JOURNAL_MAGIC;
    hdr->block_count = vol->jtxn_count;
    for (uint32_t i = 0; i < vol->jtxn_count; i++)
        hdr->block_idx[i] = vol->jtxn_blocks[i];

    if (!exfs_write_block(vol, vol->sb.journal_block, hdr_buf)) {
        vol->jtxn_count = 0;
        return false;
    }

    if (g_exfs_crash_point == 2) exfs_debug_crash_now();

    /* (3) apply to real locations */
    for (uint32_t i = 0; i < vol->jtxn_count; i++)
        exfs_write_block(vol, vol->jtxn_blocks[i], vol->jtxn_data[i]);

    if (g_exfs_crash_point == 3) exfs_debug_crash_now();

    /* (4) clear the journal */
    memset(hdr_buf, 0, EXFS_BLOCK_SIZE);
    exfs_write_block(vol, vol->sb.journal_block, hdr_buf);

    vol->jtxn_count = 0;
    return true;
}

/*
 * exfs_journal_replay — called once at mount, before the volume is
 * handed back to the VFS. If a committed transaction was left behind
 * by a crash (header has a valid magic), redo it: write every staged
 * block back to its real location, then clear the header. No-op for
 */
void exfs_journal_replay(exfs_volume_t *vol)
{
    if (!vol->sb.journal_size) return;

    typedef struct __attribute__((packed)) {
        uint32_t magic;
        uint32_t block_count;
        uint64_t block_idx[EXFS_JOURNAL_MAX_BLOCKS];
    } jhdr_t;

    static uint8_t hdr_buf[EXFS_BLOCK_SIZE];
    if (!exfs_read_block(vol, vol->sb.journal_block, hdr_buf)) return;

    jhdr_t *hdr = (jhdr_t *)hdr_buf;
    if (hdr->magic != EXFS_JOURNAL_MAGIC) return; /* nothing to replay */

    serial_print("[ExFS] Journal: replaying committed transaction after "
                 "an unclean shutdown\n");

    static uint8_t blk[EXFS_BLOCK_SIZE];
    uint32_t count = hdr->block_count;
    if (count > EXFS_JOURNAL_MAX_BLOCKS) count = EXFS_JOURNAL_MAX_BLOCKS;

    for (uint32_t i = 0; i < count; i++) {
        if (exfs_read_block(vol, vol->sb.journal_block + 1 + i, blk))
            exfs_write_block(vol, hdr->block_idx[i], blk);
    }

    memset(hdr_buf, 0, EXFS_BLOCK_SIZE);
    exfs_write_block(vol, vol->sb.journal_block, hdr_buf);

    serial_print("[ExFS] Journal: replay complete\n");
}

