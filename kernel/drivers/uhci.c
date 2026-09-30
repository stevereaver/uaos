/* uhci.c — UAOS UHCI (USB 1.1) Host Controller Driver
 *
 * Drives Intel UHCI-class controllers (ICH8 has 5 on the MacBookPro4,1;
 * QEMU provides piix3-usb-uhci).  BAR4 is a 32-byte I/O register space.
 *
 * Schedule model (single anchor queue):
 *   frame list[1024] ──> qh_anchor ──link──> per-transfer QHs ──> T
 *     - Each logical transfer (control message, interrupt-in pipe) is a
 *       QH whose element pointer leads a vertical TD chain.
 *     - Control QHs are inserted at the head and removed when done.
 *     - Interrupt-in QHs persist; their single TD is re-armed on
 *       completion so the HC re-polls the endpoint every 1 ms frame.
 *
 * Completion is found either by the INTx IRQ (IOC bit on TDs) or by
 * UHCI_Poll() from the PIT tick — TD status lives in RAM so polling is
 * a plain memory read.
 */

#include "usb.h"
#include "../irq/irq.h"
#include "../klog/klog.h"
#include "../dos/dma.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* I/O registers (offsets from BAR4)                                   */
/* ------------------------------------------------------------------ */
#define U_USBCMD     0x00    /* 16 */
#define U_USBSTS     0x02    /* 16 */
#define U_USBINTR    0x04    /* 16 */
#define U_FRNUM      0x06    /* 16 */
#define U_FRBASE     0x08    /* 32 */
#define U_SOFMOD     0x0C    /* 8  */
#define U_PORTSC1    0x10    /* 16 */
#define U_PORTSC2    0x12    /* 16 */

#define CMD_RS       0x0001  /* run/stop */
#define CMD_HCRESET  0x0002
#define CMD_GRESET   0x0004
#define CMD_MAXP     0x0080  /* allow 64-byte packets/frame */
#define CMD_CF       0x0040  /* configure flag */

#define STS_USBINT   0x0001
#define STS_ERROR    0x0002
#define STS_RD       0x0004  /* resume detect */
#define STS_HSE      0x0008  /* host system error */
#define STS_HCPE     0x0010  /* host controller process error */
#define STS_HCH      0x0020  /* halted */

#define INTR_IOC     0x0004  /* interrupt on complete */
#define INTR_SPI     0x0008  /* short-packet interrupt */
#define INTR_TOCRC   0x0001  /* timeout/CRC */
#define INTR_RESUME  0x0002

#define PSC_CCS      0x0001  /* current connect status */
#define PSC_CSC      0x0002  /* connect status change (W1C) */
#define PSC_PE       0x0004  /* port enable */
#define PSC_PESC     0x0008  /* port enable/status change (W1C) */
#define PSC_LS_MASK  0x0030  /* line status */
#define PSC_RD       0x0040  /* resume detect */
#define PSC_LSDA     0x0100  /* low-speed device attached */
#define PSC_PR       0x0200  /* port reset */
#define PSC_SUSP     0x1000

#define PSC_W1C      (PSC_CSC | PSC_PESC)  /* never write garbage here */

/* ------------------------------------------------------------------ */
/* TD / QH structures (must live in DMA memory, 16-byte aligned)        */
/* ------------------------------------------------------------------ */

#define TD_LINK_T   0x01
#define TD_LINK_Q   0x02
#define TD_LINK_VF  0x04

/* status dword */
#define TD_ST_ACTIVE   (1u << 23)
#define TD_ST_STALLED  (1u << 22)
#define TD_ST_DBERR    (1u << 21)
#define TD_ST_BABBLE   (1u << 20)
#define TD_ST_NAK      (1u << 19)
#define TD_ST_CRC      (1u << 18)
#define TD_ST_BITSTUFF (1u << 17)
#define TD_ST_IOC      (1u << 24)
#define TD_ST_LS       (1u << 26)
#define TD_ST_SPD      (1u << 29)
#define TD_ST_CERR(x)  (((x) & 3u) << 27)
#define TD_ST_ERRMSK   (TD_ST_STALLED|TD_ST_DBERR|TD_ST_BABBLE|TD_ST_CRC|TD_ST_BITSTUFF)
#define TD_ST_ACTLEN(s) ((s) & 0x7FF)

