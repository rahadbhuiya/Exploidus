#include "exfs.h"
#include "../../drivers/serial.h"
#include "../../mm/kmalloc.h"
#include "../../audit/audit.h"
#include "../../proc/process.h"
#include "../../crypto/blake3.h"
#include <string.h>

/*
 * Deterministic crash-injection hook for testing journal recovery
 * (EXFS_JOURNAL_MAGIC above). Timing a real crash by hand against a
 * live QEMU session can't reliably land inside a multi-microsecond
 * window, so this lets a test deliberately halt execution at one of
 * the three points that matter for exfs_journal_commit()'s
 * correctness argument, armed via SYS_DEBUG_EXFS_CRASH from the
 * shell's `crashtest` command:
 *
 *   1 = after the journal's data slots are written, before the
 *       header (commit point) — nothing should be replayed on
 *       reboot; the operation should look like it never happened.
 *   2 = after the header (commit point), before the real blocks are
 *       applied — this is the case the whole journal exists for:
 *       replay must apply the operation on the next mount.
 *   3 = after the real blocks are applied, before the header is
 *       cleared — replay must be safe to run again (idempotent):
 *       the operation already happened, reapplying it must be a
 *       no-op, not corruption.
 *
 * Disabled (0) in normal operation; this file is not conditionally
 * compiled out because the hook is inert (single branch, single
 * global) unless explicitly armed, and keeping it always-present
 * means the exact same kernel binary is what's being tested rather
 * than a special "test build".
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
static bool exfs_meta_write_block(exfs_volume_t *vol, uint64_t block_idx,
                                   const void *data);
static bool exfs_txn_read_block(exfs_volume_t *vol, uint64_t block_idx,
                                 void *buf);


/*  Low-level block I/O                                                 */


