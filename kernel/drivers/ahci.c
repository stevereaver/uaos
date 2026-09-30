/* ahci.c — UAOS AHCI (SATA) host controller driver
 *
 * Minimal synchronous AHCI 1.x driver:
 *   - finds the HBA on PCI (Intel ICH8M 8086:2829, or any class 01/06/01)
 *   - resets the controller, takes BIOS ownership if needed
 *   - per port: command list (1 KiB), received FIS (256 B), one command
 *     table with PRDTs — all allocated from the DMA pool
 *   - IDENTIFY to get capacity, then 48-bit READ/WRITE DMA EXT
 *   - registers each disk with the BlockDev layer
 *   - MSI interrupt attach when the routing layer can provide it;
 *     completion is still confirmed by polling CI so polling works
 *     regardless.
 */

#include "ahci.h"
#include "../dos/blockdev.h"
#include "../dos/dma.h"
#include "../boot/kprint.h"
#include "../irq/irq.h"
#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* PCI                                                                 */
/* ------------------------------------------------------------------ */

#define PCI_ADDR 0xCF8
#define PCI_DATA 0xCFC

static inline void outl(uint16_t port, uint32_t val)
{
    __asm__ volatile ("outl %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint32_t inl(uint16_t port)
{
    uint32_t v;
    __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static uint32_t pci_r32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    outl(PCI_ADDR, (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)dev << 11)
         | ((uint32_t)fn << 8) | (off & 0xFC));
    return inl(PCI_DATA);
}
static uint16_t pci_r16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    uint32_t d = pci_r32(bus, dev, fn, off & 0xFC);
    return (uint16_t)(d >> ((off & 2) * 8));
}
static void pci_w32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v)
{
    outl(PCI_ADDR, (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)dev << 11)
         | ((uint32_t)fn << 8) | (off & 0xFC));
    outl(PCI_DATA, v);
}
static void pci_w16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint16_t v)
{
    uint32_t d = pci_r32(bus, dev, fn, off & 0xFC);
    uint32_t sh = (off & 2) * 8;
    pci_w32(bus, dev, fn, off & 0xFC,
            (d & ~(0xFFFFu << sh)) | ((uint32_t)v << sh));
}

/* ------------------------------------------------------------------ */
/* HBA registers (BAR5 memory space)                                   */
/* ------------------------------------------------------------------ */

#define HBA_CAP   0x00
#define HBA_GHC   0x04
#define HBA_IS    0x08
#define HBA_PI    0x0C
#define HBA_BOHC  0x28

#define CAP_NP(x)    (((x) & 0x1F) + 1)
#define CAP_BOSS     (1u << 24)      /* BIOS/OS ownership handoff */

#define GHC_AE       (1u << 31)      /* AHCI enable */
#define GHC_IE       (1u << 1)
#define GHC_HR       (1u << 0)       /* HBA reset */

#define BOHC_OOS     (1u << 1)       /* OS owns semaphore */
#define BOHC_BOS     (1u << 0)       /* BIOS owns semaphore */

/* Port register block at base + 0x100 + port*0x80 */
#define PX_CLB   0x00
#define PX_CLBU  0x04
#define PX_FB    0x08
#define PX_FBU   0x0C
#define PX_IS    0x10
#define PX_IE    0x14
#define PX_CMD   0x18
#define PX_TFD   0x20
#define PX_SIG   0x24
#define PX_SSTS  0x28
#define PX_SERR  0x30
#define PX_CI    0x38

#define PXCMD_ST     (1u << 0)   /* start (command processing) */
#define PXCMD_FRE    (1u << 4)   /* FIS receive enable */
#define PXCMD_FR     (1u << 14)  /* FIS receive running */
#define PXCMD_CR     (1u << 15)  /* command list running */

#define TFD_BSY  (1u << 7)
#define TFD_DRQ  (1u << 3)

#define IS_TFES  (1u << 30)      /* task file error */

static volatile uint32_t *g_hba = 0;     /* BAR5 base */
static uint8_t  g_bus = 0, g_dev = 0, g_fn = 0;

static volatile uint32_t *px(int port, uint16_t reg)
{
    return g_hba + (0x100 + port * 0x80 + reg) / 4;
}

