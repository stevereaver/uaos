/* irq.c — Unified interrupt routing layer
 *
 * Two backends:
 *   PIC    — legacy 8259A pair, IRQs 0-15 map to vectors 32-47 (QEMU
 *            i440fx default path, kept for regression safety).
 *   IOAPIC — APIC-mode routing used when the MADT exposes an IO-APIC.
 *            ISA IRQs go through the ISO table; PCI INTx is decoded
 *            from ICH8/ICH9-class LPC routing registers (DxxIP/DxxIR
 *            + PIRQx_ROUT) falling back to the PCI interrupt-line
 *            register; MSI is available for devices that support it.
 *
 * Vector layout: 32+GSI for line-based interrupts, 0x60-0x7F pool for
 * MSI.  Every vector registered through this layer is recorded in
 * g_kind[] so ISR_Dispatch can send the right EOI.
 */

#include "irq.h"
#include "acpi.h"
#include "ioapic.h"
#include "../boot/kprint.h"
#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Port I/O                                                            */
/* ------------------------------------------------------------------ */

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
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

/* ------------------------------------------------------------------ */
/* PCI config access                                                   */
/* ------------------------------------------------------------------ */

#define PCI_ADDR 0xCF8
#define PCI_DATA 0xCFC

static uint32_t pci_r32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    uint32_t a = (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)dev << 11)
               | ((uint32_t)fn << 8) | (off & 0xFC);
    outl(PCI_ADDR, a);
    return inl(PCI_DATA);
}
static uint16_t pci_r16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    uint32_t d = pci_r32(bus, dev, fn, off & 0xFC);
    return (uint16_t)(d >> ((off & 2) * 8));
}
static uint8_t pci_r8(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    uint32_t d = pci_r32(bus, dev, fn, off & 0xFC);
    return (uint8_t)(d >> ((off & 3) * 8));
}
static void pci_w32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v)
{
    uint32_t a = (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)dev << 11)
               | ((uint32_t)fn << 8) | (off & 0xFC);
    outl(PCI_ADDR, a);
    outl(PCI_DATA, v);
}
static void pci_w16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint16_t v)
{
    uint32_t d = pci_r32(bus, dev, fn, off & 0xFC);
    uint32_t sh = (off & 2) * 8;
    d = (d & ~(0xFFFFu << sh)) | ((uint32_t)v << sh);
    pci_w32(bus, dev, fn, off & 0xFC, d);
}
static void pci_w8(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint8_t v)
{
    uint32_t d = pci_r32(bus, dev, fn, off & 0xFC);
    uint32_t sh = (off & 3) * 8;
    d = (d & ~(0xFFu << sh)) | ((uint32_t)v << sh);
    pci_w32(bus, dev, fn, off & 0xFC, d);
}

/* Extended config space via ECAM (offsets >= 0x100). */
static int ecam_avail(void) { return ACPI_EcamBase() != 0; }

static uint8_t *ecam_addr(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off)
{
    uint64_t base = ACPI_EcamBase();
    return (uint8_t *)(uintptr_t)(base + ((uint32_t)bus << 20)
          + ((uint32_t)dev << 15) + ((uint32_t)fn << 12) + off);
}
static uint32_t ecam_r32(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off)
{
    return *(volatile uint32_t *)ecam_addr(bus, dev, fn, off);
}
static uint16_t ecam_r16(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off)
{
    return *(volatile uint16_t *)ecam_addr(bus, dev, fn, off);
}

/* Root Complex Base Address block — LPC bridge (00:1f.0) config 0xF0.
 * The DxxIP/DxxIR interrupt-map registers live in RCBA MMIO space, NOT
 * in PCI config space (offsets 0x31xx exceed the 4 KB ECAM window —
 * reading them via ECAM returns garbage 0xFF). */
#define LPC_RCBA_REG   0xF0
#define RCBA_ENABLE    0x01
#define RCBA_BASE_MASK 0xFFFFC000u   /* 16 KB window */
#define RCBA_DEFAULT   0xFED1C000u   /* conventional chipset assignment */

