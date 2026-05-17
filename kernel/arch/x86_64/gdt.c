#include "gdt.h"
#include <stdint.h>
#include <string.h>

/* GDT layout:
 * 0x00  null
 * 0x08  kernel code (ring 0, 64-bit)
 * 0x10  kernel data (ring 0)
 * 0x18  user DATA   (ring 3)   SS for sysretq = 0x1B
 * 0x20  user CODE   (ring 3)   CS for sysretq = 0x23
 * 0x28  TSS low
 * 0x30  TSS high
 *
 * sysretq: CS = STAR[63:48]+16|3 = 0x10+16|3 = 0x23
 *          SS = STAR[63:48]+ 8|3 = 0x10+ 8|3 = 0x1B
 */

#define GDT_ENTRIES 7

typedef struct __attribute__((packed)) {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} gdt_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high2;
    uint32_t base_high3;
    uint32_t reserved;
} gdt_tss_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} gdt_ptr_t;

typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} tss_t;

#define GDT_ACCESS_PRESENT  (1 << 7)
#define GDT_ACCESS_RING0    (0 << 5)
#define GDT_ACCESS_RING3    (3 << 5)
#define GDT_ACCESS_CODE_SEG (1 << 4)
#define GDT_ACCESS_DATA_SEG (1 << 4)
#define GDT_ACCESS_EXEC     (1 << 3)
#define GDT_ACCESS_RW       (1 << 1)
#define GDT_GRAN_4K         (1 << 7)
#define GDT_GRAN_64BIT      (1 << 5)
#define GDT_GRAN_32BIT      (1 << 6)
#define GDT_ACCESS_TSS      0x89

static gdt_entry_t g_gdt[GDT_ENTRIES];
static gdt_ptr_t   g_gdt_ptr;
static tss_t       g_tss;

static uint8_t g_kernel_stack[32768] __attribute__((aligned(16)));
static uint8_t g_df_stack[4096]      __attribute__((aligned(16)));

uint64_t g_syscall_kernel_rsp;

extern void gdt_flush(uint64_t gdt_ptr_addr);

static void gdt_set_entry(int idx, uint32_t base, uint32_t limit,
                           uint8_t access, uint8_t gran)
{
    g_gdt[idx].limit_low   = (uint16_t)(limit & 0xFFFF);
    g_gdt[idx].base_low    = (uint16_t)(base  & 0xFFFF);
    g_gdt[idx].base_mid    = (uint8_t)((base  >> 16) & 0xFF);
    g_gdt[idx].access      = access;
    g_gdt[idx].granularity = (uint8_t)((limit >> 16) & 0x0F) | (gran & 0xF0);
    g_gdt[idx].base_high   = (uint8_t)((base  >> 24) & 0xFF);
}

static void gdt_set_tss(int i, uint64_t base, uint32_t limit)
{
    gdt_tss_entry_t *e = (gdt_tss_entry_t *)&g_gdt[i];
    e->limit_low   = (uint16_t)(limit & 0xFFFF);
    e->base_low    = (uint16_t)(base  & 0xFFFF);
    e->base_mid    = (uint8_t)((base  >> 16) & 0xFF);
    e->access      = GDT_ACCESS_TSS;
    e->granularity = (uint8_t)((limit >> 16) & 0x0F);
    e->base_high2  = (uint8_t)((base  >> 24) & 0xFF);
    e->base_high3  = (uint32_t)(base  >> 32);
    e->reserved    = 0;
}

void gdt_init(void)
{
    gdt_set_entry(0, 0, 0, 0, 0);

    /* 0x08 kernel code */
    gdt_set_entry(1, 0, 0xFFFFFFFF,
        GDT_ACCESS_PRESENT | GDT_ACCESS_RING0 |
        GDT_ACCESS_CODE_SEG | GDT_ACCESS_EXEC | GDT_ACCESS_RW,
        GDT_GRAN_4K | GDT_GRAN_64BIT);

    /* 0x10 kernel data */
    gdt_set_entry(2, 0, 0xFFFFFFFF,
        GDT_ACCESS_PRESENT | GDT_ACCESS_RING0 |
        GDT_ACCESS_DATA_SEG | GDT_ACCESS_RW,
        GDT_GRAN_4K | GDT_GRAN_32BIT);

    /* 0x18 user DATA -> selector 0x1B */
    gdt_set_entry(3, 0, 0xFFFFFFFF,
        GDT_ACCESS_PRESENT | GDT_ACCESS_RING3 |
        GDT_ACCESS_DATA_SEG | GDT_ACCESS_RW,
        GDT_GRAN_4K | GDT_GRAN_32BIT);

    /* 0x20 user CODE -> selector 0x23 */
    gdt_set_entry(4, 0, 0xFFFFFFFF,
        GDT_ACCESS_PRESENT | GDT_ACCESS_RING3 |
        GDT_ACCESS_CODE_SEG | GDT_ACCESS_EXEC | GDT_ACCESS_RW,
        GDT_GRAN_4K | GDT_GRAN_64BIT);

    /* 0x28 TSS */
    memset(&g_tss, 0, sizeof(g_tss));
    g_tss.rsp0        = (uint64_t)(uintptr_t)(g_kernel_stack + sizeof(g_kernel_stack));
    g_tss.ist[0]      = (uint64_t)(uintptr_t)(g_df_stack + sizeof(g_df_stack));
    g_tss.iopb_offset = (uint16_t)sizeof(tss_t);
    g_syscall_kernel_rsp = g_tss.rsp0;

    gdt_set_tss(5, (uint64_t)(uintptr_t)&g_tss, (uint32_t)(sizeof(g_tss) - 1));

    g_gdt_ptr.limit = (uint16_t)(sizeof(g_gdt) - 1);
    g_gdt_ptr.base  = (uint64_t)(uintptr_t)g_gdt;
    gdt_flush((uint64_t)(uintptr_t)&g_gdt_ptr);

    __asm__ volatile ("ltr %0" :: "r"((uint16_t)0x28));
}

void tss_set_rsp0(uint64_t rsp0)
{
    g_tss.rsp0           = rsp0;
    g_syscall_kernel_rsp = rsp0;
}