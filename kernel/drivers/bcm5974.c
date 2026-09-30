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
 * Finger reports become absolute pointer motion on g_mouse; the
 * physical click button arrives on the separate button endpoint.
 */

#include "usb.h"
#include "../irq/ps2mouse.h"
#include "../display/cursor.h"
#include "../klog/klog.h"
#include "../dos/dma.h"
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
        return -1;
    }
    data[WS_UM_SWITCH_IDX] = on ? WS_UM_SWITCH_ON : WS_UM_SWITCH_OFF;

    /* write it back */
    if (usb_ctrl(dev, USB_RT_OUT | USB_RT_CLASS | USB_RT_IF,
                 WS_MODE_WRITE_REQ, WS_UM_REQ_VAL, WS_UM_REQ_IDX,
                 data, WS_UM_SIZE) != 0) {
        klog_puts(KLOG_USB, KLOG_WARN, "bcm5974: mode write failed\n");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Trackpad report — header + N finger blocks                          */
/* ------------------------------------------------------------------ */
static void bcm5974_tp_cb(void *ctx, void *buf, int len)
{
    (void)ctx;
    uint8_t *d = (uint8_t *)buf;

    /* 2-byte packets are control-response noise — ignore */
    if (len <= 2) return;
    if (len < TP_HEADER_T1 || (len - TP_HEADER_T1) % TP_FSIZE_T1 != 0)
        return;

    int nfingers = (len - TP_HEADER_T1) / TP_FSIZE_T1;
    int mx = (int)g_fb_width_irq  - 1;
    int my = (int)g_fb_height_irq - 1;

    /* Single-finger pointer: first finger with a real touch reading */
    for (int i = 0; i < nfingers; i++) {
        const uint8_t *f = d + TP_HEADER_T1 + i * TP_FSIZE_T1;
        if (rd16s(f + TF_TOUCH_MAJ) == 0)
            continue;                       /* finger lifted */

        int ax = rd16s(f + TF_ABS_X);
        int ay = rd16s(f + TF_ABS_Y);       /* y is inverted vs screen */

        /* map pad coords → screen coords */
        int sx = (ax - WS2_X_MIN) * mx / (WS2_X_MAX - WS2_X_MIN);
        int sy = (WS2_Y_MAX - ay) * my / (WS2_Y_MAX - WS2_Y_MIN);
        if (sx < 0) sx = 0; else if (sx > mx) sx = mx;
        if (sy < 0) sy = 0; else if (sy > my) sy = my;

        g_mouse.x = sx;
        g_mouse.y = sy;
        Cursor_Move(sx, sy);
        g_tp.had_finger = 1;
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
    g_mouse.btn_left = d[1] ? 1 : 0;
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
