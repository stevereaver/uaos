/*
 * e1000.c — UAOS Intel 82540EM (e1000) Gigabit Ethernet Driver
 *
 * Hardware: Intel 82540EM "PRO/1000 MT Desktop"
 *   PCI vendor 0x8086, device 0x100E (primary target)
 *   Also accepts other common 8254x family device IDs (see pci_find_e1000).
 *
 * Register model: BAR0 = 128 KB MMIO space, all registers 32-bit.
 *
 * Design notes:
 *   - Legacy (non-extended) TX/RX descriptors throughout.
 *   - Static ring buffers in BSS (no dynamic allocation).
 *   - Single TX at a time: write descriptor, bump TDT, spin on DD bit.
 *   - RX: driver-side head pointer tracks the next descriptor to check.
 *   - IRQ: reads ICR (self-clearing), calls e1000_poll(), sends EOI.
 *   - Polling: safe to call from both IRQ and main loop contexts.
 */

#include "e1000.h"
#include "../irq/idt.h"    /* IDT_SetHandler, PIC_UnmaskIRQ, PIC_SendEOI */

/* -------------------------------------------------------------------------
 * Serial debug helpers (COM1 = 0x3F8) — freestanding, no printf
 * ------------------------------------------------------------------------- */
static inline void _e_outb(uint16_t p, uint8_t v)
{
    __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p));
}
static inline uint8_t _e_inb(uint16_t p)
{
    uint8_t v;
    __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}
static void _e_putc(char c)
{
    while ((_e_inb(0x3FD) & 0x20) == 0) {}
    _e_outb(0x3F8, (uint8_t)c);
    if (c == '\n') { while ((_e_inb(0x3FD) & 0x20) == 0) {} _e_outb(0x3F8, '\r'); }
}
static void _e_puts(const char *s) { while (*s) _e_putc(*s++); }
static void _e_phex(uint32_t v)
{
    static const char h[] = "0123456789ABCDEF";
    _e_puts("0x");
    for (int i = 28; i >= 0; i -= 4) _e_putc(h[(v >> i) & 0xF]);
}

/* -------------------------------------------------------------------------
 * PCI config-space access (I/O port CF8/CFC)
 * ------------------------------------------------------------------------- */
static inline void _e_outl(uint16_t p, uint32_t v)
{
    __asm__ volatile("outl %0,%1" :: "a"(v), "Nd"(p));
}
static inline uint32_t _e_inl(uint16_t p)
{
    uint32_t v;
    __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}

static uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg)
{
    uint32_t addr = 0x80000000U | ((uint32_t)bus << 16) | ((uint32_t)dev << 11)
                  | ((uint32_t)fn << 8) | (reg & 0xFC);
    _e_outl(0xCF8, addr);
    return _e_inl(0xCFC);
}

static void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint32_t val)
{
    uint32_t addr = 0x80000000U | ((uint32_t)bus << 16) | ((uint32_t)dev << 11)
                  | ((uint32_t)fn << 8) | (reg & 0xFC);
    _e_outl(0xCF8, addr);
    _e_outl(0xCFC, val);
}

static uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg)
{
    return (uint16_t)(pci_read32(bus, dev, fn, reg) >> ((reg & 2) * 8));
}

static void pci_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint16_t val)
{
    uint32_t addr = 0x80000000U | ((uint32_t)bus << 16) | ((uint32_t)dev << 11)
                  | ((uint32_t)fn << 8) | (reg & 0xFC);
    _e_outl(0xCF8, addr);
    uint32_t cur = _e_inl(0xCFC);
    int shift = (reg & 2) * 8;
    cur = (cur & ~(0xFFFFU << shift)) | ((uint32_t)val << shift);
    _e_outl(0xCFC, cur);
}

/* -------------------------------------------------------------------------
 * MMIO helpers — volatile 32-bit read/write to BAR0 address space
 * ------------------------------------------------------------------------- */
static inline uint32_t mmio_r32(uint32_t base, uint32_t off)
{
    return *((volatile uint32_t *)(uintptr_t)(base + off));
}
static inline void mmio_w32(uint32_t base, uint32_t off, uint32_t val)
{
    *((volatile uint32_t *)(uintptr_t)(base + off)) = val;
}

/* -------------------------------------------------------------------------
 * e1000 register offsets (BAR0-relative, bytes)
 * ------------------------------------------------------------------------- */
