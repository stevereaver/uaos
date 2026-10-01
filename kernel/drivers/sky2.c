/*
 * sky2.c — UAOS Marvell Yukon-2 (Sky2) Gigabit Ethernet Driver
 *
 * Hardware: Marvell 88E8058 "Yukon-2 EC Ultra" (PCI 11ab:436a)
 *   - Ethernet controller in the Apple MacBookPro4,1 (Early 2008)
 *   - BAR0 = 16 KB MMIO register space (64-bit BAR)
 *   - Single MAC port (port 0); second port registers unused
 *
 * Register model adapted from Linux drivers/net/ethernet/marvell/sky2.c.
 * Register window layout:
 *   SK_REG(port, reg)      = (port << 7) + reg        — per-port control regs
 *   SK_GMAC_REG(port, reg) = 0x2800 + port*0x1000 + reg — GMAC (GM_*) regs
 *   Q_ADDR(q, offs)        = 0x0400 + q + offs        — BMU queue regs
 *   Y2_QADDR(q, reg)       = 0x0450 + q + reg         — prefetch unit regs
 *   RB_ADDR(q, offs)       = 0x0800 + q + offs        — RAM buffer regs
 *   RAM_BUFFER(port, reg)  = reg | (port << 6)        — RAM interface regs
 *   PCI config via MMIO    = 0x1c00 + cfg_offset      — Y2_CFG_SPC window
 *
 * Design:
 *   - Synchronous TX: submit a PACKET list element, kick PUT_IDX, then
 *     drain the status ring until the TX index reaches our entry.
 *   - RX: buffers posted via ADDR64+PACKET list elements; completions
 *     reported as OP_RXSTAT entries in the shared status ring.
 *   - IRQ: B0_Y2_SP_ISRC2 masks interrupts when read; re-armed by
 *     reading B0_Y2_SP_LISR after the status ring is drained.
 */

#include "sky2.h"
#include "sky2_golden.h"   /* Linux golden register dump (real-HW bring-up) */
#include "../irq/idt.h"    /* IDT_SetHandler */
#include "../irq/irq.h"    /* IRQ_AttachPCI, IRQ_EOI */
#include "../exec/task.h"  /* Task_ScheduleFromIRQ */
#include "../klog/klog.h"
#include "../dos/dma.h"
#include <string.h>

/* -------------------------------------------------------------------------
 * PCI config-space access (I/O port CF8/CFC) — used for probing only.
 * Device config registers are later accessed through the MMIO window.
 * ------------------------------------------------------------------------- */
static inline void _s_outl(uint16_t p, uint32_t v)
{
    __asm__ volatile("outl %0,%1" :: "a"(v), "Nd"(p));
}
static inline uint32_t _s_inl(uint16_t p)
{
    uint32_t v;
    __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}

static uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg)
{
    uint32_t addr = 0x80000000U | ((uint32_t)bus << 16) | ((uint32_t)dev << 11)
                  | ((uint32_t)fn << 8) | (reg & 0xFC);
    _s_outl(0xCF8, addr);
    return _s_inl(0xCFC);
}

static uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg)
{
    return (uint16_t)(pci_read32(bus, dev, fn, reg) >> ((reg & 2) * 8));
}

static void pci_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint16_t val)
{
    uint32_t addr = 0x80000000U | ((uint32_t)bus << 16) | ((uint32_t)dev << 11)
                  | ((uint32_t)fn << 8) | (reg & 0xFC);
    _s_outl(0xCF8, addr);
    uint32_t cur = _s_inl(0xCFC);
    int shift = (reg & 2) * 8;
    cur = (cur & ~(0xFFFFU << shift)) | ((uint32_t)val << shift);
    _s_outl(0xCFC, cur);
}

/* -------------------------------------------------------------------------
 * MMIO helpers
 * ------------------------------------------------------------------------- */
static volatile uint8_t *g_regs = 0;

static inline uint8_t  r8 (uint32_t r) { return *(volatile uint8_t  *)(g_regs + r); }
static inline uint16_t r16(uint32_t r) { return *(volatile uint16_t *)(g_regs + r); }
static inline uint32_t r32(uint32_t r) { return *(volatile uint32_t *)(g_regs + r); }
static inline void w8 (uint32_t r, uint8_t  v) { *(volatile uint8_t  *)(g_regs + r) = v; }
static inline void w16(uint32_t r, uint16_t v) { *(volatile uint16_t *)(g_regs + r) = v; }
static inline void w32(uint32_t r, uint32_t v) { *(volatile uint32_t *)(g_regs + r) = v; }

/* PCI config space window inside BAR0 */
#define Y2_CFG_SPC  0x1c00
#define Y2_CFG_AER  0x1d00
static inline uint16_t pcr16(uint32_t r) { return r16(Y2_CFG_SPC + r); }
static inline uint32_t pcr32(uint32_t r) { return r32(Y2_CFG_SPC + r); }
static inline void pcw16(uint32_t r, uint16_t v) { w16(Y2_CFG_SPC + r, v); }
static inline void pcw32(uint32_t r, uint32_t v) { w32(Y2_CFG_SPC + r, v); }

/* Per-port register selectors */
#define SK_REG(port, reg)      (((port) << 7) + (reg))
#define SK_GMAC_REG(port, reg) (0x2800 + (port) * 0x1000 + (reg))
#define Q_ADDR(q, offs)        (0x0400 + (q) + (offs))
#define Y2_QADDR(q, reg)       (0x0450 + (q) + (reg))
#define RB_ADDR(q, offs)       (0x0800 + (q) + (offs))
#define RAM_BUFFER(port, reg)  ((reg) | ((port) << 6))

static inline uint16_t gma_r16(int port, uint32_t reg)
{
    return r16(SK_GMAC_REG(port, reg));
}
static inline void gma_w16(int port, uint32_t reg, uint16_t v)
{
    w16(SK_GMAC_REG(port, reg), v);
}

/* -------------------------------------------------------------------------
 * Register definitions (subset of Linux sky2.h needed for Yukon-2 EC_U)
 * ------------------------------------------------------------------------- */

/* B0 control/status */
#define B0_CTST          0x0004   /* 24 bit Control/Status */
#define B0_POWER_CTRL    0x0007   /*  8 bit Power Control */
#define B0_IMSK          0x000c   /* 32 bit Interrupt Mask */
#define B0_HWE_ISRC      0x0010   /* 32 bit HW Error IRQ Source */
#define B0_HWE_IMSK      0x0014   /* 32 bit HW Error Interrupt Mask */
#define B0_Y2_SP_ISRC2   0x001c   /* 32 bit Special IRQ Source (read masks) */
#define B0_Y2_SP_EISR    0x0024   /* 32 bit Enter ISR */
#define B0_Y2_SP_LISR    0x0028   /* 32 bit Leave ISR */
#define B0_Y2_SP_ICR     0x002c   /* 32 bit IRQ Control */

/* B0_CTST bits */
#define CS_RST_SET       0x01
#define CS_RST_CLR       0x02
#define CS_MRST_CLR      0x08
#define Y2_LED_STAT_ON   (1u << 9)
#define Y2_ASF_DISABLE   (1u << 12)
#define Y2_HW_WOL_ON     (1u << 15)

/* B0_POWER_CTRL bits */
#define PC_VAUX_ENA      (1u << 7)
#define PC_VCC_ENA       (1u << 5)
#define PC_VAUX_OFF      (1u << 2)
#define PC_VCC_ON        (1u << 1)

/* B2 registers */
#define B2_MAC_1         0x0100   /* MAC address, 8 bytes per port */
#define B2_PMD_TYP       0x0119
#define B2_MAC_CFG       0x011a
#define B2_CHIP_ID       0x011b
#define B2_E_0           0x011c   /* RAM buffer size (4K blocks) */
#define B2_Y2_CLK_GATE   0x011d
#define B2_Y2_HW_RES     0x011e
#define B2_Y2_CLK_CTRL   0x0120
#define B2_TI_CTRL       0x0138
#define B2_TST_CTRL1     0x0158
#define B2_GP_IO         0x015c
#define B2_I2C_IRQ       0x0168

#define CFG_CHIP_R_MSK   (0xf << 4)
#define Y2_CLK_DIV_DIS   0x1
#define TST_CFG_WRITE_ON  0x2
#define TST_CFG_WRITE_OFF 0x1
#define GLB_GPIO_STAT_RACE_DIS (1u << 13)
#define TIM_STOP         0x2
#define TIM_CLR_IRQ      0x1
#define TIM_START        0x4

/* B3 RAM interface (use RAM_BUFFER() macro) */
#define B3_RI_WTO_R1     0x0190
#define B3_RI_WTO_XA1    0x0191
#define B3_RI_WTO_XS1    0x0192
#define B3_RI_RTO_R1     0x0193
#define B3_RI_RTO_XA1    0x0194
#define B3_RI_RTO_XS1    0x0195
#define B3_RI_CTRL       0x01a0

#define RI_RST_CLR       0x2
#define SK_RI_TO_53      36

/* Transmit arbiter (use SK_REG) */
#define TXA_CTRL         0x0210
#define TXA_ENA_ARB      0x2

/* Queue selectors */
#define Q_R1             0x0000
#define Q_XS1            0x0200
#define Q_XA1            0x0280

/* BMU queue register offsets (use Q_ADDR) */
#define Q_CSR            0x34
#define Q_TEST           0x38
#define Q_WM             0x40
#define Q_RL             0x4a    /*  8 bit  FIFO Read Level */
#define Q_WL             0x4e    /*  8 bit  FIFO Write Level */

#define BMU_FIFO_OP_ON   (1u << 7)
#define BMU_FIFO_ENA     (1u << 5)
#define BMU_FIFO_RST     (1u << 4)
#define BMU_OP_ON        (1u << 3)
#define BMU_OP_OFF       (1u << 2)
#define BMU_RST_CLR      (1u << 1)
#define BMU_START        (1u << 8)
#define BMU_CLR_IRQ_CHK  (1u << 10)
#define BMU_CLR_IRQ_PAR  (1u << 11)

#define BMU_CLR_RESET    (BMU_FIFO_RST | BMU_OP_OFF | BMU_RST_CLR)
#define BMU_OPER_INIT    (BMU_CLR_IRQ_PAR | BMU_CLR_IRQ_CHK | BMU_START | \
                          BMU_FIFO_ENA | BMU_OP_ON)
#define BMU_WM_DEFAULT   0x600
#define BMU_WM_PEX       0x80