/* token dword */
#define TD_PID_SETUP   0x2D
#define TD_PID_IN      0x69
#define TD_PID_OUT     0xE1
#define TD_TOK_DEV(x)  (((x) & 0x7Fu) << 8)
#define TD_TOK_EP(x)   (((x) & 0xFu)  << 15)
#define TD_TOK_D(x)    (((x) & 1u)    << 19)
#define TD_TOK_LEN(x)  ((((x) ? (uint32_t)((x) - 1) : 0x7FFu) & 0x7FFu) << 21)

typedef struct __attribute__((packed, aligned(16))) {
    uint32_t link;
    volatile uint32_t status; /* HC writes back status — must be volatile */
    uint32_t token;
    uint32_t buffer;
    uint32_t pad[4];          /* 32 bytes */
} UhciTD;

typedef struct __attribute__((packed, aligned(16))) {
    uint32_t link;            /* horizontal: next QH (Q=1) or T */
    uint32_t element;         /* vertical: first TD (Q=0) */
    uint32_t pad[2];          /* 16 bytes */
} UhciQH;

/* ------------------------------------------------------------------ */
/* HC state                                                            */
/* ------------------------------------------------------------------ */

#define UHCI_MAX_INTR   8     /* armed interrupt-in pipes per HC */

typedef struct {
    UhciTD   *td;             /* TD array, ntd entries — reports >mps span
                             * several packets in one vertical chain */
    UhciQH   *qh;             /* persistent QH — HC overwrites its element
                             * pointer on completion; must be repointed at
                             * td[0] when re-arming */
    int       ntd;
    uint16_t  mps;
    int       toggle;         /* next DATA toggle for td[0] */
    void      (*cb)(void *ctx, void *buf, int len);
    void     *ctx;
} UhciIntr;

typedef struct {
    UsbHc         pub;        /* public vtable — MUST be first member so
                               * (UhciHc *)pub casts line up */
    uint16_t      io;
    uint8_t       bus, dev, fn;
    volatile uint32_t *fl;    /* 1024 frame pointers, 4K aligned */
    UhciQH     *anchor;       /* all FL entries point here */
    UhciQH     *chain_head;   /* first transfer QH (anchor.link target) */
    UhciIntr    intr[UHCI_MAX_INTR];
    int         irq_vec;
} UhciHc;

#define MAX_UHCI 6
static UhciHc g_hc[MAX_UHCI];
static int    g_nhc = 0;

/* ------------------------------------------------------------------ */
/* I/O helpers                                                         */
/* ------------------------------------------------------------------ */
static inline void uoutb(uint16_t p, uint8_t v)
    { __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p)); }
static inline void uoutw(uint16_t p, uint16_t v)
    { __asm__ volatile("outw %0,%1" :: "a"(v), "Nd"(p)); }
static inline void uoutl(uint16_t p, uint32_t v)
    { __asm__ volatile("outl %0,%1" :: "a"(v), "Nd"(p)); }
static inline uint8_t uinb(uint16_t p)
    { uint8_t v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline uint16_t uinw(uint16_t p)
    { uint16_t v; __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(p)); return v; }

static inline void w16(UhciHc *h, uint16_t r, uint16_t v) { uoutw(h->io + r, v); }
static inline void w32(UhciHc *h, uint16_t r, uint32_t v) { uoutl(h->io + r, v); }
static inline void w8 (UhciHc *h, uint16_t r, uint8_t  v) { uoutb(h->io + r, v); }
static inline uint16_t rg16(UhciHc *h, uint16_t r) { return uinw(h->io + r); }

static void udelay(uint32_t us)
{
    for (uint32_t i = 0; i < us; i++)
        for (volatile int j = 0; j < 8; j++)
            __asm__ volatile("inb $0x80, %%al" ::: "eax");
}
static void msleep(uint32_t ms) { udelay(ms * 1000); }

