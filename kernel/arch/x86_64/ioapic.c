#include "ioapic.h"
#include "acpi.h"
#include "../../drivers/serial.h"

#define IOREGSEL 0x00
#define IOWIN    0x10

#define IOAPIC_REG_ID  0x00
#define IOAPIC_REG_VER 0x01
#define IOAPIC_REG_REDTBL(n) (0x10 + 2 * (n))

static volatile uint32_t *g_ioapic_base = NULL;
static uint32_t g_max_redirection_entries = 24;

static uint32_t ioapic_read(uint8_t reg)
{
    if (!g_ioapic_base) return 0;
    g_ioapic_base[IOREGSEL / 4] = reg;
    __asm__ volatile ("" ::: "memory");
    return g_ioapic_base[IOWIN / 4];
}

static void ioapic_write(uint8_t reg, uint32_t value)
{
    if (!g_ioapic_base) return;
    g_ioapic_base[IOREGSEL / 4] = reg;
    __asm__ volatile ("" ::: "memory");
    g_ioapic_base[IOWIN / 4] = value;
    __asm__ volatile ("" ::: "memory");
}

bool ioapic_is_enabled(void)
{
    return g_ioapic_base != NULL;
}

void ioapic_init(uint64_t ioapic_phys_addr)
{
    if (!ioapic_phys_addr) {
        ioapic_phys_addr = 0xFEC00000ULL;
    }

    g_ioapic_base = (volatile uint32_t *)(uintptr_t)ioapic_phys_addr;

    uint32_t ver = ioapic_read(IOAPIC_REG_VER);
    g_max_redirection_entries = ((ver >> 16) & 0xFF) + 1;

    serial_print("[IOAPIC] Initialized at ");
    serial_printhex(ioapic_phys_addr);
    serial_print(", max redirection entries: ");
    serial_printdec(g_max_redirection_entries);
    serial_print("\n");

    /* Mask all entries by default during initialization */
    for (uint32_t i = 0; i < g_max_redirection_entries; i++) {
        /* Set mask bit (bit 16) */
        ioapic_write(IOAPIC_REG_REDTBL(i), 0x00010000 | (0x20 + i));
        ioapic_write(IOAPIC_REG_REDTBL(i) + 1, 0);
    }
}

void ioapic_route_irq(uint8_t irq, uint8_t vector, uint8_t dest_apic_id, bool masked)
{
    if (!g_ioapic_base) return;

    uint32_t gsi = acpi_remap_irq_to_gsi(irq);
    if (gsi >= g_max_redirection_entries) return;

    uint32_t low = vector;
    if (masked) {
        low |= (1 << 16); /* Mask bit */
    }

    uint32_t high = ((uint32_t)dest_apic_id) << 24;

    ioapic_write(IOAPIC_REG_REDTBL(gsi), low);
    ioapic_write(IOAPIC_REG_REDTBL(gsi) + 1, high);
}

void ioapic_mask_irq(uint8_t irq)
{
    if (!g_ioapic_base) return;

    uint32_t gsi = acpi_remap_irq_to_gsi(irq);
    if (gsi >= g_max_redirection_entries) return;

    uint32_t low = ioapic_read(IOAPIC_REG_REDTBL(gsi));
    low |= (1 << 16);
    ioapic_write(IOAPIC_REG_REDTBL(gsi), low);
}

void ioapic_unmask_irq(uint8_t irq)
{
    if (!g_ioapic_base) return;

    uint32_t gsi = acpi_remap_irq_to_gsi(irq);
    if (gsi >= g_max_redirection_entries) return;

    uint32_t low = ioapic_read(IOAPIC_REG_REDTBL(gsi));
    low &= ~(1 << 16);
    ioapic_write(IOAPIC_REG_REDTBL(gsi), low);
}
