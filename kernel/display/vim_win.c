/* vim_win.c — UAOS Vim Editor
 *
 * A modal text editor with Normal, Insert, Visual, and Command-line modes.
 * Supports multiple editor windows (VIM_MAX_WINDOWS).
 */

#include "vim_win.h"
#include "wm.h"
#include "framebuffer.h"
#include "../dos/vfs.h"
#include "../shell/cmd_internal.h"
#include <stdint.h>
#include <stddef.h>

/* =========================================================================
 * Constants
 * ========================================================================= */

#define VIM_MAX_WINDOWS   4
#define VIM_MAX_LINES     4096
#define VIM_MAX_LINE_LEN  256
#define VIM_MAX_FNAME     64
#define VIM_MAX_CMD_LEN   64
#define VIM_MAX_SEARCH    64
#define VIM_CLIP_MAX_LEN  (VIM_MAX_LINE_LEN * 2)
#define VIM_STATUS_H      20
#define VIM_LNUM_W        (5 * 8)

/* Virtual key codes from ps2kbd.c */
#define VKEY_PGUP   0x01
#define VKEY_PGDN   0x02
#define VKEY_UP     0x03
#define VKEY_DOWN   0x04
#define VKEY_LEFT   0x05
#define VKEY_RIGHT  0x06

typedef enum {
    VIM_MODE_NORMAL = 0,
    VIM_MODE_INSERT,
    VIM_MODE_VISUAL,
    VIM_MODE_CMDLINE
} VimMode;

typedef struct {
    char  text[VIM_MAX_LINE_LEN];
    int   len;
} VimLine;

typedef struct {
    VimLine lines[VIM_MAX_LINES];
    int     n_lines;
    int     modified;
    char    filename[VIM_MAX_FNAME];
} VimBuffer;

typedef struct {
    int row, col;
    int scroll_y;
    int want_col;
    int sel_row, sel_col;
} VimCursor;

typedef struct {
    VimBuffer   buf;
    VimCursor   cur;
    VimMode     mode;

    char  cmd_buf[VIM_MAX_CMD_LEN];
    int   cmd_len;

    int   pending_count;
    char  pending_op;
    char  replace_char;

    char  search_pat[VIM_MAX_SEARCH];
    int   search_dir;
    int   search_active;

    /* config copies */
    int   showmode;
    int   number;
    int   hlsearch;
    int   tabstop;
    int   autoindent;
    int   smartcase;
    int   wrap;

    int   wm_handle;
    int   win_x, win_y, win_w, win_h;
    int   active;
    int   dirty;

    /* inline mode */
    int       inline_mode;
    VimQuitFn on_quit;
    void     *shell_extra;

    /* single-level undo */
    VimBuffer undo_buf;
    int       has_undo;

    /* clipboard */
    char  clip[VIM_CLIP_MAX_LEN];
    int   clip_len;
    int   clip_is_line;
} VimInstance;

/* =========================================================================
 * Globals
 * ========================================================================= */

static VimInstance g_vims[VIM_MAX_WINDOWS];
static int         g_vim_config_loaded = 0;

static int g_cfg_showmode = 1;
static int g_cfg_number   = 0;
static int g_cfg_hlsearch = 1;
static int g_cfg_tabstop  = 4;
static int g_cfg_autoindent = 1;
static int g_cfg_smartcase  = 0;
static int g_cfg_wrap       = 1;

/* =========================================================================
 * Tiny helpers
 * ========================================================================= */

static int v_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void v_strcpy(char *d, const char *s, int max)
{
    int i = 0;
    while (i < max - 1 && s[i]) { d[i] = s[i]; i++; }
    d[i] = '\0';
}

static void v_strcat(char *d, const char *s, int max)
{
    int dl = v_strlen(d);
    int i = 0;
    while (dl + i < max - 1 && s[i]) { d[dl + i] = s[i]; i++; }
    d[dl + i] = '\0';
}

static int v_strcmp(const char *a, const char *b)
{
    int i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == b[i];
}

static int v_isspace(char c) { return c == ' ' || c == '\t'; }

static int v_isword(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static int v_min(int a, int b) { return a < b ? a : b; }
static int v_max(int a, int b) { return a > b ? a : b; }

static int v_atoi(const char *s)
{
    int n = 0;
    while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); s++; }
    return n;
}

static void v_itoa(int n, char *buf, int max)
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

/* =========================================================================
 * Config file loader
 * ========================================================================= */

void VimWin_Init(void)
{
    if (g_vim_config_loaded) return;
    g_vim_config_loaded = 1;

    VfsFile fh;
    if (!VFS_Open(&fh, "S:vim.conf", VFS_READ)) return;

    uint32_t sz = VFS_Size(&fh);
    if (sz == 0 || sz > 4096) { VFS_Close(&fh); return; }

    char line[128];
    uint32_t pos = 0;
    while (pos < sz) {
        uint8_t chbuf[1];
        int li = 0;
        while (pos < sz && li < 127) {
            VFS_Read(&fh, chbuf, 1);
            pos++;
            if (chbuf[0] == '\n') break;
            if (chbuf[0] != '\r') line[li++] = (char)chbuf[0];
        }
        line[li] = '\0';

        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (p[0] != 's' || p[1] != 'e' || p[2] != 't') continue;
        p += 3;
        while (*p == ' ' || *p == '\t') p++;

        char name[32]; int ni = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != '=' && ni < 31)
            name[ni++] = *p++;
        name[ni] = '\0';

        while (*p == ' ' || *p == '\t') p++;
        int val = 1;
        if (*p == '=') {
            p++;
            while (*p == ' ' || *p == '\t') p++;
            val = v_atoi(p);
        }

        if (v_strcmp(name, "tabstop"))      g_cfg_tabstop     = val;
        else if (v_strcmp(name, "number"))      g_cfg_number      = val;
        else if (v_strcmp(name, "showmode"))    g_cfg_showmode    = val;
        else if (v_strcmp(name, "hlsearch"))    g_cfg_hlsearch    = val;
        else if (v_strcmp(name, "autoindent"))  g_cfg_autoindent  = val;
        else if (v_strcmp(name, "smartcase"))   g_cfg_smartcase   = val;
        else if (v_strcmp(name, "wrap"))        g_cfg_wrap        = val;
    }
    VFS_Close(&fh);
}

/* =========================================================================
 * Buffer helpers
 * ========================================================================= */

static void buf_clear(VimBuffer *b)
{
    b->n_lines = 1;
    b->modified = 0;
    b->lines[0].len = 0;
    b->lines[0].text[0] = '\0';
}

static int buf_load(VimBuffer *b, const char *path)
{
    VfsFile fh;
    if (!VFS_Open(&fh, path, VFS_READ)) return -1;

    buf_clear(b);
    v_strcpy(b->filename, path, VIM_MAX_FNAME);

    uint32_t sz = VFS_Size(&fh);
    uint32_t pos = 0;
    int line_idx = 0;
    int col = 0;

    while (pos < sz && line_idx < VIM_MAX_LINES) {
        uint8_t ch;
        uint32_t rd = VFS_Read(&fh, &ch, 1);
        if (rd == 0) break;
        pos++;
        if (ch == '\n') {
            b->lines[line_idx].len = col;
            b->lines[line_idx].text[col] = '\0';
            line_idx++;
            col = 0;
            if (line_idx < VIM_MAX_LINES) {
                b->lines[line_idx].len = 0;
                b->lines[line_idx].text[0] = '\0';
            }
        } else if (ch != '\r' && col < VIM_MAX_LINE_LEN - 1) {
            b->lines[line_idx].text[col++] = (char)ch;
        }
    }
    b->lines[line_idx].len = col;
    b->lines[line_idx].text[col] = '\0';
    b->n_lines = line_idx + 1;
    VFS_Close(&fh);
    return 0;
}

