/*
 * virtio_blk.c — UAOS VirtIO Block Device Driver
 *
 * Implements VirtIO block device driver for QEMU external disk support.
 * VirtIO is a paravirtualized I/O framework used by QEMU/KVM.
 *
 * Supports multiple disks: every virtio-blk PCI device found is registered
 * as virtio0, virtio1, ... in PCI enumeration order.  Each device has its
 * own virtqueue; request/response scratch buffers are shared because all
 * I/O is synchronous and serialised by the BlockDev layer.
 */

#include "virtio_blk.h"
#include "../dos/blockdev.h"
#include "../dos/dma.h"
#include "../boot/kprint.h"
#include "idt.h"
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* =========================================================================
 * VirtIO PCI Configuration Space
 * ========================================================================= */

#define VIRTIO_PCI_VENDOR_ID      0x1AF4  /* Red Hat */
#define VIRTIO_PCI_DEVICE_ID_BLK  0x1001  /* VirtIO Block Device */

#define PCI_CONFIG_ADDRESS_PORT   0xCF8
#define PCI_CONFIG_DATA_PORT      0xCFC

#define PCI_VENDOR_ID_OFFSET      0x00
#define PCI_DEVICE_ID_OFFSET      0x02
#define PCI_CLASS_CODE_OFFSET     0x0B
#define PCI_BAR0_OFFSET           0x10
#define PCI_BAR1_OFFSET           0x14
#define PCI_BAR2_OFFSET           0x18
#define PCI_BAR3_OFFSET           0x1C
#define PCI_BAR4_OFFSET           0x20
#define PCI_BAR5_OFFSET           0x24

/* =========================================================================
 * VirtIO Block Device Configuration
 * ========================================================================= */

/* Device config offsets within BAR0 (after common config at 0x00-0x13) */
#define VIRTIO_BLK_CAPACITY      0x14  /* Number of 512-byte sectors */
#define VIRTIO_BLK_SIZE_MAX      0x1C  /* Maximum segment size */
#define VIRTIO_BLK_SEG_MAX       0x20  /* Maximum number of segments */
#define VIRTIO_BLK_BLK_SIZE      0x24  /* Block size in bytes */

/* VirtIO Block Device Features */
#define VIRTIO_BLK_F_BARRIER     (1 << 0)
#define VIRTIO_BLK_F_SIZE_MAX    (1 << 1)
#define VIRTIO_BLK_F_SEG_MAX     (1 << 2)
#define VIRTIO_BLK_F_GEOMETRY    (1 << 4)
#define VIRTIO_BLK_F_RO          (1 << 5)
#define VIRTIO_BLK_F_BLK_SIZE    (1 << 6)
#define VIRTIO_BLK_F_FLUSH       (1 << 9)

/* VirtIO Block Device Request Types */
#define VIRTIO_BLK_T_IN          0
#define VIRTIO_BLK_T_OUT         1
#define VIRTIO_BLK_T_FLUSH       4

/* VirtIO Block Device Status */
#define VIRTIO_BLK_S_OK          0
#define VIRTIO_BLK_S_IOERR       1
#define VIRTIO_BLK_S_UNSUPP      2

/* VirtIO Legacy PCI Queue Registers (in BAR0) */
#define VIRTIO_PCI_QUEUE_NOTIFY    0x10
#define VIRTIO_PCI_QUEUE_ADDR      0x08
#define VIRTIO_PCI_QUEUE_SIZE      0x0C
#define VIRTIO_PCI_QUEUE_SEL       0x0E
#define VIRTIO_PCI_QUEUE_NUM       0x0C

/* VirtIO Legacy PCI Common Configuration (in BAR0) */
#define VIRTIO_PCI_STATUS          0x12
#define VIRTIO_PCI_DEVICE_FEATURES 0x00
#define VIRTIO_PCI_DRIVER_FEATURES 0x04
#define VIRTIO_PCI_ISR             0x19

/* VirtIO PCI Configuration Space offsets */
#define PCI_INTERRUPT_LINE_OFFSET  0x3C

/* VirtIO Status bits */
#define VIRTIO_STATUS_ACKNOWLEDGE     0x01
#define VIRTIO_STATUS_DRIVER          0x02
#define VIRTIO_STATUS_DRIVER_OK       0x04
#define VIRTIO_STATUS_FEATURES_OK     0x08
#define VIRTIO_STATUS_DEVICE_NEEDS_RESET 0x40
#define VIRTIO_STATUS_FAILED          0x80

