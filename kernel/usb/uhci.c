#include "uhci.h"
#include "../drivers/serial.h"
#include <string.h>

/*  Port I/O helpers (same pattern as e1000.c / ata.c)  */

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
static inline void outw(uint16_t port, uint16_t val)
{
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port) : "memory");
}
static inline uint16_t inw(uint16_t port)
{
    uint16_t v;
    __asm__ volatile ("inw %1, %0" : "=a"(v) : "Nd"(port) : "memory");
    return v;
}
static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port) : "memory");
}

/*  PCI config access (same pattern as e1000.c)  */

#define PCI_CFG_ADDR  0xCF8
#define PCI_CFG_DATA  0xCFC

static uint32_t pci_read(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg)
{
    uint32_t addr = 0x80000000u
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)dev << 11)
                  | ((uint32_t)fn  <<  8)
                  | (reg & 0xFC);
    outl(PCI_CFG_ADDR, addr);
    return inl(PCI_CFG_DATA);
}

static void pci_write(uint8_t bus, uint8_t dev, uint8_t fn,
                       uint8_t reg, uint32_t val)
{
    uint32_t addr = 0x80000000u
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)dev << 11)
                  | ((uint32_t)fn  <<  8)
                  | (reg & 0xFC);
    outl(PCI_CFG_ADDR, addr);
    outl(PCI_CFG_DATA, val);
}

/*
 * Find a UHCI controller by class code rather than vendor/device ID
 * (unlike e1000's pci_find_e1000) -- UHCI is a standardized PCI
 * class (0x0C = Serial Bus Controller, 0x03 = USB, prog-if 0x00 =
 * UHCI), implemented by many different vendors, so matching on
 * class/subclass/prog-if is the correct way to find "any UHCI
 * controller" rather than one specific chip.
 *
 * NOTE: only checks function 0 of each device, like e1000's scan --
 * a multi-function PCI device with UHCI on a non-zero function
 * wouldn't be found. Acceptable for now (QEMU's piix3-usb-uhci is a
 * single-function device); worth revisiting if real hardware with a
 * multi-function southbridge needs to be supported later.
 */
static bool pci_find_uhci(uint8_t *bus_out, uint8_t *dev_out)
{
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            uint32_t id = pci_read((uint8_t)bus, dev, 0, 0x00);
            uint16_t vendor = (uint16_t)(id & 0xFFFF);
            if (vendor == 0xFFFF) continue; /* no device here */

            uint32_t class_reg = pci_read((uint8_t)bus, dev, 0, 0x08);
            uint8_t class_code = (uint8_t)((class_reg >> 24) & 0xFF);
            uint8_t subclass   = (uint8_t)((class_reg >> 16) & 0xFF);
            uint8_t prog_if    = (uint8_t)((class_reg >>  8) & 0xFF);

            if (class_code == 0x0C && subclass == 0x03 && prog_if == 0x00) {
                *bus_out = (uint8_t)bus;
                *dev_out = dev;
                return true;
            }
        }
    }
    return false;
}

/*  UHCI I/O register offsets (from IOBASE, all little-endian)  */

#define UHCI_USBCMD    0x00  /* 16-bit */
#define UHCI_USBSTS    0x02  /* 16-bit */
#define UHCI_USBINTR   0x04  /* 16-bit */
#define UHCI_FRNUM     0x06  /* 16-bit */
#define UHCI_FRBASEADD 0x08  /* 32-bit */
#define UHCI_SOFMOD    0x0C  /* 8-bit  */
#define UHCI_PORTSC1   0x10  /* 16-bit */
#define UHCI_PORTSC2   0x12  /* 16-bit */

#define USBCMD_RS       (1 << 0)  /* Run/Stop */
#define USBCMD_HCRESET  (1 << 1)  /* Host Controller Reset (self-clearing) */
#define USBCMD_GRESET   (1 << 2)  /* Global Reset */
#define USBCMD_CF       (1 << 6)  /* Configure Flag (software convention bit) */
#define USBCMD_MAXP     (1 << 7)  /* Max Packet: 0=32 bytes, 1=64 bytes */

#define USBSTS_HCHALTED (1 << 5)

#define PORTSC_CCS      (1 << 0)  /* Current Connect Status */
#define PORTSC_CSC      (1 << 1)  /* Connect Status Change (write-1-clear) */
#define PORTSC_PED      (1 << 2)  /* Port Enabled */
#define PORTSC_PEDC     (1 << 3)  /* Port Enable Change (write-1-clear) */
#define PORTSC_RESET    (1 << 9)  /* Port Reset */

static uint16_t g_iobase = 0;

