/*
 * virtio_net.c — UAOS VirtIO Network Device Driver
 *
 * Implements:
 *   - PCI scan for VirtIO-Net (vendor 0x1AF4, device 0x1000 legacy
 *     and 0x1041 modern non-transitional)
 *   - Virtqueue split-ring setup for RX (queue 0) and TX (queue 1)
 *   - Packet transmission via the TX virtqueue
 *   - Packet reception via polling / IRQ
 *   - IRQ handler registration
 *
 * Two transports are supported:
 *   - Legacy (pre-1.0): device 0x1000, BAR0 I/O-port register block.
 *   - Modern (virtio 1.0+): device 0x1041, capability-based MMIO
 *     transport (virtio_pci_common_cfg + notify + ISR + device-config
 *     vendor capabilities), same model as kernel/irq/virtio_scsi.c.
 *     MMIO regions are reached by dereferencing the physical BAR
 *     address — valid because UAOS identity-maps the low 4GB.
 *
 * VirtIO legacy (pre-1.0) register layout (I/O port based):
 *   BAR0 (I/O):
 *     +0x00  DEVICE_FEATURES   (R)
 *     +0x04  GUEST_FEATURES    (W)
 *     +0x08  QUEUE_PFN         (R/W)
 *     +0x0C  QUEUE_SIZE        (R)
 *     +0x0E  QUEUE_SELECT      (W)
 *     +0x10  QUEUE_NOTIFY      (W)
 *     +0x12  DEVICE_STATUS     (R/W)
 *     +0x13  ISR_STATUS        (R, clears on read)
 *     +0x14  device-specific config (MAC[0..5], status)
 *
 * NOTE: the legacy QUEUE_SIZE register is read-only — the device dictates
 * the virtqueue size and the guest must lay out desc/avail/used rings at
 * exactly that size.  QEMU reports 256, VirtualBox reports 1024.  This
 * driver sizes the ring memory for VIRTQ_MAX_SIZE and computes all ring
 * offsets from the device-reported size at init time.  On the modern
 * transport QUEUE_SIZE is writable, so the driver clamps to
 * VIRTQ_MAX_SIZE and publishes the 64-bit ring addresses itself.
 */

#include "virtio_net.h"
#include "../irq/idt.h"
#include "../exec/task.h"
#include <stdint.h>
#include <stddef.h>

/* Serial debug (COM1 = 0x3F8) */
static inline void _vn_ob(uint16_t p,uint8_t v){__asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));}
static inline uint8_t _vn_ib(uint16_t p){uint8_t v;__asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p));return v;}
static void _vn_pc(char c){while((_vn_ib(0x3FD)&0x20)==0){}_vn_ob(0x3F8,(uint8_t)c);if(c=='\n'){while((_vn_ib(0x3FD)&0x20)==0){}_vn_ob(0x3F8,'\r');}}
static void _vn_ps(const char *s){while(*s)_vn_pc(*s++);}
static void _vn_ph(uint32_t v){static const char h[]="0123456789ABCDEF";_vn_ps("0x");for(int i=28;i>=0;i-=4)_vn_pc(h[(v>>i)&0xF]);}

/* forward declared in idt.h as void (*ISRHandler)(uint64_t vector, uint64_t error_code) */

/* -------------------------------------------------------------------------
 * PCI access helpers (config space via CF8/CFC)
 * ------------------------------------------------------------------------- */

static inline void outb(uint16_t port, uint8_t  v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(port)); }
static inline void outw(uint16_t port, uint16_t v){ __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(port)); }
static inline void outl(uint16_t port, uint32_t v){ __asm__ volatile("outl %0,%1"::"a"(v),"Nd"(port)); }
static inline uint8_t  inb(uint16_t port){ uint8_t  v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(port)); return v; }
static inline uint16_t inw(uint16_t port){ uint16_t v; __asm__ volatile("inw %1,%0":"=a"(v):"Nd"(port)); return v; }
static inline uint32_t inl(uint16_t port){ uint32_t v; __asm__ volatile("inl %1,%0":"=a"(v):"Nd"(port)); return v; }

static inline void io_delay(void){ __asm__ volatile("outb %%al,$0x80"::"a"(0)); }

static uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg)
{
    uint32_t addr = 0x80000000U | ((uint32_t)bus<<16) | ((uint32_t)dev<<11)
                  | ((uint32_t)fn<<8) | (reg & 0xFC);
    outl(0xCF8, addr);
    return inl(0xCFC);
}
static uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg)
{
    return (uint16_t)(pci_read32(bus,dev,fn,reg) >> ((reg&2)*8));
}
static void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint32_t val)
{
    uint32_t addr = 0x80000000U | ((uint32_t)bus<<16) | ((uint32_t)dev<<11)
                  | ((uint32_t)fn<<8) | (reg & 0xFC);
    outl(0xCF8, addr);
    outl(0xCFC, val);
}
static void pci_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint16_t val)
{
    uint32_t addr = 0x80000000U | ((uint32_t)bus<<16) | ((uint32_t)dev<<11)
                  | ((uint32_t)fn<<8) | (reg & 0xFC);
    outl(0xCF8, addr);
    /* read-modify-write the 16-bit half */
    uint32_t cur = inl(0xCFC);
    int shift = (reg & 2) * 8;
    cur = (cur & ~(0xFFFFU << shift)) | ((uint32_t)val << shift);
    outl(0xCFC, cur);
}
static uint8_t pci_read8(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg)
{
    return (uint8_t)(pci_read32(bus,dev,fn,reg) >> ((reg&3)*8));
}
static void pci_write8(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint8_t val)
{
    uint32_t addr = 0x80000000U | ((uint32_t)bus<<16) | ((uint32_t)dev<<11)
                  | ((uint32_t)fn<<8) | (reg & 0xFC);
    outl(0xCF8, addr);
    uint32_t cur = inl(0xCFC);
    int shift = (reg & 3) * 8;
    cur = (cur & ~(0xFFU << shift)) | ((uint32_t)val << shift);
    outl(0xCFC, cur);
}

/* Walk the PCI capabilities list and disable MSI (cap ID 0x05) so the
 * device uses legacy INTx instead.  On Q35 QEMU defaults to MSI for
 * virtio-net-pci, which bypasses the 8259 PIC entirely. */
