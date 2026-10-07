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
 * Faithful port of drivers/input/mouse/appletouch.c (Geyser 3/4 path:
 * atp_geyser_init, atp_calculate_abs, atp_complete_geyser_3_4,
 * atp_reinit).  Linux reports absolute coordinates and leaves relative
 * conversion to userspace; here the 7/8-EMA absolute position is
 * differenced into cursor motion directly.
 *
 * Frame layout (datalen 64):
 *   bytes 1..15   Y sensors, triplets "-,Y1,Y2" at j=1,4,7,10,13
 *   bytes 19..48  X sensors, triplets "-,X1,X2" at j=19,22,...,46
 *   byte  63      status: bit0 = physical button, bit2 = base update,
 *                 bit4 = from-reset
 *
 * The single mechanical button is emulated to left/right/middle from
 * the live finger count at press (UAOS-135).
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
#define ATP_STATUS_FROM_RST  0x10

#define ATP_THRESHOLD        5       /* sensor delta noise floor */
#define ATP_SCALE            12      /* fixed-point shift for smoothing */
#define ATP_SMOOTHSZ         34      /* nb_sensors + 4 pad either side */
#define ATP_IDLE_FRAMES      10      /* empties before re-init */
#define ATP_MOVE_TH          2       /* filtered pad units before cursor moves */

/* Cursor gain: screen px per pad unit, 8.8 fixed point — ~1.5 screen
 * widths per full pad sweep (same feel as bcm5974). */
#define ATP_GAIN_FP(px, rng) (((px) * 3 * 256) / (2 * (rng)))

extern unsigned int g_fb_width_irq;
extern unsigned int g_fb_height_irq;

typedef struct {
    int      claimed;
    uint8_t *buf;
    UsbDev  *dev;
    UaosTask *reinit_task;
    int      reinit_pending;
    int      reinits;           /* mode re-switch attempts so far */
    int      saw_raw;           /* first 64-byte frame seen */
    int      base_valid;        /* baseline seeded */
    int      idlecount;         /* consecutive empty frames */

    int8_t   xy_old[ATP_NSENS]; /* untouched-pad baseline */
    int      xy_acc[ATP_NSENS]; /* per-sensor deltas */
    int      smooth[ATP_SMOOTHSZ];
    int      smooth_tmp[ATP_SMOOTHSZ];

    int      x_old, y_old;      /* EMA'd absolute position, -1 = none */
    int      fingers_old;
    int      adx, ady;          /* accumulating deadband (pad units) */
    int      rem_x, rem_y;      /* sub-pixel remainder, 8.8 */
    int      btn_emu;           /* latched emulated button (0..3) */

    /* Telemetry (peek): status-frame counters and last-32 trajectory */
    int      n_base_upd, n_from_reset, n_frames;
    int      dbg_ring[32][6];   /* rx, ry, fx, fy, fingers|xz<<8|yz<<20, seq */
    int      dbg_ridx;
} Atp;

static Atp g_atp;

static void udelay(unsigned int us)
{
    while (us--)
        __asm__ volatile("inb $0x80, %%al" ::: "eax");
}
static void msleep(uint32_t ms) { udelay(ms * 1000); }

