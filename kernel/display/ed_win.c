/* ed_win.c — UAOS ED Editor
 *
 * A simple AmigaED-style editor with:
 * - Edit mode: type text, cursor movement, basic editing
 * - Command mode (ESC): save, quit, search, goto line
 * - Simpler than vim: no modal confusion, just ESC for commands
 *
 * Based on the classic AmigaED editor concept.
 */

#include "ed_win.h"
#include "wm.h"
#include "framebuffer.h"
#include "../dos/vfs.h"
#include "../shell/cmd_internal.h"
#include <stdint.h>
#include <stddef.h>

/* =========================================================================
 * Constants
 * ========================================================================= */

#define ED_MAX_WINDOWS   2
#define ED_MAX_LINES     2048
#define ED_MAX_LINE_LEN  256
#define ED_MAX_FNAME     64
#define ED_MAX_CMD_LEN   80
#define ED_STATUS_H      20

/* Virtual key codes */
#define VKEY_PGUP   0x01
#define VKEY_PGDN   0x02
#define VKEY_UP     0x03
#define VKEY_DOWN   0x04
#define VKEY_LEFT   0x05
#define VKEY_RIGHT  0x06

typedef enum {
    ED_MODE_EDIT = 0,
    ED_MODE_COMMAND
} EdMode;

typedef struct {
    char text[ED_MAX_LINE_LEN];
    int  len;
} EdLine;

typedef struct {
    EdLine lines[ED_MAX_LINES];
    int    n_lines;
    int    modified;
    char   filename[ED_MAX_FNAME];
} EdBuffer;

typedef struct {
    int row, col;
    int scroll_y;
    int want_col;
} EdCursor;

typedef struct {
    EdBuffer   buf;
    EdCursor   cur;
    EdMode     mode;

    char  cmd_buf[ED_MAX_CMD_LEN];
    int   cmd_len;

    char  search_pat[64];
    int   search_dir;

    int   wm_handle;
    int   win_x, win_y, win_w, win_h;
    int   active;
    int   dirty;

    /* inline mode */
    int       inline_mode;
    EdQuitFn  on_quit;
    void     *shell_extra;

    /* message line (for status messages) */
    char  msg[80];
    int   msg_timer;
} EdInstance;

/* =========================================================================
 * Globals
 * ========================================================================= */

static EdInstance g_eds[ED_MAX_WINDOWS];

/* =========================================================================
 * Helpers
 * ========================================================================= */

static int e_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void e_strcpy(char *d, const char *s, int max)
{
    int i = 0;
    while (i < max - 1 && s[i]) { d[i] = s[i]; i++; }
    d[i] = '\0';
}

static void e_strcat(char *d, const char *s, int max)
{
    int dl = e_strlen(d);
    int i = 0;
    while (dl + i < max - 1 && s[i]) { d[dl + i] = s[i]; i++; }
    d[dl + i] = '\0';
}

static int e_strcmp(const char *a, const char *b)
{
    int i = 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; }
    return a[i] == b[i];
}

static int e_min(int a, int b) { return a < b ? a : b; }
static int e_max(int a, int b) { return a > b ? a : b; }

static int e_atoi(const char *s)
{
    int n = 0;
    while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); s++; }
    return n;
}

static void e_itoa(int n, char *buf, int max)
{
    if (n == 0) { if (max > 1) { buf[0] = '0'; buf[1] = '\0'; } return; }
    char tmp[12]; int i = 0;
    int neg = n < 0; if (neg) n = -n;
    while (n && i < 11) { tmp[i++] = (char)('0' + (n % 10)); n /= 10; }
    int j = 0;
    if (neg && j < max - 1) buf[j++] = '-';
    while (i-- && j < max - 1) buf[j++] = tmp[i];
    buf[j] = '\0';
}

static void e_set_msg(EdInstance *e, const char *msg)
{
    e_strcpy(e->msg, msg, sizeof(e->msg));
    e->msg_timer = 200;  /* display for ~2 seconds */
}

/* =========================================================================
 * Buffer operations
 * ========================================================================= */

static void buf_clear(EdBuffer *b)
{
    b->n_lines = 1;
    b->modified = 0;
    b->lines[0].len = 0;
    b->lines[0].text[0] = '\0';
}

