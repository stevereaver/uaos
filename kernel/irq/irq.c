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

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

#define VEC_NONE  0
#define VEC_PIC   1
#define VEC_APIC  2
#define VEC_MSI   3

static int     g_mode = IRQ_MODE_PIC;
static uint8_t g_kind[256];    /* VEC_* per vector */
static uint8_t g_gsi[256];     /* GSI per vector (diagnostics) */
static int     g_msi_next = 0x60;   /* MSI vector pool 0x60-0x7F */

int IRQ_Mode(void) { return g_mode; }

/* ------------------------------------------------------------------ */
/* ICH8/ICH9-class PCI INTx decoding                                   */
/* ------------------------------------------------------------------ */

/* LPC bridge (00:1f.0) registers:
 *   PIRQA-H routing : classic config 0x60-0x67 (bit7=1 -> unrouted,
 *                     bits4:0 = IRQ/GSI number)
 *   DxxIP (pin map) : extended config 0x3100 + 4*(31-dev)
 *   DxxIR (pin->PIRQ): extended config 0x3140 + 2*(31-dev)           */

static int ich_dip_off(uint8_t dev)
{
    if (dev >= 25 && dev <= 31) return 0x3100 + 4 * (31 - dev);
    return -1;
}
static int ich_dir_off(uint8_t dev)
{
    if (dev >= 25 && dev <= 31) return 0x3140 + 2 * (31 - dev);
    return -1;
}

static int lpc_is_intel_ich(void)
{
    uint16_t v = pci_r16(0, 31, 0, 0x00);
    return v == 0x8086;
}

/* Resolve a bus-0 function's INTx to a GSI via chipset registers.
 * Returns GSI (>=0) or -1 if undecodable. */
static int ich_route_gsi(uint8_t bus, uint8_t dev, uint8_t fn)
{
    uint8_t pin = pci_r8(bus, dev, fn, 0x3D);      /* interrupt pin */
    if (pin < 1 || pin > 4) return -1;

    if (!lpc_is_intel_ich() || !ecam_avail()) return -1;
    int ioff = ich_dip_off(dev);
    int roff = ich_dir_off(dev);
    if (ioff < 0 || roff < 0) return -1;

    /* DxxIP: nibble per function holds the pin the function drives. */
    uint32_t ip = ecam_r32(0, 31, 0, (uint16_t)ioff);
    uint8_t chip_pin = (uint8_t)((ip >> (fn * 4)) & 0xF);
    if (chip_pin >= 1 && chip_pin <= 4) pin = chip_pin;

    /* DxxIR: nibble per pin selects PIRQA-H (0-7), 0xF = unrouted. */
    uint16_t ir = ecam_r16(0, 31, 0, (uint16_t)roff);
    uint8_t pirq = (uint8_t)((ir >> ((pin - 1) * 4)) & 0xF);
    if (pirq > 7) return -1;

    uint8_t route = pci_r8(0, 31, 0, (uint8_t)(0x60 + pirq));
    if (route & 0x80) return -1;
    return route & 0x1F;
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

    if (g_mode == IRQ_MODE_PIC) {
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

int IRQ_AttachPCI(uint8_t bus, uint8_t dev, uint8_t fn,
                  ISRHandler handler, const char *name)
{
    uint8_t pin = pci_r8(bus, dev, fn, 0x3D);
    if (pin == 0 || pin > 4) {
        kprint("[IRQ] pci device has no INTx pin\n");
        return -1;
    }

    if (g_mode == IRQ_MODE_PIC) {
        uint8_t line = pci_r8(bus, dev, fn, 0x3C);
        if (line == 0xFF || line >= 16) return -1;
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
    int vec = 32 + gsi;
    /* PCI INTx is level-triggered, active-low. */
    IOAPIC_ProgramGSI((uint32_t)gsi, (uint8_t)vec,
                      IOAPIC_RTE_LEVEL | IOAPIC_RTE_LOW);
    IDT_SetHandler((uint8_t)vec, handler, name);
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
        IOAPIC_Init();
        kprint("[IRQ] IO-APIC mode\n");
    } else {
        g_mode = IRQ_MODE_PIC;
        kprint("[IRQ] PIC mode (no IO-APIC)\n");
    }
}
