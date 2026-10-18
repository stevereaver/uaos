/* early_startup.c — UAOS Early Startup Control
 *
 * Presents a boot-time menu (similar to Amiga Early Startup Control)
 * that appears for a brief countdown. If the user presses a key during
 * the countdown, a menu of boot options is shown.
 *
 * Options:
 *  1. Normal boot (default)
 *  2. Boot without Startup-Sequence
 *  3. Boot to shell only (no Workbench)
 *  4. Display system information
 *
 * Uses the framebuffer directly (pre-scheduler, pre-WM).
 */

#include "early_startup.h"
#include "framebuffer.h"
#include "../irq/ps2kbd.h"
#include "../irq/irq.h"
#include "../irq/pit.h"
#include "../irq/idt.h"
#include "../irq/ioapic.h"
#include "../irq/acpi.h"
#include "../boot/kprint.h"
#include "../dbg/diag.h"
#include <stdint.h>
#include <stddef.h>

extern volatile uint64_t g_pit_ticks;   /* 100 Hz — uaos_kernel_main.c */
extern volatile int      g_irq_depth;   /* idt.c */

/* =========================================================================
 * Helpers (no libc)
 * ========================================================================= */

static int es_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void es_strcpy(char *d, const char *s)
{
    int i = 0;
    while (s[i]) { d[i] = s[i]; i++; }
    d[i] = '\0';
}