/* ------------------------------------------------------------------ */
/* PCI config (probe time only)                                        */
/* ------------------------------------------------------------------ */
static uint32_t pci_r32(uint8_t b, uint8_t d, uint8_t f, uint8_t off)
{
    uoutl(0xCF8, 0x80000000u | ((uint32_t)b << 16) | ((uint32_t)d << 11)
                | ((uint32_t)f << 8) | (off & 0xFC));
    return (uint32_t)uinw(0xCFC) | ((uint32_t)uinw(0xCFE) << 16);
}
static void pci_w32(uint8_t b, uint8_t d, uint8_t f, uint8_t off, uint32_t v)
{
    uoutl(0xCF8, 0x80000000u | ((uint32_t)b << 16) | ((uint32_t)d << 11)
                | ((uint32_t)f << 8) | (off & 0xFC));
    uoutl(0xCFC, v);
}
static void pci_w16(uint8_t b, uint8_t d, uint8_t f, uint8_t off, uint16_t v)
{
    uint32_t cur = pci_r32(b, d, f, off);
    uint32_t sh = (off & 2) * 8;
    cur = (cur & ~(0xFFFFu << sh)) | ((uint32_t)v << sh);
    pci_w32(b, d, f, off, cur);
}
static uint16_t pci_r16(uint8_t b, uint8_t d, uint8_t f, uint8_t off)
{
    return (uint16_t)(pci_r32(b, d, f, off) >> ((off & 2) * 8));
}

/* ------------------------------------------------------------------ */
/* DMA helpers — TDs/QHs come from a small bump arena                  */
/* ------------------------------------------------------------------ */
static void *td_alloc(void)
{
    return DMA_Alloc(sizeof(UhciTD), 16);
}
static UhciQH *qh_alloc(void)
{
    UhciQH *qh = (UhciQH *)DMA_Alloc(sizeof(UhciQH), 16);
    return qh;
}

/* ------------------------------------------------------------------ */
/* Queue manipulation                                                  */
/* ------------------------------------------------------------------ */

/* Insert a QH right after the anchor (head of transfer chain). */
static void qh_insert_head(UhciHc *h, UhciQH *qh)
{
    qh->link = h->anchor->link;      /* inherit old head / T */
    __asm__ volatile("mfence" ::: "memory");
    h->anchor->link = ((uint32_t)(uintptr_t)qh) | TD_LINK_Q;
}

/* Remove a QH — linear walk of the chain starting at anchor. */
static void qh_remove(UhciHc *h, UhciQH *qh)
{
    volatile uint32_t *prev = &h->anchor->link;
    UhciQH *cur = (UhciQH *)(uintptr_t)(*prev & ~0xFu);
    while (cur) {
        if (cur == qh) {
            *prev = cur->link;
            __asm__ volatile("mfence" ::: "memory");
            return;
        }
        prev = &cur->link;
        uint32_t l = cur->link;
        cur = (l & TD_LINK_T) ? 0 : (UhciQH *)(uintptr_t)(l & ~0xFu);
    }
}

static void td_init(UhciTD *td, uint8_t pid, uint8_t dev, uint8_t ep,
                    int toggle, void *buf, uint16_t len, int low_speed)
{
    memset(td, 0, sizeof(*td));
    td->link   = TD_LINK_T;
    td->status = TD_ST_ACTIVE | TD_ST_IOC | TD_ST_CERR(3) |
                 (low_speed ? TD_ST_LS : 0);
    td->token  = pid | TD_TOK_DEV(dev) | TD_TOK_EP(ep) |
                 TD_TOK_D(toggle) | TD_TOK_LEN(len);
    td->buffer = (uint32_t)(uintptr_t)buf;
}

