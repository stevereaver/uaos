/* requester.c — UAOS AmigaOS-style EasyRequest / file requester system
 *
 * Modal dialog windows for confirmation, string input, and information.
 * Uses the WM window manager for rendering and input.
 */

#include "requester.h"
#include "wm.h"
#include "framebuffer.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* =========================================================================
 * Helpers
 * ========================================================================= */

static int str_eq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static int str_len(const char *s)
{
    int n = 0; while (s[n]) n++; return n;
}

static void str_cp(char *dst, const char *src, int max)
{
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void draw_bevel(int x, int y, int w, int h, int raised)
{
    uint32_t lo = raised ? WB_DARK_GREY : WB_WHITE;
    uint32_t hi = raised ? WB_WHITE     : WB_DARK_GREY;
    FB_DrawHLine(x,         y,         w, hi);
    FB_DrawVLine(x,         y,         h, hi);
    FB_DrawHLine(x,         y + h - 1, w, lo);
    FB_DrawVLine(x + w - 1, y,         h, lo);
}

/* =========================================================================
 * Requester state
 * ========================================================================= */

#define REQ_MAX_TEXT    128
#define REQ_MAX_LINES    8
#define REQ_MAX_LABEL   64
#define REQ_BTN_MAX      2

typedef enum {
    REQ_TYPE_CONFIRM,
    REQ_TYPE_STRING,
    REQ_TYPE_INFO,
} ReqType;

typedef struct {
    ReqType      type;
    int          wm_handle;
    char         title[32];
    char         body_lines[REQ_MAX_LINES][REQ_MAX_LABEL];
    int          n_lines;
    char         btn_labels[REQ_BTN_MAX][16];
    int          n_buttons;
    char         text[REQ_MAX_TEXT];
    int          text_len;
    int          cursor_pos;
    int          max_chars;
    int          active_btn;    /* 0 = left, 1 = right (hover/pressed) */
    int          btn_pressed;   /* 1 while mouse held on a button */
    int          text_focused;  /* 1 = text field has focus (for string req) */
    ReqCallback  callback;
    void        *user_data;
    /* Cached client rect */
    int cx, cy, cw, ch;
    /* Button rects */
    int btn_x[REQ_BTN_MAX], btn_y[REQ_BTN_MAX], btn_w[REQ_BTN_MAX], btn_h[REQ_BTN_MAX];
    /* Text field rect */
    int tf_x, tf_y, tf_w, tf_h;
} Requester;

static Requester g_req;

/* Last-message storage — recorded whenever a requester is opened so the
 * Workbench ▸ Last Message menu item can replay it. */
static char g_last_title[32];
static char g_last_body[256];
static int  g_last_valid = 0;

/* Record the current requester's title + body into the last-message store. */
static void record_last_message(void)
{
    int ti = 0;
    while (ti < (int)sizeof(g_last_title) - 1 && g_req.title[ti]) {
        g_last_title[ti] = g_req.title[ti]; ti++;
    }
    g_last_title[ti] = '\0';

    int bi = 0;
    for (int li = 0; li < g_req.n_lines && bi < (int)sizeof(g_last_body) - 2; li++) {
        if (li > 0 && bi < (int)sizeof(g_last_body) - 2)
            g_last_body[bi++] = '\n';
        int ci = 0;
        while (g_req.body_lines[li][ci] && bi < (int)sizeof(g_last_body) - 2) {
            g_last_body[bi++] = g_req.body_lines[li][ci++];
        }
    }
    g_last_body[bi] = '\0';
    g_last_valid = 1;
}

/* =========================================================================
 * Layout constants
 * ========================================================================= */

#define REQ_PAD        8
#define REQ_LINE_H     16
#define REQ_BTN_W     72
#define REQ_BTN_H     22
#define REQ_TF_H      22
#define REQ_MIN_W    260
#define REQ_MAX_W    420

/* =========================================================================
 * Split body text into lines on '\n'
 * ========================================================================= */
static void split_lines(const char *body)
{
    g_req.n_lines = 0;
    if (!body) return;
    int li = 0;
    int ci = 0;
    while (body[li] && g_req.n_lines < REQ_MAX_LINES) {
        if (body[li] == '\n') {
            g_req.body_lines[g_req.n_lines][ci] = '\0';
            g_req.n_lines++;
            ci = 0;
            li++;
        } else if (ci < REQ_MAX_LABEL - 1) {
            g_req.body_lines[g_req.n_lines][ci++] = body[li++];
        } else {
            li++;
        }
    }
    if (g_req.n_lines < REQ_MAX_LINES) {
        g_req.body_lines[g_req.n_lines][ci] = '\0';
        g_req.n_lines++;
    }
}

/* =========================================================================
 * Compute window size based on content
 * ========================================================================= */
static void compute_size(int *out_w, int *out_h)
{
    int max_text_w = 0;
    for (int i = 0; i < g_req.n_lines; i++) {
        int w = str_len(g_req.body_lines[i]) * 8;
        if (w > max_text_w) max_text_w = w;
    }

    int content_w = max_text_w + REQ_PAD * 2;
    if (g_req.type == REQ_TYPE_STRING) {
        int prompt_w = str_len(g_req.body_lines[0]) * 8 + REQ_PAD * 2;
        if (prompt_w > content_w) content_w = prompt_w;
        int tf_w = g_req.max_chars * 8 + REQ_PAD * 2 + 4;
        if (tf_w > content_w) content_w = tf_w;
    }

    /* Button row width */
    int btn_row_w = REQ_PAD;
    for (int i = 0; i < g_req.n_buttons; i++) {
        btn_row_w += REQ_BTN_W + REQ_PAD;
    }
    if (btn_row_w > content_w) content_w = btn_row_w;

    if (content_w < REQ_MIN_W) content_w = REQ_MIN_W;
    if (content_w > REQ_MAX_W) content_w = REQ_MAX_W;

    int content_h = REQ_PAD + g_req.n_lines * REQ_LINE_H + REQ_PAD;

    if (g_req.type == REQ_TYPE_STRING) {
        content_h += REQ_LINE_H + REQ_TF_H + REQ_PAD;
    }

    content_h += REQ_BTN_H + REQ_PAD;

    *out_w = content_w;
    *out_h = content_h + WM_TITLEBAR_H + WM_SCROLLBAR_W;
}

/* =========================================================================
 * Draw callback
 * ========================================================================= */
static void req_draw(int wx, int wy, int ww, int wh)
{
    (void)ww; (void)wh;
    int cx = wx + 1;
    int cy = wy + WM_TITLEBAR_H;
    int cw = ww - 1 - WM_SCROLLBAR_W;
    int ch = wh - WM_TITLEBAR_H - WM_SCROLLBAR_W;
    g_req.cx = cx; g_req.cy = cy; g_req.cw = cw; g_req.ch = ch;

    FB_FillRect(cx, cy, cw, ch, WB_GREY);
    draw_bevel(cx, cy, cw, ch, 0);

    int y = cy + REQ_PAD;

    /* Body text lines */
    for (int i = 0; i < g_req.n_lines; i++) {
        if (g_req.type == REQ_TYPE_STRING && i == 0) {
            /* Prompt label */
            FB_PutStr(cx + REQ_PAD, y, g_req.body_lines[i], WB_BLACK, WB_GREY);
            y += REQ_LINE_H;
            /* Text input field */
            int tf_w = cw - REQ_PAD * 2 - 4;
            if (tf_w > g_req.max_chars * 8 + 4) tf_w = g_req.max_chars * 8 + 4;
            g_req.tf_x = cx + REQ_PAD;
            g_req.tf_y = y;
            g_req.tf_w = tf_w;
            g_req.tf_h = REQ_TF_H;
            FB_FillRect(g_req.tf_x, g_req.tf_y, tf_w, REQ_TF_H, WB_WHITE);
            draw_bevel(g_req.tf_x, g_req.tf_y, tf_w, REQ_TF_H, 0);
            /* Text content */
            FB_PutStr(g_req.tf_x + 4, g_req.tf_y + 3, g_req.text, WB_BLACK, WB_WHITE);
            /* Cursor */
            if (g_req.text_focused) {
                int cx_pos = g_req.tf_x + 4 + g_req.cursor_pos * 8;
                FB_DrawVLine(cx_pos, g_req.tf_y + 3, REQ_TF_H - 6, WB_BLACK);
            }
            y += REQ_TF_H + REQ_PAD;
        } else {
            FB_PutStr(cx + REQ_PAD, y, g_req.body_lines[i], WB_BLACK, WB_GREY);
            y += REQ_LINE_H;
        }
    }

    /* Buttons */
    int btn_row_w = REQ_PAD;
    for (int i = 0; i < g_req.n_buttons; i++)
        btn_row_w += REQ_BTN_W + REQ_PAD;

    int bx_start = cx + (cw - btn_row_w) / 2 + REQ_PAD;
    int by = cy + ch - REQ_BTN_H - REQ_PAD;

    for (int i = 0; i < g_req.n_buttons; i++) {
        int bx = bx_start + i * (REQ_BTN_W + REQ_PAD);
        g_req.btn_x[i] = bx;
        g_req.btn_y[i] = by;
        g_req.btn_w[i] = REQ_BTN_W;
        g_req.btn_h[i] = REQ_BTN_H;

        uint32_t bg = WB_GREY;
        int raised = 1;
        if (g_req.btn_pressed && g_req.active_btn == i) {
            raised = 0;
        }
        FB_FillRect(bx, by, REQ_BTN_W, REQ_BTN_H, bg);
        draw_bevel(bx, by, REQ_BTN_W, REQ_BTN_H, raised);

        /* Centre button label */
        int lbl_len = str_len(g_req.btn_labels[i]);
        int tx = bx + (REQ_BTN_W - lbl_len * 8) / 2;
        int ty = by + (REQ_BTN_H - 16) / 2;
        FB_PutStr(tx, ty, g_req.btn_labels[i], WB_BLACK, bg);
    }
}

/* =========================================================================
 * Key callback
 * ========================================================================= */
static void req_key(char c)
{
    if (g_req.type == REQ_TYPE_STRING && g_req.text_focused) {
        if (c == '\n' || c == '\r') {
            /* Enter = OK */
            if (g_req.callback)
                g_req.callback(REQ_BTN_OK, g_req.text, g_req.user_data);
            Requester_Close();
            return;
        }
        if (c == 27) {
            /* Escape = Cancel */
            if (g_req.callback)
                g_req.callback(REQ_BTN_CANCEL, NULL, g_req.user_data);
            Requester_Close();
            return;
        }
        if (c == '\b') {
            if (g_req.cursor_pos > 0) {
                for (int i = g_req.cursor_pos - 1; i < g_req.text_len; i++)
                    g_req.text[i] = g_req.text[i + 1];
                g_req.cursor_pos--;
                g_req.text_len--;
            }
            WM_Redraw();
            return;
        }
        if (c >= 32 && c < 127) {
            if (g_req.text_len < g_req.max_chars - 1) {
                for (int i = g_req.text_len; i >= g_req.cursor_pos; i--)
                    g_req.text[i + 1] = g_req.text[i];
                g_req.text[g_req.cursor_pos] = c;
                g_req.cursor_pos++;
                g_req.text_len++;
            }
            WM_Redraw();
            return;
        }
        /* Arrow keys */
        if (c == 0x05 && g_req.cursor_pos > 0) {
            g_req.cursor_pos--;
            WM_Redraw();
            return;
        }
        if (c == 0x06 && g_req.cursor_pos < g_req.text_len) {
            g_req.cursor_pos++;
            WM_Redraw();
            return;
        }
        if (c == 0x03) {
            g_req.cursor_pos = 0;
            WM_Redraw();
            return;
        }
        if (c == 0x04) {
            g_req.cursor_pos = g_req.text_len;
            WM_Redraw();
            return;
        }
    } else {
        /* Non-string requester: Enter = first button, Esc = cancel/close */
        if (c == '\n' || c == '\r') {
            if (g_req.callback)
                g_req.callback(REQ_BTN_OK, NULL, g_req.user_data);
            Requester_Close();
            return;
        }
        if (c == 27) {
            if (g_req.n_buttons > 1 && g_req.callback)
                g_req.callback(REQ_BTN_CANCEL, NULL, g_req.user_data);
            Requester_Close();
            return;
        }
    }
}

/* =========================================================================
 * Click callback
 * ========================================================================= */
static void req_click(int handle, int mx, int my)
{
    (void)handle;

    /* Check text field click (string requester) */
    if (g_req.type == REQ_TYPE_STRING) {
        if (mx >= g_req.tf_x && mx < g_req.tf_x + g_req.tf_w &&
            my >= g_req.tf_y && my < g_req.tf_y + g_req.tf_h) {
            g_req.text_focused = 1;
            /* Position cursor at click */
            int rel = (mx - g_req.tf_x - 4) / 8;
            if (rel < 0) rel = 0;
            if (rel > g_req.text_len) rel = g_req.text_len;
            g_req.cursor_pos = rel;
            WM_Redraw();
            return;
        }
        /* Click outside text field — defocus */
        g_req.text_focused = 0;
    }

    /* Check button hits */
    for (int i = 0; i < g_req.n_buttons; i++) {
        if (mx >= g_req.btn_x[i] && mx < g_req.btn_x[i] + g_req.btn_w[i] &&
            my >= g_req.btn_y[i] && my < g_req.btn_y[i] + g_req.btn_h[i]) {
            g_req.active_btn = i;
            g_req.btn_pressed = 1;
            WM_Redraw();
            return;
        }
    }
}

static void req_move(int handle, int mx, int my)
{
    (void)handle;
    /* Update button hover */
    int new_active = -1;
    for (int i = 0; i < g_req.n_buttons; i++) {
        if (mx >= g_req.btn_x[i] && mx < g_req.btn_x[i] + g_req.btn_w[i] &&
            my >= g_req.btn_y[i] && my < g_req.btn_y[i] + g_req.btn_h[i]) {
            new_active = i;
            break;
        }
    }
    if (new_active != g_req.active_btn || !g_req.btn_pressed) {
        g_req.active_btn = new_active;
        g_req.btn_pressed = (new_active >= 0) ? 1 : 0;
        WM_Redraw();
    }
}

static void req_release(int handle, int mx, int my)
{
    (void)handle;
    if (g_req.btn_pressed && g_req.active_btn >= 0) {
        int btn = g_req.active_btn;
        /* Verify release is still on the button */
        if (mx >= g_req.btn_x[btn] && mx < g_req.btn_x[btn] + g_req.btn_w[btn] &&
            my >= g_req.btn_y[btn] && my < g_req.btn_y[btn] + g_req.btn_h[btn]) {
            g_req.btn_pressed = 0;
            /* Map button index to OK/CANCEL */
            int btn_id = (btn == 0) ? REQ_BTN_OK : REQ_BTN_CANCEL;
            const char *text = (g_req.type == REQ_TYPE_STRING) ? g_req.text : NULL;
            ReqCallback cb = g_req.callback;
            void *ud = g_req.user_data;
            Requester_Close();
            if (cb) cb(btn_id, text, ud);
            return;
        }
    }
    g_req.btn_pressed = 0;
    WM_Redraw();
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void Requester_Confirm(const char *title, const char *body,
                       const char *btn1, const char *btn2,
                       ReqCallback cb, void *user_data)
{
    if (g_req.wm_handle >= 0) return;  /* already active */

    memset(&g_req, 0, sizeof(g_req));
    g_req.type = REQ_TYPE_CONFIRM;
    g_req.callback = cb;
    g_req.user_data = user_data;
    g_req.text_focused = 0;
    str_cp(g_req.title, title, sizeof(g_req.title));
    split_lines(body);

    g_req.n_buttons = 0;
    if (btn1) { str_cp(g_req.btn_labels[0], btn1, sizeof(g_req.btn_labels[0])); g_req.n_buttons++; }
    if (btn2) { str_cp(g_req.btn_labels[1], btn2, sizeof(g_req.btn_labels[1])); g_req.n_buttons++; }
    if (g_req.n_buttons == 0) { str_cp(g_req.btn_labels[0], "OK", 16); g_req.n_buttons = 1; }

    int w, h;
    compute_size(&w, &h);

    int sx = (int)g_fb.width;
    int sy = (int)g_fb.height;
    int wx = (sx - w) / 2;
    int wy = (sy - h) / 2;
    if (wy < WM_TITLEBAR_H + 4) wy = WM_TITLEBAR_H + 4;

    g_req.wm_handle = WM_AddWindow(wx, wy, w, h, g_req.title, req_draw, req_key);
    if (g_req.wm_handle < 0) return;
    WM_SetClickHandler(g_req.wm_handle, req_click);
    WM_SetMouseMoveHandler(g_req.wm_handle, req_move);
    WM_SetMouseReleaseHandler(g_req.wm_handle, req_release);
    record_last_message();
    WM_RaiseWindow(g_req.wm_handle);
    WM_Redraw();
}

void Requester_String(const char *title, const char *prompt,
                      const char *initial, int max_chars,
                      ReqCallback cb, void *user_data)
{
    if (g_req.wm_handle >= 0) return;

    memset(&g_req, 0, sizeof(g_req));
    g_req.type = REQ_TYPE_STRING;
    g_req.callback = cb;
    g_req.user_data = user_data;
    g_req.text_focused = 1;
    g_req.max_chars = max_chars;
    if (g_req.max_chars > REQ_MAX_TEXT) g_req.max_chars = REQ_MAX_TEXT;
    str_cp(g_req.title, title, sizeof(g_req.title));
    split_lines(prompt);

    if (initial) {
        str_cp(g_req.text, initial, g_req.max_chars);
        g_req.text_len = str_len(g_req.text);
        g_req.cursor_pos = g_req.text_len;
    } else {
        g_req.text[0] = '\0';
        g_req.text_len = 0;
        g_req.cursor_pos = 0;
    }

    str_cp(g_req.btn_labels[0], "OK", 16);
    str_cp(g_req.btn_labels[1], "Cancel", 16);
    g_req.n_buttons = 2;

    int w, h;
    compute_size(&w, &h);

    int sx = (int)g_fb.width;
    int sy = (int)g_fb.height;
    int wx = (sx - w) / 2;
    int wy = (sy - h) / 2;
    if (wy < WM_TITLEBAR_H + 4) wy = WM_TITLEBAR_H + 4;

    g_req.wm_handle = WM_AddWindow(wx, wy, w, h, g_req.title, req_draw, req_key);
    if (g_req.wm_handle < 0) return;
    WM_SetClickHandler(g_req.wm_handle, req_click);
    WM_SetMouseMoveHandler(g_req.wm_handle, req_move);
    WM_SetMouseReleaseHandler(g_req.wm_handle, req_release);
    record_last_message();
    WM_RaiseWindow(g_req.wm_handle);
    WM_Redraw();
}

void Requester_Info(const char *title, const char **lines,
                    ReqCallback cb, void *user_data)
{
    if (g_req.wm_handle >= 0) return;

    memset(&g_req, 0, sizeof(g_req));
    g_req.type = REQ_TYPE_INFO;
    g_req.callback = cb;
    g_req.user_data = user_data;
    str_cp(g_req.title, title, sizeof(g_req.title));

    g_req.n_lines = 0;
    if (lines) {
        while (lines[g_req.n_lines] && g_req.n_lines < REQ_MAX_LINES) {
            str_cp(g_req.body_lines[g_req.n_lines], lines[g_req.n_lines], REQ_MAX_LABEL);
            g_req.n_lines++;
        }
    }

    str_cp(g_req.btn_labels[0], "OK", 16);
    g_req.n_buttons = 1;

    int w, h;
    compute_size(&w, &h);

    int sx = (int)g_fb.width;
    int sy = (int)g_fb.height;
    int wx = (sx - w) / 2;
    int wy = (sy - h) / 2;
    if (wy < WM_TITLEBAR_H + 4) wy = WM_TITLEBAR_H + 4;

    g_req.wm_handle = WM_AddWindow(wx, wy, w, h, g_req.title, req_draw, req_key);
    if (g_req.wm_handle < 0) return;
    WM_SetClickHandler(g_req.wm_handle, req_click);
    WM_SetMouseMoveHandler(g_req.wm_handle, req_move);
    WM_SetMouseReleaseHandler(g_req.wm_handle, req_release);
    record_last_message();
    WM_RaiseWindow(g_req.wm_handle);
    WM_Redraw();
}

void Requester_Close(void)
{
    if (g_req.wm_handle >= 0) {
        WM_CloseWindow(g_req.wm_handle);
        g_req.wm_handle = -1;
    }
    g_req.callback = NULL;
    g_req.user_data = NULL;
    g_req.text_focused = 0;
    g_req.btn_pressed = 0;
    WM_Redraw();
}

int Requester_IsActive(void)
{
    return (g_req.wm_handle >= 0 && WM_IsWindowActive(g_req.wm_handle)) ? 1 : 0;
}

void Requester_GetLastMessage(char *title_out, int title_max,
                              char *body_out, int body_max)
{
    if (!g_last_valid || !title_out || !body_out) {
        if (title_out && title_max > 0) title_out[0] = '\0';
        if (body_out && body_max > 0) body_out[0] = '\0';
        return;
    }
    int i = 0;
    while (i < title_max - 1 && g_last_title[i]) {
        title_out[i] = g_last_title[i]; i++;
    }
    title_out[i] = '\0';
    i = 0;
    while (i < body_max - 1 && g_last_body[i]) {
        body_out[i] = g_last_body[i]; i++;
    }
    body_out[i] = '\0';
}