#define E1000_CTRL      0x0000  /* Device Control */
#define E1000_STATUS    0x0008  /* Device Status */
#define E1000_EECD      0x0010  /* EEPROM/Flash Control */
#define E1000_EERD      0x0014  /* EEPROM Read */
#define E1000_ICR       0x00C0  /* Interrupt Cause Read (clears on read) */
#define E1000_IMS       0x00D0  /* Interrupt Mask Set */
#define E1000_IMC       0x00D8  /* Interrupt Mask Clear */
#define E1000_RCTL      0x0100  /* Receive Control */
#define E1000_TCTL      0x0400  /* Transmit Control */
#define E1000_TIPG      0x0410  /* Transmit IPG */
#define E1000_RDBAL     0x2800  /* RX Desc Base Address Low */
#define E1000_RDBAH     0x2804  /* RX Desc Base Address High */
#define E1000_RDLEN     0x2808  /* RX Descriptor Ring Length */
#define E1000_RDH       0x2810  /* RX Descriptor Head */
#define E1000_RDT       0x2818  /* RX Descriptor Tail */
#define E1000_TDBAL     0x3800  /* TX Desc Base Address Low */
#define E1000_TDBAH     0x3804  /* TX Desc Base Address High */
#define E1000_TDLEN     0x3808  /* TX Descriptor Ring Length */
#define E1000_TDH       0x3810  /* TX Descriptor Head */
#define E1000_TDT       0x3818  /* TX Descriptor Tail */
#define E1000_RAL0      0x5400  /* Receive Address Low  [0] */
#define E1000_RAH0      0x5404  /* Receive Address High [0] */
#define E1000_MTA_BASE  0x5200  /* Multicast Table Array (128 × 32-bit) */

/* CTRL bits */
#define E1000_CTRL_FD       (1u <<  0)   /* Full Duplex */
#define E1000_CTRL_ASDE     (1u <<  3)   /* Auto Speed Detect Enable */
#define E1000_CTRL_SLU      (1u <<  6)   /* Set Link Up */
#define E1000_CTRL_RST      (1u << 26)   /* Device Reset */

/* STATUS bits */
#define E1000_STATUS_LU     (1u <<  1)   /* Link Up */

/* EERD bits */
#define E1000_EERD_START    (1u <<  0)   /* Start read */
#define E1000_EERD_DONE     (1u <<  4)   /* Read done */

/* ICR / IMS bits */
#define E1000_ICR_TXDW      (1u <<  0)   /* TX descriptor written back */
#define E1000_ICR_TXQE      (1u <<  1)   /* TX queue empty */
#define E1000_ICR_LSC       (1u <<  2)   /* Link status change */
#define E1000_ICR_RXDMT0    (1u <<  4)   /* RX descriptor min threshold */
#define E1000_ICR_RXO       (1u <<  6)   /* RX overrun */
#define E1000_ICR_RXT0      (1u <<  7)   /* RX timer interrupt */

/* RCTL bits */
#define E1000_RCTL_EN       (1u <<  1)   /* Receiver Enable */
#define E1000_RCTL_SBP      (1u <<  2)   /* Store Bad Packets */
#define E1000_RCTL_UPE      (1u <<  3)   /* Unicast Promiscuous */
#define E1000_RCTL_MPE      (1u <<  4)   /* Multicast Promiscuous */
#define E1000_RCTL_BAM      (1u << 15)   /* Broadcast Accept Mode */
#define E1000_RCTL_BSIZE_2K (0u << 16)   /* Buffer size 2048 (default) */
#define E1000_RCTL_SECRC    (1u << 26)   /* Strip Ethernet CRC */

/* TCTL bits */
#define E1000_TCTL_EN       (1u <<  1)   /* Transmitter Enable */
#define E1000_TCTL_PSP      (1u <<  3)   /* Pad Short Packets */
#define E1000_TCTL_CT_SHIFT 4            /* Collision Threshold field */
#define E1000_TCTL_COLD_SHIFT 12         /* Collision Distance field */

/* TX descriptor CMD byte bits */
#define E1000_TXD_CMD_EOP   0x01   /* End of Packet */
#define E1000_TXD_CMD_IFCS  0x02   /* Insert FCS/CRC */
#define E1000_TXD_CMD_RS    0x08   /* Report Status (set DD when done) */

/* TX/RX descriptor status: DD = Descriptor Done */
#define E1000_DESC_DD       0x01

/* RAH validity bit */
#define E1000_RAH_AV        (1u << 31)

