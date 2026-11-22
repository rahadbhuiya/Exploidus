#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "blockdev.h"

#define NVME_SECTOR_SIZE        512
#define NVME_QUEUE_ENTRIES      64
#define NVME_TIMEOUT_CYCLES     2000000

/* NVMe Register Offsets (Base MMIO via BAR0) */
#define NVME_REG_CAP            0x0000  /* Controller Capabilities (64-bit) */
#define NVME_REG_VS             0x0008  /* Version (32-bit) */
#define NVME_REG_INTMS          0x000C  /* Interrupt Mask Set (32-bit) */
#define NVME_REG_INTMC          0x0010  /* Interrupt Mask Clear (32-bit) */
#define NVME_REG_CC             0x0014  /* Controller Configuration (32-bit) */
#define NVME_REG_CSTS           0x001C  /* Controller Status (32-bit) */
#define NVME_REG_NSSR           0x0020  /* NVM Subsystem Reset (32-bit) */
#define NVME_REG_AQA            0x0024  /* Admin Queue Attributes (32-bit) */
#define NVME_REG_ASQ            0x0028  /* Admin Submission Queue Base (64-bit) */
#define NVME_REG_ACQ            0x0030  /* Admin Completion Queue Base (64-bit) */

/* Controller Configuration (CC) bits */
#define NVME_CC_EN              (1u << 0)   /* Enable */
#define NVME_CC_CSS_NVM         (0u << 4)   /* NVM Command Set */
#define NVME_CC_MPS_4K          (0u << 7)   /* 4KB Memory Page Size (2 ^ (12 + 0)) */
#define NVME_CC_AMS_RR          (0u << 11)  /* Round Robin Arbitration */
#define NVME_CC_SHN_NONE        (0u << 14)  /* No Shutdown Notification */
#define NVME_CC_IOSQES_64B      (6u << 16)  /* 64 bytes SQ entry size (2 ^ 6) */
#define NVME_CC_IOCQES_16B      (4u << 20)  /* 16 bytes CQ entry size (2 ^ 4) */

/* Controller Status (CSTS) bits */
#define NVME_CSTS_RDY           (1u << 0)   /* Ready */
#define NVME_CSTS_CFS           (1u << 1)   /* Controller Fatal Status */
#define NVME_CSTS_SHST_MASK     (3u << 2)   /* Shutdown Status Mask */

/* Admin Opcodes */
#define NVME_ADMIN_OP_DELETE_IO_SQ  0x00
#define NVME_ADMIN_OP_CREATE_IO_SQ  0x01
#define NVME_ADMIN_OP_DELETE_IO_CQ  0x02
#define NVME_ADMIN_OP_CREATE_IO_CQ  0x05
#define NVME_ADMIN_OP_IDENTIFY      0x06
#define NVME_ADMIN_OP_ABORT         0x08
#define NVME_ADMIN_OP_SET_FEATURES  0x09
#define NVME_ADMIN_OP_GET_FEATURES  0x0A

/* NVM Command Set Opcodes (I/O) */
#define NVME_NVM_OP_FLUSH           0x00
#define NVME_NVM_OP_WRITE           0x01
#define NVME_NVM_OP_READ            0x02

/* NVMe Submission Queue Entry (64 bytes) */
typedef struct {
    uint8_t  opcode;
    uint8_t  flags;
    uint16_t cid;
    uint32_t nsid;
    uint64_t reserved0;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} __attribute__((packed)) nvme_sqe_t;

/* NVMe Completion Queue Entry (16 bytes) */
typedef struct {
    uint32_t command_specific;
    uint32_t reserved;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status; /* bit 0: phase bit, bits 15:1: status code and type */
} __attribute__((packed)) nvme_cqe_t;

/* Identify Namespace (relevant fields from 4096-byte struct) */
typedef struct {
    uint64_t nsze;          /* Namespace Size (total logical blocks) */
    uint64_t ncap;          /* Namespace Capacity */
    uint64_t nuse;          /* Namespace Utilization */
    uint8_t  nsfeat;        /* Namespace Features */
    uint8_t  nlbaf;         /* Number of LBA Formats */
    uint8_t  flbas;         /* Formatted LBA Size */
    uint8_t  mc;            /* Metadata Capabilities */
    uint8_t  dpc;           /* End-to-end Data Protection Capabilities */
    uint8_t  dps;           /* End-to-end Data Protection Settings */
    uint8_t  nmic;          /* Namespace Multi-path I/O and Namespace Sharing Capabilities */
    uint8_t  rescap;        /* Reservation Capabilities */
    uint8_t  fpi;           /* Format Progress Indicator */
    uint8_t  dlfeat;        /* Deallocate Logical Block Features */
    uint16_t nawun;         /* Namespace Atomic Write Unit Normal */
    uint16_t nawupf;        /* Namespace Atomic Write Unit Power Fail */
    uint16_t nacwu;         /* Namespace Atomic Compare & Write Unit */
    uint16_t nabsn;         /* Namespace Atomic Boundary Size Normal */
    uint16_t nabo;          /* Namespace Atomic Boundary Offset */
    uint16_t nabspf;        /* Namespace Atomic Boundary Size Power Fail */
    uint16_t noiob;         /* Namespace Optimal I/O Boundary */
    uint8_t  nvmcap[16];    /* NVM Capacity */
    uint8_t  reserved[40];
    uint8_t  nguid[16];     /* Globally Unique Identifier */
    uint8_t  eui64[8];      /* IEEE Extended Unique Identifier */
    struct {
        uint16_t ms;        /* Metadata Size */
        uint8_t  lbads;     /* LBA Data Size (2^n) */
        uint8_t  rp:2;      /* Relative Performance */
        uint8_t  reserved:6;
    } lbaf[16];
    uint8_t  vendor_specific[3712];
} __attribute__((packed)) nvme_id_ns_t;

/* Public Driver Interface */
void            nvme_init(void);
bool            nvme_is_present(void);
block_device_t *nvme_get_blockdev(void);
bool            nvme_read_sector(uint32_t lba, uint8_t *buf);
bool            nvme_write_sector(uint32_t lba, const uint8_t *buf);
uint64_t        nvme_get_sector_count(void);
uint32_t        nvme_get_sector_size(void);
