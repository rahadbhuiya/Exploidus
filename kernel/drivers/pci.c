#include "pci.h"
#include "serial.h"
#include "driver.h"
#include <string.h>

static inline void outl(uint16_t port, uint32_t val)
{
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port) : "memory");
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t v;
    __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port) : "memory");
    return v;
}

static inline uint32_t pci_config_addr(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    return 0x80000000u
         | ((uint32_t)bus  << 16)
         | ((uint32_t)slot << 11)
         | ((uint32_t)func <<  8)
         | (offset & 0xFC);
}

uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    outl(PCI_CONFIG_ADDRESS, pci_config_addr(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    uint32_t dword = pci_read32(bus, slot, func, offset);
    return (uint16_t)((dword >> ((offset & 2) * 8)) & 0xFFFF);
}

uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    uint32_t dword = pci_read32(bus, slot, func, offset);
    return (uint8_t)((dword >> ((offset & 3) * 8)) & 0xFF);
}

void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val)
{
    outl(PCI_CONFIG_ADDRESS, pci_config_addr(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, val);
}

void pci_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t val)
{
    uint32_t dword = pci_read32(bus, slot, func, offset);
    uint32_t shift = (offset & 2) * 8;
    dword &= ~(0xFFFFu << shift);
    dword |= ((uint32_t)val << shift);
    pci_write32(bus, slot, func, offset, dword);
}

void pci_write8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t val)
{
    uint32_t dword = pci_read32(bus, slot, func, offset);
    uint32_t shift = (offset & 3) * 8;
    dword &= ~(0xFFu << shift);
    dword |= ((uint32_t)val << shift);
    pci_write32(bus, slot, func, offset, dword);
}

static pci_device_t g_pci_devices[PCI_MAX_DEVICES];
static uint32_t     g_pci_device_count = 0;

const char *pci_class_name(uint8_t class_code)
{
    switch (class_code) {
    case PCI_CLASS_UNCLASSIFIED: return "Unclassified";
    case PCI_CLASS_STORAGE:      return "Mass Storage";
    case PCI_CLASS_NETWORK:      return "Network Controller";
    case PCI_CLASS_DISPLAY:      return "Display Controller";
    case PCI_CLASS_MULTIMEDIA:   return "Multimedia Controller";
    case PCI_CLASS_MEMORY:       return "Memory Controller";
    case PCI_CLASS_BRIDGE:       return "Bridge Device";
    case PCI_CLASS_COMM:         return "Communication Device";
    case PCI_CLASS_SYSTEM:       return "System Peripheral";
    case PCI_CLASS_INPUT:        return "Input Device";
    case PCI_CLASS_SERIAL_BUS:   return "Serial Bus Controller";
    default:                     return "Unknown Device";
    }
}

