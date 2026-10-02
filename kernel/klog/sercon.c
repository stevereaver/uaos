/* sercon.c — two-way serial debug console (UAOS-207)
 *
 * uart.c is TX-only; this adds the RX half as a low-priority native task
 * that polls the 16550 LSR data-ready bit and runs a minimal command set
 * against kernel state.  It exists for the cases where the GUI and
 * telnetd are both unavailable: bare-metal bring-up, dead net stack, or
 * a frozen desktop while the scheduler still runs.
 *
 * Polling (not IRQ4) is deliberate — no interrupt routing involved, so
 * it works even when IRQ delivery is what's being debugged.
 *
 * Commands (serial line, CR-terminated):
 *   help                command list
 *   ps                  task table (Task_DiagDump)
 *   taskdump <name>     saved-frame decode for one task
 *   dmesg [n]           last n klog ring entries
 *   irqstat             per-vector counters
 *   klog <sub>=<lvl>    set klog level (e.g. "klog net=debug")
 *   mem                 memory summary
 *   tick                g_pit_ticks + switch count
 *   reboot              warm reboot
 *
 * Started at boot when the kernel cmdline contains "sercon" or
 * "console=ttyS0"; runtime control via "C:sercon on|off|status".
 */

#include "klog.h"
#include "../exec/task.h"
#include "../irq/idt.h"
#include "../dbg/diag.h"
#include "../system_reboot.h"
#include "../exec/mem_info.h"
#include <stdint.h>
#include <stddef.h>

extern volatile uint64_t g_pit_ticks;

static volatile int g_sercon_on;
static volatile int g_sercon_started;

/* uart_puts-based emit so Task_DiagDump/IDT dumps stream to serial. */
static void sercon_emit(void *ctx, const char *line)
{
    (void)ctx;
    uart_puts(line);
    uart_puts("\n");
}

static int s_eq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static void sercon_ps(void)
{
    DiagLine l;
    dl_reset(&l);
    dl_add(&l, " #  name                 type    state    pri  cpu   ctxsw  stackpk");
    sercon_emit(NULL, l.buf);
    Task_DiagDump(NULL, sercon_emit, NULL, 0);
}

static void sercon_dmesg(const char *args)
{
    uint32_t n = 20;
    if (args && *args >= '0' && *args <= '9') {
        n = 0;
        while (*args >= '0' && *args <= '9') {
            n = n * 10 + (uint32_t)(*args - '0');
            args++;
        }
    }
    uint32_t total = klog_ring_count();
    if (n > total) n = total;
    DiagLine l;
    for (uint32_t i = total - n; i < total; i++) {
        int sub = 0, lvl = 0;
        const char *txt = NULL;
        if (!klog_ring_get(i, &sub, &lvl, &txt)) break;
        dl_reset(&l);
        dl_ch(&l, '[');
        const char *sn = klog_subsys_name(sub);
        dl_add(&l, sn ? sn : "?");
        dl_add(&l, "] ");
        dl_add(&l, txt ? txt : "");
        sercon_emit(NULL, l.buf);
    }
}

static void sercon_irqstat(void)
{
    DiagLine l;
    for (int v = 0; v < 256; v++) {
        uint64_t c = IDT_VectorCount(v);
        if (!c) continue;
        dl_reset(&l);
        dl_add(&l, " vec "); dl_dec(&l, (uint64_t)v); dl_pad(&l, 8);
        const char *nm = IDT_VectorName((uint8_t)v);
        if (nm) { dl_add(&l, nm); dl_pad(&l, 24); }
        dl_dec(&l, c);
        sercon_emit(NULL, l.buf);
    }
}