static int buf_load(EdBuffer *b, const char *path)
{
    VfsFile fh;
    if (!VFS_Open(&fh, path, VFS_READ)) return -1;

    buf_clear(b);
    e_strcpy(b->filename, path, ED_MAX_FNAME);

    uint32_t sz = VFS_Size(&fh);
    uint32_t pos = 0;
    int line_idx = 0;
    int col = 0;

    while (pos < sz && line_idx < ED_MAX_LINES) {
        uint8_t ch;
        uint32_t rd = VFS_Read(&fh, &ch, 1);
        if (rd == 0) break;
        pos++;
        if (ch == '\n') {
            b->lines[line_idx].len = col;
            b->lines[line_idx].text[col] = '\0';
            line_idx++;
            col = 0;
            if (line_idx < ED_MAX_LINES) {
                b->lines[line_idx].len = 0;
                b->lines[line_idx].text[0] = '\0';
            }
        } else if (ch != '\r' && col < ED_MAX_LINE_LEN - 1) {
            b->lines[line_idx].text[col++] = (char)ch;
        }
    }
    b->lines[line_idx].len = col;
    b->lines[line_idx].text[col] = '\0';
    b->n_lines = line_idx + 1;
    VFS_Close(&fh);
    return 0;
}

static int buf_save(EdBuffer *b)
{
    if (!b->filename[0]) return -1;
    VfsFile fh;
    if (!VFS_Open(&fh, b->filename, VFS_WRITE | VFS_CREATE | VFS_TRUNC)) return -1;

    for (int i = 0; i < b->n_lines; i++) {
        if (b->lines[i].len > 0)
            VFS_Write(&fh, (uint8_t *)b->lines[i].text, (uint32_t)b->lines[i].len);
        uint8_t nl = '\n';
        VFS_Write(&fh, &nl, 1);
    }
    VFS_Close(&fh);
    b->modified = 0;
    return 0;
}

/* =========================================================================
 * Cursor helpers
 * ========================================================================= */

static void cur_clamp(EdInstance *e)
{
    if (e->cur.row < 0) e->cur.row = 0;
    if (e->cur.row >= e->buf.n_lines) e->cur.row = e->buf.n_lines - 1;
    if (e->cur.row < 0) e->cur.row = 0;
    int linelen = e->buf.lines[e->cur.row].len;
    if (e->cur.col > linelen) e->cur.col = linelen;
    if (e->cur.col < 0) e->cur.col = 0;
}

static void cur_set_want(EdInstance *e) { e->cur.want_col = e->cur.col; }

static void cur_recall_want(EdInstance *e)
{
    int linelen = e->buf.lines[e->cur.row].len;
    e->cur.col = e->cur.want_col;
    if (e->cur.col > linelen) e->cur.col = linelen;
}

/* =========================================================================
 * Scroll helpers
 * ========================================================================= */

static int ed_lines_visible(const EdInstance *e)
{
    int client_h = e->win_h - WM_TITLEBAR_H - WM_SCROLLBAR_W;
    int text_h = client_h - ED_STATUS_H;
    return e_max(text_h / 16, 1);
}

static void ed_ensure_cursor_visible(EdInstance *e)
{
    int vis = ed_lines_visible(e);
    if (e->cur.row < e->cur.scroll_y) e->cur.scroll_y = e->cur.row;
    if (e->cur.row >= e->cur.scroll_y + vis) e->cur.scroll_y = e->cur.row - vis + 1;
    if (e->cur.scroll_y < 0) e->cur.scroll_y = 0;
    if (e->cur.scroll_y >= e->buf.n_lines) e->cur.scroll_y = e->buf.n_lines - 1;
}

static void ed_sync_scroll_wm(EdInstance *e)
{
    if (e->wm_handle < 0) return;
    int total_h = e->buf.n_lines * 16;
    int client_h = e->win_h - WM_TITLEBAR_H - WM_SCROLLBAR_W;
    int view_h = client_h - ED_STATUS_H;
    if (view_h < 1) view_h = 1;
    WM_SetScrollInfoEx(e->wm_handle, e->win_w, total_h, view_h);
    int sy = e->cur.scroll_y * 16;
    WM_SetScrollY(e->wm_handle, sy);
}