static void pci_disable_msi(uint8_t bus, uint8_t dev, uint8_t fn)
{
    /* Capabilities present only if bit 4 of Status register is set */
    uint16_t status = pci_read16(bus, dev, fn, 0x06);
    if (!(status & 0x10)) return;

    uint8_t cap_ptr = pci_read8(bus, dev, fn, 0x34) & 0xFC;
    _vn_ps("[VNET] cap_list start="); _vn_ph(cap_ptr); _vn_ps("\n");
    int limit = 48;   /* guard against loops */
    while (cap_ptr && limit--) {
        uint8_t cap_id   = pci_read8(bus, dev, fn, cap_ptr);
        uint8_t cap_next = pci_read8(bus, dev, fn, (uint8_t)(cap_ptr + 1));
        _vn_ps("[VNET] cap @"); _vn_ph(cap_ptr);
        _vn_ps(" id="); _vn_ph(cap_id);
        _vn_ps(" next="); _vn_ph(cap_next);
        _vn_ps(" mc="); _vn_ph(pci_read16(bus,dev,fn,(uint8_t)(cap_ptr+2))); _vn_ps("\n");
        if (cap_id == 0x05) {
            /* MSI: Message Control is at cap_ptr+2, bit 0 = MSI Enable */
            uint16_t mc = pci_read16(bus, dev, fn, (uint8_t)(cap_ptr + 2));
            if (mc & 1) {
                _vn_ps("[VNET] disabling MSI at cap="); _vn_ph(cap_ptr); _vn_ps("\n");
                pci_write16(bus, dev, fn, (uint8_t)(cap_ptr + 2), (uint16_t)(mc & ~1));
            }
        }
        if (cap_id == 0x11) {
            /* MSI-X: Message Control is at cap_ptr+2, bit 15 = MSI-X Enable
             * Disable unconditionally — QEMU enables it during DRIVER_OK
             * negotiation even if the bit was clear beforehand. */
            uint16_t mc = pci_read16(bus, dev, fn, (uint8_t)(cap_ptr + 2));
            _vn_ps("[VNET] disabling MSI-X at cap="); _vn_ph(cap_ptr);
            _vn_ps(" mc="); _vn_ph(mc); _vn_ps("\n");
            pci_write16(bus, dev, fn, (uint8_t)(cap_ptr + 2), (uint16_t)(mc & ~0x8000));
        }
        cap_ptr = cap_next & 0xFC;
    }
}

/* Read a full 64-bit BAR address (handles 32-bit and 64-bit BARs).
 * Returns 0 if the BAR is I/O type or unmapped. */
static uint64_t pci_read_bar(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t bar_idx)
{
    uint8_t bar_off = (uint8_t)(0x10 + bar_idx * 4);
    uint32_t bar_lo = pci_read32(bus, dev, fn, bar_off);
    if (bar_lo & 1) return 0;           /* I/O BAR — not usable for MMIO */
    uint64_t addr = bar_lo & 0xFFFFFFF0U;
    if ((bar_lo & 0x6) == 0x4) {        /* 64-bit BAR */
        uint32_t bar_hi = pci_read32(bus, dev, fn, (uint8_t)(bar_off + 4));
        addr |= (uint64_t)bar_hi << 32;
    }
    return addr;
}

/* -------------------------------------------------------------------------
 * MMIO helpers (volatile access to memory-mapped register space)
 * ------------------------------------------------------------------------- */

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

/* -------------------------------------------------------------------------
 * Transport abstraction (legacy I/O port vs modern MMIO capabilities)
 * ------------------------------------------------------------------------- */

typedef enum { VNET_LEGACY, VNET_MODERN } vnet_transport_t;

/* Modern transport: vendor capability location */
typedef struct {
    uint8_t  bar;       /* BAR index (0-5) */
    uint32_t offset;    /* offset within the BAR */
    uint32_t length;    /* length of the capability region */
    uint64_t bar_addr;  /* resolved physical BAR address */
} vnet_cap_t;

static vnet_transport_t g_transport = VNET_LEGACY;
static vnet_cap_t g_common_cap;
static vnet_cap_t g_notify_cap;
static vnet_cap_t g_isr_cap;
static vnet_cap_t g_devcfg_cap;    /* device-specific config (MAC) */
static int        g_devcfg_found = 0;
static uint32_t   g_notify_off_mult = 0;

/* When a capability's BAR cannot be reached by MMIO dereference (e.g.
 * firmware placed a 64-bit BAR above the 4GB identity map), all config
 * regions are accessed through the VIRTIO_PCI_CAP_PCI_CFG window in PCI
 * config space instead. */
static int        g_use_pcicfg = 0;
static uint8_t    g_pcicfg_pos = 0;   /* cfg-space offset of the PCI_CFG cap */

/* Per-queue notify offsets (modern transport), cached at queue setup */
static uint16_t g_q_notify_off[2];

/* PCI capability IDs */
#define PCI_CAP_ID_VENDOR           0x09

/* VirtIO vendor capability types */
#define VIRTIO_PCI_CAP_COMMON_CFG   1
#define VIRTIO_PCI_CAP_NOTIFY_CFG   2
#define VIRTIO_PCI_CAP_ISR_CFG      3
#define VIRTIO_PCI_CAP_DEVICE_CFG   4
#define VIRTIO_PCI_CAP_PCI_CFG      5

/* PCI command register bits */
#define PCI_CMD_IO_SPACE            0x01
#define PCI_CMD_MEMORY_SPACE        0x02
#define PCI_CMD_BUS_MASTER          0x04

/* Walk the PCI vendor capabilities and locate the virtio-1.0 config
 * regions: common config, notify, ISR, (optionally) device-specific
 * config, and the PCI_CFG config-space access window.  Regions are
 * recorded even when their BAR cannot be mapped — in that case access
 * falls back to the PCI_CFG window. */
