/* shell_win.c — UAOS Shell Window
 *
 * Draws a resizable Workbench-style CLI window on the framebuffer.
 * Accepts character input from the keyboard driver, renders an input
 * line with a blinking cursor, scrolls a history buffer of output lines,
 * and dispatches commands via a small built-in command table.
 *
 * Layout (all coordinates relative to window top-left):
 *   Title bar     : 20 px
 *   History area  : (WIN_H - TITLEBAR - INPUTBAR) px, scrollable
 *   Separator     : 1 px
 *   Input bar     : 18 px  "1.UAOS> _"
 */

#include "shell_win.h"
#include "framebuffer.h"
#include "cursor.h"
#include "wm.h"
#include <stdint.h>
#include <stddef.h>

/* =========================================================================
 * Window geometry
 * ========================================================================= */

#define TITLEBAR_H  WM_TITLEBAR_H
#define INPUTBAR_H  18
#define BORDER      2

/* Dynamic geometry — updated by WM draw callback */
static int s_wx = 24;
static int s_wy = 28;
static int s_ww = 600;
static int s_wh = 340;

#define HIST_X      (s_wx + BORDER + 4)
#define HIST_Y      (s_wy + TITLEBAR_H + 4)
#define HIST_W      (s_ww - BORDER*2 - 8)
#define HIST_H      (s_wh - TITLEBAR_H - INPUTBAR_H - BORDER*2 - 8)
#define HIST_ROWS   (HIST_H / 16)
#define HIST_COLS   (HIST_W / 8)

#define INPUT_X     (s_wx + BORDER + 4)
#define INPUT_Y     (s_wy + s_wh - INPUTBAR_H - BORDER - 2)

/* =========================================================================
 * History buffer — circular array of text lines
 * ========================================================================= */

#define MAX_HIST_LINES  128
#define MAX_LINE_LEN    128           /* fixed max width (600px / 8 = 75 + slack) */
#define MAX_INPUT       (MAX_LINE_LEN - 8 - 1)

static char   hist[MAX_HIST_LINES][MAX_LINE_LEN];
static int    hist_count  = 0;    /* total lines ever written    */
static int    hist_scroll = 0;    /* lines scrolled up from bottom */

/* Input line */
static char input_buf[MAX_INPUT + 1];
static int  input_len = 0;

static const char *PROMPT = "1.UAOS> ";
#define PROMPT_LEN 8

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

static int slen(const char *s) {
    int n = 0; while (s[n]) n++; return n;
}
static void scopy(char *dst, const char *src, int max) {
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}
static void scat(char *dst, const char *src, int max) {
    int dl = slen(dst);
    scopy(dst + dl, src, max - dl);
}

/* Push a new line into the history ring */
static void hist_push(const char *line)
{
    int idx = hist_count % MAX_HIST_LINES;
    scopy(hist[idx], line, MAX_LINE_LEN);
    hist_count++;
    hist_scroll = 0;   /* auto-scroll to bottom on new output */
}

/* =========================================================================
 * Command dispatch
 * ========================================================================= */

static void shell_print(const char *s) { hist_push(s); }

static void cmd_help(void)
{
    shell_print("UAOS Shell v0.1 - built-in commands:");
    shell_print("  help     show this help");
    shell_print("  version  show OS version");
    shell_print("  mem      memory information");
    shell_print("  clear    clear the shell window");
    shell_print("  reboot   warm reboot via keyboard controller");
}

static void cmd_version(void)
{
    shell_print("Ultimate Amiga OS  v0.1.0-dev");
    shell_print("Kernel: x86_64 ELF64, Multiboot2, long mode");
    shell_print("Display: 1024x768 32bpp linear framebuffer (GOP)");
    shell_print("Input: PS/2 keyboard + mouse, IRQ1/IRQ12");
}

static void cmd_mem(void)
{
    shell_print("RAM:  512 MB (QEMU)");
    shell_print("Kernel load: 0x0000000000100000");
    shell_print("Framebuffer: mapped (GOP physical address)");
    shell_print("Stack: 16 KB (bootstrap), no heap allocator yet");
}

static void cmd_clear(void)
{
    hist_count  = 0;
    hist_scroll = 0;
    for (int i = 0; i < MAX_HIST_LINES; i++)
        hist[i][0] = 0;
}

static void cmd_reboot(void)
{
    shell_print("Rebooting...");
    ShellWin_Redraw();
    /* Pulse keyboard controller reset line */
    __asm__ volatile (
        "1: inb  $0x64, %%al\n"
        "   testb $0x02, %%al\n"
        "   jnz 1b\n"
        "   movb $0xFE, %%al\n"
        "   outb %%al, $0x64\n"
        :: : "eax"
    );
    for (;;) __asm__ volatile ("hlt");
}

