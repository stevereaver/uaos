/*
 * chip_emu.c — UAOS AGA/ECS custom chip emulator
 *
 * Provides a sparse register dispatch table for the classic Amiga custom
 * chip register area at guest physical address 0x00DFF000.  Accesses arrive
 * from the x86_64 page fault handler as offsets relative to the chip window
 * base (0x00B00000).
 *
 * Tier 2 implementation: basic chipset state machines for the control
 * registers most Amiga code touches first, plus a 256-entry AGA color
 * palette and storage for DMA pointer registers.  No real DMA rendering yet.
 */

#include "chipset/chip_emu.h"
#include "display/framebuffer.h"
#include "uaos_emu.h"
#include <stdint.h>
#include <stddef.h>

/* -----------------------------------------------------------------------
 * Register window geometry
 *
 * The page fault handler forwards offsets relative to CHIP_WINDOW_START
 * (0x00B00000).  The classic Amiga custom chip register block lives at
 * 0x00DFF000 and spans 0x1000 bytes (0x800 16-bit registers).  AGA extends
 * the chipset into the 0xDFF200-0xDFFFF0 region, so we reserve a 2048-entry
 * sparse table to cover the full 4 KB window.
 * ----------------------------------------------------------------------- */

#define AGA_REG_BASE_ABS   0x00DFF000ULL
#define CHIP_WINDOW_START  0x00B00000ULL
#define AGA_REG_BASE_OFF   (AGA_REG_BASE_ABS - CHIP_WINDOW_START) /* 0x2FF000 */
#define AGA_REG_SIZE       2048u  /* 16-bit entries, covers 4 KB */
#define AGA_REG_MASK       (AGA_REG_SIZE - 1u)

/* 16-bit register backing store for registers without special state. */
static uint16_t g_aga_regs[AGA_REG_SIZE];

/* Convert a chip-window offset to a register offset (0..0xFFF) inside the
 * AGA register area, or 0xFFFFFFFF if it is outside. */
static uint32_t offset_to_regoff(uint32_t offset)
{
    if (offset < AGA_REG_BASE_OFF)
        return 0xFFFFFFFFu;

    uint32_t rel = offset - AGA_REG_BASE_OFF;
    if (rel >= 0x1000u)
        return 0xFFFFFFFFu;

    return rel;
}

static uint32_t regoff_to_index(uint32_t regoff)
{
    return (regoff >> 1) & AGA_REG_MASK;
}

/* -----------------------------------------------------------------------
 * Tier 2 control register state machines
 * ----------------------------------------------------------------------- */

#define REG_DMACON   0x096
#define REG_INTENA   0x09A
#define REG_INTREQ   0x09C
#define REG_ADKCON   0x09E

#define REG_VPOSR    0x004
#define REG_VHPOSR   0x006

#define REG_COP1LC   0x080
#define REG_COP2LC   0x084
#define REG_COPJMP1  0x088
#define REG_COPJMP2  0x08A

#define REG_BPLCON0  0x100
#define REG_BPLCON1  0x102
#define REG_BPLCON2  0x104
#define REG_BPLCON3  0x106
#define REG_BPLCON4  0x10C

#define REG_BPL1PT   0x0E0
#define REG_BPL2PT   0x0E4
#define REG_BPL3PT   0x0E8
#define REG_BPL4PT   0x0EC
#define REG_BPL5PT   0x0F0
#define REG_BPL6PT   0x0F4
#define REG_BPL7PT   0x0F8
#define REG_BPL8PT   0x0FC

#define REG_BPLMOD1  0x108
#define REG_BPLMOD2  0x10A

#define REG_DIWSTART 0x08E
#define REG_DIWSTOP  0x090
#define REG_DDFSTART 0x092
#define REG_DDFSTOP  0x094
#define REG_COPCON   0x02E

#define REG_SPR0PT   0x120
#define REG_SPR1PT   0x124
#define REG_SPR2PT   0x128
#define REG_SPR3PT   0x12C
#define REG_SPR4PT   0x130
#define REG_SPR5PT   0x134
#define REG_SPR6PT   0x138
#define REG_SPR7PT   0x13C

