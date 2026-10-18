/* usbhub.c — UAOS USB hub class driver (UAOS-134)
 *
 * Binds to hub-class interfaces (0x09) and publishes each hub as a
 * pseudo-HC: the UsbHc vtable forwards all DMA work (control and
 * interrupt-IN transfers) to the real host controller while the port
 * helpers are implemented with hub-class requests — GET_STATUS /
 * SET_FEATURE / CLEAR_FEATURE on the hub's downstream ports.  The USB
 * core then enumerates children through the same enumerate_port() it
 * uses for root ports: USB_Init's `i < g_nhcs` loop picks up the
 * freshly registered pseudo-HC in the same pass, and the usb-enum task
 * watches hub ports for late attaches exactly like root ports.
 *
 * Change notification uses the hub's status-change interrupt endpoint
 * (UAOS-314): its report is a bitmap — bit N flags downstream port N
 * (1-based; bit 0 is the hub itself and is ignored).  The IRQ-context
 * callback only ORs report bytes into per-hub state, and the enum
 * task's port watch spends a GET_STATUS transfer just on flagged
 * ports instead of polling every downstream port every scan.
 *
 * USB addressing is flat — a child talks on the hub's bus with its own
 * address, so control/interrupt transfers need no topology knowledge
 * in the HC; only dev->speed matters (the TD low-speed bit for a
 * low-speed child behind the hub) and that comes from the hub port's
 * status word rather than the root PORTSC.
 *
 * Covers full-speed USB 1.1 hubs: the MBP4,1's internal BCM2046
 * (Bluetooth HCI and the Apple IR receiver hang off its downstream
 * ports) and QEMU's usb-hub (0409:55aa), which QEMU auto-inserts for
 * USB devices not pinned to explicit root ports, are both this kind.
 * USB2 transaction-translator (single/multi-TT) handling is EHCI
 * territory and out of scope here.
 */

#include "usb.h"
#include "../klog/klog.h"
#include "../dos/dma.h"
#include "../exec/task.h"
#include <string.h>

/* Hub descriptor (class-specific, device recipient) */
#define HUB_DESC_TYPE       0x29

/* Port feature selectors (spec table: CLEAR/SET_FEATURE wValue) */
#define HUB_FEAT_PORT_RESET      4
#define HUB_FEAT_PORT_POWER      8
#define HUB_FEAT_C_CONNECTION   16
#define HUB_FEAT_C_ENABLE       17
#define HUB_FEAT_C_SUSPEND      18
#define HUB_FEAT_C_OVER_CURRENT 19
#define HUB_FEAT_C_RESET        20

/* GET_STATUS port reply: wPortStatus in the low word, wPortChange in
 * the high word, packed into one uint32. */
#define HUB_PS_CONNECTION   0x00000001u
#define HUB_PS_ENABLE       0x00000002u
#define HUB_PS_POWER        0x00000100u
#define HUB_PS_LOW_SPEED    0x00000200u
#define HUB_PC_CONNECTION   0x00010000u
#define HUB_PC_ENABLE       0x00020000u
#define HUB_PC_SUSPEND      0x00040000u
#define HUB_PC_OVER_CURRENT 0x00080000u
#define HUB_PC_RESET        0x00100000u

#define HUB_PC_ALL      (HUB_PC_CONNECTION | HUB_PC_ENABLE | \
                         HUB_PC_SUSPEND | HUB_PC_OVER_CURRENT | \
                         HUB_PC_RESET)
/* Everything but C_RESET — hub_port_reset owns that latch. */
#define HUB_PC_NO_RESET (HUB_PC_ALL & ~HUB_PC_RESET)

#define HUB_MAX             4

typedef struct {
    UsbHc    pub;          /* pseudo-HC — MUST be first member */
    UsbDev  *hubdev;       /* the hub's own device; class reqs go here */
    UsbHc   *root;         /* real HC that performs the DMA */
    /* Status-change EP state (UAOS-314): change_bits accumulates the
     * interrupt report bitmap (bit N = downstream port N, 1-based; bit
     * 0 is the hub itself, ignored).  conn[] caches the CONNECTION bit
     * per 0-based port so port_connected needs no transfer while the
     * pipe is armed. */
    volatile uint32_t change_bits;
    uint8_t  conn[USB_MAX_PORTS];
    uint8_t *intr_buf;     /* DMA buffer for the report */
    uint8_t  intr_ok;      /* status-change EP armed */
} UsbHub;

static UsbHub  g_hubs[HUB_MAX];
static int     g_nhubs;

/* Shared 4-byte status buffer: port helpers run only on the boot
 * thread or the single usb-enum task (never IRQ context), and the
 * control transfer is synchronous, so one buffer serves every hub. */
static uint8_t *g_stbuf;

