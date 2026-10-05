/* usb.c — UAOS USB core: enumeration and class binding
 *
 * Walks registered host controllers, resets ports with devices
 * attached, assigns addresses, reads descriptors, configures the
 * device, and offers each interface to registered class drivers
 * (HID).
 */

#include "usb.h"
#include "../klog/klog.h"
#include "../dos/dma.h"
#include "../exec/task.h"
#include <string.h>

#define USB_MAX_DEVS   16
#define USB_MAX_IFS    32
#define USB_MAX_CLASSES 4
#define USB_MAX_HCS     8
#define USB_MAX_PORTS   8

static UsbDev g_devs[USB_MAX_DEVS];
static UsbIf  g_ifs [USB_MAX_IFS];
static int    g_ndevs = 0;
static int    g_nifs  = 0;

static usb_class_probe_fn g_classes[USB_MAX_CLASSES];
static int g_nclasses = 0;

static UsbHc *g_hcs[USB_MAX_HCS];
static int    g_nhcs = 0;

extern volatile uint64_t g_pit_ticks;      /* 100 Hz */

/* ------------------------------------------------------------------ */
/* Standard request wrapper                                            */
/* ------------------------------------------------------------------ */
int usb_ctrl(UsbDev *dev, uint8_t bmRT, uint8_t bReq,
             uint16_t wVal, uint16_t wIdx, void *data, uint16_t len)
{
    if (!dev || !dev->hc || !dev->hc->control) return -1;
    return dev->hc->control(dev->hc, dev, 0, bmRT, bReq, wVal, wIdx,
                            data, len);
}

void USB_RegisterClass(usb_class_probe_fn probe)
{
    if (g_nclasses < USB_MAX_CLASSES)
        g_classes[g_nclasses++] = probe;
}

int USB_DeviceCount(void) { return g_ndevs; }

void USB_RegisterHc(UsbHc *hc)
{
    if (g_nhcs < USB_MAX_HCS)
        g_hcs[g_nhcs++] = hc;
}

static void usb_msleep(uint32_t ms)
{
    /* In task context (the deferred enum task, UAOS-258) yield to the
     * scheduler instead of busy-spinning; before the scheduler exists
     * there is nothing to switch to, so spin on port-0x80 reads. */
    if (Task_Current()) {
        Task_SleepTicks((ms + 9) / 10);   /* 100 Hz tick */
        return;
    }
    for (uint32_t i = 0; i < ms; i++)
        for (volatile int j = 0; j < 8000; j++)
            __asm__ volatile("inb $0x80, %%al" ::: "eax");
}

/* ------------------------------------------------------------------ */
/* Descriptor fetch                                                    */
/* ------------------------------------------------------------------ */
static int get_desc(UsbDev *dev, uint8_t dtype, uint8_t idx,
                    void *buf, uint16_t len)
{
    return usb_ctrl(dev, USB_RT_IN | USB_RT_STD | USB_RT_DEV,
                    USB_REQ_GET_DESCRIPTOR,
                    (uint16_t)(dtype << 8) | idx, 0, buf, len);
}

