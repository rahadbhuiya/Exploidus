#include "blockdev.h"
#include <string.h>

static block_device_t *g_devices[BLOCKDEV_MAX_DEVICES];
static block_device_t *g_root = NULL;

bool blockdev_register(block_device_t *dev)
{
    if (!dev || !dev->name) return false;

    /* Re-registering an existing name overwrites it in place. */
    for (int i = 0; i < BLOCKDEV_MAX_DEVICES; i++) {
        if (g_devices[i] && strcmp(g_devices[i]->name, dev->name) == 0) {
            g_devices[i] = dev;
            return true;
        }
    }
    for (int i = 0; i < BLOCKDEV_MAX_DEVICES; i++) {
        if (!g_devices[i]) {
            g_devices[i] = dev;
            return true;
        }
    }
    return false; /* table full */
}

block_device_t *blockdev_find(const char *name)
{
    if (!name) return NULL;
    for (int i = 0; i < BLOCKDEV_MAX_DEVICES; i++) {
        if (g_devices[i] && strcmp(g_devices[i]->name, name) == 0) {
            return g_devices[i];
        }
    }
    return NULL;
}

void blockdev_set_root(block_device_t *dev) { g_root = dev; }
block_device_t *blockdev_root(void) { return g_root; }