static void pci_probe_bars(pci_device_t *dev)
{
    uint8_t htype = dev->header_type & 0x7F;
    if (htype != 0) return; /* Only standard headers have BAR0..BAR5 */

    for (int bar = 0; bar < 6; bar++) {
        uint8_t reg = (uint8_t)(PCI_REG_BAR0 + (bar * 4));
        uint32_t orig_val = pci_read32(dev->bus, dev->slot, dev->func, reg);

        /* Temporarily disable decoding to size the BAR safely */
        uint16_t cmd = pci_read16(dev->bus, dev->slot, dev->func, PCI_REG_COMMAND);
        pci_write16(dev->bus, dev->slot, dev->func, PCI_REG_COMMAND,
                    cmd & ~(PCI_CMD_IO_SPACE | PCI_CMD_MEMORY_SPACE));

        pci_write32(dev->bus, dev->slot, dev->func, reg, 0xFFFFFFFF);
        uint32_t mask = pci_read32(dev->bus, dev->slot, dev->func, reg);

        pci_write32(dev->bus, dev->slot, dev->func, reg, orig_val);
        pci_write16(dev->bus, dev->slot, dev->func, PCI_REG_COMMAND, cmd);

        if (mask == 0 || mask == 0xFFFFFFFF) {
            dev->bar_address[bar] = 0;
            dev->bar_size[bar]    = 0;
            continue;
        }

        if (orig_val & 1) {
            /* I/O Space BAR */
            dev->bar_is_io[bar]       = true;
            dev->bar_is_64[bar]       = false;
            dev->bar_prefetchable[bar] = false;
            dev->bar_address[bar]     = orig_val & ~0x3u;
            dev->bar_size[bar]        = (~(mask & ~0x3u)) + 1;
        } else {
            /* Memory Space BAR */
            dev->bar_is_io[bar]        = false;
            uint8_t type = (orig_val >> 1) & 0x3;
            dev->bar_prefetchable[bar] = (orig_val >> 3) & 0x1;

            if (type == 0x02 && bar < 5) {
                /* 64-bit Memory BAR */
                dev->bar_is_64[bar] = true;
                uint8_t reg_hi = (uint8_t)(reg + 4);
                uint32_t orig_hi = pci_read32(dev->bus, dev->slot, dev->func, reg_hi);

                pci_write16(dev->bus, dev->slot, dev->func, PCI_REG_COMMAND,
                            cmd & ~(PCI_CMD_IO_SPACE | PCI_CMD_MEMORY_SPACE));

                pci_write32(dev->bus, dev->slot, dev->func, reg_hi, 0xFFFFFFFF);
                uint32_t mask_hi = pci_read32(dev->bus, dev->slot, dev->func, reg_hi);

                pci_write32(dev->bus, dev->slot, dev->func, reg_hi, orig_hi);
                pci_write16(dev->bus, dev->slot, dev->func, PCI_REG_COMMAND, cmd);

                uint64_t full_addr = ((uint64_t)orig_hi << 32) | (orig_val & ~0xFu);
                uint64_t full_mask = ((uint64_t)mask_hi << 32) | (mask & ~0xFu);

                dev->bar_address[bar] = full_addr;
                dev->bar_size[bar]    = (~full_mask) + 1;

                /* Skip high dword */
                bar++;
                dev->bar_address[bar] = 0;
                dev->bar_size[bar]    = 0;
                dev->bar_is_64[bar]   = true;
            } else {
                /* 32-bit Memory BAR */
                dev->bar_is_64[bar]   = false;
                dev->bar_address[bar] = orig_val & ~0xFu;
                dev->bar_size[bar]    = (~(mask & ~0xFu)) + 1;
            }
        }
    }
}

static void pci_probe_device(uint8_t bus, uint8_t slot, uint8_t func)
{
    if (g_pci_device_count >= PCI_MAX_DEVICES) return;

    uint32_t id = pci_read32(bus, slot, func, PCI_REG_VENDOR_ID);
    uint16_t vendor = (uint16_t)(id & 0xFFFF);
    uint16_t device = (uint16_t)(id >> 16);
    if (vendor == 0xFFFF || vendor == 0x0000) return;

    pci_device_t *dev = &g_pci_devices[g_pci_device_count];
    memset(dev, 0, sizeof(*dev));

    dev->bus       = bus;
    dev->slot      = slot;
    dev->func      = func;
    dev->vendor_id = vendor;
    dev->device_id = device;

    uint32_t class_reg = pci_read32(bus, slot, func, PCI_REG_REVISION_ID);
    dev->revision   = (uint8_t)(class_reg & 0xFF);
    dev->prog_if    = (uint8_t)((class_reg >> 8) & 0xFF);
    dev->subclass   = (uint8_t)((class_reg >> 16) & 0xFF);
    dev->class_code = (uint8_t)((class_reg >> 24) & 0xFF);

    uint32_t misc = pci_read32(bus, slot, func, PCI_REG_CACHE_LINE_SIZE);
    dev->header_type = (uint8_t)((misc >> 16) & 0xFF);

    uint32_t intr = pci_read32(bus, slot, func, PCI_REG_INTERRUPT_LINE);
    dev->irq_line = (uint8_t)(intr & 0xFF);
    dev->irq_pin  = (uint8_t)((intr >> 8) & 0xFF);

    pci_probe_bars(dev);

    /* Log device to serial console */
    serial_print("[PCI ] ");
    serial_printhex((uint64_t)bus);
    serial_print(":");
    serial_printhex((uint64_t)slot);
    serial_print(".");
    serial_printhex((uint64_t)func);
    serial_print(" [");
    serial_printhex((uint64_t)vendor);
    serial_print(":");
    serial_printhex((uint64_t)device);
    serial_print("] class=");
    serial_printhex((uint64_t)dev->class_code);
    serial_print(":");
    serial_printhex((uint64_t)dev->subclass);
    serial_print(" (");
    serial_print(pci_class_name(dev->class_code));
    serial_print(")");

    if (dev->irq_line != 0 && dev->irq_line != 0xFF) {
        serial_print(" IRQ=");
        serial_printhex((uint64_t)dev->irq_line);
    }

    if (dev->bar_address[0] != 0) {
        serial_print(dev->bar_is_io[0] ? " BAR0=IO:0x" : " BAR0=MMIO:0x");
        serial_printhex(dev->bar_address[0]);
    }
    serial_print("\n");

    g_pci_device_count++;
}

