/*
 * virtio_scsi.c — UAOS VirtIO-SCSI Block Device Driver
 *
 * Supports two transports:
 *   - Modern (virtio 1.0+, non-transitional): PCI 1af4:1048
 *     Uses capability-based MMIO transport (virtio_pci_common_cfg,
 *     notify, ISR, device-specific config capabilities).
 *     This is what VirtualBox exposes when "virtio-scsi" is selected
 *     as the storage controller.
 *   - Legacy (pre-1.0): PCI 1af4:1004
 *     Uses the simple BAR0 I/O register interface, identical to the
 *     existing virtio_blk / virtio_net legacy drivers.
 *
 * The disk (port 0, LUN 0) is registered as block device "virtio0"
 * so the existing partition / MBR / mount / assign boot flow in
 * uaos_kernel_main.c works unchanged.
 *
 * I/O model: synchronous single-request.  Each read/write submits one
 * SCSI READ(10)/WRITE(10) request on the command virtqueue (queue 2)
 * and polls the used ring for completion.  An IRQ handler is also
 * registered for interrupt-driven completion as a forward path.
 */

#include "virtio_scsi.h"
#include "../dos/blockdev.h"
#include "../dos/dma.h"
#include "../boot/kprint.h"
#include "idt.h"
#include <stdint.h>
#include <stddef.h>

/* =========================================================================
 * PCI Configuration Space
 * ========================================================================= */

#define VIRTIO_PCI_VENDOR_ID        0x1AF4
#define VIRTIO_PCI_DEVICE_ID_SCSI_LEGACY  0x1004
#define VIRTIO_PCI_DEVICE_ID_SCSI_MODERN  0x1048

#define PCI_CONFIG_ADDRESS_PORT     0xCF8
#define PCI_CONFIG_DATA_PORT        0xCFC

#define PCI_REG_VENDOR_DEVICE       0x00
#define PCI_REG_COMMAND             0x04
#define PCI_REG_STATUS              0x06
#define PCI_REG_CLASS               0x08
#define PCI_REG_BAR(n)              (0x10 + (n) * 4)
#define PCI_REG_CAP_PTR             0x34
#define PCI_REG_INTERRUPT_LINE      0x3C

/* PCI command register bits */
#define PCI_CMD_IO_SPACE            0x01
#define PCI_CMD_MEMORY_SPACE        0x02
#define PCI_CMD_BUS_MASTER          0x04

/* PCI capability IDs */
#define PCI_CAP_ID_MSIX             0x11
#define PCI_CAP_ID_MSI              0x05
#define PCI_CAP_ID_VENDOR           0x09

/* VirtIO vendor capability types */
#define VIRTIO_PCI_CAP_COMMON_CFG   1
#define VIRTIO_PCI_CAP_NOTIFY_CFG   2
#define VIRTIO_PCI_CAP_ISR_CFG      3
#define VIRTIO_PCI_CAP_DEVICE_CFG   4

/* =========================================================================
 * I/O port helpers
 * ========================================================================= */

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" :: "a"(val), "Nd"(port));
}
static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v;
}
static inline uint16_t inw(uint16_t port) {
    uint16_t v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port)); return v;
}
static inline uint32_t inl(uint16_t port) {
    uint32_t v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port)); return v;
}

/* =========================================================================
 * PCI config space access
 * ========================================================================= */

static uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg) {
    uint32_t addr = 0x80000000U | ((uint32_t)bus << 16) | ((uint32_t)dev << 11)
                  | ((uint32_t)fn << 8) | (reg & 0xFC);
    outl(PCI_CONFIG_ADDRESS_PORT, addr);
    return inl(PCI_CONFIG_DATA_PORT);
}
static uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg) {
    return (uint16_t)(pci_read32(bus, dev, fn, reg) >> ((reg & 2) * 8));
}
static uint8_t pci_read8(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg) {
    return (uint8_t)(pci_read32(bus, dev, fn, reg) >> ((reg & 3) * 8));
}
static void pci_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint16_t val) {
    uint32_t addr = 0x80000000U | ((uint32_t)bus << 16) | ((uint32_t)dev << 11)
                  | ((uint32_t)fn << 8) | (reg & 0xFC);
    outl(PCI_CONFIG_ADDRESS_PORT, addr);
    uint32_t cur = inl(PCI_CONFIG_DATA_PORT);
    int shift = (reg & 2) * 8;
    cur = (cur & ~(0xFFFFU << shift)) | ((uint32_t)val << shift);
    outl(PCI_CONFIG_DATA_PORT, cur);
}
static void pci_write8(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint8_t val) {
    uint32_t addr = 0x80000000U | ((uint32_t)bus << 16) | ((uint32_t)dev << 11)
                  | ((uint32_t)fn << 8) | (reg & 0xFC);
    outl(PCI_CONFIG_ADDRESS_PORT, addr);
    uint32_t cur = inl(PCI_CONFIG_DATA_PORT);
    int shift = (reg & 3) * 8;
    cur = (cur & ~(0xFFU << shift)) | ((uint32_t)val << shift);
    outl(PCI_CONFIG_DATA_PORT, cur);
}

/* Read a full 64-bit BAR address (handles 32-bit and 64-bit BARs).
 * Returns 0 if the BAR is I/O type or unmapped. */
static uint64_t pci_read_bar(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t bar_idx) {
    uint8_t bar_off = (uint8_t)(0x10 + bar_idx * 4);
    uint32_t bar_lo = pci_read32(bus, dev, fn, bar_off);
    if (bar_lo & 1) return 0;           /* I/O BAR — not what we want here */
    uint64_t addr = bar_lo & 0xFFFFFFF0U;
    if ((bar_lo & 0x6) == 0x4) {        /* 64-bit BAR */
        uint32_t bar_hi = pci_read32(bus, dev, fn, (uint8_t)(bar_off + 4));
        addr |= (uint64_t)bar_hi << 32;
    }
    return addr;
}

/* =========================================================================
 * MSI / MSI-X disable (force legacy INTx)
 *
 * VirtualBox and QEMU enable MSI-X by default for virtio devices, which
 * bypasses the 8259 PIC entirely.  We disable it so the interrupt line
 * in PCI config (offset 0x3C) is used instead.
 * ========================================================================= */

static void pci_disable_msi(uint8_t bus, uint8_t dev, uint8_t fn) {
    uint16_t status = pci_read16(bus, dev, fn, PCI_REG_STATUS);
    if (!(status & 0x10)) return;       /* no capabilities list */

    uint8_t cap_ptr = pci_read8(bus, dev, fn, PCI_REG_CAP_PTR) & 0xFC;
    int limit = 48;
    while (cap_ptr && limit--) {
        uint8_t cap_id   = pci_read8(bus, dev, fn, cap_ptr);
        uint8_t cap_next = pci_read8(bus, dev, fn, (uint8_t)(cap_ptr + 1));
        if (cap_id == PCI_CAP_ID_MSI) {
            uint16_t mc = pci_read16(bus, dev, fn, (uint8_t)(cap_ptr + 2));
            if (mc & 1)
                pci_write16(bus, dev, fn, (uint8_t)(cap_ptr + 2), (uint16_t)(mc & ~1));
        }
        if (cap_id == PCI_CAP_ID_MSIX) {
            uint16_t mc = pci_read16(bus, dev, fn, (uint8_t)(cap_ptr + 2));
            pci_write16(bus, dev, fn, (uint8_t)(cap_ptr + 2), (uint16_t)(mc & ~0x8000));
        }
        cap_ptr = cap_next & 0xFC;
    }
}

/* =========================================================================
 * MMIO helpers (volatile 32-bit access to memory-mapped register space)
 * ========================================================================= */

static inline uint32_t mmio_r32(uint64_t addr) {
    return *((volatile uint32_t *)(uintptr_t)addr);
}
static inline void mmio_w32(uint64_t addr, uint32_t val) {
    *((volatile uint32_t *)(uintptr_t)addr) = val;
}
static inline uint8_t mmio_r8(uint64_t addr) {
    return *((volatile uint8_t *)(uintptr_t)addr);
}
static inline void mmio_w8(uint64_t addr, uint8_t val) {
    *((volatile uint8_t *)(uintptr_t)addr) = val;
}
static inline uint16_t mmio_r16(uint64_t addr) {
    return *((volatile uint16_t *)(uintptr_t)addr);
}
static inline void mmio_w16(uint64_t addr, uint16_t val) {
    *((volatile uint16_t *)(uintptr_t)addr) = val;
}

static inline void memory_barrier(void) {
    /* Full x86 memory fence — ensures all prior stores are globally visible
     * before notifying the VirtIO device.  Without this the device can see
     * a stale avail_idx and miss the request, causing a 3-second timeout. */
    __asm__ volatile("mfence" ::: "memory");
}