static int buf_save(VimBuffer *b)
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

static void buf_copy(VimBuffer *dst, const VimBuffer *src)
{
    dst->n_lines = src->n_lines;
    dst->modified = src->modified;
    v_strcpy(dst->filename, src->filename, VIM_MAX_FNAME);
    for (int i = 0; i < src->n_lines && i < VIM_MAX_LINES; i++) {
        v_strcpy(dst->lines[i].text, src->lines[i].text, VIM_MAX_LINE_LEN);
        dst->lines[i].len = src->lines[i].len;
    }
}

/* =========================================================================
 * Cursor helpers
 * ========================================================================= */

static void cur_clamp(VimInstance *v)
{
    if (v->cur.row < 0) v->cur.row = 0;
    if (v->cur.row >= v->buf.n_lines) v->cur.row = v->buf.n_lines - 1;
    if (v->cur.row < 0) v->cur.row = 0;

    int linelen = v->buf.lines[v->cur.row].len;
    if (v->mode == VIM_MODE_NORMAL) {
        if (v->cur.col > linelen) v->cur.col = linelen;
        if (v->cur.col < 0) v->cur.col = 0;
    } else {
        if (v->cur.col < 0) v->cur.col = 0;
        if (v->cur.col > linelen) v->cur.col = linelen;
    }
}

static void cur_set_want(VimInstance *v) { v->cur.want_col = v->cur.col; }

static void cur_recall_want(VimInstance *v)
{
    int linelen = v->buf.lines[v->cur.row].len;
    v->cur.col = v->cur.want_col;
    if (v->cur.col > linelen) v->cur.col = linelen;
}

/* =========================================================================
 * Scroll helpers
 * ========================================================================= */

static int vim_lines_visible(const VimInstance *v)
{
    int client_h = v->win_h - WM_TITLEBAR_H - WM_SCROLLBAR_W;
    int text_h = client_h - VIM_STATUS_H;
    return v_max(text_h / 16, 1);
}

static void vim_ensure_cursor_visible(VimInstance *v)
{
    int vis = vim_lines_visible(v);
    if (v->cur.row < v->cur.scroll_y) v->cur.scroll_y = v->cur.row;
    if (v->cur.row >= v->cur.scroll_y + vis) v->cur.scroll_y = v->cur.row - vis + 1;
    if (v->cur.scroll_y < 0) v->cur.scroll_y = 0;
    if (v->cur.scroll_y >= v->buf.n_lines) v->cur.scroll_y = v->buf.n_lines - 1;
}

static void vim_sync_scroll_wm(VimInstance *v)
{
    if (v->wm_handle < 0) return;
    int total_h = v->buf.n_lines * 16;
    int client_h = v->win_h - WM_TITLEBAR_H - WM_SCROLLBAR_W;
    int view_h = client_h - VIM_STATUS_H;
    if (view_h < 1) view_h = 1;
    WM_SetScrollInfoEx(v->wm_handle, v->win_w, total_h, view_h);
    int sy = v->cur.scroll_y * 16;
    WM_SetScrollY(v->wm_handle, sy);
}

/* =========================================================================
 * Text mutations
 * ========================================================================= */

static void vim_save_undo(VimInstance *v)
{
    buf_copy(&v->undo_buf, &v->buf);
    v->has_undo = 1;
}

static void vim_undo(VimInstance *v)
{
    if (!v->has_undo) return;
    VimBuffer tmp;
    buf_copy(&tmp, &v->buf);
    buf_copy(&v->buf, &v->undo_buf);
    buf_copy(&v->undo_buf, &tmp);
    v->dirty = 1;
    v->buf.modified = v->has_undo ? 1 : 0;
    cur_clamp(v);
    vim_ensure_cursor_visible(v);
}

static void vim_insert_char(VimInstance *v, char c)
{
    VimLine *ln = &v->buf.lines[v->cur.row];
    if (ln->len >= VIM_MAX_LINE_LEN - 1) return;
    for (int i = ln->len; i > v->cur.col; i--) ln->text[i] = ln->text[i - 1];
    ln->text[v->cur.col] = c;
    ln->len++;
    ln->text[ln->len] = '\0';
    v->cur.col++;
    v->buf.modified = 1;
    v->dirty = 1;
}

static void vim_backspace(VimInstance *v)
{
    if (v->cur.col > 0) {
        VimLine *ln = &v->buf.lines[v->cur.row];
        for (int i = v->cur.col - 1; i < ln->len - 1; i++)
            ln->text[i] = ln->text[i + 1];
        ln->len--;
        ln->text[ln->len] = '\0';
        v->cur.col--;
        v->buf.modified = 1;
        v->dirty = 1;
    } else if (v->cur.row > 0) {
        int prev_row = v->cur.row - 1;
        VimLine *prev = &v->buf.lines[prev_row];
        VimLine *cur = &v->buf.lines[v->cur.row];
        int avail = VIM_MAX_LINE_LEN - 1 - prev->len;
        int take = (cur->len < avail) ? cur->len : avail;
        for (int i = 0; i < take; i++) prev->text[prev->len + i] = cur->text[i];
        prev->len += take;
        prev->text[prev->len] = '\0';
        for (int i = v->cur.row; i < v->buf.n_lines - 1; i++) {
            v->buf.lines[i] = v->buf.lines[i + 1];
        }
        v->buf.n_lines--;
        v->cur.row = prev_row;
        v->cur.col = prev->len - take;
        v->buf.modified = 1;
        v->dirty = 1;
    }
}

static void vim_delete_char(VimInstance *v)
{
    VimLine *ln = &v->buf.lines[v->cur.row];
    if (v->cur.col < ln->len) {
        for (int i = v->cur.col; i < ln->len - 1; i++)
            ln->text[i] = ln->text[i + 1];
        ln->len--;
        ln->text[ln->len] = '\0';
        v->buf.modified = 1;
        v->dirty = 1;
    } else if (v->cur.row < v->buf.n_lines - 1) {
        VimLine *cur = &v->buf.lines[v->cur.row];
        VimLine *next = &v->buf.lines[v->cur.row + 1];
        int avail = VIM_MAX_LINE_LEN - 1 - cur->len;
        int take = (next->len < avail) ? next->len : avail;
        for (int i = 0; i < take; i++) cur->text[cur->len + i] = next->text[i];
        cur->len += take;
        cur->text[cur->len] = '\0';
        for (int i = v->cur.row + 1; i < v->buf.n_lines - 1; i++)
            v->buf.lines[i] = v->buf.lines[i + 1];
        v->buf.n_lines--;
        v->buf.modified = 1;
        v->dirty = 1;
    }
}