/* Prefetch unit offsets (use Y2_QADDR) */
#define PREF_UNIT_CTRL      0x00
#define PREF_UNIT_LAST_IDX  0x04
#define PREF_UNIT_ADDR_LO   0x08
#define PREF_UNIT_ADDR_HI   0x0c
#define PREF_UNIT_GET_IDX   0x10
#define PREF_UNIT_PUT_IDX   0x14
#define PREF_UNIT_FIFO_LEV  0x2c

#define PREF_UNIT_RST_SET   0x1
#define PREF_UNIT_RST_CLR   0x2
#define PREF_UNIT_OP_ON     0x8

/* RAM buffer offsets (use RB_ADDR) */
#define RB_START     0x00
#define RB_END       0x04
#define RB_WP        0x08
#define RB_RP        0x0c
#define RB_RX_UTPP   0x10
#define RB_RX_LTPP   0x14
#define RB_RX_UTHP   0x18
#define RB_RX_LTHP   0x1c
#define RB_CTRL      0x28

#define RB_RST_SET     0x1
#define RB_RST_CLR     0x2
#define RB_DIS_OP_MD   0x4
#define RB_ENA_OP_MD   0x8
#define RB_ENA_STFWD   0x20

/* Per-port GMAC FIFO / control (use SK_REG) */
#define LNK_LED_REG      0x0c3c
#define RX_GMF_CTRL_T    0x0c48
#define RX_GMF_FL_MSK    0x0c4c
#define RX_GMF_FL_THR    0x0c50
#define RX_GMF_TR_THR    0x0c54
#define RX_GMF_UP_THR    0x0c58
#define RX_GMF_LP_THR    0x0c5a
#define TX_GMF_CTRL_T    0x0d48
#define TX_GMF_AE_THR    0x0d44
#define GMAC_TI_ST_CTRL  0x0e18
#define GMAC_CTRL        0x0f00
#define GPHY_CTRL        0x0f04
#define GMAC_IRQ_SRC     0x0f08
#define GMAC_IRQ_MSK     0x0f0c
#define GMAC_LINK_CTRL   0x0f10

#define GMLC_RST_SET     0x1
#define GMLC_RST_CLR     0x2
#define GPC_RST_SET      0x1
#define GPC_RST_CLR      0x2
#define GMC_RST_SET      0x1
#define GMC_RST_CLR      0x2
#define GMC_PAUSE_ON     0x8
#define GMC_PAUSE_OFF    0x4
#define GMF_RST_CLR      0x2
#define GMF_OPER_ON      0x8
#define GMF_RX_F_FL_ON   0x80
#define TX_STFW_ENA      (1u << 30)  /* Tx GMAC FIFO store & forward */
#define GMT_ST_STOP      0x2
#define GMT_ST_CLR_IRQ   0x1
#define RX_TRUNC_ON      (1u << 27)
#define RX_VLAN_STRIP_OFF (1u << 24)
#define TX_VLAN_TAG_OFF  (1u << 24)
#define LINKLED_ON       0x1
#define LINKLED_OFF      0x0
#define LINKLED_BLINK_OFF 0x0
#define LINKLED_LINKSYNC_OFF 0x0
#define RX_GMF_FL_THR_DEF 0xa

/* GMAC IRQ bits */
#define GM_IS_TX_FF_UR   (1u << 3)
#define GM_IS_RX_FF_OR   (1u << 1)
#define GMAC_DEF_MSK     (GM_IS_TX_FF_UR | GM_IS_RX_FF_OR)

/* Misc */
#define B28_DPT_CTRL     0x0e08
#define DPT_STOP         0x1
#define B28_Y2_ASF_STAT_CMD 0x0e68
#define Y2_ASF_RESET     0x8

/* Status-BMU ring */
#define STAT_CTRL          0x0e80
#define STAT_LAST_IDX      0x0e84
#define STAT_LIST_ADDR_LO  0x0e88
#define STAT_LIST_ADDR_HI  0x0e8c
#define STAT_TX_IDX_TH     0x0e98
#define STAT_PUT_IDX       0x0e9c
#define STAT_FIFO_LEVEL    0x0ea8
#define STAT_FIFO_WM       0x0eac
#define STAT_FIFO_ISR_WM   0x0ead
#define STAT_LEV_TIMER_INI 0x0eb0
#define STAT_LEV_TIMER_CTRL 0x0eb8
#define STAT_TX_TIMER_INI  0x0ec0
#define STAT_TX_TIMER_CTRL 0x0ec8
#define STAT_ISR_TIMER_INI 0x0ed0
#define STAT_ISR_TIMER_CTRL 0x0ed8

#define SC_STAT_RST_SET  0x1
#define SC_STAT_RST_CLR  0x2
#define SC_STAT_OP_ON    0x8
#define SC_STAT_CLR_IRQ  0x10

/* Interrupt source bits (B0_IMSK / B0_Y2_SP_*) */
#define Y2_IS_HW_ERR     (1u << 31)
#define Y2_IS_STAT_BMU   (1u << 30)
#define Y2_IS_IRQ_PHY2   (1u << 12)
#define Y2_IS_IRQ_MAC2   (1u << 11)
#define Y2_IS_CHK_RX2    (1u << 10)
#define Y2_IS_CHK_TXA2   (1u << 8)
#define Y2_IS_IRQ_PHY1   (1u << 4)
#define Y2_IS_IRQ_MAC1   (1u << 3)
#define Y2_IS_CHK_RX1    (1u << 2)
#define Y2_IS_CHK_TXA1   (1u << 0)

#define Y2_IS_PORT_1  (Y2_IS_IRQ_PHY1 | Y2_IS_IRQ_MAC1 | \
                       Y2_IS_CHK_TXA1 | Y2_IS_CHK_RX1)
#define Y2_IS_BASE    (Y2_IS_HW_ERR | Y2_IS_STAT_BMU)

#define Y2_IS_PCI_EXP    (1u << 25)
#define Y2_IS_TIST_OV    (1u << 29)
#define Y2_IS_MST_ERR    (1u << 27)
#define Y2_IS_IRQ_STAT   (1u << 26)
#define Y2_IS_PAR_RD1    (1u << 5)
#define Y2_IS_PAR_WR1    (1u << 4)
#define Y2_IS_PAR_MAC1   (1u << 3)
#define Y2_IS_PAR_RX1    (1u << 2)
#define Y2_IS_TCP_TXS1   (1u << 1)
#define Y2_IS_TCP_TXA1   (1u << 0)
#define Y2_IS_PAR_RD2    (1u << 13)
#define Y2_IS_PAR_WR2    (1u << 12)
#define Y2_IS_PAR_MAC2   (1u << 11)
#define Y2_IS_PAR_RX2    (1u << 10)
#define Y2_IS_TCP_TXS2   (1u << 9)
#define Y2_IS_TCP_TXA2   (1u << 8)

#define Y2_HWE_ALL_MASK (Y2_IS_TIST_OV | Y2_IS_MST_ERR | Y2_IS_IRQ_STAT | \
                         Y2_IS_PAR_RD1 | Y2_IS_PAR_WR1 | Y2_IS_PAR_MAC1 | \
                         Y2_IS_PAR_RX1 | Y2_IS_TCP_TXS1 | Y2_IS_TCP_TXA1 | \
                         Y2_IS_PAR_RD2 | Y2_IS_PAR_WR2 | Y2_IS_PAR_MAC2 | \
                         Y2_IS_PAR_RX2 | Y2_IS_TCP_TXS2 | Y2_IS_TCP_TXA2)

/* PCI device-specific config registers (accessed via Y2_CFG_SPC) */
#define PCI_DEV_REG1     0x40
#define PCI_DEV_REG3     0x80
#define PCI_DEV_REG4     0x84
#define PCI_DEV_REG5     0x88
#define PCI_CFG_REG_1    0x94
#define PCI_STATUS       0x06
#define PCI_STATUS_ERROR_BITS 0xf900
#define PCI_ERR_UNCOR_STATUS  0x04
#define PCI_Y2_PHY1_POWD (1u << 26)
#define P_ASPM_CONTROL_MSK     ((1u<<15)|(1u<<14)|(1u<<13)|(1u<<12))
#define P_CTL_TIM_VMAIN_AV_MSK (3u << 27)

/* Chip IDs */
#define CHIP_ID_YUKON_EC    0xb6
#define CHIP_ID_YUKON_EC_U  0xb4
#define CHIP_ID_YUKON_EX    0xb5
#define CHIP_ID_YUKON_SUPR  0xb7
#define CHIP_ID_YUKON_UL_2  0xba
#define CHIP_ID_YUKON_OPT   0xbc
#define CHIP_ID_YUKON_PRM   0xbd
#define CHIP_ID_YUKON_OP_2  0xbe
#define CHIP_ID_YUKON_FE    0xb8
#define CHIP_ID_YUKON_FE_P  0xb9
#define CHIP_ID_YUKON_XL    0xb3

#define CHIP_REV_YU_EC_U_A0 1
#define CHIP_REV_YU_EC_U_B1 5

/* GM_* registers inside the GMAC window */
#define GM_GP_STAT       0x0000
#define GM_GP_CTRL       0x0004
#define GM_TX_CTRL       0x0008
#define GM_RX_CTRL       0x000c
#define GM_TX_FLOW_CTRL  0x0010
#define GM_TX_PARAM      0x0014
#define GM_SERIAL_MODE   0x0018
#define GM_SRC_ADDR_1L   0x001c
#define GM_SRC_ADDR_2L   0x0028
#define GM_MC_ADDR_H1    0x0034
#define GM_MC_ADDR_H2    0x0038
#define GM_MC_ADDR_H3    0x003c
#define GM_MC_ADDR_H4    0x0040
#define GM_TX_IRQ_MSK    0x0050
#define GM_RX_IRQ_MSK    0x0054
#define GM_TR_IRQ_MSK    0x0058
#define GM_SMI_CTRL      0x0080
#define GM_SMI_DATA      0x0084
#define GM_PHY_ADDR      0x0088
#define GM_MIB_CNT_BASE  0x0100
#define GM_MIB_CNT_END   0x025C
#define GM_TXF_BC_OK     (GM_MIB_CNT_BASE + 200)  /* bcast frames tx'd */

