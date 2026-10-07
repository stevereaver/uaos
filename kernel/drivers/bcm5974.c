/* bcm5974.c — Apple Wellspring multitouch trackpad (BCM5974)
 *
 * The internal keyboard/trackpad composite device (05ac:0230 on the
 * MacBookPro4,1) presents a boot-protocol keyboard interface plus one
 * or more HID "mouse" interfaces that carry the trackpad streams.
 * In normal mode the trackpad emits nothing useful — a class control
 * message switches it into "wellspring" mode, after which the
 * interrupt-IN endpoint streams raw finger blocks.
 *
 * Ported from Linux drivers/input/mouse/bcm5974.c (TYPE1/WELLSPRING2
 * layout; TYPE2+ devices need their own config rows).
 *
 * Finger reports become relative pointer motion on g_mouse; the
 * physical click button arrives on the separate button endpoint.
 * There is only one mechanical button, so the live finger count at
 * click time emulates the missing Amiga buttons: 1 finger = left,
 * 2 = right, 3+ = middle (UAOS-135).
 */

#include "usb.h"
#include "../irq/ps2mouse.h"
#include "../display/cursor.h"
#include "../klog/klog.h"
#include "../dos/dma.h"
#include "../exec/task.h"
#include <string.h>

#define APPLE_VID            0x05AC

/* WELLSPRING2 (MacBookPro4,1 / Penryn era) product IDs */
#define WS2_ANSI             0x0230
#define WS2_ISO              0x0231
#define WS2_JIS              0x0232

/* Wellspring mode switch (class control transfers on ep0).
 * Read config (req 1), patch the switch byte, write back (req 9). */
#define WS_MODE_READ_REQ     1
#define WS_MODE_WRITE_REQ    9
#define WS_UM_SIZE           8
#define WS_UM_REQ_VAL        0x0300
#define WS_UM_REQ_IDX        0
#define WS_UM_SWITCH_IDX     0
#define WS_UM_SWITCH_ON      0x01
#define WS_UM_SWITCH_OFF     0x08

/* HID class requests (shared with usbhid.c) */
#define HID_REQ_SET_IDLE     0x0A

static void udelay(unsigned int us)
{
    while (us--)
        __asm__ volatile("inb $0x80, %%al" ::: "eax");
}
static void msleep(uint32_t ms) { udelay(ms * 1000); }

/* TYPE1 report layout (le16-aligned) */
#define TP_HEADER_T1         (13 * 2)   /* 26-byte header */
#define TP_FSIZE_T1          (14 * 2)   /* 28 bytes per finger */
#define TP_BUTTON_T1         0          /* unused — button is on bt_ep */
#define TP_DATALEN_T1        (TP_HEADER_T1 + 16 * TP_FSIZE_T1)  /* 474 */
#define TP_EP_T1             0x81
#define BT_EP_T1             0x84
#define BT_DATALEN_T1        4

/* Coordinate ranges for WELLSPRING2 (bcm5974_config_table[1]) */
#define WS2_X_MIN            (-4824)
#define WS2_X_MAX            4824
#define WS2_Y_MIN            (-172)
#define WS2_Y_MAX            4290

/* tp_finger fields (le16 offsets within a 28-byte block) */
#define TF_ORIGIN      0
#define TF_ABS_X       2
#define TF_ABS_Y       4
#define TF_TOUCH_MAJ  16

extern unsigned int g_fb_width_irq;
extern unsigned int g_fb_height_irq;

typedef struct {
    int      claimed_tp, claimed_bt;
    int      opened;             /* mode switch applied */
    uint8_t *tp_buf;             /* TP_DATALEN_T1 DMA buffer */
    uint8_t *bt_buf;             /* BT_DATALEN_T1 DMA buffer */
    int      had_finger;         /* edge detect for click/drag */
    UsbDev  *dev;                /* device for the reset worker */
    UaosTask *reset_task;        /* deferred mode-reset worker */
    int      resets;             /* mode-reset attempts so far */
    int      px, py;             /* last tracked pad position */
    int      fslot;              /* finger slot being tracked */
    int      nfingers;           /* live fingers in the latest report */
    int      btn_emu;            /* latched emulated button (0..3) */
} Bcm5974;

static Bcm5974 g_tp;

static inline int16_t rd16s(const uint8_t *p)
{
    return (int16_t)(p[0] | ((uint16_t)p[1] << 8));
}

