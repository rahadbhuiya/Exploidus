#include "elf.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../mm/kmalloc.h"
#include "../drivers/serial.h"
#include <string.h>

#define PAGE_SIZE        4096UL
#define USER_STACK_TOP   0x00007FFFFFFFE000ULL
#define USER_STACK_PAGES 64

#define PML4_IDX(a) (((a) >> 39) & 0x1FF)
#define PDPT_IDX(a) (((a) >> 30) & 0x1FF)
#define PD_IDX(a)   (((a) >> 21) & 0x1FF)
#define PT_IDX(a)   (((a) >> 12) & 0x1FF)

/*
 * ASLR_MASK: bits [28:21] of RDRAND output, producing a 2MB-aligned
 * random offset in the range [0, ~512 MB). Since any 2MB-aligned
 * value only ever varies in bits 21+, entropy is bounded by how many
 * *high* bits the mask keeps, not by the mask's total width -- the
 * previous mask (bits 27:12) only ever contributed bits 27:21 after
 * 2MB-alignment, i.e. 7 bits of real entropy. This mask keeps bits
 * 28:21, i.e. 8 bits (256 possible load addresses instead of 128).
 *
 * ASLR_MIN_BASE: 32 MB minimum -- safely above the kernel-reserved
 * pd0 entries (indices 0-15, covering 0-32 MB huge pages). Adding
 * ASLR_MIN_BASE guarantees the final base is always in
 * [32MB, 32MB + 510MB] = [32MB, 542MB], which stays comfortably
 * inside the user-zeroed pd0 region (indices 16-511, i.e. up to
 * 1024MB) that make_isolated_pml4() clears for user mappings --
 * leaving ~480MB of headroom above the highest possible base for
 * the ELF image itself plus any future PT_LOAD growth.
 *
 * NOTE: 8 bits is still weak by desktop-OS standards (Linux typically
 * gives 28+ bits for mmap ASLR). True strong entropy would need
 * make_isolated_pml4() to clear more than one PD table's worth of
 * user address space -- a bigger architectural change, tracked as a
 * follow-up. This is a safe, contained improvement within the
 * existing single-PD-table user region.
 */
#define ASLR_MASK     0x000000001FE00000ULL
#define ASLR_MIN_BASE 0x0000000002000000ULL  /* 32 MB */

/*
 * STACK_ASLR_MASK: random *sub-page* offset within the single 2MB
 * PD entry (pd1[511]) that make_isolated_pml4() clears for the
 * stack. USER_STACK_PAGES (64 pages = 256KB) must still fit below
 * USER_STACK_TOP after the offset is subtracted, so the offset is
 * capped well under the ~1.75MB of slack available in that entry.
 * Bits [20:12] give a 4KB-aligned offset in [0, ~2MB), which we then
 * mask down further to stay clear of the stack pages themselves.
 */
#define STACK_ASLR_MASK 0x00000000001FF000ULL /* up to ~2MB, 4KB-aligned */

/*
 * aslr_rdrand — generate a hardware-random 64-bit value.
 * Retries up to 10 times as recommended by Intel's RDRAND guidance.
 * Falls back to a fixed constant only if RDRAND keeps failing (a
 * hardware fault condition that should not occur on supported CPUs).
 */
static uint64_t aslr_rdrand(void)
{
    uint64_t val = 0;
    uint8_t  ok  = 0;
    for (int i = 0; i < 10; i++) {
        __asm__ volatile (
            "rdrand %0\n"
            "setc   %1\n"
            : "=r"(val), "=qm"(ok)
            :
            : "cc"
        );
        if (ok) return val;
    }
    /* RDRAND failed — hardware fault, not normal operation.
     * Return a non-zero page-aligned constant so the loader still
     * applies some offset rather than falling back to the link address. */
    serial_print("[ELF] ASLR: RDRAND failed, using fallback offset\n");
    return 0x0000000003370000ULL;
}


