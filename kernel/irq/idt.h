/* idt.h — UAOS x86_64 IDT and PIC interface */

#ifndef UAOS_IDT_H
#define UAOS_IDT_H

#include <stdint.h>

/* Initialise the 256-entry IDT and load it with lidt */
void IDT_Init(void);

/* Remap 8259A PIC so IRQ0-15 map to vectors 32-47, then mask all */
void PIC_Init(void);

/* Unmask a specific IRQ line (0-15) */
void PIC_UnmaskIRQ(int irq);

/* Mask a specific IRQ line (0-15) */
void PIC_MaskIRQ(int irq);

/* Send End-Of-Interrupt to master (and slave if irq >= 8) */
void PIC_SendEOI(int irq);

/* Install a C handler for a given vector */
typedef void (*ISRHandler)(uint64_t vector, uint64_t error_code);
void IDT_SetHandler(uint8_t vector, ISRHandler handler);

/* Install a raw assembly entry point directly into the IDT.
 * Used for vector 0x80 so the syscall path can pass the full
 * interrupt frame to Syscall_Dispatch without going through ISR_Dispatch. */
void IDT_SetRawHandler(uint8_t vector, void (*handler)(void));

/* C dispatch entry (called from idt_stubs.asm isr_common) */
void ISR_Dispatch(uint64_t vector, uint64_t error_code);

#endif