static void sercon_mem(void)
{
    struct UaosMemInfo mi;
    Mem_GetInfo(&mi);
    DiagLine l;
    dl_reset(&l);
    dl_add(&l, "x64 heap: "); dl_dec(&l, mi.x64_used);
    dl_add(&l, "/"); dl_dec(&l, mi.x64_total);
    dl_add(&l, "  m68k slots: "); dl_dec(&l, mi.m68k_slots_used);
    dl_add(&l, "/"); dl_dec(&l, mi.m68k_slots_total);
    dl_add(&l, "  tasks: "); dl_dec(&l, mi.tasks_running);
    dl_add(&l, " run / "); dl_dec(&l, mi.tasks_waiting);
    dl_add(&l, " wait / "); dl_dec(&l, mi.tasks_total);
    dl_add(&l, " total");
    sercon_emit(NULL, l.buf);
}

static void sercon_exec(char *line)
{
    /* trim leading spaces */
    while (*line == ' ') line++;
    if (!*line) return;

    /* split cmd / rest */
    char *args = line;
    while (*args && *args != ' ') args++;
    if (*args) *args++ = '\0';
    while (*args == ' ') args++;

    uart_puts("\n");

    if (s_eq(line, "help")) {
        uart_puts("commands: help ps taskdump <name> dmesg [n] irqstat "
                  "klog <sub>=<lvl> mem tick reboot\n");
    } else if (s_eq(line, "ps") || s_eq(line, "status")) {
        sercon_ps();
    } else if (s_eq(line, "taskdump")) {
        if (!*args) uart_puts("usage: taskdump <task-name>\n");
        else Task_DiagDump(NULL, sercon_emit, args, 1);
    } else if (s_eq(line, "dmesg")) {
        sercon_dmesg(args);
    } else if (s_eq(line, "irqstat")) {
        sercon_irqstat();
    } else if (s_eq(line, "klog")) {
        /* klog <subsys>=<level> — tiny subset of C:klog */
        char *eq = args;
        while (*eq && *eq != '=') eq++;
        if (*eq == '=') {
            *eq++ = '\0';
            int sub = klog_subsys_find(args);
            int lvl = klog_level_find(eq);
            if (sub >= 0 && lvl >= 0) {
                klog_set_level(sub, lvl);
                uart_puts("ok\n");
            } else uart_puts("unknown subsys/level\n");
        } else uart_puts("usage: klog <subsys>=<level>\n");
    } else if (s_eq(line, "mem")) {
        sercon_mem();
    } else if (s_eq(line, "tick")) {
        DiagLine l;
        dl_reset(&l);
        dl_add(&l, "pit_ticks="); dl_dec(&l, g_pit_ticks);
        dl_add(&l, " switches="); dl_dec(&l, g_ctx_switches);
        dl_add(&l, " runnable="); dl_dec(&l, (uint64_t)Task_RunnableCount());
        sercon_emit(NULL, l.buf);
    } else if (s_eq(line, "reboot")) {
        uart_puts("rebooting\n");
        System_Reboot();
    } else {
        uart_puts("? unknown command — try 'help'\n");
    }
    uart_puts("sercon> ");
}

static void sercon_task(void *arg)
{
    (void)arg;
    static char linebuf[96];
    int len = 0;

    uart_puts("\nsercon> ");
    for (;;) {
        int c = uart_getchar();
        if (c < 0) {
            Task_SleepTicks(3);    /* ~30 ms poll cadence */
            continue;
        }
        if (!g_sercon_on) { len = 0; continue; }
        if (c == '\r' || c == '\n') {
            linebuf[len] = '\0';
            sercon_exec(linebuf);
            len = 0;
        } else if (c == 0x7F || c == 0x08) {
            if (len > 0) { len--; uart_puts("\b \b"); }
        } else if (len < (int)sizeof(linebuf) - 1) {
            linebuf[len++] = (char)c;
            uart_putchar((char)c);   /* echo */
        }
    }
}

int Sercon_Running(void) { return g_sercon_on; }

void Sercon_Start(void)
{
    g_sercon_on = 1;
    if (g_sercon_started) return;
    if (Task_CreateNative("sercon", -64, sercon_task, NULL)) {
        g_sercon_started = 1;
        kprint("[sercon] serial console task started (polled RX, 9600+ baud ok)\n");
    }
}

void Sercon_Stop(void)
{
    g_sercon_on = 0;
    kprint("[sercon] console input disabled (task idles)\n");
}
