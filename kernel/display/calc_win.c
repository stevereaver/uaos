/* calc_win.c — UAOS Calculator
 *
 * Amiga Workbench 3.1-style calculator.
 * Layout (matches AmigaOS 3.1 Calc):
 *
 *   ┌─────────────────────────────┐
 *   │  [ display — right-aligned ]│
 *   ├─────────────────────────────┤
 *   │  7   8   9   CR  CE         │
 *   │  4   5   6   *   /          │
 *   │  1   2   3   +   -          │
 *   │  0   .   <-  +-  =          │
 *   └─────────────────────────────┘
 */

#include "calc_win.h"
#include "wm.h"
#include "framebuffer.h"
#include <stdint.h>
#include <stddef.h>

/* =========================================================================
 * Window layout constants
 * ========================================================================= */

#define WIN_W   196
#define WIN_H   196

#define TITLEBAR_H  WM_TITLEBAR_H
#define BORDER      2

#define DISP_H      22
#define BTN_COLS    5
#define BTN_ROWS    4
#define BTN_PAD     3

/* Button area starts below display */
#define BTN_AREA_X  (BORDER)
#define BTN_AREA_Y  (TITLEBAR_H + BORDER + DISP_H + BTN_PAD)

/* =========================================================================
 * Calculator state
 * ========================================================================= */

#define DISP_MAX  16

static int   g_wm_handle  = -1;
static int   g_win_x = 0, g_win_y = 0, g_win_w = 0, g_win_h = 0;

static char  g_disp[DISP_MAX + 1];   /* current display string              */
static int   g_disp_len   = 0;
static double g_accum     = 0.0;     /* accumulated value                   */
static double g_operand   = 0.0;     /* pending operand                     */
static char  g_op         = 0;       /* pending operator (+,-,*,/)          */
static int   g_new_num    = 1;       /* 1 = next digit starts a new number  */
static int   g_has_dot    = 0;       /* 1 = decimal point already entered   */

/* =========================================================================
 * Simple no-libc helpers
 * ========================================================================= */

static int str_len(const char *s) { int n=0; while(s[n]) n++; return n; }

/* Convert a double to a display string — integer if no fractional part */
static void double_to_str(double v, char *buf, int max)
{
    /* Handle sign */
    int neg = 0;
    if (v < 0.0) { neg = 1; v = -v; }

    /* Split into integer and fractional parts */
    long long iv = (long long)v;
    double frac  = v - (double)iv;

    /* Convert integer part */
    char tmp[24];
    int  ti = 0;
    if (iv == 0) {
        tmp[ti++] = '0';
    } else {
        long long t = iv;
        while (t > 0 && ti < 20) { tmp[ti++] = (char)('0' + t % 10); t /= 10; }
        /* reverse */
        for (int a=0, b=ti-1; a<b; a++, b--) { char c=tmp[a]; tmp[a]=tmp[b]; tmp[b]=c; }
    }

    int bi = 0;
    if (neg && bi < max-1) buf[bi++] = '-';
    for (int i=0; i<ti && bi<max-1; i++) buf[bi++] = tmp[i];

    /* Fractional digits (up to 6) if non-zero */
    if (frac > 0.0000001) {
        if (bi < max-1) buf[bi++] = '.';
        for (int d=0; d<6 && bi<max-1; d++) {
            frac *= 10.0;
            int digit = (int)frac;
            buf[bi++] = (char)('0' + digit);
            frac -= (double)digit;
            if (frac < 0.0000001) break;
        }
    }

    buf[bi] = '\0';
}

static void update_disp_from_double(double v)
{
    double_to_str(v, g_disp, DISP_MAX);
    g_disp_len = str_len(g_disp);
    g_has_dot  = 0;
    for (int i=0; i<g_disp_len; i++) if (g_disp[i]=='.') { g_has_dot=1; break; }
}

static double str_to_double(const char *s)
{
    double result = 0.0;
    int neg = 0, i = 0;
    if (s[i] == '-') { neg = 1; i++; }
    while (s[i] >= '0' && s[i] <= '9') { result = result*10.0 + (s[i]-'0'); i++; }
    if (s[i] == '.') {
        i++;
        double frac = 0.1;
        while (s[i] >= '0' && s[i] <= '9') { result += (s[i]-'0')*frac; frac*=0.1; i++; }
    }
    return neg ? -result : result;
}

/* =========================================================================
 * Button layout
 * ========================================================================= */

