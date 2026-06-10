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
#include "../../emulation/uaos_emu.h"
#include "dos/vfs.h"
#include "dos/ramfs.h"
#include "dos/blockdev.h"
#include "dos/partition.h"
#include "dos/fat32.h"
#include "exec/rom_modules.h"
#include "shell/native_cmd.h"
#include "exec/uaos_binary.h"
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Forward declarations */
typedef struct ShellInstance ShellInstance;
static void inst_print(ShellInstance *s, const char *line);
static void inst_dispatch(ShellInstance *s, const char *line);
static NativeCmdCtx shell_make_ctx(ShellInstance *s);
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
#define BORDER_L        1                  /* left: just the outline */
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
};
typedef struct ShellInstance ShellInstance;

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

    /* When not scrolled up, always pin to the bottom regardless of geometry
     * timing — avoids missing last lines due to stale wh/scroll_y mismatch. */
    int start;
    if (s->hist_scroll == 0) {
        start = s->hist_count - rows;
    } else {
        start = s->hist_count - rows - s->hist_scroll;
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

static void inst_draw_input(ShellInstance *s)
{
    int wx=s->wx, wy=s->wy, ww=s->ww, wh=s->wh;
    int ix = wx + BORDER_L + 4;
    int iy = wy + wh - INPUTBAR_H - WM_SCROLLBAR_W - 2;

    /* Build Amiga-style prompt from current volume */
    char vol[32];
    extract_vol_prompt(s->cwd, vol, sizeof(vol));
    /* Show just the volume name without the number prefix */
    char prompt[40];
    int pi = 0;
    /* Copy volume name */
    int vi = 0;
    while (vol[vi] && pi < 35) {
        prompt[pi++] = vol[vi++];
    }
    /* Add "> " */
    if (pi < 38) { prompt[pi++] = '>'; prompt[pi++] = ' '; }
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
    int content_h = s->hist_count * 16;
    int view_h = rows * 16;
    /* Ensure content_h >= view_h so scrollbar range is valid */
    WM_SetScrollInfo(s->wm_handle, 0, content_h > view_h ? content_h : view_h);
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
typedef struct { void *shell; VfsFile fh; int active; } RedirCtx;
static RedirCtx g_redir;

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

static inline void _ser_putc(char c) {
    while ((inb(0x3F8 + 5) & 0x20) == 0) {}
    outb(0x3F8, (uint8_t)c);
    if (c == '\n') { _ser_putc('\r'); }
}
static inline void _ser_puts(const char *s) {
    while (*s) _ser_putc(*s++);
}

static void inst_print(ShellInstance *s, const char *line)
{
    /* Also mirror to serial port so QEMU -serial stdio captures emu traces */
    _ser_puts(line);
    _ser_putc('\n');

    /* If stdout is redirected, write to file instead of shell history */
    if (g_redir.active) {
        VFS_Write(&g_redir.fh, (const uint8_t *)line, (uint32_t)slen(line));
        uint8_t nl = '\n';
        VFS_Write(&g_redir.fh, &nl, 1);
        return;
    }
    int slot = s->hist_count % MAX_HIST_LINES;
    scopy(g_hist_buf[s->index][slot], line, MAX_LINE_LEN);
    s->hist_count++;
    s->hist_scroll = 0;
    s->auto_scroll = 1;  /* draw shim will pin to bottom with fresh geometry */
    /* Update WM content size so scrollbar thumb adapts to new content */
    inst_update_scrollinfo(s);
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
    inst_print(s, "  execute <script>   run a script file");
    inst_print(s, "  loadwb             launch Workbench desktop");
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
 * If arg already contains ':', treat as absolute. */
static void make_abs_path(ShellInstance *s, const char *arg,
                           char *out, int max)
{
    /* Check for volume prefix (contains ':') */
    const char *p = arg;
    while (*p && *p != ':') p++;
    if (*p == ':') {
        scopy(out, arg, max);
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
        inst_print(s, "Usage: assign <name>: <target>");
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

    /* Extract target */
    int ti = 0;
    while (*p && ti < 63) {
        target[ti++] = *p++;
    }
    target[ti] = '\0';

    if (!name[0] || !target[0]) {
        inst_print(s, "Usage: assign <name>: <target>");
        inst_print(s, "Example: assign C: Workbench:C");
        return;
    }

    /* Add the assign */
    if (VFS_AddAssign(name, target) == 0) {
        char msg[MAX_LINE_LEN];
        scopy(msg, "Assigned ", MAX_LINE_LEN);
        scat(msg, name, MAX_LINE_LEN);
        scat(msg, " -> ", MAX_LINE_LEN);
        scat(msg, target, MAX_LINE_LEN);
        inst_print(s, msg);
    } else {
        inst_print(s, "Failed to create assign.");
    }
}

/* Static buffer for script execution (max 4KB scripts) */
#define MAX_SCRIPT_SIZE 4096
static char g_script_buf[MAX_SCRIPT_SIZE];

static void inst_cmd_execute(ShellInstance *s, const char *arg)
{
    if (!arg || !*arg) {
        inst_print(s, "Usage: execute <script>");
        inst_print(s, "Executes a script file line by line.");
        return;
    }

    /* Parse script path */
    char path[64];
    make_abs_path(s, arg, path, 64);

    /* Open the script file */
    VfsFile fh;
    if (!VFS_Open(&fh, path, VFS_READ)) {
        char msg[MAX_LINE_LEN];
        scopy(msg, "Cannot open: ", MAX_LINE_LEN);
        scat(msg, path, MAX_LINE_LEN);
        inst_print(s, msg);
        return;
    }

    /* Read script into static buffer */
    uint32_t size = VFS_Size(&fh);
    if (size == 0 || size >= MAX_SCRIPT_SIZE) {
        inst_print(s, "Script empty or too large (max 4KB)");
        VFS_Close(&fh);
        return;
    }

    uint32_t read = VFS_Read(&fh, (uint8_t *)g_script_buf, size);
    g_script_buf[read] = '\0';
    VFS_Close(&fh);

    /* Execute script line by line */
    char line[MAX_LINE_LEN];
    const char *p = g_script_buf;
    int line_num = 0;

    inst_print(s, "Executing script...");

    while (*p) {
        /* Extract one line */
        int li = 0;
        while (*p && *p != '\n' && li < MAX_LINE_LEN - 1) {
            line[li++] = *p++;
        }
        line[li] = '\0';
        if (*p == '\n') p++; /* skip newline */

        line_num++;

        /* Skip empty lines and comments */
        const char *lp = line;
        while (*lp == ' ') lp++;
        if (*lp == '\0' || *lp == ';' || *lp == '*') continue;

        /* Execute the line */
        inst_dispatch(s, line);
    }

    char msg[32];
    scopy(msg, "Script complete (", 32);
    char num[8];
    uint_to_dec_s((uint32_t)line_num, num, 8);
    scat(msg, num, 32);
    scat(msg, " lines)", 32);
    inst_print(s, msg);
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

/* Launch Workbench desktop */
static void shell_loadwb(void)
{
    Desktop_MarkWorkbenchLoaded();
    Desktop_Draw();
    WM_Redraw();
}

/* Clear shell history buffer */
static void shell_clear_history(void *shell_extra)
{
    ShellInstance *s = (ShellInstance *)shell_extra;
    s->hist_count  = 0;
    s->hist_scroll = 0;
    for (int i = 0; i < MAX_HIST_LINES; i++) g_hist_buf[s->index][i][0] = 0;
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
        "setenv", "unsetenv", NULL
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

/* Build a NativeCmdCtx for the given shell instance */
static NativeCmdCtx shell_make_ctx(ShellInstance *s)
{
    NativeCmdCtx ctx;
    ctx.shell          = s;
    ctx.print          = (void (*)(void *, const char *))inst_print;
    ctx.cwd            = s->cwd;
    ctx.path           = s->path;
    ctx.shell_extra    = s;
    ctx.set_fdisk_mode = shell_set_fdisk_mode;
    ctx.loadwb         = shell_loadwb;
    ctx.clear_history  = shell_clear_history;
    ctx.is_builtin     = shell_is_builtin;
    ctx.dispatch_line  = shell_dispatch_line;
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

/* Static payload buffer for M68K binaries loaded from VFS (max 512 KB) */
#define UAOS_MAX_BIN_PAYLOAD (512 * 1024)
static uint8_t g_bin_payload[UAOS_MAX_BIN_PAYLOAD];

static int inst_exec_uaos_bin(ShellInstance *s, const char *full_path,
                               const char *args)
{
    VfsFile fh;
    if (!VFS_Open(&fh, full_path, VFS_READ)) return -1;

    uint32_t file_size = VFS_Size(&fh);

    /* --- Try to read UAOS header --- */
    if (file_size >= UAOS_BIN_HEADER_SIZE) {
        uint8_t hdr[UAOS_BIN_HEADER_SIZE];
        if (VFS_Read(&fh, hdr, UAOS_BIN_HEADER_SIZE) == UAOS_BIN_HEADER_SIZE
            && uaos_bin_check_magic(hdr)) {

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
                static const char *m68k_argv[18];
                static char m68k_argstore[256];
                m68k_argv[0] = bin_name;
                int argc = 1;
                if (args && *args) {
                    /* Copy args into mutable store and split on spaces */
                    int ai = 0;
                    while (*args && ai < 254) m68k_argstore[ai++] = *args++;
                    m68k_argstore[ai] = '\0';
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
                UAOS_Emu_SetCwd(s->cwd);
                UAOS_Emu_LoadAndRun(g_bin_payload, payload_size,
                                    m68k_argv, s, (UAOS_PrintFn)inst_print);
                return 0;
            }

            /* Unknown type */
            VFS_Close(&fh);
            inst_print(s, "Unknown UAOS binary type");
            return -2;
        }
        /* Not a UAOS binary — rewind and fall through to script execution */
        VFS_Seek(&fh, 0);
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
        "setenv", "unsetenv",
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

/* Expand $VarName references in src into dst[max].  Reads local env store.
 * Also reads ENV:<name> file as fallback for vars not in local store. */
static void expand_vars(ShellInstance *s, const char *src, char *dst, int max)
{
    int di = 0;
    while (*src && di < max - 1) {
        if (*src == '$') {
            src++;
            char vname[MAX_ENV_NAME];
            int vi = 0;
            while (*src && (*src == '_' ||
                   (*src >= 'A' && *src <= 'Z') ||
                   (*src >= 'a' && *src <= 'z') ||
                   (*src >= '0' && *src <= '9')) && vi < MAX_ENV_NAME - 1)
                vname[vi++] = *src++;
            vname[vi] = '\0';
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

static void inst_dispatch(ShellInstance *s, const char *line)
{
    /* Expand $variables before anything else */
    char expanded_line[MAX_LINE_LEN];
    expand_vars(s, line, expanded_line, MAX_LINE_LEN);
    line = expanded_line;

    /* Echo prompt */
    char echo_line[MAX_LINE_LEN];
    scopy(echo_line, s->cwd, MAX_LINE_LEN);
    scat(echo_line, "> ", MAX_LINE_LEN);
    scat(echo_line, line, MAX_LINE_LEN);
    inst_print(s, echo_line);

    while (*line == ' ') line++;
    if (!*line) return;
    if (*line == ';') return; /* skip comment lines */

    /* Parse redirect operators out of line */
    char cmd_only[MAX_LINE_LEN];
    char redir_path[64];
    int redir_mode = parse_redirects(s, line, cmd_only, MAX_LINE_LEN,
                                     redir_path, 64);

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
        }
        return;
    }

    /* stdout redirect (> or >>) */
    if (redir_mode == 1 || redir_mode == 2) {
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

        g_redir.shell  = s;
        g_redir.active = 1;
        run_cmd(s, cmd_only);
        g_redir.active = 0;
        VFS_Close(&g_redir.fh);
        return;
    }

    /* No redirect — normal execution */
    run_cmd(s, cmd_only);
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
        inst_push_scroll_to_wm(s);
        inst_draw_history(s);
        return;
    }
    if (c == VKEY_PGDN) {
        s->hist_scroll -= SCROLL_LINES;
        if (s->hist_scroll < 0) s->hist_scroll = 0;
        inst_push_scroll_to_wm(s);
        inst_draw_history(s);
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
        /* New output arrived — pin to bottom using fresh geometry */ \
        s->hist_scroll = 0; \
        s->auto_scroll = 0; \
        if (s->wm_handle >= 0) { \
            int rows2 = inst_rows(s); \
            int from_top2 = s->hist_count - rows2; \
            if (from_top2 < 0) from_top2 = 0; \
            WM_SetScrollY(s->wm_handle, from_top2 * 16); \
        } \
    } else if (s->wm_handle >= 0) { \
        /* Sync from WM scroll_y to reflect user scrollbar interaction */ \
        inst_sync_from_wm(s); \
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
    s->fdisk_mode = 0;
    s->fdisk_dev  = NULL;
    memset(&s->fdisk_pt, 0, sizeof(PartitionTable));
    scopy(s->cwd, "RAM:", 64);
    /* Default AmigaDOS-style search path */
    scopy(s->path, "C: S: SYS:Utilities SYS:Rexx SYS:System SYS:Prefs SYS:WBStartup SYS:Tools SYS:Tools/Commodities", 256);
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

/* =========================================================================
 * Startup-Sequence execution
 * ========================================================================= */

void ShellWin_RunStartupSequence(void)
{
    if (g_n_shells == 0) return;
    ShellInstance *s = &g_shells[0];

    inst_print(s, "Executing S:Startup-Sequence...");

    /* Debug: show what S: resolves to */
    char resolved[128];
    const char *assign_target = VFS_ResolveAssign("S");
    if (assign_target) {
        inst_print(s, "[Debug] S: assign target found");
    } else {
        inst_print(s, "[Debug] S: assign NOT found - checking assigns...");
        char buf[256];
        int n = VFS_ListAssigns(buf, sizeof(buf));
        if (n > 0) {
            inst_print(s, "[Debug] Current assigns:");
            inst_print(s, buf);
        } else {
            inst_print(s, "[Debug] No assigns defined!");
        }
    }

    VfsFile fh;
    if (!VFS_Open(&fh, "S:Startup-Sequence", VFS_READ)) {
        inst_print(s, "S:Startup-Sequence not found.");
        return;
    }

    char line[256];
    int pos = 0;
    for (;;) {
        uint8_t c;
        int r = VFS_Read(&fh, &c, 1);
        if (r <= 0) {
            /* EOF — dispatch last line */
            if (pos > 0) {
                line[pos] = '\0';
                inst_dispatch(s, line);
            }
            break;
        }
        if (c == '\n' || c == '\r') {
            if (pos > 0) {
                line[pos] = '\0';
                inst_dispatch(s, line);
            }
            pos = 0;
        } else if (pos < 255) {
            line[pos++] = (char)c;
        }
    }

    VFS_Close(&fh);
    inst_print(s, "Startup-Sequence complete.");
}
