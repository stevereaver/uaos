/* appletouch.c — Apple "Geyser" trackpads (Linux appletouch class)
 *
 * The internal keyboard/trackpad composite on Core 2 Duo MacBooks and
 * MacBook Pros enumerates as 05ac:0217-021c / 0229-022b (Geyser 3/4).
 * These are *not* bcm5974/Wellspring pads: by default they stream
 * HID-style mouse packets; a class control transfer switches them to
 * vendor ("raw sensor") mode, where the interrupt-IN endpoint carries
 * 64-byte frames of per-sensor readings — deltas from an untouched
 * baseline, not absolute coordinates.
 *
 * Frame layout (Geyser3/4, datalen 64 — drivers/input/mouse/appletouch.c):
 *   bytes 1..15   Y sensors, triplets "-,Y1,Y2" at j=1,4,7,10,13
 *   bytes 19..48  X sensors, triplets "-,X1,X2" at j=19,22,...,46
 *   byte  63      status: bit0 = physical button, bit2 = base update,
 *                 bit4 = from-reset
 *
 * Finger count comes from counting rising "humps" across the sensor
 * delta arrays (Jason Parekh's heuristic upstream), position from a
 * smoothed centroid.  The single mechanical button is emulated to
 * left/right/middle from the live finger count at press (UAOS-135).
 *
 * Geysers keep streaming empty packets after the first touch; Linux
 * re-issues the mode switch after ~10 idle frames to stop them —
 * mirrored here via a deferred worker (IRQ context can't do control
 * transfers).
 */

#include "usb.h"
#include "../irq/ps2mouse.h"
#include "../display/cursor.h"
#include "../klog/klog.h"
#include "../dos/dma.h"
#include "../exec/task.h"
#include <string.h>

#define APPLE_VID            0x05AC

/* Geyser mode switch (class control transfers on ep0): read config
 * (req 1), set the vendor-mode byte, write back (req 9). */
#define ATP_MODE_READ_REQ    1
#define ATP_MODE_WRITE_REQ   9
#define ATP_MODE_REQ_VAL     0x0300
#define ATP_MODE_REQ_IDX     0
#define ATP_MODE_SIZE        8
#define ATP_MODE_VENDOR      0x04

#define HID_REQ_SET_IDLE     0x0A

/* Geyser3/4 report geometry (shared across all matched PIDs) */
#define ATP_DATALEN          64
#define ATP_XSENS            20
#define ATP_YSENS            10
#define ATP_NSENS            (ATP_XSENS + ATP_YSENS)
#define ATP_XFACT            64      /* abs x range ~= xsens * xfact */
#define ATP_YFACT            64
#define ATP_XRANGE           (ATP_XSENS * ATP_XFACT)
#define ATP_YRANGE           (ATP_YSENS * ATP_YFACT)

#define ATP_STATUS_BUTTON    0x01
#define ATP_STATUS_BASE_UPD  0x04

#define ATP_THRESHOLD        5       /* sensor delta noise floor */
#define ATP_SCALE            12      /* fixed-point shift for smoothing */
#define ATP_SMOOTHSZ         34      /* nb_sensors + 4 pad either side */
#define ATP_IDLE_FRAMES      10      /* empties before re-init */

extern unsigned int g_fb_width_irq;
extern unsigned int g_fb_height_irq;

typedef struct {
    int      claimed;
    uint8_t *buf;
    UsbDev  *dev;
    UaosTask *reinit_task;
    int      reinit_pending;
    int      reinits;           /* mode re-switch attempts so far */
    int      idle;              /* consecutive fingerless frames */

    int8_t   base[ATP_NSENS];   /* untouched-pad baseline */
    int      acc [ATP_NSENS];   /* per-sensor deltas */
    int      smooth[ATP_SMOOTHSZ];
    int      smooth_tmp[ATP_SMOOTHSZ];

    int      had_finger;
    int      px, py;            /* last tracked pad position */
    int      fingers;           /* live finger census */
    int      btn_emu;           /* latched emulated button (0..3) */
} Atp;

static Atp g_atp;

static void udelay(unsigned int us)
{
    while (us--)
        __asm__ volatile("inb $0x80, %%al" ::: "eax");
}
static void msleep(uint32_t ms) { udelay(ms * 1000); }

