#pragma once
#include <stdint.h>
#include <stdbool.h>

#define PCI_CONFIG_ADDRESS      0xCF8
#define PCI_CONFIG_DATA         0xCFC

/* Configuration Registers Offsets */
#define PCI_REG_VENDOR_ID       0x00
#define PCI_REG_DEVICE_ID       0x02
#define PCI_REG_COMMAND         0x04
#define PCI_REG_STATUS          0x06
#define PCI_REG_REVISION_ID     0x08
#define PCI_REG_PROG_IF         0x09
#define PCI_REG_SUBCLASS        0x0A
#define PCI_REG_CLASS           0x0B
#define PCI_REG_CACHE_LINE_SIZE 0x0C
#define PCI_REG_LATENCY_TIMER   0x0D
#define PCI_REG_HEADER_TYPE     0x0E
#define PCI_REG_BIST            0x0F
#define PCI_REG_BAR0            0x10
#define PCI_REG_BAR1            0x14
#define PCI_REG_BAR2            0x18
#define PCI_REG_BAR3            0x1C
#define PCI_REG_BAR4            0x20
#define PCI_REG_BAR5            0x24
#define PCI_REG_INTERRUPT_LINE  0x3C
#define PCI_REG_INTERRUPT_PIN   0x3D

/* Command Register Bits */
#define PCI_CMD_IO_SPACE        (1u << 0)
#define PCI_CMD_MEMORY_SPACE    (1u << 1)
#define PCI_CMD_BUS_MASTER      (1u << 2)
#define PCI_CMD_SPECIAL_CYCLES  (1u << 3)
#define PCI_CMD_MWI_ENABLE      (1u << 4)
#define PCI_CMD_VGA_PALETTE     (1u << 5)
#define PCI_CMD_PARITY_ERROR    (1u << 6)
#define PCI_CMD_SERR_ENABLE     (1u << 8)
#define PCI_CMD_FAST_B2B        (1u << 9)
#define PCI_CMD_INT_DISABLE     (1u << 10)

/* Common Class Codes */
#define PCI_CLASS_UNCLASSIFIED  0x00
#define PCI_CLASS_STORAGE       0x01
#define PCI_CLASS_NETWORK       0x02
#define PCI_CLASS_DISPLAY       0x03
#define PCI_CLASS_MULTIMEDIA    0x04
#define PCI_CLASS_MEMORY        0x05
#define PCI_CLASS_BRIDGE        0x06
#define PCI_CLASS_COMM          0x07
#define PCI_CLASS_SYSTEM        0x08
#define PCI_CLASS_INPUT         0x09
#define PCI_CLASS_SERIAL_BUS    0x0C

/* Common Subclasses */
#define PCI_SUBCLASS_IDE        0x01
#define PCI_SUBCLASS_FLOPPY     0x02
#define PCI_SUBCLASS_SATA       0x06
#define PCI_SUBCLASS_NVME       0x08
#define PCI_SUBCLASS_ETHERNET   0x00
#define PCI_SUBCLASS_VGA        0x00
#define PCI_SUBCLASS_USB        0x03

/* Max discovered PCI devices table size */
#define PCI_MAX_DEVICES         64

typedef struct {
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  revision;
    uint8_t  header_type;
    uint8_t  irq_line;
    uint8_t  irq_pin;

    /* Base Address Registers (BAR0..BAR5) */
    uint64_t bar_address[6];
    uint64_t bar_size[6];
    bool     bar_is_io[6];
    bool     bar_is_64[6];
    bool     bar_prefetchable[6];
} pci_device_t;

/* Low-level config space access */
uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint8_t  pci_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);

void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val);
void pci_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t val);
void pci_write8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t val);

/* High-level PCI management */
void pci_init(void);
void pci_scan(void);

pci_device_t *pci_find_device(uint16_t vendor_id, uint16_t device_id);
pci_device_t *pci_find_class(uint8_t class_code, uint8_t subclass, int prog_if);

void pci_enable_bus_mastering(pci_device_t *dev);
void pci_enable_memory_space(pci_device_t *dev);
void pci_enable_io_space(pci_device_t *dev);

uint32_t pci_get_device_count(void);
pci_device_t *pci_get_device(uint32_t index);
const char *pci_class_name(uint8_t class_code);
