#ifndef EXPLOIDUS_XHCI_H
#define EXPLOIDUS_XHCI_H

#include <stdint.h>
#include <stdbool.h>

/* xHCI Capability Registers (Offsets from MMIO Base) */
#define XHCI_CAP_CAPLENGTH      0x00    /* 8-bit  Capability Register Length */
#define XHCI_CAP_HCIVERSION     0x02    /* 16-bit Interface Version Number */
#define XHCI_CAP_HCSPARAMS1     0x04    /* 32-bit Structural Parameters 1 */
#define XHCI_CAP_HCSPARAMS2     0x08    /* 32-bit Structural Parameters 2 */
#define XHCI_CAP_HCSPARAMS3     0x0C    /* 32-bit Structural Parameters 3 */
#define XHCI_CAP_HCCPARAMS1     0x10    /* 32-bit Capability Parameters 1 */
#define XHCI_CAP_DBOFF          0x14    /* 32-bit Doorbell Offset */
#define XHCI_CAP_RTSOFF         0x18    /* 32-bit Runtime Register Space Offset */
#define XHCI_CAP_HCCPARAMS2     0x1C    /* 32-bit Capability Parameters 2 */

/* xHCI Operational Registers (Offsets from Op Base = MMIO + CAPLENGTH) */
#define XHCI_OP_USBCMD          0x00    /* USB Command */
#define XHCI_OP_USBSTS          0x04    /* USB Status */
#define XHCI_OP_PAGESIZE        0x08    /* Page Size */
#define XHCI_OP_DNCTRL          0x14    /* Device Notification Control */
#define XHCI_OP_CRCR            0x18    /* Command Ring Control (64-bit) */
#define XHCI_OP_DCBAAP          0x30    /* Device Context Base Address Array (64-bit) */
#define XHCI_OP_CONFIG          0x38    /* Configure */
#define XHCI_OP_PORTSC_BASE     0x400   /* Port Status and Control Base */

/* USBCMD Register Bits */
#define XHCI_CMD_RS             (1u << 0)   /* Run/Stop */
#define XHCI_CMD_HCRST          (1u << 1)   /* Host Controller Reset */
#define XHCI_CMD_INTE           (1u << 2)   /* Interrupter Enable */
#define XHCI_CMD_HSEE           (1u << 3)   /* Host System Error Enable */
#define XHCI_CMD_LHCRST         (1u << 7)   /* Light Host Controller Reset */

/* USBSTS Register Bits */
#define XHCI_STS_HCH            (1u << 0)   /* Host Controller Halted */
#define XHCI_STS_HSE            (1u << 2)   /* Host System Error */
#define XHCI_STS_EINT           (1u << 3)   /* Event Interrupt */
#define XHCI_STS_PCD            (1u << 4)   /* Port Change Detect */
#define XHCI_STS_SSS            (1u << 8)   /* Save/Restore State Status */
#define XHCI_STS_RSS            (1u << 9)   /* Restore State Status */
#define XHCI_STS_SRE            (1u << 10)  /* Save/Restore Error */
#define XHCI_STS_CNR            (1u << 11)  /* Controller Not Ready */
#define XHCI_STS_HCE            (1u << 12)  /* Host Controller Error */

/* PORTSC Register Bits */
#define XHCI_PORTSC_CCS         (1u << 0)   /* Current Connect Status */
#define XHCI_PORTSC_PED         (1u << 1)   /* Port Enabled/Disabled */
#define XHCI_PORTSC_OCA         (1u << 3)   /* Over-current Active */
#define XHCI_PORTSC_PR          (1u << 4)   /* Port Reset */
#define XHCI_PORTSC_PLS_MASK    (0xFu << 5) /* Port Link State */
#define XHCI_PORTSC_PP          (1u << 9)   /* Port Power */
#define XHCI_PORTSC_SPEED_SHIFT 10
#define XHCI_PORTSC_SPEED_MASK  (0xFu << 10)/* Port Speed */
#define XHCI_PORTSC_LWS         (1u << 16)  /* Port Link State Write Strobe */
#define XHCI_PORTSC_CSC         (1u << 17)  /* Connect Status Change */
#define XHCI_PORTSC_PEC         (1u << 18)  /* Port Enable/Disable Change */
#define XHCI_PORTSC_WRC         (1u << 19)  /* Warm Port Reset Change */
#define XHCI_PORTSC_OCC         (1u << 20)  /* Over-current Change */
#define XHCI_PORTSC_PRC         (1u << 21)  /* Port Reset Change */
#define XHCI_PORTSC_PLC         (1u << 22)  /* Port Link State Change */
#define XHCI_PORTSC_CEC         (1u << 23)  /* Port Config Error Change */
#define XHCI_PORTSC_CAS         (1u << 24)  /* Cold Attach Status */
#define XHCI_PORTSC_WCE         (1u << 25)  /* Wake on Connect Enable */
#define XHCI_PORTSC_WDE         (1u << 26)  /* Wake on Disconnect Enable */
#define XHCI_PORTSC_WOE         (1u << 27)  /* Wake on Over-current Enable */
#define XHCI_PORTSC_DR          (1u << 30)  /* Device Removable */
#define XHCI_PORTSC_WPR         (1u << 31)  /* Warm Port Reset */

/* Transfer Request Block (TRB) - 16 bytes */
typedef struct {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
} __attribute__((packed)) xhci_trb_t;

/* TRB Types */
#define XHCI_TRB_NORMAL         1
#define XHCI_TRB_SETUP_STAGE    2
#define XHCI_TRB_DATA_STAGE     3
#define XHCI_TRB_STATUS_STAGE   4
#define XHCI_TRB_LINK           6
#define XHCI_TRB_ENABLE_SLOT    9
#define XHCI_TRB_DISABLE_SLOT   10
#define XHCI_TRB_ADDRESS_DEV    11
#define XHCI_TRB_CONFIG_EP      12
#define XHCI_TRB_EVAL_CTX       13
#define XHCI_TRB_RESET_EP       14
#define XHCI_TRB_NOOP           23

/* Device Context Base Address Array entry */
#define XHCI_MAX_SLOTS_SUPPORTED 64
#define XHCI_CMD_RING_TRB_COUNT  256

/* Public Driver Interface */
void     xhci_init(void);
bool     xhci_is_present(void);
uint32_t xhci_port_count(void);
uint32_t xhci_connected_devices(void);

#endif /* EXPLOIDUS_XHCI_H */
