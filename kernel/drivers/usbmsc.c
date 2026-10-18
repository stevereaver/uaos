/* usbmsc.c — UAOS USB mass-storage class driver (BOT + SCSI)
 *
 * Binds interfaces of class 0x08 / subclass 0x06 (SCSI transparent
 * command set) / protocol 0x50 (Bulk-Only Transport) and publishes
 * each LUN as a BlockDev — usb0, usb1, ... — so a USB stick shows up
 * to the partition scanner and VFS exactly like a virtio disk
 * (UAOS-317).
 *
 * BOT transaction shape (all over the bulk-IN/-OUT endpoints):
 *   CBW (31 B) out → optional data stage → CSW (13 B) in.
 * A STALL anywhere on the data stage is protocol, not an error: the
 * device is telling us the real residue; the CSW carries the verdict.
 * Recovery follows BOT spec 5.3: CLEAR_FEATURE(HALT) on the halted
 * endpoint, software toggle resets, then a second CSW read.
 *
 * SCSI command set implemented: TEST UNIT READY, INQUIRY,
 * READ CAPACITY(10), READ(10), WRITE(10), plus REQUEST SENSE after a
 * failed CSW (one retry — a media-change sense is the usual cause).
 *
 * Data goes through a per-device DMA bounce buffer: BlockDev callers
 * hand us arbitrary kernel buffers and the HC needs a contiguous
 * identity-mapped run, so reads/writes chunk at BOUNCE_BYTES.
 */

#include "usb.h"
#include "../klog/klog.h"
#include "../dos/dma.h"
#include "../dos/blockdev.h"
#include "../exec/task.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* BOT wire structures                                                 */
/* ------------------------------------------------------------------ */
#define CBW_SIG     0x43425355u   /* 'USBC' */
#define CSW_SIG     0x53425355u   /* 'USBS' */
#define CBW_LEN     31
#define CSW_LEN     13

typedef struct __attribute__((packed)) {
    uint32_t sig;
    uint32_t tag;
    uint32_t dlen;          /* expected data transfer length */
    uint8_t  flags;         /* bit7 = data-IN */
    uint8_t  lun;
    uint8_t  cbdlen;
    uint8_t  cbd[16];
} Cbw;

typedef struct __attribute__((packed)) {
    uint32_t sig;
    uint32_t tag;
    uint32_t residue;
    uint8_t  status;        /* 0 ok, 1 cmd failed, 2 phase error */
} Csw;

/* SCSI opcodes */
#define SCSI_TEST_UNIT_READY   0x00
#define SCSI_REQUEST_SENSE     0x03
#define SCSI_INQUIRY           0x12
#define SCSI_READ_CAPACITY_10  0x25
#define SCSI_READ_10           0x28
#define SCSI_WRITE_10          0x2A

/* Class request: GET MAX LUN */
#define MSC_REQ_GET_MAX_LUN    0xFE
#define MSC_REQ_RESET          0xFF

#define MSC_MAX_DEV   4
#define BOUNCE_BYTES  (64 * 1024)     /* 128 sectors @512B per chunk */

typedef struct {
    UsbDev  *dev;
    UsbIf   *ifc;
    uint8_t  ep_in, ep_out;
    uint16_t mps_in, mps_out;
    int      dt_in, dt_out;           /* bulk toggle state */
    uint32_t tag;                     /* CBW tag counter */
    uint8_t  lun;
    uint8_t *cbw;                     /* DMA buffers, alloc'd once */
    uint8_t *csw;
    uint8_t *bounce;
    BlockDev bd;
    char     name[8];                 /* "usbN" */
    char     label[36];               /* "vendor product" */
} MscDev;

static MscDev g_msc[MSC_MAX_DEV];
static int    g_nmsc = 0;

/* kernel main's partition-scan + automount helper — VFS is already up
 * when USB enumeration runs, so late-bound USB drives mount the same
 * way virtio/ahci disks do at boot. */
extern void boot_automount_partitions(BlockDev *bdev);

/* ------------------------------------------------------------------ */
/* Endpoint-halt recovery (BOT 5.3.2)                                  */
/* ------------------------------------------------------------------ */
static void msc_clear_halt(MscDev *m, uint8_t ep, int dir_in)
{
    usb_ctrl(m->dev,
             USB_RT_OUT | USB_RT_STD | USB_RT_EP,
             USB_REQ_CLEAR_FEATURE, 0 /* ENDPOINT_HALT */,
             (uint16_t)ep, 0, 0);
    if (dir_in) m->dt_in = 0;
    else        m->dt_out = 0;
}