static void vim_break_line(VimInstance *v)
{
    if (v->buf.n_lines >= VIM_MAX_LINES) return;
    VimLine *ln = &v->buf.lines[v->cur.row];
    int rest = ln->len - v->cur.col;
    if (rest < 0) rest = 0;
    for (int i = v->buf.n_lines; i > v->cur.row + 1; i--)
        v->buf.lines[i] = v->buf.lines[i - 1];
    v->buf.n_lines++;

    VimLine *newln = &v->buf.lines[v->cur.row + 1];
    newln->len = 0;
    int ai = 0;
    if (v->autoindent) {
        while (ai < ln->len && v_isspace(ln->text[ai])) ai++;
        int j;
        for (j = 0; j < ai && j < VIM_MAX_LINE_LEN - 1; j++)
            newln->text[j] = ln->text[j];
        newln->len = j;
    }
    int take = (rest < VIM_MAX_LINE_LEN - 1 - newln->len) ? rest : VIM_MAX_LINE_LEN - 1 - newln->len;
    for (int i = 0; i < take; i++)
        newln->text[newln->len + i] = ln->text[v->cur.col + i];
    newln->len += take;
    newln->text[newln->len] = '\0';

    ln->len = v->cur.col;
    ln->text[ln->len] = '\0';

    v->cur.row++;
    v->cur.col = ai;
    v->buf.modified = 1;
    v->dirty = 1;
}

static void vim_delete_line(VimInstance *v, int row)
{
    if (v->buf.n_lines <= 1) {
        v->buf.lines[0].len = 0;
        v->buf.lines[0].text[0] = '\0';
        v->buf.modified = 1;
        v->dirty = 1;
        return;
    }
    for (int i = row; i < v->buf.n_lines - 1; i++)
        v->buf.lines[i] = v->buf.lines[i + 1];
    v->buf.n_lines--;
    v->buf.modified = 1;
    v->dirty = 1;
}

static void vim_yank_line(VimInstance *v, int row)
{
    if (row < 0 || row >= v->buf.n_lines) return;
    VimLine *ln = &v->buf.lines[row];
    int take = ln->len;
    if (take > VIM_CLIP_MAX_LEN - 1) take = VIM_CLIP_MAX_LEN - 1;
    for (int i = 0; i < take; i++) v->clip[i] = ln->text[i];
    v->clip_len = take;
    v->clip_is_line = 1;
}

static void vim_paste_after(VimInstance *v)
{
    if (v->clip_len == 0) return;
    if (v->clip_is_line) {
        if (v->buf.n_lines >= VIM_MAX_LINES) return;
        for (int i = v->buf.n_lines; i > v->cur.row + 1; i--)
            v->buf.lines[i] = v->buf.lines[i - 1];
        v->buf.n_lines++;
        VimLine *newln = &v->buf.lines[v->cur.row + 1];
        int take = v->clip_len;
        if (take > VIM_MAX_LINE_LEN - 1) take = VIM_MAX_LINE_LEN - 1;
        for (int i = 0; i < take; i++) newln->text[i] = v->clip[i];
        newln->len = take;
        newln->text[newln->len] = '\0';
        v->cur.row++;
        v->cur.col = 0;
    } else {
        VimLine *ln = &v->buf.lines[v->cur.row];
        if (ln->len + v->clip_len >= VIM_MAX_LINE_LEN) return;
        for (int i = ln->len - 1; i >= v->cur.col; i--)
            ln->text[i + v->clip_len] = ln->text[i];
        for (int i = 0; i < v->clip_len; i++)
            ln->text[v->cur.col + i] = v->clip[i];
        ln->len += v->clip_len;
        ln->text[ln->len] = '\0';
        v->cur.col += v->clip_len;
    }
    v->buf.modified = 1;
    v->dirty = 1;
}

static void vim_paste_before(VimInstance *v)
{
    if (v->clip_len == 0) return;
    if (v->clip_is_line) {
        if (v->buf.n_lines >= VIM_MAX_LINES) return;
        for (int i = v->buf.n_lines; i > v->cur.row; i--)
            v->buf.lines[i] = v->buf.lines[i - 1];
        v->buf.n_lines++;
        VimLine *newln = &v->buf.lines[v->cur.row];
        int take = v->clip_len;
        if (take > VIM_MAX_LINE_LEN - 1) take = VIM_MAX_LINE_LEN - 1;
        for (int i = 0; i < take; i++) newln->text[i] = v->clip[i];
        newln->len = take;
        newln->text[newln->len] = '\0';
        v->cur.col = 0;
    } else {
        vim_paste_after(v);
        v->cur.col -= v->clip_len;
        if (v->cur.col < 0) v->cur.col = 0;
    }
    v->buf.modified = 1;
    v->dirty = 1;
}

static void vim_delete_to_end(VimInstance *v)
{
    VimLine *ln = &v->buf.lines[v->cur.row];
    if (v->cur.col < ln->len) {
        int take = ln->len - v->cur.col;
        if (take > VIM_CLIP_MAX_LEN - 1) take = VIM_CLIP_MAX_LEN - 1;
        for (int i = 0; i < take; i++) v->clip[i] = ln->text[v->cur.col + i];
        v->clip_len = take;
        v->clip_is_line = 0;
        ln->len = v->cur.col;
        ln->text[ln->len] = '\0';
        v->buf.modified = 1;
        v->dirty = 1;
    }
}

static void vim_change_to_end(VimInstance *v)
{
    vim_delete_to_end(v);
    v->mode = VIM_MODE_INSERT;
}

static void vim_delete_word(VimInstance *v)
{
    VimLine *ln = &v->buf.lines[v->cur.row];
    int start = v->cur.col;
    int end = start;
    while (end < ln->len && v_isword(ln->text[end])) end++;
    while (end < ln->len && v_isspace(ln->text[end])) end++;
    if (end == start && end < ln->len) end++;
    int take = end - start;
    if (take > VIM_CLIP_MAX_LEN - 1) take = VIM_CLIP_MAX_LEN - 1;
    for (int i = 0; i < take; i++) v->clip[i] = ln->text[start + i];
    v->clip_len = take;
    v->clip_is_line = 0;
    for (int i = end; i < ln->len; i++) ln->text[start + i - end] = ln->text[i];
    ln->len -= take;
    ln->text[ln->len] = '\0';
    v->cur.col = start;
    v->buf.modified = 1;
    v->dirty = 1;
}

static void vim_yank_word(VimInstance *v)
{
    VimLine *ln = &v->buf.lines[v->cur.row];
    int start = v->cur.col;
    int end = start;
    while (end < ln->len && v_isword(ln->text[end])) end++;
    while (end < ln->len && v_isspace(ln->text[end])) end++;
    if (end == start && end < ln->len) end++;
    int take = end - start;
    if (take > VIM_CLIP_MAX_LEN - 1) take = VIM_CLIP_MAX_LEN - 1;
    for (int i = 0; i < take; i++) v->clip[i] = ln->text[start + i];
    v->clip_len = take;
    v->clip_is_line = 0;
}

static void vim_change_word(VimInstance *v)
{
    vim_delete_word(v);
    v->mode = VIM_MODE_INSERT;
}

static void vim_join_lines(VimInstance *v)
{
    if (v->cur.row >= v->buf.n_lines - 1) return;
    VimLine *cur = &v->buf.lines[v->cur.row];
    VimLine *next = &v->buf.lines[v->cur.row + 1];
    int need_space = (cur->len > 0 && next->len > 0 && !v_isspace(cur->text[cur->len - 1]));
    int avail = VIM_MAX_LINE_LEN - 1 - cur->len;
    int add = need_space ? 1 : 0;
    int take = (next->len < avail - add) ? next->len : avail - add;
    if (need_space && cur->len < VIM_MAX_LINE_LEN - 1) {
        cur->text[cur->len++] = ' ';
    }
    for (int i = 0; i < take; i++) cur->text[cur->len + i] = next->text[i];
    cur->len += take;
    cur->text[cur->len] = '\0';
    for (int i = v->cur.row + 1; i < v->buf.n_lines - 1; i++)
        v->buf.lines[i] = v->buf.lines[i + 1];
    v->buf.n_lines--;
    v->buf.modified = 1;
    v->dirty = 1;
}

