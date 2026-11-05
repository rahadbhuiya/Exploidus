#pragma once
#include <stdint.h>
#include <stdbool.h>

#define CBW_SIGNATURE 0x43425355u
#define CSW_SIGNATURE 0x53425355u

typedef struct {
    uint8_t device_addr;
    uint8_t bulk_in, bulk_in_mp;
    bool   *toggle_in;
    uint8_t bulk_out, bulk_out_mp;
    bool   *toggle_out;
} usb_msc_ep_t;

bool usb_msc_bot_command(const usb_msc_ep_t *ep,
                         const uint8_t *cdb, uint8_t cdb_len,
                         uint8_t *data, uint16_t data_len, bool data_in,
                         uint16_t *actual_len_out);

bool usb_msc_inquiry(const usb_msc_ep_t *ep);
bool usb_msc_read_capacity(const usb_msc_ep_t *ep, uint32_t *last_lba_out, uint32_t *block_size_out);
bool usb_msc_read10(const usb_msc_ep_t *ep, uint32_t lba, uint16_t transfer_blocks, uint32_t block_size, uint8_t *buf, uint32_t buf_len);
bool usb_msc_write10(const usb_msc_ep_t *ep, uint32_t lba, uint16_t transfer_blocks, uint32_t block_size, const uint8_t *buf, uint32_t buf_len);

bool usb_msc_probe_and_register(const usb_msc_ep_t *ep, bool toggle_in, bool toggle_out);
