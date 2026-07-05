#include "driver.h"
#include "serial.h"
#include <string.h>

static driver_t g_drivers[MAX_DRIVERS];
static uint32_t g_driver_count = 0;

void driver_register(const char *name)
{
    if (g_driver_count >= MAX_DRIVERS || !name) return;
    g_drivers[g_driver_count].name        = name;
    g_drivers[g_driver_count].initialized = false;
    g_driver_count++;
}

void driver_mark_initialized(const char *name)
{
    if (!name) return;
    for (uint32_t i = 0; i < g_driver_count; i++) {
        if (g_drivers[i].name && strcmp(g_drivers[i].name, name) == 0) {
            g_drivers[i].initialized = true;
            return;
        }
    }
}

uint32_t driver_count(void) { return g_driver_count; }

const driver_t *driver_get(uint32_t index)
{
    if (index >= g_driver_count) return NULL;
    return &g_drivers[index];
}

void driver_list_print(void)
{
    serial_print("[DRV ] Registered drivers:\n");
    for (uint32_t i = 0; i < g_driver_count; i++) {
        serial_print("  - ");
        serial_print(g_drivers[i].name);
        serial_print(g_drivers[i].initialized ? " [OK]\n" : " [FAILED]\n");
    }
}