/* ------------------------------------------------------------------ */
/* Command structures                                                  */
/* ------------------------------------------------------------------ */

/* H2D Register FIS */
typedef struct __attribute__((packed)) {
    uint8_t  fis_type;     /* 0x27 */
    uint8_t  pmport_c;     /* bit7 = command */
    uint8_t  command;
    uint8_t  feature_lo;
    uint8_t  lba0, lba1, lba2;
    uint8_t  device;
    uint8_t  lba3, lba4, lba5;
    uint8_t  feature_hi;
    uint16_t count;
    uint8_t  icc;
    uint8_t  control;
    uint8_t  reserved[4];
} FisH2D;

typedef struct __attribute__((packed)) {
    uint32_t dba;
    uint32_t dbau;
    uint32_t reserved;
    uint32_t dbc;          /* bits 21:0 = byte count - 1, bit31 = IOC */
} PrdtEntry;

typedef struct __attribute__((packed)) {
    FisH2D   cfis_fis;
    uint8_t  cfis_pad[64 - sizeof(FisH2D)];   /* CFIS area is 64 bytes */
    uint8_t  acmd[16];                        /* ATAPI command area */
    uint8_t  rsvd[48];                        /* pad to 0x80 */
    PrdtEntry prdt[8];                        /* PRDT must start at offset 0x80 */
} CmdTable;

typedef struct __attribute__((packed)) {
    uint8_t  flags;        /* CFL bits0-4, W bit6, A bit5, P bit7 */
    uint8_t  flags2;       /* PMP bits0-3 */
    uint16_t prdtl;
    volatile uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t rsvd[4];
} CmdHeader;              /* 32 bytes; 32 of them = 1 KiB command list */

#define MAX_PORTS  32
#define PRDT_MAX   8

typedef struct {
    CmdHeader *cl;         /* 1 KiB aligned */
    uint8_t   *fis;        /* 256 B aligned */
    CmdTable  *ct;         /* 128 B aligned (slot 0) */
    int        present;
    int        is_atapi;
    uint64_t   sectors;
    char       model[41];
    char       name[8];
    BlockDev   bdev;
} AhciPort;

static AhciPort g_ports[MAX_PORTS];
static int      g_ndisks = 0;
static volatile int g_irq_seen = 0;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void ms_spin(uint32_t ms)
{
    for (uint32_t m = 0; m < ms; m++)
        for (volatile int i = 0; i < 2000; i++)
            __asm__ volatile ("pause");
}

static int wait_not_busy(int port, int timeout_ms)
{
    for (int i = 0; i < timeout_ms * 10; i++) {
        uint32_t tfd = *px(port, PX_TFD);
        if (!(tfd & (TFD_BSY | TFD_DRQ))) return 0;
        ms_spin(1);
    }
    return -1;
}

static void ahci_irq_handler(uint64_t vector, uint64_t error_code)
{
    (void)vector; (void)error_code;
    g_irq_seen = 1;
    IRQ_EOI((int)vector);
}

/* ------------------------------------------------------------------ */
/* Command issue                                                       */
/* ------------------------------------------------------------------ */

/* Issue one command in slot 0.  fislen is in dwords; write=1 sets the
 * H2D 'W' flag (device expects to receive data). */