bool elf_validate(const uint8_t *data, uint64_t size)
{
    if (size < sizeof(elf64_hdr_t)) { serial_print("[ELF] too small\n"); return false; }
    const elf64_hdr_t *h = (const elf64_hdr_t *)data;
    if (h->magic   != ELF_MAGIC)        { serial_print("[ELF] bad magic\n");   return false; }
    if (h->class   != ELF_CLASS_64)     { serial_print("[ELF] not 64b\n");     return false; }
    if (h->data    != ELF_DATA_LE)      { serial_print("[ELF] not LE\n");      return false; }
    if (h->type != ELF_TYPE_EXEC && h->type != ELF_TYPE_DYN) {
        serial_print("[ELF] not exec or PIE\n"); return false;
    }
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

    uint64_t kpdpt_phys = kpml4[0] & VMM_ADDR_MASK;
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
        uint64_t kpd0_phys = kpdpt[0] & VMM_ADDR_MASK;
        uint64_t *kpd0     = (uint64_t *)(uintptr_t)kpd0_phys;

        uint64_t pd0_phys = pmm_alloc(ZONE_GREEN);
        if (!pd0_phys) { pmm_free(pdpt_phys); pmm_free(pml4_phys); return 0; }
        uint64_t *pd0 = (uint64_t *)(uintptr_t)pd0_phys;

        for (int i = 0; i < 512; i++)
            pd0[i] = (i < 16) ? kpd0[i] : 0; /* keep kernel 0-32MB, clear user range */

        pdpt[0] = pd0_phys | (kpdpt[0] & 0xFFF);
    }

    /* PDPT[1]: private PD1 — clear last entry (stack range 0x7FE00000+) */
    if (kpdpt[1] & 1) {
        uint64_t kpd1_phys = kpdpt[1] & VMM_ADDR_MASK;
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
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4[PML4_IDX(vaddr)] & VMM_ADDR_MASK);

    if (!(pdpt[PDPT_IDX(vaddr)] & 1)) {
        uint64_t p = pmm_alloc(ZONE_GREEN);
        if (!p) return 0;
        memset((void *)(uintptr_t)p, 0, PAGE_SIZE);
        pdpt[PDPT_IDX(vaddr)] = p | 0x7;
    } else if (is_user) {
        pdpt[PDPT_IDX(vaddr)] |= VMM_USER;
    }
    uint64_t *pd = (uint64_t *)(uintptr_t)(pdpt[PDPT_IDX(vaddr)] & VMM_ADDR_MASK);

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
    uint64_t *pt = (uint64_t *)(uintptr_t)(pd[PD_IDX(vaddr)] & VMM_ADDR_MASK);

    pt[PT_IDX(vaddr)] = (paddr & ~0xFFFULL) | flags | VMM_PRESENT;
    __asm__ volatile ("invlpg (%0)" :: "r"(vaddr) : "memory");
    return 1;
}


/*
 * vaddr_to_file_offset -- find which PT_LOAD segment covers `vaddr`
 * (a pre-ASLR virtual address, straight from the ELF's own program
 * headers) and return the corresponding offset into the file data
 * buffer. Returns UINT64_MAX if no segment covers it.
 */
static uint64_t vaddr_to_file_offset(const uint8_t *data, uint64_t size,
                                      const elf64_hdr_t *h, uint64_t vaddr)
{
    for (int i = 0; i < h->phnum; i++) {
        uint64_t phdr_off = h->phoff + (uint64_t)i * h->phentsize;
        if (phdr_off > size || h->phentsize > size - phdr_off) continue;
        const elf64_phdr_t *ph = (const elf64_phdr_t *)(data + phdr_off);
        if (ph->type != PT_LOAD) continue;
        if (vaddr >= ph->vaddr && vaddr < ph->vaddr + ph->filesz) {
            return ph->offset + (vaddr - ph->vaddr);
        }
    }
    return ~0ULL;
}

