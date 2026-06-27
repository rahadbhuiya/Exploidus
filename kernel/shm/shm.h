/*
 * shm.h — Exploidus Shared Memory
 *
 * Allows one process to create a named shared memory region and other
 * processes to map it into their own address spaces.
 *
 * Primary use case: compositor framebuffer sharing.
 *   1. App calls SYS_SHM_CREATE(size)  → gets back a shm_id (uint32_t)
 *   2. App draws into its mapped VA range
 *   3. Compositor calls SYS_SHM_MAP(shm_id) → maps the SAME physical
 *      pages into its own address space, reads and blends them
 *   4. Either side calls SYS_SHM_UNMAP(va, size) to unmap
 *   5. Original creator calls SYS_SHM_DESTROY(shm_id) when done
 *
 * Physical pages are ref-counted so they're only freed when no mapping
 * is left.  No kmalloc for the page frames themselves — they come from
 * PMM just like mmap pages.
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>

/*  tunables  */
#define SHM_MAX_REGIONS  32          /* max concurrent shared regions      */
#define SHM_MAX_PAGES    256         /* max pages per region (1 MB)        */
#define SHM_PAGE_SIZE    4096UL

/*  region descriptor  */
typedef struct {
    uint32_t  id;                    /* unique handle handed to userspace  */
    uint32_t  owner_pid;             /* creator PID                        */
    uint64_t  size;                  /* bytes (page-rounded)               */
    uint32_t  page_count;
    uint64_t  phys_pages[SHM_MAX_PAGES]; /* physical addresses             */
    uint8_t   map_count;             /* number of active mappings          */
    bool      valid;
} shm_region_t;

/*  subsystem API  */
void shm_init(void);

/*
 * shm_create — allocate physical pages and register a region.
 *   size  : bytes requested (rounded up to page boundary)
 *   Returns shm_id (>0) on success, 0 on failure.
 */
uint32_t shm_create(uint32_t owner_pid, uint64_t size);

/*
 * shm_map — map an existing region into the given process's address space.
 *   Returns the virtual address where the region was mapped, or 0 on error.
 *   The virtual address is chosen by the kernel (grows down from mmap_top).
 */
uint64_t shm_map(uint32_t shm_id, uint32_t pid);

/*
 * shm_unmap — unmap a previously mapped region from the given process.
 *   va   : virtual address returned by shm_map
 *   size : size used at shm_map time
 */
void shm_unmap(uint64_t va, uint64_t size, uint32_t pid);

/*
 * shm_destroy — release physical pages when no mappings remain.
 *   Caller must unmap before destroying.
 *   Returns 0 on success, -1 if still mapped by other processes.
 */
int shm_destroy(uint32_t shm_id, uint32_t caller_pid);

/*
 * shm_get — look up a region by id (kernel-internal use).
 */
shm_region_t *shm_get(uint32_t id);