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
#include "desktop.h"
#include "cursor.h"
#include "pointer_prefs.h"
#include "wm.h"
#include "vim_win.h"
#include "ed_win.h"
#include "../../emulation/uaos_emu.h"
#include "dos/vfs.h"
#include "dos/ramfs.h"
#include "dos/blockdev.h"
#include "dos/partition.h"
#include "dos/fat32.h"
#include "exec/rom_modules.h"
#include "shell/native_cmd.h"
#include "shell/resident_cmd.h"
#include "exec/uaos_binary.h"
#include "exec/elf64_loader.h"
#include "../net/stack.h"
#include "../irq/ps2mouse.h"
#include "../irq/ps2kbd.h"
#include "../exec/task.h"
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Forward declarations */
typedef struct ShellInstance ShellInstance;
static void inst_print(ShellInstance *s, const char *line);
static void inst_dispatch(ShellInstance *s, const char *line);
static void run_cmd(ShellInstance *s, const char *line);
static NativeCmdCtx shell_make_ctx(ShellInstance *s);
static int arith_itoa(int v, char *buf, int max);
static ShellInstance *g_fdisk_shell = NULL;
static void inst_print_wrapper(const char *line)
{
    if (g_fdisk_shell) inst_print(g_fdisk_shell, line);
}

/* =========================================================================
 * Constants
 * ========================================================================= */

#define TITLEBAR_H      WM_TITLEBAR_H
#define INPUTBAR_H      18
#define BORDER_L        WM_BORDER          /* left: white/blue/black bevel */
#define BORDER_R        WM_SCROLLBAR_W     /* right: scrollbar width */
#define BORDER          BORDER_L           /* legacy alias for top inset */
#define MAX_HIST_LINES  1000
#define MAX_LINE_LEN    96
#define MAX_INPUT       80
#define MAX_SHELLS      4
#define SCROLL_LINES    4    /* lines per Page-Up/Down tick */

/* Special virtual keys injected by kbd driver for scroll/history */
#define VKEY_PGUP  0x01
#define VKEY_PGDN  0x02
#define VKEY_UP    0x03
#define VKEY_DOWN  0x04
#define VKEY_LEFT  0x05
#define VKEY_RIGHT 0x06

#define MAX_CMD_HIST  64   /* command history entries per shell */
#define MAX_ALIASES   32   /* max aliases per shell */
#define MAX_ALIAS_LEN 128  /* max alias expansion string */
#define MAX_ENV_VARS   32   /* max environment variables per shell */
#define MAX_ENV_NAME  32   /* max env var name length */
#define MAX_ENV_VAL   128  /* max env var value length */

/* =========================================================================
 * Per-instance state
 * ========================================================================= */

struct ShellInstance {
    /* WM */
    int  wm_handle;
    int  number;        /* shell number shown in prompt, 1-based */
    int  index;         /* index into g_shells[] and g_hist_buf[] */

    /* Geometry — updated by draw callback */
    int  wx, wy, ww, wh;

    /* History — ring buffer indices, actual storage in g_hist_buf below */
    int  hist_count;
    int  hist_scroll;

    /* Command history (up/down arrow recall) */
    char cmd_hist[MAX_CMD_HIST][MAX_INPUT + 1];
    int  cmd_hist_count;  /* total commands entered */
    int  cmd_hist_nav;    /* navigation offset: 0 = live input, 1 = last cmd */

    /* Aliases */
    char alias_names[MAX_ALIASES][32];
    char alias_values[MAX_ALIASES][MAX_ALIAS_LEN];
    int  alias_count;

    /* Local environment variables */
    char env_names[MAX_ENV_VARS][MAX_ENV_NAME];
    char env_values[MAX_ENV_VARS][MAX_ENV_VAL];
    int  env_count;

    /* Command search path (space-separated list like "C: S: SYS:Utilities") */
    char path[256];

    /* Current working directory (AmigaDOS path, e.g. "RAM:") */
    char cwd[64];

    /* Input */
    char input_buf[MAX_INPUT + 1];
    char input_saved[MAX_INPUT + 1]; /* saved live input while navigating */
    int  input_len;
    int  input_cur;    /* cursor position within input_buf, 0..input_len */
    int  auto_scroll;   /* 1 = pin to bottom on next draw (set by inst_print) */

    /* Fdisk interactive mode */
    int          fdisk_mode;    /* 0 = normal, 1 = fdisk interactive */
    BlockDev    *fdisk_dev;     /* Device being edited */
    PartitionTable fdisk_pt;    /* Partition table being edited */

    /* Vim inline mode */
    int          vim_mode;      /* 0 = normal, 1 = vim inline */
    int          vim_slot;      /* slot in g_vims */

    /* ED inline mode */
    int          ed_mode;       /* 0 = normal, 1 = ed inline */
    int          ed_slot;       /* slot in g_eds */

    /* Ask mode - for interactive input prompts */
    int          ask_mode;      /* 0 = normal, 1 = waiting for ask input */
    char         ask_prompt[MAX_INPUT + 1];  /* Custom prompt to display */
    char         ask_result[MAX_INPUT + 1];  /* Result buffer */
    int          ask_result_ready;           /* 1 = result is ready */

    /* Custom prompt (set by PROMPT command) */
    char         custom_prompt[64];

    /* Last command return code (set by WHY-compatible commands) */
    int          last_rc;

    /* FAILAT threshold — minimum return code treated as failure (default 10) */
    int          failat_threshold;

    /* Script quit flag (set by QUIT command) */
    int          quit_flag;

    /* Keyboard ring buffer — fed by WM/idle task, consumed by shell task */
#define SHELL_KB_BUFSIZE 64
    char         kb_buf[SHELL_KB_BUFSIZE];
    int          kb_head;
    int          kb_tail;
};
typedef struct ShellInstance ShellInstance;

/* History storage in BSS (not on stack) — 1000×96 × 4 shells = 384 KB */
static char g_hist_buf[MAX_SHELLS][MAX_HIST_LINES][MAX_LINE_LEN];

static ShellInstance g_shells[MAX_SHELLS];
static int           g_n_shells = 0;

/* -------------------------------------------------------------------------
 * Keyboard ring buffer helpers (interrupt-safe)
 * ------------------------------------------------------------------------- */
static int shell_kb_enqueue(ShellInstance *s, char c)
{
    __asm__ volatile ("cli");
    int next = (s->kb_tail + 1) % SHELL_KB_BUFSIZE;
    if (next == s->kb_head) {
        __asm__ volatile ("sti");
        return 0;   /* full */
    }
    s->kb_buf[s->kb_tail] = c;
    s->kb_tail = next;
    __asm__ volatile ("sti");
    return 1;
}

static int shell_kb_dequeue(ShellInstance *s, char *c)
{
    __asm__ volatile ("cli");
    if (s->kb_head == s->kb_tail) {
        __asm__ volatile ("sti");
        return 0;   /* empty */
    }
    *c = s->kb_buf[s->kb_head];
    s->kb_head = (s->kb_head + 1) % SHELL_KB_BUFSIZE;
    __asm__ volatile ("sti");
    return 1;
}

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

static int seq(const char *a, const char *b)
{
    int i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == b[i];
}

static int seq_ci(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    char ca = *a, cb = *b;
    if (ca >= 'A' && ca <= 'Z') ca += 32;
    if (cb >= 'A' && cb <= 'Z') cb += 32;
    return ca == cb;
}

/* =========================================================================
 * Per-instance rendering
 * ========================================================================= */