static void es_itoa(uint32_t n, char *buf)
{
    if (n == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[12]; int i = 0;
    while (n && i < 11) { tmp[i++] = (char)('0' + (n % 10)); n /= 10; }
    int j = 0;
    while (i--) buf[j++] = tmp[i];
    buf[j] = '\0';
}

static void es_puthex(uint32_t v, char *buf)
{
    static const char hex[] = "0123456789ABCDEF";
    buf[0] = '0'; buf[1] = 'x';
    int pos = 9;
    char tmp[8];
    int i = 0;
    do { tmp[i++] = hex[v & 0xF]; v >>= 4; } while (v && i < 8);
    int j = 2;
    while (i--) buf[j++] = tmp[i];
    buf[j] = '\0';
    (void)pos;
}

static inline void es_outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint8_t es_inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* =========================================================================
 * Drawing helpers
 * ========================================================================= */

#define ES_CHAR_W  8
#define ES_CHAR_H  16
#define ES_COLS    80
#define ES_ROWS    25

static void es_clear(void)
{
    if (g_fb.valid)
        FB_FillRect(0, 0, g_fb.width, g_fb.height, WB_BLUE);
}

static void es_putc(int col, int row, char c, uint32_t fg, uint32_t bg)
{
    if (!g_fb.valid) return;
    int x = col * ES_CHAR_W;
    int y = row * ES_CHAR_H;
    FB_PutChar(x, y, c, fg, bg);
}

static void es_puts(int col, int row, const char *s, uint32_t fg, uint32_t bg)
{
    if (!g_fb.valid) return;
    int x = col * ES_CHAR_W;
    int y = row * ES_CHAR_H;
    /* Fill background behind text */
    int len = es_strlen(s);
    FB_FillRect(x, y, len * ES_CHAR_W, ES_CHAR_H, bg);
    FB_PutStr(x, y, s, fg, bg);
}

static void es_fill_line(int row, uint32_t bg)
{
    if (!g_fb.valid) return;
    FB_FillRect(0, row * ES_CHAR_H, g_fb.width, ES_CHAR_H, bg);
}

static void es_center(int row, const char *s, uint32_t fg, uint32_t bg)
{
    int len = es_strlen(s);
    int col = (ES_COLS - len) / 2;
    if (col < 0) col = 0;
    es_puts(col, row, s, fg, bg);
}

/* =========================================================================
 * Menu items
 * ========================================================================= */

typedef struct {
    const char *key;
    const char *label;
    int         action;  /* 0=normal, 1=no startup, 2=shell only, 3=info, -1=redraw */
} EsMenuItem;

static const EsMenuItem es_menu[] = {
    { "1", "Normal Boot",                     0 },
    { "2", "Boot without Startup-Sequence",   1 },
    { "3", "Boot to Shell only (no Workbench)", 2 },
    { "4", "Display System Information",      3 },
    { "ESC", "Continue with Normal Boot",     0 },
};

#define ES_MENU_COUNT  (int)(sizeof(es_menu) / sizeof(es_menu[0]))

/* =========================================================================
 * Screens
 * ========================================================================= */

static void draw_banner(void)
{
    es_clear();

    /* Title */
    es_center(2, "*** EARLY STARTUP CONTROL ***", WB_WHITE, WB_BLUE);
    es_center(3, "Ultimate Amiga OS (UAOS)", WB_CREAM, WB_BLUE);

    /* Separator */
    if (g_fb.valid)
        FB_DrawHLine(4 * ES_CHAR_W, 5 * ES_CHAR_H,
                     (ES_COLS - 8) * ES_CHAR_W, WB_CREAM);
}

static void draw_countdown(int seconds_left)
{
    draw_banner();

    es_center(7, "Press any key for boot options...", WB_WHITE, WB_BLUE);

    char msg[40];
    es_strcpy(msg, "Continuing in ");
    char num[12];
    es_itoa((uint32_t)seconds_left, num);
    int mi = 14;
    int ni = 0;
    while (num[ni] && mi < 38) msg[mi++] = num[ni++];
    msg[mi++] = ' ';
    msg[mi++] = 's';
    msg[mi++] = '.';
    msg[mi++] = '.';
    msg[mi++] = '.';
    msg[mi++] = '\0';
    es_center(9, msg, WB_CREAM, WB_BLUE);
}

static void draw_menu(int selected)
{
    draw_banner();

    es_center(7, "Select boot option:", WB_WHITE, WB_BLUE);
    es_center(8, "(use number keys or arrows + ENTER)", WB_CREAM, WB_BLUE);

    int y = 10;
    for (int i = 0; i < ES_MENU_COUNT; i++) {
        char line[80];
        int li = 0;

        /* Selection indicator */
        if (i == selected) {
            line[li++] = '>';
            line[li++] = ' ';
        } else {
            line[li++] = ' ';
            line[li++] = ' ';
        }

        /* Key */
        const char *k = es_menu[i].key;
        int ki = 0;
        while (k[ki] && li < 75) line[li++] = k[ki++];
        line[li++] = ' ';
        line[li++] = '-';
        line[li++] = ' ';

        /* Label */
        const char *l = es_menu[i].label;
        int ji = 0;
        while (l[ji] && li < 78) line[li++] = l[ji++];
        line[li] = '\0';

        uint32_t fg = (i == selected) ? WB_WHITE : WB_CREAM;
        uint32_t bg = (i == selected) ? WB_LIGHT_BLUE : WB_BLUE;
        es_center(y + i, line, fg, bg);
    }
}

static void draw_sysinfo(void)
{
    draw_banner();

    es_center(7, "System Information", WB_WHITE, WB_BLUE);

    int row = 9;

    /* Screen mode */
    char line[80];
    es_strcpy(line, "Screen: ");
    char num[12];
    es_itoa(g_fb.width, num);
    int mi = 8;
    int ni = 0;
    while (num[ni] && mi < 78) line[mi++] = num[ni++];
    line[mi++] = 'x';
    es_itoa(g_fb.height, num);
    ni = 0;
    while (num[ni] && mi < 78) line[mi++] = num[ni++];
    line[mi++] = ' ';
    es_itoa(g_fb.bpp, num);
    ni = 0;
    while (num[ni] && mi < 78) line[mi++] = num[ni++];
    line[mi++] = 'b';
    line[mi++] = 'p';
    line[mi++] = 'p';
    line[mi++] = '\0';
    es_puts(8, row++, line, WB_CREAM, WB_BLUE);
    row++;

    /* Framebuffer address */
    es_strcpy(line, "Framebuffer: ");
    es_puthex((uint32_t)(g_fb.phys_addr & 0xFFFFFFFF), line + 13);
    es_puts(8, row++, line, WB_CREAM, WB_BLUE);

    /* Pitch */
    es_strcpy(line, "Pitch: ");
    es_itoa(g_fb.pitch, num);
    mi = 7; ni = 0;
    while (num[ni] && mi < 78) line[mi++] = num[ni++];
    line[mi++] = ' ';
    line[mi++] = 'b';
    line[mi++] = 'y';
    line[mi++] = 't';
    line[mi++] = 'e';
    line[mi++] = 's';
    line[mi++] = '\0';
    es_puts(8, row++, line, WB_CREAM, WB_BLUE);
    row++;

    es_puts(8, row++, "Kernel: UAOS x86_64 bare-metal", WB_CREAM, WB_BLUE);
    es_puts(8, row++, "Emulation: M68k Musashi (transparent)", WB_CREAM, WB_BLUE);
    es_puts(8, row++, "Filesystems: RAMFS, FAT32, ISO9660, EXT4, PFS3, CrossDOS", WB_CREAM, WB_BLUE);
    es_puts(8, row++, "Network: VirtIO-Net (TCP/IP stack)", WB_CREAM, WB_BLUE);
    row++;

    es_center(row + 1, "Press any key to return to menu...", WB_WHITE, WB_BLUE);
}

/* =========================================================================
 * Input
 * ========================================================================= */

/* Early startup runs before Task_StartFirst with interrupts enabled for
 * the duration of the menu (EarlyStartup_Run saves/restores IF), so the
 * PIT tick keeps g_pit_ticks advancing and IRQ1/USB-HID IRQs fill the key
 * ring.  Waits are g_pit_ticks deadlines with hlt between polls — the
 * PIT wakes the CPU each tick.  If the PIT stops advancing entirely
 * (broken timer hardware/QEMU), a TSC-bounded spin stands in so the boot
 * can never hang here. */

/* ~50 ms of no tick advance -> declare the PIT dead.  TSC frequency is
 * uncalibrated at this point in boot, so this is only an order-of-
 * magnitude bound: ~50 ms at 4 GHz, ~100 ms at 2 GHz. */
#define ES_TICK_STALL_CYCLES  200000000ULL

/* =========================================================================
 * IRQ diagnostic snapshot (UAOS-336)
 * ========================================================================= */

/* On the MBP4,1 the menu held boot ~48 s: no PIT/RTC IRQs arrived at all
 * during the menu despite sti, so es_wait_key fell into the dead-PIT
 * spin for its whole (iteration-estimated) budget.  This dump captures
 * the interrupt-delivery state at three points — entry, first stall,
 * exit — so the next metal boot identifies where delivery stops
 * (IF clear in flags? vectors armed but LAPIC TPR/LINT0 masking them?
 * IO-APIC RTE masked? PIC still the route?). */

static uint64_t es_t0, es_tick0;    /* captured at EarlyStartup_Run entry */
static uint64_t es_wt0;             /* captured at each es_wait_key entry */
static int      es_fallback;        /* set when the dead-PIT path fired  */

static void es_irq_snapshot(const char *tag)
{
    uint64_t fl;
    __asm__ volatile ("pushfq; pop %0" : "=r"(fl) :: "memory");

    /* Line 1: IF, elapsed TSC/ticks vs ES entry and wait entry, per-
     * vector dispatch counts (34=PIT GSI2, 40=RTC IRQ8 on QEMU), the
     * nonzero-vector list, IRQ depth, fallback flag, IRQ mode. */
    kprint("[ES] "); kprint(tag);
    kprint(": if=");  kprintdec((uint32_t)((fl >> 9) & 1));
    kprint(" +t=");   kprintdec64(diag_rdtsc() - es_t0);
    kprint(" +ticks="); kprintdec64(g_pit_ticks - es_tick0);
    if (tag[0] == 's') {   /* stall: also TSC since wait entry */
        kprint(" +wt=");
        kprintdec64(diag_rdtsc() - es_wt0);
    }
    kprint(" v34=");  kprintdec64(IDT_VectorCount(34));
    kprint(" v40=");  kprintdec64(IDT_VectorCount(40));
    {
        uint64_t tot = 0;
        for (int v = 0; v < 256; v++) tot += IDT_VectorCount(v);
        kprint(" vt="); kprintdec64(tot);
        kprint(" [");
        for (int v = 0; v < 256; v++) {
            uint64_t c = IDT_VectorCount(v);
            if (c) {
                kprint("v"); kprintdec((uint32_t)v);
                kprint("="); kprintdec64(c); kprint(" ");
            }
        }
        kprint("]");
    }
    kprint(" irqd="); kprintdec((uint32_t)g_irq_depth);
    kprint(" fb=");   kprintdec((uint32_t)es_fallback);
    kprint(" mode="); kprint(IRQ_Mode() == IRQ_MODE_IOAPIC ? "ioapic" : "pic");
    kprint("\n");

    /* Line 2: LAPIC state (TPR/PPR/SVR/LINT0 + nonzero ISR/IRR words),
     * IO-APIC RTEs for the two timer GSIs, 8259 IMR/ISR. */
    if (ACPI_LapicBase()) {
        kprint("  lapic tpr="); kprinthex_min(LAPIC_Read(0x080));
        kprint(" ppr=");        kprinthex_min(LAPIC_Read(0x0A0));
        kprint(" svr=");        kprinthex_min(LAPIC_Read(0x0F0));
        kprint(" lint0=");      kprinthex_min(LAPIC_Read(0x350));
        for (int i = 0; i < 8; i++) {
            uint32_t v = LAPIC_Read(0x100 + (uint32_t)i * 0x10);
            if (v) { kprint(" isr"); kprintdec((uint32_t)i);
                     kprint("="); kprinthex_min(v); }
        }
        for (int i = 0; i < 8; i++) {
            uint32_t v = LAPIC_Read(0x200 + (uint32_t)i * 0x10);
            if (v) { kprint(" irr"); kprintdec((uint32_t)i);
                     kprint("="); kprinthex_min(v); }
        }
    } else {
        kprint("  lapic=none");
    }
    {
        uint32_t lo, hi;
        if (IOAPIC_ReadRTE(2, &lo, &hi) == 0) {
            kprint(" rte2="); kprinthex_min(lo);
        }
        if (IOAPIC_ReadRTE(8, &lo, &hi) == 0) {
            kprint(" rte8="); kprinthex_min(lo);
        }
    }
    {
        uint8_t imr1 = es_inb(0x21), imr2 = es_inb(0xA1);
        es_outb(0x20, 0x0B);            /* OCW3: read ISR on next inb */
        uint8_t isr1 = es_inb(0x20);
        es_outb(0x20, 0x0A);            /* restore: read IRR (POR def.) */
        es_outb(0xA0, 0x0B);
        uint8_t isr2 = es_inb(0xA0);
        es_outb(0xA0, 0x0A);
        kprint(" pic imr1="); kprinthex_min(imr1);
        kprint(" imr2="); kprinthex_min(imr2);
        kprint(" isr1="); kprinthex_min(isr1);
        kprint(" isr2="); kprinthex_min(isr2);
    }
    kprint("\n");
}

/* Wait for a key, with timeout in PIT ticks (UAOS_PIT_HZ, so HZ ticks =
 * 1 second).  Returns the key char, or 0 if timeout. */
static char es_wait_key(uint64_t timeout_ticks)
{
    uint64_t tick0     = g_pit_ticks;
    uint64_t last_tick = tick0;
    uint64_t start_tsc = diag_rdtsc();
    uint64_t stall_tsc = start_tsc;
    es_wt0 = start_tsc;

    /* TSC bound for the dead-PIT fallback: end the wait once the
     * requested timeout's worth of wall time has elapsed, so a menu on
     * a box with dead IRQ delivery costs ~timeout, not however long the
     * iteration estimate happens to take on that CPU (UAOS-336). */
    uint64_t hz = Tickmon_TscHz();
    if (hz == 0) hz = 3000000000ULL;
    uint64_t tsc_deadline =
        start_tsc + (timeout_ticks * hz) / UAOS_PIT_HZ;

    for (;;) {
        if (PS2Kbd_HasChar())
            return PS2Kbd_GetChar();

        uint64_t now = g_pit_ticks;
        if (now != last_tick) {
            last_tick  = now;
            stall_tsc  = diag_rdtsc();
        } else if (diag_rdtsc() - stall_tsc > ES_TICK_STALL_CYCLES) {
            /* PIT not advancing — bounded by TSC, not iterations.
             * Snapshot once per boot on the first stall. */
            if (!es_fallback) {
                es_fallback = 1;
                es_irq_snapshot("stall");
            }
            if (diag_rdtsc() >= tsc_deadline)
                return 0;
            continue;               /* no hlt: nothing may wake us */
        }
        if (now - tick0 >= timeout_ticks)
            return 0;
        __asm__ volatile ("hlt" ::: "memory");   /* IF=1 — any IRQ wakes */
    }
}

static char es_wait_key_no_timeout(void)
{
    for (;;) {
        if (PS2Kbd_HasChar())
            return PS2Kbd_GetChar();
        __asm__ volatile ("hlt" ::: "memory");
    }
}

/* Drop every char currently in the ring — used after the countdown so a
 * key (or key-repeat burst) that opened the menu does not immediately
 * also select an item. */
static void es_flush_keys(void)
{
    while (PS2Kbd_HasChar())
        (void)PS2Kbd_GetChar();
}

/* =========================================================================
 * Main entry
 * ========================================================================= */

int EarlyStartup_Run(void)
{
    if (!g_fb.valid) return 0;

    /* The menu runs before Task_StartFirst with IF=0 on entry (the boot
     * path's last cli is still in effect) — meaning no PIT ticks, no
     * IRQ1 keystrokes, and no USB HID input would ever arrive.  Enable
     * interrupts for the duration of the menu and restore the entry
     * state on every exit path so the rest of boot is unaffected.
     * Pre-scheduler this is safe: do_schedule() early-returns while
     * g_current == NULL, the wait queues are empty, and every other PIT
     * handler body (timer_ProcessTicks, CpuFreq_Tick, the RTC second
     * tick, USB poll, EventPump_Wake) tolerates no-current-task. */
    uint64_t saved_fl = irq_save();
    __asm__ volatile ("sti" ::: "memory");

    es_t0        = diag_rdtsc();
    es_tick0     = g_pit_ticks;
    es_fallback  = 0;
    es_irq_snapshot("entry");

    int mode = 0;

    /* Countdown: 2 seconds (2 * UAOS_PIT_HZ ticks) */
    for (int sec = 2; sec > 0; sec--) {
        draw_countdown(sec);
        char c = es_wait_key(UAOS_PIT_HZ);
        if (c != 0) {
            /* Key pressed — the countdown may have queued further chars
             * (auto-repeat, a quick double tap); drop them so they can't
             * also select a menu item, then show the menu. */
            es_flush_keys();
            goto show_menu;
        }
    }

    /* No key pressed — normal boot */
    goto out;

show_menu:
    {
        int selected = 0;

        for (;;) {
            draw_menu(selected);
            char c = es_wait_key_no_timeout();

            /* Number keys select directly */
            if (c >= '1' && c <= '4') {
                int idx = c - '1';
                if (idx < ES_MENU_COUNT - 1) {
                    int action = es_menu[idx].action;
                    if (action == 3) {
                        /* Info screen */
                        draw_sysinfo();
                        es_wait_key_no_timeout();
                        es_flush_keys();
                        continue;
                    }
                    mode = action;
                    goto out;
                }
            }

            /* ESC = normal boot */
            if (c == 27) {
                mode = 0;
                goto out;
            }

            /* Enter = select current */
            if (c == '\n' || c == '\r') {
                int action = es_menu[selected].action;
                if (action == 3) {
                    draw_sysinfo();
                    es_wait_key_no_timeout();
                    es_flush_keys();
                    continue;
                }
                mode = action;
                goto out;
            }

            /* Arrow up/down — the ring delivers KBD_VKEY_* codes (both
             * the PS/2 and USB HID paths translate to them). */
            if (c == KBD_VKEY_UP || c == 'k') {
                selected--;
                if (selected < 0) selected = ES_MENU_COUNT - 1;
            }
            if (c == KBD_VKEY_DOWN || c == 'j') {
                selected++;
                if (selected >= ES_MENU_COUNT) selected = 0;
            }
        }
    }

out:
    es_irq_snapshot("exit");
    irq_restore(saved_fl);
    return mode;
}
