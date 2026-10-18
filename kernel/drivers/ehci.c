/* ehci.c — UAOS EHCI (USB 2.0 high-speed) Host Controller Driver
 *
 * Drives PCI class 0x0C0320 controllers — the two ICH8M EHCIs on the
 * MacBookPro4,1 (00:1A.7 / 00:1D.7) and QEMU's usb-ehci.  BAR0 is an
 * MMIO space: capability registers at offset 0, operational registers
 * at BAR0 + CAPLENGTH.
 *
 * Schedule model:
 *   Async ring    — circular QH list rooted at a reclamation-head QH;
 *                   control and bulk transfers build a private qTD
 *                   chain inside a transient QH, link it after the
 *                   head, poll the last qTD's ACTIVE bit, then unlink.
 *                   Like uhci.c this works with interrupts off and
 *                   before IRQ_Init — qTD status lives in RAM.
 *   Periodic ring — every frame-list slot points at a shared anchor
 *                   QH whose horizontal chain carries each armed
 *                   interrupt-IN pipe (same "visit every frame" model
 *                   as UHCI; the HC NAKs quiet pipes).
 *
 * Port ownership: with CONFIGFLAG=1 all root ports route to EHCI.
 * A port reset that leaves PE clear means the attached device is
 * full/low-speed — the driver sets PortOwner and the matching UHCI
 * companion port reports the attach to the normal enum path.  This
 * replaces uhci.c's blanket ehci_release_ports() for driven
 * controllers (gated through EHCI_IsDriven()).
 *
 * Split transactions (full/low-speed devices behind a USB2 hub's
 * transaction translator) are not implemented — those devices fail
 * enumeration loudly rather than silently.
 */

#include "usb.h"
#include "../irq/irq.h"
#include "../klog/klog.h"
#include "../dos/dma.h"
#include "../exec/task.h"
#include "../dbg/diag.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* Registers                                                           */
/* ------------------------------------------------------------------ */
/* Capability registers (offsets into BAR0) */
#define CAP_LENGTH      0x00    /* u8  — operational-regs offset */
#define CAP_VERSION     0x02    /* u16 — 0x0100 expected */
#define CAP_HCSPARAMS   0x04    /* u32 — N_PORTS[3:0], N_CC[7:4], N_PCC[11:8] */
#define CAP_HCCPARAMS   0x08    /* u32 — EECP[15:8], PPC[4], 64BIT[0] */

/* Operational registers (offsets from BAR0 + CAP_LENGTH) */
#define EH_USBCMD       0x00
#define EH_USBSTS       0x04
#define EH_USBINTR      0x08
#define EH_FRINDEX      0x0C
#define EH_CTRLDSSEG    0x10
#define EH_PERBASE      0x14
#define EH_ASYNCBASE    0x18
#define EH_CFGFLAG      0x40
#define EH_PORTSC       0x44    /* +4 per port */

/* USBCMD */
#define CMD_RS          (1u << 0)
#define CMD_HCRESET     (1u << 1)
#define CMD_FLS_MASK    (3u << 2)   /* 00b = 1024 entries */
#define CMD_PSE         (1u << 4)   /* periodic schedule enable */
#define CMD_ASE         (1u << 5)   /* async schedule enable */
#define CMD_IAA         (1u << 6)   /* interrupt-on-async-advance doorbell */
#define CMD_ITC(n)      ((uint32_t)(n) << 16)

/* USBSTS */
#define STS_USBINT      (1u << 0)
#define STS_ERR         (1u << 1)
#define STS_PCD         (1u << 2)   /* port change detect */
#define STS_FLR         (1u << 3)
#define STS_HSE         (1u << 4)
#define STS_IAA         (1u << 5)
#define STS_HCH         (1u << 12)  /* HCHalted */
#define STS_RECL        (1u << 13)  /* reclamation (async empty) */
#define STS_PSS         (1u << 14)  /* periodic schedule status */
#define STS_ASS         (1u << 15)  /* async schedule status */
#define STS_W1C_ALL     (STS_USBINT|STS_ERR|STS_PCD|STS_FLR|STS_HSE|STS_IAA)

/* USBINTR enables mirror the STS bits 0-5 */
#define INTR_ALL        (STS_USBINT|STS_ERR|STS_PCD|STS_FLR|STS_HSE|STS_IAA)

/* PORTSC */
#define PS_CCS          (1u << 0)   /* current connect status */
#define PS_CSC          (1u << 1)   /* connect status change (W1C) */
#define PS_PE           (1u << 2)   /* port enabled */
#define PS_PEC          (1u << 3)   /* port enable change (W1C) */
#define PS_OCC          (1u << 4)   /* over-current change (W1C) */
#define PS_OCA          (1u << 5)   /* over-current active */
#define PS_FPR          (1u << 6)   /* force port resume */
#define PS_SUSP         (1u << 7)
#define PS_PR           (1u << 8)   /* port reset */
#define PS_LS_K         (1u << 10)  /* line status K-state → low speed */
#define PS_PP           (1u << 12)  /* port power */
#define PS_PO           (1u << 13)  /* port owner: 0=ehci, 1=companion */
#define PS_W1C          (PS_CSC | PS_PEC | PS_OCC)

/* USBLEGSUP semaphore bits (extended-capability space) */
#define LEGSUP_BIOS     (1u << 16)
#define LEGSUP_OS       (1u << 24)
#define LEGSUP_STS_W1C  (0xFFu << 0) /* SMI status bits are W1C */

/* ------------------------------------------------------------------ */
/* qTD / QH structures (DMA memory, 32-byte aligned)                   */
/* ------------------------------------------------------------------ */
#define LP_T            (1u << 0)
#define LP_TYP_QH       (1u << 1)   /* link-pointer type field = QH */