static void dispatch(const char *line)
{
    /* Echo the command with prompt */
    char echo[MAX_LINE_LEN];
    scopy(echo, PROMPT, MAX_LINE_LEN);
    scat(echo, line, MAX_LINE_LEN);
    hist_push(echo);

    /* Skip leading spaces */
    while (*line == ' ') line++;

    if (!*line) return;

    /* Compare command word */
    const char *cmds[] = { "help","version","mem","clear","reboot", NULL };
    void (*fns[])(void) = { cmd_help, cmd_version, cmd_mem, cmd_clear, cmd_reboot };

    for (int i = 0; cmds[i]; i++) {
        const char *c = cmds[i];
        int cl = slen(c);
        int match = 1;
        for (int j = 0; j < cl; j++) {
            if (line[j] != c[j]) { match = 0; break; }
        }
        if (match && (line[cl] == 0 || line[cl] == ' ')) {
            fns[i]();
            return;
        }
    }

    char msg[MAX_LINE_LEN];
    scopy(msg, "Unknown command: ", MAX_LINE_LEN);
    scat(msg, line, MAX_LINE_LEN);
    hist_push(msg);
}

/* =========================================================================
 * Rendering
 * ========================================================================= */

static void draw_window_contents(void)
{
    /* History area background */
    FB_FillRect(s_wx+BORDER, s_wy+TITLEBAR_H+1,
                s_ww-BORDER*2, s_wh-TITLEBAR_H-INPUTBAR_H-BORDER-1,
                WB_BLACK);

    /* Separator above input bar */
    FB_DrawHLine(s_wx+BORDER,
                 s_wy+s_wh-INPUTBAR_H-BORDER-1,
                 s_ww-BORDER*2, WB_DARK_GREY);

    /* Input bar background */
    FB_FillRect(s_wx+BORDER,
                s_wy+s_wh-INPUTBAR_H-BORDER,
                s_ww-BORDER*2, INPUTBAR_H,
                WB_BLACK);
}

static void draw_history(void)
{
    /* Clear history area */
    FB_FillRect(s_wx+BORDER+1, HIST_Y,
                s_ww-BORDER*2-2, HIST_H, WB_BLACK);

    /* Determine which lines to show */
    int visible = HIST_ROWS;
    int start_line = hist_count - visible - hist_scroll;
    if (start_line < 0) start_line = 0;

    for (int row = 0; row < visible; row++) {
        int li = start_line + row;
        if (li >= hist_count) break;
        int idx = li % MAX_HIST_LINES;
        FB_PutStr(HIST_X, HIST_Y + row * 16,
                  hist[idx], WB_CREAM, WB_BLACK);
    }
}

static void draw_input_line(void)
{
    /* Clear input bar */
    FB_FillRect(s_wx+BORDER+1,
                INPUT_Y, s_ww-BORDER*2-2, INPUTBAR_H, WB_BLACK);

    /* Prompt */
    FB_PutStr(INPUT_X, INPUT_Y + 1, PROMPT, WB_GREEN, WB_BLACK);

    /* Input text */
    int px = INPUT_X + PROMPT_LEN * 8;
    FB_PutStr(px, INPUT_Y + 1, input_buf, WB_WHITE, WB_BLACK);

    /* Block cursor */
    int cx = px + input_len * 8;
    FB_FillRect(cx, INPUT_Y + 1, 8, 14, WB_WHITE);
}

/* =========================================================================
 * Public API
 * ========================================================================= */

/* WM draw callback — called by WM with current window geometry */
static void shell_wm_draw(int wx, int wy, int ww, int wh)
{
    s_wx = wx;
    s_wy = wy;
    s_ww = ww;
    s_wh = wh;
    draw_window_contents();
    draw_history();
    draw_input_line();
}

void ShellWin_Init(void)
{
    if (!g_fb.valid) return;
    cmd_clear();
    shell_print("UAOS Shell  v0.1 - type 'help' for commands");
    shell_print("");
    WM_AddWindow(s_wx, s_wy, s_ww, s_wh, "UAOS Shell",
                 shell_wm_draw, ShellWin_HandleKey);
    WM_Redraw();
}

void ShellWin_Redraw(void)
{
    if (!g_fb.valid) return;
    WM_Redraw();
}

void ShellWin_HandleKey(char c)
{
    if (!g_fb.valid) return;

    if (c == '\n' || c == '\r') {
        input_buf[input_len] = 0;
        dispatch(input_buf);
        input_len = 0;
        input_buf[0] = 0;
    } else if (c == '\b') {
        if (input_len > 0) {
            input_len--;
            input_buf[input_len] = 0;
        }
    } else if (c >= 0x20 && c < 0x7F) {
        if (input_len < MAX_INPUT) {
            input_buf[input_len++] = c;
            input_buf[input_len]   = 0;
        }
    }

    draw_history();
    draw_input_line();
    /* Do NOT call Cursor_Redraw here — mouse IRQ owns the cursor position.
     * Calling it here re-saves bg with stale cursor pixels causing ghosts. */
}