/* ------------------------------------------------------------------ */
/* Synchronous control transfer                                        */
/* ------------------------------------------------------------------ */
static int uhci_control(UsbHc *pub, UsbDev *dev, uint8_t ep,
                        uint8_t bmRT, uint8_t bReq,
                        uint16_t wVal, uint16_t wIdx,
                        void *data, uint16_t len)
{
    UhciHc *h = (UhciHc *)pub;
    int ls = (dev->speed == USB_SPEED_LOW);
    uint16_t mps = dev->ep0_mps ? dev->ep0_mps : 8;
    int data_in = (bmRT & USB_RT_IN) != 0;

    /* Setup packet — 8 bytes in DMA memory */
    uint8_t *setup = (uint8_t *)DMA_Alloc(8, 8);
    if (!setup) return -1;
    setup[0] = bmRT;
    setup[1] = bReq;
    setup[2] = (uint8_t)wVal;
    setup[3] = (uint8_t)(wVal >> 8);
    setup[4] = (uint8_t)wIdx;
    setup[5] = (uint8_t)(wIdx >> 8);
    setup[6] = (uint8_t)len;
    setup[7] = (uint8_t)(len >> 8);

    int ndata = (len + mps - 1) / mps;
    int ntd   = 1 + ndata + 1;                 /* setup + data + status */

    UhciQH *qh = qh_alloc();
    UhciTD *tds = (UhciTD *)DMA_Alloc(ntd * sizeof(UhciTD), 16);
    if (!qh || !tds) return -1;

    /* setup stage — DATA0 */
    td_init(&tds[0], TD_PID_SETUP, dev->addr, ep, 0, setup, 8, ls);

    /* data stage — toggling starting at DATA1 */
    for (int i = 0; i < ndata; i++) {
        uint16_t chunk = (uint16_t)(len - i * mps > mps ? mps : len - i * mps);
        td_init(&tds[1 + i],
                data_in ? TD_PID_IN : TD_PID_OUT,
                dev->addr, ep, (i & 1) ? 0 : 1,
                (uint8_t *)data + i * mps, chunk, ls);
    }

    /* status stage — opposite direction, DATA1, zero length */
    td_init(&tds[ntd - 1], data_in ? TD_PID_OUT : TD_PID_IN,
            dev->addr, ep, 1, 0, 0, ls);

    /* stitch the chain: setup->first data (or status), linear;
     * VF (depth-first) on intermediate links so the HC executes the
     * whole chain in one visit rather than one TD per frame */
    for (int i = 0; i < ntd - 1; i++)
        tds[i].link = (uint32_t)(uintptr_t)&tds[i + 1] | TD_LINK_VF;
    tds[ntd - 1].link = TD_LINK_T;

    qh->element = (uint32_t)(uintptr_t)&tds[0];
    qh->link    = TD_LINK_T;
    qh_insert_head(h, qh);

    /* wait for the last TD to go inactive */
    int err = -1;
    for (int ms = 0; ms < 500; ms++) {
        uint32_t st = tds[ntd - 1].status;
        if (!(st & TD_ST_ACTIVE)) { err = 0; break; }
        /* also bail on hard error in an earlier TD */
        if (tds[ntd - 1].status & TD_ST_ERRMSK) { err = -1; break; }
        msleep(1);
    }

    /* check every TD for errors */
    for (int i = 0; i < ntd && err == 0; i++) {
        if (tds[i].status & TD_ST_ERRMSK) {
            err = -1;
        }
    }

    if (err != 0) {
        klog_puts(KLOG_USB, KLOG_WARN, "uhci: ctrl sts=");
        klog_appendf(KLOG_USB, KLOG_WARN, "0x%08X", rg16(h, U_USBSTS));
        klog_puts(KLOG_USB, KLOG_WARN, " frnum=");
        klog_appendf(KLOG_USB, KLOG_WARN, "0x%08X", rg16(h, U_FRNUM));
        for (int i = 0; i < ntd; i++) {
            klog_puts(KLOG_USB, KLOG_WARN, " td");
            klog_appendf(KLOG_USB, KLOG_WARN, "0x%08X", i);
            klog_puts(KLOG_USB, KLOG_WARN, "=");
            klog_appendf(KLOG_USB, KLOG_WARN, "0x%08X", tds[i].status);
        }
        klog_puts(KLOG_USB, KLOG_WARN, "\n");
    }

    qh_remove(h, qh);
    return err;
}