static void vim_replace_char(VimInstance *v, char c)
{
    VimLine *ln = &v->buf.lines[v->cur.row];
    if (v->cur.col < ln->len) {
        ln->text[v->cur.col] = c;
        v->buf.modified = 1;
        v->dirty = 1;
    } else if (v->cur.col == ln->len && ln->len < VIM_MAX_LINE_LEN - 1) {
        ln->text[ln->len++] = c;
        ln->text[ln->len] = '\0';
        v->buf.modified = 1;
        v->dirty = 1;
    }
    if (v->cur.col < ln->len) v->cur.col++;
}

static void vim_goto_line(VimInstance *v, int line1)
{
    line1--;
    if (line1 < 0) line1 = 0;
    if (line1 >= v->buf.n_lines) line1 = v->buf.n_lines - 1;
    v->cur.row = line1;
    cur_clamp(v);
    vim_ensure_cursor_visible(v);
    v->dirty = 1;
}

/* =========================================================================
 * Search
 * ========================================================================= */

static int vim_search_next(VimInstance *v)
{
    if (!v->search_pat[0]) return 0;
    int patlen = v_strlen(v->search_pat);
    int start_row = v->cur.row;
    int start_col = v->cur.col + 1;
    for (int r = start_row; r < v->buf.n_lines; r++) {
        VimLine *ln = &v->buf.lines[r];
        int c0 = (r == start_row) ? start_col : 0;
        for (int c = c0; c <= ln->len - patlen; c++) {
            int match = 1;
            for (int i = 0; i < patlen; i++) {
                char a = ln->text[c + i];
                char b = v->search_pat[i];
                if (v->smartcase) {
                    if (a != b) { match = 0; break; }
                } else {
                    if (a >= 'A' && a <= 'Z') a += 32;
                    if (b >= 'A' && b <= 'Z') b += 32;
                    if (a != b) { match = 0; break; }
                }
            }
            if (match) {
                v->cur.row = r;
                v->cur.col = c;
                vim_ensure_cursor_visible(v);
                v->dirty = 1;
                return 1;
            }
        }
    }
    return 0;
}

static int vim_search_prev(VimInstance *v)
{
    if (!v->search_pat[0]) return 0;
    int patlen = v_strlen(v->search_pat);
    int start_row = v->cur.row;
    int start_col = v->cur.col - 1;
    for (int r = start_row; r >= 0; r--) {
        VimLine *ln = &v->buf.lines[r];
        int c0 = (r == start_row) ? start_col : ln->len - patlen;
        if (c0 > ln->len - patlen) c0 = ln->len - patlen;
        for (int c = c0; c >= 0; c--) {
            int match = 1;
            for (int i = 0; i < patlen; i++) {
                char a = ln->text[c + i];
                char b = v->search_pat[i];
                if (v->smartcase) {
                    if (a != b) { match = 0; break; }
                } else {
                    if (a >= 'A' && a <= 'Z') a += 32;
                    if (b >= 'A' && b <= 'Z') b += 32;
                    if (a != b) { match = 0; break; }
                }
            }
            if (match) {
                v->cur.row = r;
                v->cur.col = c;
                vim_ensure_cursor_visible(v);
                v->dirty = 1;
                return 1;
            }
        }
    }
    return 0;
}

/* =========================================================================
 * Motions
 * ========================================================================= */

static void motion_left(VimInstance *v, int count)
{
    v->cur.col -= count;
    if (v->cur.col < 0) v->cur.col = 0;
    cur_set_want(v);
    v->dirty = 1;
}

static void motion_right(VimInstance *v, int count)
{
    int linelen = v->buf.lines[v->cur.row].len;
    v->cur.col += count;
    if (v->cur.col > linelen) v->cur.col = linelen;
    cur_set_want(v);
    v->dirty = 1;
}

static void motion_up(VimInstance *v, int count)
{
    v->cur.row -= count;
    if (v->cur.row < 0) v->cur.row = 0;
    cur_recall_want(v);
    vim_ensure_cursor_visible(v);
    v->dirty = 1;
}

static void motion_down(VimInstance *v, int count)
{
    v->cur.row += count;
    if (v->cur.row >= v->buf.n_lines) v->cur.row = v->buf.n_lines - 1;
    cur_recall_want(v);
    vim_ensure_cursor_visible(v);
    v->dirty = 1;
}

static void motion_line_start(VimInstance *v)
{
    v->cur.col = 0;
    cur_set_want(v);
    v->dirty = 1;
}

static void motion_line_end(VimInstance *v)
{
    v->cur.col = v->buf.lines[v->cur.row].len;
    cur_set_want(v);
    v->dirty = 1;
}

static void motion_first_nonblank(VimInstance *v)
{
    VimLine *ln = &v->buf.lines[v->cur.row];
    int c = 0;
    while (c < ln->len && v_isspace(ln->text[c])) c++;
    v->cur.col = c;
    cur_set_want(v);
    v->dirty = 1;
}

static void motion_word_forward(VimInstance *v, int count)
{
    while (count-- > 0) {
        VimLine *ln = &v->buf.lines[v->cur.row];
        int c = v->cur.col;
        if (c < ln->len && v_isword(ln->text[c])) {
            while (c < ln->len && v_isword(ln->text[c])) c++;
        } else if (c < ln->len) {
            c++;
        }
        while (c < ln->len && v_isspace(ln->text[c])) c++;
        if (c >= ln->len && v->cur.row < v->buf.n_lines - 1) {
            v->cur.row++;
            v->cur.col = 0;
            continue;
        }
        v->cur.col = c;
    }
    cur_set_want(v);
    vim_ensure_cursor_visible(v);
    v->dirty = 1;
}

static void motion_word_back(VimInstance *v, int count)
{
    while (count-- > 0) {
        VimLine *ln = &v->buf.lines[v->cur.row];
        int c = v->cur.col;
        if (c > 0) c--;
        while (c > 0 && v_isspace(ln->text[c])) c--;
        if (c == 0 && v_isspace(ln->text[c])) {
            if (v->cur.row > 0) {
                v->cur.row--;
                v->cur.col = v->buf.lines[v->cur.row].len;
                continue;
            }
        }
        if (c > 0 && v_isword(ln->text[c])) {
            while (c > 0 && v_isword(ln->text[c - 1])) c--;
        } else if (c > 0) {
            while (c > 0 && !v_isword(ln->text[c - 1]) && !v_isspace(ln->text[c - 1])) c--;
        }
        v->cur.col = c;
    }
    cur_set_want(v);
    vim_ensure_cursor_visible(v);
    v->dirty = 1;
}

static void motion_goto_top(VimInstance *v)    { v->cur.row = 0; cur_clamp(v); vim_ensure_cursor_visible(v); v->dirty = 1; }
static void motion_goto_bottom(VimInstance *v) { v->cur.row = v->buf.n_lines - 1; cur_clamp(v); vim_ensure_cursor_visible(v); v->dirty = 1; }

