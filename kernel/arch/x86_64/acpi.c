#include "acpi.h"
#include "../../drivers/serial.h"
#include "../../include/multiboot2.h"
#include <string.h>

#define MB2_TAG_OLD_ACPI 14
#define MB2_TAG_NEW_ACPI 15

acpi_info_t g_acpi_info = {0};

static bool acpi_checksum(const void *ptr, size_t length)
{
    const uint8_t *b = (const uint8_t *)ptr;
    uint8_t sum = 0;
    for (size_t i = 0; i < length; i++) {
        sum += b[i];
    }
    return sum == 0;
}

static acpi_rsdp_t *scan_rsdp_range(uint64_t start, uint64_t end)
{
    for (uint64_t p = start; p < end; p += 16) {
        if (memcmp((const void *)(uintptr_t)p, "RSD PTR ", 8) == 0) {
            acpi_rsdp_t *rsdp = (acpi_rsdp_t *)(uintptr_t)p;
            if (acpi_checksum(rsdp, 20)) {
                if (rsdp->revision >= 2) {
                    if (acpi_checksum(rsdp, rsdp->length)) {
                        return rsdp;
                    }
                } else {
                    return rsdp;
                }
            }
        }
    }
    return NULL;
}

static acpi_rsdp_t *find_rsdp(uint64_t mb_info_phys)
{
    /* 1. Try finding from Multiboot2 tags */
    if (mb_info_phys) {
        mb2_info_t *info = (mb2_info_t *)(uintptr_t)mb_info_phys;
        uint8_t *ptr = (uint8_t *)(info + 1);
        uint8_t *end = (uint8_t *)info + info->total_size;

        while (ptr < end) {
            mb2_tag_t *tag = (mb2_tag_t *)ptr;
            if (tag->type == 0) break;

            if (tag->type == MB2_TAG_NEW_ACPI || tag->type == MB2_TAG_OLD_ACPI) {
                acpi_rsdp_t *candidate = (acpi_rsdp_t *)(uintptr_t)(ptr + sizeof(mb2_tag_t));
                if (memcmp(candidate->signature, "RSD PTR ", 8) == 0) {
                    return candidate;
                }
            }
            ptr += MB2_ALIGN(tag->size);
        }
    }

    /* 2. Check Extended BIOS Data Area (EBDA) */
    uint16_t ebda_seg = *(const volatile uint16_t *)0x40E;
    if (ebda_seg) {
        uint64_t ebda_phys = (uint64_t)ebda_seg << 4;
        acpi_rsdp_t *found = scan_rsdp_range(ebda_phys, ebda_phys + 1024);
        if (found) return found;
    }

    /* 3. Scan main BIOS ROM memory region (0xE0000 - 0xFFFFF) */
    return scan_rsdp_range(0xE0000, 0x100000);
}

static void parse_madt(acpi_madt_t *madt)
{
    g_acpi_info.lapic_address = madt->lapic_address;
    g_acpi_info.core_count = 0;
    g_acpi_info.ioapic_count = 0;
    g_acpi_info.iso_count = 0;

    uint8_t *ptr = (uint8_t *)(madt + 1);
    uint8_t *end = (uint8_t *)madt + madt->header.length;

    while (ptr < end) {
        acpi_madt_entry_header_t *entry = (acpi_madt_entry_header_t *)ptr;
        if (entry->length == 0) break;

        switch (entry->type) {
            case 0: { /* Processor Local APIC */
                acpi_madt_lapic_t *lapic = (acpi_madt_lapic_t *)ptr;
                if ((lapic->flags & 1) && g_acpi_info.core_count < ACPI_MAX_CORES) {
                    g_acpi_info.core_apic_ids[g_acpi_info.core_count] = lapic->apic_id;
                    g_acpi_info.core_count++;
                }
                break;
            }
            case 1: { /* I/O APIC */
                acpi_madt_ioapic_t *ioapic = (acpi_madt_ioapic_t *)ptr;
                if (g_acpi_info.ioapic_count < ACPI_MAX_IOAPIC) {
                    uint32_t idx = g_acpi_info.ioapic_count;
                    g_acpi_info.ioapic_addresses[idx] = ioapic->ioapic_address;
                    g_acpi_info.ioapic_gsi_bases[idx] = ioapic->global_system_interrupt_base;
                    g_acpi_info.ioapic_count++;
                }
                break;
            }
            case 2: { /* Interrupt Source Override */
                acpi_madt_iso_t *iso = (acpi_madt_iso_t *)ptr;
                if (g_acpi_info.iso_count < ACPI_MAX_ISO) {
                    g_acpi_info.iso_entries[g_acpi_info.iso_count] = *iso;
                    g_acpi_info.iso_count++;
                }
                break;
            }
            case 5: { /* Local APIC Address Override (64-bit) */
                struct {
                    acpi_madt_entry_header_t header;
                    uint16_t reserved;
                    uint64_t lapic_addr;
                } __attribute__((packed)) *lapic_override = (void *)ptr;
                g_acpi_info.lapic_address = lapic_override->lapic_addr;
                break;
            }
            default:
                break;
        }

        ptr += entry->length;
    }
}

