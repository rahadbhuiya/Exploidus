#include "pmm.h"
#include "../drivers/serial.h"
#include <string.h>

static page_frame_t *g_frames      = NULL;
static uint64_t      g_frame_base  = 0;
static uint64_t      g_frame_count = 0;
static uint64_t      g_alloc_start = 0;

static uint64_t g_zone_cursor[ZONE_COUNT];
static uint64_t g_zone_free[ZONE_COUNT];

static inline uint64_t phys_to_idx(uint64_t phys)
{
    return (phys - g_frame_base) >> PAGE_SHIFT;
}

static inline uint64_t idx_to_phys(uint64_t idx)
{
    return g_frame_base + (idx << PAGE_SHIFT);
}

/*  INIT  */
void pmm_init(uint64_t mem_base, uint64_t mem_size)
{
    uint64_t frame_count = mem_size / PAGE_SIZE;
    uint64_t db_bytes    = frame_count * sizeof(page_frame_t);
    uint64_t db_pages    = (db_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    g_frame_base  = mem_base;
    g_frame_count = frame_count;
    g_frames      = (page_frame_t *)(uintptr_t)mem_base;
    g_alloc_start = db_pages;

    memset(g_frames, 0, db_bytes);

    uint64_t usable    = frame_count - db_pages;
    uint64_t zone_size = usable / ZONE_COUNT;
    uint64_t red_size  = usable - (zone_size * 2);

    for (uint64_t i = db_pages; i < frame_count; i++) {
        uint64_t rel = i - db_pages;

        if (rel < zone_size)
            g_frames[i].zone = ZONE_GREEN;
        else if (rel < zone_size * 2)
            g_frames[i].zone = ZONE_YELLOW;
        else
            g_frames[i].zone = ZONE_RED;

        g_frames[i].flags     = PF_FREE;
        g_frames[i].ref_count = 0;
    }

    for (uint64_t i = 0; i < db_pages; i++) {
        g_frames[i].zone      = ZONE_GREEN;
        g_frames[i].flags     = PF_LOCKED;
        g_frames[i].ref_count = 1;
    }

    g_zone_cursor[ZONE_GREEN]  = g_alloc_start;
    g_zone_cursor[ZONE_YELLOW] = g_alloc_start + zone_size;
    g_zone_cursor[ZONE_RED]    = g_alloc_start + (zone_size * 2);

    g_zone_free[ZONE_GREEN]  = zone_size;
    g_zone_free[ZONE_YELLOW] = zone_size;
    g_zone_free[ZONE_RED]    = red_size;

    serial_print("[PMM] Initialized OK\n");
}

/*  ALLOC  */
uint64_t pmm_alloc(mem_zone_t zone)
{
    if (zone >= ZONE_COUNT) return 0;
    if (g_zone_free[zone] == 0) return 0;

    uint64_t start = g_zone_cursor[zone];
    uint64_t i = start;

    do {
        if (i >= g_frame_count)
            i = g_alloc_start;

        if (g_frames[i].zone == zone &&
            (g_frames[i].flags & PF_FREE) &&
            !(g_frames[i].flags & PF_LOCKED) &&
            g_frames[i].ref_count == 0)
        {
            g_frames[i].flags &= ~PF_FREE;
            g_frames[i].ref_count = 1;

            g_zone_cursor[zone] = i + 1;
            if (g_zone_cursor[zone] >= g_frame_count)
                g_zone_cursor[zone] = g_alloc_start;

            g_zone_free[zone]--;

            return idx_to_phys(i);
        }

        i++;
    } while (i != start);

    return 0;
}

/*  FREE  */
void pmm_free(uint64_t phys)
{
    if (!phys) return;

    uint64_t idx = phys_to_idx(phys);
    if (idx >= g_frame_count) return;

    if (g_frames[idx].flags & PF_LOCKED) return;

    if (g_frames[idx].ref_count > 0)
        g_frames[idx].ref_count--;

    if (g_frames[idx].ref_count == 0) {
        g_frames[idx].flags |= PF_FREE;
        g_zone_free[g_frames[idx].zone]++;
    }
}

/*  REF  */
void pmm_ref(uint64_t phys)
{
    uint64_t idx = phys_to_idx(phys);
    if (idx < g_frame_count)
        g_frames[idx].ref_count++;
}

/*  QUERY  */
mem_zone_t pmm_zone_of(uint64_t phys)
{
    uint64_t idx = phys_to_idx(phys);
    if (idx >= g_frame_count) return ZONE_RED;
    return g_frames[idx].zone;
}

uint64_t pmm_free_pages(mem_zone_t zone)
{
    if (zone >= ZONE_COUNT) return 0;
    return g_zone_free[zone];
}