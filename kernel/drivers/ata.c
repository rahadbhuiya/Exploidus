#include "ata.h"
#include "serial.h"
#include "../arch/x86_64/irq.h"
#include <string.h>

/* ATA primary bus ports */
#define ATA_DATA        0x1F0
#define ATA_ERROR       0x1F1
#define ATA_SECTOR_CNT  0x1F2
#define ATA_LBA_LO      0x1F3
#define ATA_LBA_MID     0x1F4
#define ATA_LBA_HI      0x1F5
#define ATA_DRIVE_SEL   0x1F6
#define ATA_STATUS      0x1F7
#define ATA_CMD         0x1F7

#define ATA_IRQ         14   /* primary ATA channel, ISA-wired IRQ14 */

#define ATA_STATUS_BSY  0x80
#define ATA_STATUS_RDY  0x40
#define ATA_STATUS_DRQ  0x08
#define ATA_STATUS_ERR  0x01

#define ATA_CMD_READ    0x20
#define ATA_CMD_WRITE   0x30
#define ATA_CMD_IDENTIFY 0xEC

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t val;
    __asm__ volatile ("inw %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void outw(uint16_t port, uint16_t val)
{
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

/* Is IF (interrupt flag) currently set? During early boot, ata_init()
 * and the first exfs_mount() read run *before* main.c's global sti,
 * so the IRQ path below would hlt forever waiting for an interrupt
 * that can never be delivered. Everything after boot (VFS reads
 * triggered by userspace syscalls) runs with interrupts on, so this
 * lets the same code safely use the IRQ path only when it's actually
 * possible to. */
static inline bool interrupts_enabled(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; pop %0" : "=r"(flags));
    return (flags & (1ULL << 9)) != 0;
}

/* Set by ata_irq_handler() when IRQ14 fires. Volatile: written from
 * interrupt context, read from a polling loop the compiler must not
 * reorder/cache across. */
static volatile bool g_ata_irq_pending = false;

static void ata_irq_handler(interrupt_frame_t *frame)
{
    (void)frame;
    /*
     * Reading the status register is how a PC AT-compatible ATA
     * controller acknowledges/clears its IRQ line (per the ATA
     * spec, INTRQ stays asserted until the host reads Status).
     * Without this read the controller would never lower the line
     * again and we'd get exactly one interrupt total.
     */
    (void)inb(ATA_STATUS);
    g_ata_irq_pending = true;
}

static void ata_irq_install(void)
{
    irq_register(ATA_IRQ, ata_irq_handler);
}

static bool ata_wait_not_busy(void)
{
    /*
     * IRQ-driven path: if interrupts are enabled, park the CPU with
     * hlt instead of spinning -- an ATA command's IRQ fires once BSY
     * clears, so we can react to it directly rather than burning a
     * core polling a port thousands of times a second. Bounded to
     * ~5M wakeups so a wedged/IRQ-less drive still times out instead
     * of hanging forever, same ceiling the old busy-poll used.
     */
    if (interrupts_enabled()) {
        g_ata_irq_pending = false;
        for (uint32_t i = 0; i < 5000000; i++) {
            uint8_t status = inb(ATA_STATUS);
            if (!(status & ATA_STATUS_BSY)) return true;
            if (!g_ata_irq_pending) {
                __asm__ volatile ("hlt");
            }
        }
        serial_print("[ATA] timeout waiting for not-busy (irq path)\n");
        return false;
    }

    /* Early-boot path (pre-sti): interrupts can't be delivered yet,
     * so this has to busy-poll. Same as the original implementation. */
    for (uint32_t i = 0; i < 5000000; i++) {
        uint8_t status = inb(ATA_STATUS);
        if (!(status & ATA_STATUS_BSY)) return true;
    }
    serial_print("[ATA] timeout waiting for not-busy\n");
    return false;
}

static bool ata_wait_drq(void)
{
    if (interrupts_enabled()) {
        g_ata_irq_pending = false;
        for (uint32_t i = 0; i < 5000000; i++) {
            uint8_t status = inb(ATA_STATUS);
            if (status & ATA_STATUS_ERR) {
                serial_print("[ATA] error bit set\n");
                return false;
            }
            if (status & ATA_STATUS_DRQ) return true;
            if (!g_ata_irq_pending) {
                __asm__ volatile ("hlt");
            }
        }
        serial_print("[ATA] timeout waiting for DRQ (irq path)\n");
        return false;
    }

    for (uint32_t i = 0; i < 5000000; i++) {
        uint8_t status = inb(ATA_STATUS);
        if (status & ATA_STATUS_ERR) {
            serial_print("[ATA] error bit set\n");
            return false;
        }
        if (status & ATA_STATUS_DRQ) return true;
    }
    serial_print("[ATA] timeout waiting for DRQ\n");
    return false;
}

void ata_init(void)
{
    /* Select master drive */
    outb(ATA_DRIVE_SEL, 0xA0);

    /* Send IDENTIFY — if it times out, no disk is present */
    outb(ATA_CMD, ATA_CMD_IDENTIFY);

    uint8_t status = inb(ATA_STATUS);
    if (status == 0) {
        serial_print("[ATA] No drive detected\n");
        return;
    }

    if (!ata_wait_not_busy()) return;

    uint16_t identify[256];
    for (int i = 0; i < 256; i++)
        identify[i] = inw(ATA_DATA);

    serial_print("[ATA] Drive present. Sectors: ");
    uint32_t sectors = ((uint32_t)identify[61] << 16) | identify[60];
    serial_printhex((uint64_t)sectors);
    serial_print("\n");

    /*
     * Register IRQ14 now. Safe even though we're still pre-sti at
     * this point in boot: irq_register() only unmasks the PIC line
     * and stores the handler pointer, it doesn't require interrupts
     * to already be globally enabled -- and ata_wait_*() checks
     * interrupts_enabled() before ever relying on the IRQ firing.
     * Registering here means the very first post-boot disk access
     * already gets the IRQ-driven path.
     */
    ata_irq_install();
}

bool ata_read_sector(uint32_t lba, uint8_t *buf)
{
    if (!ata_wait_not_busy()) return false;

    outb(ATA_DRIVE_SEL, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
    outb(ATA_SECTOR_CNT, 1);
    outb(ATA_LBA_LO,  (uint8_t)(lba & 0xFF));
    outb(ATA_LBA_MID, (uint8_t)((lba >> 8)  & 0xFF));
    outb(ATA_LBA_HI,  (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_CMD, ATA_CMD_READ);

    if (!ata_wait_drq()) return false;

    for (int i = 0; i < 256; i++) {
        uint16_t word = inw(ATA_DATA);
        buf[i * 2]     = (uint8_t)(word & 0xFF);
        buf[i * 2 + 1] = (uint8_t)(word >> 8);
    }
    return true;
}

bool ata_write_sector(uint32_t lba, const uint8_t *buf)
{
    if (!ata_wait_not_busy()) return false;

    outb(ATA_DRIVE_SEL, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
    outb(ATA_SECTOR_CNT, 1);
    outb(ATA_LBA_LO,  (uint8_t)(lba & 0xFF));
    outb(ATA_LBA_MID, (uint8_t)((lba >> 8)  & 0xFF));
    outb(ATA_LBA_HI,  (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_CMD, ATA_CMD_WRITE);

    if (!ata_wait_drq()) return false;

    for (int i = 0; i < 256; i++) {
        uint16_t word = (uint16_t)buf[i * 2] |
                        ((uint16_t)buf[i * 2 + 1] << 8);
        outw(ATA_DATA, word);
    }

    /* Flush write cache */
    outb(ATA_CMD, 0xE7);
    ata_wait_not_busy();
    return true;
}

/*  Block-device interface (see kernel/drivers/blockdev.h)  */

static bool ata_bd_read(block_device_t *dev, uint32_t lba, uint8_t *buf)
{
    (void)dev; /* single controller/drive -- no per-device state needed */
    return ata_read_sector(lba, buf);
}

static bool ata_bd_write(block_device_t *dev, uint32_t lba,
                          const uint8_t *buf)
{
    (void)dev;
    return ata_write_sector(lba, buf);
}

static block_device_t g_ata_blockdev = {
    .name = "ata0",
    .read_sector = ata_bd_read,
    .write_sector = ata_bd_write,
    .driver_data = NULL,
};

block_device_t *ata_get_blockdev(void)
{
    return &g_ata_blockdev;
}