void pci_scan(void)
{
    g_pci_device_count = 0;
    serial_print("[PCI ] Enumerating PCI/PCIe configuration space...\n");

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint16_t vendor = pci_read16((uint8_t)bus, slot, 0, PCI_REG_VENDOR_ID);
            if (vendor == 0xFFFF || vendor == 0x0000) continue;

            uint8_t header_type = pci_read8((uint8_t)bus, slot, 0, PCI_REG_HEADER_TYPE);
            uint8_t max_func = (header_type & 0x80) ? 8 : 1;

            for (uint8_t func = 0; func < max_func; func++) {
                uint16_t f_vendor = pci_read16((uint8_t)bus, slot, func, PCI_REG_VENDOR_ID);
                if (f_vendor == 0xFFFF || f_vendor == 0x0000) continue;
                pci_probe_device((uint8_t)bus, slot, func);
            }
        }
    }

    serial_print("[PCI ] Enumeration complete. Total devices discovered: ");
    serial_printhex((uint64_t)g_pci_device_count);
    serial_print("\n");
}

void pci_init(void)
{
    pci_scan();
}

pci_device_t *pci_find_device(uint16_t vendor_id, uint16_t device_id)
{
    for (uint32_t i = 0; i < g_pci_device_count; i++) {
        if (g_pci_devices[i].vendor_id == vendor_id &&
            g_pci_devices[i].device_id == device_id) {
            return &g_pci_devices[i];
        }
    }
    return NULL;
}

pci_device_t *pci_find_class(uint8_t class_code, uint8_t subclass, int prog_if)
{
    for (uint32_t i = 0; i < g_pci_device_count; i++) {
        if (g_pci_devices[i].class_code == class_code &&
            g_pci_devices[i].subclass == subclass) {
            if (prog_if < 0 || g_pci_devices[i].prog_if == (uint8_t)prog_if) {
                return &g_pci_devices[i];
            }
        }
    }
    return NULL;
}

void pci_enable_bus_mastering(pci_device_t *dev)
{
    if (!dev) return;
    uint16_t cmd = pci_read16(dev->bus, dev->slot, dev->func, PCI_REG_COMMAND);
    cmd |= PCI_CMD_BUS_MASTER;
    pci_write16(dev->bus, dev->slot, dev->func, PCI_REG_COMMAND, cmd);
}

void pci_enable_memory_space(pci_device_t *dev)
{
    if (!dev) return;
    uint16_t cmd = pci_read16(dev->bus, dev->slot, dev->func, PCI_REG_COMMAND);
    cmd |= PCI_CMD_MEMORY_SPACE;
    pci_write16(dev->bus, dev->slot, dev->func, PCI_REG_COMMAND, cmd);
}

void pci_enable_io_space(pci_device_t *dev)
{
    if (!dev) return;
    uint16_t cmd = pci_read16(dev->bus, dev->slot, dev->func, PCI_REG_COMMAND);
    cmd |= PCI_CMD_IO_SPACE;
    pci_write16(dev->bus, dev->slot, dev->func, PCI_REG_COMMAND, cmd);
}

uint32_t pci_get_device_count(void)
{
    return g_pci_device_count;
}

pci_device_t *pci_get_device(uint32_t index)
{
    if (index >= g_pci_device_count) return NULL;
    return &g_pci_devices[index];
}
