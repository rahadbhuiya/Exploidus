#include "uhci.h"
#include "../drivers/serial.h"
#include <string.h>

/*  Port I/O helpers (same pattern as e1000.c / ata.c)  */

static inline void outl(uint16_t port, uint32_t val)
{
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port) : "memory");
}
static inline uint32_t inl(uint16_t port)
{
    uint32_t v;
    __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port) : "memory");
    return v;
}
static inline void outw(uint16_t port, uint16_t val)
{
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port) : "memory");
}
static inline uint16_t inw(uint16_t port)
{
    uint16_t v;
    __asm__ volatile ("inw %1, %0" : "=a"(v) : "Nd"(port) : "memory");
    return v;
}
static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port) : "memory");
}

/*  PCI config access (same pattern as e1000.c)  */

#define PCI_CFG_ADDR  0xCF8
#define PCI_CFG_DATA  0xCFC

static uint32_t pci_read(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg)
{
    uint32_t addr = 0x80000000u
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)dev << 11)
                  | ((uint32_t)fn  <<  8)
                  | (reg & 0xFC);
    outl(PCI_CFG_ADDR, addr);
    return inl(PCI_CFG_DATA);
}

static void pci_write(uint8_t bus, uint8_t dev, uint8_t fn,
                       uint8_t reg, uint32_t val)
{
    uint32_t addr = 0x80000000u
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)dev << 11)
                  | ((uint32_t)fn  <<  8)
                  | (reg & 0xFC);
    outl(PCI_CFG_ADDR, addr);
    outl(PCI_CFG_DATA, val);
}

/*
 * Find a UHCI controller by class code rather than vendor/device ID
 * (unlike e1000's pci_find_e1000) -- UHCI is a standardized PCI
 * class (0x0C = Serial Bus Controller, 0x03 = USB, prog-if 0x00 =
 * UHCI), implemented by many different vendors, so matching on
 * class/subclass/prog-if is the correct way to find "any UHCI
 * controller" rather than one specific chip.
 *
 * NOTE: only checks function 0 of each device, like e1000's scan --
 * a multi-function PCI device with UHCI on a non-zero function
 * wouldn't be found. Acceptable for now (QEMU's piix3-usb-uhci is a
 * single-function device); worth revisiting if real hardware with a
 * multi-function southbridge needs to be supported later.
 */
static bool pci_find_uhci(uint8_t *bus_out, uint8_t *dev_out)
{
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            uint32_t id = pci_read((uint8_t)bus, dev, 0, 0x00);
            uint16_t vendor = (uint16_t)(id & 0xFFFF);
            if (vendor == 0xFFFF) continue; /* no device here */

            uint32_t class_reg = pci_read((uint8_t)bus, dev, 0, 0x08);
            uint8_t class_code = (uint8_t)((class_reg >> 24) & 0xFF);
            uint8_t subclass   = (uint8_t)((class_reg >> 16) & 0xFF);
            uint8_t prog_if    = (uint8_t)((class_reg >>  8) & 0xFF);

            if (class_code == 0x0C && subclass == 0x03 && prog_if == 0x00) {
                *bus_out = (uint8_t)bus;
                *dev_out = dev;
                return true;
            }
        }
    }
    return false;
}

/*  UHCI I/O register offsets (from IOBASE, all little-endian)  */

#define UHCI_USBCMD    0x00  /* 16-bit */
#define UHCI_USBSTS    0x02  /* 16-bit */
#define UHCI_USBINTR   0x04  /* 16-bit */
#define UHCI_FRNUM     0x06  /* 16-bit */
#define UHCI_FRBASEADD 0x08  /* 32-bit */
#define UHCI_SOFMOD    0x0C  /* 8-bit  */
#define UHCI_PORTSC1   0x10  /* 16-bit */
#define UHCI_PORTSC2   0x12  /* 16-bit */

#define USBCMD_RS       (1 << 0)  /* Run/Stop */
#define USBCMD_HCRESET  (1 << 1)  /* Host Controller Reset (self-clearing) */
#define USBCMD_GRESET   (1 << 2)  /* Global Reset */
#define USBCMD_CF       (1 << 6)  /* Configure Flag (software convention bit) */
#define USBCMD_MAXP     (1 << 7)  /* Max Packet: 0=32 bytes, 1=64 bytes */

#define USBSTS_HCHALTED (1 << 5)

#define PORTSC_CCS      (1 << 0)  /* Current Connect Status */
#define PORTSC_CSC      (1 << 1)  /* Connect Status Change (write-1-clear) */
#define PORTSC_PED      (1 << 2)  /* Port Enabled */
#define PORTSC_PEDC     (1 << 3)  /* Port Enable Change (write-1-clear) */
#define PORTSC_RESET    (1 << 9)  /* Port Reset */

static uint16_t g_iobase = 0;

/*
 * Frame List — 1024 32-bit pointers, one per USB frame (1ms each,
 * 1024 frames = ~1 second cycle). Must be 4KB-aligned physical
 * memory per the UHCI spec (FRBASEADD's low 12 bits are hardwired
 * to 0). Static + aligned, matching e1000.c's pattern of using
 * statically-allocated kernel arrays directly as DMA buffers (this
 * kernel identity-maps its own image, so a kernel virtual address
 * doubles as its physical address here — same assumption e1000's
 * g_tx_ring/g_tx_bufs already rely on).
 *
 * Every entry is left Terminate (bit0=1) for now, i.e. an empty
 * schedule -- no Queue Head is linked in yet, since QH/TD
 * construction is deliberately deferred to the next step (see
 * uhci.h). The frame list still needs to exist and be valid before
 * starting the controller (RS bit), even with nothing scheduled.
 */
static uint32_t g_frame_list[1024] __attribute__((aligned(4096)));

/*
 * Approximate busy-wait, NOT calibrated against a real timer --
 * ata_init()'s IDENTIFY wait and this driver's port-reset delay both
 * run pre-sti, before any interrupt-driven tick counter would be
 * incrementing, so there's no calibrated clock available to use yet.
 * Deliberately generous (errs toward *longer* than the USB-spec
 * minimum) since USB reset timing requirements are minimums, not
 * exact windows -- waiting longer than required is harmless, waiting
 * less risks an unreliable reset on real hardware (QEMU is usually
 * lenient about this, but real UHCI silicon may not be).
 */
static void busy_wait_ms(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        for (volatile uint32_t j = 0; j < 200000; j++) {
            __asm__ volatile ("pause");
        }
    }
}

static inline uint16_t cmd_read(void)  { return inw(g_iobase + UHCI_USBCMD); }
static inline void cmd_write(uint16_t v) { outw(g_iobase + UHCI_USBCMD, v); }
static inline uint16_t sts_read(void)  { return inw(g_iobase + UHCI_USBSTS); }
static inline void sts_write(uint16_t v) { outw(g_iobase + UHCI_USBSTS, v); }
static inline uint16_t portsc_read(int port)
{
    return inw(g_iobase + (port == 0 ? UHCI_PORTSC1 : UHCI_PORTSC2));
}
static inline void portsc_write(int port, uint16_t v)
{
    outw(g_iobase + (port == 0 ? UHCI_PORTSC1 : UHCI_PORTSC2), v);
}

