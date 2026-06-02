/*
 * virtio_blk.c — UAOS VirtIO Block Device Driver
 *
 * Implements VirtIO block device driver for QEMU external disk support.
 * VirtIO is a paravirtualized I/O framework used by QEMU/KVM.
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
 /* VirtIO Block Device Configuration
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

/* Virtqueue state */
static volatile virtq_t g_virtq __attribute__((aligned(4096)));
static uint16_t g_virtq_free_idx = 0;
static uint16_t g_virtq_used_idx = 0;
static volatile int g_virtio_irq_pending = 0;
int g_canary_before = 0xDEADBEEF;
int g_virtio_irq_line = -1;
int g_canary_after = 0xCAFEBABE;

/* DMA-aligned buffers for I/O */
static uint8_t g_virtq_buffer[4096] __attribute__((aligned(4096)));
static virtio_blk_req_t g_blk_req __attribute__((aligned(4096)));
static volatile virtio_blk_resp_t g_blk_resp __attribute__((aligned(4096)));
static uint8_t g_data_buffer[65536] __attribute__((aligned(4096)));

/* =========================================================================
 * Global State
 * ========================================================================= */

static uint32_t virtio_blk_mmio_base = 0;
static uint64_t virtio_blk_capacity = 0;
static BlockDev g_virtio_blk_dev;

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
 * VirtIO MMIO Register Access
 * ========================================================================= */

static inline uint32_t virtio_readl(uint32_t offset)
{
    if (virtio_blk_mmio_base & 0x01) {
        /* Legacy I/O space - use port I/O */
        return inl(virtio_blk_mmio_base + offset);
    } else {
        /* Memory-mapped I/O - use memory access */
        volatile uint32_t *mmio = (volatile uint32_t *)(virtio_blk_mmio_base + offset);
        return *mmio;
    }
}

static inline void virtio_writel(uint32_t offset, uint32_t value)
{
    if (virtio_blk_mmio_base & 0x01) {
        /* Legacy I/O space - use port I/O */
        outl(virtio_blk_mmio_base + offset, value);
    } else {
        /* Memory-mapped I/O - use memory access */
        volatile uint32_t *mmio = (volatile uint32_t *)(virtio_blk_mmio_base + offset);
        *mmio = value;
    }
}

/* =========================================================================
 * VirtIO Device Initialization
 * ========================================================================= */

static int virtio_device_init(void)
{
    kprint("[VIRTIO] Starting device initialization...\n");
    
    /* Reset the device */
    kprint("[VIRTIO] Resetting device...\n");
    outb(virtio_blk_mmio_base + VIRTIO_PCI_STATUS, 0);
    
    /* Set ACKNOWLEDGE status bit */
    kprint("[VIRTIO] Setting ACKNOWLEDGE...\n");
    uint8_t status = inb(virtio_blk_mmio_base + VIRTIO_PCI_STATUS);
    outb(virtio_blk_mmio_base + VIRTIO_PCI_STATUS, status | VIRTIO_STATUS_ACKNOWLEDGE);
    
    /* Set DRIVER status bit */
    kprint("[VIRTIO] Setting DRIVER...\n");
    status = inb(virtio_blk_mmio_base + VIRTIO_PCI_STATUS);
    outb(virtio_blk_mmio_base + VIRTIO_PCI_STATUS, status | VIRTIO_STATUS_DRIVER);
    
    /* Read device features (we accept all for now) */
    kprint("[VIRTIO] Reading device features...\n");
    uint32_t features = inl(virtio_blk_mmio_base + VIRTIO_PCI_DEVICE_FEATURES);
    outl(virtio_blk_mmio_base + VIRTIO_PCI_DRIVER_FEATURES, features);
    
    /* Set FEATURES_OK status bit */
    kprint("[VIRTIO] Setting FEATURES_OK...\n");
    status = inb(virtio_blk_mmio_base + VIRTIO_PCI_STATUS);
    outb(virtio_blk_mmio_base + VIRTIO_PCI_STATUS, status | VIRTIO_STATUS_FEATURES_OK);
    
    /* Check if FEATURES_OK is still set (device accepted our features) */
    status = inb(virtio_blk_mmio_base + VIRTIO_PCI_STATUS);
    if (!(status & VIRTIO_STATUS_FEATURES_OK)) {
        kprint("[VIRTIO] Device rejected our features\n");
        return -1;
    }
    
    /* Set DRIVER_OK status bit */
    kprint("[VIRTIO] Setting DRIVER_OK...\n");
    status = inb(virtio_blk_mmio_base + VIRTIO_PCI_STATUS);
    outb(virtio_blk_mmio_base + VIRTIO_PCI_STATUS, status | VIRTIO_STATUS_DRIVER_OK);
    
    kprint("[VIRTIO] Device initialization complete\n");
    return 0;
}