/* qTD token (dword 2) */
#define QTD_ST_PING     (1u << 0)
#define QTD_ST_SPLIT    (1u << 1)
#define QTD_ST_MISSED   (1u << 2)
#define QTD_ST_XACT     (1u << 3)   /* transaction error */
#define QTD_ST_BABBLE   (1u << 4)
#define QTD_ST_BUFERR   (1u << 5)
#define QTD_ST_HALTED   (1u << 6)
#define QTD_ST_ACTIVE   (1u << 7)
#define QTD_ERRMASK     (QTD_ST_XACT|QTD_ST_BABBLE|QTD_ST_BUFERR|QTD_ST_HALTED)
#define QTD_PID_OUT     (0u << 8)
#define QTD_PID_IN      (1u << 8)
#define QTD_PID_SETUP   (2u << 8)
#define QTD_CERR(n)     (((uint32_t)(n) & 3u) << 10)
#define QTD_IOC         (1u << 15)
#define QTD_LEN(n)      (((uint32_t)(n) & 0x7FFFu) << 16)
#define QTD_REMAIN(tok) (((tok) >> 16) & 0x7FFFu)
#define QTD_DT(n)       (((uint32_t)(n) & 1u) << 31)

/* One qTD spans at most 5 buffer pages; cap a qTD payload at 16 KB so
 * a mid-page start still fits comfortably. */
#define QTD_MAX_PAYLOAD  0x4000u

typedef struct __attribute__((packed, aligned(32))) {
    volatile uint32_t next;     /* next qTD pointer | T */
    uint32_t alt;               /* alternate next on short packet */
    volatile uint32_t token;    /* status + control — HC writes back */
    uint32_t buf[5];            /* page pointers (buf[0] carries offset) */
    uint32_t ext[5];            /* high dwords for >4GB pages — unused */
    uint32_t pad[2];            /* pad to 64 bytes */
} EhciTD;

/* QH endpoint-characteristics dword */
#define QHC_ADDR(n)     ((uint32_t)(n) & 0x7Fu)
#define QHC_EP(n)       (((uint32_t)(n) & 0xFu) << 8)
#define QHC_SPD_FS      (0u << 12)
#define QHC_SPD_LS      (1u << 12)
#define QHC_SPD_HS      (2u << 12)
#define QHC_DTC         (1u << 14)  /* data toggle comes from qTD */
#define QHC_HEAD        (1u << 15)  /* head of reclamation list */
#define QHC_MPS(n)      (((uint32_t)(n) & 0x7FFu) << 16)
#define QHC_RL(n)       (((uint32_t)(n) & 0xFu) << 28)

/* QH endpoint-capabilities dword */
#define QHC2_SMASK(m)   ((uint32_t)(m) & 0xFFu)
#define QHC2_CMASK(m)   (((uint32_t)(m) & 0xFFu) << 8)
#define QHC2_MULT(n)    (((uint32_t)(n) & 3u) << 30)

typedef struct __attribute__((packed, aligned(32))) {
    volatile uint32_t hor;      /* horizontal link pointer */
    uint32_t epchar;            /* endpoint characteristics */
    uint32_t epcaps;            /* endpoint capabilities */
    volatile uint32_t curtd;    /* current qTD (HC working ptr) */
    /* overlay area — the HC's working copy of the active qTD */
    volatile uint32_t ov_next;
    volatile uint32_t ov_alt;
    volatile uint32_t ov_token;
    volatile uint32_t ov_buf[5];
    uint32_t ov_ext[5];         /* >4GB extensions — unused */
    uint32_t pad[3];            /* pad to 64 bytes */
} EhciQH;

/* ------------------------------------------------------------------ */
/* HC state                                                            */
/* ------------------------------------------------------------------ */
#define EHCI_MAX_INTR   4

typedef struct {
    EhciQH *qh;                 /* persistent periodic QH */
    EhciTD *td;                 /* single re-armed IN qTD */
    void   *buf;                /* report buffer (caller-owned, DMA ok) */
    uint16_t mps;
    uint16_t buflen;
    int     toggle;
    void  (*cb)(void *ctx, void *buf, int len);
    void   *ctx;
    int     err_n;
    int     stall_n;
} EhciIntr;

typedef struct {
    UsbHc     pub;              /* MUST be first member */
    uint8_t   bus, dev, fn;
    volatile uint32_t *op;      /* operational register base */
    int       nports;
    volatile uint32_t *fl;      /* 1024 periodic frame pointers */
    EhciQH   *async_head;       /* reclamation head — ring stays non-empty */
    EhciQH   *peri_anchor;      /* all fl[] slots point here */
    EhciIntr  intr[EHCI_MAX_INTR];
    int       irq_vec;
    uint32_t  irq_hits;
    uint32_t  cf_n;             /* consecutive ctrl-fail count (throttle) */
} EhciHc;

#define MAX_EHCI 4
static EhciHc  g_hc[MAX_EHCI];
static int     g_nhc = 0;
static struct { uint8_t b, d, f; } g_driven[MAX_EHCI];

extern volatile uint64_t g_pit_ticks;      /* 100 Hz */

/* BDFs the driver successfully claimed — uhci.c's release/quiesce
 * helpers consult this so driven controllers keep CONFIGFLAG=1 and
 * their INTx line (UAOS-317). */