/* =========================================================================
 * Text mutations
 * ========================================================================= */

static void ed_insert_char(EdInstance *e, char c)
{
    EdLine *ln = &e->buf.lines[e->cur.row];
    if (ln->len >= ED_MAX_LINE_LEN - 1) return;
    for (int i = ln->len; i > e->cur.col; i--) ln->text[i] = ln->text[i - 1];
    ln->text[e->cur.col] = c;
    ln->len++;
    ln->text[ln->len] = '\0';
    e->cur.col++;
    e->buf.modified = 1;
    e->dirty = 1;
}

static void ed_backspace(EdInstance *e)
{
    if (e->cur.col > 0) {
        EdLine *ln = &e->buf.lines[e->cur.row];
        for (int i = e->cur.col - 1; i < ln->len - 1; i++)
            ln->text[i] = ln->text[i + 1];
        ln->len--;
        ln->text[ln->len] = '\0';
        e->cur.col--;
        e->buf.modified = 1;
        e->dirty = 1;
    } else if (e->cur.row > 0) {
        int prev_row = e->cur.row - 1;
        EdLine *prev = &e->buf.lines[prev_row];
        EdLine *cur = &e->buf.lines[e->cur.row];
        int avail = ED_MAX_LINE_LEN - 1 - prev->len;
        int take = (cur->len < avail) ? cur->len : avail;
        for (int i = 0; i < take; i++) prev->text[prev->len + i] = cur->text[i];
        prev->len += take;
        prev->text[prev->len] = '\0';
        for (int i = e->cur.row; i < e->buf.n_lines - 1; i++)
            e->buf.lines[i] = e->buf.lines[i + 1];
        e->buf.n_lines--;
        e->cur.row = prev_row;
        e->cur.col = prev->len - take;
        e->buf.modified = 1;
        e->dirty = 1;
    }
}

static void ed_delete_char(EdInstance *e)
{
    EdLine *ln = &e->buf.lines[e->cur.row];
    if (e->cur.col < ln->len) {
        for (int i = e->cur.col; i < ln->len - 1; i++)
            ln->text[i] = ln->text[i + 1];
        ln->len--;
        ln->text[ln->len] = '\0';
        e->buf.modified = 1;
        e->dirty = 1;
    } else if (e->cur.row < e->buf.n_lines - 1) {
        EdLine *cur = &e->buf.lines[e->cur.row];
        EdLine *next = &e->buf.lines[e->cur.row + 1];
        int avail = ED_MAX_LINE_LEN - 1 - cur->len;
        int take = (next->len < avail) ? next->len : avail;
        for (int i = 0; i < take; i++) cur->text[cur->len + i] = next->text[i];
        cur->len += take;
        cur->text[cur->len] = '\0';
        for (int i = e->cur.row + 1; i < e->buf.n_lines - 1; i++)
            e->buf.lines[i] = e->buf.lines[i + 1];
        e->buf.n_lines--;
        e->buf.modified = 1;
        e->dirty = 1;
    }
}

static void ed_break_line(EdInstance *e)
{
    if (e->buf.n_lines >= ED_MAX_LINES) return;
    EdLine *ln = &e->buf.lines[e->cur.row];
    int rest = ln->len - e->cur.col;
    if (rest < 0) rest = 0;
    for (int i = e->buf.n_lines; i > e->cur.row + 1; i--)
        e->buf.lines[i] = e->buf.lines[i - 1];
    e->buf.n_lines++;

    EdLine *newln = &e->buf.lines[e->cur.row + 1];
    newln->len = 0;
    /* Simple auto-indent: copy leading whitespace */
    int ai = 0;
    while (ai < ln->len && (ln->text[ai] == ' ' || ln->text[ai] == '\t')) ai++;
    int j;
    for (j = 0; j < ai && j < ED_MAX_LINE_LEN - 1; j++)
        newln->text[j] = ln->text[j];
    newln->len = j;

    int take = (rest < ED_MAX_LINE_LEN - 1 - newln->len) ? rest : ED_MAX_LINE_LEN - 1 - newln->len;
    for (int i = 0; i < take; i++)
        newln->text[newln->len + i] = ln->text[e->cur.col + i];
    newln->len += take;
    newln->text[newln->len] = '\0';

    ln->len = e->cur.col;
    ln->text[ln->len] = '\0';

    e->cur.row++;
    e->cur.col = ai;
    e->buf.modified = 1;
    e->dirty = 1;
}