/* ------------------------------------------------------------------ */
/* Persistent interrupt-IN pipe                                        */
/* ------------------------------------------------------------------ */
static int uhci_intr_in(UsbHc *pub, UsbDev *dev, uint8_t ep,
                        uint16_t mps, void *buf, uint16_t buflen,
                        void (*cb)(void *ctx, void *buf, int len),
                        void *ctx)
{
    UhciHc *h = (UhciHc *)pub;
    if (!mps) mps = 8;
    if (!buflen) buflen = mps;
    int ntd = (buflen + mps - 1) / mps;
    int ls = (dev->speed == USB_SPEED_LOW);

    for (int i = 0; i < UHCI_MAX_INTR; i++) {
        if (h->intr[i].td) continue;
        UhciQH *qh = qh_alloc();
        UhciTD *tds = (UhciTD *)DMA_Alloc(ntd * sizeof(UhciTD), 16);
        if (!qh || !tds) return -1;

        /* Chain of ntd IN transactions, VF-linked so the HC runs the
         * whole report in one visit; SPD lets a short packet end it
         * early; IOC on the last TD only.  Interrupt pipes start at
         * DATA0 (toggle reset on SET_CONFIGURATION). */
        for (int t = 0; t < ntd; t++) {
            uint16_t chunk = (uint16_t)((buflen - t * mps > mps)
                                        ? mps : buflen - t * mps);
            td_init(&tds[t], TD_PID_IN, dev->addr, ep, t & 1,
                    (uint8_t *)buf + t * mps, chunk, ls);
            tds[t].status = TD_ST_ACTIVE | TD_ST_SPD | TD_ST_CERR(3) |
                            (t == ntd - 1 ? TD_ST_IOC : 0) |
                            (ls ? TD_ST_LS : 0);
            tds[t].link = (t + 1 < ntd)
                ? (uint32_t)(uintptr_t)&tds[t + 1] | TD_LINK_VF
                : TD_LINK_T;
        }

        qh->element = (uint32_t)(uintptr_t)&tds[0];
        qh->link    = TD_LINK_T;
        qh_insert_head(h, qh);

        h->intr[i].td     = tds;
        h->intr[i].qh     = qh;
        h->intr[i].ntd    = ntd;
        h->intr[i].mps    = mps;
        h->intr[i].toggle = 0;         /* td[0] starts at DATA0 */
        h->intr[i].cb     = cb;
        h->intr[i].ctx    = ctx;
        return 0;
    }
    return -1;
}

/* Scan armed interrupt pipes; dispatch + re-arm completed chains.
 *
 * A chain is done when the last TD went inactive, or an earlier TD
 * completed short (<mps) or with NAK/error (the HC abandons the rest
 * of the queue, leaving later TDs armed — we reset them on re-arm). */
static void uhci_scan_intr(UhciHc *h)
{
    for (int i = 0; i < UHCI_MAX_INTR; i++) {
        UhciIntr *p = &h->intr[i];
        UhciTD *tds = p->td;
        if (!tds) continue;

        int end = -1;            /* index of the TD that ended the xfer */
        for (int t = 0; t < p->ntd; t++) {
            uint32_t st = tds[t].status;
            if (st & TD_ST_ACTIVE) continue;
            if (st & (TD_ST_ERRMSK | TD_ST_NAK)) { end = t; break; }
            if (t == p->ntd - 1 ||
                (int)TD_ST_ACTLEN(st) + 1 < p->mps) { end = t; break; }
        }
        if (end < 0) continue;

        /* total bytes + count of packets that actually transferred */
        int total = 0, ndone = 0;
        int err = 0;
        for (int t = 0; t <= end; t++) {
            uint32_t st = tds[t].status;
            if (st & (TD_ST_ERRMSK | TD_ST_NAK)) { err = 1; break; }
            total += (int)TD_ST_ACTLEN(st) + 1;
            ndone++;
        }

        /* Re-arm the whole chain.  Only packets that transferred
         * advance the data toggle; CERR is reset — it may have expired
         * while the device was idle.  The HC overwrote qh->element
         * with the last TD's link (T) on completion — repoint it.
         * p->toggle = toggle that td[0] carries when re-armed. */
        p->toggle ^= ndone & 1;
        uint32_t ls = tds[0].status & TD_ST_LS;
        for (int t = 0; t < p->ntd; t++) {
            uint32_t tok = tds[t].token;
            if (p->toggle ^ (t & 1)) tok |=  TD_TOK_D(1);
            else                     tok &= ~TD_TOK_D(1);
            tds[t].token = tok;
            tds[t].status = TD_ST_ACTIVE | TD_ST_SPD | TD_ST_CERR(3) |
                            (t == p->ntd - 1 ? TD_ST_IOC : 0) | ls;
        }
        __asm__ volatile("mfence" ::: "memory");
        p->qh->element = (uint32_t)(uintptr_t)&tds[0];
        __asm__ volatile("mfence" ::: "memory");

        if (p->cb && total > 0 && !err)
            p->cb(p->ctx, (void *)(uintptr_t)tds[0].buffer, total);
    }
}