static int vnet_parse_capabilities(uint8_t bus, uint8_t dev, uint8_t fn)
{
    uint16_t status = pci_read16(bus, dev, fn, 0x06);
    if (!(status & 0x10)) {
        _vn_ps("[VNET] no PCI capabilities list\n");
        return 0;
    }

    uint8_t cap_ptr = pci_read8(bus, dev, fn, 0x34) & 0xFC;
    int found_common = 0, found_notify = 0, found_isr = 0;
    int usable_common = 0, usable_notify = 0, usable_isr = 0;
    int limit = 48;

    while (cap_ptr && limit--) {
        uint8_t cap_id   = pci_read8(bus, dev, fn, cap_ptr);
        uint8_t cap_next = pci_read8(bus, dev, fn, (uint8_t)(cap_ptr + 1));
        uint8_t cap_len  = pci_read8(bus, dev, fn, (uint8_t)(cap_ptr + 2));

        if (cap_id != PCI_CAP_ID_VENDOR) {
            cap_ptr = cap_next & 0xFC;
            continue;
        }

        uint8_t cfg_type = pci_read8(bus, dev, fn, (uint8_t)(cap_ptr + 3));

        if (cfg_type == VIRTIO_PCI_CAP_PCI_CFG && cap_len >= 20) {
            /* PCI config-space access window — bar/offset/length fields
             * are driver-writable for this cap type. */
            g_pcicfg_pos = cap_ptr;
            _vn_ps("[VNET] pcicfg window @"); _vn_ph(cap_ptr); _vn_ps("\n");
            cap_ptr = cap_next & 0xFC;
            continue;
        }

        if (cap_len < 16) {
            cap_ptr = cap_next & 0xFC;
            continue;
        }

        uint8_t bar_idx  = pci_read8(bus, dev, fn, (uint8_t)(cap_ptr + 4));
        uint32_t offset  = pci_read32(bus, dev, fn, (uint8_t)(cap_ptr + 8));
        uint32_t length  = pci_read32(bus, dev, fn, (uint8_t)(cap_ptr + 12));

        uint64_t bar_addr = pci_read_bar(bus, dev, fn, bar_idx);
        int usable = bar_addr && bar_addr <= 0xFFFFFFFFULL;

        vnet_cap_t *cap = NULL;
        if (cfg_type == VIRTIO_PCI_CAP_COMMON_CFG)      { cap = &g_common_cap; found_common = 1;
            if (usable) usable_common = 1; }
        else if (cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG) { cap = &g_notify_cap; found_notify = 1;
            /* notify_off_multiplier is at cap_ptr + 16 */
            g_notify_off_mult = pci_read32(bus, dev, fn, (uint8_t)(cap_ptr + 16));
            if (usable) usable_notify = 1; }
        else if (cfg_type == VIRTIO_PCI_CAP_ISR_CFG)    { cap = &g_isr_cap; found_isr = 1;
            if (usable) usable_isr = 1; }
        else if (cfg_type == VIRTIO_PCI_CAP_DEVICE_CFG) { cap = &g_devcfg_cap; g_devcfg_found = 1; }

        if (cap) {
            cap->bar      = bar_idx;
            cap->offset   = offset;
            cap->length   = length;
            cap->bar_addr = bar_addr;
            _vn_ps("[VNET] cap type="); _vn_ph(cfg_type);
            _vn_ps(" bar="); _vn_ph(bar_idx);
            _vn_ps(" off="); _vn_ph(offset);
            _vn_ps(" len="); _vn_ph(length);
            _vn_ps(" base="); _vn_ph((uint32_t)(bar_addr >> 32));
            _vn_ph((uint32_t)bar_addr);
            _vn_ps(usable ? "\n" : " (not mappable)\n");
        }
        cap_ptr = cap_next & 0xFC;
    }

    if (!found_common || !found_notify || !found_isr) {
        _vn_ps("[VNET] missing required caps common="); _vn_ph(found_common);
        _vn_ps(" notify="); _vn_ph(found_notify);
        _vn_ps(" isr="); _vn_ph(found_isr); _vn_ps("\n");
        return 0;
    }
    if (g_notify_off_mult == 0) g_notify_off_mult = 1;

    /* All three required regions must share one access mode. */
    if (usable_common && usable_notify && usable_isr) {
        g_use_pcicfg = 0;
        _vn_ps("[VNET] using MMIO register access\n");
    } else if (g_pcicfg_pos) {
        g_use_pcicfg = 1;
        _vn_ps("[VNET] BAR not mappable, using PCI_CFG window access\n");
    } else {
        _vn_ps("[VNET] BAR not mappable and no PCI_CFG window\n");
        return 0;
    }
    return 1;
}

/* -------------------------------------------------------------------------
 * VirtIO legacy register offsets (BAR0 I/O base)
 * ------------------------------------------------------------------------- */
#define VIRTIO_PCI_HOST_FEATURES    0x00
#define VIRTIO_PCI_GUEST_FEATURES   0x04
#define VIRTIO_PCI_QUEUE_PFN        0x08
#define VIRTIO_PCI_QUEUE_SIZE       0x0C
#define VIRTIO_PCI_QUEUE_SEL        0x0E
#define VIRTIO_PCI_QUEUE_NOTIFY     0x10
#define VIRTIO_PCI_STATUS           0x12
#define VIRTIO_PCI_ISR              0x13
#define VIRTIO_PCI_CONFIG           0x14   /* device-specific: MAC + link status */

/* VirtIO device status bits */
#define VIRTIO_STATUS_RESET         0x00
#define VIRTIO_STATUS_ACK           0x01
#define VIRTIO_STATUS_DRIVER        0x02
#define VIRTIO_STATUS_DRIVER_OK     0x04
#define VIRTIO_STATUS_FEATURES_OK   0x08
#define VIRTIO_STATUS_FAILED        0x80

/* VirtIO-Net feature bits */
#define VIRTIO_NET_F_MAC            (1 << 5)
#define VIRTIO_NET_F_STATUS         (1 << 16)

/* VIRTIO_F_VERSION_1 = feature bit 32 (index 0 of the select=1 half) */
#define VIRTIO_F_VERSION_1_BIT      (1u << 0)

/* -------------------------------------------------------------------------
 * VirtIO modern common config register offsets
 * (virtio_pci_common_cfg, virtio 1.0+ spec)
 * ------------------------------------------------------------------------- */

#define VNET_COMMON_DEVICE_FEATURE_SELECT   0x00
#define VNET_COMMON_DEVICE_FEATURE          0x04
#define VNET_COMMON_DRIVER_FEATURE_SELECT   0x08
#define VNET_COMMON_DRIVER_FEATURE          0x0C
#define VNET_COMMON_MSIX_CONFIG             0x10
#define VNET_COMMON_NUM_QUEUES              0x12
#define VNET_COMMON_DEVICE_STATUS           0x14
#define VNET_COMMON_CONFIG_GENERATION       0x15
#define VNET_COMMON_QUEUE_SELECT            0x16
#define VNET_COMMON_QUEUE_SIZE              0x18
#define VNET_COMMON_QUEUE_MSIX_VECTOR       0x1A
#define VNET_COMMON_QUEUE_ENABLE            0x1C
#define VNET_COMMON_QUEUE_NOTIFY_OFF        0x1E
#define VNET_COMMON_QUEUE_DESC_LO           0x20
#define VNET_COMMON_QUEUE_DESC_HI           0x24
#define VNET_COMMON_QUEUE_AVAIL_LO          0x28
#define VNET_COMMON_QUEUE_AVAIL_HI          0x2C
#define VNET_COMMON_QUEUE_USED_LO           0x30
#define VNET_COMMON_QUEUE_USED_HI           0x34

#define VNET_MSIX_NO_VECTOR                 0xFFFF

/* -------------------------------------------------------------------------
 * Split virtqueue structures (4KB page-aligned, legacy ring layout)
 * ------------------------------------------------------------------------- */

/* Virtqueue descriptor */
typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} VirtqDesc;

#define VIRTQ_DESC_F_NEXT       1
#define VIRTQ_DESC_F_WRITE      2   /* device writes (RX) */

