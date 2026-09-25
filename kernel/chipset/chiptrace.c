/* chiptrace — custom-chip / CIA access tracer (UAOS-68)
 *
 * Hot-path instrumentation for chip_emu_read()/chip_emu_write().  Emits
 * through klog only — safe inside the page-fault handler and the Musashi
 * memory callbacks because klog writes go straight to the ring + UART.
 *
 * Overhead when disabled: one flag test per access.  When enabled, a cheap
 * class-mask test runs first; consecutive repeats of the same (op, offset,
 * value) are folded into a single "xN" line and total output is capped
 * per second.
 */

#include "chipset/chiptrace.h"
#include "klog/klog.h"
#include "exec/task.h"
#include <stdint.h>
#include <stddef.h>

extern volatile uint64_t g_pit_ticks;   /* 100 Hz — uaos_kernel_main.c */

/* Musashi — mirrored from emulation/src/musashi/m68k.h (not on the include
 * path for kernel sources). */
extern int           m68k_get_reg(void *ctx, int regnum);
extern unsigned int  m68k_disassemble(char *str_buff, unsigned int pc,
                                      unsigned int cpu_type);
#define M68K_REG_PC_        16
#define M68K_CPU_TYPE_68020 4

#define CHIP_WINDOW_START 0x00B00000u
#define AGA_REG_BASE_OFF  0x002FF000u
#define CIA_B_BASE_OFF    0x000FD000u
#define CIA_A_BASE_OFF    0x000FE000u

/* ------------------------------------------------------------------ state */

static uint32_t g_mask       = 0;
static uint32_t g_emitted    = 0;      /* lines emitted since enable/clear  */
static uint32_t g_dropped    = 0;      /* lines suppressed by the rate cap  */
static uint32_t g_pc_lines   = 0;
static uint32_t g_pc_rate    = 2;      /* ticks between PC samples          */
static uint64_t g_pc_last    = 0;

/* Rate cap: max emitted lines per one-second window. */
#define CT_MAX_PER_SEC 400u
static uint32_t g_sec_emitted = 0;
static uint64_t g_sec_tick    = 0;

/* Repeat folding: last (op, offset, value) and run length. */
static int      g_rep_valid = 0;
static uint32_t g_rep_off   = 0;
static uint32_t g_rep_val   = 0;
static int      g_rep_wr    = 0;
static int      g_rep_width = 0;
static uint32_t g_rep_count = 0;
static uint32_t g_rep_pc    = 0;
static int      g_rep_haspc = 0;

/* ----------------------------------------------------- register naming */

typedef struct { uint16_t off; const char *name; } RegName;

static const RegName k_regnames[] = {
    { 0x002, "DMACONR"  }, { 0x004, "VPOSR"   }, { 0x006, "VHPOSR"  },
    { 0x008, "DSKDATR"  }, { 0x00A, "JOY0DAT" }, { 0x00C, "JOY1DAT" },
    { 0x00E, "CLXDAT"   }, { 0x010, "ADKCONR" }, { 0x012, "POT0DAT" },
    { 0x014, "POT1DAT"  }, { 0x016, "POTGOR"  }, { 0x018, "SERDATR" },
    { 0x01A, "DSKBYTR"  }, { 0x01C, "INTENAR" }, { 0x01E, "INTREQR" },
    { 0x020, "DSKPTH"   }, { 0x022, "DSKPTL"  }, { 0x024, "DSKLEN"  },
    { 0x026, "DSKDAT"   }, { 0x028, "REFPTR"  }, { 0x02A, "VPOSW"   },
    { 0x02C, "VHPOSW"   }, { 0x02E, "COPCON"  }, { 0x030, "SERDAT"  },
    { 0x032, "SERPER"   }, { 0x034, "POTGO"   }, { 0x036, "JOYTEST" },
    { 0x038, "STREQU"   }, { 0x03A, "STRVBL"  }, { 0x03C, "STRHOR"  },
    { 0x03E, "STRLONG"  },
    { 0x040, "BLTCON0"  }, { 0x042, "BLTCON1" }, { 0x044, "BLTAFWM" },
    { 0x046, "BLTALWM"  }, { 0x048, "BLTCPTH" }, { 0x04A, "BLTCPTL" },
    { 0x04C, "BLTBPTH"  }, { 0x04E, "BLTBPTL" }, { 0x050, "BLTAPTH" },
    { 0x052, "BLTAPTL"  }, { 0x054, "BLTDPTH" }, { 0x056, "BLTDPTL" },
    { 0x058, "BLTSIZE"  }, { 0x05A, "BLTCON0L"}, { 0x05C, "BLTSIZV" },
    { 0x05E, "BLTSIZH"  }, { 0x060, "BLTCMOD" }, { 0x062, "BLTBMOD" },
    { 0x064, "BLTAMOD"  }, { 0x066, "BLTDMOD" }, { 0x070, "BLTCDAT" },
    { 0x072, "BLTBDAT"  }, { 0x074, "BLTADAT" }, { 0x07C, "DENISEID"},
    { 0x07E, "DSKSYNC"  },
    { 0x080, "COP1LCH"  }, { 0x082, "COP1LCL" }, { 0x084, "COP2LCH" },
    { 0x086, "COP2LCL"  }, { 0x088, "COPJMP1" }, { 0x08A, "COPJMP2" },
    { 0x08C, "COPINS"   }, { 0x08E, "DIWSTRT" }, { 0x090, "DIWSTOP" },
    { 0x092, "DDFSTRT"  }, { 0x094, "DDFSTOP" }, { 0x096, "DMACON"  },
    { 0x098, "CLXCON"   }, { 0x09A, "INTENA"  }, { 0x09C, "INTREQ"  },
    { 0x09E, "ADKCON"   },
    { 0x100, "BPLCON0"  }, { 0x102, "BPLCON1" }, { 0x104, "BPLCON2" },
    { 0x106, "BPLCON3"  }, { 0x108, "BPL1MOD" }, { 0x10A, "BPL2MOD" },
    { 0x10C, "BPLCON4"  }, { 0x10E, "CLXCON2" },
    { 0x1C0, "HTOTAL"   }, { 0x1C2, "HSSTOP"  }, { 0x1C4, "HBSTRT"  },
    { 0x1C6, "HBSTOP"   }, { 0x1C8, "VTOTAL"  }, { 0x1CA, "VSSTOP"  },
    { 0x1CC, "VBSTRT"   }, { 0x1CE, "VBSTOP"  }, { 0x1DC, "BEAMCON0"},
    { 0x1DE, "HSSTRT"   }, { 0x1E0, "VSSTRT"  }, { 0x1E2, "HCENTER" },
    { 0x1E4, "DIWHIGH"  }, { 0x1FC, "FMODE"   },
};

