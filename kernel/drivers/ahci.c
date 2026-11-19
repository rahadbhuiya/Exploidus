#include "ahci.h"
#include "pci.h"
#include "serial.h"
#include "../mm/pmm.h"
#include <string.h>

#define AHCI_TIMEOUT 1000000

static volatile hba_mem_t  *g_abar         = NULL;
static volatile hba_port_t *g_port         = NULL;
static int                  g_active_port  = -1;
static bool                 g_ahci_ready   = false;

/* DMA memory buffers allocated from PMM */
static hba_cmd_header_t    *g_cmd_list     = NULL;
static uint64_t             g_cmd_list_phys= 0;

static uint8_t             *g_fis_buf      = NULL;
static uint64_t             g_fis_phys     = 0;

static hba_cmd_table_t     *g_cmd_tbl      = NULL;
static uint64_t             g_cmd_tbl_phys = 0;

static uint8_t             *g_dma_buf      = NULL;
static uint64_t             g_dma_phys     = 0;

/*  Forward declarations for blockdev  */
static bool ahci_bd_read(block_device_t *dev, uint32_t lba, uint8_t *buf);
static bool ahci_bd_write(block_device_t *dev, uint32_t lba, const uint8_t *buf);

static block_device_t g_ahci_blockdev = {
    .name         = "ahci0",
    .read_sector  = ahci_bd_read,
    .write_sector = ahci_bd_write,
    .driver_data  = NULL,
};

bool ahci_is_present(void)
{
    return g_ahci_ready && (g_port != NULL);
}

int ahci_get_active_port(void)
{
    return g_active_port;
}

block_device_t *ahci_get_blockdev(void)
{
    return &g_ahci_blockdev;
}

static bool stop_port(volatile hba_port_t *port)
{
    port->cmd &= ~HBA_PxCMD_ST;
    port->cmd &= ~HBA_PxCMD_FRE;

    for (volatile int i = 0; i < AHCI_TIMEOUT; i++) {
        if (!(port->cmd & (HBA_PxCMD_FR | HBA_PxCMD_CR))) {
            return true;
        }
        __asm__ volatile ("pause");
    }
    return false;
}

static void start_port(volatile hba_port_t *port)
{
    for (volatile int i = 0; i < AHCI_TIMEOUT; i++) {
        if (!(port->cmd & HBA_PxCMD_CR)) {
            break;
        }
        __asm__ volatile ("pause");
    }

    port->cmd |= HBA_PxCMD_FRE;
    port->cmd |= HBA_PxCMD_ST;
}

static bool configure_port(volatile hba_port_t *port, int port_no)
{
    if (!stop_port(port)) {
        serial_print("[AHCI] Warning: Failed to stop port ");
        serial_printhex((uint64_t)port_no);
        serial_print("\n");
    }

    /* Allocate Command List (1024 bytes, 1KB aligned) */
    g_cmd_list_phys = pmm_alloc(ZONE_GREEN);
    if (!g_cmd_list_phys) return false;
    g_cmd_list = (hba_cmd_header_t *)(uintptr_t)g_cmd_list_phys;
    memset(g_cmd_list, 0, PAGE_SIZE);

    /* Allocate Received FIS structure (256 bytes, 256-byte aligned) */
    g_fis_phys = pmm_alloc(ZONE_GREEN);
    if (!g_fis_phys) return false;
    g_fis_buf = (uint8_t *)(uintptr_t)g_fis_phys;
    memset(g_fis_buf, 0, PAGE_SIZE);

    /* Allocate Command Table (4096 bytes) */
    g_cmd_tbl_phys = pmm_alloc(ZONE_GREEN);
    if (!g_cmd_tbl_phys) return false;
    g_cmd_tbl = (hba_cmd_table_t *)(uintptr_t)g_cmd_tbl_phys;
    memset(g_cmd_tbl, 0, PAGE_SIZE);

    /* Allocate DMA Bounce Buffer for sector transfers */
    g_dma_phys = pmm_alloc(ZONE_GREEN);
    if (!g_dma_phys) return false;
    g_dma_buf = (uint8_t *)(uintptr_t)g_dma_phys;
    memset(g_dma_buf, 0, PAGE_SIZE);

    /* Configure Command List and Received FIS Base in Port Registers */
    port->clb  = (uint32_t)g_cmd_list_phys;
    port->clbu = (uint32_t)(g_cmd_list_phys >> 32);

    port->fb   = (uint32_t)g_fis_phys;
    port->fbu  = (uint32_t)(g_fis_phys >> 32);

    /* Setup Slot 0 Command Header pointing to Command Table */
    g_cmd_list[0].ctba  = (uint32_t)g_cmd_tbl_phys;
    g_cmd_list[0].ctbau = (uint32_t)(g_cmd_tbl_phys >> 32);
    g_cmd_list[0].prdtl = 1;

    /* Clear pending interrupt status and error registers */
    port->serr = port->serr;
    port->is   = port->is;

    start_port(port);
    return true;
}