/* Button labels — 4 rows × 5 cols */
static const char *k_labels[BTN_ROWS][BTN_COLS] = {
    { "7", "8", "9", "CR", "CE" },
    { "4", "5", "6", "*",  "/"  },
    { "1", "2", "3", "+",  "-"  },
    { "0", ".",  "<-","+-", "=" },
};

/* Compute button pixel rect for given row/col within a window */
static void btn_rect(int wx, int wy, int ww,
                     int row, int col,
                     int *bx, int *by, int *bw, int *bh)
{
    int area_x = wx + BTN_AREA_X;
    int area_y = wy + BTN_AREA_Y;
    int area_w = ww - BORDER*2;
    int area_h = (wy + g_win_h) - (wy + BTN_AREA_Y) - BORDER;

    int total_pad_x = BTN_PAD * (BTN_COLS + 1);
    int total_pad_y = BTN_PAD * (BTN_ROWS + 1);
    int cell_w = (area_w - total_pad_x) / BTN_COLS;
    int cell_h = (area_h - total_pad_y) / BTN_ROWS;

    *bx = area_x + BTN_PAD + col * (cell_w + BTN_PAD);
    *by = area_y + BTN_PAD + row * (cell_h + BTN_PAD);
    *bw = cell_w;
    *bh = cell_h;
}

/* =========================================================================
 * Calculator logic
 * ========================================================================= */

static void calc_apply_op(void)
{
    double cur = str_to_double(g_disp);
    double result = g_accum;
    switch (g_op) {
        case '+': result = g_accum + cur; break;
        case '-': result = g_accum - cur; break;
        case '*': result = g_accum * cur; break;
        case '/': result = (cur != 0.0) ? g_accum / cur : 0.0; break;
        default:  result = cur; break;
    }
    g_accum = result;
    update_disp_from_double(result);
}

static void calc_press(const char *label)
{
    char c = label[0];

    /* Digit */
    if (c >= '0' && c <= '9') {
        if (g_new_num) {
            g_disp[0] = c; g_disp[1] = '\0';
            g_disp_len = 1; g_has_dot = 0; g_new_num = 0;
        } else if (g_disp_len < DISP_MAX) {
            g_disp[g_disp_len++] = c;
            g_disp[g_disp_len]   = '\0';
        }
        return;
    }

    /* Decimal point */
    if (c == '.') {
        if (g_new_num) {
            g_disp[0]='0'; g_disp[1]='.'; g_disp[2]='\0';
            g_disp_len=2; g_has_dot=1; g_new_num=0;
        } else if (!g_has_dot && g_disp_len < DISP_MAX) {
            g_disp[g_disp_len++] = '.';
            g_disp[g_disp_len]   = '\0';
            g_has_dot = 1;
        }
        return;
    }

    /* Backspace */
    if (c == '<') {
        if (!g_new_num && g_disp_len > 0) {
            if (g_disp[g_disp_len-1] == '.') g_has_dot = 0;
            g_disp[--g_disp_len] = '\0';
            if (g_disp_len == 0) {
                g_disp[0]='0'; g_disp[1]='\0'; g_disp_len=1; g_new_num=1;
            }
        }
        return;
    }

    /* CR — clear result (keep accumulator) */
    if (c == 'C' && label[1] == 'R') {
        g_disp[0]='0'; g_disp[1]='\0'; g_disp_len=1;
        g_has_dot=0; g_new_num=1;
        return;
    }

    /* CE — clear everything */
    if (c == 'C' && label[1] == 'E') {
        g_disp[0]='0'; g_disp[1]='\0'; g_disp_len=1;
        g_has_dot=0; g_new_num=1;
        g_accum=0.0; g_operand=0.0; g_op=0;
        return;
    }

    /* +/- toggle sign */
    if (c == '+' && label[1] == '-') {
        double v = str_to_double(g_disp);
        update_disp_from_double(-v);
        g_new_num = 0;
        return;
    }

    /* = compute */
    if (c == '=') {
        if (g_op) {
            calc_apply_op();
            g_op = 0; g_new_num = 1;
        }
        return;
    }

    /* Operator: + - * / */
    if (c == '+' || c == '-' || c == '*' || c == '/') {
        if (g_op && !g_new_num) {
            calc_apply_op();
        } else {
            g_accum = str_to_double(g_disp);
        }
        g_op = c;
        g_new_num = 1;
        return;
    }
}

/* =========================================================================
 * Drawing
 * ========================================================================= */

