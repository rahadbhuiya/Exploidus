#include "nvme.h"
#include "pci.h"
#include "serial.h"
#include "../mm/pmm.h"
#include <string.h>

static volatile uint8_t *g_mmio         = NULL;
static uint64_t          g_cap          = 0;
static uint32_t          g_dstrd        = 4;
static bool              g_nvme_ready   = false;
static uint64_t          g_sector_count = 0;
static uint32_t          g_sector_size  = NVME_SECTOR_SIZE;

/* Admin Queues (Allocated via PMM) */
static nvme_sqe_t       *g_admin_sq     = NULL;
static uint64_t          g_admin_sq_phys= 0;
static uint16_t          g_admin_sq_tail= 0;

static nvme_cqe_t       *g_admin_cq     = NULL;
static uint64_t          g_admin_cq_phys= 0;
static uint16_t          g_admin_cq_head= 0;
static uint8_t           g_admin_cq_phase = 1;

/* I/O Queues (Queue ID 1) */
static nvme_sqe_t       *g_io_sq        = NULL;
static uint64_t          g_io_sq_phys   = 0;
static uint16_t          g_io_sq_tail   = 0;

static nvme_cqe_t       *g_io_cq        = NULL;
static uint64_t          g_io_cq_phys   = 0;
static uint16_t          g_io_cq_head   = 0;
static uint8_t           g_io_cq_phase  = 1;

/* DMA bounce buffer for 4KB transfers */
static uint8_t          *g_dma_buf      = NULL;
static uint64_t          g_dma_phys     = 0;

/* Command ID counter */
static uint16_t          g_cmd_id       = 0;

/* Forward declarations for block device interface */
static bool nvme_bd_read(block_device_t *dev, uint32_t lba, uint8_t *buf);
static bool nvme_bd_write(block_device_t *dev, uint32_t lba, const uint8_t *buf);

static block_device_t g_nvme_blockdev = {
    .name         = "nvme0",
    .read_sector  = nvme_bd_read,
    .write_sector = nvme_bd_write,
    .driver_data  = NULL,
};

bool nvme_is_present(void)
{
    return g_nvme_ready;
}

block_device_t *nvme_get_blockdev(void)
{
    return &g_nvme_blockdev;
}

uint64_t nvme_get_sector_count(void)
{
    return g_sector_count;
}

uint32_t nvme_get_sector_size(void)
{
    return g_sector_size;
}

/* MMIO Register Access Helpers */
static inline uint32_t nvme_read32(uint32_t offset)
{
    return *(volatile uint32_t *)(g_mmio + offset);
}

static inline void nvme_write32(uint32_t offset, uint32_t val)
{
    *(volatile uint32_t *)(g_mmio + offset) = val;
}

static inline uint64_t nvme_read64(uint32_t offset)
{
    uint32_t low = nvme_read32(offset);
    uint32_t high = nvme_read32(offset + 4);
    return ((uint64_t)high << 32) | low;
}

static inline void nvme_write64(uint32_t offset, uint64_t val)
{
    nvme_write32(offset, (uint32_t)val);
    nvme_write32(offset + 4, (uint32_t)(val >> 32));
}

/* Doorbell Helpers */
static inline volatile uint32_t *nvme_sq_doorbell(uint16_t qid)
{
    return (volatile uint32_t *)(g_mmio + 0x1000 + (2 * qid * g_dstrd));
}

static inline volatile uint32_t *nvme_cq_doorbell(uint16_t qid)
{
    return (volatile uint32_t *)(g_mmio + 0x1000 + ((2 * qid + 1) * g_dstrd));
}

/* Admin Command Execution (Synchronous Polling) */
static bool nvme_submit_admin_cmd(nvme_sqe_t *cmd, uint16_t *out_status)
{
    uint16_t cid = ++g_cmd_id;
    cmd->cid = cid;

    /* Copy command into submission queue tail */
    memcpy(&g_admin_sq[g_admin_sq_tail], cmd, sizeof(nvme_sqe_t));

    /* Advance SQ tail and ring doorbell */
    g_admin_sq_tail = (g_admin_sq_tail + 1) % NVME_QUEUE_ENTRIES;
    *nvme_sq_doorbell(0) = g_admin_sq_tail;

    /* Poll Admin Completion Queue */
    for (volatile int i = 0; i < NVME_TIMEOUT_CYCLES; i++) {
        volatile nvme_cqe_t *cqe = &g_admin_cq[g_admin_cq_head];
        uint16_t status = cqe->status;
        uint8_t phase = (uint8_t)(status & 0x01);

        if (phase == g_admin_cq_phase) {
            /* Matched current completion */
            if (out_status) {
                *out_status = (status >> 1);
            }

            /* Advance CQ head */
            g_admin_cq_head = (g_admin_cq_head + 1) % NVME_QUEUE_ENTRIES;
            if (g_admin_cq_head == 0) {
                g_admin_cq_phase ^= 1;
            }

            /* Ring CQ doorbell */
            *nvme_cq_doorbell(0) = g_admin_cq_head;

            /* Check status code (0 = success) */
            return ((status >> 1) == 0);
        }
        __asm__ volatile ("pause");
    }

    serial_print("[NVME] Timeout waiting on Admin Command.\n");
    return false;
}

