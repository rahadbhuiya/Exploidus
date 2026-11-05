#include "msc.h"
#include "uhci.h"
#include "../drivers/serial.h"
#include "../drivers/blockdev.h"
#include <string.h>

static void msc_hexdump(const uint8_t *buf, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        serial_printhex(buf[i]);
        serial_print(" ");
    }
    serial_print("\n");
}

bool usb_msc_bot_command(const usb_msc_ep_t *ep,
                         const uint8_t *cdb, uint8_t cdb_len,
                         uint8_t *data, uint16_t data_len, bool data_in,
                         uint16_t *actual_len_out)
{
    static uint8_t cbw[31] __attribute__((aligned(16)));
    static uint8_t csw[13] __attribute__((aligned(16)));
    memset(cbw, 0, sizeof(cbw));
    memset(csw, 0, sizeof(csw));

    static uint32_t tag_counter = 0x51535430u;
    uint32_t tag = ++tag_counter;

    cbw[0] = (uint8_t)(CBW_SIGNATURE);
    cbw[1] = (uint8_t)(CBW_SIGNATURE >> 8);
    cbw[2] = (uint8_t)(CBW_SIGNATURE >> 16);
    cbw[3] = (uint8_t)(CBW_SIGNATURE >> 24);
    cbw[4] = (uint8_t)(tag);
    cbw[5] = (uint8_t)(tag >> 8);
    cbw[6] = (uint8_t)(tag >> 16);
    cbw[7] = (uint8_t)(tag >> 24);
    cbw[8]  = (uint8_t)(data_len);
    cbw[9]  = (uint8_t)(data_len >> 8);
    cbw[10] = 0; cbw[11] = 0;
    cbw[12] = (data_len > 0 && data_in) ? 0x80 : 0x00;
    cbw[13] = 0; /* LUN 0 */
    cbw[14] = cdb_len;
    memcpy(&cbw[15], cdb, cdb_len);

    uint16_t junk = 0;
    if (!uhci_bulk_transfer(ep->device_addr, ep->bulk_out, ep->toggle_out,
                            cbw, sizeof(cbw), ep->bulk_out_mp, false,
                            &junk)) {
        serial_print("[USB-MSC] BOT command: CBW send failed\n");
        return false;
    }

    uint16_t data_actual = 0;
    if (data_len > 0) {
        bool ok = data_in
            ? uhci_bulk_transfer(ep->device_addr, ep->bulk_in, ep->toggle_in,
                                 data, data_len, ep->bulk_in_mp, true,
                                 &data_actual)
            : uhci_bulk_transfer(ep->device_addr, ep->bulk_out, ep->toggle_out,
                                 data, data_len, ep->bulk_out_mp, false,
                                 &data_actual);
        if (!ok) {
            serial_print("[USB-MSC] BOT command: data phase failed\n");
            return false;
        }
    }

    uint16_t csw_actual = 0;
    if (!uhci_bulk_transfer(ep->device_addr, ep->bulk_in, ep->toggle_in,
                            csw, sizeof(csw), ep->bulk_in_mp, true,
                            &csw_actual)) {
        serial_print("[USB-MSC] BOT command: CSW read failed\n");
        return false;
    }

    uint32_t csw_sig = (uint32_t)csw[0] | ((uint32_t)csw[1] << 8) |
                       ((uint32_t)csw[2] << 16) | ((uint32_t)csw[3] << 24);
    uint8_t csw_status = csw[12];

    if (csw_sig != CSW_SIGNATURE) {
        serial_print("[USB-MSC] BOT command: bad CSW signature=");
        serial_printhex(csw_sig);
        serial_print("\n");
        return false;
    }
    if (csw_status != 0) {
        serial_print("[USB-MSC] BOT command: status=");
        serial_printhex(csw_status);
        serial_print("\n");
        return false;
    }

    if (actual_len_out) *actual_len_out = data_actual;
    return true;
}

bool usb_msc_inquiry(const usb_msc_ep_t *ep)
{
    static uint8_t data[36] __attribute__((aligned(16)));
    memset(data, 0, sizeof(data));

    uint8_t cdb[6] = { 0x12, 0x00, 0x00, 0x00, sizeof(data), 0x00 };
    uint16_t actual = 0;
    if (!usb_msc_bot_command(ep, cdb, sizeof(cdb), data, sizeof(data), true, &actual)) {
        serial_print("[USB-MSC] INQUIRY failed\n");
        return false;
    }

    serial_print("[USB-MSC] INQUIRY ok, data: ");
    msc_hexdump(data, (uint16_t)(actual > sizeof(data) ? sizeof(data) : actual));
    return (actual >= 36);
}

bool usb_msc_read_capacity(const usb_msc_ep_t *ep,
                            uint32_t *last_lba_out,
                            uint32_t *block_size_out)
{
    static uint8_t data[8] __attribute__((aligned(16)));
    memset(data, 0, sizeof(data));

    uint8_t cdb[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    uint16_t actual = 0;
    if (!usb_msc_bot_command(ep, cdb, sizeof(cdb), data, sizeof(data), true, &actual) || actual < 8) {
        serial_print("[USB-MSC] READ CAPACITY failed\n");
        return false;
    }

    uint32_t last_lba = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
                        ((uint32_t)data[2] << 8)  |  (uint32_t)data[3];
    uint32_t block_size = ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) |
                          ((uint32_t)data[6] << 8)  |  (uint32_t)data[7];

    if (last_lba_out) *last_lba_out = last_lba;
    if (block_size_out) *block_size_out = block_size;
    return true;
}

