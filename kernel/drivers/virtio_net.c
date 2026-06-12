/*
 * virtio_net.c — UAOS VirtIO Network Device Driver
 *
 * Implements:
 *   - PCI scan for VirtIO-Net (vendor 0x1AF4, device 0x1000 legacy)
 *   - Virtqueue split-ring setup for RX (queue 0) and TX (queue 1)
 *   - Packet transmission via the TX virtqueue
 *   - Packet reception via polling / IRQ
 *   - IRQ handler registration
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
 */

#include "virtio_net.h"
#include "../irq/idt.h"
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
    uint16_t ring[VIRTQ_SIZE];
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
    VirtqUsedElem ring[VIRTQ_SIZE];
    uint16_t avail_event;
} VirtqUsed;

/* One complete virtqueue (descriptor table + avail + used, page-aligned) */
#define VIRTQ_ALIGN     4096
/* Each virtqueue needs enough space for all three parts, aligned to 4K */
/* Size = desc_table(16*N) + avail(6+2*N) padded to 4K + used(6+8*N) padded */
#define VIRTQ_BYTES     8192   /* 2 pages is always enough for N=64 */

/* VirtIO net header (legacy, 10 bytes) */
typedef struct __attribute__((packed)) {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
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

static VirtqDesc  *g_txq_desc;
static VirtqAvail *g_txq_avail;
static VirtqUsed  *g_txq_used;
static uint16_t    g_txq_last_used;
static uint16_t    g_txq_free_head;

/* RX packet buffers (one per descriptor slot) */
static uint8_t g_rx_bufs[VIRTQ_SIZE][VIRTIO_NET_RX_BUFSZ] __attribute__((aligned(16)));

/* TX bounce buffer (one at a time) */
static uint8_t g_tx_hdr_buf[VIRTIO_NET_HDR_SIZE + VIRTIO_NET_MTU + 2] __attribute__((aligned(16)));

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

/* -------------------------------------------------------------------------
 * Virtqueue helpers
 * ------------------------------------------------------------------------- */

static void vq_init_ptrs(int qidx, VirtqDesc **desc, VirtqAvail **avail, VirtqUsed **used)
{
    uint8_t *base = g_vq_mem[qidx];
    /* Descriptor table starts at offset 0 */
    *desc  = (VirtqDesc *)base;
    /* Available ring immediately after descriptor table, no alignment needed beyond 2 */
    uint32_t avail_off = VIRTQ_SIZE * sizeof(VirtqDesc);
    *avail = (VirtqAvail *)(base + avail_off);
    /* Used ring at next 4K boundary after avail */
    uint32_t used_off = (avail_off + sizeof(VirtqAvail) + 4095) & ~4095U;
    if (used_off + sizeof(VirtqUsed) > VIRTQ_BYTES)
        used_off = VIRTQ_BYTES - sizeof(VirtqUsed);
    *used  = (VirtqUsed *)(base + used_off);
}

/* Clear the virtqueue memory and reset indices */
static void vq_reset(VirtqDesc *desc, VirtqAvail *avail, VirtqUsed *used)
{
    for (int i = 0; i < VIRTQ_SIZE; i++) {
        desc[i].addr  = 0;
        desc[i].len   = 0;
        desc[i].flags = 0;
        desc[i].next  = (uint16_t)(i + 1);
    }
    avail->flags = 0;
    avail->idx   = 0;
    for (int i = 0; i < VIRTQ_SIZE; i++) avail->ring[i] = 0;
    used->flags  = 0;
    used->idx    = 0;
}

/* Push all RX descriptors into the available ring so the device can fill them */
static void rxq_refill_all(void)
{
    for (int i = 0; i < VIRTQ_SIZE; i++) {
        g_rxq_desc[i].addr  = (uint64_t)(uintptr_t)g_rx_bufs[i];
        g_rxq_desc[i].len   = VIRTIO_NET_RX_BUFSZ;
        g_rxq_desc[i].flags = VIRTQ_DESC_F_WRITE;
        g_rxq_desc[i].next  = 0;
        g_rxq_avail->ring[i] = (uint16_t)i;
    }
    /* Memory barrier before updating idx */
    __asm__ volatile("mfence" ::: "memory");
    g_rxq_avail->idx = VIRTQ_SIZE;
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
                /* VirtIO legacy net: vendor=0x1AF4, device=0x1000, subsystem=1 */
                if (vendor == 0x1AF4 && (device == 0x1000 || device == 0x1041)) {
                    /* Check subsystem device ID == 1 (net) for 0x1000 */
                    if (device == 0x1000) {
                        uint32_t sub = pci_read32((uint8_t)bus, dev, fn, 0x2C);
                        uint16_t subsys_dev = (uint16_t)(sub >> 16);
                        if (subsys_dev != 1) continue;
                    }
                    /* Dump key PCI config regs for debug */
                    _vn_ps("[VNET] found dev="); _vn_ph(device);
                    _vn_ps(" bus="); _vn_ph(bus);
                    _vn_ps(" slot="); _vn_ph(dev); _vn_ps("\n");
                    _vn_ps("[VNET] status="); _vn_ph(pci_read16((uint8_t)bus,dev,fn,0x06));
                    _vn_ps(" cmd="); _vn_ph(pci_read16((uint8_t)bus,dev,fn,0x04));
                    _vn_ps(" cap_ptr="); _vn_ph(pci_read8((uint8_t)bus,dev,fn,0x34)); _vn_ps("\n");
                    /* Read BAR0 (I/O) */
                    uint32_t bar0 = pci_read32((uint8_t)bus, dev, fn, 0x10);
                    _vn_ps("[VNET] bar0="); _vn_ph(bar0); _vn_ps("\n");
                    if (!(bar0 & 1)) continue;   /* must be I/O space */
                    *iobase_out = (uint16_t)(bar0 & ~3U);
                    /* Read interrupt line */
                    uint32_t irq_reg = pci_read32((uint8_t)bus, dev, fn, 0x3C);
                    *irq_out = (uint8_t)(irq_reg & 0xFF);
                    /* Disable MSI/MSI-X so legacy INTx (8259 PIC) is used */
                    pci_disable_msi((uint8_t)bus, dev, fn);
                    /* Enable bus-master + I/O space */
                    uint16_t cmd = pci_read16((uint8_t)bus, dev, fn, 0x04);
                    pci_write16((uint8_t)bus, dev, fn, 0x04, cmd | 0x05);
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
    /* Write guest physical address of queue (page number, 4K pages) */
    uint32_t pfn = (uint32_t)((uintptr_t)mem >> 12);
    outl(iobase + VIRTIO_PCI_QUEUE_PFN, pfn);
}

/* -------------------------------------------------------------------------
 * IRQ handler
 * ------------------------------------------------------------------------- */

static void virtio_net_irq_handler(uint64_t vector, uint64_t error_code)
{
    (void)vector; (void)error_code;
    if (!g_io_base) return;
    uint8_t isr = inb(g_io_base + VIRTIO_PCI_ISR);
    _vn_ps("[IRQ] isr="); _vn_ph(isr); _vn_ps("\n");
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

    /* 1. Reset device */
    outb(iobase + VIRTIO_PCI_STATUS, VIRTIO_STATUS_RESET);
    io_delay(); io_delay();

    /* 2. Acknowledge + driver */
    outb(iobase + VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

    /* 3. Read and accept features (request MAC support) */
    uint32_t host_feat = inl(iobase + VIRTIO_PCI_HOST_FEATURES);
    uint32_t guest_feat = host_feat & (VIRTIO_NET_F_MAC);
    outl(iobase + VIRTIO_PCI_GUEST_FEATURES, guest_feat);

    /* 4. Read MAC from device config */
    if (guest_feat & VIRTIO_NET_F_MAC) {
        for (int i = 0; i < ETH_ALEN; i++)
            g_mac[i] = inb(iobase + VIRTIO_PCI_CONFIG + i);
    } else {
        /* Fallback: hardcode a locally-administered MAC */
        g_mac[0] = 0x52; g_mac[1] = 0x54; g_mac[2] = 0x00;
        g_mac[3] = 0x12; g_mac[4] = 0x34; g_mac[5] = 0x56;
    }

    /* 5. Setup RX virtqueue (queue 0) */
    vq_init_ptrs(0, &g_rxq_desc, &g_rxq_avail, &g_rxq_used);
    vq_reset(g_rxq_desc, g_rxq_avail, g_rxq_used);
    vq_register(iobase, 0, g_vq_mem[0]);
    rxq_refill_all();

    /* 6. Setup TX virtqueue (queue 1) */
    vq_init_ptrs(1, &g_txq_desc, &g_txq_avail, &g_txq_used);
    vq_reset(g_txq_desc, g_txq_avail, g_txq_used);
    vq_register(iobase, 1, g_vq_mem[1]);
    g_txq_free_head = 0;
    g_txq_last_used = 0;

    /* 7. Driver OK */
    outb(iobase + VIRTIO_PCI_STATUS,
         VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

    /* Re-disable MSI-X after DRIVER_OK: QEMU re-enables it during
     * feature negotiation even if we cleared it earlier. */
    pci_disable_msi(bus, dev, fn);

    /* Kick RX queue so device knows buffers are available immediately */
    outw(iobase + VIRTIO_PCI_QUEUE_NOTIFY, 0);

    g_up = 1;
    return 1;
}

void virtio_net_set_rx_callback(virtio_net_rx_cb cb)
{
    g_rx_cb = cb;
}

int virtio_net_send(const uint8_t *data, uint16_t len)
{
    if (!g_up || !g_io_base) return 0;
    if (len > VIRTIO_NET_MTU) return 0;

    /* Wait for the previous TX descriptor to be returned by the device before
     * overwriting g_tx_hdr_buf.  The used->idx is updated by the device when
     * it finishes with the descriptor.  Spin briefly; QEMU is fast. */
    {
        uint32_t spin = 0;
        while (g_txq_last_used != g_txq_used->idx) {
            __asm__ volatile("pause" ::: "memory");
            if (++spin > 500000) break;   /* ~5 ms safety exit */
        }
        /* Drain whatever came back so last_used tracks used->idx */
        g_txq_last_used = g_txq_used->idx;
    }
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
    uint8_t *payload = g_tx_hdr_buf + VIRTIO_NET_HDR_SIZE;
    for (uint16_t i = 0; i < len; i++) payload[i] = data[i];
    uint16_t total = (uint16_t)(VIRTIO_NET_HDR_SIZE + len);

    /* Place into descriptor 0 */
    g_txq_desc[0].addr  = (uint64_t)(uintptr_t)g_tx_hdr_buf;
    g_txq_desc[0].len   = total;
    g_txq_desc[0].flags = 0;
    g_txq_desc[0].next  = 0;

    g_txq_avail->ring[g_txq_avail->idx % VIRTQ_SIZE] = 0;
    __asm__ volatile("mfence" ::: "memory");
    g_txq_avail->idx++;
    __asm__ volatile("mfence" ::: "memory");

    /* Notify device: queue 1 = TX */
    _vn_ps("[TX] sending len="); _vn_ph(total); _vn_ps(" avail_idx="); _vn_ph(g_txq_avail->idx); _vn_ps("\n");
    outw(g_io_base + VIRTIO_PCI_QUEUE_NOTIFY, 1);

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
    if (g_rxq_last_used != g_rxq_used->idx) {
        _vn_ps("[RX] used->idx="); _vn_ph(g_rxq_used->idx);
        _vn_ps(" last="); _vn_ph(g_rxq_last_used); _vn_ps("\n");
    }
    while (g_rxq_last_used != g_rxq_used->idx) {
        uint16_t ui = g_rxq_last_used % VIRTQ_SIZE;
        uint32_t received_len = g_rxq_used->ring[ui].len;
        uint16_t desc_id      = (uint16_t)(g_rxq_used->ring[ui].id % VIRTQ_SIZE);
        _vn_ps("[RX] pkt len="); _vn_ph(received_len); _vn_ps(" desc="); _vn_ph(desc_id); _vn_ps("\n");

        if (received_len > VIRTIO_NET_HDR_SIZE) {
            uint16_t frame_len = (uint16_t)(received_len - VIRTIO_NET_HDR_SIZE);
            const uint8_t *frame = g_rx_bufs[desc_id] + VIRTIO_NET_HDR_SIZE;
            /* Log first 4 bytes of Ethernet dst MAC */
            _vn_ps("[RX] dst="); _vn_ph(frame[0]); _vn_pc(':'); _vn_ph(frame[1]);
            _vn_ps(" ethertype="); _vn_ph((uint32_t)((frame[12]<<8)|frame[13])); _vn_ps("\n");
            if (g_rx_cb)
                g_rx_cb(frame, frame_len);
        }

        /* Re-add descriptor to available ring */
        g_rxq_desc[desc_id].addr  = (uint64_t)(uintptr_t)g_rx_bufs[desc_id];
        g_rxq_desc[desc_id].len   = VIRTIO_NET_RX_BUFSZ;
        g_rxq_desc[desc_id].flags = VIRTQ_DESC_F_WRITE;
        g_rxq_avail->ring[g_rxq_avail->idx % VIRTQ_SIZE] = desc_id;
        __asm__ volatile("mfence" ::: "memory");
        g_rxq_avail->idx++;

        g_rxq_last_used++;
    }
    /* Notify device we've refilled RX queue */
    if (g_up)
        outw(g_io_base + VIRTIO_PCI_QUEUE_NOTIFY, 0);

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