/* I/O Command Execution (Synchronous Polling) */
static bool nvme_submit_io_cmd(nvme_sqe_t *cmd, uint16_t *out_status)
{
    uint16_t cid = ++g_cmd_id;
    cmd->cid = cid;

    /* Copy command into I/O submission queue tail */
    memcpy(&g_io_sq[g_io_sq_tail], cmd, sizeof(nvme_sqe_t));

    /* Advance SQ tail and ring doorbell */
    g_io_sq_tail = (g_io_sq_tail + 1) % NVME_QUEUE_ENTRIES;
    *nvme_sq_doorbell(1) = g_io_sq_tail;

    /* Poll I/O Completion Queue */
    for (volatile int i = 0; i < NVME_TIMEOUT_CYCLES; i++) {
        volatile nvme_cqe_t *cqe = &g_io_cq[g_io_cq_head];
        uint16_t status = cqe->status;
        uint8_t phase = (uint8_t)(status & 0x01);

        if (phase == g_io_cq_phase) {
            if (out_status) {
                *out_status = (status >> 1);
            }

            g_io_cq_head = (g_io_cq_head + 1) % NVME_QUEUE_ENTRIES;
            if (g_io_cq_head == 0) {
                g_io_cq_phase ^= 1;
            }

            *nvme_cq_doorbell(1) = g_io_cq_head;
            return ((status >> 1) == 0);
        }
        __asm__ volatile ("pause");
    }

    serial_print("[NVME] Timeout waiting on I/O Command.\n");
    return false;
}