/* -------------------------------------------------------------------------
 * Descriptor structures (legacy format, 16 bytes each)
 * ------------------------------------------------------------------------- */

/* Legacy receive descriptor */
typedef struct __attribute__((packed)) {
    uint64_t addr;      /* Physical address of receive buffer */
    uint16_t length;    /* Bytes written by hardware */
    uint16_t checksum;
    uint8_t  status;    /* Bit 0 = DD, Bit 1 = EOP */
    uint8_t  errors;
    uint16_t special;
} E1000RxDesc;

/* Legacy transmit descriptor */
typedef struct __attribute__((packed)) {
    uint64_t addr;      /* Physical address of transmit buffer */
    uint16_t length;    /* Frame length */
    uint8_t  cso;       /* Checksum offset (unused) */
    uint8_t  cmd;       /* EOP | IFCS | RS */
    uint8_t  sta;       /* Bit 0 = DD when done */
    uint8_t  css;       /* Checksum start (unused) */
    uint16_t special;
} E1000TxDesc;

/* -------------------------------------------------------------------------
 * Static ring + buffer storage
 * BSS — no dynamic allocation.  Descriptors must be 16-byte aligned;
 * the aligned(16) attribute ensures this.
 * ------------------------------------------------------------------------- */

static E1000RxDesc g_rx_desc[E1000_NUM_RX_DESC] __attribute__((aligned(16)));
static E1000TxDesc g_tx_desc[E1000_NUM_TX_DESC] __attribute__((aligned(16)));

/* One RX buffer per descriptor */
static uint8_t g_rx_buf[E1000_NUM_RX_DESC][E1000_RX_BUFSZ] __attribute__((aligned(16)));

/* Single TX bounce buffer (one packet in flight at a time) */
static uint8_t g_tx_buf[E1000_MTU + 64] __attribute__((aligned(16)));

/* -------------------------------------------------------------------------
 * Driver state
 * ------------------------------------------------------------------------- */
static uint32_t g_bar0    = 0;   /* BAR0 base physical address (identity-mapped) */
static uint8_t  g_mac[E1000_ETH_ALEN];
static int      g_up      = 0;
static uint8_t  g_irq     = 0;
static uint16_t g_rx_tail = 0;   /* next descriptor to check for DD */
static volatile uint8_t g_poll_lock = 0;

static e1000_rx_cb g_rx_cb = 0;

/* -------------------------------------------------------------------------
 * Delay helpers
 * ------------------------------------------------------------------------- */
static void io_delay(void)
{
    __asm__ volatile("inb $0x80, %%al" ::: "eax");
}

static void msdelay(uint32_t ms)
{
    /* ~1 ms per 1 000 000 iterations on a ~1 GHz machine — rough but sufficient
     * for reset / link-up waits where precise timing is not critical. */
    for (uint32_t m = 0; m < ms; m++)
        for (volatile uint32_t i = 0; i < 100000; i++) io_delay();
}

/* -------------------------------------------------------------------------
 * PCI scan — find the first supported e1000 device
 * Supported device IDs (all Intel, vendor 0x8086):
 *   0x100E  82540EM Desktop   ← primary VirtualBox / QEMU target
 *   0x100F  82545EM Desktop
 *   0x1011  82545EM Fiber
 *   0x1026  82545GM Desktop
 *   0x1027  82545GM Mobile
 *   0x1028  82545GM Mobile (variant)
 * ------------------------------------------------------------------------- */
static const uint16_t k_e1000_devids[] = {
    0x100E, 0x100F, 0x1011, 0x1026, 0x1027, 0x1028, 0
};