static void draw_bevel(int x, int y, int w, int h, int raised)
{
    uint32_t hi = raised ? WB_WHITE     : WB_DARK_GREY;
    uint32_t lo = raised ? WB_DARK_GREY : WB_WHITE;
    FB_DrawHLine(x,         y,         w, hi);
    FB_DrawVLine(x,         y,         h, hi);
    FB_DrawHLine(x,         y + h - 1, w, lo);
    FB_DrawVLine(x + w - 1, y,         h, lo);
}

static void calc_draw(int wx, int wy, int ww, int wh)
{
    g_win_x = wx; g_win_y = wy; g_win_w = ww; g_win_h = wh;

    /* Client background */
    int cx = wx + BORDER;
    int cy = wy + TITLEBAR_H;
    int cw = ww - BORDER * 2;
    int ch = wh - TITLEBAR_H - BORDER;
    FB_FillRect(cx, cy, cw, ch, WB_GREY);

    /* Display area */
    int dx = cx;
    int dy = cy + BORDER;
    int dw = cw;
    int dh = DISP_H;
    FB_FillRect(dx, dy, dw, dh, WB_WHITE);
    draw_bevel(dx, dy, dw, dh, 0);

    /* Display text — right-aligned */
    int tlen = str_len(g_disp);
    int tx   = dx + dw - tlen * 8 - 6;
    int ty   = dy + (dh - 16) / 2;
    FB_PutStr(tx, ty, g_disp, WB_BLACK, WB_WHITE);

    /* Buttons */
    for (int row = 0; row < BTN_ROWS; row++) {
        for (int col = 0; col < BTN_COLS; col++) {
            int bx, by, bw, bh;
            btn_rect(wx, wy, ww, row, col, &bx, &by, &bw, &bh);

            FB_FillRect(bx, by, bw, bh, WB_GREY);
            draw_bevel(bx, by, bw, bh, 1);

            const char *lbl = k_labels[row][col];
            FB_PutStrCentred(bx, by, bw, bh, lbl, WB_BLACK, WB_GREY);
        }
    }
}

/* =========================================================================
 * Input handling
 * ========================================================================= */

static void calc_key(char c)
{
    const char *label = NULL;
    char buf[3] = { c, '\0', '\0' };

    if (c >= '0' && c <= '9')       label = buf;
    else if (c == '.')              label = ".";
    else if (c == '+')              label = "+";
    else if (c == '-')              label = "-";
    else if (c == '*')              label = "*";
    else if (c == '/')              label = "/";
    else if (c == '=' || c == '\r') label = "=";
    else if (c == '\b')             label = "<-";

    if (label) {
        calc_press(label);
        if (g_wm_handle >= 0) {
            calc_draw(g_win_x, g_win_y, g_win_w, g_win_h);
        }
    }
}

static void calc_click(int handle, int mx, int my)
{
    (void)handle;
    for (int row = 0; row < BTN_ROWS; row++) {
        for (int col = 0; col < BTN_COLS; col++) {
            int bx, by, bw, bh;
            btn_rect(g_win_x, g_win_y, g_win_w, row, col, &bx, &by, &bw, &bh);
            if (mx >= bx && mx < bx + bw &&
                my >= by && my < by + bh) {
                calc_press(k_labels[row][col]);
                calc_draw(g_win_x, g_win_y, g_win_w, g_win_h);
                return;
            }
        }
    }
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void CalcWin_Open(void)
{
    /* Verify the slot still belongs to us — WM slots are recycled, so
     * IsWindowActive alone is not sufficient (another window could reuse
     * the same slot index after Calc was closed). */
    if (g_wm_handle >= 0 && WM_GetDrawFn(g_wm_handle) == calc_draw) {
        WM_RaiseWindow(g_wm_handle);
        WM_Redraw();
        return;
    }
    g_wm_handle = -1;

    /* Reset state */
    g_disp[0]='0'; g_disp[1]='\0'; g_disp_len=1;
    g_has_dot=0; g_new_num=1;
    g_accum=0.0; g_operand=0.0; g_op=0;

    /* Centre on screen */
    int wx = ((int)g_fb.width  - WIN_W) / 2;
    int wy = ((int)g_fb.height - WIN_H) / 2;
    if (wy < WM_TITLEBAR_H + 4) wy = WM_TITLEBAR_H + 4;

    g_wm_handle = WM_AddWindow(wx, wy, WIN_W, WIN_H,
                               "Calc", calc_draw, calc_key);
    if (g_wm_handle < 0) {
        /* Failed to create window (likely too many windows open) */
        g_wm_handle = -1;  /* Reset handle since we failed */
        return;
    }
    WM_SetClickHandler(g_wm_handle, calc_click);
    WM_Redraw();
}
