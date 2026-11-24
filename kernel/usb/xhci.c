#include "xhci.h"
#include "../drivers/pci.h"
#include "../drivers/serial.h"
#include "../mm/pmm.h"
#include <string.h>

static volatile uint8_t *g_cap_base      = NULL;
static volatile uint8_t *g_op_base       = NULL;
static bool              g_xhci_ready    = false;
static uint32_t          g_max_slots     = 0;
static uint32_t          g_max_ports     = 0;
static uint32_t          g_connected_devs= 0;

/* Device Context Base Address Array (DCBAA) */
static uint64_t         *g_dcbaa __attribute__((unused)) = NULL;
static uint64_t          g_dcbaa_phys    = 0;

/* Command Ring */
static xhci_trb_t       *g_cmd_ring      = NULL;
static uint64_t          g_cmd_ring_phys = 0;
static uint32_t          g_cmd_ring_tail __attribute__((unused)) = 0;
static uint8_t           g_cmd_cycle_state __attribute__((unused)) = 1;

static inline uint32_t xhci_read32(volatile uint8_t *base, uint32_t offset)
{
    return *(volatile uint32_t *)(base + offset);
}

static inline void xhci_write32(volatile uint8_t *base, uint32_t offset, uint32_t val)
{
    *(volatile uint32_t *)(base + offset) = val;
}

static inline uint64_t __attribute__((unused)) xhci_read64(volatile uint8_t *base, uint32_t offset)
{
    return *(volatile uint64_t *)(base + offset);
}

static inline void xhci_write64(volatile uint8_t *base, uint32_t offset, uint64_t val)
{
    *(volatile uint64_t *)(base + offset) = val;
}

bool xhci_is_present(void)
{
    return g_xhci_ready;
}

uint32_t xhci_port_count(void)
{
    return g_max_ports;
}

uint32_t xhci_connected_devices(void)
{
    return g_connected_devs;
}

static const char *xhci_speed_name(uint8_t speed)
{
    switch (speed) {
    case 1:  return "Full-Speed (12 Mbps, USB 1.1)";
    case 2:  return "Low-Speed (1.5 Mbps, USB 1.0)";
    case 3:  return "High-Speed (480 Mbps, USB 2.0)";
    case 4:  return "SuperSpeed (5 Gbps, USB 3.0)";
    case 5:  return "SuperSpeedPlus (10 Gbps, USB 3.1/3.2)";
    default: return "Unknown Speed";
    }
}

