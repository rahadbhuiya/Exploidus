#include "elf.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../mm/kmalloc.h"
#include "../drivers/serial.h"
#include <string.h>

#define PAGE_SIZE        4096UL
#define USER_STACK_TOP   0x00007FFFFFFFE000ULL
#define USER_STACK_PAGES 8

#define PML4_IDX(a) (((a) >> 39) & 0x1FF)
#define PDPT_IDX(a) (((a) >> 30) & 0x1FF)
#define PD_IDX(a)   (((a) >> 21) & 0x1FF)
#define PT_IDX(a)   (((a) >> 12) & 0x1FF)


bool elf_validate(const uint8_t *data, uint64_t size)
{
    if (size < sizeof(elf64_hdr_t)) { serial_print("[ELF] too small\n"); return false; }
    const elf64_hdr_t *h = (const elf64_hdr_t *)data;
    if (h->magic   != ELF_MAGIC)        { serial_print("[ELF] bad magic\n");   return false; }
    if (h->class   != ELF_CLASS_64)     { serial_print("[ELF] not 64b\n");     return false; }
    if (h->data    != ELF_DATA_LE)      { serial_print("[ELF] not LE\n");      return false; }
    if (h->type    != ELF_TYPE_EXEC)    { serial_print("[ELF] not exec\n");    return false; }
    if (h->machine != ELF_MACH_X86_64)  { serial_print("[ELF] not x86-64\n"); return false; }
    if (h->phnum   == 0)                { serial_print("[ELF] no phdrs\n");    return false; }
    return true;
}


/* Build an isolated PML4 for a user process.                          */
/* Creates private copies of the low-half page directory structures     */
/* so that:                                                             */
/*  - Kernel code (0x0-0xFFFFFFF, pd0[0-7]) stays huge-page mapped    */
/*  - User ELF range (pd0[8]+) is cleared for fresh 4KB mappings       */
/*  - Stack range (pd1[511]) is cleared for fresh 4KB mappings         */
/*  - MMIO range (pd3, 0xC000_0000+) is shared from kernel             */

static uint64_t make_isolated_pml4(void)
{
    uint64_t kpml4_phys = vmm_get_kernel_pml4();
    uint64_t *kpml4     = (uint64_t *)(uintptr_t)kpml4_phys;

    /* New PML4 */
    uint64_t pml4_phys = pmm_alloc(ZONE_GREEN);
    if (!pml4_phys) return 0;
    uint64_t *pml4 = (uint64_t *)(uintptr_t)pml4_phys;
    memset(pml4, 0, PAGE_SIZE);

    /* Copy upper half (256-511): kernel upper mappings */
    for (int i = 256; i < 512; i++)
        pml4[i] = kpml4[i];

    /* PML4[0]: need private PDPT */
    if (!(kpml4[0] & 1)) return pml4_phys;

    uint64_t kpdpt_phys = kpml4[0] & ~0xFFFULL;
    uint64_t *kpdpt     = (uint64_t *)(uintptr_t)kpdpt_phys;

    uint64_t pdpt_phys = pmm_alloc(ZONE_GREEN);
    if (!pdpt_phys) { pmm_free(pml4_phys); return 0; }
    uint64_t *pdpt = (uint64_t *)(uintptr_t)pdpt_phys;

    /* Copy ALL PDPT entries from kernel — needed for high memory access */
    for (int i = 0; i < 512; i++)
        pdpt[i] = kpdpt[i];
    /* Note: pd0 and pd1 get private copies below to isolate user space */

    /* PDPT[0]: private PD0 — keep only first 8 entries (kernel 0-15MB) */
    if (kpdpt[0] & 1) {
        uint64_t kpd0_phys = kpdpt[0] & ~0xFFFULL;
        uint64_t *kpd0     = (uint64_t *)(uintptr_t)kpd0_phys;

        uint64_t pd0_phys = pmm_alloc(ZONE_GREEN);
        if (!pd0_phys) { pmm_free(pdpt_phys); pmm_free(pml4_phys); return 0; }
        uint64_t *pd0 = (uint64_t *)(uintptr_t)pd0_phys;

        for (int i = 0; i < 512; i++)
            pd0[i] = (i < 8) ? kpd0[i] : 0;  /* keep kernel 0-16MB, clear user range */

        pdpt[0] = pd0_phys | (kpdpt[0] & 0xFFF);
    }

    /* PDPT[1]: private PD1 — clear last entry (stack range 0x7FE00000+) */
    if (kpdpt[1] & 1) {
        uint64_t kpd1_phys = kpdpt[1] & ~0xFFFULL;
        uint64_t *kpd1     = (uint64_t *)(uintptr_t)kpd1_phys;

        uint64_t pd1_phys = pmm_alloc(ZONE_GREEN);
        if (!pd1_phys) { pmm_free(pdpt_phys); pmm_free(pml4_phys); return 0; }
        uint64_t *pd1 = (uint64_t *)(uintptr_t)pd1_phys;

        for (int i = 0; i < 512; i++)
            pd1[i] = (i < 511) ? kpd1[i] : 0; /* keep all but last (stack) */

        pdpt[1] = pd1_phys | (kpdpt[1] & 0xFFF);
    }

    pml4[0] = pdpt_phys | (kpml4[0] & 0xFFF);
    return pml4_phys;
}


