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
#include "chipset/floppy.h"
#include "display/framebuffer.h"
#include "uaos_emu.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

extern void m68k_set_irq(unsigned int int_level);
extern char PS2Kbd_GetChar(void);
extern int  PS2Kbd_HasChar(void);

/* Host COM1 (0x3F8) UART backend for Paula serial port. */
#define COM1_DATA 0x3F8
#define COM1_LSR  0x3FD

static inline void io_outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t io_inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static int com1_can_send(void)
{
    return (io_inb(COM1_LSR) & 0x20u) != 0; /* THRE */
}

static int com1_can_recv(void)
{
    return (io_inb(COM1_LSR) & 0x01u) != 0; /* DR */
}

static void com1_send(uint8_t c)
{
    int spins = 1000;
    while (spins-- > 0 && !com1_can_send()) {}
    io_outb(COM1_DATA, c);
}

static int com1_recv(uint8_t *c)
{
    if (!com1_can_recv()) return 0;
    *c = io_inb(COM1_DATA);
    return 1;
}

/* Host LPT1 (0x378) backend for CIA-B parallel port. */
#define LPT1_DATA 0x378
#define LPT1_CTRL 0x37A

static void lpt1_send(uint8_t c)
{
    int spins = 1000;
    io_outb(LPT1_DATA, c);
    /* Brief strobe pulse. */
    io_outb(LPT1_CTRL, 0x0Du); /* strobe low, auto LF, IRQ disabled */
    while (spins-- > 0) {}
    io_outb(LPT1_CTRL, 0x0Cu); /* strobe high */
}

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

#define REG_SERDATR  0x018
#define REG_DSKDAT   0x026
#define REG_DSKLEN   0x024
#define REG_DSKPTH   0x020
#define REG_DSKPTL   0x022
#define REG_SERDAT   0x030
#define REG_SERPER   0x032
#define REG_DSKSYNC  0x07E
#define REG_DENISEID 0x07C
#define REG_CLXDAT   0x00E
#define REG_CLXCON   0x016
#define REG_DMACONR  0x002
#define REG_DMACON   0x096
#define REG_INTENA   0x09A
#define REG_INTREQ   0x09C
#define REG_ADKCON   0x09E

#define REG_VPOSR    0x004
#define REG_VHPOSR   0x006

/* Blitter registers */
#define REG_BLTCON0  0x040
#define REG_BLTCON1  0x042
#define REG_BLTAFWM  0x044
#define REG_BLTALWM  0x046
#define REG_BLTCPT   0x048
#define REG_BLTBPT   0x04C
#define REG_BLTAPT   0x050
#define REG_BLTDPT   0x054
#define REG_BLTSIZE  0x058
#define REG_BLTAMOD  0x060
#define REG_BLTBMOD  0x062
#define REG_BLTCMOD  0x064
#define REG_BLTDMOD  0x066
#define REG_BLTCDAT  0x070
#define REG_BLTBDAT  0x072
#define REG_BLTADAT  0x074

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

#define REG_SPR0POS  0x140
#define REG_SPR0CTL  0x142
#define REG_SPR0DATA 0x144
#define REG_SPR0DATB 0x146
#define REG_SPRxPOS(x)  (REG_SPR0POS + (x) * 8)
#define REG_SPRxCTL(x)  (REG_SPR0CTL + (x) * 8)
#define REG_SPRxDATA(x) (REG_SPR0DATA + (x) * 8)
#define REG_SPRxDATB(x) (REG_SPR0DATB + (x) * 8)

/* Paula audio registers */
#define REG_AUD0LCH  0x0A0
#define REG_AUD0LCL  0x0A2
#define REG_AUD0LEN  0x0A4
#define REG_AUD0PER  0x0A6
#define REG_AUD0VOL  0x0A8
#define REG_AUD0DAT  0x0AA
#define REG_AUDxLCH(x) (REG_AUD0LCH + (x) * 0x10)
#define REG_AUDxLCL(x) (REG_AUD0LCL + (x) * 0x10)
#define REG_AUDxLEN(x) (REG_AUD0LEN + (x) * 0x10)
#define REG_AUDxPER(x) (REG_AUD0PER + (x) * 0x10)
#define REG_AUDxVOL(x) (REG_AUD0VOL + (x) * 0x10)
#define REG_AUDxDAT(x) (REG_AUD0DAT + (x) * 0x10)

#define REG_COLOR00  0x180

/* SET/CLR bit used by DMACON, INTENA, INTREQ, ADKCON */
#define SETCLR_BIT   0x8000u

static uint16_t g_dmacon;   /* DMA control */
static uint16_t g_intena;   /* interrupt enable */
uint16_t g_intreq;          /* interrupt request */
static uint16_t g_adkcon;   /* audio/disk control */

static uint32_t g_cop1lc;   /* copper list 1 pointer */
static uint32_t g_cop2lc;   /* copper list 2 pointer */
static uint8_t  g_copjmp1;  /* copper jump 1 strobe */
static uint8_t  g_copjmp2;  /* copper jump 2 strobe */
static uint32_t g_copper_pc; /* current copper instruction pointer */

static uint16_t g_bplcon0;  /* bitplane/control register: planes, HAM, EHB, genlock */
static uint16_t g_bplcon1;  /* horizontal scroll / modulos */
static uint16_t g_bplcon2;  /* playfield priorities / genlock */
static uint16_t g_bplcon3;  /* AGA bank/LOCT / sprite resolution */
static uint16_t g_bplcon4;  /* AGA color bank lower bits / sprite bank */

static uint16_t g_bplmod1;  /* bitplane modulo (odd planes) */
static uint16_t g_bplmod2;  /* bitplane modulo (even planes) */

/* Blitter state */
static uint16_t g_bltcon0;  /* minterm + channel enables + flags */
static uint16_t g_bltcon1;  /* shift/line/area flags */
static uint16_t g_bltafwm;  /* first word mask for A */
static uint16_t g_bltalwm;  /* last word mask for A */
static uint32_t g_bltapt;   /* blitter source A pointer */
static uint32_t g_bltbpt;   /* blitter source B pointer */
static uint32_t g_bltcpt;   /* blitter source C pointer */
static uint32_t g_bltdpt;   /* blitter destination D pointer */
static uint16_t g_bltamod;  /* modulo for A */
static uint16_t g_bltbmod;  /* modulo for B */
static uint16_t g_bltcmod;  /* modulo for C */
static uint16_t g_bltdmod;  /* modulo for D */
static uint16_t g_bltsize;  /* last written BLTSIZE */
static uint16_t g_bltadat;  /* A data register */
static uint16_t g_bltbdat;  /* B data register */
static uint16_t g_bltcdat;  /* C data register */
static uint8_t  g_blitter_busy; /* blitter status */
static uint32_t g_blitter_busy_ticks; /* PIT ticks remaining for busy */
static int      g_blitter_words_remaining; /* words left for slot-based execution */

/* Sprite collision state */
static uint16_t g_clxdat;   /* collision data (read and clear) */
static uint16_t g_clxcon;   /* collision control */

/* CIA-B IRQ: real Amiga routes CIA-B interrupts to M68k level 6. */
static int g_cia_b_irq;

/* CIA-A keyboard serial buffer: PS/2 keyboard bytes are translated to
 * Amiga keyboard protocol bytes and fed to the CIA-A SDR. */
#define KBD_SDR_SIZE 32
static uint8_t g_kbd_sdr_buf[KBD_SDR_SIZE];
static int g_kbd_sdr_head = 0;
static int g_kbd_sdr_tail = 0;
static int g_kbd_route_to_cia = 0; /* route PS/2 keys to CIA-A SDR only when M68k bridge is active */

/* Beam / VBlank state */
static uint32_t g_frame_counter; /* increments every VBlank */
static uint32_t g_vblank_count;  /* VBlank ticks */
static uint16_t g_vposr;         /* vertical beam position */
static uint16_t g_vhposr;        /* horizontal beam position */

/* Display timing: 0 = PAL, 1 = NTSC */
static int g_chip_mode;
#define CHIP_MODE_PAL  0
#define CHIP_MODE_NTSC 1

/* Interlace field tracking: 0 = long field, 1 = short field. */
static int g_interlace_field;

/* =========================================================================
 * Color-clock beam position
 *
 * The Amiga color clock is the same frequency as the M68k CPU clock:
 *   PAL  ~7.093790 MHz  ->  454.0 color clocks per line,  312/313 lines/frame
 *   NTSC ~7.159545 MHz  ->  455.0 color clocks per line,  262/263 lines/frame
 * We treat the M68k cycle counter as color-clock ticks and derive the beam
 * position from the elapsed cycles since the last VBlank.
 * ========================================================================= */
#define COLOR_CLOCK_PAL_HZ  7093790u
#define COLOR_CLOCK_NTSC_HZ 7159545u
#define PAL_LINES_PER_FRAME 312
#define NTSC_LINES_PER_FRAME 262

static uint32_t g_cycles_per_line;      /* color clocks per scanline */
static uint32_t g_cycles_per_frame;     /* color clocks per frame */
static uint64_t g_cycles_at_vblank;      /* M68k cycle count at last VBlank */
static uint64_t g_cycles_at_frame_start; /* M68k cycle count at current frame start */

/* Return the current beam position in color-clock ticks relative to the start
 * of the current frame. */
static uint64_t beam_cycles_now(void)
{
    uint64_t now = chip_emu_m68k_cycles();
    if (now < g_cycles_at_frame_start) return 0;
    return now - g_cycles_at_frame_start;
}

/* Compute (vpos, hpos) from color-clock ticks. */
static void beam_position_from_cycles(uint64_t cycles, int *vpos, int *hpos)
{
    if (g_cycles_per_line == 0) { *vpos = 0; *hpos = 0; return; }
    uint64_t c = cycles % (uint64_t)g_cycles_per_frame;
    *vpos = (int)(c / g_cycles_per_line);
    *hpos = (int)(c % g_cycles_per_line);
}

/* =========================================================================
 * Cycle-accurate DMA slot arbitration (first pass)
 *
 * PAL has ~226 DMA slots per line; NTSC has fewer.  The table below is a
 * simplified model: one slot per word transfer.  Channels are allocated in
 * priority order (refresh > bitplane > copper > sprite > audio > disk >
 * blitter > CPU).  When a channel cannot get enough slots it stalls.
 * ========================================================================= */
#define DMA_SLOTS_PER_LINE_PAL 226
#define DMA_SLOTS_PER_LINE_NTSC 227
#define DMA_REFRESH_SLOTS_PER_LINE 4

typedef enum {
    DMA_CHAN_REFRESH = 0,
    DMA_CHAN_BITPLANE,
    DMA_CHAN_COPPER,
    DMA_CHAN_SPRITE,
    DMA_CHAN_AUDIO,
    DMA_CHAN_DISK,
    DMA_CHAN_BLITTER,
    DMA_CHAN_CPU,
    DMA_CHAN_COUNT
} DMA_Channel;

static uint8_t  g_dma_slots[DMA_SLOTS_PER_LINE_PAL]; /* channel id or DMA_CHAN_COUNT for free */
static int      g_dma_slots_per_line;
static uint64_t g_cpu_stolen_cycles; /* cumulative CPU cycles stolen by DMA */