#define REG_COLOR00  0x180

/* SET/CLR bit used by DMACON, INTENA, INTREQ, ADKCON */
#define SETCLR_BIT   0x8000u

static uint16_t g_dmacon;   /* DMA control */
static uint16_t g_intena;   /* interrupt enable */
static uint16_t g_intreq;   /* interrupt request */
static uint16_t g_adkcon;   /* audio/disk control */

static uint32_t g_cop1lc;   /* copper list 1 pointer */
static uint32_t g_cop2lc;   /* copper list 2 pointer */
static uint8_t  g_copjmp1;  /* copper jump 1 strobe */
static uint8_t  g_copjmp2;  /* copper jump 2 strobe */

static uint16_t g_bplcon0;  /* bitplane/control register: planes, HAM, EHB, genlock */
static uint16_t g_bplcon1;  /* horizontal scroll / modulos */
static uint16_t g_bplcon2;  /* playfield priorities / genlock */
static uint16_t g_bplcon3;  /* AGA bank/LOCT / sprite resolution */
static uint16_t g_bplcon4;  /* AGA color bank lower bits / sprite bank */

static uint16_t g_bplmod1;  /* bitplane modulo (odd planes) */
static uint16_t g_bplmod2;  /* bitplane modulo (even planes) */

static uint16_t g_diwstart; /* display window start */
static uint16_t g_diwstop;  /* display window stop */
static uint16_t g_ddfstart; /* display data fetch start */
static uint16_t g_ddfstop;  /* display data fetch stop */
static uint16_t g_copcon;   /* copper control (dangerous bits) */

/* Basic DMA pointer registers — stored but not yet rendered. */
static uint32_t g_bpl_pt[8];
static uint32_t g_spr_pt[8];

/* AGA 256-entry color palette (host 0x00RRGGBB format). */
#define AGA_PALETTE_SIZE 256
static uint32_t g_aga_palette[AGA_PALETTE_SIZE];

/* Update a 16-bit "SET/CLR" control register. */
static uint16_t update_setclr(uint16_t state, uint16_t value)
{
    if (value & SETCLR_BIT)
        return (uint16_t)(state | (value & 0x7FFFu));
    return (uint16_t)(state & ~(value & 0x7FFFu));
}

/* -----------------------------------------------------------------------
 * AGA color palette handling
 *
 * AGA exposes 256 24-bit colors through the 32 classic COLOR00-COLOR31
 * addresses.  The 32-entry bank is selected by bits 13-15 of BPLCON3.
 * Within a bank, LOCT (bit 9 of BPLCON3) selects whether the write updates
 * the 4 high nibbles (LOCT=0) or the 4 low nibbles (LOCT=1) of the RGB
 * components.
 * ----------------------------------------------------------------------- */

static void aga_color_write(uint32_t color_index, uint16_t value)
{
    uint32_t color = g_aga_palette[color_index];

    if (g_bplcon3 & 0x0200u) { /* LOCT=1: update low nibbles */
        uint32_t r0 = (value >> 8) & 0x0Fu;
        uint32_t g0 = (value >> 4) & 0x0Fu;
        uint32_t b0 = value & 0x0Fu;
        uint32_t r = (color >> 16) & 0xFFu;
        uint32_t g = (color >> 8)  & 0xFFu;
        uint32_t b = color & 0xFFu;
        r = (r & 0xF0u) | r0;
        g = (g & 0xF0u) | g0;
        b = (b & 0xF0u) | b0;
        color = (r << 16) | (g << 8) | b;
    } else { /* LOCT=0: update high nibbles, duplicate them into low nibbles */
        uint32_t r4 = (value >> 8) & 0x0Fu;
        uint32_t g4 = (value >> 4) & 0x0Fu;
        uint32_t b4 = value & 0x0Fu;
        uint32_t r = (r4 << 4) | r4;
        uint32_t g = (g4 << 4) | g4;
        uint32_t b = (b4 << 4) | b4;
        color = (r << 16) | (g << 8) | b;
    }

    g_aga_palette[color_index] = color;
}

