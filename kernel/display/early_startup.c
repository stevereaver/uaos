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
#include <stdint.h>
#include <stddef.h>

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
        while (k[ki] && li < 78) line[li++] = k[ki++];
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

/* Wait for a key, with timeout in PIT ticks (100 Hz, so 100 = 1 second).
 * Returns the key char, or 0 if timeout. */
static char es_wait_key(int timeout_ticks)
{
    /* At this point interrupts may be enabled (PIT is running).
     * PS2Kbd_HasChar checks the ring buffer filled by IRQ1. */
    while (timeout_ticks > 0) {
        if (PS2Kbd_HasChar())
            return PS2Kbd_GetChar();
        /* Busy-wait with a small delay — we can't use Task_Yield yet
         * (scheduler not started). PIT is running so IRQ1 will fire. */
        for (volatile int i = 0; i < 100000; i++);
        timeout_ticks--;
    }
    return 0;
}

static char es_wait_key_no_timeout(void)
{
    for (;;) {
        if (PS2Kbd_HasChar())
            return PS2Kbd_GetChar();
        for (volatile int i = 0; i < 100000; i++);
    }
}

/* =========================================================================
 * Main entry
 * ========================================================================= */

int EarlyStartup_Run(void)
{
    if (!g_fb.valid) return 0;

    /* Countdown: 2 seconds (200 PIT ticks at 100 Hz) */
    for (int sec = 2; sec > 0; sec--) {
        draw_countdown(sec);
        /* 100 ticks = 1 second */
        char c = es_wait_key(100);
        if (c != 0) {
            /* Key pressed — show menu */
            goto show_menu;
        }
    }

    /* No key pressed — normal boot */
    return 0;

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
                        continue;
                    }
                    return action;
                }
            }

            /* ESC = normal boot */
            if (c == 27)
                return 0;

            /* Enter = select current */
            if (c == '\n' || c == '\r') {
                int action = es_menu[selected].action;
                if (action == 3) {
                    draw_sysinfo();
                    es_wait_key_no_timeout();
                    continue;
                }
                return action;
            }

            /* Arrow up/down */
            if (c == 0x48 || c == 'k') {  /* Up */
                selected--;
                if (selected < 0) selected = ES_MENU_COUNT - 1;
            }
            if (c == 0x50 || c == 'j') {  /* Down */
                selected++;
                if (selected >= ES_MENU_COUNT) selected = 0;
            }
        }
    }
}