/* ------------------------------------------------------------------ */
/* Wellspring mode switch                                              */
/* ------------------------------------------------------------------ */
static int bcm5974_wellspring_mode(UsbDev *dev, int on)
{
    uint8_t *data = (uint8_t *)DMA_Alloc(WS_UM_SIZE, 8);
    if (!data) return -1;

    /* read config block */
    if (usb_ctrl(dev, USB_RT_IN | USB_RT_CLASS | USB_RT_IF,
                 WS_MODE_READ_REQ, WS_UM_REQ_VAL, WS_UM_REQ_IDX,
                 data, WS_UM_SIZE) != 0) {
        klog_puts(KLOG_USB, KLOG_WARN, "bcm5974: mode read failed\n");
        goto fail;
    }
    /* The readback tells us whether the device understood the request —
     * a sane config block vs zeros/garbage. */
    {
        uint32_t lo = (uint32_t)(data[0] | (data[1] << 8) |
                                 (data[2] << 16) | ((uint32_t)data[3] << 24));
        uint32_t hi = (uint32_t)(data[4] | (data[5] << 8) |
                                 (data[6] << 16) | ((uint32_t)data[7] << 24));
        klog_puts(KLOG_USB, KLOG_DEBUG, "bcm5974: mode cfg=");
        klog_appendf(KLOG_USB, KLOG_DEBUG, "0x%08X", lo);
        klog_appendf(KLOG_USB, KLOG_DEBUG, ":%08X", hi);
        klog_puts(KLOG_USB, KLOG_DEBUG, "\n");
    }
    data[WS_UM_SWITCH_IDX] = on ? WS_UM_SWITCH_ON : WS_UM_SWITCH_OFF;

    /* Write it back, then verify — the device ACKs the control write
     * even when it does not apply the switch, so re-read and retry. */
    for (int attempt = 0; attempt < 4; attempt++) {
        if (usb_ctrl(dev, USB_RT_OUT | USB_RT_CLASS | USB_RT_IF,
                     WS_MODE_WRITE_REQ, WS_UM_REQ_VAL, WS_UM_REQ_IDX,
                     data, WS_UM_SIZE) != 0) {
            klog_puts(KLOG_USB, KLOG_WARN, "bcm5974: mode write failed\n");
            goto fail;
        }
        msleep(20);
        memset(data, 0, WS_UM_SIZE);
        if (usb_ctrl(dev, USB_RT_IN | USB_RT_CLASS | USB_RT_IF,
                     WS_MODE_READ_REQ, WS_UM_REQ_VAL, WS_UM_REQ_IDX,
                     data, WS_UM_SIZE) != 0) {
            klog_puts(KLOG_USB, KLOG_WARN, "bcm5974: mode vfy read failed\n");
            goto fail;
        }
        klog_puts(KLOG_USB, KLOG_DEBUG, "bcm5974: mode vfy=");
        klog_appendf(KLOG_USB, KLOG_DEBUG, "0x%08X", (uint32_t)data[WS_UM_SWITCH_IDX]);
        klog_puts(KLOG_USB, KLOG_DEBUG, "\n");
        if (data[WS_UM_SWITCH_IDX] == (uint8_t)(on ? WS_UM_SWITCH_ON : WS_UM_SWITCH_OFF)) {
            DMA_Free(data, WS_UM_SIZE);
            return 0;
        }
        data[WS_UM_SWITCH_IDX] = on ? WS_UM_SWITCH_ON : WS_UM_SWITCH_OFF;
        msleep(50);
    }
    klog_puts(KLOG_USB, KLOG_WARN, "bcm5974: mode switch did not stick\n");
fail:
    DMA_Free(data, WS_UM_SIZE);
    return -1;
}

/* ------------------------------------------------------------------ */
/* Mode-reset worker — runs in task context (control transfers are      */
/* too slow for the IRQ-side USB_Poll callback).  A mode switch sent    */
/* before the device has drained its control response is ignored: the   */
/* fix is to drop back to normal mode, wait, and switch again           */
/* (Linux commit fc1e8a6 — "bcm5974: bad trackpad package, length: 8"). */
/* ------------------------------------------------------------------ */
static void bcm5974_reset_task(void *arg)
{
    (void)arg;
    for (;;) {
        Task_WaitTicks(SIGF_TP, UINT64_MAX / 2);
        if (!g_tp.opened || !g_tp.dev)
            continue;
        g_tp.resets++;
        klog_puts(KLOG_USB, KLOG_DEBUG, "bcm5974: mode reset #");
        klog_appendf(KLOG_USB, KLOG_DEBUG, "0x%08X", (uint32_t)g_tp.resets);
        klog_puts(KLOG_USB, KLOG_DEBUG, "\n");
        if (bcm5974_wellspring_mode(g_tp.dev, 0) != 0)
            continue;
        msleep(50);                     /* spec needs ~1ms; be generous */
        bcm5974_wellspring_mode(g_tp.dev, 1);
    }
}