static volatile uint8_t *g_rcba;
static int g_rcba_tried;

static volatile uint8_t *ich_rcba(void)
{
    if (g_rcba_tried) return g_rcba;
    g_rcba_tried = 1;
    uint32_t v = pci_r32(0, 31, 0, LPC_RCBA_REG);
    if (!(v & RCBA_ENABLE) || !(v & RCBA_BASE_MASK)) {
        /* Firmware left the block undecoded — give it the conventional
         * chipset MMIO window. */
        pci_w32(0, 31, 0, LPC_RCBA_REG, RCBA_DEFAULT | RCBA_ENABLE);
        v = pci_r32(0, 31, 0, LPC_RCBA_REG);
        if (!(v & RCBA_ENABLE) || !(v & RCBA_BASE_MASK)) {
            kprint("[IRQ] ich-route: RCBA unavailable (0xF0=");
            kprinthex(v); kprint(")\n");
            return 0;
        }
        kprint("[IRQ] ich-route: assigned RCBA ");
        kprinthex(v & RCBA_BASE_MASK); kprint("\n");
    }
    g_rcba = (volatile uint8_t *)(uintptr_t)(v & RCBA_BASE_MASK);
    return g_rcba;
}

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

#define VEC_NONE  0
#define VEC_PIC   1
#define VEC_APIC  2
#define VEC_MSI   3

static int     g_mode = IRQ_MODE_PIC;
static int     g_pic_fallback = 0;   /* IO-APIC detected but delivery dead
                                        (probe found no ticks); all new
                                        attaches ride the 8259 instead. */

void IRQ_PicFallback(void) { g_pic_fallback = 1; }
int  IRQ_PicFallbackActive(void) { return g_pic_fallback; }
static uint8_t g_kind[256];    /* VEC_* per vector */
static uint8_t g_gsi[256];     /* GSI per vector (diagnostics) */
static int     g_msi_next = 0x60;   /* MSI vector pool 0x60-0x7F */

/* Shared level-triggered lines: on PIIX3-class chipsets several PCI
 * functions land on the same GSI.  The first attach owns the vector;
 * later attaches chain so every device gets a chance to ack its INTx. */
#define IRQ_SHARED_MAX 4
static ISRHandler  g_shared[256][IRQ_SHARED_MAX];
static uint8_t     g_shared_n[256];
static ISRHandler  g_pri[256];   /* primary handler per IRQ-layer vector */

static void irq_shared_dispatch(uint64_t vector, uint64_t error_code)
{
    uint8_t n = g_shared_n[(uint8_t)vector];
    for (uint8_t i = 0; i < n; i++)
        g_shared[(uint8_t)vector][i](vector, error_code);
}

/* Returns 0 on success.  Moves the existing primary handler into slot 0
 * of the chain and installs the fan-out trampoline as the primary. */
static int irq_shared_add(int vec, ISRHandler handler, const char *name)
{
    if (g_kind[vec] != VEC_APIC) return -1;
    if (g_shared_n[vec] == 0) {
        ISRHandler pri = g_pri[vec];
        if (!pri) return -1;
        g_shared[vec][0] = pri;
        g_shared_n[vec]  = 1;
        IDT_SetHandler((uint8_t)vec, irq_shared_dispatch, "shared");
    }
    if (g_shared_n[vec] >= IRQ_SHARED_MAX) return -1;
    g_shared[vec][g_shared_n[vec]++] = handler;
    kprint("[IRQ] sharing vector "); kprintdec((uint32_t)vec);
    kprint(" with "); kprint(name ? name : "?"); kprint("\n");
    return 0;
}

int IRQ_Mode(void) { return g_mode; }

/* Storm failsafe — sampled once per PIT tick from ISR_Dispatch counters.
 * A level-triggered line nobody can ack would otherwise saturate the CPU
 * (observed on VirtualBox: EFI-leftover INTx assertion on a shared GSI).
 * Threshold 400 dispatches/10 ms (~40k/s); mask + warn once, and the
 * vector is re-armed if a shared-handler attach re-unmasks the GSI. */