bool usb_msc_read10(const usb_msc_ep_t *ep, uint32_t lba,
                    uint16_t transfer_blocks, uint32_t block_size,
                    uint8_t *buf, uint32_t buf_len)
{
    uint32_t want = (uint32_t)transfer_blocks * block_size;
    if (want == 0 || want > buf_len) return false;
    if (want > (uint32_t)UHCI_MAX_DATA_TDS * ep->bulk_in_mp) return false;

    uint8_t cdb[10] = {
        0x28, 0x00,
        (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
        (uint8_t)(lba >> 8),  (uint8_t)lba,
        0x00,
        (uint8_t)(transfer_blocks >> 8), (uint8_t)transfer_blocks,
        0x00
    };

    uint16_t actual = 0;
    return usb_msc_bot_command(ep, cdb, sizeof(cdb), buf, (uint16_t)want, true, &actual) && (actual >= want);
}

bool usb_msc_write10(const usb_msc_ep_t *ep, uint32_t lba,
                     uint16_t transfer_blocks, uint32_t block_size,
                     const uint8_t *buf, uint32_t buf_len)
{
    uint32_t want = (uint32_t)transfer_blocks * block_size;
    if (want == 0 || want > buf_len) return false;
    if (want > (uint32_t)UHCI_MAX_DATA_TDS * ep->bulk_out_mp) return false;

    uint8_t cdb[10] = {
        0x2A, 0x00,
        (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
        (uint8_t)(lba >> 8),  (uint8_t)lba,
        0x00,
        (uint8_t)(transfer_blocks >> 8), (uint8_t)transfer_blocks,
        0x00
    };

    uint16_t actual = 0;
    return usb_msc_bot_command(ep, cdb, sizeof(cdb), (uint8_t *)buf, (uint16_t)want, false, &actual) && (actual >= want);
}

static bool g_msc_present = false;
static usb_msc_ep_t g_msc_ep;
static bool g_msc_toggle_in, g_msc_toggle_out;
static uint32_t g_msc_block_size;

static bool usb_msc_bd_read(block_device_t *dev, uint32_t lba, uint8_t *buf)
{
    (void)dev;
    if (!g_msc_present) return false;
    return usb_msc_read10(&g_msc_ep, lba, 1, g_msc_block_size, buf, BLOCKDEV_SECTOR_SIZE);
}

static bool usb_msc_bd_write(block_device_t *dev, uint32_t lba, const uint8_t *buf)
{
    (void)dev;
    if (!g_msc_present) return false;
    return usb_msc_write10(&g_msc_ep, lba, 1, g_msc_block_size, buf, BLOCKDEV_SECTOR_SIZE);
}

static block_device_t g_usb_blockdev = {
    .name = "usb0",
    .read_sector = usb_msc_bd_read,
    .write_sector = usb_msc_bd_write,
    .driver_data = NULL,
};

bool usb_msc_probe_and_register(const usb_msc_ep_t *ep, bool toggle_in, bool toggle_out)
{
    if (!usb_msc_inquiry(ep)) {
        serial_print("[USB-MSC] INQUIRY failed, skipping device\n");
        return false;
    }

    uint32_t last_lba = 0, block_size = 0;
    if (!usb_msc_read_capacity(ep, &last_lba, &block_size)) {
        serial_print("[USB-MSC] READ CAPACITY failed\n");
        return false;
    }

    if (block_size != BLOCKDEV_SECTOR_SIZE) {
        serial_print("[USB-MSC] Unsupported block size != 512\n");
        return false;
    }

    static uint8_t sector[4096] __attribute__((aligned(16)));
    memset(sector, 0, sizeof(sector));
    if (usb_msc_read10(ep, 0, 1, block_size, sector, sizeof(sector))) {
        serial_print("[USB-MSC] LBA 0 first 32 bytes: ");
        msc_hexdump(sector, 32);
    }

    static uint8_t pattern[4096] __attribute__((aligned(16)));
    static uint8_t verify[4096] __attribute__((aligned(16)));
    memset(pattern, 0, sizeof(pattern));
    for (uint32_t i = 0; i < block_size && i < sizeof(pattern); i++) {
        pattern[i] = (uint8_t)(i ^ 0xA5);
    }

    if (usb_msc_write10(ep, 1, 1, block_size, pattern, sizeof(pattern))) {
        memset(verify, 0, sizeof(verify));
        if (usb_msc_read10(ep, 1, 1, block_size, verify, sizeof(verify))) {
            bool match = (memcmp(pattern, verify, block_size) == 0);
            serial_print("[USB-MSC] WRITE/READ test: ");
            serial_print(match ? "PASSED\n" : "MISMATCH\n");
        }
    }

    g_msc_ep = *ep;
    g_msc_toggle_in = toggle_in;
    g_msc_toggle_out = toggle_out;
    g_msc_ep.toggle_in = &g_msc_toggle_in;
    g_msc_ep.toggle_out = &g_msc_toggle_out;
    g_msc_block_size = block_size;
    g_msc_present = true;

    blockdev_register(&g_usb_blockdev);
    serial_print("[USB-MSC] Registered block device \"usb0\"\n");
    return true;
}