void acpi_init(uint64_t mb_info_phys)
{
    g_acpi_info.initialized = false;
    g_acpi_info.core_count = 1;
    g_acpi_info.lapic_address = 0xFEE00000ULL;
    g_acpi_info.ioapic_count = 1;
    g_acpi_info.ioapic_addresses[0] = 0xFEC00000ULL;
    g_acpi_info.ioapic_gsi_bases[0] = 0;

    acpi_rsdp_t *rsdp = find_rsdp(mb_info_phys);
    if (!rsdp) {
        serial_print("[ACPI] RSDP pointer not found; using fallback APIC topology.\n");
        return;
    }

    serial_print("[ACPI] Found RSDP (rev ");
    serial_printdec(rsdp->revision);
    serial_print(")\n");

    /* Parse XSDT (64-bit pointers) if ACPI 2.0+, else RSDT (32-bit pointers) */
    if (rsdp->revision >= 2 && rsdp->xsdt_address != 0) {
        acpi_header_t *xsdt = (acpi_header_t *)(uintptr_t)rsdp->xsdt_address;
        if (acpi_checksum(xsdt, xsdt->length)) {
            uint32_t entries = (xsdt->length - sizeof(acpi_header_t)) / sizeof(uint64_t);
            uint64_t *table_ptrs = (uint64_t *)(uintptr_t)(xsdt + 1);

            for (uint32_t i = 0; i < entries; i++) {
                acpi_header_t *hdr = (acpi_header_t *)(uintptr_t)table_ptrs[i];
                if (hdr && memcmp(hdr->signature, "APIC", 4) == 0 && acpi_checksum(hdr, hdr->length)) {
                    parse_madt((acpi_madt_t *)hdr);
                    g_acpi_info.initialized = true;
                    break;
                }
            }
        }
    } else if (rsdp->rsdt_address != 0) {
        acpi_header_t *rsdt = (acpi_header_t *)(uintptr_t)rsdp->rsdt_address;
        if (acpi_checksum(rsdt, rsdt->length)) {
            uint32_t entries = (rsdt->length - sizeof(acpi_header_t)) / sizeof(uint32_t);
            uint32_t *table_ptrs = (uint32_t *)(uintptr_t)(rsdt + 1);

            for (uint32_t i = 0; i < entries; i++) {
                acpi_header_t *hdr = (acpi_header_t *)(uintptr_t)table_ptrs[i];
                if (hdr && memcmp(hdr->signature, "APIC", 4) == 0 && acpi_checksum(hdr, hdr->length)) {
                    parse_madt((acpi_madt_t *)hdr);
                    g_acpi_info.initialized = true;
                    break;
                }
            }
        }
    }

    if (g_acpi_info.initialized) {
        serial_print("[ACPI] MADT parsed: detected ");
        serial_printdec(g_acpi_info.core_count);
        serial_print(" active CPU core(s), ");
        serial_printdec(g_acpi_info.ioapic_count);
        serial_print(" I/O APIC(s).\n");
    } else {
        serial_print("[ACPI] MADT table not found; using fallback configuration.\n");
    }
}

uint32_t acpi_get_core_count(void)
{
    return g_acpi_info.core_count ? g_acpi_info.core_count : 1;
}

uint64_t acpi_get_lapic_addr(void)
{
    return g_acpi_info.lapic_address ? g_acpi_info.lapic_address : 0xFEE00000ULL;
}

uint64_t acpi_get_ioapic_addr(uint32_t index)
{
    if (index < g_acpi_info.ioapic_count) {
        return g_acpi_info.ioapic_addresses[index];
    }
    return 0xFEC00000ULL;
}

uint32_t acpi_remap_irq_to_gsi(uint8_t irq)
{
    for (uint32_t i = 0; i < g_acpi_info.iso_count; i++) {
        if (g_acpi_info.iso_entries[i].bus_source == 0 && 
            g_acpi_info.iso_entries[i].irq_source == irq) {
            return g_acpi_info.iso_entries[i].global_system_interrupt;
        }
    }
    return irq;
}