void xhci_init(void)
{
    serial_print("[xHCI] Scanning PCI bus for USB 3.0 xHCI controller (class 0x0C:0x03:0x30)...\n");

    pci_device_t *dev = pci_find_class(PCI_CLASS_SERIAL_BUS, 0x03, 0x30);
    if (!dev) {
        serial_print("[xHCI] No xHCI controller found on PCI bus.\n");
        return;
    }

    serial_print("[xHCI] Discovered USB 3.0 xHCI controller at PCI ");
    serial_printhex((uint64_t)dev->bus);
    serial_print(":");
    serial_printhex((uint64_t)dev->slot);
    serial_print(".");
    serial_printhex((uint64_t)dev->func);
    serial_print(" [");
    serial_printhex((uint64_t)dev->vendor_id);
    serial_print(":");
    serial_printhex((uint64_t)dev->device_id);
    serial_print("]\n");

    uint64_t bar0 = dev->bar_address[0];
    if (bar0 == 0) {
        serial_print("[xHCI] Error: Invalid or unmapped BAR0.\n");
        return;
    }

    pci_enable_bus_mastering(dev);
    pci_enable_memory_space(dev);

    g_cap_base = (volatile uint8_t *)(uintptr_t)bar0;

    /* Read Capability Registers */
    uint8_t caplength = g_cap_base[XHCI_CAP_CAPLENGTH];
    uint16_t hciversion = *(volatile uint16_t *)(g_cap_base + XHCI_CAP_HCIVERSION);
    uint32_t hcsparams1 = xhci_read32(g_cap_base, XHCI_CAP_HCSPARAMS1);
    uint32_t hccparams1 = xhci_read32(g_cap_base, XHCI_CAP_HCCPARAMS1);

    g_max_slots = hcsparams1 & 0xFF;
    if (g_max_slots > XHCI_MAX_SLOTS_SUPPORTED) {
        g_max_slots = XHCI_MAX_SLOTS_SUPPORTED;
    }
    g_max_ports = (hcsparams1 >> 24) & 0xFF;

    g_op_base = g_cap_base + caplength;

    serial_print("[xHCI] Hardware Interface Version: ");
    serial_printhex((uint64_t)(hciversion >> 8));
    serial_print(".");
    serial_printhex((uint64_t)(hciversion & 0xFF));
    serial_print(" | Max Slots: ");
    serial_printhex((uint64_t)g_max_slots);
    serial_print(" | Root Ports: ");
    serial_printhex((uint64_t)g_max_ports);
    serial_print(" | CSZ: ");
    serial_print((hccparams1 & (1 << 2)) ? "64-byte" : "32-byte");
    serial_print("\n");

    /* Step 1: Wait for Controller Ready (USBSTS.CNR == 0) */
    uint32_t timeout = 500000;
    while ((xhci_read32(g_op_base, XHCI_OP_USBSTS) & XHCI_STS_CNR) && --timeout) {
        __asm__ volatile ("pause");
    }
    if (timeout == 0) {
        serial_print("[xHCI] Warning: Timeout waiting for Controller Ready (CNR=0).\n");
    }

    /* Step 2: Halt controller if running */
    uint32_t usbcmd = xhci_read32(g_op_base, XHCI_OP_USBCMD);
    if (usbcmd & XHCI_CMD_RS) {
        xhci_write32(g_op_base, XHCI_OP_USBCMD, usbcmd & ~XHCI_CMD_RS);
        timeout = 500000;
        while (!(xhci_read32(g_op_base, XHCI_OP_USBSTS) & XHCI_STS_HCH) && --timeout) {
            __asm__ volatile ("pause");
        }
    }

    /* Step 3: Issue Host Controller Reset (HCRST) */
    serial_print("[xHCI] Resetting Host Controller via USBCMD.HCRST...\n");
    xhci_write32(g_op_base, XHCI_OP_USBCMD, XHCI_CMD_HCRST);
    timeout = 1000000;
    while ((xhci_read32(g_op_base, XHCI_OP_USBCMD) & XHCI_CMD_HCRST) && --timeout) {
        __asm__ volatile ("pause");
    }
    if (timeout == 0) {
        serial_print("[xHCI] Error: Reset timed out.\n");
        return;
    }

    /* Wait for CNR to settle back to 0 */
    timeout = 1000000;
    while ((xhci_read32(g_op_base, XHCI_OP_USBSTS) & XHCI_STS_CNR) && --timeout) {
        __asm__ volatile ("pause");
    }

    /* Step 4: Configure Max Device Slots Enabled */
    xhci_write32(g_op_base, XHCI_OP_CONFIG, g_max_slots);

    /* Step 5: Allocate Device Context Base Address Array (DCBAA) */
    g_dcbaa_phys = pmm_alloc(ZONE_GREEN);
    if (!g_dcbaa_phys) {
        serial_print("[xHCI] Error: Out of physical memory for DCBAA.\n");
        return;
    }
    g_dcbaa = (uint64_t *)(uintptr_t)g_dcbaa_phys;
    memset(g_dcbaa, 0, 4096);
    xhci_write64(g_op_base, XHCI_OP_DCBAAP, g_dcbaa_phys);

    /* Step 6: Allocate Command Ring */
    g_cmd_ring_phys = pmm_alloc(ZONE_GREEN);
    if (!g_cmd_ring_phys) {
        serial_print("[xHCI] Error: Out of physical memory for Command Ring.\n");
        return;
    }
    g_cmd_ring = (xhci_trb_t *)(uintptr_t)g_cmd_ring_phys;
    memset(g_cmd_ring, 0, 4096);
    g_cmd_ring_tail = 0;
    g_cmd_cycle_state = 1;

    /* Initialize Link TRB at the end of ring pointing back to start */
    g_cmd_ring[XHCI_CMD_RING_TRB_COUNT - 1].parameter = g_cmd_ring_phys;
    g_cmd_ring[XHCI_CMD_RING_TRB_COUNT - 1].status = 0;
    g_cmd_ring[XHCI_CMD_RING_TRB_COUNT - 1].control = (XHCI_TRB_LINK << 10) | (1u << 1); /* Toggle Cycle bit */

    /* Program Command Ring Control Register (CRCR) with cycle state 1 */
    xhci_write64(g_op_base, XHCI_OP_CRCR, g_cmd_ring_phys | 1u);

    /* Step 7: Power up root hub ports */
    for (uint32_t i = 0; i < g_max_ports; i++) {
        uint32_t port_reg = XHCI_OP_PORTSC_BASE + (i * 0x10);
        uint32_t val = xhci_read32(g_op_base, port_reg);
        /* Mask write-1-to-clear change bits (bits 17-23) to avoid accidental acknowledgment */
        uint32_t write_mask = val & ~0x00FE0000u;
        if (!(val & XHCI_PORTSC_PP)) {
            xhci_write32(g_op_base, port_reg, write_mask | XHCI_PORTSC_PP);
        }
    }

    /* Step 8: Start Host Controller (USBCMD.RS = 1) */
    serial_print("[xHCI] Starting Host Controller execution engine (USBCMD.RS=1)...\n");
    xhci_write32(g_op_base, XHCI_OP_USBCMD, XHCI_CMD_RS);
    timeout = 500000;
    while ((xhci_read32(g_op_base, XHCI_OP_USBSTS) & XHCI_STS_HCH) && --timeout) {
        __asm__ volatile ("pause");
    }
    if (timeout == 0) {
        serial_print("[xHCI] Warning: Controller still indicates halted state.\n");
    }

    /* Small delay for port connection detection */
    for (volatile int d = 0; d < 2000000; d++) {
        __asm__ volatile ("pause");
    }

    /* Step 9: Enumerate and probe root hub ports */
    g_connected_devs = 0;
    for (uint32_t i = 0; i < g_max_ports; i++) {
        uint32_t port_reg = XHCI_OP_PORTSC_BASE + (i * 0x10);
        uint32_t status = xhci_read32(g_op_base, port_reg);

        if (status & XHCI_PORTSC_CCS) {
            uint8_t speed = (status >> XHCI_PORTSC_SPEED_SHIFT) & 0x0F;
            g_connected_devs++;

            serial_print("[xHCI] Port ");
            serial_printhex((uint64_t)(i + 1));
            serial_print(": Connected | Status: 0x");
            serial_printhex((uint64_t)status);
            serial_print(" | Protocol: ");
            serial_print(xhci_speed_name(speed));
            serial_print("\n");
        }
    }

    serial_print("[xHCI] Host Controller operational. Total connected devices: ");
    serial_printhex((uint64_t)g_connected_devs);
    serial_print("\n");

    g_xhci_ready = true;
}