/* ------------------------------------------------------------------ */
/* Trackpad report — header + N finger blocks                          */
/* ------------------------------------------------------------------ */
static void bcm5974_tp_cb(void *ctx, void *buf, int len)
{
    (void)ctx;
    uint8_t *d = (uint8_t *)buf;

    /* Debug: the first few deliveries tell us whether the pipe streams
     * at all and whether the report shape matches TYPE1. */
    static int dbg_n;
    if (dbg_n < 24) {
        dbg_n++;
        klog_puts(KLOG_USB, KLOG_DEBUG, "bcm5974: tp rx len=");
        klog_appendf(KLOG_USB, KLOG_DEBUG, "0x%08X", (uint32_t)len);
        klog_puts(KLOG_USB, KLOG_DEBUG, " d0=");
        klog_appendf(KLOG_USB, KLOG_DEBUG, "0x%08X",
                     len >= 4 ? (uint32_t)(d[0] | (d[1]<<8) |
                              (d[2]<<16) | ((uint32_t)d[3]<<24)) : 0);
        klog_puts(KLOG_USB, KLOG_DEBUG, "\n");
    }

    /* 2-byte packets are control-response noise — ignore */
    if (len <= 2) return;
    if (len < TP_HEADER_T1 || (len - TP_HEADER_T1) % TP_FSIZE_T1 != 0) {
        /* Unknown small packets (typically 8-byte HID-style reports)
         * mean the mode switch was ignored — kick the reset worker.
         * We're in IRQ context; the worker does the slow control xfers. */
        if (g_tp.reset_task && g_tp.resets < 16)
            Signal(g_tp.reset_task, SIGF_TP);
        return;
    }

    int nslots = (len - TP_HEADER_T1) / TP_FSIZE_T1;
    int mx = (int)g_fb_width_irq  - 1;
    int my = (int)g_fb_height_irq - 1;

    /* Live-finger census — the button endpoint rides a separate pipe,
     * so bt_cb can't recount at click time. */
    g_tp.nfingers = 0;
    for (int i = 0; i < nslots; i++)
        if (rd16s(d + TP_HEADER_T1 + i * TP_FSIZE_T1 + TF_TOUCH_MAJ) != 0)
            g_tp.nfingers++;

    /* Single-finger pointer: first finger with a real touch reading */
    for (int i = 0; i < nslots; i++) {
        const uint8_t *f = d + TP_HEADER_T1 + i * TP_FSIZE_T1;
        if (rd16s(f + TF_TOUCH_MAJ) == 0)
            continue;                       /* finger lifted */

        int ax = rd16s(f + TF_ABS_X);
        int ay = rd16s(f + TF_ABS_Y);       /* pad y is inverted vs screen */

        /* Relative motion: finger-down — or a different finger slot
         * taking over — re-bases the origin without moving the cursor.
         * Subsequent reports move by the pad delta scaled so a full
         * pad sweep covers ~1.5 screen widths. */
        if (!g_tp.had_finger || g_tp.fslot != i) {
            g_tp.px = ax;
            g_tp.py = ay;
            g_tp.fslot = i;
            g_tp.had_finger = 1;
            return;
        }

        int dx = ax - g_tp.px;
        int dy = g_tp.py - ay;
        g_tp.px = ax;
        g_tp.py = ay;

        int nx = g_mouse.x + dx * 3 * mx / (2 * (WS2_X_MAX - WS2_X_MIN));
        int ny = g_mouse.y + dy * 3 * my / (2 * (WS2_Y_MAX - WS2_Y_MIN));
        if (nx < 0) nx = 0; else if (nx > mx) nx = mx;
        if (ny < 0) ny = 0; else if (ny > my) ny = my;

        g_mouse.x = nx;
        g_mouse.y = ny;
        Cursor_Move(nx, ny);
        EventPump_Wake();
        return;
    }
    g_tp.had_finger = 0;
}

