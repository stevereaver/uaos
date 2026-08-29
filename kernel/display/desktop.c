/* desktop.c — UAOS Workbench 3.x-style Desktop Renderer
 *
 * Draws a complete Workbench-inspired graphical desktop on the linear
 * framebuffer:
 *   - Menu bar (top, 20px high) with Workbench menus and memory display
 *   - Desktop backdrop (solid Amiga grey)
 *   - Disk icons (VFS-mounted volumes, discovered dynamically)
 */

#include "desktop.h"
#include "framebuffer.h"
#include "wm.h"
#include "filebrowser.h"
#include "requester.h"
#include "../exec/intuition_lib.h"
#include "about_win.h"
#include "shell_win.h"
#include "icon_render.h"
#include "../exec/mem_info.h"
#include "../dos/vfs.h"
#include "../dos/ramfs.h"
#include "../dos/icon_loader.h"
#include "../exec/workbench_lib.h"
#include "../irq/rtc.h"
#include "blanker.h"
#include "format_win.h"
#include "../system_reboot.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Debug output */
#define DT_DEBUG 0
#if DT_DEBUG
    #define DT_LOG(msg) do { extern void kprint(const char *); kprint(msg); } while(0)
    #define DT_LOG_DEC(v) do { extern void kprintdec(uint32_t); kprintdec((uint32_t)(v)); } while(0)
#else
    #define DT_LOG(msg) do {} while(0)
    #define DT_LOG_DEC(v) do {} while(0)
#endif

