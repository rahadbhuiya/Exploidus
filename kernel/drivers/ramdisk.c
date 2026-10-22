#include "ramdisk.h"
#include <string.h>

static uint8_t *g_ramdisk_base = NULL;
static uint64_t g_ramdisk_size = 0;
static bool     g_ramdisk_active = false;

static bool ramdisk_read_sector(block_device_t *dev, uint32_t lba, uint8_t *buf)
{
    if (!g_ramdisk_active || !buf) return false;
    uint64_t offset = (uint64_t)lba * BLOCKDEV_SECTOR_SIZE;
    if (offset + BLOCKDEV_SECTOR_SIZE > g_ramdisk_size) {
        return false;
    }
    memcpy(buf, g_ramdisk_base + offset, BLOCKDEV_SECTOR_SIZE);
    return true;
}

static bool ramdisk_write_sector(block_device_t *dev, uint32_t lba, const uint8_t *buf)
{
    if (!g_ramdisk_active || !buf) return false;
    uint64_t offset = (uint64_t)lba * BLOCKDEV_SECTOR_SIZE;
    if (offset + BLOCKDEV_SECTOR_SIZE > g_ramdisk_size) {
        return false;
    }
    memcpy(g_ramdisk_base + offset, buf, BLOCKDEV_SECTOR_SIZE);
    return true;
}

static block_device_t g_ramdisk_dev = {
    .name         = "ram0",
    .read_sector  = ramdisk_read_sector,
    .write_sector = ramdisk_write_sector,
    .driver_data  = NULL,
};

void ramdisk_init(uint64_t base_addr, uint64_t size)
{
    if (!base_addr || size < BLOCKDEV_SECTOR_SIZE) {
        g_ramdisk_active = false;
        return;
    }
    g_ramdisk_base   = (uint8_t *)(uintptr_t)base_addr;
    g_ramdisk_size   = size;
    g_ramdisk_active = true;
    blockdev_register(&g_ramdisk_dev);
}

bool ramdisk_is_available(void)
{
    return g_ramdisk_active;
}

block_device_t *ramdisk_get_blockdev(void)
{
    return g_ramdisk_active ? &g_ramdisk_dev : NULL;
}

uint64_t ramdisk_get_base(void)
{
    return (uint64_t)(uintptr_t)g_ramdisk_base;
}

uint64_t ramdisk_get_size(void)
{
    return g_ramdisk_size;
}