/* ------------------------------------------------------------------ */
/* Config descriptor parse → fill g_ifs                                */
/* ------------------------------------------------------------------ */
static void parse_config(UsbDev *dev, const uint8_t *buf, uint16_t len)
{
    uint16_t off = 0;
    UsbIf *cur = 0;

    while (off + 2 <= len) {
        uint8_t blen = buf[off];
        uint8_t btype = buf[off + 1];
        if (blen < 2) break;

        if (btype == USB_DESC_INTERFACE && blen >= sizeof(UsbIfDesc)) {
            const UsbIfDesc *id = (const UsbIfDesc *)(buf + off);
            if (g_nifs < USB_MAX_IFS) {
                cur = &g_ifs[g_nifs++];
                memset(cur, 0, sizeof(*cur));
                cur->dev   = dev;
                cur->ifnum = id->bInterfaceNumber;
                cur->cls   = id->bInterfaceClass;
                cur->sub   = id->bInterfaceSubClass;
                cur->proto = id->bInterfaceProtocol;
                klog_puts(KLOG_USB, KLOG_DEBUG, "usb: if cls=");
                klog_appendf(KLOG_USB, KLOG_DEBUG, "0x%08X", cur->cls);
                klog_puts(KLOG_USB, KLOG_DEBUG, " sub=");
                klog_appendf(KLOG_USB, KLOG_DEBUG, "0x%08X", cur->sub);
                klog_puts(KLOG_USB, KLOG_DEBUG, " proto=");
                klog_appendf(KLOG_USB, KLOG_DEBUG, "0x%08X", cur->proto);
                klog_puts(KLOG_USB, KLOG_DEBUG, "\n");
            }
        } else if (btype == USB_DESC_ENDPOINT && cur &&
                   blen >= sizeof(UsbEpDesc)) {
            const UsbEpDesc *ed = (const UsbEpDesc *)(buf + off);
            uint8_t xfer = ed->bmAttributes & 0x3;
            klog_puts(KLOG_USB, KLOG_DEBUG, "usb: ep if=");
            klog_appendf(KLOG_USB, KLOG_DEBUG, "0x%08X", cur->ifnum);
            klog_puts(KLOG_USB, KLOG_DEBUG, " addr=");
            klog_appendf(KLOG_USB, KLOG_DEBUG, "0x%08X",
                         ed->bEndpointAddress);
            klog_puts(KLOG_USB, KLOG_DEBUG, " mps=");
            klog_appendf(KLOG_USB, KLOG_DEBUG, "0x%08X",
                         ed->wMaxPacketSize);
            klog_puts(KLOG_USB, KLOG_DEBUG, "\n");
            if ((ed->bEndpointAddress & USB_EP_DIR_IN) &&
                xfer == USB_EP_XFER_INT && !cur->int_ep) {
                cur->int_ep = ed->bEndpointAddress & 0x0F;
                cur->int_mps = ed->wMaxPacketSize;
                cur->int_interval = ed->bInterval;
            }
        }
        off += blen;
    }
}