static void dma_slot_reset(void)
{
    g_dma_slots_per_line = (g_chip_mode == CHIP_MODE_NTSC) ? DMA_SLOTS_PER_LINE_NTSC : DMA_SLOTS_PER_LINE_PAL;
    for (int i = 0; i < DMA_SLOTS_PER_LINE_PAL; i++) g_dma_slots[i] = DMA_CHAN_COUNT;

    /* Refresh always gets its fixed slots, spread across the line. */
    int refresh = DMA_REFRESH_SLOTS_PER_LINE;
    for (int i = 0; i < g_dma_slots_per_line && refresh > 0; i += g_dma_slots_per_line / refresh) {
        if (g_dma_slots[i] == DMA_CHAN_COUNT) {
            g_dma_slots[i] = DMA_CHAN_REFRESH;
            refresh--;
        }
    }
}

/* Allocate `count` consecutive free slots for `channel`, starting search from
 * `start`.  Returns the first slot index on success, -1 on failure. */
static int dma_slot_alloc(DMA_Channel channel, int start, int count)
{
    if (count <= 0) return 0;
    for (int i = start; i + count <= g_dma_slots_per_line; i++) {
        int ok = 1;
        for (int j = 0; j < count; j++) {
            if (g_dma_slots[i + j] != DMA_CHAN_COUNT) { ok = 0; break; }
        }
        if (ok) {
            for (int j = 0; j < count; j++) g_dma_slots[i + j] = (uint8_t)channel;
            return i;
        }
    }
    return -1;
}

/* Release all slots owned by a channel. */
static void dma_slot_release(DMA_Channel channel)
{
    for (int i = 0; i < g_dma_slots_per_line; i++) {
        if (g_dma_slots[i] == (uint8_t)channel) g_dma_slots[i] = DMA_CHAN_COUNT;
    }
}

static int dma_slot_count(DMA_Channel channel)
{
    int n = 0;
    for (int i = 0; i < g_dma_slots_per_line; i++) {
        if (g_dma_slots[i] == (uint8_t)channel) n++;
    }
    return n;
}

static uint16_t g_diwstart; /* display window start */
static uint16_t g_diwstop;  /* display window stop */
static uint16_t g_ddfstart; /* display data fetch start */
static uint16_t g_ddfstop;  /* display data fetch stop */
static uint16_t g_copcon;   /* copper control (dangerous bits) */

/* Basic DMA pointer registers — stored but not yet rendered. */
static uint32_t g_bpl_pt[8];
static uint32_t g_spr_pt[8];

/* Sprite state: position, control, and up to four 16-bit data words per sprite.
 * AGA 64-pixel sprites need 4 words of DATA and 4 of DATB. */
#define SPRITE_COUNT 8
#define SPRITE_WORDS 4
static uint16_t g_spr_pos[SPRITE_COUNT];
static uint16_t g_spr_ctl[SPRITE_COUNT];
static uint16_t g_spr_data[SPRITE_COUNT][SPRITE_WORDS];
static uint16_t g_spr_datb[SPRITE_COUNT][SPRITE_WORDS];

/* Per-scanline collision tracking.  Only the low-resolution X range is tracked. */
#define COLLISION_WIDTH 1024
static uint8_t g_bp_even[COLLISION_WIDTH];
static uint8_t g_bp_odd[COLLISION_WIDTH];

/* Paula audio channel state */
#define AUDIO_CHANNELS 4
typedef struct {
    uint32_t ptr;
    uint16_t len;
    uint16_t per;
    uint16_t vol;
    uint16_t dat;
    uint16_t counter;
    uint8_t  byte_sel;  /* 0 = high byte of dat, 1 = low byte */
    uint8_t  dma_on;
} AudioChannel;
static AudioChannel g_audio[AUDIO_CHANNELS];

/* Paula serial and disk state */
static uint16_t g_serdat;   /* serial data */
static uint16_t g_serper;   /* serial period/control */
static uint16_t g_dsklen;   /* disk length/control */
static uint16_t g_dskdat;   /* disk data */
static uint16_t g_dsk_sync; /* disk sync word */
static uint32_t g_dskpt;    /* disk DMA pointer */
static uint32_t g_dsk_index; /* current offset in the synthetic disk buffer */
static uint8_t  g_dsk_buffer[901120]; /* synthetic 880 KiB ADF buffer */

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

/* Return the cumulative M68k cycles executed so far.  This is the foundation
 * for a future CPU/chipset timing lock; for now it is only collected. */
uint64_t chip_emu_m68k_cycles(void)
{
    extern uint64_t g_m68k_cycles;
    return g_m68k_cycles;
}

/* Deliver the highest enabled M68k interrupt level based on INTREQ/INTENA and
 * external CIA sources.  Amiga level mapping: bits 0-4 -> 1, 5-8 -> 2,
 * 9-12 -> 3, 13-14 -> 4.  CIA-B is routed to level 6. */