static int ahci_cmd(AhciPort *p, uint8_t cmd, uint64_t lba, uint16_t count,
                    void *buf, uint32_t bytes, int write)
{
    int port = (int)(p - g_ports);

    if (wait_not_busy(port, 500) != 0) {
        kprint("[AHCI] port busy\n");
        return -1;
    }

    CmdHeader *h = &p->cl[0];
    h->flags   = (uint8_t)((sizeof(FisH2D) / 4) | (write ? (1 << 6) : 0));
    h->flags2  = 0;
    h->prdtl   = 1;
    h->prdbc   = 0;
    h->ctba    = (uint32_t)(uintptr_t)p->ct;
    h->ctbau   = 0;

    uint8_t *f = (uint8_t *)&p->ct->cfis_fis;
    for (uint32_t i = 0; i < sizeof(FisH2D); i++) f[i] = 0;
    p->ct->cfis_fis.fis_type  = 0x27;
    p->ct->cfis_fis.pmport_c  = 0x80;
    p->ct->cfis_fis.command   = cmd;
    p->ct->cfis_fis.device    = 0x40;           /* LBA mode */
    p->ct->cfis_fis.lba0 = (uint8_t)(lba & 0xFF);
    p->ct->cfis_fis.lba1 = (uint8_t)((lba >> 8) & 0xFF);
    p->ct->cfis_fis.lba2 = (uint8_t)((lba >> 16) & 0xFF);
    p->ct->cfis_fis.lba3 = (uint8_t)((lba >> 24) & 0xFF);
    p->ct->cfis_fis.lba4 = (uint8_t)((lba >> 32) & 0xFF);
    p->ct->cfis_fis.lba5 = (uint8_t)((lba >> 40) & 0xFF);
    p->ct->cfis_fis.count     = count;

    p->ct->prdt[0].dba  = (uint32_t)(uintptr_t)buf;
    p->ct->prdt[0].dbau = 0;
    p->ct->prdt[0].reserved = 0;
    p->ct->prdt[0].dbc  = bytes - 1;              /* IOC=0 */

    *px(port, PX_IS) = 0xFFFFFFFF;                /* clear pending */
    *px(port, PX_CI) = 1;                         /* issue slot 0 */

    /* Wait for CI bit0 to clear (poll; IRQ also acknowledged by handler) */
    for (int i = 0; i < 5000; i++) {
        uint32_t is = *px(port, PX_IS);
        if (is & IS_TFES) {
            *px(port, PX_IS) = is;
            kprint("[AHCI] TFES error serr=");
            kprinthex(*px(port, PX_SERR)); kprint("\n");
            return -1;
        }
        if (!(*px(port, PX_CI) & 1)) return 0;
        ms_spin(1);
    }
    kprint("[AHCI] command timeout\n");
    return -1;
}

/* ------------------------------------------------------------------ */
/* Port bring-up                                                       */
/* ------------------------------------------------------------------ */

static void port_stop(int port)
{
    uint32_t cmd = *px(port, PX_CMD);
    cmd &= ~PXCMD_ST;
    *px(port, PX_CMD) = cmd;
    for (int i = 0; i < 500 && (*px(port, PX_CMD) & PXCMD_CR); i++)
        ms_spin(1);
    cmd &= ~PXCMD_FRE;
    *px(port, PX_CMD) = cmd;
    for (int i = 0; i < 500 && (*px(port, PX_CMD) & PXCMD_FR); i++)
        ms_spin(1);
}

static int port_start(int port)
{
    AhciPort *p = &g_ports[port];

    port_stop(port);

    p->cl  = (CmdHeader *)DMA_Alloc(1024, 1024);
    p->fis = (uint8_t *)  DMA_Alloc(256, 256);
    p->ct  = (CmdTable *) DMA_Alloc(sizeof(CmdTable), 128);
    if (!p->cl || !p->fis || !p->ct) {
        kprint("[AHCI] DMA_Alloc failed\n");
        return -1;
    }

    *px(port, PX_CLB)  = (uint32_t)(uintptr_t)p->cl;
    *px(port, PX_CLBU) = 0;
    *px(port, PX_FB)   = (uint32_t)(uintptr_t)p->fis;
    *px(port, PX_FBU)  = 0;

    *px(port, PX_SERR) = 0xFFFFFFFF;   /* clear errors */
    *px(port, PX_IS)   = 0xFFFFFFFF;   /* clear pending */

    *px(port, PX_IE) = 0xFFFFFFFF;     /* all port interrupts on */
    *px(port, PX_CMD) |= PXCMD_FRE;
    *px(port, PX_CMD) |= PXCMD_ST;
    return 0;
}

/* ------------------------------------------------------------------ */
/* IDENTIFY                                                            */
/* ------------------------------------------------------------------ */

static uint16_t g_ident[256] __attribute__((aligned(256)));

