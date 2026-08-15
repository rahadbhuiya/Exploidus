#pragma once
#include <stdint.h>
#include <stdbool.h>

/*
 * UHCI (Universal Host Controller Interface) driver -- FOUNDATION
 * ONLY at this stage: PCI detection, register I/O, controller reset,
 * frame list setup, and root port detection/reset.
 *
 * Deliberately NOT implemented yet: Transfer Descriptor / Queue Head
 * construction and control transfers (i.e. no device enumeration --
 * GET_DESCRIPTOR, SET_ADDRESS, etc). That bit-level layout is easy
 * to get subtly wrong without a way to verify against real hardware
 * or a datasheet, and a wrong TD/QH layout tends to fail silently or
 * behave unpredictably rather than loudly -- much harder to debug
 * than a missing feature. This foundation is fully testable on its
 * own (find controller, reset it, detect/reset a connected port) and
 * is the safe base to build the transfer layer on next.
 */

bool uhci_init(void);