/* ------------------------------------------------------------------ */
/* Vendor-mode switch — atp_geyser_init: one read, one write.          */
/* ------------------------------------------------------------------ */
static int atp_geyser_init(UsbDev *dev)
{
    uint8_t *data = (uint8_t *)DMA_Alloc(ATP_MODE_SIZE, 8);
    int ret = -1;
    if (!data) return -1;

    if (usb_ctrl(dev, USB_RT_IN | USB_RT_CLASS | USB_RT_IF,
                 ATP_MODE_READ_REQ, ATP_MODE_REQ_VAL, ATP_MODE_REQ_IDX,
                 data, ATP_MODE_SIZE) != 0) {
        klog_puts(KLOG_USB, KLOG_WARN, "appletouch: mode read failed\n");
        goto out;
    }
    data[0] = ATP_MODE_VENDOR;
    if (usb_ctrl(dev, USB_RT_OUT | USB_RT_CLASS | USB_RT_IF,
                 ATP_MODE_WRITE_REQ, ATP_MODE_REQ_VAL, ATP_MODE_REQ_IDX,
                 data, ATP_MODE_SIZE) != 0) {
        klog_puts(KLOG_USB, KLOG_WARN, "appletouch: mode write failed\n");
        goto out;
    }
    ret = 0;
out:
    DMA_Free(data, ATP_MODE_SIZE);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Re-init worker — atp_reinit.  Geysers 3/4 keep streaming after the  */
/* first touch; re-issuing the mode switch puts the pad to sleep.      */
/* ------------------------------------------------------------------ */
static void atp_reinit_task(void *arg)
{
    (void)arg;
    for (;;) {
        Task_WaitTicks(SIGF_ATP, UINT64_MAX / 2);
        if (!g_atp.claimed || !g_atp.dev)
            continue;
        g_atp.reinits++;
        atp_geyser_init(g_atp.dev);
        g_atp.reinit_pending = 0;
    }
}

/* ------------------------------------------------------------------ */
/* atp_calculate_abs — hump census + smoothed centroid                 */
/* ------------------------------------------------------------------ */
static int atp_calculate_abs(int offset, int nb_sensors, int fact,
                             int *z, int *fingers)
{
    int *xy_sensors = g_atp.xy_acc + offset;
    int pcum = 0, psum = 0;
    int is_increasing = 0;
    int i, pass;

    *fingers = 0;
    for (i = 0; i < nb_sensors; i++) {
        if (xy_sensors[i] < ATP_THRESHOLD) {
            if (is_increasing)
                is_increasing = 0;
        } else if (i < 1 ||
                   (!is_increasing && xy_sensors[i - 1] < xy_sensors[i])) {
            (*fingers)++;
            is_increasing = 1;
        } else if (i > 0 &&
                   (xy_sensors[i - 1] - xy_sensors[i] > ATP_THRESHOLD)) {
            is_increasing = 0;
        }
    }
    if (*fingers < 1)
        return 0;

    memset(g_atp.smooth, 0, 4 * sizeof(int));
    for (i = 0; i < nb_sensors; i++)
        g_atp.smooth[i + 4] = xy_sensors[i] << ATP_SCALE;
    memset(&g_atp.smooth[nb_sensors + 4], 0, 4 * sizeof(int));

    for (pass = 0; pass < 4; pass++) {
        g_atp.smooth_tmp[0] = (g_atp.smooth[0] + g_atp.smooth[1]) / 2;
        for (i = 1; i < nb_sensors + 7; i++)
            g_atp.smooth_tmp[i] = (g_atp.smooth[i - 1] +
                                   g_atp.smooth[i] * 2 +
                                   g_atp.smooth[i + 1]) / 4;
        g_atp.smooth_tmp[i] = (g_atp.smooth[i - 1] + g_atp.smooth[i]) / 2;
        memcpy(g_atp.smooth, g_atp.smooth_tmp, sizeof(g_atp.smooth));
    }

    for (i = 0; i < nb_sensors + 8; i++) {
        if ((g_atp.smooth[i] >> ATP_SCALE) > 0) {
            pcum += g_atp.smooth[i] * i;
            psum += g_atp.smooth[i];
        }
    }
    if (psum > 0) {
        *z = psum >> ATP_SCALE;
        return pcum * fact / psum;
    }
    return 0;
}

/* Relative motion from the EMA'd absolute position (what userspace
 * does with ABS_X/ABS_Y on Linux), with sub-pixel remainder carry. */
static void atp_move(int dx, int dy)
{
    int mx = (int)g_fb_width_irq  - 1;
    int my = (int)g_fb_height_irq - 1;

    g_atp.rem_x += dx * ATP_GAIN_FP(mx, ATP_XRANGE);
    g_atp.rem_y += dy * ATP_GAIN_FP(my, ATP_YRANGE);
    int px = g_atp.rem_x / 256, py = g_atp.rem_y / 256;
    g_atp.rem_x -= px * 256;
    g_atp.rem_y -= py * 256;
    if (!px && !py) return;

    int nx = g_mouse.x + px, ny = g_mouse.y + py;
    if (nx < 0) nx = 0; else if (nx > mx) nx = mx;
    if (ny < 0) ny = 0; else if (ny > my) ny = my;
    if (nx != g_mouse.x || ny != g_mouse.y) {
        g_mouse.x = nx;
        g_mouse.y = ny;
        Cursor_Move(nx, ny);
    }
}

/* ------------------------------------------------------------------ */
/* Report handler — atp_complete_geyser_3_4 (IRQ/poll context)         */
/* ------------------------------------------------------------------ */
static void atp_cb(void *ctx, void *buf, int len)
{
    (void)ctx;
    uint8_t *d = (uint8_t *)buf;
    int x, y, x_z = 0, y_z = 0, x_f, y_f;
    int i, j, key, fingers;

    /* Short frames before the first raw report mean vendor mode
     * didn't stick — kick the worker to re-switch.  Once raw frames
     * are flowing, non-64-byte packets are ignored. */
    if (len != ATP_DATALEN) {
        if (!g_atp.saw_raw && len > 2 && g_atp.reinit_task &&
            !g_atp.reinit_pending && g_atp.reinits < 16) {
            g_atp.reinit_pending = 1;
            Signal(g_atp.reinit_task, SIGF_ATP);
        }
        return;
    }
    g_atp.saw_raw = 1;
    g_atp.n_frames++;
    uint8_t status = d[ATP_DATALEN - 1];
    if (status & ATP_STATUS_FROM_RST) g_atp.n_from_reset++;

    int8_t xy_cur[ATP_NSENS];
    for (i = 0, j = 19; i < ATP_XSENS; i += 2, j += 3) {
        xy_cur[i]     = (int8_t)d[j + 1];
        xy_cur[i + 1] = (int8_t)d[j + 2];
    }
    for (i = 0, j = 1; i < ATP_YSENS - 1; i += 2, j += 3) {
        xy_cur[ATP_XSENS + i]     = (int8_t)d[j + 1];
        xy_cur[ATP_XSENS + i + 1] = (int8_t)d[j + 2];
    }

    /* Baseline: BASE_UPDATE frames carry the untouched pad (Linux
     * takes the baseline only from these).  Metal fallback: if none
     * has arrived yet, seed from the first raw frame after arm. */
    if (status & ATP_STATUS_BASE_UPD) {
        g_atp.n_base_upd++;
        memcpy(g_atp.xy_old, xy_cur, sizeof(g_atp.xy_old));
        g_atp.base_valid = 1;
        return;
    }
    if (!g_atp.base_valid) {
        memcpy(g_atp.xy_old, xy_cur, sizeof(g_atp.xy_old));
        g_atp.base_valid = 1;
        return;
    }

    for (i = 0; i < ATP_NSENS; i++) {
        g_atp.xy_acc[i] = xy_cur[i] - g_atp.xy_old[i];
        if (g_atp.xy_acc[i] > 127)  g_atp.xy_acc[i] -= 256;
        if (g_atp.xy_acc[i] < -127) g_atp.xy_acc[i] += 256;
        if (g_atp.xy_acc[i] < 0)    g_atp.xy_acc[i] = 0;
    }

    x = atp_calculate_abs(0, ATP_XSENS, ATP_XFACT, &x_z, &x_f);
    y = atp_calculate_abs(ATP_XSENS, ATP_YSENS, ATP_YFACT, &y_z, &y_f);
    int xr = x, yr = y;
    key = status & ATP_STATUS_BUTTON;
    fingers = x_f > y_f ? x_f : y_f;

    if (x && y && fingers == g_atp.fingers_old) {
        if (g_atp.x_old != -1) {
            x = (g_atp.x_old * 7 + x) >> 3;
            y = (g_atp.y_old * 7 + y) >> 3;
            /* Motion only under single-finger contact: a second
             * finger resting for a right-click must not drag the
             * cursor (two-finger gestures are userspace on Linux).
             * Accumulating deadband: the EMA still passes ±1-2 unit
             * tremor at light touch (metal trace: raw ±15, filtered
             * ±2) — alternating deltas cancel in the accumulator so
             * a resting finger stays put, while real motion crosses
             * the band and is delivered whole. */
            if (fingers == 1) {
                g_atp.adx += x - g_atp.x_old;
                g_atp.ady += y - g_atp.y_old;
                int ax = g_atp.adx < 0 ? -g_atp.adx : g_atp.adx;
                int ay = g_atp.ady < 0 ? -g_atp.ady : g_atp.ady;
                if (ax >= ATP_MOVE_TH || ay >= ATP_MOVE_TH) {
                    atp_move(g_atp.adx, g_atp.ady);
                    g_atp.adx = g_atp.ady = 0;
                }
            }
        }
        g_atp.x_old = x;
        g_atp.y_old = y;
    } else if (!x && !y) {
        g_atp.x_old = g_atp.y_old = -1;
        g_atp.fingers_old = 0;
        g_atp.adx = g_atp.ady = 0;
        g_atp.rem_x = g_atp.rem_y = 0;
        memset(g_atp.xy_acc, 0, sizeof(g_atp.xy_acc));
    }

    if (fingers != g_atp.fingers_old) {
        g_atp.x_old = g_atp.y_old = -1;
        g_atp.adx = g_atp.ady = 0;
        g_atp.rem_x = g_atp.rem_y = 0;
    }
    g_atp.fingers_old = fingers;

    if (fingers > 0) {
        int *r = g_atp.dbg_ring[g_atp.dbg_ridx & 31];
        g_atp.dbg_ridx++;
        r[0] = xr;
        r[1] = yr;
        r[2] = g_atp.x_old;
        r[3] = g_atp.y_old;
        r[4] = fingers | (x_z << 8) | (y_z << 20);
        r[5] = g_atp.dbg_ridx;
    }

    /* Button emulation: census at the press edge picks 1=left,
     * 2=right, 3+=middle, held until release. */
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

    /* Idle: after 10 empty frames re-init so the pad stops streaming
     * (Linux: schedule_work(atp_reinit)). */
    if (!x && !y && !key) {
        if (++g_atp.idlecount == ATP_IDLE_FRAMES) {
            g_atp.idlecount = 0;
            g_atp.x_old = g_atp.y_old = -1;
            if (g_atp.reinit_task && !g_atp.reinit_pending) {
                g_atp.reinit_pending = 1;
                Signal(g_atp.reinit_task, SIGF_ATP);
            }
        }
    } else {
        g_atp.idlecount = 0;
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

    /* Vendor mode before traffic starts (best-effort: the pipe-side
     * retry in atp_cb re-switches if raw frames never come). */
    atp_geyser_init(dev);

    g_atp.buf = (uint8_t *)DMA_Alloc(ATP_DATALEN, 64);
    if (!g_atp.buf) return 0;
    if (dev->hc->intr_in(dev->hc, dev, ifc->int_ep, ifc->int_mps,
                         g_atp.buf, ATP_DATALEN, atp_cb, 0) != 0)
        return 0;
    g_atp.dev = dev;
    g_atp.claimed = 1;
    g_atp.x_old = g_atp.y_old = -1;
    klog_puts(KLOG_USB, KLOG_DEBUG, "appletouch: trackpad armed\n");
    return 1;
}

void AppleTouch_Init(void)
{
    memset(&g_atp, 0, sizeof(g_atp));
    g_atp.x_old = g_atp.y_old = -1;
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
