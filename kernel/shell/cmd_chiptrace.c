/* cmd_chiptrace.c — C:chiptrace — custom-chip / CIA access tracer
 *
 * Instruments chip_emu_read()/chip_emu_write() and emits decoded register
 * accesses through klog (serial + ring buffer) — never the WM paint path.
 * Optionally samples the M68k program counter with the Musashi
 * disassembler at a bounded rate.
 *
 *   chiptrace                    show status
 *   chiptrace ON                 trace all classes
 *   chiptrace OFF                stop tracing
 *   chiptrace <class> [ON|OFF]   toggle one class:
 *                                  CHIP  — custom-chip registers
 *                                  CIA   — CIA-A/B registers
 *                                  PAULA — Paula audio regs (AUD0-3)
 *                                  DISK  — floppy/disk regs (DSK*)
 *   chiptrace PC [N]             toggle M68k PC disasm sampling
 *                                (one line every N ticks, default 2;
 *                                 N=0 turns sampling off)
 *   chiptrace CLEAR              zero emitted/dropped counters
 *
 * Output goes to klog: watch serial or use `dmesg chip`.
 */

#include "cmd_internal.h"
#include "chipset/chiptrace.h"
#include "chipset/chip_emu.h"

/* Case-insensitive prefix match: does 's' start with 'prefix'? */
static int ct_starts_ci(const char *s, const char *prefix)
{
    while (*prefix) {
        char a = *s, b = *prefix;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
        s++; prefix++;
    }
    return 1;
}

static uint32_t ct_parse_u32(const char *p, int *ok)
{
    uint32_t n = 0;
    if (!*p) { *ok = 0; return 0; }
    while (*p >= '0' && *p <= '9') {
        n = n * 10 + (uint32_t)(*p - '0');
        p++;
    }
    *ok = (*p == '\0' || *p == ' ');
    return n;
}