static void ed_delete_line(EdInstance *e, int row)
{
    if (e->buf.n_lines <= 1) {
        e->buf.lines[0].len = 0;
        e->buf.lines[0].text[0] = '\0';
        e->buf.modified = 1;
        e->dirty = 1;
        return;
    }
    for (int i = row; i < e->buf.n_lines - 1; i++)
        e->buf.lines[i] = e->buf.lines[i + 1];
    e->buf.n_lines--;
    e->buf.modified = 1;
    e->dirty = 1;
}

static void ed_goto_line(EdInstance *e, int line1)
{
    line1--;
    if (line1 < 0) line1 = 0;
    if (line1 >= e->buf.n_lines) line1 = e->buf.n_lines - 1;
    e->cur.row = line1;
    cur_clamp(e);
    ed_ensure_cursor_visible(e);
    e->dirty = 1;
}

/* =========================================================================
 * Search
 * ========================================================================= */

static int ed_search(EdInstance *e, int forward)
{
    if (!e->search_pat[0]) return 0;
    int patlen = e_strlen(e->search_pat);
    if (forward) {
        int start_row = e->cur.row;
        int start_col = e->cur.col + 1;
        for (int r = start_row; r < e->buf.n_lines; r++) {
            EdLine *ln = &e->buf.lines[r];
            int c0 = (r == start_row) ? start_col : 0;
            for (int c = c0; c <= ln->len - patlen; c++) {
                int match = 1;
                for (int i = 0; i < patlen; i++) {
                    char a = ln->text[c + i];
                    char b = e->search_pat[i];
                    if (a >= 'A' && a <= 'Z') a += 32;
                    if (b >= 'A' && b <= 'Z') b += 32;
                    if (a != b) { match = 0; break; }
                }
                if (match) {
                    e->cur.row = r; e->cur.col = c;
                    ed_ensure_cursor_visible(e);
                    e->dirty = 1;
                    return 1;
                }
            }
        }
    } else {
        int start_row = e->cur.row;
        int start_col = e->cur.col - 1;
        for (int r = start_row; r >= 0; r--) {
            EdLine *ln = &e->buf.lines[r];
            int c0 = (r == start_row) ? start_col : ln->len - patlen;
            if (c0 > ln->len - patlen) c0 = ln->len - patlen;
            for (int c = c0; c >= 0; c--) {
                int match = 1;
                for (int i = 0; i < patlen; i++) {
                    char a = ln->text[c + i];
                    char b = e->search_pat[i];
                    if (a >= 'A' && a <= 'Z') a += 32;
                    if (b >= 'A' && b <= 'Z') b += 32;
                    if (a != b) { match = 0; break; }
                }
                if (match) {
                    e->cur.row = r; e->cur.col = c;
                    ed_ensure_cursor_visible(e);
                    e->dirty = 1;
                    return 1;
                }
            }
        }
    }
    return 0;
}

/* =========================================================================
 * Motions
 * ========================================================================= */

static void motion_left(EdInstance *e)  { e->cur.col--; if (e->cur.col < 0) e->cur.col = 0; cur_set_want(e); e->dirty = 1; }
static void motion_right(EdInstance *e) { int ll = e->buf.lines[e->cur.row].len; e->cur.col++; if (e->cur.col > ll) e->cur.col = ll; cur_set_want(e); e->dirty = 1; }
static void motion_up(EdInstance *e)    { e->cur.row--; if (e->cur.row < 0) e->cur.row = 0; cur_recall_want(e); ed_ensure_cursor_visible(e); e->dirty = 1; }
static void motion_down(EdInstance *e)  { e->cur.row++; if (e->cur.row >= e->buf.n_lines) e->cur.row = e->buf.n_lines - 1; cur_recall_want(e); ed_ensure_cursor_visible(e); e->dirty = 1; }
static void motion_home(EdInstance *e)  { e->cur.col = 0; cur_set_want(e); e->dirty = 1; }
static void motion_end(EdInstance *e)   { e->cur.col = e->buf.lines[e->cur.row].len; cur_set_want(e); e->dirty = 1; }
static void motion_top(EdInstance *e)   { e->cur.row = 0; cur_clamp(e); ed_ensure_cursor_visible(e); e->dirty = 1; }
static void motion_bottom(EdInstance *e){ e->cur.row = e->buf.n_lines - 1; cur_clamp(e); ed_ensure_cursor_visible(e); e->dirty = 1; }