void nvme_init(void)
{
    g_nvme_ready = false;
    g_mmio       = NULL;

    /* Discover NVMe Storage Controller: Class 0x01, Subclass 0x08, Prog-IF 0x02 */
    pci_device_t *pdev = pci_find_class(PCI_CLASS_STORAGE, PCI_SUBCLASS_NVME, 0x02);
    if (!pdev) {
        /* Fallback probe for any NVMe subclass device */
        pdev = pci_find_class(PCI_CLASS_STORAGE, PCI_SUBCLASS_NVME, -1);
    }

    if (!pdev) {
        serial_print("[NVME] No NVMe storage controller detected on PCI bus.\n");
        return;
    }

    serial_print("[NVME] Controller detected at PCI ");
    serial_printhex((uint64_t)pdev->bus);
    serial_print(":");
    serial_printhex((uint64_t)pdev->slot);
    serial_print(" Vendor=0x");
    serial_printhex((uint64_t)pdev->vendor_id);
    serial_print(" Device=0x");
    serial_printhex((uint64_t)pdev->device_id);
    serial_print("\n");

    /* Enable Bus Mastering and Memory Space Access */
    pci_enable_bus_mastering(pdev);
    pci_enable_memory_space(pdev);

    /* Base MMIO registers are at BAR0 (64-bit) */
    uint64_t mmio_phys = pdev->bar_address[0];
    if (!mmio_phys) {
        serial_print("[NVME] Error: BAR0 is unassigned.\n");
        return;
    }

    g_mmio = (volatile uint8_t *)(uintptr_t)mmio_phys;

    /* Read Controller Capabilities */
    g_cap = nvme_read64(NVME_REG_CAP);
    uint32_t vs = nvme_read32(NVME_REG_VS);
    g_dstrd = 4 << ((g_cap >> 32) & 0x0F);

    serial_print("[NVME] Version: ");
    serial_printhex((uint64_t)(vs >> 16));
    serial_print(".");
    serial_printhex((uint64_t)((vs >> 8) & 0xFF));
    serial_print(" Doorbell Stride: ");
    serial_printhex((uint64_t)g_dstrd);
    serial_print(" bytes\n");

    /* Reset / Disable controller if currently enabled */
    uint32_t cc = nvme_read32(NVME_REG_CC);
    if (cc & NVME_CC_EN) {
        nvme_write32(NVME_REG_CC, cc & ~NVME_CC_EN);
    }

    /* Wait for CSTS.RDY == 0 */
    for (volatile int i = 0; i < NVME_TIMEOUT_CYCLES; i++) {
        if (!(nvme_read32(NVME_REG_CSTS) & NVME_CSTS_RDY)) {
            break;
        }
        __asm__ volatile ("pause");
    }

    /* Allocate Admin Queues (4KB page each) */
    g_admin_sq_phys = pmm_alloc(ZONE_GREEN);
    g_admin_cq_phys = pmm_alloc(ZONE_GREEN);
    if (!g_admin_sq_phys || !g_admin_cq_phys) {
        serial_print("[NVME] Error: Failed to allocate physical memory for Admin Queues.\n");
        return;
    }

    g_admin_sq = (nvme_sqe_t *)(uintptr_t)g_admin_sq_phys;
    g_admin_cq = (nvme_cqe_t *)(uintptr_t)g_admin_cq_phys;
    memset(g_admin_sq, 0, PAGE_SIZE);
    memset(g_admin_cq, 0, PAGE_SIZE);

    g_admin_sq_tail  = 0;
    g_admin_cq_head  = 0;
    g_admin_cq_phase = 1;

    /* Configure Admin Queue Attributes (Size = 64, 0-based is 63) */
    uint32_t aqa = ((NVME_QUEUE_ENTRIES - 1) << 16) | (NVME_QUEUE_ENTRIES - 1);
    nvme_write32(NVME_REG_AQA, aqa);

    /* Write Admin Queue Base Addresses */
    nvme_write64(NVME_REG_ASQ, g_admin_sq_phys);
    nvme_write64(NVME_REG_ACQ, g_admin_cq_phys);

    /* Configure & Enable Controller */
    uint32_t new_cc = NVME_CC_EN | NVME_CC_CSS_NVM | NVME_CC_MPS_4K |
                      NVME_CC_AMS_RR | NVME_CC_IOSQES_64B | NVME_CC_IOCQES_16B;
    nvme_write32(NVME_REG_CC, new_cc);

    /* Wait for CSTS.RDY == 1 */
    bool ready = false;
    for (volatile int i = 0; i < NVME_TIMEOUT_CYCLES; i++) {
        if (nvme_read32(NVME_REG_CSTS) & NVME_CSTS_RDY) {
            ready = true;
            break;
        }
        __asm__ volatile ("pause");
    }

    if (!ready) {
        serial_print("[NVME] Error: Controller failed to become ready.\n");
        return;
    }

    /* Allocate I/O Queues */
    g_io_sq_phys = pmm_alloc(ZONE_GREEN);
    g_io_cq_phys = pmm_alloc(ZONE_GREEN);
    g_dma_phys   = pmm_alloc(ZONE_GREEN);

    if (!g_io_sq_phys || !g_io_cq_phys || !g_dma_phys) {
        serial_print("[NVME] Error: Failed to allocate physical memory for I/O Queues.\n");
        return;
    }

    g_io_sq = (nvme_sqe_t *)(uintptr_t)g_io_sq_phys;
    g_io_cq = (nvme_cqe_t *)(uintptr_t)g_io_cq_phys;
    g_dma_buf = (uint8_t *)(uintptr_t)g_dma_phys;

    memset(g_io_sq, 0, PAGE_SIZE);
    memset(g_io_cq, 0, PAGE_SIZE);
    memset(g_dma_buf, 0, PAGE_SIZE);

    g_io_sq_tail  = 0;
    g_io_cq_head  = 0;
    g_io_cq_phase = 1;

    /* Create I/O CQ 1 via Admin Command (Opcode 0x05) */
    nvme_sqe_t create_cq_cmd;
    memset(&create_cq_cmd, 0, sizeof(create_cq_cmd));
    create_cq_cmd.opcode = NVME_ADMIN_OP_CREATE_IO_CQ;
    create_cq_cmd.prp1   = g_io_cq_phys;
    create_cq_cmd.cdw10  = ((NVME_QUEUE_ENTRIES - 1) << 16) | 1; /* Size 64, QID 1 */
    create_cq_cmd.cdw11  = 0x01; /* Physically Contiguous, Interrupts Disabled */

    if (!nvme_submit_admin_cmd(&create_cq_cmd, NULL)) {
        serial_print("[NVME] Error: Failed to create I/O Completion Queue.\n");
        return;
    }

    /* Create I/O SQ 1 via Admin Command (Opcode 0x01) */
    nvme_sqe_t create_sq_cmd;
    memset(&create_sq_cmd, 0, sizeof(create_sq_cmd));
    create_sq_cmd.opcode = NVME_ADMIN_OP_CREATE_IO_SQ;
    create_sq_cmd.prp1   = g_io_sq_phys;
    create_sq_cmd.cdw10  = ((NVME_QUEUE_ENTRIES - 1) << 16) | 1; /* Size 64, QID 1 */
    create_sq_cmd.cdw11  = (1u << 16) | 0x01; /* CQID 1, Physically Contiguous */

    if (!nvme_submit_admin_cmd(&create_sq_cmd, NULL)) {
        serial_print("[NVME] Error: Failed to create I/O Submission Queue.\n");
        return;
    }

    /* Identify Namespace 1 (Opcode 0x06, CNS = 0, NSID = 1) */
    nvme_sqe_t id_cmd;
    memset(&id_cmd, 0, sizeof(id_cmd));
    id_cmd.opcode = NVME_ADMIN_OP_IDENTIFY;
    id_cmd.nsid   = 1;
    id_cmd.prp1   = g_dma_phys;
    id_cmd.cdw10  = 0; /* CNS = 0 (Identify Namespace) */

    if (nvme_submit_admin_cmd(&id_cmd, NULL)) {
        nvme_id_ns_t *id_ns = (nvme_id_ns_t *)g_dma_buf;
        g_sector_count = id_ns->nsze;
        uint8_t flbas = id_ns->flbas & 0x0F;
        uint8_t lbads = id_ns->lbaf[flbas].lbads;
        if (lbads >= 9 && lbads <= 12) {
            g_sector_size = (1u << lbads);
        } else {
            g_sector_size = NVME_SECTOR_SIZE;
        }

        serial_print("[NVME] Namespace 1 Online: ");
        serial_printhex(g_sector_count);
        serial_print(" sectors (");
        serial_printhex((g_sector_count * g_sector_size) / (1024 * 1024));
        serial_print(" MB, Sector size: ");
        serial_printhex((uint64_t)g_sector_size);
        serial_print(" bytes)\n");
    } else {
        serial_print("[NVME] Warning: Identify Namespace command failed, using defaults.\n");
        g_sector_count = 2097152; /* Default 1GB */
        g_sector_size  = NVME_SECTOR_SIZE;
    }

    g_nvme_ready = true;
    serial_print("[NVME] High-performance NVMe PCIe storage driver initialized.\n");
}