static void ct_print_status(NativeCmdCtx *ctx)
{
    uint32_t mask, emitted, dropped, pc_lines;
    Chiptrace_Stats(&mask, &emitted, &dropped, &pc_lines);

    char line[CMD_MAX_LINE], num[24];
    cmd_scopy(line, "chiptrace: ", CMD_MAX_LINE);
    cmd_scat(line, Chiptrace_Enabled() ? "ON" : "off", CMD_MAX_LINE);
    cmd_scat(line, "  classes=[", CMD_MAX_LINE);
    cmd_scat(line, (mask & CT_CHIP)  ? "chip "  : "", CMD_MAX_LINE);
    cmd_scat(line, (mask & CT_CIA)   ? "cia "   : "", CMD_MAX_LINE);
    cmd_scat(line, (mask & CT_PAULA) ? "paula " : "", CMD_MAX_LINE);
    cmd_scat(line, (mask & CT_DISK)  ? "disk"   : "", CMD_MAX_LINE);
    cmd_scat(line, "]  pc-sampling=", CMD_MAX_LINE);
    cmd_scat(line, (mask & CT_PC) ? "on" : "off", CMD_MAX_LINE);
    PRINT(line);

    cmd_scopy(line, "  emitted=", CMD_MAX_LINE);
    cmd_uint_to_dec(emitted, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
    cmd_scat(line, "  dropped=", CMD_MAX_LINE);
    cmd_uint_to_dec(dropped, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
    cmd_scat(line, "  pc-lines=", CMD_MAX_LINE);
    cmd_uint_to_dec(pc_lines, num, sizeof(num)); cmd_scat(line, num, CMD_MAX_LINE);
    PRINT(line);
}

void Cmd_Chiptrace(NativeCmdCtx *ctx, const char *args)
{
    if (!args || !*args) {
        ct_print_status(ctx);
        return;
    }

    if (cmd_seq_ci(args, "on") || cmd_seq_ci(args, "all")) {
        Chiptrace_Enable(CT_ALLREG);
        PRINT("chiptrace: tracing all register classes — watch serial/dmesg chip");
        return;
    }
    if (cmd_seq_ci(args, "off") || cmd_seq_ci(args, "stop")) {
        Chiptrace_Disable();
        PRINT("chiptrace: off");
        return;
    }
    if (cmd_seq_ci(args, "clear")) {
        Chiptrace_ClearStats();
        PRINT("chiptrace: counters cleared.");
        return;
    }
    if (cmd_seq_ci(args, "test")) {
        /* Self-test: emit one access per class through chip_emu itself.
         * Registers chosen to be harmless: COLOR00 (palette), INTENAR
         * (read-only), CIA-B DDRA, AUD0VOL=0 (mute), DSKSYNC standard. */
        uint32_t prev, d0, d1, d2;
        Chiptrace_Stats(&prev, &d0, &d1, &d2);
        Chiptrace_Enable(CT_ALLREG);
        chip_emu_write(0x2FF000 + 0x180, 0x0F0F, 2);   /* COLOR00 */
        (void)chip_emu_read(0x2FF000 + 0x01C, 2);      /* INTENAR */
        (void)chip_emu_read(0x2FF000 + 0x01C, 2);      /* again: folds */
        (void)chip_emu_read(0x2FF000 + 0x01C, 2);      /* again: folds */
        chip_emu_write(0xFD200, 0xFF, 1);              /* CIA-B DDRA */
        chip_emu_write(0x2FF000 + 0x0A8, 0, 2);        /* AUD0VOL=0 */
        chip_emu_write(0x2FF000 + 0x07E, 0x4489, 2);   /* DSKSYNC */
        if (prev) Chiptrace_Enable(prev); else Chiptrace_Disable();
        PRINT("chiptrace: test accesses emitted — check dmesg chip");
        return;
    }

    /* Class toggles: "chip", "cia off", "paula on", ... */
    static const struct { const char *name; uint32_t bit; } k_classes[] = {
        { "chip",  CT_CHIP  },
        { "cia",   CT_CIA   },
        { "paula", CT_PAULA },
        { "disk",  CT_DISK  },
    };
    for (int i = 0; i < 4; i++) {
        if (!ct_starts_ci(args, k_classes[i].name))
            continue;
        const char *rest = args + cmd_slen(k_classes[i].name);
        while (*rest == ' ') rest++;
        uint32_t mask, d0, d1, d2;
        Chiptrace_Stats(&mask, &d0, &d1, &d2);
        int enable = 1;
        if (cmd_seq_ci(rest, "off") || cmd_seq_ci(rest, "0"))
            enable = 0;
        else if (*rest && !cmd_seq_ci(rest, "on") && !cmd_seq_ci(rest, "1")) {
            PRINT("usage: chiptrace <class> [on|off]");
            return;
        }
        mask = enable ? (mask | k_classes[i].bit) : (mask & ~k_classes[i].bit);
        if (mask)
            Chiptrace_Enable(mask);
        else
            Chiptrace_Disable();
        PRINT(enable ? "chiptrace: class enabled" : "chiptrace: class disabled");
        return;
    }

    /* PC sampling: "pc", "pc 5", "pc off" */
    if (ct_starts_ci(args, "pc")) {
        const char *rest = args + 2;
        while (*rest == ' ') rest++;
        uint32_t mask, d0, d1, d2;
        Chiptrace_Stats(&mask, &d0, &d1, &d2);

        if (cmd_seq_ci(rest, "off") || !*rest) {
            if (!*rest) {
                /* bare "pc" toggles with default rate */
                int on = !(mask & CT_PC);
                if (on) {
                    Chiptrace_SetPcRate(2);
                    Chiptrace_Enable(mask | CT_PC);
                    PRINT("chiptrace: PC sampling on (rate=2 ticks)");
                } else {
                    Chiptrace_Enable(mask & ~CT_PC);
                    PRINT("chiptrace: PC sampling off");
                }
                return;
            }
            Chiptrace_Enable(mask & ~CT_PC);
            PRINT("chiptrace: PC sampling off");
            return;
        }
        int ok;
        uint32_t n = ct_parse_u32(rest, &ok);
        if (!ok) {
            PRINT("usage: chiptrace pc [N|off]");
            return;
        }
        if (n == 0) {
            Chiptrace_Enable(mask & ~CT_PC);
            PRINT("chiptrace: PC sampling off");
            return;
        }
        Chiptrace_SetPcRate(n);
        Chiptrace_Enable(mask | CT_PC);
        PRINT("chiptrace: PC sampling on");
        return;
    }

    PRINT("usage: chiptrace [on|off|clear|test|<class> [on|off]|pc [N|off]]");
    PRINT("       classes: chip cia paula disk");
}