/* -----------------------------------------------------------------------
 * Public chip emulator entry points
 * ----------------------------------------------------------------------- */

void chip_emu_write(uint32_t offset, uint32_t value, int width_bytes)
{
    uint32_t regoff = offset_to_regoff(offset);
    if (regoff >= 0x1000u)
        return; /* outside AGA register area: swallow */

    /* Handle byte and half-word writes to the register backing store first.
     * Special registers below may override the simple mirroring. */
    if (width_bytes == 1) {
        uint32_t idx = regoff_to_index(regoff);
        if (offset & 1u)
            g_aga_regs[idx] = (uint16_t)((g_aga_regs[idx] & 0x00FFu) | ((value & 0xFFu) << 8));
        else
            g_aga_regs[idx] = (uint16_t)((g_aga_regs[idx] & 0xFF00u) | (value & 0xFFu));
    } else if (width_bytes == 2) {
        g_aga_regs[regoff_to_index(regoff)] = (uint16_t)(value & 0xFFFFu);
    } else {
        /* 32-bit write fills two consecutive 16-bit registers, big-endian. */
        uint32_t idx = regoff_to_index(regoff);
        g_aga_regs[idx]     = (uint16_t)((value >> 16) & 0xFFFFu);
        g_aga_regs[idx + 1] = (uint16_t)(value & 0xFFFFu);
    }

    /* Special register handling. */
    switch (regoff) {
        case REG_DMACON: g_dmacon = update_setclr(g_dmacon, (uint16_t)value); break;
        case REG_INTENA: g_intena = update_setclr(g_intena, (uint16_t)value); break;
        case REG_INTREQ: g_intreq = update_setclr(g_intreq, (uint16_t)value); break;
        case REG_ADKCON: g_adkcon = update_setclr(g_adkcon, (uint16_t)value); break;

        case REG_COP1LC:     g_cop1lc = (width_bytes >= 4) ? value : ((g_cop1lc & 0x0000FFFFu) | ((value & 0xFFFFu) << 16)); break;
        case REG_COP1LC + 2: g_cop1lc = (g_cop1lc & 0xFFFF0000u) | (value & 0xFFFFu); break;
        case REG_COP2LC:     g_cop2lc = (width_bytes >= 4) ? value : ((g_cop2lc & 0x0000FFFFu) | ((value & 0xFFFFu) << 16)); break;
        case REG_COP2LC + 2: g_cop2lc = (g_cop2lc & 0xFFFF0000u) | (value & 0xFFFFu); break;
        case REG_COPJMP1: g_copjmp1 = 1; break;
        case REG_COPJMP2: g_copjmp2 = 1; break;

        case REG_BPLCON0: g_bplcon0 = (uint16_t)value; break;
        case REG_BPLCON1: g_bplcon1 = (uint16_t)value; break;
        case REG_BPLCON2: g_bplcon2 = (uint16_t)value; break;
        case REG_BPLCON3: g_bplcon3 = (uint16_t)value; break;
        case REG_BPLCON4: g_bplcon4 = (uint16_t)value; break;

        case REG_BPLMOD1: g_bplmod1 = (uint16_t)value; break;
        case REG_BPLMOD2: g_bplmod2 = (uint16_t)value; break;

        case REG_DIWSTART: g_diwstart = (uint16_t)value; break;
        case REG_DIWSTOP:  g_diwstop  = (uint16_t)value; break;
        case REG_DDFSTART: g_ddfstart = (uint16_t)value; break;
        case REG_DDFSTOP:  g_ddfstop  = (uint16_t)value; break;
        case REG_COPCON:   g_copcon   = (uint16_t)value; break;

        case REG_BPL1PT:     g_bpl_pt[0] = (width_bytes >= 4) ? value : ((g_bpl_pt[0] & 0x0000FFFFu) | ((value & 0xFFFFu) << 16)); break;
        case REG_BPL1PT + 2: g_bpl_pt[0] = (g_bpl_pt[0] & 0xFFFF0000u) | (value & 0xFFFFu); break;
        case REG_BPL2PT:     g_bpl_pt[1] = (width_bytes >= 4) ? value : ((g_bpl_pt[1] & 0x0000FFFFu) | ((value & 0xFFFFu) << 16)); break;
        case REG_BPL2PT + 2: g_bpl_pt[1] = (g_bpl_pt[1] & 0xFFFF0000u) | (value & 0xFFFFu); break;
        case REG_BPL3PT:     g_bpl_pt[2] = (width_bytes >= 4) ? value : ((g_bpl_pt[2] & 0x0000FFFFu) | ((value & 0xFFFFu) << 16)); break;
        case REG_BPL3PT + 2: g_bpl_pt[2] = (g_bpl_pt[2] & 0xFFFF0000u) | (value & 0xFFFFu); break;
        case REG_BPL4PT:     g_bpl_pt[3] = (width_bytes >= 4) ? value : ((g_bpl_pt[3] & 0x0000FFFFu) | ((value & 0xFFFFu) << 16)); break;
        case REG_BPL4PT + 2: g_bpl_pt[3] = (g_bpl_pt[3] & 0xFFFF0000u) | (value & 0xFFFFu); break;
        case REG_BPL5PT:     g_bpl_pt[4] = (width_bytes >= 4) ? value : ((g_bpl_pt[4] & 0x0000FFFFu) | ((value & 0xFFFFu) << 16)); break;
        case REG_BPL5PT + 2: g_bpl_pt[4] = (g_bpl_pt[4] & 0xFFFF0000u) | (value & 0xFFFFu); break;
        case REG_BPL6PT:     g_bpl_pt[5] = (width_bytes >= 4) ? value : ((g_bpl_pt[5] & 0x0000FFFFu) | ((value & 0xFFFFu) << 16)); break;
        case REG_BPL6PT + 2: g_bpl_pt[5] = (g_bpl_pt[5] & 0xFFFF0000u) | (value & 0xFFFFu); break;
        case REG_BPL7PT:     g_bpl_pt[6] = (width_bytes >= 4) ? value : ((g_bpl_pt[6] & 0x0000FFFFu) | ((value & 0xFFFFu) << 16)); break;
        case REG_BPL7PT + 2: g_bpl_pt[6] = (g_bpl_pt[6] & 0xFFFF0000u) | (value & 0xFFFFu); break;
        case REG_BPL8PT:     g_bpl_pt[7] = (width_bytes >= 4) ? value : ((g_bpl_pt[7] & 0x0000FFFFu) | ((value & 0xFFFFu) << 16)); break;
        case REG_BPL8PT + 2: g_bpl_pt[7] = (g_bpl_pt[7] & 0xFFFF0000u) | (value & 0xFFFFu); break;

        case REG_SPR0PT:     g_spr_pt[0] = (width_bytes >= 4) ? value : ((g_spr_pt[0] & 0x0000FFFFu) | ((value & 0xFFFFu) << 16)); break;
        case REG_SPR0PT + 2: g_spr_pt[0] = (g_spr_pt[0] & 0xFFFF0000u) | (value & 0xFFFFu); break;
        case REG_SPR1PT:     g_spr_pt[1] = (width_bytes >= 4) ? value : ((g_spr_pt[1] & 0x0000FFFFu) | ((value & 0xFFFFu) << 16)); break;
        case REG_SPR1PT + 2: g_spr_pt[1] = (g_spr_pt[1] & 0xFFFF0000u) | (value & 0xFFFFu); break;
        case REG_SPR2PT:     g_spr_pt[2] = (width_bytes >= 4) ? value : ((g_spr_pt[2] & 0x0000FFFFu) | ((value & 0xFFFFu) << 16)); break;
        case REG_SPR2PT + 2: g_spr_pt[2] = (g_spr_pt[2] & 0xFFFF0000u) | (value & 0xFFFFu); break;
        case REG_SPR3PT:     g_spr_pt[3] = (width_bytes >= 4) ? value : ((g_spr_pt[3] & 0x0000FFFFu) | ((value & 0xFFFFu) << 16)); break;
        case REG_SPR3PT + 2: g_spr_pt[3] = (g_spr_pt[3] & 0xFFFF0000u) | (value & 0xFFFFu); break;
        case REG_SPR4PT:     g_spr_pt[4] = (width_bytes >= 4) ? value : ((g_spr_pt[4] & 0x0000FFFFu) | ((value & 0xFFFFu) << 16)); break;
        case REG_SPR4PT + 2: g_spr_pt[4] = (g_spr_pt[4] & 0xFFFF0000u) | (value & 0xFFFFu); break;
        case REG_SPR5PT:     g_spr_pt[5] = (width_bytes >= 4) ? value : ((g_spr_pt[5] & 0x0000FFFFu) | ((value & 0xFFFFu) << 16)); break;
        case REG_SPR5PT + 2: g_spr_pt[5] = (g_spr_pt[5] & 0xFFFF0000u) | (value & 0xFFFFu); break;
        case REG_SPR6PT:     g_spr_pt[6] = (width_bytes >= 4) ? value : ((g_spr_pt[6] & 0x0000FFFFu) | ((value & 0xFFFFu) << 16)); break;
        case REG_SPR6PT + 2: g_spr_pt[6] = (g_spr_pt[6] & 0xFFFF0000u) | (value & 0xFFFFu); break;
        case REG_SPR7PT:     g_spr_pt[7] = (width_bytes >= 4) ? value : ((g_spr_pt[7] & 0x0000FFFFu) | ((value & 0xFFFFu) << 16)); break;
        case REG_SPR7PT + 2: g_spr_pt[7] = (g_spr_pt[7] & 0xFFFF0000u) | (value & 0xFFFFu); break;

        default: {
            /* Color registers: COLOR00-COLOR31 at 0x180-0x1BE. */
            if (regoff >= REG_COLOR00 && regoff < REG_COLOR00 + 0x40u) {
                uint32_t bank = (g_bplcon3 >> 13) & 0x7u;
                uint32_t idx  = (regoff - REG_COLOR00) >> 1;
                uint32_t color_index = (bank << 5) | idx;
                if (color_index < AGA_PALETTE_SIZE)
                    aga_color_write(color_index, (uint16_t)value);
            }
            break;
        }
    }
}

