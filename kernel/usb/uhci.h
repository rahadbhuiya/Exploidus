#pragma once
#include <stdint.h>
#include <stdbool.h>

#define UHCI_MAX_DATA_TDS 8

bool uhci_init(void);
bool uhci_bulk_transfer(uint8_t device_addr, uint8_t endpoint,
                        bool *toggle, uint8_t *buf, uint16_t len,
                        uint8_t max_packet, bool data_in,
                        uint16_t *actual_len_out);
