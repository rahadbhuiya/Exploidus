#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "pmm.h"

/* Page flags */
#define VMM_PRESENT   (1ULL << 0)
#define VMM_WRITE     (1ULL << 1)
#define VMM_USER      (1ULL << 2)
#define VMM_HUGE      (1ULL << 7)
#define VMM_NX        (1ULL << 63)

/*
 * Mask to extract the physical address out of a page table entry:
 * bits 12-51 (AMD64's maximum physical address width). `& ~0xFFFULL`
 * alone only clears the low 12 flag bits and leaves NX (bit 63) and
 * any other high bits untouched -- fine for entries you already know
 */
#define VMM_ADDR_MASK 0x000FFFFFFFFFF000ULL

void     vmm_init(void);

/* Map into the global kernel PML4 */
bool     vmm_map(uint64_t phys, uint64_t virt, uint64_t flags);

/* Map into an arbitrary PML4 (used by sys_mmap for per-process mappings) */
bool     vmm_map_into(uint64_t pml4_phys, uint64_t phys,
                      uint64_t virt, uint64_t flags);

void     vmm_unmap(uint64_t virt);
uint64_t vmm_get_phys(uint64_t virt);
void     vmm_unmap_into(uint64_t pml4_phys, uint64_t virt);
uint64_t vmm_get_phys_into(uint64_t pml4_phys, uint64_t virt);
void     vmm_switch(uint64_t pml4_phys);

uint64_t vmm_get_kernel_pml4(void);
uint64_t vmm_make_user_pml4(void);

/* Demand paging & page fault resolution */
struct interrupt_frame;
bool     vmm_handle_page_fault(void *frame_ptr, uint64_t cr2);
