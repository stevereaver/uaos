/* usb.h — UAOS USB core
 *
 * Minimal USB 1.1/2.0 host stack: enumeration, standard control
 * requests, and class-driver binding.  Host controllers (UHCI today,
 * EHCI later) register a vtable of transfer ops; the core walks ports,
 * assigns addresses, parses descriptors, and hands interfaces to
 * class drivers (HID).
 *
 * Data model:
 *   UsbHc   — one host controller (its ops do the DMA)
 *   UsbDev  — one addressed device on a port of an HC
 *   UsbIf   — one interface inside a device (class binding unit)
 */

#ifndef UAOS_USB_H
#define UAOS_USB_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Standard request/descriptor constants                               */
/* ------------------------------------------------------------------ */

#define USB_REQ_GET_STATUS      0x00
#define USB_REQ_CLEAR_FEATURE   0x01
#define USB_REQ_SET_FEATURE     0x03
#define USB_REQ_SET_ADDRESS     0x05
#define USB_REQ_GET_DESCRIPTOR  0x06
#define USB_REQ_SET_DESCRIPTOR  0x07
#define USB_REQ_GET_CONFIG      0x08
#define USB_REQ_SET_CONFIG      0x09
#define USB_REQ_GET_INTERFACE   0x0A
#define USB_REQ_SET_INTERFACE   0x0B

#define USB_DESC_DEVICE         0x01
#define USB_DESC_CONFIG         0x02
#define USB_DESC_STRING         0x03
#define USB_DESC_INTERFACE      0x04
#define USB_DESC_ENDPOINT       0x05
#define USB_DESC_HID            0x21
#define USB_DESC_REPORT         0x22

/* bmRequestType */
#define USB_RT_OUT   0x00
#define USB_RT_IN    0x80
#define USB_RT_STD   0x00
#define USB_RT_CLASS 0x20
#define USB_RT_DEV   0x00
#define USB_RT_IF    0x01
#define USB_RT_EP    0x02

#define USB_CLASS_HID        0x03
#define USB_IFPROTO_KBD      0x01
#define USB_IFPROTO_MOUSE    0x02

#define USB_EP_DIR_IN        0x80
#define USB_EP_XFER_INT      0x03
#define USB_EP_XFER_BULK     0x02
#define USB_EP_XFER_CTRL     0x00

#define USB_SPEED_LOW   0
#define USB_SPEED_FULL  1
#define USB_SPEED_HIGH  2

/* ------------------------------------------------------------------ */
/* Descriptors (packed wire format)                                    */
/* ------------------------------------------------------------------ */

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} UsbDeviceDesc;

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} UsbConfigDesc;

typedef struct __attribute__((packed)) {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} UsbIfDesc;

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} UsbEpDesc;

/* ------------------------------------------------------------------ */
/* Core object model                                                   */
/* ------------------------------------------------------------------ */

struct UsbDev;

typedef struct UsbHc {
    const char *name;
    void       *priv;

    /* Synchronous control transfer on endpoint 0 (or given ep).
     * Returns 0 on success, <0 on error/timeout: -1 for a protocol
     * level failure from a live device (STALL, babble, ...), -2 when
     * nothing answered at all (wire timeout/CRC, NAK exhaustion, or
     * the HC never ran the chain) so callers can back off sooner
     * instead of re-poking a deaf port (UAOS-262). */
    int (*control)(struct UsbHc *hc, struct UsbDev *dev, uint8_t ep,
                   uint8_t bmRequestType, uint8_t bRequest,
                   uint16_t wValue, uint16_t wIndex,
                   void *data, uint16_t wLength);

    /* Arm a persistent interrupt-IN transfer on (dev, ep).  The HC
     * invokes cb(buf, actual_len) from its IRQ/poll path whenever a
     * report arrives, then re-arms.  Reports longer than mps are
     * delivered via a multi-packet TD chain (buflen bytes buffer).
     * Returns 0 on success. */
    int (*intr_in)(struct UsbHc *hc, struct UsbDev *dev, uint8_t ep,
                   uint16_t mps, void *buf, uint16_t buflen,
                   void (*cb)(void *ctx, void *buf, int len), void *ctx);

    /* Port helpers (root hub) */
    int  (*port_connected)(struct UsbHc *hc, int port); /* -1 = none */
    int  (*port_reset)(struct UsbHc *hc, int port);     /* returns speed */
    /* Optional: return 1 when a connect-status-change latched since the
     * last call, clearing it (W1C).  NULL → the deferred enum task
     * falls back to tracking the CCS bit itself (UAOS-258). */
    int  (*port_csc)(struct UsbHc *hc, int port);
    /* Optional: raw root-port status register (UHCI PORTSC) — the
     * deaf-port verdict logs it so "connected-but-silent" vs
     * "port never enabled" is visible in klog (UAOS-293).  Returns
     * -1 for an invalid port; NULL → callers skip the detail. */
    int  (*port_status)(struct UsbHc *hc, int port);
    int   nports;
} UsbHc;

