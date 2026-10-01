/*
 * dma.c — UAOS DMA Address Mapping Implementation
 *
 * UAOS uses identity mapping for the 4GB address space (0x00000000-0xFFFFFFFF),
 * so virtual addresses equal physical addresses for buffers in that range.
 * This simplifies DMA operations significantly.
 */

#include "dma.h"
#include "../klog/klog.h"
#include "../irq/irq.h"
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/* DMA-safe memory pool (in low memory, identity-mapped) */
#define DMA_POOL_SIZE    (2 * 1024 * 1024)  /* 2 MB DMA pool */

static uint8_t g_dma_pool[DMA_POOL_SIZE] __attribute__((aligned(4096)));

/* -------------------------------------------------------------------------
 * Pool allocator (UAOS-173)
 *
 * The pool used to be a never-freed bump allocator: every UHCI control
 * transfer leaked its setup packet + QH + TD array, so runtime control
 * transfers (wellspring mode switches, SET_REPORT, re-enumeration)
 * drained the pool until DMA_Alloc failed for USB, sky2, and AHCI.
 *
 * This is a K&R-style first-fit free list over the pool:
 *   - each block starts with a 16-byte header { size, link }
 *   - free blocks are threaded through link in address order
 *   - a used block stores DMA_USED_MAGIC in link, and the 8 bytes right
 *     before the aligned payload hold the offset back to the block start
 *     (so DMA_Free needs only the pointer, not the size)
 *   - free-list insert coalesces with physically adjacent neighbours
 * ------------------------------------------------------------------------- */
typedef struct DmaBlk {
    uint64_t size;      /* total bytes, header included */
    uint64_t link;      /* free: next block addr | used: DMA_USED_MAGIC */
} DmaBlk;

/* Header reservation before the payload: 16 bytes for DmaBlk itself
 * plus 8 more so the payload-minus-8 back-offset never overlaps the
 * header's link field even at alignment 16 (payload-base >= 24). */
#define DMA_HDR         24
#define DMA_USED_MAGIC  0xD44AB10C6A110C6DULL
#define DMA_MIN_SPLIT   (DMA_HDR + 32)      /* smallest leftover worth a block */

static DmaBlk *g_free  = NULL;              /* address-sorted free list head */
static uint64_t g_used_bytes = 0;           /* payload bytes in used blocks */

static void dma_pool_init(void)
{
    g_free = (DmaBlk *)g_dma_pool;
    g_free->size = DMA_POOL_SIZE;
    g_free->link = 0;
    g_used_bytes = 0;
}

static inline uint64_t dma_align_up(uint64_t v, uint64_t a)
{
    return (v + a - 1) & ~(a - 1);
}

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
    /* All callers use power-of-two alignments; round up defensively so the
     * mask arithmetic stays valid if a non-pow2 value ever sneaks in. */
    alignment--;
    alignment |= alignment >> 1;
    alignment |= alignment >> 2;
    alignment |= alignment >> 4;
    alignment |= alignment >> 8;
    alignment++;

    uint64_t fl = irq_save();
    if (!g_free) dma_pool_init();

    DmaBlk *prev = NULL, *blk = g_free;
    while (blk) {
        uint64_t base    = (uint64_t)(uintptr_t)blk;
        uint64_t payload = dma_align_up(base + DMA_HDR, alignment);
        uint64_t need    = (payload - base) + size;   /* span incl. align pad */
        if (blk->size >= need) {
            uint64_t leftover = blk->size - need;
            if (leftover >= DMA_MIN_SPLIT) {
                DmaBlk *nb = (DmaBlk *)(uintptr_t)(base + need);
                nb->size = leftover;
                nb->link = blk->link;
                blk->size = need;
                if (prev) prev->link = (uint64_t)(uintptr_t)nb;
                else      g_free = nb;
            } else {
                /* Consume the whole block — slack stays inside it. */
                if (prev) prev->link = blk->link;
                else      g_free = (DmaBlk *)(uintptr_t)blk->link;
            }
            blk->link = DMA_USED_MAGIC;
            *(uint64_t *)(uintptr_t)(payload - 8) = payload - base;
            g_used_bytes += blk->size;
            irq_restore(fl);
            KLOG(KLOG_KERN, KLOG_TRACE, "[DMA] Alloc %zu @ %p\n", size,
                 (void *)(uintptr_t)payload);
            return (void *)(uintptr_t)payload;
        }
        prev = blk;
        blk = (DmaBlk *)(uintptr_t)blk->link;
    }

    irq_restore(fl);
    KLOG(KLOG_KERN, KLOG_WARN,
         "[DMA] Out of DMA memory (requested: %zu)", size);
    return NULL;
}

/* =========================================================================
 * DMA Memory Deallocation
 * ========================================================================= */

void DMA_Free(void *ptr, size_t size)
{
    (void)size;
    if (!ptr) return;

    /* Recover the block via the back-offset stored at payload-8. */
    uint64_t payload = (uint64_t)(uintptr_t)ptr;
    if (payload < (uint64_t)(uintptr_t)g_dma_pool + DMA_HDR ||
        payload >= (uint64_t)(uintptr_t)g_dma_pool + DMA_POOL_SIZE) {
        KLOG(KLOG_KERN, KLOG_WARN,
             "[DMA] Free of out-of-pool ptr %p\n", ptr);
        return;
    }
    uint64_t back    = *(uint64_t *)(uintptr_t)(payload - 8);
    DmaBlk  *blk     = (DmaBlk *)(uintptr_t)(payload - back);
    if (back < DMA_HDR || (uint8_t *)blk < g_dma_pool) {
        KLOG(KLOG_KERN, KLOG_WARN,
             "[DMA] Free of corrupt ptr %p\n", ptr);
        return;
    }

    uint64_t fl = irq_save();
    if (blk->link != DMA_USED_MAGIC) {
        irq_restore(fl);
        KLOG(KLOG_KERN, KLOG_WARN,
             "[DMA] Free of non-DMA or double-free ptr %p\n", ptr);
        return;
    }

    /* Insert sorted by address so physical neighbours can merge. */
    DmaBlk *prev = NULL, *cur = g_free;
    while (cur && (uint64_t)(uintptr_t)cur < (uint64_t)(uintptr_t)blk) {
        prev = cur;
        cur  = (DmaBlk *)(uintptr_t)cur->link;
    }
    blk->link = (uint64_t)(uintptr_t)cur;
    if (prev) prev->link = (uint64_t)(uintptr_t)blk;
    else      g_free = blk;
    if (g_used_bytes >= blk->size) g_used_bytes -= blk->size;

    /* Coalesce with the following block. */
    if (cur && (uint64_t)(uintptr_t)blk + blk->size ==
               (uint64_t)(uintptr_t)cur) {
        blk->size += cur->size;
        blk->link  = cur->link;
    }
    /* Coalesce with the preceding block. */
    if (prev && (uint64_t)(uintptr_t)prev + prev->size ==
                (uint64_t)(uintptr_t)blk) {
        prev->size += blk->size;
        prev->link  = blk->link;
    }
    irq_restore(fl);
}

/* Pool usage for the mem command (UAOS-173): used bytes are whole block
 * spans (header + alignment pad + payload); free is the remainder. */
void DMA_Usage(uint64_t *used, uint64_t *freeb)
{
    uint64_t fl = irq_save();
    uint64_t f = 0;
    for (DmaBlk *b = g_free; b; b = (DmaBlk *)(uintptr_t)b->link)
        f += b->size;
    if (used)  *used  = g_used_bytes;
    if (freeb) *freeb = f;
    irq_restore(fl);
}