/* ------------------------------------------------------------------ */
/* Button report — bt_data {unk, button, rel_x, rel_y}                 */
/* ------------------------------------------------------------------ */
static void bcm5974_bt_cb(void *ctx, void *buf, int len)
{
    (void)ctx;
    if (len != BT_DATALEN_T1) return;
    const uint8_t *d = (const uint8_t *)buf;

    /* Button emulation: the finger census at press time picks which
     * Amiga button to report — 1 = left, 2 = right, 3+ = middle.
     * The pick latches until release so lifting a finger mid-drag
     * can't emit a mismatched release (right-down then left-up). */
    if (d[1]) {
        if (!g_tp.btn_emu)
            g_tp.btn_emu = g_tp.nfingers >= 3 ? 3 :
                           g_tp.nfingers == 2 ? 2 : 1;
    } else {
        g_tp.btn_emu = 0;
    }
    g_mouse.btn_left   = (g_tp.btn_emu == 1);
    g_mouse.btn_right  = (g_tp.btn_emu == 2);
    g_mouse.btn_middle = (g_tp.btn_emu == 3);
    EventPump_Wake();
}

/* ------------------------------------------------------------------ */
/* Class probe — claims the trackpad/button interfaces of matched      */
/* Apple composite devices.  Registered BEFORE generic usbhid so it    */
/* wins the proto-mouse interfaces that belong to the trackpad.        */
/* ------------------------------------------------------------------ */
static int bcm5974_probe(UsbIf *ifc)
{
    UsbDev *dev = ifc->dev;
    if (dev->vid != APPLE_VID) return 0;
    if (dev->pid != WS2_ANSI && dev->pid != WS2_ISO && dev->pid != WS2_JIS)
        return 0;
    if (!ifc->int_ep) return 0;
    if (ifc->proto != USB_IFPROTO_MOUSE && ifc->proto != 0)
        return 0;                           /* leave the kbd if alone */

    uint8_t ep = USB_EP_DIR_IN | ifc->int_ep;

    if (ep == TP_EP_T1 && !g_tp.claimed_tp) {
        /* Under Linux, usbhid binds this interface too and issues
         * SET_IDLE during claim — do the same before the mode switch. */
        usb_ctrl(dev, USB_RT_OUT | USB_RT_CLASS | USB_RT_IF,
                 HID_REQ_SET_IDLE, 0, ifc->ifnum, 0, 0);
        msleep(10);

        /* Mode switch must happen before traffic starts */
        if (bcm5974_wellspring_mode(dev, 1) != 0)
            return 0;
        g_tp.opened = 1;

        g_tp.tp_buf = (uint8_t *)DMA_Alloc(TP_DATALEN_T1, 64);
        if (!g_tp.tp_buf) return 0;
        if (dev->hc->intr_in(dev->hc, dev, ifc->int_ep,
                             ifc->int_mps, g_tp.tp_buf, TP_DATALEN_T1,
                             bcm5974_tp_cb, 0) != 0)
            return 0;
        g_tp.dev = dev;
        g_tp.claimed_tp = 1;
        klog_puts(KLOG_USB, KLOG_DEBUG, "bcm5974: trackpad armed\n");
        return 1;
    }

    if (ep == BT_EP_T1 && !g_tp.claimed_bt) {
        g_tp.bt_buf = (uint8_t *)DMA_Alloc(BT_DATALEN_T1, 64);
        if (!g_tp.bt_buf) return 0;
        if (dev->hc->intr_in(dev->hc, dev, ifc->int_ep,
                             ifc->int_mps, g_tp.bt_buf, BT_DATALEN_T1,
                             bcm5974_bt_cb, 0) != 0)
            return 0;
        g_tp.claimed_bt = 1;
        klog_puts(KLOG_USB, KLOG_DEBUG, "bcm5974: button armed\n");
        return 1;
    }

    return 0;
}

void BCM5974_Init(void)
{
    memset(&g_tp, 0, sizeof(g_tp));
    USB_RegisterClass(bcm5974_probe);
}

/* 1 when a Wellspring trackpad interface was claimed — used by the
 * showconfig/version input summary (trackpad isn't a HID proto device
 * so USBHid_MouseCount() never sees it). */
int BCM5974_Present(void) { return g_tp.claimed_tp; }

/* Called after TaskScheduler_Init — USB enum runs before the scheduler
 * exists, so the reset worker must be spawned late. */
void BCM5974_StartWorker(void)
{
    if (g_tp.dev && !g_tp.reset_task)
        g_tp.reset_task = Task_CreateNative("bcm5974-reset", 0,
                                            bcm5974_reset_task, 0);
}