uint32_t chip_emu_read(uint32_t offset, int width_bytes)
{
    uint32_t regoff = offset_to_regoff(offset);
    if (regoff >= 0x1000u)
        return 0; /* outside AGA register area: harmless zero */

    uint32_t value = 0;

    /* Special register read behavior. */
    switch (regoff) {
        case REG_DMACON: value = g_dmacon; break;
        case REG_INTENA: value = g_intena; break;
        case REG_INTREQ: value = g_intreq; break;
        case REG_ADKCON: value = g_adkcon; break;

        case REG_VPOSR:  value = 0; break; /* TODO: vertical beam position */
        case REG_VHPOSR: value = 0; break; /* TODO: horizontal beam position */

        case REG_BPLCON0: value = g_bplcon0; break;
        case REG_BPLCON1: value = g_bplcon1; break;
        case REG_BPLCON2: value = g_bplcon2; break;
        case REG_BPLCON3: value = g_bplcon3; break;
        case REG_BPLCON4: value = g_bplcon4; break;

        case REG_BPLMOD1: value = g_bplmod1; break;
        case REG_BPLMOD2: value = g_bplmod2; break;

        case REG_DIWSTART: value = g_diwstart; break;
        case REG_DIWSTOP:  value = g_diwstop;  break;
        case REG_DDFSTART: value = g_ddfstart; break;
        case REG_DDFSTOP:  value = g_ddfstop;  break;
        case REG_COPCON:   value = g_copcon;   break;

        case REG_COP1LC:     value = (g_cop1lc >> 16) & 0xFFFFu; break;
        case REG_COP1LC + 2: value = g_cop1lc & 0xFFFFu; break;
        case REG_COP2LC:     value = (g_cop2lc >> 16) & 0xFFFFu; break;
        case REG_COP2LC + 2: value = g_cop2lc & 0xFFFFu; break;
        case REG_COPJMP1: return 0;
        case REG_COPJMP2: return 0;

        case REG_BPL1PT:     value = (g_bpl_pt[0] >> 16) & 0xFFFFu; break;
        case REG_BPL1PT + 2: value = g_bpl_pt[0] & 0xFFFFu; break;
        case REG_BPL2PT:     value = (g_bpl_pt[1] >> 16) & 0xFFFFu; break;
        case REG_BPL2PT + 2: value = g_bpl_pt[1] & 0xFFFFu; break;
        case REG_BPL3PT:     value = (g_bpl_pt[2] >> 16) & 0xFFFFu; break;
        case REG_BPL3PT + 2: value = g_bpl_pt[2] & 0xFFFFu; break;
        case REG_BPL4PT:     value = (g_bpl_pt[3] >> 16) & 0xFFFFu; break;
        case REG_BPL4PT + 2: value = g_bpl_pt[3] & 0xFFFFu; break;
        case REG_BPL5PT:     value = (g_bpl_pt[4] >> 16) & 0xFFFFu; break;
        case REG_BPL5PT + 2: value = g_bpl_pt[4] & 0xFFFFu; break;
        case REG_BPL6PT:     value = (g_bpl_pt[5] >> 16) & 0xFFFFu; break;
        case REG_BPL6PT + 2: value = g_bpl_pt[5] & 0xFFFFu; break;
        case REG_BPL7PT:     value = (g_bpl_pt[6] >> 16) & 0xFFFFu; break;
        case REG_BPL7PT + 2: value = g_bpl_pt[6] & 0xFFFFu; break;
        case REG_BPL8PT:     value = (g_bpl_pt[7] >> 16) & 0xFFFFu; break;
        case REG_BPL8PT + 2: value = g_bpl_pt[7] & 0xFFFFu; break;

        case REG_SPR0PT:     value = (g_spr_pt[0] >> 16) & 0xFFFFu; break;
        case REG_SPR0PT + 2: value = g_spr_pt[0] & 0xFFFFu; break;
        case REG_SPR1PT:     value = (g_spr_pt[1] >> 16) & 0xFFFFu; break;
        case REG_SPR1PT + 2: value = g_spr_pt[1] & 0xFFFFu; break;
        case REG_SPR2PT:     value = (g_spr_pt[2] >> 16) & 0xFFFFu; break;
        case REG_SPR2PT + 2: value = g_spr_pt[2] & 0xFFFFu; break;
        case REG_SPR3PT:     value = (g_spr_pt[3] >> 16) & 0xFFFFu; break;
        case REG_SPR3PT + 2: value = g_spr_pt[3] & 0xFFFFu; break;
        case REG_SPR4PT:     value = (g_spr_pt[4] >> 16) & 0xFFFFu; break;
        case REG_SPR4PT + 2: value = g_spr_pt[4] & 0xFFFFu; break;
        case REG_SPR5PT:     value = (g_spr_pt[5] >> 16) & 0xFFFFu; break;
        case REG_SPR5PT + 2: value = g_spr_pt[5] & 0xFFFFu; break;
        case REG_SPR6PT:     value = (g_spr_pt[6] >> 16) & 0xFFFFu; break;
        case REG_SPR6PT + 2: value = g_spr_pt[6] & 0xFFFFu; break;
        case REG_SPR7PT:     value = (g_spr_pt[7] >> 16) & 0xFFFFu; break;
        case REG_SPR7PT + 2: value = g_spr_pt[7] & 0xFFFFu; break;

        default: {
            value = g_aga_regs[regoff_to_index(regoff)];
            break;
        }
    }

    if (width_bytes == 4) {
        /* 32-bit reads combine the current word with the next word in the
         * backing store.  For pointer registers this yields the full 32-bit
         * pointer; for control registers it returns two adjacent registers. */
        uint16_t v2 = g_aga_regs[regoff_to_index(regoff) + 1];
        return ((uint32_t)(value & 0xFFFFu) << 16) | (uint32_t)v2;
    }

    if (width_bytes == 1) {
        if (offset & 1u)
            return (uint32_t)(value >> 8) & 0xFFu;
        return (uint32_t)(value & 0xFFu);
    }

    return (uint32_t)(value & 0xFFFFu);
}

