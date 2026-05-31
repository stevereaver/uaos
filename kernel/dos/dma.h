/*
 * dma.h — UAOS DMA Address Mapping
 *
 * Provides virtual-to-physical address translation for DMA operations.
 * UAOS uses identity mapping for the 4GB address space, so physical
 * addresses are the same as virtual addresses for buffers in that range.
 */

#ifndef UAOS_DMA_H
#define UAOS_DMA_H

#include <stdint.h>
#include <stddef.h>

/* Convert virtual address to physical address for DMA
 * Returns 0 if the address is not DMA-accessible */
uint64_t DMA_VirtToPhys(void *virt_addr);

/* Allocate DMA-safe memory (aligned, in low memory)
 * Returns NULL on failure */
void *DMA_Alloc(size_t size, size_t alignment);

/* Free DMA-allocated memory */
void DMA_Free(void *ptr, size_t size);

/* Check if an address is DMA-accessible (in identity-mapped region) */
int DMA_IsAccessible(void *virt_addr);

#endif /* UAOS_DMA_H */