static const char *const k_cia_regs[16] = {
    "PRA", "PRB", "DDRA", "DDRB", "TALO", "TAHI", "TBLO", "TBHI",
    "TODLO", "TODMID", "TODHI", "?", "SDR", "ICR", "CRA", "CRB"
};

/* Assemble "<prefix><n><suffix>" into buf (n 0-31).  When pad2 is set the
 * number is always two digits ("COLOR00"); otherwise it is minimal-width. */
static const char *ct_name3(char *buf, size_t bufsz,
                            const char *prefix, uint32_t n, const char *suffix,
                            int pad2)
{
    size_t i = 0;
    const char *p;
    for (p = prefix; *p && i + 1 < bufsz; p++) buf[i++] = *p;
    if (n >= 10 || pad2) {
        if (i + 1 < bufsz) buf[i++] = (char)('0' + n / 10);
    }
    if (i + 1 < bufsz) buf[i++] = (char)('0' + n % 10);
    for (p = suffix; *p && i + 1 < bufsz; p++) buf[i++] = *p;
    buf[i] = '\0';
    return buf;
}

/* Assemble "<prefix><name>" into buf. */
static const char *ct_name2(char *buf, size_t bufsz,
                            const char *prefix, const char *name)
{
    size_t i = 0;
    const char *p;
    for (p = prefix; *p && i + 1 < bufsz; p++) buf[i++] = *p;
    for (p = name;   *p && i + 1 < bufsz; p++) buf[i++] = *p;
    buf[i] = '\0';
    return buf;
}

/* Format a name for AUDx / BPLxPT / SPRx / COLORx ranges, else NULL. */
static const char *ct_range_name(uint32_t regoff, char *buf, size_t bufsz)
{
    static const char *const aud_fields[6] =
        { "LCH", "LCL", "LEN", "PER", "VOL", "DAT" };

    /* AUD0-3: 0x0A0 + ch*0x10 + field*2 (fields 0..5) */
    if (regoff >= 0x0A0 && regoff < 0x0E0) {
        uint32_t ch = (regoff - 0x0A0) >> 4;
        uint32_t f  = (regoff - 0x0A0) & 0xF;
        if (ch < 4 && f < 0x0C && !(f & 1))
            return ct_name3(buf, bufsz, "AUD", ch, aud_fields[f >> 1], 0);
        return NULL;
    }
    /* BPL1-8PT H/L: 0x0E0-0x0FF */
    if (regoff >= 0x0E0 && regoff < 0x100 && !(regoff & 1)) {
        uint32_t r = (regoff - 0x0E0) >> 1;
        return ct_name3(buf, bufsz, "BPL", (r >> 1) + 1,
                        (r & 1) ? "PTL" : "PTH", 0);
    }
    /* SPR0-7PT H/L: 0x120-0x13F */
    if (regoff >= 0x120 && regoff < 0x140 && !(regoff & 1)) {
        uint32_t r = (regoff - 0x120) >> 1;
        return ct_name3(buf, bufsz, "SPR", r >> 1, (r & 1) ? "PTL" : "PTH", 0);
    }
    /* SPRxPOS/CTL/DATA/DATB: 0x140-0x17F */
    if (regoff >= 0x140 && regoff < 0x180 && !(regoff & 1)) {
        static const char *const spr_fields[4] = { "POS", "CTL", "DATA", "DATB" };
        uint32_t r = (regoff - 0x140) >> 1;
        return ct_name3(buf, bufsz, "SPR", r >> 2, spr_fields[r & 3], 0);
    }
    /* COLOR00-31: 0x180-0x1BF */
    if (regoff >= 0x180 && regoff < 0x1C0 && !(regoff & 1))
        return ct_name3(buf, bufsz, "COLOR", (regoff - 0x180) >> 1, "", 1);
    return NULL;
}