/* =========================================================================
 * Tier 3 — Copper emulator and bitplane renderer
 * ========================================================================= */

/* Copper instruction format (two big-endian 16-bit words):
 *   MOVE: word1 bit 0 = 0, bits 15-1 = register offset (0x000-0x1FE).
 *         word2 = data to write.
 *   WAIT: word1 bit 0 = 1, word2 bit 0 = 0.
 *   SKIP: word1 bit 0 = 1, word2 bit 0 = 1.
 * The destination register offset is relative to 0xDFF000. */

#define COPPER_MOVE_LIMIT 4096

static int copper_run(uint32_t list_addr)
{
    if (!list_addr) return 0;

    for (int i = 0; i < COPPER_MOVE_LIMIT; i++) {
        if (list_addr + 4 >= GUEST_RAM_SIZE) return 0;

        uint16_t w1 = (uint16_t)((g_ram[list_addr] << 8) | g_ram[list_addr + 1]);
        uint16_t w2 = (uint16_t)((g_ram[list_addr + 2] << 8) | g_ram[list_addr + 3]);

        if ((w1 & 1u) == 0) {
            /* MOVE */
            uint32_t regoff = (uint32_t)(w1 & 0xFFFEu);
            chip_emu_write(AGA_REG_BASE_OFF + regoff, w2, 2);
            list_addr += 4;
        } else if ((w2 & 1u) == 0) {
            /* WAIT.  Stop on the impossible WAIT $FFFEFFFE. */
            if ((w1 & 0xFFFEu) == 0xFFFEu && (w2 & 0xFFFEu) == 0xFFFEu)
                return 1;
            list_addr += 4;
        } else {
            /* SKIP — skip next instruction if beam position is already past
             * the WAIT target.  Since we are not simulating beam position,
             * always skip the next instruction. */
            list_addr += 8;
        }
    }
    return 0;
}