static void motion_page_down(VimInstance *v)
{
    int vis = vim_lines_visible(v);
    v->cur.row += vis;
    if (v->cur.row >= v->buf.n_lines) v->cur.row = v->buf.n_lines - 1;
    cur_recall_want(v);
    vim_ensure_cursor_visible(v);
    v->dirty = 1;
}

static void motion_page_up(VimInstance *v)
{
    int vis = vim_lines_visible(v);
    v->cur.row -= vis;
    if (v->cur.row < 0) v->cur.row = 0;
    cur_recall_want(v);
    vim_ensure_cursor_visible(v);
    v->dirty = 1;
}

/* =========================================================================
 * Insert mode key handler
 * ========================================================================= */

static void vim_insert_key(VimInstance *v, char c)
{
    if (c == 27) { /* Escape */
        v->mode = VIM_MODE_NORMAL;
        if (v->cur.col > 0) v->cur.col--;
        cur_clamp(v);
        cur_set_want(v);
        v->dirty = 1;
        return;
    }
    switch (c) {
        case VKEY_UP:    motion_up(v, 1); break;
        case VKEY_DOWN:  motion_down(v, 1); break;
        case VKEY_LEFT:  motion_left(v, 1); break;
        case VKEY_RIGHT: motion_right(v, 1); break;
        case VKEY_PGUP:  motion_page_up(v); break;
        case VKEY_PGDN:  motion_page_down(v); break;
        case '\b':
        case 127:
            vim_backspace(v);
            break;
        case '\n':
        case '\r':
            vim_break_line(v);
            break;
        case '\t': {
            int ts = v->tabstop;
            if (ts < 1) ts = 4;
            int col = v->cur.col;
            int spaces = ts - (col % ts);
            for (int i = 0; i < spaces; i++) vim_insert_char(v, ' ');
            break;
        }
        default:
            if ((unsigned char)c >= 32 && (unsigned char)c < 127) {
                vim_insert_char(v, c);
            }
            break;
    }
    vim_ensure_cursor_visible(v);
    vim_sync_scroll_wm(v);
}

/* =========================================================================
 * Command-line mode key handler
 * ========================================================================= */

static void vim_cmdline_key(VimInstance *v, char c)
{
    if (c == 27) { /* Escape */
        v->mode = VIM_MODE_NORMAL;
        v->cmd_len = 0;
        v->cmd_buf[0] = '\0';
        v->dirty = 1;
        return;
    }
    if (c == '\n' || c == '\r') {
        v->cmd_buf[v->cmd_len] = '\0';
        v->mode = VIM_MODE_NORMAL;
        v->dirty = 1;

        const char *cmd = v->cmd_buf;
        int force = 0;
        int do_write = 0;
        int do_quit  = 0;

        /* Parse command letter and check for ! */
        if (cmd[0] == 'w' && cmd[1] == 'q') {
            do_write = 1; do_quit = 1;
            if (cmd[2] == '!') force = 1;
        } else if (cmd[0] == 'x') {
            do_write = 1; do_quit = 1;
            if (cmd[1] == '!') force = 1;
        } else if (cmd[0] == 'w') {
            do_write = 1;
            if (cmd[1] == '!') force = 1;
        } else if (cmd[0] == 'q') {
            do_quit = 1;
            if (cmd[1] == '!') force = 1;
        } else if (cmd[0] >= '0' && cmd[0] <= '9') {
            int line = v_atoi(cmd);
            vim_goto_line(v, line);
        }

        /* Execute write and/or quit */
        if (do_write) {
            if (buf_save(&v->buf) == 0) {
                v->has_undo = 0;
            }
        }
        if (do_quit) {
            if (!force && v->buf.modified) {
                v->mode = VIM_MODE_NORMAL;
                v->dirty = 1;
                v->cmd_len = 0;
                v->cmd_buf[0] = '\0';
                return;
            }
            if (v->inline_mode) {
                v->active = 0;
                if (v->on_quit) v->on_quit(v->shell_extra);
            } else if (v->wm_handle >= 0) {
                WM_CloseWindow(v->wm_handle);
                v->wm_handle = -1;
                v->active = 0;
            }
        }
        v->cmd_len = 0;
        v->cmd_buf[0] = '\0';
        return;
    }
    if (c == '\b' || c == 127) {
        if (v->cmd_len > 0) v->cmd_len--;
        v->cmd_buf[v->cmd_len] = '\0';
        v->dirty = 1;
        return;
    }
    if ((unsigned char)c >= 32 && (unsigned char)c < 127 && v->cmd_len < VIM_MAX_CMD_LEN - 1) {
        v->cmd_buf[v->cmd_len++] = c;
        v->cmd_buf[v->cmd_len] = '\0';
        v->dirty = 1;
    }
}

/* =========================================================================
 * Normal mode key handler
 * ========================================================================= */

static void exec_pending_op(VimInstance *v, char motion)
{
    int count = v->pending_count ? v->pending_count : 1;
    char op = v->pending_op;
    v->pending_op = 0;
    v->pending_count = 0;

    if (op == 'r') {
        vim_replace_char(v, motion);
        return;
    }

    switch (motion) {
        case 'd':
            if (op == 'd') {
                vim_save_undo(v);
                vim_yank_line(v, v->cur.row);
                vim_delete_line(v, v->cur.row);
                if (v->cur.row >= v->buf.n_lines) v->cur.row = v->buf.n_lines - 1;
                cur_clamp(v);
                v->dirty = 1;
            } else if (op == 'c') {
                vim_save_undo(v);
                vim_yank_line(v, v->cur.row);
                vim_delete_line(v, v->cur.row);
                if (v->cur.row >= v->buf.n_lines) v->cur.row = v->buf.n_lines - 1;
                v->mode = VIM_MODE_INSERT;
                v->dirty = 1;
            } else if (op == 'y') {
                vim_yank_line(v, v->cur.row);
                v->dirty = 1;
            }
            break;
        case 'w':
            if (op == 'd') { vim_save_undo(v); vim_delete_word(v); }
            else if (op == 'c') { vim_save_undo(v); vim_change_word(v); }
            else if (op == 'y') { vim_yank_word(v); }
            break;
        case '$':
            if (op == 'd') { vim_save_undo(v); vim_delete_to_end(v); }
            else if (op == 'c') { vim_save_undo(v); vim_change_to_end(v); }
            break;
    }
}