/* ------------------------------------------------------------------ */
/* BOT transaction                                                     */
/*                                                                     */
/* Runs one SCSI command: CBW out → data → CSW in.  Returns bytes of  */
/* data-stage payload moved (≥0) or <0 on transport failure.          */
/* ------------------------------------------------------------------ */
static int msc_bot(MscDev *m, const void *cbd, uint8_t cbdlen,
                   void *data, uint32_t dlen, int dir_in)
{
    Cbw *cbw = (Cbw *)m->cbw;
    memset(cbw, 0, CBW_LEN);
    cbw->sig    = CBW_SIG;
    cbw->tag    = ++m->tag;
    cbw->dlen   = dlen;
    cbw->flags  = dir_in ? 0x80 : 0x00;
    cbw->lun    = m->lun;
    cbw->cbdlen = cbdlen;
    memcpy(cbw->cbd, cbd, cbdlen > 16 ? 16 : cbdlen);

    uint32_t actual = 0;
    if (usb_bulk(m->dev, m->ep_out, 0, cbw, CBW_LEN,
                 m->mps_out, &m->dt_out, &actual) != 0 ||
        actual != CBW_LEN) {
        msc_clear_halt(m, m->ep_out, 0);
        return -1;
    }

    uint32_t moved = 0;
    if (dlen) {
        if (usb_bulk(m->dev, dir_in ? m->ep_in : m->ep_out, dir_in,
                     data, dlen, dir_in ? m->mps_in : m->mps_out,
                     dir_in ? &m->dt_in : &m->dt_out, &moved) != 0)
            /* Data-stage STALL/error is recoverable — the CSW says
             * what the device meant.  Un-halt the endpoint first. */
            msc_clear_halt(m, dir_in ? m->ep_in : m->ep_out, dir_in);
    }

    Csw *csw = (Csw *)m->csw;
    memset(csw, 0, CSW_LEN);
    int r = usb_bulk(m->dev, m->ep_in, 1, csw, CSW_LEN,
                     m->mps_in, &m->dt_in, &actual);
    if (r != 0) {
        /* Halted CSW endpoint — clear and retry once (BOT 5.3.3). */
        msc_clear_halt(m, m->ep_in, 1);
        memset(csw, 0, CSW_LEN);
        r = usb_bulk(m->dev, m->ep_in, 1, csw, CSW_LEN,
                     m->mps_in, &m->dt_in, &actual);
        if (r != 0) return -1;
    }

    if (csw->sig != CSW_SIG || csw->tag != cbw->tag || csw->status == 2)
        return -1;                          /* phase error → would need
                                               a full BOT reset; the
                                               caller treats it as I/O
                                               failure */
    if (csw->status == 1)
        return -2;                          /* CHECK CONDITION —
                                               caller REQUEST SENSEs
                                               and retries once */
    return (int)moved;
}

/* ------------------------------------------------------------------ */
/* SCSI helpers                                                        */
/* ------------------------------------------------------------------ */
static int msc_cmd(MscDev *m, const void *cbd, uint8_t cbdlen,
                   void *data, uint32_t dlen, int dir_in)
{
    int r = msc_bot(m, cbd, cbdlen, data, dlen, dir_in);
    if (r == -2) {
        /* Command failed — issue REQUEST SENSE (clears the pending
         * check-condition) and retry once: UNIT ATTENTION /
         * media-changed senses are routine on first access. */
        uint8_t rs[6]  = { SCSI_REQUEST_SENSE, 0, 0, 0, 18, 0 };
        uint8_t sense[18];
        memset(sense, 0, sizeof(sense));
        msc_bot(m, rs, 6, sense, sizeof(sense), 1);
        r = msc_bot(m, cbd, cbdlen, data, dlen, dir_in);
    }
    return r;
}

static int msc_ready(MscDev *m)
{
    uint8_t tur[6] = { SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0 };
    for (int i = 0; i < 4; i++)
        if (msc_cmd(m, tur, 6, 0, 0, 0) >= 0)
            return 0;
    return -1;
}