/* Available ring */
typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VIRTQ_MAX_SIZE];
    uint16_t used_event;
} VirtqAvail;

/* Used ring element */
typedef struct __attribute__((packed)) {
    uint32_t id;
    uint32_t len;
} VirtqUsedElem;

/* Used ring */
typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    VirtqUsedElem ring[VIRTQ_MAX_SIZE];
    uint16_t avail_event;
} VirtqUsed;

/* One complete virtqueue (descriptor table + avail + used, page-aligned) */
#define VIRTQ_ALIGN     4096
/* Each virtqueue needs enough space for all three parts, aligned to 4K */
/* Size = desc_table(16*N) + avail(6+2*N) padded to 4K + used(6+8*N) padded */
#define VIRTQ_BYTES     32768  /* 8 pages — enough for N=1024 (VirtualBox) */

/* VirtIO net header.  Legacy (pre-1.0) uses only the first 10 bytes;
 * when VIRTIO_F_VERSION_1 is negotiated the header is 12 bytes and
 * includes num_buffers (device-written on RX, zero on TX). */
typedef struct __attribute__((packed)) {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers;
} VirtioNetHdr;

/* -------------------------------------------------------------------------
 * Static storage (BSS — no dynamic allocation)
 * ------------------------------------------------------------------------- */

/* 2 virtqueues: 0=RX, 1=TX */
static uint8_t g_vq_mem[2][VIRTQ_BYTES] __attribute__((aligned(4096)));

/* Pointers into the virtqueue memory regions */
static VirtqDesc  *g_rxq_desc;
static VirtqAvail *g_rxq_avail;
static VirtqUsed  *g_rxq_used;
static uint16_t    g_rxq_last_used;
static uint16_t    g_rxq_free_head;
static uint16_t    g_rx_qsize;      /* device-reported RX queue size */
static uint16_t    g_rx_nbufs;      /* RX descriptors posted (<= VNET_RX_BUFS) */

static VirtqDesc  *g_txq_desc;
static VirtqAvail *g_txq_avail;
static VirtqUsed  *g_txq_used;
static uint16_t    g_txq_last_used;
static uint16_t    g_txq_free_head;
static uint16_t    g_tx_qsize;      /* device-reported TX queue size */
static int         g_tx_no_used;    /* device never posts TX used entries (QEMU) */

/* RX packet buffers (one per posted descriptor) */
static uint8_t g_rx_bufs[VNET_RX_BUFS][VIRTIO_NET_RX_BUFSZ] __attribute__((aligned(16)));

/* TX bounce buffer (one at a time) */
static uint8_t g_tx_hdr_buf[VIRTIO_NET_HDR_V1_SIZE + VIRTIO_NET_MTU + 2] __attribute__((aligned(16)));

/* Driver state */
static uint16_t  g_io_base = 0;
static uint8_t   g_mac[ETH_ALEN];
static int       g_up = 0;
static uint8_t   g_irq_line = 0;
/* PCI coordinates — stored so we can re-disable MSI-X after DRIVER_OK */
static uint8_t   g_pci_bus = 0, g_pci_dev = 0, g_pci_fn = 0;

static virtio_net_rx_cb g_rx_cb = 0;

/* Reentrancy guard: virtio_net_poll() is called from both main loop and IRQ */
static volatile uint8_t g_poll_lock = 0;

/* VirtIO net header length actually in use: 10 bytes legacy, 12 bytes when
 * VIRTIO_F_VERSION_1 is negotiated (adds the num_buffers field). */
static uint8_t g_hdr_len = VIRTIO_NET_HDR_SIZE;

/* -------------------------------------------------------------------------
 * Modern-region access wrappers
 *
 * Each modern config region lives at (cap->bar_addr + cap->offset) when
 * the BAR is directly mappable (g_use_pcicfg == 0).  When the BAR is not
 * mappable, accesses go through the VIRTIO_PCI_CAP_PCI_CFG window:
 * the driver writes the target bar/offset/length into the capability's
 * fields and the device proxies the transfer through pci_cfg_data
 * (cap_ptr + 16).
 * ------------------------------------------------------------------------- */

static uint32_t vn_pcicfg_read(vnet_cap_t *cap, uint32_t reg, uint32_t len)
{
    pci_write8 (g_pci_bus, g_pci_dev, g_pci_fn, (uint8_t)(g_pcicfg_pos + 4),  cap->bar);
    pci_write32(g_pci_bus, g_pci_dev, g_pci_fn, (uint8_t)(g_pcicfg_pos + 8),  cap->offset + reg);
    pci_write32(g_pci_bus, g_pci_dev, g_pci_fn, (uint8_t)(g_pcicfg_pos + 12), len);
    return pci_read32(g_pci_bus, g_pci_dev, g_pci_fn, (uint8_t)(g_pcicfg_pos + 16));
}
static void vn_pcicfg_write(vnet_cap_t *cap, uint32_t reg, uint32_t len, uint32_t val)
{
    pci_write8 (g_pci_bus, g_pci_dev, g_pci_fn, (uint8_t)(g_pcicfg_pos + 4),  cap->bar);
    pci_write32(g_pci_bus, g_pci_dev, g_pci_fn, (uint8_t)(g_pcicfg_pos + 8),  cap->offset + reg);
    pci_write32(g_pci_bus, g_pci_dev, g_pci_fn, (uint8_t)(g_pcicfg_pos + 12), len);
    pci_write32(g_pci_bus, g_pci_dev, g_pci_fn, (uint8_t)(g_pcicfg_pos + 16), val);
}

static uint8_t vn_r8(vnet_cap_t *cap, uint32_t reg) {
    if (g_use_pcicfg) return (uint8_t)vn_pcicfg_read(cap, reg, 1);
    return mmio_r8(cap->bar_addr + cap->offset + reg);
}
static uint16_t vn_r16(vnet_cap_t *cap, uint32_t reg) {
    if (g_use_pcicfg) return (uint16_t)vn_pcicfg_read(cap, reg, 2);
    return mmio_r16(cap->bar_addr + cap->offset + reg);
}
static uint32_t vn_r32(vnet_cap_t *cap, uint32_t reg) {
    if (g_use_pcicfg) return vn_pcicfg_read(cap, reg, 4);
    return mmio_r32(cap->bar_addr + cap->offset + reg);
}
static void vn_w8(vnet_cap_t *cap, uint32_t reg, uint8_t val) {
    if (g_use_pcicfg) { vn_pcicfg_write(cap, reg, 1, val); return; }
    mmio_w8(cap->bar_addr + cap->offset + reg, val);
}
static void vn_w16(vnet_cap_t *cap, uint32_t reg, uint16_t val) {
    if (g_use_pcicfg) { vn_pcicfg_write(cap, reg, 2, val); return; }
    mmio_w16(cap->bar_addr + cap->offset + reg, val);
}
static void vn_w32(vnet_cap_t *cap, uint32_t reg, uint32_t val) {
    if (g_use_pcicfg) { vn_pcicfg_write(cap, reg, 4, val); return; }
    mmio_w32(cap->bar_addr + cap->offset + reg, val);
}