/* =========================================================================
 * Transport abstraction
 * ========================================================================= */

typedef enum { VIO_SCSI_LEGACY, VIO_SCSI_MODERN } transport_t;

/* Modern transport: vendor capability location */
typedef struct {
    uint8_t  bar;       /* BAR index (0-5) */
    uint32_t offset;    /* offset within the BAR */
    uint32_t length;    /* length of the capability region */
    uint64_t bar_addr;  /* resolved physical BAR address */
} vio_cap_t;

/* =========================================================================
 * VirtIO status bits
 * ========================================================================= */

#define VIRTIO_STATUS_RESET         0x00
#define VIRTIO_STATUS_ACKNOWLEDGE   0x01
#define VIRTIO_STATUS_DRIVER        0x02
#define VIRTIO_STATUS_DRIVER_OK     0x04
#define VIRTIO_STATUS_FEATURES_OK   0x08
#define VIRTIO_STATUS_FAILED        0x80

/* =========================================================================
 * VirtIO legacy register offsets (BAR0 I/O, legacy transport only)
 * ========================================================================= */

#define VIO_LEGACY_HOST_FEATURES     0x00
#define VIO_LEGACY_GUEST_FEATURES    0x04
#define VIO_LEGACY_QUEUE_PFN         0x08
#define VIO_LEGACY_QUEUE_SIZE        0x0C
#define VIO_LEGACY_QUEUE_SEL         0x0E
#define VIO_LEGACY_QUEUE_NOTIFY      0x10
#define VIO_LEGACY_STATUS            0x12
#define VIO_LEGACY_ISR               0x13

/* =========================================================================
 * VirtIO modern common config register offsets
 * (virtio_pci_common_cfg, virtio 1.0+ spec)
 * ========================================================================= */

#define VIO_COMMON_DEVICE_FEATURE_SELECT   0x00
#define VIO_COMMON_DEVICE_FEATURE          0x04
#define VIO_COMMON_DRIVER_FEATURE_SELECT   0x08
#define VIO_COMMON_DRIVER_FEATURE          0x0C
#define VIO_COMMON_MSIX_CONFIG             0x10
#define VIO_COMMON_NUM_QUEUES              0x12
#define VIO_COMMON_DEVICE_STATUS           0x14
#define VIO_COMMON_CONFIG_GENERATION       0x15
#define VIO_COMMON_QUEUE_SELECT            0x16
#define VIO_COMMON_QUEUE_SIZE              0x18
#define VIO_COMMON_QUEUE_MSIX_VECTOR       0x1A
#define VIO_COMMON_QUEUE_ENABLE            0x1C
#define VIO_COMMON_QUEUE_NOTIFY_OFF        0x1E
#define VIO_COMMON_QUEUE_DESC_LO           0x20
#define VIO_COMMON_QUEUE_DESC_HI           0x24
#define VIO_COMMON_QUEUE_AVAIL_LO          0x28
#define VIO_COMMON_QUEUE_AVAIL_HI          0x2C
#define VIO_COMMON_QUEUE_USED_LO           0x30
#define VIO_COMMON_QUEUE_USED_HI           0x34

#define VIO_MSIX_NO_VECTOR                 0xFFFF

/* VIRTIO_F_VERSION_1 = feature bit 32 (index 0 in select=1 half) */
#define VIRTIO_F_VERSION_1_BIT             (1u << 0)

/* =========================================================================
 * Virtqueue structures (split-ring, legacy-compatible layout)
 * ========================================================================= */

#define VIO_SCSI_QSIZE     32

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed)) vio_desc_t;

#define VIO_DESC_F_NEXT    1
#define VIO_DESC_F_WRITE   2

/* Layout (page-aligned for legacy compatibility):
 *   desc table:  QSIZE * 16
 *   avail ring:  6 + QSIZE * 2  (flags + idx + ring + used_event)
 *   --- 4K boundary ---
 *   used ring:   6 + QSIZE * 8  (flags + idx + ring + avail_event)
 */
#define VIO_DESC_BYTES   (VIO_SCSI_QSIZE * 16)
#define VIO_AVAIL_BYTES  (6 + VIO_SCSI_QSIZE * 2)
#define VIO_USED_OFF     (((VIO_DESC_BYTES + VIO_AVAIL_BYTES) + 4095) & ~4095U)
#define VIO_USED_BYTES   (6 + VIO_SCSI_QSIZE * 8)
#define VIO_QBYTES       (VIO_USED_OFF + VIO_USED_BYTES)

/* 3 virtqueues: 0=control, 1=event, 2=command */
#define VIO_Q_CONTROL    0
#define VIO_Q_EVENT      1
#define VIO_Q_COMMAND    2
#define VIO_NUM_QUEUES   3

typedef struct __attribute__((aligned(4096))) {
    vio_desc_t desc[VIO_SCSI_QSIZE];
    uint16_t avail_flags;
    uint16_t avail_idx;
    uint16_t avail_ring[VIO_SCSI_QSIZE];
    uint16_t avail_used_event;
    uint8_t  padding[VIO_USED_OFF - VIO_DESC_BYTES - VIO_AVAIL_BYTES];
    uint16_t used_flags;
    uint16_t used_idx;
    struct { uint32_t id; uint32_t len; } used_ring[VIO_SCSI_QSIZE];
    uint16_t used_avail_event;
} vio_virtq_t;

/* Static virtqueue memory (BSS, page-aligned) */
static vio_virtq_t g_vq[VIO_NUM_QUEUES];
static uint16_t g_command_last_used;

/* =========================================================================
 * VirtIO-SCSI request / response structures
 * ========================================================================= */

typedef struct __attribute__((packed)) {
    uint8_t  lun[8];
    uint64_t id;
    uint8_t  task_attr;
    uint8_t  prio;
    uint8_t  crn;
    uint8_t  cdb[32];
} vio_scsi_req_t;

typedef struct __attribute__((packed)) {
    uint32_t sense_len;
    uint32_t resid;
    uint16_t status_qualifier;
    uint8_t  status;
    uint8_t  response;
    uint8_t  sense[96];
} vio_scsi_resp_t;

/* SCSI CDB opcodes */
#define SCSI_TEST_UNIT_READY    0x00
#define SCSI_INQUIRY            0x12
#define SCSI_READ_CAPACITY_10   0x25
#define SCSI_READ_10            0x28
#define SCSI_WRITE_10           0x2A

/* SCSI response codes */
#define VIO_SCSI_RESP_OK        0
#define VIO_SCSI_RESP_CHECK     2
#define VIO_SCSI_RESP_BUSY      0x0A

/* SCSI status codes */
#define SCSI_STATUS_GOOD        0
#define SCSI_STATUS_CHECK_COND  0x02

/* SCSI peripheral device types (INQUIRY byte 0, bits 4-0) */
#define SCSI_PERIPH_DIRECT      0x00  /* Direct-access block (hard disk) */
#define SCSI_PERIPH_CDROM       0x05  /* CD/DVD-ROM */
#define SCSI_PERIPH_UNKNOWN     0x1F  /* Unknown/no device */

/* Maximum number of targets to scan */
#define VIO_SCSI_MAX_PORTS      8
#define VIO_SCSI_MAX_LUN        1  /* LUNs per port to scan */

/* =========================================================================
 * Per-device state
 * ========================================================================= */

typedef struct {
    int       active;
    int       port;
    int       lun;        /* SCSI port */
    int       is_cdrom;      /* 1 if CD/DVD-ROM, 0 if hard disk */
    uint32_t  sector_size;   /* 512 for hard disk, 2048 for CD-ROM */
    uint64_t  capacity;      /* in sector_size-byte sectors */
    BlockDev  bdev;
} vio_scsi_device_t;

/* =========================================================================
 * Driver state
 * ========================================================================= */

static transport_t g_transport   = VIO_SCSI_LEGACY;
static uint16_t    g_legacy_io   = 0;       /* legacy BAR0 I/O base */
static uint8_t     g_pci_bus     = 0;
static uint8_t     g_pci_dev     = 0;
static uint8_t     g_pci_fn      = 0;
static int         g_irq_line    = -1;
static int         g_active      = 0;       /* controller initialised */

static vio_scsi_device_t g_devices[VIO_SCSI_MAX_PORTS];
static int g_num_devices = 0;

/* Modern transport capability locations */
static vio_cap_t g_common_cap;
static vio_cap_t g_notify_cap;
static vio_cap_t g_isr_cap;
static uint32_t  g_notify_off_mult = 0;

/* Completion tracking (synchronous I/O) */
static volatile int g_irq_pending = 0;

