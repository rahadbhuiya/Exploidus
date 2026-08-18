#pragma once
#include <stdint.h>
#include <stdbool.h>

#define BLOCKDEV_SECTOR_SIZE 512
#define BLOCKDEV_MAX_DEVICES 4

/*
 * Generic block-device interface. Any storage backend (ATA, USB mass
 * storage, ...) implements read_sector/write_sector and registers
 * itself; filesystem code (ExFS) goes through this interface instead
 * of calling a specific driver's functions directly, so it works
 * unmodified regardless of which physical device backs a given
 * volume.
 *
 * driver_data is an opaque pointer the backing driver can use for
 * whatever per-device state it needs (e.g. USB mass storage needs to
 * remember which endpoint pair/device address a given stick is on --
 * ATA doesn't need this, since there's only one drive/controller,
 * but the field exists so the interface doesn't have to change if
 * that ever stops being true).
 */
typedef struct block_device {
    const char *name;
    bool (*read_sector)(struct block_device *dev, uint32_t lba, uint8_t *buf);
    bool (*write_sector)(struct block_device *dev, uint32_t lba,
                          const uint8_t *buf);
    void *driver_data;
} block_device_t;

/* Registers dev in the global device table (by name -- re-registering
 * the same name overwrites the previous entry, which is fine for
 * something like "the USB stick was unplugged and a different one
 * plugged in"). Returns false if the table is full. */
bool blockdev_register(block_device_t *dev);

block_device_t *blockdev_find(const char *name);

/* The device the root filesystem is mounted from. NULL until
 * blockdev_set_root() is called (main.c does this right after
 * ata_init(), before exfs_mount()). */
void blockdev_set_root(block_device_t *dev);
block_device_t *blockdev_root(void);