static void motion_page_down(EdInstance *e)
{
    int vis = ed_lines_visible(e);
    e->cur.row += vis;
    if (e->cur.row >= e->buf.n_lines) e->cur.row = e->buf.n_lines - 1;
    cur_recall_want(e);
    ed_ensure_cursor_visible(e);
    e->dirty = 1;
}

static void motion_page_up(EdInstance *e)
{
    int vis = ed_lines_visible(e);
    e->cur.row -= vis;
    if (e->cur.row < 0) e->cur.row = 0;
    cur_recall_want(e);
    ed_ensure_cursor_visible(e);
    e->dirty = 1;
}

/* =========================================================================
 * Edit mode key handler
 * ========================================================================= */

static void ed_edit_key(EdInstance *e, char c)
{
    if (c == 27) { /* ESC — enter command mode */
        e->mode = ED_MODE_COMMAND;
        e->cmd_len = 0;
        e->cmd_buf[0] = '\0';
        e->dirty = 1;
        return;
    }
    switch (c) {
        case VKEY_UP:    motion_up(e); break;
        case VKEY_DOWN:  motion_down(e); break;
        case VKEY_LEFT:  motion_left(e); break;
        case VKEY_RIGHT: motion_right(e); break;
        case VKEY_PGUP:  motion_page_up(e); break;
        case VKEY_PGDN:  motion_page_down(e); break;
        case '\b':
        case 127:
            ed_backspace(e);
            break;
        case '\n':
        case '\r':
            ed_break_line(e);
            break;
        case '\t': {
            int col = e->cur.col;
            int spaces = 4 - (col % 4);
            for (int i = 0; i < spaces; i++) ed_insert_char(e, ' ');
            break;
        }
        default:
            if ((unsigned char)c >= 32 && (unsigned char)c < 127)
                ed_insert_char(e, c);
            break;
    }
    ed_ensure_cursor_visible(e);
    ed_sync_scroll_wm(e);
}

/* =========================================================================
 * Command mode key handler
 * ========================================================================= */

