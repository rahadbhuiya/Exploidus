#pragma once
#include <stdint.h>
#include <stdbool.h>

/*
 * kernel/drivers/driver.h — minimal driver registry.
 * Honest scope: this is a REGISTRY, not a full probe/attach driver
 * model. Exploidus boots on a known, fixed virtual hardware target
 * (QEMU), so there's no dynamic bus enumeration (PCI, USB, ACPI
 */

#define MAX_DRIVERS 32

typedef struct {
    const char *name;
    bool        initialized;
} driver_t;

void            driver_register(const char *name);
void            driver_mark_initialized(const char *name);
uint32_t        driver_count(void);
const driver_t *driver_get(uint32_t index);
void            driver_list_print(void); /* dumps the table to serial */