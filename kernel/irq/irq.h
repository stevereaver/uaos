/* irq.h — Unified interrupt routing layer
 *
 * Hides whether interrupts are delivered through the legacy 8259A PIC
 * (SeaBIOS/i440fx fallback), the IO-APIC (real hardware, q35), or MSI.
 * Drivers attach handlers by ISA IRQ or PCI BDF and get back the IDT
 * vector; EOI ownership stays centralised in ISR_Dispatch.
 *
 * IRQ_Init() must be called after IDT_Init()/PIC_Init()/APIC (LAPIC)
 * setup, and after UAOS_MMU_Init() (the 4 GB identity map must cover
 * the IO-APIC/LAPIC/ECAM MMIO windows).
 */

#ifndef UAOS_IRQ_H
#define UAOS_IRQ_H

#include <stdint.h>
#include "idt.h"

#define IRQ_MODE_PIC    0
#define IRQ_MODE_IOAPIC 1

/* Critical-section helpers — the save/restore IF pair every short
 * atomic region should use.  A bare cli … sti pair silently re-enables
 * interrupts when the caller already had IF=0 (inside Disable(),
 * another irq_save() region, or an outer critical section), breaking
 * its atomicity; restoring the caller's RFLAGS preserves whatever IF
 * it entered with.  Safe from task and IRQ context. */
static inline uint64_t irq_save(void)
{
    uint64_t f;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    return f;
}

static inline void irq_restore(uint64_t f)
{
    __asm__ volatile("pushq %0; popfq" :: "r"(f) : "memory", "cc");
}

/* mb2_phys is the Multiboot2 info physical address. */
void IRQ_Init(uint32_t mb2_phys);

int  IRQ_Mode(void);            /* IRQ_MODE_PIC or IRQ_MODE_IOAPIC */

/* Attach a handler to an ISA IRQ (0-15).  In IO-APIC mode the source
 * override table is applied (e.g. IRQ0 -> GSI2).  Returns the IDT
 * vector, or -1 on failure. */
int  IRQ_AttachISA(int isa_irq, ISRHandler handler, const char *name);

/* Force-attach on the legacy 8259 PIC regardless of routing mode —
 * survival fallback when IO-APIC delivery is confirmed dead. */
int  IRQ_AttachISAPIC(int isa_irq, ISRHandler handler, const char *name);

/* Called when the boot-time PIT probe proves the IO-APIC is not
 * delivering; subsequent ISA/PCI attaches use the 8259 paths so the
 * machine stays functional instead of hanging on a dead APIC path. */
void IRQ_PicFallback(void);
int  IRQ_PicFallbackActive(void);

/* Attach a handler to a PCI function's INTx pin.  The GSI is resolved
 * from chipset routing registers (ICH8/ICH9 DxxIP/DxxIR + PIRQx_ROUT)
 * with the PCI interrupt-line register as fallback.  Returns the IDT
 * vector, or -1 if the line cannot be resolved. */
int  IRQ_AttachPCI(uint8_t bus, uint8_t dev, uint8_t fn,
                   ISRHandler handler, const char *name);

/* Enable MSI on a PCI function and attach a handler to a fresh vector
 * allocated from the MSI pool.  Returns the IDT vector, or -1 if the
 * device has no MSI capability. */
int  IRQ_AttachMSI(uint8_t bus, uint8_t dev, uint8_t fn,
                   ISRHandler handler, const char *name);

/* End-of-interrupt for a vector — called by ISR_Dispatch and by
 * handlers that historically sent their own PIC EOI.  Safe to call
 * on any vector (no-op for non-IRQ vectors). */
void IRQ_EOI(int vector);

/* Mask/unmask by GSI (IO-APIC mode) or legacy IRQ (PIC mode). */
void IRQ_Mask(int gsi);
void IRQ_Unmask(int gsi);

/* Per-tick interrupt-storm failsafe — call from the PIT handler.
 * Compares ISR_Dispatch counters against the previous tick and masks
 * any APIC vector whose rate exceeds the storm threshold. */
void IRQ_StormTick(void);

#endif
