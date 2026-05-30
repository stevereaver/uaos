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
#include "pointer_prefs.h"
#include "wm.h"
#include "../../emulation/uaos_emu.h"
#include "dos/vfs.h"
#include "exec/rom_modules.h"
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

/* Special virtual keys injected by kbd driver for scroll/history */
#define VKEY_PGUP  0x01
#define VKEY_PGDN  0x02
#define VKEY_UP    0x03
#define VKEY_DOWN  0x04
#define VKEY_LEFT  0x05
#define VKEY_RIGHT 0x06

#define MAX_CMD_HIST  64   /* command history entries per shell */

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

    /* Command history (up/down arrow recall) */
    char cmd_hist[MAX_CMD_HIST][MAX_INPUT + 1];
    int  cmd_hist_count;  /* total commands entered */
    int  cmd_hist_nav;    /* navigation offset: 0 = live input, 1 = last cmd */

    /* Current working directory (AmigaDOS path, e.g. "RAM:") */
    char cwd[64];

    /* Input */
    char input_buf[MAX_INPUT + 1];
    char input_saved[MAX_INPUT + 1]; /* saved live input while navigating */
    int  input_len;
    int  input_cur;    /* cursor position within input_buf, 0..input_len */
    int  auto_scroll;   /* 1 = pin to bottom on next draw (set by inst_print) */
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
    int new_sy = from_top * 16;
    WM_SetScrollY(s->wm_handle, new_sy);
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
    WM_Redraw();
}

static void inst_cmd_help(ShellInstance *s)
{
    inst_print(s, "UAOS Shell v0.1 - built-in commands:");
    inst_print(s, "  help               show this help");
    inst_print(s, "  version            show OS version");
    inst_print(s, "  mem                memory information");
    inst_print(s, "  libs               show loaded kernel libraries");
    inst_print(s, "  clear              clear the shell window");
    inst_print(s, "  reboot             warm reboot");
    inst_print(s, "  dir [path]         list directory");
    inst_print(s, "  cd [path]          change/show directory");
    inst_print(s, "  makedir <path>     create directory");
    inst_print(s, "  delete <path>      delete file or empty dir");
    inst_print(s, "  type <file>        print file contents");
    inst_print(s, "  copy <src> <dst>   copy file");
    inst_print(s, "  pwd                print working directory");
    inst_print(s, "  echo <text>         print text to shell");
    inst_print(s, "  pointer            open pointer preferences");
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
        int n = VFS_Read(&fsrc, buf, 256);
        if (n <= 0) break;
        VFS_Write(&fdst, buf, n);
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

static void inst_cmd_pointer(ShellInstance *s)
{
    (void)s;
    PointerPrefs_Show();
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
 * Dispatch
 * ========================================================================= */

static void run_cmd(ShellInstance *s, const char *line)
{
    const char *cmds[] = {
        "help","version","mem","libs","clear","reboot","run",
        "dir","cd","makedir","delete","type","copy","pwd","echo","pointer",
        NULL
    };

    for (int i = 0; cmds[i]; i++) {
        const char *c = cmds[i];
        int cl = slen(c);
        if (!cmd_match(line, c, cl)) continue;

        const char *args = line + cl;
        while (*args == ' ') args++;

        if (i==0) inst_cmd_help(s);
        else if (i==1) inst_cmd_version(s);
        else if (i==2) inst_cmd_mem(s);
        else if (i==3) inst_cmd_libs(s);
        else if (i==4) inst_cmd_clear(s);
        else if (i==5) inst_cmd_reboot(s);
        else if (i==6) { UAOS_Emu_SetCwd(s->cwd); UAOS_Emu_RunByName(args, s, (UAOS_PrintFn)inst_print); }
        else if (i==7) inst_cmd_dir(s, args);
        else if (i==8) inst_cmd_cd(s, args);
        else if (i==9) inst_cmd_makedir(s, args);
        else if (i==10) inst_cmd_delete(s, args);
        else if (i==11) inst_cmd_type(s, args);
        else if (i==12) inst_cmd_copy(s, args);
        else if (i==13) inst_print(s, s->cwd);
        else if (i==14) inst_print(s, *args ? args : "");
        else if (i==15) inst_cmd_pointer(s);
        return;
    }

    char msg[MAX_LINE_LEN];
    scopy(msg, "Unknown command: ", MAX_LINE_LEN);
    scat(msg, line, MAX_LINE_LEN);
    inst_print(s, msg);
}

static void inst_dispatch(ShellInstance *s, const char *line)
{
    /* Echo prompt */
    char echo_line[MAX_LINE_LEN];
    scopy(echo_line, s->cwd, MAX_LINE_LEN);
    scat(echo_line, "> ", MAX_LINE_LEN);
    scat(echo_line, line, MAX_LINE_LEN);
    inst_print(s, echo_line);

    while (*line == ' ') line++;
    if (!*line) return;

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
    s->wx=wx;s->wy=wy;s->ww=ww;s->wh=wh; \
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
        /* Scrollbar thumb may have moved — sync hist_scroll from scroll_y */ \
        int sy = WM_GetScrollY(s->wm_handle); \
        int rows2 = inst_rows(s); \
        int from_top2 = sy / 16; \
        int hs = s->hist_count - rows2 - from_top2; \
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
    scopy(s->cwd, "RAM:", 64);
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