/* DMA-safe buffers for SCSI requests (4K-aligned) */
static vio_scsi_req_t  g_scsi_req  __attribute__((aligned(4096)));
static vio_scsi_resp_t g_scsi_resp __attribute__((aligned(4096)));
static uint8_t g_data_buffer[65536] __attribute__((aligned(4096)));

/* =========================================================================
 * Transport accessors
 *
 * For legacy: all registers are at BAR0 I/O base + offset (port I/O).
 * For modern: common config is at common_cap.bar_addr + common_cap.offset + reg.
 * ========================================================================= */

/* --- Status register --- */
static uint8_t vio_status_read(void) {
    if (g_transport == VIO_SCSI_LEGACY)
        return inb(g_legacy_io + VIO_LEGACY_STATUS);
    return mmio_r8(g_common_cap.bar_addr + g_common_cap.offset + VIO_COMMON_DEVICE_STATUS);
}
static void vio_status_write(uint8_t val) {
    if (g_transport == VIO_SCSI_LEGACY)
        outb(g_legacy_io + VIO_LEGACY_STATUS, val);
    else
        mmio_w8(g_common_cap.bar_addr + g_common_cap.offset + VIO_COMMON_DEVICE_STATUS, val);
}

/* --- Feature negotiation --- */
static uint32_t vio_host_features_read(void) {
    if (g_transport == VIO_SCSI_LEGACY)
        return inl(g_legacy_io + VIO_LEGACY_HOST_FEATURES);
    /* Modern: read feature select 0 (bits 0-31) */
    mmio_w32(g_common_cap.bar_addr + g_common_cap.offset + VIO_COMMON_DEVICE_FEATURE_SELECT, 0);
    return mmio_r32(g_common_cap.bar_addr + g_common_cap.offset + VIO_COMMON_DEVICE_FEATURE);
}
static void vio_guest_features_write(uint32_t select, uint32_t val) {
    if (g_transport == VIO_SCSI_LEGACY) {
        outl(g_legacy_io + VIO_LEGACY_GUEST_FEATURES, val);
        return;
    }
    mmio_w32(g_common_cap.bar_addr + g_common_cap.offset + VIO_COMMON_DRIVER_FEATURE_SELECT, select);
    mmio_w32(g_common_cap.bar_addr + g_common_cap.offset + VIO_COMMON_DRIVER_FEATURE, val);
}

/* --- Queue selection --- */
static void vio_queue_select(uint16_t idx) {
    if (g_transport == VIO_SCSI_LEGACY)
        outw(g_legacy_io + VIO_LEGACY_QUEUE_SEL, idx);
    else
        mmio_w16(g_common_cap.bar_addr + g_common_cap.offset + VIO_COMMON_QUEUE_SELECT, idx);
}

/* --- Queue size read --- */
static uint16_t vio_queue_size_read(void) {
    if (g_transport == VIO_SCSI_LEGACY)
        return inw(g_legacy_io + VIO_LEGACY_QUEUE_SIZE);
    return mmio_r16(g_common_cap.bar_addr + g_common_cap.offset + VIO_COMMON_QUEUE_SIZE);
}

/* --- Queue notify --- */
static void vio_queue_notify(uint16_t idx) {
    if (g_transport == VIO_SCSI_LEGACY) {
        /* Legacy: select the queue before notifying */
        outw(g_legacy_io + VIO_LEGACY_QUEUE_SEL, idx);
        outw(g_legacy_io + VIO_LEGACY_QUEUE_NOTIFY, idx);
        return;
    }
    /* Modern: read notify_off for this queue, then write to notify region */
    vio_queue_select(idx);
    uint16_t notify_off = mmio_r16(g_common_cap.bar_addr + g_common_cap.offset +
                                   VIO_COMMON_QUEUE_NOTIFY_OFF);
    uint64_t notify_addr = g_notify_cap.bar_addr + g_notify_cap.offset
                         + (uint64_t)notify_off * g_notify_off_mult;
    mmio_w16(notify_addr, idx);
}

/* --- ISR read (clears interrupt) --- */
static uint8_t vio_isr_read(void) {
    if (g_transport == VIO_SCSI_LEGACY)
        return inb(g_legacy_io + VIO_LEGACY_ISR);
    return mmio_r8(g_isr_cap.bar_addr + g_isr_cap.offset);
}

/* =========================================================================
 * Virtqueue setup
 * ========================================================================= */

static int vio_setup_queue(uint16_t qidx) {
    vio_queue_select(qidx);
    uint16_t qsize = vio_queue_size_read();
    if (qsize == 0) {
        kprint("[VIO-SCSI] Queue "); kprinthex((uint64_t)qidx);
        kprint(" not available\n");
        return -1;
    }
    if (qsize > VIO_SCSI_QSIZE) qsize = VIO_SCSI_QSIZE;
    /* For modern, write the actual queue size */
    if (g_transport == VIO_SCSI_MODERN) {
        mmio_w16(g_common_cap.bar_addr + g_common_cap.offset + VIO_COMMON_QUEUE_SIZE, qsize);
    }

    /* Get physical address of the virtqueue */
    uint64_t vq_phys = DMA_VirtToPhys(&g_vq[qidx]);
    if (!vq_phys) {
        kprint("[VIO-SCSI] Failed to get virtqueue physical address\n");
        return -1;
    }

    if (g_transport == VIO_SCSI_LEGACY) {
        /* Legacy: write PFN (page number) of descriptor table */
        outl(g_legacy_io + VIO_LEGACY_QUEUE_PFN, (uint32_t)(vq_phys >> 12));
    } else {
        /* Modern: write 64-bit addresses for desc, avail, used */
        uint64_t desc_addr  = vq_phys;
        uint64_t avail_addr = vq_phys + VIO_DESC_BYTES;
        uint64_t used_addr  = vq_phys + VIO_USED_OFF;

        mmio_w32(g_common_cap.bar_addr + g_common_cap.offset + VIO_COMMON_QUEUE_DESC_LO,
                 (uint32_t)(desc_addr & 0xFFFFFFFF));
        mmio_w32(g_common_cap.bar_addr + g_common_cap.offset + VIO_COMMON_QUEUE_DESC_HI,
                 (uint32_t)(desc_addr >> 32));
        mmio_w32(g_common_cap.bar_addr + g_common_cap.offset + VIO_COMMON_QUEUE_AVAIL_LO,
                 (uint32_t)(avail_addr & 0xFFFFFFFF));
        mmio_w32(g_common_cap.bar_addr + g_common_cap.offset + VIO_COMMON_QUEUE_AVAIL_HI,
                 (uint32_t)(avail_addr >> 32));
        mmio_w32(g_common_cap.bar_addr + g_common_cap.offset + VIO_COMMON_QUEUE_USED_LO,
                 (uint32_t)(used_addr & 0xFFFFFFFF));
        mmio_w32(g_common_cap.bar_addr + g_common_cap.offset + VIO_COMMON_QUEUE_USED_HI,
                 (uint32_t)(used_addr >> 32));

        /* Disable MSI-X vector for this queue (use legacy INTx) */
        mmio_w16(g_common_cap.bar_addr + g_common_cap.offset + VIO_COMMON_QUEUE_MSIX_VECTOR,
                 VIO_MSIX_NO_VECTOR);

        /* Enable the queue */
        mmio_w16(g_common_cap.bar_addr + g_common_cap.offset + VIO_COMMON_QUEUE_ENABLE, 1);
    }

    /* Clear the virtqueue memory */
    vio_virtq_t *vq = &g_vq[qidx];
    for (int i = 0; i < VIO_SCSI_QSIZE; i++) {
        vq->desc[i].addr  = 0;
        vq->desc[i].len   = 0;
        vq->desc[i].flags = 0;
        vq->desc[i].next  = 0;
    }
    vq->avail_flags = 0;
    vq->avail_idx   = 0;
    for (int i = 0; i < VIO_SCSI_QSIZE; i++) vq->avail_ring[i] = 0;
    vq->used_flags  = 0;
    vq->used_idx    = 0;
    if (qidx == VIO_Q_COMMAND) g_command_last_used = 0;

    kprint("[VIO-SCSI] Queue "); kprinthex((uint64_t)qidx);
    kprint(" setup OK (size="); kprinthex((uint64_t)qsize); kprint(")\n");
    return 0;
}

/* =========================================================================
 * Device initialization
 * ========================================================================= */