void ahci_init(void)
{
    g_ahci_ready  = false;
    g_port        = NULL;
    g_active_port = -1;

    /* Discover AHCI SATA Controller: Class 0x01 (Mass Storage), Subclass 0x06 (SATA) */
    pci_device_t *pdev = pci_find_class(PCI_CLASS_STORAGE, PCI_SUBCLASS_SATA, 0x01);
    if (!pdev) {
        /* Fallback probe for any SATA controller */
        pdev = pci_find_class(PCI_CLASS_STORAGE, PCI_SUBCLASS_SATA, -1);
    }

    if (!pdev) {
        serial_print("[AHCI] No SATA/AHCI controller detected on PCI bus.\n");
        return;
    }

    serial_print("[AHCI] Controller found at PCI ");
    serial_printhex((uint64_t)pdev->bus);
    serial_print(":");
    serial_printhex((uint64_t)pdev->slot);
    serial_print(" Vendor=");
    serial_printhex((uint64_t)pdev->vendor_id);
    serial_print(" Device=");
    serial_printhex((uint64_t)pdev->device_id);
    serial_print("\n");

    /* Enable Bus Mastering and Memory Space Access */
    pci_enable_bus_mastering(pdev);
    pci_enable_memory_space(pdev);

    /* ABAR is located at BAR5 */
    uint64_t abar_phys = pdev->bar_address[5];
    if (!abar_phys) {
        serial_print("[AHCI] Error: ABAR (BAR5) is unassigned.\n");
        return;
    }

    g_abar = (volatile hba_mem_t *)(uintptr_t)abar_phys;

    /* Enable AHCI Mode in Global Host Control */
    g_abar->ghc |= HBA_GHC_AE;

    uint32_t pi = g_abar->pi;
    serial_print("[AHCI] Ports Implemented bitmask: ");
    serial_printhex((uint64_t)pi);
    serial_print("\n");

    /* Probe implemented SATA ports */
    for (int i = 0; i < AHCI_MAX_PORTS; i++) {
        if (!(pi & (1u << i))) {
            continue;
        }

        volatile hba_port_t *port = &g_abar->ports[i];
        uint32_t ssts = port->ssts;
        uint8_t ipm = (uint8_t)((ssts >> 8) & 0x0F);
        uint8_t det = (uint8_t)(ssts & 0x0F);

        /* DET == 3: Device detected & Phy communication established */
        /* IPM == 1: Device in active power state */
        if (det == 3 && ipm == 1) {
            uint32_t sig = port->sig;
            if (sig == SATA_SIG_ATA) {
                serial_print("[AHCI] SATA drive found on port ");
                serial_printhex((uint64_t)i);
                serial_print("\n");

                if (configure_port(port, i)) {
                    g_port = port;
                    g_active_port = i;
                    g_ahci_ready = true;
                    serial_print("[AHCI] Port initialized successfully for DMA.\n");
                    break;
                }
            } else if (sig == SATA_SIG_ATAPI) {
                serial_print("[AHCI] SATAPI drive detected on port ");
                serial_printhex((uint64_t)i);
                serial_print(" (skipping)\n");
            }
        }
    }

    if (!g_ahci_ready) {
        serial_print("[AHCI] No active SATA disk connected.\n");
    }
}