static inline void outb_ser(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb_ser(uint16_t port)
{
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void _ser_putc(char c) {
    while ((inb_ser(0x3F8 + 5) & 0x20) == 0) {}
    outb_ser(0x3F8, (uint8_t)c);
    if (c == '\n') outb_ser(0x3F8, '\r');
}
static inline void _ser_puts(const char *s) { while (*s) _ser_putc(*s++); }
static inline void _ser_putd(int v) {
    char buf[12]; int i = 0;
    if (v == 0) { _ser_putc('0'); return; }
    if (v < 0) { _ser_putc('-'); v = -v; }
    while (v > 0) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i > 0) _ser_putc(buf[--i]);
}

static void inst_draw_contents(ShellInstance *s)
{
    int wx=s->wx, wy=s->wy, ww=s->ww, wh=s->wh;

    int body_w = ww - BORDER_L - BORDER_R;
    int body_y  = wy + TITLEBAR_H;
    int body_h  = wh - TITLEBAR_H - INPUTBAR_H - WM_SCROLLBAR_W;

    /* P6: inst_draw_history fills the text area (inset 4px) with WB_BLACK,
     * so here we only fill the 4px top/bottom margins that it doesn't cover,
     * avoiding a redundant double fill of the text area on every repaint. */
    FB_FillRect(wx+BORDER_L, body_y, body_w, 4, WB_BLACK);
    FB_FillRect(wx+BORDER_L, body_y + body_h - 4, body_w, 4, WB_BLACK);

    /* Separator */
    FB_DrawHLine(wx+BORDER_L, body_y + body_h - 1, body_w, WB_DARK_GREY);

    /* Input bar */
    FB_FillRect(wx+BORDER_L, body_y + body_h,
                body_w, INPUTBAR_H, WB_BLACK);
}

static void inst_draw_history(ShellInstance *s)
{
    int wx=s->wx, wy=s->wy, ww=s->ww, wh=s->wh;

    if (s->vim_mode) {
        VimWin_DrawInline(s->vim_slot, wx, wy, ww, wh);
        return;
    }
    if (s->ed_mode) {
        EdWin_DrawInline(s->ed_slot, wx, wy, ww, wh);
        return;
    }

    int hx = wx + BORDER_L + 4;
    int hy = wy + TITLEBAR_H + 4;
    int hh = wh - TITLEBAR_H - INPUTBAR_H - WM_SCROLLBAR_W - 8;
    int rows = hh / 16;

    FB_FillRect(wx+BORDER_L, hy, ww-BORDER_L-BORDER_R, hh, WB_BLACK);

    /* Max chars that fit in the client width (8px per char, 4px left margin) */
    int max_chars = (ww - BORDER_L - BORDER_R - 8) / 8;
    if (max_chars < 1) max_chars = 1;

    /* Derive start line directly from WM scroll_y.
     * scroll_y=0 -> top of content, scroll_y=max -> bottom (newest lines). */
    int start;
    if (s->wm_handle >= 0) {
        int sy = WM_GetScrollY(s->wm_handle);
        start = sy / 16;
    } else {
        start = s->hist_count - rows;
        if (start < 0) start = 0;
    }
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

/* Extract volume name from path (e.g., "RAM:" or "Workbench:") */
static void extract_vol_prompt(const char *path, char *out, int max)
{
    int i = 0;
    while (i < max - 1 && path[i] && path[i] != '/') {
        out[i] = path[i];
        i++;
    }
    out[i] = '\0';
}

/* Expand AmigaDOS-style prompt escapes: %N = shell number, %S = current
 * directory, %R = last return code.  Unknown %X is copied literally. */
static void expand_prompt(const ShellInstance *s, const char *src,
                          char *dst, int max)
{
    int di = 0;
    while (*src && di < max - 1) {
        if (*src == '%') {
            char c = src[1];
            const char *p = NULL;
            char num[12];
            if (c == 'N' || c == 'n') {
                arith_itoa(s->number, num, sizeof(num));
                p = num;
            } else if (c == 'S' || c == 's') {
                p = s->cwd;
            } else if (c == 'R' || c == 'r') {
                arith_itoa(s->last_rc, num, sizeof(num));
                p = num;
            }
            if (p) {
                while (*p && di < max - 1) dst[di++] = *p++;
                src += 2;
            } else {
                dst[di++] = *src++;
            }
        } else {
            dst[di++] = *src++;
        }
    }
    dst[di] = '\0';
}

static void inst_draw_input(ShellInstance *s)
{
    int wx=s->wx, wy=s->wy, ww=s->ww, wh=s->wh;

    if (s->vim_mode) return;
    if (s->ed_mode) return;

    int ix = wx + BORDER_L + 4;
    int iy = wy + wh - INPUTBAR_H - WM_SCROLLBAR_W;

    /* Build Amiga-style prompt from current volume, or use ask prompt if in ask mode */
    char prompt[80];
    int pi = 0;

    if (s->ask_mode && s->ask_prompt[0]) {
        /* Use custom ask prompt */
        while (s->ask_prompt[pi] && pi < 75) {
            prompt[pi] = s->ask_prompt[pi];
            pi++;
        }
        /* Add ": " if there's room */
        if (pi < 77) { prompt[pi++] = ':'; prompt[pi++] = ' '; }
    } else if (s->custom_prompt[0]) {
        /* Use PROMPT command string, expanding AmigaDOS escapes */
        expand_prompt(s, s->custom_prompt, prompt, sizeof(prompt));
        pi = slen(prompt);
    } else {
        /* Build normal prompt from current volume */
        char vol[32];
        extract_vol_prompt(s->cwd, vol, sizeof(vol));
        /* Copy volume name */
        int vi = 0;
        while (vol[vi] && pi < 35) {
            prompt[pi++] = vol[vi++];
        }
        /* Add "> " */
        if (pi < 38) { prompt[pi++] = '>'; prompt[pi++] = ' '; }
    }
    prompt[pi] = '\0';
    int plen = pi;

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
        /* Cursor block at input_cur position */
        int cur_x = px + s->input_cur * 8;
        if (cur_x + 8 <= right_edge) {
            uint32_t cur_ch_col = WB_BLACK;
            char cur_ch = s->input_buf[s->input_cur] ? s->input_buf[s->input_cur] : ' ';
            FB_FillRect(cur_x, iy+1, 8, 14, WB_WHITE);
            char tmp[2]; tmp[0] = cur_ch; tmp[1] = 0;
            FB_PutStr(cur_x, iy+1, tmp, cur_ch_col, WB_WHITE);
        }
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

/* Update WM's content size for scrollbar thumb proportion. Call when content or window size changes. */
static void inst_update_scrollinfo(ShellInstance *s)
{
    if (s->wm_handle < 0) return;
    int rows = inst_rows(s);
    int view_h    = rows * 16;
    int content_h = s->hist_count * 16;
    if (content_h < view_h) content_h = view_h;
    /* Pass view_h so WM_SetScrollY clamps against the history area,
     * not the full client height which includes input bar + margins. */
    WM_SetScrollInfoEx(s->wm_handle, 0, content_h, view_h);
}

/* Sync shell's hist_scroll from WM's scroll_y. Call before drawing to reflect user scrollbar interaction. */
static void inst_sync_from_wm(ShellInstance *s)
{
    if (s->wm_handle < 0) return;
    int rows = inst_rows(s);
    int sy = WM_GetScrollY(s->wm_handle);
    int max_scroll = s->hist_count - rows;
    if (max_scroll < 0) max_scroll = 0;
    /* WM scroll_y is pixels from top. Convert to lines from top, then to hist_scroll (lines from bottom). */
    int from_top_lines = sy / 16;
    s->hist_scroll = max_scroll - from_top_lines;
    if (s->hist_scroll < 0) s->hist_scroll = 0;
    if (s->hist_scroll > max_scroll) s->hist_scroll = max_scroll;
}

/* Push shell's hist_scroll to WM's scroll_y. Call after keyboard scrolling to update scrollbar thumb. */
static void inst_push_scroll_to_wm(ShellInstance *s)
{
    if (s->wm_handle < 0) return;
    int rows = inst_rows(s);
    int max_scroll = s->hist_count - rows;
    if (max_scroll < 0) max_scroll = 0;
    /* Convert hist_scroll (lines from bottom) to WM scroll_y (pixels from top) */
    int from_top_lines = max_scroll - s->hist_scroll;
    if (from_top_lines < 0) from_top_lines = 0;
    WM_SetScrollY(s->wm_handle, from_top_lines * 16);
}

/* Forward declaration — defined below after inst_dispatch */
typedef struct { void *shell; VfsFile fh; int active; int null; } RedirCtx;
static RedirCtx g_redir;

/* -------------------------------------------------------------------------
 * Pipe support
 * ------------------------------------------------------------------------- */

#define MAX_PIPE_LINES    512
#define MAX_PIPE_SEGMENTS 4

typedef struct {
    char lines[MAX_PIPE_LINES][MAX_LINE_LEN];
    int count;
} PipeBuf;

static PipeBuf g_pipe_buf;
static int     g_pipe_active = 0;
static int     g_pipe_next_idx = 0;
static char    g_pipe_in_file[64];
static int     g_pipe_in_active = 0;

/* -------------------------------------------------------------------------
 * Background job support
 * ------------------------------------------------------------------------- */

#define MAX_BG_JOBS 8

typedef struct {
    char     cmd[MAX_LINE_LEN];
    ShellInstance *shell;
    int      active;      /* 1 = running, 0 = queued */
    int      number;      /* job number for user display */
    int      done;        /* 1 = finished, waiting for removal */
} BgJob;

static BgJob g_bg_jobs[MAX_BG_JOBS];
static int   g_bg_job_count = 0;
static int   g_next_job_num = 1;
static int   g_bg_running   = 0;  /* prevent nested bg dispatch */

static void bg_enqueue(ShellInstance *s, const char *cmd)
{
    if (g_bg_job_count >= MAX_BG_JOBS) {
        inst_print(s, "Job queue full.");
        return;
    }
    BgJob *job = &g_bg_jobs[g_bg_job_count];
    scopy(job->cmd, cmd, MAX_LINE_LEN);
    job->shell   = s;
    job->active  = 0;
    job->done    = 0;
    job->number  = g_next_job_num++;
    g_bg_job_count++;

    char msg[MAX_LINE_LEN];
    scopy(msg, "[", MAX_LINE_LEN);
    char num[8];
    uint_to_dec_s((uint32_t)job->number, num, 8);
    scat(msg, num, MAX_LINE_LEN);
    scat(msg, "] ", MAX_LINE_LEN);
    scat(msg, cmd, MAX_LINE_LEN);
    inst_print(s, msg);
}

static void bg_remove_done(void)
{
    int write = 0;
    for (int read = 0; read < g_bg_job_count; read++) {
        if (g_bg_jobs[read].done) continue;
        if (write != read) {
            g_bg_jobs[write] = g_bg_jobs[read];
        }
        write++;
    }
    g_bg_job_count = write;
}

static void bg_run_next(void)
{
    if (g_bg_running) return;
    bg_remove_done();
    if (g_bg_job_count == 0) return;

    /* Find first queued (not active) job */
    int idx = -1;
    for (int i = 0; i < g_bg_job_count; i++) {
        if (!g_bg_jobs[i].active && !g_bg_jobs[i].done) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return;

    BgJob *job = &g_bg_jobs[idx];
    job->active = 1;
    g_bg_running = 1;

    /* Suppress prompt echo for background execution by calling run_cmd
     * directly — inst_dispatch would print the command again.  If the
     * command contains redirects or pipes, run_cmd won't handle them,
     * so for now we fall back to inst_dispatch and accept the extra echo.
     * A cleaner future approach would be to refactor redirect/pipe logic
     * into a shared helper callable from here. */
    char check[MAX_LINE_LEN];
    scopy(check, job->cmd, MAX_LINE_LEN);
    int has_pipe = 0, has_redir = 0;
    for (int i = 0; check[i]; i++) {
        if (check[i] == '|') has_pipe = 1;
        if (check[i] == '>' || check[i] == '<') has_redir = 1;
    }

    if (!has_pipe && !has_redir) {
        run_cmd(job->shell, job->cmd);
    } else {
        /* Re-parse redirects/pipes via inst_dispatch */
        inst_dispatch(job->shell, job->cmd);
    }

    char done_msg[MAX_LINE_LEN];
    scopy(done_msg, "[", MAX_LINE_LEN);
    char num[8];
    uint_to_dec_s((uint32_t)job->number, num, 8);
    scat(done_msg, num, MAX_LINE_LEN);
    scat(done_msg, "] done", MAX_LINE_LEN);
    inst_print(job->shell, done_msg);

    job->active = 0;
    job->done   = 1;
    g_bg_running = 0;
}

static void pipe_print(void *shell, const char *line)
{
    (void)shell;
    if (g_pipe_buf.count < MAX_PIPE_LINES) {
        scopy(g_pipe_buf.lines[g_pipe_buf.count], line, MAX_LINE_LEN);
        g_pipe_buf.count++;
    }
}

static void pipe_print_raw(void *shell, const char *text)
{
    (void)shell;
    if (g_pipe_buf.count == 0) g_pipe_buf.count = 1;
    int idx = g_pipe_buf.count - 1;
    int cur = slen(g_pipe_buf.lines[idx]);
    int tl = slen(text);
    int i = 0;
    while (i < tl && cur + i < MAX_LINE_LEN - 1) {
        g_pipe_buf.lines[idx][cur + i] = text[i];
        i++;
    }
    g_pipe_buf.lines[idx][cur + i] = '\0';
}

static NativeCmdCtx shell_make_pipe_ctx(ShellInstance *s)
{
    NativeCmdCtx ctx = shell_make_ctx(s);
    ctx.print     = pipe_print;
    ctx.print_raw = pipe_print_raw;
    return ctx;
}

static void write_pipe_to_temp_file(const char *path)
{
    VfsFile fh;
    if (!VFS_Open(&fh, path, VFS_WRITE | VFS_CREATE | VFS_TRUNC)) return;
    for (int i = 0; i < g_pipe_buf.count; i++) {
        VFS_Write(&fh, (const uint8_t *)g_pipe_buf.lines[i], (uint32_t)slen(g_pipe_buf.lines[i]));
        uint8_t nl = '\n';
        VFS_Write(&fh, &nl, 1);
    }
    VFS_Close(&fh);
}

static int parse_pipes(const char *line, char segs[MAX_PIPE_SEGMENTS][MAX_LINE_LEN])
{
    int count = 0;
    const char *p = line;
    while (*p && count < MAX_PIPE_SEGMENTS) {
        /* skip leading spaces before this segment */
        while (*p == ' ') p++;
        int i = 0;
        while (*p && *p != '|' && i < MAX_LINE_LEN - 1) {
            segs[count][i++] = *p++;
        }
        segs[count][i] = '\0';
        /* trim trailing spaces */
        while (i > 0 && segs[count][i - 1] == ' ') {
            segs[count][--i] = '\0';
        }
        count++;
        if (*p == '|') p++;
    }
    return count;
}

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static void inst_print(ShellInstance *s, const char *line)
{
    /* Also mirror to serial port so QEMU -serial stdio captures emu traces */
    _ser_puts(line);
    _ser_putc('\n');

    /* If stdout is redirected, write to file instead of shell history */
    if (g_redir.active) {
        if (g_redir.null) return;
        VFS_Write(&g_redir.fh, (const uint8_t *)line, (uint32_t)slen(line));
        uint8_t nl = '\n';
        VFS_Write(&g_redir.fh, &nl, 1);
        return;
    }
    int slot = s->hist_count % MAX_HIST_LINES;
    scopy(g_hist_buf[s->index][slot], line, MAX_LINE_LEN);
    s->hist_count++;
    s->auto_scroll = 1;
    WM_Redraw();
}

static void inst_cmd_help(ShellInstance *s)
{
    inst_print(s, "UAOS Shell v0.1");
    inst_print(s, "");
    inst_print(s, "Shell built-in commands:");
    inst_print(s, "  help               show this help");
    inst_print(s, "  cd [path]          change/show directory");
    inst_print(s, "  alias [name cmd]   create/list command aliases");
    inst_print(s, "  unalias <name>     remove an alias");
    inst_print(s, "  set [name val]     set/list local environment variable");
    inst_print(s, "  unset <name>       remove local environment variable");
    inst_print(s, "  path [dirs...]     show/set command search path");
    inst_print(s, "");
    inst_print(s, "C: binaries (type 'which <cmd>' to locate):");
    inst_print(s, "  version            OS version info");
    inst_print(s, "  showconfig         show hardware configuration");
    inst_print(s, "  mem                memory information");
    inst_print(s, "  libs               loaded kernel libraries");
    inst_print(s, "  clear              clear the shell window");
    inst_print(s, "  reboot             warm reboot");
    inst_print(s, "  dir [path]         list directory");
    inst_print(s, "  makedir <path>     create directory");
    inst_print(s, "  delete <path>      delete file or empty dir");
    inst_print(s, "  type <file>        print file contents");
    inst_print(s, "  copy <src> <dst>   copy file");
    inst_print(s, "  rename <from> <to> rename/move file");
    inst_print(s, "  pwd                print working directory");
    inst_print(s, "  echo <text>        print text to shell");
    inst_print(s, "  protect <f> <path> set file attributes (+r,-r,+h,-h)");
    inst_print(s, "  attr <path>        show file attributes");
    inst_print(s, "  info [device]      show mounted disks/volumes");
    inst_print(s, "  date               show current date/time");
    inst_print(s, "  which <cmd>        locate a command");
    inst_print(s, "  disks              list detected block devices");
    inst_print(s, "  fdisk <device>     partition a block device");
    inst_print(s, "  format <dev> [fs]  format a partition");
    inst_print(s, "  pointer            open pointer preferences");
    inst_print(s, "  run <prog> [args]  run an embedded Amiga binary");
    inst_print(s, "  assign [name tgt]  create/list assigns (AmigaDOS)");
    inst_print(s, "  mount <dev> [from] mount a handler device");
    inst_print(s, "  execute <script>   run a script file");
    inst_print(s, "  loadwb             launch Workbench desktop");
    inst_print(s, "  ps                 list running tasks");
    inst_print(s, "");
    inst_print(s, "Script flow control:");
    inst_print(s, "  IF <c> THEN <cmd>                    single-line conditional");
    inst_print(s, "  IF <c> ... ELSE ... ENDIF            multi-line conditional");
    inst_print(s, "  IF <c> ... ELSE IF <c> ... ENDIF     chained conditional");
    inst_print(s, "  FOR <v>=<a> TO <b> ... ENDFOR        numeric loop");
    inst_print(s, "  LAB <name>                           define script label");
    inst_print(s, "  SKIP <label|n> [BACK]                jump to label or skip lines");
    inst_print(s, "  Conditions: EXISTS <file>, <a> EQ <b>, <a> NE <b>, NOT <c>");
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

static void inst_cmd_showconfig(ShellInstance *s)
{
    char line[MAX_LINE_LEN];
    char num[16];

    /* PROCESSOR */
    inst_print(s, "PROCESSOR:    CPU x86_64 (64-bit, long mode)");

    /* CUSTOM CHIPS — equivalent: our display/input subsystem */
    scopy(line, "DISPLAY:      ", MAX_LINE_LEN);
    uint_to_dec_s(g_fb.width,  num, 16); scat(line, num, MAX_LINE_LEN);
    scat(line, "x", MAX_LINE_LEN);
    uint_to_dec_s(g_fb.height, num, 16); scat(line, num, MAX_LINE_LEN);
    scat(line, ", ", MAX_LINE_LEN);
    uint_to_dec_s(g_fb.bpp,    num, 16); scat(line, num, MAX_LINE_LEN);
    scat(line, "bpp linear framebuffer (VirtIO VGA)", MAX_LINE_LEN);
    inst_print(s, line);

    /* VERSION */
    inst_print(s, "VERS:         UAOS v0.1.0-dev, Kernel build 1, Exec 1.0");

    /* RAM — describe regions in AmigaDOS ShowConfig style */
    inst_print(s, "RAM:");
    inst_print(s, "      Node type $A, Attributes $005 (FAST), at $0000000-$1FFFFFFF (512.0 meg)");
    inst_print(s, "      Node type $A, Attributes $703 (CHIP), at $0000000-$000FFFFF (~1.0 meg)");

    /* BOARDS / expansion — list hardware we know about */
    inst_print(s, "BOARDS:");
    inst_print(s, "  Board (UEFI GOP framebuffer):  VirtIO VGA, linear");
    scopy(line, "  Board (PS/2 controller):       IRQ1 keyboard, IRQ12 mouse", MAX_LINE_LEN);
    inst_print(s, line);
    inst_print(s, "  Board (APIC/PIC):              8259A-compat PIC, APIC mapped $FEE00000");
    inst_print(s, "  Board (PIT timer):             8253/8254, 10 Hz, IRQ0");
    inst_print(s, "  Board (UART):                  16550A COM1 $3F8, IRQ4");
    inst_print(s, "  Board (RTC):                   MC146818 CMOS RTC, IRQ8");
}

static void inst_cmd_mem(ShellInstance *s)
{
    inst_print(s, "RAM:  512 MB (QEMU)");
    inst_print(s, "Kernel load: 0x0000000000100000");
    inst_print(s, "Framebuffer: mapped (GOP physical address)");
    inst_print(s, "Stack: 16 KB (bootstrap), no heap allocator yet");
}

static void inst_cmd_libs(ShellInstance *s)
{
    char *names[64];
    uint16_t versions[64];
    int count = UAOS_ROM_ListAll(names, versions, 64);
    
    if (count == 0) {
        inst_print(s, "No kernel libraries loaded.");
        return;
    }
    
    char hdr[MAX_LINE_LEN];
    scopy(hdr, "Loaded kernel libraries (", MAX_LINE_LEN);
    char num[12];
    uint_to_dec_s(count, num, 12);
    scat(hdr, num, MAX_LINE_LEN);
    scat(hdr, "):", MAX_LINE_LEN);
    inst_print(s, hdr);
    
    for (int i = 0; i < count; i++) {
        char line[MAX_LINE_LEN];
        scopy(line, "  ", MAX_LINE_LEN);
        scat(line, names[i], MAX_LINE_LEN);
        scat(line, " v", MAX_LINE_LEN);
        uint_to_dec_s(versions[i], num, 12);
        scat(line, num, MAX_LINE_LEN);
        inst_print(s, line);
    }
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

/* =========================================================================
 * Path helpers
 * ========================================================================= */

/* Build an absolute VFS path from cwd + user-supplied arg.
 *
 * AmigaDOS-style rules:
 *   NAME:...       absolute volume reference
 *   :              root of current volume
 *   :dir           relative to root of current volume
 *   /              parent directory (one level up)
 *   //             two levels up
 *   /foo           parent directory, then into "foo"
 *   foo            relative to cwd
 */
static void make_abs_path(ShellInstance *s, const char *arg,
                           char *out, int max)
{
    if (!arg || !*arg) {
        scopy(out, s->cwd, max);
        return;
    }

    /* Absolute volume reference: NAME:... (NAME is non-empty) */
    if (arg[0] != ':' && arg[0] != '/') {
        const char *p = arg;
        while (*p && *p != ':') p++;
        if (*p == ':') {
            scopy(out, arg, max);
            return;
        }
    }

    /* Root-relative on current volume: ":" or ":dir" */
    if (arg[0] == ':') {
        const char *colon = s->cwd;
        while (*colon && *colon != ':') colon++;
        int vol_len = (int)(colon - s->cwd) + 1; /* include ':' */
        if (vol_len >= max) vol_len = max - 1;
        int i = 0;
        for (; i < vol_len && i < max - 1; i++) out[i] = s->cwd[i];
        out[i] = '\0';
        scat(out, arg + 1, max);
        return;
    }

    /* Parent navigation: leading "/" goes up N levels, then appends. */
    if (arg[0] == '/') {
        const char *colon = s->cwd;
        while (*colon && *colon != ':') colon++;
        int vol_len = (int)(colon - s->cwd) + 1; /* include ':' */

        int up = 0;
        while (arg[up] == '/') up++;

        scopy(out, s->cwd, max);
        int len = slen(out);

        for (int i = 0; i < up && len > vol_len; i++) {
            while (len > vol_len && out[len - 1] == '/')
                out[--len] = '\0';
            while (len > vol_len && out[len - 1] != '/')
                out[--len] = '\0';
            while (len > vol_len && out[len - 1] == '/')
                out[--len] = '\0';
        }

        const char *rest = arg + up;
        if (*rest) {
            if (len > 0 && out[len - 1] != ':' && len < max - 1) {
                out[len] = '/';
                out[len + 1] = '\0';
                len++;
            }
            scat(out, rest, max);
        }
        return;
    }

    /* Relative: prepend cwd */
    scopy(out, s->cwd, max);
    /* Ensure cwd ends with '/' unless it ends with ':' */
    int cl = slen(out);
    if (cl > 0 && out[cl-1] != ':' && out[cl-1] != '/') {
        if (cl < max - 1) { out[cl] = '/'; out[cl+1] = '\0'; }
    }
    scat(out, arg, max);
}

/* =========================================================================
 * DOS-style shell commands
 * ========================================================================= */

static void inst_cmd_dir(ShellInstance *s, const char *arg)
{
    char path[64];
    if (arg && *arg) make_abs_path(s, arg, path, 64);
    else scopy(path, s->cwd, 64);

    RamFsNode *child = VFS_OpenDir(path);

    /* Print header */
    char hdr[MAX_LINE_LEN];
    scopy(hdr, "Directory of ", MAX_LINE_LEN);
    scat(hdr, path, MAX_LINE_LEN);
    inst_print(s, hdr);
    inst_print(s, "");

    if (!child) {
        inst_print(s, "  (empty or not found)");
        inst_print(s, "");
        return;
    }

    int count = 0;
    while (child) {
        char line[MAX_LINE_LEN];
        if (child->type == RAMFS_TYPE_DIR) {
            scopy(line, "  ", MAX_LINE_LEN);
            scat(line, child->name, MAX_LINE_LEN);
            scat(line, "  (dir)", MAX_LINE_LEN);
        } else {
            char sz[12];
            uint_to_dec_s(child->size, sz, 12);
            scopy(line, "  ", MAX_LINE_LEN);
            scat(line, child->name, MAX_LINE_LEN);
            scat(line, "  ", MAX_LINE_LEN);
            scat(line, sz, MAX_LINE_LEN);
            scat(line, " bytes", MAX_LINE_LEN);
        }
        inst_print(s, line);
        count++;
        child = child->next_sibling;
    }

    inst_print(s, "");
    char summary[MAX_LINE_LEN];
    char cn[8]; uint_to_dec_s((uint32_t)count, cn, 8);
    scopy(summary, cn, MAX_LINE_LEN);
    scat(summary, " item(s)", MAX_LINE_LEN);
    inst_print(s, summary);
}

static void inst_cmd_cd(ShellInstance *s, const char *arg)
{
    if (!arg || !*arg) {
        inst_print(s, s->cwd);
        return;
    }
    char path[64];
    make_abs_path(s, arg, path, 64);

    /* Resolve the node directly — VFS_OpenDir returns first_child which is
     * NULL for empty dirs, so we can't use it to check existence. */
    RamFsNode *node = VFS_ResolveDir(path);
    if (!node) {
        char msg[MAX_LINE_LEN];
        scopy(msg, "Not found: ", MAX_LINE_LEN);
        scat(msg, path, MAX_LINE_LEN);
        inst_print(s, msg);
        return;
    }

    scopy(s->cwd, path, 64);
}

static void inst_cmd_makedir(ShellInstance *s, const char *arg)
{
    if (!arg || !*arg) { inst_print(s, "Usage: makedir <path>"); return; }
    char path[64];
    make_abs_path(s, arg, path, 64);
    int rc = VFS_MkDir(path);
    if (rc == 0) {
        char msg[MAX_LINE_LEN];
        scopy(msg, "Created: ", MAX_LINE_LEN);
        scat(msg, path, MAX_LINE_LEN);
        inst_print(s, msg);
    } else {
        inst_print(s, "Failed to create directory.");
    }
}

static void inst_cmd_delete(ShellInstance *s, const char *arg)
{
    if (!arg || !*arg) { inst_print(s, "Usage: delete <path>"); return; }
    char path[64];
    make_abs_path(s, arg, path, 64);
    int rc = VFS_Delete(path);
    if (rc == 0) {
        char msg[MAX_LINE_LEN];
        scopy(msg, "Deleted: ", MAX_LINE_LEN);
        scat(msg, path, MAX_LINE_LEN);
        inst_print(s, msg);
    } else if (rc == -2) {
        inst_print(s, "Directory not empty.");
    } else {
        inst_print(s, "Not found.");
    }
}

static void inst_cmd_type(ShellInstance *s, const char *arg)
{
    if (!arg || !*arg) { inst_print(s, "Usage: type <file>"); return; }
    char path[64];
    make_abs_path(s, arg, path, 64);

    VfsFile fh;
    if (!VFS_Open(&fh, path, VFS_READ)) {
        char msg[MAX_LINE_LEN];
        scopy(msg, "Cannot open: ", MAX_LINE_LEN);
        scat(msg, path, MAX_LINE_LEN);
        inst_print(s, msg);
        return;
    }

    uint8_t buf[MAX_LINE_LEN];
    uint32_t pos = 0;
    uint32_t size = VFS_Size(&fh);
    while (pos < size) {
        /* Read one line at a time */
        int col = 0;
        while (pos < size && col < MAX_LINE_LEN - 1) {
            uint8_t c;
            if (VFS_Read(&fh, &c, 1) == 0) break;
            pos++;
            if (c == '\n') break;
            if (c != '\r') buf[col++] = c;
        }
        buf[col] = '\0';
        inst_print(s, (char *)buf);
    }
    VFS_Close(&fh);
}

static void inst_cmd_copy(ShellInstance *s, const char *arg)
{
    if (!arg || !*arg) { inst_print(s, "Usage: copy <src> <dst>"); return; }

    /* Split arg into src and dst at first space */
    char src[64], dst[64];
    const char *p = arg;
    int i = 0;
    while (*p && *p != ' ' && i < 63) { src[i++] = *p++; }
    src[i] = '\0';
    while (*p == ' ') p++;
    i = 0;
    while (*p && i < 63) { dst[i++] = *p++; }
    dst[i] = '\0';

    if (!src[0] || !dst[0]) { inst_print(s, "Usage: copy <src> <dst>"); return; }

    char abs_src[64], abs_dst[64];
    make_abs_path(s, src, abs_src, 64);
    make_abs_path(s, dst, abs_dst, 64);

    VfsFile fsrc;
    if (!VFS_Open(&fsrc, abs_src, VFS_READ)) {
        char msg[MAX_LINE_LEN];
        scopy(msg, "Cannot open source: ", MAX_LINE_LEN);
        scat(msg, abs_src, MAX_LINE_LEN);
        inst_print(s, msg);
        return;
    }

    VfsFile fdst;
    if (!VFS_Open(&fdst, abs_dst, VFS_WRITE)) {
        char msg[MAX_LINE_LEN];
        scopy(msg, "Cannot open destination: ", MAX_LINE_LEN);
        scat(msg, abs_dst, MAX_LINE_LEN);
        inst_print(s, msg);
        VFS_Close(&fsrc);
        return;
    }

    char buf[256];
    int total = 0;
    while (1) {
        int n = (int)VFS_Read(&fsrc, (uint8_t *)buf, 256);
        if (n <= 0) break;
        VFS_Write(&fdst, (const uint8_t *)buf, (uint32_t)n);
        total += n;
    }

    VFS_Close(&fsrc);
    VFS_Close(&fdst);

    char msg[MAX_LINE_LEN];
    scopy(msg, "Copied ", MAX_LINE_LEN);
    uint_to_dec_s(total, msg + slen(msg), 12);
    scat(msg, " bytes", MAX_LINE_LEN);
    inst_print(s, msg);
}

static void inst_cmd_protect(ShellInstance *s, const char *arg)
{
    if (!arg || !*arg) { inst_print(s, "Usage: protect [+r|-r][+h|-h] <path>"); return; }

    /* Parse flags: +r, -r, +h, -h */
    uint8_t new_attrs = 0;
    uint8_t clear_mask = 0;
    const char *p = arg;

    while (*p && (*p == '+' || *p == '-')) {
        char op = *p++;
        char flag = *p++;
        if (flag == 'r') {
            if (op == '+') new_attrs |= RAMFS_ATTR_READONLY;
            else clear_mask |= RAMFS_ATTR_READONLY;
        } else if (flag == 'h') {
            if (op == '+') new_attrs |= RAMFS_ATTR_HIDDEN;
            else clear_mask |= RAMFS_ATTR_HIDDEN;
        } else {
            inst_print(s, "Invalid flag. Use: +r, -r, +h, -h");
            return;
        }
        while (*p == ' ') p++;
    }

    /* Skip to path */
    while (*p == ' ') p++;
    if (!*p) { inst_print(s, "Usage: protect [+r|-r][+h|-h] <path>"); return; }

    char path[64];
    int i = 0;
    while (*p && i < 63) { path[i++] = *p++; }
    path[i] = '\0';

    char abs_path[64];
    make_abs_path(s, path, abs_path, 64);

    /* Get current attributes */
    uint8_t current = VFS_GetAttrs(abs_path);
    if (current == 0 && VFS_ResolveDir(abs_path) == NULL) {
        /* Check if file exists */
        VfsFile test;
        if (!VFS_Open(&test, abs_path, VFS_READ)) {
            char msg[MAX_LINE_LEN];
            scopy(msg, "File not found: ", MAX_LINE_LEN);
            scat(msg, abs_path, MAX_LINE_LEN);
            inst_print(s, msg);
            return;
        }
        VFS_Close(&test);
    }

    /* Apply changes */
    uint8_t final = (current & ~clear_mask) | new_attrs;
    if (VFS_SetAttrs(abs_path, final) == 0) {
        inst_print(s, "Attributes updated");
    } else {
        inst_print(s, "Failed to set attributes");
    }
}

static void inst_cmd_attr(ShellInstance *s, const char *arg)
{
    if (!arg || !*arg) { inst_print(s, "Usage: attr <path>"); return; }

    char path[64];
    int i = 0;
    while (*arg && *arg != ' ' && i < 63) { path[i++] = *arg++; }
    path[i] = '\0';

    char abs_path[64];
    make_abs_path(s, path, abs_path, 64);

    uint8_t attrs = VFS_GetAttrs(abs_path);
    if (attrs == 0) {
        /* Check if path exists */
        VfsFile test;
        if (!VFS_Open(&test, abs_path, VFS_READ)) {
            RamFsNode *dir = VFS_ResolveDir(abs_path);
            if (!dir) {
                char msg[MAX_LINE_LEN];
                scopy(msg, "Not found: ", MAX_LINE_LEN);
                scat(msg, abs_path, MAX_LINE_LEN);
                inst_print(s, msg);
                return;
            }
            attrs = RamFS_GetAttrs(dir);
        } else {
            VFS_Close(&test);
        }
    }

    char msg[MAX_LINE_LEN];
    scopy(msg, "Attributes: ", MAX_LINE_LEN);
    if (attrs & RAMFS_ATTR_READONLY) scat(msg, "Read-Only ", MAX_LINE_LEN);
    if (attrs & RAMFS_ATTR_HIDDEN) scat(msg, "Hidden ", MAX_LINE_LEN);
    if (attrs == 0) scat(msg, "None", MAX_LINE_LEN);
    inst_print(s, msg);
}

/* Append src to dst[max], then pad with spaces up to width.
 * Truncates if src is longer than width. */
static void pad_field(char *dst, const char *src, int max, int width)
{
    int dl = slen(dst);
    int si = 0;
    while (si < width && dl < max - 1 && src[si]) {
        dst[dl++] = src[si++];
    }
    while (si++ < width && dl < max - 1) {
        dst[dl++] = ' ';
    }
    dst[dl] = '\0';
}

static void format_cap(uint64_t bytes, char *out, int max)
{
    if (bytes < 1024) {
        uint_to_dec_s((uint32_t)bytes, out, max);
        scat(out, "B", max);
    } else if (bytes < 1024 * 1024) {
        uint_to_dec_s((uint32_t)(bytes / 1024), out, max);
        scat(out, "K", max);
    } else if (bytes < 1024ULL * 1024 * 1024) {
        uint_to_dec_s((uint32_t)(bytes / (1024 * 1024)), out, max);
        scat(out, "M", max);
    } else {
        uint_to_dec_s((uint32_t)(bytes / (1024ULL * 1024 * 1024)), out, max);
        scat(out, "G", max);
    }
}

static void inst_cmd_info(ShellInstance *s, const char *arg)
{
    if (arg && *arg) {
        /* Info for a specific device */
        char devname[32] = {0};
        int i = 0;
        while (*arg && *arg != ' ' && i < 31) { devname[i++] = *arg++; }
        devname[i] = '\0';

        /* Handle RAM: special case */
        if (seq(devname, "RAM") || seq(devname, "RAM:")) {
            inst_print(s, "Unit: RAM:");
            inst_print(s, "Size: Dynamic");
            inst_print(s, "Status: Read/Write");
            return;
        }

        BlockDev *dev = BlockDev_Find(devname);
        if (!dev) {
            BlockDev *all = BlockDev_GetList();
            while (all) {
                if (all->display_name && seq(all->display_name, devname)) {
                    dev = all; break;
                }
                all = all->next;
            }
        }
        if (!dev) {
            char msg[MAX_LINE_LEN];
            scopy(msg, "Device not found: ", MAX_LINE_LEN);
            scat(msg, devname, MAX_LINE_LEN);
            inst_print(s, msg);
            return;
        }

        char msg[MAX_LINE_LEN];
        scopy(msg, "Unit: ", MAX_LINE_LEN);
        scat(msg, dev->display_name ? dev->display_name : dev->name, MAX_LINE_LEN);
        inst_print(s, msg);

        uint64_t cap = BlockDev_GetCapacity(dev);
        uint64_t bytes = cap * dev->sector_size;
        char sz[16]; sz[0] = '\0';
        format_cap(bytes, sz, 16);

        scopy(msg, "Size: ", MAX_LINE_LEN);
        scat(msg, sz, MAX_LINE_LEN);
        inst_print(s, msg);

        scopy(msg, "Status: Read/Write", MAX_LINE_LEN);
        inst_print(s, msg);
        return;
    }

    /* Show all mounted disks */
    inst_print(s, "Mounted disks:");
    inst_print(s, "Unit      Size       Used       Free      Full  Errs Status        Name");

    BlockDev *dev = BlockDev_GetList();
    while (dev) {
        if (dev->part_offset != 0) {
            uint64_t cap = BlockDev_GetCapacity(dev);
            uint64_t bytes = cap * dev->sector_size;
            char sz[16]; sz[0] = '\0';
            format_cap(bytes, sz, 16);

            const char *name = dev->display_name ? dev->display_name : dev->name;
            char vol_label[16] = {0};
            BlockDev_ReadVolLabel(dev, vol_label, sizeof(vol_label));
            const char *vol_name = vol_label[0] ? vol_label : name;

            char line[MAX_LINE_LEN];
            line[0] = '\0';
            pad_field(line, name,         MAX_LINE_LEN, 10);
            pad_field(line, sz,           MAX_LINE_LEN, 11);
            pad_field(line, "0",          MAX_LINE_LEN, 11);
            pad_field(line, sz,           MAX_LINE_LEN, 11);
            pad_field(line, "0%",         MAX_LINE_LEN, 6);
            pad_field(line, "0",          MAX_LINE_LEN, 5);
            pad_field(line, "Read/Write", MAX_LINE_LEN, 14);
            pad_field(line, vol_name,     MAX_LINE_LEN, 10);
            inst_print(s, line);
        }
        dev = dev->next;
    }

    /* RAM: pseudo-entry */
    {
        char line[MAX_LINE_LEN];
        line[0] = '\0';
        pad_field(line, "RAM:",      MAX_LINE_LEN, 10);
        pad_field(line, "Dynamic",   MAX_LINE_LEN, 11);
        pad_field(line, "0",         MAX_LINE_LEN, 11);
        pad_field(line, "—",         MAX_LINE_LEN, 11);
        pad_field(line, "0%",        MAX_LINE_LEN, 6);
        pad_field(line, "0",         MAX_LINE_LEN, 5);
        pad_field(line, "Read/Write",MAX_LINE_LEN, 14);
        pad_field(line, "RAM",       MAX_LINE_LEN, 10);
        inst_print(s, line);
    }

    inst_print(s, "");
    inst_print(s, "Volumes available:");
    int n = VFS_GetMountCount();
    for (int i = 0; i < n; i++) {
        char vol[16];
        if (VFS_GetMountName(i, vol, sizeof(vol))) {
            char line[MAX_LINE_LEN];
            scopy(line, vol, MAX_LINE_LEN);
            scat(line, ": [Mounted]", MAX_LINE_LEN);
            inst_print(s, line);
        }
    }
}

static void inst_cmd_alias(ShellInstance *s, const char *arg)
{
    if (!arg || !*arg) {
        /* List all aliases */
        if (s->alias_count == 0) {
            inst_print(s, "No aliases defined");
            return;
        }
        for (int i = 0; i < s->alias_count; i++) {
            char line[MAX_LINE_LEN];
            scopy(line, s->alias_names[i], MAX_LINE_LEN);
            scat(line, " = ", MAX_LINE_LEN);
            scat(line, s->alias_values[i], MAX_LINE_LEN);
            inst_print(s, line);
        }
        return;
    }

    /* Parse: alias name value */
    char name[32];
    int i = 0;
    while (*arg && *arg != ' ' && i < 31) { name[i++] = *arg++; }
    name[i] = '\0';
    while (*arg == ' ') arg++;

    if (!name[0]) { inst_print(s, "Usage: alias <name> <command>"); return; }
    if (!*arg) { inst_print(s, "Usage: alias <name> <command>"); return; }

    /* Check if alias already exists */
    for (int i = 0; i < s->alias_count; i++) {
        if (seq(s->alias_names[i], name)) {
            scopy(s->alias_values[i], arg, MAX_ALIAS_LEN);
            inst_print(s, "Alias updated");
            return;
        }
    }

    /* Add new alias */
    if (s->alias_count >= MAX_ALIASES) {
        inst_print(s, "Alias limit reached");
        return;
    }
    scopy(s->alias_names[s->alias_count], name, 32);
    scopy(s->alias_values[s->alias_count], arg, MAX_ALIAS_LEN);
    s->alias_count++;
    inst_print(s, "Alias added");
}

static void inst_cmd_unalias(ShellInstance *s, const char *arg)
{
    if (!arg || !*arg) { inst_print(s, "Usage: unalias <name>"); return; }

    char name[32];
    int i = 0;
    while (*arg && *arg != ' ' && i < 31) { name[i++] = *arg++; }
    name[i] = '\0';

    for (int i = 0; i < s->alias_count; i++) {
        if (seq(s->alias_names[i], name)) {
            /* Shift remaining aliases down */
            for (int j = i; j < s->alias_count - 1; j++) {
                scopy(s->alias_names[j], s->alias_names[j + 1], 32);
                scopy(s->alias_values[j], s->alias_values[j + 1], MAX_ALIAS_LEN);
            }
            s->alias_count--;
            inst_print(s, "Alias removed");
            return;
        }
    }
    inst_print(s, "Alias not found");
}

static void inst_cmd_set(ShellInstance *s, const char *arg)
{
    if (!arg || !*arg) {
        for (int i = 0; i < s->env_count; i++) {
            char line[MAX_LINE_LEN];
            /* Name padded to 16 chars, then value */
            int ni = 0;
            while (s->env_names[i][ni]) { line[ni] = s->env_names[i][ni]; ni++; }
            while (ni < 16) line[ni++] = ' ';
            line[ni] = '\0';
            scat(line, s->env_values[i], MAX_LINE_LEN);
            inst_print(s, line);
        }
        return;
    }

    /* Parse: Set <name> [<value>] — value may be empty */
    char name[MAX_ENV_NAME];
    int i = 0;
    while (*arg && *arg != ' ' && i < MAX_ENV_NAME - 1) { name[i++] = *arg++; }
    name[i] = '\0';
    while (*arg == ' ') arg++;

    if (!name[0]) { inst_print(s, "Usage: Set <name> [<value>]"); return; }

    /* Check if env var already exists — update silently */
    for (int i = 0; i < s->env_count; i++) {
        if (seq_ci(s->env_names[i], name)) {
            scopy(s->env_values[i], arg, MAX_ENV_VAL);
            return;
        }
    }

    /* Add new env var — silently */
    if (s->env_count >= MAX_ENV_VARS) {
        inst_print(s, "Too many variables");
        return;
    }
    scopy(s->env_names[s->env_count], name, MAX_ENV_NAME);
    scopy(s->env_values[s->env_count], arg, MAX_ENV_VAL);
    s->env_count++;
}

static void inst_cmd_unset(ShellInstance *s, const char *arg)
{
    if (!arg || !*arg) {
        /* List local vars same as Set with no args */
        for (int i = 0; i < s->env_count; i++) {
            char line[MAX_LINE_LEN];
            int ni = 0;
            while (s->env_names[i][ni]) { line[ni] = s->env_names[i][ni]; ni++; }
            while (ni < 16) line[ni++] = ' ';
            line[ni] = '\0';
            scat(line, s->env_values[i], MAX_LINE_LEN);
            inst_print(s, line);
        }
        return;
    }

    char name[MAX_ENV_NAME];
    int i = 0;
    while (*arg && *arg != ' ' && i < MAX_ENV_NAME - 1) { name[i++] = *arg++; }
    name[i] = '\0';

    for (int i = 0; i < s->env_count; i++) {
        if (seq_ci(s->env_names[i], name)) {
            for (int j = i; j < s->env_count - 1; j++) {
                scopy(s->env_names[j], s->env_names[j + 1], MAX_ENV_NAME);
                scopy(s->env_values[j], s->env_values[j + 1], MAX_ENV_VAL);
            }
            s->env_count--;
            return;
        }
    }
}

/* SetEnv <name> <value> — set global env var (local + ENV: file) */
static void inst_cmd_setenv(ShellInstance *s, const char *arg)
{
    if (!arg || !*arg) {
        /* List names of all ENV: variables (read ENV: directory) */
        RamFsNode *child = VFS_OpenDir("ENV:");
        while (child) {
            if (child->type != RAMFS_TYPE_DIR)
                inst_print(s, child->name);
            child = child->next_sibling;
        }
        return;
    }

    char name[MAX_ENV_NAME];
    int i = 0;
    while (*arg && *arg != ' ' && i < MAX_ENV_NAME - 1) { name[i++] = *arg++; }
    name[i] = '\0';
    while (*arg == ' ') arg++;

    if (!name[0]) { inst_print(s, "Usage: SetEnv <name> <value>"); return; }

    /* Update or insert in local env store */
    int found = 0;
    for (int j = 0; j < s->env_count; j++) {
        if (seq_ci(s->env_names[j], name)) {
            scopy(s->env_values[j], arg, MAX_ENV_VAL);
            found = 1;
            break;
        }
    }
    if (!found) {
        if (s->env_count >= MAX_ENV_VARS) {
            inst_print(s, "Environment variable limit reached");
            return;
        }
        scopy(s->env_names[s->env_count], name, MAX_ENV_NAME);
        scopy(s->env_values[s->env_count], arg, MAX_ENV_VAL);
        s->env_count++;
    }

    /* Write value to ENV:<name> file */
    char env_path[64];
    scopy(env_path, "ENV:", 64);
    scat(env_path, name, 64);
    VfsFile fh;
    if (VFS_Open(&fh, env_path, VFS_WRITE | VFS_CREATE | VFS_TRUNC)) {
        VFS_Write(&fh, (const uint8_t *)arg, slen(arg));
        VFS_Close(&fh);
    }
}

/* UnSet <name> — remove global env var (local + ENV: file) */
static void inst_cmd_unsetenv(ShellInstance *s, const char *arg)
{
    if (!arg || !*arg) { inst_print(s, "Usage: UnSet <name>"); return; }

    char name[MAX_ENV_NAME];
    int i = 0;
    while (*arg && *arg != ' ' && i < MAX_ENV_NAME - 1) { name[i++] = *arg++; }
    name[i] = '\0';

    /* Remove from local env store */
    for (int j = 0; j < s->env_count; j++) {
        if (seq_ci(s->env_names[j], name)) {
            for (int k = j; k < s->env_count - 1; k++) {
                scopy(s->env_names[k], s->env_names[k + 1], MAX_ENV_NAME);
                scopy(s->env_values[k], s->env_values[k + 1], MAX_ENV_VAL);
            }
            s->env_count--;
            break;
        }
    }

    /* Delete ENV:<name> file */
    char env_path[64];
    scopy(env_path, "ENV:", 64);
    scat(env_path, name, 64);
    VFS_Delete(env_path);
}

static void inst_cmd_rename(ShellInstance *s, const char *arg)
{
    (void)s; (void)arg;
    inst_print(s, "Rename not yet implemented - use copy and delete");
}

static void inst_cmd_date(ShellInstance *s, const char *arg)
{
    (void)arg; /* TODO: support setting date */
    /* UAOS doesn't have a real-time clock yet, display build date */
    inst_print(s, "Ultimate Amiga OS - Build Date: 2026");
    inst_print(s, "Note: Real-time clock not yet implemented");
}

static void inst_cmd_which(ShellInstance *s, const char *arg)
{
    /* Delegate to the native C:which binary */
    NativeCmdCtx nctx = shell_make_ctx(s);
    NativeCmd_Run("which", &nctx, arg ? arg : "");
}

static void inst_cmd_assign(ShellInstance *s, const char *arg)
{
    if (!arg || !*arg) {
        /* List current assigns */
        char buf[512];
        int n = VFS_ListAssigns(buf, sizeof(buf));
        if (n > 0) {
            inst_print(s, "Current assigns:");
            /* Parse lines and print individually */
            const char *p = buf;
            char line[64];
            int li = 0;
            while (*p && li < (int)sizeof(line) - 1) {
                if (*p == '\n') {
                    line[li] = '\0';
                    if (li > 0) inst_print(s, line);
                    li = 0;
                } else {
                    line[li++] = *p;
                }
                p++;
            }
            if (li > 0) {
                line[li] = '\0';
                inst_print(s, line);
            }
        } else {
            inst_print(s, "No assigns defined.");
        }
        inst_print(s, "");
        inst_print(s, "Usage: assign <name>: <target> [ADD | DEFER]");
        inst_print(s, "Example: assign C: Workbench:C");
        return;
    }

    /* Parse assign name and target */
    char name[32];
    char target[64];
    const char *p = arg;

    /* Skip leading spaces */
    while (*p == ' ') p++;

    /* Extract assign name (e.g., "C:") */
    int ni = 0;
    while (*p && *p != ' ' && ni < 31) {
        name[ni++] = *p++;
    }
    name[ni] = '\0';

    /* Skip spaces */
    while (*p == ' ') p++;

    /* Check for optional "TO" keyword (AmigaDOS style) */
    if ((name[0] == 'T' || name[0] == 't') &&
        (name[1] == 'O' || name[1] == 'o') &&
        name[2] == '\0') {
        /* "TO" was specified, re-read the actual name */
        ni = 0;
        while (*p && *p != ' ' && ni < 31) {
            name[ni++] = *p++;
        }
        name[ni] = '\0';
        while (*p == ' ') p++;
    }

    /* Extract target (stop before ADD / DEFER) */
    int ti = 0;
    while (*p && *p != ' ' && ti < 63) {
        target[ti++] = *p++;
    }
    target[ti] = '\0';

    while (*p == ' ') p++;

    /* Check for ADD / DEFER keywords */
    int add = 0;
    int defer = 0;
    if ((p[0] == 'A' || p[0] == 'a') &&
        (p[1] == 'D' || p[1] == 'd') &&
        (p[2] == 'D' || p[2] == 'd') &&
        (p[3] == '\0' || p[3] == ' ')) {
        add = 1;
        p += 3;
        while (*p == ' ') p++;
    }
    if ((p[0] == 'D' || p[0] == 'd') &&
        (p[1] == 'E' || p[1] == 'e') &&
        (p[2] == 'F' || p[2] == 'f') &&
        (p[3] == 'E' || p[3] == 'e') &&
        (p[4] == 'R' || p[4] == 'r') &&
        (p[5] == '\0' || p[5] == ' ')) {
        defer = 1;
    }

    if (!name[0] || !target[0]) {
        inst_print(s, "Usage: assign <name>: <target> [ADD | DEFER]");
        inst_print(s, "Example: assign C: Workbench:C");
        return;
    }

    /* Add the assign */
    if (VFS_AddAssign(name, target, add, defer) == 0) {
        char msg[MAX_LINE_LEN];
        scopy(msg, add ? "Added " : "Assigned ", MAX_LINE_LEN);
        scat(msg, name, MAX_LINE_LEN);
        scat(msg, " -> ", MAX_LINE_LEN);
        scat(msg, target, MAX_LINE_LEN);
        inst_print(s, msg);
    } else {
        inst_print(s, "Failed to create assign.");
    }
}

/* Forward declaration — defined below after run_cmd */
static void expand_vars(ShellInstance *s, const char *src, char *dst, int max);

/* -------------------------------------------------------------------------
 * Script flow-control runner
 * ------------------------------------------------------------------------- */
#define MAX_SCRIPT_SIZE   4096
#define MAX_SCRIPT_NEST   4
#define MAX_SCRIPT_LINES  128

static char g_script_buf[MAX_SCRIPT_NEST][MAX_SCRIPT_SIZE];
static int  g_script_nest_level = 0;

#define MAX_SCRIPT_LABELS 32
static struct {
    char name[16];
    int pc;
} g_script_labels[MAX_SCRIPT_LABELS];
static int g_script_label_count = 0;

static char *script_acquire_buf(void)
{
    if (g_script_nest_level >= MAX_SCRIPT_NEST) return NULL;
    return g_script_buf[g_script_nest_level++];
}

static void script_release_buf(void)
{
    if (g_script_nest_level > 0) g_script_nest_level--;
}

static const char *script_skip_sp(const char *p)
{
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* Case-insensitive keyword match; kw must be lower-case. */
static int script_kw_match(const char *line, const char *kw)
{
    const char *p = script_skip_sp(line);
    const char *k = kw;
    while (*k) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c != *k) return 0;
        p++; k++;
    }
    char c = *p;
    if (c && c != ' ' && c != '\t') return 0;
    return 1;
}

static int script_parse_int(const char *p, int *out)
{
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    if (*p < '0' || *p > '9') return 0;
    int v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        p++;
    }
    *out = neg ? -v : v;
    return 1;
}

static void script_set_var(ShellInstance *s, const char *name, const char *value)
{
    int found = 0;
    for (int j = 0; j < s->env_count; j++) {
        if (seq_ci(s->env_names[j], name)) {
            scopy(s->env_values[j], value, MAX_ENV_VAL);
            found = 1;
            break;
        }
    }
    if (!found) {
        if (s->env_count < MAX_ENV_VARS) {
            scopy(s->env_names[s->env_count], name, MAX_ENV_NAME);
            scopy(s->env_values[s->env_count], value, MAX_ENV_VAL);
            s->env_count++;
        }
    }
}

static int script_exists(const char *path)
{
    VfsFile fh;
    if (VFS_Open(&fh, path, VFS_READ)) {
        VFS_Close(&fh);
        return 1;
    }
    return 0;
}

/* Evaluate a condition string (text after "IF "). Returns 1=true, 0=false. */
static int script_eval_cond(ShellInstance *s, const char *cond)
{
    /* Expand $variables in the condition first */
    char expanded[MAX_LINE_LEN];
    expand_vars(s, cond, expanded, MAX_LINE_LEN);
    cond = script_skip_sp(expanded);
    if (!*cond) return 0;

    /* NOT <condition> */
    if (script_kw_match(cond, "not")) {
        const char *p = script_skip_sp(cond + 3);
        return !script_eval_cond(s, p);
    }

    /* EXISTS <path> */
    if (script_kw_match(cond, "exists")) {
        const char *p = script_skip_sp(cond + 6);
        char path[64];
        make_abs_path(s, p, path, 64);
        return script_exists(path);
    }

    /* <left> EQ <right>  or  <left> NE <right> */
    const char *lp = cond;
    while (*lp && *lp != ' ' && *lp != '\t') lp++;
    int left_len = lp - cond;
    if (left_len <= 0) return 0;
    lp = script_skip_sp(lp);

    int is_eq = script_kw_match(lp, "eq");
    int is_ne = script_kw_match(lp, "ne");
    if (is_eq || is_ne) {
        const char *rp = script_skip_sp(lp + 2);
        const char *re = rp;
        while (*re && *re != ' ' && *re != '\t') re++;
        int right_len = re - rp;

        int same = (left_len == right_len);
        if (same) {
            for (int i = 0; i < left_len; i++) {
                char a = cond[i], b = rp[i];
                if (a >= 'A' && a <= 'Z') a += 32;
                if (b >= 'A' && b <= 'Z') b += 32;
                if (a != b) { same = 0; break; }
            }
        }
        return is_eq ? same : !same;
    }

    /* WARN / ERROR / FAIL — check last command return code */
    if (script_kw_match(cond, "warn"))
        return s->last_rc >= 5;
    if (script_kw_match(cond, "error"))
        return s->last_rc >= 10;
    if (script_kw_match(cond, "fail"))
        return s->last_rc >= 20;

    /* Bare word: true if non-empty */
    return *cond ? 1 : 0;
}

static int script_run_line(ShellInstance *s, const char **lines, int line_count, int pc);

static int script_run_block(ShellInstance *s, const char **lines, int line_count, int start, int end)
{
    int pc = start;
    while (pc < end && !s->quit_flag) {
        pc = script_run_line(s, lines, line_count, pc);
    }
    return pc;
}

static int script_run_line(ShellInstance *s, const char **lines, int line_count, int pc)
{
    if (pc >= line_count) return line_count;
    const char *line = lines[pc];
    pc++;

    const char *lp = script_skip_sp(line);
    if (!*lp || *lp == ';' || *lp == '*') return pc;

    /* IF block */
    if (script_kw_match(line, "if")) {
        const char *cond = script_skip_sp(line + 2);

        /* Single-line: IF <cond> THEN <cmd> */
        const char *tp = cond;
        while (*tp && *tp != ' ' && *tp != '\t') tp++;
        tp = script_skip_sp(tp);
        int is_then = (tp[0] == 'T' || tp[0] == 't') &&
                      (tp[1] == 'H' || tp[1] == 'h') &&
                      (tp[2] == 'E' || tp[2] == 'e') &&
                      (tp[3] == 'N' || tp[3] == 'n') &&
                      (tp[4] == ' ' || tp[4] == '\t' || tp[4] == '\0');
        if (is_then) {
            if (script_eval_cond(s, cond)) {
                inst_dispatch(s, script_skip_sp(tp + 4));
            }
            return pc;
        }

        /* Multi-line IF with optional ELSE IF chaining */
        int depth = 1;
        int scan = pc;
        int branch_pc[16];
        const char *branch_cond[16];
        int branch_count = 0;

        branch_pc[0] = pc;
        branch_cond[0] = cond;
        branch_count = 1;

        while (scan < line_count && depth > 0) {
            if (script_kw_match(lines[scan], "if")) {
                depth++;
            } else if (script_kw_match(lines[scan], "else")) {
                if (depth == 1 && branch_count < 16) {
                    const char *after_else = script_skip_sp(lines[scan] + 4);
                    int is_else_if = script_kw_match(after_else, "if");
                    if (!is_else_if && scan + 1 < line_count)
                        is_else_if = script_kw_match(lines[scan + 1], "if");

                    if (is_else_if) {
                        if (script_kw_match(after_else, "if")) {
                            /* IF cond on same line after ELSE */
                            branch_cond[branch_count] = script_skip_sp(after_else + 2);
                            branch_pc[branch_count] = scan + 1;
                            branch_count++;
                        } else {
                            /* IF on next line */
                            branch_cond[branch_count] = script_skip_sp(lines[scan + 1] + 2);
                            branch_pc[branch_count] = scan + 2;
                            branch_count++;
                            scan++; /* skip the IF line so it doesn't increment depth */
                        }
                    } else {
                        /* Simple ELSE */
                        branch_cond[branch_count] = NULL;
                        branch_pc[branch_count] = scan + 1;
                        branch_count++;
                    }
                }
            } else if (script_kw_match(lines[scan], "endif")) {
                depth--;
                if (depth == 0 && branch_count < 16) {
                    branch_pc[branch_count] = scan;
                    branch_cond[branch_count] = NULL;
                    branch_count++;
                }
            }
            scan++;
        }

        int endif_pc = branch_pc[branch_count - 1];
        if (branch_count < 2 || endif_pc < 0) {
            inst_print(s, "Script error: IF without ENDIF");
            return line_count;
        }

        /* Execute the first true branch */
        for (int b = 0; b < branch_count - 1; b++) {
            if (branch_cond[b] == NULL || script_eval_cond(s, branch_cond[b])) {
                script_run_block(s, lines, line_count,
                                 branch_pc[b], branch_pc[b + 1]);
                break;
            }
        }
        return endif_pc + 1;
    }

    /* ELSE / ENDIF / ENDFOR consumed by their block openers */
    if (script_kw_match(line, "else") || script_kw_match(line, "endif") ||
        script_kw_match(line, "endfor")) {
        return pc;
    }

    /* QUIT — abort script execution */
    if (script_kw_match(line, "quit")) {
        s->quit_flag = 1;
        return line_count;
    }

    /* LAB — label marker (no-op at runtime; labels are pre-scanned) */
    if (script_kw_match(line, "lab")) {
        return pc;
    }

    /* SKIP — jump to a label or skip lines */
    if (script_kw_match(line, "skip")) {
        const char *arg = script_skip_sp(line + 4);
        if (!*arg) return pc; /* no arg: skip 1 line forward */

        int backward = 0;
        if (script_kw_match(arg, "back")) {
            backward = 1;
            arg = script_skip_sp(arg + 4);
        }

        int skip_n;
        if (script_parse_int(arg, &skip_n)) {
            if (backward)
                return (pc > skip_n) ? (pc - skip_n) : 0;
            return pc + skip_n;
        }

        /* Label lookup */
        for (int i = 0; i < g_script_label_count; i++) {
            if (seq_ci(arg, g_script_labels[i].name)) {
                return g_script_labels[i].pc + 1;
            }
        }
        inst_print(s, "Script error: SKIP label not found");
        return line_count;
    }

    /* FOR block */
    if (script_kw_match(line, "for")) {
        const char *rest = script_skip_sp(line + 3);
        char varname[MAX_ENV_NAME];
        int vi = 0;
        while (*rest && *rest != ' ' && *rest != '\t' && *rest != '=' && vi < MAX_ENV_NAME - 1)
            varname[vi++] = *rest++;
        varname[vi] = '\0';
        rest = script_skip_sp(rest);
        if (*rest != '=') {
            inst_print(s, "Script error: FOR syntax (expected =)");
            return line_count;
        }
        rest = script_skip_sp(rest + 1);
        int start_val, end_val, step_val = 1;
        if (!script_parse_int(rest, &start_val)) {
            inst_print(s, "Script error: FOR expected numeric start");
            return line_count;
        }
        while (*rest && ((*rest >= '0' && *rest <= '9') || *rest == '-')) rest++;
        rest = script_skip_sp(rest);
        if (!script_kw_match(rest, "to")) {
            inst_print(s, "Script error: FOR syntax (expected TO)");
            return line_count;
        }
        rest = script_skip_sp(rest + 2);
        if (!script_parse_int(rest, &end_val)) {
            inst_print(s, "Script error: FOR expected numeric end");
            return line_count;
        }
        while (*rest && ((*rest >= '0' && *rest <= '9') || *rest == '-')) rest++;
        rest = script_skip_sp(rest);
        if (script_kw_match(rest, "step")) {
            rest = script_skip_sp(rest + 4);
            if (!script_parse_int(rest, &step_val)) {
                inst_print(s, "Script error: FOR expected numeric step");
                return line_count;
            }
            while (*rest && ((*rest >= '0' && *rest <= '9') || *rest == '-')) rest++;
            rest = script_skip_sp(rest);
        }

        /* Find matching ENDFOR */
        int depth = 1;
        int endfor_pc = -1;
        int scan = pc;
        while (scan < line_count && depth > 0) {
            if (script_kw_match(lines[scan], "for")) depth++;
            else if (script_kw_match(lines[scan], "endfor")) {
                depth--;
                if (depth == 0) endfor_pc = scan;
            }
            scan++;
        }
        if (endfor_pc < 0) {
            inst_print(s, "Script error: FOR without ENDFOR");
            return line_count;
        }

        /* Execute loop body */
        for (int v = start_val; (step_val > 0) ? (v <= end_val) : (v >= end_val); v += step_val) {
            char valstr[16];
            int n = v, neg = 0;
            if (n < 0) { neg = 1; n = -n; }
            char tmp[16]; int ti = 0;
            do { tmp[ti++] = '0' + (n % 10); n /= 10; } while (n > 0);
            int di = 0;
            if (neg) valstr[di++] = '-';
            while (ti-- > 0) valstr[di++] = tmp[ti];
            valstr[di] = '\0';

            script_set_var(s, varname, valstr);
            script_run_block(s, lines, line_count, pc, endfor_pc);
        }
        return endfor_pc + 1;
    }

    /* Normal command line */
    inst_dispatch(s, line);
    return pc;
}

static void run_script_text(ShellInstance *s, char *text)
{
    const char *lines[MAX_SCRIPT_LINES];
    int line_count = 0;

    char *p = text;
    while (*p && line_count < MAX_SCRIPT_LINES) {
        lines[line_count++] = p;
        while (*p && *p != '\n' && *p != '\r') p++;
        if (*p == '\r') {
            char *term = p;
            p++;
            if (*p == '\n') p++;
            *term = '\0';
        } else if (*p == '\n') {
            *p = '\0';
            p++;
        }
    }

    s->quit_flag = 0;

    /* Pre-scan for labels */
    g_script_label_count = 0;
    for (int i = 0; i < line_count && g_script_label_count < MAX_SCRIPT_LABELS; i++) {
        if (script_kw_match(lines[i], "lab")) {
            const char *name = script_skip_sp(lines[i] + 3);
            int j = 0;
            while (name[j] && name[j] != ' ' && name[j] != '\t' && j < 15) {
                g_script_labels[g_script_label_count].name[j] = name[j];
                j++;
            }
            g_script_labels[g_script_label_count].name[j] = '\0';
            g_script_labels[g_script_label_count].pc = i;
            g_script_label_count++;
        }
    }

    script_run_block(s, lines, line_count, 0, line_count);
    s->quit_flag = 0;
}

static void shell_run_script(void *shell_extra, const char *text)
{
    char *buf = script_acquire_buf();
    if (!buf) return;
    int len = 0;
    while (text[len] && len < MAX_SCRIPT_SIZE - 1) {
        buf[len] = text[len];
        len++;
    }
    buf[len] = '\0';
    run_script_text((ShellInstance *)shell_extra, buf);
    script_release_buf();
}

static void inst_cmd_execute(ShellInstance *s, const char *arg)
{
    if (!arg || !*arg) {
        inst_print(s, "Usage: execute <script>");
        inst_print(s, "Executes a script file with flow-control support.");
        return;
    }

    char path[64];
    make_abs_path(s, arg, path, 64);

    VfsFile fh;
    if (!VFS_Open(&fh, path, VFS_READ)) {
        char msg[MAX_LINE_LEN];
        scopy(msg, "Cannot open: ", MAX_LINE_LEN);
        scat(msg, path, MAX_LINE_LEN);
        inst_print(s, msg);
        return;
    }

    uint32_t size = VFS_Size(&fh);
    if (size == 0 || size >= MAX_SCRIPT_SIZE) {
        inst_print(s, "Script empty or too large (max 4KB)");
        VFS_Close(&fh);
        return;
    }

    char *buf = script_acquire_buf();
    if (!buf) {
        inst_print(s, "Script nesting too deep");
        VFS_Close(&fh);
        return;
    }

    uint32_t nread = VFS_Read(&fh, (uint8_t *)buf, size);
    buf[nread] = '\0';
    VFS_Close(&fh);

    inst_print(s, "Executing script...");
    run_script_text(s, buf);
    script_release_buf();
    inst_print(s, "Script complete.");
}

static void inst_cmd_path(ShellInstance *s, const char *arg)
{
    /* If no argument, show current path */
    if (!arg || !*arg) {
        inst_print(s, "Current command search path:");
        /* Parse and display each path entry */
        char buf[256];
        scopy(buf, s->path, 256);
        char *p = buf;
        while (*p) {
            /* Skip leading spaces */
            while (*p == ' ') p++;
            if (!*p) break;
            /* Find end of this path entry */
            char entry[64];
            int ei = 0;
            while (*p && *p != ' ' && ei < 63) {
                entry[ei++] = *p++;
            }
            entry[ei] = '\0';
            if (ei > 0) {
                char msg[MAX_LINE_LEN];
                scopy(msg, "  ", MAX_LINE_LEN);
                scat(msg, entry, MAX_LINE_LEN);
                inst_print(s, msg);
            }
        }
        inst_print(s, "");
        inst_print(s, "The shell searches for commands in:");
        inst_print(s, "  1. Built-in commands");
        inst_print(s, "  2. Current directory");
        inst_print(s, "  3. Path directories (in order)");
        return;
    }

    /* Set new path - copy the argument directly */
    scopy(s->path, arg, 256);

    inst_print(s, "Command search path updated.");
}

static void inst_cmd_pointer(ShellInstance *s)
{
    (void)s;
    PointerPrefs_Show();
}

static void inst_cmd_disks(ShellInstance *s, const char *arg)
{
    (void)arg;
    /* List all registered block devices */
    extern BlockDev *BlockDev_Find(const char *name);
    extern uint64_t BlockDev_GetCapacity(BlockDev *dev);
    
    /* We need to iterate through the block device list */
    /* Since BlockDev_Find only finds by name, we'll iterate through known names */
    /* For now, just check for virtio0 */
    BlockDev *dev = BlockDev_Find("virtio0");
    if (dev) {
        char msg[MAX_LINE_LEN];
        scopy(msg, "Device: ", MAX_LINE_LEN);
        scat(msg, dev->name, MAX_LINE_LEN);
        inst_print(s, msg);
        
        uint64_t capacity = BlockDev_GetCapacity(dev);
        uint64_t mb = (capacity * 512) / (1024 * 1024);
        
        scopy(msg, "  Capacity: ", MAX_LINE_LEN);
        uint_to_dec_s(capacity, msg + slen(msg), MAX_LINE_LEN - slen(msg));
        scat(msg, " sectors (", MAX_LINE_LEN);
        uint_to_dec_s(mb, msg + slen(msg), MAX_LINE_LEN - slen(msg));
        scat(msg, " MB)", MAX_LINE_LEN);
        inst_print(s, msg);
        
        scopy(msg, "  Sector size: ", MAX_LINE_LEN);
        uint_to_dec_s(dev->sector_size, msg + slen(msg), MAX_LINE_LEN - slen(msg));
        scat(msg, " bytes", MAX_LINE_LEN);
        inst_print(s, msg);
    } else {
        inst_print(s, "No block devices detected");
    }
}

/* Fdisk interactive command handler */
static void fdisk_handle_cmd(ShellInstance *s, const char *cmd)
{
    char msg[MAX_LINE_LEN];
    PartitionTable *pt = &s->fdisk_pt;

    if (!cmd || !*cmd) {
        inst_print(s, "Command (m for help):");
        return;
    }

    switch (cmd[0]) {
        case 'm':  /* Help */
            inst_print(s, "Partition table editor commands:");
            inst_print(s, "  m   print this menu");
            inst_print(s, "  p   print the partition table");
            inst_print(s, "  n   add a new partition");
            inst_print(s, "  d   delete a partition");
            inst_print(s, "  t   change a partition type");
            inst_print(s, "  N   set partition name (e.g. N 1 DH0:)");
            inst_print(s, "  a   toggle auto-mount flag (a 1)");
            inst_print(s, "  B   toggle bootable flag (B 1)");
            inst_print(s, "  P   set boot priority (P 1 0)");
            inst_print(s, "  w   write table to disk and exit");
            inst_print(s, "  q   quit without saving");
            inst_print(s, "  x   create new partition table");
            inst_print(s, "  g   select GPT scheme");
            inst_print(s, "  r   select RDB (Amiga) scheme");
            inst_print(s, "  b   select MBR scheme");
            break;

        case 'p':  /* Print partition table */
            if (!pt->valid) {
                inst_print(s, "No partition table loaded. Use 'x' to create one.");
            } else {
                partition_print(pt, (void (*)(const char *))inst_print_wrapper);
            }
            break;

        case 'n':  /* New partition */
            if (!pt->valid) {
                inst_print(s, "No partition table. Use 'x' to create one first.");
                break;
            }
            if (pt->scheme != PART_SCHEME_MBR) {
                inst_print(s, "New partition only implemented for MBR.");
                break;
            }
            {
                /* Find first free sector */
                uint32_t start = 2048;
                uint32_t end = (uint32_t)(pt->disk_sectors > 0xFFFFFFFF ? 0xFFFFFFFF : pt->disk_sectors);
                for (int i = 0; i < MBR_PART_COUNT; i++) {
                    if (pt->mbr.partitions[i].type_code != PART_TYPE_EMPTY) {
                        uint32_t part_end = pt->mbr.partitions[i].lba_start + pt->mbr.partitions[i].sector_count;
                        if (part_end > start) start = part_end;
                    }
                }
                if (start >= end) {
                    inst_print(s, "No free space for new partition.");
                    break;
                }
                uint32_t count = end - start;
                if (count > 0xFFFFFFFF) count = 0xFFFFFFFF;

                int idx = mbr_add_partition(pt, start, count, PART_TYPE_LINUX);
                if (idx < 0) {
                    inst_print(s, "Failed to add partition (table full).");
                } else {
                    scopy(msg, "Created partition ", MAX_LINE_LEN);
                    msg[18] = '1' + idx;
                    msg[19] = '\0';
                    scat(msg, " (Linux, ", MAX_LINE_LEN);
                    uint64_t mb = ((uint64_t)count * 512) / (1024 * 1024);
                    uint_to_dec_s((uint32_t)mb, msg + slen(msg), MAX_LINE_LEN - slen(msg));
                    scat(msg, " MB)", MAX_LINE_LEN);
                    inst_print(s, msg);
                }
            }
            break;

        case 'd':  /* Delete partition */
            if (!pt->valid || pt->scheme != PART_SCHEME_MBR) {
                inst_print(s, "Delete only implemented for MBR.");
                break;
            }
            {
                int idx = -1;
                if (cmd[1] == ' ' && cmd[2] >= '1' && cmd[2] <= '4') {
                    idx = cmd[2] - '1';
                } else {
                    inst_print(s, "Usage: d <partition_number> (1-4)");
                    break;
                }
                if (mbr_delete_partition(pt, idx) == 0) {
                    scopy(msg, "Deleted partition ", MAX_LINE_LEN);
                    msg[18] = '1' + idx;
                    msg[19] = '\0';
                    inst_print(s, msg);
                } else {
                    inst_print(s, "Partition not found or already empty.");
                }
            }
            break;

        case 't':  /* Change partition type */
            if (!pt->valid || pt->scheme != PART_SCHEME_MBR) {
                inst_print(s, "Type change only implemented for MBR.");
                break;
            }
            {
                int idx = -1;
                uint8_t type = 0;
                if (cmd[1] == ' ' && cmd[2] >= '1' && cmd[2] <= '4') {
                    idx = cmd[2] - '1';
                    /* Parse hex type from cmd+4 */
                    const char *tp = cmd + 4;
                    while (*tp == ' ') tp++;
                    if (tp[0] && tp[1]) {
                        /* Simple hex parse */
                        int h1 = tp[0], h2 = tp[1];
                        if (h1 >= '0' && h1 <= '9') h1 -= '0';
                        else if (h1 >= 'a' && h1 <= 'f') h1 = h1 - 'a' + 10;
                        else if (h1 >= 'A' && h1 <= 'F') h1 = h1 - 'A' + 10;
                        else h1 = 0;
                        if (h2 >= '0' && h2 <= '9') h2 -= '0';
                        else if (h2 >= 'a' && h2 <= 'f') h2 = h2 - 'a' + 10;
                        else if (h2 >= 'A' && h2 <= 'F') h2 = h2 - 'A' + 10;
                        else h2 = 0;
                        type = (uint8_t)((h1 << 4) | h2);
                    }
                }
                if (idx < 0 || type == 0) {
                    inst_print(s, "Usage: t <part> <hex_type>");
                    inst_print(s, "  Example: t 1 83  (Linux)");
                    inst_print(s, "  Common types: 01=FAT12, 06=FAT16, 0B=FAT32");
                    inst_print(s, "                07=NTFS, 83=Linux, EF=EFI");
                    break;
                }
                if (pt->mbr.partitions[idx].type_code == PART_TYPE_EMPTY) {
                    inst_print(s, "Partition is empty.");
                    break;
                }
                pt->mbr.partitions[idx].type_code = type;
                pt->mbr_modified = 1;
                scopy(msg, "Changed partition ", MAX_LINE_LEN);
                msg[18] = '1' + idx;
                msg[19] = '\0';
                scat(msg, " type to ", MAX_LINE_LEN);
                scat(msg, partition_type_name(type), MAX_LINE_LEN);
                inst_print(s, msg);
            }
            break;

        case 'N':  /* Set partition name */
            if (!pt->valid || pt->scheme != PART_SCHEME_MBR) {
                inst_print(s, "Name set only for MBR.");
                break;
            }
            {
                int idx = -1;
                if (cmd[1] == ' ' && cmd[2] >= '1' && cmd[2] <= '4') {
                    idx = cmd[2] - '1';
                } else {
                    inst_print(s, "Usage: N <part> <name>");
                    inst_print(s, "  Example: N 1 DH0:");
                    break;
                }
                if (pt->mbr.partitions[idx].type_code == PART_TYPE_EMPTY) {
                    inst_print(s, "Partition is empty.");
                    break;
                }
                /* Parse name after partition number */
                const char *np = cmd + 4;
                while (*np == ' ') np++;
                if (!*np) {
                    inst_print(s, "Usage: N <part> <name>");
                    break;
                }
                int ni = 0;
                for (ni = 0; ni < UAOS_PART_MAX_NAME - 1 && np[ni] && np[ni] != ' '; ni++)
                    pt->uaos_meta.parts[idx].name[ni] = np[ni];
                pt->uaos_meta.parts[idx].name[ni] = '\0';
                pt->meta_modified = 1;
                scopy(msg, "Set partition ", MAX_LINE_LEN);
                msg[14] = '1' + idx;
                msg[15] = ' '; msg[16] = 'n'; msg[17] = 'a'; msg[18] = 'm'; msg[19] = 'e'; msg[20] = '='; msg[21] = '\0';
                scat(msg, pt->uaos_meta.parts[idx].name, MAX_LINE_LEN);
                inst_print(s, msg);
            }
            break;

        case 'a':  /* Toggle auto-mount */
            if (!pt->valid || pt->scheme != PART_SCHEME_MBR) {
                inst_print(s, "Auto-mount toggle only for MBR.");
                break;
            }
            {
                int idx = -1;
                if (cmd[1] == ' ' && cmd[2] >= '1' && cmd[2] <= '4') {
                    idx = cmd[2] - '1';
                } else {
                    inst_print(s, "Usage: a <partition_number> (1-4)");
                    break;
                }
                if (pt->mbr.partitions[idx].type_code == PART_TYPE_EMPTY) {
                    inst_print(s, "Partition is empty.");
                    break;
                }
                pt->uaos_meta.parts[idx].automount = !pt->uaos_meta.parts[idx].automount;
                pt->meta_modified = 1;
                scopy(msg, "Partition ", MAX_LINE_LEN);
                msg[10] = '1' + idx;
                msg[11] = ' '; msg[12] = '\0';
                scat(msg, pt->uaos_meta.parts[idx].automount ? "auto-mount ON" : "auto-mount OFF", MAX_LINE_LEN);
                inst_print(s, msg);
            }
            break;

        case 'B':  /* Toggle bootable */
            if (!pt->valid || pt->scheme != PART_SCHEME_MBR) {
                inst_print(s, "Bootable toggle only for MBR.");
                break;
            }
            {
                int idx = -1;
                if (cmd[1] == ' ' && cmd[2] >= '1' && cmd[2] <= '4') {
                    idx = cmd[2] - '1';
                } else {
                    inst_print(s, "Usage: B <partition_number> (1-4)");
                    break;
                }
                if (pt->mbr.partitions[idx].type_code == PART_TYPE_EMPTY) {
                    inst_print(s, "Partition is empty.");
                    break;
                }
                pt->uaos_meta.parts[idx].bootable = !pt->uaos_meta.parts[idx].bootable;
                if (pt->uaos_meta.parts[idx].bootable) {
                    pt->mbr.partitions[idx].boot_flag = 0x80;
                } else {
                    pt->mbr.partitions[idx].boot_flag = 0x00;
                }
                pt->meta_modified = 1;
                pt->mbr_modified = 1;
                scopy(msg, "Partition ", MAX_LINE_LEN);
                msg[10] = '1' + idx;
                msg[11] = ' '; msg[12] = '\0';
                scat(msg, pt->uaos_meta.parts[idx].bootable ? "bootable ON" : "bootable OFF", MAX_LINE_LEN);
                inst_print(s, msg);
            }
            break;

        case 'P':  /* Set boot priority */
            if (!pt->valid || pt->scheme != PART_SCHEME_MBR) {
                inst_print(s, "Priority set only for MBR.");
                break;
            }
            {
                int idx = -1;
                int pri = 0;
                if (cmd[1] == ' ' && cmd[2] >= '1' && cmd[2] <= '4') {
                    idx = cmd[2] - '1';
                    const char *pp = cmd + 4;
                    while (*pp == ' ') pp++;
                    while (*pp >= '0' && *pp <= '9') {
                        pri = pri * 10 + (*pp - '0');
                        pp++;
                        if (pri > 255) pri = 255;
                    }
                } else {
                    inst_print(s, "Usage: P <part> <priority>");
                    inst_print(s, "  Example: P 1 10");
                    break;
                }
                if (pt->mbr.partitions[idx].type_code == PART_TYPE_EMPTY) {
                    inst_print(s, "Partition is empty.");
                    break;
                }
                pt->uaos_meta.parts[idx].boot_pri = (uint8_t)pri;
                pt->meta_modified = 1;
                scopy(msg, "Partition ", MAX_LINE_LEN);
                msg[10] = '1' + idx;
                msg[11] = ' '; msg[12] = 'p'; msg[13] = 'r'; msg[14] = 'i'; msg[15] = '='; msg[16] = '\0';
                char prbuf[8];
                uint_to_dec_s((uint32_t)pri, prbuf, 8);
                scat(msg, prbuf, MAX_LINE_LEN);
                inst_print(s, msg);
            }
            break;

        case 'w':  /* Write to disk */
            if (!pt->valid) {
                inst_print(s, "No partition table to write.");
                break;
            }
            if (!s->fdisk_dev) {
                inst_print(s, "No device selected.");
                break;
            }
            {
                inst_print(s, "Writing partition table...");
                int ret = partition_write(s->fdisk_dev, pt);
                if (ret == 0) {
                    inst_print(s, "Partition table written to disk.");
                    /* Unregister old partitions and register new ones */
                    BlockDev_UnregisterPartitions(s->fdisk_dev);
                    for (int i = 0; i < MBR_PART_COUNT; i++) {
                        if (pt->mbr.partitions[i].type_code != PART_TYPE_EMPTY) {
                            char namebuf[16];
                            const char *dname = uaos_meta_get_name(&pt->uaos_meta, i, namebuf, sizeof(namebuf));
                            BlockDev_RegisterPartition(
                                s->fdisk_dev, i + 1,
                                pt->mbr.partitions[i].lba_start,
                                pt->mbr.partitions[i].sector_count, dname);
                        }
                    }
                    s->fdisk_mode = 0;
                    inst_print(s, "Exiting fdisk.");
                } else {
                    char em[48];
                    int code = ret < 0 ? -ret : ret;
                    scopy(em, "Write failed (code: ", 48);
                    /* Simple int to string */
                    char num[8];
                    int ni = 0;
                    if (code == 0) { num[0] = '0'; ni = 1; }
                    while (code > 0 && ni < 7) { num[ni++] = '0' + (code % 10); code /= 10; }
                    for (int j = 0; j < ni; j++) {
                        int sl = 0;
                        while (em[sl] && sl < 47) sl++;
                        em[sl] = num[ni - 1 - j];
                        em[sl + 1] = '\0';
                    }
                    int sl = 0;
                    while (em[sl] && sl < 47) sl++;
                    em[sl] = ')';
                    em[sl + 1] = '\0';
                    inst_print(s, em);
                }
            }
            break;

        case 'q':  /* Quit without saving */
            if (pt->valid && (pt->mbr_modified || pt->gpt_modified || pt->rdb_modified || pt->meta_modified)) {
                inst_print(s, "WARNING: Unsaved changes will be lost.");
            }
            s->fdisk_mode = 0;
            inst_print(s, "Exiting fdisk.");
            break;

        case 'x':  /* Create new partition table */
            {
                int scheme = PART_SCHEME_MBR;
                if (cmd[1] == ' ' && cmd[2] == 'g') scheme = PART_SCHEME_GPT;
                if (cmd[1] == ' ' && cmd[2] == 'r') scheme = PART_SCHEME_RDB;

                if (scheme == PART_SCHEME_MBR) {
                    mbr_create_new(pt);
                    uaos_meta_init(&pt->uaos_meta);
                    pt->meta_modified = 1;
                    inst_print(s, "Created new MBR partition table.");
                } else if (scheme == PART_SCHEME_GPT) {
                    inst_print(s, "GPT not yet implemented. Using MBR.");
                    mbr_create_new(pt);
                    uaos_meta_init(&pt->uaos_meta);
                    pt->meta_modified = 1;
                    pt->scheme = PART_SCHEME_MBR;
                } else {
                    inst_print(s, "RDB not yet implemented. Using MBR.");
                    mbr_create_new(pt);
                    uaos_meta_init(&pt->uaos_meta);
                    pt->meta_modified = 1;
                    pt->scheme = PART_SCHEME_MBR;
                }
            }
            break;

        case 'g':
            inst_print(s, "Selected GPT scheme (not yet fully implemented).");
            if (!pt->valid) mbr_create_new(pt);
            pt->scheme = PART_SCHEME_GPT;
            break;

        case 'r':
            inst_print(s, "Selected RDB (Amiga) scheme (not yet fully implemented).");
            if (!pt->valid) mbr_create_new(pt);
            pt->scheme = PART_SCHEME_RDB;
            break;

        case 'b':
            inst_print(s, "Selected MBR scheme.");
            if (!pt->valid) mbr_create_new(pt);
            pt->scheme = PART_SCHEME_MBR;
            break;

        default:
            scopy(msg, "Unknown command: '", MAX_LINE_LEN);
            {
                int i = 18;
                int j = 0;
                while (cmd[j] && i < 40 && cmd[j] != ' ') {
                    msg[i++] = cmd[j++];
                }
                msg[i] = '\0';
            }
            scat(msg, "'. Type 'm' for help.", MAX_LINE_LEN);
            inst_print(s, msg);
            break;
    }
}

static void inst_cmd_fdisk(ShellInstance *s, const char *arg)
{
    /* Check for -l option to list disks */
    if (arg && arg[0] == '-' && arg[1] == 'l') {
        inst_print(s, "Available block devices:");
        
        BlockDev *dev = BlockDev_GetList();
        int count = 0;
        while (dev) {
            /* Skip partition devices — only list whole disks */
            if (dev->part_offset != 0) {
                dev = dev->next;
                continue;
            }
            char msg[MAX_LINE_LEN];
            scopy(msg, "  ", MAX_LINE_LEN);
            scat(msg, dev->name, MAX_LINE_LEN);
            scat(msg, " - ", MAX_LINE_LEN);
            
            uint64_t capacity = BlockDev_GetCapacity(dev);
            uint64_t mb = (capacity * 512) / (1024 * 1024);
            uint_to_dec_s(mb, msg + slen(msg), MAX_LINE_LEN - slen(msg));
            scat(msg, " MB", MAX_LINE_LEN);
            
            inst_print(s, msg);
            dev = dev->next;
            count++;
        }
        
        if (count == 0) {
            inst_print(s, "  No block devices found.");
        }
        
        inst_print(s, "");
        inst_print(s, "Usage: fdisk <device>");
        inst_print(s, "       fdisk -l      (list available disks)");
        inst_print(s, "Example: fdisk virtio0");
        return;
    }
    
    if (!arg || !*arg) {
        inst_print(s, "Usage: fdisk <device>");
        inst_print(s, "       fdisk -l      (list available disks)");
        inst_print(s, "Example: fdisk virtio0");
        inst_print(s, "");
        inst_print(s, "Note: Partition operations require disk I/O support.");
        return;
    }

    char devname[32];
    int i = 0;
    while (*arg && *arg != ' ' && i < 31) { devname[i++] = *arg++; }
    devname[i] = '\0';

    BlockDev *dev = BlockDev_Find(devname);
    if (!dev) {
        char msg[MAX_LINE_LEN];
        scopy(msg, "Device not found: ", MAX_LINE_LEN);
        scat(msg, devname, MAX_LINE_LEN);
        inst_print(s, msg);
        inst_print(s, "");
        inst_print(s, "Use 'fdisk -l' to list available devices.");
        return;
    }

    /* Enter fdisk interactive mode */
    s->fdisk_mode = 1;
    s->fdisk_dev = dev;
    memset(&s->fdisk_pt, 0, sizeof(PartitionTable));

    /* Try to read existing partition table */
    if (partition_read(dev, &s->fdisk_pt) == 0) {
        char msg[MAX_LINE_LEN];
        scopy(msg, "Reading partition table from ", MAX_LINE_LEN);
        scat(msg, devname, MAX_LINE_LEN);
        inst_print(s, msg);
    } else {
        char msg[MAX_LINE_LEN];
        scopy(msg, "No valid partition table on ", MAX_LINE_LEN);
        scat(msg, devname, MAX_LINE_LEN);
        inst_print(s, msg);
        inst_print(s, "Use 'x' to create a new partition table.");
    }

    inst_print(s, "");
    inst_print(s, "Type 'm' for help, 'q' to quit.");
    inst_print(s, "Command (m for help):");
}

static void inst_cmd_format(ShellInstance *s, const char *arg)
{
    if (!arg || !*arg) {
        inst_print(s, "Usage: format <device> [filesystem]");
        inst_print(s, "       format Device=DH0: Name=Workbench FFS");
        inst_print(s, "");
        inst_print(s, "Supported filesystems: fat32");
        inst_print(s, "");
        inst_print(s, "Note: Format a partition (e.g. virtio01 or DH0:),");
        inst_print(s, "      not the whole disk (virtio0).");
        return;
    }

    /* Parse Amiga-style keyword parameters */
    char devname[32] = {0};
    char volname[12] = {0};
    char fs[16] = {0};

    const char *p = arg;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;

        /* Check for keyword=value */
        if ((p[0] == 'D' || p[0] == 'd') &&
            (p[1] == 'e' || p[1] == 'E') &&
            (p[2] == 'V' || p[2] == 'v') &&
            p[3] == 'i' && p[4] == 'c' && p[5] == 'e' && p[6] == '=') {
            /* Device= */
            p += 7;
            int i = 0;
            while (*p && *p != ' ' && i < 31) { devname[i++] = *p++; }
            devname[i] = '\0';
        } else if ((p[0] == 'N' || p[0] == 'n') &&
                   (p[1] == 'a' || p[1] == 'A') &&
                   (p[2] == 'M' || p[2] == 'm') &&
                   (p[3] == 'e' || p[3] == 'E') && p[4] == '=') {
            /* Name= */
            p += 5;
            int i = 0;
            while (*p && *p != ' ' && i < 11) { volname[i++] = *p++; }
            volname[i] = '\0';
        } else if ((p[0] == 'F' || p[0] == 'f') &&
                   (p[1] == 'F' || p[1] == 'f') &&
                   (p[2] == 'S' || p[2] == 's')) {
            /* FFS */
            scopy(fs, "fat32", 16);
            p += 3;
        } else {
            /* Bare token: device name or fs type */
            char tok[32];
            int i = 0;
            while (*p && *p != ' ' && i < 31) { tok[i++] = *p++; }
            tok[i] = '\0';

            if (!devname[0]) {
                scopy(devname, tok, 32);
            } else if (!fs[0]) {
                scopy(fs, tok, 16);
            }
        }
    }

    if (!fs[0]) scopy(fs, "fat32", 16);

    if (!devname[0]) {
        inst_print(s, "No device specified.");
        inst_print(s, "Example: format virtio01 fat32");
        inst_print(s, "         format Device=DH0: Name=Workbench FFS");
        return;
    }

    /* Try to find by display name first, then by device name */
    BlockDev *dev = NULL;
    BlockDev *bdev = BlockDev_GetList();
    while (bdev) {
        if (bdev->display_name && seq(bdev->display_name, devname)) {
            dev = bdev;
            break;
        }
        bdev = bdev->next;
    }
    if (!dev) dev = BlockDev_Find(devname);

    if (!dev) {
        char msg[MAX_LINE_LEN];
        scopy(msg, "Device not found: ", MAX_LINE_LEN);
        scat(msg, devname, MAX_LINE_LEN);
        inst_print(s, msg);
        return;
    }

    /* Refuse to format a whole disk — partitions only */
    if (dev->part_offset == 0) {
        int len = 0;
        while (devname[len]) len++;
        if (len > 0 && !(devname[len - 1] >= '0' && devname[len - 1] <= '9')) {
            inst_print(s, "Cannot format whole disk.");
            inst_print(s, "Use fdisk to create a partition, then format it.");
            inst_print(s, "Example: format virtio01 fat32");
            return;
        }
    }

    char msg[MAX_LINE_LEN];
    scopy(msg, "format: ", MAX_LINE_LEN);
    scat(msg, dev->display_name ? dev->display_name : dev->name, MAX_LINE_LEN);
    if (volname[0]) {
        scat(msg, " name=", MAX_LINE_LEN);
        scat(msg, volname, MAX_LINE_LEN);
    }
    scat(msg, " as ", MAX_LINE_LEN);
    scat(msg, fs, MAX_LINE_LEN);
    inst_print(s, msg);
    inst_print(s, "");

    if (seq(fs, "fat32")) {
        inst_print(s, "Formatting... please wait.");
        int ret = FAT32_Format(dev, volname[0] ? volname : NULL);
        if (ret == 0) {
            dev->formatted = 1;
            inst_print(s, "Format complete.");
            /* Auto-mount in VFS so cd/dir work */
            const char *dname = dev->display_name ? dev->display_name : dev->name;
            char mnt_name[16];
            int ni = 0, si = 0;
            while (si < 15 && dname[si] && dname[si] != ':')
                mnt_name[ni++] = dname[si++];
            mnt_name[ni] = '\0';

            /* Mount by volume label if provided, else fall back to device name */
            const char *vol_mnt = volname[0] ? volname : mnt_name;
            if (vol_mnt[0]) {
                if (VFS_MountPartition(vol_mnt) == 0) {
                    char msg2[MAX_LINE_LEN];
                    scopy(msg2, "Mounted as ", MAX_LINE_LEN);
                    scat(msg2, vol_mnt, MAX_LINE_LEN);
                    scat(msg2, ":", MAX_LINE_LEN);
                    inst_print(s, msg2);
                }
            }
        } else {
            inst_print(s, "Format failed.");
        }
    } else {
        inst_print(s, "Unsupported filesystem.");
        inst_print(s, "Supported: fat32");
    }
}

/* =========================================================================
 * Dispatch
 * ========================================================================= */

/* Match command name (case-insensitive for first token) */
static int cmd_match(const char *line, const char *cmd, int cl)
{
    for (int j = 0; j < cl; j++) {
        char lc = line[j];
        if (lc >= 'A' && lc <= 'Z') lc += 32;
        char cc = cmd[j];
        if (cc >= 'A' && cc <= 'Z') cc += 32;
        if (lc != cc) return 0;
    }
    return line[cl] == 0 || line[cl] == ' ';
}

/* =========================================================================
 * Redirect support
 * ========================================================================= */


/* Parse the command line for redirect operators.
 * Fills cmd_only (the command + args without redirect tokens),
 * redir_path (the file path), redir_mode: 0=none 1=>write 2=>>append 3=<read.
 * Returns redir_mode. */
static int parse_redirects(ShellInstance *s, const char *line,
                            char *cmd_only, int cmd_max,
                            char *redir_path, int path_max)
{
    redir_path[0] = '\0';
    int mode = 0;
    int ci = 0;
    const char *p = line;

    while (*p) {
        /* Check for >> before > */
        if (p[0] == '>' && p[1] == '>') {
            mode = 2; p += 2;
            while (*p == ' ') p++;
            /* Grab path token */
            char raw[64]; int ri = 0;
            while (*p && *p != ' ' && ri < 63) raw[ri++] = *p++;
            raw[ri] = '\0';
            make_abs_path(s, raw, redir_path, path_max);
            continue;
        }
        if (p[0] == '>') {
            mode = 1; p++;
            while (*p == ' ') p++;
            char raw[64]; int ri = 0;
            while (*p && *p != ' ' && ri < 63) raw[ri++] = *p++;
            raw[ri] = '\0';
            make_abs_path(s, raw, redir_path, path_max);
            continue;
        }
        if (p[0] == '<') {
            mode = 3; p++;
            while (*p == ' ') p++;
            char raw[64]; int ri = 0;
            while (*p && *p != ' ' && ri < 63) raw[ri++] = *p++;
            raw[ri] = '\0';
            make_abs_path(s, raw, redir_path, path_max);
            continue;
        }
        if (ci < cmd_max - 1) cmd_only[ci++] = *p;
        p++;
    }
    /* Trim trailing spaces from cmd_only */
    while (ci > 0 && cmd_only[ci-1] == ' ') ci--;
    cmd_only[ci] = '\0';
    return mode;
}

/* =========================================================================
 * NativeCmdCtx callbacks — bridge between native commands and ShellInstance
 * ========================================================================= */

/* Activate fdisk interactive mode on this shell instance */
static void shell_set_fdisk_mode(void *shell_extra, struct BlockDev *dev)
{
    ShellInstance *s = (ShellInstance *)shell_extra;
    s->fdisk_mode = 1;
    s->fdisk_dev  = dev;
    memset(&s->fdisk_pt, 0, sizeof(PartitionTable));

    char msg[MAX_LINE_LEN];
    if (partition_read(dev, &s->fdisk_pt) == 0) {
        scopy(msg, "Reading partition table from ", MAX_LINE_LEN);
        scat(msg, dev->name, MAX_LINE_LEN);
        inst_print(s, msg);
    } else {
        scopy(msg, "No valid partition table on ", MAX_LINE_LEN);
        scat(msg, dev->name, MAX_LINE_LEN);
        inst_print(s, msg);
        inst_print(s, "Use 'x' to create a new partition table.");
    }
    inst_print(s, "");
    inst_print(s, "Type 'm' for help, 'q' to quit.");
    inst_print(s, "Command (m for help):");
}

/* Quit callback for inline vim */
static void shell_vim_quit(void *shell_extra)
{
    ShellInstance *s = (ShellInstance *)shell_extra;
    s->vim_mode = 0;
    s->vim_slot = -1;
}

/* Quit callback for inline ed */
static void shell_ed_quit(void *shell_extra)
{
    ShellInstance *s = (ShellInstance *)shell_extra;
    s->ed_mode = 0;
    s->ed_slot = -1;
}

/* Activate vim inline mode on this shell instance */
static void shell_set_vim_mode(void *shell_extra, const char *filename)
{
    ShellInstance *s = (ShellInstance *)shell_extra;
    int slot = VimWin_OpenInline(filename, s, shell_vim_quit);
    if (slot < 0) {
        inst_print(s, "vim: failed to open editor");
        return;
    }
    s->vim_mode = 1;
    s->vim_slot = slot;
}

/* Activate ed inline mode on this shell instance */
static void shell_set_ed_mode(void *shell_extra, const char *filename)
{
    ShellInstance *s = (ShellInstance *)shell_extra;
    int slot = EdWin_OpenInline(filename, s, shell_ed_quit);
    if (slot < 0) {
        inst_print(s, "ed: failed to open editor");
        return;
    }
    s->ed_mode = 1;
    s->ed_slot = slot;
}

/* Launch Workbench desktop */
static int g_shell_only_mode = 0;

void ShellWin_SetShellOnlyMode(int mode) { g_shell_only_mode = mode; }

static void shell_loadwb(void)
{
    if (g_shell_only_mode) {
        ShellInstance *s = &g_shells[0];
        inst_print(s, "loadwb: Workbench disabled (shell-only boot mode)");
        return;
    }
    Desktop_MarkWorkbenchLoaded();
    Desktop_Draw();
    WM_Redraw();
    /* Register the screen blanker commodity */
    extern void Blanker_Init(void);
    Blanker_Init();
}

/* Clear shell history buffer */
static void shell_clear_history(void *shell_extra)
{
    ShellInstance *s = (ShellInstance *)shell_extra;
    s->hist_count  = 0;
    s->hist_scroll = 0;
    for (int i = 0; i < MAX_HIST_LINES; i++) g_hist_buf[s->index][i][0] = 0;
}

/* Print text without appending a newline (append to current last line). */
static void shell_print_raw(void *shell_extra, const char *text)
{
    ShellInstance *s = (ShellInstance *)shell_extra;
    if (!s || !text) return;
    _ser_puts(text);
    if (s->hist_count > 0) {
        int slot = (s->hist_count - 1) % MAX_HIST_LINES;
        char *last = g_hist_buf[s->index][slot];
        int ll = slen(last);
        int tl = slen(text);
        int i = 0;
        while (i < tl && ll + i < MAX_LINE_LEN - 1) {
            last[ll + i] = text[i];
            i++;
        }
        last[ll + i] = '\0';
    } else {
        int slot = 0;
        scopy(g_hist_buf[s->index][slot], text, MAX_LINE_LEN);
        s->hist_count = 1;
    }
    s->auto_scroll = 1;
    WM_Redraw();
}

static void shell_dispatch_line(void *shell_extra, const char *line)
{
    inst_dispatch((ShellInstance *)shell_extra, line);
}

/* Check if name is a shell built-in (non-binary commands that stay in shell) */
static int shell_is_builtin(const char *name)
{
    static const char *builtins[] = {
        "help", "cd", "alias", "unalias", "set", "unset", "path",
        "setenv", "unsetenv", "showconfig", NULL
    };
    for (int i = 0; builtins[i]; i++) {
        int j = 0;
        const char *a = builtins[i], *b = name;
        while (a[j] && b[j]) {
            char ac = a[j]; if (ac >= 'A' && ac <= 'Z') ac += 32;
            char bc = b[j]; if (bc >= 'A' && bc <= 'Z') bc += 32;
            if (ac != bc) goto next;
            j++;
        }
        if (!a[j] && !b[j]) return 1;
        next:;
    }
    return 0;
}

/* Cooperative yield — voluntary reschedule for approximately ms milliseconds.
 * Under the preemptive scheduler this simply calls Task_Yield() repeatedly;
 * the idle task handles mouse, keyboard, and network polling. */
static void shell_yield_ms(void *shell_extra, uint32_t ms)
{
    (void)shell_extra;
    /* PIT runs at 100 Hz → 1 tick = 10 ms.  Convert ms to ticks.
     * Poll the network stack each iteration so that while the shell
     * task is waiting, incoming packets (DNS replies, ICMP, NTP, …)
     * still get processed even if the idle task hasn't run yet. */
    extern volatile uint64_t g_pit_ticks;
    uint64_t ticks = ms / 10;
    if (ticks == 0) ticks = 1;
    uint64_t start = g_pit_ticks;
    while (g_pit_ticks - start < ticks) {
        net_stack_poll();
        __asm__ volatile ("pause");
    }
}

/* Blocking key read — waits for a key in the shell's keyboard buffer,
 * yielding the CPU so other tasks remain responsive. */
static char shell_read_key(void *shell_extra)
{
    ShellInstance *s = (ShellInstance *)shell_extra;
    for (;;) {
        char c;
        if (shell_kb_dequeue(s, &c))
            return c;
        Task_Yield();
    }
}

/* Set ask mode — sets a custom prompt for the next input line.
 * Used by 'ask' command to display a custom prompt to the user. */
static void shell_set_ask_mode(void *shell_extra, const char *prompt)
{
    ShellInstance *s = (ShellInstance *)shell_extra;
    s->ask_mode = 1;
    s->ask_result_ready = 0;
    s->ask_result[0] = '\0';
    if (prompt) {
        scopy(s->ask_prompt, prompt, MAX_INPUT);
    } else {
        s->ask_prompt[0] = '\0';
    }
    /* Clear the regular input buffer to prepare for fresh input */
    s->input_len = 0;
    s->input_buf[0] = '\0';
    s->input_cur = 0;
}

/* Blocking line read — waits for Enter in the shell's keyboard buffer,
 * yielding the CPU so other tasks remain responsive.
 * Returns number of characters read (excluding null terminator). */
static int shell_read_line(void *shell_extra, char *buf, int max)
{
    ShellInstance *s = (ShellInstance *)shell_extra;

    /* Clear the result buffer and flag */
    s->ask_result[0] = '\0';
    s->ask_result_ready = 0;

    for (;;) {
        char c;
        if (shell_kb_dequeue(s, &c)) {
            /* Handle Enter - line complete */
            if (c == '\r' || c == '\n') {
                s->ask_result[s->input_len] = '\0';
                s->ask_result_ready = 1;
                s->ask_mode = 0;  /* Exit ask mode */
                /* Copy result to caller's buffer */
                int len = s->input_len;
                if (len >= max) len = max - 1;
                for (int i = 0; i < len; i++) buf[i] = s->ask_result[i];
                buf[len] = '\0';
                /* Clear input buffer for next time */
                s->input_len = 0;
                s->input_buf[0] = '\0';
                s->input_cur = 0;
                return len;
            }

            /* Handle backspace */
            if (c == '\b' || c == 0x7F) {
                if (s->input_len > 0) {
                    s->input_len--;
                    s->input_cur--;
                    s->input_buf[s->input_len] = '\0';
                }
                continue;
            }

            /* Handle printable characters */
            if (c >= 32 && c < 127 && s->input_len < MAX_INPUT) {
                s->input_buf[s->input_len++] = c;
                s->input_cur++;
                s->input_buf[s->input_len] = '\0';
                /* Copy to result buffer */
                s->ask_result[s->input_len] = '\0';
                for (int i = 0; i < s->input_len; i++) {
                    s->ask_result[i] = s->input_buf[i];
                }
            }
        }
        Task_Yield();
    }
}

/* Enumerate running tasks for the ps command.
 * idx starts at 0; returns 1 and fills out while tasks remain, 0 at end. */
static int shell_enum_tasks(void *shell_extra, int idx, char *out, int max)
{
    (void)shell_extra;
    int n = 0;

    /* Shell CLI instances */
    for (int i = 0; i < g_n_shells; i++) {
        if (n++ == idx) {
            char msg[96];
            scopy(msg, "CLI #", 96);
            char num[8];
            uint_to_dec_s((uint32_t)g_shells[i].number, num, sizeof(num));
            scat(msg, num, 96);
            scat(msg, "   Shell  ", 96);
            scat(msg, g_shells[i].cwd, 96);
            if (g_shells[i].vim_mode) {
                scat(msg, "  (vim)", 96);
            } else if (g_shells[i].fdisk_mode) {
                scat(msg, "  (fdisk)", 96);
            } else if (g_shells[i].ask_mode) {
                scat(msg, "  (ask)", 96);
            }
            scopy(out, msg, max);
            return 1;
        }
    }

    /* WM windows that are not already listed above (e.g. Calculator, Clock) */
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (!WM_IsWindowActive(i)) continue;

        /* Skip shell windows — already listed as CLI #N */
        int is_shell = 0;
        for (int j = 0; j < g_n_shells; j++) {
            if (g_shells[j].wm_handle == i) { is_shell = 1; break; }
        }
        if (is_shell) continue;

        if (n++ == idx) {
            char title[32];
            WM_GetWindowTitle(i, title, sizeof(title));
            char msg[96];
            scopy(msg, title, 96);
            scat(msg, "   Window", 96);
            scopy(out, msg, max);
            return 1;
        }
    }

    return 0;
}

/* Set custom prompt (PROMPT command) */
static void shell_set_prompt(void *shell_extra, const char *prompt)
{
    ShellInstance *s = (ShellInstance *)shell_extra;
    if (prompt) {
        scopy(s->custom_prompt, prompt, sizeof(s->custom_prompt));
    } else {
        s->custom_prompt[0] = '\0';
    }
}

/* Close the current shell window (ENDCLI command) */
static void shell_close_shell(void *shell_extra)
{
    ShellInstance *s = (ShellInstance *)shell_extra;
    if (s->wm_handle >= 0) {
        WM_CloseWindow(s->wm_handle);
    }
}

/* Get last command return code (WHY command) */
static int shell_get_last_rc(void *shell_extra)
{
    ShellInstance *s = (ShellInstance *)shell_extra;
    return s->last_rc;
}

/* Set last command return code */
static void shell_set_rc(void *shell_extra, int rc)
{
    ShellInstance *s = (ShellInstance *)shell_extra;
    s->last_rc = rc;
}

/* Get FAILAT threshold */
static int shell_get_failat(void *shell_extra)
{
    ShellInstance *s = (ShellInstance *)shell_extra;
    return s->failat_threshold;
}

/* Set FAILAT threshold */
static void shell_set_failat(void *shell_extra, int threshold)
{
    ShellInstance *s = (ShellInstance *)shell_extra;
    s->failat_threshold = threshold;
}

/* Check if last_rc meets or exceeds FAILAT threshold and print a message */
static void check_failat(ShellInstance *s)
{
    if (s->last_rc != 0 && s->last_rc >= s->failat_threshold) {
        char msg[MAX_LINE_LEN];
        char num[8], th[8];
        uint_to_dec_s((uint32_t)s->last_rc, num, 8);
        uint_to_dec_s((uint32_t)s->failat_threshold, th, 8);
        scopy(msg, "FAILAT: return code ", MAX_LINE_LEN);
        scat(msg, num, MAX_LINE_LEN);
        scat(msg, " >= threshold ", MAX_LINE_LEN);
        scat(msg, th, MAX_LINE_LEN);
        inst_print(s, msg);
    }
}

/* Get environment variable value */
static int shell_get_env(void *shell_extra, const char *name, char *buf, int max)
{
    ShellInstance *s = (ShellInstance *)shell_extra;
    if (!name || !buf || max < 1) return 0;
    for (int i = 0; i < s->env_count; i++) {
        if (seq_ci(s->env_names[i], name)) {
            scopy(buf, s->env_values[i], max);
            return 1;
        }
    }
    buf[0] = '\0';
    return 0;
}

/* Set a shell environment variable by name (requestchoice/requestfile) */
static void shell_set_env(void *shell_extra, const char *name, const char *value)
{
    ShellInstance *s = (ShellInstance *)shell_extra;
    if (!name || !value) return;
    for (int i = 0; i < s->env_count; i++) {
        if (seq_ci(s->env_names[i], name)) {
            scopy(s->env_values[i], value, MAX_ENV_VAL);
            return;
        }
    }
    if (s->env_count < MAX_ENV_VARS) {
        scopy(s->env_names[s->env_count], name, MAX_ENV_NAME);
        scopy(s->env_values[s->env_count], value, MAX_ENV_VAL);
        s->env_count++;
    }
}

/* Change the priority of a named task (changetaskpri) */
static int shell_change_task_pri(void *shell_extra, const char *name, int8_t pri)
{
    (void)shell_extra;
    UaosTask *t = Task_FindByName(name);
    if (!t) return 0;
    t->ln_Pri = pri;
    return 1;
}

/* Signal script runner to quit (QUIT command) */
static void shell_quit_script(void *shell_extra, int rc)
{
    ShellInstance *s = (ShellInstance *)shell_extra;
    s->quit_flag = 1;
    s->last_rc = rc;
}

/* Compute the number of text rows visible in the history pane of shell s */
static int shell_visible_rows(ShellInstance *s)
{
    int hh = s->wh - TITLEBAR_H - INPUTBAR_H - WM_SCROLLBAR_W - 8;
    int rows = hh / 16;
    return rows > 0 ? rows : 20;
}

/* Build a NativeCmdCtx for the given shell instance */
static NativeCmdCtx shell_make_ctx(ShellInstance *s)
{
    NativeCmdCtx ctx;
    ctx.shell          = s;
    ctx.print          = (void (*)(void *, const char *))inst_print;
    ctx.print_raw      = shell_print_raw;
    ctx.cwd            = s->cwd;
    ctx.path           = s->path;
    ctx.shell_extra    = s;
    ctx.set_fdisk_mode = shell_set_fdisk_mode;
    ctx.set_vim_mode   = shell_set_vim_mode;
    ctx.set_ed_mode    = shell_set_ed_mode;
    ctx.loadwb         = shell_loadwb;
    ctx.clear_history  = shell_clear_history;
    ctx.is_builtin     = shell_is_builtin;
    ctx.dispatch_line  = shell_dispatch_line;
    ctx.run_script     = shell_run_script;
    ctx.yield_ms       = shell_yield_ms;
    ctx.read_key       = shell_read_key;
    ctx.read_line      = shell_read_line;
    ctx.set_ask_mode   = shell_set_ask_mode;
    ctx.visible_rows   = shell_visible_rows(s);
    ctx.enum_tasks     = shell_enum_tasks;
    ctx.set_prompt     = shell_set_prompt;
    ctx.close_shell    = shell_close_shell;
    ctx.get_last_rc    = shell_get_last_rc;
    ctx.set_rc         = shell_set_rc;
    ctx.get_failat     = shell_get_failat;
    ctx.set_failat     = shell_set_failat;
    ctx.get_env        = shell_get_env;
    ctx.set_env        = shell_set_env;
    ctx.change_task_pri = shell_change_task_pri;
    ctx.quit_script    = shell_quit_script;
    ctx.pipe_file      = g_pipe_in_active ? g_pipe_in_file : NULL;
    return ctx;
}

/* =========================================================================
 * UAOS binary executor
 *
 * Opens a VFS file, reads the 32-byte UAOS header, then:
 *   NATIVE  -> NativeCmd_Run(name, ctx, args)
 *   M68K    -> UAOS_Emu_LoadAndRun with the payload bytes
 *   fallback -> execute as a text script (legacy / plain text files)
 * Returns 0 on success, -1 if file not found, -2 bad magic / unknown type.
 * ========================================================================= */

/* Static payload buffer for M68K binaries loaded from VFS (max 2 MB) */
#define UAOS_MAX_BIN_PAYLOAD (2 * 1024 * 1024)
static uint8_t g_bin_payload[UAOS_MAX_BIN_PAYLOAD];

/* Print adapters for raw M68k binaries launched as background tasks.
 * Each adapter is bound to a fixed shell slot so the task keeps its output
 * routed to the correct shell even after concurrent launches. */
static void raw_m68k_print_0(const char *line) { if (g_shells[0].wm_handle) inst_print(&g_shells[0], line); }
static void raw_m68k_print_1(const char *line) { if (g_shells[1].wm_handle) inst_print(&g_shells[1], line); }
static void raw_m68k_print_2(const char *line) { if (g_shells[2].wm_handle) inst_print(&g_shells[2], line); }
static void raw_m68k_print_3(const char *line) { if (g_shells[3].wm_handle) inst_print(&g_shells[3], line); }
static void (*raw_m68k_print[4])(const char *) = {
    raw_m68k_print_0, raw_m68k_print_1, raw_m68k_print_2, raw_m68k_print_3
};

static int inst_exec_uaos_bin(ShellInstance *s, const char *full_path,
                               const char *args)
{
    VfsFile fh;
    if (!VFS_Open(&fh, full_path, VFS_READ)) {
        return -1;
    }

    uint32_t file_size = VFS_Size(&fh);
    if (file_size < 4) {
        VFS_Close(&fh);
        return -2;
    }

    /* Read the first 4 bytes to detect either a UAOS wrapper or raw Hunk. */
    uint8_t first4[4];
    if (VFS_Read(&fh, first4, 4) != 4) {
        VFS_Close(&fh);
        return -2;
    }
    uint32_t magic = uaos_bin_u32(first4);

    if (magic == UAOS_BIN_MAGIC) {
        if (file_size < UAOS_BIN_HEADER_SIZE) {
            VFS_Close(&fh);
            return -2;
        }

        uint8_t hdr[UAOS_BIN_HEADER_SIZE];
        memcpy(hdr, first4, 4);
        if (VFS_Read(&fh, hdr + 4, UAOS_BIN_HEADER_SIZE - 4) != UAOS_BIN_HEADER_SIZE - 4) {
            VFS_Close(&fh);
            return -2;
        }

        uint16_t type         = uaos_bin_u16(hdr + 4);
        uint32_t payload_size = uaos_bin_u32(hdr + 8);

        /* Extract name from header (NUL-padded, 16 bytes at offset 12) */
        char bin_name[17];
        int ni = 0;
        while (ni < 16 && hdr[12 + ni]) { bin_name[ni] = (char)hdr[12 + ni]; ni++; }
        bin_name[ni] = '\0';

        if (type == UAOS_BIN_TYPE_NATIVE) {
            VFS_Close(&fh);
            NativeCmdCtx nctx = shell_make_ctx(s);
            if (NativeCmd_Run(bin_name, &nctx, args) == 0) return 0;
            char msg[MAX_LINE_LEN];
            scopy(msg, "Native handler not found: ", MAX_LINE_LEN);
            scat(msg, bin_name, MAX_LINE_LEN);
            inst_print(s, msg);
            return -2;
        }

        if (type == UAOS_BIN_TYPE_M68K) {
            if (payload_size == 0 || payload_size > UAOS_MAX_BIN_PAYLOAD) {
                VFS_Close(&fh);
                inst_print(s, "M68K binary: payload size invalid or too large");
                return -2;
            }
            uint32_t n = VFS_Read(&fh, g_bin_payload, payload_size);
            VFS_Close(&fh);
            if (n != payload_size) {
                inst_print(s, "M68K binary: read error");
                return -2;
            }
            /* Build argv from bin_name + args */
            const char *m68k_argv[18];
            char m68k_argstore[256];
            m68k_argv[0] = bin_name;
            int argc = 1;
            int ai = 0;
            if (args && *args) {
                while (*args && ai < 254) m68k_argstore[ai++] = *args++;
            }
            if (g_pipe_in_active && ai < 254) {
                if (ai > 0) m68k_argstore[ai++] = ' ';
                const char *pf = g_pipe_in_file;
                while (*pf && ai < 254) m68k_argstore[ai++] = *pf++;
            }
            m68k_argstore[ai] = '\0';
            if (ai > 0) {
                char *tok = m68k_argstore;
                while (*tok && argc < 16) {
                    while (*tok == ' ') tok++;
                    if (!*tok) break;
                    m68k_argv[argc++] = tok;
                    while (*tok && *tok != ' ') tok++;
                    if (*tok == ' ') *tok++ = '\0';
                }
            }
            m68k_argv[argc] = NULL;
            int slot = (int)(s - g_shells);
            UAOS_Emu_SetCwd(s->cwd);
            UaosTask *t = Task_CreateM68k(bin_name, -128,
                                          g_bin_payload, payload_size,
                                          m68k_argv,
                                          (slot >= 0 && slot < MAX_SHELLS)
                                              ? raw_m68k_print[slot]
                                              : NULL);
            if (!t) {
                inst_print(s, "M68K binary: failed to create task");
                return -2;
            }
            /* Wait for the foreground M68k command to finish before
             * returning to the prompt, so output appears before the
             * next prompt line. When the shell is not running as a
             * scheduled task (e.g. during the pre-scheduler startup
             * sequence), there is no parent task context to wait in. */
            UaosTask *cur = Task_Current();
            if (cur) {
                Wait(SIGF_CHILD);
            }
            return 0;
        }

        if (type == UAOS_BIN_TYPE_X64) {
            if (payload_size == 0 || payload_size > UAOS_MAX_BIN_PAYLOAD) {
                VFS_Close(&fh);
                inst_print(s, "X64 binary: payload size invalid or too large");
                return -2;
            }
            uint32_t n = VFS_Read(&fh, g_bin_payload, payload_size);
            VFS_Close(&fh);
            if (n != payload_size) {
                inst_print(s, "X64 binary: read error");
                return -2;
            }
            /* Build argv from bin_name + args */
            const char *x64_argv[18];
            char x64_argstore[256];
            x64_argv[0] = bin_name;
            int argc = 1;
            int ai = 0;
            if (args && *args) {
                while (*args && ai < 254) x64_argstore[ai++] = *args++;
            }
            if (g_pipe_in_active && ai < 254) {
                if (ai > 0) x64_argstore[ai++] = ' ';
                const char *pf = g_pipe_in_file;
                while (*pf && ai < 254) x64_argstore[ai++] = *pf++;
            }
            x64_argstore[ai] = '\0';
            if (ai > 0) {
                char *tok = x64_argstore;
                while (*tok && argc < 16) {
                    while (*tok == ' ') tok++;
                    if (!*tok) break;
                    x64_argv[argc++] = tok;
                    while (*tok && *tok != ' ') tok++;
                    if (*tok == ' ') *tok++ = '\0';
                }
            }
            x64_argv[argc] = NULL;

            ELF64Result result;
            if (ELF64_Load(g_bin_payload, payload_size, x64_argv, &result) != 0) {
                inst_print(s, "X64 binary: load failed");
                return -2;
            }
            int8_t pri = 0;
            UaosTask *cur = Task_Current();
            if (cur) pri = cur->ln_Pri;
            UaosTask *t = Task_CreateX64(bin_name, pri,
                                         result.entry_rip, result.initial_rsp,
                                         s->cwd,
                                         (void (*)(void *, const char *))inst_print,
                                         s);
            if (!t) {
                inst_print(s, "X64 binary: failed to create task");
                return -2;
            }
            /* Wait for the foreground X64 command to finish before
             * returning to the prompt, so output appears before the
             * next prompt line. When the shell is not running as a
             * scheduled task (e.g. during the pre-scheduler startup
             * sequence), there is no parent task context to wait in; the
             * newly created task will be picked up by the scheduler once
             * it starts. */
            if (cur) {
                Wait(SIGF_CHILD);
            }
            return 0;
        }

        /* Unknown type */
        VFS_Close(&fh);
        inst_print(s, "Unknown UAOS binary type");
        return -2;
    }

    if (magic == UAOS_BIN_HUNK_MAGIC) {
        /* Raw Amiga Hunk binary — run as a background M68k task. */
        if (file_size > UAOS_MAX_BIN_PAYLOAD) {
            VFS_Close(&fh);
            inst_print(s, "M68K binary: payload size too large");
            return -2;
        }
        VFS_Seek(&fh, 0);
        uint32_t n = VFS_Read(&fh, g_bin_payload, file_size);
        VFS_Close(&fh);
        if (n != file_size) {
            inst_print(s, "M68K binary: read error");
            return -2;
        }

        /* Derive a name from the file path */
        const char *name = full_path;
        for (const char *p = full_path; *p; p++) {
            if (*p == '/' || *p == ':' || *p == '\\') name = p + 1;
        }
        char bin_name[16];
        int bi = 0;
        while (bi < 15 && name[bi] && name[bi] != '.') {
            bin_name[bi] = name[bi];
            bi++;
        }
        bin_name[bi] = '\0';

        /* Build argv from bin_name + args */
        const char *m68k_argv[18];
        char m68k_argstore[256];
        m68k_argv[0] = bin_name;
        int argc = 1;
        int ai = 0;
        if (args && *args) {
            while (*args && ai < 254) m68k_argstore[ai++] = *args++;
        }
        if (g_pipe_in_active && ai < 254) {
            if (ai > 0) m68k_argstore[ai++] = ' ';
            const char *pf = g_pipe_in_file;
            while (*pf && ai < 254) m68k_argstore[ai++] = *pf++;
        }
        m68k_argstore[ai] = '\0';
        if (ai > 0) {
            char *tok = m68k_argstore;
            while (*tok && argc < 16) {
                while (*tok == ' ') tok++;
                if (!*tok) break;
                m68k_argv[argc++] = tok;
                while (*tok && *tok != ' ') tok++;
                if (*tok == ' ') *tok++ = '\0';
            }
        }
        m68k_argv[argc] = NULL;

        int slot = (int)(s - g_shells);
        UAOS_Emu_SetCwd(s->cwd);
        UaosTask *t = Task_CreateM68k(bin_name, -128,
                                      g_bin_payload, file_size,
                                      m68k_argv,
                                      (slot >= 0 && slot < MAX_SHELLS)
                                          ? raw_m68k_print[slot]
                                          : NULL);
        (void)t;
        return 0;
    }

    VFS_Close(&fh);
    /* Fallback: execute as a text script */
    inst_cmd_execute(s, full_path);
    return 0;
}

/* =========================================================================
 * Dispatch
 * ========================================================================= */

static void run_cmd(ShellInstance *s, const char *line)
{
    const char *lp = script_skip_sp(line);

    /* Single-line IF at the interactive prompt */
    if (script_kw_match(lp, "if")) {
        const char *cond = script_skip_sp(lp + 2);
        const char *tp = cond;
        while (*tp && *tp != ' ' && *tp != '\t') tp++;
        tp = script_skip_sp(tp);
        int is_then = (tp[0] == 'T' || tp[0] == 't') &&
                      (tp[1] == 'H' || tp[1] == 'h') &&
                      (tp[2] == 'E' || tp[2] == 'e') &&
                      (tp[3] == 'N' || tp[3] == 'n') &&
                      (tp[4] == ' ' || tp[4] == '\t' || tp[4] == '\0');
        if (is_then) {
            if (script_eval_cond(s, cond)) {
                inst_dispatch(s, script_skip_sp(tp + 4));
            }
            return;
        }
        inst_print(s, "Multi-line IF blocks are only valid inside scripts.");
        return;
    }

    /* Block keywords at the prompt */
    if (script_kw_match(lp, "else") || script_kw_match(lp, "endif") ||
        script_kw_match(lp, "endfor")) {
        inst_print(s, "Block keyword only valid inside scripts.");
        return;
    }

    /* Single-line FOR at the interactive prompt */
    if (script_kw_match(lp, "for")) {
        const char *rest = script_skip_sp(lp + 3);
        char varname[MAX_ENV_NAME];
        int vi = 0;
        while (*rest && *rest != ' ' && *rest != '\t' && *rest != '=' && vi < MAX_ENV_NAME - 1)
            varname[vi++] = *rest++;
        varname[vi] = '\0';
        rest = script_skip_sp(rest);
        if (*rest != '=') goto for_prompt_err;
        rest = script_skip_sp(rest + 1);
        int start_val, end_val, step_val = 1;
        if (!script_parse_int(rest, &start_val)) goto for_prompt_err;
        while (*rest && ((*rest >= '0' && *rest <= '9') || *rest == '-')) rest++;
        rest = script_skip_sp(rest);
        if (!script_kw_match(rest, "to")) goto for_prompt_err;
        rest = script_skip_sp(rest + 2);
        if (!script_parse_int(rest, &end_val)) goto for_prompt_err;
        while (*rest && ((*rest >= '0' && *rest <= '9') || *rest == '-')) rest++;
        rest = script_skip_sp(rest);
        if (script_kw_match(rest, "step")) {
            rest = script_skip_sp(rest + 4);
            if (!script_parse_int(rest, &step_val)) goto for_prompt_err;
            while (*rest && ((*rest >= '0' && *rest <= '9') || *rest == '-')) rest++;
            rest = script_skip_sp(rest);
        }
        if (!script_kw_match(rest, "do")) goto for_prompt_err;
        rest = script_skip_sp(rest + 2);

        for (int v = start_val; (step_val > 0) ? (v <= end_val) : (v >= end_val); v += step_val) {
            char valstr[16];
            int n = v, neg = 0;
            if (n < 0) { neg = 1; n = -n; }
            char tmp[16]; int ti = 0;
            do { tmp[ti++] = '0' + (n % 10); n /= 10; } while (n > 0);
            int di = 0;
            if (neg) valstr[di++] = '-';
            while (ti-- > 0) valstr[di++] = tmp[ti];
            valstr[di] = '\0';
            script_set_var(s, varname, valstr);
            inst_dispatch(s, rest);
        }
        return;
    for_prompt_err:
        inst_print(s, "Multi-line FOR blocks are only valid inside scripts.");
        return;
    }

    /* Check for alias expansion first */
    char first_word[32];
    const char *p = line;
    int i = 0;
    while (*p && *p != ' ' && i < 31) { first_word[i++] = *p++; }
    first_word[i] = '\0';

    /* Try to expand alias */
    char expanded[MAX_LINE_LEN];
    const char *cmd_to_run = line;
    for (int i = 0; i < s->alias_count; i++) {
        if (seq_ci(s->alias_names[i], first_word)) {
            /* Expand alias: alias_value + remaining args */
            scopy(expanded, s->alias_values[i], MAX_LINE_LEN);
            while (*p == ' ') p++;
            if (*p) {
                scat(expanded, " ", MAX_LINE_LEN);
                scat(expanded, p, MAX_LINE_LEN);
            }
            cmd_to_run = expanded;
            break;
        }
    }

    /* Re-extract first_word from the (possibly expanded) command */
    {
        const char *q = cmd_to_run;
        int j = 0;
        while (*q && *q != ' ' && j < 31) { first_word[j++] = *q++; }
        first_word[j] = '\0';
    }

    /* ---- Explicit path: user typed "C:cmd", "SYS:tools/foo", etc. ----
     *
     * If first_word contains a colon it is already a fully-qualified VFS
     * path.  Try it directly.  For the common AmigaDOS convention of
     * "C:CommandName" we also lowercase the part after the colon so that
     * "C:LoadWB", "c:loadwb" and "C:LOADWB" all resolve to the same file.
     * -------------------------------------------------------------------- */
    {
        const char *colon = first_word;
        while (*colon && *colon != ':') colon++;
        if (*colon == ':') {
            /* first_word IS the path — args_tail follows it in cmd_to_run */
            const char *expl_args = cmd_to_run + slen(first_word);
            while (*expl_args == ' ') expl_args++;

            /* Try the path exactly as typed first */
            if (inst_exec_uaos_bin(s, first_word, expl_args) != -1)
                return;

            /* Build a lowercased version of the filename part and retry.
             * e.g. "C:LoadWB" -> "C:loadwb" */
            char lower_path[64];
            int li = 0;
            /* copy up to and including the colon verbatim */
            const char *fp = first_word;
            while (*fp && *fp != ':' && li < 62) lower_path[li++] = *fp++;
            if (*fp == ':') lower_path[li++] = ':';
            fp++; /* skip colon */
            while (*fp && li < 62) {
                char c = *fp++;
                if (c >= 'A' && c <= 'Z') c += 32;
                lower_path[li++] = c;
            }
            lower_path[li] = '\0';

            if (!seq(lower_path, first_word)) {
                if (inst_exec_uaos_bin(s, lower_path, expl_args) != -1)
                    return;
            }

            /* Not found at that explicit path */
            char msg[MAX_LINE_LEN];
            scopy(msg, "Unknown command: ", MAX_LINE_LEN);
            scat(msg, first_word, MAX_LINE_LEN);
            inst_print(s, msg);
            return;
        }
    }

    /* ---- Shell built-in commands (not discrete C: binaries) ---- */
    const char *builtins[] = {
        "help", "cd", "alias", "unalias", "set", "unset", "path",
        "setenv", "unsetenv", "showconfig",
        NULL
    };

    for (int i = 0; builtins[i]; i++) {
        const char *c = builtins[i];
        int cl = slen(c);
        if (!cmd_match(cmd_to_run, c, cl)) continue;

        const char *args = cmd_to_run + cl;
        while (*args == ' ') args++;

        if (i==0) inst_cmd_help(s);
        else if (i==1) inst_cmd_cd(s, args);
        else if (i==2) inst_cmd_alias(s, args);
        else if (i==3) inst_cmd_unalias(s, args);
        else if (i==4) inst_cmd_set(s, args);
        else if (i==5) inst_cmd_unset(s, args);
        else if (i==6) inst_cmd_path(s, args);
        else if (i==7) inst_cmd_setenv(s, args);
        else if (i==8) inst_cmd_unsetenv(s, args);
        else if (i==9) inst_cmd_showconfig(s);
        return;
    }

    /* ---- Check resident commands (in-memory cached binaries) ---- */
    if (Resident_Exists(first_word)) {
        NativeCmdCtx ctx = shell_make_ctx(s);
        if (Resident_Run(first_word, &ctx, cmd_to_run + slen(first_word)) == 0) {
            return;
        }
    }

    /* ---- Check native command registry (C: binaries) ---- */
    if (NativeCmd_Exists(first_word)) {
        NativeCmdCtx ctx = shell_make_ctx(s);
        /* Skip command name in args */
        const char *args = cmd_to_run + slen(first_word);
        while (*args == ' ') args++;
        NativeCmd_Run(first_word, &ctx, args);
        return;
    }

    /* ---- PATH search: look for the command in each PATH directory ----
     *
     * Each file found is opened and its UAOS binary header is inspected:
     *   NATIVE header -> call NativeCmd_Run by the name embedded in the header
     *   M68K   header -> pass payload to UAOS_Emu_LoadAndRun (transparent)
     *   no header     -> execute as a text script (legacy / plain scripts)
     * -------------------------------------------------------------------- */
    char path_buf[256];
    scopy(path_buf, s->path, 256);
    char *path_p = path_buf;

    const char *args_tail = cmd_to_run + slen(first_word);
    while (*args_tail == ' ') args_tail++;

    while (*path_p) {
        while (*path_p == ' ') path_p++;
        if (!*path_p) break;

        char entry[64];
        int ei = 0;
        while (*path_p && *path_p != ' ' && ei < 63) {
            entry[ei++] = *path_p++;
        }
        entry[ei] = '\0';

        if (ei > 0) {
            char full_path[128];
            scopy(full_path, entry, 128);
            if (ei > 0 && entry[ei-1] != ':' && entry[ei-1] != '/')
                scat(full_path, "/", 128);
            scat(full_path, first_word, 128);

            /* Try to open — inst_exec_uaos_bin checks file existence */
            if (inst_exec_uaos_bin(s, full_path, args_tail) != -1)
                return;  /* executed (or error printed) */
        }
    }

    /* Also search the current working directory */
    {
        char cwd_path[128];
        scopy(cwd_path, s->cwd, 128);
        int cl = slen(cwd_path);
        if (cl > 0 && cwd_path[cl-1] != ':' && cwd_path[cl-1] != '/')
            scat(cwd_path, "/", 128);
        scat(cwd_path, first_word, 128);
        if (inst_exec_uaos_bin(s, cwd_path, args_tail) != -1)
            return;
    }

    char msg[MAX_LINE_LEN];
    scopy(msg, "Unknown command: ", MAX_LINE_LEN);
    scat(msg, cmd_to_run, MAX_LINE_LEN);
    inst_print(s, msg);
}

/* -------------------------------------------------------------------------
 * $[expr] arithmetic evaluator
 *
 * Supports: integer literals, $VarName references, unary minus, binary
 *           + - * / % operators with standard precedence, and parentheses.
 * Division/modulo by zero yields 0.
 * Variable lookup is done from the shell's local env store.
 * ------------------------------------------------------------------------- */

typedef struct { const char *p; ShellInstance *s; } ArithCtx;

static void arith_skip_sp(ArithCtx *a) { while (*a->p == ' ') a->p++; }

/* Resolve a $VarName or a bare name used as a variable.  Returns its integer
 * value, or 0 if not found / not numeric. */
static int arith_lookup_var(ShellInstance *s, const char *name)
{
    for (int i = 0; i < s->env_count; i++) {
        if (seq_ci(s->env_names[i], name)) {
            const char *v = s->env_values[i];
            int neg = 0, n = 0;
            if (*v == '-') { neg = 1; v++; }
            while (*v >= '0' && *v <= '9') { n = n * 10 + (*v - '0'); v++; }
            return neg ? -n : n;
        }
    }
    return 0;
}

static int arith_expr(ArithCtx *a);  /* forward */

/* Parse a primary: number, $var, (expr), or bare identifier */
static int arith_primary(ArithCtx *a)
{
    arith_skip_sp(a);
    /* Unary minus */
    if (*a->p == '-') { a->p++; return -arith_primary(a); }
    /* Parenthesised expression */
    if (*a->p == '(') {
        a->p++;
        int v = arith_expr(a);
        arith_skip_sp(a);
        if (*a->p == ')') a->p++;
        return v;
    }
    /* Variable reference: $NAME */
    if (*a->p == '$') {
        a->p++;
        char vname[MAX_ENV_NAME]; int vi = 0;
        while (*a->p && (*a->p == '_' ||
               (*a->p >= 'A' && *a->p <= 'Z') ||
               (*a->p >= 'a' && *a->p <= 'z') ||
               (*a->p >= '0' && *a->p <= '9')) && vi < MAX_ENV_NAME - 1)
            vname[vi++] = *a->p++;
        vname[vi] = '\0';
        return arith_lookup_var(a->s, vname);
    }
    /* Decimal integer literal */
    if (*a->p >= '0' && *a->p <= '9') {
        int v = 0;
        while (*a->p >= '0' && *a->p <= '9') { v = v * 10 + (*a->p - '0'); a->p++; }
        return v;
    }
    /* Bare identifier (variable name without $) */
    if ((*a->p >= 'A' && *a->p <= 'Z') || (*a->p >= 'a' && *a->p <= 'z') || *a->p == '_') {
        char vname[MAX_ENV_NAME]; int vi = 0;
        while (*a->p && (*a->p == '_' ||
               (*a->p >= 'A' && *a->p <= 'Z') ||
               (*a->p >= 'a' && *a->p <= 'z') ||
               (*a->p >= '0' && *a->p <= '9')) && vi < MAX_ENV_NAME - 1)
            vname[vi++] = *a->p++;
        vname[vi] = '\0';
        return arith_lookup_var(a->s, vname);
    }
    return 0;
}

/* Multiplicative: * / % */
static int arith_mul(ArithCtx *a)
{
    int v = arith_primary(a);
    for (;;) {
        arith_skip_sp(a);
        char op = *a->p;
        if (op != '*' && op != '/' && op != '%') break;
        a->p++;
        int r = arith_primary(a);
        if (op == '*') v *= r;
        else if (op == '/') v = r ? v / r : 0;
        else                v = r ? v % r : 0;
    }
    return v;
}

/* Additive: + - */
static int arith_expr(ArithCtx *a)
{
    int v = arith_mul(a);
    for (;;) {
        arith_skip_sp(a);
        char op = *a->p;
        if (op != '+' && op != '-') break;
        a->p++;
        int r = arith_mul(a);
        v = (op == '+') ? v + r : v - r;
    }
    return v;
}

/* Evaluate a NUL-terminated expression string.  Returns result as int. */
static int arith_eval(ShellInstance *s, const char *expr)
{
    ArithCtx a;
    a.p = expr;
    a.s = s;
    return arith_expr(&a);
}

/* Write a signed decimal integer into buf[max].  Returns bytes written. */
static int arith_itoa(int v, char *buf, int max)
{
    if (max < 2) return 0;
    char tmp[16]; int ti = 0, di = 0;
    int neg = (v < 0);
    unsigned int uv = neg ? (unsigned int)(-(v + 1)) + 1u : (unsigned int)v;
    if (uv == 0) { tmp[ti++] = '0'; }
    else { while (uv) { tmp[ti++] = '0' + (int)(uv % 10u); uv /= 10u; } }
    if (neg && di < max - 1) buf[di++] = '-';
    while (ti-- > 0 && di < max - 1) buf[di++] = tmp[ti];
    buf[di] = '\0';
    return di;
}

/* Expand $VarName references in src into dst[max].  Reads local env store.
 * Also reads ENV:<name> file as fallback for vars not in local store.
 * Supports $[<expr>] arithmetic expansion. */
static void expand_vars(ShellInstance *s, const char *src, char *dst, int max)
{
    int di = 0;
    while (*src && di < max - 1) {
        if (*src == '$') {
            src++;

            /* $[expr] — arithmetic expansion */
            if (*src == '[') {
                src++;
                /* Collect everything up to matching ']' */
                char expr[128]; int ei = 0;
                while (*src && *src != ']' && ei < 127) expr[ei++] = *src++;
                expr[ei] = '\0';
                if (*src == ']') src++;
                int result = arith_eval(s, expr);
                char numstr[20];
                arith_itoa(result, numstr, sizeof(numstr));
                const char *np = numstr;
                while (*np && di < max - 1) dst[di++] = *np++;
                continue;
            }

            char vname[MAX_ENV_NAME];
            int vi = 0;
            if (*src == '{') {
                src++;
                while (*src && *src != '}' && vi < MAX_ENV_NAME - 1)
                    vname[vi++] = *src++;
                if (*src == '}') src++;
                vname[vi] = '\0';
            } else {
                while (*src && (*src == '_' ||
                       (*src >= 'A' && *src <= 'Z') ||
                       (*src >= 'a' && *src <= 'z') ||
                       (*src >= '0' && *src <= '9')) && vi < MAX_ENV_NAME - 1)
                    vname[vi++] = *src++;
                vname[vi] = '\0';
            }
            if (!vi) { if (di < max - 1) dst[di++] = '$'; continue; }
            /* Look up in local store first */
            const char *val = NULL;
            for (int i = 0; i < s->env_count; i++) {
                if (seq_ci(s->env_names[i], vname)) { val = s->env_values[i]; break; }
            }
            /* Fallback: read from ENV:<name> file */
            if (!val) {
                static char env_file_buf[MAX_ENV_VAL];
                char env_path[64];
                scopy(env_path, "ENV:", 64);
                scat(env_path, vname, 64);
                VfsFile fh;
                if (VFS_Open(&fh, env_path, VFS_READ)) {
                    uint32_t n = VFS_Read(&fh, (uint8_t *)env_file_buf, MAX_ENV_VAL - 1);
                    env_file_buf[n] = '\0';
                    VFS_Close(&fh);
                    val = env_file_buf;
                }
            }
            if (val) {
                while (*val && di < max - 1) dst[di++] = *val++;
            }
        } else {
            dst[di++] = *src++;
        }
    }
    dst[di] = '\0';
}

static void strip_trailing_ampersand(char *s)
{
    int len = slen(s);
    while (len > 0 && s[len - 1] == ' ') len--;
    if (len > 0 && s[len - 1] == '&') {
        len--;
        while (len > 0 && s[len - 1] == ' ') len--;
        s[len] = '\0';
    }
}

static void inst_dispatch(ShellInstance *s, const char *line)
{
    /* Expand $variables before anything else */
    char expanded_line[MAX_LINE_LEN];
    expand_vars(s, line, expanded_line, MAX_LINE_LEN);
    line = expanded_line;

    /* Echo prompt (skip when dispatching a queued background job) */
    if (!g_bg_running) {
        char echo_line[MAX_LINE_LEN];
        scopy(echo_line, s->cwd, MAX_LINE_LEN);
        scat(echo_line, "> ", MAX_LINE_LEN);
        scat(echo_line, line, MAX_LINE_LEN);
        inst_print(s, echo_line);
    }

    while (*line == ' ') line++;
    if (!*line) return;
    if (*line == ';') return; /* skip comment lines */
    g_redir.null = 0;

    /* Detect background operator (&) before redirect parsing */
    int bg = 0;
    {
        int len = slen(line);
        while (len > 0 && line[len - 1] == ' ') len--;
        bg = (len > 0 && line[len - 1] == '&');
    }

    /* Parse redirect operators out of line */
    char cmd_only[MAX_LINE_LEN];
    char redir_path[64];
    int redir_mode = parse_redirects(s, line, cmd_only, MAX_LINE_LEN,
                                     redir_path, 64);
    strip_trailing_ampersand(cmd_only);

    /* If background, enqueue the clean line and return immediately */
    if (bg) {
        char bg_line[MAX_LINE_LEN];
        scopy(bg_line, line, MAX_LINE_LEN);
        strip_trailing_ampersand(bg_line);
        bg_enqueue(s, bg_line);
        return;
    }

    /* stdin redirect (<): treat as: type <file>, pass content as stdin.
     * For simplicity: < just feeds the file content as if typed — currently
     * we support it by making type read from redir_path. */
    if (redir_mode == 3) {
        /* < redirect: run cmd with file as implicit first arg if no arg given */
        if (cmd_only[0]) {
            char augmented[MAX_LINE_LEN];
            scopy(augmented, cmd_only, MAX_LINE_LEN);
            scat(augmented, " ", MAX_LINE_LEN);
            scat(augmented, redir_path, MAX_LINE_LEN);
            run_cmd(s, augmented);
            check_failat(s);
        }
        return;
    }

    /* stdout redirect (> or >>) */
    if (redir_mode == 1 || redir_mode == 2) {
        g_redir.null = (redir_path[0] != '\0' && seq_ci(redir_path, "nil:"));
        if (!g_redir.null) {
            int flags = VFS_WRITE | VFS_CREATE | (redir_mode == 1 ? VFS_TRUNC : 0);
            if (!VFS_Open(&g_redir.fh, redir_path, flags)) {
                char msg[MAX_LINE_LEN];
                scopy(msg, "Cannot open for write: ", MAX_LINE_LEN);
                scat(msg, redir_path, MAX_LINE_LEN);
                inst_print(s, msg);
                return;
            }
            /* For append, seek to end */
            if (redir_mode == 2)
                VFS_Seek(&g_redir.fh, VFS_Size(&g_redir.fh));
        }

        g_redir.shell  = s;
        g_redir.active = 1;
        run_cmd(s, cmd_only);
        g_redir.active = 0;
        check_failat(s);
        if (!g_redir.null)
            VFS_Close(&g_redir.fh);
        return;
    }

    /* Pipe handling (|) */
    char pipe_segs[MAX_PIPE_SEGMENTS][MAX_LINE_LEN];
    int pipe_count = parse_pipes(cmd_only, pipe_segs);
    if (pipe_count > 1) {
        char pipe_files[MAX_PIPE_SEGMENTS][64];
        int start_idx = g_pipe_next_idx;
        for (int i = 0; i < pipe_count - 1; i++) {
            char path[64];
            scopy(path, "T:pipe", 64);
            char idx_str[8];
            uint_to_dec_s((uint32_t)g_pipe_next_idx++, idx_str, 8);
            scat(path, idx_str, 64);
            scopy(pipe_files[i], path, 64);
            if (!VFS_Open(&g_redir.fh, path, VFS_WRITE | VFS_CREATE | VFS_TRUNC)) {
                char msg[MAX_LINE_LEN];
                scopy(msg, "Cannot create pipe file: ", MAX_LINE_LEN);
                scat(msg, path, MAX_LINE_LEN);
                inst_print(s, msg);
                for (int j = 0; j < i; j++) VFS_Delete(pipe_files[j]);
                return;
            }
            g_redir.shell  = s;
            g_redir.active = 1;
            run_cmd(s, pipe_segs[i]);
            g_redir.active = 0;
            VFS_Close(&g_redir.fh);
        }
        /* Run last segment with pipe input */
        g_pipe_in_active = 1;
        scopy(g_pipe_in_file, pipe_files[pipe_count - 2], 64);
        run_cmd(s, pipe_segs[pipe_count - 1]);
        g_pipe_in_active = 0;
        check_failat(s);
        /* Clean up temp files */
        for (int i = 0; i < pipe_count - 1; i++) {
            VFS_Delete(pipe_files[i]);
        }
        g_pipe_next_idx = start_idx; /* reuse indices */
        return;
    }

    /* No redirect — normal execution */
    run_cmd(s, cmd_only);
    check_failat(s);
}

/* =========================================================================
 * Tab completion
 * ========================================================================= */

/* Copy at most max-1 chars of src into dst; NUL-terminate. */
static void tc_scopy(char *dst, const char *src, int max)
{
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* Case-insensitive prefix match: does entry start with pfx? */
static int tc_has_prefix(const char *entry, const char *pfx, int pfx_len)
{
    for (int i = 0; i < pfx_len; i++) {
        char a = entry[i], b = pfx[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (!a || a != b) return 0;
    }
    return 1;
}

/* Extend *common_len so that common[0..n-1] is the longest prefix shared by
 * common and candidate.  On first call set *common_len = -1. */
static void tc_extend_common(char *common, int *common_len, const char *candidate)
{
    int clen = 0; while (candidate[clen]) clen++;
    if (*common_len < 0) {
        /* First candidate — seed with full name */
        tc_scopy(common, candidate, MAX_INPUT);
        *common_len = clen;
        return;
    }
    /* Trim common to match candidate */
    int lim = *common_len < clen ? *common_len : clen;
    int i = 0;
    while (i < lim) {
        char a = common[i], b = candidate[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) break;
        i++;
    }
    common[i] = '\0';
    *common_len = i;
}

/*
 * inst_tab_complete — handle a Tab keypress on instance s.
 *
 * Strategy:
 *   - Extract the word currently under/before the cursor.
 *   - If it is the first (and only) token → command completion
 *       (shell builtins + native commands in C:).
 *   - Otherwise → filename completion relative to cwd.
 *   - Single match  → fill in the rest, append '/' (dir) or ' ' (file).
 *   - Multi  match  → print all matches, keep input unchanged.
 *   - No match      → bell (no-op for now; keep input unchanged).
 */
static void inst_tab_complete(ShellInstance *s)
{
    /* ---------------------------------------------------------------
     * Step 1: find word boundaries up to cursor position.
     * We only complete the token that ends at input_cur.
     * --------------------------------------------------------------- */
    int cur = s->input_cur;
    /* Walk backward to find start of the current word */
    int word_start = cur;
    while (word_start > 0 && s->input_buf[word_start - 1] != ' ')
        word_start--;

    /* The prefix typed so far */
    char prefix[MAX_INPUT + 1];
    int pfx_len = cur - word_start;
    if (pfx_len < 0) pfx_len = 0;
    if (pfx_len > MAX_INPUT) pfx_len = MAX_INPUT;
    for (int i = 0; i < pfx_len; i++) prefix[i] = s->input_buf[word_start + i];
    prefix[pfx_len] = '\0';

    /* Is this the first token? (no non-space chars before word_start) */
    int is_first_token = 1;
    for (int i = 0; i < word_start; i++) {
        if (s->input_buf[i] != ' ') { is_first_token = 0; break; }
    }

    /* Storage for matches — keep up to 64 names (MAX_INPUT+1 each) */
#define TC_MAX_MATCHES 64
    static char tc_matches[TC_MAX_MATCHES][MAX_INPUT + 1];
    int tc_count = 0;
    char tc_common[MAX_INPUT + 1];
    int  tc_common_len = -1;  /* -1 = not seeded yet */
    tc_common[0] = '\0';

    /* Length of the *name* portion of the prefix (after any dir separator).
     * For "S:St" the name portion is "St" (len=2); for bare "dir" it equals
     * pfx_len.  Filled in during the completion branch below. */
    int name_pfx_len_out = pfx_len;

    /* ---------------------------------------------------------------
     * Helper lambda (macro) — record one match candidate
     * --------------------------------------------------------------- */
#define TC_ADD(name) do { \
    if (tc_count < TC_MAX_MATCHES) { \
        tc_scopy(tc_matches[tc_count], (name), MAX_INPUT + 1); \
        tc_extend_common(tc_common, &tc_common_len, (name)); \
        tc_count++; \
    } \
} while(0)

    /* ---------------------------------------------------------------
     * Does the prefix contain a ':' or '/'? Then it is an explicit
     * path — skip command completion, go straight to file completion.
     * --------------------------------------------------------------- */
    int has_colon = 0;
    for (int i = 0; i < pfx_len; i++)
        if (prefix[i] == ':') { has_colon = 1; break; }

    if (is_first_token && !has_colon) {
        /* ===========================================================
         * Command completion
         * ===========================================================
         * Candidates: shell builtins + native command table.
         * =========================================================== */

        /* Shell builtins */
        const char *builtins[] = {
            "help", "cd", "alias", "unalias", "set", "unset", "path",
            "setenv", "unsetenv", "showconfig", NULL
        };
        for (int i = 0; builtins[i]; i++) {
            if (tc_has_prefix(builtins[i], prefix, pfx_len))
                TC_ADD(builtins[i]);
        }

        /* Native commands — iterate k_native_cmds via NativeCmd_Exists +
         * direct table walk.  We re-declare a minimal local table because
         * k_native_cmds is static to native_cmd.c.  The canonical list is
         * the same as k_native_cmds[] in native_cmd.c. */
        const char *natcmds[] = {
            "version","mem","libs","clear","reboot","dir","makedir",
            "delete","type","copy","rename","pwd","echo","protect","attr",
            "info","date","which","disks","fdisk","format","pointer","run",
            "assign","execute","loadwb","calculator","ifconfig","ping",
            "route","nslookup","ntpd","clock","grep","more", NULL
        };
        for (int i = 0; natcmds[i]; i++) {
            if (tc_has_prefix(natcmds[i], prefix, pfx_len))
                TC_ADD(natcmds[i]);
        }

    } else {
        /* ===========================================================
         * Filename completion
         * ===========================================================
         * Split prefix into a directory part and a name prefix.
         * e.g. "RAM:foo/bar" → dir="RAM:foo/" name_pfx="bar"
         *      "bar"         → dir=cwd        name_pfx="bar"
         * =========================================================== */

        /* Find last path separator in prefix */
        int sep_pos = -1;
        for (int i = pfx_len - 1; i >= 0; i--) {
            if (prefix[i] == '/' || prefix[i] == ':') { sep_pos = i; break; }
        }

        char dir_part[MAX_INPUT + 1];
        char name_pfx[MAX_INPUT + 1];
        int  name_pfx_len;

        if (sep_pos >= 0) {
            /* Include the separator in dir_part */
            for (int i = 0; i <= sep_pos; i++) dir_part[i] = prefix[i];
            dir_part[sep_pos + 1] = '\0';
            for (int i = sep_pos + 1; i < pfx_len; i++)
                name_pfx[i - sep_pos - 1] = prefix[i];
            name_pfx_len = pfx_len - sep_pos - 1;
            name_pfx[name_pfx_len] = '\0';
        } else {
            /* No separator — use cwd as directory */
            tc_scopy(dir_part, s->cwd, MAX_INPUT + 1);
            tc_scopy(name_pfx, prefix, MAX_INPUT + 1);
            name_pfx_len = pfx_len;
        }
        /* Expose to insertion code: only the name part was matched */
        name_pfx_len_out = name_pfx_len;

        /* Enumerate directory */
        RamFsNode *child = VFS_OpenDir(dir_part);
        while (child) {
            if (tc_has_prefix(child->name, name_pfx, name_pfx_len)) {
                /* Build the full candidate: dir_part + child->name [+ '/'] */
                char cand[MAX_INPUT + 1];
                /* For display we use just the name portion after dir_part */
                tc_scopy(cand, child->name, MAX_INPUT + 1);
                if (child->type == RAMFS_TYPE_DIR) {
                    int nl = 0; while (cand[nl]) nl++;
                    if (nl < MAX_INPUT) { cand[nl] = '/'; cand[nl+1] = '\0'; }
                }
                TC_ADD(cand);
            }
            child = child->next_sibling;
        }
    }

#undef TC_ADD

    /* ---------------------------------------------------------------
     * Act on results
     * --------------------------------------------------------------- */
    if (tc_count == 0) {
        /* No matches — nothing to do */
        return;
    }

    if (tc_count == 1 || (tc_common_len > name_pfx_len_out)) {
        /* Single match OR unambiguous common extension → complete in-place.
         *
         * 'fill' contains only the NAME portion of the completion
         * (e.g. "Startup-Sequence"), NOT the directory prefix ("S:").
         * name_pfx_len_out is how many chars of that name the user already
         * typed (e.g. 2 for "St"), so we insert fill[name_pfx_len_out..].
         */
        const char *fill = (tc_count == 1) ? tc_matches[0] : tc_common;
        int fill_len = 0; while (fill[fill_len]) fill_len++;

        /* How many chars to insert: remainder of name after the typed portion */
        int insert_len = fill_len - name_pfx_len_out;

        /* Make room in input_buf at cursor position */
        if (s->input_len + insert_len > MAX_INPUT)
            insert_len = MAX_INPUT - s->input_len;
        if (insert_len <= 0) goto show_all;

        /* Shift tail right */
        for (int i = s->input_len; i >= cur; i--)
            s->input_buf[i + insert_len] = s->input_buf[i];

        /* Fill the gap with the new characters */
        for (int i = 0; i < insert_len; i++)
            s->input_buf[cur + i] = fill[name_pfx_len_out + i];

        s->input_len += insert_len;
        s->input_cur += insert_len;
        s->input_buf[s->input_len] = '\0';

        /* For a single unambiguous match, append a trailing space or '/'
         * (unless already there or unless we already ended with '/') */
        if (tc_count == 1) {
            char last = s->input_buf[s->input_cur - 1];
            if (last != ' ' && last != '/' && s->input_len < MAX_INPUT) {
                /* Shift tail right by 1 */
                for (int i = s->input_len; i >= s->input_cur; i--)
                    s->input_buf[i + 1] = s->input_buf[i];
                s->input_buf[s->input_cur] = ' ';
                s->input_len++;
                s->input_cur++;
                s->input_buf[s->input_len] = '\0';
            }
        }
        return;
    }

show_all:
    /* Multiple ambiguous matches — print them, leave input unchanged */
    {
        /* Print a blank line then all matches on one line separated by spaces */
        char line_buf[MAX_LINE_LEN];
        line_buf[0] = '\0';
        int li = 0;
        for (int i = 0; i < tc_count && li < MAX_LINE_LEN - 2; i++) {
            if (i > 0 && li < MAX_LINE_LEN - 2) { line_buf[li++] = ' '; line_buf[li] = '\0'; }
            int nl = 0; while (tc_matches[i][nl] && li < MAX_LINE_LEN - 1) {
                line_buf[li++] = tc_matches[i][nl++];
            }
            line_buf[li] = '\0';
        }
        inst_print(s, line_buf);
    }
}

/* =========================================================================
 * Key handler (operates on a specific instance)
 * ========================================================================= */

static void inst_handle_key(ShellInstance *s, char c)
{
    if (!g_fb.valid) return;

    /* Skip normal handling in special modes (handled by their own loops) */
    if (s->vim_mode) {
        VimWin_KeyInline(s->vim_slot, c);
        if (!VimWin_IsActive(s->vim_slot)) {
            s->vim_mode = 0;
            s->vim_slot = -1;
        }
        inst_draw_contents(s);
        inst_draw_history(s);
        inst_draw_input(s);
        return;
    }
    if (s->ed_mode) {
        EdWin_KeyInline(s->ed_slot, c);
        if (!EdWin_IsActive(s->ed_slot)) {
            s->ed_mode = 0;
            s->ed_slot = -1;
        }
        inst_draw_contents(s);
        inst_draw_history(s);
        inst_draw_input(s);
        return;
    }

    /* Ask mode handles input in its own polling loop - skip here */
    if (s->ask_mode) {
        return;
    }

    if (c == VKEY_PGUP) {
        if (s->wm_handle >= 0) {
            int sy = WM_GetScrollY(s->wm_handle);
            sy -= SCROLL_LINES * 16;
            if (sy < 0) sy = 0;
            WM_SetScrollY(s->wm_handle, sy);
        }
        inst_draw_history(s);
        return;
    }
    if (c == VKEY_PGDN) {
        if (s->wm_handle >= 0) {
            int rows = inst_rows(s);
            int sy = WM_GetScrollY(s->wm_handle);
            int max_sy = (s->hist_count - rows) * 16;
            if (max_sy < 0) max_sy = 0;
            sy += SCROLL_LINES * 16;
            if (sy > max_sy) sy = max_sy;
            WM_SetScrollY(s->wm_handle, sy);
        }
        inst_draw_history(s);
        return;
    }
    if (c == '\t') {
        if (!s->fdisk_mode) {
            inst_tab_complete(s);
            inst_draw_history(s);
            inst_draw_input(s);
        }
        return;
    }
    if (c == VKEY_LEFT) {
        if (s->input_cur > 0) s->input_cur--;
        inst_draw_input(s);
        return;
    }
    if (c == VKEY_RIGHT) {
        if (s->input_cur < s->input_len) s->input_cur++;
        inst_draw_input(s);
        return;
    }
    if (c == VKEY_UP) {
        if (s->cmd_hist_count == 0) return;
        if (s->cmd_hist_nav == 0)
            scopy(s->input_saved, s->input_buf, MAX_INPUT + 1); /* save live input */
        if (s->cmd_hist_nav < s->cmd_hist_count)
            s->cmd_hist_nav++;
        int idx = (s->cmd_hist_count - s->cmd_hist_nav) % MAX_CMD_HIST;
        scopy(s->input_buf, s->cmd_hist[idx], MAX_INPUT + 1);
        s->input_len = 0;
        while (s->input_buf[s->input_len]) s->input_len++;
        s->input_cur = s->input_len;
        inst_draw_input(s);
        return;
    }
    if (c == VKEY_DOWN) {
        if (s->cmd_hist_nav == 0) return;
        s->cmd_hist_nav--;
        if (s->cmd_hist_nav == 0) {
            scopy(s->input_buf, s->input_saved, MAX_INPUT + 1);
        } else {
            int idx = (s->cmd_hist_count - s->cmd_hist_nav) % MAX_CMD_HIST;
            scopy(s->input_buf, s->cmd_hist[idx], MAX_INPUT + 1);
        }
        s->input_len = 0;
        while (s->input_buf[s->input_len]) s->input_len++;
        s->input_cur = s->input_len;
        inst_draw_input(s);
        return;
    }
    if (c == '\n' || c == '\r') {
        s->input_buf[s->input_len] = 0;
        if (s->fdisk_mode) {
            /* Fdisk interactive mode */
            g_fdisk_shell = s;
            fdisk_handle_cmd(s, s->input_buf);
            g_fdisk_shell = NULL;
            if (s->fdisk_mode) {
                inst_print(s, "Command (m for help):");
            }
            s->input_len = 0;
            s->input_cur = 0;
            s->input_buf[0] = 0;
        } else {
            /* Save non-empty command to cmd_hist */
            if (s->input_len > 0) {
                int slot = s->cmd_hist_count % MAX_CMD_HIST;
                scopy(s->cmd_hist[slot], s->input_buf, MAX_INPUT + 1);
                s->cmd_hist_count++;
            }
            s->cmd_hist_nav = 0;
            s->input_saved[0] = 0;
            inst_dispatch(s, s->input_buf);
            s->input_len = 0;
            s->input_cur = 0;
            s->input_buf[0] = 0;
            /* Commands may open/raise windows (e.g. Calculator, LoadWB).
             * Do a full WM redraw to avoid shell direct-draw overwriting
             * other windows' pixels. */
            WM_Redraw();
            return;
        }
    } else if (c == '\b') {
        s->cmd_hist_nav = 0;
        if (s->input_cur > 0) {
            /* Delete char before cursor */
            int i = s->input_cur - 1;
            while (i < s->input_len - 1) {
                s->input_buf[i] = s->input_buf[i+1]; i++;
            }
            s->input_len--;
            s->input_cur--;
            s->input_buf[s->input_len] = 0;
        }
    } else if (c >= 0x20 && c < 0x7F) {
        s->cmd_hist_nav = 0;
        if (s->input_len < MAX_INPUT) {
            /* Insert at cursor */
            for (int i = s->input_len; i > s->input_cur; i--)
                s->input_buf[i] = s->input_buf[i-1];
            s->input_buf[s->input_cur] = c;
            s->input_len++;
            s->input_cur++;
            s->input_buf[s->input_len] = 0;
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
    int old_ww = s->ww, old_wh = s->wh; \
    s->wx=wx;s->wy=wy;s->ww=ww;s->wh=wh; \
    if (s->wm_handle >= 0 && (ww != old_ww || wh != old_wh)) { \
        /* Window resized — update WM content size for proper scrollbar thumb */ \
        inst_update_scrollinfo(s); \
    } \
    if (s->auto_scroll) { \
        s->auto_scroll = 0; \
        if (s->wm_handle >= 0) { \
            int rows2    = inst_rows(s); \
            int view_h2  = rows2 * 16; \
            int cont_h2  = s->hist_count * 16; \
            if (cont_h2 < view_h2) cont_h2 = view_h2; \
            WM_SetScrollInfoEx(s->wm_handle, 0, cont_h2, view_h2); \
            int from_top2 = s->hist_count - rows2; \
            if (from_top2 < 0) from_top2 = 0; \
            WM_SetScrollY(s->wm_handle, from_top2 * 16); \
        } \
    } \
    inst_draw_contents(s); inst_draw_history(s); inst_draw_input(s); } \
static void shell_key_##N(char c) { shell_kb_enqueue(&g_shells[N], c); }

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
 * Shell task entry — each shell window runs as a native scheduled task
 * ========================================================================= */

static void shell_task_entry(void *arg)
{
    ShellInstance *s = (ShellInstance *)arg;
    for (;;) {
        char c;
        if (shell_kb_dequeue(s, &c)) {
            inst_handle_key(s, c);
        }
        Task_Yield();
    }
}

/* =========================================================================
 * Internal: open one shell instance
 * ========================================================================= */

static ShellInstance *open_shell(int stagger)
{
    if (g_n_shells >= MAX_SHELLS) return NULL;
    if (!g_fb.valid) return NULL;

    int idx = g_n_shells++;
    ShellInstance *s = &g_shells[idx];

    s->wm_handle  = -1;
    s->number     = idx + 1;
    s->index      = idx;
    s->wx         = 24 + stagger * 28;
    s->wy         = MENUBAR_H + stagger * 28;
    s->ww         = 600;
    s->wh         = 340;
    s->hist_count = 0;
    s->hist_scroll= 0;
    s->input_len  = 0;
    s->input_buf[0] = 0;
    s->fdisk_mode = 0;
    s->fdisk_dev  = NULL;
    memset(&s->fdisk_pt, 0, sizeof(PartitionTable));
    s->vim_mode = 0;
    s->vim_slot = -1;
    s->ed_mode = 0;
    s->ed_slot = -1;
    s->ask_mode = 0;
    s->ask_prompt[0] = '\0';
    s->ask_result[0] = '\0';
    s->ask_result_ready = 0;
    s->custom_prompt[0] = '\0';
    s->last_rc = 0;
    s->failat_threshold = 10;
    s->quit_flag = 0;
    scopy(s->cwd, "RAM:", 64);
    /* Default AmigaDOS-style search path */
    /* Default AmigaDOS search path.  SYS: is the boot volume root,
     * so SYS:Tools resolves to Workbench:Tools/ etc. */
    scopy(s->path, "C: S: SYS:Tools SYS:Utilities SYS:Prefs", 256);
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
    s->kb_head = 0;
    s->kb_tail = 0;
    Task_CreateNative("Shell", -128, shell_task_entry, s);
    WM_Redraw();
    return s;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void ShellWin_Init(void)
{
    Resident_Init();
    open_shell(0);
}

void ShellWin_Open(void)
{
    ShellWin_OpenWithScript(NULL);
}

void ShellWin_OpenWithScript(const char *script_path)
{
    ShellInstance *s = NULL;

    /* Find a free slot — also allow re-use of a closed slot */
    for (int i = 0; i < g_n_shells; i++) {
        if (!WM_IsWindowActive(g_shells[i].wm_handle)) {
            /* Reclaim this slot */
            g_n_shells = i;
            s = open_shell(i);
            break;
        }
    }
    if (!s) s = open_shell(g_n_shells);
    if (!s) return;

    /* Optionally execute a startup script in the new shell, mirroring
     * the S:Startup-Sequence path used at boot.  The script runs
     * synchronously from the caller's task context — the new shell's
     * own task is idle (no keys queued) until we return, so there is
     * no contention on the shell instance state. */
    if (!script_path || !*script_path) return;

    VfsFile fh;
    if (!VFS_Open(&fh, script_path, VFS_READ)) {
        inst_print(s, "newcli: cannot open startup script");
        return;
    }

    uint32_t size = VFS_Size(&fh);
    if (size == 0 || size >= MAX_SCRIPT_SIZE) {
        inst_print(s, "newcli: startup script empty or too large (max 4KB)");
        VFS_Close(&fh);
        return;
    }

    char *buf = script_acquire_buf();
    if (!buf) {
        inst_print(s, "newcli: script nesting too deep");
        VFS_Close(&fh);
        return;
    }

    uint32_t nread = VFS_Read(&fh, (uint8_t *)buf, size);
    buf[nread] = '\0';
    VFS_Close(&fh);

    inst_print(s, "Executing startup script...");
    run_script_text(s, buf);
    script_release_buf();
    inst_print(s, "Startup script complete.");
}

void ShellWin_Redraw(void)
{
    if (!g_fb.valid) return;
    WM_Redraw();
}

void ShellWin_PollJobs(void)
{
    bg_run_next();
}

void ShellWin_ListJobs(void *shell, void (*print)(void *, const char *))
{
    if (g_bg_job_count == 0) {
        print(shell, "No active jobs.");
        return;
    }
    print(shell, "Job  Status  Command");
    print(shell, "-----------------------------");
    for (int i = 0; i < g_bg_job_count; i++) {
        char line[MAX_LINE_LEN];
        line[0] = '\0';
        char num[8];
        uint_to_dec_s((uint32_t)g_bg_jobs[i].number, num, 8);
        scat(line, num, MAX_LINE_LEN);
        scat(line, "    ", MAX_LINE_LEN);
        scat(line, g_bg_jobs[i].active ? "run   " : "queue ", MAX_LINE_LEN);
        scat(line, g_bg_jobs[i].cmd, MAX_LINE_LEN);
        print(shell, line);
    }
}

void ShellWin_HandleKey(char c)
{
    /* Legacy entry point — route to the focused window's key buffer */
    int focus = WM_GetFocus();
    for (int i = 0; i < g_n_shells; i++) {
        if (g_shells[i].wm_handle == focus) {
            shell_kb_enqueue(&g_shells[i], c);
            return;
        }
    }
}

/* =========================================================================
 * Startup-Sequence execution
 * ========================================================================= */

void ShellWin_RunStartupSequence(void)
{
    if (g_n_shells == 0) return;
    ShellInstance *s = &g_shells[0];

    inst_print(s, "Executing S:Startup-Sequence...");

    VfsFile fh;
    if (!VFS_Open(&fh, "S:Startup-Sequence", VFS_READ)) {
        inst_print(s, "S:Startup-Sequence not found.");
        return;
    }

    uint32_t size = VFS_Size(&fh);
    if (size == 0 || size >= MAX_SCRIPT_SIZE) {
        inst_print(s, "Startup-Sequence empty or too large.");
        VFS_Close(&fh);
        return;
    }

    char *buf = script_acquire_buf();
    if (!buf) {
        inst_print(s, "Startup-Sequence nesting too deep");
        VFS_Close(&fh);
        return;
    }

    uint32_t nread = VFS_Read(&fh, (uint8_t *)buf, size);
    buf[nread] = '\0';
    VFS_Close(&fh);

    /* Pre-populate the startup variables described in the manual.
     * Startup-Sequence later uses SetEnv/UnSet on these. */
    script_set_var(s, "Workbench", "Workbench:");
    script_set_var(s, "Kickstart", "47.1");

    run_script_text(s, buf);
    script_release_buf();
    inst_print(s, "Startup-Sequence complete.");
}

void ShellWin_DispatchLine(const char *line)
{
    if (g_n_shells == 0 || !line || !*line) return;
    ShellInstance *s = &g_shells[0];
    inst_dispatch(s, line);
}