static void hub_msleep(uint32_t ms)
{
    /* Same yield-vs-spin split as usb.c's usb_msleep. */
    if (Task_Current()) {
        Task_SleepTicks((ms + 9) / 10);
        return;
    }
    for (uint32_t i = 0; i < ms; i++)
        for (volatile int j = 0; j < 8000; j++)
            __asm__ volatile("inb $0x80, %%al" ::: "eax");
}

/* Status-change report handler — runs in IRQ/poll context (the pipe is
 * scanned from UHCI_Poll on the PIT tick or the INTx dispatch), so it
 * only folds the bitmap into change_bits; all transfers happen later
 * in hub_port_csc from the usb-enum task.  Bitmap is up to 4 bytes —
 * bit 0 is the hub itself and stays ignored by the consumers. */
static void hub_intr_cb(void *ctx, void *buf, int len)
{
    UsbHub *hb = (UsbHub *)ctx;
    const uint8_t *b = (const uint8_t *)buf;
    uint32_t bits = 0;
    for (int i = 0; i < len && i < 4; i++)
        bits |= (uint32_t)b[i] << (i * 8);
    if (bits)
        __atomic_fetch_or(&hb->change_bits, bits, __ATOMIC_RELAXED);
}

/* wPortChange bit -> CLEAR_FEATURE selector.  The hub keeps flagging a
 * port in its status-change bitmap while ANY latched change bit is
 * set, so a flagged port needs every pending change acked, not just
 * the connection one. */
static const struct { uint32_t bit; uint16_t feat; } k_change_feats[] = {
    { HUB_PC_CONNECTION,   HUB_FEAT_C_CONNECTION   },
    { HUB_PC_ENABLE,       HUB_FEAT_C_ENABLE       },
    { HUB_PC_SUSPEND,      HUB_FEAT_C_SUSPEND      },
    { HUB_PC_OVER_CURRENT, HUB_FEAT_C_OVER_CURRENT },
    { HUB_PC_RESET,        HUB_FEAT_C_RESET        },
};

static void hub_port_ack(UsbHub *hb, int port, int st, uint32_t mask)
{
    for (unsigned i = 0; i < sizeof(k_change_feats) /
                           sizeof(k_change_feats[0]); i++) {
        if (!(st & mask & k_change_feats[i].bit)) continue;
        usb_ctrl(hb->hubdev, USB_RT_OUT | USB_RT_CLASS | USB_RT_OTHER,
                 USB_REQ_CLEAR_FEATURE, k_change_feats[i].feat,
                 (uint16_t)(port + 1), 0, 0);
    }
}

/* GET_STATUS (class, other-recipient) on a hub port — port numbers on
 * the wire are 1-based, our UsbHc port indices are 0-based.  Returns
 * the packed status word or <0 on transfer failure. */
static int hub_pstat(UsbHub *hb, int port)
{
    if (!g_stbuf)
        g_stbuf = (uint8_t *)DMA_Alloc(4, 4);
    if (!g_stbuf) return -1;
    int r = usb_ctrl(hb->hubdev,
                     USB_RT_IN | USB_RT_CLASS | USB_RT_OTHER,
                     USB_REQ_GET_STATUS, 0, (uint16_t)(port + 1),
                     g_stbuf, 4);
    if (r != 0) return -1;
    return (int)((uint32_t)g_stbuf[0]        | ((uint32_t)g_stbuf[1] << 8) |
                 ((uint32_t)g_stbuf[2] << 16) | ((uint32_t)g_stbuf[3] << 24));
}

static int hub_port_connected(UsbHc *pub, int port)
{
    UsbHub *hb = (UsbHub *)pub;
    /* With the status-change EP armed the cached bit is authoritative —
     * an attach/detach lands as a change report that refreshes conn[]
     * through hub_port_csc first.  This is what takes the per-port
     * GET_STATUS out of the 100 ms enum scan (UAOS-314). */
    if (hb->intr_ok)
        return hb->conn[port];
    int st = hub_pstat(hb, port);
    if (st < 0) return -1;
    hb->conn[port] = (st & HUB_PS_CONNECTION) ? 1 : 0;
    return hb->conn[port];
}

/* Latched connect-change read for the deferred enum task — the hub
 * reports C_PORT_CONNECTION in wPortChange; clear it (the equivalent
 * of PORTSC.CSC W1C) so the edge isn't seen twice.  With the interrupt
 * pipe armed the report bitmap says which port changed: test-and-clear
 * bit port+1, then one GET_STATUS both refreshes the cached CONNECTION
 * bit and names the change bits — all of them get acked or the hub
 * re-flags the port on the next report. */