/* VirtIO Descriptor flags */
#define VIRTQ_DESC_F_NEXT     1
#define VIRTQ_DESC_F_WRITE    2
#define VIRTQ_DESC_F_INDIRECT 4

/* =========================================================================
 * VirtIO Queue (Virtqueue) Structure
 * ========================================================================= */

#define VIRTIO_QUEUE_SIZE        256
#define VIRTIO_QUEUE_ALIGN       4096

typedef struct {
    uint64_t addr;      /* Physical address */
    uint32_t len;       /* Length */
    uint16_t flags;     /* Flags */
    uint16_t next;      /* Next descriptor */
} __attribute__((packed)) virtq_desc_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VIRTIO_QUEUE_SIZE];
} __attribute__((packed)) virtq_avail_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VIRTIO_QUEUE_SIZE];
} __attribute__((packed)) virtq_used_t;

/* Virtqueue structure for legacy VirtIO
 * Layout per VirtIO 1.0 spec (Legacy PCI, Section 4.1.5.1.2):
 *   - Descriptor table: offset 0
 *   - Available ring: immediately after descriptor table
 *   - Used ring: next 4096-byte (page) boundary after available ring
 *
 * For queue_size = 256:
 *   desc_table   = 256 * 16 = 4096 bytes
 *   avail_ring   = 2 + 2 + 256 * 2 = 516 bytes
 *   total_before = 4612 bytes
 *   used_offset  = round_up(4612, 4096) = 8192
 *   padding      = 8192 - 4612 = 3580 bytes
 */
#define VIRTQ_DESC_SIZE   (VIRTIO_QUEUE_SIZE * sizeof(virtq_desc_t))
#define VIRTQ_AVAIL_SIZE  (4 + VIRTIO_QUEUE_SIZE * 2)
#define VIRTQ_USED_OFFSET ((((VIRTQ_DESC_SIZE + VIRTQ_AVAIL_SIZE) + 4095) / 4096) * 4096)
#define VIRTQ_PADDING     (VIRTQ_USED_OFFSET - VIRTQ_DESC_SIZE - VIRTQ_AVAIL_SIZE)

typedef struct {
    /* Descriptor table at offset 0 */
    virtq_desc_t desc[VIRTIO_QUEUE_SIZE];

    /* Available ring immediately after */
    uint16_t avail_flags;
    uint16_t avail_idx;
    uint16_t avail_ring[VIRTIO_QUEUE_SIZE];

    /* Padding to align used ring to next 4096-byte boundary */
    uint8_t padding[VIRTQ_PADDING];

    /* Used ring at aligned offset */
    uint16_t used_flags;
    uint16_t used_idx;
    struct {
        uint32_t id;
        uint32_t len;
    } used_ring[VIRTIO_QUEUE_SIZE];
} virtq_t;

/* =========================================================================
 * VirtIO Block Device Request/Response
 * ========================================================================= */

typedef struct {
    uint32_t type;
    uint32_t ioprio;
    uint64_t sector;
} virtio_blk_req_t;

typedef struct {
    uint8_t status;
} virtio_blk_resp_t;

/* =========================================================================
 * Per-device state
 * ========================================================================= */

#define VBLK_MAX_DEVS 4

/* Virtqueue + bookkeeping for one disk.  The 4096-byte type alignment makes
 * every array element page-aligned so each vq can be handed to QueuePFN. */
typedef struct __attribute__((aligned(4096))) {
    volatile virtq_t vq;
    uint16_t free_idx;
    uint16_t used_idx;
    volatile int irq_seen;
} vblk_qstate_t;

typedef struct {
    uint8_t   pci_bus, pci_dev, pci_func;
    uint32_t  mmio_base;      /* BAR0 I/O port base */
    uint64_t  capacity;       /* sectors */
    int       irq_line;
    vblk_qstate_t *q;
    BlockDev  bdev;
    char      name[12];       /* "virtio0" .. "virtio3" */
    char      disp[12];
} vblk_dev_t;

static vblk_dev_t    g_devs[VBLK_MAX_DEVS];
static vblk_qstate_t g_qstates[VBLK_MAX_DEVS];
static int           g_ndevs = 0;