#define IRQ_STORM_PER_TICK 400

void IRQ_StormTick(void)
{
    if (g_mode != IRQ_MODE_IOAPIC) return;
    static uint64_t last[256];
    static uint8_t  stormed[256];
    uint64_t now[256];
    IDT_SnapshotCounts(now);
    for (int v = 32; v < 128; v++) {
        uint64_t d = now[v] - last[v];
        last[v] = now[v];
        if (d <= IRQ_STORM_PER_TICK || g_kind[v] != VEC_APIC || stormed[v])
            continue;
        stormed[v] = 1;
        kprint("[IRQ] storm on vector "); kprintdec((uint32_t)v);
        kprint(" (gsi "); kprintdec(g_gsi[v]); kprint(") — masking\n");
        IOAPIC_Mask(g_gsi[v]);
    }
}

/* Immediate storm break — called from ISR_Dispatch when one vector has
 * been delivered N times in a row with no other interrupt interleaved.
 * This is the only detection that works when the storm vector outranks
 * the PIT (LAPIC serves the highest pending vector, so a storming vec43
 * starves vec34 and the tick-based check above never runs). */
void IRQ_StormMask(int vector)
{
    if (vector < 32 || vector > 255) return;
    if (g_kind[vector] != VEC_APIC) return;
    kprint("[IRQ] storm on vector "); kprintdec((uint32_t)vector);
    kprint(" (gsi "); kprintdec(g_gsi[vector]); kprint(") — masking\n");
    IOAPIC_Mask(g_gsi[vector]);
}

/* ------------------------------------------------------------------ */
/* ICH8/ICH9-class PCI INTx decoding                                   */
/* ------------------------------------------------------------------ */

/* LPC bridge (00:1f.0) registers:
 *   PIRQA-H routing : classic config 0x60-0x67 (bit7=1 -> unrouted,
 *                     bits4:0 = IRQ/GSI number)
 *   DxxIP (pin map) : RCBA 0x3100 + 4*(31-dev) — uniform 32-bit stride
 *   DxxIR (pin->PIRQ): RCBA, per-device table — NOT uniform: D27IR ends
 *                     at 0x3148 and D26IR resumes at 0x314C (0x314A/0x314E
 *                     are holes), per ICH8 datasheet 7.1.59-65          */

static int ich_dip_off(uint8_t dev)
{
    if (dev >= 25 && dev <= 31) return 0x3100 + 4 * (31 - dev);
    return -1;
}
static int ich_dir_off(uint8_t dev)
{
    /* A uniform 2*(31-dev) stride lands dev26 on the 0x314A hole —
     * reads return 0 and the reprogram write goes nowhere, so both
     * 00:1A.x UHCIs decoded as PIRQA while the silicon drove whatever
     * the real D26IR said (UAOS-174: zero dispatches on MBP4,1). */
    switch (dev) {
    case 31: return 0x3140;
    case 30: return 0x3142;
    case 29: return 0x3144;
    case 28: return 0x3146;
    case 27: return 0x3148;
    case 26: return 0x314C;
    case 25: return 0x3150;
    default: return -1;
    }
}

static int lpc_is_intel_ich(void)
{
    if (pci_r16(0, 31, 0, 0x00) != 0x8086) return 0;
    /* Restrict to real ICH8/9/10 LPC bridge DIDs — PIIX3 (QEMU) is also
     * vendor 8086 but has no RCBA at cfg 0xF0. */
    uint16_t did = pci_r16(0, 31, 0, 0x02);
    return (did >= 0x2810 && did <= 0x281F) ||   /* ICH8  */
           (did >= 0x2910 && did <= 0x291F) ||   /* ICH9  */
           (did >= 0x3A10 && did <= 0x3A4F);     /* ICH10 */
}

/* Resolve a bus-0 function's INTx to a GSI via chipset registers.
 * Returns GSI (>=0) or -1 if undecodable.  Stage telemetry is logged so
 * a firmware-specific failure point (Apple EFI leaves a lot unrouted)
 * is visible instead of collapsing to a bare "cannot resolve". */