static int vio_device_init(void) {
    /* 1. Reset — the virtio spec requires the driver to wait until
     * device_status reads back 0 before reinitialising the device.
     * A fixed delay can race the device model's reset processing and
     * leave the status/queue state inconsistent (intermittent init
     * failures on VirtualBox), so poll with a bounded spin. */
    vio_status_write(VIRTIO_STATUS_RESET);
    {
        int reset_ok = 0;
        for (volatile int i = 0; i < 1000000; i++) {
            if (vio_status_read() == VIRTIO_STATUS_RESET) { reset_ok = 1; break; }
        }
        if (!reset_ok) {
            kprint("[VIO-SCSI] Device did not complete reset\n");
            return -1;
        }
    }

    /* 2. Acknowledge + Driver */
    vio_status_write(VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* 3. Feature negotiation */
    if (g_transport == VIO_SCSI_LEGACY) {
        /* Legacy: accept all offered features (simplest) */
        uint32_t features = vio_host_features_read();
        vio_guest_features_write(0, features);
    } else {
        /* Modern: negotiate VIRTIO_F_VERSION_1 (bit 32) if offered */
        /* Read feature bits 32-63 */
        mmio_w32(g_common_cap.bar_addr + g_common_cap.offset + VIO_COMMON_DEVICE_FEATURE_SELECT, 1);
        uint32_t host_feat_hi = mmio_r32(g_common_cap.bar_addr + g_common_cap.offset +
                                         VIO_COMMON_DEVICE_FEATURE);
        /* Read feature bits 0-31 */
        mmio_w32(g_common_cap.bar_addr + g_common_cap.offset + VIO_COMMON_DEVICE_FEATURE_SELECT, 0);
        uint32_t host_feat_lo = mmio_r32(g_common_cap.bar_addr + g_common_cap.offset +
                                         VIO_COMMON_DEVICE_FEATURE);

        /* Accept VIRTIO_F_VERSION_1 if offered, no other features */
        uint32_t guest_feat_hi = (host_feat_hi & VIRTIO_F_VERSION_1_BIT);
        uint32_t guest_feat_lo = 0;

        vio_guest_features_write(1, guest_feat_hi);
        vio_guest_features_write(0, guest_feat_lo);

        /* Set FEATURES_OK and verify device accepted */
        vio_status_write(VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                         VIRTIO_STATUS_FEATURES_OK);
        uint8_t status = vio_status_read();
        if (!(status & VIRTIO_STATUS_FEATURES_OK)) {
            kprint("[VIO-SCSI] Device rejected our features\n");
            return -1;
        }
    }

    /* 4. Set up virtqueues */
    for (int q = 0; q < VIO_NUM_QUEUES; q++) {
        if (vio_setup_queue((uint16_t)q) != 0) {
            kprint("[VIO-SCSI] Failed to set up queue "); kprinthex((uint64_t)q); kprint("\n");
            return -1;
        }
    }

    /* 5. Driver OK */
    vio_status_write(VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                     VIRTIO_STATUS_DRIVER_OK);

    /* 6. Re-disable MSI-X after DRIVER_OK (QEMU/VBox re-enables it) */
    if (g_transport == VIO_SCSI_MODERN) {
        pci_disable_msi(g_pci_bus, g_pci_dev, g_pci_fn);
    }

    return 0;
}

/* =========================================================================
 * PCI capability walking (modern transport)
 * ========================================================================= */

static int vio_parse_capabilities(uint8_t bus, uint8_t dev, uint8_t fn) {
    uint16_t status = pci_read16(bus, dev, fn, PCI_REG_STATUS);
    if (!(status & 0x10)) {
        kprint("[VIO-SCSI] No PCI capabilities list\n");
        return -1;
    }

    uint8_t cap_ptr = pci_read8(bus, dev, fn, PCI_REG_CAP_PTR) & 0xFC;
    int found_common = 0, found_notify = 0, found_isr = 0;
    int limit = 48;

    while (cap_ptr && limit--) {
        uint8_t cap_id   = pci_read8(bus, dev, fn, cap_ptr);
        uint8_t cap_next = pci_read8(bus, dev, fn, (uint8_t)(cap_ptr + 1));
        uint8_t cap_len  = pci_read8(bus, dev, fn, (uint8_t)(cap_ptr + 2));

        if (cap_id == PCI_CAP_ID_VENDOR && cap_len >= 16) {
            uint8_t cfg_type = pci_read8(bus, dev, fn, (uint8_t)(cap_ptr + 3));
            uint8_t bar_idx  = pci_read8(bus, dev, fn, (uint8_t)(cap_ptr + 4));
            uint32_t offset  = pci_read32(bus, dev, fn, (uint8_t)(cap_ptr + 8));
            uint32_t length  = pci_read32(bus, dev, fn, (uint8_t)(cap_ptr + 12));

            uint64_t bar_addr = pci_read_bar(bus, dev, fn, bar_idx);
            if (!bar_addr || bar_addr > 0xFFFFFFFFULL) {
                kprint("[VIO-SCSI] Cap type "); kprinthex((uint64_t)cfg_type);
                kprint(" BAR "); kprinthex((uint64_t)bar_idx);
                kprint(" unusable (addr="); kprinthex(bar_addr); kprint(")\n");
                cap_ptr = cap_next & 0xFC;
                continue;
            }

            vio_cap_t *cap = NULL;
            if (cfg_type == VIRTIO_PCI_CAP_COMMON_CFG)      { cap = &g_common_cap; found_common = 1; }
            else if (cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG) { cap = &g_notify_cap; found_notify = 1;
                /* notify_off_multiplier is at cap_ptr + 16 */
                g_notify_off_mult = pci_read32(bus, dev, fn, (uint8_t)(cap_ptr + 16));
            }
            else if (cfg_type == VIRTIO_PCI_CAP_ISR_CFG)    { cap = &g_isr_cap; found_isr = 1; }

            if (cap) {
                cap->bar      = bar_idx;
                cap->offset   = offset;
                cap->length   = length;
                cap->bar_addr = bar_addr;
                kprint("[VIO-SCSI] Cap type="); kprinthex((uint64_t)cfg_type);
                kprint(" bar="); kprinthex((uint64_t)bar_idx);
                kprint(" off="); kprinthex((uint64_t)offset);
                kprint(" len="); kprinthex((uint64_t)length);
                kprint(" base="); kprinthex(bar_addr); kprint("\n");
            }
        }
        cap_ptr = cap_next & 0xFC;
    }

    if (!found_common || !found_notify || !found_isr) {
        kprint("[VIO-SCSI] Missing required capabilities (common=");
        kprinthex((uint64_t)found_common);
        kprint(" notify="); kprinthex((uint64_t)found_notify);
        kprint(" isr="); kprinthex((uint64_t)found_isr); kprint(")\n");
        return -1;
    }
    return 0;
}

/* =========================================================================
 * PCI device scan
 * ========================================================================= */

static int vio_find_device(void) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t fn = 0; fn < 8; fn++) {
                uint32_t id = pci_read32((uint8_t)bus, dev, fn, PCI_REG_VENDOR_DEVICE);
                if (id == 0xFFFFFFFF) { if (fn == 0) break; continue; }
                uint16_t vendor = (uint16_t)(id & 0xFFFF);
                uint16_t device = (uint16_t)(id >> 16);

                if (vendor != VIRTIO_PCI_VENDOR_ID) { if (fn == 0) break; continue; }

                if (device == VIRTIO_PCI_DEVICE_ID_SCSI_MODERN) {
                    kprint("[VIO-SCSI] Found modern virtio-scsi (1af4:1048)\n");
                    g_transport = VIO_SCSI_MODERN;
                } else if (device == VIRTIO_PCI_DEVICE_ID_SCSI_LEGACY) {
                    kprint("[VIO-SCSI] Found legacy virtio-scsi (1af4:1004)\n");
                    g_transport = VIO_SCSI_LEGACY;
                } else {
                    if (fn == 0) break;
                    continue;
                }

                g_pci_bus = (uint8_t)bus;
                g_pci_dev = dev;
                g_pci_fn  = fn;

                /* Disable MSI/MSI-X before reading IRQ line */
                pci_disable_msi((uint8_t)bus, dev, fn);

                /* Read IRQ line */
                g_irq_line = pci_read8((uint8_t)bus, dev, fn, PCI_REG_INTERRUPT_LINE);
                kprint("[VIO-SCSI] IRQ line: "); kprinthex((uint64_t)g_irq_line); kprint("\n");

                if (g_transport == VIO_SCSI_LEGACY) {
                    /* Legacy: read BAR0 (must be I/O space) */
                    uint32_t bar0 = pci_read32((uint8_t)bus, dev, fn, PCI_REG_BAR(0));
                    if (!(bar0 & 1)) {
                        kprint("[VIO-SCSI] Legacy BAR0 is not I/O space\n");
                        return 0;
                    }
                    g_legacy_io = (uint16_t)(bar0 & ~3U);
                    kprint("[VIO-SCSI] Legacy I/O base: "); kprinthex((uint64_t)g_legacy_io); kprint("\n");
                    /* Enable I/O space + bus master */
                    uint16_t cmd = pci_read16((uint8_t)bus, dev, fn, PCI_REG_COMMAND);
                    pci_write16((uint8_t)bus, dev, fn, PCI_REG_COMMAND,
                                (uint16_t)(cmd | PCI_CMD_IO_SPACE | PCI_CMD_BUS_MASTER));
                } else {
                    /* Modern: parse vendor capabilities */
                    if (vio_parse_capabilities((uint8_t)bus, dev, fn) != 0)
                        return 0;
                    /* Enable memory space + bus master */
                    uint16_t cmd = pci_read16((uint8_t)bus, dev, fn, PCI_REG_COMMAND);
                    pci_write16((uint8_t)bus, dev, fn, PCI_REG_COMMAND,
                                (uint16_t)(cmd | PCI_CMD_MEMORY_SPACE | PCI_CMD_BUS_MASTER));
                }

                return 1;
            }
        }
    }
    return 0;
}