void chip_emu_copper_jump(int list, uint32_t addr)
{
    if (list == 1) {
        if (addr) g_cop1lc = addr;
        g_copjmp1 = 1;
        copper_run(g_cop1lc);
    } else if (list == 2) {
        if (addr) g_cop2lc = addr;
        g_copjmp2 = 1;
        copper_run(g_cop2lc);
    }
}

/* Helper: read one bitplane byte, advancing the pointer by one line's worth. */
static uint8_t *bpl_line_ptr(int plane, int y)
{
    uint32_t base = g_bpl_pt[plane];
    if (!base) return NULL;
    /* Interleaved vs. non-interleaved is determined by BPLCON0 bit 1 (BPM).
     * For simplicity we treat all bitplanes as non-interleaved with the same
     * row stride.  The real stride is encoded in BPLMOD; we default to 40
     * bytes per low-res line and adjust with the modulo. */
    int stride = 40 + (int)(int16_t)g_bplmod1;
    if (stride < 2) stride = 2;
    if (base + (uint32_t)(y * stride) >= GUEST_RAM_SIZE) return NULL;
    return g_ram + base + (uint32_t)(y * stride);
}

/* Mode helpers from BPLCON0. */
static int bpl_depth(void)    { return (g_bplcon0 >> 12) & 0x7; }
static int bpl_ham(void)      { return (g_bplcon0 >> 11) & 1; }
static int bpl_dblpf(void)    { return (g_bplcon0 >> 10) & 1; }
static int bpl_hires(void)    { return (g_bplcon0 >> 15) & 1; }