static int ahci_identify(AhciPort *p)
{
    uint8_t cmd = p->is_atapi ? 0xA1 : 0xEC;
    if (ahci_cmd(p, cmd, 0, 0, g_ident, 512, 0) != 0)
        return -1;

    if (g_ident[83] & (1 << 10)) {
        p->sectors = (uint64_t)g_ident[100]
                   | ((uint64_t)g_ident[101] << 16)
                   | ((uint64_t)g_ident[102] << 32)
                   | ((uint64_t)g_ident[103] << 48);
    } else {
        p->sectors = (uint64_t)g_ident[60] | ((uint64_t)g_ident[61] << 16);
    }

    for (int i = 0; i < 20; i++) {
        uint16_t w = g_ident[27 + i];
        p->model[i * 2]     = (char)(w >> 8);
        p->model[i * 2 + 1] = (char)(w & 0xFF);
    }
    p->model[40] = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* BlockDev ops                                                        */
/* ------------------------------------------------------------------ */

static uint8_t g_io_buf[PRDT_MAX * 65536] __attribute__((aligned(512)));

static int ahci_read(BlockDev *dev, uint64_t sector, void *buffer,
                     uint32_t num_sectors)
{
    AhciPort *p = (AhciPort *)dev->private_data;
    if (p->is_atapi) return -1;
    while (num_sectors) {
        uint32_t chunk = num_sectors > (PRDT_MAX * 128)
                       ? (PRDT_MAX * 128) : num_sectors;
        if (ahci_cmd(p, 0x25, sector, (uint16_t)chunk,
                     g_io_buf, chunk * 512, 0) != 0)
            return -1;
        uint8_t *d = (uint8_t *)buffer;
        const uint8_t *s = g_io_buf;
        for (uint32_t i = 0; i < chunk * 512; i++) d[i] = s[i];
        buffer = d;
        sector += chunk;
        num_sectors -= chunk;
    }
    return 0;
}

static int ahci_write(BlockDev *dev, uint64_t sector, const void *buffer,
                      uint32_t num_sectors)
{
    AhciPort *p = (AhciPort *)dev->private_data;
    if (p->is_atapi) return -1;
    while (num_sectors) {
        uint32_t chunk = num_sectors > (PRDT_MAX * 128)
                       ? (PRDT_MAX * 128) : num_sectors;
        const uint8_t *s = (const uint8_t *)buffer;
        for (uint32_t i = 0; i < chunk * 512; i++) g_io_buf[i] = s[i];
        if (ahci_cmd(p, 0x35, sector, (uint16_t)chunk,
                     g_io_buf, chunk * 512, 1) != 0)
            return -1;
        buffer = s + chunk * 512;
        sector += chunk;
        num_sectors -= chunk;
    }
    return 0;
}

static uint64_t ahci_capacity(BlockDev *dev)
{
    return ((AhciPort *)dev->private_data)->sectors;
}

static BlockDevOps g_ahci_ops = {
    .read = ahci_read,
    .write = ahci_write,
    .get_capacity = ahci_capacity,
};

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */

static int find_hba(uint8_t *bus, uint8_t *dev, uint8_t *fn)
{
    for (int b = 0; b < 256; b++)
        for (int d = 0; d < 32; d++)
            for (int f = 0; f < 8; f++) {
                uint32_t id = pci_r32((uint8_t)b, (uint8_t)d, (uint8_t)f, 0x00);
                if (id == 0xFFFFFFFF) continue;
                uint32_t cls = pci_r32((uint8_t)b, (uint8_t)d, (uint8_t)f, 0x08);
                uint8_t class_code = (uint8_t)(cls >> 24);
                uint8_t subclass   = (uint8_t)(cls >> 16);
                uint8_t progif     = (uint8_t)(cls >> 8);
                /* Prefer the known ICH8M device, accept generic AHCI */
                if (id == 0x28298086 ||
                    (class_code == 0x01 && subclass == 0x06 && progif == 0x01)) {
                    *bus = (uint8_t)b; *dev = (uint8_t)d; *fn = (uint8_t)f;
                    kprint("[AHCI] HBA at ");
                    kprinthex((uint64_t)b); kprint(":");
                    kprinthex((uint64_t)d); kprint(".");
                    kprintdec((uint32_t)f); kprint(" id=");
                    kprinthex(id); kprint("\n");
                    return 1;
                }
            }
    return 0;
}

int AHCI_Init(void)
{
    if (!find_hba(&g_bus, &g_dev, &g_fn))
        return -1;

    uint32_t bar5 = pci_r32(g_bus, g_dev, g_fn, 0x24);
    if (bar5 & 1) {
        kprint("[AHCI] BAR5 is I/O space — not supported\n");
        return -1;
    }
    g_hba = (volatile uint32_t *)(uintptr_t)(bar5 & 0xFFFFFFF0u);
    kprint("[AHCI] ABAR="); kprinthex(bar5 & 0xFFFFFFF0u); kprint("\n");

    /* Enable memory space + bus mastering */
    uint16_t cmd = pci_r16(g_bus, g_dev, g_fn, 0x04);
    pci_w16(g_bus, g_dev, g_fn, 0x04, (uint16_t)(cmd | 0x06));

    uint32_t cap = g_hba[HBA_CAP / 4];

    /* Take ownership from BIOS if the hand-off mechanism exists */
    if (cap & CAP_BOSS) {
        g_hba[HBA_BOHC / 4] |= BOHC_OOS;
        for (int i = 0; i < 250; i++) {
            if (!(g_hba[HBA_BOHC / 4] & BOHC_BOS)) break;
            ms_spin(10);
        }
    }

    /* Enable AHCI mode then reset the HBA */
    g_hba[HBA_GHC / 4] |= GHC_AE;
    g_hba[HBA_GHC / 4] |= GHC_HR;
    for (int i = 0; i < 1000 && (g_hba[HBA_GHC / 4] & GHC_HR); i++)
        ms_spin(1);
    if (g_hba[HBA_GHC / 4] & GHC_HR) {
        kprint("[AHCI] HBA reset timed out\n");
        return -1;
    }
    g_hba[HBA_GHC / 4] |= GHC_AE | GHC_IE;

    uint32_t pi = g_hba[HBA_PI / 4];
    kprint("[AHCI] ports-implemented="); kprinthex(pi); kprint("\n");

    int nports = CAP_NP(cap);
    for (int port = 0; port < nports && port < MAX_PORTS; port++) {
        if (!(pi & (1u << port))) continue;
        uint32_t ssts = *px(port, PX_SSTS);
        kprint("[AHCI] port "); kprintdec((uint32_t)port);
        kprint(" ssts="); kprinthex(ssts); kprint("\n");
        if ((ssts & 0xF) != 3) continue;          /* device not detected */

        AhciPort *p = &g_ports[port];
        p->present = 1;

        if (port_start(port) != 0) continue;
        uint32_t sig = *px(port, PX_SIG);
        p->is_atapi = (sig == 0xEB140101);         /* ATAPI signature */

        int idr = ahci_identify(p);
        if (idr != 0 || p->is_atapi || p->sectors == 0) continue;

        /* Build the block device name: ahci0, ahci1, ... */
        p->name[0] = 'a'; p->name[1] = 'h'; p->name[2] = 'c';
        p->name[3] = 'i'; p->name[4] = (char)('0' + g_ndisks);
        p->name[5] = 0;

        p->bdev.name          = p->name;
        p->bdev.display_name  = p->model;
        p->bdev.sector_size   = 512;
        p->bdev.num_sectors   = p->sectors;
        p->bdev.private_data  = p;
        p->bdev.ops           = &g_ahci_ops;
        p->bdev.next          = 0;

        if (BlockDev_Register(&p->bdev) == 0) {
            g_ndisks++;
            kprint("[AHCI] "); kprint(p->name);
            kprint(": "); kprint(p->model);
            kprint(" "); kprinthex(p->sectors);
            kprint(" sectors\n");
        }
    }

    if (g_ndisks == 0) {
        kprint("[AHCI] no disks found\n");
        return 0;
    }
    return g_ndisks;
}

/* Called after IDT/PIC/IRQ routing are up — attaches the interrupt.
 * Polling still works if this fails, so failure is non-fatal. */
void AHCI_SetupIRQ(void)
{
    if (!g_hba) return;
    int vec = IRQ_AttachMSI(g_bus, g_dev, g_fn, ahci_irq_handler, "ahci");
    if (vec < 0)
        vec = IRQ_AttachPCI(g_bus, g_dev, g_fn, ahci_irq_handler, "ahci");
    if (vec >= 0) {
        kprint("[AHCI] IRQ attached, vector ");
        kprinthex((uint64_t)vec); kprint("\n");
    } else {
        kprint("[AHCI] no IRQ route — polling only\n");
    }
}