/* ------------------------------------------------------------------ */
/* Vendor-mode switch                                                   */
/* ------------------------------------------------------------------ */
static int atp_vendor_mode(UsbDev *dev, int on)
{
    uint8_t *data = (uint8_t *)DMA_Alloc(ATP_MODE_SIZE, 8);
    if (!data) return -1;

    if (usb_ctrl(dev, USB_RT_IN | USB_RT_CLASS | USB_RT_IF,
                 ATP_MODE_READ_REQ, ATP_MODE_REQ_VAL, ATP_MODE_REQ_IDX,
                 data, ATP_MODE_SIZE) != 0) {
        klog_puts(KLOG_USB, KLOG_WARN, "appletouch: mode read failed\n");
        goto fail;
    }

    /* Write + verify loop — Geyser ACKs a write it doesn't apply, same
     * class of race bcm5974 shows (drained control response). */
    for (int attempt = 0; attempt < 4; attempt++) {
        data[0] = on ? ATP_MODE_VENDOR : 0;
        if (usb_ctrl(dev, USB_RT_OUT | USB_RT_CLASS | USB_RT_IF,
                     ATP_MODE_WRITE_REQ, ATP_MODE_REQ_VAL, ATP_MODE_REQ_IDX,
                     data, ATP_MODE_SIZE) != 0) {
            klog_puts(KLOG_USB, KLOG_WARN, "appletouch: mode write failed\n");
            goto fail;
        }
        msleep(20);
        memset(data, 0, ATP_MODE_SIZE);
        if (usb_ctrl(dev, USB_RT_IN | USB_RT_CLASS | USB_RT_IF,
                     ATP_MODE_READ_REQ, ATP_MODE_REQ_VAL, ATP_MODE_REQ_IDX,
                     data, ATP_MODE_SIZE) != 0) {
            klog_puts(KLOG_USB, KLOG_WARN, "appletouch: mode vfy read failed\n");
            goto fail;
        }
        if (data[0] == (uint8_t)(on ? ATP_MODE_VENDOR : 0)) {
            DMA_Free(data, ATP_MODE_SIZE);
            return 0;
        }
        msleep(50);
    }
    klog_puts(KLOG_USB, KLOG_WARN, "appletouch: mode switch did not stick\n");
fail:
    DMA_Free(data, ATP_MODE_SIZE);
    return -1;
}