static int ehb_active(void)
{
    int d = bpl_depth();
    return (d == 6) && !bpl_ham() && !bpl_dblpf();
}

static int ham_active(void)
{
    int d = bpl_depth();
    return (d >= 6) && bpl_ham();
}

/* Convert a planar pixel index into a host RGB colour. */
static uint32_t pixel_to_rgb(int index, int prev_rgb)
{
    if (ham_active()) {
        int control = (index >> 4) & 0x3; /* HAM6: upper 2 bits of 6-bit pixel */
        int value   = index & 0xF;
        int r = (prev_rgb >> 16) & 0xFF;
        int g = (prev_rgb >> 8)  & 0xFF;
        int b = prev_rgb & 0xFF;
        switch (control) {
            case 0: return g_aga_palette[value & 0x1F];      /* palette index */
            case 1: b = (value << 4) | value; break;         /* modify blue  */
            case 2: r = (value << 4) | value; break;         /* modify red   */
            case 3: g = (value << 4) | value; break;         /* modify green */
        }
        return (uint32_t)((r << 16) | (g << 8) | b);
    }

    if (ehb_active() && index >= 32) {
        uint32_t c = g_aga_palette[index - 32];
        int r = ((c >> 16) & 0xFF) >> 1;
        int g = ((c >> 8)  & 0xFF) >> 1;
        int b = (c & 0xFF) >> 1;
        return (uint32_t)((r << 16) | (g << 8) | b);
    }

    return g_aga_palette[index & 0xFF];
}

