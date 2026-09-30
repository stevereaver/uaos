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

/* mb2_phys is the Multiboot2 info physical address. */
void IRQ_Init(uint32_t mb2_phys);

int  IRQ_Mode(void);            /* IRQ_MODE_PIC or IRQ_MODE_IOAPIC */

/* Attach a handler to an ISA IRQ (0-15).  In IO-APIC mode the source
 * override table is applied (e.g. IRQ0 -> GSI2).  Returns the IDT
 * vector, or -1 on failure. */
int  IRQ_AttachISA(int isa_irq, ISRHandler handler, const char *name);

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

#endif