/* ------------------------------------------------------------------ */
/* Re-init worker — the pad keeps streaming empty frames after a touch  */
/* ends; re-issuing the mode switch silences it (upstream atp_reinit).  */
/* ------------------------------------------------------------------ */
static void atp_reinit_task(void *arg)
{
    (void)arg;
    for (;;) {
        Task_WaitTicks(SIGF_ATP, UINT64_MAX / 2);
        if (!g_atp.claimed || !g_atp.dev)
            continue;
        g_atp.reinit_pending = 0;
        klog_puts(KLOG_USB, KLOG_DEBUG, "appletouch: reinit #");
        klog_appendf(KLOG_USB, KLOG_DEBUG, "0x%08X", (uint32_t)g_atp.reinits);
        klog_puts(KLOG_USB, KLOG_DEBUG, "\n");
        atp_vendor_mode(g_atp.dev, 1);
        memset(g_atp.base, 0, sizeof(g_atp.base));
        g_atp.had_finger = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Sensor deltas → finger humps + smoothed centroid                     */
/* (atp_calculate_abs port; returns position in sensor*fact units)      */
/* ------------------------------------------------------------------ */
static int atp_humps(const int *s, int n)
{
    int fingers = 0, increasing = 0;

    for (int i = 0; i < n; i++) {
        if (s[i] < ATP_THRESHOLD) {
            increasing = 0;
        } else if (i < 1 || (!increasing && s[i - 1] < s[i])) {
            fingers++;
            increasing = 1;
        } else if (i > 0 && s[i - 1] - s[i] > ATP_THRESHOLD) {
            increasing = 0;
        }
    }
    return fingers;
}

static int atp_centroid(const int *s, int n, int fact, int *z)
{
    int pcum = 0, psum = 0;

    memset(g_atp.smooth, 0, 4 * sizeof(int));
    for (int i = 0; i < n; i++)
        g_atp.smooth[i + 4] = s[i] << ATP_SCALE;
    memset(&g_atp.smooth[n + 4], 0, 4 * sizeof(int));

    for (int pass = 0; pass < 4; pass++) {
        int i;
        g_atp.smooth_tmp[0] = (g_atp.smooth[0] + g_atp.smooth[1]) / 2;
        for (i = 1; i < n + 7; i++)
            g_atp.smooth_tmp[i] = (g_atp.smooth[i - 1] +
                                   g_atp.smooth[i] * 2 +
                                   g_atp.smooth[i + 1]) / 4;
        g_atp.smooth_tmp[i] = (g_atp.smooth[i - 1] + g_atp.smooth[i]) / 2;
        memcpy(g_atp.smooth, g_atp.smooth_tmp, sizeof(g_atp.smooth));
    }

    for (int i = 0; i < n + 8; i++)
        if ((g_atp.smooth[i] >> ATP_SCALE) > 0) {
            pcum += g_atp.smooth[i] * i;
            psum += g_atp.smooth[i];
        }
    if (psum <= 0) return 0;
    *z = psum >> ATP_SCALE;
    return pcum * fact / psum;
}

/* ------------------------------------------------------------------ */
/* Report handler — IRQ/poll context                                    */
/* ------------------------------------------------------------------ */
static void atp_cb(void *ctx, void *buf, int len)
{
    (void)ctx;
    uint8_t *d = (uint8_t *)buf;

    static int dbg_n;
    if (dbg_n < 24) {
        dbg_n++;
        klog_puts(KLOG_USB, KLOG_DEBUG, "appletouch: rx len=");
        klog_appendf(KLOG_USB, KLOG_DEBUG, "0x%08X", (uint32_t)len);
        klog_puts(KLOG_USB, KLOG_DEBUG, " d0=");
        klog_appendf(KLOG_USB, KLOG_DEBUG, "0x%08X",
                     len >= 4 ? (uint32_t)(d[0] | (d[1]<<8) |
                              (d[2]<<16) | ((uint32_t)d[3]<<24)) : 0);
        klog_puts(KLOG_USB, KLOG_DEBUG, "\n");
    }

    /* Short frames (e.g. the 8-byte Report-ID-2 HID packets the pad
     * emits pre-switch) mean vendor mode didn't stick — kick the
     * worker to re-switch.  IRQ context; the worker does the control
     * transfers. */
    if (len != ATP_DATALEN) {
        if (len > 2 && g_atp.reinit_task && !g_atp.reinit_pending &&
            g_atp.reinits < 16) {
            g_atp.reinits++;
            g_atp.reinit_pending = 1;
            Signal(g_atp.reinit_task, SIGF_ATP);
        }
        return;
    }
    uint8_t status = d[ATP_DATALEN - 1];
    int     key    = status & ATP_STATUS_BUTTON;

    /* Baseline refresh frames carry the untouched pad — snapshot and
     * stop (they must not count toward the idle reinit). */
    if (status & ATP_STATUS_BASE_UPD) {
        int i, j;
        for (i = 0, j = 1; i < ATP_YSENS - 1; i += 2, j += 3) {
            g_atp.base[ATP_XSENS + i]     = (int8_t)d[j + 1];
            g_atp.base[ATP_XSENS + i + 1] = (int8_t)d[j + 2];
        }
        for (i = 0, j = 19; i < ATP_XSENS; i += 2, j += 3) {
            g_atp.base[i]     = (int8_t)d[j + 1];
            g_atp.base[i + 1] = (int8_t)d[j + 2];
        }
        return;
    }

    /* Per-sensor deltas vs baseline (round-robin wrap handling, clamp
     * negatives — mirrors upstream). */
    {
        int8_t cur[ATP_NSENS];
        int i, j;
        for (i = 0, j = 1; i < ATP_YSENS - 1; i += 2, j += 3) {
            cur[ATP_XSENS + i]     = (int8_t)d[j + 1];
            cur[ATP_XSENS + i + 1] = (int8_t)d[j + 2];
        }
        for (i = 0, j = 19; i < ATP_XSENS; i += 2, j += 3) {
            cur[i]     = (int8_t)d[j + 1];
            cur[i + 1] = (int8_t)d[j + 2];
        }
        for (i = 0; i < ATP_NSENS; i++) {
            int delta = cur[i] - g_atp.base[i];
            if (delta > 127)  delta -= 256;
            if (delta < -127) delta += 256;
            g_atp.acc[i] = delta < 0 ? 0 : delta;
        }
    }

    int x_f = atp_humps(g_atp.acc, ATP_XSENS);
    int y_f = atp_humps(g_atp.acc + ATP_XSENS, ATP_YSENS);
    int fingers = x_f > y_f ? x_f : y_f;
    int prev_fingers = g_atp.fingers;
    g_atp.fingers = fingers;

    int x = 0, y = 0, x_z = 0, y_z = 0;
    if (fingers) {
        x = atp_centroid(g_atp.acc, ATP_XSENS, ATP_XFACT, &x_z);
        y = atp_centroid(g_atp.acc + ATP_XSENS, ATP_YSENS, ATP_YFACT, &y_z);
    }

    /* Motion: re-base on touch start or finger-count change, else
     * apply the pad delta scaled to ~1.5 screen widths per sweep
     * (same feel as bcm5974).  Pad y runs inverted vs screen. */
    if (x && y) {
        if (!g_atp.had_finger || fingers != prev_fingers) {
            g_atp.px = x;
            g_atp.py = y;
            g_atp.had_finger = 1;
        } else {
            int dx = x - g_atp.px;
            int dy = g_atp.py - y;
            int mx = (int)g_fb_width_irq  - 1;
            int my = (int)g_fb_height_irq - 1;
            g_atp.px = x;
            g_atp.py = y;

            int nx = g_mouse.x + dx * 3 * mx / (2 * ATP_XRANGE);
            int ny = g_mouse.y + dy * 3 * my / (2 * ATP_YRANGE);
            if (nx < 0) nx = 0; else if (nx > mx) nx = mx;
            if (ny < 0) ny = 0; else if (ny > my) ny = my;
            if (nx != g_mouse.x || ny != g_mouse.y) {
                g_mouse.x = nx;
                g_mouse.y = ny;
                Cursor_Move(nx, ny);
            }
        }
    } else {
        g_atp.had_finger = 0;
    }

    /* Button emulation: same latch as bcm5974 — the census at the
     * press edge picks 1=left, 2=right, 3+=middle, held until
     * release.  Here the button rides the same report, so the census
     * and click are always consistent. */
    if (key) {
        if (!g_atp.btn_emu)
            g_atp.btn_emu = fingers >= 3 ? 3 : fingers == 2 ? 2 : 1;
    } else {
        g_atp.btn_emu = 0;
    }
    g_mouse.btn_left   = (g_atp.btn_emu == 1);
    g_mouse.btn_right  = (g_atp.btn_emu == 2);
    g_mouse.btn_middle = (g_atp.btn_emu == 3);
    EventPump_Wake();

    /* Stream silence: after a run of empty frames kick the worker to
     * re-issue the mode switch so the pad stops pacing the HC. */
    if (!fingers && !key && g_atp.reinit_task && !g_atp.reinit_pending) {
        if (++g_atp.idle >= ATP_IDLE_FRAMES) {
            g_atp.idle = 0;
            g_atp.reinit_pending = 1;
            Signal(g_atp.reinit_task, SIGF_ATP);
        }
    } else if (fingers || key) {
        g_atp.idle = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Class probe — claims the trackpad interface (the HID proto-mouse    */
/* interface with the large report endpoint) of matched Geyser devices. */
/* ------------------------------------------------------------------ */
static int atp_probe(UsbIf *ifc)
{
    static const uint16_t pids[] = {
        0x0217, 0x0218, 0x0219,         /* Geyser 3 */
        0x021a, 0x021b, 0x021c,         /* Geyser 4 */
        0x0229, 0x022a, 0x022b,         /* Geyser 4 HF */
    };
    UsbDev *dev = ifc->dev;
    int match = 0;

    if (dev->vid != APPLE_VID) return 0;
    for (unsigned int i = 0; i < sizeof(pids) / sizeof(pids[0]); i++)
        if (dev->pid == pids[i]) { match = 1; break; }
    if (!match) return 0;
    if (ifc->proto != USB_IFPROTO_MOUSE || !ifc->int_ep) return 0;
    if (g_atp.claimed) return 0;

    usb_ctrl(dev, USB_RT_OUT | USB_RT_CLASS | USB_RT_IF,
             HID_REQ_SET_IDLE, 0, ifc->ifnum, 0, 0);
    msleep(10);

    /* Vendor mode before traffic starts.  Best-effort: a Geyser that
     * applies the write but doesn't echo 0x04 on readback would fail
     * verification — arming anyway costs nothing (the pipe-side retry
     * in atp_cb re-switches if raw frames never come) and keeps us
     * from losing the pad to a cosmetic verify quirk. */
    atp_vendor_mode(dev, 1);

    g_atp.buf = (uint8_t *)DMA_Alloc(ATP_DATALEN, 64);
    if (!g_atp.buf) return 0;
    if (dev->hc->intr_in(dev->hc, dev, ifc->int_ep, ifc->int_mps,
                         g_atp.buf, ATP_DATALEN, atp_cb, 0) != 0)
        return 0;
    g_atp.dev = dev;
    g_atp.claimed = 1;
    klog_puts(KLOG_USB, KLOG_DEBUG, "appletouch: trackpad armed\n");
    return 1;
}

void AppleTouch_Init(void)
{
    memset(&g_atp, 0, sizeof(g_atp));
    USB_RegisterClass(atp_probe);
}

/* Called after TaskScheduler_Init — see bcm5974 for why the worker is
 * spawned late. */
void AppleTouch_StartWorker(void)
{
    if (g_atp.dev && !g_atp.reinit_task)
        g_atp.reinit_task = Task_CreateNative("appletouch-reinit", 0,
                                              atp_reinit_task, 0);
}