/*
 * apply_relocations -- self-relocates a statically-linked PIE
 * (built with -static-pie: one self-contained executable, no
 * external symbols, so the only relocation type that ever appears
 * is R_X86_64_RELATIVE).
 *
 * dyn_off/dyn_filesz locate the PT_DYNAMIC segment's raw bytes in
 * the file data buffer (found during the PT_LOAD scan in elf_load).
 * Walks its DT_RELA/DT_RELASZ/DT_RELAENT entries to find the actual
 * .rela.dyn table, then for every R_X86_64_RELATIVE entry writes
 * (aslr_base + addend) into the already-mapped page at
 * (aslr_base + r_offset), via vmm_get_phys_into() so it works
 * regardless of which segment or page the target falls in.
 *
 * Without this, any absolute address baked into .data/.got by the
 * compiler (a string-literal pointer in an array, a stored function
 * pointer, etc) is left holding its link-time (base-0) value instead
 * of the correct ASLR-shifted runtime address -- the exact bug
 * fixed.ld previously had to work around by disabling ASLR entirely.
 */
static bool apply_relocations(uint64_t pml4, const uint8_t *data, uint64_t size,
                               const elf64_hdr_t *h,
                               uint64_t dyn_off, uint64_t dyn_filesz,
                               uint64_t aslr_base)
{
    uint64_t rela_vaddr = 0, rela_size = 0, rela_entsize = sizeof(elf64_rela_t);

    uint64_t n_dyn = dyn_filesz / sizeof(elf64_dyn_t);
    for (uint64_t i = 0; i < n_dyn; i++) {
        uint64_t off = dyn_off + i * sizeof(elf64_dyn_t);
        if (off > size || sizeof(elf64_dyn_t) > size - off) break;
        const elf64_dyn_t *d = (const elf64_dyn_t *)(data + off);
        if (d->d_tag == DT_NULL) break;
        if (d->d_tag == DT_RELA)    rela_vaddr   = d->d_val;
        if (d->d_tag == DT_RELASZ)  rela_size    = d->d_val;
        if (d->d_tag == DT_RELAENT && d->d_val)  rela_entsize = d->d_val;
    }

    if (rela_size == 0) return true; /* nothing to relocate, not an error */

    uint64_t rela_file_off = vaddr_to_file_offset(data, size, h, rela_vaddr);
    if (rela_file_off == ~0ULL) {
        serial_print("[ELF] .rela.dyn vaddr not inside any segment\n");
        return false;
    }

    uint64_t count = rela_size / rela_entsize;
    for (uint64_t i = 0; i < count; i++) {
        uint64_t off = rela_file_off + i * rela_entsize;
        if (off > size || sizeof(elf64_rela_t) > size - off) {
            serial_print("[ELF] .rela.dyn entry outside image\n");
            return false;
        }
        const elf64_rela_t *r = (const elf64_rela_t *)(data + off);

        if (ELF64_R_TYPE(r->r_info) != R_X86_64_RELATIVE) continue;

        uint64_t target_vaddr = r->r_offset + aslr_base;
        uint64_t value = aslr_base + (uint64_t)r->r_addend;

        /* Byte-at-a-time so a target straddling a page boundary still
         * works, not just the common (page-aligned-enough) case. */
        for (int b = 0; b < 8; b++) {
            uint64_t phys = vmm_get_phys_into(pml4, target_vaddr + (uint64_t)b);
            if (!phys) {
                serial_print("[ELF] relocation target not mapped\n");
                return false;
            }
            ((uint8_t *)(uintptr_t)phys)[0] = (uint8_t)(value >> (b * 8));
        }
    }

    serial_print("[ELF] applied ");
    serial_printhex(count);
    serial_print(" relocation(s)\n");
    return true;
}