static bool exfs_read_block(exfs_volume_t *vol, uint64_t block_idx,
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

static bool exfs_write_block(exfs_volume_t *vol, uint64_t block_idx,
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
static void exfs_free_block(exfs_volume_t *vol, uint64_t blk)
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

    static uint8_t block_buf[EXFS_BLOCK_SIZE];
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
 * legitimately touch the same directory block twice (e.g. remove one
 * entry, add another) and that must not overflow the fixed-size
 * journal.
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
static bool exfs_meta_write_block(exfs_volume_t *vol, uint64_t block_idx,
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
 * exfs_read_block() when there's no active transaction or this block
 * hasn't been touched by it yet.
 *
 * This matters for any read-modify-write against a block that more
 * than one call in the same transaction might touch -- the clearest
 * example is the inode table: EXFS_BLOCK_SIZE / EXFS_INODE_SIZE = 16
 * inodes share one on-disk block, so creating a file in a directory
 * that needs its first data block allocated writes *two* different
 * inodes (the new file's, and the parent directory's, for its updated
 * direct[] pointer) that can easily land in the same block. Without
 * this, exfs_write_inode()'s plain exfs_read_block() would miss the
 * first inode's still-only-staged update, and journal_stage's
 * same-block dedup ("stage again -> overwrite") would silently
 * discard it -- exactly the rename() bug fixed earlier, but for
 * inodes instead of dirents. Caught in testing: creating the first
 * file in a freshly made subdirectory left the new file's own inode
 * effectively blank (mode 0), so opening it for write failed
 * permission checks that a mode of 0 always fails.
 */
static bool exfs_txn_read_block(exfs_volume_t *vol, uint64_t block_idx,
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

static void exfs_journal_begin(exfs_volume_t *vol)
{
    vol->jtxn_active = true;
    vol->jtxn_count  = 0;
}

/*
 * exfs_journal_commit — atomically apply everything staged since the
 * matching exfs_journal_begin(). Sequence: (1) write every staged
 * block's new content into the journal's data slots, (2) write the
 * journal header last, with a valid magic and the block count/index
 * list — this single block write is the commit point: if the kernel
 * crashes before it lands, exfs_journal_replay() at next mount sees no
 * valid header and does nothing, so the real (pre-transaction) blocks
 * are untouched. (3) apply every staged block to its real location.
 * (4) clear the header. Because step 3 writes the exact same final
 * content step 1 already durably recorded, replaying step 3 again
 * after a crash between (3) and (4) is always safe (idempotent) — so
 * is re-replaying an already-fully-applied transaction if the crash
 * landed between (2) and (4) in general.
 */
static bool exfs_journal_commit(exfs_volume_t *vol)
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
 * legacy (journal_size == 0) volumes and for a clean unmount (header
 * already cleared by the last exfs_journal_commit()).
 */
static void exfs_journal_replay(exfs_volume_t *vol)
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


/*  Logical file-block -> disk-block resolution (direct/indirect/       */
/*  double-indirect), shared by read and write                         */


/*
 * exfs_resolve_block — translate a logical block index within a file
 * into a disk block number, walking direct pointers, then single
 * indirect, then double indirect (see EXFS_INDIRECT_PTRS in exfs.h for
 * the addressable range each level covers).
 *
 * If `alloc` is false: returns 0 for a hole (unallocated block) —
 * used by reads, which should treat holes as EOF/end-of-data, same as
 * the original direct-only code did.
 *
 * If `alloc` is true: allocates any missing index blocks and the
 * final data block as needed, staging every index-block write as
 * metadata (so it participates in whatever journal transaction the
 * caller has open). Sets *inode_dirty = true if a *top-level* inode
 * field (direct[i], indirect, or double_indirect) was assigned for
 * the first time — the caller is responsible for persisting the
 * inode itself afterward (exfs_write_inode) when that happens;
 * changes to blocks an existing pointer already points *at* are
 * written here directly and don't require an inode rewrite.
 *
 * Returns 0 on allocation failure (volume full) or if file_block is
 * beyond what direct+indirect+double_indirect can address (triple
 * indirect is not implemented — see EXFS_INDIRECT_PTRS comment).
 */
static uint64_t exfs_resolve_block(exfs_volume_t *vol, exfs_inode_t *in,
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
 * of this field that only covered the bytes touched by the most recent
 * write() call -- that missed corruption in any earlier-written part
 * of the file; this catches corruption anywhere in it.
 *
 * Cost: O(file size) per call, and exfs_op_write() calls this once
 * per write() -- so N sequential small writes to the same file cost
 * O(N^2) total, not O(N). Deliberate, not an oversight: this
 * filesystem already leans toward correctness/detectability over
 * raw throughput (the provenance ring buffer, the journal), and
 * nothing on this kernel currently does the kind of write-heavy
 * workload (databases, log-structured writes) where that would bite.
 * If that changes, incremental hashing would need either an
 * append-only fast path (BLAKE3's streaming API already supports
 * that -- just don't restart from 0) or accepting that only the
 * last-written range is covered, as before.
 */
static void exfs_compute_whole_file_hash(exfs_volume_t *vol,
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
 * on-disk corruption (bit rot, a bad sector, a bug somewhere else
 * entirely) for testing exfs_op_open()'s integrity-hash verification,
 * as opposed to corruption introduced through this filesystem's own
 * write path, which would legitimately update the hash and shouldn't
 * be flagged. Returns 0 on success, -1 if the node isn't a regular
 * ExFS file with at least one byte of content.
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

    /* Integrity check: if this file has ever actually been written to
     * (a populated, non-zero stored block_hash -- see
     * exfs_compute_whole_file_hash()), recompute its whole-file hash
     * now and compare. A mismatch means the on-disk content changed
     * since the hash was last set without going through a normal
     * write() -- log it loudly; this adds active detection on top of
     * the provenance trail that already records legitimate writes.
     * Skipped when the stored hash is all-zero: that's this field's
     * untouched-since-create default (exfs_op_create/exfs_op_symlink
     * memset the new inode to zero), not a genuine "empty file"
     * hash -- BLAKE3 of zero bytes is a specific non-zero digest, so
     * comparing against an unset zero hash would always spuriously
     * mismatch. */
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

        static uint8_t block_buf[EXFS_BLOCK_SIZE];
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

    /* Refuse to create a name that already exists in this directory.
     * Without this check, two dirents with the identical name could
     * both exist (each pointing at a different inode) — e.g. any
     * caller that falls back to create() after a failed open() for an
     * unrelated reason would silently duplicate the file instead of
     * either failing or reusing the existing one. Real filesystems
     * never allow this; POSIX create()/O_CREAT without O_EXCL opens
     * the existing file instead of duplicating it, which callers
     * needing that exact semantic should implement by open()-first,
     * same as today — this only closes the "duplicate name" hole,
     * it doesn't change create()'s own contract of "always makes a
     * new file". */
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
 * data block + the parent's dirent insertion) with one addition: the
 * target string is written into the new inode's first data block
 * directly, in the same transaction, so a crash can't produce a
 * symlink inode with no content or a content block with no inode
 * pointing at it.
 *
 * Target length is capped at EXFS_NAME_MAX (255) bytes -- matches the
 * fixed-size buffer vfs_lookup_impl() reads a link's target into when
 * following it (see VFS_SYMLINK_MAX_DEPTH in vfs.c) -- comfortably
 * fits in the one data block this always allocates, so there's no
 * need to go through the general (indirect-block-capable) write path
 * for this.
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
 *
 * Frees the file's direct blocks and (if present) its indirect block
 * and everything it points to, zeroes the inode so exfs_op_create's
 * free-inode scan (mode==0 && size==0) can reuse it, then clears the
 * directory entry.
 *
 * Only regular files are supported — returns -2 for directories
 * (recursive delete is out of scope) and -1 if the name isn't found.
 *
 * Note: this does not check whether the file is currently open via
 * another fd. Nothing else in this codebase reference-counts open
 * files either, so that is a pre-existing gap, not new with this.
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

                /* Order matters for crash safety: the dirent removal
                 * + inode zeroing happen FIRST, atomically (one
                 * journal transaction), and the child's data blocks
                 * are only freed AFTER that transaction durably
                 * commits. Freeing blocks before removing the dirent
                 * (the previous order) meant a crash in between left
                 * the directory still listing the file, pointing at
                 * an inode whose blocks had already been handed back
                 * to the allocator — a *different* file created in
                 * that window could then be silently corrupted by
                 * whatever still resolved the dangling entry. With
                 * this order, a crash before the commit leaves the
                 * file fully intact (nothing was freed yet); a crash
                 * after the commit but before freeing finishes leaves
                 * at worst a leaked block (bitmap says used, nothing
                 * references it) — the same acceptable, fsck.py-
                 * repairable risk this journal's design doc already
                 * documents for the "no data-block journaling" case,
                 * not new corruption. */
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
 * in. If new_name already exists and is a regular file, it is removed
 * first (POSIX rename() overwrite semantics); if it's a directory,
 * the rename is refused (-1) rather than attempting a merge.
 *
 * This does not update ".." for a moved directory because ExFS
 * directories don't store a real ".." dirent at all (see the comment
 * in vfs_create() about why an on-disk ".." is deliberately never
 * created) — so moving a directory to a new parent needs no such
 * fixup here.
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

    /* The actual move: remove the source dirent, insert the
     * destination dirent. One transaction — a crash between the two
     * must never leave the entry visible in neither directory (lost)
     * or in both (duplicated).
     *
     * When source and destination land in the *same* physical block
     * (always true for a same-directory rename, and possible even
     * across directories if their allocators happened to reuse the
     * same block), both edits are applied to one in-memory copy and
     * written/staged exactly once. Staging the same block twice with
     * two different partial edits would be wrong: the journal's
     * "same block staged again -> overwrite" dedup (see
     * exfs_journal_stage) means only the *second* staged copy would
     * survive, silently discarding whichever edit was staged first --
     * this was an actual bug caught by testing, not a hypothetical. */
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
    uint64_t inode_table_block = 2;
    uint64_t block_bitmap_block = 1;
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

    /* Block bitmap (block 2) — mark metadata blocks as used */
    static uint8_t bitmap_buf[EXFS_BLOCK_SIZE];
    memset(bitmap_buf, 0, EXFS_BLOCK_SIZE);
    /* Blocks 0 .. data_start_block-1 are reserved for metadata */
    for (uint64_t b = 0; b < sb->data_start_block && b < BITS_PER_BLK; b++) {
        bitmap_buf[b / 8] |= (uint8_t)(1u << (b % 8));
    }
    for (uint64_t b = sb->provenance_block;
         b < sb->total_blocks && b < BITS_PER_BLK; b++) {
        bitmap_buf[b / 8] |= (uint8_t)(1u << (b % 8));
    }
    for (uint64_t b = sb->journal_block;
         b < sb->journal_block + sb->journal_size && b < BITS_PER_BLK; b++) {
        bitmap_buf[b / 8] |= (uint8_t)(1u << (b % 8));
    }
    if (!exfs_write_block(&vol, block_bitmap_block, bitmap_buf)) return false;

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