/*
 * dma.c — UAOS DMA Address Mapping Implementation
 *
 * UAOS uses identity mapping for the 4GB address space (0x00000000-0xFFFFFFFF),
 * so virtual addresses equal physical addresses for buffers in that range.
 * This simplifies DMA operations significantly.
 */

#include "dma.h"
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/* DMA-safe memory pool (in low memory, identity-mapped) */
#define DMA_POOL_SIZE    (2 * 1024 * 1024)  /* 2 MB DMA pool */
#define DMA_POOL_BASE    0x10000000          /* 256 MB physical base */

static uint8_t g_dma_pool[DMA_POOL_SIZE] __attribute__((aligned(4096)));
static size_t g_dma_pool_offset = 0;

/* =========================================================================
 * Virtual to Physical Address Translation
 * ========================================================================= */

uint64_t DMA_VirtToPhys(void *virt_addr)
{
    uint64_t virt = (uint64_t)(uintptr_t)virt_addr;
    
    /* UAOS uses identity mapping for the 4GB address space */
    if (virt < 0x100000000ULL) {
        return virt;  /* Identity mapping */
    }
    
    /* Addresses above 4GB are not identity-mapped */
    printf("[DMA] Warning: Address 0x%llx not in identity-mapped region\n", virt);
    return 0;
}

/* =========================================================================
 * DMA Accessibility Check
 * ========================================================================= */

int DMA_IsAccessible(void *virt_addr)
{
    uint64_t virt = (uint64_t)(uintptr_t)virt_addr;
    
    /* DMA is accessible if in identity-mapped region (0-4GB) */
    return (virt < 0x100000000ULL);
}

/* =========================================================================
 * DMA Memory Allocation
 * ========================================================================= */

void *DMA_Alloc(size_t size, size_t alignment)
{
    if (size == 0) return NULL;
    if (alignment == 0) alignment = 1;
    
    /* Align the offset */
    size_t aligned_offset = (g_dma_pool_offset + alignment - 1) & ~(alignment - 1);
    
    /* Check if we have enough space */
    if (aligned_offset + size > DMA_POOL_SIZE) {
        printf("[DMA] Out of DMA memory (requested: %zu, available: %zu)\n",
               size, DMA_POOL_SIZE - g_dma_pool_offset);
        return NULL;
    }
    
    /* Allocate from pool */
    void *ptr = (void *)(g_dma_pool + aligned_offset);
    g_dma_pool_offset = aligned_offset + size;
    
    printf("[DMA] Allocated %zu bytes at %p (phys: 0x%llx)\n",
           size, ptr, DMA_VirtToPhys(ptr));
    
    return ptr;
}

/* =========================================================================
 * DMA Memory Deallocation
 * ========================================================================= */

void DMA_Free(void *ptr, size_t size)
{
    (void)ptr; (void)size;
    /* For simplicity, we don't implement deallocation from the pool.
     * The pool is reset on reboot. For a more sophisticated system,
     * we would implement a proper allocator with free list tracking. */
    printf("[DMA] Free not implemented (pool-based allocator)\n");
}