static int pci_find_e1000(uint8_t *bus_out, uint8_t *dev_out,
                           uint8_t *fn_out,  uint8_t *irq_out,
                           uint32_t *bar0_out)
{
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t fn = 0; fn < 8; fn++) {
                uint32_t id = pci_read32((uint8_t)bus, dev, fn, 0x00);
                if (id == 0xFFFFFFFF) { if (fn == 0) goto next_dev; continue; }
                uint16_t vendor = (uint16_t)(id & 0xFFFF);
                uint16_t device = (uint16_t)(id >> 16);
                if (vendor != 0x8086) continue;
                int match = 0;
                for (int k = 0; k_e1000_devids[k]; k++)
                    if (device == k_e1000_devids[k]) { match = 1; break; }
                if (!match) continue;

                _e_puts("[E1000] found dev="); _e_phex(device);
                _e_puts(" bus="); _e_phex(bus);
                _e_puts(" slot="); _e_phex(dev); _e_puts("\n");

                /* BAR0 — must be a memory BAR (bit 0 = 0) */
                uint32_t bar0 = pci_read32((uint8_t)bus, dev, fn, 0x10);
                _e_puts("[E1000] bar0="); _e_phex(bar0); _e_puts("\n");
                if (bar0 & 1) {
                    _e_puts("[E1000] BAR0 is I/O, skipping\n");
                    continue;
                }
                uint32_t bar0_base = bar0 & 0xFFFFFFF0U;

                /* IRQ line */
                uint8_t irq = (uint8_t)(pci_read32((uint8_t)bus, dev, fn, 0x3C) & 0xFF);

                /* Enable bus-master + memory space */
                uint16_t cmd = pci_read16((uint8_t)bus, dev, fn, 0x04);
                pci_write16((uint8_t)bus, dev, fn, 0x04,
                            (uint16_t)(cmd | 0x06)); /* Memory Space | Bus Master */

                *bus_out  = (uint8_t)bus;
                *dev_out  = dev;
                *fn_out   = fn;
                *irq_out  = irq;
                *bar0_out = bar0_base;
                return 1;
            }
            next_dev:;
        }
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * EEPROM word read via EERD register
 * The 82540EM EERD format (small EEPROM interface):
 *   bits  7:0  = word address
 *   bit   8    = START
 *   bit   4    = DONE (read-back)
 *   bits 31:16 = data
 * ------------------------------------------------------------------------- */
static uint16_t eeprom_read(uint16_t word)
{
    mmio_w32(g_bar0, E1000_EERD, ((uint32_t)word << 8) | E1000_EERD_START);
    /* Poll DONE — timeout after ~10 ms */
    uint32_t val = 0;
    for (uint32_t i = 0; i < 100000; i++) {
        val = mmio_r32(g_bar0, E1000_EERD);
        if (val & E1000_EERD_DONE) break;
        io_delay();
    }
    return (uint16_t)(val >> 16);
}

/* Read MAC from EEPROM words 0, 1, 2 */
static void read_mac_from_eeprom(void)
{
    uint16_t w0 = eeprom_read(0);
    uint16_t w1 = eeprom_read(1);
    uint16_t w2 = eeprom_read(2);
    g_mac[0] = (uint8_t)(w0 & 0xFF);
    g_mac[1] = (uint8_t)(w0 >> 8);
    g_mac[2] = (uint8_t)(w1 & 0xFF);
    g_mac[3] = (uint8_t)(w1 >> 8);
    g_mac[4] = (uint8_t)(w2 & 0xFF);
    g_mac[5] = (uint8_t)(w2 >> 8);
    _e_puts("[E1000] MAC=");
    for (int i = 0; i < 6; i++) {
        static const char h[] = "0123456789ABCDEF";
        _e_putc(h[g_mac[i] >> 4]); _e_putc(h[g_mac[i] & 0xF]);
        if (i < 5) _e_putc(':');
    }
    _e_putc('\n');
}

/* -------------------------------------------------------------------------
 * RX ring initialisation
 * ------------------------------------------------------------------------- */
static void rx_init(void)
{
    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        g_rx_desc[i].addr   = (uint64_t)(uintptr_t)g_rx_buf[i];
        g_rx_desc[i].status = 0;
    }

    uint32_t base = (uint32_t)(uintptr_t)g_rx_desc;
    mmio_w32(g_bar0, E1000_RDBAL, base);
    mmio_w32(g_bar0, E1000_RDBAH, 0);
    mmio_w32(g_bar0, E1000_RDLEN, (uint32_t)E1000_NUM_RX_DESC * 16);
    mmio_w32(g_bar0, E1000_RDH, 0);
    /* Set RDT to last descriptor so hardware owns all slots */
    mmio_w32(g_bar0, E1000_RDT, (uint32_t)(E1000_NUM_RX_DESC - 1));
    g_rx_tail = 0;

    _e_puts("[E1000] rx_init rdbal="); _e_phex(base);
    _e_puts(" rdlen="); _e_phex(E1000_NUM_RX_DESC * 16); _e_puts("\n");
}

/* -------------------------------------------------------------------------
 * TX ring initialisation
 * ------------------------------------------------------------------------- */