/* =====================================================================
 * Transfer layer: Transfer Descriptors (TD) and Queue Heads (QH).
 *
 * CONFIDENCE NOTE: the register/reset/port code above is
 * high-confidence (simple, single-purpose bits, consistent across
 * every source). The TD control/status bit layout below is NOT at
 * that same confidence level -- it's built from best recollection of
 * the Linux uhci-hcd driver's TD_CTRL_* constants, without a way to
 * verify against a datasheet or real hardware in this environment.
 * If a transfer doesn't behave as expected, uhci_control_transfer()
 * dumps every TD's raw ctrl_status DWORD to serial specifically so
 * the actual bit pattern can be inspected/cross-checked by hand
 * instead of trusting these #defines blindly.
 * =====================================================================
 */

typedef struct __attribute__((packed, aligned(16))) {
    uint32_t link;         /* -> next TD, or Terminate */
    uint32_t ctrl_status;
    uint32_t token;
    uint32_t buffer;       /* physical address of this TD's data buffer */
} uhci_td_t;

typedef struct __attribute__((packed, aligned(16))) {
    uint32_t head_link;    /* -> next QH in the schedule, or Terminate */
    uint32_t element_link; /* -> first TD of this QH's chain, or Terminate */
} uhci_qh_t;

/* Link Pointer flag bits (shared format used by frame list entries,
 * QH links, and TD links alike). */
#define UHCI_PTR_TERMINATE (1u << 0)
#define UHCI_PTR_QH        (1u << 1)
#define UHCI_PTR_DEPTH     (1u << 2) /* depth-first: HC moves straight to
                                       * this TD's own link next, instead
                                       * of returning to the frame list */

/* TD ctrl_status DWORD (best-recollection layout, see CONFIDENCE NOTE) */
#define TD_CTRL_CERR3    (3u << 27) /* error counter: retry limit 3 */
#define TD_CTRL_LS       (1u << 26) /* low-speed device */
#define TD_CTRL_IOC      (1u << 24) /* interrupt on complete */
#define TD_CTRL_ACTIVE   (1u << 23)
#define TD_CTRL_STALLED  (1u << 22)
#define TD_CTRL_DBUFERR  (1u << 21)
#define TD_CTRL_BABBLE   (1u << 20)
#define TD_CTRL_NAK      (1u << 19)
#define TD_CTRL_CRCTIMEO (1u << 18)
#define TD_CTRL_BITSTUFF (1u << 17)
#define TD_CTRL_ACTLEN_MASK 0x7FFu
#define TD_CTRL_ERROR_MASK (TD_CTRL_STALLED | TD_CTRL_DBUFERR | \
                             TD_CTRL_BABBLE | TD_CTRL_CRCTIMEO | \
                             TD_CTRL_BITSTUFF)

/* TD token DWORD */
#define USB_PID_SETUP 0x2Du
#define USB_PID_IN    0x69u
#define USB_PID_OUT   0xE1u

static inline uint16_t maxlen_field(uint16_t len)
{
    /* UHCI encodes "N-1" for an N-byte packet, EXCEPT a 0-byte
     * (zero-length) packet, which is the special value 0x7FF. Using
     * unsigned wraparound for len=0 lands on 0xFFFF -> masked to
     * 0x7FF automatically, so no separate special case is needed. */
    return (uint16_t)((uint16_t)(len - 1u) & 0x7FFu);
}

static inline uint32_t td_token(uint8_t pid, uint8_t addr, uint8_t endp,
                                 bool data1, uint16_t len)
{
    return (uint32_t)pid
         | ((uint32_t)(addr & 0x7F) << 8)
         | ((uint32_t)(endp & 0x0F) << 15)
         | ((data1 ? 1u : 0u) << 19)
         | ((uint32_t)maxlen_field(len) << 21);
}

/* One persistent QH used for every control transfer we issue.
 * Linked into every frame-list entry once at init, so it's always
 * visited (empty QHs are cheap for the HC to skip past). */
static uhci_qh_t g_control_qh __attribute__((aligned(16)));

static uint8_t g_setup_buf[8] __attribute__((aligned(16)));

static void dump_td(const char *label, const uhci_td_t *td)
{
    serial_print("[UHCI]   ");
    serial_print(label);
    serial_print(" ctrl_status=");
    serial_printhex(td->ctrl_status);
    serial_print(" token=");
    serial_printhex(td->token);
    serial_print("\n");
}

#define UHCI_MAX_DATA_TDS 8  /* supports up to 64 bytes at the smallest
                               * legal max-packet-size (8) -- enough
                               * for a typical simple HID config
                               * descriptor (Config+Interface+HID+
                               * Endpoint = ~34 bytes) */

/*
 * Synchronous control transfer: SETUP + optional multi-packet DATA
 * stage + STATUS. The DATA stage is split into ceil(data_len /
 * max_packet) packets, each its own TD, chained together with
 * alternating data toggle starting at DATA1 (USB rule: SETUP is
 * always DATA0, the first DATA-stage packet is always DATA1,
 * alternating after that; STATUS is always DATA1 regardless of how
 * many DATA packets preceded it -- that's a fixed rule, not a
 * continuation of the alternation).
 */
