/* ioapic.c — 82093AA IO-APIC driver
 *
 * Each IO-APIC is accessed via an index/data register pair in its MMIO
 * window: IOREGSEL (offset 0) selects, IOWIN (offset 0x10) transfers.
 */

#include "ioapic.h"
#include "acpi.h"
#include "irq.h"
#include "../boot/kprint.h"
#include <stdint.h>
#include <stddef.h>

#define IOREGSEL 0x00
#define IOWIN    0x10

#define IOAPIC_ID    0x00
#define IOAPIC_VER   0x01
#define IOAPIC_REDTBL 0x10   /* RTE n at 0x10 + 2n (lo), 0x11 + 2n (hi) */

/* RTE bits */
#define RTE_MASK       (1u << 16)
#define RTE_LEVEL      (1u << 15)
#define RTE_LOW        (1u << 13)
#define RTE_LOGICAL    (1u << 11)

/* Find the IO-APIC that owns a GSI (the one with the highest gsi_base
 * not exceeding gsi). */
static const AcpiIoApic *ioapic_for_gsi(uint32_t gsi)
{
    const AcpiIoApic *best = 0;
    for (int i = 0; i < ACPI_NumIoApics(); i++) {
        const AcpiIoApic *a = ACPI_IoApic(i);
        if (gsi >= a->gsi_base && (!best || a->gsi_base > best->gsi_base))
            best = a;
    }
    return best;
}

static uint32_t io_r32(uint32_t base, uint8_t reg)
{
    *(volatile uint32_t *)(uintptr_t)(base + IOREGSEL) = reg;
    return *(volatile uint32_t *)(uintptr_t)(base + IOWIN);
}

static void io_w32(uint32_t base, uint8_t reg, uint32_t val)
{
    *(volatile uint32_t *)(uintptr_t)(base + IOREGSEL) = reg;
    *(volatile uint32_t *)(uintptr_t)(base + IOWIN)    = val;
}

int IOAPIC_Available(void)
{
    return ACPI_NumIoApics() > 0;
}

void IOAPIC_Init(void)
{
    for (int i = 0; i < ACPI_NumIoApics(); i++) {
        const AcpiIoApic *a = ACPI_IoApic(i);
        uint32_t ver = io_r32(a->mmio_base, IOAPIC_VER);
        int maxredir = ((ver >> 16) & 0xFF) + 1;
        kprint("[IOAPIC] id="); kprintdec(a->id);
        kprint(" ver="); kprintdec(ver & 0xFF);
        kprint(" pins="); kprintdec(maxredir);
        kprint(" gsi_base="); kprintdec(a->gsi_base); kprint("\n");
        /* Mask everything — entries are programmed+unmasked on demand. */
        for (int p = 0; p < maxredir; p++) {
            io_w32(a->mmio_base, IOAPIC_REDTBL + 2 * p,
                   RTE_MASK | (uint32_t)(32 + a->gsi_base + p));
            io_w32(a->mmio_base, IOAPIC_REDTBL + 2 * p + 1, 0);
        }
    }
}

void IOAPIC_ProgramGSI(uint32_t gsi, uint8_t vector, uint32_t flags)
{
    const AcpiIoApic *a = ioapic_for_gsi(gsi);
    if (!a) {
        kprint("[IOAPIC] no APIC covers gsi "); kprintdec(gsi); kprint("\n");
        return;
    }
    uint32_t pin = gsi - a->gsi_base;
    uint32_t lo  = RTE_MASK | vector;
    if (flags & IOAPIC_RTE_LEVEL) lo |= RTE_LEVEL;
    if (flags & IOAPIC_RTE_LOW)   lo |= RTE_LOW;
    /* Hi dword: destination = LAPIC physical ID 0, physical dest mode. */
    io_w32(a->mmio_base, IOAPIC_REDTBL + 2 * pin + 1, 0);
    io_w32(a->mmio_base, IOAPIC_REDTBL + 2 * pin,     lo);
}

void IOAPIC_Unmask(uint32_t gsi)
{
    const AcpiIoApic *a = ioapic_for_gsi(gsi);
    if (!a) return;
    uint32_t pin = gsi - a->gsi_base;
    uint32_t lo = io_r32(a->mmio_base, IOAPIC_REDTBL + 2 * pin);
    io_w32(a->mmio_base, IOAPIC_REDTBL + 2 * pin, lo & ~RTE_MASK);
}

void IOAPIC_Mask(uint32_t gsi)
{
    const AcpiIoApic *a = ioapic_for_gsi(gsi);
    if (!a) return;
    uint32_t pin = gsi - a->gsi_base;
    uint32_t lo = io_r32(a->mmio_base, IOAPIC_REDTBL + 2 * pin);
    io_w32(a->mmio_base, IOAPIC_REDTBL + 2 * pin, lo | RTE_MASK);
}

/* C:usbdiag — read one RTE.  IOREGSEL/IOWIN is an index pair shared
 * with the storm-mask path in ISR context, so read it with IRQs off. */
int IOAPIC_ReadRTE(uint32_t gsi, uint32_t *lo, uint32_t *hi)
{
    const AcpiIoApic *a = ioapic_for_gsi(gsi);
    if (!a) return -1;
    uint32_t pin = gsi - a->gsi_base;
    uint64_t fl = irq_save();
    *lo = io_r32(a->mmio_base, IOAPIC_REDTBL + 2 * pin);
    *hi = io_r32(a->mmio_base, IOAPIC_REDTBL + 2 * pin + 1);
    irq_restore(fl);
    return 0;
}

uint32_t LAPIC_Read(uint32_t off)
{
    return *(volatile uint32_t *)(uintptr_t)(ACPI_LapicBase() + off);
}

void LAPIC_EOI(void)
{
    *(volatile uint32_t *)(uintptr_t)(ACPI_LapicBase() + 0xB0) = 0;
}
