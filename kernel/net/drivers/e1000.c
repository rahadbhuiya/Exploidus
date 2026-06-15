#include "e1000.h"
#include "../net.h"
#include "../../drivers/serial.h"
#include "../../mm/kmalloc.h"
#include <string.h>


/*  PCI helpers */


#define PCI_CFG_ADDR  0xCF8
#define PCI_CFG_DATA  0xCFC

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
static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port) : "memory");
}
static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port) : "memory");
    return v;
}

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


/*  e1000 MMIO register offsets    */


#define E1000_CTRL    0x0000
#define E1000_STATUS  0x0008
#define E1000_EECD    0x0010
#define E1000_EERD    0x0014
#define E1000_ICR     0x00C0
#define E1000_IMS     0x00D0
#define E1000_IMC     0x00D8
#define E1000_RCTL    0x0100
#define E1000_TCTL    0x0400
#define E1000_TIPG    0x0410
#define E1000_RDBAL   0x2800
#define E1000_RDBAH   0x2804
#define E1000_RDLEN   0x2808
#define E1000_RDH     0x2810
#define E1000_RDT     0x2818
#define E1000_TDBAL   0x3800
#define E1000_TDBAH   0x3804
#define E1000_TDLEN   0x3808
#define E1000_TDH     0x3810
#define E1000_TDT     0x3818
#define E1000_RAL     0x5400
#define E1000_RAH     0x5404

/* CTRL bits */
#define CTRL_RST      (1u << 26)
#define CTRL_ASDE     (1u <<  5)
#define CTRL_SLU      (1u <<  6)

/* RCTL bits */
#define RCTL_EN       (1u <<  1)
#define RCTL_SBP      (1u <<  2)
#define RCTL_UPE      (1u <<  3)
#define RCTL_MPE      (1u <<  4)
#define RCTL_BAM      (1u << 15)
#define RCTL_BSIZE_2K (0u << 16)
#define RCTL_SECRC    (1u << 26)

/* TCTL bits */
#define TCTL_EN       (1u <<  1)
#define TCTL_PSP      (1u <<  3)
#define TCTL_CT_SHIFT 4
#define TCTL_COLD_SHIFT 12

/* TX descriptor command bits */
#define TDESC_CMD_EOP   0x01
#define TDESC_CMD_IFCS  0x02
#define TDESC_CMD_RS    0x08

/* TX descriptor status */
#define TDESC_STAT_DD   0x01

/* RX descriptor status */
#define RDESC_STAT_DD   0x01
#define RDESC_STAT_EOP  0x02


/*  Descriptor ring sizes — must be multiples of 8                      */


#define TX_RING_SIZE  16
#define RX_RING_SIZE  16

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} e1000_tx_desc_t;

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} e1000_rx_desc_t;


/*  Driver state  */


static volatile uint32_t *g_mmio    = NULL;
static netif_t            g_iface;

static e1000_tx_desc_t g_tx_ring[TX_RING_SIZE] __attribute__((aligned(16)));
static e1000_rx_desc_t g_rx_ring[RX_RING_SIZE] __attribute__((aligned(16)));
static uint8_t         g_rx_bufs[RX_RING_SIZE][2048];
static uint32_t        g_tx_tail = 0;
static uint32_t        g_rx_tail = 0;

static inline uint32_t e1000_read(uint32_t reg)
{
    return g_mmio[reg / 4];
}

static inline void e1000_write(uint32_t reg, uint32_t val)
{
    g_mmio[reg / 4] = val;
    if (reg == 0x0018) { serial_print("[E1000] TDT updated\n"); }
}

static void e1000_read_mac(void)
{
    /*
     * Read MAC from Receive Address registers.
     * RAL contains bytes 0-3, RAH contains bytes 4-5.
     */
    uint32_t ral = e1000_read(E1000_RAL);
    uint32_t rah = e1000_read(E1000_RAH);

    g_iface.mac.b[0] = (uint8_t)(ral & 0xFF);
    g_iface.mac.b[1] = (uint8_t)((ral >>  8) & 0xFF);
    g_iface.mac.b[2] = (uint8_t)((ral >> 16) & 0xFF);
    g_iface.mac.b[3] = (uint8_t)((ral >> 24) & 0xFF);
    g_iface.mac.b[4] = (uint8_t)(rah & 0xFF);
    g_iface.mac.b[5] = (uint8_t)((rah >>  8) & 0xFF);
}


