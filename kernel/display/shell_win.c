/* shell_win.c — UAOS Shell Window
 *
 * Supports up to MAX_SHELLS independent shell windows.
 * Each instance has its own history buffer, input line, and geometry.
 * A per-slot draw/key shim routes WM callbacks to the correct instance.
 *
 * Layout (relative to window top-left):
 *   Title bar  : 20 px
 *   History    : variable (scrollable)
 *   Separator  : 1 px
 *   Input bar  : 18 px   "1.UAOS> _"
 */

#include "shell_win.h"
#include "framebuffer.h"
#include "cursor.h"
#include "wm.h"
#include "../../emulation/uaos_emu.h"
#include <stdint.h>
#include <stddef.h>

/* =========================================================================
 * Constants
 * ========================================================================= */

#define TITLEBAR_H      WM_TITLEBAR_H
#define INPUTBAR_H      18
#define BORDER_L        1                  /* left: just the outline */
#define BORDER_R        WM_SCROLLBAR_W     /* right: scrollbar width */
#define BORDER          BORDER_L           /* legacy alias for top inset */
#define MAX_HIST_LINES  1000
#define MAX_LINE_LEN    96
#define MAX_INPUT       80
#define MAX_SHELLS      4
#define SCROLL_LINES    4    /* lines per Page-Up/Down tick */

/* Special virtual keys injected by kbd driver for scroll */
#define VKEY_PGUP  0x01
#define VKEY_PGDN  0x02

/* =========================================================================
 * Per-instance state
 * ========================================================================= */

typedef struct {
    /* WM */
    int  wm_handle;
    int  number;        /* shell number shown in prompt, 1-based */
    int  index;         /* index into g_shells[] and g_hist_buf[] */

    /* Geometry — updated by draw callback */
    int  wx, wy, ww, wh;

    /* History — ring buffer indices, actual storage in g_hist_buf below */
    int  hist_count;
    int  hist_scroll;

    /* Input */
    char input_buf[MAX_INPUT + 1];
    int  input_len;
} ShellInstance;

/* History storage in BSS (not on stack) — 1000×96 × 4 shells = 384 KB */
static char g_hist_buf[MAX_SHELLS][MAX_HIST_LINES][MAX_LINE_LEN];

static ShellInstance g_shells[MAX_SHELLS];
static int           g_n_shells = 0;

/* =========================================================================
 * String helpers
 * ========================================================================= */

static int slen(const char *s) {
    int n = 0; while (s[n]) n++; return n;
}
static void scopy(char *d, const char *s, int max) {
    int i = 0;
    while (i < max - 1 && s[i]) { d[i] = s[i]; i++; }
    d[i] = 0;
}
static void scat(char *d, const char *s, int max) {
    int dl = slen(d); scopy(d + dl, s, max - dl);
}
static void uint_to_dec_s(uint32_t v, char *buf, int max)
{
    char tmp[12]; int i = 0, j = 0;
    if (!v) { buf[j++]='0'; buf[j]=0; return; }
    while (v && i<11) { tmp[i++]=(char)('0'+v%10); v/=10; }
    while (i-- && j<max-1) buf[j++]=tmp[i];
    buf[j]=0;
}

/* =========================================================================
 * Per-instance rendering
 * ========================================================================= */

static void inst_draw_contents(ShellInstance *s)
{
    int wx=s->wx, wy=s->wy, ww=s->ww, wh=s->wh;

    int body_w = ww - BORDER_L - BORDER_R;
    int body_y  = wy + TITLEBAR_H + 1;
    int body_h  = wh - TITLEBAR_H - INPUTBAR_H - WM_SCROLLBAR_W - 1;

    /* History area */
    FB_FillRect(wx+BORDER_L, body_y, body_w, body_h, WB_BLACK);

    /* Separator */
    FB_DrawHLine(wx+BORDER_L, body_y + body_h, body_w, WB_DARK_GREY);

    /* Input bar */
    FB_FillRect(wx+BORDER_L, body_y + body_h + 1,
                body_w, INPUTBAR_H, WB_BLACK);
}