static void ed_exec_command(EdInstance *e, const char *cmd)
{
    /* q / quit — exit */
    if (e_strcmp(cmd, "q") || e_strcmp(cmd, "quit")) {
        if (e->buf.modified) {
            e_set_msg(e, "Modified: use 'q!' to force quit");
            return;
        }
        if (e->inline_mode) {
            e->active = 0;
            if (e->on_quit) e->on_quit(e->shell_extra);
        } else if (e->wm_handle >= 0) {
            WM_CloseWindow(e->wm_handle);
            e->wm_handle = -1;
            e->active = 0;
        }
        return;
    }
    /* q! — force quit */
    if (e_strcmp(cmd, "q!")) {
        if (e->inline_mode) {
            e->active = 0;
            if (e->on_quit) e->on_quit(e->shell_extra);
        } else if (e->wm_handle >= 0) {
            WM_CloseWindow(e->wm_handle);
            e->wm_handle = -1;
            e->active = 0;
        }
        return;
    }
    /* w / write — save */
    if (e_strcmp(cmd, "w") || e_strcmp(cmd, "write")) {
        if (buf_save(&e->buf) == 0)
            e_set_msg(e, "Saved.");
        else
            e_set_msg(e, "Save failed.");
        return;
    }
    /* wq — save and quit */
    if (e_strcmp(cmd, "wq")) {
        if (buf_save(&e->buf) == 0) {
            if (e->inline_mode) {
                e->active = 0;
                if (e->on_quit) e->on_quit(e->shell_extra);
            } else if (e->wm_handle >= 0) {
                WM_CloseWindow(e->wm_handle);
                e->wm_handle = -1;
                e->active = 0;
            }
        } else {
            e_set_msg(e, "Save failed.");
        }
        return;
    }
    /* /pattern — search forward */
    if (cmd[0] == '/') {
        e_strcpy(e->search_pat, cmd + 1, sizeof(e->search_pat));
        e->search_dir = 1;
        if (ed_search(e, 1))
            e_set_msg(e, "Found.");
        else
            e_set_msg(e, "Not found.");
        return;
    }
    /* ?pattern — search backward */
    if (cmd[0] == '?') {
        e_strcpy(e->search_pat, cmd + 1, sizeof(e->search_pat));
        e->search_dir = 0;
        if (ed_search(e, 0))
            e_set_msg(e, "Found.");
        else
            e_set_msg(e, "Not found.");
        return;
    }
    /* n — repeat search */
    if (e_strcmp(cmd, "n")) {
        if (ed_search(e, e->search_dir))
            e_set_msg(e, "Found.");
        else
            e_set_msg(e, "Not found.");
        return;
    }
    /* N — reverse search */
    if (e_strcmp(cmd, "N")) {
        if (ed_search(e, !e->search_dir))
            e_set_msg(e, "Found.");
        else
            e_set_msg(e, "Not found.");
        return;
    }
    /* number — goto line */
    if (cmd[0] >= '0' && cmd[0] <= '9') {
        ed_goto_line(e, e_atoi(cmd));
        return;
    }
    /* d — delete current line */
    if (e_strcmp(cmd, "d")) {
        ed_delete_line(e, e->cur.row);
        if (e->cur.row >= e->buf.n_lines) e->cur.row = e->buf.n_lines - 1;
        cur_clamp(e);
        e_set_msg(e, "Line deleted.");
        return;
    }
    /* h — help */
    if (e_strcmp(cmd, "h") || e_strcmp(cmd, "help")) {
        e_set_msg(e, "ESC:cmd  w:save  q:quit  wq:save+quit  /pat:search  N:line");
        return;
    }
    e_set_msg(e, "Unknown command. 'h' for help.");
}

static void ed_command_key(EdInstance *e, char c)
{
    if (c == 27) { /* ESC again — back to edit mode */
        e->mode = ED_MODE_EDIT;
        e->cmd_len = 0;
        e->cmd_buf[0] = '\0';
        e->dirty = 1;
        return;
    }
    if (c == '\n' || c == '\r') {
        e->cmd_buf[e->cmd_len] = '\0';
        e->mode = ED_MODE_EDIT;
        e->dirty = 1;
        ed_exec_command(e, e->cmd_buf);
        e->cmd_len = 0;
        e->cmd_buf[0] = '\0';
        return;
    }
    if (c == '\b' || c == 127) {
        if (e->cmd_len > 0) e->cmd_len--;
        e->cmd_buf[e->cmd_len] = '\0';
        e->dirty = 1;
        return;
    }
    if ((unsigned char)c >= 32 && (unsigned char)c < 127 && e->cmd_len < ED_MAX_CMD_LEN - 1) {
        e->cmd_buf[e->cmd_len++] = c;
        e->cmd_buf[e->cmd_len] = '\0';
        e->dirty = 1;
    }
}

/* =========================================================================
 * Drawing
 * ========================================================================= */

