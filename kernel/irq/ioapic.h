/* ioapic.h — 82093AA IO-APIC driver
 *
 * Programs redirection table entries so GSIs are delivered to the BSP's
 * local APIC.  ISA IRQs route edge/high; PCI INTx routes level/low.
 */

#ifndef UAOS_IOAPIC_H
#define UAOS_IOAPIC_H

#include <stdint.h>

/* RTE flags */
#define IOAPIC_RTE_LEVEL    (1u << 0)   /* level-triggered (default edge)   */
#define IOAPIC_RTE_LOW      (1u << 1)   /* active-low       (default high)  */

int  IOAPIC_Available(void);
void IOAPIC_Init(void);

/* Program the RTE for a GSI (masked).  Vector = destination IDT vector. */
void IOAPIC_ProgramGSI(uint32_t gsi, uint8_t vector, uint32_t flags);

void IOAPIC_Unmask(uint32_t gsi);
void IOAPIC_Mask(uint32_t gsi);

/* Local APIC end-of-interrupt (IO-APIC and MSI interrupts). */
void LAPIC_EOI(void);

/* C:usbdiag diagnostics (read-only) */
#define RTE_BIT_DELIVS   (1u << 12)   /* delivery status: send pending */
#define RTE_BIT_RIRR     (1u << 14)   /* remote IRR: level IRQ awaiting EOI */
int      IOAPIC_ReadRTE(uint32_t gsi, uint32_t *lo, uint32_t *hi);
uint32_t LAPIC_Read(uint32_t off);

#endif