/*  PCI device scan — find e1000 (vendor 0x8086, device 0x100E)         */


static bool pci_find_e1000(uint8_t *bus_out, uint8_t *dev_out)
{
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            uint32_t id = pci_read((uint8_t)bus, dev, 0, 0);
            uint16_t vendor = (uint16_t)(id & 0xFFFF);
            uint16_t device = (uint16_t)(id >> 16);
            if (vendor == 0x8086 && device == 0x100E) {
                *bus_out = (uint8_t)bus;
                *dev_out = dev;
                return true;
            }
        }
    }
    return false;
}


/*  Transmit */


/* Per-descriptor TX bounce buffers — NIC DMA's from these, not from netbuf */
static uint8_t g_tx_bufs[TX_RING_SIZE][2048];

bool e1000_transmit(netif_t *iface, netbuf_t *buf)
{
    serial_print("[E1000] transmit called\n");
    (void)iface;
    if (!g_mmio || !buf || !buf->len) return false;

    uint32_t tail = g_tx_tail;
    e1000_tx_desc_t *desc = &g_tx_ring[tail];

    /* Wait for descriptor to be free */
    uint32_t spins = 0;
    while (!(desc->status & TDESC_STAT_DD) && desc->cmd && spins < 100000) {
        spins++;
        __asm__ volatile ("pause");
    }

    /* Copy payload into per-slot bounce buffer so caller can free buf safely */
    uint16_t len = buf->len;
    if (len > sizeof(g_tx_bufs[tail])) len = sizeof(g_tx_bufs[tail]);
    memcpy(g_tx_bufs[tail], buf->data, len);

    desc->addr   = (uint64_t)(uintptr_t)g_tx_bufs[tail];
    desc->length = len;
    desc->cmd    = TDESC_CMD_EOP | TDESC_CMD_IFCS | TDESC_CMD_RS;
    desc->status = 0;

    g_tx_tail = (tail + 1) % TX_RING_SIZE;
    e1000_write(E1000_TDT, g_tx_tail);

    return true;
}


/*  Receive poll — called from net_poll() in the main loop   */


extern void eth_input(netif_t *iface, netbuf_t *buf);

void e1000_poll(void)
{
    if (!g_mmio) return;

    uint32_t head = e1000_read(E1000_RDH);
    /* RX debug disabled */
    uint32_t tail = g_rx_tail;

    while (tail != head) {
        e1000_rx_desc_t *desc = &g_rx_ring[tail];

        /* Only process descriptors marked done by hardware (DD bit) */
        if (!(desc->status & RDESC_STAT_DD)) break;

        if (desc->length > 0 && (desc->status & RDESC_STAT_EOP)) {
            netbuf_t *buf = netbuf_alloc();
            if (buf) {
                uint16_t len = desc->length;
                if (len > NETBUF_CAP - NETBUF_HEADROOM)
                    len = NETBUF_CAP - NETBUF_HEADROOM;
                memcpy(buf->data, g_rx_bufs[tail], len);
                buf->len = len;
                eth_input(&g_iface, buf);
            }
        }

        /* Give descriptor back to hardware */
        desc->status = 0;
        desc->addr   = (uint64_t)(uintptr_t)g_rx_bufs[tail];

        tail = (tail + 1) % RX_RING_SIZE;
    }

    g_rx_tail = tail;
    e1000_write(E1000_RDT, (tail == 0) ? RX_RING_SIZE - 1 : tail - 1);
}


/*  Initialization  */