/* Render one scanline of bitplanes into the host framebuffer. */
static void render_scanline(int y, int x_start, int width)
{
    int depth = bpl_depth();
    if (depth < 1 || depth > 8) return;
    int hires = bpl_hires();
    int pix_scale = hires ? 1 : 2;              /* low-res pixels are doubled */
    int fb_w = (int)g_fb.width;
    int fb_h = (int)g_fb.height;
    if (y < 0 || y >= fb_h) return;

    /* Fetch pointers for each plane. */
    uint8_t *planes[8];
    for (int p = 0; p < depth; p++) {
        planes[p] = bpl_line_ptr(p, y);
    }

    uint32_t prev_rgb = 0;
    for (int x = 0; x < width; x++) {
        int byte = x / 8;
        int bit  = 7 - (x & 7);
        int index = 0;
        for (int p = 0; p < depth; p++) {
            if (planes[p] && (planes[p][byte] & (1 << bit)))
                index |= (1 << p);
        }
        uint32_t rgb = pixel_to_rgb(index, prev_rgb);
        prev_rgb = rgb;

        int dst_x = x_start + x * pix_scale;
        for (int s = 0; s < pix_scale && (dst_x + s) < fb_w; s++) {
            FB_PutPixel(dst_x + s, y, rgb);
        }
    }
}

/* Render the current chipset state to the host framebuffer. */
void chip_emu_render_frame(void)
{
    if (!g_fb.valid) return;

    /* Execute the primary copper list if one has been activated. */
    if (g_cop1lc) {
        chip_emu_copper_jump(1, 0);
    }

    /* Bitplane DMA must be enabled. */
    if (!(g_dmacon & 0x0100u)) {
        /* No bitplane output: keep whatever is already on screen. */
        return;
    }

    int depth = bpl_depth();
    if (depth < 1) return;

    /* Derive display window. */
    int diw_y = (g_diwstart >> 8) & 0xFF;
    int diw_h_stop = (g_diwstop >> 8) & 0xFF;
    if (diw_h_stop < diw_y) diw_h_stop += 0x100;
    int diw_x = (g_diwstart & 0xFF) - 0x80; /* DIWSTART horizontal is offset by $80 */
    int diw_x_stop = (g_diwstop & 0xFF) - 0x80;
    if (diw_x_stop < diw_x) diw_x_stop += 0x100;
    int diw_width = diw_x_stop - diw_x;
    if (diw_width <= 0) diw_width = 320;

    /* Derive fetch width from DDF if it looks sane, otherwise use DIW. */
    int ddf_start = g_ddfstart & 0xFF;
    int ddf_stop  = g_ddfstop & 0xFF;
    int fetch_width = diw_width;
    if (ddf_stop > ddf_start && (ddf_stop - ddf_start) >= 8) {
        int words = (ddf_stop - ddf_start) / 8;
        fetch_width = words * 16;
    }
    if (fetch_width <= 0 || fetch_width > 1280) fetch_width = 320;

    int x_start = diw_x < 0 ? 0 : diw_x;
    int y_start = diw_y;
    int y_end   = diw_h_stop;
    if (y_end > (int)g_fb.height) y_end = (int)g_fb.height;

    for (int y = y_start; y < y_end; y++) {
        render_scanline(y, x_start, fetch_width);
    }
}