int EHCI_IsDriven(uint8_t b, uint8_t d, uint8_t f)
{
    for (int i = 0; i < g_nhc; i++)
        if (g_driven[i].b == b && g_driven[i].d == d &&
            g_driven[i].f == f)
            return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* I/O helpers                                                         */
/* ------------------------------------------------------------------ */
static inline uint32_t ord(EhciHc *h, uint32_t r) { return h->op[r >> 2]; }
static inline void owr(EhciHc *h, uint32_t r, uint32_t v)
    { h->op[r >> 2] = v; }

static uint32_t psc(EhciHc *h, int port)
    { return ord(h, EH_PORTSC + (uint32_t)port * 4); }

/* PORTSC write helper: never write 1 to a W1C bit unintentionally —
 * mask is applied to the value so callers pass "new RW state". */
static void psc_set(EhciHc *h, int port, uint32_t v)
    { owr(h, EH_PORTSC + (uint32_t)port * 4, v & ~PS_W1C); }

/* Same timing helper as uhci.c — port-0x80 spin pre-TSC, rdtsc after. */
static void udelay(uint32_t us)
{
    uint64_t hz = Tickmon_TscHz();
    if (hz) {
        uint64_t deadline = diag_rdtsc() + ((uint64_t)us * hz) / 1000000;
        while (diag_rdtsc() < deadline)
            __asm__ volatile("pause");
        return;
    }
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
    __asm__ volatile("outl %0,%1" :: "a"(0x80000000u |
        ((uint32_t)b << 16) | ((uint32_t)d << 11) | ((uint32_t)f << 8) |
        (off & 0xFC)), "Nd"(0xCF8));
    uint32_t v;
    __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(0xCFC));
    return v;
}
static void pci_w32(uint8_t b, uint8_t d, uint8_t f, uint8_t off, uint32_t v)
{
    __asm__ volatile("outl %0,%1" :: "a"(0x80000000u |
        ((uint32_t)b << 16) | ((uint32_t)d << 11) | ((uint32_t)f << 8) |
        (off & 0xFC)), "Nd"(0xCF8));
    __asm__ volatile("outl %0,%1" :: "a"(v), "Nd"(0xCFC));
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
/* Queue helpers                                                       */
/* ------------------------------------------------------------------ */
static EhciQH *qh_alloc(void)
{
    EhciQH *q = (EhciQH *)DMA_Alloc(sizeof(EhciQH), 32);
    if (q) memset(q, 0, sizeof(*q));
    return q;
}

/* Fill a qTD: buffers cover the whole payload — buf[0] keeps the byte
 * offset, buf[1..4] get the following page bases unconditionally (the
 * HC only consumes them when the transfer crosses the boundary). */
static void qtd_init(EhciTD *t, uint32_t pid, int dt, void *buf,
                     uint32_t len)
{
    memset(t, 0, sizeof(*t));
    uintptr_t a = (uintptr_t)buf;
    t->buf[0] = (uint32_t)a;
    uintptr_t pg = a & ~0xFFFu;
    for (int i = 1; i < 5; i++)
        t->buf[i] = (uint32_t)(pg + (uintptr_t)i * 0x1000u);
    t->next  = LP_T;
    t->alt   = LP_T;
    t->token = QTD_ST_ACTIVE | QTD_CERR(3) | pid | QTD_DT(dt) |
               QTD_LEN(len);
}

/* Insert a QH right after the async reclamation head. */
static void async_insert(EhciHc *h, EhciQH *q)
{
    Forbid();
    q->hor = h->async_head->hor;
    __asm__ volatile("mfence" ::: "memory");
    h->async_head->hor = ((uint32_t)(uintptr_t)q) | LP_TYP_QH;
    Permit();
}

/* Remove a QH from the async ring, then issue the IAA doorbell so the
 * HC flushes its cached copy before the memory is freed (spec 4.8.5). */
static void async_remove(EhciHc *h, EhciQH *q)
{
    Forbid();
    volatile uint32_t *prev = &h->async_head->hor;
    for (;;) {
        uint32_t l = *prev;
        EhciQH *n = (l & LP_T) ? 0 : (EhciQH *)(uintptr_t)(l & ~0x1Fu);
        if (!n || n == h->async_head) break;      /* walked the ring */
        if (n == q) {
            *prev = n->hor;
            __asm__ volatile("mfence" ::: "memory");
            break;
        }
        prev = &n->hor;
    }
    Permit();

    /* IAA handshake: the HC reports the next traversal past the
     * unlinked position; on a broken HC fall back to a 2 ms grace. */
    owr(h, EH_USBSTS, STS_IAA);
    owr(h, EH_USBCMD, ord(h, EH_USBCMD) | CMD_IAA);
    int ok = 0;
    for (int i = 0; i < 20; i++) {
        if (ord(h, EH_USBSTS) & STS_IAA) { ok = 1; break; }
        msleep(1);
    }
    owr(h, EH_USBSTS, STS_IAA);
    if (!ok) msleep(2);
}

/* Wait for a qTD chain to retire.  Mirrors uhci's discipline:
 *   - done when the last qTD loses ACTIVE (or a chain qTD errors);
 *   - `deaf_ms` with zero retired qTDs is dead air → -2 (control
 *     transfers pass 40 — a live device answers within a few frames;
 *     bulk passes ~0 to disable the check entirely: BOT devices may
 *     legitimately NAK a CBW/CSW for hundreds of ms while they finish
 *     the previous command, which is indistinguishable from a dead
 *     async schedule and must not fail the transfer).
 * Returns 0 ok, -1 protocol error, -2 deaf. */
static int wait_chain(EhciHc *h, EhciTD *tds, int ntd, int deaf_ms)
{
    int sched = (Task_Current() != NULL);
    int max_iter = sched ? 210 : 2000;   /* ~2 s worst case */
    int dead_ms = 0;
    int deaf = 0;

    for (int i = 0; i < max_iter; i++) {
        int progress = 0, err = 0;
        for (int t = 0; t < ntd; t++) {
            uint32_t tok = tds[t].token;
            if (tok & QTD_ST_ACTIVE) continue;
            progress = 1;
            if (tok & QTD_ERRMASK) err = 1;
        }
        if (!(tds[ntd - 1].token & QTD_ST_ACTIVE) || err)
            return err ? -1 : 0;
        if (progress) dead_ms = 0;
        else if (deaf_ms &&
                 (dead_ms += (i < 6 || !sched) ? 1 : 10) >= deaf_ms) {
            deaf = 1;
            break;
        }
        if (i < 6 || !sched) msleep(1);
        else                 Task_SleepTicks(1);
    }
    (void)h;
    return deaf ? -2 : -1;
}

static void ctrl_fail_log(EhciHc *h, UsbDev *dev, uint8_t bReq,
                          EhciTD *tds, int ntd, int err)
{
    if (++h->cf_n <= 2) {
        klog_puts(KLOG_USB, KLOG_WARN, "ehci: xfer fail req=");
        klog_appendf(KLOG_USB, KLOG_WARN, "0x%08X", (uint32_t)bReq);
        klog_puts(KLOG_USB, KLOG_WARN, " dev=");
        klog_appendf(KLOG_USB, KLOG_WARN, "%u", (uint32_t)dev->addr);
        klog_puts(KLOG_USB, KLOG_WARN, " port=");
        klog_appendf(KLOG_USB, KLOG_WARN, "%u", (uint32_t)dev->port);
        klog_puts(KLOG_USB, KLOG_WARN, " err=");
        klog_appendf(KLOG_USB, KLOG_WARN, "%d", (uint32_t)err);
        klog_puts(KLOG_USB, KLOG_WARN, " tds");
        for (int i = 0; i < ntd && i < 7; i++)
            klog_appendf(KLOG_USB, KLOG_WARN, " 0x%08X", tds[i].token);
        klog_puts(KLOG_USB, KLOG_WARN, " ov=");
        klog_appendf(KLOG_USB, KLOG_WARN, "0x%08X", 0u);
        klog_puts(KLOG_USB, KLOG_WARN, "\n");
    }
}

/* ------------------------------------------------------------------ */
/* Synchronous control transfer                                        */
/* ------------------------------------------------------------------ */
static int ehci_control(UsbHc *pub, UsbDev *dev, uint8_t ep,
                        uint8_t bmRT, uint8_t bReq,
                        uint16_t wVal, uint16_t wIdx,
                        void *data, uint16_t len)
{
    EhciHc *h = (EhciHc *)pub;
    int data_in = (bmRT & USB_RT_IN) != 0;
    uint32_t mps = dev->ep0_mps ? dev->ep0_mps : 64;

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

    int ndata = (int)((len + QTD_MAX_PAYLOAD - 1) / QTD_MAX_PAYLOAD);
    int ntd   = 1 + ndata + 1;            /* setup + data + status */

    EhciQH *qh  = qh_alloc();
    EhciTD *tds = (EhciTD *)DMA_Alloc((size_t)ntd * sizeof(EhciTD), 32);
    if (!qh || !tds) {
        DMA_Free(setup, 8);
        if (qh)  DMA_Free(qh, sizeof(EhciQH));
        if (tds) DMA_Free(tds, (size_t)ntd * sizeof(EhciTD));
        return -1;
    }

    /* setup stage — DATA0 */
    qtd_init(&tds[0], QTD_PID_SETUP, 0, setup, 8);

    /* data stage — toggling from DATA1; a short packet jumps to the
     * status qTD through the alt pointer (spec 3.6.3). */
    for (int i = 0; i < ndata; i++) {
        uint32_t off = (uint32_t)i * QTD_MAX_PAYLOAD;
        uint32_t chunk = len - off > QTD_MAX_PAYLOAD
                         ? QTD_MAX_PAYLOAD : len - off;
        EhciTD *t = &tds[1 + i];
        qtd_init(t, data_in ? QTD_PID_IN : QTD_PID_OUT,
                 i & 1 ? 0 : 1, (uint8_t *)data + off, chunk);
        t->alt = (uint32_t)(uintptr_t)&tds[ntd - 1];
        t->next = (uint32_t)(uintptr_t)&tds[1 + i + 1];
    }
    if (ndata) tds[0].next = (uint32_t)(uintptr_t)&tds[1];
    else       tds[0].next = (uint32_t)(uintptr_t)&tds[ntd - 1];

    /* status stage — opposite direction, DATA1, zero length */
    qtd_init(&tds[ntd - 1], data_in ? QTD_PID_OUT : QTD_PID_IN,
             1, 0, 0);

    /* QH: DTC=1 → each qTD's DT bit programs the toggle; MPS comes
     * from ep0_mps; speed is HIGH for everything EHCI enumerates
     * (full/low-speed devices route to the companions). */
    qh->epchar = QHC_ADDR(dev->addr) | QHC_EP(ep) |
                 (dev->speed == USB_SPEED_HIGH ? QHC_SPD_HS
                  : dev->speed == USB_SPEED_LOW ? QHC_SPD_LS : QHC_SPD_FS) |
                 QHC_DTC | QHC_MPS(mps) | QHC_RL(8);
    qh->epcaps = QHC2_MULT(1);
    qh->curtd  = LP_T;
    qh->ov_next  = (uint32_t)(uintptr_t)&tds[0];
    qh->ov_alt   = LP_T;
    qh->ov_token = 0;                     /* overlay inactive → fetch */
    async_insert(h, qh);

    int err = wait_chain(h, tds, ntd, 40);
    if (err == 0) {
        /* last qTD retired — verify no stage carried an error bit */
        for (int i = 0; i < ntd; i++)
            if (tds[i].token & QTD_ERRMASK) { err = -1; break; }
    }
    if (err == 0) h->cf_n = 0;
    else          ctrl_fail_log(h, dev, bReq, tds, ntd, err);

    async_remove(h, qh);
    DMA_Free(setup, 8);
    DMA_Free(qh, sizeof(EhciQH));
    DMA_Free(tds, (size_t)ntd * sizeof(EhciTD));
    return err;
}

/* ------------------------------------------------------------------ */
/* Synchronous bulk transfer                                           */
/* ------------------------------------------------------------------ */
static int ehci_bulk(UsbHc *pub, UsbDev *dev, uint8_t ep, int dir_in,
                     void *data, uint32_t len, uint16_t mps,
                     int *toggle, uint32_t *actual)
{
    EhciHc *h = (EhciHc *)pub;
    if (!mps) mps = 512;
    if (!DMA_IsAccessible(data) && len) return -1;
    int t0 = toggle ? (*toggle & 1) : 0;
    if (actual) *actual = 0;

    int ntd = (int)((len + QTD_MAX_PAYLOAD - 1) / QTD_MAX_PAYLOAD);
    if (ntd < 1) ntd = 1;

    EhciQH *qh  = qh_alloc();
    EhciTD *tds = (EhciTD *)DMA_Alloc((size_t)ntd * sizeof(EhciTD), 32);
    if (!qh || !tds) {
        if (qh)  DMA_Free(qh, sizeof(EhciQH));
        if (tds) DMA_Free(tds, (size_t)ntd * sizeof(EhciTD));
        return -1;
    }

    /* Chain qTDs — packets-per-qTD derived from mps so DT alternates
     * at packet granularity: a qTD carries ceil(len/mps) packets, so
     * the starting toggle of qTD i is t0 ^ (packets before it & 1).
     * Compute per qTD directly from its byte range. */
    for (int i = 0; i < ntd; i++) {
        uint32_t off = (uint32_t)i * QTD_MAX_PAYLOAD;
        uint32_t chunk = len - off > QTD_MAX_PAYLOAD
                         ? QTD_MAX_PAYLOAD : len - off;
        int pkts_before = (int)(off / mps);
        EhciTD *t = &tds[i];
        qtd_init(t, dir_in ? QTD_PID_IN : QTD_PID_OUT,
                 (t0 ^ pkts_before) & 1, (uint8_t *)data + off, chunk);
        if (i + 1 < ntd)
            t->next = (uint32_t)(uintptr_t)&tds[i + 1];
        /* alt stays T — a short packet ends the bulk transfer */
    }

    qh->epchar = QHC_ADDR(dev->addr) | QHC_EP(ep) |
                 (dev->speed == USB_SPEED_HIGH ? QHC_SPD_HS
                  : dev->speed == USB_SPEED_LOW ? QHC_SPD_LS : QHC_SPD_FS) |
                 QHC_DTC | QHC_MPS(mps) | QHC_RL(8);
    qh->epcaps = QHC2_MULT(1);
    qh->curtd  = LP_T;
    qh->ov_next  = (uint32_t)(uintptr_t)&tds[0];
    qh->ov_alt   = LP_T;
    qh->ov_token = 0;
    async_insert(h, qh);

    int err = wait_chain(h, tds, ntd, 0);   /* BOT may NAK for ages */

    /* Tally moved bytes and consumed packets from retired qTDs. */
    uint32_t moved = 0;
    uint32_t pkt_total = 0;
    int qerr = err;
    for (int i = 0; i < ntd; i++) {
        uint32_t tok = tds[i].token;
        if (tok & QTD_ST_ACTIVE) break;
        if (tok & QTD_ERRMASK) { qerr = -1; break; }
        uint32_t want = (i + 1 < ntd) ? QTD_MAX_PAYLOAD
                                      : len - (uint32_t)i * QTD_MAX_PAYLOAD;
        uint32_t got = want - QTD_REMAIN(tok);
        moved += got;
        pkt_total += (got + mps - 1) / mps;
        /* Short qTD = end of transfer (HC followed alt=T). */
        if (QTD_REMAIN(tok)) break;
    }
    if (actual) *actual = moved;
    if (toggle)
        *toggle = (t0 ^ (int)(pkt_total & 1)) & 1;

    if (qerr != 0)
        ctrl_fail_log(h, dev, dir_in ? 0x80 : 0x00, tds, ntd, qerr);
    else
        h->cf_n = 0;

    async_remove(h, qh);
    DMA_Free(qh, sizeof(EhciQH));
    DMA_Free(tds, (size_t)ntd * sizeof(EhciTD));
    return qerr;
}

/* ------------------------------------------------------------------ */
/* Persistent interrupt-IN pipe (periodic schedule)                    */
/* ------------------------------------------------------------------ */
/* Insert a pipe QH right after the periodic anchor — the HC visits
 * the whole horizontal chain every frame. */
static void peri_insert(EhciHc *h, EhciQH *q)
{
    Forbid();
    q->hor = h->peri_anchor->hor;
    __asm__ volatile("mfence" ::: "memory");
    h->peri_anchor->hor = ((uint32_t)(uintptr_t)q) | LP_TYP_QH;
    Permit();
}

static int ehci_intr_in(UsbHc *pub, UsbDev *dev, uint8_t ep,
                        uint16_t mps, void *buf, uint16_t buflen,
                        void (*cb)(void *ctx, void *buf, int len),
                        void *ctx)
{
    EhciHc *h = (EhciHc *)pub;
    if (!mps) mps = 8;
    if (!buflen) buflen = mps;
    /* One qTD covers the whole report; reports larger than one qTD's
     * payload ceiling aren't supported (hub status EPs are 1-4 B). */
    if (buflen > QTD_MAX_PAYLOAD) buflen = QTD_MAX_PAYLOAD;
    if (!DMA_IsAccessible(buf)) return -1;

    for (int i = 0; i < EHCI_MAX_INTR; i++) {
        EhciIntr *p = &h->intr[i];
        if (p->qh) continue;
        EhciQH *qh = qh_alloc();
        EhciTD *td = (EhciTD *)DMA_Alloc(sizeof(EhciTD), 32);
        if (!qh || !td) {
            if (qh) DMA_Free(qh, sizeof(EhciQH));
            if (td) DMA_Free(td, sizeof(EhciTD));
            return -1;
        }
        /* Interrupt endpoints toggle via the overlay (DTC=0): the HC
         * keeps the running toggle in the QH overlay between frames,
         * which is exactly what a persistent pipe wants. */
        qh->epchar = QHC_ADDR(dev->addr) | QHC_EP(ep) |
                     (dev->speed == USB_SPEED_HIGH ? QHC_SPD_HS
                      : dev->speed == USB_SPEED_LOW ? QHC_SPD_LS
                                                  : QHC_SPD_FS) |
                     QHC_MPS(mps) | QHC_RL(0);
        /* s-mask=1 → poll on the first microframe of every 1 ms frame;
         * c-mask=0 (no splits — high-speed only). */
        qh->epcaps = QHC2_SMASK(1) | QHC2_MULT(1);
        qh->curtd  = LP_T;
        qh->ov_next  = (uint32_t)(uintptr_t)td;
        qh->ov_alt   = LP_T;
        qh->ov_token = 0;

        qtd_init(td, QTD_PID_IN, 0, buf, buflen);
        td->token |= QTD_IOC;

        p->qh     = qh;
        p->td     = td;
        p->buf    = buf;
        p->mps    = mps;
        p->buflen = buflen;
        p->cb     = cb;
        p->ctx    = ctx;
        p->toggle = 0;
        p->err_n = 0;
        p->stall_n = 0;
        peri_insert(h, qh);
        return 0;
    }
    return -1;
}

/* Scan armed pipes; dispatch + re-arm completed qTDs.  Called from the
 * IRQ handler and EHCI_Poll — same model as uhci_scan_intr. */
static void ehci_scan_intr(EhciHc *h)
{
    for (int i = 0; i < EHCI_MAX_INTR; i++) {
        EhciIntr *p = &h->intr[i];
        EhciTD *td = p->td;
        if (!td) continue;
        uint32_t tok = td->token;
        if (tok & QTD_ST_ACTIVE) {
            if (++p->stall_n == 400) {
                klog_puts(KLOG_USB, KLOG_DEBUG, "ehci: intr hc=");
                klog_appendf(KLOG_USB, KLOG_DEBUG, "%u",
                             (uint32_t)(h - g_hc));
                klog_puts(KLOG_USB, KLOG_DEBUG, " pipe=");
                klog_appendf(KLOG_USB, KLOG_DEBUG, "%u", i);
                klog_puts(KLOG_USB, KLOG_DEBUG,
                          " no completion in ~4 s\n");
            }
            continue;
        }
        p->stall_n = 0;
        if (tok & QTD_ERRMASK) {
            if (p->err_n < 8) {
                p->err_n++;
                klog_puts(KLOG_USB, KLOG_WARN, "ehci: intr err tok=");
                klog_appendf(KLOG_USB, KLOG_WARN, "0x%08X", tok);
                klog_puts(KLOG_USB, KLOG_WARN, " pipe=");
                klog_appendf(KLOG_USB, KLOG_WARN, "%u", i);
                klog_puts(KLOG_USB, KLOG_WARN, "\n");
            }
        } else if (p->cb) {
            int len = (int)(p->buflen - QTD_REMAIN(tok));
            if (len > 0)
                p->cb(p->ctx, p->buf, len);
        }
        /* Re-arm: same qTD — DTC=0 means the HC keeps the toggle in
         * the QH overlay, so the qTD's DT bit is ignored here. */
        qtd_init(td, QTD_PID_IN, 0, p->buf, p->buflen);
        td->token |= QTD_IOC;
        __asm__ volatile("mfence" ::: "memory");
        p->qh->ov_next = (uint32_t)(uintptr_t)td;
    }
}

void EHCI_Poll(void)
{
    for (int i = 0; i < g_nhc; i++)
        ehci_scan_intr(&g_hc[i]);
}

/* ------------------------------------------------------------------ */
/* IRQ — completions land here once INTx is attached                   */
/* ------------------------------------------------------------------ */
static void ehci_irq_handler(uint64_t vector, uint64_t error_code)
{
    (void)error_code;
    int serviced = 0;
    for (int i = 0; i < g_nhc; i++) {
        EhciHc *h = &g_hc[i];
        if (h->irq_vec != (int)vector) continue;
        uint32_t st = ord(h, EH_USBSTS) & STS_W1C_ALL;
        if (st) {
            serviced = 1;
            h->irq_hits++;
            owr(h, EH_USBSTS, st);
        }
        ehci_scan_intr(h);
    }
    IRQ_EOI((int)vector);
    (void)serviced;   /* shared level line — foreign asserts possible */
}

/* ------------------------------------------------------------------ */
/* Root-hub ports                                                      */
/* ------------------------------------------------------------------ */
static int ehci_port_connected(UsbHc *pub, int port)
{
    EhciHc *h = (EhciHc *)pub;
    if (port < 0 || port >= h->nports) return -1;
    uint32_t v = psc(h, port);
    if (v & PS_PO) return 0;        /* routed to a UHCI companion */
    return (v & PS_CCS) ? 1 : 0;
}

static int ehci_port_csc(UsbHc *pub, int port)
{
    EhciHc *h = (EhciHc *)pub;
    if (port < 0 || port >= h->nports) return -1;
    uint32_t v = psc(h, port);
    if (!(v & PS_CSC)) return 0;
    /* W1C just CSC — preserve the RW state and other change bits. */
    owr(h, EH_PORTSC + (uint32_t)port * 4,
        (v & ~PS_W1C) | PS_CSC);
    return 1;
}

static int ehci_port_status(UsbHc *pub, int port)
{
    EhciHc *h = (EhciHc *)pub;
    if (port < 0 || port >= h->nports) return -1;
    return (int)psc(h, port);
}

/* Release a port to its UHCI companion: set PortOwner, wait out the
 * re-mux bounce (~20 ms), then report "no device" so the EHCI side
 * drops the port — the companion UHCI sees a fresh attach instead. */
static int ehci_release_port(EhciHc *h, int port)
{
    psc_set(h, port, psc(h, port) | PS_PO);
    msleep(20);
    return -1;
}

static int ehci_port_reset(UsbHc *pub, int port)
{
    EhciHc *h = (EhciHc *)pub;
    if (port < 0 || port >= h->nports) return -1;

    uint32_t v = psc(h, port);
    if (v & PS_PO) return -1;            /* companion owns it */
    if (!(v & PS_CCS)) return -1;        /* nothing attached */

    /* Line-status K-state = a low-speed device — EHCI never resets
     * those; straight to the companion. */
    if (v & PS_LS_K) return ehci_release_port(h, port);

    /* Assert port reset ≥10 ms (use 50 — same margin as UHCI). */
    psc_set(h, port, v | PS_PR);
    msleep(50);
    psc_set(h, port, psc(h, port) & ~PS_PR);
    msleep(20);

    v = psc(h, port);
    if (!(v & PS_CCS)) return -1;
    if (v & PS_PE) {
        /* High-speed handshake succeeded — settle before SETUP. */
        msleep(50);
        return USB_SPEED_HIGH;
    }
    /* Device didn't enable → full-speed → companion territory. */
    return ehci_release_port(h, port);
}

/* ------------------------------------------------------------------ */
/* Controller init                                                     */
/* ------------------------------------------------------------------ */
static int ehci_controller_init(EhciHc *h, uint8_t bus, uint8_t dev,
                                uint8_t fn)
{
    uint32_t bar = pci_r32(bus, dev, fn, 0x10);
    if ((bar & 1) || !(bar & ~0xFu)) return -1;   /* need MMIO BAR0 */
    h->bus = bus; h->dev = dev; h->fn = fn;

    /* MEMSE + bus master */
    pci_w16(bus, dev, fn, 0x04,
            (uint16_t)(pci_r16(bus, dev, fn, 0x04) | 0x06));

    volatile uint8_t *cap = (volatile uint8_t *)(uintptr_t)(bar & ~0xFu);
    uint8_t caplen = cap[CAP_LENGTH];
    uint32_t hcs = *(volatile uint32_t *)(cap + CAP_HCSPARAMS);
    uint32_t hcc = *(volatile uint32_t *)(cap + CAP_HCCPARAMS);
    if (!caplen || caplen > 0x40) return -1;
    h->nports = (int)(hcs & 0xF);
    if (h->nports < 1) return -1;
    if (h->nports > USB_MAX_PORTS) h->nports = USB_MAX_PORTS;
    h->op = (volatile uint32_t *)(cap + caplen);

    /* BIOS→OS handoff via USBLEGSUP (EECP offset in PCI cfg space). */
    uint8_t eecp = (uint8_t)((hcc >> 8) & 0xFF);
    if (eecp) {
        uint32_t legsup = pci_r32(bus, dev, fn, eecp);
        if (legsup & LEGSUP_BIOS) {
            pci_w32(bus, dev, fn, eecp,
                    (legsup & ~LEGSUP_STS_W1C) | LEGSUP_OS);
            for (int i = 0; i < 100 &&
                 (pci_r32(bus, dev, fn, eecp) & LEGSUP_BIOS); i++)
                msleep(1);
        }
    }

    /* Stop the schedule, then HCRESET. */
    owr(h, EH_USBCMD, ord(h, EH_USBCMD) & ~CMD_RS);
    for (int i = 0; i < 100 && !(ord(h, EH_USBSTS) & STS_HCH); i++)
        msleep(1);
    owr(h, EH_USBCMD, ord(h, EH_USBCMD) | CMD_HCRESET);
    for (int i = 0; i < 100 && (ord(h, EH_USBCMD) & CMD_HCRESET); i++)
        msleep(1);
    if (ord(h, EH_USBCMD) & CMD_HCRESET) {
        klog_puts(KLOG_USB, KLOG_WARN, "ehci: HCRESET stuck\n");
        return -1;
    }

    /* Periodic frame list — 1024 entries, all pointing at the anchor
     * QH (whose horizontal chain carries the intr pipes). */
    h->fl = (volatile uint32_t *)DMA_Alloc(4096, 4096);
    h->peri_anchor = qh_alloc();
    h->async_head  = qh_alloc();
    if (!h->fl || !h->peri_anchor || !h->async_head) return -1;

    h->peri_anchor->hor = LP_T;
    h->peri_anchor->curtd = LP_T;
    h->peri_anchor->ov_next = LP_T;
    for (int i = 0; i < 1024; i++)
        h->fl[i] = ((uint32_t)(uintptr_t)h->peri_anchor) | LP_TYP_QH;

    /* Async ring = the reclamation head alone, linked to itself. */
    h->async_head->epchar = QHC_HEAD;
    h->async_head->hor = ((uint32_t)(uintptr_t)h->async_head) | LP_TYP_QH;
    h->async_head->curtd = LP_T;
    h->async_head->ov_next = LP_T;

    owr(h, EH_CTRLDSSEG, 0);                    /* <4GB segments */
    owr(h, EH_PERBASE, (uint32_t)(uintptr_t)h->fl);
    owr(h, EH_ASYNCBASE, (uint32_t)(uintptr_t)h->async_head);
    owr(h, EH_FRINDEX, 0);
    owr(h, EH_USBINTR, 0);                      /* armed in SetupIRQs */
    owr(h, EH_USBSTS, STS_W1C_ALL);             /* ack all */

    /* Claim the port-ownership mux — every root port now routes to
     * EHCI; full/low-speed devices get released per-port later. */
    owr(h, EH_CFGFLAG, 1);

    /* Port power on — harmless on always-powered ports. */
    for (int p = 0; p < h->nports; p++)
        psc_set(h, p, psc(h, p) | PS_PP);
    msleep(20);

    /* Run: async + periodic schedules, 1024-entry list, ITC=8 (one
     * interrupt batch per 1 ms frame). */
    owr(h, EH_USBCMD, CMD_RS | CMD_ASE | CMD_PSE | CMD_ITC(8));
    msleep(2);
    if (ord(h, EH_USBSTS) & STS_HCH) {
        klog_puts(KLOG_USB, KLOG_WARN, "ehci: hc halted after run\n");
        return -1;
    }
    for (int i = 0; i < 20 &&
         !(ord(h, EH_USBSTS) & (STS_ASS | STS_PSS)); i++)
        msleep(1);

    klog_puts(KLOG_USB, KLOG_DEBUG, "ehci: ");
    klog_appendf(KLOG_USB, KLOG_DEBUG, "%u", (uint32_t)bus);
    klog_puts(KLOG_USB, KLOG_DEBUG, ":");
    klog_appendf(KLOG_USB, KLOG_DEBUG, "%u", (uint32_t)dev);
    klog_puts(KLOG_USB, KLOG_DEBUG, ".");
    klog_appendf(KLOG_USB, KLOG_DEBUG, "%u", (uint32_t)fn);
    klog_puts(KLOG_USB, KLOG_DEBUG, " ports=");
    klog_appendf(KLOG_USB, KLOG_DEBUG, "%u", (uint32_t)h->nports);
    klog_puts(KLOG_USB, KLOG_DEBUG, " sts=");
    klog_appendf(KLOG_USB, KLOG_DEBUG, "0x%08X", ord(h, EH_USBSTS));
    klog_puts(KLOG_USB, KLOG_DEBUG, "\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public entry points                                                 */
/* ------------------------------------------------------------------ */
int EHCI_Init(void)
{
    int found = 0;
    for (uint16_t b = 0; b < 256 && g_nhc < MAX_EHCI; b++)
        for (uint8_t d = 0; d < 32 && g_nhc < MAX_EHCI; d++)
            for (uint8_t f = 0; f < 8 && g_nhc < MAX_EHCI; f++) {
                uint32_t id = pci_r32((uint8_t)b, d, f, 0x00);
                if (id == 0xFFFFFFFF) { if (f == 0) break; continue; }
                if ((pci_r32((uint8_t)b, d, f, 0x08) >> 8) != 0x0C0320u)
                    continue;                      /* serial/USB/EHCI */

                EhciHc *h = &g_hc[g_nhc];
                memset(h, 0, sizeof(*h));
                h->pub.name = "ehci";
                h->pub.priv = h;
                h->pub.control        = ehci_control;
                h->pub.intr_in        = ehci_intr_in;
                h->pub.bulk           = ehci_bulk;
                h->pub.port_connected = ehci_port_connected;
                h->pub.port_reset     = ehci_port_reset;
                h->pub.port_csc       = ehci_port_csc;
                h->pub.port_status    = ehci_port_status;
                h->irq_vec = -1;

                if (ehci_controller_init(h, (uint8_t)b, d, f) != 0) {
                    klog_puts(KLOG_USB, KLOG_WARN,
                              "ehci: init failed\n");
                    continue;
                }
                h->pub.nports = h->nports;
                g_driven[g_nhc].b = (uint8_t)b;
                g_driven[g_nhc].d = d;
                g_driven[g_nhc].f = f;
                USB_RegisterHc(&h->pub);
                g_nhc++;
                found++;
                klog_puts(KLOG_USB, KLOG_INFO, "ehci up\n");
            }
    return found;
}

/* C:usbdiag snapshot — reads only (UAOS-317). */
int EHCI_DiagCount(void) { return g_nhc; }

int EHCI_DiagRead(int idx, EhciDiag *d)
{
    if (idx < 0 || idx >= g_nhc) return -1;
    EhciHc *h = &g_hc[idx];
    memset(d, 0, sizeof(*d));
    d->bus = h->bus; d->dev = h->dev; d->fn = h->fn;
    d->nports  = (uint8_t)h->nports;
    d->irq_vec = h->irq_vec;
    d->irq_hits = h->irq_hits;
    d->usbcmd  = ord(h, EH_USBCMD);
    d->usbsts  = ord(h, EH_USBSTS);
    d->cfgflag = ord(h, EH_CFGFLAG);
    for (int i = 0; i < h->nports && i < 8; i++)
        d->portsc[i] = psc(h, i);
    for (int i = 0; i < EHCI_MAX_INTR; i++)
        if (h->intr[i].qh) d->npipes++;
    return 0;
}

/* Attach INTx for each HC — called from kernel main after IRQ_Init,
 * same contract as UHCI_SetupIRQs. */
void EHCI_SetupIRQs(void)
{
    for (int i = 0; i < g_nhc; i++) {
        EhciHc *h = &g_hc[i];
        int vec = IRQ_AttachPCI(h->bus, h->dev, h->fn,
                                ehci_irq_handler, "ehci");
        h->irq_vec = vec;
        if (vec >= 0) {
            /* Route live — enable interrupt generation now. */
            owr(h, EH_USBSTS, STS_W1C_ALL);
            owr(h, EH_USBINTR, INTR_ALL);
        }
        klog_puts(KLOG_USB, KLOG_DEBUG, "ehci: irq vec=");
        klog_appendf(KLOG_USB, KLOG_DEBUG, "%u", (uint32_t)vec);
        klog_puts(KLOG_USB, KLOG_DEBUG, "\n");
    }
}