static void tx_init(void)
{
    for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
        g_tx_desc[i].addr = 0;
        g_tx_desc[i].sta  = E1000_DESC_DD; /* mark all as done initially */
    }

    uint32_t base = (uint32_t)(uintptr_t)g_tx_desc;
    mmio_w32(g_bar0, E1000_TDBAL, base);
    mmio_w32(g_bar0, E1000_TDBAH, 0);
    mmio_w32(g_bar0, E1000_TDLEN, (uint32_t)E1000_NUM_TX_DESC * 16);
    mmio_w32(g_bar0, E1000_TDH, 0);
    mmio_w32(g_bar0, E1000_TDT, 0);

    _e_puts("[E1000] tx_init tdbal="); _e_phex(base);
    _e_puts(" tdlen="); _e_phex(E1000_NUM_TX_DESC * 16); _e_puts("\n");
}

/* -------------------------------------------------------------------------
 * IRQ handler
 * ------------------------------------------------------------------------- */
static void e1000_irq_handler(uint64_t vector, uint64_t error_code)
{
    (void)vector; (void)error_code;
    if (!g_bar0) return;
    uint32_t icr = mmio_r32(g_bar0, E1000_ICR); /* read clears */
    if (icr & (E1000_ICR_RXT0 | E1000_ICR_RXDMT0 | E1000_ICR_RXO))
        e1000_poll();
    PIC_SendEOI((int)g_irq);
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

int e1000_init(void)
{
    uint8_t  bus, dev, fn, irq;
    uint32_t bar0;

    if (!pci_find_e1000(&bus, &dev, &fn, &irq, &bar0)) {
        _e_puts("[E1000] not found\n");
        return 0;
    }

    g_bar0 = bar0;
    g_irq  = irq;

    _e_puts("[E1000] bar0_base="); _e_phex(bar0);
    _e_puts(" irq="); _e_phex(irq); _e_puts("\n");

    /* 1. Disable all interrupts */
    mmio_w32(g_bar0, E1000_IMC, 0xFFFFFFFFU);

    /* 2. Global software reset */
    uint32_t ctrl = mmio_r32(g_bar0, E1000_CTRL);
    mmio_w32(g_bar0, E1000_CTRL, ctrl | E1000_CTRL_RST);
    msdelay(10);   /* wait for reset to complete */

    /* 3. Disable interrupts again (reset may have re-enabled some) */
    mmio_w32(g_bar0, E1000_IMC, 0xFFFFFFFFU);

    /* 4. Set link up, auto-speed detect */
    ctrl = mmio_r32(g_bar0, E1000_CTRL);
    ctrl &= ~(E1000_CTRL_RST);
    ctrl |=  E1000_CTRL_SLU | E1000_CTRL_ASDE | E1000_CTRL_FD;
    mmio_w32(g_bar0, E1000_CTRL, ctrl);

    /* 5. Read MAC address from EEPROM */
    read_mac_from_eeprom();

    /* 6. Program MAC address into receive address filter RAL0/RAH0 */
    uint32_t ral = (uint32_t)g_mac[0]
                 | ((uint32_t)g_mac[1] <<  8)
                 | ((uint32_t)g_mac[2] << 16)
                 | ((uint32_t)g_mac[3] << 24);
    uint32_t rah = (uint32_t)g_mac[4]
                 | ((uint32_t)g_mac[5] << 8)
                 | E1000_RAH_AV;
    mmio_w32(g_bar0, E1000_RAL0, ral);
    mmio_w32(g_bar0, E1000_RAH0, rah);

    /* 7. Clear multicast table array */
    for (int i = 0; i < 128; i++)
        mmio_w32(g_bar0, E1000_MTA_BASE + (uint32_t)(i * 4), 0);

    /* 8. Initialise RX and TX rings */
    rx_init();
    tx_init();

    /* 9. Configure receive control:
     *    EN | BAM (accept broadcast) | SECRC (strip CRC) | 2KB buffers */
    mmio_w32(g_bar0, E1000_RCTL,
             E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_SECRC | E1000_RCTL_BSIZE_2K);

    /* 10. Configure transmit control:
     *     EN | PSP | CT=0x10 (16 collisions) | COLD=0x200 (512, full-duplex) */
    uint32_t tctl = E1000_TCTL_EN | E1000_TCTL_PSP
                  | (0x10u << E1000_TCTL_CT_SHIFT)
                  | (0x40u << E1000_TCTL_COLD_SHIFT);
    mmio_w32(g_bar0, E1000_TCTL, tctl);

    /* 11. Transmit IPG: standard IEEE 802.3 values */
    mmio_w32(g_bar0, E1000_TIPG, 0x00702008U);

    /* 12. Enable interrupts: RX (RXT0, RXDMT0, RXO) + link change */
    mmio_w32(g_bar0, E1000_IMS,
             E1000_ICR_RXT0 | E1000_ICR_RXDMT0 | E1000_ICR_RXO | E1000_ICR_LSC);

    /* 13. Wait for link — poll STATUS.LU for up to ~2 s */
    _e_puts("[E1000] waiting for link...\n");
    for (int i = 0; i < 200; i++) {
        if (mmio_r32(g_bar0, E1000_STATUS) & E1000_STATUS_LU) break;
        msdelay(10);
    }
    uint32_t status = mmio_r32(g_bar0, E1000_STATUS);
    _e_puts("[E1000] STATUS="); _e_phex(status); _e_puts("\n");
    if (!(status & E1000_STATUS_LU))
        _e_puts("[E1000] WARNING: link not up after reset\n");

    g_up = 1;
    _e_puts("[E1000] init OK\n");
    return 1;
}

int e1000_is_up(void) { return g_up; }

void e1000_get_mac(uint8_t *buf)
{
    for (int i = 0; i < E1000_ETH_ALEN; i++) buf[i] = g_mac[i];
}

int e1000_send(const uint8_t *data, uint16_t len)
{
    if (!g_up || !g_bar0) return 0;
    if (len > E1000_MTU) return 0;

    /* Always use descriptor slot 0 — simple single-packet model */
    /* Wait for any previous TX to complete (DD bit) */
    uint32_t spin = 0;
    while (!(g_tx_desc[0].sta & E1000_DESC_DD)) {
        __asm__ volatile("pause" ::: "memory");
        if (++spin > 500000) {
            _e_puts("[E1000] TX timeout\n");
            break;
        }
    }

    /* Copy frame into bounce buffer */
    for (uint16_t i = 0; i < len; i++) g_tx_buf[i] = data[i];

    /* Fill descriptor 0 */
    g_tx_desc[0].addr   = (uint64_t)(uintptr_t)g_tx_buf;
    g_tx_desc[0].length = len;
    g_tx_desc[0].cso    = 0;
    g_tx_desc[0].cmd    = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
    g_tx_desc[0].sta    = 0;   /* clear DD — hardware will set it */
    g_tx_desc[0].css    = 0;
    g_tx_desc[0].special = 0;

    __asm__ volatile("mfence" ::: "memory");

    /* Reset head to 0, then set tail to 1 — kicks transmission */
    mmio_w32(g_bar0, E1000_TDH, 0);
    mmio_w32(g_bar0, E1000_TDT, 1);

    _e_puts("[E1000] TX len="); _e_phex(len); _e_puts("\n");
    return 1;
}

void e1000_poll(void)
{
    if (!g_up || !g_bar0) return;

    /* Reentrancy guard */
    uint8_t locked = __sync_lock_test_and_set(&g_poll_lock, 1);
    if (locked) return;

    /* Walk RX ring from g_rx_tail, process all descriptors with DD set */
    for (int checked = 0; checked < E1000_NUM_RX_DESC; checked++) {
        E1000RxDesc *d = &g_rx_desc[g_rx_tail];
        if (!(d->status & E1000_DESC_DD)) break;

        uint16_t pkt_len = d->length;
        _e_puts("[E1000] RX len="); _e_phex(pkt_len); _e_puts("\n");

        if (pkt_len > 0 && pkt_len <= E1000_RX_BUFSZ && g_rx_cb) {
            g_rx_cb(g_rx_buf[g_rx_tail], pkt_len);
        }

        /* Hand buffer back to hardware: clear status, advance RDT */
        d->status = 0;
        uint16_t old_tail = g_rx_tail;
        g_rx_tail = (uint16_t)((g_rx_tail + 1) % E1000_NUM_RX_DESC);
        mmio_w32(g_bar0, E1000_RDT, old_tail);
    }

    __sync_lock_release(&g_poll_lock);
}

void e1000_set_rx_callback(e1000_rx_cb cb)
{
    g_rx_cb = cb;
}

void e1000_setup_irq(void)
{
    if (!g_up) return;
    _e_puts("[E1000] setup_irq line="); _e_phex(g_irq); _e_puts("\n");
    IDT_SetHandler((uint8_t)(32 + g_irq), e1000_irq_handler);
    PIC_UnmaskIRQ((int)g_irq);
}
