/*
 * mtrr.h — MTRR helpers
 *
 * Used to mark the linear framebuffer write-combining on real hardware:
 * firmware leaves GPU BAR regions uncached, which makes per-pixel
 * framebuffer writes brutally slow (visible scanline update).
 */

#ifndef UAOS_MTRR_H
#define UAOS_MTRR_H

#include <stdint.h>

/* Mark [base, base+size) write-combining using a spare variable MTRR.
 * Rounds the range out to a power-of-two-aligned covering region (VRAM
 * apertures are large aligned windows, so neighbours inside the covered
 * padding are still framebuffer).  Returns 0 on success, -1 if MTRRs or
 * WC are unsupported / no slot is free. */
int MTRR_MarkWC(uint64_t base, uint64_t size);

#endif /* UAOS_MTRR_H */