static void inst_draw_history(ShellInstance *s)
{
    int wx=s->wx, wy=s->wy, ww=s->ww, wh=s->wh;
    int hx = wx + BORDER_L + 4;
    int hy = wy + TITLEBAR_H + 4;
    int hh = wh - TITLEBAR_H - INPUTBAR_H - WM_SCROLLBAR_W - 8;
    int rows = hh / 16;

    FB_FillRect(wx+BORDER_L, hy, ww-BORDER_L-BORDER_R, hh, WB_BLACK);

    /* Max chars that fit in the client width (8px per char, 4px left margin) */
    int max_chars = (ww - BORDER_L - BORDER_R - 8) / 8;
    if (max_chars < 1) max_chars = 1;

    int start = s->hist_count - rows - s->hist_scroll;
    if (start < 0) start = 0;
    for (int r = 0; r < rows; r++) {
        int li = start + r;
        if (li >= s->hist_count) break;
        /* Copy and truncate to fit */
        char clipped[MAX_LINE_LEN];
        const char *src = g_hist_buf[s->index][li % MAX_HIST_LINES];
        int ci = 0;
        while (ci < max_chars && src[ci]) { clipped[ci] = src[ci]; ci++; }
        clipped[ci] = '\0';
        FB_PutStr(hx, hy + r*16, clipped, WB_CREAM, WB_BLACK);
    }
}

static void inst_draw_input(ShellInstance *s)
{
    int wx=s->wx, wy=s->wy, ww=s->ww, wh=s->wh;
    int ix = wx + BORDER_L + 4;
    int iy = wy + wh - INPUTBAR_H - WM_SCROLLBAR_W - 2;

    /* Build prompt "N.UAOS> " */
    char prompt[12];
    prompt[0] = (char)('0' + s->number);
    prompt[1] = '.'; prompt[2]='U'; prompt[3]='A'; prompt[4]='O';
    prompt[5] = 'S'; prompt[6]='>'; prompt[7]=' '; prompt[8]=0;
    int plen = 8;

    int right_edge = wx + ww - BORDER_R;  /* pixel x of right clip boundary */

    FB_FillRect(wx+BORDER_L, iy, ww-BORDER_L-BORDER_R, INPUTBAR_H, WB_BLACK);
    FB_PutStr(ix, iy+1, prompt, WB_GREEN, WB_BLACK);

    int px = ix + plen*8;
    /* Only draw input text if prompt fits */
    if (px < right_edge) {
        int input_max_chars = (right_edge - px - 8) / 8; /* leave room for cursor */
        if (input_max_chars > 0) {
            char clipped[MAX_INPUT + 1];
            int ci = 0;
            while (ci < input_max_chars && s->input_buf[ci])
                { clipped[ci] = s->input_buf[ci]; ci++; }
            clipped[ci] = '\0';
            FB_PutStr(px, iy+1, clipped, WB_WHITE, WB_BLACK);
        }
        /* Block cursor — only if it fits */
        int cur_x = px + s->input_len * 8;
        if (cur_x + 8 <= right_edge)
            FB_FillRect(cur_x, iy+1, 8, 14, WB_WHITE);
    }
}

/* =========================================================================
 * Command dispatch (operates on a specific instance)
 * ========================================================================= */

/* Compute visible rows for this shell's current window height */
static int inst_rows(ShellInstance *s)
{
    int hh = s->wh - TITLEBAR_H - INPUTBAR_H - WM_SCROLLBAR_W - 8;
    return (hh > 0) ? hh / 16 : 1;
}

/* Push current scroll state into WM so the scrollbar thumb is correct */
static void inst_sync_scrollbar(ShellInstance *s)
{
    if (s->wm_handle < 0) return;
    int rows = inst_rows(s);
    int content_h = s->hist_count * 16;
    int view_h    = rows * 16;
    WM_SetScrollInfo(s->wm_handle, 0, content_h > view_h ? content_h : view_h + 1);
    /* scroll_y = lines-from-top * 16 */
    int from_top = s->hist_count - rows - s->hist_scroll;
    if (from_top < 0) from_top = 0;
    /* Directly update scroll_y via WM_GetScrollY trick: set via SetScrollInfo side-effect
     * is not enough — we need to write scroll_y. Use a small helper exposed below. */
    WM_SetScrollY(s->wm_handle, from_top * 16);
}

static void inst_print(ShellInstance *s, const char *line)
{
    int slot = s->hist_count % MAX_HIST_LINES;
    scopy(g_hist_buf[s->index][slot], line, MAX_LINE_LEN);
    s->hist_count++;
    s->hist_scroll = 0;
    inst_sync_scrollbar(s);
}