/* =========================================================================
 * SCSI command submission and completion
 * ========================================================================= */

/* Build a SCSI LUN field for the given port and LUN.
 * This matches the encoding used by the Linux kernel virtio_scsi driver:
 *   lun[0] = 1
 *   lun[1] = target (port)
 *   lun[2] = (LUN >> 8) | 0x40
 *   lun[3] = LUN & 0xFF
 *   lun[4..7] = 0 */
static void vio_scsi_set_lun(uint8_t lun[8], int port, int lun_num) {
    for (int i = 0; i < 8; i++) lun[i] = 0;
    lun[0] = 0x01;
    lun[1] = (uint8_t)(port & 0xFF);
    lun[2] = (uint8_t)(((lun_num >> 8) & 0x3F) | 0x40);
    lun[3] = (uint8_t)(lun_num & 0xFF);
}

static uint64_t g_scsi_req_id = 1;  /* monotonic request identifier */

/* Submit a SCSI command on the command virtqueue (queue 2) to the given port.
 * data_phys / data_len describe the data buffer (0 if no data).
 * is_write = 1 for WRITE (data is device-readable),
 *            0 for READ  (data is device-writable).
 * Returns 0 on success. */
static int vio_scsi_submit(const uint8_t cdb[32], int port, int lun_num,
                           uint64_t data_phys, uint32_t data_len,
                           int is_write) {
    vio_virtq_t *vq = &g_vq[VIO_Q_COMMAND];

    /* Build request header */
    vio_scsi_set_lun(g_scsi_req.lun, port, lun_num);
    g_scsi_req.id         = g_scsi_req_id++;
    g_scsi_req.task_attr  = 0;  /* simple */
    g_scsi_req.prio       = 0;
    g_scsi_req.crn        = 0;
    for (int i = 0; i < 32; i++) g_scsi_req.cdb[i] = cdb[i];

    /* Clear response */
    g_scsi_resp.sense_len = 0;
    g_scsi_resp.resid     = 0;
    g_scsi_resp.status    = 0;
    g_scsi_resp.response  = 0;

    uint64_t req_phys  = DMA_VirtToPhys(&g_scsi_req);
    uint64_t resp_phys = DMA_VirtToPhys(&g_scsi_resp);
    if (!req_phys || !resp_phys) {
        kprint("[VIO-SCSI] Failed to get DMA addresses for req/resp\n");
        return -1;
    }

    /* Build descriptor chain per virtio-scsi spec:
     *   READ:  request (out) → response (in) → data (in)
     *   WRITE: request (out) → data (out) → response (in)
     *   NONE:  request (out) → response (in)
     * The response buffer must always be the FIRST writable descriptor. */
    if (data_len > 0 && !is_write) {
        /* READ: request → response → data */
        vq->desc[0].addr  = req_phys;
        vq->desc[0].len   = sizeof(vio_scsi_req_t);
        vq->desc[0].flags = VIO_DESC_F_NEXT;
        vq->desc[0].next  = 1;

        vq->desc[1].addr  = resp_phys;
        vq->desc[1].len   = sizeof(vio_scsi_resp_t);
        vq->desc[1].flags = VIO_DESC_F_NEXT | VIO_DESC_F_WRITE;
        vq->desc[1].next  = 2;

        vq->desc[2].addr  = data_phys;
        vq->desc[2].len   = data_len;
        vq->desc[2].flags = VIO_DESC_F_WRITE;
        vq->desc[2].next  = 0;
    } else if (data_len > 0 && is_write) {
        /* WRITE: request → data → response */
        vq->desc[0].addr  = req_phys;
        vq->desc[0].len   = sizeof(vio_scsi_req_t);
        vq->desc[0].flags = VIO_DESC_F_NEXT;
        vq->desc[0].next  = 1;

        vq->desc[1].addr  = data_phys;
        vq->desc[1].len   = data_len;
        vq->desc[1].flags = VIO_DESC_F_NEXT;  /* device-readable */
        vq->desc[1].next  = 2;

        vq->desc[2].addr  = resp_phys;
        vq->desc[2].len   = sizeof(vio_scsi_resp_t);
        vq->desc[2].flags = VIO_DESC_F_WRITE;
        vq->desc[2].next  = 0;
    } else {
        /* No data phase: request → response */
        vq->desc[0].addr  = req_phys;
        vq->desc[0].len   = sizeof(vio_scsi_req_t);
        vq->desc[0].flags = VIO_DESC_F_NEXT;
        vq->desc[0].next  = 1;

        vq->desc[1].addr  = resp_phys;
        vq->desc[1].len   = sizeof(vio_scsi_resp_t);
        vq->desc[1].flags = VIO_DESC_F_WRITE;
        vq->desc[1].next  = 0;
    }

    memory_barrier();

    /* Submit to available ring */
    vq->avail_ring[vq->avail_idx % VIO_SCSI_QSIZE] = 0;  /* start at desc 0 */
    vq->avail_idx++;

    memory_barrier();

    /* Notify the device */
    vio_queue_notify(VIO_Q_COMMAND);

    return 0;
}

/* Wait for the command virtqueue to report completion.
 * Returns 0 on success (response OK + SCSI status GOOD).
 * Returns -1 on error or timeout.  *out_response is set to the virtio-scsi
 * response code so callers can distinguish BUSY from other failures. */
static int vio_scsi_wait_completion_ext(uint8_t *out_response) {
    vio_virtq_t *vq = &g_vq[VIO_Q_COMMAND];

    uint32_t iterations = 0;
    while (1) {
        memory_barrier();

        if (vq->used_idx != g_command_last_used) {
            g_command_last_used = vq->used_idx;
            memory_barrier();
            if (out_response) *out_response = g_scsi_resp.response;
            if (g_scsi_resp.response != VIO_SCSI_RESP_OK) {
                kprint("[VIO-SCSI] resp="); kprinthex((uint64_t)g_scsi_resp.response);
                kprint(" status="); kprinthex((uint64_t)g_scsi_resp.status); kprint("\n");
                return -1;
            }
            if (g_scsi_resp.status != SCSI_STATUS_GOOD) {
                kprint("[VIO-SCSI] SCSI status="); kprinthex((uint64_t)g_scsi_resp.status);
                kprint("\n");
                return -1;
            }
            return 0;
        }

        __asm__ volatile("pause");
        if ((iterations % 500) == 0)
            (void)inb(0x80);  /* force I/O exit for emulator event loop */

        iterations++;
        if (iterations > 20000000) {
            if (out_response) *out_response = 0xFF;
            kprint("[VIO-SCSI] Timeout waiting for completion\n");
            return -1;
        }
    }
}

static int vio_scsi_wait_completion(void) {
    return vio_scsi_wait_completion_ext(NULL);
}

/* =========================================================================
 * SCSI probe operations
 * ========================================================================= */

/* INQUIRY — returns the peripheral device type (bits 4-0 of byte 0).
 * Returns the device type on success, or -1 on failure. */