bool e1000_init(void)
{
    uint8_t bus, dev;
    if (!pci_find_e1000(&bus, &dev)) {
        serial_print("[e1000] Not found on PCI bus\n");
        return false;
    }
    serial_print("[e1000] Found at PCI ");
    serial_printhex((uint64_t)bus);
    serial_print(":");
    serial_printhex((uint64_t)dev);
    serial_print("\n");

    /* Enable bus mastering */
    uint32_t cmd = pci_read(bus, dev, 0, 0x04);
    cmd |= 0x4;
    pci_write(bus, dev, 0, 0x04, cmd);

    /* Read BAR0 — MMIO base address */
    uint32_t bar0 = pci_read(bus, dev, 0, 0x10) & ~0xFu;
    g_mmio = (volatile uint32_t *)(uintptr_t)bar0;

    serial_print("[e1000] MMIO base=");
    serial_printhex((uint64_t)bar0);
    serial_print("\n");

    /* Software reset */
    e1000_write(E1000_CTRL, e1000_read(E1000_CTRL) | CTRL_RST);
    for (volatile int i = 0; i < 100000; i++) __asm__ volatile ("pause");

    /* Auto-speed detection + set link up */
    e1000_write(E1000_CTRL,
                e1000_read(E1000_CTRL) | CTRL_ASDE | CTRL_SLU);

    /* Disable interrupts */
    e1000_write(E1000_IMC, 0xFFFFFFFF);

    /* Read MAC address */
    e1000_read_mac();
    serial_print("[e1000] MAC=");
    for (int i = 0; i < 6; i++) {
        serial_printhex((uint64_t)g_iface.mac.b[i]);
        if (i < 5) serial_print(":");
    }
    serial_print("\n");

    /* Set up TX ring */
    memset(g_tx_ring, 0, sizeof(g_tx_ring));
    /* Mark all descriptors as done so the first TX doesn't stall */
    for (int i = 0; i < TX_RING_SIZE; i++)
        g_tx_ring[i].status = TDESC_STAT_DD;

    e1000_write(E1000_TDBAL, (uint32_t)(uintptr_t)g_tx_ring);
    e1000_write(E1000_TDBAH, (uint32_t)((uintptr_t)g_tx_ring >> 32));
    e1000_write(E1000_TDLEN, TX_RING_SIZE * sizeof(e1000_tx_desc_t));
    e1000_write(E1000_TDH,  0);
    e1000_write(E1000_TDT,  0);
    e1000_write(E1000_TCTL,
                TCTL_EN | TCTL_PSP |
                (0x0F << TCTL_CT_SHIFT) |
                (0x40 << TCTL_COLD_SHIFT));
    e1000_write(E1000_TIPG, 0x0060200A);

    /* Set up RX ring */
    memset(g_rx_ring, 0, sizeof(g_rx_ring));
    for (int i = 0; i < RX_RING_SIZE; i++) {
        g_rx_ring[i].addr   = (uint64_t)(uintptr_t)g_rx_bufs[i];
        g_rx_ring[i].status = 0;
    }

    e1000_write(E1000_RDBAL, (uint32_t)(uintptr_t)g_rx_ring);
    e1000_write(E1000_RDBAH, (uint32_t)((uintptr_t)g_rx_ring >> 32));
    e1000_write(E1000_RDLEN, RX_RING_SIZE * sizeof(e1000_rx_desc_t));
    
    e1000_write(E1000_RDT,  RX_RING_SIZE - 1);
    e1000_write(E1000_RCTL,
                RCTL_EN | RCTL_BAM | RCTL_UPE | RCTL_MPE | RCTL_BSIZE_2K | RCTL_SECRC);

    /* Register as a network interface */
    memcpy(g_iface.name, "eth0", 5);
    g_iface.ip   = IP4(10, 0, 2, 15);   /* QEMU default */
    g_iface.mask = IP4(255, 255, 255, 0);
    g_iface.gw   = IP4(10, 0, 2, 2);
    g_iface.up   = true;
    g_iface.tx   = e1000_transmit;

    netif_register(&g_iface);

    serial_print("[e1000] Ready. IP=10.0.2.15\n");
    return true;
}