static int msc_read(MscDev *m, uint32_t lba, uint16_t nsect, void *buf)
{
    uint8_t cdb[10] = {
        SCSI_READ_10, 0,
        (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
        (uint8_t)(lba >> 8), (uint8_t)lba,
        0,
        (uint8_t)(nsect >> 8), (uint8_t)nsect, 0
    };
    int r = msc_cmd(m, cdb, 10, buf, (uint32_t)nsect * 512, 1);
    return r >= 0 ? 0 : -1;
}

static int msc_write(MscDev *m, uint32_t lba, uint16_t nsect,
                     const void *buf)
{
    uint8_t cdb[10] = {
        SCSI_WRITE_10, 0,
        (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
        (uint8_t)(lba >> 8), (uint8_t)lba,
        0,
        (uint8_t)(nsect >> 8), (uint8_t)nsect, 0
    };
    int r = msc_cmd(m, cdb, 10, (void *)buf, (uint32_t)nsect * 512, 0);
    return r >= 0 ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* BlockDev ops                                                      */
/* ------------------------------------------------------------------ */
static int msc_bd_read(BlockDev *bd, uint64_t sector, void *buffer,
                       uint32_t num_sectors)
{
    MscDev *m = (MscDev *)bd->private_data;
    uint32_t chunk_max = BOUNCE_BYTES / bd->sector_size;
    while (num_sectors) {
        uint32_t n = num_sectors > chunk_max ? chunk_max : num_sectors;
        if (msc_read(m, (uint32_t)sector, (uint16_t)n, m->bounce) != 0)
            return -1;
        memcpy(buffer, m->bounce, n * bd->sector_size);
        buffer = (uint8_t *)buffer + n * bd->sector_size;
        sector += n;
        num_sectors -= n;
    }
    return 0;
}

static int msc_bd_write(BlockDev *bd, uint64_t sector,
                        const void *buffer, uint32_t num_sectors)
{
    MscDev *m = (MscDev *)bd->private_data;
    uint32_t chunk_max = BOUNCE_BYTES / bd->sector_size;
    while (num_sectors) {
        uint32_t n = num_sectors > chunk_max ? chunk_max : num_sectors;
        memcpy(m->bounce, buffer, n * bd->sector_size);
        if (msc_write(m, (uint32_t)sector, (uint16_t)n, m->bounce) != 0)
            return -1;
        buffer = (const uint8_t *)buffer + n * bd->sector_size;
        sector += n;
        num_sectors -= n;
    }
    return 0;
}

static uint64_t msc_bd_capacity(BlockDev *bd)
{
    return bd->num_sectors;
}

static const BlockDevOps g_msc_ops = {
    .read         = msc_bd_read,
    .write        = msc_bd_write,
    .get_capacity = msc_bd_capacity,
};

/* ------------------------------------------------------------------ */
/* Class probe                                                         */
/* ------------------------------------------------------------------ */
static int msc_probe(UsbIf *ifc)
{
    if (ifc->cls != USB_CLASS_MSC || ifc->sub != 0x06 ||
        ifc->proto != 0x50)
        return 0;
    UsbDev *dev = ifc->dev;
    if (!dev->hc->bulk) return 0;            /* HC has no bulk path */
    if (!ifc->bulk_in_ep || !ifc->bulk_out_ep) return 0;
    if (g_nmsc >= MSC_MAX_DEV) return 0;

    MscDev *m = &g_msc[g_nmsc];
    memset(m, 0, sizeof(*m));
    m->dev = dev;
    m->ifc = ifc;
    m->ep_in   = ifc->bulk_in_ep;
    m->ep_out  = ifc->bulk_out_ep;
    m->mps_in  = ifc->bulk_in_mps  ? ifc->bulk_in_mps  : 64;
    m->mps_out = ifc->bulk_out_mps ? ifc->bulk_out_mps : 64;
    m->lun = 0;

    m->cbw    = (uint8_t *)DMA_Alloc(CBW_LEN, 8);
    m->csw    = (uint8_t *)DMA_Alloc(CSW_LEN, 8);
    m->bounce = (uint8_t *)DMA_Alloc(BOUNCE_BYTES, 8);
    if (!m->cbw || !m->csw || !m->bounce) return 0;

    /* GET MAX LUN (class, interface) — informational; LUN 0 covers
     * every single-LUN stick, which is what this driver binds. */
    uint8_t maxlun = 0;
    if (usb_ctrl(dev, USB_RT_IN | USB_RT_CLASS | USB_RT_IF,
                 MSC_REQ_GET_MAX_LUN, 0, ifc->ifnum, m->csw, 1) == 0)
        maxlun = m->csw[0];

    if (msc_ready(m) != 0) {
        klog_puts(KLOG_USB, KLOG_WARN, "usbmsc: unit not ready\n");
        return 0;
    }

    /* INQUIRY — vendor/product strings for the log + display name. */
    uint8_t inq[6] = { SCSI_INQUIRY, 0, 0, 0, 36, 0 };
    memset(m->bounce, 0, 36);
    if (msc_cmd(m, inq, 6, m->bounce, 36, 1) >= 0) {
        /* vendor[8] + product[16], space-padded — copy then rtrim. */
        int n = 0;
        for (int i = 8; i < 16; i++) m->label[n++] = (char)m->bounce[i];
        m->label[n++] = ' ';
        for (int i = 16; i < 32; i++) m->label[n++] = (char)m->bounce[i];
        m->label[n] = 0;
        while (n > 0 && m->label[n - 1] == ' ') m->label[--n] = 0;
    } else {
        memcpy(m->label, "USB storage", 12);
    }

    /* READ CAPACITY(10) — last LBA + block size. */
    uint8_t rc[10] = { SCSI_READ_CAPACITY_10, 0,0,0,0,0,0,0,0,0 };
    memset(m->bounce, 0, 8);
    if (msc_cmd(m, rc, 10, m->bounce, 8, 1) < 0) {
        klog_puts(KLOG_USB, KLOG_WARN,
                  "usbmsc: READ CAPACITY failed\n");
        return 0;
    }
    uint32_t last_lba = ((uint32_t)m->bounce[0] << 24) |
                        ((uint32_t)m->bounce[1] << 16) |
                        ((uint32_t)m->bounce[2] << 8)  |
                        (uint32_t)m->bounce[3];
    uint32_t blksz = ((uint32_t)m->bounce[4] << 24) |
                     ((uint32_t)m->bounce[5] << 16) |
                     ((uint32_t)m->bounce[6] << 8)  |
                     (uint32_t)m->bounce[7];
    if (blksz != 512 || last_lba == 0 || last_lba == 0xFFFFFFFF) {
        klog_puts(KLOG_USB, KLOG_WARN, "usbmsc: odd geometry blksz=");
        klog_appendf(KLOG_USB, KLOG_WARN, "%u", blksz);
        klog_puts(KLOG_USB, KLOG_WARN, " lba=");
        klog_appendf(KLOG_USB, KLOG_WARN, "0x%08X", last_lba);
        klog_puts(KLOG_USB, KLOG_WARN, " -- skipped\n");
        return 0;
    }

    m->name[0] = 'u'; m->name[1] = 's'; m->name[2] = 'b';
    m->name[3] = (char)('0' + g_nmsc);
    m->name[4] = 0;

    memset(&m->bd, 0, sizeof(m->bd));
    m->bd.name         = m->name;
    m->bd.display_name = m->label;
    m->bd.sector_size  = 512;
    m->bd.num_sectors  = (uint64_t)last_lba + 1;
    m->bd.private_data = m;
    m->bd.ops          = &g_msc_ops;

    if (BlockDev_Register(&m->bd) != 0) {
        klog_puts(KLOG_USB, KLOG_WARN,
                  "usbmsc: blockdev table full\n");
        return 0;
    }

    klog_puts(KLOG_USB, KLOG_INFO, "usbmsc: ");
    klog_puts(KLOG_USB, KLOG_INFO, m->name);
    klog_puts(KLOG_USB, KLOG_INFO, " '");
    klog_puts(KLOG_USB, KLOG_INFO, m->label);
    klog_puts(KLOG_USB, KLOG_INFO, "' vid=");
    klog_appendf(KLOG_USB, KLOG_INFO, "0x%08X", dev->vid);
    klog_puts(KLOG_USB, KLOG_INFO, " pid=");
    klog_appendf(KLOG_USB, KLOG_INFO, "0x%08X", dev->pid);
    klog_puts(KLOG_USB, KLOG_INFO, " maxlun=");
    klog_appendf(KLOG_USB, KLOG_INFO, "%u", (uint32_t)maxlun);
    klog_puts(KLOG_USB, KLOG_INFO, " sectors=");
    klog_appendf(KLOG_USB, KLOG_INFO, "%u",
                (uint32_t)(last_lba + 1));
    klog_puts(KLOG_USB, KLOG_INFO, "\n");

    g_nmsc++;
    boot_automount_partitions(&m->bd);
    return 1;
}

void USBMSC_Init(void)
{
    USB_RegisterClass(msc_probe);
}