/* -------------------------------------------------------------------------
 * Transport-aware register accessors
 *
 * Legacy: all registers at BAR0 I/O base + offset (port I/O).
 * Modern: common config at common_cap.bar_addr + common_cap.offset + reg;
 *         notify and ISR live in their own capability regions.
 * ------------------------------------------------------------------------- */

static uint8_t vn_status_read(void) {
    if (g_transport == VNET_LEGACY)
        return inb(g_io_base + VIRTIO_PCI_STATUS);
    return vn_r8(&g_common_cap, VNET_COMMON_DEVICE_STATUS);
}
static void vn_status_write(uint8_t val) {
    if (g_transport == VNET_LEGACY)
        outb(g_io_base + VIRTIO_PCI_STATUS, val);
    else
        vn_w8(&g_common_cap, VNET_COMMON_DEVICE_STATUS, val);
}

static uint32_t vn_host_features_read(uint32_t select) {
    if (g_transport == VNET_LEGACY)
        return inl(g_io_base + VIRTIO_PCI_HOST_FEATURES);
    vn_w32(&g_common_cap, VNET_COMMON_DEVICE_FEATURE_SELECT, select);
    return vn_r32(&g_common_cap, VNET_COMMON_DEVICE_FEATURE);
}
static void vn_guest_features_write(uint32_t select, uint32_t val) {
    if (g_transport == VNET_LEGACY) {
        outl(g_io_base + VIRTIO_PCI_GUEST_FEATURES, val);
        return;
    }
    vn_w32(&g_common_cap, VNET_COMMON_DRIVER_FEATURE_SELECT, select);
    vn_w32(&g_common_cap, VNET_COMMON_DRIVER_FEATURE, val);
}

static void vn_queue_select(uint16_t idx) {
    if (g_transport == VNET_LEGACY)
        outw(g_io_base + VIRTIO_PCI_QUEUE_SEL, idx);
    else
        vn_w16(&g_common_cap, VNET_COMMON_QUEUE_SELECT, idx);
}

static uint16_t vn_queue_size_read(void) {
    if (g_transport == VNET_LEGACY)
        return inw(g_io_base + VIRTIO_PCI_QUEUE_SIZE);
    return vn_r16(&g_common_cap, VNET_COMMON_QUEUE_SIZE);
}

static void vn_queue_notify(uint16_t idx) {
    if (g_transport == VNET_LEGACY) {
        outw(g_io_base + VIRTIO_PCI_QUEUE_NOTIFY, idx);
        return;
    }
    /* Modern: doorbell is in the notify capability region at
     * queue_notify_off * notify_off_multiplier (offsets cached at setup) */
    vn_w16(&g_notify_cap, (uint32_t)g_q_notify_off[idx] * g_notify_off_mult, idx);
}

/* ISR read (clears the interrupt) */
static uint8_t vn_isr_read(void) {
    if (g_transport == VNET_LEGACY)
        return inb(g_io_base + VIRTIO_PCI_ISR);
    return vn_r8(&g_isr_cap, 0);
}

/* -------------------------------------------------------------------------
 * Virtqueue helpers
 * ------------------------------------------------------------------------- */

static int vq_init_ptrs(int qidx, uint16_t qsize,
                        VirtqDesc **desc, VirtqAvail **avail, VirtqUsed **used)
{
    uint8_t *base = g_vq_mem[qidx];
    /* Descriptor table starts at offset 0 */
    *desc  = (VirtqDesc *)base;
    /* Available ring immediately after descriptor table, no alignment needed beyond 2 */
    uint32_t avail_off = (uint32_t)qsize * sizeof(VirtqDesc);
    *avail = (VirtqAvail *)(base + avail_off);
    /* Used ring at next 4K boundary after avail (flags+idx+ring[qsize]) */
    uint32_t avail_bytes = 6 + (uint32_t)qsize * 2;
    uint32_t used_off = (avail_off + avail_bytes + 4095) & ~4095U;
    uint32_t used_bytes = 6 + (uint32_t)qsize * 8;
    if (used_off + used_bytes > VIRTQ_BYTES)
        return 0;
    *used  = (VirtqUsed *)(base + used_off);
    return 1;
}

/* Clear the virtqueue memory and reset indices */
static void vq_reset(VirtqDesc *desc, VirtqAvail *avail, VirtqUsed *used,
                     uint16_t qsize)
{
    for (int i = 0; i < qsize; i++) {
        desc[i].addr  = 0;
        desc[i].len   = 0;
        desc[i].flags = 0;
        desc[i].next  = (uint16_t)(i + 1);
    }
    avail->flags = 0;
    avail->idx   = 0;
    for (int i = 0; i < qsize; i++) avail->ring[i] = 0;
    used->flags  = 0;
    used->idx    = 0;
}

/* Push RX descriptors into the available ring so the device can fill them.
 * We post VNET_RX_BUFS buffers at most, even if the device queue is bigger —
 * the avail ring simply holds fewer entries than its capacity. */
static void rxq_refill_all(void)
{
    g_rx_nbufs = g_rx_qsize < VNET_RX_BUFS ? g_rx_qsize : VNET_RX_BUFS;
    for (int i = 0; i < g_rx_nbufs; i++) {
        g_rxq_desc[i].addr  = (uint64_t)(uintptr_t)g_rx_bufs[i];
        g_rxq_desc[i].len   = VIRTIO_NET_RX_BUFSZ;
        g_rxq_desc[i].flags = VIRTQ_DESC_F_WRITE;
        g_rxq_desc[i].next  = 0;
        g_rxq_avail->ring[i] = (uint16_t)i;
    }
    /* Memory barrier before updating idx */
    __asm__ volatile("mfence" ::: "memory");
    g_rxq_avail->idx = g_rx_nbufs;
    g_rxq_free_head  = 0;
    g_rxq_last_used  = 0;
}

/* -------------------------------------------------------------------------
 * PCI scan for VirtIO-Net
 * ------------------------------------------------------------------------- */

