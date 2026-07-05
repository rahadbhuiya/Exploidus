#pragma once
#include <stdint.h>
#include <stdbool.h>

/*
 * kernel/drivers/driver.h — minimal driver registry.
 *
 * Honest scope: this is a REGISTRY, not a full probe/attach driver
 * model. Exploidus boots on a known, fixed virtual hardware target
 * (QEMU), so there's no dynamic bus enumeration (PCI, USB, ACPI
 * device trees) driving *which* drivers get loaded — every driver
 * that exists today already gets initialized unconditionally by
 * kernel_main() in a specific, dependency-sensitive order (e.g. the
 * heap must exist before the process table, which must exist before
 * the scheduler). Retrofitting a real probe/attach/detach model would
 * mean re-deriving and re-validating all of those ordering
 * dependencies — risky to do without being able to build and boot the
 * result.
 *
 * What this DOES give you, safely and additively (it changes no
 * existing init call or its ordering):
 *   - a single place that knows the name of every hardware driver
 *   - whether each one reported itself initialized
 *   - a serial dump of that list at boot, and a C API other code
 *     (e.g. a future "lsdrv" shell command) can query
 *
 * Migrating to real probe/attach dispatch (drivers registering
 * themselves and the kernel deciding what to start, rather than
 * main.c calling each xxx_init() by name) is the natural next step
 * once there's an actual variable-hardware target that needs it.
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