typedef struct UsbDev {
    UsbHc   *hc;
    int      port;
    uint8_t  addr;
    uint8_t  speed;
    uint8_t  ep0_mps;
    uint16_t vid, pid;
    uint8_t  devclass;
    char     name[8];          /* usbN */
} UsbDev;

typedef struct UsbIf {
    UsbDev  *dev;
    uint8_t  ifnum;
    uint8_t  cls, sub, proto;
    uint8_t  int_ep;           /* endpoint number (0-15), 0 if none */
    uint16_t int_mps;
    uint8_t  int_interval;
    uint8_t  used;             /* claimed by a class driver */
} UsbIf;

/* ------------------------------------------------------------------ */
/* Core API                                                            */
/* ------------------------------------------------------------------ */

/* HC drivers call this from their init to publish a controller. */
void USB_RegisterHc(UsbHc *hc);

/* Probe all host controllers and enumerate their ports. */
int  USB_Init(void);

/* Periodic service — drains completed interrupt transfers on all HCs
 * that lack IRQ delivery (safe to call even when IRQs work). */
void USB_Poll(void);

/* Spawn the deferred re-enumeration task — call once the scheduler
 * exists (post-TaskScheduler_Init).  Re-probes ports that reported
 * attached-but-deaf at boot, plus real post-boot hotplug (UAOS-258). */
void USB_StartEnumTask(void);

/* Host-controller entry points (defined in uhci.c). */
int  UHCI_Init(void);
void UHCI_SetupIRQs(void);
void UHCI_Poll(void);

/* Emit diagnostics the IRQ handler deferred (spurious-IRQ counts,
 * storm masks, mid-dispatch catches) — task context only, the usb-enum
 * task runs it once per scan round (UAOS-294). */
void UHCI_DiagFlush(void);

/* Read-only register snapshot for C:usbdiag (UAOS-183). */
typedef struct {
    uint8_t  bus, dev, fn, int_line, int_pin;
    uint16_t io;
    int      irq_vec;
    uint32_t irq_hits;      /* dispatches seen on our vector */
    uint32_t irq_late;      /* completions caught on the post-dispatch recheck */
    uint32_t poll_usbint;   /* times the poll path found USBSTS.USBINT latched */
    uint16_t usbcmd, usbsts, usbintr, frnum, portsc[2];
    uint16_t pci_cmd, pci_sts, legsup;
    int      npipes, pipes_ioc;   /* armed intr pipes / with IOC on last TD */
} UhciDiag;
int  UHCI_DiagCount(void);
int  UHCI_DiagRead(int idx, UhciDiag *d);

/* Synchronous standard request helper. */
int  usb_ctrl(UsbDev *dev, uint8_t bmRequestType, uint8_t bRequest,
              uint16_t wValue, uint16_t wIndex, void *data, uint16_t len);

/* Class-driver registration: probe() is called for every interface of
 * every enumerated device; return non-zero to claim it. */
typedef int (*usb_class_probe_fn)(UsbIf *ifc);
void USB_RegisterClass(usb_class_probe_fn probe);

/* Called by HCs when a periodic input report completes.  Class driver
 * internals — not for general use. */

/* Number of enumerated devices. */
int  USB_DeviceCount(void);

/* Bound HID-class interfaces (boot-protocol keyboard/mouse), for
 * inventory reporting — 0/0 on machines with no USB input. */
int  USBHid_DeviceCount(void);
int  USBHid_KbdCount(void);
int  USBHid_MouseCount(void);

#endif /* UAOS_USB_H */
