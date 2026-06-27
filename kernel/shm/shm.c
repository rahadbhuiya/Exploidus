/*
 * shm.c — Exploidus Shared Memory subsystem
 */

#include "shm.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../proc/process.h"
#include "../drivers/serial.h"

/*  region table  */

static shm_region_t g_regions[SHM_MAX_REGIONS];
static uint32_t     g_next_id = 1;   /* IDs start at 1; 0 means "invalid" */

/*  init  */

void shm_init(void)
{
    for (int i = 0; i < SHM_MAX_REGIONS; i++) {
        g_regions[i].valid = false;
        g_regions[i].id    = 0;
    }
    serial_print("[SHM ] Shared memory subsystem ready\n");
}

/*  helpers  */

static shm_region_t *find_free_slot(void)
{
    for (int i = 0; i < SHM_MAX_REGIONS; i++)
        if (!g_regions[i].valid) return &g_regions[i];
    return NULL;
}

shm_region_t *shm_get(uint32_t id)
{
    if (!id) return NULL;
    for (int i = 0; i < SHM_MAX_REGIONS; i++)
        if (g_regions[i].valid && g_regions[i].id == id)
            return &g_regions[i];
    return NULL;
}

/*  shm_create  */

uint32_t shm_create(uint32_t owner_pid, uint64_t size)
{
    if (!size) return 0;

    /* Round up to page boundary */
    uint64_t pages = (size + SHM_PAGE_SIZE - 1) / SHM_PAGE_SIZE;
    if (pages > SHM_MAX_PAGES) {
        serial_print("[SHM ] create: too large\n");
        return 0;
    }

    shm_region_t *r = find_free_slot();
    if (!r) {
        serial_print("[SHM ] create: no free slots\n");
        return 0;
    }

    /* Allocate physical pages */
    for (uint32_t i = 0; i < pages; i++) {
        uint64_t phys = pmm_alloc(ZONE_RED);
        if (!phys) {
            /* Roll back already-allocated pages */
            for (uint32_t j = 0; j < i; j++)
                pmm_free(r->phys_pages[j]);
            serial_print("[SHM ] create: pmm_alloc failed\n");
            return 0;
        }
        r->phys_pages[i] = phys;
    }

    r->id         = g_next_id++;
    r->owner_pid  = owner_pid;
    r->size       = pages * SHM_PAGE_SIZE;
    r->page_count = (uint32_t)pages;
    r->map_count  = 0;
    r->valid      = true;

    serial_print("[SHM ] created id=");
    serial_printhex((uint64_t)r->id);
    serial_print(" pages=");
    serial_printhex((uint64_t)pages);
    serial_print("\n");

    return r->id;
}

/*  shm_map  */

uint64_t shm_map(uint32_t shm_id, uint32_t pid)
{
    shm_region_t *r = shm_get(shm_id);
    if (!r) return 0;

    process_t *proc = proc_get(pid);
    if (!proc) return 0;

    uint64_t size = r->size;

    /* Choose virtual address: grow mmap_top downward (same as sys_mmap) */
    uint64_t virt_top = proc->mmap_top;
    uint64_t virt     = virt_top - size;
    if (virt >= virt_top) return 0;  /* wraparound */
    if (virt < 0x1000)   return 0;

    /* Map each physical page into the process's page table */
    for (uint32_t i = 0; i < r->page_count; i++) {
        if (!vmm_map_into(proc->cr3,
                          r->phys_pages[i],
                          virt + (uint64_t)i * SHM_PAGE_SIZE,
                          VMM_WRITE | VMM_USER | VMM_NX)) {
            /* Partial map — unmap what we did and bail */
            for (uint32_t j = 0; j < i; j++)
                vmm_unmap_into(proc->cr3,
                               virt + (uint64_t)j * SHM_PAGE_SIZE);
            serial_print("[SHM ] map: vmm_map_into failed\n");
            return 0;
        }
    }

    proc->mmap_top = virt;
    r->map_count++;

    serial_print("[SHM ] mapped id=");
    serial_printhex((uint64_t)shm_id);
    serial_print(" va=");
    serial_printhex(virt);
    serial_print("\n");

    return virt;
}

/*  shm_unmap  */

void shm_unmap(uint64_t va, uint64_t size, uint32_t pid)
{
    if (!va || !size) return;

    process_t *proc = proc_get(pid);
    if (!proc) return;

    uint64_t pages = (size + SHM_PAGE_SIZE - 1) / SHM_PAGE_SIZE;

    /* Unmap virtual pages — do NOT free the physical frames here;
     * that's shm_destroy's job once map_count reaches 0.             */
    for (uint64_t i = 0; i < pages; i++)
        vmm_unmap_into(proc->cr3, va + i * SHM_PAGE_SIZE);

    /* Decrement map_count for the matching region */
    for (int i = 0; i < SHM_MAX_REGIONS; i++) {
        shm_region_t *r = &g_regions[i];
        if (!r->valid) continue;
        if (r->size != (pages * SHM_PAGE_SIZE)) continue;
        /* Best effort: match by size (caller should pass correct size) */
        if (r->map_count > 0) {
            r->map_count--;
            break;
        }
    }
}

/*  shm_destroy  */

int shm_destroy(uint32_t shm_id, uint32_t caller_pid)
{
    shm_region_t *r = shm_get(shm_id);
    if (!r) return -1;

    /* Only the owner can destroy */
    if (r->owner_pid != caller_pid) return -1;

    /* Refuse if other processes still have it mapped */
    if (r->map_count > 0) {
        serial_print("[SHM ] destroy: still mapped\n");
        return -1;
    }

    /* Free physical pages */
    for (uint32_t i = 0; i < r->page_count; i++)
        pmm_free(r->phys_pages[i]);

    serial_print("[SHM ] destroyed id=");
    serial_printhex((uint64_t)shm_id);
    serial_print("\n");

    r->valid = false;
    r->id    = 0;
    return 0;
}