/* acpi.h — Minimal ACPI table discovery for interrupt routing
 *
 * Locates the RSDP (via Multiboot2 tags or a BIOS-area scan), walks the
 * XSDT/RSDT, and parses the MADT (IO-APICs, interrupt source overrides)
 * and MCFG (PCIe ECAM base).  This is not a general AML interpreter —
 * just the static tables needed to route interrupts on real hardware.
 */

#ifndef UAOS_ACPI_H
#define UAOS_ACPI_H

#include <stdint.h>

#define ACPI_MAX_IOAPICS 4
#define ACPI_MAX_ISOS    16

typedef struct {
    uint8_t  id;
    uint32_t mmio_base;
    uint32_t gsi_base;
} AcpiIoApic;

typedef struct {
    uint8_t  bus;        /* 0 = ISA */
    uint8_t  source;     /* ISA IRQ number */
    uint32_t gsi;
    uint16_t flags;      /* bits 0-1 polarity, 2-3 trigger */
} AcpiIso;

/* Discover and parse ACPI tables.  mb2_phys is the Multiboot2 info
 * structure physical address (0 if unavailable). */
void ACPI_Init(uint32_t mb2_phys);

/* Nonzero if a valid RSDP + MADT were found. */
int ACPI_Present(void);

/* Look up a top-level table by 4-char signature ("APIC", "MCFG", ...).
 * Returns a pointer into the (identity-mapped) table or NULL. */
const void *ACPI_FindTable(const char sig[4]);

/* Local APIC physical base (from MADT, usually 0xFEE00000). */
uint32_t ACPI_LapicBase(void);

/* IO-APICs described by the MADT. */
int                 ACPI_NumIoApics(void);
const AcpiIoApic   *ACPI_IoApic(int idx);

/* Interrupt Source Overrides (ISA IRQ -> GSI remaps). */
int              ACPI_NumIsos(void);
const AcpiIso   *ACPI_Iso(int idx);

/* Resolve an ISA IRQ to a GSI, applying source overrides.
 * flags_out (may be NULL) receives the ISO flags word. */
int ACPI_IsaToGsi(int isa_irq, uint16_t *flags_out);

/* PCIe ECAM (enhanced config) base from MCFG, 0 if absent. */
uint64_t ACPI_EcamBase(void);

/* MADT flags bit0 — system has a dual-8259 PIC (PC-AT compatible), so an
 * IMCR register (ports 0x22/0x23) may gate interrupt routing. */
int ACPI_PcatCompat(void);

#endif