static bool uhci_control_transfer(uint8_t device_addr,
                                   const uint8_t setup[8],
                                   uint8_t *data_buf, uint16_t data_len,
                                   uint8_t max_packet,
                                   bool data_in,
                                   uint16_t *actual_len_out)
{
    static uhci_td_t td_setup  __attribute__((aligned(16)));
    static uhci_td_t td_data[UHCI_MAX_DATA_TDS] __attribute__((aligned(16)));
    static uhci_td_t td_status __attribute__((aligned(16)));

    bool has_data = (data_len > 0 && data_buf != NULL);
    memcpy(g_setup_buf, setup, 8);

    if (max_packet == 0) max_packet = 8; /* sane fallback */

    uint32_t n_data_tds = 0;
    if (has_data) {
        n_data_tds = (data_len + max_packet - 1) / max_packet;
        if (n_data_tds > UHCI_MAX_DATA_TDS) {
            serial_print("[UHCI] control transfer: data too large for "
                         "TD chain, aborting\n");
            return false;
        }
    }

    td_setup.token       = td_token(USB_PID_SETUP, device_addr, 0, false, 8);
    td_setup.buffer      = (uint32_t)(uintptr_t)g_setup_buf;
    td_setup.ctrl_status = TD_CTRL_ACTIVE | TD_CTRL_CERR3;

    if (has_data) {
        td_setup.link = (uint32_t)(uintptr_t)&td_data[0] | UHCI_PTR_DEPTH;

        uint16_t remaining = data_len;
        uint8_t *cursor = data_buf;
        for (uint32_t i = 0; i < n_data_tds; i++) {
            uint16_t this_len = remaining < max_packet
                               ? remaining : (uint16_t)max_packet;
            bool toggle = (i % 2 == 0) ? true : false; /* DATA1,DATA0,... */

            td_data[i].token  = td_token(data_in ? USB_PID_IN : USB_PID_OUT,
                                          device_addr, 0, toggle, this_len);
            td_data[i].buffer = (uint32_t)(uintptr_t)cursor;
            td_data[i].ctrl_status = TD_CTRL_ACTIVE | TD_CTRL_CERR3;
            td_data[i].link = (i + 1 < n_data_tds)
                ? ((uint32_t)(uintptr_t)&td_data[i + 1] | UHCI_PTR_DEPTH)
                : ((uint32_t)(uintptr_t)&td_status | UHCI_PTR_DEPTH);

            cursor    += this_len;
            remaining -= this_len;
        }
    } else {
        td_setup.link = (uint32_t)(uintptr_t)&td_status | UHCI_PTR_DEPTH;
    }

    /* STATUS stage: opposite direction of DATA (or IN, if there was
     * no DATA stage), always DATA1, always zero-length. */
    bool status_in = has_data ? !data_in : true;
    td_status.token       = td_token(status_in ? USB_PID_IN : USB_PID_OUT,
                                      device_addr, 0, true, 0);
    td_status.buffer      = (uint32_t)(uintptr_t)g_setup_buf; /* unused (0-len) */
    td_status.ctrl_status = TD_CTRL_ACTIVE | TD_CTRL_CERR3 | TD_CTRL_IOC;
    td_status.link        = UHCI_PTR_TERMINATE;

    /* Kick the whole chain off by pointing our QH's element link at
     * the SETUP TD. The HC advances through SETUP -> DATA... ->
     * STATUS on its own (following each TD's own link field), across
     * however many 1ms frames it takes. */
    g_control_qh.element_link = (uint32_t)(uintptr_t)&td_setup;

    bool ok = false;
    for (uint32_t i = 0; i < 500; i++) { /* ~500ms bound */
        if (!(td_status.ctrl_status & TD_CTRL_ACTIVE)) {
            ok = !(td_status.ctrl_status & TD_CTRL_ERROR_MASK);
            break;
        }
        busy_wait_ms(1);
    }

    if (!ok) {
        serial_print("[UHCI] control transfer failed/timed out, dumping TDs:\n");
        dump_td("SETUP ", &td_setup);
        for (uint32_t i = 0; i < n_data_tds; i++) dump_td("DATA  ", &td_data[i]);
        dump_td("STATUS", &td_status);
    } else if (actual_len_out) {
        uint16_t total = 0;
        for (uint32_t i = 0; i < n_data_tds; i++) {
            total += (uint16_t)((td_data[i].ctrl_status & TD_CTRL_ACTLEN_MASK) + 1);
        }
        *actual_len_out = total;
    }

    /* Detach the chain so this QH goes back to idle for next time. */
    g_control_qh.element_link = UHCI_PTR_TERMINATE;

    return ok;
}

static void hexdump(const uint8_t *buf, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        serial_printhex(buf[i]);
        serial_print(" ");
    }
    serial_print("\n");
}

/*
 * Interrupt IN transfer -- a single bare IN-token TD, no SETUP/STATUS
 * (those are control-transfer-only concepts). Used for HID reports
 * (mouse/tablet/keyboard position & button state).
 *
 * IMPORTANT: a timeout here does NOT necessarily mean failure. Per
 * spec, a NAK response does not clear the TD's Active bit or consume
 * the error-retry counter -- the HC just keeps retrying the same TD
 * on its own every frame until either real data arrives or a genuine
 * error occurs. So "device has nothing new to report yet" and
 * "transfer timed out waiting" look identical from here: Active is
 * still 1 when our poll bound runs out. That's expected/normal for
 * an interrupt endpoint with no new input, not a driver bug -- the
 * return value distinguishes it (false + *had_error_out=false) from
 * a real error (false + *had_error_out=true).
 */
static bool uhci_interrupt_in(uint8_t device_addr, uint8_t endpoint,
                               bool *toggle, uint8_t *buf, uint16_t len,
                               uint16_t *actual_len_out, bool *had_error_out)
{
    static uhci_td_t td __attribute__((aligned(16)));

    td.token       = td_token(USB_PID_IN, device_addr, endpoint, *toggle, len);
    td.buffer      = (uint32_t)(uintptr_t)buf;
    td.ctrl_status = TD_CTRL_ACTIVE | TD_CTRL_CERR3;
    td.link        = UHCI_PTR_TERMINATE;

    g_control_qh.element_link = (uint32_t)(uintptr_t)&td;

    bool got_data = false;
    bool had_error = false;
    for (uint32_t i = 0; i < 50; i++) { /* ~50ms -- short, this is a poll,
                                          * not a one-shot blocking wait */
        if (!(td.ctrl_status & TD_CTRL_ACTIVE)) {
            had_error = (td.ctrl_status & TD_CTRL_ERROR_MASK) != 0;
            got_data = !had_error;
            break;
        }
        busy_wait_ms(1);
    }

    g_control_qh.element_link = UHCI_PTR_TERMINATE;

    if (had_error_out) *had_error_out = had_error;
    if (!got_data) return false;

    *toggle = !*toggle; /* only advance the toggle on a real completion */
    if (actual_len_out) {
        *actual_len_out = (uint16_t)((td.ctrl_status & TD_CTRL_ACTLEN_MASK) + 1);
    }
    return true;
}

/* Descriptor type constants, for walking a raw configuration
 * descriptor buffer (Config -> Interface -> [Class-specific] ->
 * Endpoint, back to back, total length given by the Config
 * descriptor's own wTotalLength field). */
#define USB_DESC_INTERFACE 0x04
#define USB_DESC_ENDPOINT  0x05
#define USB_EP_ATTR_TYPE_MASK 0x03
#define USB_EP_ATTR_INTERRUPT 0x03
#define USB_EP_ATTR_BULK       0x02
#define USB_EP_DIR_IN 0x80

#define USB_CLASS_MASS_STORAGE 0x08
#define USB_CLASS_HID          0x03

typedef struct {
    uint8_t if_class, if_subclass, if_protocol;
    bool have_interrupt_in;
    uint8_t interrupt_in_addr, interrupt_in_maxpacket;
    bool have_bulk_in;
    uint8_t bulk_in_addr, bulk_in_maxpacket;
    bool have_bulk_out;
    uint8_t bulk_out_addr, bulk_out_maxpacket;
} usb_config_info_t;

/*
 * Single-pass walk of a raw configuration descriptor, pulling out
 * the first interface's class/subclass/protocol and any interrupt-IN
 * / bulk-IN / bulk-OUT endpoints it declares. Deliberately simple:
 * only looks at the FIRST interface found (no multi-interface
 * composite device support, no alternate-setting handling) -- fine
 * for the single-function devices (a HID tablet, a mass-storage
 * stick) this driver targets so far.
 */