static void inst_cmd_help(ShellInstance *s)
{
    inst_print(s, "UAOS Shell v0.1 - built-in commands:");
    inst_print(s, "  help     show this help");
    inst_print(s, "  version  show OS version");
    inst_print(s, "  mem      memory information");
    inst_print(s, "  clear    clear the shell window");
    inst_print(s, "  reboot   warm reboot via keyboard controller");
    inst_print(s, "  run <prog> [args]  run an embedded Amiga binary");
}

static void inst_cmd_version(ShellInstance *s)
{
    inst_print(s, "Ultimate Amiga OS  v0.1.0-dev");
    inst_print(s, "Kernel: x86_64 ELF64, Multiboot2, long mode");

    char res[48];
    scopy(res, "Display: ", 48);
    char num[12];
    uint_to_dec_s(g_fb.width,  num, 12); scat(res, num, 48);
    scat(res, "x", 48);
    uint_to_dec_s(g_fb.height, num, 12); scat(res, num, 48);
    scat(res, " ", 48);
    uint_to_dec_s(g_fb.bpp,   num, 12); scat(res, num, 48);
    scat(res, "bpp linear framebuffer", 48);
    inst_print(s, res);

    inst_print(s, "Input: PS/2 keyboard + mouse, IRQ1/IRQ12");
}

static void inst_cmd_mem(ShellInstance *s)
{
    inst_print(s, "RAM:  512 MB (QEMU)");
    inst_print(s, "Kernel load: 0x0000000000100000");
    inst_print(s, "Framebuffer: mapped (GOP physical address)");
    inst_print(s, "Stack: 16 KB (bootstrap), no heap allocator yet");
}

static void inst_cmd_clear(ShellInstance *s)
{
    s->hist_count = 0;
    s->hist_scroll = 0;
    for (int i = 0; i < MAX_HIST_LINES; i++) g_hist_buf[s->index][i][0] = 0;
}

static void inst_cmd_reboot(ShellInstance *s)
{
    inst_print(s, "Rebooting...");
    inst_draw_history(s);
    inst_draw_input(s);
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

static void inst_dispatch(ShellInstance *s, const char *line)
{
    /* Echo */
    char echo[MAX_LINE_LEN];
    char prompt[12];
    prompt[0]=(char)('0'+s->number);
    prompt[1]='.';prompt[2]='U';prompt[3]='A';prompt[4]='O';
    prompt[5]='S';prompt[6]='>';prompt[7]=' ';prompt[8]=0;
    scopy(echo, prompt, MAX_LINE_LEN);
    scat(echo, line, MAX_LINE_LEN);
    inst_print(s, echo);

    while (*line == ' ') line++;
    if (!*line) return;

    const char *cmds[] = { "help","version","mem","clear","reboot","run", NULL };
    for (int i = 0; cmds[i]; i++) {
        const char *c = cmds[i];
        int cl = slen(c), match = 1;
        for (int j = 0; j < cl; j++)
            if (line[j] != c[j]) { match=0; break; }
        if (match && (line[cl]==0 || line[cl]==' ')) {
            if (i==0) inst_cmd_help(s);
            else if (i==1) inst_cmd_version(s);
            else if (i==2) inst_cmd_mem(s);
            else if (i==3) inst_cmd_clear(s);
            else if (i==4) inst_cmd_reboot(s);
            else if (i==5) {
                /* run <prog> [args] — forward to emulator */
                const char *args = line + 3;
                while (*args == ' ') args++;
                UAOS_Emu_RunByName(args, s, (UAOS_PrintFn)inst_print);
            }
            return;
        }
    }

    char msg[MAX_LINE_LEN];
    scopy(msg, "Unknown command: ", MAX_LINE_LEN);
    scat(msg, line, MAX_LINE_LEN);
    inst_print(s, msg);
}

/* =========================================================================
 * Key handler (operates on a specific instance)
 * ========================================================================= */

static void inst_handle_key(ShellInstance *s, char c)
{
    if (!g_fb.valid) return;
    if (c == VKEY_PGUP) {
        int rows = inst_rows(s);
        int max_scroll = s->hist_count - rows;
        if (max_scroll < 0) max_scroll = 0;
        s->hist_scroll += SCROLL_LINES;
        if (s->hist_scroll > max_scroll) s->hist_scroll = max_scroll;
        inst_sync_scrollbar(s);
        inst_draw_history(s);
        return;
    }
    if (c == VKEY_PGDN) {
        s->hist_scroll -= SCROLL_LINES;
        if (s->hist_scroll < 0) s->hist_scroll = 0;
        inst_sync_scrollbar(s);
        inst_draw_history(s);
        return;
    }
    if (c == '\n' || c == '\r') {
        s->input_buf[s->input_len] = 0;
        inst_dispatch(s, s->input_buf);
        s->input_len = 0;
        s->input_buf[0] = 0;
    } else if (c == '\b') {
        if (s->input_len > 0) { s->input_len--; s->input_buf[s->input_len]=0; }
    } else if (c >= 0x20 && c < 0x7F) {
        if (s->input_len < MAX_INPUT) {
            s->input_buf[s->input_len++] = c;
            s->input_buf[s->input_len]   = 0;
        }
    }
    inst_draw_history(s);
    inst_draw_input(s);
}

/* =========================================================================
 * WM draw/key shims — one per slot (routes WM callback to instance)
 * ========================================================================= */

#define MAKE_SHIMS(N) \
static void shell_draw_##N(int wx,int wy,int ww,int wh) { \
    ShellInstance *s=&g_shells[N]; \
    s->wx=wx;s->wy=wy;s->ww=ww;s->wh=wh; \
    /* Sync hist_scroll from WM scroll_y (covers scrollbar thumb drag) */ \
    if (s->wm_handle >= 0) { \
        int sy = WM_GetScrollY(s->wm_handle); \
        int rows = inst_rows(s); \
        int from_top = sy / 16; \
        int hs = s->hist_count - rows - from_top; \
        if (hs < 0) hs = 0; \
        s->hist_scroll = hs; \
    } \
    inst_draw_contents(s); inst_draw_history(s); inst_draw_input(s); } \
