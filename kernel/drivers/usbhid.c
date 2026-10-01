/* usbhid.c — UAOS USB HID class driver (boot protocol)
 *
 * Binds to USB HID interfaces (class 0x03): protocol 1 = keyboard,
 * protocol 2 = mouse.  Requests boot protocol, then arms the
 * interrupt-IN endpoint through the host controller.
 *
 * Keyboard reports feed the shared key ring buffer (same one the PS/2
 * driver fills) so every consumer works unchanged; modifier state goes
 * into g_kbd_mods.  Mouse reports update g_mouse and move the cursor.
 *
 * The Apple internal keyboard/trackpad (05ac:0230) is a composite
 * device: interface 0 is a boot-protocol keyboard, the trackpad
 * interface needs the bcm5974 wellspring mode toggle for multitouch —
 * absolute-mode support is a separate work item (UAOS-135).
 */

#include "usb.h"
#include "../irq/ps2kbd.h"
#include "../irq/ps2mouse.h"
#include "../display/cursor.h"
#include "../exec/task.h"
#include "../klog/klog.h"
#include "../dos/dma.h"
#include <string.h>

#define HID_REQ_GET_REPORT    0x01
#define HID_REQ_SET_IDLE      0x0A
#define HID_REQ_SET_PROTOCOL  0x0B

#define HID_PROTO_BOOT        0
#define HID_REPORT_FEATURE    3

/* ------------------------------------------------------------------ */
/* HID usage → ASCII tables (US layout), usage page 0x07               */
/* ------------------------------------------------------------------ */

static const char hid_normal[128] = {
    [0x04]='a',[0x05]='b',[0x06]='c',[0x07]='d',[0x08]='e',
    [0x09]='f',[0x0A]='g',[0x0B]='h',[0x0C]='i',[0x0D]='j',
    [0x0E]='k',[0x0F]='l',[0x10]='m',[0x11]='n',[0x12]='o',
    [0x13]='p',[0x14]='q',[0x15]='r',[0x16]='s',[0x17]='t',
    [0x18]='u',[0x19]='v',[0x1A]='w',[0x1B]='x',[0x1C]='y',
    [0x1D]='z',
    [0x1E]='1',[0x1F]='2',[0x20]='3',[0x21]='4',[0x22]='5',
    [0x23]='6',[0x24]='7',[0x25]='8',[0x26]='9',[0x27]='0',
    [0x28]='\n',[0x29]=27,[0x2A]='\b',[0x2B]='\t',[0x2C]=' ',
    [0x2D]='-',[0x2E]='=',[0x2F]='[',[0x30]=']',[0x31]='\\',
    [0x33]=';',[0x34]='\'',[0x35]='`',[0x36]=',',[0x37]='.',
    [0x38]='/',
    /* keypad */
    [0x54]='/',[0x55]='*',[0x56]='-',[0x57]='+',[0x58]='\n',
    [0x59]='1',[0x5A]='2',[0x5B]='3',[0x5C]='4',[0x5D]='5',
    [0x5E]='6',[0x5F]='7',[0x60]='8',[0x61]='9',[0x62]='0',
    [0x63]='.',
};

static const char hid_shifted[128] = {
    [0x04]='A',[0x05]='B',[0x06]='C',[0x07]='D',[0x08]='E',
    [0x09]='F',[0x0A]='G',[0x0B]='H',[0x0C]='I',[0x0D]='J',
    [0x0E]='K',[0x0F]='L',[0x10]='M',[0x11]='N',[0x12]='O',
    [0x13]='P',[0x14]='Q',[0x15]='R',[0x16]='S',[0x17]='T',
    [0x18]='U',[0x19]='V',[0x1A]='W',[0x1B]='X',[0x1C]='Y',
    [0x1D]='Z',
    [0x1E]='!',[0x1F]='@',[0x20]='#',[0x21]='$',[0x22]='%',
    [0x23]='^',[0x24]='&',[0x25]='*',[0x26]='(',[0x27]=')',
    [0x2D]='_',[0x2E]='+',[0x2F]='{',[0x30]='}',[0x31]='|',
    [0x33]=':',[0x34]='"',[0x35]='~',[0x36]='<',[0x37]='>',
    [0x38]='?',
};

