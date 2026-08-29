/* format_win.c — UAOS Format Window
 *
 * AmigaOS-style Format window for formatting block devices.
 * Opened from the Icons ▸ Format menu item.
 *
 * Widgets:
 *   - Device cycle gadget (lists formattable block devices)
 *   - Volume name string field
 *   - Filesystem label (FAT32 — only supported FS)
 *   - Format button (with confirm requester)
 *   - Cancel button
 *   - Status line
 */

#include "format_win.h"
#include "wm.h"
#include "framebuffer.h"
#include "requester.h"
#include "../dos/blockdev.h"
#include "../dos/fat32.h"
#include "../dos/vfs.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* =========================================================================
 * Helpers (no libc)
 * ========================================================================= */

static int str_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void str_cp(char *dst, const char *src, int max)
{
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static void uint_to_dec(uint32_t v, char *buf, int max)
{
    char tmp[12];
    int i = 0, j = 0;
    if (v == 0) { buf[j++] = '0'; buf[j] = '\0'; return; }
    while (v && i < 11) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    while (i-- && j < max - 1) buf[j++] = tmp[i];
    buf[j] = '\0';
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
 * Window state
 * ========================================================================= */

#define WIN_W  360
#define WIN_H  220

#define MAX_FORMAT_DEVS 16

static int  g_wm_handle = -1;
static int  g_cx, g_cy, g_cw, g_ch;

/* Device list */
static BlockDev *g_devs[MAX_FORMAT_DEVS];
static int       g_dev_count = 0;
static int       g_dev_sel   = 0;

/* Volume name input */
static char g_volname[32];
static int  g_volname_len = 0;
static int  g_volname_focused = 1;

/* Status */
static char g_status[64] = "Select a device and click Format.";
static int  g_formatting = 0;

/* Widget rects (updated during draw) */
static int g_dev_btn_x, g_dev_btn_y, g_dev_btn_w, g_dev_btn_h;
static int g_tf_x, g_tf_y, g_tf_w, g_tf_h;
static int g_fmt_x, g_fmt_y, g_fmt_w, g_fmt_h;
static int g_cancel_x, g_cancel_y, g_cancel_w, g_cancel_h;

/* =========================================================================
 * Device list — populate from BlockDev_GetList, filtering to partitions
 * ========================================================================= */

static void refresh_device_list(void)
{
    g_dev_count = 0;
    BlockDev *bdev = BlockDev_GetList();
    while (bdev && g_dev_count < MAX_FORMAT_DEVS) {
        /* Only list partitions (part_offset != 0) or named devices */
        if (bdev->part_offset != 0 || (bdev->display_name && bdev->display_name[0])) {
            g_devs[g_dev_count++] = bdev;
        }
        bdev = bdev->next;
    }
    if (g_dev_sel >= g_dev_count) g_dev_sel = 0;
}

/* =========================================================================
 * Draw callback
 * ========================================================================= */

static void format_draw(int wx, int wy, int ww, int wh)
{
    (void)ww; (void)wh;
    int cx = wx + 1;
    int cy = wy + WM_TITLEBAR_H;
    int cw = ww - 1 - WM_SCROLLBAR_W;
    int ch = wh - WM_TITLEBAR_H - WM_SCROLLBAR_W;
    g_cx = cx; g_cy = cy; g_cw = cw; g_ch = ch;

    FB_FillRect(cx, cy, cw, ch, WB_GREY);

    int pad = 8;
    int y = cy + pad;
    int label_h = 16;

    /* Device label + cycle button */
    FB_PutStr(cx + pad, y, "Device:", WB_BLACK, WB_GREY);
    y += label_h + 2;

    g_dev_btn_x = cx + pad;
    g_dev_btn_y = y;
    g_dev_btn_w = cw - pad * 2;
    g_dev_btn_h = 22;
    FB_FillRect(g_dev_btn_x, g_dev_btn_y, g_dev_btn_w, g_dev_btn_h, WB_WHITE);
    draw_bevel(g_dev_btn_x, g_dev_btn_y, g_dev_btn_w, g_dev_btn_h, 0);
    const char *dev_label = "<no devices>";
    if (g_dev_count > 0 && g_dev_sel < g_dev_count) {
        BlockDev *d = g_devs[g_dev_sel];
        dev_label = d->display_name ? d->display_name : d->name;
    }
    FB_PutStr(g_dev_btn_x + 4, g_dev_btn_y + 3, dev_label, WB_BLACK, WB_WHITE);
    /* Cycle arrow on the right */
    FB_PutStr(g_dev_btn_x + g_dev_btn_w - 12, g_dev_btn_y + 3, ">", WB_BLACK, WB_WHITE);
    y += g_dev_btn_h + pad;

    /* Volume name label + text field */
    FB_PutStr(cx + pad, y, "Volume Name:", WB_BLACK, WB_GREY);
    y += label_h + 2;

    g_tf_x = cx + pad;
    g_tf_y = y;
    g_tf_w = cw - pad * 2;
    g_tf_h = 22;
    FB_FillRect(g_tf_x, g_tf_y, g_tf_w, g_tf_h,
                g_volname_focused ? WB_WHITE : WB_LIGHT_GREY);
    draw_bevel(g_tf_x, g_tf_y, g_tf_w, g_tf_h, 0);
    FB_PutStr(g_tf_x + 4, g_tf_y + 3, g_volname, WB_BLACK,
              g_volname_focused ? WB_WHITE : WB_LIGHT_GREY);
    /* Cursor */
    if (g_volname_focused) {
        int cx_pos = g_tf_x + 4 + g_volname_len * 8;
        FB_DrawVLine(cx_pos, g_tf_y + 3, g_tf_h - 6, WB_BLACK);
    }
    y += g_tf_h + pad;

    /* Filesystem label (fixed — FAT32 only) */
    FB_PutStr(cx + pad, y, "Filesystem: FAT32", WB_DARK_GREY, WB_GREY);
    y += label_h + pad;

    /* Status line */
    FB_PutStr(cx + pad, y, g_status, WB_DARK_GREY, WB_GREY);
    y += label_h + pad;

    /* Buttons */
    int btn_w = 80, btn_h = 22;
    int btn_gap = 8;
    int total_btn_w = btn_w * 2 + btn_gap;
    int btn_x_start = cx + (cw - total_btn_w) / 2;
    int btn_y = cy + ch - btn_h - pad;

    g_fmt_w = btn_w; g_fmt_h = btn_h;
    g_fmt_x = btn_x_start; g_fmt_y = btn_y;
    FB_FillRect(g_fmt_x, g_fmt_y, btn_w, btn_h, WB_GREY);
    draw_bevel(g_fmt_x, g_fmt_y, btn_w, btn_h, 1);
    FB_PutStrCentred(g_fmt_x, g_fmt_y, btn_w, btn_h, "Format", WB_BLACK, WB_GREY);

    g_cancel_w = btn_w; g_cancel_h = btn_h;
    g_cancel_x = btn_x_start + btn_w + btn_gap;
    g_cancel_y = btn_y;
    FB_FillRect(g_cancel_x, g_cancel_y, btn_w, btn_h, WB_GREY);
    draw_bevel(g_cancel_x, g_cancel_y, btn_w, btn_h, 1);
    FB_PutStrCentred(g_cancel_x, g_cancel_y, btn_w, btn_h, "Cancel", WB_BLACK, WB_GREY);
}

/* =========================================================================
 * Key callback — volume name text entry
 * ========================================================================= */

static void format_key(char c)
{
    if (g_formatting) return;
    if (c == 27) {  /* ESC */
        WM_CloseWindow(g_wm_handle);
        g_wm_handle = -1;
        return;
    }
    if (!g_volname_focused) return;

    if (c == 8) {  /* Backspace */
        if (g_volname_len > 0) {
            g_volname_len--;
            g_volname[g_volname_len] = '\0';
            WM_Redraw();
        }
        return;
    }
    if (c >= 32 && c < 127 && g_volname_len < 30) {
        g_volname[g_volname_len++] = c;
        g_volname[g_volname_len] = '\0';
        WM_Redraw();
    }
}

/* =========================================================================
 * Format confirm callback
 * ========================================================================= */

static void format_confirm_cb(int button, const char *text, void *user_data)
{
    (void)text; (void)user_data;
    if (button != REQ_BTN_OK) {
        g_formatting = 0;
        str_cp(g_status, "Format cancelled.", 64);
        WM_Redraw();
        return;
    }

    if (g_dev_sel >= g_dev_count || !g_devs[g_dev_sel]) {
        str_cp(g_status, "No device selected.", 64);
        g_formatting = 0;
        WM_Redraw();
        return;
    }

    BlockDev *dev = g_devs[g_dev_sel];
    str_cp(g_status, "Formatting...", 64);
    WM_Redraw();

    int ret = FAT32_Format(dev, g_volname[0] ? g_volname : (void*)0);
    if (ret == 0) {
        dev->formatted = 1;
        /* Auto-mount in VFS */
        const char *dname = dev->display_name ? dev->display_name : dev->name;
        char mnt_name[16];
        int ni = 0, si = 0;
        while (si < 15 && dname[si] && dname[si] != ':')
            mnt_name[ni++] = dname[si++];
        mnt_name[ni] = '\0';
        const char *vol_mnt = g_volname[0] ? g_volname : mnt_name;
        if (vol_mnt[0]) VFS_MountPartition(vol_mnt);
        str_cp(g_status, "Format complete.", 64);
    } else {
        str_cp(g_status, "Format failed.", 64);
    }
    g_formatting = 0;
    WM_Redraw();
}

/* =========================================================================
 * Click callback
 * ========================================================================= */

static void format_click(int handle, int mx, int my)
{
    (void)handle;

    /* Device cycle button */
    if (mx >= g_dev_btn_x && mx < g_dev_btn_x + g_dev_btn_w &&
        my >= g_dev_btn_y && my < g_dev_btn_y + g_dev_btn_h) {
        if (g_dev_count > 0) {
            g_dev_sel = (g_dev_sel + 1) % g_dev_count;
        }
        g_volname_focused = 0;
        WM_Redraw();
        return;
    }

    /* Volume name text field — click to focus */
    if (mx >= g_tf_x && mx < g_tf_x + g_tf_w &&
        my >= g_tf_y && my < g_tf_y + g_tf_h) {
        g_volname_focused = 1;
        WM_Redraw();
        return;
    }

    /* Format button */
    if (mx >= g_fmt_x && mx < g_fmt_x + g_fmt_w &&
        my >= g_fmt_y && my < g_fmt_y + g_fmt_h) {
        if (g_formatting) return;
        if (g_dev_sel >= g_dev_count || !g_devs[g_dev_sel]) {
            str_cp(g_status, "No device selected.", 64);
            WM_Redraw();
            return;
        }
        g_formatting = 1;
        BlockDev *dev = g_devs[g_dev_sel];
        const char *dname = dev->display_name ? dev->display_name : dev->name;
        char body[80];
        int bi = 0;
        const char *p = "Format device ";
        while (*p && bi < 70) body[bi++] = *p++;
        p = dname;
        while (*p && bi < 70) body[bi++] = *p++;
        p = "?\nAll data will be lost!";
        while (*p && bi < 78) body[bi++] = *p++;
        body[bi] = '\0';
        Requester_Confirm("Format", body, "Format", "Cancel",
                          format_confirm_cb, NULL);
        return;
    }

    /* Cancel button */
    if (mx >= g_cancel_x && mx < g_cancel_x + g_cancel_w &&
        my >= g_cancel_y && my < g_cancel_y + g_cancel_h) {
        WM_CloseWindow(g_wm_handle);
        g_wm_handle = -1;
        return;
    }

    /* Click on empty area — unfocus text field */
    g_volname_focused = 0;
    WM_Redraw();
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void FormatWin_Show(void)
{
    if (g_wm_handle >= 0 && WM_IsWindowActive(g_wm_handle)) {
        WM_RaiseWindow(g_wm_handle);
        WM_Redraw();
        return;
    }
    g_wm_handle = -1;
    g_volname[0] = '\0';
    g_volname_len = 0;
    g_volname_focused = 1;
    g_formatting = 0;
    str_cp(g_status, "Select a device and click Format.", 64);
    refresh_device_list();

    int sx = (int)g_fb.width;
    int sy = (int)g_fb.height;
    int wx = (sx - WIN_W) / 2;
    int wy = (sy - WIN_H) / 2;
    if (wy < WM_TITLEBAR_H + 4) wy = WM_TITLEBAR_H + 4;

    g_wm_handle = WM_AddWindow(wx, wy, WIN_W, WIN_H,
                               "Format", format_draw, format_key);
    if (g_wm_handle < 0) return;
    WM_SetClickHandler(g_wm_handle, format_click);
    WM_RaiseWindow(g_wm_handle);
    WM_Redraw();
}