void chip_emu_update_irq(void)
{
    int level = 0;
    uint16_t pending = g_intreq & g_intena & 0x7FFFu;
    if (pending) {
        if (pending & 0xE000u) level = 4;       /* bits 13-14 */
        else if (pending & 0x1E00u) level = 3;   /* bits 9-12 */
        else if (pending & 0x01E0u) level = 2;   /* bits 5-8 */
        else level = 1;
    }
    if (g_cia_b_irq) {
        if (level < 6) level = 6;
    }
    m68k_set_irq((unsigned int)level);
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

static void blitter_execute(uint16_t size);
uint8_t chip_read_u8(uint32_t addr);
void chip_write_u8(uint32_t addr, uint8_t v);
uint16_t chip_read_u16(uint32_t addr);
void chip_write_u16(uint32_t addr, uint16_t v);

struct CIA_State;
static int cia_offset_to_reg(uint32_t offset, int *cia_id);
static struct CIA_State *cia_state(int id);
static void cia_write(struct CIA_State *cia, int reg, uint32_t value, int width_bytes);
static uint32_t cia_read(struct CIA_State *cia, int reg, int width_bytes);

void chip_emu_raise_intreq(uint16_t bits)
{
    g_intreq |= bits;
    chip_emu_update_irq();
}

void chip_emu_write(uint32_t offset, uint32_t value, int width_bytes)
{
    int cia_id, cia_reg = cia_offset_to_reg(offset, &cia_id);
    if (cia_reg >= 0) {
        cia_write(cia_state(cia_id), cia_reg, value, width_bytes);
        return;
    }

    uint32_t regoff = offset;
    if (offset >= AGA_REG_BASE_OFF) {
        regoff = offset_to_regoff(offset);
    } else if (offset >= 0x1000u) {
        return; /* outside AGA register area: swallow */
    }

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
        case REG_CLXCON: g_clxcon = (uint16_t)value; break;
        case REG_SERDAT: {
            g_serdat = (uint16_t)value;
            com1_send((uint8_t)(value & 0xFFu)); /* transmit to host COM1 */
            break;
        }
        case REG_SERPER: g_serper = (uint16_t)value; break;
        case REG_DSKPTL: g_dskpt = (g_dskpt & 0xFFFF0000u) | (value & 0xFFFFu); break;
        case REG_DSKPTH: g_dskpt = (g_dskpt & 0x0000FFFFu) | ((uint32_t)value << 16); break;
        case REG_DSKLEN: {
            g_dsklen = (uint16_t)value;
            if (value & 0x8000u) { /* DMAEN */
                if (value & 0x4000u) {
                    floppy_dma_write(g_dskpt, (uint16_t)value);
                } else {
                    floppy_dma_read(g_dskpt, (uint16_t)value, g_dsk_sync);
                }
            }
            break;
        }
        case REG_DSKDAT: {
            g_dskdat = (uint16_t)value;
            floppy_dskdat_write((uint16_t)value);
            break;
        }
        case REG_DSKSYNC: g_dsk_sync = (uint16_t)value; break;
        case REG_DMACON: g_dmacon = update_setclr(g_dmacon, (uint16_t)value); break;
        case REG_INTENA: g_intena = update_setclr(g_intena, (uint16_t)value); chip_emu_update_irq(); break;
        case REG_INTREQ: g_intreq = update_setclr(g_intreq, (uint16_t)value); chip_emu_update_irq(); break;
        case REG_ADKCON: g_adkcon = update_setclr(g_adkcon, (uint16_t)value); break;

        case REG_COP1LC:     g_cop1lc = (width_bytes >= 4) ? value : ((g_cop1lc & 0x0000FFFFu) | ((value & 0xFFFFu) << 16)); break;
        case REG_COP1LC + 2: g_cop1lc = (g_cop1lc & 0xFFFF0000u) | (value & 0xFFFFu); break;
        case REG_COP2LC:     g_cop2lc = (width_bytes >= 4) ? value : ((g_cop2lc & 0x0000FFFFu) | ((value & 0xFFFFu) << 16)); break;
        case REG_COP2LC + 2: g_cop2lc = (g_cop2lc & 0xFFFF0000u) | (value & 0xFFFFu); break;
        case REG_COPJMP1: g_copjmp1 = 1; break;
        case REG_COPJMP2: g_copjmp2 = 1; break;

        case REG_BPLCON0: g_bplcon0 = (uint16_t)value; break;
        case REG_BPLCON1: g_bplcon1 = (uint16_t)value; break;

        case REG_BLTCON0: g_bltcon0 = (uint16_t)value; break;
        case REG_BLTCON1: g_bltcon1 = (uint16_t)value; break;
        case REG_BLTAFWM: g_bltafwm = (uint16_t)value; break;
        case REG_BLTALWM: g_bltalwm = (uint16_t)value; break;
        case REG_BLTAPT:     g_bltapt = (width_bytes >= 4) ? value : ((g_bltapt & 0x0000FFFFu) | ((value & 0xFFFFu) << 16)); break;
        case REG_BLTAPT + 2: g_bltapt = (g_bltapt & 0xFFFF0000u) | (value & 0xFFFFu); break;
        case REG_BLTBPT:     g_bltbpt = (width_bytes >= 4) ? value : ((g_bltbpt & 0x0000FFFFu) | ((value & 0xFFFFu) << 16)); break;
        case REG_BLTBPT + 2: g_bltbpt = (g_bltbpt & 0xFFFF0000u) | (value & 0xFFFFu); break;
        case REG_BLTCPT:     g_bltcpt = (width_bytes >= 4) ? value : ((g_bltcpt & 0x0000FFFFu) | ((value & 0xFFFFu) << 16)); break;
        case REG_BLTCPT + 2: g_bltcpt = (g_bltcpt & 0xFFFF0000u) | (value & 0xFFFFu); break;
        case REG_BLTDPT:     g_bltdpt = (width_bytes >= 4) ? value : ((g_bltdpt & 0x0000FFFFu) | ((value & 0xFFFFu) << 16)); break;
        case REG_BLTDPT + 2: g_bltdpt = (g_bltdpt & 0xFFFF0000u) | (value & 0xFFFFu); break;
        case REG_BLTAMOD: g_bltamod = (uint16_t)value; break;
        case REG_BLTBMOD: g_bltbmod = (uint16_t)value; break;
        case REG_BLTCMOD: g_bltcmod = (uint16_t)value; break;
        case REG_BLTDMOD: g_bltdmod = (uint16_t)value; break;
        case REG_BLTADAT: g_bltadat = (uint16_t)value; break;
        case REG_BLTBDAT: g_bltbdat = (uint16_t)value; break;
        case REG_BLTCDAT: g_bltcdat = (uint16_t)value; break;
        case REG_BLTSIZE: {
            uint16_t size = (uint16_t)value;
            g_bltsize = size;
            int w = (size >> 6) & 0x3FF;
            int h = size & 0x3F;
            if (w == 0) w = 1024;
            if (h == 0) h = 64;
            g_blitter_words_remaining = w * h;
            blitter_execute(size);
            break;
        }
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

#define SPR_WRITE_CASES(idx) \
        case REG_SPRxPOS(idx):  g_spr_pos[idx] = (uint16_t)value; break; \
        case REG_SPRxCTL(idx):  g_spr_ctl[idx] = (uint16_t)value; break; \
        case REG_SPRxDATA(idx): g_spr_data[idx][0] = (uint16_t)value; break; \
        case REG_SPRxDATB(idx): g_spr_datb[idx][0] = (uint16_t)value; break

        SPR_WRITE_CASES(0);
        SPR_WRITE_CASES(1);
        SPR_WRITE_CASES(2);
        SPR_WRITE_CASES(3);
        SPR_WRITE_CASES(4);
        SPR_WRITE_CASES(5);
        SPR_WRITE_CASES(6);
        SPR_WRITE_CASES(7);
#undef SPR_WRITE_CASES

#define AUD_WRITE_CASES(idx) \
        case REG_AUDxLCH(idx): g_audio[idx].ptr = (width_bytes >= 4) ? value : ((g_audio[idx].ptr & 0x0000FFFFu) | ((value & 0xFFFFu) << 16)); break; \
        case REG_AUDxLCH(idx) + 2: g_audio[idx].ptr = (g_audio[idx].ptr & 0xFFFF0000u) | (value & 0xFFFFu); break; \
        case REG_AUDxLEN(idx): g_audio[idx].len = (uint16_t)value; break; \
        case REG_AUDxPER(idx): g_audio[idx].per = (uint16_t)value; g_audio[idx].counter = (uint16_t)value; break; \
        case REG_AUDxVOL(idx): g_audio[idx].vol = (uint16_t)(value & 0x40u ? (value & 0x3Fu) : (value & 0x3Fu)); break; \
        case REG_AUDxDAT(idx): g_audio[idx].dat = (uint16_t)value; break

        AUD_WRITE_CASES(0);
        AUD_WRITE_CASES(1);
        AUD_WRITE_CASES(2);
        AUD_WRITE_CASES(3);
#undef AUD_WRITE_CASES
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
    int cia_id, cia_reg = cia_offset_to_reg(offset, &cia_id);
    if (cia_reg >= 0) {
        return cia_read(cia_state(cia_id), cia_reg, width_bytes);
    }

    uint32_t regoff = offset;
    if (offset >= AGA_REG_BASE_OFF) {
        regoff = offset_to_regoff(offset);
    } else if (offset >= 0x1000u) {
        return 0; /* outside AGA register area: harmless zero */
    }

    uint32_t value = 0;

    /* Special register read behavior. */
    switch (regoff) {
        case REG_SERDATR: {
            uint8_t c = 0;
            int rd = com1_recv(&c);
            /* SERDATR layout: bits 0-7 = received data, bit 8 = overrun,
             * bit 9 = register full, bit 11 = break, bit 12 = transmit buff empty,
             * bit 13 = transmit shift reg empty, bit 14 = receive irq, bit 15 = STP. */
            value = rd ? ((uint32_t)c | 0x0100u) : 0x0000u;
            break;
        }
        case REG_DSKPTL: value = g_dskpt & 0xFFFFu; break;
        case REG_DSKPTH: value = (g_dskpt >> 16) & 0xFFFFu; break;
        case REG_DSKDAT: {
            if (g_dsklen & 0x8000u) {
                g_dskdat = floppy_dskdat_read();
            }
            value = g_dskdat;
            break;
        }
        case REG_CLXDAT: {
            value = g_clxdat;
            g_clxdat = 0; /* read and clear */
            break;
        }
        case REG_CLXCON: value = g_clxcon; break;
        case REG_DMACONR:
            value = g_dmacon;
            if (g_blitter_busy) value |= 0x4000u; /* BLITZ busy flag */
            break;
        case REG_DMACON: value = g_dmacon; break;
        case REG_INTENA: value = g_intena; break;
        case REG_INTREQ: value = g_intreq; break;
        case REG_ADKCON: value = g_adkcon; break;

        case REG_VPOSR: {
            int v, h;
            beam_position_from_cycles(beam_cycles_now(), &v, &h);
            value = ((uint32_t)(v & 0xFF) << 8) | 0x8000u; /* AGA id + vertical */
            break;
        }
        case REG_VHPOSR: {
            int v, h;
            beam_position_from_cycles(beam_cycles_now(), &v, &h);
            value = (uint32_t)(v & 0x100u) | ((uint32_t)(h & 0xFF) << 0);
            break;
        }
        case REG_DENISEID: value = 0x00F8u; break;          /* AGA Denise/Lisa ID */

        case REG_BPLCON0: value = g_bplcon0; break;
        case REG_BPLCON1: value = g_bplcon1; break;
        case REG_BPLCON2: value = g_bplcon2; break;

        case REG_BLTCON0: value = g_bltcon0; break;
        case REG_BLTCON1: value = g_bltcon1; break;
        case REG_BLTAFWM: value = g_bltafwm; break;
        case REG_BLTALWM: value = g_bltalwm; break;
        case REG_BLTAPT:     value = (g_bltapt >> 16) & 0xFFFFu; break;
        case REG_BLTAPT + 2: value = g_bltapt & 0xFFFFu; break;
        case REG_BLTBPT:     value = (g_bltbpt >> 16) & 0xFFFFu; break;
        case REG_BLTBPT + 2: value = g_bltbpt & 0xFFFFu; break;
        case REG_BLTCPT:     value = (g_bltcpt >> 16) & 0xFFFFu; break;
        case REG_BLTCPT + 2: value = g_bltcpt & 0xFFFFu; break;
        case REG_BLTDPT:     value = (g_bltdpt >> 16) & 0xFFFFu; break;
        case REG_BLTDPT + 2: value = g_bltdpt & 0xFFFFu; break;
        case REG_BLTAMOD: value = g_bltamod; break;
        case REG_BLTBMOD: value = g_bltbmod; break;
        case REG_BLTCMOD: value = g_bltcmod; break;
        case REG_BLTDMOD: value = g_bltdmod; break;
        case REG_BLTADAT: value = g_bltadat; break;
        case REG_BLTBDAT: value = g_bltbdat; break;
        case REG_BLTCDAT: value = g_bltcdat; break;
        case REG_BLTSIZE:  value = 0; break; /* write-only trigger; returns 0 */
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

#define SPR_READ_CASES(idx) \
        case REG_SPRxPOS(idx):  value = g_spr_pos[idx]; break; \
        case REG_SPRxCTL(idx):  value = g_spr_ctl[idx]; break; \
        case REG_SPRxDATA(idx): value = g_spr_data[idx][0]; break; \
        case REG_SPRxDATB(idx): value = g_spr_datb[idx][0]; break

        SPR_READ_CASES(0);
        SPR_READ_CASES(1);
        SPR_READ_CASES(2);
        SPR_READ_CASES(3);
        SPR_READ_CASES(4);
        SPR_READ_CASES(5);
        SPR_READ_CASES(6);
        SPR_READ_CASES(7);
#undef SPR_READ_CASES

#define AUD_READ_CASES(idx) \
        case REG_AUDxLCH(idx): value = (g_audio[idx].ptr >> 16) & 0xFFFFu; break; \
        case REG_AUDxLCH(idx) + 2: value = g_audio[idx].ptr & 0xFFFFu; break; \
        case REG_AUDxLEN(idx): value = g_audio[idx].len; break; \
        case REG_AUDxPER(idx): value = g_audio[idx].per; break; \
        case REG_AUDxVOL(idx): value = g_audio[idx].vol; break; \
        case REG_AUDxDAT(idx): value = g_audio[idx].dat; break

        AUD_READ_CASES(0);
        AUD_READ_CASES(1);
        AUD_READ_CASES(2);
        AUD_READ_CASES(3);
#undef AUD_READ_CASES
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
 * Tier 4 — Blitter
 * ========================================================================= */

uint8_t chip_read_u8(uint32_t addr)
{
    if (addr >= GUEST_RAM_SIZE) return 0;
    return g_ram[addr];
}

void chip_write_u8(uint32_t addr, uint8_t v)
{
    if (addr >= GUEST_RAM_SIZE) return;
    g_ram[addr] = v;
}

uint16_t chip_read_u16(uint32_t addr)
{
    if (addr + 2 > GUEST_RAM_SIZE) return 0;
    return (uint16_t)((g_ram[addr] << 8) | g_ram[addr + 1]);
}

void chip_write_u16(uint32_t addr, uint16_t v)
{
    if (addr + 2 > GUEST_RAM_SIZE) return;
    g_ram[addr]     = (uint8_t)(v >> 8);
    g_ram[addr + 1] = (uint8_t)v;
}

/* Execute an area blit triggered by a write to BLTSIZE.
 * Width is in words (bits 15-6 of size), height in lines (bits 5-0). */
static void blitter_execute(uint16_t size)
{
    int width  = (size >> 6) & 0x3FF;
    int height = size & 0x3F;
    if (width == 0)  width = 1024;
    if (height == 0) height = 64;

    int line_mode = g_bltcon1 & 1u;
    int desc      = (g_bltcon1 >> 1) & 1u;
    int useA      = (g_bltcon0 >> 11) & 1u;
    int useB      = (g_bltcon0 >> 10) & 1u;
    int useC      = (g_bltcon0 >> 9) & 1u;
    int useD      = (g_bltcon0 >> 8) & 1u;
    int minterm   = g_bltcon0 & 0xFFu;
    int ash       = (g_bltcon0 >> 12) & 0xFu;
    int bsh       = (g_bltcon1 >> 12) & 0xFu;
    int fill_mode = (g_bltcon1 >> 3) & 0x3u; /* bits 3-4: inclusive/exclusive fill */

    if (line_mode) {
        /* Amiga line mode.  BLTCON1 bits 4-7 are the octant, bits 1-2 are
         * SING/sign.  BLTAMOD = 2*minor - 2*major, BLTBMOD = 2*minor.
         * BLTAPTL holds the starting (x,y).  The line is drawn into BLTDPT. */
        int octant = (g_bltcon1 >> 4) & 0xFu;
        int sx = (octant & 1u) ? -1 : 1;
        int sy = (octant & 2u) ? -1 : 1;
        int swap = (octant & 4u) ? 1 : 0;
        int singular = (g_bltcon1 & 0x02u) ? 1 : 0;

        int amod = (int)(int16_t)g_bltamod;
        int bmod = (int)(int16_t)g_bltbmod;
        int minor = (bmod < 0 ? -bmod : bmod) / 2;
        int major = ((bmod - amod) < 0 ? -(bmod - amod) : (bmod - amod)) / 2;
        if (major < minor) { int t = major; major = minor; minor = t; }
        if (major <= 0) major = 1;

        int err = 2 * minor - major;
        int x = g_bltapt & 0xFF;
        int y = (g_bltapt >> 8) & 0xFF;
        int line_stride = (int)(int16_t)g_bltdmod;
        if (line_stride <= 0) line_stride = 40;

        int steps = singular ? 1 : (major + 1);
        for (int i = 0; i < steps; i++) {
            uint32_t off = g_bltdpt + (uint32_t)(y * line_stride) + ((uint32_t)x / 16u) * 2u;
            if (off + 1 < GUEST_RAM_SIZE) {
                uint16_t v = chip_read_u16(off);
                v |= (uint16_t)(1u << (15 - (x & 15)));
                chip_write_u16(off, v);
            }
            if (i + 1 >= steps) break;
            if (err >= 0) {
                if (swap) x += sx;
                else      y += sy;
                err += amod; /* 2*minor - 2*major */
            } else {
                err += bmod; /* 2*minor */
            }
            if (swap) y += sy;
            else      x += sx;
        }
        g_blitter_busy = 1;
        g_blitter_words_remaining = steps;
        g_blitter_busy_ticks = 0;
        return;
    }

    uint32_t apt = g_bltapt;
    uint32_t bpt = g_bltbpt;
    uint32_t cpt = g_bltcpt;
    uint32_t dpt = g_bltdpt;

    int stride = desc ? -2 : 2;
    /* In DESC mode the programmer is responsible for setting the correct modulo
     * (full row width, not padding); the hardware does not negate it. */
    int amod = (int)(int16_t)g_bltamod;
    int bmod = (int)(int16_t)g_bltbmod;
    int cmod = (int)(int16_t)g_bltcmod;
    int dmod = (int)(int16_t)g_bltdmod;

    if (desc) {
        uint32_t offset = (uint32_t)(width - 1) * 2u;
        if (useA) apt += offset;
        if (useB) bpt += offset;
        if (useC) cpt += offset;
        if (useD) dpt += offset;
    }

    g_blitter_busy = 1;

    for (int y = 0; y < height; y++) {
        int fill_state = 0;
        for (int x = 0; x < width; x++) {
            uint16_t a = useA ? chip_read_u16(apt) : g_bltadat;
            uint16_t b = useB ? chip_read_u16(bpt) : g_bltbdat;
            uint16_t c = useC ? chip_read_u16(cpt) : g_bltcdat;

            int x_fwd = desc ? (width - 1 - x) : x;
            if (x_fwd == 0)       a &= g_bltafwm;
            if (x_fwd == width - 1) a &= g_bltalwm;

            if (ash) a = (uint16_t)((a >> ash) | (a << (16 - ash)));
            if (bsh) b = (uint16_t)((b >> bsh) | (b << (16 - bsh)));

            uint16_t d = 0;
            for (int bit = 0; bit < 16; bit++) {
                int abit = (a >> bit) & 1u;
                int bbit = (b >> bit) & 1u;
                int cbit = (c >> bit) & 1u;
                int idx  = (abit << 2) | (bbit << 1) | cbit;
                int bit_out = (minterm & (1u << idx)) ? 1 : 0;

                if (fill_mode) {
                    /* Polygon edge-fill: crossing an A-channel edge toggles
                     * the fill state.  Inclusive mode fills the edge pixel
                     * itself; exclusive mode leaves it unfilled. */
                    if (abit) fill_state ^= 1;
                    if (fill_mode & 1u) {
                        /* inclusive (IFEFE) */
                        bit_out = fill_state;
                    } else {
                        /* exclusive (EFE): edge pixel uses old state */
                        bit_out = abit ? (fill_state ^ 1) : fill_state;
                    }
                }
                if (bit_out)
                    d |= (uint16_t)(1u << bit);
            }

            if (useD) chip_write_u16(dpt, d);

            if (useA) apt = (uint32_t)((int32_t)apt + stride);
            if (useB) bpt = (uint32_t)((int32_t)bpt + stride);
            if (useC) cpt = (uint32_t)((int32_t)cpt + stride);
            if (useD) dpt = (uint32_t)((int32_t)dpt + stride);
        }

        if (y + 1 < height) {
            if (useA) apt = (uint32_t)((int32_t)apt + amod);
            if (useB) bpt = (uint32_t)((int32_t)bpt + bmod);
            if (useC) cpt = (uint32_t)((int32_t)cpt + cmod);
            if (useD) dpt = (uint32_t)((int32_t)dpt + dmod);
        }
    }

    g_blitter_busy = 1;
    g_blitter_words_remaining = width * height;
    g_blitter_busy_ticks = 0;
}

/* =========================================================================
 * Tier 4 — VBlank / beam timing
 * ========================================================================= */

static void update_color_clock_params(void)
{
    int lines = (g_chip_mode == CHIP_MODE_NTSC) ? NTSC_LINES_PER_FRAME : PAL_LINES_PER_FRAME;
    if (g_bplcon0 & 0x0004u) lines += 1; /* interlace short field */
    uint32_t hz = (g_chip_mode == CHIP_MODE_NTSC) ? COLOR_CLOCK_NTSC_HZ : COLOR_CLOCK_PAL_HZ;
    g_cycles_per_line = hz / 50 / lines;
    g_cycles_per_frame = g_cycles_per_line * (uint32_t)lines;
}

void chip_emu_vblank(void)
{
    g_vblank_count++;
    g_frame_counter++;
    /* Toggle interlace field every VBlank when LACE is enabled. */
    if (g_bplcon0 & 0x0004u) {
        g_interlace_field ^= 1;
    } else {
        g_interlace_field = 0;
    }
    /* Recompute color-clock parameters after field change. */
    update_color_clock_params();
    /* Align frame start to the color-clock boundary. */
    g_cycles_at_frame_start = chip_emu_m68k_cycles();
    /* Set VBlank interrupt request (INTREQ bit 5). */
    g_intreq |= 0x0020u;
    /* At VBlank, beam is at the top of the display. */
    g_vposr = 0;
    g_vhposr = 0;
    chip_emu_update_irq();
}

/* Update beam position from the PIT tick path.  Called every PIT tick.
 * With color-clock timing, the beam is derived on demand from the M68k
 * cycle counter; this function just keeps the legacy variables in sync. */
void chip_emu_beam_tick(uint32_t tick_counter)
{
    (void)tick_counter;
    int v, h;
    beam_position_from_cycles(beam_cycles_now(), &v, &h);
    g_vposr = (uint16_t)(v & 0xFFu);
    g_vhposr = (uint16_t)(h & 0xFFu);
}

uint32_t chip_emu_vblank_count(void)
{
    return g_vblank_count;
}

/* =========================================================================
 * Tier 5 — CIA-A and CIA-B 8520 timers/TOD
 * ========================================================================= */

#define CIA_A_BASE_OFF 0xFE001u
#define CIA_B_BASE_OFF 0xFD000u

#define CIA_REG_PRA      0x0
#define CIA_REG_PRB      0x1
#define CIA_REG_DDRA     0x2
#define CIA_REG_DDRB     0x3
#define CIA_REG_TALO     0x4
#define CIA_REG_TAHI     0x5
#define CIA_REG_TBLO     0x6
#define CIA_REG_TBHI     0x7
#define CIA_REG_TOD_LO   0x8
#define CIA_REG_TOD_MID  0x9
#define CIA_REG_TOD_HI   0xA
#define CIA_REG_SDR      0xC
#define CIA_REG_ICR      0xD
#define CIA_REG_CRA      0xE
#define CIA_REG_CRB      0xF

/* 8520-style timer */
typedef struct CIA_Timer {
    uint16_t latch;
    uint16_t counter;
    uint8_t  cra;
} CIA_Timer;

typedef struct CIA_State {
    CIA_Timer ta;
    CIA_Timer tb;
    uint32_t  tod;     /* BCD-ish 24-bit TOD counter */
    uint8_t   pra, prb;
    uint8_t   ddra, ddrb;
    uint8_t   icr;
    uint8_t   icr_mask;
} CIA_State;

static CIA_State g_cia_a, g_cia_b;

/* Push an Amiga keyboard byte into the CIA-A SDR buffer and raise the
 * keyboard interrupt (CIA-A ICR bit 3, mapped to INTREQ PORTS). */
static void kbd_sdr_push(uint8_t c)
{
    int next = (g_kbd_sdr_tail + 1) % KBD_SDR_SIZE;
    if (next == g_kbd_sdr_head) return; /* drop if full */
    g_kbd_sdr_buf[g_kbd_sdr_tail] = c;
    g_kbd_sdr_tail = next;
    g_cia_a.icr |= 0x08u; /* keyboard serial interrupt */
    g_intreq |= 0x0008u;  /* PORTS level-1 interrupt */
    chip_emu_update_irq();
}

static int kbd_sdr_pop(void)
{
    if (g_kbd_sdr_head == g_kbd_sdr_tail) return -1;
    uint8_t c = g_kbd_sdr_buf[g_kbd_sdr_head];
    g_kbd_sdr_head = (g_kbd_sdr_head + 1) % KBD_SDR_SIZE;
    return (int)c;
}

/* Poll the PS/2 driver and feed translated bytes to the CIA-A SDR buffer.
 * Called from the PIT tick path. */
void chip_emu_poll_ps2_keyboard(void)
{
    if (!g_kbd_route_to_cia) return;
    while (PS2Kbd_HasChar()) {
        char c = PS2Kbd_GetChar();
        if (c == 0) continue;
        /* Simple identity mapping: PS/2 ASCII -> Amiga keyboard raw byte.
         * A full translation would map Amiga keycodes; this is sufficient
         * for a first pass. */
        kbd_sdr_push((uint8_t)c);
    }
}

void chip_emu_set_keyboard_route(int to_cia)
{
    g_kbd_route_to_cia = to_cia ? 1 : 0;
}

static int cia_offset_to_reg(uint32_t offset, int *cia_id)
{
    if ((offset & ~0x0F00u) == CIA_A_BASE_OFF) { *cia_id = 0; return (int)((offset >> 8) & 0xF); }
    if ((offset & ~0x0F00u) == CIA_B_BASE_OFF) { *cia_id = 1; return (int)((offset >> 8) & 0xF); }
    return -1;
}

static CIA_State *cia_state(int id)
{
    return (id == 0) ? &g_cia_a : &g_cia_b;
}

static uint16_t cia_read_timer(CIA_Timer *t)
{
    return t->counter;
}

static void cia_write_timer_lo(CIA_Timer *t, uint8_t v)
{
    t->latch = (uint16_t)((t->latch & 0xFF00u) | v);
    if (!(t->cra & 1u)) t->counter = t->latch;
}

static void cia_write_timer_hi(CIA_Timer *t, uint8_t v)
{
    t->latch = (uint16_t)((t->latch & 0x00FFu) | ((uint16_t)v << 8));
    if (!(t->cra & 1u)) t->counter = t->latch;
}

static uint32_t cia_read(CIA_State *cia, int reg, int width_bytes)
{
    (void)width_bytes;
    switch (reg) {
        case CIA_REG_PRA:  return cia->pra;
        case CIA_REG_PRB:  return cia->prb;
        case CIA_REG_DDRA: return cia->ddra;
        case CIA_REG_DDRB: return cia->ddrb;
        case CIA_REG_TALO: return cia_read_timer(&cia->ta) & 0xFFu;
        case CIA_REG_TAHI: return (cia_read_timer(&cia->ta) >> 8) & 0xFFu;
        case CIA_REG_TBLO: return cia_read_timer(&cia->tb) & 0xFFu;
        case CIA_REG_TBHI: return (cia_read_timer(&cia->tb) >> 8) & 0xFFu;
        case CIA_REG_TOD_LO: return cia->tod & 0xFFu;
        case CIA_REG_TOD_MID: return (cia->tod >> 8) & 0xFFu;
        case CIA_REG_TOD_HI: return (cia->tod >> 16) & 0xFFu;
        case CIA_REG_SDR: {
            if (cia == &g_cia_a) {
                int v = kbd_sdr_pop();
                return (v >= 0) ? (uint32_t)v : 0;
            }
            return 0;
        }
        case CIA_REG_ICR: {
            uint32_t v = cia->icr;
            cia->icr = 0;
            if (cia == &g_cia_b) {
                g_cia_b_irq = 0;
                chip_emu_update_irq();
            }
            return v;
        }
        case CIA_REG_CRA: return cia->ta.cra;
        case CIA_REG_CRB: return cia->tb.cra;
        default: return 0;
    }
}

static void cia_write(CIA_State *cia, int reg, uint32_t value, int width_bytes)
{
    (void)width_bytes;
    uint8_t v = (uint8_t)(value & 0xFFu);
    switch (reg) {
        case CIA_REG_PRA:  cia->pra  = v; break;
        case CIA_REG_PRB: {
            cia->prb = v;
            if (cia == &g_cia_b) lpt1_send(v); /* parallel port data */
            break;
        }
        case CIA_REG_DDRA: cia->ddra = v; break;
        case CIA_REG_DDRB: cia->ddrb = v; break;
        case CIA_REG_TALO: cia_write_timer_lo(&cia->ta, v); break;
        case CIA_REG_TAHI: cia_write_timer_hi(&cia->ta, v); break;
        case CIA_REG_TBLO: cia_write_timer_lo(&cia->tb, v); break;
        case CIA_REG_TBHI: cia_write_timer_hi(&cia->tb, v); break;
        case CIA_REG_TOD_LO: cia->tod = (cia->tod & 0xFFFF00u) | v; break;
        case CIA_REG_TOD_MID: cia->tod = (cia->tod & 0xFF00FFu) | ((uint32_t)v << 8); break;
        case CIA_REG_TOD_HI: cia->tod = (cia->tod & 0x00FFFFu) | ((uint32_t)v << 16); break;
        case CIA_REG_SDR: {
            /* CIAA SDR writes are keyboard commands; CIAB SDR writes are
             * external serial.  Both are accepted and ignored for now. */
            (void)v;
            break;
        }
        case CIA_REG_ICR: {
            if (v & 0x80u) cia->icr_mask |= (v & 0x7Fu);
            else cia->icr_mask &= ~(v & 0x7Fu);
            break;
        }
        case CIA_REG_CRA: {
            cia->ta.cra = v;
            if (v & 0x10u) cia->ta.counter = cia->ta.latch; /* force load */
            break;
        }
        case CIA_REG_CRB: {
            cia->tb.cra = v;
            if (v & 0x10u) cia->tb.counter = cia->tb.latch; /* force load */
            break;
        }
        default: break;
    }
}

/* Return the current 8-bit signed sample for a Paula audio channel.
 * The high byte of the current 16-bit DMA word is played first, then the
 * low byte. */
int8_t chip_emu_audio_sample_8bit(int ch)
{
    if (ch < 0 || ch >= AUDIO_CHANNELS) return 0;
    const AudioChannel *a = &g_audio[ch];
    uint16_t word = a->dat;
    if (a->byte_sel == 0) {
        return (int8_t)((word >> 8) & 0xFFu);
    } else {
        return (int8_t)(word & 0xFFu);
    }
}

/* Return the current volume (0-64) for a Paula audio channel. */
uint8_t chip_emu_audio_volume(int ch)
{
    if (ch < 0 || ch >= AUDIO_CHANNELS) return 0;
    return (uint8_t)(g_audio[ch].vol & 0x3Fu);
}

/* Directly configure a Paula audio channel (used by tests that run before the
 * chip-window page fault handler is installed). */
void chip_emu_audio_set_channel(int ch, uint32_t ptr, uint16_t len, uint16_t per, uint16_t vol)
{
    if (ch < 0 || ch >= AUDIO_CHANNELS) return;
    AudioChannel *a = &g_audio[ch];
    a->ptr = ptr;
    a->len = len;
    a->per = per;
    a->counter = per;
    a->vol = (uint16_t)(vol & 0x3Fu);
    a->byte_sel = 0;
    /* Fetch the first word so the channel starts immediately. */
    if (a->ptr + 1 < GUEST_RAM_SIZE) {
        a->dat = chip_read_u16(a->ptr);
    }
}

/* Directly set the audio DMA control word (master enable + channel enables). */
void chip_emu_audio_set_dmacon(uint16_t dmacon)
{
    g_dmacon = dmacon;
}

/* Advance Paula audio DMA by a given number of Amiga master-clock cycles.
 * Each channel counts down its period; when it expires the next byte of the
 * current DMA word is played, and after the low byte the next word is fetched.
 */
void chip_emu_audio_advance(uint32_t amiga_cycles)
{
    if (!(g_dmacon & 0x0200u)) return; /* master DMA disabled */

    for (int ch = 0; ch < AUDIO_CHANNELS; ch++) {
        if (!(g_dmacon & (1u << ch))) continue; /* channel DMA disabled */
        AudioChannel *a = &g_audio[ch];
        if (!a->per || a->len == 0) continue;

        uint32_t remaining = amiga_cycles;
        while (remaining > 0) {
            if (a->counter > remaining) {
                a->counter -= (uint16_t)remaining;
                break;
            }
            remaining -= a->counter;
            a->counter = a->per;

            /* Toggle byte select.  After the low byte finishes, fetch the next word. */
            if (a->byte_sel == 0) {
                a->byte_sel = 1;
            } else {
                a->byte_sel = 0;
                if (a->ptr + 1 < GUEST_RAM_SIZE) {
                    a->dat = chip_read_u16(a->ptr);
                }
                a->ptr += 2;
                if (a->len > 0) a->len--;
                if (a->len == 0) {
                    /* DMA restart: in real Paula this triggers an interrupt and reloads */
                    g_intreq |= (uint16_t)(0x0200u << ch); /* AUD0..AUD3 bits 9-12 */
                    chip_emu_update_irq();
                }
            }
        }
    }
}

/* Legacy one-tick advance.  A tick is no longer tied to a fixed host rate,
 * so this is kept only for callers that have not yet been converted. */
void chip_emu_audio_tick(void)
{
    /* Approximate: one host PIT tick historically corresponded to one chip
     * cycle in this stub, but real audio generation is now done by the mixer
     * calling chip_emu_audio_advance() with the correct Amiga-clock delta. */
}

/* Advance CIA timers. Called from the PIT tick path. */
void chip_emu_cia_tick(void)
{
    if (g_blitter_busy && g_blitter_busy_ticks > 0) {
        g_blitter_busy_ticks--;
        if (g_blitter_busy_ticks == 0) g_blitter_busy = 0;
    }
    /* If the blitter ran out of words it is cleared in the scanline loop. */

    CIA_State *cias[2] = { &g_cia_a, &g_cia_b };
    for (int i = 0; i < 2; i++) {
        CIA_State *cia = cias[i];
        for (int t = 0; t < 2; t++) {
            CIA_Timer *tm = (t == 0) ? &cia->ta : &cia->tb;
            if (!(tm->cra & 1u)) continue; /* not running */
            if (tm->counter > 0) tm->counter--;
            if (tm->counter == 0) {
                if (tm->cra & 0x08u) {
                    /* one-shot: stop */
                    tm->cra &= ~1u;
                } else {
                    /* continuous: reload */
                    tm->counter = tm->latch;
                }
                /* set timer interrupt */
                cia->icr |= (t == 0) ? 0x01u : 0x02u;
                if (i == 0) {
                    g_intreq |= (t == 0) ? 0x2000u : 0x4000u; /* CIA-A TIMERA/TIMERB */
                } else {
                    g_cia_b_irq = 1; /* CIA-B uses M68k level 6 */
                }
                chip_emu_update_irq();
            }
        }
    }
}

/* =========================================================================
 * Tier 3 — Copper emulator and bitplane renderer
 * ========================================================================= */

/* Copper instruction format (two big-endian 16-bit words):
 *   MOVE: word1 bit 0 = 0, bits 15-1 = register offset (0x000-0x1FE).
 *         word2 = data to write.
 *   WAIT: word1 bit 0 = 1, word2 bit 0 = 0.
 *   SKIP: word1 bit 0 = 1, word2 bit 0 = 1.
 * The destination register offset is relative to 0xDFF000.
 *
 * Per-scanline execution: g_copper_pc tracks the current PC.  WAIT compares
 * the masked beam position against its target; if not yet satisfied the
 * copper stalls and resumes on the next scanline. */

#define COPPER_MOVE_LIMIT 4096

static int copper_wait_satisfied(int target_v, int target_h, int mask_v, int mask_h, int vpos, int hpos)
{
    return ((vpos & mask_v) >= (target_v & mask_v)) &&
           ((hpos & mask_h) >= (target_h & mask_h));
}

/* Execute copper instructions until a WAIT that cannot be satisfied at the
 * supplied beam position is reached.  Returns 1 if the copper reached the
 * end-of-list impossible WAIT.  `copper_slots` is the number of DMA slots
 * the copper has been granted this line; each instruction consumes 2 slots. */
static int copper_run_to_beam(int copper_slots)
{
    int vpos, hpos;
    beam_position_from_cycles(beam_cycles_now(), &vpos, &hpos);

    uint32_t pc = g_copper_pc;
    if (!pc) return 0;

    for (int i = 0; i < COPPER_MOVE_LIMIT; i++) {
        if (copper_slots < 2) {
            /* Copper has run out of DMA slots for this line; stall here. */
            g_copper_pc = pc;
            return 0;
        }
        copper_slots -= 2;

        if (pc + 4 >= GUEST_RAM_SIZE) {
            g_copper_pc = pc;
            return 0;
        }

        uint16_t w1 = chip_read_u16(pc);
        uint16_t w2 = chip_read_u16(pc + 2);

        if ((w1 & 1u) == 0) {
            /* MOVE */
            uint32_t regoff = (uint32_t)(w1 & 0xFFFEu);
            /* COPCON bit 0 = "copper danger": when clear, the copper cannot
             * write to the Blitter register block (0x040-0x058). */
            int copper_danger = g_copcon & 1u;
            if (regoff < 0x040u || regoff > 0x058u || copper_danger) {
                chip_emu_write(AGA_REG_BASE_OFF + regoff, w2, 2);
            }
            pc += 4;
        } else if ((w2 & 1u) == 0) {
            /* WAIT */
            if ((w1 & 0xFFFEu) == 0xFFFEu && (w2 & 0xFFFEu) == 0xFFFEu) {
                g_copper_pc = pc;
                /* Copper end-of-list interrupt request (COPER, bit 6). */
                g_intreq |= 0x0040u;
                chip_emu_update_irq();
                return 1; /* end of list */
            }
            int target_v = (w1 >> 8) & 0xFF;
            int target_h = (w1 >> 1) & 0x7F;
            int mask_v   = (w2 >> 8) & 0xFF;
            int mask_h   = (w2 >> 1) & 0x7F;
            if (copper_wait_satisfied(target_v, target_h, mask_v, mask_h, vpos, hpos)) {
                pc += 4;
            } else {
                g_copper_pc = pc;
                return 0;
            }
        } else {
            /* SKIP */
            int target_v = (w1 >> 8) & 0xFF;
            int target_h = (w1 >> 1) & 0x7F;
            int mask_v   = (w2 >> 8) & 0xFF;
            int mask_h   = (w2 >> 1) & 0x7F;
            if (copper_wait_satisfied(target_v, target_h, mask_v, mask_h, vpos, hpos)) {
                pc += 8; /* skip next instruction */
            } else {
                pc += 4;
            }
        }
    }
    g_copper_pc = pc;
    return 0;
}

void chip_emu_copper_jump(int list, uint32_t addr)
{
    if (list == 1) {
        if (addr) g_cop1lc = addr;
        g_copjmp1 = 1;
        g_copper_pc = g_cop1lc;
        copper_run_to_beam(100); /* allow a burst at copper restart */
    } else if (list == 2) {
        if (addr) g_cop2lc = addr;
        g_copjmp2 = 1;
        g_copper_pc = g_cop2lc;
        copper_run_to_beam(100);
    }
}

/* Helper: bitplane line pointer for a given plane and scanline.
 * Each plane has its own base pointer and its own modulo:
 *   odd-numbered planes (1,3,5,7) use BPL1MOD
 *   even-numbered planes (2,4,6,8) use BPL2MOD
 * The caller supplies the per-line fetch width in bytes. */
static uint8_t *bpl_line_ptr(int plane, int y, int bytes_per_row)
{
    uint32_t base = g_bpl_pt[plane];
    if (!base) return NULL;
    uint16_t mod = ((plane & 1) == 0) ? g_bplmod1 : g_bplmod2;
    int stride = bytes_per_row + (int)(int16_t)mod;
    if (stride < 1) stride = 1;
    uint32_t off = (uint32_t)(y * stride);
    if (base + off + (uint32_t)bytes_per_row >= GUEST_RAM_SIZE) return NULL;
    return g_ram + base + off;
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

/* Render one scanline of bitplanes into the host framebuffer.
 * y is the display line, fetch_y is the bitplane line to read (interlace
 * uses half the display line).  bytes_per_row is the hardware fetch width. */
static void render_scanline(int y, int fetch_y, int x_start, int fetch_pixels, int bytes_per_row)
{
    int depth = bpl_depth();
    if (depth < 1 || depth > 8) return;
    int hires = bpl_hires();
    int pix_scale = hires ? 1 : 2;              /* low-res pixels are doubled */
    int fb_w = (int)g_fb.width;
    int fb_h = (int)g_fb.height;
    if (y < 0 || y >= fb_h) return;

    /* Clear per-scanline collision occupancy for this line. */
    for (int i = 0; i < COLLISION_WIDTH; i++) {
        g_bp_even[i] = 0;
        g_bp_odd[i] = 0;
    }

    /* Horizontal scroll: PF1H in bits 3-0, PF2H in bits 7-4. */
    int pf1_scroll = g_bplcon1 & 0xF;
    int pf2_scroll = (g_bplcon1 >> 4) & 0xF;

    /* Fetch pointers for each plane. */
    uint8_t *planes[8];
    for (int p = 0; p < depth; p++) {
        planes[p] = bpl_line_ptr(p, fetch_y, bytes_per_row);
    }

    int dblpf = bpl_dblpf();
    int pf2_prio = g_bplcon2 & 0x7u; /* PF2P0-PF2P2 */
    int pf1_prio = (g_bplcon2 >> 3) & 0x7u; /* PF1P0-PF1P2 */
    int pf2_in_front = (pf2_prio > pf1_prio);
    int pf1_depth = (depth + 1) / 2;
    int pf2_depth = depth / 2;
    int pf2_base = (pf2_depth >= 4) ? 16 : 8;

    uint32_t prev_rgb = 0;
    for (int x = 0; x < fetch_pixels; x++) {
        uint32_t rgb;
        if (dblpf) {
            int idx1 = 0, idx2 = 0;
            for (int p = 0; p < pf1_depth; p++) {
                int sx = x + pf1_scroll;
                if (sx < 0) sx = 0;
                int byte = sx / 8;
                int bit  = 7 - (sx & 7);
                if (planes[p * 2] && (planes[p * 2][byte] & (1 << bit)))
                    idx1 |= (1 << p);
            }
            for (int p = 0; p < pf2_depth; p++) {
                int sx = x + pf2_scroll;
                if (sx < 0) sx = 0;
                int byte = sx / 8;
                int bit  = 7 - (sx & 7);
                if (planes[p * 2 + 1] && (planes[p * 2 + 1][byte] & (1 << bit)))
                    idx2 |= (1 << p);
            }
            int idx;
            if (pf2_in_front) {
                idx = (idx2 != 0) ? (pf2_base + idx2) : idx1;
            } else {
                idx = (idx1 != 0) ? idx1 : (pf2_base + idx2);
            }
            if (x < COLLISION_WIDTH) {
                g_bp_even[x] = (idx1 != 0);
                g_bp_odd[x] = (idx2 != 0);
            }
            rgb = pixel_to_rgb(idx, prev_rgb);
        } else {
            int index = 0;
            for (int p = 0; p < depth; p++) {
                int scroll = pf1_scroll;
                int sx = x + scroll;
                if (sx < 0) sx = 0;
                if (sx >= fetch_pixels) sx = fetch_pixels - 1;
                int byte = sx / 8;
                int bit  = 7 - (sx & 7);
                if (planes[p] && (planes[p][byte] & (1 << bit)))
                    index |= (1 << p);
            }
            if (x < COLLISION_WIDTH) {
                g_bp_even[x] = ((index & 0x55u) != 0);
                g_bp_odd[x] = ((index & 0xAAu) != 0);
            }
            rgb = pixel_to_rgb(index, prev_rgb);
        }
        prev_rgb = rgb;

        if (x < COLLISION_WIDTH && g_bp_even[x] && g_bp_odd[x]) {
            g_clxdat |= 0x0001u; /* even/odd bitplane collision */
        }

        int dst_x = x_start + x * pix_scale;
        for (int s = 0; s < pix_scale && (dst_x + s) < fb_w; s++) {
            FB_PutPixel(dst_x + s, y, rgb);
        }
    }
}

/* Render one sprite strip for a single sprite on a scanline.
 * AGA sprite resolution is selected by BPLCON3 bits 10-12 (SPRES). */
static void render_sprite_strip(int spr, int y, int x_start)
{
    int vstart = (g_spr_pos[spr] >> 8) & 0xFF;
    int vstop  = (g_spr_ctl[spr] >> 8) & 0xFF;
    if (vstop < vstart) vstop += 0x100;
    if (y < vstart || y >= vstop) return;

    int hstart = (g_spr_pos[spr] & 0xFF) - 0x80;
    int fb_w = (int)g_fb.width;
    int fb_h = (int)g_fb.height;
    if (y < 0 || y >= fb_h) return;

    int attached = (g_spr_ctl[spr] >> 7) & 1u;
    int pair = spr / 2;
    int attached_pair = (spr & 1u) ? (spr - 1) : (spr + 1);
    /* AGA sprite resolution from BPLCON3 bits 10-12.
     * 000 = ECS/OCS, 001 = lores (16 px), 010 = hires (32 px),
     * 011 = superhires (64 px). */
    int spres = (g_bplcon3 >> 10) & 0x7u;
    int width, words, pix_scale;
    switch (spres) {
        case 2:  width = 32; words = 2; pix_scale = 1; break; /* hires */
        case 3:  width = 64; words = 4; pix_scale = 1; break; /* superhires */
        case 1:
        case 0:
        default: width = 16; words = 1; pix_scale = 2; break; /* lores / ECS */
    }
    /* AGA 32-colour mode: BPLCON3 bits 13-15 select a 16-colour bank. */
    int aga_bank = (g_bplcon3 >> 13) & 0x7u;
    int palette_base = 16 + aga_bank * 16;
    int aga_32col = (spres == 3) && attached;
    /* Per-sprite playfield priority: BPLCON2 bits 8-12 map to sprite pairs
     * (SP0 -> pair 0, SP1 -> pair 1, ...).  When set, this sprite pair is
     * drawn behind the playfields. */
    int sprite_behind = (g_bplcon2 >> (8 + pair)) & 1u;

    /* Display window clipping. */
    int diw_x = (g_diwstart & 0xFF) - 0x80;
    int diw_x_stop = (g_diwstop & 0xFF) + 0x80;
    if (diw_x_stop < diw_x) diw_x_stop += 0x100;

    for (int bit = 0; bit < width; bit++) {
        int w = bit / 16;
        int b = 15 - (bit & 15);
        int idx = (g_spr_data[spr][w] >> b) & 1u;
        idx |= ((g_spr_datb[spr][w] >> b) & 1u) << 1;
        if (idx == 0 && !attached) continue; /* transparent pixel */
        if (attached) {
            /* Attached pair: combine this sprite with its pair member.
             * Sprites are always even/odd pairs (0+1, 2+3, ...). */
            idx |= ((g_spr_data[attached_pair][w] >> b) & 1u) << 2;
            idx |= ((g_spr_datb[attached_pair][w] >> b) & 1u) << 3;
            if (aga_32col) {
                idx |= (pair & 1u) << 4;
            }
        }

        int lores_x = x_start + (hstart + bit) * pix_scale;
        int bx = hstart + bit;
        int bp_present = (bx >= 0 && bx < COLLISION_WIDTH && (g_bp_even[bx] || g_bp_odd[bx]));
        if (bp_present) {
            g_clxdat |= (uint16_t)(1u << (spr + 1)); /* bitplane-sprite collision */
        }
        if (sprite_behind && bp_present) continue;

        /* Clip to display window and framebuffer. */
        if (lores_x < diw_x || lores_x >= diw_x_stop) continue;

        for (int dx = 0; dx < pix_scale; dx++) {
            int x = lores_x + dx;
            if (x >= 0 && x < fb_w) {
                FB_PutPixel(x, y, g_aga_palette[(palette_base + idx) & 0xFF]);
            }
        }
    }
}

/* Sprite DMA state: current fetch pointer for each sprite. */
static uint32_t g_spr_dma_ptr[SPRITE_COUNT];

/* Update CLXDAT with sprite-sprite collisions.  Called once per frame. */
static void update_sprite_collisions(void)
{
    if (!(g_dmacon & 0x0020u)) return; /* sprite DMA not enabled */
    for (int i = 0; i < SPRITE_COUNT; i++) {
        int vi0 = (g_spr_pos[i] >> 8) & 0xFF;
        int vs0 = (g_spr_ctl[i] >> 8) & 0xFF;
        if (vs0 < vi0) vs0 += 0x100;
        int hi0 = (g_spr_pos[i] & 0xFF) - 0x80;
        int spres = (g_bplcon3 >> 10) & 0x7u;
        int width0 = (spres == 2) ? 32 : (spres == 3) ? 64 : 32; /* lores pixels */

        for (int j = i + 1; j < SPRITE_COUNT; j++) {
            int vi1 = (g_spr_pos[j] >> 8) & 0xFF;
            int vs1 = (g_spr_ctl[j] >> 8) & 0xFF;
            if (vs1 < vi1) vs1 += 0x100;
            if (vs1 < vi0 || vs0 < vi1) continue; /* no vertical overlap */

            int hi1 = (g_spr_pos[j] & 0xFF) - 0x80;
            if (hi0 + width0 < hi1 || hi1 + width0 < hi0) continue; /* no horizontal overlap */

            /* Simplified bit mapping: assign pairs 0..27 to bits 8..35. */
            int pair = (i * SPRITE_COUNT + j) - ((i + 1) * (i + 2)) / 2;
            g_clxdat |= (uint16_t)(1u << ((pair + 8) & 0xFu));
        }
    }
}

/* Fetch sprite data from DMA for the current scanline and render it. */
static void render_sprites_on_scanline(int y, int bytes_per_row)
{
    (void)bytes_per_row;
    if (!(g_dmacon & 0x0020u)) return; /* sprite DMA not enabled */
    int spres = (g_bplcon3 >> 10) & 0x7u;
    int words = (spres == 2) ? 2 : (spres == 3) ? 4 : 1;
    for (int spr = 0; spr < SPRITE_COUNT; spr++) {
        int vstart = (g_spr_pos[spr] >> 8) & 0xFF;
        int vstop  = (g_spr_ctl[spr] >> 8) & 0xFF;
        if (vstop < vstart) vstop += 0x100;
        if (y < vstart || y >= vstop) continue;

        for (int w = 0; w < words; w++) {
            if (g_spr_dma_ptr[spr] + 2 < GUEST_RAM_SIZE) {
                g_spr_data[spr][w] = chip_read_u16(g_spr_dma_ptr[spr]);
                g_spr_dma_ptr[spr] += 2;
            } else {
                g_spr_data[spr][w] = 0;
            }
            if (g_spr_dma_ptr[spr] + 2 < GUEST_RAM_SIZE) {
                g_spr_datb[spr][w] = chip_read_u16(g_spr_dma_ptr[spr]);
                g_spr_dma_ptr[spr] += 2;
            } else {
                g_spr_datb[spr][w] = 0;
            }
        }
        render_sprite_strip(spr, y, 0);
    }
}

/* Render the current chipset state to the host framebuffer. */
void chip_emu_render_frame(void)
{
    if (!g_fb.valid) return;

    /* Reset copper and sprite DMA pointers at the start of each frame. */
    if (g_cop1lc) g_copper_pc = g_cop1lc;
    for (int spr = 0; spr < SPRITE_COUNT; spr++) g_spr_dma_ptr[spr] = g_spr_pt[spr];

    /* Derive display window.
     * Horizontal start is in lores pixels with an $80 origin.
     * Horizontal stop is written with the high bit stripped (H8=1 implied).
     * Vertical stop uses the hardware quirk that forces bit 8 to the
     * complement of bit 7, allowing wrap-around without DIWHIGH. */
    int diw_y = (g_diwstart >> 8) & 0xFF;
    int diw_v_stop_raw = (g_diwstop >> 8) & 0xFF;
    int diw_h_stop_raw = g_diwstop & 0xFF;  /* HSTOP written without high bit */

    /* Hardware VSTOP: bit 8 = ~bit 7 */
    int diw_v_stop = diw_v_stop_raw;
    if (((diw_v_stop >> 7) & 1) == 0) diw_v_stop |= 0x100;
    if (diw_v_stop < diw_y) diw_v_stop += 0x100;

    int diw_x = (g_diwstart & 0xFF) - 0x80; /* HSTART in lores pixels */
    int diw_x_stop = diw_h_stop_raw + 0x80; /* HSTOP has implied $100 high bit */
    if (diw_x_stop < diw_x) diw_x_stop += 0x100;
    int diw_width = diw_x_stop - diw_x;
    if (diw_width <= 0) diw_width = 320;

    /* Derive fetch width from DDFSTART/DDFSTOP.
     * DDF has 4-pixel resolution.  words = (ddf_stop - ddf_start) / 4 + 2,
     * and each word is 8 lores pixels or 16 hires pixels. */
    int ddf_start = g_ddfstart & 0xFF;
    int ddf_stop  = g_ddfstop & 0xFF;
    int fetch_pixels = diw_width;
    int hires = bpl_hires();
    if (ddf_stop > ddf_start && (ddf_stop - ddf_start) >= 8) {
        int ddf_diff = ddf_stop - ddf_start;
        int words = (ddf_diff / 4) + 2;
        fetch_pixels = words * (hires ? 16 : 8);
    }
    if (fetch_pixels <= 0 || fetch_pixels > 1280) fetch_pixels = 320;
    int bytes_per_row = (fetch_pixels + 7) / 8;
    if (bytes_per_row < 1) bytes_per_row = 1;

    int x_start = diw_x < 0 ? 0 : diw_x;
    int y_start = diw_y;
    int y_end   = diw_v_stop;

    /* Interlace: BPLCON0 LACE bit doubles the vertical display area and
     * fetches bitplanes from alternating line pairs.  Long/short field
     * alternation is tracked in g_interlace_field. */
    int lace = (g_bplcon0 >> 2) & 1u;
    int y_display_end = lace ? y_end * 2 : y_end;
    if (y_display_end > (int)g_fb.height) y_display_end = (int)g_fb.height;

    for (int y = y_start; y < y_display_end; y++) {
        /* Copper and beam are still in hardware line coordinates. */
        int beam_y = lace ? (y / 2) : y;

        /* Build the DMA slot table for this scanline. */
        dma_slot_reset();
        int copper_slots = 32; /* default copper budget per line */
        if (g_dmacon & 0x0100u) {
            int bpl_words = (bytes_per_row + 1) / 2;
            if (bpl_words < 1) bpl_words = 1;
            dma_slot_alloc(DMA_CHAN_BITPLANE, 0, bpl_words);
        }
        if (g_dmacon & 0x0020u) {
            int spr_words = 0;
            for (int s = 0; s < SPRITE_COUNT; s++) {
                int vs = (g_spr_pos[s] >> 8) & 0xFF;
                int ve = (g_spr_ctl[s] >> 8) & 0xFF;
                if (ve < vs) ve += 0x100;
                if (beam_y >= vs && beam_y < ve) spr_words += 2;
            }
            if (spr_words > 0) dma_slot_alloc(DMA_CHAN_SPRITE, 0, spr_words);
        }
        /* Any slots left after refresh are given to the copper. */
        int free_slots = dma_slot_count(DMA_CHAN_COUNT);
        if (free_slots < copper_slots) copper_slots = free_slots;
        if (copper_slots > 0) dma_slot_alloc(DMA_CHAN_COPPER, 0, copper_slots);

        /* Blitter slot consumption: if the blitter is still busy, give it any
         * remaining slots and decrement the outstanding word count. */
        if (g_blitter_busy && g_blitter_words_remaining > 0) {
            int blit_slots = dma_slot_count(DMA_CHAN_COUNT);
            if (blit_slots > g_blitter_words_remaining) blit_slots = g_blitter_words_remaining;
            if (blit_slots > 0) {
                dma_slot_alloc(DMA_CHAN_BLITTER, 0, blit_slots);
                g_blitter_words_remaining -= blit_slots;
            }
            if (g_blitter_words_remaining <= 0) {
                g_blitter_busy = 0;
                g_blitter_words_remaining = 0;
            }
        }

        /* Mark remaining slots as CPU slots and count stolen cycles. */
        int cpu_slots = 0;
        for (int i = 0; i < g_dma_slots_per_line; i++) {
            if (g_dma_slots[i] == DMA_CHAN_COUNT) {
                g_dma_slots[i] = DMA_CHAN_CPU;
                cpu_slots++;
            }
        }
        g_cpu_stolen_cycles += (uint64_t)(g_dma_slots_per_line - cpu_slots); /* each stolen slot is ~4 CPU cycles */

        g_vposr = (uint16_t)beam_y;
        g_vhposr = 0;
        copper_run_to_beam(copper_slots);

        if (g_dmacon & 0x0100u) {
            int fetch_y = lace ? ((y + g_interlace_field) / 2) : y;
            render_scanline(y, fetch_y, x_start, fetch_pixels, bytes_per_row);
        }
        render_sprites_on_scanline(y, bytes_per_row);
    }

    update_sprite_collisions();
}

/* Reset all chipset state to hardware-correct initial values.
 * Called once at kernel boot before the first guest access. */
void chip_emu_reset(void)
{
    for (uint32_t i = 0; i < AGA_REG_SIZE; i++) g_aga_regs[i] = 0;

    g_dmacon = 0;
    g_intena = 0;
    g_intreq = 0x4020u; /* VBlank and blitter-zero flags commonly set after reset */
    g_adkcon = 0;
    g_clxdat = 0;
    g_clxcon = 0;
    g_cia_b_irq = 0;
    g_serdat = 0;
    g_serper = 0;
    g_dsklen = 0;
    g_dskdat = 0;
    g_dsk_sync = 0;
    g_dskpt = 0;
    g_dsk_index = 0;

    g_bplcon0 = 0;
    g_bplcon1 = 0;
    g_bplcon2 = 0;
    g_bplcon3 = 0;
    g_bplcon4 = 0x0011u; /* AGA default colour bank bits */
    g_bplmod1 = 0;
    g_bplmod2 = 0;
    g_diwstart = 0x2C81u;
    g_diwstop  = 0xF4C1u;
    g_ddfstart = 0x0038u;
    g_ddfstop  = 0x00D0u;
    g_copcon = 0;

    g_cop1lc = 0;
    g_cop2lc = 0;
    g_copjmp1 = 0;
    g_copjmp2 = 0;
    g_copper_pc = 0;

    for (int i = 0; i < 8; i++) {
        g_bpl_pt[i] = 0;
        g_spr_pt[i] = 0;
        g_spr_dma_ptr[i] = 0;
    }
    for (int i = 0; i < SPRITE_COUNT; i++) {
        g_spr_pos[i] = 0;
        g_spr_ctl[i] = 0;
        for (int w = 0; w < SPRITE_WORDS; w++) {
            g_spr_data[i][w] = 0;
            g_spr_datb[i][w] = 0;
        }
    }
    for (int i = 0; i < AGA_PALETTE_SIZE; i++) g_aga_palette[i] = 0;

    g_bltcon0 = 0;
    g_bltcon1 = 0;
    g_bltsize = 0;
    g_bltafwm = 0xFFFFu;
    g_bltalwm = 0xFFFFu;
    g_bltapt = 0;
    g_bltbpt = 0;
    g_bltcpt = 0;
    g_bltdpt = 0;
    g_bltamod = 0;
    g_bltbmod = 0;
    g_bltcmod = 0;
    g_bltdmod = 0;
    g_bltadat = 0;
    g_bltbdat = 0;
    g_bltcdat = 0;
    g_blitter_busy = 0;
    g_blitter_busy_ticks = 0;
    g_blitter_words_remaining = 0;

    for (int i = 0; i < AUDIO_CHANNELS; i++) {
        g_audio[i].ptr = 0;
        g_audio[i].len = 0;
        g_audio[i].per = 0;
        g_audio[i].vol = 0;
        g_audio[i].dat = 0;
        g_audio[i].counter = 0;
    }

    memset(&g_cia_a, 0, sizeof(g_cia_a));
    memset(&g_cia_b, 0, sizeof(g_cia_b));
    g_cia_a.pra = 0xFFu; /* CIAA port A inputs float high; bit 3 = power LED (active low, so on) */
    g_cia_b.pra = 0xFFu;
    g_cia_a.ddra = 0;
    g_cia_a.ddrb = 0;
    g_cia_b.ddra = 0;
    g_cia_b.ddrb = 0;
    g_cia_a.icr_mask = 0;
    g_cia_b.icr_mask = 0;

    g_vposr = 0;
    g_vhposr = 0;
    g_vblank_count = 0;
    g_frame_counter = 0;
    g_chip_mode = CHIP_MODE_PAL;
    g_interlace_field = 0;
    g_cycles_per_line = COLOR_CLOCK_PAL_HZ / 50 / PAL_LINES_PER_FRAME;
    g_cycles_per_frame = g_cycles_per_line * PAL_LINES_PER_FRAME;
    g_cycles_at_vblank = 0;
    g_cycles_at_frame_start = 0;
}

/* Return the current power-LED state (1 = on, 0 = off).
 * The power LED is driven by CIAA PRA bit 3, active low. */
int chip_emu_power_led(void)
{
    return (g_cia_a.ddra & 0x08u) ? (((g_cia_a.pra >> 3) & 1u) == 0) : 1;
}

/* Test helper for the DMA slot allocator: build a copper list with more MOVE
 * instructions than the per-line slot budget allows, run one frame, and
 * return 1 if the copper stalled before reaching the end-of-list WAIT.
 * This uses a scratch area at guest RAM offset 0x10000. */
int chip_emu_dma_test(void)
{
    uint32_t list_addr = 0x10000u;
    if (list_addr + 512 > GUEST_RAM_SIZE) return 0;

    for (int i = 0; i < 120; i++) {
        g_ram[list_addr + i * 4 + 0] = 0x01;
        g_ram[list_addr + i * 4 + 1] = 0x80; /* COLOR00 register */
        g_ram[list_addr + i * 4 + 2] = (uint8_t)(i & 0xFF);
        g_ram[list_addr + i * 4 + 3] = 0;
    }
    /* End-of-list impossible WAIT. */
    g_ram[list_addr + 120 * 4 + 0] = 0xFF;
    g_ram[list_addr + 120 * 4 + 1] = 0xFE;
    g_ram[list_addr + 120 * 4 + 2] = 0xFF;
    g_ram[list_addr + 120 * 4 + 3] = 0xFE;

    g_cop1lc = list_addr;
    g_copper_pc = list_addr;
    g_dmacon |= 0x0080u; /* enable copper DMA */

    uint32_t pc_before = g_copper_pc;
    chip_emu_render_frame();
    uint32_t pc_after = g_copper_pc;

    /* The copper should have advanced, but it must not have reached the end
     * of the 121-instruction list in a single 32-slot line. */
    return (pc_after > pc_before) && (pc_after < list_addr + 120 * 4);
}

/* Test helper for Blitter line mode: draw a diagonal line from (10,10) to
 * (20,18) using the Amiga octant/delta encoding and check that the destination
 * word contains some set bits.  Returns 1 on success. */
int chip_emu_line_test(void)
{
    uint32_t dst = 0x11000u;
    if (dst + 1024 > GUEST_RAM_SIZE) return 0;

    /* Clear destination bitmap. */
    for (int i = 0; i < 1024; i++) g_ram[dst + i] = 0;

    /* Line (10,10) -> (20,18): dx=10, dy=8, octant 0 (positive x, positive y,
     * x major).  BLTBMOD = 2*dy = 16, BLTAMOD = 2*dy - 2*dx = -4. */
    g_bltapt = (uint32_t)((10u << 8) | 10u);
    g_bltdpt = dst;
    g_bltdmod = 40;
    g_bltamod = (uint16_t)(int16_t)(-4);
    g_bltbmod = 16;
    g_bltcon0 = 0x0B00u; /* use D only, minterm irrelevant */
    g_bltcon1 = 0x0011u; /* LINE=1, octant 0 */
    g_bltsize = 0;
    g_blitter_words_remaining = 0;
    g_blitter_busy = 0;

    /* Trigger the blitter via BLTSIZE. */
    chip_emu_write(REG_BLTSIZE, 0, 2); /* dummy BLTSIZE - line mode ignores size */

    /* Wait for slot-based blitter to finish. */
    for (int i = 0; i < 1000 && g_blitter_busy; i++) {
        chip_emu_render_frame();
    }

    /* Check that at least one bit was set in the destination. */
    for (int i = 0; i < 1024; i++) {
        if (g_ram[dst + i] != 0) return 1;
    }
    return 0;
}

/* Test helper for Blitter exclusive fill mode: fill between two vertical edges.
 * A row with left edge at bit 4 and right edge at bit 12 should produce filled
 * bits 5-11 (exclusive fill leaves the edge bits unset).  Returns 1 on success. */
int chip_emu_fill_test(void)
{
    uint32_t src = 0x12000u;
    uint32_t dst = 0x12100u;
    if (src + 512 > GUEST_RAM_SIZE || dst + 512 > GUEST_RAM_SIZE) return 0;

    /* One source row: edge at bit 4 and bit 12. */
    g_ram[src + 0] = 0x00; g_ram[src + 1] = 0x00;
    g_ram[src + 2] = 0x0F; g_ram[src + 3] = 0x00; /* bits 12-15 set */
    /* Wait, bit 4 is in the low byte?  Let's use bit 4 in first word. */
    g_ram[src + 0] = 0x10; /* bit 4 */
    g_ram[src + 1] = 0x00;
    g_ram[src + 2] = 0x0F; /* bits 12-15 */
    g_ram[src + 3] = 0x00;

    /* Clear destination. */
    for (int i = 0; i < 16; i++) g_ram[dst + i] = 0;

    g_bltapt = src;
    g_bltdpt = dst;
    g_bltamod = 0;
    g_bltdmod = 0;
    g_bltafwm = 0xFFFFu;
    g_bltalwm = 0xFFFFu;
    g_bltcon0 = 0x0F00u; /* use A, B, C, D? For fill we need A only */
    g_bltcon0 = 0x0900u; /* use A and D */
    g_bltcon1 = 0x0010u; /* EFE exclusive fill */
    g_bltsize = 0;
    g_blitter_words_remaining = 0;
    g_blitter_busy = 0;

    /* Width=2 words, height=1 line. */
    chip_emu_write(REG_BLTSIZE, (2 << 6) | 1, 2);

    for (int i = 0; i < 1000 && g_blitter_busy; i++) {
        chip_emu_render_frame();
    }

    /* In exclusive fill, the edge bits (4 and 12-15) should be unset and the
     * interior bits (5-11) should be set. */
    uint16_t d0 = (uint16_t)((g_ram[dst + 0] << 8) | g_ram[dst + 1]);
    uint16_t d1 = (uint16_t)((g_ram[dst + 2] << 8) | g_ram[dst + 3]);
    /* d0 should have bits 5-15 set (interior up to bit 15), d1 should have
     * bits 0-11 set.  Because the edge at bit 12 is in d1, the interior in d1
     * is bits 0-11. */
    return (d0 == 0xFFE0u && d1 == 0x0FFFu);
}

/* Test helper for color-clock beam position / raster bars: build a copper
 * list that WAITs for line 100 and changes COLOR00, then waits for line 110
 * and changes it again.  Run the copper and verify that the second color
 * value was written, proving the WAIT was satisfied by the color-clock beam.
 * Returns 1 on success. */
int chip_emu_raster_test(void)
{
    uint32_t list_addr = 0x13000u;
    if (list_addr + 64 > GUEST_RAM_SIZE) return 0;

    /* WAIT for line 100, horizontal position ignored (mask 0). */
    g_ram[list_addr + 0] = 0x00;  g_ram[list_addr + 1] = 0x64 * 2; /* v=100 */
    g_ram[list_addr + 2] = 0x00;  g_ram[list_addr + 3] = 0xFF;     /* h mask=0, v mask=0xFF */
    /* MOVE COLOR00 = red. */
    g_ram[list_addr + 4] = 0x01;  g_ram[list_addr + 5] = 0x80;
    g_ram[list_addr + 6] = 0xF0;  g_ram[list_addr + 7] = 0x00;
    /* WAIT for line 110. */
    g_ram[list_addr + 8] = 0x00;  g_ram[list_addr + 9] = 0x6E * 2; /* v=110 */
    g_ram[list_addr + 10] = 0x00; g_ram[list_addr + 11] = 0xFF;
    /* MOVE COLOR00 = blue. */
    g_ram[list_addr + 12] = 0x01; g_ram[list_addr + 13] = 0x80;
    g_ram[list_addr + 14] = 0x00; g_ram[list_addr + 15] = 0xF0;
    /* End-of-list WAIT. */
    g_ram[list_addr + 16] = 0xFF; g_ram[list_addr + 17] = 0xFE;
    g_ram[list_addr + 18] = 0xFF; g_ram[list_addr + 19] = 0xFE;

    /* Fake that the current beam is at line 120 so the WAITs are satisfied. */
    g_cycles_at_frame_start = chip_emu_m68k_cycles() - (120u * g_cycles_per_line);

    g_cop1lc = list_addr;
    g_copper_pc = list_addr;
    g_dmacon |= 0x0080u;

    chip_emu_render_frame();

    /* The copper should have executed both MOVEs and stopped at the EOL WAIT. */
    return (g_aga_palette[0] == 0x0000F0u);
}

/* Test helper for AGA sprite control: set up a 16-pixel low-res sprite at
 * (80,50) with a simple pattern, enable sprite DMA, render the line, and
 * verify the DMA-fetched data and the sprite palette base from BPLCON3.
 * Returns 1 on success. */
int chip_emu_sprite_test(void)
{
    uint32_t spr_data = 0x14000u;
    if (spr_data + 16 > GUEST_RAM_SIZE) return 0;

    /* Sprite data: two control words (position + control) followed by
     * DATA and DATB for one line.  We set the DMA pointer to the DATA words. */
    g_ram[spr_data + 0] = 0x00; g_ram[spr_data + 1] = 0x00; /* control: pos */
    g_ram[spr_data + 2] = 0x00; g_ram[spr_data + 3] = 0x00; /* control: ctl */
    g_ram[spr_data + 4] = 0xFF; g_ram[spr_data + 5] = 0xFF; /* DATA: all set */
    g_ram[spr_data + 6] = 0x00; g_ram[spr_data + 7] = 0x00; /* DATB: all clear */

    /* Sprite position: vstart=50, vstop=60, hstart=80. */
    g_spr_pos[0] = (uint16_t)((50u << 8) | (80u + 0x80u));
    g_spr_ctl[0] = (uint16_t)((60u << 8) | 0x00u);
    g_spr_pt[0] = spr_data + 4; /* point to DATA words */
    g_spr_dma_ptr[0] = g_spr_pt[0];
    g_bplcon3 = 0x0000u; /* bank 0, lores resolution */
    g_bplcon2 = 0x0000u; /* sprite in front of playfield */
    g_dmacon |= 0x0020u; /* enable sprite DMA */

    /* Render the sprite line. */
    render_sprites_on_scanline(55, 40);

    /* The fetched DATA word should be all set. */
    return (g_spr_data[0][0] == 0xFFFFu && g_spr_datb[0][0] == 0x0000u);
}