/* GM_GP_CTRL bits */
#define GM_GPCR_TX_ENA       (1u << 12)
#define GM_GPCR_RX_ENA       (1u << 11)
#define GM_GPCR_GIGS_ENA     (1u << 7)
#define GM_GPCR_DUP_FULL     (1u << 5)
#define GM_GPCR_FC_RX_DIS    (1u << 4)
#define GM_GPCR_SPEED_100    (1u << 3)
#define GM_GPCR_AU_DUP_DIS   (1u << 2)
#define GM_GPCR_AU_FCT_DIS   (1u << 1)
#define GM_GPCR_AU_SPD_DIS   (1u << 0)
#define GM_GPCR_SPEED_1000   (GM_GPCR_GIGS_ENA | GM_GPCR_SPEED_100)
#define GM_GPCR_FC_TX_DIS    (1u << 13)
#define GM_GPCR_FL_PASS      (1u << 6)

/* GM_RX_CTRL bits */
#define GM_RXCR_UCF_ENA  (1u << 15)
#define GM_RXCR_MCF_ENA  (1u << 14)
#define GM_RXCR_CRC_DIS  (1u << 13)

/* GM_TX_CTRL */
#define TX_COL_THR(x)    (((x) << 10) & (0x1f << 10))
#define TX_COL_DEF       0x04

/* GM_TX_PARAM defaults */
#define TX_JAM_LEN_VAL(x)  (((x) << 14) & (0x03 << 14))
#define TX_JAM_IPG_VAL(x)  (((x) << 9)  & (0x1f << 9))
#define TX_IPG_JAM_DATA(x) (((x) << 4)  & (0x1f << 4))
#define TX_BACK_OFF_LIM(x) ((x) & 0x0f)
#define TX_JAM_LEN_DEF   0x03
#define TX_JAM_IPG_DEF   0x0b
#define TX_IPG_JAM_DEF   0x1c
#define TX_BOF_LIM_DEF   0x04

/* GM_SERIAL_MODE */
#define GM_SMOD_VLAN_ENA (1u << 9)
#define GM_NEW_FLOW_CTRL (1u << 6)
#define DATA_BLIND_VAL(x) (((x) << 11) & (0x1f << 11))
#define IPG_DATA_VAL(x)   ((x) & 0x1f)
#define DATA_BLIND_DEF    0x04
#define IPG_DATA_DEF_1000 0x1e
#define IPG_DATA_DEF_10_100 0x18

/* GM_SMI_CTRL */
#define GM_SMI_CT_PHY_AD(x) (((uint16_t)(x) << 11) & (0x1f << 11))
#define GM_SMI_CT_REG_AD(x) (((uint16_t)(x) << 6)  & (0x1f << 6))
#define GM_SMI_CT_OP_RD     (1u << 5)
#define GM_SMI_CT_RD_VAL    (1u << 4)
#define GM_SMI_CT_BUSY      (1u << 3)
#define PHY_ADDR_MARV       0

#define GM_PAR_MIB_CLR   (1u << 5)

/* PHY registers */
#define PHY_MARV_CTRL       0x00
#define PHY_MARV_STAT       0x01
#define PHY_MARV_ID0        0x02
#define PHY_MARV_ID1        0x03
#define PHY_MARV_AUNE_ADV   0x04
#define PHY_MARV_AUNE_LP    0x05
#define PHY_MARV_1000T_CTRL 0x09
#define PHY_MARV_PHY_CTRL   0x10
#define PHY_MARV_PHY_STAT   0x11
#define PHY_MARV_INT_MASK   0x12
#define PHY_MARV_INT_STAT   0x13
#define PHY_MARV_EXT_CTRL   0x14
#define PHY_MARV_EXT_ADR    0x16

#define PHY_CT_RESET    (1u << 15)
#define PHY_CT_ANE      (1u << 12)
#define PHY_CT_RE_CFG   (1u << 9)
#define PHY_CT_DUP_MD   (1u << 8)
#define PHY_CT_SP1000   (1u << 6)
#define PHY_CT_SP100    (1u << 13)

#define PHY_AN_CSMA        (1u << 0)
#define PHY_M_AN_100_FD    (1u << 8)
#define PHY_M_AN_100_HD    (1u << 7)
#define PHY_M_AN_10_FD     (1u << 6)
#define PHY_M_AN_10_HD     (1u << 5)
#define PHY_M_AN_PC        (1u << 10)
#define PHY_M_AN_ASP       (1u << 11)

#define PHY_M_1000C_AFD    (1u << 9)
#define PHY_M_1000C_AHD    (1u << 8)

#define PHY_M_PC_EN_DET_MSK    (3u << 8)
#define PHY_M_PC_MDI_XMODE(x)  (((uint16_t)(x) << 5) & (3u << 5))
#define PHY_M_PC_ENA_AUTO      3
#define PHY_M_PC_DSC_MSK       (7u << 12)
#define PHY_M_PC_DOWN_S_ENA    (1u << 11)
#define PHY_M_PC_DSC(x)        (((uint16_t)(x) << 12) & PHY_M_PC_DSC_MSK)

#define PHY_M_PS_FULL_DUP   (1u << 13)
#define PHY_M_PS_SPDUP_RES  (1u << 11)
#define PHY_M_PS_LINK_UP    (1u << 10)
#define PHY_M_PS_SPEED_MSK  (3u << 14)

#define PHY_M_IS_LSP_CHANGE  (1u << 14)
#define PHY_M_IS_DUP_CHANGE  (1u << 13)
#define PHY_M_IS_LST_CHANGE  (1u << 10)
#define PHY_M_DEF_MSK  (PHY_M_IS_LSP_CHANGE | PHY_M_IS_LST_CHANGE | \
                      PHY_M_IS_DUP_CHANGE)

/* LED control (page 3 PHY regs) */
#define PHY_M_LED_PULS_DUR(x)  (((uint16_t)(x) << 12) & (7u << 12))
#define PHY_M_LED_BLINK_RT(x)  (((uint16_t)(x) << 8)  & (7u << 8))
#define PHY_M_LEDC_LOS_CTRL(x) (((uint16_t)(x) << 12) & (0xfu << 12))
#define PHY_M_LEDC_INIT_CTRL(x)(((uint16_t)(x) << 8)  & (0xfu << 8))
#define PHY_M_LEDC_STA1_CTRL(x)(((uint16_t)(x) << 4)  & (0xfu << 4))
#define PHY_M_LEDC_STA0_CTRL(x)(((uint16_t)(x) << 0)  & 0xfu)
#define PULS_170MS   4
#define BLINK_84MS   1

/* Frame status bits in OP_RXSTAT entries */
#define GMR_FS_LEN       (0x7fffu << 16)
#define GMR_FS_RX_OK     (1u << 8)
#define GMR_FS_ANY_ERR   (GMR_FS_RX_FF_OV | GMR_FS_CRC_ERR | GMR_FS_FRAGMENT | \
                          GMR_FS_LONG_ERR | GMR_FS_MII_ERR | GMR_FS_BAD_FC |  \
                          GMR_FS_UN_SIZE | GMR_FS_JABBER)
#define GMR_FS_JABBER    (1u << 12)
#define GMR_FS_UN_SIZE   (1u << 11)
#define GMR_FS_GOOD_FC   (1u << 7)
#define GMR_FS_BAD_FC    (1u << 6)
#define GMR_FS_MII_ERR   (1u << 5)
#define GMR_FS_LONG_ERR  (1u << 4)
#define GMR_FS_FRAGMENT  (1u << 3)
#define GMR_FS_CRC_ERR   (1u << 1)
#define GMR_FS_RX_FF_OV  (1u << 0)

/* List element opcodes */
#define HW_OWNER     0x80
#define OP_ADDR64    0x21
#define OP_PACKET    0x41
#define OP_RXSTAT    0x60
#define OP_TXINDEXLE 0x68
#define CSS_LINK_BIT 0x01
#define TX_LE_EOP    (1u << 7)   /* ctrl byte: end-of-packet marker */

/* Chip IDs we accept at PCI probe */
static const uint16_t k_sky2_devids[] = {
    0x436a, /* 88E8058 — MacBookPro4,1 */
    0x436b, 0x436c, 0x436d, /* 88E8071/88E8055/88E8057-class */
    0x4360, 0x4362, 0x4363, 0x4364, 0x4365, 0x4366, 0x4369,
    0x4350, 0x4351, 0x4352, 0x4353, 0x4354, 0x4355, 0x4356, 0x4357,
    0x435a, 0x435b, 0x4340, 0x4341, 0x4342, 0x4343, 0x4344, 0x4345,
    0x4346, 0x4347, 0x4358, 0x4359, 0x4370,
    0
};

/* -------------------------------------------------------------------------
 * Ring geometry
 * ------------------------------------------------------------------------- */
#define SKY2_RX_LES      128   /* RX list elements (2 per buffer) */
#define SKY2_RX_BUFS     48    /* posted receive buffers */
#define SKY2_TX_LES      128   /* TX list elements — matches Linux (LAST_IDX 0x7F);
                                * at 64 the chip ran GET past 0x3F and reported
                                * done=0x40, so TX died on the first wrap */
#define SKY2_TX_BUFSZ    1536  /* per-LE TX bounce buffer */
#define SKY2_STATUS_LES  256   /* status ring entries */
#define SKY2_RING_ALIGN  32768 /* LE/status ring base alignment (HW ignores low bits) */

typedef struct __attribute__((packed)) {
    uint32_t addr;
    uint16_t length;
    uint8_t  ctrl;
    uint8_t  opcode;
} Sky2LE;

typedef Sky2LE Sky2RxLE;
typedef Sky2LE Sky2TxLE;

/* Status LE: same 8-byte layout, but first dword is a status word
 * and the css byte carries the port in bit 0. */
typedef struct __attribute__((packed)) {
    uint32_t status;
    uint16_t length;
    uint8_t  css;
    uint8_t  opcode;
} Sky2StatusLE;

/* -------------------------------------------------------------------------
 * Driver state
 * ------------------------------------------------------------------------- */
static uint8_t  g_mac[6];
static int      g_up = 0;
static uint8_t  g_bus = 0, g_dev = 0, g_fn = 0;
static uint8_t  g_chip_id = 0, g_chip_rev = 0;
static int      g_has_ram_buffer = 0;

static Sky2RxLE     *g_rx_le = 0;
static Sky2TxLE     *g_tx_le = 0;
static Sky2StatusLE *g_st_le = 0;
static uint8_t      *g_rx_buf = 0;   /* SKY2_RX_BUFS * SKY2_RX_BUFSZ */
static uint8_t      *g_tx_buf = 0;   /* SKY2_TX_LES * SKY2_TX_BUFSZ */