/* Resolve a name for a chip-window offset.  Returns a static literal or
 * fills 'buf'.  'abs' receives the display address (DFFxxx/BFDxxx/BFExxx). */
static const char *ct_reg_name(uint32_t off, char *buf, size_t bufsz,
                               uint32_t *abs)
{
    /* CIA-B: BFD000-BFDFFF, CIA-A: BFE000-BFEFFF */
    if (off >= CIA_B_BASE_OFF && off < CIA_B_BASE_OFF + 0x1000) {
        *abs = 0x00BFD000u + (off - CIA_B_BASE_OFF);
        return ct_name2(buf, bufsz, "CIA-B.",
                        k_cia_regs[(off >> 8) & 0xF]);
    }
    if (off >= CIA_A_BASE_OFF && off < CIA_A_BASE_OFF + 0x1000) {
        *abs = 0x00BFE000u + (off - CIA_A_BASE_OFF);
        return ct_name2(buf, bufsz, "CIA-A.",
                        k_cia_regs[(off >> 8) & 0xF]);
    }

    /* Custom-chip register offset (either direct regoff <0x1000 or
     * AGA-window absolute). */
    uint32_t regoff;
    if (off >= AGA_REG_BASE_OFF && off < AGA_REG_BASE_OFF + 0x1000)
        regoff = off - AGA_REG_BASE_OFF;
    else if (off < 0x1000)
        regoff = off;
    else {
        *abs = CHIP_WINDOW_START + off;
        return NULL;
    }

    *abs = 0x00DFF000u + regoff;

    for (size_t i = 0; i < sizeof(k_regnames) / sizeof(k_regnames[0]); i++)
        if (k_regnames[i].off == regoff)
            return k_regnames[i].name;

    return ct_range_name(regoff, buf, bufsz);
}

/* ------------------------------------------------------- classification */

static uint32_t ct_classify(uint32_t off)
{
    if (off >= CIA_B_BASE_OFF && off < CIA_A_BASE_OFF + 0x1000)
        return CT_CIA;

    uint32_t regoff;
    if (off >= AGA_REG_BASE_OFF && off < AGA_REG_BASE_OFF + 0x1000)
        regoff = off - AGA_REG_BASE_OFF;
    else if (off < 0x1000)
        regoff = off;
    else
        return CT_CHIP;   /* non-register window space — lump with chip */

    if (regoff >= 0x0A0 && regoff < 0x0E0)
        return CT_PAULA;
    if (regoff == 0x008 || regoff == 0x01A ||
        (regoff >= 0x020 && regoff <= 0x026) || regoff == 0x07E)
        return CT_DISK;
    return CT_CHIP;
}

/* ------------------------------------------------------------ emission */

static void ct_emit_line(uint32_t off, uint32_t val, int width, int wr,
                         uint32_t rep, uint32_t pc, int haspc)
{
    char nbuf[24];
    uint32_t abs = 0;
    const char *name = ct_reg_name(off, nbuf, sizeof(nbuf), &abs);
    (void)width;

    if (wr) {
        if (rep > 1)
            klog_emit(KLOG_CHIP, KLOG_INFO, "W %06X %s = 0x%x  x%u",
                      abs, name ? name : "?", val, rep);
        else if (haspc)
            klog_emit(KLOG_CHIP, KLOG_INFO, "W %06X %s = 0x%x  pc=%06X",
                      abs, name ? name : "?", val, pc);
        else
            klog_emit(KLOG_CHIP, KLOG_INFO, "W %06X %s = 0x%x",
                      abs, name ? name : "?", val);
    } else {
        if (rep > 1)
            klog_emit(KLOG_CHIP, KLOG_INFO, "R %06X %s -> 0x%x  x%u",
                      abs, name ? name : "?", val, rep);
        else if (haspc)
            klog_emit(KLOG_CHIP, KLOG_INFO, "R %06X %s -> 0x%x  pc=%06X",
                      abs, name ? name : "?", val, pc);
        else
            klog_emit(KLOG_CHIP, KLOG_INFO, "R %06X %s -> 0x%x",
                      abs, name ? name : "?", val);
    }
}