static void parse_config_descriptor(const uint8_t *buf, uint16_t total_len,
                                     usb_config_info_t *info)
{
    memset(info, 0, sizeof(*info));
    bool seen_interface = false;

    uint16_t off = 0;
    while (off + 2 <= total_len) {
        uint8_t len  = buf[off];
        uint8_t type = buf[off + 1];
        if (len == 0 || off + len > total_len) break; /* malformed, stop */

        if (type == USB_DESC_INTERFACE && len >= 9) {
            if (seen_interface) break; /* stop at the 2nd interface */
            seen_interface = true;
            info->if_class    = buf[off + 5];
            info->if_subclass = buf[off + 6];
            info->if_protocol = buf[off + 7];
        } else if (type == USB_DESC_ENDPOINT && len >= 7) {
            uint8_t addr = buf[off + 2];
            uint8_t attr = buf[off + 3];
            uint8_t ep_type = attr & USB_EP_ATTR_TYPE_MASK;
            bool is_in = (addr & USB_EP_DIR_IN) != 0;
            uint8_t ep_num = (uint8_t)(addr & 0x0F);
            uint8_t maxpacket = buf[off + 4]; /* low byte -- fine for the
                                                * small (<256) sizes these
                                                * endpoint types use */

            if (ep_type == USB_EP_ATTR_INTERRUPT && is_in &&
                !info->have_interrupt_in) {
                info->have_interrupt_in = true;
                info->interrupt_in_addr = ep_num;
                info->interrupt_in_maxpacket = maxpacket;
            } else if (ep_type == USB_EP_ATTR_BULK && is_in &&
                       !info->have_bulk_in) {
                info->have_bulk_in = true;
                info->bulk_in_addr = ep_num;
                info->bulk_in_maxpacket = maxpacket;
            } else if (ep_type == USB_EP_ATTR_BULK && !is_in &&
                       !info->have_bulk_out) {
                info->have_bulk_out = true;
                info->bulk_out_addr = ep_num;
                info->bulk_out_maxpacket = maxpacket;
            }
        }
        off += len;
    }
}

/*
 * Bulk transfer -- like uhci_interrupt_in() but for bulk endpoints,
 * and supports IN or OUT, and multi-packet transfers (chained TDs
 * with alternating toggle, same splitting logic as the control
 * transfer's DATA stage). No SETUP/STATUS phases -- those are
 * control-transfer-only concepts; a bulk transfer is just the data
 * packets themselves.
 *
 * Bulk endpoints don't get the "NAK doesn't mean error" treatment
 * interrupt endpoints do in the same forgiving way -- a bulk OUT to
 * a mass-storage device is expected to be accepted promptly, so a
 * timeout here is treated as a real failure, not a normal "nothing
 * to report yet" (there's no periodic-polling concept for bulk).
 */
static bool uhci_bulk_transfer(uint8_t device_addr, uint8_t endpoint,
                                bool *toggle, uint8_t *buf, uint16_t len,
                                uint8_t max_packet, bool data_in,
                                uint16_t *actual_len_out)
{
    static uhci_td_t tds[UHCI_MAX_DATA_TDS] __attribute__((aligned(16)));

    if (max_packet == 0) max_packet = 8;
    uint32_t n = (len + max_packet - 1) / max_packet;
    if (n == 0) n = 1; /* a zero-length bulk transfer is still 1 packet */
    if (n > UHCI_MAX_DATA_TDS) {
        serial_print("[UHCI] bulk transfer: too large for TD chain\n");
        return false;
    }

    uint16_t remaining = len;
    uint8_t *cursor = buf;
    for (uint32_t i = 0; i < n; i++) {
        uint16_t this_len = remaining < max_packet
                           ? remaining : (uint16_t)max_packet;
        tds[i].token = td_token(data_in ? USB_PID_IN : USB_PID_OUT,
                                 device_addr, endpoint, *toggle, this_len);
        tds[i].buffer = (uint32_t)(uintptr_t)cursor;
        tds[i].ctrl_status = TD_CTRL_ACTIVE | TD_CTRL_CERR3;
        tds[i].link = (i + 1 < n)
            ? ((uint32_t)(uintptr_t)&tds[i + 1] | UHCI_PTR_DEPTH)
            : UHCI_PTR_TERMINATE;
        *toggle = !*toggle;
        cursor += this_len;
        remaining -= this_len;
    }

    g_control_qh.element_link = (uint32_t)(uintptr_t)&tds[0];

    bool ok = false;
    for (uint32_t i = 0; i < 1000; i++) { /* ~1s bound -- storage commands
                                            * can legitimately take a
                                            * while (spin-up, seeks) */
        if (!(tds[n - 1].ctrl_status & TD_CTRL_ACTIVE)) {
            ok = !(tds[n - 1].ctrl_status & TD_CTRL_ERROR_MASK);
            break;
        }
        busy_wait_ms(1);
    }

    g_control_qh.element_link = UHCI_PTR_TERMINATE;

    if (!ok) {
        serial_print("[UHCI] bulk transfer failed/timed out\n");
        for (uint32_t i = 0; i < n; i++) dump_td("BULK  ", &tds[i]);
        return false;
    }

    if (actual_len_out) {
        uint16_t total = 0;
        for (uint32_t i = 0; i < n; i++) {
            total += (uint16_t)((tds[i].ctrl_status & TD_CTRL_ACTLEN_MASK) + 1);
        }
        *actual_len_out = total;
    }
    return true;
}

/*
 * USB Mass Storage: Bulk-Only Transport (BOT), the near-universal
 * transport for USB flash drives. Every command is:
 *   1. bulk-OUT a 31-byte Command Block Wrapper (CBW): fixed
 *      signature, a tag we choose, expected response length/
 *      direction, and a SCSI Command Descriptor Block (CDB) of up to
 *      16 bytes.
 *   2. bulk-IN or bulk-OUT the actual data, if the command has a
 *      data phase (direction/length came from the CBW).
 *   3. bulk-IN a 13-byte Command Status Wrapper (CSW): matching
 *      signature/tag, and a status byte (0 = success).
 *
 * SCSI INQUIRY (CDB opcode 0x12) is the standard first command to
 * send to any SCSI-family device -- it doesn't touch media at all,
 * just asks the device to identify itself (vendor/product strings,
 * device type), making it the natural analogue of HID's
 * GET_DESCRIPTOR for proving the bulk/BOT layer works end-to-end.
 */
#define CBW_SIGNATURE 0x43425355u /* "USBC" little-endian */
#define CSW_SIGNATURE 0x53425355u /* "USBS" little-endian */

typedef struct {
    uint8_t device_addr;
    uint8_t bulk_in, bulk_in_mp;
    bool   *toggle_in;
    uint8_t bulk_out, bulk_out_mp;
    bool   *toggle_out;
} usb_msc_ep_t;

/*
 * Generic Bulk-Only Transport command: CBW out, optional data phase
 * (IN or OUT, direction taken from data_in), CSW in. Handles the
 * bookkeeping every SCSI command needs; callers just supply a CDB
 * and, if there's a data phase, a buffer.
 *
 * Returns true only if the whole exchange succeeded AND the device
 * reported CSW status 0 (command succeeded) -- "the transport worked
 * but the drive rejected the command" is deliberately still false
 * here, since callers only care about "did I get valid data back".
 */