/* Virtual keys matching ps2kbd.h */
static char hid_vkey(uint8_t usage)
{
    switch (usage) {
    case 0x4B: return KBD_VKEY_PGUP;
    case 0x4E: return KBD_VKEY_PGDN;
    case 0x52: return KBD_VKEY_UP;
    case 0x51: return KBD_VKEY_DOWN;
    case 0x50: return KBD_VKEY_LEFT;
    case 0x4F: return KBD_VKEY_RIGHT;
    case 0x4A: return KBD_VKEY_HOME;
    case 0x4D: return KBD_VKEY_END;
    case 0x4C: return KBD_VKEY_DEL;
    case 0x3A: return 0x1C;   /* F1 — help, same mapping as PS/2 driver */
    default:   return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Per-interface state                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    UsbIf   *ifc;
    uint8_t *buf;             /* DMA report buffer */
    uint16_t buflen;
    uint8_t  prev_keys[6];    /* last keyboard report key array */
    uint8_t  is_kbd;
} HidDev;

#define HID_MAX_DEVS 8
static HidDev g_hid[HID_MAX_DEVS];
static int    g_nhid = 0;

extern unsigned int g_fb_width_irq;
extern unsigned int g_fb_height_irq;

/* ------------------------------------------------------------------ */
/* Keyboard report processing                                          */
/* ------------------------------------------------------------------ */
static void hid_push_ascii(char ascii)
{
    if (!ascii) return;

    /* Mirror the PS/2 driver's Amiga-key mapping */
    if (g_kbd_mods.super_right) {
        char upper = ascii;
        if (ascii >= 'a' && ascii <= 'z') upper = (char)(ascii - 'a' + 'A');
        if (upper >= 'A' && upper <= 'Z') {
            PS2Kbd_PushChar((char)(0x80 | (unsigned char)upper));
            return;
        }
    }
    if (g_kbd_mods.super_left) {
        char upper = ascii;
        if (ascii >= 'a' && ascii <= 'z') upper = (char)(ascii - 'a' + 'A');
        switch (upper) {
            case 'V': PS2Kbd_PushChar(AMIGA_LV); return;
            case 'B': PS2Kbd_PushChar(AMIGA_LB); return;
            case 'M': PS2Kbd_PushChar(AMIGA_LM); return;
            case 'N': PS2Kbd_PushChar(AMIGA_LN); return;
        }
    }
    PS2Kbd_PushChar(ascii);
}

static void hid_kbd_report(HidDev *hd, const uint8_t *r, int len)
{
    if (len < 8) return;

    uint8_t mods = r[0];
    g_kbd_mods.shift       = !!(mods & 0x22);
    g_kbd_mods.ctrl        = !!(mods & 0x11);
    g_kbd_mods.alt         = !!(mods & 0x44);
    g_kbd_mods.super_left  = !!(mods & 0x08);
    g_kbd_mods.super_right = !!(mods & 0x80);

    /* Newly-pressed keys = present now, absent from the previous report */
    for (int i = 2; i < 8; i++) {
        uint8_t u = r[i];
        if (!u || u == 1) continue;              /* 0 = none, 1 = rollover err */

        int was_held = 0;
        for (int j = 0; j < 6; j++)
            if (hd->prev_keys[j] == u) { was_held = 1; break; }
        if (was_held) continue;

        /* Caps Lock on keypress */
        if (u == 0x39) { g_kbd_mods.caps_lock ^= 1; continue; }

        char vk = hid_vkey(u);
        if (vk) { PS2Kbd_PushChar(vk); continue; }
        if (u >= 0x80) continue;

        int shift = g_kbd_mods.shift;
        char base = hid_normal[u];
        if (g_kbd_mods.caps_lock && base >= 'a' && base <= 'z')
            shift ^= 1;
        char ascii = shift ? hid_shifted[u] : base;

        if (g_kbd_mods.ctrl && ascii >= 'a' && ascii <= 'z')
            ascii = (char)(ascii - 'a' + 1);
        else if (g_kbd_mods.ctrl && ascii >= 'A' && ascii <= 'Z')
            ascii = (char)(ascii - 'A' + 1);

        hid_push_ascii(ascii);
    }

    /* Snapshot the pressed-key array */
    for (int j = 0; j < 6; j++)
        hd->prev_keys[j] = (j + 2 < len) ? r[j + 2] : 0;
}

/* ------------------------------------------------------------------ */
/* Mouse report (boot protocol): buttons, dx, dy                       */
/* ------------------------------------------------------------------ */
static void hid_mouse_report(const uint8_t *r, int len)
{
    if (len < 3) return;
    int dx = (int)(int8_t)r[1];
    int dy = (int)(int8_t)r[2];   /* HID +Y = down — matches screen */

    int nx = g_mouse.x + dx;
    int ny = g_mouse.y + dy;
    int mx = (int)g_fb_width_irq  - 1;
    int my = (int)g_fb_height_irq - 1;
    if (nx < 0) nx = 0; else if (nx > mx) nx = mx;
    if (ny < 0) ny = 0; else if (ny > my) ny = my;

    g_mouse.x = nx;
    g_mouse.y = ny;
    g_mouse.btn_left   = (r[0] & 0x01) ? 1 : 0;
    g_mouse.btn_right  = (r[0] & 0x02) ? 1 : 0;
    g_mouse.btn_middle = (r[0] & 0x04) ? 1 : 0;

    Cursor_Move(nx, ny);
    EventPump_Wake();
}

/* ------------------------------------------------------------------ */
/* Interrupt-IN completion callback (from UHCI poll/IRQ)               */
/* ------------------------------------------------------------------ */
static void hid_in_cb(void *ctx, void *buf, int len)
{
    HidDev *hd = (HidDev *)ctx;
    const uint8_t *r = (const uint8_t *)buf;
    if (hd->is_kbd) hid_kbd_report(hd, r, len);
    else            hid_mouse_report(r, len);
}

/* ------------------------------------------------------------------ */
/* Class probe — called by usb.c for every interface                   */
/* ------------------------------------------------------------------ */
static int hid_probe(UsbIf *ifc)
{
    if (ifc->cls != USB_CLASS_HID) return 0;
    if (ifc->proto != USB_IFPROTO_KBD && ifc->proto != USB_IFPROTO_MOUSE)
        return 0;
    if (!ifc->int_ep) return 0;
    if (g_nhid >= HID_MAX_DEVS) return 0;

    UsbDev *dev = ifc->dev;
    uint8_t ifnum = ifc->ifnum;
    int is_kbd = (ifc->proto == USB_IFPROTO_KBD);

    /* Boot protocol + zero idle (report only on change) */
    usb_ctrl(dev, USB_RT_OUT | USB_RT_CLASS | USB_RT_IF,
             HID_REQ_SET_PROTOCOL, HID_PROTO_BOOT, ifnum, 0, 0);
    usb_ctrl(dev, USB_RT_OUT | USB_RT_CLASS | USB_RT_IF,
             HID_REQ_SET_IDLE, 0, ifnum, 0, 0);

    HidDev *hd = &g_hid[g_nhid++];
    memset(hd, 0, sizeof(*hd));
    hd->ifc    = ifc;
    hd->is_kbd = (uint8_t)is_kbd;
    hd->buflen = is_kbd ? 8 : (uint16_t)(ifc->int_mps ? ifc->int_mps : 8);
    hd->buf    = (uint8_t *)DMA_Alloc(hd->buflen + 8, 64);
    if (!hd->buf) { g_nhid--; return 0; }

    if (dev->hc->intr_in(dev->hc, dev, ifc->int_ep, ifc->int_mps,
                         hd->buf, hd->buflen, hid_in_cb, hd) != 0) {
        g_nhid--;
        return 0;
    }

    klog_puts(KLOG_USB, KLOG_DEBUG, is_kbd ? "hid: kbd" : "hid: mouse");
    klog_puts(KLOG_USB, KLOG_DEBUG, " on if ");
    klog_appendf(KLOG_USB, KLOG_DEBUG, "0x%08X", ifnum);
    klog_puts(KLOG_USB, KLOG_DEBUG, "\n");
    return 1;
}

void USBHID_Init(void)
{
    USB_RegisterClass(hid_probe);
}
