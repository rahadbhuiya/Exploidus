#include "vmm.h"
#include "pmm.h"
#include "../drivers/serial.h"
#include <string.h>

static uint64_t kernel_pml4 = 0;

static inline uint64_t read_cr3(void)
{
    uint64_t v;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline void write_cr3(uint64_t v)
{
    __asm__ volatile ("mov %0, %%cr3" :: "r"(v) : "memory");
}

static inline uint64_t *phys_to_virt(uint64_t p)
{
    return (uint64_t *)(uintptr_t)p;
}

/*
 * alloc_table — return the physical address of the next-level page table
 * pointed to by *entry, allocating and zeroing a new page if needed.
 */
static uint64_t alloc_table(uint64_t *entry)
{
    if (*entry & VMM_PRESENT)
        return *entry & ~0xFFFULL;
    uint64_t p = pmm_alloc(ZONE_GREEN);
    if (!p) return 0;
    memset(phys_to_virt(p), 0, 4096);
    *entry = (p & ~0xFFFULL) | VMM_PRESENT | VMM_WRITE | VMM_USER;
    return p;
}


/*  vmm_init  */


void vmm_init(void)
{
    kernel_pml4 = read_cr3() & ~0xFFFULL;
    serial_print("[VMM] init OK\n");
}


/*  vmm_get_kernel_pml4  */
/*                                                                       */
/*  Returns the physical address of the original kernel PML4 saved at  */
/*  boot time. Every new process PML4 must be initialised by copying   */
/*  all 512 entries from this table so that:                            */
/*    - Kernel code/data at low physical addresses (PML4 entry 0) is   */
/*      accessible while running kernel code in that process context.   */
/*    - Upper-half kernel mappings (entries 256-511) are also present.  */
/*                                                                       */
/*  Kernel pages do NOT have the USER bit set, so user-mode code        */
/*  cannot read or write them even though they appear in the PML4.      */


uint64_t vmm_get_kernel_pml4(void)
{
    return kernel_pml4;
}


/*  vmm_make_user_pml4    */
/*                                                                       */
/*  Allocate a fresh PML4 pre-populated with all kernel mappings.      */
/*  Call this from elf_load() instead of allocating a bare zeroed page. */


uint64_t vmm_make_user_pml4(void)
{
    uint64_t phys = pmm_alloc(ZONE_GREEN);
    if (!phys) return 0;

    uint64_t *new_pml4  = phys_to_virt(phys);
    uint64_t *kpml4     = phys_to_virt(kernel_pml4);

    /* Copy ALL 512 kernel entries — this includes the low-half kernel
     * identity mappings (entry 0 where the kernel binary lives) as
     * well as any upper-half mappings (entries 256-511).             */
    for (int i = 0; i < 512; i++)
        new_pml4[i] = kpml4[i];

    return phys;
}


/*  vmm_map — map into the global kernel PML4     */


bool vmm_map(uint64_t phys, uint64_t virt, uint64_t flags)
{
    uint64_t *pml4 = phys_to_virt(kernel_pml4);
    uint64_t pdpt_phys = alloc_table(&pml4[(virt >> 39) & 0x1FF]);
    if (!pdpt_phys) return false;
    uint64_t *pdpt = phys_to_virt(pdpt_phys);
    uint64_t pd_phys = alloc_table(&pdpt[(virt >> 30) & 0x1FF]);
    if (!pd_phys) return false;
    uint64_t *pd = phys_to_virt(pd_phys);
    uint64_t pt_phys = alloc_table(&pd[(virt >> 21) & 0x1FF]);
    if (!pt_phys) return false;
    uint64_t *pt = phys_to_virt(pt_phys);
    pt[(virt >> 12) & 0x1FF] = (phys & ~0xFFFULL) | flags | VMM_PRESENT;
    __asm__ volatile ("invlpg (%0)" :: "r"(virt) : "memory");
    return true;
}


/*  vmm_map_into — map into an arbitrary PML4   */


bool vmm_map_into(uint64_t pml4_phys, uint64_t phys,
                  uint64_t virt, uint64_t flags)
{
    if (!pml4_phys) return false;
    uint64_t *pml4 = phys_to_virt(pml4_phys);
    uint64_t pdpt_phys = alloc_table(&pml4[(virt >> 39) & 0x1FF]);
    if (!pdpt_phys) return false;
    uint64_t *pdpt = phys_to_virt(pdpt_phys);
    uint64_t pd_phys = alloc_table(&pdpt[(virt >> 30) & 0x1FF]);
    if (!pd_phys) return false;
    uint64_t *pd = phys_to_virt(pd_phys);
    uint64_t pt_phys = alloc_table(&pd[(virt >> 21) & 0x1FF]);
    if (!pt_phys) return false;
    uint64_t *pt = phys_to_virt(pt_phys);
    pt[(virt >> 12) & 0x1FF] = (phys & ~0xFFFULL) | flags | VMM_PRESENT;
    __asm__ volatile ("invlpg (%0)" :: "r"(virt) : "memory");
    return true;
}


/*  vmm_unmap — remove mapping from the kernel PML4  */


void vmm_unmap_into(uint64_t pml4_phys, uint64_t virt)
{
    if (!pml4_phys) return;
    uint64_t *pml4 = phys_to_virt(pml4_phys);
    uint64_t e4 = pml4[(virt >> 39) & 0x1FF];
    if (!(e4 & VMM_PRESENT)) return;
    uint64_t *pdpt = phys_to_virt(e4 & ~0xFFFULL);
    uint64_t e3 = pdpt[(virt >> 30) & 0x1FF];
    if (!(e3 & VMM_PRESENT)) return;
    if (e3 & VMM_HUGE) return;
    uint64_t *pd = phys_to_virt(e3 & ~0xFFFULL);
    uint64_t e2 = pd[(virt >> 21) & 0x1FF];
    if (!(e2 & VMM_PRESENT)) return;
    if (e2 & VMM_HUGE) return;
    uint64_t *pt = phys_to_virt(e2 & ~0xFFFULL);
    pt[(virt >> 12) & 0x1FF] = 0;
    __asm__ volatile ("invlpg (%0)" :: "r"(virt) : "memory");
}

void vmm_unmap(uint64_t virt)
{
    vmm_unmap_into(kernel_pml4, virt);
}


/*  vmm_get_phys */


uint64_t vmm_get_phys_into(uint64_t pml4_phys, uint64_t virt)
{
    if (!pml4_phys) return 0;
    uint64_t *pml4 = phys_to_virt(pml4_phys);
    uint64_t e4 = pml4[(virt >> 39) & 0x1FF];
    if (!(e4 & VMM_PRESENT)) return 0;
    uint64_t *pdpt = phys_to_virt(e4 & ~0xFFFULL);
    uint64_t e3 = pdpt[(virt >> 30) & 0x1FF];
    if (!(e3 & VMM_PRESENT)) return 0;
    if (e3 & VMM_HUGE) {
        return (e3 & ~0x3FFFFFFFULL) | (virt & 0x3FFFFFFFULL);
    }
    uint64_t *pd = phys_to_virt(e3 & ~0xFFFULL);
    uint64_t e2 = pd[(virt >> 21) & 0x1FF];
    if (!(e2 & VMM_PRESENT)) return 0;
    if (e2 & VMM_HUGE) {
        return (e2 & ~0x1FFFFFULL) | (virt & 0x1FFFFFULL);
    }
    uint64_t *pt = phys_to_virt(e2 & ~0xFFFULL);
    uint64_t pte = pt[(virt >> 12) & 0x1FF];
    if (!(pte & VMM_PRESENT)) return 0;
    return (pte & ~0xFFFULL) | (virt & 0xFFF);
}

uint64_t vmm_get_phys(uint64_t virt)
{
    return vmm_get_phys_into(kernel_pml4, virt);
}


/*  vmm_switch */


void vmm_switch(uint64_t pml4)
{
    write_cr3(pml4);
}