static int hub_port_csc(UsbHc *pub, int port)
{
    UsbHub *hb = (UsbHub *)pub;
    if (hb->intr_ok) {
        uint32_t bit = 1u << (port + 1);
        uint32_t prev = __atomic_fetch_and(&hb->change_bits, ~bit,
                                           __ATOMIC_RELAXED);
        if (!(prev & bit)) return 0;
        int st = hub_pstat(hb, port);
        if (st < 0) {
            __atomic_fetch_or(&hb->change_bits, bit, __ATOMIC_RELAXED);
            return 0;
        }
        hb->conn[port] = (st & HUB_PS_CONNECTION) ? 1 : 0;
        hub_port_ack(hb, port, st, HUB_PC_ALL);
        return (st & HUB_PC_CONNECTION) ? 1 : 0;
    }
    int st = hub_pstat(hb, port);
    if (st < 0) return 0;
    hb->conn[port] = (st & HUB_PS_CONNECTION) ? 1 : 0;
    /* No interrupt feed, so a status read is the only chance to retire
     * the noise latches (C_ENABLE/C_SUSPEND/C_OVER_CURRENT) as well —
     * C_RESET stays with hub_port_reset. */
    hub_port_ack(hb, port, st, HUB_PC_NO_RESET);
    return (st & HUB_PC_CONNECTION) ? 1 : 0;
}

static int hub_port_reset(UsbHc *pub, int port)
{
    UsbHub *hb = (UsbHub *)pub;
    int st = hub_pstat(hb, port);
    if (st < 0 || !(st & HUB_PS_CONNECTION)) return -1;

    usb_ctrl(hb->hubdev, USB_RT_OUT | USB_RT_CLASS | USB_RT_OTHER,
             USB_REQ_SET_FEATURE, HUB_FEAT_PORT_RESET,
             (uint16_t)(port + 1), 0, 0);

    /* The hub performs the reset asynchronously and latches
     * C_PORT_RESET when done (~10 ms worst case; QEMU is faster). */
    int done = 0;
    for (int i = 0; i < 30; i++) {
        hub_msleep(10);
        st = hub_pstat(hb, port);
        if (st < 0) return -1;
        if (st & HUB_PC_RESET) { done = 1; break; }
    }
    /* Ack the change bit whether or not it was seen — a wedged latch
     * makes every later status read look fresh. */
    usb_ctrl(hb->hubdev, USB_RT_OUT | USB_RT_CLASS | USB_RT_OTHER,
             USB_REQ_CLEAR_FEATURE, HUB_FEAT_C_RESET,
             (uint16_t)(port + 1), 0, 0);
    if (!done) return -1;

    /* Reset-recovery settle — same margin as the root-hub path. */
    hub_msleep(50);

    st = hub_pstat(hb, port);
    if (st >= 0)
        hb->conn[port] = (st & HUB_PS_CONNECTION) ? 1 : 0;
    if (st < 0 || !(st & HUB_PS_CONNECTION) || !(st & HUB_PS_ENABLE))
        return -1;
    return (st & HUB_PS_LOW_SPEED) ? USB_SPEED_LOW : USB_SPEED_FULL;
}

/* ------------------------------------------------------------------ */
/* DMA work — forwarded to the real host controller.  The child's     */
/* UsbDev carries the address/speed the HC needs; nothing about the   */
/* transfer itself is hub-specific.                                    */
/* ------------------------------------------------------------------ */
static int hub_control(UsbHc *pub, UsbDev *dev, uint8_t ep,
                       uint8_t bmRT, uint8_t bReq, uint16_t wVal,
                       uint16_t wIdx, void *data, uint16_t len)
{
    UsbHub *hb = (UsbHub *)pub;
    return hb->root->control(hb->root, dev, ep, bmRT, bReq, wVal,
                             wIdx, data, len);
}

static int hub_intr_in(UsbHc *pub, UsbDev *dev, uint8_t ep, uint16_t mps,
                       void *buf, uint16_t buflen,
                       void (*cb)(void *ctx, void *buf, int len),
                       void *ctx)
{
    UsbHub *hb = (UsbHub *)pub;
    return hb->root->intr_in(hb->root, dev, ep, mps, buf, buflen, cb, ctx);
}

static int hub_bulk(UsbHc *pub, UsbDev *dev, uint8_t ep, int dir_in,
                    void *data, uint32_t len, uint16_t mps,
                    int *toggle, uint32_t *actual)
{
    UsbHub *hb = (UsbHub *)pub;
    if (!hb->root->bulk) return -1;
    return hb->root->bulk(hb->root, dev, ep, dir_in, data, len, mps,
                          toggle, actual);
}