bool ahci_read_sector(uint32_t lba, uint8_t *buf)
{
    if (!g_ahci_ready || !g_port || !buf) return false;

    /* Spin while drive is busy */
    for (volatile int i = 0; i < AHCI_TIMEOUT; i++) {
        if (!(g_port->tfd & (HBA_PxTFD_BSY | HBA_PxTFD_DRQ))) {
            break;
        }
        __asm__ volatile ("pause");
    }

    g_port->is = g_port->is;

    /* Setup Command Header (Slot 0) */
    g_cmd_list[0].cfl   = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
    g_cmd_list[0].w     = 0; /* Read from device */
    g_cmd_list[0].prdtl = 1;
    g_cmd_list[0].prdbc = 0;

    /* Setup Physical Region Descriptor Table Entry 0 */
    g_cmd_tbl->prdt_entry[0].dba       = (uint32_t)g_dma_phys;
    g_cmd_tbl->prdt_entry[0].dbau      = (uint32_t)(g_dma_phys >> 32);
    g_cmd_tbl->prdt_entry[0].dbc       = AHCI_SECTOR_SIZE - 1;
    g_cmd_tbl->prdt_entry[0].reserved0 = 0;

    /* Setup Command FIS (Register FIS - Host to Device) */
    fis_reg_h2d_t *cmdfis = (fis_reg_h2d_t *)(&g_cmd_tbl->cfis[0]);
    memset(cmdfis, 0, sizeof(*cmdfis));
    cmdfis->fis_type = FIS_TYPE_REG_H2D;
    cmdfis->pmport_c = 0x80; /* Command bit set */
    cmdfis->command  = ATA_CMD_READ_DMA_EXT;

    cmdfis->lba0     = (uint8_t)(lba);
    cmdfis->lba1     = (uint8_t)(lba >> 8);
    cmdfis->lba2     = (uint8_t)(lba >> 16);
    cmdfis->device   = 1 << 6; /* LBA mode */

    cmdfis->lba3     = (uint8_t)(lba >> 24);
    cmdfis->lba4     = 0;
    cmdfis->lba5     = 0;

    cmdfis->countl   = 1; /* 1 sector */
    cmdfis->counth   = 0;

    /* Issue command to slot 0 */
    g_port->ci = (1u << 0);

    /* Poll for command completion */
    for (volatile int i = 0; i < AHCI_TIMEOUT; i++) {
        if (!(g_port->ci & (1u << 0))) {
            break;
        }
        if (g_port->is & HBA_PxIS_TFES) {
            serial_print("[AHCI] Read error: Task file error status.\n");
            return false;
        }
        __asm__ volatile ("pause");
    }

    if (g_port->ci & (1u << 0)) {
        serial_print("[AHCI] Read timeout.\n");
        return false;
    }

    if (g_port->tfd & HBA_PxTFD_ERR) {
        serial_print("[AHCI] Read error in TFD.\n");
        return false;
    }

    /* Copy DMA buffer to caller's buffer */
    memcpy(buf, g_dma_buf, AHCI_SECTOR_SIZE);
    return true;
}

bool ahci_write_sector(uint32_t lba, const uint8_t *buf)
{
    if (!g_ahci_ready || !g_port || !buf) return false;

    /* Copy caller's data into DMA buffer */
    memcpy(g_dma_buf, buf, AHCI_SECTOR_SIZE);

    /* Spin while drive is busy */
    for (volatile int i = 0; i < AHCI_TIMEOUT; i++) {
        if (!(g_port->tfd & (HBA_PxTFD_BSY | HBA_PxTFD_DRQ))) {
            break;
        }
        __asm__ volatile ("pause");
    }

    g_port->is = g_port->is;

    /* Setup Command Header (Slot 0) */
    g_cmd_list[0].cfl   = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
    g_cmd_list[0].w     = 1; /* Write to device */
    g_cmd_list[0].prdtl = 1;
    g_cmd_list[0].prdbc = 0;

    /* Setup PRDT Entry 0 */
    g_cmd_tbl->prdt_entry[0].dba       = (uint32_t)g_dma_phys;
    g_cmd_tbl->prdt_entry[0].dbau      = (uint32_t)(g_dma_phys >> 32);
    g_cmd_tbl->prdt_entry[0].dbc       = AHCI_SECTOR_SIZE - 1;
    g_cmd_tbl->prdt_entry[0].reserved0 = 0;

    /* Setup Command FIS */
    fis_reg_h2d_t *cmdfis = (fis_reg_h2d_t *)(&g_cmd_tbl->cfis[0]);
    memset(cmdfis, 0, sizeof(*cmdfis));
    cmdfis->fis_type = FIS_TYPE_REG_H2D;
    cmdfis->pmport_c = 0x80;
    cmdfis->command  = ATA_CMD_WRITE_DMA_EXT;

    cmdfis->lba0     = (uint8_t)(lba);
    cmdfis->lba1     = (uint8_t)(lba >> 8);
    cmdfis->lba2     = (uint8_t)(lba >> 16);
    cmdfis->device   = 1 << 6;

    cmdfis->lba3     = (uint8_t)(lba >> 24);
    cmdfis->lba4     = 0;
    cmdfis->lba5     = 0;

    cmdfis->countl   = 1;
    cmdfis->counth   = 0;

    /* Issue command to slot 0 */
    g_port->ci = (1u << 0);

    /* Poll for completion */
    for (volatile int i = 0; i < AHCI_TIMEOUT; i++) {
        if (!(g_port->ci & (1u << 0))) {
            break;
        }
        if (g_port->is & HBA_PxIS_TFES) {
            serial_print("[AHCI] Write error: Task file error status.\n");
            return false;
        }
        __asm__ volatile ("pause");
    }

    if (g_port->ci & (1u << 0)) {
        serial_print("[AHCI] Write timeout.\n");
        return false;
    }

    if (g_port->tfd & HBA_PxTFD_ERR) {
        serial_print("[AHCI] Write error in TFD.\n");
        return false;
    }

    return true;
}

static bool ahci_bd_read(block_device_t *dev, uint32_t lba, uint8_t *buf)
{
    (void)dev;
    return ahci_read_sector(lba, buf);
}

static bool ahci_bd_write(block_device_t *dev, uint32_t lba, const uint8_t *buf)
{
    (void)dev;
    return ahci_write_sector(lba, buf);
}