/* Sector Read Operation (512-byte LBA) */
bool nvme_read_sector(uint32_t lba, uint8_t *buf)
{
    if (!g_nvme_ready || !buf) return false;

    nvme_sqe_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_NVM_OP_READ;
    cmd.nsid   = 1;
    cmd.prp1   = g_dma_phys;
    cmd.cdw10  = lba;
    cmd.cdw11  = 0;
    cmd.cdw12  = 0; /* 0-based: 0 = 1 sector */

    if (!nvme_submit_io_cmd(&cmd, NULL)) {
        serial_print("[NVME] Error reading sector at LBA ");
        serial_printhex((uint64_t)lba);
        serial_print("\n");
        return false;
    }

    memcpy(buf, g_dma_buf, NVME_SECTOR_SIZE);
    return true;
}

/* Sector Write Operation (512-byte LBA) */
bool nvme_write_sector(uint32_t lba, const uint8_t *buf)
{
    if (!g_nvme_ready || !buf) return false;

    memcpy(g_dma_buf, buf, NVME_SECTOR_SIZE);

    nvme_sqe_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_NVM_OP_WRITE;
    cmd.nsid   = 1;
    cmd.prp1   = g_dma_phys;
    cmd.cdw10  = lba;
    cmd.cdw11  = 0;
    cmd.cdw12  = 0; /* 0-based: 0 = 1 sector */

    if (!nvme_submit_io_cmd(&cmd, NULL)) {
        serial_print("[NVME] Error writing sector at LBA ");
        serial_printhex((uint64_t)lba);
        serial_print("\n");
        return false;
    }

    return true;
}

static bool nvme_bd_read(block_device_t *dev, uint32_t lba, uint8_t *buf)
{
    (void)dev;
    return nvme_read_sector(lba, buf);
}

static bool nvme_bd_write(block_device_t *dev, uint32_t lba, const uint8_t *buf)
{
    (void)dev;
    return nvme_write_sector(lba, buf);
}