static void vim_normal_key(VimInstance *v, char c)
{
    int count = v->pending_count ? v->pending_count : 1;

    /* Pending operator awaiting motion */
    if (v->pending_op) {
        exec_pending_op(v, c);
        return;
    }

    /* Count prefix */
    if (c >= '1' && c <= '9') {
        v->pending_count = v->pending_count * 10 + (c - '0');
        return;
    }
    v->pending_count = 0;

    switch (c) {
        /* Movement */
        case 'h': case VKEY_LEFT:  motion_left(v, count); break;
        case 'j': case VKEY_DOWN:  motion_down(v, count); break;
        case 'k': case VKEY_UP:    motion_up(v, count); break;
        case 'l': case VKEY_RIGHT: motion_right(v, count); break;
        case '0': motion_line_start(v); break;
        case '$': motion_line_end(v); break;
        case '^': motion_first_nonblank(v); break;
        case 'w': motion_word_forward(v, count); break;
        case 'b': motion_word_back(v, count); break;
        case 'g':
            /* gg = go to top; handled as pending since we need second g */
            v->pending_op = 'g';
            break;
        case 'G': motion_goto_bottom(v); break;
        case VKEY_PGUP: motion_page_up(v); break;
        case VKEY_PGDN: motion_page_down(v); break;

        /* Operators */
        case 'd': v->pending_op = 'd'; break;
        case 'c': v->pending_op = 'c'; break;
        case 'y': v->pending_op = 'y'; break;
        case 'r': v->pending_op = 'r'; break;

        /* Edit */
        case 'i': v->mode = VIM_MODE_INSERT; v->dirty = 1; break;
        case 'I': motion_first_nonblank(v); v->mode = VIM_MODE_INSERT; v->dirty = 1; break;
        case 'a': motion_right(v, 1); v->mode = VIM_MODE_INSERT; v->dirty = 1; break;
        case 'A': motion_line_end(v); v->mode = VIM_MODE_INSERT; v->dirty = 1; break;
        case 'o':
            motion_line_end(v);
            vim_break_line(v);
            v->mode = VIM_MODE_INSERT;
            break;
        case 'O': {
            v->cur.col = 0;
            vim_break_line(v);
            v->cur.row--;
            v->mode = VIM_MODE_INSERT;
            break;
        }

        /* Delete / change single char */
        case 'x':
            vim_save_undo(v);
            vim_delete_char(v);
            break;
        case 'X':
            if (v->cur.col > 0) {
                vim_save_undo(v);
                v->cur.col--;
                vim_delete_char(v);
            }
            break;
        case 'J':
            vim_save_undo(v);
            vim_join_lines(v);
            break;

        /* Yank / paste */
        case 'p': vim_paste_after(v); break;
        case 'P': vim_paste_before(v); break;

        /* Undo */
        case 'u': vim_undo(v); break;

        /* Search */
        case 'n':
            if (v->search_dir > 0) vim_search_next(v);
            else vim_search_prev(v);
            break;
        case 'N':
            if (v->search_dir > 0) vim_search_prev(v);
            else vim_search_next(v);
            break;

        /* Enter command line */
        case ':':
            v->mode = VIM_MODE_CMDLINE;
            v->cmd_len = 0;
            v->cmd_buf[0] = '\0';
            v->dirty = 1;
            break;
        case '/':
            v->mode = VIM_MODE_CMDLINE;
            v->cmd_len = 1;
            v->cmd_buf[0] = '/';
            v->cmd_buf[1] = '\0';
            v->dirty = 1;
            break;

        /* Enter visual mode */
        case 'v':
            v->mode = VIM_MODE_VISUAL;
            v->cur.sel_row = v->cur.row;
            v->cur.sel_col = v->cur.col;
            v->dirty = 1;
            break;
    }

    /* Resolve pending 'g' (gg) */
    if (v->pending_op == 'g') {
        if (c == 'g') motion_goto_top(v);
        v->pending_op = 0;
    }

    vim_sync_scroll_wm(v);
}

/* =========================================================================
 * Visual mode key handler
 * ========================================================================= */

static void vim_visual_key(VimInstance *v, char c)
{
    int count = v->pending_count ? v->pending_count : 1;

    if (c == 27) { /* Escape */
        v->mode = VIM_MODE_NORMAL;
        v->dirty = 1;
        return;
    }

    switch (c) {
        case 'h': case VKEY_LEFT:  motion_left(v, count); break;
        case 'j': case VKEY_DOWN:  motion_down(v, count); break;
        case 'k': case VKEY_UP:    motion_up(v, count); break;
        case 'l': case VKEY_RIGHT: motion_right(v, count); break;
        case 'w': motion_word_forward(v, count); break;
        case 'b': motion_word_back(v, count); break;
        case '0': motion_line_start(v); break;
        case '$': motion_line_end(v); break;
        case '^': motion_first_nonblank(v); break;
        case 'g': v->pending_op = 'g'; break;
        case 'G': motion_goto_bottom(v); break;
        case 'd': {
            /* Delete selection (simplified: char-wise from anchor to cursor) */
            vim_save_undo(v);
            if (v->cur.sel_row == v->cur.row) {
                VimLine *ln = &v->buf.lines[v->cur.row];
                int s = v->cur.sel_col;
                int e = v->cur.col;
                if (s > e) { int t = s; s = e; e = t; }
                if (e >= ln->len) e = ln->len - 1;
                if (s <= e && ln->len > 0) {
                    int take = e - s + 1;
                    if (take > VIM_CLIP_MAX_LEN - 1) take = VIM_CLIP_MAX_LEN - 1;
                    for (int i = 0; i < take; i++) v->clip[i] = ln->text[s + i];
                    v->clip_len = take;
                    v->clip_is_line = 0;
                    for (int i = e + 1; i < ln->len; i++) ln->text[s + i - e - 1] = ln->text[i];
                    ln->len -= take;
                    ln->text[ln->len] = '\0';
                    v->cur.col = s;
                    v->buf.modified = 1;
                }
            } else {
                /* Multi-line visual delete is complex; do line-wise for now */
                int sr = v->cur.sel_row;
                int er = v->cur.row;
                if (sr > er) { int t = sr; sr = er; er = t; }
                int take = 0;
                for (int r = sr; r <= er && take < VIM_CLIP_MAX_LEN - 2; r++) {
                    int ll = v->buf.lines[r].len;
                    if (ll + take >= VIM_CLIP_MAX_LEN - 2) ll = VIM_CLIP_MAX_LEN - 2 - take;
                    for (int i = 0; i < ll; i++) v->clip[take + i] = v->buf.lines[r].text[i];
                    v->clip[take + ll] = '\n';
                    take += ll + 1;
                }
                v->clip_len = take;
                v->clip_is_line = 1;
                for (int r = sr; r <= er; r++) vim_delete_line(v, sr);
                v->cur.row = sr;
                if (v->cur.row >= v->buf.n_lines) v->cur.row = v->buf.n_lines - 1;
                cur_clamp(v);
                v->buf.modified = 1;
            }
            v->mode = VIM_MODE_NORMAL;
            v->dirty = 1;
            break;
        }
        case 'y': {
            if (v->cur.sel_row == v->cur.row) {
                VimLine *ln = &v->buf.lines[v->cur.row];
                int s = v->cur.sel_col;
                int e = v->cur.col;
                if (s > e) { int t = s; s = e; e = t; }
                if (e >= ln->len) e = ln->len - 1;
                int take = e - s + 1;
                if (take > VIM_CLIP_MAX_LEN - 1) take = VIM_CLIP_MAX_LEN - 1;
                for (int i = 0; i < take; i++) v->clip[i] = ln->text[s + i];
                v->clip_len = take;
                v->clip_is_line = 0;
            } else {
                int sr = v->cur.sel_row;
                int er = v->cur.row;
                if (sr > er) { int t = sr; sr = er; er = t; }
                int take = 0;
                for (int r = sr; r <= er && take < VIM_CLIP_MAX_LEN - 2; r++) {
                    int ll = v->buf.lines[r].len;
                    if (ll + take >= VIM_CLIP_MAX_LEN - 2) ll = VIM_CLIP_MAX_LEN - 2 - take;
                    for (int i = 0; i < ll; i++) v->clip[take + i] = v->buf.lines[r].text[i];
                    v->clip[take + ll] = '\n';
                    take += ll + 1;
                }
                v->clip_len = take;
                v->clip_is_line = 1;
            }
            v->mode = VIM_MODE_NORMAL;
            v->dirty = 1;
            break;
        }
    }

    if (v->pending_op == 'g') {
        if (c == 'g') motion_goto_top(v);
        v->pending_op = 0;
    }
    vim_sync_scroll_wm(v);
}