/* =========================================================================
 * VirtQueue Setup
 * ========================================================================= */

static int virtio_setup_queue(void)
{
    kprint("[VIRTIO] === virtio_setup_queue START ===\n");
    
    /* Select queue 0 (the only queue for block device) */
    kprint("[VIRTIO] Selecting queue 0...\n");
    outw(virtio_blk_mmio_base + VIRTIO_PCI_QUEUE_SEL, 0);
    
    /* Read max queue size (Queue Num is read-only in legacy PCI) */
    uint16_t max_queue_size = inw(virtio_blk_mmio_base + VIRTIO_PCI_QUEUE_NUM);
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
    
    /* Get physical address of the entire virtqueue structure */
    uint64_t virtq_phys = DMA_VirtToPhys(&g_virtq);
    
    if (!virtq_phys) {
        kprint("[VIRTIO] Failed to get virtqueue physical address\n");
        return -1;
    }
    
    /* For legacy VirtIO, the Queue Address register expects a PFN
     * (page frame number = physical address >> 12), not a raw address */
    uint32_t virtq_pfn = (uint32_t)(virtq_phys >> 12);
    
    /* Set queue address (PFN of descriptor table) */
    kprint("[VIRTIO] Setting queue PFN...\n");
    outl(virtio_blk_mmio_base + VIRTIO_PCI_QUEUE_ADDR, virtq_pfn);
    
    kprint("[VIRTIO] Queue setup complete\n");
    return 0;
}

/* =========================================================================
 * Memory Barriers
 * ========================================================================= */

static inline void memory_barrier(void)
{
    /* x86 memory barrier - ensures all memory operations are complete */
    __asm__ volatile("" ::: "memory");
}

static inline void io_barrier(void)
{
    /* x86 I/O barrier - ensures all I/O operations are complete */
    __asm__ volatile("" ::: "memory");
}

/* =========================================================================
 * VirtQueue Request Submission
 * ========================================================================= */

static int virtio_submit_request(uint64_t req_phys, uint64_t data_phys, uint32_t data_len, uint64_t resp_phys, int is_write)
{
    /* Get free descriptor index */
    uint16_t desc_idx = g_virtq_free_idx;
    if (desc_idx >= VIRTIO_QUEUE_SIZE) {
        kprint("[VIRTIO] No free descriptors\n");
        return -1;
    }
    
    /* Setup descriptor 0: request header (device-readable) */
    g_virtq.desc[desc_idx].addr = req_phys;
    g_virtq.desc[desc_idx].len = sizeof(virtio_blk_req_t);
    g_virtq.desc[desc_idx].flags = VIRTQ_DESC_F_NEXT;
    g_virtq.desc[desc_idx].next = desc_idx + 1;
    
    /* Setup descriptor 1: data buffer */
    g_virtq.desc[desc_idx + 1].addr = data_phys;
    g_virtq.desc[desc_idx + 1].len = data_len;
    if (is_write) {
        /* Device reads from buffer */
        g_virtq.desc[desc_idx + 1].flags = VIRTQ_DESC_F_NEXT;
    } else {
        /* Device writes to buffer */
        g_virtq.desc[desc_idx + 1].flags = VIRTQ_DESC_F_NEXT | VIRTQ_DESC_F_WRITE;
    }
    g_virtq.desc[desc_idx + 1].next = desc_idx + 2;
    
    /* Setup descriptor 2: response (device-writable) */
    g_virtq.desc[desc_idx + 2].addr = resp_phys;
    g_virtq.desc[desc_idx + 2].len = sizeof(virtio_blk_resp_t);
    g_virtq.desc[desc_idx + 2].flags = VIRTQ_DESC_F_WRITE;  /* Device writes response */
    g_virtq.desc[desc_idx + 2].next = 0;
    
    /* Memory barrier to ensure descriptors are written before notifying device */
    memory_barrier();
    
    /* Update available ring */
    g_virtq.avail_ring[g_virtq.avail_idx % VIRTIO_QUEUE_SIZE] = desc_idx;
    g_virtq.avail_idx++;
    
    /* Memory barrier to ensure available ring is updated before notification */
    memory_barrier();
    
    /* Notify device */
    outw(virtio_blk_mmio_base + VIRTIO_PCI_QUEUE_NOTIFY, 0);
    
    /* Update free index (simple bump allocator) */
    g_virtq_free_idx += 3;
    
    return desc_idx;
}