static int ich_route_gsi(uint8_t bus, uint8_t dev, uint8_t fn)
{
    uint8_t pin = pci_r8(bus, dev, fn, 0x3D);      /* interrupt pin */
    if (pin < 1 || pin > 4) {
        kprint("[IRQ] ich-route dev=");
        kprinthex(dev); kprint("."); kprintdec(fn);
        kprint(" fail: intpin="); kprinthex(pin); kprint("\n");
        return -1;
    }

    if (!lpc_is_intel_ich()) {
        kprint("[IRQ] ich-route: lpc not intel\n");
        return -1;
    }
    volatile uint8_t *rcba = ich_rcba();
    if (!rcba) {
        kprint("[IRQ] ich-route: no RCBA\n");
        return -1;
    }
    int ioff = ich_dip_off(dev);
    int roff = ich_dir_off(dev);
    if (ioff < 0 || roff < 0) {
        kprint("[IRQ] ich-route dev=");
        kprinthex(dev); kprint(" no DxxIP/DxxIR (dev>31 range)\n");
        return -1;
    }

    /* DxxIP: nibble per function holds the pin the function drives. */
    uint32_t ip = *(volatile uint32_t *)(rcba + ioff);
    uint8_t chip_pin = (uint8_t)((ip >> (fn * 4)) & 0xF);
    if (chip_pin >= 1 && chip_pin <= 4) pin = chip_pin;

    /* DxxIR: nibble per pin selects PIRQA-H (0-7), 0xF = unrouted. */
    volatile uint16_t *irp = (volatile uint16_t *)(rcba + roff);
    uint16_t ir = *irp;

    /* A full-zero DxxIR is anomalous — the ICH8M reset default is
     * 0x3210.  It either means firmware left the whole register cleared
     * (then every pin claims PIRQA, wrong for pins B-D) or the read is
     * landing on a non-decoding offset.  Either way, program the
     * canonical pin->PIRQ map for OUR pin and verify the write sticks
     * before trusting the decode (UAOS-174, MBP4,1 D26IR read 0x0). */
    if (ir == 0) {
        /* Write the documented ICH8M reset default (INTA->PIRQA …
         * INTD->PIRQD) rather than just our nibble, so sibling
         * functions on other pins keep a sane route. */
        *irp = 0x3210;
        ir = *irp;
        if (((ir >> ((pin - 1) * 4)) & 0xF) != ((pin - 1) & 0x7)) {
            kprint("[IRQ] ich-route dev=");
            kprinthex(dev); kprint("."); kprintdec(fn);
            kprint(" dir read 0 and write failed — untrusted\n");
            return -1;      /* fall back to the PCI intline register */
        }
        kprint("[IRQ] ich-route dev=");
        kprinthex(dev); kprint("."); kprintdec(fn);
        kprint(" dir was 0x0 — programmed pin map, dir=0x");
        kprinthex(ir); kprint("\n");
    }

    uint8_t nib = (uint8_t)((ir >> ((pin - 1) * 4)) & 0xF);
    uint8_t pirq = nib;
    if (nib == 0xF) {
        /* Firmware left this pin unrouted — try programming the nibble
         * ourselves: INTA->PIRQA, INTB->PIRQB, ... (the canonical map a
         * BIOS POST would write).  Readback proves the register exists
         * and is writable. */
        uint8_t want = (uint8_t)(pin - 1);
        uint16_t nv = (uint16_t)((ir & ~(0xFu << ((pin - 1) * 4)))
                                 | ((uint16_t)want << ((pin - 1) * 4)));
        *irp = nv;
        ir = *irp;
        pirq = (uint8_t)((ir >> ((pin - 1) * 4)) & 0xF);
        if (pirq > 7) {
            kprint("[IRQ] ich-route dev=");
            kprinthex(dev); kprint("."); kprintdec(fn);
            kprint(" pin="); kprinthex(pin);
            kprint(" dip=0x"); kprinthex(ip);
            kprint(" dir=0x"); kprinthex(ir);
            kprint(" fail: dir nibble unrouted\n");
            return -1;
        }
        kprint("[IRQ] ich-route dev=");
        kprinthex(dev); kprint("."); kprintdec(fn);
        kprint(" programmed dir pin="); kprinthex(pin);
        kprint(" -> PIRQ"); kprinthex(pirq); kprint("\n");
    }
    if (pirq > 7) {
        kprint("[IRQ] ich-route dev=");
        kprinthex(dev); kprint("."); kprintdec(fn);
        kprint(" fail: dir nibble="); kprinthex(nib); kprint("\n");
        return -1;
    }

    /* APIC mode: the chipset PIRQA-H lines are hardwired to IO-APIC
     * inputs 16-23 — the cfg 0x60-0x67 PIRQ_ROUTE registers only select
     * the ISA IRQ for 8259/PIC mode and must not be consulted here
     * (they read 0 on Apple EFI, which we mistook for "routed to GSI0"
     * -> level line stormed on vector 32). */
    kprint("[IRQ] ich-route dev=");
    kprinthex(dev); kprint("."); kprintdec(fn);
    kprint(" pin="); kprinthex(pin);
    kprint(" dip=0x"); kprinthex(ip); kprint(" dir=0x"); kprinthex(ir);
    kprint(" -> PIRQ"); kprinthex(pirq);
    kprint(" = gsi "); kprintdec((uint32_t)(16 + pirq)); kprint("\n");
    return 16 + pirq;
}

