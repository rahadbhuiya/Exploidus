#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "blockdev.h"

void ramdisk_init(uint64_t base_addr, uint64_t size);
bool ramdisk_is_available(void);
block_device_t *ramdisk_get_blockdev(void);
uint64_t ramdisk_get_base(void);
uint64_t ramdisk_get_size(void);