void UHCI_Poll(void)
{
    for (int i = 0; i < g_nhc; i++)
        uhci_scan_intr(&g_hc[i]);
}

/* ------------------------------------------------------------------ */
/* IRQ handler — IOC completions land here                             */
/* ------------------------------------------------------------------ */
static void uhci_irq_handler(uint64_t vector, uint64_t error_code)
{
    (void)error_code;
    for (int i = 0; i < g_nhc; i++) {
        UhciHc *h = &g_hc[i];
        if (h->irq_vec != (int)vector) continue;
        uint16_t st = rg16(h, U_USBSTS);
        w16(h, U_USBSTS, st);            /* W1C ack */
        uhci_scan_intr(h);
    }
    IRQ_EOI((int)vector);
}

/* ------------------------------------------------------------------ */
/* Root hub ports                                                      */
/* ------------------------------------------------------------------ */
static int uhci_port_connected(UsbHc *pub, int port)
{
    UhciHc *h = (UhciHc *)pub;
    if (port < 0 || port > 1) return -1;
    return (rg16(h, U_PORTSC1 + port * 2) & PSC_CCS) ? 1 : 0;
}

static int uhci_port_reset(UsbHc *pub, int port)
{
    UhciHc *h = (UhciHc *)pub;
    uint16_t r = (uint16_t)(U_PORTSC1 + port * 2);

    uint16_t v = rg16(h, r);
    if (!(v & PSC_CCS)) return -1;                     /* nothing attached */

    /* assert port reset >= 10 ms (use 50) */
    uoutw(h->io + r, (uint16_t)((v & ~(PSC_W1C)) | PSC_PR));
    msleep(50);
    uoutw(h->io + r, (uint16_t)(rg16(h, r) & ~(PSC_PR | PSC_W1C)));
    msleep(10);

    /* clear change bits and enable the port — QEMU does not set PE by
     * itself; real HW treats an explicit PE write as idempotent */
    uoutw(h->io + r, (uint16_t)(PSC_PE | PSC_W1C));
    for (int i = 0; i < 20 && !(rg16(h, r) & PSC_PE); i++)
        msleep(1);

    v = rg16(h, r);
    if (!(v & PSC_PE) || !(v & PSC_CCS))
        return -1;
    return (v & PSC_LSDA) ? USB_SPEED_LOW : USB_SPEED_FULL;
}

