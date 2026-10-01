/* format_win.c — UAOS Format Window
 *
 * AmigaOS-style Format window for formatting block devices.
 * Opened from the Icons ▸ Format menu item.
 *
 * Widgets:
 *   - Device cycle gadget (lists formattable block devices)
 *   - Volume name string field
 *   - Filesystem label (FAT32 — only supported FS)
 *   - Trashcan checkbox (like Format NOICON when unchecked)
 *   - Format button (with confirm requester)
 *   - Cancel button
 *   - Status line
 */

#include "format_win.h"
#include "wm.h"
#include "framebuffer.h"
#include "gadgets.h"
#include "requester.h"
#include "../dos/blockdev.h"
#include "../dos/fat32.h"
#include "../dos/vfs.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* str/bevel helpers live in gadgets.c */
#define str_cp     gad_str_cp

/* =========================================================================
 * Window state
 * ========================================================================= */

#define WIN_W  360
#define WIN_H  248

#define MAX_FORMAT_DEVS 16

static int  g_wm_handle = -1;
static int  g_cx, g_cy, g_cw, g_ch;

/* Device list */
static BlockDev *g_devs[MAX_FORMAT_DEVS];
static int       g_dev_count = 0;
static int       g_dev_sel   = 0;

/* Volume name input — storage for the string gadget */
static char g_volname[32];

/* Status */
static char g_status[64] = "Select a device and click Format.";
static int  g_formatting = 0;

/* Shared gadgets (rects assigned during draw) */
static Gad         g_dev_cyc;                    /* device picker     */
static const char *g_dev_names[MAX_FORMAT_DEVS];
static Gad         g_volname_g;                  /* volume name field */
static Gad         g_trash_chk;                  /* create Trashcan?   */
static Gad         g_fmt_btn, g_cancel_btn;

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
            g_devs[g_dev_count] = bdev;
            g_dev_names[g_dev_count] =
                bdev->display_name ? bdev->display_name : bdev->name;
            g_dev_count++;
        }
        bdev = bdev->next;
    }
    if (g_dev_sel >= g_dev_count) g_dev_sel = 0;
    g_dev_cyc.kind     = GAD_CYCLE;
    g_dev_cyc.choices  = g_dev_names;
    g_dev_cyc.nchoices = g_dev_count;
    g_dev_cyc.val      = g_dev_sel;
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

    /* Device label + cycle gadget */
    FB_PutStr(cx + pad, y, "Device:", WB_BLACK, WB_GREY);
    y += label_h + 2;

    g_dev_cyc.x = cx + pad;   g_dev_cyc.y = y;
    g_dev_cyc.w = cw - pad * 2; g_dev_cyc.h = 22;
    gad_draw(&g_dev_cyc);
    y += g_dev_cyc.h + pad;

    /* Volume name label + string gadget */
    FB_PutStr(cx + pad, y, "Volume Name:", WB_BLACK, WB_GREY);
    y += label_h + 2;

    g_volname_g.kind = GAD_STRING;
    g_volname_g.x = cx + pad;   g_volname_g.y = y;
    g_volname_g.w = cw - pad * 2; g_volname_g.h = 22;
    gad_draw(&g_volname_g);
    y += g_volname_g.h + pad;

    /* Filesystem label (fixed — FAT32 only) */
    FB_PutStr(cx + pad, y, "Filesystem: FAT32", WB_DARK_GREY, WB_GREY);
    y += label_h + pad;

    /* Trashcan checkbox — Amiga Format NOICON equivalent */
    g_trash_chk.kind = GAD_CHECKBOX;
    g_trash_chk.x = cx + pad;   g_trash_chk.y = y;
    g_trash_chk.w = 14;         g_trash_chk.h = 14;
    g_trash_chk.text = "Trashcan";
    gad_draw(&g_trash_chk);
    y += GAD_CHECK_SZ + pad;

    /* Status line */
    FB_PutStr(cx + pad, y, g_status, WB_DARK_GREY, WB_GREY);
    y += label_h + pad;

    /* Buttons */
    int btn_w = 80, btn_h = 22;
    int btn_gap = 8;
    int total_btn_w = btn_w * 2 + btn_gap;
    int btn_x_start = cx + (cw - total_btn_w) / 2;
    int btn_y = cy + ch - btn_h - pad;

    g_fmt_btn.kind = GAD_BUTTON;
    g_fmt_btn.x = btn_x_start; g_fmt_btn.y = btn_y;
    g_fmt_btn.w = btn_w; g_fmt_btn.h = btn_h;
    g_fmt_btn.text = "Format";
    gad_draw(&g_fmt_btn);

    g_cancel_btn.kind = GAD_BUTTON;
    g_cancel_btn.x = btn_x_start + btn_w + btn_gap;
    g_cancel_btn.y = btn_y;
    g_cancel_btn.w = btn_w; g_cancel_btn.h = btn_h;
    g_cancel_btn.text = "Cancel";
    gad_draw(&g_cancel_btn);
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
    /* String gadget handles insert/backspace/cursor keys */
    if (gad_event(&g_volname_g, GAD_KEY, c, 0) == GADE_CHANGE)
        WM_Redraw();
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
        if (vol_mnt[0]) {
            VFS_MountPartition(vol_mnt);
            /* Amiga: format adds the Trashcan drawer to the new
             * filesystem unless the Trashcan checkbox (NOICON) is off. */
            if (g_trash_chk.val) VFS_CreateTrashcan(vol_mnt);
        }
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

    /* Device cycle gadget */
    if (gad_event(&g_dev_cyc, GAD_DOWN, mx, my) == GADE_CHANGE) {
        g_dev_sel = g_dev_cyc.val;
        g_volname_g.focused = 0;
        WM_Redraw();
        return;
    }

    /* Volume name field — click to focus */
    if (gad_hit(&g_volname_g, mx, my)) {
        gad_event(&g_volname_g, GAD_DOWN, mx, my);
        WM_Redraw();
        return;
    }

    /* Trashcan checkbox */
    if (gad_event(&g_trash_chk, GAD_DOWN, mx, my) == GADE_CHANGE) {
        WM_Redraw();
        return;
    }

    /* Format button */
    if (gad_hit(&g_fmt_btn, mx, my)) {
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
    if (gad_hit(&g_cancel_btn, mx, my)) {
        WM_CloseWindow(g_wm_handle);
        g_wm_handle = -1;
        return;
    }

    /* Click on empty area — unfocus text field */
    g_volname_g.focused = 0;
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
    g_volname_g.buf     = g_volname;
    g_volname_g.buf_max = sizeof(g_volname);
    g_volname_g.buf_len = 0;
    g_volname_g.cursor  = 0;
    g_volname_g.focused = 1;
    memset(&g_trash_chk, 0, sizeof(g_trash_chk));
    g_trash_chk.val = 1;   /* Trashcan on by default (NOICON clears it) */
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