/* Public resolver: chipset decode first, then the firmware-programmed
 * interrupt line register (SeaBIOS/QEMU writes the GSI here). */
int IRQ_ResolvePCI(uint8_t bus, uint8_t dev, uint8_t fn)
{
    if (bus == 0) {
        int gsi = ich_route_gsi(bus, dev, fn);
        if (gsi >= 0) {
            kprint("[IRQ] pci 0:"); kprinthex(dev); kprint(".");
            kprintdec(fn); kprint(" -> gsi ");
            kprintdec((uint32_t)gsi); kprint(" (ich-route)\n");
            return gsi;
        }
    }
    uint8_t line = pci_r8(bus, dev, fn, 0x3C);
    if (line != 0xFF && line != 0) {
        kprint("[IRQ] pci "); kprinthex(bus); kprint(":");
        kprinthex(dev); kprint("."); kprintdec(fn);
        kprint(" -> gsi "); kprintdec(line); kprint(" (intline)\n");
        return line;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Attach operations                                                   */
/* ------------------------------------------------------------------ */

int IRQ_AttachISA(int isa_irq, ISRHandler handler, const char *name)
{
    if (isa_irq < 0 || isa_irq > 15) return -1;

    if (g_mode == IRQ_MODE_PIC || g_pic_fallback) {
        int vec = 32 + isa_irq;
        IDT_SetHandler((uint8_t)vec, handler, name);
        g_kind[vec] = VEC_PIC;
        g_gsi[vec]  = (uint8_t)isa_irq;
        PIC_UnmaskIRQ(isa_irq);
        return vec;
    }

    uint16_t flags = 0;
    int gsi = ACPI_IsaToGsi(isa_irq, &flags);
    int vec = 32 + gsi;
    uint32_t rte = 0;
    if ((flags & 3) == 3) rte |= IOAPIC_RTE_LOW;    /* polarity low */
    if ((flags & 0xC) == 0xC) rte |= IOAPIC_RTE_LEVEL; /* trigger level */
    IOAPIC_ProgramGSI((uint32_t)gsi, (uint8_t)vec, rte);
    IDT_SetHandler((uint8_t)vec, handler, name);
    g_kind[vec] = VEC_APIC;
    g_gsi[vec]  = (uint8_t)gsi;
    IOAPIC_Unmask((uint32_t)gsi);
    return vec;
}

int IRQ_AttachISAPIC(int isa_irq, ISRHandler handler, const char *name)
{
    if (isa_irq < 0 || isa_irq > 15) return -1;
    int vec = 32 + isa_irq;
    IDT_SetHandler((uint8_t)vec, handler, name);
    g_kind[vec] = VEC_PIC;
    g_gsi[vec]  = (uint8_t)isa_irq;
    PIC_UnmaskIRQ(isa_irq);
    return vec;
}

int IRQ_AttachPCI(uint8_t bus, uint8_t dev, uint8_t fn,
                  ISRHandler handler, const char *name)
{
    uint8_t pin = pci_r8(bus, dev, fn, 0x3D);
    if (pin == 0 || pin > 4) {
        kprint("[IRQ] pci device has no INTx pin\n");
        return -1;
    }

    if (g_mode == IRQ_MODE_PIC || g_pic_fallback) {
        uint8_t line = pci_r8(bus, dev, fn, 0x3C);
        if (line == 0xFF || line >= 16) return -1;
        uint16_t cmd = pci_r16(bus, dev, fn, 0x04);
        if (cmd & 0x0400)
            pci_w16(bus, dev, fn, 0x04, (uint16_t)(cmd & ~0x0400u));
        int vec = 32 + line;
        IDT_SetHandler((uint8_t)vec, handler, name);
        g_kind[vec] = VEC_PIC;
        g_gsi[vec]  = line;
        PIC_UnmaskIRQ(line);
        return vec;
    }

    int gsi = IRQ_ResolvePCI(bus, dev, fn);
    if (gsi < 0 || gsi >= 64) {
        kprint("[IRQ] cannot resolve PCI IRQ for ");
        kprinthex(bus); kprint(":"); kprinthex(dev); kprint(".");
        kprintdec(fn); kprint("\n");
        return -1;
    }

    /* Ensure INTx is allowed to assert — firmware may have left the
     * PCI command INTx-disable bit (bit 10) set. */
    {
        uint16_t cmd = pci_r16(bus, dev, fn, 0x04);
        if (cmd & 0x0400) {
            pci_w16(bus, dev, fn, 0x04, (uint16_t)(cmd & ~0x0400u));
            kprint("[IRQ] cleared INTx-disable on ");
            kprinthex(bus); kprint(":"); kprinthex(dev); kprint(".");
            kprintdec(fn); kprint("\n");
        }
    }

    int vec = 32 + gsi;
    if (g_kind[vec] == VEC_APIC) {
        /* Another PCI device already owns this GSI — level-triggered
         * lines are shared, so chain the handler instead of replacing
         * it.  Re-unmask in case the storm failsafe masked it while no
         * handler could ack the line. */
        if (irq_shared_add(vec, handler, name) != 0) {
            kprint("[IRQ] shared-vector table full for gsi ");
            kprintdec((uint32_t)gsi); kprint("\n");
            return -1;
        }
        IOAPIC_Unmask((uint32_t)gsi);
        return vec;
    }
    /* PCI INTx is level-triggered, active-low. */
    IOAPIC_ProgramGSI((uint32_t)gsi, (uint8_t)vec,
                      IOAPIC_RTE_LEVEL | IOAPIC_RTE_LOW);
    IDT_SetHandler((uint8_t)vec, handler, name);
    g_pri[vec]  = handler;
    g_kind[vec] = VEC_APIC;
    g_gsi[vec]  = (uint8_t)gsi;
    IOAPIC_Unmask((uint32_t)gsi);
    return vec;
}

int IRQ_AttachMSI(uint8_t bus, uint8_t dev, uint8_t fn,
                  ISRHandler handler, const char *name)
{
    /* Locate the MSI capability (cap ID 0x05). */
    if (!(pci_r16(bus, dev, fn, 0x06) & (1 << 4))) return -1;
    uint8_t ptr = pci_r8(bus, dev, fn, 0x34) & 0xFC;
    uint8_t msi = 0;
    while (ptr) {
        if (pci_r8(bus, dev, fn, ptr) == 0x05) { msi = ptr; break; }
        ptr = pci_r8(bus, dev, fn, (uint8_t)(ptr + 1)) & 0xFC;
    }
    if (!msi) return -1;

    /* Allocate a vector from the MSI pool. */
    int vec = -1;
    while (g_msi_next < 0x80) {
        if (g_kind[g_msi_next] == VEC_NONE) { vec = g_msi_next++; break; }
        g_msi_next++;
    }
    if (vec < 0) return -1;

    uint16_t ctl = pci_r16(bus, dev, fn, (uint8_t)(msi + 2));
    int is64 = (ctl & (1 << 7)) != 0;

    /* Message address: LAPIC target, destination id 0, physical mode. */
    pci_w32(bus, dev, fn, (uint8_t)(msi + 4), 0xFEE00000u);
    if (is64)
        pci_w32(bus, dev, fn, (uint8_t)(msi + 8), 0);
    uint8_t data_off = (uint8_t)(msi + (is64 ? 0x0C : 0x08));
    pci_w16(bus, dev, fn, data_off, (uint16_t)vec);   /* edge, fixed */

    /* Single message, enable. */
    pci_w16(bus, dev, fn, (uint8_t)(msi + 2), (uint16_t)((ctl & ~0x70u) | 1));

    IDT_SetHandler((uint8_t)vec, handler, name);
    g_kind[vec] = VEC_MSI;
    kprint("[IRQ] MSI "); kprinthex(bus); kprint(":"); kprinthex(dev);
    kprint("."); kprintdec(fn); kprint(" -> vector ");
    kprintdec((uint32_t)vec); kprint("\n");
    return vec;
}

/* ------------------------------------------------------------------ */
/* Mask / EOI                                                          */
/* ------------------------------------------------------------------ */

void IRQ_Unmask(int gsi)
{
    if (g_mode == IRQ_MODE_PIC) { if (gsi < 16) PIC_UnmaskIRQ(gsi); }
    else IOAPIC_Unmask((uint32_t)gsi);
}

void IRQ_Mask(int gsi)
{
    if (g_mode == IRQ_MODE_PIC) { if (gsi < 16) PIC_MaskIRQ(gsi); }
    else IOAPIC_Mask((uint32_t)gsi);
}

void IRQ_EOI(int vector)
{
    if (vector < 32 || vector > 255) return;
    uint8_t k = g_kind[vector];
    if (k == VEC_APIC || k == VEC_MSI) {
        LAPIC_EOI();
    } else if (k == VEC_PIC || (g_mode == IRQ_MODE_PIC && vector < 48)) {
        PIC_SendEOI(vector - 32);
    }
}

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */

void IRQ_Init(uint32_t mb2_phys)
{
    ACPI_Init(mb2_phys);

    if (ACPI_Present() && IOAPIC_Available()) {
        g_mode = IRQ_MODE_IOAPIC;
        /* IMCR (MP spec): on PCAT-compat machines a mux defaults to
         * PIC-mode delivery and must be flipped to symmetric I/O APIC.
         * QEMU ignores this; VirtualBox models it — without the write
         * every IRQ goes to the (masked) 8259 and the box wedges at the
         * first tick-dependent wait.  No-op on hardware without IMCR. */
        if (ACPI_PcatCompat()) {
            outb(0x22, 0x70);
            outb(0x23, 0x01);
            kprint("[IRQ] IMCR -> symmetric I/O mode\n");
        }
        /* ICH8-class LPC: OIC (RCBA+0x31FF) bit0 = AEN enables the
         * internal IOxAPIC and its MMIO decode — datasheet default is
         * 0 and firmware may leave it clear under EFI.  Must run before
         * IOAPIC_Init or the RTE writes go nowhere (datasheet requires
         * a read-back after modifying AEN before touching the IOxAPIC
         * range). */
        if (lpc_is_intel_ich()) {
            volatile uint8_t *rcba = ich_rcba();
            if (rcba && !(rcba[0x31FF] & 0x01)) {
                rcba[0x31FF] |= 0x01;
                (void)rcba[0x31FF];
                kprint("[IRQ] ich: OIC AEN set — IOxAPIC enabled\n");
            }
        }
        IOAPIC_Init();
        kprint("[IRQ] IO-APIC mode\n");
    } else {
        g_mode = IRQ_MODE_PIC;
        kprint("[IRQ] PIC mode (no IO-APIC)\n");
    }
}