static void ed_draw(EdInstance *e, int wx, int wy, int ww, int wh)
{
    e->win_x = wx; e->win_y = wy; e->win_w = ww; e->win_h = wh;

    int cx = wx + 1;
    int cy = wy + WM_TITLEBAR_H;
    int cw = ww - 1 - WM_SCROLLBAR_W;
    int ch = wh - WM_TITLEBAR_H - WM_SCROLLBAR_W;

    FB_FillRect(cx, cy, cw, ch, WB_WHITE);

    int text_x = cx + 4;
    int text_w = cw - 8;
    int text_h = ch - ED_STATUS_H;
    int max_vis = text_h / 16;
    if (max_vis < 1) max_vis = 1;

    int scroll_y = e->inline_mode ? (e->cur.scroll_y * 16) : WM_GetScrollY(e->wm_handle);
    int first_line = scroll_y / 16;
    if (first_line < 0) first_line = 0;

    for (int i = 0; i < max_vis; i++) {
        int row = first_line + i;
        if (row >= e->buf.n_lines) break;
        int ly = cy + i * 16;

        EdLine *ln = &e->buf.lines[row];
        int draw_len = ln->len;
        if (draw_len > text_w / 8) draw_len = text_w / 8;

        /* Cursor line highlight */
        if (row == e->cur.row && e->mode == ED_MODE_EDIT) {
            FB_FillRect(text_x, ly, text_w, 16, FB_RGB(0xEE, 0xEE, 0xFF));
        }

        if (draw_len > 0) {
            char seg[64];
            int sl = draw_len; if (sl > 63) sl = 63;
            for (int j = 0; j < sl; j++) seg[j] = ln->text[j];
            seg[sl] = '\0';
            FB_PutStr(text_x, ly, seg, WB_BLACK, WB_WHITE);
        }
    }

    /* Cursor */
    {
        int cr = e->cur.row - first_line;
        if (cr >= 0 && cr < max_vis) {
            int cc = e->cur.col;
            if (e->mode == ED_MODE_EDIT) {
                FB_DrawRect(text_x + cc * 8, cy + cr * 16, 8, 16, WB_BLACK);
            } else {
                /* Block cursor in command mode */
                FB_FillRect(text_x + cc * 8, cy + cr * 16, 8, 16, WB_BLUE);
                EdLine *ln = &e->buf.lines[e->cur.row];
                if (cc < ln->len) {
                    char ch2[2] = { ln->text[cc], '\0' };
                    FB_PutStr(text_x + cc * 8, cy + cr * 16, ch2, WB_WHITE, WB_BLUE);
                }
            }
        }
    }

    /* Status bar */
    int sb_y = cy + ch - ED_STATUS_H;
    FB_FillRect(cx + 1, sb_y, cw - 2, ED_STATUS_H, WB_BLUE);

    char status[128];
    if (e->mode == ED_MODE_COMMAND) {
        FB_FillRect(cx + 1, sb_y, cw - 2, ED_STATUS_H, WB_DARK_GREY);
        char cmdl[90];
        e_strcpy(cmdl, "ESC ", 90);
        e_strcat(cmdl, e->cmd_buf, 90);
        FB_PutStr(cx + 4, sb_y + 2, cmdl, WB_WHITE, WB_DARK_GREY);
    } else {
        const char *modestr = "EDIT";
        e_strcpy(status, modestr, 128);
        if (e->buf.modified) e_strcat(status, " [+]", 128);
        e_strcat(status, " ", 128);
        e_strcat(status, e->buf.filename[0] ? e->buf.filename : "[No Name]", 128);

        /* Line/col info */
        e_strcat(status, "  L", 128);
        char num[12];
        e_itoa(e->cur.row + 1, num, 12);
        e_strcat(status, num, 128);
        e_strcat(status, " C", 128);
        e_itoa(e->cur.col + 1, num, 12);
        e_strcat(status, num, 128);

        FB_PutStr(cx + 4, sb_y + 2, status, WB_WHITE, WB_BLUE);

        /* Message line (if timer active) */
        if (e->msg_timer > 0 && e->msg[0]) {
            FB_PutStr(cx + 4, sb_y + 12, e->msg, WB_CREAM, WB_BLUE);
            e->msg_timer--;
        }
    }

    e->dirty = 0;
}

/* =========================================================================
 * Draw / key shims
 * ========================================================================= */

static void draw_shim_0(int wx, int wy, int ww, int wh) { ed_draw(&g_eds[0], wx, wy, ww, wh); }
static void draw_shim_1(int wx, int wy, int ww, int wh) { ed_draw(&g_eds[1], wx, wy, ww, wh); }

typedef void (*EdDrawShim)(int,int,int,int);
static const EdDrawShim k_ed_draw_shims[ED_MAX_WINDOWS] = {
    draw_shim_0, draw_shim_1
};