/* =========================================================================
 * Drawing
 * ========================================================================= */

static void vim_draw(VimInstance *v, int wx, int wy, int ww, int wh)
{
    v->win_x = wx; v->win_y = wy; v->win_w = ww; v->win_h = wh;

    int cx = wx + 1;
    int cy = wy + WM_TITLEBAR_H;
    int cw = ww - 1 - WM_SCROLLBAR_W;
    int ch = wh - WM_TITLEBAR_H - WM_SCROLLBAR_W;

    /* Background */
    FB_FillRect(cx, cy, cw, ch, WB_WHITE);

    int lnum_w = v->number ? VIM_LNUM_W : 0;
    int text_x = cx + 4 + lnum_w;
    int text_w = cw - 8 - lnum_w;
    int text_h = ch - VIM_STATUS_H;
    int max_vis_lines = text_h / 16;
    if (max_vis_lines < 1) max_vis_lines = 1;

    int scroll_y = WM_GetScrollY(v->wm_handle);
    int first_line = scroll_y / 16;
    if (first_line < 0) first_line = 0;

    /* Draw text lines */
    for (int i = 0; i < max_vis_lines; i++) {
        int row = first_line + i;
        if (row >= v->buf.n_lines) break;
        int ly = cy + i * 16;

        /* Line number */
        if (v->number) {
            char numstr[8];
            v_itoa(row + 1, numstr, 8);
            int nsl = v_strlen(numstr);
            int nx = cx + 4 + (VIM_LNUM_W - nsl * 8) - 8;
            uint32_t nfg = (row == v->cur.row) ? WB_BLACK : WB_DARK_GREY;
            FB_PutStr(nx, ly, numstr, nfg, WB_WHITE);
            FB_DrawVLine(cx + 4 + VIM_LNUM_W - 4, ly, 16, WB_LIGHT_GREY);
        }

        VimLine *ln = &v->buf.lines[row];
        int draw_len = ln->len;
        if (draw_len > text_w / 8) draw_len = text_w / 8;

        /* Cursor line highlight */
        if (row == v->cur.row && v->mode != VIM_MODE_NORMAL) {
            FB_FillRect(text_x, ly, text_w, 16, FB_RGB(0xDD, 0xEE, 0xFF));
        }

        /* Search highlight */
        if (v->hlsearch && v->search_active && v->search_pat[0]) {
            int patlen = v_strlen(v->search_pat);
            for (int c = 0; c <= ln->len - patlen; c++) {
                int match = 1;
                for (int j = 0; j < patlen; j++) {
                    char a = ln->text[c + j];
                    char b = v->search_pat[j];
                    if (a >= 'A' && a <= 'Z') a += 32;
                    if (b >= 'A' && b <= 'Z') b += 32;
                    if (a != b) { match = 0; break; }
                }
                if (match) {
                    FB_FillRect(text_x + c * 8, ly, patlen * 8, 16, FB_RGB(0xFF, 0xFF, 0x88));
                }
            }
        }

        /* Visual selection highlight */
        if (v->mode == VIM_MODE_VISUAL && row >= v_min(v->cur.sel_row, v->cur.row) &&
            row <= v_max(v->cur.sel_row, v->cur.row)) {
            int s, e;
            if (v->cur.sel_row == v->cur.row) {
                s = v_min(v->cur.sel_col, v->cur.col);
                e = v_max(v->cur.sel_col, v->cur.col);
                if (e >= ln->len) e = ln->len - 1;
            } else if (row == v_min(v->cur.sel_row, v->cur.row)) {
                s = (v->cur.sel_row < v->cur.row) ? v->cur.sel_col : v->cur.col;
                e = ln->len - 1;
            } else if (row == v_max(v->cur.sel_row, v->cur.row)) {
                s = 0;
                e = (v->cur.sel_row > v->cur.row) ? v->cur.sel_col : v->cur.col;
            } else {
                s = 0; e = ln->len - 1;
            }
            if (e >= ln->len) e = ln->len - 1;
            if (s <= e && ln->len > 0) {
                FB_FillRect(text_x + s * 8, ly, (e - s + 1) * 8, 16, WB_BLUE);
                char seg[64];
                int sl = e - s + 1;
                if (sl > 63) sl = 63;
                for (int j = 0; j < sl; j++) seg[j] = ln->text[s + j];
                seg[sl] = '\0';
                FB_PutStr(text_x + s * 8, ly, seg, WB_WHITE, WB_BLUE);
                /* skip normal draw for this segment */
                int seg_end = s + sl;
                int before = s;
                if (before > 0) {
                    char pre[64]; int pl = before; if (pl > 63) pl = 63;
                    for (int j = 0; j < pl; j++) pre[j] = ln->text[j];
                    pre[pl] = '\0';
                    FB_PutStr(text_x, ly, pre, WB_BLACK, WB_WHITE);
                }
                int after = ln->len - seg_end;
                if (after > 0) {
                    char post[64]; int pl = after; if (pl > 63) pl = 63;
                    for (int j = 0; j < pl; j++) post[j] = ln->text[seg_end + j];
                    post[pl] = '\0';
                    FB_PutStr(text_x + seg_end * 8, ly, post, WB_BLACK, WB_WHITE);
                }
                goto line_done;
            }
        }

        if (draw_len > 0) {
            char seg[64];
            int sl = draw_len; if (sl > 63) sl = 63;
            for (int j = 0; j < sl; j++) seg[j] = ln->text[j];
            seg[sl] = '\0';
            FB_PutStr(text_x, ly, seg, WB_BLACK, WB_WHITE);
        }
line_done:;
    }

    /* Cursor */
    if (v->mode == VIM_MODE_NORMAL || v->mode == VIM_MODE_VISUAL) {
        int cr = v->cur.row - first_line;
        if (cr >= 0 && cr < max_vis_lines) {
            int cc = v->cur.col;
            FB_FillRect(text_x + cc * 8, cy + cr * 16, 8, 16, WB_BLACK);
            VimLine *ln = &v->buf.lines[v->cur.row];
            if (cc < ln->len) {
                char ch[2] = { ln->text[cc], '\0' };
                FB_PutStr(text_x + cc * 8, cy + cr * 16, ch, WB_WHITE, WB_BLACK);
            }
        }
    } else if (v->mode == VIM_MODE_INSERT) {
        int cr = v->cur.row - first_line;
        if (cr >= 0 && cr < max_vis_lines) {
            FB_DrawRect(text_x + v->cur.col * 8, cy + cr * 16, 8, 16, WB_BLACK);
        }
    }

    /* Status bar */
    int sb_y = cy + ch - VIM_STATUS_H;
    FB_FillRect(cx + 1, sb_y, cw - 2, VIM_STATUS_H, WB_BLUE);
    char status[128];
    const char *modestr;
    switch (v->mode) {
        case VIM_MODE_NORMAL:   modestr = "NORMAL"; break;
        case VIM_MODE_INSERT:   modestr = "INSERT"; break;
        case VIM_MODE_VISUAL:   modestr = "VISUAL"; break;
        case VIM_MODE_CMDLINE:  modestr = "COMMAND"; break;
        default: modestr = "";
    }
    v_strcpy(status, modestr, 128);
    if (v->buf.modified) v_strcat(status, " [+]", 128);
    v_strcat(status, " ", 128);
    v_strcat(status, v->buf.filename[0] ? v->buf.filename : "[No Name]", 128);
    FB_PutStr(cx + 4, sb_y + 2, status, WB_WHITE, WB_BLUE);

    /* Command line */
    if (v->mode == VIM_MODE_CMDLINE) {
        FB_FillRect(cx + 1, sb_y, cw - 2, VIM_STATUS_H, WB_DARK_GREY);
        char cmdl[80];
        v_strcpy(cmdl, ":", 80);
        v_strcat(cmdl, v->cmd_buf, 80);
        FB_PutStr(cx + 4, sb_y + 2, cmdl, WB_WHITE, WB_DARK_GREY);
    }

    v->dirty = 0;
}