/* =========================================================================
 * VirtIO Interrupt Handler
 * ========================================================================= */

static void virtio_irq_handler(uint64_t vector, uint64_t error_code)
{
    /* Memory barrier to ensure we see the device's writes */
    memory_barrier();
    
    /* Read ISR to clear interrupt (legacy VirtIO) */
    uint8_t isr = inb(virtio_blk_mmio_base + VIRTIO_PCI_ISR);
    (void)isr;
    
    /* Check if used ring has been updated */
    if (g_virtq.used_idx != g_virtq_used_idx) {
        /* Mark that an interrupt is pending */
        g_virtio_irq_pending = 1;
        g_virtq_used_idx = g_virtq.used_idx;
    }
    
    /* Note: EOI is sent by ISR_Dispatch in idt.c, don't send it here */
}

/* =========================================================================
 * VirtQueue Completion Polling
 * ========================================================================= */

static int virtio_wait_completion(uint16_t desc_idx, uint32_t timeout_ms)
{
    uint32_t start = 0;  /* TODO: Get actual timestamp */
    uint16_t initial_used_idx = g_virtq.used_idx;
    
    /* Wait for completion by polling the used ring index */
    uint32_t iterations = 0;
    while (1) {
        /* Memory barrier to ensure we see the device's writes */
        memory_barrier();
        
        /* Poll used_idx to detect completion */
        if (g_virtq.used_idx != initial_used_idx) {
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
        
        /* Simple timeout check (TODO: implement proper timer) */
        iterations++;
        if (iterations > 20000000) {
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
    if (!virtio_blk_mmio_base) {
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
    
    kprint("[VIRTIO] Read operation\n");
    
    /* Setup read request */
    g_blk_req.type = VIRTIO_BLK_T_IN;
    g_blk_req.ioprio = 0;
    g_blk_req.sector = sector;
    
    /* Get physical addresses for DMA */
    uint64_t req_phys = DMA_VirtToPhys(&g_blk_req);
    uint64_t resp_phys = DMA_VirtToPhys(&g_blk_resp);
    uint64_t data_phys = DMA_VirtToPhys(buffer);
    
    if (!req_phys || !resp_phys || !data_phys) {
        kprint("[VIRTIO] Failed to get physical addresses\n");
        return -1;
    }
    
    /* Submit request */
    int desc_idx = virtio_submit_request(req_phys, data_phys, num_sectors * 512, resp_phys, 0);
    if (desc_idx < 0) {
        kprint("[VIRTIO] Failed to submit request\n");
        return -1;
    }
    
    /* Wait for completion */
    if (virtio_wait_completion(desc_idx, 1000) != 0) {
        kprint("[VIRTIO] Request failed or timed out\n");
        return -1;
    }

    /* Reset descriptor index for reuse (synchronous driver) */
    g_virtq_free_idx = 0;

    return 0;
}

static int virtio_blk_write_op(BlockDev *dev, uint64_t sector, const void *buffer, uint32_t num_sectors)
{
    if (!virtio_blk_mmio_base) {
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
    
    kprint("[VIRTIO] Write operation\n");
    
    /* Setup write request */
    g_blk_req.type = VIRTIO_BLK_T_OUT;
    g_blk_req.ioprio = 0;
    g_blk_req.sector = sector;
    
    /* Get physical addresses for DMA */
    uint64_t req_phys = DMA_VirtToPhys(&g_blk_req);
    uint64_t resp_phys = DMA_VirtToPhys(&g_blk_resp);
    uint64_t data_phys = DMA_VirtToPhys((void *)buffer);
    
    if (!req_phys || !resp_phys || !data_phys) {
        kprint("[VIRTIO] Failed to get physical addresses\n");
        return -4;
    }
    
    /* Submit request */
    int desc_idx = virtio_submit_request(req_phys, data_phys, num_sectors * 512, resp_phys, 1);
    if (desc_idx < 0) {
        kprint("[VIRTIO] Failed to submit request\n");
        return -5;
    }
    
    /* Wait for completion */
    if (virtio_wait_completion(desc_idx, 1000) != 0) {
        kprint("[VIRTIO] Request failed or timed out\n");
        return -6;
    }

    /* Reset descriptor index for reuse (synchronous driver) */
    g_virtq_free_idx = 0;

    return 0;
}

static uint64_t virtio_blk_get_capacity_op(BlockDev *dev)
{
    (void)dev;
    return virtio_blk_capacity;
}

static const BlockDevOps virtio_blk_ops = {
    .read = virtio_blk_read_op,
    .write = virtio_blk_write_op,
    .get_capacity = virtio_blk_get_capacity_op,
};

/* =========================================================================
 * VirtIO Block Device Initialization
 * ========================================================================= */

int virtio_blk_init(void)
{
    /* Scan PCI bus for VirtIO block device */
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            for (int func = 0; func < 8; func++) {
                uint16_t vendor_id = pci_config_read_word(bus, dev, func, PCI_VENDOR_ID_OFFSET);
                uint16_t device_id = pci_config_read_word(bus, dev, func, PCI_DEVICE_ID_OFFSET);
                
                if (vendor_id == VIRTIO_PCI_VENDOR_ID && device_id == VIRTIO_PCI_DEVICE_ID_BLK) {
                    kprint("[VIRTIO] Found VirtIO block device\n");
                    
                    /* Get BAR0 (MMIO space for VirtIO device) */
                    uint32_t bar0 = pci_config_read_dword(bus, dev, func, PCI_BAR0_OFFSET);
                    
                    /* Check if BAR0 is memory-mapped (bit 0 = 0) or I/O (bit 0 = 1) */
                    if (bar0 & 0x01) {
                        /* Legacy I/O space - not ideal but try it */
                        virtio_blk_mmio_base = bar0 & ~0x03;
                    } else {
                        /* Memory-mapped I/O - preferred */
                        virtio_blk_mmio_base = bar0 & ~0x0F;
                    }
                    
                    /* Read device capacity using port I/O */
                    uint32_t capacity_low = inl(virtio_blk_mmio_base + VIRTIO_BLK_CAPACITY);
                    uint32_t capacity_high = inl(virtio_blk_mmio_base + VIRTIO_BLK_CAPACITY + 4);
                    virtio_blk_capacity = ((uint64_t)capacity_high << 32) | capacity_low;
                    
                    /* Get IRQ line from PCI configuration */
                    g_virtio_irq_line = pci_config_read_byte(bus, dev, func, PCI_INTERRUPT_LINE_OFFSET);
                    kprint("[VIRTIO] IRQ line: ");
                    kprinthex(g_virtio_irq_line);
                    kprint("\n");
                    
                    /* Initialize VirtIO device */
                    if (virtio_device_init() != 0) {
                        kprint("[VIRTIO] Failed to initialize device\n");
                        /* Continue anyway - device is registered */
                    }
                    
                    /* Setup virtqueue */
                    if (virtio_setup_queue() != 0) {
                        kprint("[VIRTIO] Failed to setup virtqueue\n");
                        return -1;
                    }
                    
                    /* Register with block device layer */
                    g_virtio_blk_dev.name = "virtio0";
                    g_virtio_blk_dev.sector_size = 512;
                    g_virtio_blk_dev.num_sectors = virtio_blk_capacity;
                    g_virtio_blk_dev.private_data = NULL;
                    g_virtio_blk_dev.ops = &virtio_blk_ops;
                    g_virtio_blk_dev.next = NULL;
                    
                    if (BlockDev_Register(&g_virtio_blk_dev) != 0) {
                        kprint("[VIRTIO] Failed to register with block device layer\n");
                        return -1;
                    }
                    
                    return 0;
                }
            }
        }
    }
    
    kprint("[VIRTIO] No VirtIO block device found\n");
    return -1;
}

/* =========================================================================
 * VirtIO IRQ Setup (must be called AFTER IDT_Init and PIC_Init)
 * ========================================================================= */

int virtio_blk_get_irq_line(void)
{
    return g_virtio_irq_line;
}

void virtio_blk_setup_irq(void)
{
    if (!virtio_blk_mmio_base) {
        kprint("[VIRTIO] No device to setup IRQ for\n");
        return;
    }

    if (g_virtio_irq_line >= 0 && g_virtio_irq_line < 16) {
        uint8_t vector = 32 + g_virtio_irq_line;
        IDT_SetHandler(vector, virtio_irq_handler);
        PIC_UnmaskIRQ(g_virtio_irq_line);
        kprint("[VIRTIO] Interrupt handler registered for IRQ ");
        kprinthex(g_virtio_irq_line);
        kprint("\n");
    } else {
        kprint("[VIRTIO] Invalid IRQ line, interrupt not registered\n");
    }
}