static void ed_key_cb(char c)
{
    int focus = WM_GetFocus();
    for (int i = 0; i < ED_MAX_WINDOWS; i++) {
        EdInstance *e = &g_eds[i];
        if (e->active && e->wm_handle == focus) {
            if (!WM_IsWindowActive(e->wm_handle)) {
                e->active = 0;
                e->wm_handle = -1;
                return;
            }
            if (e->mode == ED_MODE_COMMAND)
                ed_command_key(e, c);
            else
                ed_edit_key(e, c);
            return;
        }
    }
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void EdWin_Init(void)
{
    /* Nothing to init yet */
}

static int ed_find_free_slot(void)
{
    for (int i = 0; i < ED_MAX_WINDOWS; i++) {
        if (!g_eds[i].active) return i;
        if (!g_eds[i].inline_mode && !WM_IsWindowActive(g_eds[i].wm_handle)) return i;
    }
    return -1;
}

static void ed_init_slot(EdInstance *e, const char *filename)
{
    for (int i = 0; i < (int)sizeof(EdInstance); i++) ((uint8_t *)e)[i] = 0;
    e->wm_handle = -1;
    e->active = 1;
    e->dirty = 1;
    e->mode = ED_MODE_EDIT;
    e->search_dir = 1;
    buf_clear(&e->buf);
    if (filename && filename[0]) {
        if (buf_load(&e->buf, filename) != 0)
            e_strcpy(e->buf.filename, filename, ED_MAX_FNAME);
    }
    e->cur.row = 0;
    e->cur.col = 0;
    e->cur.scroll_y = 0;
    e->cur.want_col = 0;
}

int EdWin_Open(const char *filename)
{
    EdWin_Init();
    int idx = ed_find_free_slot();
    if (idx < 0) return -1;

    EdInstance *e = &g_eds[idx];
    ed_init_slot(e, filename);

    int wx = 80 + idx * 24;
    int wy = 80 + idx * 24;
    int ww = 600;
    int wh = 400;

    e->wm_handle = WM_AddWindow(wx, wy, ww, wh, "ED", k_ed_draw_shims[idx], ed_key_cb);
    if (e->wm_handle < 0) {
        e->active = 0;
        return -1;
    }

    e->win_x = wx; e->win_y = wy; e->win_w = ww; e->win_h = wh;
    ed_sync_scroll_wm(e);
    WM_RaiseWindow(e->wm_handle);
    WM_Redraw();
    return 0;
}

void EdWin_HandleKey(char c) { ed_key_cb(c); }

void EdWin_Redraw(void)
{
    for (int i = 0; i < ED_MAX_WINDOWS; i++) {
        EdInstance *e = &g_eds[i];
        if (e->active && !e->inline_mode && WM_IsWindowActive(e->wm_handle))
            e->dirty = 1;
    }
}

/* ----- Inline mode ----- */

int EdWin_OpenInline(const char *filename, void *shell_extra, EdQuitFn on_quit)
{
    EdWin_Init();
    int idx = ed_find_free_slot();
    if (idx < 0) return -1;

    EdInstance *e = &g_eds[idx];
    ed_init_slot(e, filename);
    e->inline_mode = 1;
    e->shell_extra = shell_extra;
    e->on_quit = on_quit;
    return idx;
}

void EdWin_DrawInline(int slot, int wx, int wy, int ww, int wh)
{
    if (slot < 0 || slot >= ED_MAX_WINDOWS) return;
    EdInstance *e = &g_eds[slot];
    if (!e->active) return;
    ed_draw(e, wx, wy, ww, wh);
}

void EdWin_KeyInline(int slot, char c)
{
    if (slot < 0 || slot >= ED_MAX_WINDOWS) return;
    EdInstance *e = &g_eds[slot];
    if (!e->active) return;
    if (e->mode == ED_MODE_COMMAND)
        ed_command_key(e, c);
    else
        ed_edit_key(e, c);
}

int EdWin_IsActive(int slot)
{
    if (slot < 0 || slot >= ED_MAX_WINDOWS) return 0;
    return g_eds[slot].active;
}

int EdWin_GetFilename(int slot, char *out, int max)
{
    if (slot < 0 || slot >= ED_MAX_WINDOWS || !g_eds[slot].active) return 0;
    int i = 0;
    while (i < max - 1 && g_eds[slot].buf.filename[i]) {
        out[i] = g_eds[slot].buf.filename[i];
        i++;
    }
    out[i] = '\0';
    return 1;
}

int EdWin_IsInline(int slot)
{
    if (slot < 0 || slot >= ED_MAX_WINDOWS || !g_eds[slot].active) return 0;
    return g_eds[slot].inline_mode;
}
