#include "ata.h"
#include "serial.h"
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

static bool ata_wait_not_busy(void)
{
    /* Poll up to ~5M iterations (~5 seconds at 1GHz) */
    for (uint32_t i = 0; i < 5000000; i++) {
        uint8_t status = inb(ATA_STATUS);
        if (!(status & ATA_STATUS_BSY)) return true;
    }
    serial_print("[ATA] timeout waiting for not-busy\n");
    return false;
}

static bool ata_wait_drq(void)
{
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