static uint16_t g_rx_put  = 0;   /* next RX LE to write */
static uint16_t g_rx_next = 0;   /* next RX buffer expected to complete */
static uint16_t g_rx_len[SKY2_RX_BUFS];   /* 0 = errored frame, drop */
static uint16_t g_rx_deliv   = 0;         /* next buffer to deliver */
static uint16_t g_rx_pending = 0;         /* completed, not yet delivered */
static uint16_t g_tx_prod = 0;   /* next TX LE to write */
static uint16_t g_tx_done = 0;   /* last TX LE index reported complete */
static uint16_t g_st_idx  = 0;   /* next status entry to consume */

static sky2_rx_cb g_rx_cb = 0;
static void (*g_notify_cb)(void) = 0;   /* fired when RX completions land */

/* -------------------------------------------------------------------------
 * Delay helpers
 * ------------------------------------------------------------------------- */
static void io_delay(void)
{
    __asm__ volatile("inb $0x80, %%al" ::: "eax");
}

static void udelay(uint32_t us)
{
    for (uint32_t i = 0; i < us; i++)
        for (volatile int j = 0; j < 10; j++) io_delay();
}

static void msdelay(uint32_t ms)
{
    for (uint32_t m = 0; m < ms; m++)
        udelay(1000);
}

/* -------------------------------------------------------------------------
 * PHY access via the SMI register pair in the GMAC window
 * ------------------------------------------------------------------------- */
static int gm_phy_write(int port, uint16_t reg, uint16_t val)
{
    gma_w16(port, GM_SMI_DATA, val);
    gma_w16(port, GM_SMI_CTRL,
            GM_SMI_CT_PHY_AD(PHY_ADDR_MARV) | GM_SMI_CT_REG_AD(reg));

    for (int i = 0; i < 100; i++) {
        uint16_t ctrl = gma_r16(port, GM_SMI_CTRL);
        if (ctrl == 0xffff) return -1;
        if (!(ctrl & GM_SMI_CT_BUSY)) return 0;
        udelay(10);
    }
    return -1;
}