/* ------------------------------------------------------------------ */
/* Class probe                                                        */
/* ------------------------------------------------------------------ */
static int hub_probe(UsbIf *ifc)
{
    if (ifc->cls != USB_CLASS_HUB) return 0;
    if (g_nhubs >= HUB_MAX) return 0;
    UsbDev *dev = ifc->dev;

    /* Hub descriptor (type 0x29, device recipient): bNbrPorts at
     * offset 2, bPwrOn2PwrGood (2 ms units) at offset 5 — everything
     * we use is inside the 9-byte fixed header, so request exactly
     * that.  Over-reading stalls on real silicon: the MBP4,1's BCM2046
     * returned two data packets then STALLed the surplus IN tokens of
     * a 64-byte read (QEMU's hub tolerated it).  One retry covers a
     * hub still waking up after SET_CONFIGURATION. */
    uint8_t *hd = (uint8_t *)DMA_Alloc(64, 8);
    if (!hd) return 0;
    int r = -1;
    for (int t = 0; t < 2 && r != 0; t++) {
        if (t) hub_msleep(30);
        r = usb_ctrl(dev, USB_RT_IN | USB_RT_CLASS | USB_RT_DEV,
                     USB_REQ_GET_DESCRIPTOR, HUB_DESC_TYPE << 8, 0,
                     hd, 9);
    }
    if (r != 0 || hd[0] < 9 || hd[1] != HUB_DESC_TYPE) {
        DMA_Free(hd, 64);
        return 0;
    }
    int nports = hd[2];
    uint32_t pwron_ms = (uint32_t)(hd[5] ? hd[5] : 50) * 2;
    DMA_Free(hd, 64);

    if (nports < 1) return 0;
    if (nports > USB_MAX_PORTS) nports = USB_MAX_PORTS;

    UsbHub *hb = &g_hubs[g_nhubs];
    memset(hb, 0, sizeof(*hb));
    hb->hubdev = dev;
    hb->root   = dev->hc;
    hb->pub.name = "usbhub";
    hb->pub.priv = hb;
    hb->pub.control        = hub_control;
    hb->pub.intr_in        = hub_intr_in;
    hb->pub.bulk           = hub_bulk;
    hb->pub.port_connected = hub_port_connected;
    hb->pub.port_reset     = hub_port_reset;
    hb->pub.port_csc       = hub_port_csc;
    hb->pub.nports         = nports;

    /* Power every downstream port: a no-op on hubs without per-port
     * switching; ganged-power hubs switch on the first request. */
    for (int p = 0; p < nports; p++)
        usb_ctrl(dev, USB_RT_OUT | USB_RT_CLASS | USB_RT_OTHER,
                 USB_REQ_SET_FEATURE, HUB_FEAT_PORT_POWER,
                 (uint16_t)(p + 1), 0, 0);
    hub_msleep(pwron_ms);

    /* Snapshot each port's CONNECTION bit — while the status-change EP
     * is armed, port_connected answers from this cache and the enum
     * scan stops polling every port with GET_STATUS (UAOS-314). */
    for (int p = 0; p < nports; p++) {
        int st = hub_pstat(hb, p);
        if (st >= 0)
            hb->conn[p] = (st & HUB_PS_CONNECTION) ? 1 : 0;
    }

    /* Arm the status-change interrupt EP when the interface exposes
     * one.  The report is a bitmap (bit N = port N changed; the bitmap
     * is ceil((nports+1)/8) bytes, the pipe polls it at int_mps with a
     * floor of 1).  hub_intr_cb only accumulates it in IRQ context —
     * hub_port_csc spends transfers just on flagged ports.  A port that
     * changed between the snapshot and the arm arrives as the pipe's
     * first report, so no edge is lost. */
    if (ifc->int_ep) {
        uint16_t mps = ifc->int_mps ? ifc->int_mps : 1;
        hb->intr_buf = (uint8_t *)DMA_Alloc(mps, 8);
        if (hb->intr_buf) {
            if (hb->root->intr_in(hb->root, dev, ifc->int_ep, mps,
                                  hb->intr_buf, mps,
                                  hub_intr_cb, hb) == 0) {
                hb->intr_ok = 1;
            } else {
                DMA_Free(hb->intr_buf, mps);
                hb->intr_buf = 0;
            }
        }
    }

    g_nhubs++;
    USB_RegisterHc(&hb->pub);

    klog_puts(KLOG_USB, KLOG_INFO, "hub: vid=");
    klog_appendf(KLOG_USB, KLOG_INFO, "0x%08X", dev->vid);
    klog_puts(KLOG_USB, KLOG_INFO, " pid=");
    klog_appendf(KLOG_USB, KLOG_INFO, "0x%08X", dev->pid);
    klog_puts(KLOG_USB, KLOG_INFO, " ports=");
    klog_appendf(KLOG_USB, KLOG_INFO, "%u", (uint32_t)nports);
    klog_puts(KLOG_USB, KLOG_INFO, " intr=");
    klog_appendf(KLOG_USB, KLOG_INFO, "%u", (uint32_t)hb->intr_ok);
    klog_puts(KLOG_USB, KLOG_INFO, "\n");
    return 1;
}

void USBHUB_Init(void)
{
    USB_RegisterClass(hub_probe);
}