static int ct_rate_ok(void)
{
    uint64_t now = g_pit_ticks;
    if (now != g_sec_tick) { g_sec_tick = now; g_sec_emitted = 0; }
    if (g_sec_emitted >= CT_MAX_PER_SEC) { g_dropped++; return 0; }
    g_sec_emitted++;
    return 1;
}

/* Flush a pending repeat run. */
static void ct_flush_rep(void)
{
    if (!g_rep_valid)
        return;
    if (ct_rate_ok()) {
        ct_emit_line(g_rep_off, g_rep_val, g_rep_width, g_rep_wr,
                     g_rep_count, g_rep_pc, g_rep_haspc);
        g_emitted++;
    } else {
        g_dropped += g_rep_count - 1;   /* folded hits count as dropped */
    }
    g_rep_valid = 0;
}

static uint32_t ct_guest_pc(int *haspc)
{
    UaosTask *t = Task_Current();
    if (t && t->type == TASK_TYPE_M68K) {
        *haspc = 1;
        return (uint32_t)m68k_get_reg(NULL, M68K_REG_PC_);
    }
    *haspc = 0;
    return 0;
}

/* -------------------------------------------------------------- public */

void Chiptrace_Hit(uint32_t offset, uint32_t value, int width, int is_write)
{
    if (!g_mask)
        return;
    if (!(ct_classify(offset) & g_mask))
        return;

    int    haspc;
    uint32_t pc = ct_guest_pc(&haspc);

    /* Fold consecutive identical (op, offset, value) accesses. */
    if (g_rep_valid && g_rep_off == offset && g_rep_wr == is_write &&
        g_rep_val == value) {
        g_rep_count++;
        g_rep_pc = pc;
        return;
    }
    ct_flush_rep();

    g_rep_valid = 1;
    g_rep_off   = offset;
    g_rep_val   = value;
    g_rep_wr    = is_write;
    g_rep_width = width;
    g_rep_count = 1;
    g_rep_pc    = pc;
    g_rep_haspc = haspc;
}

void Chiptrace_PcSample(void)
{
    if (!(g_mask & CT_PC) || !g_pc_rate)
        return;

    uint64_t now = g_pit_ticks;
    if (now - g_pc_last < g_pc_rate)
        return;
    g_pc_last = now;

    uint32_t pc = (uint32_t)m68k_get_reg(NULL, M68K_REG_PC_);
    if (!pc || pc >= 0x00B00000u)   /* don't disasm outside guest RAM */
        return;

    char buf[96];
    m68k_disassemble(buf, pc, M68K_CPU_TYPE_68020);
    klog_emit(KLOG_CHIP, KLOG_INFO, "PC %06X  %s", pc, buf);
    g_pc_lines++;
}

void Chiptrace_Enable(uint32_t mask)
{
    ct_flush_rep();
    g_emitted = g_dropped = g_pc_lines = 0;
    g_sec_emitted = 0;
    if (!mask) {
        g_mask = 0;
        klog_emit(KLOG_CHIP, KLOG_INFO, "chiptrace off");
        return;
    }
    g_mask = mask;
    klog_emit(KLOG_CHIP, KLOG_INFO, "chiptrace on mask=0x%x", mask);
}

void Chiptrace_Disable(void)
{
    ct_flush_rep();
    if (g_mask)
        klog_emit(KLOG_CHIP, KLOG_INFO,
                  "chiptrace off emitted=%u dropped=%u pc=%u",
                  g_emitted, g_dropped, g_pc_lines);
    g_mask = 0;
    g_rep_valid = 0;
}

int Chiptrace_Enabled(void)
{
    return g_mask != 0;
}

void Chiptrace_Stats(uint32_t *mask, uint32_t *emitted,
                     uint32_t *dropped, uint32_t *pc_lines)
{
    if (mask)     *mask     = g_mask;
    if (emitted)  *emitted  = g_emitted;
    if (dropped)  *dropped  = g_dropped;
    if (pc_lines) *pc_lines = g_pc_lines;
}

void Chiptrace_ClearStats(void)
{
    g_emitted = g_dropped = g_pc_lines = 0;
}

void Chiptrace_SetPcRate(uint32_t ticks_per_line)
{
    g_pc_rate = ticks_per_line;
}

uint32_t Chiptrace_GetPcRate(void)
{
    return g_pc_rate;
}
