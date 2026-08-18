#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "blockdev.h"

#define ATA_SECTOR_SIZE 512

void ata_init(void);
bool ata_read_sector(uint32_t lba, uint8_t *buf);
bool ata_write_sector(uint32_t lba, const uint8_t *buf);

/* The ATA drive's block_device_t, for registering with blockdev_*()
 * / passing to exfs_mount(). Always returns the same static device
 * -- this driver only supports one drive/controller. */
block_device_t *ata_get_blockdev(void);