static int vio_scsi_inquiry(int port) {
    kprint("[VIO-SCSI] Sending INQUIRY to port "); kprinthex((uint64_t)port); kprint("\n");

    uint8_t cdb[32];
    uint64_t data_phys = DMA_VirtToPhys(g_data_buffer);
    if (!data_phys) return -1;

    /* Retry a few times on timeout — a single stalled request here would
     * otherwise drop the whole disk for this boot. */
    uint8_t inq_resp = 0;
    int ok = 0;
    for (int attempt = 0; attempt < 3; attempt++) {
        for (int i = 0; i < 32; i++) cdb[i] = 0;
        cdb[0] = SCSI_INQUIRY;
        cdb[1] = 0;        /* EVPD=0, standard inquiry */
        cdb[2] = 0;        /* page code */
        cdb[3] = 0;        /* (MSB of allocation length) */
        cdb[4] = 36;       /* allocation length — standard inquiry */

        if (vio_scsi_submit(cdb, port, 0, data_phys, 36, 0) != 0) return -1;
        if (vio_scsi_wait_completion_ext(&inq_resp) == 0) { ok = 1; break; }

        /* Only retry on transport timeout; a real device response means
         * the target genuinely can't answer INQUIRY. */
        if (inq_resp != 0xFF) break;
        kprint("[VIO-SCSI] INQUIRY timed out for port ");
        kprinthex((uint64_t)port); kprint(", retrying\n");
    }
    if (!ok) {
        kprint("[VIO-SCSI] INQUIRY wait failed for port "); kprinthex((uint64_t)port);
        kprint(" (resp="); kprinthex((uint64_t)inq_resp); kprint(")\n");
        return -1;
    }

    /* Validate INQUIRY data — if all zeros, DMA didn't work */
    int all_zero = 1;
    for (int i = 0; i < 8; i++) if (g_data_buffer[i] != 0) { all_zero = 0; break; }
    if (all_zero) {
        kprint("[VIO-SCSI] INQUIRY returned all-zero data (DMA issue?), treating as no device\n");
        return -1;
    }

    kprint("[VIO-SCSI] INQUIRY data[0]="); kprinthex((uint64_t)g_data_buffer[0]);
    kprint(" type="); kprinthex((uint64_t)(g_data_buffer[0] & 0x1F)); kprint("\n");
    /* Byte 0: bits 4-0 = peripheral device type */
    return g_data_buffer[0] & 0x1F;
}

/* TEST UNIT READY — returns 0 if the unit is ready, -1 if not ready/error.
 * For CD-ROMs, this may need to be retried while the media spins up. */
static int vio_scsi_test_unit_ready(int port) {
    uint8_t cdb[32];
    for (int i = 0; i < 32; i++) cdb[i] = 0;
    cdb[0] = SCSI_TEST_UNIT_READY;

    /* No data phase — just request + response */
    if (vio_scsi_submit(cdb, port, 0, 0, 0, 0) != 0) return -1;
    return vio_scsi_wait_completion();
}

/* TEST UNIT READY with retries — waits for a CD-ROM to spin up.
 * Returns 0 if the unit reports SCSI GOOD status, -1 if not ready. */
static int vio_scsi_wait_unit_ready(int port, int max_retries) {
    for (int i = 0; i < max_retries; i++) {
        uint8_t resp = 0;
        uint8_t cdb[32];
        for (int j = 0; j < 32; j++) cdb[j] = 0;
        cdb[0] = SCSI_TEST_UNIT_READY;

        if (vio_scsi_submit(cdb, port, 0, 0, 0, 0) != 0) return -1;
        if (vio_scsi_wait_completion_ext(&resp) == 0) return 0;

        if (resp == 0xFF) {
            continue;
        }

        /* BUSY or CHECK CONDITION (resp OK but status not GOOD) is normal
         * for not-ready media.  Retry after a longer delay to give the
         * CD-ROM time to spin up. */
        if (resp == VIO_SCSI_RESP_BUSY || resp == VIO_SCSI_RESP_OK) {
            kprint("[VIO-SCSI] TUR not ready, retry "); kprinthex((uint64_t)i);
            kprint(" (resp="); kprinthex((uint64_t)resp); kprint(")\n");
            for (volatile int d = 0; d < 500000; d++);
            continue;
        }
        /* Other errors — device probably doesn't exist */
        return -1;
    }
    return -1;
}

/* =========================================================================
 * SCSI operations
 * ========================================================================= */

/* READ CAPACITY(10) — returns capacity in the device's native sector size.
 * Response: 8 bytes = max_lba (4 BE) + block_size (4 BE)
 * *out_blk_size receives the block size (512 for disk, 2048 for CD-ROM). */
static int vio_scsi_read_capacity(int port, uint64_t *out_sectors,
                                  uint32_t *out_blk_size) {
    uint8_t cdb[32];
    uint64_t data_phys = DMA_VirtToPhys(g_data_buffer);
    if (!data_phys) return -1;

    /* Retry READ CAPACITY up to 30 times — CD-ROMs may return BUSY
     * (response=0x0A) while the media is spinning up.  Use a long
     * timeout because the virtio-scsi host may hold the request while
     * waiting for the port to become ready. */
    for (int attempt = 0; attempt < 30; attempt++) {
        for (int i = 0; i < 32; i++) cdb[i] = 0;
        cdb[0] = SCSI_READ_CAPACITY_10;

        if (vio_scsi_submit(cdb, port, 0, data_phys, 8, 0) != 0) return -1;
        uint8_t rc_resp = 0;
        if (vio_scsi_wait_completion_ext(&rc_resp) == 0) {
            /* Success — parse response */
            uint8_t *r = g_data_buffer;
            uint32_t max_lba = ((uint32_t)r[0] << 24) | ((uint32_t)r[1] << 16)
                             | ((uint32_t)r[2] << 8) | r[3];
            uint32_t blk_size = ((uint32_t)r[4] << 24) | ((uint32_t)r[5] << 16)
                              | ((uint32_t)r[6] << 8) | r[7];
            if (blk_size == 0) blk_size = 512;
            *out_sectors = (uint64_t)(max_lba + 1);
            if (out_blk_size) *out_blk_size = blk_size;

            kprint("[VIO-SCSI] READ CAPACITY (port "); kprinthex((uint64_t)port);
            kprint("): max_lba="); kprinthex((uint64_t)max_lba);
            kprint(" blk_size="); kprinthex((uint64_t)blk_size);
            kprint(" sectors="); kprinthex(*out_sectors); kprint("\n");
            return 0;
        }

        if (rc_resp == 0xFF) {
            continue;
        }

        /* BUSY or CHECK CONDITION — wait and retry */
        if (rc_resp == VIO_SCSI_RESP_BUSY || rc_resp == VIO_SCSI_RESP_OK) {
            if (attempt < 29) {
                if (attempt % 5 == 0) {
                    kprint("[VIO-SCSI] READ CAPACITY BUSY (resp=");
                    kprinthex((uint64_t)rc_resp);
                    kprint("), retrying "); kprinthex((uint64_t)attempt);
                    kprint("/30...\n");
                }
                for (volatile int d = 0; d < 1000000; d++);
                continue;
            }
        }

        kprint("[VIO-SCSI] READ CAPACITY: response=");
        kprinthex((uint64_t)rc_resp);
        kprint(" status="); kprinthex((uint64_t)g_scsi_resp.status);
        kprint("\n");
        return -1;
    }

    kprint("[VIO-SCSI] READ CAPACITY exhausted retries\n");
    return -1;
}

/* =========================================================================
 * BlockDevOps implementation
 *
 * The private_data field of each BlockDev points to the vio_scsi_device_t
 * so the read/write/capacity callbacks know which port to address.
 * ========================================================================= */

static int vio_scsi_bdev_read(BlockDev *bdev, uint64_t sector, void *buffer,
                              uint32_t num_sectors) {
    if (!g_active || !bdev || !bdev->private_data) return -1;
    vio_scsi_device_t *dev = (vio_scsi_device_t *)bdev->private_data;
    uint32_t ss = dev->sector_size;

    if (num_sectors * ss > sizeof(g_data_buffer)) {
        kprint("[VIO-SCSI] Read too large\n");
        return -1;
    }

    /* Build READ(10) CDB */
    uint8_t cdb[32];
    for (int i = 0; i < 32; i++) cdb[i] = 0;
    cdb[0] = SCSI_READ_10;
    cdb[2] = (uint8_t)((sector >> 24) & 0xFF);
    cdb[3] = (uint8_t)((sector >> 16) & 0xFF);
    cdb[4] = (uint8_t)((sector >> 8) & 0xFF);
    cdb[5] = (uint8_t)(sector & 0xFF);
    cdb[7] = (uint8_t)((num_sectors >> 8) & 0xFF);
    cdb[8] = (uint8_t)(num_sectors & 0xFF);

    uint64_t data_phys = DMA_VirtToPhys(g_data_buffer);
    if (!data_phys) {
        if (DMA_IsAccessible(buffer)) {
            data_phys = DMA_VirtToPhys(buffer);
            if (vio_scsi_submit(cdb, dev->port, dev->lun, data_phys, num_sectors * ss, 0) != 0)
                return -1;
            return vio_scsi_wait_completion();
        }
        kprint("[VIO-SCSI] No DMA-accessible buffer\n");
        return -1;
    }

    /* Retry reads up to 3 times — a single timed-out request during
     * probing or auto-mount drops the whole disk for that boot. */
    for (int attempt = 0; attempt < 3; attempt++) {
        if (vio_scsi_submit(cdb, dev->port, dev->lun, data_phys, num_sectors * ss, 0) != 0) {
            for (volatile int d = 0; d < 100000; d++);
            continue;
        }
        if (vio_scsi_wait_completion() == 0) {
            uint8_t *dst = (uint8_t *)buffer;
            for (uint32_t i = 0; i < num_sectors * ss; i++)
                dst[i] = g_data_buffer[i];
            return 0;
        }
        kprint("[VIO-SCSI] READ retry "); kprinthex((uint64_t)(attempt + 1));
        kprint("/3 (sector="); kprinthex(sector); kprint(")\n");
        for (volatile int d = 0; d < 1000000; d++);
    }
    kprint("[VIO-SCSI] READ exhausted retries (sector="); kprinthex(sector);
    kprint(" nsec="); kprinthex((uint64_t)num_sectors); kprint(")\n");
    return -1;
}