bool elf_load(const uint8_t *data, uint64_t size,
              uint64_t *pml4_out, uint64_t *entry_out, uint64_t *stack_out,
              const char **argv, const char **envp)
{
    if (!elf_validate(data, size)) return false;
    const elf64_hdr_t *h = (const elf64_hdr_t *)data;

    uint64_t pml4 = make_isolated_pml4();
    if (!pml4) { serial_print("[ELF] pml4 alloc failed\n"); return false; }

    /*
     * ASLR: only PIE (ET_DYN) binaries can safely be loaded at an
     * arbitrary base -- and now that apply_relocations() below fixes
     * up every R_X86_64_RELATIVE entry, an ET_DYN binary's absolute
     * addresses (a string-literal pointer stored in an array, etc)
     * get correctly shifted too, not just its RIP-relative code.
     *
     * ET_EXEC binaries have no relocations at all (that is what
     * ET_EXEC means) and embed absolute addresses assuming they run
     * at their exact linked address -- always load them there,
     * aslr_base = 0, regardless of what that linked address happens
     * to be. (A previous version of this check special-cased a low
     * linked address as "apply ASLR anyway", which directly
     * contradicted this and was the actual cause of a real crash --
     * any ET_EXEC binary linked at a low base, e.g. via a linker
     * script that starts at 0, would get its absolute-address data
     * silently corrupted.)
     */
    uint64_t aslr_base = 0;
    if (h->type == ELF_TYPE_DYN) {
        /* 2MB-aligned random offset + 32MB minimum */
        aslr_base = (aslr_rdrand() & ASLR_MASK & ~0x1FFFFFULL) + ASLR_MIN_BASE;
        serial_print("[ELF] ET_DYN ASLR base=");
        serial_printhex(aslr_base);
        serial_print("\n");
    } else {
        serial_print("[ELF] ET_EXEC: fixed addr, no ASLR\n");
    }

    uint64_t dyn_off = 0, dyn_filesz = 0;
    bool     have_dyn = false;

    for (int i = 0; i < h->phnum; i++) {
        uint64_t phdr_off = h->phoff + (uint64_t)i * h->phentsize;
        if (phdr_off > size || h->phentsize > size - phdr_off) {
            serial_print("[ELF] phdr outside image\n");
            return false;
        }
        const elf64_phdr_t *ph =
            (const elf64_phdr_t *)(data + phdr_off);
        if (ph->type == PT_DYNAMIC) {
            dyn_off    = ph->offset;
            dyn_filesz = ph->filesz;
            have_dyn   = true;
        }
        if (ph->type != PT_LOAD || ph->memsz == 0) continue;
        if (ph->filesz > ph->memsz || ph->offset > size ||
            ph->filesz > size - ph->offset) {
            serial_print("[ELF] segment outside image\n");
            return false;
        }

        uint64_t load_vaddr = ph->vaddr + aslr_base;
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

            if (!map_page(pml4, load_vaddr + j * PAGE_SIZE, phys, flags)) {
                serial_print("[ELF] map_page failed for segment\n");
                return false;
            }
        }
        serial_print("[ELF] seg vaddr=");
        serial_printhex(load_vaddr);
        serial_print(" filesz=");
        serial_printhex(ph->filesz);
        serial_print("\n");
    }

    if (h->type == ELF_TYPE_DYN && have_dyn) {
        if (!apply_relocations(pml4, data, size, h, dyn_off, dyn_filesz, aslr_base)) {
            serial_print("[ELF] relocation processing failed\n");
            return false;
        }
    }

    /*
     * Stack ASLR — randomise the stack's position within the 2MB
     * region make_isolated_pml4() clears for it (pd1[511]), instead
     * of always starting exactly at USER_STACK_TOP. Applied
     * unconditionally (ET_EXEC and ET_DYN alike): unlike code/data
     * ASLR, the stack has no absolute-address-in-.data problem, so
     * there's no relocation hazard here — only RIP-relative/SP-
     * relative access, which self-corrects under any shift.
     */
    uint64_t stack_rand = aslr_rdrand() & STACK_ASLR_MASK;
    uint64_t stack_top  = USER_STACK_TOP - stack_rand;
    uint64_t stack_base = stack_top -
                          (uint64_t)USER_STACK_PAGES * PAGE_SIZE;
    serial_print("[ELF] stack_rand="); serial_printhex(stack_rand); serial_print("\n");
    serial_print("[ELF] stack_base="); serial_printhex(stack_base); serial_print("\n");
    for (int i = 0; i < USER_STACK_PAGES; i++) {
        uint64_t phys = pmm_alloc(ZONE_RED);
        if (!phys) {
            serial_print("[ELF] stack pmm_alloc failed at page ");
            serial_printhex((uint64_t)i);
            serial_print("\n");
            return false;
        }
        memset((void *)(uintptr_t)phys, 0, PAGE_SIZE);
        if (!map_page(pml4, stack_base + (uint64_t)i * PAGE_SIZE,
                      phys, VMM_USER | VMM_WRITE | VMM_NX)) {
            serial_print("[ELF] map_page failed for stack\n");
            return false;
        }
    }
    serial_print("[ELF] stack mapped OK\n");

    *pml4_out  = pml4;
    *entry_out = h->entry + aslr_base;

    /*
     * Build System V ABI initial stack layout:
     *
     *   [stack_top - ...]  strings (argv[0], argv[1], ..., envp strings)
     *   ...
     *   NULL               end of envp pointers
     *   envp[n-1] ptr
     *   ...
     *   envp[0] ptr
     *   NULL               end of argv pointers
     *   argv[argc-1] ptr
     *   ...
     *   argv[0] ptr
     *   argc               <-- rsp points here on entry
     *
     * We write into the physical pages that were just mapped.
     * The stack is identity-mapped phys==virt during setup so
     * we can write directly via the physical address.
     */
    {
        /* Collect argv strings */
        const char *default_argv0 = "exploidus";
        const char *default_argv[2] = { default_argv0, (const char *)0 };
        if (!argv || !argv[0]) argv = default_argv;

        /* Count argc and total string bytes needed */
        int argc = 0;
        uint64_t str_bytes = 0;
        while (argv[argc]) {
            uint64_t l = 0;
            while (argv[argc][l]) l++;
            str_bytes += l + 1;
            argc++;
        }
        int envc = 0;
        if (envp) {
            while (envp[envc]) {
                uint64_t l = 0;
                while (envp[envc][l]) l++;
                str_bytes += l + 1;
                envc++;
            }
        }

        /*
         * Layout on stack (growing down from stack_top):
         *   [strings area]  str_bytes bytes
         *   NULL (envp end)
         *   envc envp ptrs  (8 bytes each)
         *   NULL (argv end)
         *   argc argv ptrs  (8 bytes each)
         *   argc            (8 bytes)
         */
        uint64_t ptr_area  = (uint64_t)(1 + argc + 1 + envc + 1) * 8;
        uint64_t total     = str_bytes + ptr_area;
        /* Align down to 16 bytes */
        total = (total + 15) & ~15ULL;

        uint64_t sp = stack_top - total;
        uint64_t str_cursor = stack_top - str_bytes;

        /* Copy all strings into a kernel buffer BEFORE switching CR3 */
        static char argv_strbuf[2048];
        static uint64_t argv_ptrs[16];
        static uint64_t envp_ptrs[32];

        if (argc > 15) argc = 15;
        if (envc > 31) envc = 31;

        uint64_t kbuf_off = 0;
        for (int i = 0; i < argc; i++) {
            argv_ptrs[i] = str_cursor + kbuf_off;
            const char *s = argv[i];
            while (*s && kbuf_off < 2046) argv_strbuf[kbuf_off++] = *s++;
            argv_strbuf[kbuf_off++] = 0;
        }
        for (int i = 0; i < envc; i++) {
            envp_ptrs[i] = str_cursor + kbuf_off;
            const char *s = envp[i];
            while (*s && kbuf_off < 2046) argv_strbuf[kbuf_off++] = *s++;
            argv_strbuf[kbuf_off++] = 0;
        }

        /* Now switch to user PML4 and write into user stack */
        uint64_t saved_cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(saved_cr3));
        __asm__ volatile("mov %0, %%cr3" :: "r"(pml4) : "memory");

        /* Write string bytes */
        uint8_t *dst = (uint8_t *)(uintptr_t)str_cursor;
        for (uint64_t i = 0; i < kbuf_off; i++) dst[i] = argv_strbuf[i];

        /* Write pointer array + argc */
        uint64_t *stack = (uint64_t *)(uintptr_t)sp;
        int idx = 0;
        stack[idx++] = (uint64_t)argc;
        for (int i = 0; i < argc; i++) stack[idx++] = argv_ptrs[i];
        stack[idx++] = 0;
        for (int i = 0; i < envc; i++) stack[idx++] = envp_ptrs[i];
        stack[idx++] = 0;

        __asm__ volatile("mov %0, %%cr3" :: "r"(saved_cr3) : "memory");

        *stack_out = sp;
    }

    serial_print("[ELF] Load OK entry=");
    serial_printhex(*entry_out);
    serial_print("\n");
    return true;
}