/* requester.c — UAOS AmigaOS-style EasyRequest / file requester system
 *
 * Modal dialog windows for confirmation, string input, and information.
 * Uses the WM window manager for rendering and input.
 */

#include "requester.h"
#include "wm.h"
#include "framebuffer.h"
#include "gadgets.h"
#include "../irq/ps2kbd.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* str_* helpers and bevel/gadget drawing now live in gadgets.c */
#define str_len  gad_slen
#define str_cp   gad_str_cp
#define draw_bevel gad_bevel

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
    int          max_chars;
    int          active_btn;    /* 0 = left, 1 = right (hover/pressed) */
    int          btn_pressed;   /* 1 while mouse held on a button */
    ReqCallback  callback;
    void        *user_data;
    /* Cached client rect */
    int cx, cy, cw, ch;
    /* Shared gadgets: buttons + string field (tf.buf -> text) */
    Gad          btns[REQ_BTN_MAX];
    Gad          tf;
} Requester;

static Requester g_req = { .wm_handle = -1 };  /* -1 = no requester active;
 * BSS-zeroed 0 would trip the "already active" guard on the first open. */

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
            g_req.tf.kind = GAD_STRING;
            g_req.tf.x = cx + REQ_PAD;
            g_req.tf.y = y;
            g_req.tf.w = tf_w;
            g_req.tf.h = REQ_TF_H;
            gad_draw(&g_req.tf);
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
        Gad *b = &g_req.btns[i];
        b->kind = GAD_BUTTON;
        b->x = bx_start + i * (REQ_BTN_W + REQ_PAD);
        b->y = by;
        b->w = REQ_BTN_W;
        b->h = REQ_BTN_H;
        b->text = g_req.btn_labels[i];
        b->pressed = (g_req.btn_pressed && g_req.active_btn == i);
        gad_draw(b);
    }
}

/* =========================================================================
 * Key callback
 * ========================================================================= */
static void req_key(char c)
{
    if (g_req.type == REQ_TYPE_STRING && g_req.tf.focused) {
        if (c == '\n' || c == '\r') {
            /* Enter = OK — close before the callback so focus returns to
             * the window that opened the requester (matches req_release). */
            ReqCallback cb = g_req.callback;
            const char *text = g_req.text;
            void *ud = g_req.user_data;
            Requester_Close();
            if (cb) cb(REQ_BTN_OK, text, ud);
            return;
        }
        if (c == 27) {
            /* Escape = Cancel */
            ReqCallback cb = g_req.callback;
            void *ud = g_req.user_data;
            Requester_Close();
            if (cb) cb(REQ_BTN_CANCEL, NULL, ud);
            return;
        }
        /* Editing (insert/delete/cursor keys) is the string gadget's job */
        if (gad_event(&g_req.tf, GAD_KEY, c, 0) == GADE_CHANGE)
            WM_Redraw();
    } else {
        /* Non-string requester: Enter = first button, Esc = cancel/close.
         * Close before the callback so focus returns first. */
        if (c == '\n' || c == '\r') {
            ReqCallback cb = g_req.callback;
            void *ud = g_req.user_data;
            Requester_Close();
            if (cb) cb(REQ_BTN_OK, NULL, ud);
            return;
        }
        if (c == 27) {
            ReqCallback cb = (g_req.n_buttons > 1) ? g_req.callback : NULL;
            void *ud = g_req.user_data;
            Requester_Close();
            if (cb) cb(REQ_BTN_CANCEL, NULL, ud);
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
        if (gad_hit(&g_req.tf, mx, my)) {
            gad_event(&g_req.tf, GAD_DOWN, mx, my);   /* focus + cursor pos */
            WM_Redraw();
            return;
        }
        /* Click outside text field — defocus */
        g_req.tf.focused = 0;
    }

    /* Check button hits */
    for (int i = 0; i < g_req.n_buttons; i++) {
        if (gad_hit(&g_req.btns[i], mx, my)) {
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
        if (gad_hit(&g_req.btns[i], mx, my)) {
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
        if (gad_hit(&g_req.btns[btn], mx, my)) {
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

/* Event handler — catches close-gadget clicks (WM_EVT_CLOSE_REQUEST) which
 * would otherwise bypass Requester_Close and leave g_req.wm_handle stale,
 * blocking all future requesters.  Treated as Cancel (same as Escape). */
static int req_event(int handle, int event, int a, int b, int c)
{
    (void)handle; (void)a; (void)b; (void)c;
    if (event == WM_EVT_CLOSE_REQUEST) {
        ReqCallback cb = g_req.callback;
        void *ud = g_req.user_data;
        int fire_cancel = (g_req.type == REQ_TYPE_STRING || g_req.n_buttons > 1);
        Requester_Close();
        if (cb && fire_cancel) cb(REQ_BTN_CANCEL, NULL, ud);
        return 0;   /* veto the WM's own close — already closed */
    }
    return 1;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void Requester_Confirm(const char *title, const char *body,
                       const char *btn1, const char *btn2,
                       ReqCallback cb, void *user_data)
{
    /* Already active?  A stale handle (window closed via the close gadget
     * or reused by another window) must not block a new requester. */
    if (g_req.wm_handle >= 0 &&
        WM_GetDrawFn(g_req.wm_handle) == req_draw) return;

    memset(&g_req, 0, sizeof(g_req));
    g_req.type = REQ_TYPE_CONFIRM;
    g_req.callback = cb;
    g_req.user_data = user_data;
    g_req.tf.focused = 0;
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
    WM_SetEventHandler(g_req.wm_handle, req_event);
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
    if (g_req.wm_handle >= 0 &&
        WM_GetDrawFn(g_req.wm_handle) == req_draw) return;

    memset(&g_req, 0, sizeof(g_req));
    g_req.type = REQ_TYPE_STRING;
    g_req.callback = cb;
    g_req.user_data = user_data;
    g_req.max_chars = max_chars;
    if (g_req.max_chars > REQ_MAX_TEXT) g_req.max_chars = REQ_MAX_TEXT;
    str_cp(g_req.title, title, sizeof(g_req.title));
    split_lines(prompt);

    /* Wire the shared string gadget onto the text buffer */
    g_req.tf.buf       = g_req.text;
    g_req.tf.buf_max   = g_req.max_chars;
    g_req.tf.buf_len   = 0;
    g_req.tf.cursor    = 0;
    g_req.tf.focused   = 1;
    g_req.text[0]      = '\0';
    if (initial) {
        str_cp(g_req.text, initial, g_req.max_chars);
        g_req.tf.buf_len = str_len(g_req.text);
        g_req.tf.cursor  = g_req.tf.buf_len;
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
    WM_SetEventHandler(g_req.wm_handle, req_event);
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
    if (g_req.wm_handle >= 0 &&
        WM_GetDrawFn(g_req.wm_handle) == req_draw) return;

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
    WM_SetEventHandler(g_req.wm_handle, req_event);
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
    g_req.tf.focused = 0;
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
