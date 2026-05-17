#pragma once
#include <stdint.h>
#include <stdbool.h>

#define ATA_SECTOR_SIZE 512

void ata_init(void);
bool ata_read_sector(uint32_t lba, uint8_t *buf);
bool ata_write_sector(uint32_t lba, const uint8_t *buf);
