#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "blockdev.h"

#define AHCI_SECTOR_SIZE        512
#define AHCI_MAX_PORTS          32

/* SATA Signatures */
#define SATA_SIG_ATA            0x00000101  /* SATA drive */
#define SATA_SIG_ATAPI          0xEB140101  /* SATAPI optical drive */
#define SATA_SIG_SEMB           0xC33C0101  /* Enclosure management bridge */
#define SATA_SIG_PM             0x96690101  /* Port multiplier */

/* FIS Types */
#define FIS_TYPE_REG_H2D        0x27        /* Register FIS - host to device */
#define FIS_TYPE_REG_D2H        0x34        /* Register FIS - device to host */
#define FIS_TYPE_DMA_ACT        0x39        /* DMA activate FIS - device to host */
#define FIS_TYPE_DMA_SETUP      0x41        /* DMA setup FIS - bidirectional */
#define FIS_TYPE_DATA           0x46        /* Data FIS - bidirectional */
#define FIS_TYPE_BIST           0x58        /* BIST activate FIS - bidirectional */
#define FIS_TYPE_PIO_SETUP      0x5F        /* PIO setup FIS - device to host */
#define FIS_TYPE_DEV_BITS       0xA1        /* Set device bits FIS - device to host */

/* ATA Commands */
#define ATA_CMD_READ_DMA_EXT    0x25
#define ATA_CMD_WRITE_DMA_EXT   0x35
#define ATA_CMD_IDENTIFY        0xEC

/* Global Host Control bits */
#define HBA_GHC_HR              (1u << 0)   /* HBA Reset */
#define HBA_GHC_IE              (1u << 1)   /* Interrupt Enable */
#define HBA_GHC_AE              (1u << 31)  /* AHCI Enable */

/* Port Command & Status bits */
#define HBA_PxCMD_ST            (1u << 0)   /* Start processing command list */
#define HBA_PxCMD_SUD           (1u << 1)   /* Spin-Up Device */
#define HBA_PxCMD_POD           (1u << 2)   /* Power On Device */
#define HBA_PxCMD_CLO           (1u << 3)   /* Command List Override */
#define HBA_PxCMD_FRE           (1u << 4)   /* FIS Receive Enable */
#define HBA_PxCMD_FR            (1u << 14)  /* FIS Receive Running */
#define HBA_PxCMD_CR            (1u << 15)  /* Command List Running */

/* Port Task File Data bits */
#define HBA_PxTFD_ERR           (1u << 0)   /* Error */
#define HBA_PxTFD_DRQ           (1u << 3)   /* Data Request */
#define HBA_PxTFD_BSY           (1u << 7)   /* Busy */

/* Port Interrupt Status bits */
#define HBA_PxIS_DHRS           (1u << 0)   /* Device to Host Register FIS Interrupt */
#define HBA_PxIS_TFES           (1u << 30)  /* Task File Error Status */

/* Host to Device Register FIS */
typedef struct {
    uint8_t  fis_type;      /* FIS_TYPE_REG_H2D */
    uint8_t  pmport_c;      /* Port multiplier:4, Reserved:3, Command/Control:1 */
    uint8_t  command;       /* ATA Command */
    uint8_t  featurel;      /* Feature Register 7:0 */

    uint8_t  lba0;          /* LBA 7:0 */
    uint8_t  lba1;          /* LBA 15:8 */
    uint8_t  lba2;          /* LBA 23:16 */
    uint8_t  device;        /* Device Register (1<<6 = LBA) */

    uint8_t  lba3;          /* LBA 31:24 */
    uint8_t  lba4;          /* LBA 39:32 */
    uint8_t  lba5;          /* LBA 47:40 */
    uint8_t  featureh;      /* Feature Register 15:8 */

    uint8_t  countl;        /* Sector Count 7:0 */
    uint8_t  counth;        /* Sector Count 15:8 */
    uint8_t  icc;           /* Isochronous Command Completion */
    uint8_t  control;       /* Control Register */

    uint8_t  reserved[4];
} __attribute__((packed)) fis_reg_h2d_t;

/* Physical Region Descriptor Table Entry */
typedef struct {
    uint32_t dba;           /* Data Base Address (low 32 bits) */
    uint32_t dbau;          /* Data Base Address (high 32 bits) */
    uint32_t reserved0;
    uint32_t dbc;           /* Byte Count:22 (count - 1), bit 31 = Interrupt On Completion (IOC) */
} __attribute__((packed)) hba_prdt_entry_t;