static int map_page(uint64_t pml4_phys, uint64_t vaddr,
                    uint64_t paddr, uint64_t flags)
{
    int is_user = !!(flags & VMM_USER);
    uint64_t *pml4 = (uint64_t *)(uintptr_t)pml4_phys;

    if (!(pml4[PML4_IDX(vaddr)] & 1)) {
        uint64_t p = pmm_alloc(ZONE_GREEN);
        if (!p) return 0;
        memset((void *)(uintptr_t)p, 0, PAGE_SIZE);
        pml4[PML4_IDX(vaddr)] = p | 0x7;
    } else if (is_user) {
        pml4[PML4_IDX(vaddr)] |= VMM_USER;
    }
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4[PML4_IDX(vaddr)] & ~0xFFFULL);

    if (!(pdpt[PDPT_IDX(vaddr)] & 1)) {
        uint64_t p = pmm_alloc(ZONE_GREEN);
        if (!p) return 0;
        memset((void *)(uintptr_t)p, 0, PAGE_SIZE);
        pdpt[PDPT_IDX(vaddr)] = p | 0x7;
    } else if (is_user) {
        pdpt[PDPT_IDX(vaddr)] |= VMM_USER;
    }
    uint64_t *pd = (uint64_t *)(uintptr_t)(pdpt[PDPT_IDX(vaddr)] & ~0xFFFULL);

    /* If PD entry is a huge page, we cannot sub-divide it.
     * This should not happen with make_isolated_pml4 since we
     * cleared the user ranges. But guard anyway. */
    if ((pd[PD_IDX(vaddr)] & 1) && (pd[PD_IDX(vaddr)] & (1 << 7))) {
        serial_print("[ELF] map_page: hit huge page — isolated PML4 broken\n");
        return 0;
    }

    if (!(pd[PD_IDX(vaddr)] & 1)) {
        uint64_t p = pmm_alloc(ZONE_GREEN);
        if (!p) return 0;
        memset((void *)(uintptr_t)p, 0, PAGE_SIZE);
        pd[PD_IDX(vaddr)] = p | 0x7;
    } else if (is_user) {
        pd[PD_IDX(vaddr)] |= VMM_USER;
    }
    uint64_t *pt = (uint64_t *)(uintptr_t)(pd[PD_IDX(vaddr)] & ~0xFFFULL);

    pt[PT_IDX(vaddr)] = (paddr & ~0xFFFULL) | flags | VMM_PRESENT;
    __asm__ volatile ("invlpg (%0)" :: "r"(vaddr) : "memory");
    return 1;
}


bool elf_load(const uint8_t *data, uint64_t size,
              uint64_t *pml4_out, uint64_t *entry_out, uint64_t *stack_out)
{
    if (!elf_validate(data, size)) return false;
    const elf64_hdr_t *h = (const elf64_hdr_t *)data;

    uint64_t pml4 = make_isolated_pml4();
    if (!pml4) { serial_print("[ELF] pml4 alloc failed\n"); return false; }

    for (int i = 0; i < h->phnum; i++) {
        uint64_t phdr_off = h->phoff + (uint64_t)i * h->phentsize;
        if (phdr_off > size || h->phentsize > size - phdr_off) {
            serial_print("[ELF] phdr outside image\n");
            return false;
        }
        const elf64_phdr_t *ph =
            (const elf64_phdr_t *)(data + phdr_off);
        if (ph->type != PT_LOAD || ph->memsz == 0) continue;
        if (ph->filesz > ph->memsz || ph->offset > size ||
            ph->filesz > size - ph->offset) {
            serial_print("[ELF] segment outside image\n");
            return false;
        }

        uint64_t pages = (ph->memsz + PAGE_SIZE - 1) / PAGE_SIZE;
        for (uint64_t j = 0; j < pages; j++) {
            uint64_t phys = pmm_alloc(ZONE_RED);
            if (!phys) { serial_print("[ELF] pmm_alloc failed\n"); return false; }
            memset((void *)(uintptr_t)phys, 0, PAGE_SIZE);

            if (j * PAGE_SIZE < ph->filesz) {
                uint64_t copy = ph->filesz - j * PAGE_SIZE;
                if (copy > PAGE_SIZE) copy = PAGE_SIZE;
                memcpy((void *)(uintptr_t)phys,
                       data + ph->offset + j * PAGE_SIZE, copy);
            }

            uint64_t flags = VMM_USER;
            if (ph->flags & PF_W)    flags |= VMM_WRITE;
            if (!(ph->flags & PF_X)) flags |= VMM_NX;

            if (!map_page(pml4, ph->vaddr + j * PAGE_SIZE, phys, flags)) {
                serial_print("[ELF] map_page failed for segment\n");
                return false;
            }
        }
        serial_print("[ELF] seg vaddr=");
        serial_printhex(ph->vaddr);
        serial_print(" filesz=");
        serial_printhex(ph->filesz);
        serial_print("\n");
    }

    /* Stack */
    uint64_t stack_base = USER_STACK_TOP -
                          (uint64_t)USER_STACK_PAGES * PAGE_SIZE;
    for (int i = 0; i < USER_STACK_PAGES; i++) {
        uint64_t phys = pmm_alloc(ZONE_RED);
        if (!phys) { serial_print("[ELF] stack alloc failed\n"); return false; }
        memset((void *)(uintptr_t)phys, 0, PAGE_SIZE);
        if (!map_page(pml4, stack_base + (uint64_t)i * PAGE_SIZE,
                      phys, VMM_USER | VMM_WRITE | VMM_NX)) {
            serial_print("[ELF] map_page failed for stack\n");
            return false;
        }
    }

    *pml4_out  = pml4;
    *entry_out = h->entry;
    *stack_out = USER_STACK_TOP;

    serial_print("[ELF] Load OK entry=");
    serial_printhex(h->entry);
    serial_print("\n");
    return true;
}