static bool uhci_scsi_command(const usb_msc_ep_t *ep,
                               const uint8_t *cdb, uint8_t cdb_len,
                               uint8_t *data, uint16_t data_len, bool data_in,
                               uint16_t *actual_len_out)
{
    static uint8_t cbw[31] __attribute__((aligned(16)));
    static uint8_t csw[13] __attribute__((aligned(16)));
    memset(cbw, 0, sizeof(cbw));
    memset(csw, 0, sizeof(csw));

    static uint32_t tag_counter = 0x51535430u;
    uint32_t tag = ++tag_counter; /* doesn't need to be meaningful, just
                                    * echoed back in the CSW -- unique
                                    * per command in case that ever
                                    * matters for matching outstanding
                                    * commands later */

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
    cbw[10] = 0; cbw[11] = 0; /* data_len is uint16_t, top bytes always 0 */
    cbw[12] = (data_len > 0 && data_in) ? 0x80 : 0x00;
    cbw[13] = 0; /* LUN 0 */
    cbw[14] = cdb_len;
    memcpy(&cbw[15], cdb, cdb_len);

    uint16_t junk = 0;
    if (!uhci_bulk_transfer(ep->device_addr, ep->bulk_out, ep->toggle_out,
                             cbw, sizeof(cbw), ep->bulk_out_mp, false,
                             &junk)) {
        serial_print("[UHCI] SCSI command: CBW send failed\n");
        return false;
    }

    uint16_t data_actual = 0;
    if (data_len > 0) {
        if (!uhci_bulk_transfer(ep->device_addr, ep->bulk_in, ep->toggle_in,
                                 data, data_len, ep->bulk_in_mp, data_in,
                                 &data_actual)) {
            serial_print("[UHCI] SCSI command: data phase failed\n");
            return false;
        }
    }

    uint16_t csw_actual = 0;
    if (!uhci_bulk_transfer(ep->device_addr, ep->bulk_in, ep->toggle_in,
                             csw, sizeof(csw), ep->bulk_in_mp, true,
                             &csw_actual)) {
        serial_print("[UHCI] SCSI command: CSW read failed\n");
        return false;
    }

    uint32_t csw_sig = (uint32_t)csw[0] | ((uint32_t)csw[1] << 8) |
                        ((uint32_t)csw[2] << 16) | ((uint32_t)csw[3] << 24);
    uint8_t csw_status = csw[12];

    if (csw_sig != CSW_SIGNATURE) {
        serial_print("[UHCI] SCSI command: bad CSW signature=");
        serial_printhex(csw_sig);
        serial_print("\n");
        return false;
    }
    if (csw_status != 0) {
        serial_print("[UHCI] SCSI command: device reported status=");
        serial_printhex(csw_status);
        serial_print(" (0=success, 1=command failed, 2=phase error)\n");
        return false;
    }

    if (actual_len_out) *actual_len_out = data_actual;
    return true;
}

/*
 * SCSI INQUIRY (CDB opcode 0x12) -- the standard first command to
 * send to any SCSI-family device. Doesn't touch media at all, just
 * asks the device to identify itself (vendor/product strings, device
 * type) -- the natural analogue of HID's GET_DESCRIPTOR for proving
 * the bulk/BOT layer works end-to-end.
 */
static bool uhci_scsi_inquiry(const usb_msc_ep_t *ep)
{
    static uint8_t data[36] __attribute__((aligned(16)));
    memset(data, 0, sizeof(data));

    uint8_t cdb[6] = {
        0x12,           /* INQUIRY */
        0x00,           /* EVPD=0 (standard inquiry data) */
        0x00,           /* page code (unused, EVPD=0) */
        0x00,           /* reserved */
        sizeof(data),   /* allocation length = 36 */
        0x00            /* control */
    };

    uint16_t actual = 0;
    if (!uhci_scsi_command(ep, cdb, sizeof(cdb), data, sizeof(data), true,
                            &actual)) {
        serial_print("[UHCI] SCSI INQUIRY failed\n");
        return false;
    }

    serial_print("[UHCI] INQUIRY data (");
    serial_printhex(actual);
    serial_print(" bytes): ");
    hexdump(data, (uint16_t)(actual > sizeof(data) ? sizeof(data) : actual));

    if (actual < 36) return false;

    serial_print("[UHCI]   peripheral device type=");
    serial_printhex((uint64_t)(data[0] & 0x1F));
    serial_print(" vendor=\"");
    for (int i = 8; i < 16; i++) {
        char c = (char)data[i];
        serial_putc(c >= 0x20 && c < 0x7F ? c : '.');
    }
    serial_print("\" product=\"");
    for (int i = 16; i < 32; i++) {
        char c = (char)data[i];
        serial_putc(c >= 0x20 && c < 0x7F ? c : '.');
    }
    serial_print("\"\n");
    return true;
}

/*
 * SCSI READ CAPACITY (10) (CDB opcode 0x25) -- returns the address of
 * the LAST valid logical block (not the total count -- off by one!)
 * and the block size in bytes, both 4-byte big-endian. Standard
 * follow-up to INQUIRY: needed before any READ/WRITE, since those
 * need to know the device's actual block size.
 */
static bool uhci_scsi_read_capacity(const usb_msc_ep_t *ep,
                                     uint32_t *last_lba_out,
                                     uint32_t *block_size_out)
{
    static uint8_t data[8] __attribute__((aligned(16)));
    memset(data, 0, sizeof(data));

    uint8_t cdb[10] = {
        0x25,                   /* READ CAPACITY (10) */
        0x00,                   /* reserved */
        0x00, 0x00, 0x00, 0x00, /* LBA = 0 (ignored unless PMI bit set) */
        0x00, 0x00,             /* reserved */
        0x00,                   /* PMI=0 (bit 0): "give me the real last LBA" */
        0x00                    /* control */
    };

    uint16_t actual = 0;
    if (!uhci_scsi_command(ep, cdb, sizeof(cdb), data, sizeof(data), true,
                            &actual) || actual < 8) {
        serial_print("[UHCI] SCSI READ CAPACITY failed\n");
        return false;
    }

    uint32_t last_lba = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
                         ((uint32_t)data[2] << 8)  |  (uint32_t)data[3];
    uint32_t block_size = ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) |
                           ((uint32_t)data[6] << 8)  |  (uint32_t)data[7];

    serial_print("[UHCI] READ CAPACITY: last_lba=");
    serial_printhex(last_lba);
    serial_print(" block_size=");
    serial_printhex(block_size);
    serial_print(" (total ");
    serial_printhex((uint64_t)(last_lba + 1) * block_size);
    serial_print(" bytes)\n");

    if (last_lba_out) *last_lba_out = last_lba;
    if (block_size_out) *block_size_out = block_size;
    return true;
}

/*
 * SCSI READ (10) (CDB opcode 0x28) -- read transfer_blocks logical
 * blocks starting at lba into buf (must be >= transfer_blocks *
 * block_size bytes). Capped by UHCI_MAX_DATA_TDS (8 TDs) at the
 * endpoint's max packet size -- e.g. one 512-byte sector at a 64-byte
 * bulk max packet is exactly 8 packets, right at the limit. Reading
 * more than that in one call isn't supported yet (would need
 * multiple back-to-back uhci_bulk_transfer calls within one SCSI
 * command's data phase).
 */
