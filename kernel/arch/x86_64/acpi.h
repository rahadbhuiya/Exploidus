#pragma once
#include <stdint.h>
#include <stdbool.h>

/* ACPI Table Structures */

typedef struct {
    char     signature[8];       /* "RSD PTR " */
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;           /* 0 for ACPI 1.0, 2 for ACPI 2.0+ */
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  extended_checksum;
    uint8_t  reserved[3];
} __attribute__((packed)) acpi_rsdp_t;

typedef struct {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) acpi_header_t;

typedef struct {
    acpi_header_t header;
    uint32_t      lapic_address;
    uint32_t      flags;
} __attribute__((packed)) acpi_madt_t;

typedef struct {
    uint8_t type;
    uint8_t length;
} __attribute__((packed)) acpi_madt_entry_header_t;

/* MADT Entry Type 0: Processor Local APIC */
typedef struct {
    acpi_madt_entry_header_t header;
    uint8_t  acpi_processor_id;
    uint8_t  apic_id;
    uint32_t flags;              /* Bit 0 = Enabled */
} __attribute__((packed)) acpi_madt_lapic_t;

/* MADT Entry Type 1: I/O APIC */
typedef struct {
    acpi_madt_entry_header_t header;
    uint8_t  ioapic_id;
    uint8_t  reserved;
    uint32_t ioapic_address;
    uint32_t global_system_interrupt_base;
} __attribute__((packed)) acpi_madt_ioapic_t;

/* MADT Entry Type 2: Interrupt Source Override */
typedef struct {
    acpi_madt_entry_header_t header;
    uint8_t  bus_source;         /* 0 = ISA */
    uint8_t  irq_source;         /* Bus-relative IRQ */
    uint32_t global_system_interrupt;
    uint16_t flags;              /* Polarity & Trigger mode */
} __attribute__((packed)) acpi_madt_iso_t;

#define ACPI_MAX_CORES 32
#define ACPI_MAX_IOAPIC 4
#define ACPI_MAX_ISO    16

typedef struct {
    bool     initialized;
    uint32_t core_count;
    uint8_t  core_apic_ids[ACPI_MAX_CORES];
    
    uint64_t lapic_address;
    
    uint32_t ioapic_count;
    uint64_t ioapic_addresses[ACPI_MAX_IOAPIC];
    uint32_t ioapic_gsi_bases[ACPI_MAX_IOAPIC];

    uint32_t iso_count;
    acpi_madt_iso_t iso_entries[ACPI_MAX_ISO];
} acpi_info_t;

extern acpi_info_t g_acpi_info;

/* Public API */
void acpi_init(uint64_t mb_info_phys);
uint32_t acpi_get_core_count(void);
uint64_t acpi_get_lapic_addr(void);
uint64_t acpi_get_ioapic_addr(uint32_t index);
uint32_t acpi_remap_irq_to_gsi(uint8_t irq);
