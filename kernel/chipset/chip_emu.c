/*
 * chip_emu.c — UAOS AGA/ECS custom chip emulator
 *
 * Provides a sparse register dispatch table for the classic Amiga custom
 * chip register area at guest physical address 0x00DFF000.  Accesses arrive
 * from the x86_64 page fault handler as offsets relative to the chip window
 * base (0x00B00000).
 *
 * Tier 1 implementation: all reads return the last written value (or zero on
 * cold boot) and all writes are swallowed without side effects.  This lets
 * boot/ROM code poke registers without crashing while the real behaviour is
 * filled in later tiers.
 */

#include "chipset/chip_emu.h"
#include <stdint.h>

/* -----------------------------------------------------------------------
 * Register window geometry
 *
 * The page fault handler forwards offsets relative to CHIP_WINDOW_START
 * (0x00B00000).  The classic Amiga custom chip register block lives at
 * 0x00DFF000 and spans 0x200 bytes (256 16-bit registers).  AGA extends
 * some registers into the 0xDFF200-0xDFFFF0 region; for now we reserve a
 * 1024-entry (2 KB) sparse table so the upper AGA banks can be wired in
 * without growing the array later.
 * ----------------------------------------------------------------------- */

#define AGA_REG_BASE_ABS   0x00DFF000ULL
#define CHIP_WINDOW_START  0x00B00000ULL
#define AGA_REG_BASE_OFF   (AGA_REG_BASE_ABS - CHIP_WINDOW_START) /* 0x2FF000 */
#define AGA_REG_SIZE       1024u  /* 16-bit entries, covers 2 KB */
#define AGA_REG_MASK       (AGA_REG_SIZE - 1u)

/* 16-bit register backing store.  Even addresses index the entry directly;
 * odd addresses are treated as the high byte of the same entry (Amiga
 * custom registers are 16-bit and ignore byte lane on odd reads). */
static uint16_t g_aga_regs[AGA_REG_SIZE];

/* Convert a chip-window offset to a register index, or AGA_REG_SIZE if the
 * offset does not fall inside the AGA register area. */
static uint32_t offset_to_index(uint32_t offset)
{
    if (offset < AGA_REG_BASE_OFF)
        return AGA_REG_SIZE;

    uint32_t rel = offset - AGA_REG_BASE_OFF;
    if (rel >= (AGA_REG_SIZE * 2u))
        return AGA_REG_SIZE;

    return (rel >> 1) & AGA_REG_MASK;
}

void chip_emu_write(uint32_t offset, uint32_t value, int width_bytes)
{
    uint32_t idx = offset_to_index(offset);
    if (idx >= AGA_REG_SIZE)
        return; /* outside AGA register area: swallow */

    /* Tier 1: no side effects.  Just mirror the value into the register. */
    switch (width_bytes) {
        case 1:
            /* Odd byte writes update the high byte of the 16-bit register. */
            if (offset & 1u)
                g_aga_regs[idx] = (uint16_t)((g_aga_regs[idx] & 0x00FFu) | ((value & 0xFFu) << 8));
            else
                g_aga_regs[idx] = (uint16_t)((g_aga_regs[idx] & 0xFF00u) | (value & 0xFFu));
            break;

        case 2:
            g_aga_regs[idx] = (uint16_t)(value & 0xFFFFu);
            break;

        case 4:
        default:
            /* 32-bit write fills two consecutive 16-bit registers, big-endian. */
            g_aga_regs[idx] = (uint16_t)((value >> 16) & 0xFFFFu);
            g_aga_regs[(idx + 1u) & AGA_REG_MASK] = (uint16_t)(value & 0xFFFFu);
            break;
    }
}

uint32_t chip_emu_read(uint32_t offset, int width_bytes)
{
    uint32_t idx = offset_to_index(offset);
    if (idx >= AGA_REG_SIZE)
        return 0; /* outside AGA register area: harmless zero */

    uint16_t v = g_aga_regs[idx];

    switch (width_bytes) {
        case 1:
            /* Odd byte reads return the high byte. */
            if (offset & 1u)
                return (uint32_t)(v >> 8);
            return (uint32_t)(v & 0xFFu);

        case 2:
            return (uint32_t)v;

        case 4:
        default:
            /* 32-bit read from two consecutive 16-bit registers, big-endian. */
            {
                uint16_t v2 = g_aga_regs[(idx + 1u) & AGA_REG_MASK];
                return ((uint32_t)v << 16) | (uint32_t)v2;
            }
    }
}