static uint16_t gm_phy_read(int port, uint16_t reg)
{
    gma_w16(port, GM_SMI_CTRL,
            GM_SMI_CT_PHY_AD(PHY_ADDR_MARV) | GM_SMI_CT_REG_AD(reg) |
            GM_SMI_CT_OP_RD);

    for (int i = 0; i < 100; i++) {
        uint16_t ctrl = gma_r16(port, GM_SMI_CTRL);
        if (ctrl == 0xffff) return 0;
        if (ctrl & GM_SMI_CT_RD_VAL)
            return gma_r16(port, GM_SMI_DATA);
        udelay(10);
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * PCI probe — find a Marvell Yukon-2 device
 * ------------------------------------------------------------------------- */
static int pci_find_sky2(uint8_t *bus_out, uint8_t *dev_out,
                         uint8_t *fn_out, uint64_t *bar0_out)
{
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t fn = 0; fn < 8; fn++) {
                uint32_t id = pci_read32((uint8_t)bus, dev, fn, 0x00);
                if (id == 0xFFFFFFFF) { if (fn == 0) goto next_dev; continue; }
                if ((id & 0xFFFF) != 0x11AB) continue;

                uint16_t device = (uint16_t)(id >> 16);
                int match = 0;
                for (int k = 0; k_sky2_devids[k]; k++)
                    if (device == k_sky2_devids[k]) { match = 1; break; }
                if (!match) continue;

                /* BAR0 is a 64-bit MMIO BAR */
                uint32_t lo = pci_read32((uint8_t)bus, dev, fn, 0x10);
                uint32_t hi = pci_read32((uint8_t)bus, dev, fn, 0x14);
                if (lo & 1) continue;              /* I/O BAR — not usable */
                uint64_t bar = ((uint64_t)hi << 32) | (lo & ~0xFULL);
                if (!(lo & ~0xFULL)) continue;
                if (bar >= 0x100000000ULL) continue; /* must stay below 4G */

                uint16_t cmd = pci_read16((uint8_t)bus, dev, fn, 0x04);
                pci_write16((uint8_t)bus, dev, fn, 0x04,
                            (uint16_t)(cmd | 0x06)); /* MEM | BusMaster */

                /* Match the PCIe config Linux ends up with on this chip:
                 * DevCtl = all error reporting on + MaxReadReq 2048B
                 * (Linux negotiates 0x400f via pcie_set_readrq), and a
                 * cache-line size of 0x40.  Without the error-report
                 * bits, posted-write failures never reach the AER regs.
                 * CRITICAL: also clear NoSnoop (bit 11) and Relaxed
                 * Ordering (bit 4) — both default to 1 at PCIe reset.
                 * With NoSnoop set the chip's DMA writes bypass CPU
                 * cache snooping, so the status ring stays at its stale
                 * cached zeros while STAT_PUT_IDX advances — and stale
                 * descriptor reads latch CHK_* errors on both queues. */
                if (!(pci_read16((uint8_t)bus, dev, fn, 0x0C) & 0xFF))
                    pci_write16((uint8_t)bus, dev, fn, 0x0C, 0x40);
                for (uint8_t cap = (uint8_t)(pci_read32((uint8_t)bus, dev, fn, 0x34) & 0xFF);
                     cap && cap != 0xFF;
                     cap = (uint8_t)((pci_read32((uint8_t)bus, dev, fn, cap) >> 8) & 0xFF)) {
                    if ((pci_read32((uint8_t)bus, dev, fn, cap) & 0xFF) != 0x10)
                        continue;   /* not the PCIe cap */
                    uint16_t dc0 = pci_read16((uint8_t)bus, dev, fn, cap + 0x08);
                    uint16_t dc = (uint16_t)((dc0 & ~0xF810u) | 0x000F | (4 << 12));
                    pci_write16((uint8_t)bus, dev, fn, cap + 0x08, dc);
                    uint16_t dc1 = pci_read16((uint8_t)bus, dev, fn, cap + 0x08);
                    klog_puts(KLOG_SKY2, KLOG_DEBUG, "devctl=");
                    klog_appendf(KLOG_SKY2, KLOG_DEBUG, "0x%08X", (uint32_t)dc0);
                    klog_puts(KLOG_SKY2, KLOG_DEBUG, "->");
                    klog_appendf(KLOG_SKY2, KLOG_DEBUG, "0x%08X\n", (uint32_t)dc1);
                    break;
                }

                *bus_out = (uint8_t)bus;
                *dev_out = dev;
                *fn_out  = fn;
                *bar0_out = bar;
                return 1;
            }
            next_dev:;
        }
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * PHY init — auto-negotiate all copper speeds, symmetric+asymmetric pause
 * Mirrors sky2_phy_init() for NEWER_PHY gigabit copper (EC_U path).
 * ------------------------------------------------------------------------- */
static void sky2_phy_power_up(int port)
{
    w8(B2_TST_CTRL1, TST_CFG_WRITE_ON);
    uint32_t r = pcr32(PCI_DEV_REG1);
    r &= ~PCI_Y2_PHY1_POWD;
    pcw32(PCI_DEV_REG1, r);
    w8(B2_TST_CTRL1, TST_CFG_WRITE_OFF);
    pcr32(PCI_DEV_REG1);

    /* EC_U has SKY2_HW_ADV_POWER_CTL: release GPHY reset */
    w8(SK_REG(port, GPHY_CTRL), GPC_RST_CLR);
}

static void sky2_phy_init(int port)
{
    uint16_t ctrl, ct1000, adv, pg;

    /* Copper + gigabit + newer PHY: disable energy detect, enable
     * automatic crossover, set downshift counter to 3x + enable */
    ctrl = gm_phy_read(port, PHY_MARV_PHY_CTRL);
    ctrl &= ~PHY_M_PC_EN_DET_MSK;
    ctrl |= PHY_M_PC_MDI_XMODE(PHY_M_PC_ENA_AUTO);
    ctrl &= ~PHY_M_PC_DSC_MSK;
    ctrl |= PHY_M_PC_DSC(2) | PHY_M_PC_DOWN_S_ENA;
    gm_phy_write(port, PHY_MARV_PHY_CTRL, ctrl);

    /* Auto-speed + auto-pause (FC_BOTH): advertise everything */
    ctrl   = PHY_CT_RESET | PHY_CT_ANE | PHY_CT_RE_CFG;
    ct1000 = PHY_M_1000C_AFD | PHY_M_1000C_AHD;
    adv    = PHY_AN_CSMA | PHY_M_AN_100_FD | PHY_M_AN_100_HD |
             PHY_M_AN_10_FD | PHY_M_AN_10_HD |
             PHY_M_AN_PC | PHY_M_AN_ASP;

    /* GMAC general purpose control: all auto-update enabled */
    gma_w16(port, GM_GP_CTRL, 0);

    gm_phy_write(port, PHY_MARV_1000T_CTRL, ct1000);
    gm_phy_write(port, PHY_MARV_AUNE_ADV, adv);
    gm_phy_write(port, PHY_MARV_CTRL, ctrl);

    /* LED setup for YUKON_EC_U / EX / SUPR: page 3 registers */
    pg = gm_phy_read(port, PHY_MARV_EXT_ADR);
    gm_phy_write(port, PHY_MARV_EXT_ADR, 3);
    gm_phy_write(port, PHY_MARV_PHY_CTRL,
                 (uint16_t)(PHY_M_LEDC_LOS_CTRL(1) | PHY_M_LEDC_INIT_CTRL(8) |
                            PHY_M_LEDC_STA1_CTRL(7) | PHY_M_LEDC_STA0_CTRL(7)));
    gm_phy_write(port, PHY_MARV_INT_MASK,
                 (uint16_t)(PHY_M_LED_PULS_DUR(PULS_170MS) |
                            PHY_M_LED_BLINK_RT(BLINK_84MS)));
    gm_phy_write(port, PHY_MARV_EXT_ADR, pg);

    /* EC_U / UL_2 PHY AFE fixes */
    if (g_chip_id == CHIP_ID_YUKON_EC_U || g_chip_id == CHIP_ID_YUKON_UL_2) {
        gm_phy_write(port, PHY_MARV_EXT_ADR, 255);
        gm_phy_write(port, 0x18, 0xaa99);   /* 10BASE-T amplitude */
        gm_phy_write(port, 0x17, 0x2011);
        if (g_chip_id == CHIP_ID_YUKON_EC_U) {
            gm_phy_write(port, 0x18, 0xa204); /* 1000BASE-T symmetry */
            gm_phy_write(port, 0x17, 0x2002);
        }
        gm_phy_write(port, PHY_MARV_EXT_ADR, 0);
    }
}

/* -------------------------------------------------------------------------
 * GMAC helpers
 * ------------------------------------------------------------------------- */
static void sky2_gmac_reset(int port)
{
    w8(SK_REG(port, GMAC_IRQ_MSK), 0);
    gma_w16(port, GM_MC_ADDR_H1, 0);
    gma_w16(port, GM_MC_ADDR_H2, 0);
    gma_w16(port, GM_MC_ADDR_H3, 0);
    gma_w16(port, GM_MC_ADDR_H4, 0);
    gma_w16(port, GM_RX_CTRL,
            (uint16_t)(gma_r16(port, GM_RX_CTRL) |
                       GM_RXCR_UCF_ENA | GM_RXCR_MCF_ENA));
}

static void sky2_gmac_addr(int port, uint32_t reg, const uint8_t *a)
{
    gma_w16(port, reg,     (uint16_t)(a[0] | ((uint16_t)a[1] << 8)));
    gma_w16(port, reg + 4, (uint16_t)(a[2] | ((uint16_t)a[3] << 8)));
    gma_w16(port, reg + 8, (uint16_t)(a[4] | ((uint16_t)a[5] << 8)));
}

static void sky2_mac_init(int port)
{
    uint16_t reg;

    w8(SK_REG(port, GPHY_CTRL), GPC_RST_SET);
    w8(SK_REG(port, GPHY_CTRL), GPC_RST_CLR);
    w8(SK_REG(port, GMAC_CTRL), GMC_RST_CLR);

    r16(SK_REG(port, GMAC_IRQ_SRC));   /* flush pending */
    w8(SK_REG(port, GMAC_IRQ_MSK), GMAC_DEF_MSK);

    sky2_phy_power_up(port);
    sky2_phy_init(port);

    /* MIB clear */
    reg = gma_r16(port, GM_PHY_ADDR);
    gma_w16(port, GM_PHY_ADDR, (uint16_t)(reg | GM_PAR_MIB_CLR));
    for (uint32_t i = GM_MIB_CNT_BASE; i <= GM_MIB_CNT_END; i += 4)
        gma_r16(port, i);
    gma_w16(port, GM_PHY_ADDR, reg);

    gma_w16(port, GM_TX_CTRL, TX_COL_THR(TX_COL_DEF));
    gma_w16(port, GM_RX_CTRL,
            GM_RXCR_UCF_ENA | GM_RXCR_CRC_DIS | GM_RXCR_MCF_ENA);
    gma_w16(port, GM_TX_FLOW_CTRL, 0xffff);
    gma_w16(port, GM_TX_PARAM,
            (uint16_t)(TX_JAM_LEN_VAL(TX_JAM_LEN_DEF) |
                       TX_JAM_IPG_VAL(TX_JAM_IPG_DEF) |
                       TX_IPG_JAM_DATA(TX_IPG_JAM_DEF) |
                       TX_BACK_OFF_LIM(TX_BOF_LIM_DEF)));

    reg = (uint16_t)(DATA_BLIND_VAL(DATA_BLIND_DEF) | GM_SMOD_VLAN_ENA |
                     IPG_DATA_VAL(IPG_DATA_DEF_1000));
    if (g_chip_id == CHIP_ID_YUKON_EC_U && g_chip_rev == CHIP_REV_YU_EC_U_B1)
        reg |= GM_NEW_FLOW_CTRL;
    gma_w16(port, GM_SERIAL_MODE, reg);

    sky2_gmac_addr(port, GM_SRC_ADDR_2L, g_mac);
    sky2_gmac_addr(port, GM_SRC_ADDR_1L, g_mac);

    gma_w16(port, GM_TX_IRQ_MSK, 0);
    gma_w16(port, GM_RX_IRQ_MSK, 0);
    gma_w16(port, GM_TR_IRQ_MSK, 0);

    /* Rx MAC FIFO */
    w8(SK_REG(port, RX_GMF_CTRL_T), GMF_RST_CLR);
    w32(SK_REG(port, RX_GMF_CTRL_T), GMF_OPER_ON | GMF_RX_F_FL_ON);
    w16(SK_REG(port, RX_GMF_FL_MSK), (uint16_t)GMR_FS_ANY_ERR);
    w16(SK_REG(port, RX_GMF_FL_THR), RX_GMF_FL_THR_DEF + 1);

    /* Tx MAC FIFO */
    w8(SK_REG(port, TX_GMF_CTRL_T), GMF_RST_CLR);
    w16(SK_REG(port, TX_GMF_CTRL_T), GMF_OPER_ON);

    /* Chips without an internal RAM buffer (B2_E_0 reads 0 — e.g. the
     * 88E8058/EC_U in the MacBookPro4,1): pause is MAC-level and the TX
     * FIFO must be switched to store-and-forward or fetched packets
     * never reach the wire (observed: prefetch GET advances, zero
     * status entries, silent TX hang).  Mirrors Linux sky2_set_tx_stfwd
     * + the RX pause thresholds. */
    if (!g_has_ram_buffer) {
        w16(SK_REG(port, RX_GMF_UP_THR), 1024 / 8);
        w16(SK_REG(port, RX_GMF_LP_THR), 768 / 8);
        w32(SK_REG(port, TX_GMF_CTRL_T), TX_STFW_ENA);
    }
}

/* -------------------------------------------------------------------------
 * Queue / RAM buffer / prefetch programming
 * ------------------------------------------------------------------------- */
static void sky2_ramset(uint16_t q, uint32_t start, uint32_t space)
{
    start *= 1024 / 8;    /* KB -> qwords */
    space *= 1024 / 8;
    uint32_t end = start + space - 1;

    w8(RB_ADDR(q, RB_CTRL), RB_RST_CLR);
    w32(RB_ADDR(q, RB_START), start);
    w32(RB_ADDR(q, RB_END), end);
    w32(RB_ADDR(q, RB_WP), start);
    w32(RB_ADDR(q, RB_RP), start);

    if (q == Q_R1) {
        w32(RB_ADDR(q, RB_RX_UTHP), space - space / 4);
        w32(RB_ADDR(q, RB_RX_LTHP), space / 2);
        w32(RB_ADDR(q, RB_RX_UTPP), space - 8192 / 8);
        w32(RB_ADDR(q, RB_RX_LTPP), space / 4);
    } else {
        w8(RB_ADDR(q, RB_CTRL), RB_ENA_STFWD);
    }
    w8(RB_ADDR(q, RB_CTRL), RB_ENA_OP_MD);
    r8(RB_ADDR(q, RB_CTRL));
}

static void sky2_qset(uint16_t q)
{
    w32(Q_ADDR(q, Q_CSR), BMU_CLR_RESET);
    w32(Q_ADDR(q, Q_CSR), BMU_OPER_INIT);
    w32(Q_ADDR(q, Q_CSR), BMU_FIFO_OP_ON);
    w32(Q_ADDR(q, Q_WM), BMU_WM_DEFAULT);
}

static void sky2_prefetch_init(uint16_t q, uint64_t addr, uint32_t last)
{
    w32(Y2_QADDR(q, PREF_UNIT_CTRL), PREF_UNIT_RST_SET);
    w32(Y2_QADDR(q, PREF_UNIT_CTRL), PREF_UNIT_RST_CLR);
    w32(Y2_QADDR(q, PREF_UNIT_ADDR_HI), (uint32_t)(addr >> 32));
    w32(Y2_QADDR(q, PREF_UNIT_ADDR_LO), (uint32_t)addr);
    w16(Y2_QADDR(q, PREF_UNIT_LAST_IDX), (uint16_t)last);
    w32(Y2_QADDR(q, PREF_UNIT_CTRL), PREF_UNIT_OP_ON);
    r32(Y2_QADDR(q, PREF_UNIT_CTRL));
    uint32_t rb = r32(Y2_QADDR(q, PREF_UNIT_ADDR_LO));
    uint16_t lrb = r16(Y2_QADDR(q, PREF_UNIT_LAST_IDX));
    if (rb != (uint32_t)addr || lrb != (uint16_t)last)
        KLOG(KLOG_SKY2, KLOG_WARN, "pref q=0x%08X addr=0x%08X/0x%08X last=0x%08X/0x%08X MISMATCH",
             (uint32_t)q, (uint32_t)addr, rb, last, (uint32_t)lrb);
}

/* -------------------------------------------------------------------------
 * RX / TX list element submission
 * ------------------------------------------------------------------------- */
static void sky2_rx_submit(int idx)
{
    uint64_t addr = (uint64_t)(uintptr_t)(g_rx_buf + idx * SKY2_RX_BUFSZ);

    Sky2RxLE *le = &g_rx_le[g_rx_put];
    g_rx_put = (g_rx_put + 1) & (SKY2_RX_LES - 1);
    le->ctrl = 0;
    le->addr = (uint32_t)(addr >> 32);
    le->opcode = OP_ADDR64 | HW_OWNER;

    le = &g_rx_le[g_rx_put];
    g_rx_put = (g_rx_put + 1) & (SKY2_RX_LES - 1);
    le->ctrl = 0;
    le->addr = (uint32_t)addr;
    le->length = SKY2_RX_BUFSZ;
    le->opcode = OP_PACKET | HW_OWNER;
}

static void sky2_rx_update(void)
{
    __asm__ volatile("mfence" ::: "memory");
    w16(Y2_QADDR(Q_R1, PREF_UNIT_PUT_IDX), g_rx_put);
}

static void sky2_rx_start(int port)
{
    (void)port;
    g_rx_put = 0;
    g_rx_next = 0;
    g_rx_deliv = 0;
    g_rx_pending = 0;
    sky2_qset(Q_R1);

    /* PCIe: lower the FIFO watermark for better performance */
    w32(Q_ADDR(Q_R1, Q_WM), BMU_WM_PEX);

    /* EC_U > A0: MAC Rx RAM read is HW controlled */
    if (g_chip_id == CHIP_ID_YUKON_EC_U && g_chip_rev > CHIP_REV_YU_EC_U_A0)
        w32(Q_ADDR(Q_R1, Q_TEST), (1u << 24)); /* F_M_RX_RAM_DIS */

    sky2_prefetch_init(Q_R1, (uint64_t)(uintptr_t)g_rx_le, SKY2_RX_LES - 1);

    for (int i = 0; i < SKY2_RX_BUFS; i++)
        sky2_rx_submit(i);

    /* Truncate oversize frames: thresh = (roundup(MTU+14+4,8)-8)/4 dwords */
    uint32_t thresh = (((SKY2_MTU + 4) + 7) & ~7u);
    thresh = (thresh - 8) / 4;
    if (thresh > 0x1ff) {
        w32(SK_REG(0, RX_GMF_CTRL_T), (1u << 26)); /* RX_TRUNC_OFF */
    } else {
        w16(SK_REG(0, RX_GMF_TR_THR), (uint16_t)thresh);
        w32(SK_REG(0, RX_GMF_CTRL_T), RX_TRUNC_ON);
    }

    sky2_rx_update();
}

/* -------------------------------------------------------------------------
 * Status ring processing
 * ------------------------------------------------------------------------- */
/* Concurrency model.  The status ring is drained from three places —
 * sky2_send() while it waits for its TX index, sky2_poll() from any task,
 * and the IRQ handler — and tasks are preemptible.  So:
 *   - draining never calls into the net stack; it runs with interrupts
 *     off and only records completions: TX index -> g_tx_done, RX frame
 *     -> per-buffer length, pushed onto an in-order pending FIFO
 *     (buffers complete in LE order, so the FIFO is just a counter).
 *   - delivery happens only from sky2_poll(): pop one buffer, copy it to
 *     the caller's stack and resubmit it to the chip atomically (so the
 *     buffer/LE order can never permute, however calls nest or tasks
 *     interleave), then run the RX callback with interrupts on.
 * There is no global "someone is draining" gate, so a task that sleeps
 * or is preempted inside the RX callback cannot stall anyone else. */
static inline uint64_t sky2_irq_save(void)
{
    return irq_save();
}
static inline void sky2_irq_restore(uint64_t f)
{
    irq_restore(f);
}

static int sky2_drain_status(void)
{
    int rx_new = 0;
    uint64_t fl = sky2_irq_save();
    while (g_st_idx != r16(STAT_PUT_IDX)) {
        Sky2StatusLE *le = &g_st_le[g_st_idx];
        uint8_t opcode = le->opcode;
        if (!(opcode & HW_OWNER))
            break;

        uint16_t length = le->length;
        uint32_t status = le->status;
        uint16_t count = (uint16_t)((status & GMR_FS_LEN) >> 16);
        klog_puts(KLOG_SKY2, KLOG_TRACE, "st#"); klog_appendf(KLOG_SKY2, KLOG_TRACE, "0x%08X",
            (uint32_t)g_st_idx);
        klog_puts(KLOG_SKY2, KLOG_TRACE, " op="); klog_appendf(KLOG_SKY2, KLOG_TRACE, "0x%08X",
            (uint32_t)opcode);
        klog_puts(KLOG_SKY2, KLOG_TRACE, " st="); klog_appendf(KLOG_SKY2, KLOG_TRACE, "0x%08X\n",
            status);
        le->opcode = 0;
        g_st_idx = (g_st_idx + 1) & (SKY2_STATUS_LES - 1);

        switch (opcode & ~HW_OWNER) {
        case OP_RXSTAT: {
            /* Data already DMA'd into rx buffer g_rx_next.  If the DMA
             * length doesn't match the PHY's frame length the packet was
             * truncated — mark it for drop.  Delivery + resubmit happen
             * later in sky2_rx_deliver(). */
            int ok = !(status & GMR_FS_ANY_ERR) && (status & GMR_FS_RX_OK) &&
                     length == count && length > 0 && length <= SKY2_RX_BUFSZ;
            if (g_rx_pending < SKY2_RX_BUFS) {
                g_rx_len[g_rx_next] = ok ? length : 0;
                g_rx_next = (uint16_t)((g_rx_next + 1) % SKY2_RX_BUFS);
                g_rx_pending++;
                rx_new = 1;
            }
            break;
        }
        case OP_TXINDEXLE:
            /* status & 0xfff = index past last completed TX LE, port 0 */
            g_tx_done = (uint16_t)(status & 0x0fff);
            break;
        default:
            break; /* ignore RXCHKS/VLAN/RSS/timestamp entries */
        }
    }
    /* Fully processed: clear status IRQ */
    w32(STAT_CTRL, SC_STAT_CLR_IRQ);
    sky2_irq_restore(fl);

    /* New RX completions are waiting for task-side delivery — wake any
     * consumer blocked in Task_WaitTicks(SIGF_NET) at interrupt time
     * instead of the next 100 Hz poll tick.  net_rx_kick() is IRQ-safe.
     * The IRQ handler uses the return value to reschedule at IRQ exit. */
    if (rx_new && g_notify_cb)
        g_notify_cb();
    return rx_new;
}

/* Deliver completed RX frames to the net stack.  Re-entrant: each frame
 * is popped, copied and its buffer resubmitted under cli, then the
 * callback runs on a private stack copy with interrupts restored. */
static void sky2_rx_deliver(void)
{
    uint8_t frame[SKY2_RX_BUFSZ];
    for (;;) {
        uint64_t fl = sky2_irq_save();
        if (!g_rx_pending) { sky2_irq_restore(fl); break; }
        uint16_t idx = g_rx_deliv;
        uint16_t len = g_rx_len[idx];
        if (len)
            memcpy(frame, g_rx_buf + idx * SKY2_RX_BUFSZ, len);
        g_rx_deliv = (uint16_t)((idx + 1) % SKY2_RX_BUFS);
        g_rx_pending--;
        sky2_rx_submit(idx);
        sky2_rx_update();
        sky2_irq_restore(fl);
        if (len && g_rx_cb)
            g_rx_cb(frame, len);
    }
}

/* -------------------------------------------------------------------------
 * Power / reset sequence (sky2_reset)
 * ------------------------------------------------------------------------- */
static void sky2_power_on(void)
{
    /* switch power to VCC (WA for VAUX problem) */
    w8(B0_POWER_CTRL, PC_VAUX_ENA | PC_VCC_ENA | PC_VAUX_OFF | PC_VCC_ON);

    /* disable core clock division, no clock gating */
    w32(B2_Y2_CLK_CTRL, Y2_CLK_DIV_DIS);
    w8(B2_Y2_CLK_GATE, 0);

    /* ADV_POWER_CTL path (EC_U/EX/SUPR/FE_P) */
    pcw32(PCI_DEV_REG3, 0);
    uint32_t r = pcr32(PCI_DEV_REG4);
    r &= P_ASPM_CONTROL_MSK;
    pcw32(PCI_DEV_REG4, r);
    r = pcr32(PCI_DEV_REG5);
    r &= P_CTL_TIM_VMAIN_AV_MSK;
    pcw32(PCI_DEV_REG5, r);
    pcw32(PCI_CFG_REG_1, 0);
    w16(B0_CTST, Y2_HW_WOL_ON);

    r = r32(B2_GP_IO);
    r |= GLB_GPIO_STAT_RACE_DIS;
    w32(B2_GP_IO, r);
    r32(B2_GP_IO);

    /* "driver loaded" LED */
    w16(B0_CTST, Y2_LED_STAT_ON);
}

static void sky2_reset(void)
{
    uint32_t hwe_mask = Y2_HWE_ALL_MASK;

    /* disable ASF firmware unit */
    w8(B28_Y2_ASF_STAT_CMD, Y2_ASF_RESET);
    w16(B0_CTST, Y2_ASF_DISABLE);
    /* ASF owns the status list if alive — log its state so a stuck unit
     * is visible during real-HW bring-up */
    klog_puts(KLOG_SKY2, KLOG_DEBUG, "asf="); klog_appendf(KLOG_SKY2, KLOG_DEBUG, "0x%08X\n",
        r32(B28_Y2_ASF_STAT_CMD));

    /* software reset */
    w8(B0_CTST, CS_RST_SET);
    w8(B0_CTST, CS_RST_CLR);

    /* allow writes to PCI config */
    w8(B2_TST_CTRL1, TST_CFG_WRITE_ON);

    /* clear PCI errors */
    uint16_t st = pcr16(PCI_STATUS);
    st |= PCI_STATUS_ERROR_BITS;
    pcw16(PCI_STATUS, st);

    w8(B0_CTST, CS_MRST_CLR);

    /* PCIe: clear advanced error reporting; if the PCI-Express error
     * bit is stuck on, ignore it (don't unmask in HWE_IMSK) */
    w32(Y2_CFG_AER + PCI_ERR_UNCOR_STATUS, 0xffffffffU);
    if (!(r32(B0_HWE_ISRC) & Y2_IS_PCI_EXP))
        hwe_mask |= Y2_IS_PCI_EXP;

    sky2_power_on();
    w8(B2_TST_CTRL1, TST_CFG_WRITE_OFF);

    /* MAC link control reset */
    w8(SK_REG(0, GMAC_LINK_CTRL), GMLC_RST_SET);
    w8(SK_REG(0, GMAC_LINK_CTRL), GMLC_RST_CLR);

    /* clear I2C IRQ noise, stop hw timer + descriptor poll + timestamps */
    w32(B2_I2C_IRQ, 1);
    w8(B2_TI_CTRL, TIM_STOP);
    w8(B2_TI_CTRL, TIM_CLR_IRQ);
    w32(B28_DPT_CTRL, DPT_STOP);
    w8(GMAC_TI_ST_CTRL, GMT_ST_STOP);
    w8(GMAC_TI_ST_CTRL, GMT_ST_CLR_IRQ);

    /* enable TX arbiter */
    w8(SK_REG(0, TXA_CTRL), TXA_ENA_ARB);

    /* RAM interface timeouts */
    w8(RAM_BUFFER(0, B3_RI_CTRL), RI_RST_CLR);
    w8(RAM_BUFFER(0, B3_RI_WTO_R1), SK_RI_TO_53);
    w8(RAM_BUFFER(0, B3_RI_WTO_XA1), SK_RI_TO_53);
    w8(RAM_BUFFER(0, B3_RI_WTO_XS1), SK_RI_TO_53);
    w8(RAM_BUFFER(0, B3_RI_RTO_R1), SK_RI_TO_53);
    w8(RAM_BUFFER(0, B3_RI_RTO_XA1), SK_RI_TO_53);
    w8(RAM_BUFFER(0, B3_RI_RTO_XS1), SK_RI_TO_53);

    w32(B0_HWE_IMSK, hwe_mask);

    sky2_gmac_reset(0);

    /* Status ring */
    memset(g_st_le, 0, SKY2_STATUS_LES * sizeof(Sky2StatusLE));
    g_st_idx = 0;
    uint64_t st_dma = (uint64_t)(uintptr_t)g_st_le;

    w32(STAT_CTRL, SC_STAT_RST_SET);
    /* CSR writes while the unit is still in reset may land in the
     * address latch before RST_CLR samples it — program the base now
     * too (harmless if the block ignores them). */
    w32(STAT_LIST_ADDR_LO, (uint32_t)st_dma);
    w32(STAT_LIST_ADDR_HI, (uint32_t)(st_dma >> 32));
    w16(STAT_LAST_IDX, SKY2_STATUS_LES - 1);
    w32(STAT_CTRL, SC_STAT_RST_CLR);

    /* On the 88E8058 the status block ignores register writes for a
     * window after reset release — poll the readback until the list
     * base actually sticks (observed: immediate rb returned 0 and all
     * status DMA then went to PA 0). */
    uint32_t stlo_rb = 0;
    for (int i = 0; i < 100000; i++) {
        w32(STAT_LIST_ADDR_LO, (uint32_t)st_dma);
        w32(STAT_LIST_ADDR_HI, (uint32_t)(st_dma >> 32));
        stlo_rb = r32(STAT_LIST_ADDR_LO);
        if (stlo_rb == (uint32_t)st_dma)
            break;
        __asm__ volatile("pause" ::: "memory");
    }
    klog_puts(KLOG_SKY2, KLOG_DEBUG, "stle_dma="); klog_appendf(KLOG_SKY2, KLOG_DEBUG, "0x%08X",
        (uint32_t)st_dma);
    klog_puts(KLOG_SKY2, KLOG_DEBUG, " rb="); klog_appendf(KLOG_SKY2, KLOG_DEBUG, "0x%08X\n",
        stlo_rb);
    w16(STAT_LAST_IDX, SKY2_STATUS_LES - 1);
    w16(STAT_TX_IDX_TH, 10);
    w8(STAT_FIFO_WM, 16);
    w8(STAT_FIFO_ISR_WM, 16);
    w32(STAT_TX_TIMER_INI, 125u * 1000);   /* 125 MHz chip clock */
    w32(STAT_ISR_TIMER_INI, 125u * 20);
    w32(STAT_LEV_TIMER_INI, 125u * 100);
    w32(STAT_CTRL, SC_STAT_OP_ON);
    /* One more poke post-enable in case the BMU samples the base
     * lazily at OP_ON rather than at reset release. */
    w32(STAT_LIST_ADDR_LO, (uint32_t)st_dma);
    w32(STAT_LIST_ADDR_HI, (uint32_t)(st_dma >> 32));
    w8(STAT_TX_TIMER_CTRL, TIM_START);
    w8(STAT_LEV_TIMER_CTRL, TIM_START);
    w8(STAT_ISR_TIMER_CTRL, TIM_START);
}

/* -------------------------------------------------------------------------
 * IRQ handler
 * ------------------------------------------------------------------------- */
static void sky2_irq_handler(uint64_t vector, uint64_t error_code)
{
    (void)error_code;
    if (!g_regs) return;

    /* Reading ISRC2 clears/masks the IRQ sources */
    uint32_t status = r32(B0_Y2_SP_ISRC2);
    if (status == 0 || status == 0xFFFFFFFFU) {
        w32(B0_Y2_SP_ICR, 2);
        IRQ_EOI((int)vector);
        return;
    }

    if (status & Y2_IS_IRQ_PHY1)
        gm_phy_read(0, PHY_MARV_INT_STAT);   /* ack PHY irq */

    /* Record completions only; frames are delivered to the net stack
     * from task context in sky2_poll(), never from interrupt context. */
    int rx_new = sky2_drain_status();
    r32(B0_Y2_SP_LISR);   /* unmask interrupts */

    /* Armed net consumers were just SIGF_NET-signalled: reschedule now
     * so the isr_common epilogue switches straight into the woken task
     * rather than waiting out the current 100 Hz tick. */
    if (rx_new)
        Task_ScheduleFromIRQ();
    IRQ_EOI((int)vector);
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */
int sky2_init(void)
{
    uint64_t bar0;

    if (!pci_find_sky2(&g_bus, &g_dev, &g_fn, &bar0)) {
        klog_puts(KLOG_SKY2, KLOG_DEBUG, "not found\n");
        return 0;
    }

    g_regs = (volatile uint8_t *)(uintptr_t)bar0;
    klog_puts(KLOG_SKY2, KLOG_DEBUG, "bar0="); klog_appendf(KLOG_SKY2, KLOG_DEBUG, "0x%08X", (uint32_t)bar0);
    klog_puts(KLOG_SKY2, KLOG_DEBUG, " bdf="); klog_appendf(KLOG_SKY2, KLOG_DEBUG, "0x%08X",
        ((uint32_t)g_bus << 16) | ((uint32_t)g_dev << 8) | g_fn);
    klog_puts(KLOG_SKY2, KLOG_DEBUG, "\n");

    /* Enable all clocks + clear software reset (PCI_DEV_REG3 write must
     * go through the MMIO config window to take effect reliably) */
    pcw32(PCI_DEV_REG3, 0);
    w8(B0_CTST, CS_RST_CLR);

    g_chip_id  = r8(B2_CHIP_ID);
    g_chip_rev = (r8(B2_MAC_CFG) & CFG_CHIP_R_MSK) >> 4;
    klog_puts(KLOG_SKY2, KLOG_DEBUG, "chip="); klog_appendf(KLOG_SKY2, KLOG_DEBUG, "0x%08X", g_chip_id);
    klog_puts(KLOG_SKY2, KLOG_DEBUG, " rev="); klog_appendf(KLOG_SKY2, KLOG_DEBUG, "0x%08X", g_chip_rev);
    klog_puts(KLOG_SKY2, KLOG_DEBUG, "\n");

    switch (g_chip_id) {
    case CHIP_ID_YUKON_EC_U:
    case CHIP_ID_YUKON_EC:
    case CHIP_ID_YUKON_EX:
    case CHIP_ID_YUKON_SUPR:
    case CHIP_ID_YUKON_UL_2:
    case CHIP_ID_YUKON_XL:
    case CHIP_ID_YUKON_FE:
    case CHIP_ID_YUKON_FE_P:
        break;
    default:
        klog_puts(KLOG_SKY2, KLOG_ERR, "unsupported chip type\n");
        return 0;
    }

    g_has_ram_buffer = (r8(B2_E_0) != 0);

    /* MAC address */
    const volatile uint8_t *mp = g_regs + B2_MAC_1;
    for (int i = 0; i < 6; i++) g_mac[i] = mp[i];
    klog_puts(KLOG_SKY2, KLOG_DEBUG, "mac=");
    for (int i = 0; i < 6; i++)
        klog_appendf(KLOG_SKY2, KLOG_DEBUG, "%s0x%08X", i ? ":" : "", g_mac[i]);
    klog_puts(KLOG_SKY2, KLOG_DEBUG, "\n");

    /* Allocate descriptor rings + buffers.  The prefetch-unit and
     * status-list base registers drop address bits [11:0] on real
     * silicon (88E8058 read back 0x031CD000 for both the TX ring and the
     * status ring, which were 0x200 apart) — so a sub-4K-aligned ring
     * makes the chip fetch LEs from / write status to the wrong place.
     * FreeBSD msk uses 32 KB alignment (MSK_RING_ALIGN/MSK_STAT_ALIGN);
     * Linux gets page-aligned coherent memory.  Match FreeBSD. */
    g_rx_le  = (Sky2RxLE *)    DMA_Alloc(SKY2_RX_LES * sizeof(Sky2RxLE), SKY2_RING_ALIGN);
    g_tx_le  = (Sky2TxLE *)    DMA_Alloc(SKY2_TX_LES * sizeof(Sky2TxLE), SKY2_RING_ALIGN);
    g_st_le  = (Sky2StatusLE *)DMA_Alloc(SKY2_STATUS_LES * sizeof(Sky2StatusLE), SKY2_RING_ALIGN);
    g_rx_buf = (uint8_t *)     DMA_Alloc(SKY2_RX_BUFS * SKY2_RX_BUFSZ, 64);
    g_tx_buf = (uint8_t *)     DMA_Alloc(SKY2_TX_LES * SKY2_TX_BUFSZ, 64);
    if (!g_rx_le || !g_tx_le || !g_st_le || !g_rx_buf || !g_tx_buf) {
        klog_puts(KLOG_SKY2, KLOG_ERR, "DMA_Alloc failed\n");
        return 0;
    }
    memset(g_rx_le, 0, SKY2_RX_LES * sizeof(Sky2RxLE));
    memset(g_tx_le, 0, SKY2_TX_LES * sizeof(Sky2TxLE));

    sky2_reset();

    /* ---- per-port bring-up (sky2_hw_up, port 0) ---- */
    g_tx_prod = 0;
    g_tx_done = 0;
    /* first TX LE is always the initial ADDR64 */
    g_tx_le[0].addr = 0;
    g_tx_le[0].ctrl = 0;
    g_tx_le[0].opcode = OP_ADDR64 | HW_OWNER;
    g_tx_prod = 1;

    sky2_mac_init(0);

    uint32_t ramsize = r8(B2_E_0) * 4;
    if (ramsize > 0) {
        uint32_t rxspace = (ramsize < 16) ? ramsize / 2
                                          : 8 + (2 * (ramsize - 16)) / 3;
        sky2_ramset(Q_R1, 0, rxspace);
        sky2_ramset(Q_XA1, rxspace, ramsize - rxspace);
        w8(RB_ADDR(Q_XS1, RB_CTRL), RB_RST_SET); /* keep sync queue off */
    }

    sky2_qset(Q_XA1);
    sky2_prefetch_init(Q_XA1, (uint64_t)(uintptr_t)g_tx_le, SKY2_TX_LES - 1);

    /* no VLAN offload */
    w32(SK_REG(0, RX_GMF_CTRL_T), RX_VLAN_STRIP_OFF);
    w32(SK_REG(0, TX_GMF_CTRL_T), TX_VLAN_TAG_OFF);

    sky2_rx_start(0);

    /* enable RX/TX in the GMAC */
    gma_w16(0, GM_GP_CTRL,
            (uint16_t)(gma_r16(0, GM_GP_CTRL) |
                       GM_GPCR_RX_ENA | GM_GPCR_TX_ENA));

    gm_phy_write(0, PHY_MARV_INT_MASK, PHY_M_DEF_MSK);
    w8(SK_REG(0, LNK_LED_REG),
       LINKLED_ON | LINKLED_BLINK_OFF | LINKLED_LINKSYNC_OFF);

    /* Unmask the base IRQ sources now.  Linux never moves traffic with
     * B0_IMSK==0, and on this silicon the status-BMU writeback may be
     * coupled to the IRQ-enable state.  The INTx line isn't routed on
     * the MacBook (IRQ attach fails), so asserting it is harmless; the
     * driver remains in poll mode. */
    w32(B0_IMSK, Y2_IS_BASE | Y2_IS_PORT_1);
    r32(B0_IMSK);

    /* wait for PHY link (autoneg) */
    klog_puts(KLOG_SKY2, KLOG_DEBUG, "waiting for link...\n");
    for (int i = 0; i < 300; i++) {
        uint16_t ps = gm_phy_read(0, PHY_MARV_PHY_STAT);
        if (ps & PHY_M_PS_LINK_UP) break;
        msdelay(10);
    }
    uint16_t ps = gm_phy_read(0, PHY_MARV_PHY_STAT);
    klog_puts(KLOG_SKY2, KLOG_DEBUG, "phy_stat="); klog_appendf(KLOG_SKY2, KLOG_DEBUG, "0x%08X", ps);
    klog_puts(KLOG_SKY2, KLOG_DEBUG, "\n");
    if (!(ps & PHY_M_PS_LINK_UP))
        klog_puts(KLOG_SKY2, KLOG_WARN, "link not up (cable?)\n");

    g_up = 1;
    /* Attach interrupts *now*, not after DHCP: on this silicon the
     * status-BMU writeback is coupled to the IRQ path, and Linux runs
     * traffic with MSI enabled.  Falling back to INTx is fine. */
    sky2_setup_irq();

    /* Clear any descriptor-checker IRQs latched during init (the BMU
     * sees zeroed ring slots with opcode 0 as invalid).  If CHK bits
     * reappear in the timeout dump the checker is still rejecting our
     * LEs; staying clear means they were init noise / stale reads. */
    w32(Q_ADDR(Q_XA1, Q_CSR), BMU_CLR_IRQ_CHK);
    w32(Q_ADDR(Q_R1, Q_CSR), BMU_CLR_IRQ_CHK);
    uint32_t sp0 = r32(B0_Y2_SP_ISRC2);
    w32(B0_Y2_SP_ICR, 2);
    klog_puts(KLOG_SKY2, KLOG_DEBUG, "sp_init="); klog_appendf(KLOG_SKY2, KLOG_DEBUG, "0x%08X\n", sp0);
    klog_puts(KLOG_SKY2, KLOG_DEBUG, "init OK\n");
    return 1;
}

int sky2_is_up(void) { return g_up; }

void sky2_get_mac(uint8_t *buf)
{
    for (int i = 0; i < 6; i++) buf[i] = g_mac[i];
}

/* True once the chip's completed index has reached or moved past
 * `target` (the index just after our LE), modulo the ring. */
static int sky2_tx_reached(uint16_t target)
{
    const uint16_t m = SKY2_TX_LES - 1;
    return ((g_tx_done - target) & m) <= ((g_tx_prod - target) & m);
}

int sky2_send(const uint8_t *data, uint16_t len)
{
    if (!g_up || !g_regs || !data || len > SKY2_MTU) return 0;

    /* Claim an LE and publish it atomically: sends come from several
     * preemptible tasks, so the slot claim + PUT update must not
     * interleave.  The frame is copied into a driver-owned per-LE
     * buffer so the caller may reuse its buffer immediately. */
    uint64_t fl = sky2_irq_save();
    /* Ring full: prod+1 would land on done — overwriting an in-flight
     * LE's bounce buffer mid-DMA.  Drop and let the caller retry; all
     * existing callers (send_buf / remote_send_raw) already loop. */
    if (((g_tx_prod + 1) & (SKY2_TX_LES - 1)) == g_tx_done) {
        sky2_irq_restore(fl);
        return 0;
    }
    uint16_t idx = g_tx_prod;
    uint8_t *txb = g_tx_buf + (uint32_t)idx * SKY2_TX_BUFSZ;
    memcpy(txb, data, len);
    Sky2TxLE *le = &g_tx_le[idx];
    le->ctrl = TX_LE_EOP;   /* single-LE packet: mark end-of-packet */
    le->addr = (uint32_t)(uintptr_t)txb;
    le->length = len;
    le->opcode = OP_PACKET | HW_OWNER;
    g_tx_prod = (g_tx_prod + 1) & (SKY2_TX_LES - 1);
    uint16_t target = g_tx_prod;

    __asm__ volatile("mfence" ::: "memory");
    w16(Y2_QADDR(Q_XA1, PREF_UNIT_PUT_IDX), g_tx_prod);
    sky2_irq_restore(fl);

    /* Synchronous completion: drain status ring (records completions
     * only — never re-enters the net stack) until the TX index report
     * has reached or passed our LE.  "Passed" matters: a concurrent
     * send completes later LEs and the chip reports only the newest
     * index, so an equality test would spin to timeout. */
    uint32_t spin = 0;
    while (!sky2_tx_reached(target) && spin < 2000000) {
        sky2_drain_status();
        __asm__ volatile("pause" ::: "memory");
        spin++;
    }
    if (!sky2_tx_reached(target)) {
        klog_puts(KLOG_SKY2, KLOG_DEBUG, "TX timeout done="); klog_appendf(KLOG_SKY2, KLOG_DEBUG, "0x%08X", g_tx_done);
        klog_puts(KLOG_SKY2, KLOG_DEBUG, " want="); klog_appendf(KLOG_SKY2, KLOG_DEBUG, "0x%08X", target); klog_puts(KLOG_SKY2, KLOG_DEBUG, "\n");
        /* Pipeline stage levels, ordered to fit the fbcon width:
         * get  = LEs the prefetch unit consumed
         * pfl  = prefetch FIFO level (LEs fetched, not eaten by TX-BMU)
         * qwl  = TX FIFO write level (packet bytes DMA'd into chip)
         * qrl  = TX FIFO read level (bytes the MAC drained)
         * gp   = GM_GP_STAT: MAC link state (bit12 = LINK_UP)
         * txbc = bcast frames on wire; stput = status writebacks */
        klog_puts(KLOG_SKY2, KLOG_DEBUG, "  get="); klog_appendf(KLOG_SKY2, KLOG_DEBUG, "0x%08X",
            r16(Y2_QADDR(Q_XA1, PREF_UNIT_GET_IDX)));
        klog_puts(KLOG_SKY2, KLOG_DEBUG, " pfl="); klog_appendf(KLOG_SKY2, KLOG_DEBUG, "0x%08X",
            (uint32_t)r8(Y2_QADDR(Q_XA1, PREF_UNIT_FIFO_LEV)));
        klog_puts(KLOG_SKY2, KLOG_DEBUG, " qwl="); klog_appendf(KLOG_SKY2, KLOG_DEBUG, "0x%08X",
            (uint32_t)r8(Q_ADDR(Q_XA1, Q_WL)));
        klog_puts(KLOG_SKY2, KLOG_DEBUG, " qrl="); klog_appendf(KLOG_SKY2, KLOG_DEBUG, "0x%08X\n",
            (uint32_t)r8(Q_ADDR(Q_XA1, Q_RL)));
        klog_puts(KLOG_SKY2, KLOG_DEBUG, "  gp="); klog_appendf(KLOG_SKY2, KLOG_DEBUG, "0x%08X",
            (uint32_t)gma_r16(0, GM_GP_STAT));
        klog_puts(KLOG_SKY2, KLOG_DEBUG, " txbc="); klog_appendf(KLOG_SKY2, KLOG_DEBUG, "0x%08X",
            (uint32_t)gma_r16(0, GM_TXF_BC_OK));
        klog_puts(KLOG_SKY2, KLOG_DEBUG, " stput="); klog_appendf(KLOG_SKY2, KLOG_DEBUG, "0x%08X",
            r16(STAT_PUT_IDX));
        klog_puts(KLOG_SKY2, KLOG_DEBUG, " hwe="); klog_appendf(KLOG_SKY2, KLOG_DEBUG, "0x%08X",
            r32(B0_HWE_ISRC));
        klog_puts(KLOG_SKY2, KLOG_DEBUG, " aer="); klog_appendf(KLOG_SKY2, KLOG_DEBUG, "0x%08X\n",
            r32(Y2_CFG_AER + PCI_ERR_UNCOR_STATUS));
        /* Full register diff only once per boot — it's ~25 fbcon lines */
        static int dumped = 0;
        if (dumped++) return 0;
        /* Register diff vs the live Linux golden dump captured on this
         * exact machine — prints off:ours/linux for every mismatch,
         * three per line so nothing runs off the fbcon edge. */
        unsigned nd = 0;
        for (unsigned i = 0; i < sizeof(k_linux_regs)/sizeof(k_linux_regs[0]); i++) {
            uint32_t ours = r32(k_linux_regs[i].off);
            if (ours == k_linux_regs[i].val) continue;
            if (!(nd & 3)) klog_puts(KLOG_SKY2, KLOG_DEBUG, "  df");
            klog_appendf(KLOG_SKY2, KLOG_DEBUG, " %04X:%08X/%08X",
                k_linux_regs[i].off, ours, k_linux_regs[i].val);
            if (++nd & 3) {} else klog_puts(KLOG_SKY2, KLOG_DEBUG, "\n");
        }
        if (nd & 3) klog_puts(KLOG_SKY2, KLOG_DEBUG, "\n");
        return 0;
    }
    return 1;
}

void sky2_poll(void)
{
    if (!g_up || !g_regs) return;

    /* PHY link events don't gate RX processing for a poll-mode driver */
    sky2_drain_status();
    sky2_rx_deliver();
}

void sky2_set_rx_callback(sky2_rx_cb cb)
{
    g_rx_cb = cb;
}

void sky2_set_notify_cb(void (*fn)(void))
{
    g_notify_cb = fn;
}

void sky2_setup_irq(void)
{
    static int attached = 0;
    if (!g_up || attached) return;   /* sky2_init and netdev both call us */

    /* Prefer MSI (Yukon-2 is a PCIe device); fall back to INTx. */
    int vec = IRQ_AttachMSI(g_bus, g_dev, g_fn, sky2_irq_handler, "sky2");
    if (vec < 0)
        vec = IRQ_AttachPCI(g_bus, g_dev, g_fn, sky2_irq_handler, "sky2");
    if (vec < 0) {
        klog_puts(KLOG_SKY2, KLOG_WARN, "IRQ attach failed — poll mode\n");
        return;
    }
    attached = 1;

    /* Handler installed: unmask device interrupts (base + port 0). */
    w32(B0_IMSK, Y2_IS_BASE | Y2_IS_PORT_1);
    r32(B0_IMSK);
    klog_puts(KLOG_SKY2, KLOG_DEBUG, "irq vector=");
    klog_appendf(KLOG_SKY2, KLOG_DEBUG, "0x%08X", vec);
    klog_puts(KLOG_SKY2, KLOG_DEBUG, "\n");
}