static void shell_key_##N(char c) { inst_handle_key(&g_shells[N],c); }

MAKE_SHIMS(0)
MAKE_SHIMS(1)
MAKE_SHIMS(2)
MAKE_SHIMS(3)

typedef void (*DrawFn)(int,int,int,int);
typedef void (*KeyFn)(char);

static const DrawFn k_draw_shims[MAX_SHELLS] = {
    shell_draw_0, shell_draw_1, shell_draw_2, shell_draw_3
};
static const KeyFn k_key_shims[MAX_SHELLS] = {
    shell_key_0, shell_key_1, shell_key_2, shell_key_3
};

/* =========================================================================
 * Internal: open one shell instance
 * ========================================================================= */

static void open_shell(int stagger)
{
    if (g_n_shells >= MAX_SHELLS) return;
    if (!g_fb.valid) return;

    int idx = g_n_shells++;
    ShellInstance *s = &g_shells[idx];

    s->wm_handle  = -1;
    s->number     = idx + 1;
    s->index      = idx;
    s->wx         = 24 + stagger * 28;
    s->wy         = 28 + stagger * 28;
    s->ww         = 600;
    s->wh         = 340;
    s->hist_count = 0;
    s->hist_scroll= 0;
    s->input_len  = 0;
    s->input_buf[0] = 0;
    for (int i = 0; i < MAX_HIST_LINES; i++) g_hist_buf[idx][i][0] = 0;

    char title[32];
    scopy(title, "Shell ", 32);
    char num[4]; num[0]=(char)('0'+s->number); num[1]=0;
    scat(title, num, 32);

    inst_print(s, "UAOS Shell  v0.1 - type 'help' for commands");
    inst_print(s, "");

    s->wm_handle = WM_AddWindow(s->wx, s->wy, s->ww, s->wh,
                                title,
                                k_draw_shims[idx],
                                k_key_shims[idx]);
    WM_Redraw();
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void ShellWin_Init(void)
{
    open_shell(0);
}

void ShellWin_Open(void)
{
    /* Find a free slot — also allow re-use of a closed slot */
    for (int i = 0; i < g_n_shells; i++) {
        if (!WM_IsWindowActive(g_shells[i].wm_handle)) {
            /* Reclaim this slot */
            g_n_shells = i;
            open_shell(i);
            return;
        }
    }
    open_shell(g_n_shells);
}

void ShellWin_Redraw(void)
{
    if (!g_fb.valid) return;
    WM_Redraw();
}

void ShellWin_HandleKey(char c)
{
    /* Legacy entry point — route to the focused window's key shim */
    int focus = WM_GetFocus();
    for (int i = 0; i < g_n_shells; i++) {
        if (g_shells[i].wm_handle == focus) {
            inst_handle_key(&g_shells[i], c);
            return;
        }
    }
}
