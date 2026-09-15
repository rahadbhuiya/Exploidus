#pragma once
#include <stdint.h>
#include <stdbool.h>

#define PAGE_SIZE  4096UL
#define PAGE_SHIFT 12

typedef enum {
    ZONE_GREEN  = 0,
    ZONE_YELLOW = 1,
    ZONE_RED    = 2,
    ZONE_COUNT  = 3
} mem_zone_t;

#define PF_FREE   (1 << 0)
#define PF_LOCKED (1 << 1)
#define PF_AUDIT  (1 << 2)

typedef struct {
    uint32_t   ref_count;
    mem_zone_t zone;
    uint8_t    flags;
    uint8_t    _pad[3];
} page_frame_t;

void       pmm_init(uint64_t mem_base, uint64_t mem_size);
uint64_t   pmm_alloc(mem_zone_t zone);
void       pmm_free(uint64_t phys);
mem_zone_t pmm_zone_of(uint64_t phys);
uint64_t   pmm_free_pages(mem_zone_t zone);
/* Total physical page frames PMM manages, across all zones combined
 * (GREEN + YELLOW + RED) -- fixed at pmm_init() time. Added for
 * SYS_MEMINFO (see meminfo_t below / sys_meminfo() in
 * kernel/syscall/table.c) so the shell's `free` command can report
 * real numbers instead of the hardcoded placeholder it used to. */
uint64_t   pmm_total_pages(void);
void       pmm_ref(uint64_t phys);

/*
 * meminfo_t -- physical + kernel-heap memory summary, filled in by
 * SYS_MEMINFO (sys_meminfo() in kernel/syscall/table.c). Mirrored
 * by hand in userspace/libc/syscall.h, same convention as
 * vfs_stat_t (kernel/fs/vfs/vfs.h) -- kernel and userspace build
 * separately with no shared header between them.
 */
typedef struct {
    uint64_t total_kb;     /* total physical memory PMM manages */
    uint64_t free_kb;      /* free physical memory, all zones summed */
    uint64_t used_kb;      /* total_kb - free_kb */
} meminfo_t;