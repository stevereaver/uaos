/* acpi_prt.h — ACPI _PRT routing-table resolver (UAOS-292)
 *
 * Byte-scans the DSDT/SSDTs for PCI _PRT objects (no AML interpreter —
 * same style as the cpufreq _PSS scanner).  Builds (bus, dev, pin)->GSI
 * sets keyed by the bus a _PRT owner routes, discovers PCI-PCI bridges
 * for the pin-swizzle walk, and resolves an INTx pin to a GSI for
 * devices behind bridges — the MBP4,1 root-port case.
 */

#ifndef UAOS_ACPI_PRT_H
#define UAOS_ACPI_PRT_H

#include <stdint.h>

/* Parse all ACPI tables and build the routing sets.  Call once after
 * ACPI_Init(); PCI config access must be usable (legacy ports 0xCF8). */
void AcpiPrt_Init(void);

/* Resolve a function's INTx pin (1-4) to a GSI through the _PRT sets,
 * swizzling up through bridges whose secondary bus is `bus` when no
 * set covers the bus directly.  *via_swizzle (may be NULL) is set
 * nonzero when bridge traversal occurred.  Returns the GSI or -1. */
int  AcpiPrt_Route(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t pin,
                   int *via_swizzle);

/* Bridge whose secondary bus was traversed by the most recent
 * AcpiPrt_Route() (for diagnostics).  Returns 0 if a bridge was used. */
int  AcpiPrt_LastBridge(uint8_t *bus, uint8_t *dev, uint8_t *fn);

/* Dump accessors for C:irqroute PRT. */
int  AcpiPrt_NumSets(void);
int  AcpiPrt_SetInfo(int set, uint8_t *bus, int *has_bridge,
                     uint8_t *bbus, uint8_t *bdev, uint8_t *bfn);
int  AcpiPrt_SetNEntries(int set);
int  AcpiPrt_SetEntry(int set, int idx,
                      uint8_t *dev, uint8_t *pin, int *gsi);

#endif