/* Legacy debug globals (referenced by ramfs/iso9660/kernel_main canaries) */
unsigned int g_canary_before = 0xDEADBEEF;
int g_virtio_irq_line = -1;
unsigned int g_canary_after = 0xCAFEBABE;

/* DMA-aligned buffers for I/O — shared across devices; all I/O is
 * synchronous and serialised at the BlockDev layer. */
static uint8_t g_virtq_buffer[4096] __attribute__((aligned(4096)));
static virtio_blk_req_t g_blk_req __attribute__((aligned(4096)));
static volatile virtio_blk_resp_t g_blk_resp __attribute__((aligned(4096)));
static uint8_t g_data_buffer[65536] __attribute__((aligned(4096)));

/* =========================================================================
 * I/O Port Access
 * ========================================================================= */

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline void outw(uint16_t port, uint16_t value)
{
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline void outl(uint16_t port, uint32_t value)
{
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/* =========================================================================
 * PCI Configuration Space Access
 * ========================================================================= */

static uint32_t pci_config_read_dword(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset)
{
    uint32_t address = (1 << 31) | (bus << 16) | (dev << 11) | (func << 8) | (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS_PORT, address);
    return inl(PCI_CONFIG_DATA_PORT);
}

static uint16_t pci_config_read_word(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset)
{
    uint32_t address = (1 << 31) | (bus << 16) | (dev << 11) | (func << 8) | (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS_PORT, address);
    return (uint16_t)(inl(PCI_CONFIG_DATA_PORT) >> ((offset & 2) * 8));
}

static uint8_t pci_config_read_byte(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset)
{
    uint32_t address = (1 << 31) | (bus << 16) | (dev << 11) | (func << 8) | (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS_PORT, address);
    return (uint8_t)(inl(PCI_CONFIG_DATA_PORT) >> ((offset & 3) * 8));
}

/* =========================================================================
 * VirtIO Device Initialization
 * ========================================================================= */

static int virtio_device_init(vblk_dev_t *d)
{
    kprint("[VIRTIO] Starting device initialization...\n");

    /* Reset the device */
    outb(d->mmio_base + VIRTIO_PCI_STATUS, 0);

    /* Set ACKNOWLEDGE status bit */
    uint8_t status = inb(d->mmio_base + VIRTIO_PCI_STATUS);
    outb(d->mmio_base + VIRTIO_PCI_STATUS, status | VIRTIO_STATUS_ACKNOWLEDGE);

    /* Set DRIVER status bit */
    status = inb(d->mmio_base + VIRTIO_PCI_STATUS);
    outb(d->mmio_base + VIRTIO_PCI_STATUS, status | VIRTIO_STATUS_DRIVER);

    /* Read device features (we accept all for now) */
    uint32_t features = inl(d->mmio_base + VIRTIO_PCI_DEVICE_FEATURES);
    outl(d->mmio_base + VIRTIO_PCI_DRIVER_FEATURES, features);

    /* Set FEATURES_OK status bit */
    status = inb(d->mmio_base + VIRTIO_PCI_STATUS);
    outb(d->mmio_base + VIRTIO_PCI_STATUS, status | VIRTIO_STATUS_FEATURES_OK);

    /* Check if FEATURES_OK is still set (device accepted our features) */
    status = inb(d->mmio_base + VIRTIO_PCI_STATUS);
    if (!(status & VIRTIO_STATUS_FEATURES_OK)) {
        kprint("[VIRTIO] Device rejected our features\n");
        return -1;
    }

    /* Set DRIVER_OK status bit */
    status = inb(d->mmio_base + VIRTIO_PCI_STATUS);
    outb(d->mmio_base + VIRTIO_PCI_STATUS, status | VIRTIO_STATUS_DRIVER_OK);

    return 0;
}

/* =========================================================================
 * VirtQueue Setup
 * ========================================================================= */

static int virtio_setup_queue(vblk_dev_t *d)
{
    /* Select queue 0 (the only queue for block device) */
    outw(d->mmio_base + VIRTIO_PCI_QUEUE_SEL, 0);

    /* Read max queue size (Queue Num is read-only in legacy PCI) */
    uint16_t max_queue_size = inw(d->mmio_base + VIRTIO_PCI_QUEUE_NUM);
    kprint("[VIRTIO] Max queue size: ");
    kprinthex((uint64_t)max_queue_size);
    kprint("\n");

    if (max_queue_size == 0) {
        kprint("[VIRTIO] Queue 0 not available\n");
        return -1;
    }

    if (max_queue_size < VIRTIO_QUEUE_SIZE) {
        kprint("[VIRTIO] Warning: Device queue size smaller than requested\n");
        /* Continue with what we have - actual used size is min(ours, device_max) */
    }

    /* Get physical address of the device's virtqueue structure */
    uint64_t virtq_phys = DMA_VirtToPhys((void *)&d->q->vq);

    if (!virtq_phys) {
        kprint("[VIRTIO] Failed to get virtqueue physical address\n");
        return -1;
    }

    /* For legacy VirtIO, the Queue Address register expects a PFN
     * (page frame number = physical address >> 12), not a raw address */
    uint32_t virtq_pfn = (uint32_t)(virtq_phys >> 12);

    /* Set queue address (PFN of descriptor table) */
    outl(d->mmio_base + VIRTIO_PCI_QUEUE_ADDR, virtq_pfn);

    return 0;
}

/* =========================================================================
 * Memory Barriers
 * ========================================================================= */

static inline void memory_barrier(void)
{
    /* Full x86 memory fence — ensures all prior stores are globally visible
     * before any subsequent store or I/O.  This is critical before notifying
     * the VirtIO device: without it the CPU store buffer may still hold the
     * avail_idx / descriptor updates, and the device sees stale data. */
    __asm__ volatile("mfence" ::: "memory");
}

static inline void io_barrier(void)
{
    /* x86 I/O barrier — serialises I/O operations */
    __asm__ volatile("mfence" ::: "memory");
}

/* =========================================================================
 * VirtQueue Request Submission
 * ========================================================================= */

static int virtio_submit_request(vblk_dev_t *d, uint64_t req_phys, uint64_t data_phys, uint32_t data_len, uint64_t resp_phys, int is_write)
{
    volatile virtq_t *vq = &d->q->vq;

    /* Get free descriptor index */
    uint16_t desc_idx = d->q->free_idx;
    if (desc_idx >= VIRTIO_QUEUE_SIZE) {
        kprint("[VIRTIO] No free descriptors\n");
        return -1;
    }

    /* Setup descriptor 0: request header (device-readable) */
    vq->desc[desc_idx].addr = req_phys;
    vq->desc[desc_idx].len = sizeof(virtio_blk_req_t);
    vq->desc[desc_idx].flags = VIRTQ_DESC_F_NEXT;
    vq->desc[desc_idx].next = desc_idx + 1;

    /* Setup descriptor 1: data buffer */
    vq->desc[desc_idx + 1].addr = data_phys;
    vq->desc[desc_idx + 1].len = data_len;
    if (is_write) {
        /* Device reads from buffer */
        vq->desc[desc_idx + 1].flags = VIRTQ_DESC_F_NEXT;
    } else {
        /* Device writes to buffer */
        vq->desc[desc_idx + 1].flags = VIRTQ_DESC_F_NEXT | VIRTQ_DESC_F_WRITE;
    }
    vq->desc[desc_idx + 1].next = desc_idx + 2;

    /* Setup descriptor 2: response (device-writable) */
    vq->desc[desc_idx + 2].addr = resp_phys;
    vq->desc[desc_idx + 2].len = sizeof(virtio_blk_resp_t);
    vq->desc[desc_idx + 2].flags = VIRTQ_DESC_F_WRITE;  /* Device writes response */
    vq->desc[desc_idx + 2].next = 0;

    /* Memory barrier to ensure descriptors are written before notifying device */
    memory_barrier();

    /* Update available ring */
    vq->avail_ring[vq->avail_idx % VIRTIO_QUEUE_SIZE] = desc_idx;
    vq->avail_idx++;

    /* Memory barrier to ensure available ring is updated before notification */
    memory_barrier();

    /* Notify device */
    outw(d->mmio_base + VIRTIO_PCI_QUEUE_NOTIFY, 0);

    /* Update free index (simple bump allocator) */
    d->q->free_idx += 3;

    return desc_idx;
}

/* =========================================================================
 * VirtIO Interrupt Handler
 * ========================================================================= */

static void virtio_irq_handler(uint64_t vector, uint64_t error_code)
{
    (void)vector; (void)error_code;

    /* Memory barrier to ensure we see the device's writes */
    memory_barrier();

    /* Poll every registered device — several disks may share one IRQ
     * line, and reading ISR also acknowledges the interrupt. */
    for (int i = 0; i < g_ndevs; i++) {
        vblk_dev_t *d = &g_devs[i];
        if (!d->mmio_base) continue;
        uint8_t isr = inb(d->mmio_base + VIRTIO_PCI_ISR);
        if (!(isr & 1)) continue;
        if (d->q->vq.used_idx != d->q->used_idx) {
            d->q->irq_seen = 1;
            d->q->used_idx = d->q->vq.used_idx;
        }
    }

    /* Note: EOI is sent by ISR_Dispatch in idt.c, don't send it here */
}

/* =========================================================================
 * VirtQueue Completion Polling
 * ========================================================================= */

static int virtio_wait_completion(vblk_dev_t *d, uint16_t desc_idx, uint32_t timeout_ms)
{
    (void)timeout_ms;
    uint16_t initial_used_idx = d->q->vq.used_idx;

    /* Wait for completion by polling the used ring index */
    uint32_t iterations = 0;
    while (1) {
        /* Memory barrier to ensure we see the device's writes */
        memory_barrier();

        /* Poll used_idx to detect completion */
        if (d->q->vq.used_idx != initial_used_idx) {
            /* Memory barrier to ensure we read the response correctly */
            memory_barrier();

            /* Check the response status */
            if (g_blk_resp.status == VIRTIO_BLK_S_OK) {
                return 0;  /* Success */
            } else {
                kprint("[VIRTIO] Device returned error status\n");
                return -1;
            }
        }

        /* Yield CPU.  In QEMU TCG we also force an I/O exit every so often
         * so the emulator’s event loop can run the virtio device. */
        __asm__ volatile("pause");
        if ((iterations % 500) == 0) {
            (void)inb(0x80);   /* dummy I/O → TCG block exit → QEMU events run */
        }

        /* Simple timeout check (TODO: implement proper timer).
         * Iteration budget must be generous: under QEMU TCG on slow
         * hosts (WSL) a qcow2-backed read can legitimately take well
         * over a second, and ~20M pause-loop iterations is only a few
         * hundred ms — too short, caused spurious I/O failures during
         * bulk scans like FAT free-space counting. */
        iterations++;
        if (iterations > 400000000) {
            kprint("[VIRTIO] Timeout waiting for completion\n");
            return -1;
        }
    }
}

/* =========================================================================
 * VirtIO Block Device Operations (BlockDevOps interface)
 * ========================================================================= */

static int virtio_blk_read_op(BlockDev *dev, uint64_t sector, void *buffer, uint32_t num_sectors)
{
    vblk_dev_t *d = (vblk_dev_t *)dev->private_data;
    if (!d || !d->mmio_base) {
        kprint("[VIRTIO] Device not initialized\n");
        return -1;
    }

    if (num_sectors * 512 > sizeof(g_data_buffer)) {
        kprint("[VIRTIO] Request too large\n");
        return -1;
    }

    /* Check if buffer is DMA-accessible */
    if (!DMA_IsAccessible(buffer)) {
        kprint("[VIRTIO] Buffer not DMA-accessible\n");
        return -1;
    }

    /* Setup read request */
    g_blk_req.type = VIRTIO_BLK_T_IN;
    g_blk_req.ioprio = 0;
    g_blk_req.sector = sector;

    /* Get physical addresses for DMA */
    uint64_t req_phys = DMA_VirtToPhys((void *)&g_blk_req);
    uint64_t resp_phys = DMA_VirtToPhys((void *)&g_blk_resp);
    uint64_t data_phys = DMA_VirtToPhys((void *)buffer);

    if (!req_phys || !resp_phys || !data_phys) {
        kprint("[VIRTIO] Failed to get physical addresses\n");
        return -1;
    }

    /* Submit request */
    int desc_idx = virtio_submit_request(d, req_phys, data_phys, num_sectors * 512, resp_phys, 0);
    if (desc_idx < 0) {
        kprint("[VIRTIO] Failed to submit request\n");
        return -1;
    }

    /* Wait for completion */
    if (virtio_wait_completion(d, desc_idx, 1000) != 0) {
        kprint("[VIRTIO] Request failed or timed out\n");
        /* Reset descriptor index even on failure so the next I/O
         * doesn't start at a stale descriptor index. */
        d->q->free_idx = 0;
        return -1;
    }

    /* Reset descriptor index for reuse (synchronous driver) */
    d->q->free_idx = 0;

    return 0;
}

static int virtio_blk_write_op(BlockDev *dev, uint64_t sector, const void *buffer, uint32_t num_sectors)
{
    vblk_dev_t *d = (vblk_dev_t *)dev->private_data;
    if (!d || !d->mmio_base) {
        kprint("[VIRTIO] Device not initialized\n");
        return -1;
    }

    if (num_sectors * 512 > sizeof(g_data_buffer)) {
        kprint("[VIRTIO] Request too large\n");
        return -2;
    }

    /* Check if buffer is DMA-accessible */
    if (!DMA_IsAccessible((void *)buffer)) {
        kprint("[VIRTIO] Buffer not DMA-accessible\n");
        return -3;
    }

    /* Setup write request */
    g_blk_req.type = VIRTIO_BLK_T_OUT;
    g_blk_req.ioprio = 0;
    g_blk_req.sector = sector;

    /* Get physical addresses for DMA */
    uint64_t req_phys = DMA_VirtToPhys((void *)&g_blk_req);
    uint64_t resp_phys = DMA_VirtToPhys((void *)&g_blk_resp);
    uint64_t data_phys = DMA_VirtToPhys((void *)buffer);

    if (!req_phys || !resp_phys || !data_phys) {
        kprint("[VIRTIO] Failed to get physical addresses\n");
        return -4;
    }

    /* Submit request */
    int desc_idx = virtio_submit_request(d, req_phys, data_phys, num_sectors * 512, resp_phys, 1);
    if (desc_idx < 0) {
        kprint("[VIRTIO] Failed to submit request\n");
        return -5;
    }

    /* Wait for completion */
    if (virtio_wait_completion(d, desc_idx, 1000) != 0) {
        kprint("[VIRTIO] Request failed or timed out\n");
        d->q->free_idx = 0;
        return -6;
    }

    /* Reset descriptor index for reuse (synchronous driver) */
    d->q->free_idx = 0;

    return 0;
}

static uint64_t virtio_blk_get_capacity_op(BlockDev *dev)
{
    vblk_dev_t *d = (vblk_dev_t *)dev->private_data;
    return d ? d->capacity : 0;
}

static const BlockDevOps virtio_blk_ops = {
    .read = virtio_blk_read_op,
    .write = virtio_blk_write_op,
    .get_capacity = virtio_blk_get_capacity_op,
};

/* =========================================================================
 * VirtIO Block Device Initialization
 * ========================================================================= */

static int virtio_blk_init_one(uint8_t bus, uint8_t dev, uint8_t func)
{
    if (g_ndevs >= VBLK_MAX_DEVS) {
        kprint("[VIRTIO] Max device count reached, skipping disk\n");
        return -1;
    }

    vblk_dev_t *d = &g_devs[g_ndevs];
    memset(d, 0, sizeof(*d));
    d->pci_bus = bus; d->pci_dev = dev; d->pci_func = func;
    d->q = &g_qstates[g_ndevs];
    memset((void *)d->q, 0, sizeof(*d->q));

    /* Get BAR0 (I/O space for legacy VirtIO device) */
    uint32_t bar0 = pci_config_read_dword(bus, dev, func, PCI_BAR0_OFFSET);
    d->mmio_base = bar0 & ~0x03;   /* strip I/O flag + low bits */

    kprint("[VIRTIO] virtio");
    kprinthex(g_ndevs);
    kprint(" at pci ");
    kprinthex(bus); kprint(":"); kprinthex(dev); kprint("."); kprinthex(func);
    kprint(" bar0=");
    kprinthex(d->mmio_base);
    kprint("\n");

    /* Read device capacity using port I/O */
    uint32_t capacity_low = inl(d->mmio_base + VIRTIO_BLK_CAPACITY);
    uint32_t capacity_high = inl(d->mmio_base + VIRTIO_BLK_CAPACITY + 4);
    d->capacity = ((uint64_t)capacity_high << 32) | capacity_low;

    /* Get IRQ line from PCI configuration */
    d->irq_line = pci_config_read_byte(bus, dev, func, PCI_INTERRUPT_LINE_OFFSET);
    kprint("[VIRTIO] IRQ line: ");
    kprinthex(d->irq_line);
    kprint("\n");

    /* Initialize VirtIO device */
    if (virtio_device_init(d) != 0) {
        kprint("[VIRTIO] Failed to initialize device\n");
        return -1;
    }

    /* Setup virtqueue */
    if (virtio_setup_queue(d) != 0) {
        kprint("[VIRTIO] Failed to setup virtqueue\n");
        return -1;
    }

    /* Register with block device layer */
    d->name[0] = 0;
    {
        static const char *pfx = "virtio";
        int i = 0;
        while (pfx[i]) { d->name[i] = pfx[i]; i++; }
        d->name[i++] = (char)('0' + g_ndevs);
        d->name[i] = 0;
    }
    d->bdev.name = d->name;
    d->bdev.display_name = d->name;
    d->bdev.sector_size = 512;
    d->bdev.num_sectors = d->capacity;
    d->bdev.private_data = d;
    d->bdev.ops = &virtio_blk_ops;
    d->bdev.next = NULL;

    if (BlockDev_Register(&d->bdev) != 0) {
        kprint("[VIRTIO] Failed to register with block device layer\n");
        return -1;
    }

    g_ndevs++;
    return 0;
}

int virtio_blk_init(void)
{
    /* Scan PCI bus for every VirtIO block device */
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            for (int func = 0; func < 8; func++) {
                uint16_t vendor_id = pci_config_read_word(bus, dev, func, PCI_VENDOR_ID_OFFSET);
                uint16_t device_id = pci_config_read_word(bus, dev, func, PCI_DEVICE_ID_OFFSET);

                if (vendor_id == VIRTIO_PCI_VENDOR_ID && device_id == VIRTIO_PCI_DEVICE_ID_BLK) {
                    virtio_blk_init_one(bus, dev, func);
                }
            }
        }
    }

    if (g_ndevs == 0) {
        kprint("[VIRTIO] No VirtIO block device found\n");
        return -1;
    }

    /* Legacy debug global — first disk's IRQ line (canary checks) */
    g_virtio_irq_line = g_devs[0].irq_line;

    kprint("[VIRTIO] Registered ");
    kprinthex(g_ndevs);
    kprint(" virtio block device(s)\n");
    return 0;
}