/* ------------------------------------------------------------------ */
/* Controller init                                                   */
/* ------------------------------------------------------------------ */
static int uhci_controller_init(UhciHc *h, uint8_t bus, uint8_t dev, uint8_t fn)
{
    uint32_t bar4 = pci_r32(bus, dev, fn, 0x20);
    if (!(bar4 & 1)) return -1;                      /* must be I/O BAR */
    h->io  = (uint16_t)(bar4 & ~0x1Fu);
    h->bus = bus; h->dev = dev; h->fn = fn;

    /* PCI command: I/O space + bus master */
    pci_w16(bus, dev, fn, 0x04,
            (uint16_t)(pci_r16(bus, dev, fn, 0x04) | 0x05));

    /* Kill legacy BIOS/SMM emulation traps */
    pci_w16(bus, dev, fn, 0xC0, 0x8F00);             /* USBLEGSUP */

    /* Global reset, then HC reset */
    w16(h, U_USBCMD, CMD_GRESET);
    msleep(15);
    w16(h, U_USBCMD, 0);
    msleep(5);
    w16(h, U_USBCMD, CMD_HCRESET);
    msleep(5);
    for (int i = 0; i < 100 && (rg16(h, U_USBCMD) & CMD_HCRESET); i++)
        msleep(1);

    /* frame list */
    h->fl = (volatile uint32_t *)DMA_Alloc(4096, 4096);
    h->anchor = qh_alloc();
    if (!h->fl || !h->anchor) return -1;
    h->anchor->link    = TD_LINK_T;
    h->anchor->element = TD_LINK_T;
    for (int i = 0; i < 1024; i++)
        h->fl[i] = ((uint32_t)(uintptr_t)h->anchor) | TD_LINK_Q;

    w32(h, U_FRBASE, (uint32_t)(uintptr_t)h->fl);
    w16(h, U_FRNUM, 0);
    w8 (h, U_SOFMOD, 64);                            /* 1 ms frame */
    w16(h, U_USBSTS, 0x3F);                          /* ack all */
    /* Interrupt enables are deferred to UHCI_SetupIRQs() — the INTx
     * line must not assert before the interrupt controller route is
     * programmed (QEMU's IO-APIC drops level asserts made while the
     * RTE is still masked-edge, and never re-evaluates the pin). */
    w16(h, U_USBINTR, 0);
    w16(h, U_USBCMD, CMD_RS | CMD_CF | CMD_MAXP);
    msleep(2);

    /* sanity: running? */
    if (rg16(h, U_USBSTS) & STS_HCH) {
        klog_puts(KLOG_USB, KLOG_WARN, "uhci: hc halted after run\n");
        return -1;
    }

    klog_puts(KLOG_USB, KLOG_DEBUG, "uhci: io=");
    klog_appendf(KLOG_USB, KLOG_DEBUG, "0x%08X", h->io);
    klog_puts(KLOG_USB, KLOG_DEBUG, " sts=");
    klog_appendf(KLOG_USB, KLOG_DEBUG, "0x%08X", rg16(h, U_USBSTS));
    klog_puts(KLOG_USB, KLOG_DEBUG, " p0=");
    klog_appendf(KLOG_USB, KLOG_DEBUG, "0x%08X", rg16(h, U_PORTSC1));
    klog_puts(KLOG_USB, KLOG_DEBUG, " p1=");
    klog_appendf(KLOG_USB, KLOG_DEBUG, "0x%08X", rg16(h, U_PORTSC2));
    klog_puts(KLOG_USB, KLOG_DEBUG, "\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public entry: find all UHCI controllers, register them with the USB  */
/* core, and enumerate ports.                                           */
/* ------------------------------------------------------------------ */
int UHCI_Init(void)
{
    int found = 0;
    for (uint16_t b = 0; b < 256 && g_nhc < MAX_UHCI; b++)
        for (uint8_t d = 0; d < 32 && g_nhc < MAX_UHCI; d++)
            for (uint8_t f = 0; f < 8 && g_nhc < MAX_UHCI; f++) {
                uint32_t id = pci_r32((uint8_t)b, d, f, 0x00);
                if (id == 0xFFFFFFFF) { if (f == 0) break; continue; }
                uint32_t cls = pci_r32((uint8_t)b, d, f, 0x08);
                if ((cls >> 8) != 0x0C0300u) continue;   /* serial/USB/UHCI */

                UhciHc *h = &g_hc[g_nhc];
                memset(h, 0, sizeof(*h));
                h->pub.name = "uhci";
                h->pub.priv = h;
                h->pub.control        = uhci_control;
                h->pub.intr_in        = uhci_intr_in;
                h->pub.port_connected = uhci_port_connected;
                h->pub.port_reset     = uhci_port_reset;
                h->pub.nports         = 2;
                h->irq_vec = -1;

                if (uhci_controller_init(h, (uint8_t)b, d, f) != 0) {
                    klog_puts(KLOG_USB, KLOG_WARN, "uhci: init failed\n");
                    continue;
                }
                USB_RegisterHc(&h->pub);
                g_nhc++;
                found++;
                klog_puts(KLOG_USB, KLOG_DEBUG, "uhci up\n");
            }
    return found;
}

/* Called from kernel main after IRQ_Init: attach INTx for each HC. */
void UHCI_SetupIRQs(void)
{
    for (int i = 0; i < g_nhc; i++) {
        UhciHc *h = &g_hc[i];
        int vec = IRQ_AttachPCI(h->bus, h->dev, h->fn,
                                uhci_irq_handler, "uhci");
        h->irq_vec = vec;
        if (vec >= 0) {
            /* Route is live — now enable interrupt generation.  Any
             * completion latched while USBINTR was clear re-asserts the
             * line here. */
            w16(h, U_USBSTS, 0x3F);
            w16(h, U_USBINTR, INTR_IOC | INTR_SPI | INTR_TOCRC | INTR_RESUME);
        }
        klog_puts(KLOG_USB, KLOG_DEBUG, "uhci: irq vec=");
        klog_appendf(KLOG_USB, KLOG_DEBUG, "0x%08X", (uint32_t)vec);
        klog_puts(KLOG_USB, KLOG_DEBUG, "\n");
    }
}