static int pci_find_virtio_net(uint8_t *bus_out, uint8_t *dev_out,
                                uint8_t *fn_out,  uint16_t *iobase_out,
                                uint8_t *irq_out)
{
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t fn = 0; fn < 8; fn++) {
                uint32_t id = pci_read32((uint8_t)bus, dev, fn, 0x00);
                if (id == 0xFFFFFFFF) continue;
                uint16_t vendor = (uint16_t)(id & 0xFFFF);
                uint16_t device = (uint16_t)(id >> 16);
                /* VirtIO net: vendor=0x1AF4, device 0x1000 (legacy,
                 * subsystem=1) or 0x1041 (modern non-transitional) */
                if (vendor == 0x1AF4 && (device == 0x1000 || device == 0x1041)) {
                    /* Check subsystem device ID == 1 (net) for 0x1000 */
                    if (device == 0x1000) {
                        uint32_t sub = pci_read32((uint8_t)bus, dev, fn, 0x2C);
                        uint16_t subsys_dev = (uint16_t)(sub >> 16);
                        if (subsys_dev != 1) continue;
                    }
                    g_transport = (device == 0x1041) ? VNET_MODERN : VNET_LEGACY;
                    /* Dump key PCI config regs for debug */
                    _vn_ps("[VNET] found dev="); _vn_ph(device);
                    _vn_ps(g_transport == VNET_MODERN ? " (modern)" : " (legacy)");
                    _vn_ps(" bus="); _vn_ph(bus);
                    _vn_ps(" slot="); _vn_ph(dev); _vn_ps("\n");
                    _vn_ps("[VNET] status="); _vn_ph(pci_read16((uint8_t)bus,dev,fn,0x06));
                    _vn_ps(" cmd="); _vn_ph(pci_read16((uint8_t)bus,dev,fn,0x04));
                    _vn_ps(" cap_ptr="); _vn_ph(pci_read8((uint8_t)bus,dev,fn,0x34)); _vn_ps("\n");
                    /* Read interrupt line */
                    uint32_t irq_reg = pci_read32((uint8_t)bus, dev, fn, 0x3C);
                    *irq_out = (uint8_t)(irq_reg & 0xFF);
                    /* Disable MSI/MSI-X so legacy INTx (8259 PIC) is used */
                    pci_disable_msi((uint8_t)bus, dev, fn);
                    uint16_t cmd = pci_read16((uint8_t)bus, dev, fn, 0x04);
                    if (g_transport == VNET_LEGACY) {
                        /* Read BAR0 (I/O) */
                        uint32_t bar0 = pci_read32((uint8_t)bus, dev, fn, 0x10);
                        _vn_ps("[VNET] bar0="); _vn_ph(bar0); _vn_ps("\n");
                        if (!(bar0 & 1)) continue;   /* must be I/O space */
                        *iobase_out = (uint16_t)(bar0 & ~3U);
                        /* Enable bus-master + I/O space */
                        pci_write16((uint8_t)bus, dev, fn, 0x04,
                                    (uint16_t)(cmd | PCI_CMD_IO_SPACE | PCI_CMD_BUS_MASTER));
                    } else {
                        /* Modern: locate the MMIO capability regions */
                        for (int b = 0; b < 6; b++) {
                            _vn_ps("[VNET] bar"); _vn_ph(b); _vn_ps("=");
                            _vn_ph(pci_read32((uint8_t)bus, dev, fn,
                                              (uint8_t)(0x10 + b * 4)));
                            _vn_ps("\n");
                        }
                        if (!vnet_parse_capabilities((uint8_t)bus, dev, fn))
                            continue;
                        *iobase_out = 0;
                        /* Enable bus-master + memory space */
                        pci_write16((uint8_t)bus, dev, fn, 0x04,
                                    (uint16_t)(cmd | PCI_CMD_MEMORY_SPACE | PCI_CMD_BUS_MASTER));
                    }
                    *bus_out = (uint8_t)bus;
                    *dev_out = dev;
                    *fn_out  = fn;
                    return 1;
                }
            }
        }
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Virtqueue registration with device
 * ------------------------------------------------------------------------- */

static void vq_register(uint16_t iobase, uint16_t qidx, void *mem)
{
    /* Select queue */
    outw(iobase + VIRTIO_PCI_QUEUE_SEL, qidx);
    /* Read back queue size to confirm device accepted the selection */
    uint16_t qsize = inw(iobase + VIRTIO_PCI_QUEUE_SIZE);
    /* Write guest physical address of queue (page number, 4K pages) */
    uint32_t pfn = (uint32_t)((uintptr_t)mem >> 12);
    _vn_ps("[VNET] vq"); _vn_ph(qidx);
    _vn_ps(" qsize="); _vn_ph(qsize);
    _vn_ps(" mem="); _vn_ph((uint32_t)(uintptr_t)mem);
    _vn_ps(" pfn="); _vn_ph(pfn); _vn_ps("\n");
    outl(iobase + VIRTIO_PCI_QUEUE_PFN, pfn);
    /* Read back PFN to confirm write was accepted */
    uint32_t pfn_rb = inl(iobase + VIRTIO_PCI_QUEUE_PFN);
    _vn_ps("[VNET] vq"); _vn_ph(qidx); _vn_ps(" pfn_readback="); _vn_ph(pfn_rb); _vn_ps("\n");
}

/* Modern transport: publish the queue through the common config.
 * The caller has already selected the queue and written QUEUE_SIZE.
 * Writes the 64-bit desc/avail/used addresses, disables the MSI-X
 * vector (legacy INTx is used), caches the notify offset, and
 * enables the queue.  Addresses are virtual==physical under UAOS's
 * identity-mapped low 4GB. */
static void vq_register_modern(uint16_t qidx,
                               VirtqDesc *desc, VirtqAvail *avail, VirtqUsed *used)
{
    uint64_t desc_addr  = (uint64_t)(uintptr_t)desc;
    uint64_t avail_addr = (uint64_t)(uintptr_t)avail;
    uint64_t used_addr  = (uint64_t)(uintptr_t)used;

    vn_w32(&g_common_cap, VNET_COMMON_QUEUE_DESC_LO,  (uint32_t)(desc_addr & 0xFFFFFFFF));
    vn_w32(&g_common_cap, VNET_COMMON_QUEUE_DESC_HI,  (uint32_t)(desc_addr >> 32));
    vn_w32(&g_common_cap, VNET_COMMON_QUEUE_AVAIL_LO, (uint32_t)(avail_addr & 0xFFFFFFFF));
    vn_w32(&g_common_cap, VNET_COMMON_QUEUE_AVAIL_HI, (uint32_t)(avail_addr >> 32));
    vn_w32(&g_common_cap, VNET_COMMON_QUEUE_USED_LO,  (uint32_t)(used_addr & 0xFFFFFFFF));
    vn_w32(&g_common_cap, VNET_COMMON_QUEUE_USED_HI,  (uint32_t)(used_addr >> 32));

    vn_w16(&g_common_cap, VNET_COMMON_QUEUE_MSIX_VECTOR, VNET_MSIX_NO_VECTOR);

    /* Cache the per-queue notify offset for the doorbell */
    g_q_notify_off[qidx] = vn_r16(&g_common_cap, VNET_COMMON_QUEUE_NOTIFY_OFF);

    vn_w16(&g_common_cap, VNET_COMMON_QUEUE_ENABLE, 1);

    _vn_ps("[VNET] vq"); _vn_ph(qidx);
    _vn_ps(" desc="); _vn_ph((uint32_t)desc_addr);
    _vn_ps(" avail="); _vn_ph((uint32_t)avail_addr);
    _vn_ps(" used="); _vn_ph((uint32_t)used_addr);
    _vn_ps(" notify_off="); _vn_ph(g_q_notify_off[qidx]); _vn_ps("\n");
}

