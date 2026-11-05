#pragma once
#include <stdint.h>
#include <stdbool.h>

/*
 * UHCI (Universal Host Controller Interface) driver -- FOUNDATION
 * ONLY at this stage: PCI detection, register I/O, controller reset,
 * frame list setup, and root port detection/reset.
 * Deliberately NOT implemented yet: Transfer Descriptor / Queue Head
 */

bool uhci_init(void);