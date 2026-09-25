/* cmd_irqstat.c — C:irqstat — per-vector interrupt counters
 *
 * ISR_Dispatch counts every vector that fires.  This command dumps the
 * counters together with a sampled rate, so you can tell at a glance
 * whether a device IRQ (RTC, virtio, e1000, ...) is actually firing
 * without instrumenting drivers by hand.
 *
 *   irqstat              counts + rates sampled over a 1s window
 *   irqstat <sec>        counts + rates sampled over <sec> seconds
 *   irqstat NOW          just dump the counters (no sampling delay)
 *   irqstat CLEAR        reset all counters
 */

#include "cmd_internal.h"
#include "../irq/idt.h"

extern volatile uint64_t g_pit_ticks;   /* 100 Hz — uaos_kernel_main.c */

static void irqstat_u64dec(uint64_t v, char *buf, int max)
{
    char tmp[24];
    int n = 0;
    if (v == 0) { cmd_scopy(buf, "0", max); return; }
    while (v && n < 23) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    int i = 0;
    while (n && i < max - 1) buf[i++] = tmp[--n];
    buf[i] = '\0';
}

static const char *irqstat_name(int vec)
{
    static const char *exc[32] = {
        "#DE divide",  "#DB debug",   "NMI",        "#BP break",
        "#OF ovf",     "#BR bound",   "#UD undef",  "#NM nofpu",
        "#DF double",  "coproc",      "#TS tss",    "#NP seg",
        "#SS stack",   "#GP genprot", "#PF page",   "rsvd15",
        "#MF fpu",     "#AC align",   "#MC mach",   "#XM simd",
        "#VE virt",    "#CP ctrl",    "rsvd22",     "rsvd23",
        "rsvd24",      "rsvd25",      "rsvd26",     "rsvd27",
        "rsvd28",      "rsvd29",      "rsvd30",     "rsvd31",
    };
    if (vec < 32) return exc[vec];
    switch (vec) {
    case 32:    return "IRQ0  PIT timer";
    case 33:    return "IRQ1  keyboard";
    case 34:    return "IRQ2  PIC cascade";
    case 35:    return "IRQ3  COM2";
    case 36:    return "IRQ4  COM1";
    case 38:    return "IRQ6  floppy";
    case 40:    return "IRQ8  RTC";
    case 43:    return "IRQ11 virtio/e1000";
    case 44:    return "IRQ12 PS/2 mouse";
    case 45:    return "IRQ13 FPU";
    case 46:    return "IRQ14 ATA pri";
    case 47:    return "IRQ15 ATA sec";
    case 0x80:  return "INT80 syscall";
    default:    break;
    }
    if (vec >= 32 && vec < 48) return "IRQ";
    return "";
}

void Cmd_Irqstat(NativeCmdCtx *ctx, const char *args)
{
    static uint64_t before[256], after[256];
    uint32_t secs = 1;
    int now = 0;

    if (args && *args) {
        if (cmd_seq_ci(args, "clear")) {
            IDT_ClearCounts();
            PRINT("irqstat: counters cleared.");
            return;
        }
        if (cmd_seq_ci(args, "now")) {
            now = 1;
        } else {
            uint32_t n = 0;
            const char *p = args;
            while (*p >= '0' && *p <= '9') { n = n * 10 + (uint32_t)(*p - '0'); p++; }
            if (n >= 1 && n <= 60) secs = n;
            else if (*args) {
                PRINT("usage: irqstat [<sec>|NOW|CLEAR]");
                return;
            }
        }
    }

    PRINT(" vec  name               count          rate/s");
    PRINT(" ---  -----------------  -------------  -------");

    IDT_SnapshotCounts(before);
    uint64_t t0 = g_pit_ticks;
    if (!now) {
        /* Sample in slices so the desktop/network stay responsive. */
        for (uint32_t i = 0; i < secs * 10; i++)
            CMD_YIELD(ctx, 100);
    }
    uint64_t t1 = g_pit_ticks;
    IDT_SnapshotCounts(after);
    /* Actual elapsed time in deciseconds — yield slices are approximate. */
    uint64_t elapsed_ds = (t1 > t0) ? (t1 - t0) : 1;   /* 100 Hz ticks */
    if (elapsed_ds == 0) elapsed_ds = 1;

    uint64_t total = 0;
    char line[CMD_MAX_LINE], num[24], num2[24];

    for (int v = 0; v < 256; v++) {
        if (!after[v]) continue;
        total += after[v];

        cmd_scopy(line, " ", CMD_MAX_LINE);
        cmd_uint_to_dec((uint32_t)v, num, sizeof(num));
        /* right-align vector number in width 3 */
        int nd = 0; while (num[nd]) nd++;
        for (int i = 0; i < 3 - nd; i++) cmd_scat(line, " ", CMD_MAX_LINE);
        cmd_scat(line, num, CMD_MAX_LINE);
        cmd_scat(line, "  ", CMD_MAX_LINE);

        const char *nm = irqstat_name(v);
        char pad[20];
        int i = 0;
        while (nm[i] && i < 18) { pad[i] = nm[i]; i++; }
        while (i < 18) pad[i++] = ' ';
        pad[i] = '\0';
        cmd_scat(line, pad, CMD_MAX_LINE);
        cmd_scat(line, "  ", CMD_MAX_LINE);

        irqstat_u64dec(after[v], num, sizeof(num));
        /* left-pad count to width 13 */
        nd = 0; while (num[nd]) nd++;
        for (i = 0; i < 13 - nd; i++) cmd_scat(line, " ", CMD_MAX_LINE);
        cmd_scat(line, num, CMD_MAX_LINE);

        if (!now) {
            /* dispatches/sec = delta * 100 ticks-per-sec / elapsed ticks */
            uint64_t rate = (after[v] - before[v]) * 100 / elapsed_ds;
            cmd_scat(line, "  ", CMD_MAX_LINE);
            irqstat_u64dec(rate, num2, sizeof(num2));
            cmd_scat(line, num2, CMD_MAX_LINE);
            /* IRQ-storm flag: a vector sustaining >5000/s is worth a look. */
            if (rate > 5000) cmd_scat(line, "  <-- storm?", CMD_MAX_LINE);
        }
        PRINT(line);
    }

    cmd_scopy(line, "total ", CMD_MAX_LINE);
    irqstat_u64dec(total, num, sizeof(num));
    cmd_scat(line, num, CMD_MAX_LINE);
    cmd_scat(line, " dispatches, uptime ", CMD_MAX_LINE);
    irqstat_u64dec(g_pit_ticks / 100, num, sizeof(num));
    cmd_scat(line, num, CMD_MAX_LINE);
    cmd_scat(line, "s", CMD_MAX_LINE);
    PRINT(line);
}