/* Select, size, lay out and register one virtqueue with the device.
 * Returns 1 on success.  For the modern transport QUEUE_SIZE is
 * writable and the 64-bit ring addresses are published via common
 * config; for legacy the device-reported size is used as-is and the
 * queue is registered by PFN. */
static int vnet_setup_queue(uint16_t qidx,
                            VirtqDesc **desc, VirtqAvail **avail, VirtqUsed **used,
                            uint16_t *qsize_out)
{
    vn_queue_select(qidx);
    uint16_t qsize = vn_queue_size_read();
    _vn_ps("[VNET] queue "); _vn_ph(qidx);
    _vn_ps(" size="); _vn_ph(qsize); _vn_ps("\n");
    if (qsize == 0 || qsize > VIRTQ_MAX_SIZE) {
        _vn_ps("[VNET] unsupported queue size\n");
        return 0;
    }
    if (g_transport == VNET_MODERN) {
        vn_w16(&g_common_cap, VNET_COMMON_QUEUE_SIZE, qsize);
    }
    if (!vq_init_ptrs(qidx, qsize, desc, avail, used))
        return 0;
    vq_reset(*desc, *avail, *used, qsize);
    if (g_transport == VNET_LEGACY)
        vq_register(g_io_base, qidx, g_vq_mem[qidx]);
    else
        vq_register_modern(qidx, *desc, *avail, *used);
    *qsize_out = qsize;
    return 1;
}

/* -------------------------------------------------------------------------
 * IRQ handler
 * ------------------------------------------------------------------------- */