static int vio_scsi_bdev_write(BlockDev *bdev, uint64_t sector, const void *buffer,
                               uint32_t num_sectors) {
    if (!g_active || !bdev || !bdev->private_data) return -1;
    vio_scsi_device_t *dev = (vio_scsi_device_t *)bdev->private_data;
    uint32_t ss = dev->sector_size;

    /* CD-ROMs are read-only */
    if (dev->is_cdrom) {
        kprint("[VIO-SCSI] Write to read-only CD-ROM\n");
        return -1;
    }

    if (num_sectors * ss > sizeof(g_data_buffer)) {
        kprint("[VIO-SCSI] Write too large\n");
        return -1;
    }

    uint8_t cdb[32];
    for (int i = 0; i < 32; i++) cdb[i] = 0;
    cdb[0] = SCSI_WRITE_10;
    cdb[2] = (uint8_t)((sector >> 24) & 0xFF);
    cdb[3] = (uint8_t)((sector >> 16) & 0xFF);
    cdb[4] = (uint8_t)((sector >> 8) & 0xFF);
    cdb[5] = (uint8_t)(sector & 0xFF);
    cdb[7] = (uint8_t)((num_sectors >> 8) & 0xFF);
    cdb[8] = (uint8_t)(num_sectors & 0xFF);

    const uint8_t *src = (const uint8_t *)buffer;
    for (uint32_t i = 0; i < num_sectors * ss; i++)
        g_data_buffer[i] = src[i];

    uint64_t data_phys = DMA_VirtToPhys(g_data_buffer);
    if (!data_phys) {
        if (DMA_IsAccessible((void *)buffer)) {
            data_phys = DMA_VirtToPhys((void *)buffer);
            if (vio_scsi_submit(cdb, dev->port, dev->lun, data_phys, num_sectors * ss, 1) != 0)
                return -1;
            return vio_scsi_wait_completion();
        }
        kprint("[VIO-SCSI] No DMA-accessible buffer\n");
        return -1;
    }

    /* Retry writes up to 3 times — the device may need a brief recovery
     * time after many large sequential writes (e.g. FAT zeroing). */
    for (int attempt = 0; attempt < 3; attempt++) {
        if (vio_scsi_submit(cdb, dev->port, dev->lun, data_phys, num_sectors * ss, 1) != 0) {
            kprint("[VIO-SCSI] WRITE submit failed (sector="); kprinthex(sector); kprint(")\n");
            /* Brief delay before retry */
            for (volatile int d = 0; d < 100000; d++);
            continue;
        }
        int rc = vio_scsi_wait_completion();
        if (rc == 0) return 0;
        kprint("[VIO-SCSI] WRITE retry "); kprinthex((uint64_t)(attempt + 1));
        kprint("/3 (sector="); kprinthex(sector); kprint(")\n");
        /* Brief delay before retry */
        for (volatile int d = 0; d < 1000000; d++);
    }
    kprint("[VIO-SCSI] WRITE exhausted retries (sector="); kprinthex(sector);
    kprint(" nsec="); kprinthex((uint64_t)num_sectors); kprint(")\n");
    return -1;
}

static uint64_t vio_scsi_bdev_capacity(BlockDev *bdev) {
    if (!bdev || !bdev->private_data) return 0;
    vio_scsi_device_t *dev = (vio_scsi_device_t *)bdev->private_data;
    return dev->capacity;
}

static const BlockDevOps vio_scsi_ops = {
    .read        = vio_scsi_bdev_read,
    .write       = vio_scsi_bdev_write,
    .get_capacity = vio_scsi_bdev_capacity,
};

/* =========================================================================
 * IRQ handler
 * ========================================================================= */

static void vio_scsi_irq_handler(uint64_t vector, uint64_t error_code) {
    (void)vector; (void)error_code;
    if (!g_active) return;
    uint8_t isr = vio_isr_read();
    if (isr & 1)
        g_irq_pending = 1;
    PIC_SendEOI(g_irq_line);
}

/* =========================================================================
 * Target scanning and device registration
 * ========================================================================= */

/* Scan a single port: probe with TEST UNIT READY + INQUIRY, then register
 * the appropriate block device.  Returns 1 if a device was registered. */