/* =========================================================================
 * VirtIO IRQ Setup (must be called AFTER IDT_Init and PIC_Init)
 * ========================================================================= */

int virtio_blk_get_irq_line(void)
{
    return g_virtio_irq_line;
}

/* =========================================================================
 * Legacy single-device entry points (virtio0)
 * ========================================================================= */

int virtio_blk_read(uint64_t sector, void *buffer, uint32_t num_sectors)
{
    if (g_ndevs == 0) return -1;
    return BlockDev_Read(&g_devs[0].bdev, sector, buffer, num_sectors);
}

int virtio_blk_write(uint64_t sector, const void *buffer, uint32_t num_sectors)
{
    if (g_ndevs == 0) return -1;
    return BlockDev_Write(&g_devs[0].bdev, sector, buffer, num_sectors);
}

uint64_t virtio_blk_get_capacity(void)
{
    return g_ndevs ? g_devs[0].capacity : 0;
}

void virtio_blk_setup_irq(void)
{
    if (g_ndevs == 0) {
        kprint("[VIRTIO] No device to setup IRQ for\n");
        return;
    }

    for (int i = 0; i < g_ndevs; i++) {
        vblk_dev_t *d = &g_devs[i];
        if (d->irq_line >= 0 && d->irq_line < 16) {
            uint8_t vector = 32 + d->irq_line;
            IDT_SetHandler(vector, virtio_irq_handler);
            PIC_UnmaskIRQ(d->irq_line);
            kprint("[VIRTIO] Interrupt handler registered for ");
            kprint(d->name);
            kprint(" IRQ ");
            kprinthex(d->irq_line);
            kprint("\n");
        } else {
            kprint("[VIRTIO] Invalid IRQ line for ");
            kprint(d->name);
            kprint(", interrupt not registered\n");
        }
    }
}