static void scpy(char *dst, const char *src, int max)
{
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

/* =========================================================================
 * Layout constants
 * ========================================================================= */

/* MENUBAR_H is defined in desktop.h */
#define ICON_W         48
#define ICON_H         56   /* 40px bitmap + 16px label */
#define ICON_LABEL_H   16
#define LABEL_W        80   /* label background wider than icon (10 chars at 8px) */

/* =========================================================================
 * Amiga-style 3-D bevel helpers
 * (light edge top-left, dark edge bottom-right)
 * ========================================================================= */

static void draw_bevel_box(int x, int y, int w, int h, int raised)
{
    uint32_t light = raised ? WB_WHITE    : WB_DARK_GREY;
    uint32_t dark  = raised ? WB_DARK_GREY : WB_WHITE;

    FB_DrawHLine(x,         y,         w, light);          /* top            */
    FB_DrawVLine(x,         y,         h, light);          /* left           */
    FB_DrawHLine(x,         y + h - 1, w, dark);           /* bottom         */
    FB_DrawVLine(x + w - 1, y,         h, dark);           /* right          */
}

/* =========================================================================
 * Menu bar
 * ========================================================================= */

/* Build a "NNNK Free" string for the menubar memory display.
 * Shows the total free memory (x64 heap free + M68k guest RAM free slots). */
static void mem_free_str(char *buf, int max)
{
    struct UaosMemInfo mi;
    Mem_GetInfo(&mi);

    uint32_t free_bytes = mi.x64_free;
    /* Add free M68k guest RAM slots */
    if (mi.m68k_slots_total > mi.m68k_slots_used)
        free_bytes += (mi.m68k_slots_total - mi.m68k_slots_used) * mi.m68k_ram_total;

    uint32_t free_kb = free_bytes / 1024;

    int i = 0;
    if (free_kb >= 10000) {
        /* Show as NNK for large values */
        if (free_kb >= 100000) {
            buf[i++] = (char)('0' + (free_kb / 100000) % 10);
            buf[i++] = (char)('0' + (free_kb / 10000) % 10);
            buf[i++] = (char)('0' + (free_kb / 1000) % 10);
            buf[i++] = (char)('0' + (free_kb / 100) % 10);
        } else if (free_kb >= 10000) {
            buf[i++] = (char)('0' + (free_kb / 10000) % 10);
            buf[i++] = (char)('0' + (free_kb / 1000) % 10);
            buf[i++] = (char)('0' + (free_kb / 100) % 10);
            buf[i++] = (char)('0' + (free_kb / 10) % 10);
        }
    } else {
        if (free_kb >= 1000) buf[i++] = (char)('0' + (free_kb / 1000) % 10);
        if (free_kb >= 100)  buf[i++] = (char)('0' + (free_kb / 100) % 10);
        if (free_kb >= 10)   buf[i++] = (char)('0' + (free_kb / 10) % 10);
        buf[i++] = (char)('0' + free_kb % 10);
    }
    if (i < max - 7) {
        buf[i++] = 'K'; buf[i++] = ' '; buf[i++] = 'F'; buf[i++] = 'r'; buf[i++] = 'e'; buf[i++] = 'e';
    }
    buf[i] = '\0';
}

/* Screen title state (updated by intuition.library ShowTitle()) */
static char g_screen_title[64] = "";
static int  g_show_screen_title = 0;

/* DisplayBeep flash state */
static uint32_t g_beep_flash_color = 0;
static uint64_t g_beep_flash_until = 0;
extern volatile uint64_t g_pit_ticks;

/* Forward declarations */
extern void WM_Redraw(void);

/* Menu item action stubs — to be filled with real behaviour later. */

/* Workbench menu actions */
static void menu_action_backdrop(void)
{
    DT_LOG("[MENU] Backdrop selected\n");
    extern void Desktop_ToggleBackdrop(void);
    Desktop_ToggleBackdrop();
}

static void menu_action_execute_command(void)
{
    DT_LOG("[MENU] Execute Command selected\n");
    ShellWin_Open();
}

static void menu_action_redraw_all(void)
{
    DT_LOG("[MENU] Redraw All selected\n");
    WM_Redraw();
}

static void menu_action_update_all(void)
{
    DT_LOG("[MENU] Update All selected\n");
    /* Refresh all open browser windows */
    for (int i = 0; i < 16; i++) {
        if (WM_IsWindowActive(i)) {
            FileBrowser_Refresh(i);
        }
    }
}

static void menu_action_last_message(void)
{
    DT_LOG("[MENU] Last Message selected\n");
    char title[32];
    char body[256];
    Requester_GetLastMessage(title, sizeof(title), body, sizeof(body));
    if (!title[0] && !body[0]) {
        const char *lines[] = { "No previous message.", NULL };
        Requester_Info("Last Message", lines, NULL, NULL);
        return;
    }
    /* Split body into lines array on '\n' for Requester_Info */
    static char line_buf[8][64];
    const char *lines[9];
    int n = 0;
    int ci = 0;
    for (int i = 0; body[i] && n < 8; i++) {
        if (body[i] == '\n') {
            line_buf[n][ci] = '\0';
            lines[n] = line_buf[n];
            n++; ci = 0;
        } else if (ci < 63) {
            line_buf[n][ci++] = body[i];
        }
    }
    if (n < 8 && ci > 0) {
        line_buf[n][ci] = '\0';
        lines[n] = line_buf[n];
        n++;
    }
    lines[n] = NULL;
    Requester_Info(title[0] ? title : "Last Message", lines, NULL, NULL);
}

static void menu_action_about(void)
{
    DT_LOG("[MENU] About selected\n");
    AboutWin_Open();
}

/* Quit confirm callback — reboots the system. */
static void quit_cb(int button, const char *text, void *user_data)
{
    (void)text; (void)user_data;
    if (button != REQ_BTN_OK) return;
    System_Reboot();
}

static void menu_action_quit(void)
{
    DT_LOG("[MENU] Quit selected\n");
    Requester_Confirm("Quit", "Quit Workbench and reboot?", "Quit", "Cancel",
                      quit_cb, NULL);
}

/* Window menu actions */

/* New Drawer callback — creates the directory after string input */
static void new_drawer_cb(int button, const char *text, void *user_data)
{
    (void)user_data;
    if (button != REQ_BTN_OK || !text || !text[0]) return;

    const char *path = FileBrowser_GetFocusedPath();
    if (!path) return;

    char full_path[128];
    /* Build path: volume/newname */
    int vi = 0;
    while (vi < 126 && path[vi]) { full_path[vi] = path[vi]; vi++; }
    int last = (vi > 0) ? path[vi - 1] : 0;
    if (last != ':' && last != '/') {
        if (vi < 127) full_path[vi++] = '/';
    }
    int ti = 0;
    while (vi < 127 && text[ti]) { full_path[vi++] = text[ti++]; }
    full_path[vi] = '\0';

    VFS_MkDir(full_path);

    /* Refresh the current browser */
    int fh = FileBrowser_GetFocusedHandle();
    if (fh >= 0) FileBrowser_Refresh(fh);
}

static void menu_action_new_drawer(void)
{
    DT_LOG("[MENU] New Drawer selected\n");
    if (!FileBrowser_GetFocusedPath()) return;
    Requester_String("New Drawer", "Enter new drawer name:",
                     "", 30, new_drawer_cb, NULL);
}

static void menu_action_open_drawer(void)
{
    DT_LOG("[MENU] Open Drawer selected\n");
    const char *name = FileBrowser_GetSelectedName();
    const char *path = FileBrowser_GetFocusedPath();
    if (!name || !path) return;
    /* Open the selected directory */
    char child_path[128];
    int vi = 0;
    while (vi < 126 && path[vi]) { child_path[vi] = path[vi]; vi++; }
    int last = (vi > 0) ? path[vi - 1] : 0;
    if (last != ':' && last != '/') {
        if (vi < 127) child_path[vi++] = '/';
    }
    int ni = 0;
    while (vi < 127 && name[ni]) { child_path[vi++] = name[ni++]; }
    child_path[vi] = '\0';
    FileBrowser_Open(child_path);
}

static void menu_action_close(void)
{
    DT_LOG("[MENU] Close selected\n");
    int fh = FileBrowser_GetFocusedHandle();
    if (fh >= 0) FileBrowser_Close(fh);
}

static void menu_action_update(void)
{
    DT_LOG("[MENU] Update selected\n");
    int fh = FileBrowser_GetFocusedHandle();
    if (fh >= 0) FileBrowser_Refresh(fh);
}

static void menu_action_select_contents(void)
{
    DT_LOG("[MENU] Select Contents selected\n");
    int fh = FileBrowser_GetFocusedHandle();
    if (fh >= 0) FileBrowser_SelectAll(fh);
}

static void menu_action_clean_up(void)
{
    DT_LOG("[MENU] Clean Up selected\n");
    int fh = FileBrowser_GetFocusedHandle();
    if (fh >= 0) FileBrowser_CleanUp(fh);
}

static void menu_action_snapshot(void)
{
    DT_LOG("[MENU] Snapshot selected\n");
    /* Snapshot saves the focused browser window's position/size so it
     * is restored when the same volume is reopened.  In-memory only. */
    int fh = FileBrowser_GetFocusedHandle();
    if (fh >= 0) FileBrowser_Snapshot(fh);
}

/* Forward declaration — Show menu item calls Information */
static void menu_action_icon_information(void);

static void menu_action_show(void)
{
    DT_LOG("[MENU] Show selected\n");
    /* Show .info for selected icon — opens Information requester */
    menu_action_icon_information();
}

/* View By flyout actions — one per view mode. */
static void menu_action_view_by_icon(void)
{
    DT_LOG("[MENU] View By Icon selected\n");
    int fh = FileBrowser_GetFocusedHandle();
    if (fh >= 0) FileBrowser_SetViewMode(fh, VIEW_ICON);
}

static void menu_action_view_by_name(void)
{
    DT_LOG("[MENU] View By Name selected\n");
    int fh = FileBrowser_GetFocusedHandle();
    if (fh >= 0) FileBrowser_SetViewMode(fh, VIEW_NAME);
}

static void menu_action_view_by_date(void)
{
    DT_LOG("[MENU] View By Date selected\n");
    int fh = FileBrowser_GetFocusedHandle();
    if (fh >= 0) FileBrowser_SetViewMode(fh, VIEW_DATE);
}

static void menu_action_view_by_size(void)
{
    DT_LOG("[MENU] View By Size selected\n");
    int fh = FileBrowser_GetFocusedHandle();
    if (fh >= 0) FileBrowser_SetViewMode(fh, VIEW_SIZE);
}

static void menu_action_view_by_type(void)
{
    DT_LOG("[MENU] View By Type selected\n");
    int fh = FileBrowser_GetFocusedHandle();
    if (fh >= 0) FileBrowser_SetViewMode(fh, VIEW_TYPE);
}

/* Icons menu actions */

static void menu_action_icon_copy(void)
{
    DT_LOG("[MENU] Copy selected\n");
    /* Copy requires a destination — in AmigaOS this opens a file requester.
     * For now, copy to RAM: as a simple destination. */
    char src[128];
    if (!FileBrowser_GetSelectedPath(src, sizeof(src))) return;
    const char *name = FileBrowser_GetSelectedName();
    if (!name) return;

    /* Build dest path: RAM:/name */
    char dst[128];
    int di = 0;
    dst[di++] = 'R'; dst[di++] = 'A'; dst[di++] = 'M'; dst[di++] = ':';
    dst[di++] = '/';
    int ni = 0;
    while (name[ni] && di < 126) dst[di++] = name[ni++];
    dst[di] = '\0';

    /* Read source file and write to dest */
    VfsFile fh_src, fh_dst;
    if (!VFS_Open(&fh_src, src, VFS_READ)) return;
    if (!VFS_Open(&fh_dst, dst, VFS_WRITE | VFS_CREATE | VFS_TRUNC)) {
        VFS_Close(&fh_src);
        return;
    }
    uint8_t buf[512];
    uint32_t n;
    while ((n = VFS_Read(&fh_src, buf, sizeof(buf))) > 0)
        VFS_Write(&fh_dst, buf, n);
    VFS_Close(&fh_src);
    VFS_Close(&fh_dst);

    int fh = FileBrowser_GetFocusedHandle();
    if (fh >= 0) FileBrowser_Refresh(fh);
}

/* Rename callback — performs the rename after string input */
static void rename_cb(int button, const char *text, void *user_data)
{
    (void)user_data;
    if (button != REQ_BTN_OK || !text || !text[0]) return;

    char old_path[128];
    if (!FileBrowser_GetSelectedPath(old_path, sizeof(old_path))) return;

    const char *path = FileBrowser_GetFocusedPath();
    if (!path) return;

    /* Build new path: volume/newname */
    char new_path[128];
    int vi = 0;
    while (vi < 126 && path[vi]) { new_path[vi] = path[vi]; vi++; }
    int last = (vi > 0) ? path[vi - 1] : 0;
    if (last != ':' && last != '/') {
        if (vi < 127) new_path[vi++] = '/';
    }
    int ti = 0;
    while (vi < 127 && text[ti]) { new_path[vi++] = text[ti++]; }
    new_path[vi] = '\0';

    VFS_Rename(old_path, new_path);

    int fh = FileBrowser_GetFocusedHandle();
    if (fh >= 0) FileBrowser_Refresh(fh);
}

static void menu_action_icon_rename(void)
{
    DT_LOG("[MENU] Rename selected\n");
    const char *name = FileBrowser_GetSelectedName();
    if (!name) return;
    Requester_String("Rename", "Enter new name:",
                     name, 30, rename_cb, NULL);
}

/* Delete confirm callback — moves file to Trashcan (RAM:/Trash) */
static void delete_cb(int button, const char *text, void *user_data)
{
    (void)text; (void)user_data;
    if (button != REQ_BTN_OK) return;

    char path[128];
    if (!FileBrowser_GetSelectedPath(path, sizeof(path))) return;
    const char *name = FileBrowser_GetSelectedName();
    if (!name) return;

    /* Build trash path: RAM:/Trash/name */
    char trash_path[128];
    int di = 0;
    const char *p = "RAM:/Trash/";
    while (*p && di < 126) trash_path[di++] = *p++;
    int ni = 0;
    while (name[ni] && di < 126) trash_path[di++] = name[ni++];
    trash_path[di] = '\0';

    /* Try to rename (move) to trash. If rename fails (cross-volume),
     * fall back to copy+delete. */
    if (VFS_Rename(path, trash_path) == 0) {
        /* Success — file moved to trash */
    } else {
        /* Cross-volume or other rename failure — copy then delete */
        VfsFile fh_src, fh_dst;
        if (VFS_Open(&fh_src, path, VFS_READ)) {
            if (VFS_Open(&fh_dst, trash_path, VFS_WRITE | VFS_CREATE | VFS_TRUNC)) {
                uint8_t buf[512];
                uint32_t n;
                while ((n = VFS_Read(&fh_src, buf, sizeof(buf))) > 0)
                    VFS_Write(&fh_dst, buf, n);
                VFS_Close(&fh_dst);
            }
            VFS_Close(&fh_src);
            VFS_Delete(path);
        }
    }

    int fh = FileBrowser_GetFocusedHandle();
    if (fh >= 0) FileBrowser_Refresh(fh);
}

static void menu_action_icon_delete(void)
{
    DT_LOG("[MENU] Delete selected\n");
    const char *name = FileBrowser_GetSelectedName();
    if (!name) return;

    char body[80];
    int bi = 0;
    const char *p = "Delete '";
    while (*p && bi < 70) body[bi++] = *p++;
    int ni = 0;
    while (name[ni] && bi < 70) body[bi++] = name[ni++];
    p = "'?";
    while (*p && bi < 78) body[bi++] = *p++;
    body[bi] = '\0';

    Requester_Confirm("Delete", body, "Delete", "Cancel",
                      delete_cb, NULL);
}

static void menu_action_icon_information(void)
{
    DT_LOG("[MENU] Information selected\n");
    char path[128];
    if (!FileBrowser_GetSelectedPath(path, sizeof(path))) return;
    const char *name = FileBrowser_GetSelectedName();
    if (!name) return;

    /* Gather file info */
    uint32_t total = 0, used = 0;
    VFS_GetVolumeInfo(path, &total, &used);
    uint16_t prot = VFS_GetProtection(path);

    /* Check if it's a directory */
    int is_dir = (VFS_ResolveDir(path) != NULL) ? 1 : 0;

    /* Build info lines */
    static char line1[64], line2[64], line3[64], line4[64], line5[64];

    /* Line 1: Name */
    int li = 0;
    const char *p = "Name: ";
    while (*p && li < 62) line1[li++] = *p++;
    int ni = 0;
    while (name[ni] && li < 62) line1[li++] = name[ni++];
    line1[li] = '\0';

    /* Line 2: Type */
    if (is_dir)
        scpy(line2, "Type: Drawer", 64);
    else
        scpy(line2, "Type: File", 64);

    /* Line 3: Size */
    VfsFile fh;
    uint32_t size = 0;
    if (VFS_Open(&fh, path, VFS_READ)) {
        size = VFS_Size(&fh);
        VFS_Close(&fh);
    }
    li = 0;
    p = "Size: ";
    while (*p && li < 62) line3[li++] = *p++;
    /* Convert size to decimal */
    char num[12];
    int ni2 = 0;
    uint32_t sv = size;
    if (sv == 0) { num[ni2++] = '0'; }
    while (sv && ni2 < 11) { num[ni2++] = '0' + sv % 10; sv /= 10; }
    while (ni2 > 0 && li < 62) line3[li++] = num[--ni2];
    p = " bytes";
    while (*p && li < 62) line3[li++] = *p++;
    line3[li] = '\0';

    /* Line 4: Protection flags */
    li = 0;
    p = "Flags: ";
    while (*p && li < 62) line4[li++] = *p++;
    /* Amiga protection: dweasprw */
    const char *pbits = "------rw";
    for (int i = 0; i < 8 && li < 62; i++) {
        line4[li++] = (prot & (1 << i)) ? pbits[i] : '-';
    }
    line4[li] = '\0';

    /* Line 5: Path */
    li = 0;
    p = "Path: ";
    while (*p && li < 62) line5[li++] = *p++;
    int pi = 0;
    while (path[pi] && li < 62) line5[li++] = path[pi++];
    line5[li] = '\0';

    const char *lines[] = { line1, line2, line3, line4, line5, NULL };
    Requester_Info("Information", lines, NULL, NULL);
}

static void menu_action_icon_snapshot(void)
{
    DT_LOG("[MENU] Snapshot selected\n");
    /* Snapshot saves the selected icon's current position in the drawer. */
    int fh = FileBrowser_GetFocusedHandle();
    if (fh >= 0) FileBrowser_SnapshotIcon(fh);
}

static void menu_action_icon_unsnapshot(void)
{
    DT_LOG("[MENU] Unsnapshot selected\n");
    /* Clears the saved position of the selected icon. */
    int fh = FileBrowser_GetFocusedHandle();
    if (fh >= 0) FileBrowser_UnsnapshotIcon(fh);
}

static void menu_action_icon_leave_out(void)
{
    DT_LOG("[MENU] Leave Out selected\n");
    /* Leave Out places a shortcut icon on the desktop pointing to the
     * selected file/drawer.  Double-clicking it opens (dir) or runs (file)
     * the target.  In-memory only (live CD). */
    char path[128];
    if (!FileBrowser_GetSelectedPath(path, sizeof(path))) return;
    const char *name = FileBrowser_GetSelectedName();
    if (!name) return;
    int is_dir = (VFS_ResolveDir(path) != NULL) ? 1 : 0;
    Desktop_LeaveOutAdd(path, name, is_dir);
}

static void menu_action_icon_put_away(void)
{
    DT_LOG("[MENU] Put Away selected\n");
    /* Put Away removes a Leave Out desktop shortcut icon.
     * Only acts when a leave-out desktop icon is currently selected. */
    Desktop_LeaveOutRemoveSelected();
}

static void menu_action_icon_format(void)
{
    DT_LOG("[MENU] Format selected\n");
    /* Opens the dedicated Format window (Amiga-style) for device selection
     * and volume naming.  The window invokes FAT32_Format on confirm. */
    extern void FormatWin_Show(void);
    FormatWin_Show();
}

/* Empty Trash confirm callback */
static void empty_trash_cb(int button, const char *text, void *user_data)
{
    (void)text; (void)user_data;
    if (button != REQ_BTN_OK) return;

    /* Delete all files in RAM:Trash */
    RamFsNode *trash_dir = VFS_ResolveDir("RAM:/Trash");
    if (!trash_dir) return;
    RamFsNode *child = RamFS_FirstChild(trash_dir);
    while (child) {
        char path[64];
        int pi = 0;
        const char *p = "RAM:/Trash/";
        while (*p && pi < 62) path[pi++] = *p++;
        int ni = 0;
        while (child->name[ni] && pi < 62) path[pi++] = child->name[ni++];
        path[pi] = '\0';
        RamFsNode *next = child->next_sibling;
        VFS_Delete(path);
        child = next;
    }
}

static void menu_action_icon_empty_trash(void)
{
    DT_LOG("[MENU] Empty Trash selected\n");
    Requester_Confirm("Empty Trash", "Empty the Trash?", "OK", "Cancel",
                      empty_trash_cb, NULL);
}

/* Tools menu actions */
static void menu_action_reset_wb(void)
{
    DT_LOG("[MENU] Reset WB selected\n");
    /* Reset Workbench: close all browser windows and redraw desktop */
    for (int i = 0; i < 16; i++) {
        if (WM_IsWindowActive(i)) {
            FileBrowser_Close(i);
        }
    }
    WM_Redraw();
}

static void menu_action_exchange(void)
{
    DT_LOG("[MENU] Exchange selected\n");
    extern void ExchangeWin_Show(void);
    ExchangeWin_Show();
}

static void menu_action_blanker(void)
{
    DT_LOG("[MENU] Blanker selected\n");
    /* Toggle blanker between active and sleeping */
    extern int Cx_FindByName(const char *);
    extern void Cx_CycleState(int);
    int idx = Cx_FindByName("Blanker");
    if (idx >= 0) Cx_CycleState(idx);
}

typedef struct MenuItem {
    const char *label;
    void (*action)(void);
    int is_divider;
    int has_submenu;            /* 1 = item opens a flyout submenu */
    int has_checkmark;          /* 1 = draw a check box column */
    int checked;                /* 1 = check box is filled */
    const struct MenuItem *submenu; /* submenu item list (NULL-terminated) */
} MenuItem;

#define MENU_ITEM(lbl, act) { lbl, act, 0, 0, 0, 0, NULL }
#define MENU_CHECK(lbl, act, chk) { lbl, act, 0, 1, (chk), 0, NULL }
#define MENU_DIVIDER        { NULL, NULL, 1, 0, 0, 0, NULL }
#define MENU_END            { NULL, NULL, 0, 0, 0, 0, NULL }

/* View By flyout submenu.  The checked flags are updated dynamically by
 * update_view_by_checks() before the menu is drawn, so this array is
 * mutable (not const).  Order matches the ViewMode enum. */
static MenuItem g_view_by_submenu[] = {
    MENU_CHECK("Icon", menu_action_view_by_icon, 1),
    MENU_CHECK("Name", menu_action_view_by_name, 0),
    MENU_CHECK("Date", menu_action_view_by_date, 0),
    MENU_CHECK("Size", menu_action_view_by_size, 0),
    MENU_CHECK("Type", menu_action_view_by_type, 0),
    MENU_END
};

/* Refresh the check marks on the View By submenu to reflect the focused
 * browser's current view mode.  Called from draw_menu_dropdown. */
static void update_view_by_checks(void)
{
    int fh = FileBrowser_GetFocusedHandle();
    ViewMode vm = (fh >= 0) ? FileBrowser_GetViewMode(fh) : VIEW_ICON;
    for (int i = 0; i < (int)VIEW_MODE_COUNT; i++)
        g_view_by_submenu[i].checked = (i == (int)vm) ? 1 : 0;
}

/* Menu table: index 0 = Workbench, index 1 = Window, index 2 = Icons,
 * index 3 = Tools, index 4 = Shell, index 5 = UAOS */
static const MenuItem * const g_menus[] = {
    (const MenuItem[]) {
        MENU_ITEM("Backdrop",        menu_action_backdrop        ),
        MENU_ITEM("Execute Command", menu_action_execute_command ),
        MENU_ITEM("Redraw All",      menu_action_redraw_all      ),
        MENU_ITEM("Update All",      menu_action_update_all      ),
        MENU_ITEM("Last Message",    menu_action_last_message    ),
        MENU_ITEM("About",           menu_action_about           ),
        MENU_ITEM("Quit",            menu_action_quit            ),
        MENU_END
    },
    (const MenuItem[]) {
        MENU_ITEM("New Drawer",      menu_action_new_drawer      ),
        MENU_ITEM("Open Drawer",     menu_action_open_drawer     ),
        MENU_ITEM("Close",           menu_action_close           ),
        MENU_ITEM("Update",          menu_action_update          ),
        MENU_ITEM("Select Contents", menu_action_select_contents ),
        MENU_ITEM("Clean Up",        menu_action_clean_up        ),
        MENU_ITEM("Snapshot",        menu_action_snapshot        ),
        MENU_ITEM("Show",            menu_action_show            ),
        { "View By", NULL, 0, 1, 0, 0, g_view_by_submenu },
        MENU_END
    },
    (const MenuItem[]) {
        MENU_ITEM("Copy",            menu_action_icon_copy         ),
        MENU_ITEM("Rename",          menu_action_icon_rename       ),
        MENU_ITEM("Information",     menu_action_icon_information  ),
        MENU_ITEM("Snapshot",        menu_action_icon_snapshot     ),
        MENU_ITEM("Unsnapshot",      menu_action_icon_unsnapshot   ),
        MENU_ITEM("Leave Out",       menu_action_icon_leave_out    ),
        MENU_ITEM("Put Away",        menu_action_icon_put_away     ),
        MENU_DIVIDER,
        MENU_ITEM("Delete",          menu_action_icon_delete       ),
        MENU_ITEM("Format",          menu_action_icon_format       ),
        MENU_ITEM("Empty Trash",     menu_action_icon_empty_trash  ),
        MENU_END
    },
    (const MenuItem[]) {
        MENU_ITEM("Exchange",       menu_action_exchange          ),
        MENU_ITEM("Blanker",        menu_action_blanker           ),
        MENU_DIVIDER,
        MENU_ITEM("Reset WB",       menu_action_reset_wb          ),
        MENU_END
    }
};

#define NUM_MENUS (sizeof(g_menus) / sizeof(g_menus[0]))

/* Open menu state */
static int      g_menu_index = -1;   /* -1 = none */
static int      g_menu_hover = -1;   /* item index under pointer, -1 = none */
static int      g_menu_x     = 0;    /* dropdown screen x */
static int      g_menu_y     = 0;    /* dropdown screen y */
static int      g_menu_w     = 0;    /* dropdown width */
static int      g_menu_h     = 0;    /* dropdown height */
static int      g_submenu_item = -1; /* top-level item with open submenu, -1 = none */
static int      g_submenu_hover = -1;/* submenu item under pointer, -1 = none */
static int      g_submenu_x  = 0;  /* submenu screen x */
static int      g_submenu_y  = 0;  /* submenu screen y */
static int      g_submenu_w  = 0;  /* submenu width */
static int      g_submenu_h  = 0;  /* submenu height */

/* Active guest menu strip (parsed from the focused window) */
static HostMenu  g_active_menus[HOST_MENU_MAX];
static int       g_active_menu_count = 0;
static int       g_guest_menu_active = 0;  /* 1 = use g_active_menus, post IDCMP_MENUPICK */

static int menu_item_count(const MenuItem *items)
{
    int n = 0;
    while (items[n].label || items[n].is_divider) n++;
    return n;
}

/* Refresh the active menu strip from the focused guest window.
 * Falls back to the hardcoded desktop menu when no guest strip is present. */
static void refresh_active_menus(void)
{
    uint32_t strip = Intuition_GetActiveWindowMenuStrip();
    if (strip) {
        int n = Intuition_GetHostMenuStrip(strip, g_active_menus, HOST_MENU_MAX);
        if (n > 0) {
            g_active_menu_count = n;
            g_guest_menu_active = 1;
            return;
        }
    }
    g_active_menu_count = 0;
    g_guest_menu_active = 0;
}

/* Compute the screen width of the longest menu label in a fallback MenuItem list.
 * Includes space for the checkmark column (14px) and submenu arrow (16px). */
static int menu_max_label_width(const MenuItem *items)
{
    int max = 0;
    for (int i = 0; items[i].label || items[i].is_divider; i++) {
        if (items[i].is_divider) continue;
        int len = 0;
        for (const char *p = items[i].label; *p; p++) len++;
        int w = len * 8;
        if (items[i].has_checkmark) w += 14;
        if (items[i].has_submenu)   w += 16;
        if (w > max) max = w;
    }
    return max;
}

/* Compute the screen width of the longest label in a fallback submenu. */
static int submenu_max_label_width(const MenuItem *items)
{
    return menu_max_label_width(items);
}

/* Compute the screen width of the longest menu label in an active HostMenu. */
static int host_menu_max_label_width(const HostMenu *menu)
{
    int max = 0;
    for (int i = 0; i < menu->item_count; i++) {
        if (!menu->items[i].label[0]) continue;
        int len = 0;
        for (const char *p = menu->items[i].label; *p; p++) len++;
        if (len * 8 > max) max = len * 8;
    }
    return max;
}

/* Return the title of the Nth menu, either from the active guest strip or
 * the hardcoded fallback titles. */
static const char *menu_title(int index)
{
    if (g_guest_menu_active && index >= 0 && index < g_active_menu_count)
        return g_active_menus[index].label;
    const char *fallback[] = { "Workbench", "Window", "Icons", "Tools" };
    if (index >= 0 && index < (int)(sizeof(fallback) / sizeof(fallback[0])))
        return fallback[index];
    return NULL;
}

/* Compute the x coordinate of the left edge of the Nth menu title. */
static int menu_title_x(int index)
{
    int x = 8;
    for (int i = 0; i < index; i++) {
        const char *title = menu_title(i);
        if (!title) break;
        int len = 0;
        for (const char *p = title; *p; p++) len++;
        x += len * 8 + 16;
    }
    return x;
}

static void draw_host_menu_item(const HostMenuItem *item, int x, int y, int w,
                                int item_h, int pad_x, int is_hover)
{
    uint32_t bg = is_hover ? WB_BLUE : WB_GREY;
    uint32_t fg = is_hover ? WB_WHITE : WB_BLACK;
    if (!item->enabled) {
        fg = WB_DARK_GREY;
        bg = WB_GREY;
    }

    int cx = x + pad_x;
    int cy = y + (item_h - 16) / 2;

    /* Highlight bar */
    FB_FillRect(x + 2, y, w - 4, item_h, bg);

    /* Checkmark box for CHECKIT items (column is always reserved). */
    int box = 10;
    int bx = cx;
    int by = y + (item_h - box) / 2;
    if (item->has_checkmark) {
        FB_DrawRect(bx, by, box, box, fg);
        if (item->checked) {
            FB_FillRect(bx + 2, by + 2, box - 4, box - 4, fg);
        }
    }
    cx += 14;

    /* Label */
    FB_PutStr(cx, cy, item->label, fg, bg);

    /* Right-side extras: command key or submenu arrow */
    int rx = x + w - pad_x - 8;
    if (item->has_submenu) {
        FB_PutStr(rx - 4, cy, ">", fg, bg);
    } else if (item->command_key) {
        char key[2] = { item->command_key, '\0' };
        FB_PutStr(rx, cy, key, fg, bg);
    }
}

static int host_menu_item_width(const HostMenuItem *item)
{
    int len = 0;
    for (const char *p = item->label; *p; p++) len++;
    int w = 8 * 2 + 14 + len * 8; /* left+right margins + checkmark column + label */
    if (item->has_submenu || item->command_key) w += 16;
    return w;
}

static int host_menu_dropdown_width(const HostMenu *menu)
{
    int max = 0;
    for (int i = 0; i < menu->item_count; i++) {
        int w = host_menu_item_width(&menu->items[i]);
        if (w > max) max = w;
    }
    return max;
}

static void draw_host_submenu(const HostMenu *submenu, int x, int y, int W)
{
    int item_h = 16;
    int pad_x = 8;
    int pad_y = 2;
    int n = submenu->item_count;
    if (n <= 0) return;

    int label_w = host_menu_dropdown_width(submenu);
    int w = label_w + pad_x * 2;
    int h = n * item_h + pad_y * 2;

    if (x + w > W) x = W - w;
    if (y + h > (int)g_fb.height) y = (int)g_fb.height - h;
    if (y < 0) y = 0;

    g_submenu_x = x;
    g_submenu_y = y;
    g_submenu_w = w;
    g_submenu_h = h;

    FB_FillRect(g_submenu_x + 4, g_submenu_y + 4, w, h, WB_DARK_GREY);
    FB_FillRect(g_submenu_x, g_submenu_y, w, h, WB_GREY);
    draw_bevel_box(g_submenu_x, g_submenu_y, w, h, 1);

    for (int i = 0; i < n; i++) {
        int iy = g_submenu_y + pad_y + i * item_h;
        draw_host_menu_item(&submenu->items[i], g_submenu_x, iy, w,
                            item_h, pad_x, i == g_submenu_hover);
    }
}

static void draw_menu_dropdown(int W)
{
    if (g_guest_menu_active) {
        if (g_menu_index < 0 || g_menu_index >= g_active_menu_count) return;
        const HostMenu *menu = &g_active_menus[g_menu_index];
        int n = menu->item_count;
        if (n <= 0) return;
        int item_h = 16;
        int pad_x  = 8;
        int pad_y  = 2;
        int label_w = host_menu_dropdown_width(menu);
        int w = label_w + pad_x * 2;
        int h = n * item_h + pad_y * 2;

        g_menu_x = menu_title_x(g_menu_index);
        g_menu_y = MENUBAR_H;
        g_menu_w = w;
        g_menu_h = h;

        if (g_menu_x + w > W) g_menu_x = W - w;

        FB_FillRect(g_menu_x + 4, g_menu_y + 4, w, h, WB_DARK_GREY);
        FB_FillRect(g_menu_x, g_menu_y, w, h, WB_GREY);
        draw_bevel_box(g_menu_x, g_menu_y, w, h, 1);

        for (int i = 0; i < n; i++) {
            int iy = g_menu_y + pad_y + i * item_h;
            draw_host_menu_item(&menu->items[i], g_menu_x, iy, w,
                                item_h, pad_x, i == g_menu_hover);
        }

        if (g_submenu_item >= 0 && g_submenu_item < n && menu->items[g_submenu_item].has_submenu) {
            int sx = g_menu_x + g_menu_w - 2;
            int sy = g_menu_y + pad_y + g_submenu_item * item_h;
            draw_host_submenu(menu->items[g_submenu_item].submenu, sx, sy, W);
        }
        return;
    }

    if (g_menu_index < 0 || g_menu_index >= (int)NUM_MENUS) return;

    /* Update View By checkmarks before drawing */
    update_view_by_checks();

    const MenuItem *items = g_menus[g_menu_index];
    int n = menu_item_count(items);
    int item_h = 16;
    int pad_x  = 8;
    int pad_y  = 2;
    int label_w = menu_max_label_width(items);
    int w = label_w + pad_x * 2;
    int h = n * item_h + pad_y * 2;

    /* Anchor to the active menu title. */
    g_menu_x = menu_title_x(g_menu_index);
    g_menu_y = MENUBAR_H;
    g_menu_w = w;
    g_menu_h = h;

    /* Clip against right edge */
    if (g_menu_x + w > W) g_menu_x = W - w;

    /* Shadow */
    FB_FillRect(g_menu_x + 4, g_menu_y + 4, w, h, WB_DARK_GREY);

    /* Menu body */
    FB_FillRect(g_menu_x, g_menu_y, w, h, WB_GREY);
    draw_bevel_box(g_menu_x, g_menu_y, w, h, 1);

    /* Items */
    for (int i = 0; i < n; i++) {
        int iy = g_menu_y + pad_y + i * item_h;
        if (items[i].is_divider) {
            FB_FillRect(g_menu_x + 2, iy, w - 4, item_h, WB_GREY);
            FB_DrawHLine(g_menu_x + pad_x, iy + item_h / 2,
                         w - pad_x * 2, WB_DARK_GREY);
            continue;
        }
        uint32_t bg = (i == g_menu_hover) ? WB_BLUE : WB_GREY;
        uint32_t fg = (i == g_menu_hover) ? WB_WHITE : WB_BLACK;
        FB_FillRect(g_menu_x + 2, iy, w - 4, item_h, bg);

        int cx = g_menu_x + pad_x;
        /* Checkmark box for items that have one */
        if (items[i].has_checkmark) {
            int box = 10;
            int bx = cx;
            int by = iy + (item_h - box) / 2;
            FB_DrawRect(bx, by, box, box, fg);
            if (items[i].checked)
                FB_FillRect(bx + 2, by + 2, box - 4, box - 4, fg);
            cx += 14;
        }
        /* Label */
        FB_PutStr(cx, iy + (item_h - 16) / 2, items[i].label, fg, bg);
        /* Submenu arrow */
        if (items[i].has_submenu) {
            int rx = g_menu_x + w - pad_x - 8;
            FB_PutStr(rx - 4, iy + (item_h - 16) / 2, ">", fg, bg);
        }
    }

    /* Draw the fallback submenu if one is open */
    if (g_submenu_item >= 0 && g_submenu_item < n &&
        items[g_submenu_item].has_submenu && items[g_submenu_item].submenu) {
        const MenuItem *sub = items[g_submenu_item].submenu;
        int sn = menu_item_count(sub);
        if (sn > 0) {
            int s_label_w = submenu_max_label_width(sub);
            int sw = s_label_w + pad_x * 2;
            int sh = sn * item_h + pad_y * 2;
            int sx = g_menu_x + g_menu_w - 2;
            int sy = g_menu_y + pad_y + g_submenu_item * item_h;
            if (sx + sw > W) sx = W - sw;
            if (sy + sh > (int)g_fb.height) sy = (int)g_fb.height - sh;
            if (sy < 0) sy = 0;

            g_submenu_x = sx;
            g_submenu_y = sy;
            g_submenu_w = sw;
            g_submenu_h = sh;

            FB_FillRect(sx + 4, sy + 4, sw, sh, WB_DARK_GREY);
            FB_FillRect(sx, sy, sw, sh, WB_GREY);
            draw_bevel_box(sx, sy, sw, sh, 1);

            for (int j = 0; j < sn; j++) {
                int iy = sy + pad_y + j * item_h;
                if (sub[j].is_divider) {
                    FB_FillRect(sx + 2, iy, sw - 4, item_h, WB_GREY);
                    FB_DrawHLine(sx + pad_x, iy + item_h / 2,
                                 sw - pad_x * 2, WB_DARK_GREY);
                    continue;
                }
                uint32_t bg = (j == g_submenu_hover) ? WB_BLUE : WB_GREY;
                uint32_t fg = (j == g_submenu_hover) ? WB_WHITE : WB_BLACK;
                FB_FillRect(sx + 2, iy, sw - 4, item_h, bg);
                int cx2 = sx + pad_x;
                if (sub[j].has_checkmark) {
                    int box = 10;
                    int bx = cx2;
                    int by2 = iy + (item_h - box) / 2;
                    FB_DrawRect(bx, by2, box, box, fg);
                    if (sub[j].checked)
                        FB_FillRect(bx + 2, by2 + 2, box - 4, box - 4, fg);
                    cx2 += 14;
                }
                FB_PutStr(cx2, iy + (item_h - 16) / 2, sub[j].label, fg, bg);
            }
        }
    }
}

static void draw_menubar(int W)
{
    /* Update the active menu strip from the focused guest window. */
    refresh_active_menus();

    /* Top bevel: black outer, white inner (K+W+B+...) */
    FB_DrawHLine(0, 0, W, WB_BLACK);
    FB_DrawHLine(0, 1, W, WB_WHITE);
    /* Blue fill between the white highlight and the black bottom */
    FB_FillRect(0, 2, W, MENUBAR_H - 3, WB_BLUE);
    /* Black bottom edge */
    FB_DrawHLine(0, MENUBAR_H - 1, W, WB_BLACK);

    /* Menu titles (clean 8x16 system font, centred in the 20px bar) */
    int mx = 8;
    for (int i = 0; ; i++) {
        const char *title = menu_title(i);
        if (!title) break;
        /* Highlight the active menu title with a black bar, white text. */
        uint32_t bg = (g_menu_index == i) ? WB_BLACK : WB_BLUE;
        FB_PutStr(mx, 2, title, WB_WHITE, bg);
        int len = 0;
        for (const char *p = title; *p; p++) len++;
        mx += len * 8 + 16;
    }

    /* Screen title — drawn between menus and clock when requested */
    if (g_show_screen_title && g_screen_title[0]) {
        int title_w = 0;
        for (const char *p = g_screen_title; *p; p++) title_w += 8;
        int title_x = mx + 16;
        if (title_x + title_w > W - 120)
            title_x = W - 120 - title_w;
        if (title_x > mx && title_w > 0)
            FB_PutStr(title_x, 2, g_screen_title, WB_WHITE, WB_BLUE);
    }

    /* Clock display — HH:MM:SS on the far right of the menubar */
    char clock_buf[16];
    {
        RtcTime t = RTC_ReadTime();
        int ci = 0;
        clock_buf[ci++] = (char)('0' + (t.hour / 10) % 10);
        clock_buf[ci++] = (char)('0' + t.hour % 10);
        clock_buf[ci++] = ':';
        clock_buf[ci++] = (char)('0' + (t.min / 10) % 10);
        clock_buf[ci++] = (char)('0' + t.min % 10);
        clock_buf[ci++] = ':';
        clock_buf[ci++] = (char)('0' + (t.sec / 10) % 10);
        clock_buf[ci++] = (char)('0' + t.sec % 10);
        clock_buf[ci] = '\0';
    }
    int clk_len = 0;
    for (const char *p = clock_buf; *p; p++) clk_len++;
    int clk_x = W - clk_len * 8 - 8;
    FB_PutStr(clk_x, 2, clock_buf, WB_WHITE, WB_BLUE);

    /* Memory display — show free memory just left of the clock */
    {
        char buf[24];
        mem_free_str(buf, (int)sizeof(buf));
        int mlen = 0;
        for (const char *p = buf; *p; p++) mlen++;
        FB_PutStr(clk_x - mlen * 8 - 16, 2, buf, WB_CREAM, WB_BLUE);
    }
}

/* =========================================================================
 * Disk icon
 * ========================================================================= */

static void draw_disk_icon(int x, int y, const char *label, uint32_t colour, int is_selected)
{
    int bx = x;
    int by = y;
    int bw = ICON_W;
    int bh = ICON_H - ICON_LABEL_H;   /* 40 */

    /* Selected state: inverse/video colours. */
    uint32_t body_col = is_selected ? (colour ^ 0x00FFFFFF) : colour;
    uint32_t slot_col = is_selected ? (WB_DARK_GREY ^ 0x00FFFFFF) : WB_DARK_GREY;
    uint32_t slot_line = is_selected ? (WB_BLACK ^ 0x00FFFFFF) : WB_BLACK;
    uint32_t label_bg = is_selected ? (WB_BLUE ^ 0x00FFFFFF) : WB_BLUE;
    uint32_t label_fg = is_selected ? (WB_WHITE ^ 0x00FFFFFF) : WB_WHITE;

    /* Icon body */
    FB_FillRect(bx, by, bw, bh, body_col);
    draw_bevel_box(bx, by, bw, bh, !is_selected);  /* swap raised/bevel in inverse */

    /* Drive slot detail */
    FB_FillRect(bx + 6, by + bh - 10, bw - 12, 5, slot_col);
    FB_DrawHLine(bx + 7, by + bh - 9, bw - 14, slot_line);

    /* Label background — wider than icon, centred under it */
    int lx = bx + (bw - LABEL_W) / 2;
    if (lx < 0) lx = 0;
    FB_FillRect(lx, by + bh, LABEL_W, ICON_LABEL_H, label_bg);
    FB_PutStrCentred(lx, by + bh, LABEL_W, ICON_LABEL_H, label, label_fg, label_bg);
}

/* =========================================================================
 * Backdrop (solid Amiga grey R:170,G:170,B:170)
 * ========================================================================= */

static void draw_backdrop(int W, int H)
{
    /* Clear the full screen first so no stale window chrome survives
     * in the menubar band after a resize or move */
    uint32_t bg = WB_GREY;
    if (g_beep_flash_color && g_beep_flash_until && g_pit_ticks < g_beep_flash_until)
        bg = g_beep_flash_color;
    FB_FillRect(0, 0, W, H, bg);

    /* If the front Intuition screen has a custom SA_BitMap, render it as
     * the desktop backdrop.  Windows and the menu bar are drawn on
     * top by the WM. */
    UAOS_Intuition_RenderScreenBackdrop();
}

/* =========================================================================
 * Icon state for desktop
 * ========================================================================= */

#define DBLCLICK_TICKS  2   /* max seconds between two clicks for double-click */
#define MAX_ICONS 16

typedef struct {
    int      x, y;         /* icon top-left on desktop */
    const char *volume;    /* FileBrowser_Open argument */
    const char *label;     /* Icon label text */
    int      is_ndos;      /* 1 = unformatted (NDOS) */
    int      is_selected;  /* 1 = icon currently selected/highlighted */
    uint32_t last_tick;    /* tick of last click */
    int      click_count;  /* clicks within window */
    ParsedIcon parsed;    /* loaded .info icon (zeroed if none) */
    int      has_parsed;    /* 1 if parsed icon is valid */
    int      is_trashcan;  /* 1 = special Trashcan icon */
    int      is_appicon;   /* 1 = workbench.library AppIcon */
    uint32_t appicon_id;   /* AppIcon ID for message dispatch */
    int      is_leaveout;  /* 1 = Leave Out desktop shortcut */
    const char *leaveout_path;  /* target path for leave-out icons */
    int      leaveout_is_dir;   /* 1 = target is a directory */
} IconState;

/* Desktop icon drag state */
static int      g_icon_drag_idx   = -1;
static int      g_icon_drag_off_x = 0;
static int      g_icon_drag_off_y = 0;
static int      g_icon_drag_moved = 0;
static int      g_icon_drag_orig_x = 0;
static int      g_icon_drag_orig_y = 0;

/* Desktop lasso (rubber-band) selection state.
 * Active when the user presses the left button on empty desktop backdrop
 * and drags — a dashed rectangle follows the cursor and any icon whose
 * bounding box intersects it is selected.  Classic Workbench behaviour. */
static int g_lasso_active  = 0;
static int g_lasso_start_x = 0;
static int g_lasso_start_y = 0;
static int g_lasso_cur_x   = 0;
static int g_lasso_cur_y   = 0;
static int g_lasso_moved   = 0;  /* 1 once the cursor moved during the drag */

/* Desktop background double-click state */
static int       g_desktop_pressed = 0;
static uint32_t  g_desktop_last_tick = 0;
static int       g_desktop_click_count = 0;

/* Backdrop visibility toggle (Workbench ▸ Backdrop).  When 1, desktop
 * icons are hidden but the grey backdrop + menu bar remain. */
static int g_backdrop_hidden = 0;

/* Leave Out registry — desktop shortcut icons placed by Icons ▸ Leave Out.
 * In-memory only (live CD).  Up to MAX_ICONS/2 leave-out icons. */
#define MAX_LEAVEOUT (MAX_ICONS / 2)
typedef struct {
    char path[128];
    char label[32];
    int  is_dir;
    int  valid;
} LeaveOutEntry;
static LeaveOutEntry g_leaveout[MAX_LEAVEOUT];
static int g_leaveout_version = 0;  /* bumped on every add/remove to
                                     * invalidate the icon cache */

/* Build the desktop icon list from real mounted volumes (VFS).
 * click_count / last_tick persist across calls by matching on volume name.
 *
 * P3: the icon list (including the expensive Icon_Load / .info decode) is
 * cached and only rebuilt when the VFS mount table changes (mount count or
 * any mount name differs from the cached fingerprint).  This avoids
 * reloading every .info from VFS on every frame / mouse event. */
static IconState *get_icons(int *count)
{
    static IconState icons[MAX_ICONS];
    static char vol_labels[MAX_ICONS][32];
    static int initialised = 0;

    /* Mount-table fingerprint for cache invalidation. */
    static int  cache_mount_count = -1;
    static char cache_mount_names[MAX_ICONS][32];
    static int  cache_count = 0;
    static int  cache_appicon_count = -1;
    static int  cache_leaveout_version = -1;

    if (!initialised) {
        for (int i = 0; i < MAX_ICONS; i++) {
            icons[i].volume = NULL;
            icons[i].label  = NULL;
            icons[i].is_ndos = 0;
            icons[i].is_selected = 0;
            icons[i].last_tick = 0;
            icons[i].click_count = 0;
            icons[i].is_trashcan = 0;
            icons[i].is_appicon = 0;
        }
        initialised = 1;
    }

    /* ── Check whether the VFS mount table, AppIcon set, or leave-out
     *     registry changed ── */
    int mount_count = VFS_GetMountCount();
    int appicon_count = WB_GetAppIconCount();
    int changed = (mount_count != cache_mount_count) ||
                  (appicon_count != cache_appicon_count) ||
                  (g_leaveout_version != cache_leaveout_version);

    if (!changed) {
        for (int mi = 0; mi < mount_count && !changed; mi++) {
            char mname[32];
            if (!VFS_GetMountName(mi, mname, 32)) { changed = 1; break; }
            /* Compare against cached fingerprint */
            int diff = 0;
            for (int k = 0; k < 32; k++) {
                if (cache_mount_names[mi][k] != mname[k]) { diff = 1; break; }
                if (mname[k] == '\0') break;
            }
            if (diff) changed = 1;
        }
    }

    if (!changed) {
        /* Cache is valid — return the existing icon list as-is.  Click /
         * selection state already lives in the icons[] array, so no copy
         * or VFS reload is needed. */
        *count = cache_count;
        return icons;
    }

    /* ── Cache miss: rebuild from VFS ──
     * Snapshot old click state so we can restore it after rebuilding.
     * Must be static — ParsedIcon is huge (~32 KB) and 16 of them on the
     * kernel stack would overflow it. */
    static IconState old_icons[MAX_ICONS];
    for (int i = 0; i < MAX_ICONS; i++) old_icons[i] = icons[i];

    int W  = (int)g_fb.width;
    int ix = W - ICON_W - 16;
    int iy = MENUBAR_H + 16;

    int n = 0;

    /* Update the fingerprint */
    cache_mount_count = mount_count;
    cache_appicon_count = appicon_count;
    cache_leaveout_version = g_leaveout_version;
    for (int mi = 0; mi < mount_count && mi < MAX_ICONS; mi++) {
        char mname[32];
        if (VFS_GetMountName(mi, mname, 32)) {
            for (int k = 0; k < 32; k++) {
                cache_mount_names[mi][k] = mname[k];
                if (mname[k] == '\0') break;
            }
        } else {
            cache_mount_names[mi][0] = '\0';
        }
    }

    /* ── VFS-mounted volumes (RAM:, Workbench:, etc.) ──
     * Only show RAM: and Workbench: on the desktop — other mounts
     * (CD, partitions, etc.) are accessible via the shell but should
     * not clutter the Workbench screen. */
    for (int mi = 0; mi < mount_count && n < MAX_ICONS; mi++) {
        char mname[32];
        if (!VFS_GetMountName(mi, mname, 32)) continue;

        /* Filter: only RAM and Workbench appear as desktop icons */
        if (!str_eq(mname, "RAM") && !str_eq(mname, "Workbench"))
            continue;

        /* Build the volume string (same format that will be stored in icons[n].volume) */
        const char *vol_str;
        if (str_eq(mname, "RAM")) {
            vol_str = "RAM:";
        } else {
            int li = 0;
            while (mname[li] && li < 30) {
                vol_labels[n][li] = mname[li];
                li++;
            }
            if (li < 31) vol_labels[n][li++] = ':';
            vol_labels[n][li] = '\0';
            vol_str = vol_labels[n];
        }

        /* Find previous icon with the same volume name to preserve state */
        uint32_t old_tick = 0;
        int old_clicks = 0;
        int old_selected = 0;
        int old_x = ix;
        int old_y = iy + n * (ICON_H + 8);
        for (int j = 0; j < MAX_ICONS; j++) {
            if (old_icons[j].volume && str_eq(old_icons[j].volume, vol_str)) {
                old_tick = old_icons[j].last_tick;
                old_clicks = old_icons[j].click_count;
                old_selected = old_icons[j].is_selected;
                old_x = old_icons[j].x;
                old_y = old_icons[j].y;
                break;
            }
        }

        /* Store icon data */
        icons[n].volume = vol_str;
        icons[n].label  = str_eq(mname, "RAM") ? "RAM Disk" : vol_str;
        icons[n].x = old_x;
        icons[n].y = old_y;
        icons[n].is_ndos = 0;
        icons[n].last_tick   = old_tick;
        icons[n].click_count = old_clicks;
        icons[n].is_selected = old_selected;
        icons[n].has_parsed  = 0;
        memset(&icons[n].parsed, 0, sizeof(ParsedIcon));

        /* Try to load a .info icon for this volume */
        if (Icon_Load(vol_str, &icons[n].parsed)) {
            icons[n].has_parsed = 1;
            /* Use .info label if present */
            if (icons[n].parsed.label[0]) {
                icons[n].label = icons[n].parsed.label;
            }
        }

        DT_LOG("[DT] VFS icon "); DT_LOG_DEC(n); DT_LOG(" mname='"); DT_LOG(mname); DT_LOG("' vol_str='"); DT_LOG(vol_str); DT_LOG("'\n");
        n++;
    }

    /* ── Trashcan icon (always present, bottom-right) ── */
    if (n < MAX_ICONS) {
        /* Position at bottom-right of desktop */
        int trash_x = W - ICON_W - 16;
        int trash_y = (int)g_fb.height - ICON_H - 24;

        /* Preserve state from previous build */
        uint32_t old_tick = 0;
        int old_clicks = 0;
        int old_selected = 0;
        for (int j = 0; j < MAX_ICONS; j++) {
            if (old_icons[j].volume && str_eq(old_icons[j].volume, "RAM:/Trash")) {
                old_tick = old_icons[j].last_tick;
                old_clicks = old_icons[j].click_count;
                old_selected = old_icons[j].is_selected;
                trash_x = old_icons[j].x;
                trash_y = old_icons[j].y;
                break;
            }
        }

        icons[n].volume = "RAM:/Trash";
        icons[n].label  = "Trashcan";
        icons[n].x = trash_x;
        icons[n].y = trash_y;
        icons[n].is_ndos = 0;
        icons[n].last_tick   = old_tick;
        icons[n].click_count = old_clicks;
        icons[n].is_selected = old_selected;
        icons[n].has_parsed  = 0;
        icons[n].is_trashcan = 1;
        memset(&icons[n].parsed, 0, sizeof(ParsedIcon));
        n++;
    }

    /* Clear any leftover slots */
    for (int i = n; i < MAX_ICONS; i++) {
        icons[i].volume = NULL;
        icons[i].label  = NULL;
        icons[i].is_trashcan = 0;
        icons[i].is_appicon = 0;
    }

    /* ── AppIcons from workbench.library ── */
    static char appicon_labels[MAX_ICONS][APPICON_MAX_LABEL];
    int n_appicons = WB_GetAppIconCount();
    int ai_x = 16;
    int ai_y = MENUBAR_H + 16;

    for (int ai = 0; ai < n_appicons && n < MAX_ICONS; ai++) {
        AppIconInfo info;
        if (!WB_GetAppIcon(ai, &info)) break;

        /* Copy label to static storage */
        for (int k = 0; k < APPICON_MAX_LABEL; k++) {
            appicon_labels[n][k] = info.label[k];
            if (info.label[k] == '\0') break;
        }
        appicon_labels[n][APPICON_MAX_LABEL - 1] = '\0';

        icons[n].volume = NULL;  /* AppIcons don't open a volume */
        icons[n].label  = appicon_labels[n];
        icons[n].x = ai_x;
        icons[n].y = ai_y;
        icons[n].is_ndos = 0;
        icons[n].last_tick = 0;
        icons[n].click_count = 0;
        icons[n].is_selected = 0;
        icons[n].has_parsed = 0;
        icons[n].is_trashcan = 0;
        icons[n].is_appicon = 1;
        icons[n].appicon_id = info.id;
        memset(&icons[n].parsed, 0, sizeof(ParsedIcon));
        n++;
        ai_y += ICON_H + 8;
    }

    /* Clear remaining slots */
    for (int i = n; i < MAX_ICONS; i++) {
        icons[i].volume = NULL;
        icons[i].label  = NULL;
        icons[i].is_trashcan = 0;
        icons[i].is_appicon = 0;
        icons[i].is_leaveout = 0;
    }

    /* ── Leave Out desktop shortcut icons ── */
    static char leaveout_labels[MAX_LEAVEOUT][32];
    static char leaveout_paths[MAX_LEAVEOUT][128];
    int lo_x = 16;
    int lo_y = MENUBAR_H + 16 + (ICON_H + 8) * 4;  /* below AppIcon column */
    for (int li = 0; li < MAX_LEAVEOUT && n < MAX_ICONS; li++) {
        if (!g_leaveout[li].valid) continue;
        /* Copy label + path to static storage */
        int k;
        for (k = 0; k < 31 && g_leaveout[li].label[k]; k++)
            leaveout_labels[n][k] = g_leaveout[li].label[k];
        leaveout_labels[n][k] = '\0';
        for (k = 0; k < 127 && g_leaveout[li].path[k]; k++)
            leaveout_paths[n][k] = g_leaveout[li].path[k];
        leaveout_paths[n][k] = '\0';

        icons[n].volume = NULL;
        icons[n].label  = leaveout_labels[n];
        icons[n].x = lo_x;
        icons[n].y = lo_y;
        icons[n].is_ndos = 0;
        icons[n].last_tick = 0;
        icons[n].click_count = 0;
        icons[n].is_selected = 0;
        icons[n].has_parsed = 0;
        icons[n].is_trashcan = 0;
        icons[n].is_appicon = 0;
        icons[n].is_leaveout = 1;
        icons[n].leaveout_path = leaveout_paths[n];
        icons[n].leaveout_is_dir = g_leaveout[li].is_dir;
        memset(&icons[n].parsed, 0, sizeof(ParsedIcon));
        n++;
        lo_y += ICON_H + 8;
    }

    /* Clear any newly-remaining slots */
    for (int i = n; i < MAX_ICONS; i++) {
        icons[i].volume = NULL;
        icons[i].label  = NULL;
        icons[i].is_trashcan = 0;
        icons[i].is_appicon = 0;
        icons[i].is_leaveout = 0;
    }

    cache_count = n;
    *count = n;
    return icons;
}

/* Draw a trashcan icon — AmigaOS-style cylindrical trashcan */
static void draw_trashcan_icon(int x, int y, int is_selected)
{
    uint32_t body_col = is_selected ? (WB_GREY ^ 0x00FFFFFF) : WB_GREY;
    uint32_t edge_col = is_selected ? (WB_DARK_GREY ^ 0x00FFFFFF) : WB_DARK_GREY;
    uint32_t hili_col = is_selected ? (WB_WHITE ^ 0x00FFFFFF) : WB_WHITE;
    uint32_t label_bg = is_selected ? (WB_BLUE ^ 0x00FFFFFF) : WB_BLUE;
    uint32_t label_fg = is_selected ? (WB_WHITE ^ 0x00FFFFFF) : WB_WHITE;

    int bw = ICON_W;
    int bh = ICON_H - ICON_LABEL_H;  /* 40px icon area */
    int lx = x;
    int ly = y;

    /* Lid (top rectangle) */
    int lid_h = 6;
    FB_FillRect(lx + 4, ly, bw - 8, lid_h, body_col);
    draw_bevel_box(lx + 4, ly, bw - 8, lid_h, !is_selected);

    /* Handle on lid */
    FB_DrawHLine(lx + 16, ly + 2, bw - 32, edge_col);

    /* Body (trapezoidal cylinder — approximated with rect) */
    int body_y = ly + lid_h;
    int body_h = bh - lid_h;
    FB_FillRect(lx + 6, body_y, bw - 12, body_h, body_col);
    draw_bevel_box(lx + 6, body_y, bw - 12, body_h, !is_selected);

    /* Vertical ribs on the body */
    for (int rx = 12; rx < bw - 12; rx += 6) {
        FB_DrawVLine(lx + rx, body_y + 2, body_h - 4, edge_col);
    }

    /* Highlight on left edge */
    FB_DrawVLine(lx + 7, body_y + 1, body_h - 2, hili_col);

    /* Label — wider than icon, centred under it */
    int label_y = ly + bh;
    int lbl_x = lx + (bw - LABEL_W) / 2;
    if (lbl_x < 0) lbl_x = 0;
    FB_FillRect(lbl_x, label_y, LABEL_W, ICON_LABEL_H, label_bg);
    FB_PutStrCentred(lbl_x, label_y, LABEL_W, ICON_LABEL_H, "Trashcan", label_fg, label_bg);
}

/* Draw an AppIcon — a tool-style icon with a small gadget appearance */
static void draw_appicon_icon(int x, int y, const char *label, int is_selected)
{
    uint32_t body_col = is_selected ? (WB_LIGHT_GREY ^ 0x00FFFFFF) : WB_LIGHT_GREY;
    uint32_t edge_col = is_selected ? (WB_DARK_GREY ^ 0x00FFFFFF) : WB_DARK_GREY;
    uint32_t hili_col = is_selected ? (WB_WHITE ^ 0x00FFFFFF) : WB_WHITE;
    uint32_t label_bg = is_selected ? (WB_BLUE ^ 0x00FFFFFF) : WB_BLUE;
    uint32_t label_fg = is_selected ? (WB_WHITE ^ 0x00FFFFFF) : WB_WHITE;

    int bw = ICON_W;
    int bh = ICON_H - ICON_LABEL_H;

    /* Body — rounded rectangle look */
    FB_FillRect(x, y, bw, bh, body_col);
    draw_bevel_box(x, y, bw, bh, !is_selected);

    /* Inner highlight */
    FB_DrawHLine(x + 1, y + 1, bw - 2, hili_col);
    FB_DrawVLine(x + 1, y + 1, bh - 2, hili_col);

    /* Draw a small "tool" glyph in the centre — a diamond shape */
    int cx = x + bw / 2;
    int cy = y + bh / 2;
    int r = 8;
    for (int dy = -r; dy <= r; dy++) {
        int dx = r - (dy < 0 ? -dy : dy);
        if (dx < 0) dx = 0;
        FB_DrawHLine(cx - dx, cy + dy, dx * 2 + 1, edge_col);
    }

    /* Label — wider than icon, centred under it */
    int label_y = y + bh;
    int lbl_x = x + (bw - LABEL_W) / 2;
    if (lbl_x < 0) lbl_x = 0;
    FB_FillRect(lbl_x, label_y, LABEL_W, ICON_LABEL_H, label_bg);
    FB_PutStrCentred(lbl_x, label_y, LABEL_W, ICON_LABEL_H, label, label_fg, label_bg);
}

/* Draw a Leave Out desktop shortcut icon — a drawer/file glyph with a
 * small "shortcut" arrow in the lower-left, indicating it points to
 * another path.  Reuses the AppIcon body style. */
static void draw_leaveout_icon(int x, int y, const char *label,
                               int is_dir, int is_selected)
{
    uint32_t body_col = is_selected ? (WB_LIGHT_GREY ^ 0x00FFFFFF) : WB_LIGHT_GREY;
    uint32_t edge_col = is_selected ? (WB_DARK_GREY ^ 0x00FFFFFF) : WB_DARK_GREY;
    uint32_t hili_col = is_selected ? (WB_WHITE ^ 0x00FFFFFF) : WB_WHITE;
    uint32_t label_bg = is_selected ? (WB_BLUE ^ 0x00FFFFFF) : WB_BLUE;
    uint32_t label_fg = is_selected ? (WB_WHITE ^ 0x00FFFFFF) : WB_WHITE;

    int bw = ICON_W;
    int bh = ICON_H - ICON_LABEL_H;

    FB_FillRect(x, y, bw, bh, body_col);
    draw_bevel_box(x, y, bw, bh, !is_selected);
    FB_DrawHLine(x + 1, y + 1, bw - 2, hili_col);
    FB_DrawVLine(x + 1, y + 1, bh - 2, hili_col);

    int cx = x + bw / 2;
    int cy = y + bh / 2;

    if (is_dir) {
        /* Drawer glyph — a folder shape */
        FB_FillRect(cx - 10, cy - 6, 20, 14, edge_col);
        FB_FillRect(cx - 10, cy - 6, 8, 4, edge_col);
        FB_FillRect(cx - 8, cy - 4, 16, 10, body_col);
    } else {
        /* File glyph — a document with a folded corner */
        FB_FillRect(cx - 8, cy - 8, 16, 18, edge_col);
        FB_FillRect(cx - 6, cy - 6, 12, 14, body_col);
        FB_DrawHLine(cx + 2, cy - 8, 6, hili_col);
        FB_DrawVLine(cx + 8, cy - 8, 6, hili_col);
    }

    /* Shortcut arrow in lower-left */
    FB_DrawHLine(x + 4, y + bh - 6, 6, WB_BLACK);
    FB_DrawVLine(x + 4, y + bh - 10, 5, WB_BLACK);
    FB_PutPixel(x + 3, y + bh - 7, WB_BLACK);
    FB_PutPixel(x + 5, y + bh - 7, WB_BLACK);
    FB_PutPixel(x + 4, y + bh - 8, WB_BLACK);

    int label_y = y + bh;
    int lbl_x = x + (bw - LABEL_W) / 2;
    if (lbl_x < 0) lbl_x = 0;
    FB_FillRect(lbl_x, label_y, LABEL_W, ICON_LABEL_H, label_bg);
    FB_PutStrCentred(lbl_x, label_y, LABEL_W, ICON_LABEL_H, label, label_fg, label_bg);
}

/* Draw an IconState using .info image when available, else procedural fallback. */
static void draw_icon_state(const IconState *ic)
{
    if (ic->is_trashcan) {
        draw_trashcan_icon(ic->x, ic->y, ic->is_selected);
        return;
    }
    if (ic->is_appicon) {
        draw_appicon_icon(ic->x, ic->y, ic->label, ic->is_selected);
        return;
    }
    if (ic->is_leaveout) {
        draw_leaveout_icon(ic->x, ic->y, ic->label, ic->leaveout_is_dir,
                           ic->is_selected);
        return;
    }
    if (ic->has_parsed && ic->parsed.image.width > 0) {
        int img_h = ic->parsed.image.height;
        int img_w = ic->parsed.image.width;
        int ix = ic->x + (ICON_W - img_w) / 2;
        int iy = ic->y + (ICON_H - ICON_LABEL_H - img_h) / 2;
        if (iy < ic->y) iy = ic->y;
        if (ic->is_selected) {
            Icon_DrawSelected(&ic->parsed, ix, iy);
        } else {
            Icon_Draw(&ic->parsed, ix, iy);
        }
        int lbl_x = ic->x + (ICON_W - LABEL_W) / 2;
        if (lbl_x < 0) lbl_x = 0;
        Icon_DrawLabel(&ic->parsed, lbl_x, ic->y + ICON_H - ICON_LABEL_H, LABEL_W);
    } else {
        uint32_t colour = ic->is_ndos ? WB_DARK_GREY : WB_ORANGE;
        draw_disk_icon(ic->x, ic->y, ic->label, colour, ic->is_selected);
    }
}

/* =========================================================================
 * Lasso (rubber-band) selection rectangle
 * ========================================================================= */

/* Draw a dashed horizontal line — 1px on / 1px off, black.
 * Approximates the Workbench marquee selection border. */
static void draw_dashed_hline(int x, int y, int len)
{
    for (int i = 0; i < len; i += 2)
        FB_PutPixel(x + i, y, WB_BLACK);
}

/* Draw a dashed vertical line — 1px on / 1px off, black. */
static void draw_dashed_vline(int x, int y, int len)
{
    for (int i = 0; i < len; i += 2)
        FB_PutPixel(x, y + i, WB_BLACK);
}

/* Draw the lasso rectangle if active.  Clipped to the desktop backdrop
 * area (below the menu bar) so it never overdraws chrome. */
static void draw_lasso(int W, int H)
{
    if (!g_lasso_active) return;

    int top    = MENUBAR_H;
    int bottom = H;

    /* Normalise the rectangle regardless of drag direction */
    int x0 = g_lasso_start_x < g_lasso_cur_x ? g_lasso_start_x : g_lasso_cur_x;
    int y0 = g_lasso_start_y < g_lasso_cur_y ? g_lasso_start_y : g_lasso_cur_y;
    int x1 = g_lasso_start_x < g_lasso_cur_x ? g_lasso_cur_x   : g_lasso_start_x;
    int y1 = g_lasso_start_y < g_lasso_cur_y ? g_lasso_cur_y   : g_lasso_start_y;

    /* Clip to desktop backdrop */
    if (y0 < top)    y0 = top;
    if (y1 >= bottom) y1 = bottom - 1;
    if (x0 < 0)      x0 = 0;
    if (x1 >= W)     x1 = W - 1;
    if (x1 <= x0 || y1 <= y0) return;

    int w = x1 - x0 + 1;
    int h = y1 - y0 + 1;

    draw_dashed_hline(x0, y0, w);
    draw_dashed_hline(x0, y1, w);
    draw_dashed_vline(x0, y0, h);
    draw_dashed_vline(x1, y0, h);
}

/* =========================================================================
 * Public entry
 * ========================================================================= */

/* Repaint a rectangular region of the desktop backdrop (stipple pattern).
 * Writes directly to the framebuffer row buffer for speed — avoids per-pixel
 * function call overhead so drag/resize does not cause full-screen flicker. */
void Desktop_RedrawRect(int rx, int ry, int rw, int rh)
{
    if (!g_fb.valid) return;
    int W = (int)g_fb.width;
    int H = (int)g_fb.height;
    int top    = MENUBAR_H;
    int bottom = H;

    /* Clip to backdrop area */
    int x0 = rx < 0 ? 0 : rx;
    int y0 = ry < top ? top : ry;
    int x1 = rx + rw > W ? W : rx + rw;
    int y1 = ry + rh > bottom ? bottom : ry + rh;
    if (x1 <= x0 || y1 <= y0) return;

    /* Base grey fill */
    FB_FillRect(x0, y0, x1 - x0, y1 - y0, WB_GREY);

    /* Repaint desktop icons — all icons come from get_icons (VFS + partitions).
     * Hidden when the Backdrop menu toggle is active. */
    if (!g_backdrop_hidden) {
        int n;
        IconState *icons = get_icons(&n);
        for (int i = 0; i < n; i++) {
            draw_icon_state(&icons[i]);
        }
    }

    /* Lasso rectangle on top of icons, below bars */
    draw_lasso(W, H);

    /* Always repaint menubar — a window may have overlapped it */
    draw_menubar(W);
}

void Desktop_Draw(void)
{
    if (!g_fb.valid) return;

    /* Use the front Intuition screen's palette for desktop chrome. */
    UAOS_Intuition_ApplyFrontScreenPalette();

    int W = (int)g_fb.width;
    int H = (int)g_fb.height;

    draw_backdrop(W, H);
    draw_menubar(W);

    /* Disk icons — all come from get_icons (VFS-mounted volumes + partitions).
     * Hidden when the Backdrop menu toggle is active. */
    if (!g_backdrop_hidden) {
        int n;
        IconState *icons = get_icons(&n);
        for (int i = 0; i < n; i++) {
            draw_icon_state(&icons[i]);
        }
    }

    /* Lasso rectangle on top of icons, below menu dropdown */
    draw_lasso(W, H);
}

/* Draw the open Workbench menu dropdown on top of everything else.
 * Called by WM_Redraw after all windows have been painted. */
void Desktop_DrawMenuDropdown(void)
{
    if (!g_fb.valid) return;
    draw_menu_dropdown((int)g_fb.width);
}

/* Workbench load control - prevents desktop from showing before LoadWB */
static int g_workbench_loaded = 0;

void Desktop_MarkWorkbenchLoaded(void)
{
    g_workbench_loaded = 1;
}

int Desktop_IsWorkbenchLoaded(void)
{
    return g_workbench_loaded;
}

int Desktop_IsMenuOpen(void)
{
    return g_menu_index >= 0;
}

void Desktop_SetScreenTitle(const char *title, int show)
{
    g_show_screen_title = show;
    if (title) {
        int i = 0;
        while (i < (int)sizeof(g_screen_title) - 1 && title[i]) {
            g_screen_title[i] = title[i];
            i++;
        }
        g_screen_title[i] = '\0';
    } else {
        g_screen_title[0] = '\0';
    }
    WM_Redraw();
}

void Desktop_DisplayBeepFlash(uint32_t color)
{
    if (color) {
        g_beep_flash_color = color;
        g_beep_flash_until = g_pit_ticks + 5;  /* 50 ms at 100 Hz */
    } else {
        g_beep_flash_color = 0;
        g_beep_flash_until = 0;
    }
}

void Desktop_ToggleBackdrop(void)
{
    g_backdrop_hidden = !g_backdrop_hidden;
    WM_Redraw();
}

void Desktop_LeaveOutAdd(const char *path, const char *name, int is_dir)
{
    if (!path || !name) return;
    /* Find a free slot (or skip if the path is already leave-out) */
    for (int i = 0; i < MAX_LEAVEOUT; i++) {
        if (g_leaveout[i].valid) {
            int same = 1;
            for (int k = 0; k < 128; k++) {
                if (g_leaveout[i].path[k] != path[k]) { same = 0; break; }
                if (path[k] == '\0') break;
            }
            if (same) return;  /* already leave-out */
        }
    }
    for (int i = 0; i < MAX_LEAVEOUT; i++) {
        if (!g_leaveout[i].valid) {
            int k;
            for (k = 0; k < 127 && path[k]; k++) g_leaveout[i].path[k] = path[k];
            g_leaveout[i].path[k] = '\0';
            for (k = 0; k < 31 && name[k]; k++) g_leaveout[i].label[k] = name[k];
            g_leaveout[i].label[k] = '\0';
            g_leaveout[i].is_dir = is_dir;
            g_leaveout[i].valid = 1;
            g_leaveout_version++;
            WM_Redraw();
            return;
        }
    }
}

void Desktop_LeaveOutRemoveSelected(void)
{
    int n;
    IconState *icons = get_icons(&n);
    for (int i = 0; i < n; i++) {
        if (icons[i].is_leaveout && icons[i].is_selected && icons[i].leaveout_path) {
            /* Find the matching registry entry and clear it */
            for (int li = 0; li < MAX_LEAVEOUT; li++) {
                if (!g_leaveout[li].valid) continue;
                int same = 1;
                for (int k = 0; k < 128; k++) {
                    if (g_leaveout[li].path[k] != icons[i].leaveout_path[k]) {
                        same = 0; break;
                    }
                    if (icons[i].leaveout_path[k] == '\0') break;
                }
                if (same) {
                    g_leaveout[li].valid = 0;
                    g_leaveout_version++;
                    WM_Redraw();
                    return;
                }
            }
        }
    }
}

/* =========================================================================
 * Tick counter — incremented by Desktop_UpdateClock (once per second)
 * Used for double-click timing: two clicks within 2 ticks = double-click
 * ========================================================================= */

static volatile uint32_t g_tick = 0;
static volatile int g_clock_dirty = 0;

/* Return the number of available menus (guest strip or fallback). */
static int active_menu_count(void)
{
    return g_guest_menu_active ? g_active_menu_count : (int)NUM_MENUS;
}

/* Hit-test the menubar and return which menu index was clicked (-1 = none).
 * Replicates the layout logic from draw_menubar. */
static int menubar_hit(int mx, int my)
{
    if (my < 0 || my >= MENUBAR_H) return -1;
    int x = 8;
    for (int i = 0; i < active_menu_count(); i++) {
        const char *title = menu_title(i);
        if (!title) break;
        int len = 0;
        for (const char *p = title; *p; p++) len++;
        int x1 = x + len * 8 + 8;  /* right edge of hit zone */
        if (mx >= x - 4 && mx < x1)
            return i;
        x += len * 8 + 16;
    }
    return -1;
}

/* Return the item index under (mx,my) when a menu is open,
 * or -1 if the point is outside the dropdown. */
static int dropdown_hit(int mx, int my)
{
    if (g_menu_index < 0 || g_menu_index >= active_menu_count()) return -1;
    if (mx < g_menu_x || mx >= g_menu_x + g_menu_w ||
        my < g_menu_y || my >= g_menu_y + g_menu_h)
        return -1;

    int item_h = 16;
    int pad_y  = 2;
    int ry = my - g_menu_y - pad_y;
    if (ry < 0) return -1;
    int idx = ry / item_h;

    if (g_guest_menu_active) {
        const HostMenu *menu = &g_active_menus[g_menu_index];
        if (idx < 0 || idx >= menu->item_count) return -1;
        if (!menu->items[idx].enabled) return -1;
        return idx;
    }

    const MenuItem *items = g_menus[g_menu_index];
    if (idx < 0 || idx >= menu_item_count(items)) return -1;
    if (items[idx].is_divider) return -1;
    return idx;
}

/* Return the submenu item index under (mx,my) when a submenu is open,
 * or -1 if the point is outside the submenu. */
static int submenu_hit(int mx, int my)
{
    if (g_submenu_item < 0) return -1;
    if (mx < g_submenu_x || mx >= g_submenu_x + g_submenu_w ||
        my < g_submenu_y || my >= g_submenu_y + g_submenu_h)
        return -1;

    int item_h = 16;
    int pad_y  = 2;
    int ry = my - g_submenu_y - pad_y;
    if (ry < 0) return -1;
    int idx = ry / item_h;

    if (g_guest_menu_active) {
        const HostMenu *menu = &g_active_menus[g_menu_index];
        if (g_submenu_item < 0 || g_submenu_item >= menu->item_count) return -1;
        const HostMenu *sm = menu->items[g_submenu_item].submenu;
        if (!sm) return -1;
        if (idx < 0 || idx >= sm->item_count) return -1;
        if (!sm->items[idx].enabled) return -1;
        return idx;
    }

    /* Fallback menu — resolve submenu from the MenuItem table */
    if (g_menu_index < 0 || g_menu_index >= (int)NUM_MENUS) return -1;
    const MenuItem *items = g_menus[g_menu_index];
    int n = menu_item_count(items);
    if (g_submenu_item < 0 || g_submenu_item >= n) return -1;
    if (!items[g_submenu_item].has_submenu || !items[g_submenu_item].submenu)
        return -1;
    const MenuItem *sub = items[g_submenu_item].submenu;
    int sn = menu_item_count(sub);
    if (idx < 0 || idx >= sn) return -1;
    if (sub[idx].is_divider) return -1;
    return idx;
}

/* Update menu hover state and request a redraw if it changed.
 * Also switches to a different menu title if the cursor moves over it while
 * a menu is already open, and opens submenus for items that have them. */
static void menu_update_hover(int mx, int my)
{
    if (g_menu_index < 0) return;

    /* Switch menus if the cursor moves over another menu title. */
    if (my >= 0 && my < MENUBAR_H) {
        int menu = menubar_hit(mx, my);
        if (menu >= 0 && menu < active_menu_count() && menu != g_menu_index) {
            g_menu_index = menu;
            g_menu_hover = -1;
            g_submenu_item = -1;
            g_submenu_hover = -1;
            WM_Redraw();
            return;
        }
    }

    if (g_guest_menu_active) {
        int sub_hover = submenu_hit(mx, my);
        if (sub_hover >= 0) {
            if (sub_hover != g_submenu_hover) {
                g_submenu_hover = sub_hover;
                WM_Redraw();
            }
            return;
        }
    } else {
        /* Fallback menu — track submenu hover too */
        int sub_hover = submenu_hit(mx, my);
        if (sub_hover >= 0) {
            if (sub_hover != g_submenu_hover) {
                g_submenu_hover = sub_hover;
                WM_Redraw();
            }
            return;
        }
    }

    int new_hover = dropdown_hit(mx, my);
    if (new_hover != g_menu_hover) {
        g_menu_hover = new_hover;
        g_submenu_hover = -1;
        if (g_guest_menu_active && g_menu_hover >= 0 &&
            g_menu_hover < g_active_menus[g_menu_index].item_count &&
            g_active_menus[g_menu_index].items[g_menu_hover].has_submenu) {
            g_submenu_item = g_menu_hover;
        } else if (!g_guest_menu_active && g_menu_hover >= 0 &&
                   g_menu_index >= 0 && g_menu_index < (int)NUM_MENUS) {
            const MenuItem *items = g_menus[g_menu_index];
            if (g_menu_hover < menu_item_count(items) &&
                items[g_menu_hover].has_submenu)
                g_submenu_item = g_menu_hover;
            else
                g_submenu_item = -1;
        } else {
            g_submenu_item = -1;
        }
        WM_Redraw();
    }
}

int Desktop_MouseEvent(int mx, int my, int left_pressed, int right_pressed)
{
    if (!left_pressed && !right_pressed) return 0;

    /* ── Right-click on menubar: open the selected menu ───── */
    if (right_pressed && my >= 0 && my < MENUBAR_H) {
        int menu = menubar_hit(mx, my);
        if (menu >= 0 && menu < active_menu_count()) {
            g_menu_index = menu;
            g_menu_hover = -1;
            g_submenu_item = -1;
            g_submenu_hover = -1;
            menu_update_hover(mx, my);
            WM_Redraw();
            return 1;
        }
        /* Right-click on empty menu bar area: close any open menu. */
        g_menu_index = -1;
        g_menu_hover = -1;
        g_submenu_item = -1;
        g_submenu_hover = -1;
        WM_Redraw();
        return 1;
    }

    /* Left-click while a menu is open: close the menu without triggering an
     * action.  This lets the user dismiss a menu with the left button. */
    if (left_pressed && g_menu_index >= 0) {
        g_menu_index = -1;
        g_menu_hover = -1;
        g_submenu_item = -1;
        g_submenu_hover = -1;
        WM_Redraw();
        return 1;
    }

    /* ── Left-click on menubar ──────────────────────────── */
    int menu = menubar_hit(mx, my);
    if (left_pressed && menu >= 0) {
        return 1;
    }

    /* ── Desktop icon press (start potential drag) ─────── */
    int n;
    IconState *icons = get_icons(&n);

    DT_LOG("[DT] Checking "); DT_LOG_DEC(n); DT_LOG(" icons for hit\n");

    for (int i = 0; i < n; i++) {
        IconState *ic = &icons[i];
        /* Hit-test includes the wider label area (LABEL_W > ICON_W) */
        int lbl_x = ic->x + (ICON_W - LABEL_W) / 2;
        if (lbl_x < 0) lbl_x = 0;
        if (mx >= lbl_x && mx < lbl_x + LABEL_W &&
            my >= ic->y && my < ic->y + ICON_H) {

            DT_LOG("[DT] Icon "); DT_LOG_DEC(i); DT_LOG(" press, volume='");
            DT_LOG(ic->volume); DT_LOG("'\n");

            /* Select the clicked icon and deselect all others */
            int changed = 0;
            for (int j = 0; j < n; j++) {
                int want = (j == i) ? 1 : 0;
                if (icons[j].is_selected != want) {
                    icons[j].is_selected = want;
                    changed = 1;
                }
            }
            if (changed) WM_Redraw();

            g_icon_drag_idx    = i;
            g_icon_drag_off_x  = mx - ic->x;
            g_icon_drag_off_y  = my - ic->y;
            g_icon_drag_orig_x = ic->x;
            g_icon_drag_orig_y = ic->y;
            g_icon_drag_moved  = 0;
            g_desktop_pressed  = 0;
            g_lasso_active     = 0;
            return 1;
        }
    }
    /* Missed all icons — deselect all, start a lasso for potential drag
     * selection, and mark as desktop background press for double-click. */
    {
        int changed = 0;
        for (int j = 0; j < n; j++) {
            if (icons[j].is_selected) {
                icons[j].is_selected = 0;
                changed = 1;
            }
        }
        if (changed) WM_Redraw();
    }
    g_desktop_pressed = 1;
    g_lasso_active  = 1;
    g_lasso_start_x = mx;
    g_lasso_start_y = my;
    g_lasso_cur_x   = mx;
    g_lasso_cur_y   = my;
    g_lasso_moved   = 0;
    return 0;
}

void Desktop_MouseHover(int mx, int my)
{
    menu_update_hover(mx, my);
}

void Desktop_RightButtonRelease(int mx, int my)
{
    (void)mx; (void)my;

    if (g_menu_index < 0) return;

    if (g_guest_menu_active) {
        HostMenuItem *mi = NULL;
        uint32_t menu_number = 0;
        if (g_submenu_item >= 0 && g_submenu_hover >= 0) {
            const HostMenu *menu = &g_active_menus[g_menu_index];
            if (g_submenu_item < menu->item_count && menu->items[g_submenu_item].submenu) {
                mi = &menu->items[g_submenu_item].submenu->items[g_submenu_hover];
                menu_number = (uint32_t)((g_menu_index & 0x1F) |
                                         ((g_submenu_item & 0x3F) << 5) |
                                         ((g_submenu_hover & 0x1F) << 11));
            }
        } else if (g_menu_hover >= 0) {
            mi = &g_active_menus[g_menu_index].items[g_menu_hover];
            menu_number = (uint32_t)((g_menu_index & 0x1F) |
                                     ((g_menu_hover & 0x3F) << 5));
        }
        if (mi) {
            Intuition_UpdateMenuItemCheck(mi->guest_item, mi->toggle);
            Intuition_PostMenuPick(menu_number);
        }
    } else {
        /* Fallback menu — dispatch submenu or top-level item */
        if (g_submenu_item >= 0 && g_submenu_hover >= 0) {
            const MenuItem *items = g_menus[g_menu_index];
            int n = menu_item_count(items);
            if (g_submenu_item < n && items[g_submenu_item].has_submenu &&
                items[g_submenu_item].submenu) {
                const MenuItem *sub = items[g_submenu_item].submenu;
                int sn = menu_item_count(sub);
                if (g_submenu_hover < sn && sub[g_submenu_hover].action)
                    sub[g_submenu_hover].action();
            }
        } else if (g_menu_hover >= 0) {
            const MenuItem *items = g_menus[g_menu_index];
            if (items[g_menu_hover].action)
                items[g_menu_hover].action();
        }
    }

    g_menu_index = -1;
    g_menu_hover = -1;
    g_submenu_item = -1;
    g_submenu_hover = -1;
    WM_Redraw();
}

void Desktop_MouseMove(int mx, int my, int btn_left)
{
    (void)btn_left;

    /* Menu hover tracking while the button is held. */
    menu_update_hover(mx, my);

    /* Lasso (rubber-band) selection — update the drag rectangle and
     * select every icon whose bounding box intersects it. */
    if (g_lasso_active) {
        if (mx == g_lasso_cur_x && my == g_lasso_cur_y) return;
        g_lasso_cur_x = mx;
        g_lasso_cur_y = my;
        g_lasso_moved = 1;

        /* Normalise lasso rectangle (drag may go any direction) */
        int lx0 = g_lasso_start_x < g_lasso_cur_x ? g_lasso_start_x : g_lasso_cur_x;
        int ly0 = g_lasso_start_y < g_lasso_cur_y ? g_lasso_start_y : g_lasso_cur_y;
        int lx1 = g_lasso_start_x < g_lasso_cur_x ? g_lasso_cur_x   : g_lasso_start_x;
        int ly1 = g_lasso_start_y < g_lasso_cur_y ? g_lasso_cur_y   : g_lasso_start_y;

        int n;
        IconState *icons = get_icons(&n);
        int changed = 0;
        for (int i = 0; i < n; i++) {
            IconState *ic = &icons[i];
            /* AABB intersection: icon bbox vs lasso rect */
            int hit = !(ic->x + ICON_W <= lx0 || ic->x >= lx1 + 1 ||
                        ic->y + ICON_H <= ly0 || ic->y >= ly1 + 1);
            if (ic->is_selected != hit) {
                ic->is_selected = hit;
                changed = 1;
            }
        }
        /* Always redraw — the lasso rectangle itself moved even if no
         * icon selection changed. */
        WM_Redraw();
        (void)changed;
        return;
    }

    if (g_icon_drag_idx < 0) return;

    int n;
    IconState *icons = get_icons(&n);
    if (g_icon_drag_idx >= n) {
        g_icon_drag_idx = -1;
        return;
    }

    IconState *ic = &icons[g_icon_drag_idx];
    int new_x = mx - g_icon_drag_off_x;
    int new_y = my - g_icon_drag_off_y;

    /* Clamp to desktop bounds */
    int W = (int)g_fb.width;
    int H = (int)g_fb.height;
    int top = MENUBAR_H;
    int bottom = H;

    if (new_x < 0) new_x = 0;
    if (new_x > W - ICON_W) new_x = W - ICON_W;
    if (new_y < top) new_y = top;
    if (new_y > bottom - ICON_H) new_y = bottom - ICON_H;

    if (new_x != ic->x || new_y != ic->y) {
        if (new_x != g_icon_drag_orig_x || new_y != g_icon_drag_orig_y)
            g_icon_drag_moved = 1;
        ic->x = new_x;
        ic->y = new_y;
        WM_Redraw();
    }
}

void Desktop_MouseRelease(int mx, int my)
{
    (void)mx; (void)my;

    if (g_icon_drag_idx >= 0) {
        int n;
        IconState *icons = get_icons(&n);
        if (g_icon_drag_idx < n && !g_icon_drag_moved) {
            /* Treat as click — double-click logic */
            IconState *ic = &icons[g_icon_drag_idx];
            uint32_t now = g_tick;
            if (ic->click_count > 0 && (now - ic->last_tick) <= DBLCLICK_TICKS) {
                DT_LOG("[DT] Double-click icon "); DT_LOG_DEC(g_icon_drag_idx);
                DT_LOG(" vol='"); DT_LOG(ic->volume ? ic->volume : "(null)");
                DT_LOG("'\n");
                ic->click_count = 0;
                if (ic->is_appicon) {
                    /* AppIcon double-click — would send AppMessage to
                     * the registered msg port.  For now, log it. */
                    DT_LOG("[DT] AppIcon activated, id=");
                    DT_LOG_DEC(ic->appicon_id);
                    DT_LOG("\n");
                } else if (ic->is_leaveout && ic->leaveout_path) {
                    /* Leave Out shortcut — open (dir) or run (file) */
                    if (ic->leaveout_is_dir) {
                        FileBrowser_Open(ic->leaveout_path);
                    } else {
                        extern void ExecFile_Run(const char *path, const char *args);
                        ExecFile_Run(ic->leaveout_path, "");
                    }
                } else if (ic->volume) {
                    FileBrowser_Open(ic->volume);
                }
            } else {
                DT_LOG("[DT] First click (release)\n");
                ic->click_count = 1;
                ic->last_tick   = now;
            }
        }
        g_icon_drag_idx = -1;
        g_desktop_pressed = 0;
        return;
    }

    /* Desktop background double-click opens a new Shell.
     * A lasso drag (cursor moved) is NOT a click — suppress double-click.
     * Also ends lasso selection — the current selection is kept. */
    if (g_desktop_pressed) {
        if (!g_lasso_moved) {
            uint32_t now = g_tick;
            if (g_desktop_click_count > 0 && (now - g_desktop_last_tick) <= DBLCLICK_TICKS) {
                g_desktop_click_count = 0;
                ShellWin_Open();
            } else {
                g_desktop_click_count = 1;
                g_desktop_last_tick = now;
            }
        }
        g_desktop_pressed = 0;
    }
    if (g_lasso_active) {
        g_lasso_active = 0;
        WM_Redraw();
    }
}

int Desktop_IsDraggingIcon(void)
{
    return g_icon_drag_idx >= 0;
}

unsigned int Desktop_GetTick(void)
{
    return (unsigned int)g_tick;
}

void Desktop_UpdateClock(void)
{
    g_tick++;
    g_clock_dirty = 1;
    Blanker_Tick();
}

void Desktop_FlushClockRedraw(void)
{
    if (g_clock_dirty) {
        g_clock_dirty = 0;
        WM_Redraw();
    }
}