static void virtio_net_irq_handler(uint64_t vector, uint64_t error_code)
{
    (void)vector; (void)error_code;
    if (!g_up) return;
    uint8_t isr = vn_isr_read();
    if (isr & 1)
        virtio_net_poll();
    PIC_SendEOI((int)(g_irq_line));
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

int virtio_net_init(void)
{
    uint8_t  bus, dev, fn;
    uint16_t iobase;
    uint8_t  irq;

    if (!pci_find_virtio_net(&bus, &dev, &fn, &iobase, &irq)) {
        return 0;
    }

    g_io_base   = iobase;
    g_irq_line  = irq;
    g_pci_bus   = bus;
    g_pci_dev   = dev;
    g_pci_fn    = fn;

    /* 1. Reset device — the virtio spec requires the driver to wait until
     * device_status reads back 0 before reinitialising.  A fixed delay can
     * race the device model's reset processing and leave the status/queue
     * state inconsistent, so poll with a bounded spin. */
    vn_status_write(VIRTIO_STATUS_RESET);
    {
        int reset_ok = 0;
        for (volatile int i = 0; i < 1000000; i++) {
            if (vn_status_read() == VIRTIO_STATUS_RESET) { reset_ok = 1; break; }
        }
        if (!reset_ok) {
            _vn_ps("[VNET] device did not complete reset\n");
            return 0;
        }
    }

    /* 2. Acknowledge + driver */
    vn_status_write(VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

    /* 3. Feature negotiation (request MAC support) */
    uint32_t guest_feat_lo;
    if (g_transport == VNET_LEGACY) {
        uint32_t host_feat = vn_host_features_read(0);
        guest_feat_lo = host_feat & VIRTIO_NET_F_MAC;
        vn_guest_features_write(0, guest_feat_lo);
        g_hdr_len = VIRTIO_NET_HDR_SIZE;
    } else {
        /* Modern: VIRTIO_F_VERSION_1 (bit 32) is mandatory for
         * non-transitional devices; also request VIRTIO_NET_F_MAC. */
        uint32_t host_hi = vn_host_features_read(1);
        uint32_t host_lo = vn_host_features_read(0);
        uint32_t guest_hi = host_hi & VIRTIO_F_VERSION_1_BIT;
        guest_feat_lo = host_lo & VIRTIO_NET_F_MAC;
        _vn_ps("[VNET] host_feat lo="); _vn_ph(host_lo);
        _vn_ps(" hi="); _vn_ph(host_hi); _vn_ps("\n");
        if (!guest_hi) {
            _vn_ps("[VNET] device did not offer VERSION_1\n");
            vn_status_write(VIRTIO_STATUS_RESET);
            return 0;
        }
        vn_guest_features_write(1, guest_hi);
        vn_guest_features_write(0, guest_feat_lo);
        /* FEATURES_OK handshake — re-read status to confirm the device
         * accepted the negotiated feature set. */
        vn_status_write(VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER |
                        VIRTIO_STATUS_FEATURES_OK);
        if (!(vn_status_read() & VIRTIO_STATUS_FEATURES_OK)) {
            _vn_ps("[VNET] device rejected features\n");
            return 0;
        }
        g_hdr_len = VIRTIO_NET_HDR_V1_SIZE;
    }

    /* 4. Read MAC from device config (modern config must be read after
     * FEATURES_OK) */
    if ((guest_feat_lo & VIRTIO_NET_F_MAC)
        && (g_transport == VNET_LEGACY || g_devcfg_found)) {
        if (g_transport == VNET_LEGACY) {
            for (int i = 0; i < ETH_ALEN; i++)
                g_mac[i] = inb(g_io_base + VIRTIO_PCI_CONFIG + i);
        } else {
            for (int i = 0; i < ETH_ALEN; i++)
                g_mac[i] = vn_r8(&g_devcfg_cap, (uint32_t)i);
        }
    } else {
        /* Fallback: hardcode a locally-administered MAC */
        g_mac[0] = 0x52; g_mac[1] = 0x54; g_mac[2] = 0x00;
        g_mac[3] = 0x12; g_mac[4] = 0x34; g_mac[5] = 0x56;
    }
    _vn_ps("[VNET] mac="); for (int i = 0; i < ETH_ALEN; i++) _vn_ph(g_mac[i]);
    _vn_ps(" hdr_len="); _vn_ph(g_hdr_len); _vn_ps("\n");

    /* 5. Setup RX virtqueue (queue 0) — honour the device-reported queue
     * size: VirtualBox reports 1024, QEMU 256.  The ring layout must match
     * the reported size exactly or the device reads/writes the wrong pages. */
    if (!vnet_setup_queue(0, &g_rxq_desc, &g_rxq_avail, &g_rxq_used, &g_rx_qsize)) {
        vn_status_write(VIRTIO_STATUS_RESET);
        return 0;
    }
    rxq_refill_all();

    /* 6. Setup TX virtqueue (queue 1) */
    if (!vnet_setup_queue(1, &g_txq_desc, &g_txq_avail, &g_txq_used, &g_tx_qsize)) {
        vn_status_write(VIRTIO_STATUS_RESET);
        return 0;
    }
    g_txq_free_head = 0;
    g_txq_last_used = 0;

    /* 7. Driver OK */
    vn_status_write(VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER |
                    VIRTIO_STATUS_DRIVER_OK);

    /* Re-disable MSI-X after DRIVER_OK: QEMU re-enables it during
     * feature negotiation even if we cleared it earlier. */
    pci_disable_msi(bus, dev, fn);

    /* Kick RX queue so device knows buffers are available immediately */
    vn_queue_notify(0);

    g_up = 1;
    return 1;
}

void virtio_net_set_rx_callback(virtio_net_rx_cb cb)
{
    g_rx_cb = cb;
}

int virtio_net_send(const uint8_t *data, uint16_t len)
{
    if (!g_up) return 0;
    if (len > VIRTIO_NET_MTU) return 0;

    /* Serialize the whole single-descriptor TX sequence: senders arrive
     * from task context AND from the NIC IRQ handler (e.g. TCP ACKs sent
     * by tcp_rx inside virtio_net_poll), so g_tx_hdr_buf and descriptor 0
     * must be claimed, filled and kicked atomically or concurrent frames
     * overwrite each other and leave the wire with corrupt packets. */
    Disable();

    /* Wait for the previous TX submission to be consumed before
     * overwriting g_tx_hdr_buf/desc0.  "Outstanding" is avail->idx -
     * used->idx.  VirtualBox posts TX used entries and drains this
     * quickly; QEMU consumes avail entries without posting used ones,
     * so after a bounded spin with no used-ring progress we stop
     * waiting entirely (QEMU forwards the frame regardless).
     * The old check (last_used != used->idx) was inverted: on hosts
     * that do post used entries it was true after the first send and
     * could never clear, so every send spun the full bound with IRQs
     * disabled — freezing the machine under VirtualBox. */
    if (!g_tx_no_used) {
        uint16_t used_before = g_txq_used->idx;
        uint32_t spin = 0;
        while ((uint16_t)(g_txq_avail->idx - g_txq_used->idx) != 0) {
            __asm__ volatile("pause" ::: "memory");
            if (++spin > 200000) {
                if (g_txq_used->idx == used_before)
                    g_tx_no_used = 1;   /* never posts TX used entries */
                break;
            }
        }
    }
    /* Drain whatever came back so last_used tracks used->idx */
    g_txq_last_used = g_txq_used->idx;
    /* Always use descriptor slot 0 (single-packet TX model) */
    g_txq_free_head = 0;

    /* Build: [VirtioNetHdr][Ethernet frame] in one contiguous buffer */
    VirtioNetHdr *hdr = (VirtioNetHdr *)g_tx_hdr_buf;
    hdr->flags      = 0;
    hdr->gso_type   = 0;
    hdr->hdr_len    = 0;
    hdr->gso_size   = 0;
    hdr->csum_start = 0;
    hdr->csum_offset= 0;
    hdr->num_buffers= 0;
    uint8_t *payload = g_tx_hdr_buf + g_hdr_len;
    for (uint16_t i = 0; i < len; i++) payload[i] = data[i];
    uint16_t total = (uint16_t)(g_hdr_len + len);

    /* Place into descriptor 0 */
    g_txq_desc[0].addr  = (uint64_t)(uintptr_t)g_tx_hdr_buf;
    g_txq_desc[0].len   = total;
    g_txq_desc[0].flags = 0;
    g_txq_desc[0].next  = 0;

    g_txq_avail->ring[g_txq_avail->idx % g_tx_qsize] = 0;
    __asm__ volatile("mfence" ::: "memory");
    g_txq_avail->idx++;
    __asm__ volatile("mfence" ::: "memory");

    /* Notify device: queue 1 = TX */
    vn_queue_notify(1);

    Enable();
    return 1;
}

void virtio_net_get_mac(uint8_t *buf)
{
    for (int i = 0; i < ETH_ALEN; i++) buf[i] = g_mac[i];
}

void virtio_net_poll(void)
{
    if (!g_up) return;

    /* Reentrancy guard — single-core, so an atomic xchg is sufficient */
    uint8_t already_locked = __sync_lock_test_and_set(&g_poll_lock, 1);
    if (already_locked) return;

    /* Drain used RX ring */
    int refilled = 0;
    while (g_rxq_last_used != g_rxq_used->idx) {
        uint16_t ui = g_rxq_last_used % g_rx_qsize;
        uint32_t received_len = g_rxq_used->ring[ui].len;
        uint16_t desc_id      = (uint16_t)g_rxq_used->ring[ui].id;

        if (desc_id < g_rx_nbufs) {
            if (received_len > g_hdr_len) {
                uint16_t frame_len = (uint16_t)(received_len - g_hdr_len);
                const uint8_t *frame = g_rx_bufs[desc_id] + g_hdr_len;
                if (g_rx_cb)
                    g_rx_cb(frame, frame_len);
            }

            /* Re-add descriptor to available ring */
            g_rxq_desc[desc_id].addr  = (uint64_t)(uintptr_t)g_rx_bufs[desc_id];
            g_rxq_desc[desc_id].len   = VIRTIO_NET_RX_BUFSZ;
            g_rxq_desc[desc_id].flags = VIRTQ_DESC_F_WRITE;
            g_rxq_avail->ring[g_rxq_avail->idx % g_rx_qsize] = desc_id;
            __asm__ volatile("mfence" ::: "memory");
            g_rxq_avail->idx++;
        }

        refilled = 1;
        g_rxq_last_used++;
    }
    /* Notify device only when we actually re-added descriptors — the
     * doorbell write is a VM exit on VirtualBox, and callers poll this
     * in tight loops. */
    if (refilled && g_up)
        vn_queue_notify(0);

    __sync_lock_release(&g_poll_lock);
}

int virtio_net_is_up(void) { return g_up; }

void virtio_net_setup_irq(void)
{
    if (!g_up) return;
    _vn_ps("[VNET] setup_irq line="); _vn_ph(g_irq_line);
    _vn_ps(" iobase="); _vn_ph(g_io_base); _vn_ps("\n");
    IDT_SetHandler((uint8_t)(32 + g_irq_line), virtio_net_irq_handler);
    PIC_UnmaskIRQ((int)g_irq_line);
}