static bool uhci_scsi_read10(const usb_msc_ep_t *ep, uint32_t lba,
                              uint16_t transfer_blocks, uint32_t block_size,
                              uint8_t *buf, uint32_t buf_len)
{
    uint32_t want = (uint32_t)transfer_blocks * block_size;
    if (want == 0 || want > buf_len) {
        serial_print("[UHCI] READ(10): bad length request\n");
        return false;
    }
    if (want > (uint32_t)UHCI_MAX_DATA_TDS * ep->bulk_in_mp) {
        serial_print("[UHCI] READ(10): more data than this driver's "
                     "single-command TD chain can carry yet\n");
        return false;
    }

    uint8_t cdb[10] = {
        0x28, 0x00,
        (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
        (uint8_t)(lba >> 8),  (uint8_t)lba,
        0x00,
        (uint8_t)(transfer_blocks >> 8), (uint8_t)transfer_blocks,
        0x00
    };

    uint16_t actual = 0;
    if (!uhci_scsi_command(ep, cdb, sizeof(cdb), buf, (uint16_t)want, true,
                            &actual) || actual < want) {
        serial_print("[UHCI] SCSI READ(10) failed\n");
        return false;
    }

    serial_print("[UHCI] READ(10): lba=");
    serial_printhex(lba);
    serial_print(" blocks=");
    serial_printhex(transfer_blocks);
    serial_print(" -> ");
    serial_printhex(actual);
    serial_print(" bytes ok\n");
    return true;
}

/*
 * Full enumeration sequence for one device found on a root port:
 *   1. GET_DESCRIPTOR(Device, 8 bytes) at address 0 -- learn
 *      bMaxPacketSize0 (needed to safely split a larger transfer
 *      into correctly-sized packets).
 *   2. SET_ADDRESS(1) at address 0 -- the device adopts address 1
 *      once the STATUS stage of *this* request completes. Per spec,
 *      wait >= 2ms afterward (Set Address Recovery Time) before
 *      sending anything else to it.
 *   3. GET_DESCRIPTOR(Device, 18 bytes) at the new address 1, now
 *      split into properly-sized packets -- the full descriptor.
 *
 * Only ever assigns address 1. Fine for one device; a real
 * multi-device enumerator would track and hand out the next free
 * address (2, 3, ...) per device instead -- not needed yet since
 * uhci_reset_port()'s caller only probes one port's device at a
 * time regardless (see the LIMITATION note in uhci_init()).
 */
static void uhci_enumerate_device(void)
{
    static uint8_t desc8[8]  __attribute__((aligned(16)));
    static uint8_t desc18[18] __attribute__((aligned(16)));
    memset(desc8, 0, sizeof(desc8));
    memset(desc18, 0, sizeof(desc18));

    const uint8_t get_desc_setup[8] = {
        0x80, 0x06,       /* bmRequestType, bRequest = GET_DESCRIPTOR */
        0x00, 0x01,       /* wValue = Descriptor Type 1 (DEVICE), Index 0 */
        0x00, 0x00,       /* wIndex = 0 */
        0x00, 0x00        /* wLength -- filled in per-call below */
    };

    /* --- Step 1: partial (8-byte) descriptor at address 0 --- */
    uint8_t setup1[8];
    memcpy(setup1, get_desc_setup, 8);
    setup1[6] = 8; setup1[7] = 0; /* wLength = 8 */

    uint16_t actual = 0;
    if (!uhci_control_transfer(0, setup1, desc8, 8, 8, true, &actual)) {
        serial_print("[UHCI] GET_DESCRIPTOR(Device, 8 bytes) failed\n");
        return;
    }
    serial_print("[UHCI] partial descriptor (");
    serial_printhex(actual);
    serial_print(" bytes): ");
    hexdump(desc8, (uint16_t)(actual > 8 ? 8 : actual));

    uint8_t max_packet = desc8[7];
    serial_print("[UHCI] bMaxPacketSize0=");
    serial_printhex(max_packet);
    serial_print("\n");

    /* --- Step 2: SET_ADDRESS(1), still at address 0 --- */
    const uint8_t set_addr_setup[8] = {
        0x00, 0x05,       /* bmRequestType, bRequest = SET_ADDRESS */
        0x01, 0x00,       /* wValue = new address (1) */
        0x00, 0x00,       /* wIndex = 0 */
        0x00, 0x00        /* wLength = 0 (no data stage) */
    };
    if (!uhci_control_transfer(0, set_addr_setup, NULL, 0, max_packet,
                                true, NULL)) {
        serial_print("[UHCI] SET_ADDRESS(1) failed\n");
        return;
    }
    busy_wait_ms(10); /* spec minimum is 2ms; generous margin */
    serial_print("[UHCI] SET_ADDRESS(1) ok, device now at address 1\n");

    /* --- Step 3: full (18-byte) descriptor at the new address --- */
    uint8_t setup2[8];
    memcpy(setup2, get_desc_setup, 8);
    setup2[6] = 18; setup2[7] = 0; /* wLength = 18 */

    actual = 0;
    if (!uhci_control_transfer(1, setup2, desc18, 18, max_packet,
                                true, &actual)) {
        serial_print("[UHCI] GET_DESCRIPTOR(Device, 18 bytes) failed\n");
        return;
    }

    serial_print("[UHCI] full descriptor (");
    serial_printhex(actual);
    serial_print(" bytes): ");
    hexdump(desc18, (uint16_t)(actual > 18 ? 18 : actual));

    if (actual >= 18) {
        uint16_t vendor  = (uint16_t)(desc18[8]  | (desc18[9]  << 8));
        uint16_t product = (uint16_t)(desc18[10] | (desc18[11] << 8));
        serial_print("[UHCI]   bDeviceClass=");
        serial_printhex(desc18[4]);
        serial_print(" idVendor=");
        serial_printhex(vendor);
        serial_print(" idProduct=");
        serial_printhex(product);
        serial_print("\n");
    }

    /* --- Step 4: configuration descriptor (9-byte header first, to
     * learn wTotalLength, then the full thing) --- */
    static uint8_t cfg9[9] __attribute__((aligned(16)));
    memset(cfg9, 0, sizeof(cfg9));
    const uint8_t get_cfg_setup9[8] = {
        0x80, 0x06,       /* GET_DESCRIPTOR */
        0x00, 0x02,       /* Descriptor Type 2 (CONFIGURATION), Index 0 */
        0x00, 0x00,
        0x09, 0x00        /* wLength = 9 (header only) */
    };
    if (!uhci_control_transfer(1, get_cfg_setup9, cfg9, 9, max_packet,
                                true, &actual) || actual < 9) {
        serial_print("[UHCI] GET_DESCRIPTOR(Config, 9 bytes) failed\n");
        return;
    }
    uint16_t total_len = (uint16_t)(cfg9[2] | (cfg9[3] << 8));
    uint8_t config_value = cfg9[5];
    serial_print("[UHCI] config descriptor wTotalLength=");
    serial_printhex(total_len);
    serial_print(" bConfigurationValue=");
    serial_printhex(config_value);
    serial_print("\n");

    if (total_len < 9 || total_len > 64) {
        serial_print("[UHCI] config descriptor size out of the range this "
                     "driver handles (9-64 bytes), stopping here\n");
        return;
    }

    static uint8_t cfg_full[64] __attribute__((aligned(16)));
    memset(cfg_full, 0, sizeof(cfg_full));
    uint8_t get_cfg_setup_full[8];
    memcpy(get_cfg_setup_full, get_cfg_setup9, 8);
    get_cfg_setup_full[6] = (uint8_t)(total_len & 0xFF);
    get_cfg_setup_full[7] = (uint8_t)(total_len >> 8);

    if (!uhci_control_transfer(1, get_cfg_setup_full, cfg_full, total_len,
                                max_packet, true, &actual) ||
        actual < total_len) {
        serial_print("[UHCI] GET_DESCRIPTOR(Config, full) failed\n");
        return;
    }
    serial_print("[UHCI] full config descriptor: ");
    hexdump(cfg_full, total_len);

    usb_config_info_t cfg_info;
    parse_config_descriptor(cfg_full, total_len, &cfg_info);
    serial_print("[UHCI] interface class=");
    serial_printhex(cfg_info.if_class);
    serial_print(" subclass=");
    serial_printhex(cfg_info.if_subclass);
    serial_print(" protocol=");
    serial_printhex(cfg_info.if_protocol);
    serial_print("\n");

    /* --- Step 5: SET_CONFIGURATION --- */
    uint8_t set_cfg_setup[8] = {
        0x00, 0x09,             /* SET_CONFIGURATION */
        config_value, 0x00,     /* wValue = the config to activate */
        0x00, 0x00, 0x00, 0x00
    };
    if (!uhci_control_transfer(1, set_cfg_setup, NULL, 0, max_packet,
                                true, NULL)) {
        serial_print("[UHCI] SET_CONFIGURATION failed\n");
        return;
    }
    serial_print("[UHCI] SET_CONFIGURATION(");
    serial_printhex(config_value);
    serial_print(") ok, device configured\n");

    /* --- Step 6: class-specific probe, just to prove the relevant
     * transfer type works end-to-end. Neither of these is a real,
     * persistent driver (see the NOTE at the end of each branch). */
    if (cfg_info.if_class == USB_CLASS_HID && cfg_info.have_interrupt_in) {
        serial_print("[UHCI] interrupt-IN endpoint=");
        serial_printhex(cfg_info.interrupt_in_addr);
        serial_print(" wMaxPacketSize=");
        serial_printhex(cfg_info.interrupt_in_maxpacket);
        serial_print("\n");

        static uint8_t report[16] __attribute__((aligned(16)));
        memset(report, 0, sizeof(report));
        bool toggle = false; /* interrupt/bulk endpoints start at DATA0 */
        uint16_t rep_len = 0;
        bool had_error = false;
        uint16_t read_len = cfg_info.interrupt_in_maxpacket < sizeof(report)
                           ? cfg_info.interrupt_in_maxpacket
                           : (uint16_t)sizeof(report);

        if (uhci_interrupt_in(1, cfg_info.interrupt_in_addr, &toggle,
                              report, read_len, &rep_len, &had_error)) {
            serial_print("[UHCI] HID report (");
            serial_printhex(rep_len);
            serial_print(" bytes): ");
            hexdump(report, rep_len);
        } else if (had_error) {
            serial_print("[UHCI] interrupt-IN poll hit a real error\n");
        } else {
            serial_print("[UHCI] interrupt-IN poll: no data within ~50ms "
                         "(NAK-retried by hardware -- not necessarily a "
                         "problem, just nothing new to report yet)\n");
        }

        serial_print("[UHCI] NOTE: one-shot poll, not a persistent "
                     "schedule -- a real HID driver needs this endpoint "
                     "linked into the frame list at its own bInterval, "
                     "polled continuously in the background. Not "
                     "implemented yet.\n");

    } else if (cfg_info.if_class == USB_CLASS_MASS_STORAGE &&
               cfg_info.have_bulk_in && cfg_info.have_bulk_out) {
        serial_print("[UHCI] bulk-IN endpoint=");
        serial_printhex(cfg_info.bulk_in_addr);
        serial_print(" wMaxPacketSize=");
        serial_printhex(cfg_info.bulk_in_maxpacket);
        serial_print(" bulk-OUT endpoint=");
        serial_printhex(cfg_info.bulk_out_addr);
        serial_print(" wMaxPacketSize=");
        serial_printhex(cfg_info.bulk_out_maxpacket);
        serial_print("\n");

        bool toggle_in = false, toggle_out = false; /* bulk endpoints
                                                       * start at DATA0 */
        usb_msc_ep_t ep = {
            .device_addr = 1,
            .bulk_in = cfg_info.bulk_in_addr,
            .bulk_in_mp = cfg_info.bulk_in_maxpacket,
            .toggle_in = &toggle_in,
            .bulk_out = cfg_info.bulk_out_addr,
            .bulk_out_mp = cfg_info.bulk_out_maxpacket,
            .toggle_out = &toggle_out,
        };

        if (!uhci_scsi_inquiry(&ep)) {
            serial_print("[UHCI] stopping (INQUIRY failed)\n");
            return;
        }

        uint32_t last_lba = 0, block_size = 0;
        if (!uhci_scsi_read_capacity(&ep, &last_lba, &block_size)) {
            serial_print("[UHCI] stopping (READ CAPACITY failed)\n");
            return;
        }

        if (block_size == 0 || block_size > 4096) {
            serial_print("[UHCI] block_size out of the range this demo "
                         "handles, stopping here\n");
            return;
        }

        static uint8_t sector[4096] __attribute__((aligned(16)));
        memset(sector, 0, sizeof(sector));
        if (uhci_scsi_read10(&ep, 0, 1, block_size, sector, sizeof(sector))) {
            serial_print("[UHCI] LBA 0 first 32 bytes: ");
            hexdump(sector, 32);
        }

        serial_print("[UHCI] NOTE: INQUIRY + READ CAPACITY + one READ(10) "
                     "proven end-to-end. Still missing: WRITE(10), reading "
                     "more than ~8 packets per command, and VFS/block-"
                     "device integration -- not a usable storage driver, "
                     "just a working transport + command set.\n");

    } else {
        serial_print("[UHCI] no driver for this interface class yet, "
                     "stopping here\n");
    }
}

/*
 * Reset one root port and report whether a device is present
 * afterward. Per the UHCI spec, unlike OHCI, a UHCI controller does
 * NOT necessarily auto-enable a port after reset -- software is
 * expected to check Port Enabled (PED) itself and set it if a device
 * is still connected. That quirk is handled below.
 */
static bool uhci_reset_port(int port)
{
    uint16_t status = portsc_read(port);
    if (!(status & PORTSC_CCS)) {
        return false; /* nothing plugged into this port */
    }

    serial_print("[UHCI] port ");
    serial_printhex((uint64_t)port);
    serial_print(": device present, resetting...\n");

    /*
     * Assert reset. Read-modify-write, but deliberately mask out the
     * write-1-to-clear change bits (CSC, PEDC) from the value we
     * read back before writing it -- otherwise a stale change bit
     * from before we started looking would get spuriously cleared
     * as a side effect of setting Reset, which isn't what "reset the
     * port" should imply.
     */
    uint16_t v = portsc_read(port) & ~(uint16_t)(PORTSC_CSC | PORTSC_PEDC);
    portsc_write(port, v | PORTSC_RESET);

    busy_wait_ms(50); /* USB spec: hold reset for >= 50ms */

    v = portsc_read(port) & ~(uint16_t)(PORTSC_CSC | PORTSC_PEDC);
    portsc_write(port, v & ~(uint16_t)PORTSC_RESET);

    busy_wait_ms(10); /* recovery time before the port is usable */

    status = portsc_read(port);
    if (!(status & PORTSC_CCS)) {
        serial_print("[UHCI] port ");
        serial_printhex((uint64_t)port);
        serial_print(": device disappeared after reset\n");
        return false;
    }

    if (!(status & PORTSC_PED)) {
        /* Not auto-enabled -- enable it explicitly (see comment above). */
        v = portsc_read(port) & ~(uint16_t)(PORTSC_CSC | PORTSC_PEDC);
        portsc_write(port, v | PORTSC_PED);
        busy_wait_ms(10);
    }

    status = portsc_read(port);
    bool low_speed = (status & (1 << 8)) != 0;
    serial_print("[UHCI] port ");
    serial_printhex((uint64_t)port);
    serial_print(": reset complete, enabled=");
    serial_printhex((status & PORTSC_PED) ? 1 : 0);
    serial_print(", low_speed=");
    serial_printhex(low_speed ? 1 : 0);
    serial_print("\n");

    return (status & PORTSC_PED) != 0;
}

bool uhci_init(void)
{
    uint8_t bus, dev;
    if (!pci_find_uhci(&bus, &dev)) {
        serial_print("[UHCI] No UHCI controller found\n");
        return false;
    }

    serial_print("[UHCI] Found controller at PCI ");
    serial_printhex(bus);
    serial_print(":");
    serial_printhex(dev);
    serial_print("\n");

    /*
     * Legacy UHCI exposes its registers as I/O space via BAR4
     * (offset 0x20 in the PCI config header, the 5th BAR). Bit 0 of
     * a BAR is set for I/O-space BARs (vs. bit 0 clear for memory
     * BARs) -- masking it off along with bits 1-2 (reserved) leaves
     * the actual base port address.
     */
    uint32_t bar4 = pci_read(bus, dev, 0, 0x20);
    if (!(bar4 & 0x1)) {
        serial_print("[UHCI] BAR4 is not I/O space, unsupported layout\n");
        return false;
    }
    g_iobase = (uint16_t)(bar4 & ~0x3u);

    serial_print("[UHCI] IO base=");
    serial_printhex(g_iobase);
    serial_print("\n");

    /* Enable I/O space access (bit0) and bus mastering (bit2) in the
     * PCI command register -- the controller can't be talked to via
     * I/O ports, or DMA the frame list, without these. */
    uint32_t pci_cmd = pci_read(bus, dev, 0, 0x04);
    pci_cmd |= (1 << 0) | (1 << 2);
    pci_write(bus, dev, 0, 0x04, pci_cmd);

    /* Make sure we're starting from Stop before resetting. */
    cmd_write(0);
    busy_wait_ms(10);

    /* Global Reset: resets the entire USB bus, including attached
     * devices. Software must hold it for >= 10ms then clear it. */
    cmd_write(USBCMD_GRESET);
    busy_wait_ms(10);
    cmd_write(0);
    busy_wait_ms(10);

    /* Host Controller Reset: resets the controller's internal state
     * (schedule, registers) without resetting the bus. Self-clearing
     * -- poll until the controller clears it (bounded, so a
     * misbehaving/emulated-wrong controller can't hang boot). */
    cmd_write(USBCMD_HCRESET);
    bool hc_reset_ok = false;
    for (int i = 0; i < 1000; i++) {
        if (!(cmd_read() & USBCMD_HCRESET)) { hc_reset_ok = true; break; }
        busy_wait_ms(1);
    }
    if (!hc_reset_ok) {
        serial_print("[UHCI] HCRESET did not clear, aborting init\n");
        return false;
    }

    /* Clear any stale status bits (write-1-to-clear register). */
    sts_write(0xFFFF);

    /* Install the frame list, with every entry pointing at our one
     * persistent control QH (idle/Terminate element for now). A QH
     * with Terminate element is cheap for the HC to skip, so linking
     * it into all 1024 entries up front means transfers can be
     * kicked off later just by changing the QH's element pointer --
     * no need to touch the frame list itself again. */
    g_control_qh.head_link    = UHCI_PTR_TERMINATE;
    g_control_qh.element_link = UHCI_PTR_TERMINATE;
    memset(g_frame_list, 0, sizeof(g_frame_list));
    for (int i = 0; i < 1024; i++) {
        g_frame_list[i] = (uint32_t)(uintptr_t)&g_control_qh | UHCI_PTR_QH;
    }
    outl(g_iobase + UHCI_FRBASEADD, (uint32_t)(uintptr_t)g_frame_list);
    outw(g_iobase + UHCI_FRNUM, 0);
    outb(g_iobase + UHCI_SOFMOD, 0x40); /* default SOF timing value */

    /* No interrupts configured yet -- this driver is polling-only
     * for now (USBINTR left at 0), consistent with everything else
     * in uhci_init() running pre-sti anyway. */
    outw(g_iobase + UHCI_USBINTR, 0);

    /* Start the controller: Run/Stop=1, Configure Flag=1 (software
     * convention signaling "system software is done configuring
     * this controller"), Max Packet=1 (64 bytes, standard for
     * full-speed devices). */
    cmd_write(USBCMD_RS | USBCMD_CF | USBCMD_MAXP);
    busy_wait_ms(10);

    if (sts_read() & USBSTS_HCHALTED) {
        serial_print("[UHCI] controller halted after start, init failed\n");
        return false;
    }

    serial_print("[UHCI] controller running\n");

    /* UHCI defines exactly 2 root ports.
     *
     * LIMITATION: uhci_enumerate_device() always assigns address 1 (the
     * USB default/unaddressed state). That's correct as long as at
     * most one device is unaddressed on the bus at a time -- fine
     * for this single-device test, but real multi-device enumeration
     * needs to reset+probe+SET_ADDRESS one port fully before moving
     * to the next, or two simultaneously-connected devices would
     * both answer address-0 requests at once. Not handled yet. */
    bool any_device = false;
    if (uhci_reset_port(0)) { any_device = true; uhci_enumerate_device(); }
    if (uhci_reset_port(1)) { any_device = true; uhci_enumerate_device(); }

    if (!any_device) {
        serial_print("[UHCI] no devices detected on root ports\n");
    }

    serial_print("[UHCI] foundation + control-transfer probe complete\n");
    return true;
}