/* Command Table */
typedef struct {
    uint8_t          cfis[64];       /* Command FIS */
    uint8_t          acmd[16];       /* ATAPI Command */
    uint8_t          reserved[48];
    hba_prdt_entry_t prdt_entry[1];  /* Physical Region Descriptor Table */
} __attribute__((packed)) hba_cmd_table_t;

/* Command Header */
typedef struct {
    uint8_t  cfl:5;         /* Command FIS length in DWORDS (5 for 20 bytes) */
    uint8_t  a:1;           /* ATAPI */
    uint8_t  w:1;           /* Write (1 = Write to device, 0 = Read from device) */
    uint8_t  p:1;           /* Prefetchable */
    uint8_t  r:1;           /* Reset */
    uint8_t  b:1;           /* BIST */
    uint8_t  c:1;           /* Clear Busy upon R_OK */
    uint8_t  reserved0:1;
    uint8_t  pmp:4;         /* Port Multiplier Port */

    uint16_t prdtl;         /* PRDT length */
    volatile uint32_t prdbc;/* PRD Byte Count transferred */
    uint32_t ctba;          /* Command Table Descriptor Base Address (low 32) */
    uint32_t ctbau;         /* Command Table Descriptor Base Address (high 32) */
    uint32_t reserved1[4];
} __attribute__((packed)) hba_cmd_header_t;

/* Port Registers */
typedef struct {
    volatile uint32_t clb;        /* 0x00: Command List Base Address (low 32) */
    volatile uint32_t clbu;       /* 0x04: Command List Base Address (high 32) */
    volatile uint32_t fb;         /* 0x08: FIS Base Address (low 32) */
    volatile uint32_t fbu;        /* 0x0C: FIS Base Address (high 32) */
    volatile uint32_t is;         /* 0x10: Interrupt Status */
    volatile uint32_t ie;         /* 0x14: Interrupt Enable */
    volatile uint32_t cmd;        /* 0x18: Command and Status */
    volatile uint32_t reserved0;  /* 0x1C */
    volatile uint32_t tfd;        /* 0x20: Task File Data */
    volatile uint32_t sig;        /* 0x24: Signature */
    volatile uint32_t ssts;       /* 0x28: SATA Status (SStatus) */
    volatile uint32_t sctl;       /* 0x2C: SATA Control (SControl) */
    volatile uint32_t serr;       /* 0x30: SATA Error (SError) */
    volatile uint32_t sact;       /* 0x34: SATA Active (SActive) */
    volatile uint32_t ci;         /* 0x38: Command Issue */
    volatile uint32_t sntf;       /* 0x3C: SATA Notification */
    volatile uint32_t fbs;        /* 0x40: FIS-based Switching Control */
    volatile uint32_t reserved1[11];
    volatile uint32_t vendor[4];
} __attribute__((packed)) hba_port_t;

/* Generic Host Control (HBA Memory Registers) */
typedef struct {
    volatile uint32_t cap;        /* 0x00: Host Capabilities */
    volatile uint32_t ghc;        /* 0x04: Global Host Control */
    volatile uint32_t is;         /* 0x08: Interrupt Status */
    volatile uint32_t pi;         /* 0x0C: Ports Implemented */
    volatile uint32_t vs;         /* 0x10: Version */
    volatile uint32_t ccc_ctl;    /* 0x14: CCC Control */
    volatile uint32_t ccc_pts;    /* 0x18: CCC Ports */
    volatile uint32_t em_loc;     /* 0x1C: Enclosure Management Location */
    volatile uint32_t em_ctl;     /* 0x20: Enclosure Management Control */
    volatile uint32_t cap2;       /* 0x24: Host Capabilities Extended */
    volatile uint32_t bohc;       /* 0x28: BIOS/OS Handoff Control and Status */
    volatile uint8_t  reserved[0x74];
    volatile uint8_t  vendor[0x60];
    hba_port_t        ports[AHCI_MAX_PORTS]; /* 0x100 - 0x10FF */
} __attribute__((packed)) hba_mem_t;

/* Public AHCI Driver API */
void            ahci_init(void);
bool            ahci_is_present(void);
int             ahci_get_active_port(void);
bool            ahci_read_sector(uint32_t lba, uint8_t *buf);
bool            ahci_write_sector(uint32_t lba, const uint8_t *buf);
block_device_t *ahci_get_blockdev(void);
