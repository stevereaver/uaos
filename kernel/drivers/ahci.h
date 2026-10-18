/* ahci.h — UAOS AHCI (SATA) host controller driver
 *
 * Targets the Intel ICH8M AHCI controller (00:1f.2, 8086:2829) on the
 * MacBookPro4,1 and any generic PCI class 01/06/01 AHCI HBA (QEMU).
 */

#ifndef UAOS_AHCI_H
#define UAOS_AHCI_H

/* Probe PCI bus, initialise the HBA, enumerate SATA disks and register
 * them as BlockDevs.  Returns number of disks found, or -1 if no HBA. */
int AHCI_Init(void);

/* Attach the HBA interrupt (MSI preferred, INTx fallback).
 * Must be called after IRQ_Init/IDT_Init. */
void AHCI_SetupIRQ(void);

struct UaosTask;
/* Called from Task_Exit before the dying task's memory is freed:
 * drains (or aborts) an in-flight slot-0 command the task owned so the
 * HBA can't DMA into freed memory (UAOS-318).  Cheap no-op when the
 * task isn't the current command owner. */
void AHCI_TaskDying(struct UaosTask *t);

#endif
