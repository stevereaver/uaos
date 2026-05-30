/*
 * virtio_blk.c — UAOS VirtIO Block Device Driver
 *
 * Implements VirtIO block device driver for QEMU external disk support.
 * VirtIO is a paravirtualized I/O framework used by QEMU/KVM.
 */

#include "virtio_blk.h"
#include "../dos/blockdev.h"
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

#define VIRTIO_BLK_CAPACITY      0x08  /* Number of 512-byte sectors */
#define VIRTIO_BLK_SIZE_MAX      0x0C  /* Maximum segment size */
#define VIRTIO_BLK_SEG_MAX       0x10  /* Maximum number of segments */
#define VIRTIO_BLK_BLK_SIZE      0x14  /* Block size in bytes */

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

/* =========================================================================
 * VirtIO Queue (Virtqueue) Structure
 * ========================================================================= */

#define VIRTIO_QUEUE_SIZE        256

typedef struct {
    uint64_t addr;      /* Physical address */
    uint32_t len;       /* Length */
    uint16_t flags;     /* Flags */
    uint16_t next;      /* Next descriptor */
} virtq_desc_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
} virtq_avail_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VIRTIO_QUEUE_SIZE];
} virtq_used_t;

/* Virtqueue will be allocated when needed */
/* static virtq_desc_t *virtq_desc = NULL; */
/* static virtq_avail_t *virtq_avail = NULL; */
/* static virtq_used_t *virtq_used = NULL; */

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

/* =========================================================================
 * VirtIO Block Device Operations (BlockDevOps interface)
 * ========================================================================= */

static int virtio_blk_read_op(uint64_t sector, void *buffer, uint32_t num_sectors)
{
    (void)buffer; /* Suppress unused warning */
    if (!virtio_blk_mmio_base) {
        printf("[VIRTIO] Device not initialized\n");
        return -1;
    }
    
    printf("[VIRTIO] Read: sector=%llu, count=%u\n", sector, num_sectors);
    /* TODO: Implement actual virtqueue-based I/O */
    return -1;
}

static int virtio_blk_write_op(uint64_t sector, const void *buffer, uint32_t num_sectors)
{
    (void)buffer; /* Suppress unused warning */
    if (!virtio_blk_mmio_base) {
        printf("[VIRTIO] Device not initialized\n");
        return -1;
    }
    
    printf("[VIRTIO] Write: sector=%llu, count=%u\n", sector, num_sectors);
    /* TODO: Implement actual virtqueue-based I/O */
    return -1;
}

static uint64_t virtio_blk_get_capacity_op(void)
{
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
                    printf("[VIRTIO] Found VirtIO block device at %02x:%02x.%x\n", bus, dev, func);
                    
                    /* Get BAR0 (I/O space for VirtIO device) */
                    uint32_t bar0 = pci_config_read_dword(bus, dev, func, PCI_BAR0_OFFSET);
                    if (bar0 & 0x01) {
                        /* I/O space */
                        virtio_blk_mmio_base = bar0 & ~0x03;
                        printf("[VIRTIO] BAR0 I/O base: 0x%04x\n", virtio_blk_mmio_base);
                    } else {
                        printf("[VIRTIO] BAR0 is not I/O space, not supported\n");
                        return -1;
                    }
                    
                    /* Read device capacity */
                    uint32_t capacity_low = inl(virtio_blk_mmio_base + VIRTIO_BLK_CAPACITY);
                    uint32_t capacity_high = inl(virtio_blk_mmio_base + VIRTIO_BLK_CAPACITY + 4);
                    virtio_blk_capacity = capacity_high;
                    printf("[VIRTIO] Capacity: %llu sectors (%llu MB)\n", 
                           ((uint64_t)capacity_high << 32) | capacity_low,
                           (((uint64_t)capacity_high << 32) | capacity_low) * 512 / (1024 * 1024));
                    
                    /* Register with block device layer */
                    g_virtio_blk_dev.name = "virtio0";
                    g_virtio_blk_dev.sector_size = 512;
                    g_virtio_blk_dev.num_sectors = virtio_blk_capacity;
                    g_virtio_blk_dev.private_data = NULL;
                    g_virtio_blk_dev.ops = &virtio_blk_ops;
                    g_virtio_blk_dev.next = NULL;
                    
                    if (BlockDev_Register(&g_virtio_blk_dev) != 0) {
                        printf("[VIRTIO] Failed to register with block device layer\n");
                        return -1;
                    }
                    
                    return 0;
                }
            }
        }
    }
    
    printf("[VIRTIO] No VirtIO block device found\n");
    return -1;
}