/*
 * Frame List — 1024 32-bit pointers, one per USB frame (1ms each,
 * 1024 frames = ~1 second cycle). Must be 4KB-aligned physical
 * memory per the UHCI spec (FRBASEADD's low 12 bits are hardwired
 * to 0). Static + aligned, matching e1000.c's pattern of using
 * statically-allocated kernel arrays directly as DMA buffers (this
 * kernel identity-maps its own image, so a kernel virtual address
 * doubles as its physical address here — same assumption e1000's
 * g_tx_ring/g_tx_bufs already rely on).
 *
 * Every entry is left Terminate (bit0=1) for now, i.e. an empty
 * schedule -- no Queue Head is linked in yet, since QH/TD
 * construction is deliberately deferred to the next step (see
 * uhci.h). The frame list still needs to exist and be valid before
 * starting the controller (RS bit), even with nothing scheduled.
 */
static uint32_t g_frame_list[1024] __attribute__((aligned(4096)));

/*
 * Approximate busy-wait, NOT calibrated against a real timer --
 * ata_init()'s IDENTIFY wait and this driver's port-reset delay both
 * run pre-sti, before any interrupt-driven tick counter would be
 * incrementing, so there's no calibrated clock available to use yet.
 * Deliberately generous (errs toward *longer* than the USB-spec
 * minimum) since USB reset timing requirements are minimums, not
 * exact windows -- waiting longer than required is harmless, waiting
 * less risks an unreliable reset on real hardware (QEMU is usually
 * lenient about this, but real UHCI silicon may not be).
 */
static void busy_wait_ms(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        for (volatile uint32_t j = 0; j < 200000; j++) {
            __asm__ volatile ("pause");
        }
    }
}

static inline uint16_t cmd_read(void)  { return inw(g_iobase + UHCI_USBCMD); }
static inline void cmd_write(uint16_t v) { outw(g_iobase + UHCI_USBCMD, v); }
static inline uint16_t sts_read(void)  { return inw(g_iobase + UHCI_USBSTS); }
static inline void sts_write(uint16_t v) { outw(g_iobase + UHCI_USBSTS, v); }
static inline uint16_t portsc_read(int port)
{
    return inw(g_iobase + (port == 0 ? UHCI_PORTSC1 : UHCI_PORTSC2));
}
static inline void portsc_write(int port, uint16_t v)
{
    outw(g_iobase + (port == 0 ? UHCI_PORTSC1 : UHCI_PORTSC2), v);
}

/*
 * Reset one root port and report whether a device is present
 * afterward. Per the UHCI spec, unlike OHCI, a UHCI controller does
 * NOT necessarily auto-enable a port after reset -- software is
 * expected to check Port Enabled (PED) itself and set it if a device
 * is still connected. That quirk is handled below.
 */
static bool uhci_reset_port(int port)
{
    uint16_t status = portsc_read(port);
    if (!(status & PORTSC_CCS)) {
        return false; /* nothing plugged into this port */
    }

    serial_print("[UHCI] port ");
    serial_printhex((uint64_t)port);
    serial_print(": device present, resetting...\n");

    /*
     * Assert reset. Read-modify-write, but deliberately mask out the
     * write-1-to-clear change bits (CSC, PEDC) from the value we
     * read back before writing it -- otherwise a stale change bit
     * from before we started looking would get spuriously cleared
     * as a side effect of setting Reset, which isn't what "reset the
     * port" should imply.
     */
    uint16_t v = portsc_read(port) & ~(uint16_t)(PORTSC_CSC | PORTSC_PEDC);
    portsc_write(port, v | PORTSC_RESET);

    busy_wait_ms(50); /* USB spec: hold reset for >= 50ms */

    v = portsc_read(port) & ~(uint16_t)(PORTSC_CSC | PORTSC_PEDC);
    portsc_write(port, v & ~(uint16_t)PORTSC_RESET);

    busy_wait_ms(10); /* recovery time before the port is usable */

    status = portsc_read(port);
    if (!(status & PORTSC_CCS)) {
        serial_print("[UHCI] port ");
        serial_printhex((uint64_t)port);
        serial_print(": device disappeared after reset\n");
        return false;
    }

    if (!(status & PORTSC_PED)) {
        /* Not auto-enabled -- enable it explicitly (see comment above). */
        v = portsc_read(port) & ~(uint16_t)(PORTSC_CSC | PORTSC_PEDC);
        portsc_write(port, v | PORTSC_PED);
        busy_wait_ms(10);
    }

    status = portsc_read(port);
    bool low_speed = (status & (1 << 8)) != 0;
    serial_print("[UHCI] port ");
    serial_printhex((uint64_t)port);
    serial_print(": reset complete, enabled=");
    serial_printhex((status & PORTSC_PED) ? 1 : 0);
    serial_print(", low_speed=");
    serial_printhex(low_speed ? 1 : 0);
    serial_print("\n");

    return (status & PORTSC_PED) != 0;
}