/* =========================================================================
 * Draw / key shims (one per slot so WM routes to the right instance)
 * ========================================================================= */

static void draw_shim_0(int wx, int wy, int ww, int wh) { vim_draw(&g_vims[0], wx, wy, ww, wh); }
static void draw_shim_1(int wx, int wy, int ww, int wh) { vim_draw(&g_vims[1], wx, wy, ww, wh); }
static void draw_shim_2(int wx, int wy, int ww, int wh) { vim_draw(&g_vims[2], wx, wy, ww, wh); }
static void draw_shim_3(int wx, int wy, int ww, int wh) { vim_draw(&g_vims[3], wx, wy, ww, wh); }

typedef void (*DrawShim)(int,int,int,int);
static const DrawShim k_draw_shims[VIM_MAX_WINDOWS] = {
    draw_shim_0, draw_shim_1, draw_shim_2, draw_shim_3
};

/* =========================================================================
 * WM callbacks
 * ========================================================================= */


static void vim_key_cb(char c)
{
    /* Find focused vim instance */
    int focus = WM_GetFocus();
    for (int i = 0; i < VIM_MAX_WINDOWS; i++) {
        VimInstance *v = &g_vims[i];
        if (v->active && v->wm_handle == focus) {
            if (!WM_IsWindowActive(v->wm_handle)) {
                v->active = 0;
                v->wm_handle = -1;
                return;
            }
            switch (v->mode) {
                case VIM_MODE_INSERT:  vim_insert_key(v, c); break;
                case VIM_MODE_CMDLINE: vim_cmdline_key(v, c); break;
                case VIM_MODE_VISUAL:  vim_visual_key(v, c); break;
                default:               vim_normal_key(v, c); break;
            }
            return;
        }
    }
}

/* =========================================================================
 * Public API
 * ========================================================================= */

/* -------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

static int vim_find_free_slot(void)
{
    for (int i = 0; i < VIM_MAX_WINDOWS; i++) {
        if (!g_vims[i].active) return i;
        if (!g_vims[i].inline_mode && !WM_IsWindowActive(g_vims[i].wm_handle)) return i;
    }
    return -1;
}

static void vim_init_slot(VimInstance *v, const char *filename)
{
    /* Zero the struct first */
    for (int i = 0; i < (int)sizeof(VimInstance); i++) ((uint8_t *)v)[i] = 0;

    v->wm_handle = -1;
    v->active = 1;
    v->dirty = 1;
    v->mode = VIM_MODE_NORMAL;
    v->showmode = g_cfg_showmode;
    v->number = g_cfg_number;
    v->hlsearch = g_cfg_hlsearch;
    v->tabstop = g_cfg_tabstop;
    v->autoindent = g_cfg_autoindent;
    v->smartcase = g_cfg_smartcase;
    v->wrap = g_cfg_wrap;
    v->search_dir = 1;

    buf_clear(&v->buf);
    if (filename && filename[0]) {
        if (buf_load(&v->buf, filename) != 0) {
            v_strcpy(v->buf.filename, filename, VIM_MAX_FNAME);
        }
    }
    v->cur.row = 0;
    v->cur.col = 0;
    v->cur.scroll_y = 0;
    v->cur.want_col = 0;
}

/* =========================================================================
 * Public API — standalone window
 * ========================================================================= */

int VimWin_Open(const char *filename)
{
    VimWin_Init();

    int idx = vim_find_free_slot();
    if (idx < 0) return -1;

    VimInstance *v = &g_vims[idx];
    vim_init_slot(v, filename);

    int wx = 60 + idx * 24;
    int wy = 60 + idx * 24;
    int ww = 640;
    int wh = 400;

    v->wm_handle = WM_AddWindow(wx, wy, ww, wh, "Vim", k_draw_shims[idx], vim_key_cb);
    if (v->wm_handle < 0) {
        v->active = 0;
        return -1;
    }

    v->win_x = wx; v->win_y = wy; v->win_w = ww; v->win_h = wh;
    vim_sync_scroll_wm(v);
    WM_RaiseWindow(v->wm_handle);
    WM_Redraw();
    return 0;
}

void VimWin_HandleKey(char c)
{
    vim_key_cb(c);
}

void VimWin_Redraw(void)
{
    for (int i = 0; i < VIM_MAX_WINDOWS; i++) {
        VimInstance *v = &g_vims[i];
        if (v->active && !v->inline_mode && WM_IsWindowActive(v->wm_handle)) {
            v->dirty = 1;
        }
    }
}

/* =========================================================================
 * Public API — inline (shell-integrated)
 * ========================================================================= */

int VimWin_OpenInline(const char *filename, void *shell_extra, VimQuitFn on_quit)
{
    VimWin_Init();

    int idx = vim_find_free_slot();
    if (idx < 0) return -1;

    VimInstance *v = &g_vims[idx];
    vim_init_slot(v, filename);
    v->inline_mode = 1;
    v->shell_extra = shell_extra;
    v->on_quit = on_quit;
    return idx;
}

void VimWin_DrawInline(int slot, int wx, int wy, int ww, int wh)
{
    if (slot < 0 || slot >= VIM_MAX_WINDOWS) return;
    VimInstance *v = &g_vims[slot];
    if (!v->active) return;
    vim_draw(v, wx, wy, ww, wh);
}

void VimWin_KeyInline(int slot, char c)
{
    if (slot < 0 || slot >= VIM_MAX_WINDOWS) return;
    VimInstance *v = &g_vims[slot];
    if (!v->active) return;
    switch (v->mode) {
        case VIM_MODE_INSERT:  vim_insert_key(v, c); break;
        case VIM_MODE_CMDLINE: vim_cmdline_key(v, c); break;
        case VIM_MODE_VISUAL:  vim_visual_key(v, c); break;
        default:               vim_normal_key(v, c); break;
    }
}

int VimWin_IsActive(int slot)
{
    if (slot < 0 || slot >= VIM_MAX_WINDOWS) return 0;
    return g_vims[slot].active;
}

int VimWin_GetFilename(int slot, char *out, int max)
{
    if (slot < 0 || slot >= VIM_MAX_WINDOWS || !g_vims[slot].active)
        return 0;
    int i = 0;
    while (i < max - 1 && g_vims[slot].buf.filename[i]) {
        out[i] = g_vims[slot].buf.filename[i];
        i++;
    }
    out[i] = '\0';
    return 1;
}

int VimWin_IsInline(int slot)
{
    if (slot < 0 || slot >= VIM_MAX_WINDOWS || !g_vims[slot].active)
        return 0;
    return g_vims[slot].inline_mode;
}