/* ------------------------------------------------------------------ */
/* Enumerate one port — returns 1 when a device was bound              */
/* ------------------------------------------------------------------ */
static int enumerate_port(UsbHc *hc, int port)
{
    int speed = hc->port_reset(hc, port);
    if (speed < 0) return 0;

    if (g_ndevs >= USB_MAX_DEVS) return 0;
    UsbDev *dev = &g_devs[g_ndevs];
    memset(dev, 0, sizeof(*dev));
    dev->hc     = hc;
    dev->port   = port;
    dev->addr   = 0;
    dev->speed  = (uint8_t)speed;
    dev->ep0_mps = 8;                    /* safe default for addr 0 */

    UsbDeviceDesc *dd = (UsbDeviceDesc *)DMA_Alloc(256, 64);
    if (!dd) return 0;

    /* First 8 bytes of the device descriptor → ep0 max packet.
     * Devices (hubs especially) need recovery time after port reset —
     * the spec minimum is 10 ms but real hardware often wants more.
     * Retry with a settle delay rather than abandoning enumeration.
     * A device left wedged by the firmware or a bouncing attach can
     * stay deaf through the whole first round — a second port reset
     * is the classic recovery (UAOS-225), so the retries are split
     * across two resets before giving up. */
    int ok = 0;
    for (int round = 0; round < 2 && !ok; round++) {
        if (round) {
            int s2 = hc->port_reset(hc, port);
            if (s2 < 0) break;                 /* device went away */
            dev->speed = (uint8_t)s2;
        }
        for (int attempt = 0; attempt < 4; attempt++) {
            if (get_desc(dev, USB_DESC_DEVICE, 0, dd, 8) == 0) {
                ok = 1;
                break;
            }
            usb_msleep(100);
        }
    }
    if (!ok) {
        klog_puts(KLOG_USB, KLOG_WARN, "usb: GET_DESCRIPTOR(8) failed\n");
        goto out_dd;
    }
    dev->ep0_mps = dd->bMaxPacketSize0 ? dd->bMaxPacketSize0 : 8;

    /* Assign an address (simple counter starting at 1) */
    uint8_t addr = (uint8_t)(g_ndevs + 1);
    if (usb_ctrl(dev, USB_RT_OUT | USB_RT_STD | USB_RT_DEV,
                 USB_REQ_SET_ADDRESS, addr, 0, 0, 0) != 0) {
        klog_puts(KLOG_USB, KLOG_WARN, "usb: SET_ADDRESS failed\n");
        goto out_dd;
    }
    usb_msleep(2);
    dev->addr = addr;

    /* Full device descriptor */
    if (get_desc(dev, USB_DESC_DEVICE, 0, dd,
                 sizeof(UsbDeviceDesc)) != 0) {
        klog_puts(KLOG_USB, KLOG_WARN, "usb: GET_DESCRIPTOR(18) failed\n");
        goto out_dd;
    }
    dev->vid      = dd->idVendor;
    dev->pid      = dd->idProduct;
    dev->devclass = dd->bDeviceClass;
    dev->name[0] = 'u'; dev->name[1] = 's'; dev->name[2] = 'b';
    dev->name[3] = (char)('0' + (addr % 10)); dev->name[4] = 0;

    klog_puts(KLOG_USB, KLOG_DEBUG, "usb: dev vid=");
    klog_appendf(KLOG_USB, KLOG_DEBUG, "0x%08X", dev->vid);
    klog_puts(KLOG_USB, KLOG_DEBUG, " pid=");
    klog_appendf(KLOG_USB, KLOG_DEBUG, "0x%08X", dev->pid);
    klog_puts(KLOG_USB, KLOG_DEBUG, "\n");

    /* Config descriptor — header first, then the whole blob.
     * cd is freed on every path — re-enumeration must not leak 4 KB
     * per device per attempt (UAOS-173). */
    UsbConfigDesc *cd = (UsbConfigDesc *)DMA_Alloc(4096, 64);
    if (!cd) goto out_dd;
    if (get_desc(dev, USB_DESC_CONFIG, 0, cd,
                 sizeof(UsbConfigDesc)) != 0) goto out_cd;
    uint16_t total = cd->wTotalLength;
    if (total > 4096) total = 4096;
    if (get_desc(dev, USB_DESC_CONFIG, 0, cd, total) != 0) goto out_cd;

    int ifbase = g_nifs;
    parse_config(dev, (const uint8_t *)cd, total);

    /* Set configuration (first config value) */
    if (cd->bConfigurationValue &&
        usb_ctrl(dev, USB_RT_OUT | USB_RT_STD | USB_RT_DEV,
                 USB_REQ_SET_CONFIG, cd->bConfigurationValue,
                 0, 0, 0) != 0) {
        klog_puts(KLOG_USB, KLOG_WARN, "usb: SET_CONFIGURATION failed\n");
        goto out_cd;
    }
    usb_msleep(2);
    g_ndevs++;
    DMA_Free(cd, 4096);
    DMA_Free(dd, 256);

    /* Bind class drivers to each interface */
    for (int i = ifbase; i < g_nifs; i++) {
        UsbIf *ifc = &g_ifs[i];
        if (ifc->used) continue;
        for (int c = 0; c < g_nclasses && !ifc->used; c++)
            if (g_classes[c](ifc))
                ifc->used = 1;
    }
    return 1;

out_cd:
    DMA_Free(cd, 4096);
out_dd:
    DMA_Free(dd, 256);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Deferred enumeration state (UAOS-258) — see usb_enum_task below     */
/* ------------------------------------------------------------------ */
#define USB_ENUM_SCAN_TICKS    10     /* 100 ms port scan cadence */
#define USB_ENUM_MAX_ATTEMPTS  5      /* retries before parking */

typedef struct {
    UsbHc   *hc;
    int      port;
    uint8_t  last_ccs;      /* CCS seen on the previous scan */
    uint8_t  attempts;      /* retries consumed since the last edge */
    uint64_t next_tick;     /* g_pit_ticks when the next retry is due */
} UsbPortWatch;

static UsbPortWatch g_watch[USB_MAX_HCS][USB_MAX_PORTS];

/* ------------------------------------------------------------------ */
/* USB_Init — enumerate every registered host controller               */
/* ------------------------------------------------------------------ */
int USB_Init(void)
{
    /* Host controllers register themselves via USB_RegisterHc() from
     * their own init (UHCI_Init is called by the boot code first). */
    int total = 0;
    for (int i = 0; i < g_nhcs; i++) {
        UsbHc *hc = g_hcs[i];
        for (int p = 0; p < hc->nports; p++) {
            if (hc->port_connected(hc, p) > 0)
                total += enumerate_port(hc, p);
        }
    }
    /* Record every port in the watch table so the deferred enum task
     * can retry the deaf ones and pick up post-boot attaches. */
    for (int i = 0; i < g_nhcs; i++) {
        UsbHc *hc = g_hcs[i];
        for (int p = 0; p < hc->nports && p < USB_MAX_PORTS; p++) {
            UsbPortWatch *w = &g_watch[i][p];
            w->hc        = hc;
            w->port      = p;
            w->last_ccs  = (uint8_t)(hc->port_connected(hc, p) > 0);
            w->attempts  = 0;
            w->next_tick = 0;
        }
    }
    klog_puts(KLOG_USB, KLOG_DEBUG, "usb: ");
    klog_appendf(KLOG_USB, KLOG_DEBUG, "0x%08X", g_ndevs);
    klog_puts(KLOG_USB, KLOG_DEBUG, " devices, ");
    klog_appendf(KLOG_USB, KLOG_DEBUG, "0x%08X", g_nifs);
    klog_puts(KLOG_USB, KLOG_DEBUG, " ifs\n");
    return total;
}

/* ------------------------------------------------------------------ */
/* Deferred enumeration task (UAOS-258)                                */
/*                                                                     */
/* Boot enumeration is one-shot: a port reporting connected that never */
/* ACKs SETUP (SMC-gated silicon like the MBP4,1 Bluetooth/IR, a hub   */
/* port that comes online late) — or a device hotplugged after boot —  */
/* is permanently invisible: USB_Poll runs in IRQ context and only     */
/* drains interrupt pipes.  This task re-probes connected-but-         */
/* unenumerated ports with bounded exponential backoff (1..16 s), then */
/* parks the port until a connect-status edge revives it.              */
/*                                                                     */
/* Removal isn't tracked: a port whose device bound earlier keeps its  */
/* (now stale) UsbDev, so unplug→replug on an enumerated port does not */
/* re-enumerate — hot-remove/teardown is a separate problem.           */
/* ------------------------------------------------------------------ */
static int port_has_dev(UsbHc *hc, int port)
{
    for (int i = 0; i < g_ndevs; i++)
        if (g_devs[i].hc == hc && g_devs[i].port == port &&
            g_devs[i].addr)
            return 1;
    return 0;
}

static void usb_enum_task(void *arg)
{
    (void)arg;
    for (;;) {
        for (int i = 0; i < g_nhcs; i++) {
            UsbHc *hc = g_hcs[i];
            for (int p = 0; p < hc->nports && p < USB_MAX_PORTS; p++) {
                UsbPortWatch *w = &g_watch[i][p];
                int ccs  = hc->port_connected(hc, p) > 0;
                /* A latched CSC or a raw CCS flip both count as edges —
                 * either revives a parked port for another round. */
                int edge = (ccs != w->last_ccs) ||
                           (hc->port_csc && hc->port_csc(hc, p) > 0);
                w->last_ccs = (uint8_t)ccs;
                if (edge) {
                    w->attempts  = 0;
                    w->next_tick = 0;
                }
                if (!ccs || port_has_dev(hc, p) ||
                    w->attempts >= USB_ENUM_MAX_ATTEMPTS ||
                    g_pit_ticks < w->next_tick)
                    continue;
                if (enumerate_port(hc, p)) {
                    /* Name the device — on the MBP4,1 a port that
                     * answers late is the disambiguating evidence for
                     * which SMC-gated peripheral lives there. */
                    UsbDev *nd = &g_devs[g_ndevs - 1];
                    klog_puts(KLOG_USB, KLOG_INFO, "usb: late enum hc=");
                    klog_appendf(KLOG_USB, KLOG_INFO, "0x%08X", i);
                    klog_puts(KLOG_USB, KLOG_INFO, " port=");
                    klog_appendf(KLOG_USB, KLOG_INFO, "0x%08X", p);
                    klog_puts(KLOG_USB, KLOG_INFO, " vid=");
                    klog_appendf(KLOG_USB, KLOG_INFO, "0x%08X", nd->vid);
                    klog_puts(KLOG_USB, KLOG_INFO, " pid=");
                    klog_appendf(KLOG_USB, KLOG_INFO, "0x%08X", nd->pid);
                    klog_puts(KLOG_USB, KLOG_INFO, "\n");
                } else {
                    w->attempts++;
                    w->next_tick = g_pit_ticks +
                                   (100ull << (w->attempts - 1));
                    if (w->attempts >= USB_ENUM_MAX_ATTEMPTS) {
                        klog_puts(KLOG_USB, KLOG_WARN,
                                  "usb: port deaf after retries — "
                                  "parked until connect edge hc=");
                        klog_appendf(KLOG_USB, KLOG_WARN, "0x%08X", i);
                        klog_puts(KLOG_USB, KLOG_WARN, " port=");
                        klog_appendf(KLOG_USB, KLOG_WARN, "0x%08X", p);
                        klog_puts(KLOG_USB, KLOG_WARN, "\n");
                    }
                }
            }
        }
        Task_SleepTicks(USB_ENUM_SCAN_TICKS);
    }
}

void USB_StartEnumTask(void)
{
    if (g_nhcs)
        Task_CreateNative("usb-enum", 0, usb_enum_task, 0);
}

void USB_Poll(void)
{
    UHCI_Poll();
}