bool uhci_init(void)
{
    uint8_t bus, dev;
    if (!pci_find_uhci(&bus, &dev)) {
        serial_print("[UHCI] No UHCI controller found\n");
        return false;
    }

    serial_print("[UHCI] Found controller at PCI ");
    serial_printhex(bus);
    serial_print(":");
    serial_printhex(dev);
    serial_print("\n");

    /*
     * Legacy UHCI exposes its registers as I/O space via BAR4
     * (offset 0x20 in the PCI config header, the 5th BAR). Bit 0 of
     * a BAR is set for I/O-space BARs (vs. bit 0 clear for memory
     * BARs) -- masking it off along with bits 1-2 (reserved) leaves
     * the actual base port address.
     */
    uint32_t bar4 = pci_read(bus, dev, 0, 0x20);
    if (!(bar4 & 0x1)) {
        serial_print("[UHCI] BAR4 is not I/O space, unsupported layout\n");
        return false;
    }
    g_iobase = (uint16_t)(bar4 & ~0x3u);

    serial_print("[UHCI] IO base=");
    serial_printhex(g_iobase);
    serial_print("\n");

    /* Enable I/O space access (bit0) and bus mastering (bit2) in the
     * PCI command register -- the controller can't be talked to via
     * I/O ports, or DMA the frame list, without these. */
    uint32_t pci_cmd = pci_read(bus, dev, 0, 0x04);
    pci_cmd |= (1 << 0) | (1 << 2);
    pci_write(bus, dev, 0, 0x04, pci_cmd);

    /* Make sure we're starting from Stop before resetting. */
    cmd_write(0);
    busy_wait_ms(10);

    /* Global Reset: resets the entire USB bus, including attached
     * devices. Software must hold it for >= 10ms then clear it. */
    cmd_write(USBCMD_GRESET);
    busy_wait_ms(10);
    cmd_write(0);
    busy_wait_ms(10);

    /* Host Controller Reset: resets the controller's internal state
     * (schedule, registers) without resetting the bus. Self-clearing
     * -- poll until the controller clears it (bounded, so a
     * misbehaving/emulated-wrong controller can't hang boot). */
    cmd_write(USBCMD_HCRESET);
    bool hc_reset_ok = false;
    for (int i = 0; i < 1000; i++) {
        if (!(cmd_read() & USBCMD_HCRESET)) { hc_reset_ok = true; break; }
        busy_wait_ms(1);
    }
    if (!hc_reset_ok) {
        serial_print("[UHCI] HCRESET did not clear, aborting init\n");
        return false;
    }

    /* Clear any stale status bits (write-1-to-clear register). */
    sts_write(0xFFFF);

    /* Install the (currently empty) frame list. */
    memset(g_frame_list, 0, sizeof(g_frame_list));
    for (int i = 0; i < 1024; i++) {
        g_frame_list[i] = 0x1; /* Terminate bit set: nothing scheduled */
    }
    outl(g_iobase + UHCI_FRBASEADD, (uint32_t)(uintptr_t)g_frame_list);
    outw(g_iobase + UHCI_FRNUM, 0);
    outb(g_iobase + UHCI_SOFMOD, 0x40); /* default SOF timing value */

    /* No interrupts configured yet -- this driver is polling-only
     * for now (USBINTR left at 0), consistent with everything else
     * in uhci_init() running pre-sti anyway. */
    outw(g_iobase + UHCI_USBINTR, 0);

    /* Start the controller: Run/Stop=1, Configure Flag=1 (software
     * convention signaling "system software is done configuring
     * this controller"), Max Packet=1 (64 bytes, standard for
     * full-speed devices). */
    cmd_write(USBCMD_RS | USBCMD_CF | USBCMD_MAXP);
    busy_wait_ms(10);

    if (sts_read() & USBSTS_HCHALTED) {
        serial_print("[UHCI] controller halted after start, init failed\n");
        return false;
    }

    serial_print("[UHCI] controller running\n");

    /* UHCI defines exactly 2 root ports. */
    bool any_device = false;
    if (uhci_reset_port(0)) any_device = true;
    if (uhci_reset_port(1)) any_device = true;

    if (!any_device) {
        serial_print("[UHCI] no devices detected on root ports\n");
    }

    serial_print("[UHCI] foundation init complete "
                 "(no enumeration yet -- transfer layer not implemented)\n");
    return true;
}