static int vio_scsi_probe_port(int port) {
    kprint("[VIO-SCSI] Probing port "); kprinthex((uint64_t)port); kprint("...\n");

    /* Wait for the unit to become ready (CD-ROMs need spin-up time).
     * Use a generous retry count to handle slow spin-ups. */
    if (vio_scsi_wait_unit_ready(port, 20) != 0) {
        kprint("[VIO-SCSI] Target "); kprinthex((uint64_t)port);
        kprint(" not ready, skipping\n");
        return 0;
    }

    /* Identify the device type via INQUIRY */
    int periph_type = vio_scsi_inquiry(port);
    if (periph_type < 0) {
        kprint("[VIO-SCSI] INQUIRY failed for port ");
        kprinthex((uint64_t)port); kprint("\n");
        return 0;
    }

    kprint("[VIO-SCSI] Target "); kprinthex((uint64_t)port);
    kprint(" peripheral type="); kprinthex((uint64_t)periph_type); kprint("\n");

    if (g_num_devices >= VIO_SCSI_MAX_PORTS) {
        kprint("[VIO-SCSI] Max devices reached, skipping port ");
        kprinthex((uint64_t)port); kprint("\n");
        return 0;
    }

    vio_scsi_device_t *dev = &g_devices[g_num_devices];
    dev->active = 1;
    dev->port = port;
    dev->lun = 0;

    if (periph_type == SCSI_PERIPH_CDROM) {
        /* CD/DVD-ROM: 2048-byte sectors, read-only */
        dev->is_cdrom    = 1;
        dev->sector_size = 2048;

        uint64_t capacity;
        uint32_t blk_size;
        if (vio_scsi_read_capacity(port, &capacity, &blk_size) != 0) {
            kprint("[VIO-SCSI] READ CAPACITY failed for CD-ROM port ");
            kprinthex((uint64_t)port); kprint("\n");
            dev->active = 0;
            return 0;
        }
        dev->capacity = capacity;

        /* Register as "vio_cd0" so the boot code can find it for ISO9660 */
        dev->bdev.name         = "vio_cd0";
        dev->bdev.display_name = "CD0:";
        dev->bdev.sector_size  = 2048;
        dev->bdev.num_sectors  = capacity;
        dev->bdev.part_offset  = 0;
        dev->bdev.formatted    = 0;
        dev->bdev.private_data = dev;
        dev->bdev.ops          = &vio_scsi_ops;
        dev->bdev.next         = NULL;

        if (BlockDev_Register(&dev->bdev) != 0) {
            kprint("[VIO-SCSI] Failed to register CD-ROM block device\n");
            dev->active = 0;
            return 0;
        }

        kprint("[VIO-SCSI] Registered CD-ROM vio_cd0 (");
        kprinthex(capacity); kprint(" sectors, 2048-byte)\n");
        g_num_devices++;
        return 1;

    } else if (periph_type == SCSI_PERIPH_DIRECT) {
        /* Hard disk: 512-byte sectors, read/write.
         * But VirtualBox's virtio-scsi may report a DVD as type 0 (direct
         * access) instead of type 5 (CD-ROM).  If READ CAPACITY fails, try
         * a test read with 2048-byte sectors to detect this case. */
        dev->is_cdrom    = 0;
        dev->sector_size = 512;

        uint64_t capacity;
        uint32_t blk_size;
        if (vio_scsi_read_capacity(port, &capacity, &blk_size) != 0) {
            kprint("[VIO-SCSI] READ CAPACITY failed for type-0 port ");
            kprinthex((uint64_t)port);
            kprint(", trying CD-ROM fallback (2048-byte)...\n");

            /* Try reading the ISO 9660 PVD at sector 16 with 2048-byte
             * sectors.  Retry a few times in case the device is still
             * spinning up.  If the data contains the "CD001" magic,
             * this is actually a CD-ROM. */
            uint64_t data_phys = DMA_VirtToPhys(g_data_buffer);

            for (int read_attempt = 0; read_attempt < 10; read_attempt++) {
                uint8_t cdb[32];
                for (int i = 0; i < 32; i++) cdb[i] = 0;
                cdb[0] = SCSI_READ_10;
                cdb[2] = 0; cdb[3] = 0; cdb[4] = 0; cdb[5] = 16;  /* LBA 16 */
                cdb[7] = 0; cdb[8] = 1;                             /* 1 sector */

                uint8_t resp = 0;
                if (!data_phys) break;
                if (vio_scsi_submit(cdb, port, 0, data_phys, 2048, 0) != 0) break;
                if (vio_scsi_wait_completion_ext(&resp) != 0) {
                    if (resp == 0xFF) {
                        continue;
                    }
                    /* BUSY — wait and retry */
                    if (resp == VIO_SCSI_RESP_BUSY || resp == VIO_SCSI_RESP_OK) {
                        for (volatile int d = 0; d < 1000000; d++);
                        continue;
                    }
                    break;
                }

                /* Check for ISO 9660 magic at offset 1: "CD001" */
                if (g_data_buffer[0] == 0x01 &&
                    g_data_buffer[1] == 'C' && g_data_buffer[2] == 'D' &&
                    g_data_buffer[3] == '0' && g_data_buffer[4] == '0' &&
                    g_data_buffer[5] == '1') {
                    kprint("[VIO-SCSI] ISO 9660 magic found — treating as CD-ROM\n");
                    dev->is_cdrom    = 1;
                    dev->sector_size = 2048;
                    /* Volume space size at PVD offset 80 (LE) or 84 (BE) */
                    uint32_t vol_size = ((uint32_t)g_data_buffer[84] << 24) |
                                        ((uint32_t)g_data_buffer[85] << 16) |
                                        ((uint32_t)g_data_buffer[86] << 8)  |
                                        (uint32_t)g_data_buffer[87];
                    if (vol_size == 0)
                        vol_size = ((uint32_t)g_data_buffer[80] << 0)  |
                                   ((uint32_t)g_data_buffer[81] << 8)  |
                                   ((uint32_t)g_data_buffer[82] << 16) |
                                   ((uint32_t)g_data_buffer[83] << 24);
                    if (vol_size == 0) vol_size = 332800;  /* fallback ~650MB */
                    dev->capacity = vol_size;

                    dev->bdev.name         = "vio_cd0";
                    dev->bdev.display_name = "CD0:";
                    dev->bdev.sector_size  = 2048;
                    dev->bdev.num_sectors  = vol_size;
                    dev->bdev.part_offset  = 0;
                    dev->bdev.formatted    = 0;
                    dev->bdev.private_data = dev;
                    dev->bdev.ops          = &vio_scsi_ops;
                    dev->bdev.next         = NULL;

                    if (BlockDev_Register(&dev->bdev) != 0) {
                        kprint("[VIO-SCSI] Failed to register CD-ROM block device\n");
                        dev->active = 0;
                        return 0;
                    }

                    kprint("[VIO-SCSI] Registered CD-ROM vio_cd0 (");
                    kprinthex((uint64_t)vol_size); kprint(" sectors, 2048-byte)\n");
                    g_num_devices++;
                    return 1;
                }

                kprint("[VIO-SCSI] Read succeeded but no ISO 9660 magic (attempt ");
                kprinthex((uint64_t)read_attempt); kprint(")\n");
                /* Dump first 8 bytes for debugging */
                kprint("[VIO-SCSI]   data[0..7]=");
                for (int b = 0; b < 8; b++) { kprinthex((uint64_t)g_data_buffer[b]); kprint(" "); }
                kprint("\n");
            }

            kprint("[VIO-SCSI] CD-ROM fallback failed for port ");
            kprinthex((uint64_t)port); kprint("\n");
            dev->active = 0;
            return 0;
        }
        /* Convert to 512-byte sectors for the block device layer */
        dev->capacity = (capacity * blk_size) / 512;

        dev->bdev.name         = "virtio0";
        dev->bdev.display_name = "DH0:";
        dev->bdev.sector_size  = 512;
        dev->bdev.num_sectors  = dev->capacity;
        dev->bdev.part_offset  = 0;
        dev->bdev.formatted    = 0;
        dev->bdev.private_data = dev;
        dev->bdev.ops          = &vio_scsi_ops;
        dev->bdev.next         = NULL;

        if (BlockDev_Register(&dev->bdev) != 0) {
            kprint("[VIO-SCSI] Failed to register disk block device\n");
            dev->active = 0;
            return 0;
        }

        kprint("[VIO-SCSI] Registered disk virtio0 (");
        kprinthex(dev->capacity); kprint(" sectors, 512-byte)\n");
        g_num_devices++;
        return 1;

    } else {
        kprint("[VIO-SCSI] Unsupported peripheral type ");
        kprinthex((uint64_t)periph_type); kprint(" at port ");
        kprinthex((uint64_t)port); kprint(", skipping\n");
        dev->active = 0;
        return 0;
    }
}

/* =========================================================================
 * Public API
 * ========================================================================= */

int virtio_scsi_init(void) {
    kprint("[VIO-SCSI] Scanning PCI bus for virtio-scsi...\n");
    if (!vio_find_device()) {
        kprint("[VIO-SCSI] No virtio-scsi controller found\n");
        return -1;
    }

    kprint("[VIO-SCSI] Initializing device...\n");
    if (vio_device_init() != 0) {
        kprint("[VIO-SCSI] Device initialization failed\n");
        return -1;
    }
    kprint("[VIO-SCSI] Device initialized successfully\n");

    g_active = 1;  /* controller is live — bdev callbacks can now use it */

    /* Scan ports 0..VIO_SCSI_MAX_PORTS-1 (LUN 0) for hard disks and CD-ROMs */
    int found = 0;
    for (int t = 0; t < VIO_SCSI_MAX_PORTS; t++) {
        if (vio_scsi_probe_port(t))
            found++;
    }

    if (found == 0) {
        kprint("[VIO-SCSI] No usable devices found on virtio-scsi controller\n");
        g_active = 0;
        return -1;
    }

    return 0;
}

void virtio_scsi_setup_irq(void) {
    if (!g_active) return;
    if (g_irq_line >= 0 && g_irq_line < 16) {
        uint8_t vector = (uint8_t)(32 + g_irq_line);
        IDT_SetHandler(vector, vio_scsi_irq_handler);
        PIC_UnmaskIRQ(g_irq_line);
        kprint("[VIO-SCSI] IRQ handler registered for IRQ ");
        kprinthex((uint64_t)g_irq_line); kprint("\n");
    } else {
        kprint("[VIO-SCSI] Invalid IRQ line, using polling only\n");
    }
}

int virtio_scsi_get_irq_line(void) {
    return g_irq_line;
}

int virtio_scsi_is_active(void) {
    return g_active;
}

int virtio_scsi_has_cdrom(void) {
    for (int i = 0; i < g_num_devices; i++) {
        if (g_devices[i].active && g_devices[i].is_cdrom)
            return 1;
    }
    return 0;
}

int virtio_scsi_read(uint64_t sector, void *buffer, uint32_t num_sectors) {
    /* Legacy API: use the first registered device */
    for (int i = 0; i < g_num_devices; i++) {
        if (g_devices[i].active && !g_devices[i].is_cdrom) {
            return vio_scsi_bdev_read(&g_devices[i].bdev, sector, buffer, num_sectors);
        }
    }
    return -1;
}

int virtio_scsi_write(uint64_t sector, const void *buffer, uint32_t num_sectors) {
    for (int i = 0; i < g_num_devices; i++) {
        if (g_devices[i].active && !g_devices[i].is_cdrom) {
            return vio_scsi_bdev_write(&g_devices[i].bdev, sector, buffer, num_sectors);
        }
    }
    return -1;
}

uint64_t virtio_scsi_get_capacity(void) {
    for (int i = 0; i < g_num_devices; i++) {
        if (g_devices[i].active && !g_devices[i].is_cdrom) {
            return g_devices[i].capacity;
        }
    }
    return 0;
}
