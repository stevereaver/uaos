/*
 * workbench_lib.c — UAOS workbench.library Implementation
 *
 * AmigaOS workbench.library provides icon management, desktop interaction,
 * and app-icon / app-window registration. This is a native implementation
 * for UAOS backed by the existing desktop and VFS layers.
 *
 * All functions read their arguments from the Musashi m68k register file.
 */

#include "rom_modules.h"
#include "workbench_lib.h"
#include "icon_def.h"
#include "../display/framebuffer.h"
#include "../dos/vfs.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

/* =========================================================================
 * Musashi register access (provided by emulation/uaos_m68k_glue.c)
 * ========================================================================= */

extern unsigned int m68k_get_reg(void *context, int reg);
extern void         m68k_set_reg(int reg, unsigned int value);
extern unsigned int m68k_read_memory_8(unsigned int addr);
extern unsigned int m68k_read_memory_16(unsigned int addr);
extern unsigned int m68k_read_memory_32(unsigned int addr);
extern void         m68k_write_memory_8(unsigned int addr, unsigned int val);
extern void         m68k_write_memory_16(unsigned int addr, unsigned int val);
extern void         m68k_write_memory_32(unsigned int addr, unsigned int val);

#define M68K_REG_D0  0
#define M68K_REG_D1  1
#define M68K_REG_D2  2
#define M68K_REG_D3  3
#define M68K_REG_A0  8
#define M68K_REG_A1  9
#define M68K_REG_A2  10

/* =========================================================================
 * Helper: read null-terminated string from guest RAM
 * ========================================================================= */

static void guest_str(char *dst, uint32_t src, int max)
{
    int i = 0;
    while (i < max - 1) {
        uint8_t c = (uint8_t)m68k_read_memory_8(src + i);
        if (c == 0) break;
        dst[i] = (char)c;
        i++;
    }
    dst[i] = '\0';
}

/* =========================================================================
 * App icon / window tracking
 * ========================================================================= */

#define MAX_APP_ICONS   16
#define MAX_APP_WINDOWS 16

typedef struct {
    uint32_t id;          /* unique app-provided ID */
    uint32_t msg_port;    /* guest pointer to MsgPort */
    uint32_t disk_obj;    /* guest pointer to DiskObject */
    char     label[ICON_MAX_LABEL];
    uint8_t  active;
} AppIconSlot;

typedef struct {
    uint32_t id;
    uint32_t msg_port;
    uint32_t window;      /* guest pointer to Window */
    uint8_t  active;
} AppWindowSlot;

static AppIconSlot   g_app_icons[MAX_APP_ICONS];
static AppWindowSlot g_app_windows[MAX_APP_WINDOWS];
static uint32_t      g_next_icon_id = 1;
static uint32_t      g_next_window_id = 1;

static AppIconSlot *alloc_app_icon(void)
{
    for (int i = 0; i < MAX_APP_ICONS; i++) {
        if (!g_app_icons[i].active) {
            g_app_icons[i].active = 1;
            g_app_icons[i].id = g_next_icon_id++;
            return &g_app_icons[i];
        }
    }
    return NULL;
}

static void free_app_icon(AppIconSlot *slot)
{
    if (slot) {
        slot->active = 0;
        slot->msg_port = 0;
        slot->disk_obj = 0;
    }
}

static AppIconSlot *find_app_icon(uint32_t id)
{
    for (int i = 0; i < MAX_APP_ICONS; i++) {
        if (g_app_icons[i].active && g_app_icons[i].id == id)
            return &g_app_icons[i];
    }
    return NULL;
}

static AppWindowSlot *alloc_app_window(void)
{
    for (int i = 0; i < MAX_APP_WINDOWS; i++) {
        if (!g_app_windows[i].active) {
            g_app_windows[i].active = 1;
            g_app_windows[i].id = g_next_window_id++;
            return &g_app_windows[i];
        }
    }
    return NULL;
}

static void free_app_window(AppWindowSlot *slot)
{
    if (slot) {
        slot->active = 0;
        slot->msg_port = 0;
        slot->window = 0;
    }
}

/* =========================================================================
 * workbench.library function indices (must match AmigaOS LVO offsets)
 * ========================================================================= */

#define WORKBENCH_OPEN_LIBRARY        1
#define WORKBENCH_CLOSE_LIBRARY       2
#define WORKBENCH_ADD_APP_ICON        3
#define WORKBENCH_REMOVE_APP_ICON     4
#define WORKBENCH_ADD_APP_WINDOW      5
#define WORKBENCH_REMOVE_APP_WINDOW   6
#define WORKBENCH_BEGIN_REFRESH       7
#define WORKBENCH_END_REFRESH         8
#define WORKBENCH_GET_NEXT_ICON        9
#define WORKBENCH_WORKBENCH_CONTROL   10
#define WORKBENCH_OPEN_WORKBENCH_OBJ  11
#define WORKBENCH_CLOSE_WORKBENCH_OBJ 12
#define WORKBENCH_ICONIFY             13
#define WORKBENCH_ADD_APP_ICONA       14
#define WORKBENCH_ADD_APP_WINDOWA     15

/* =========================================================================
 * Implementation
 * ========================================================================= */

static void workbench_OpenLibrary(void)
{
    /* OpenLibrary — return library base (set by exec_OpenLibrary in glue) */
}

static void workbench_CloseLibrary(void)
{
    /* CloseLibrary — no-op for ROM library */
}

/* AddAppIcon(id, text, msgPort, lock, diskObj, tagList)
 * D0 = id, A0 = text, A1 = msgPort, D1 = lock, A2 = diskObj, A3 = tagList
 * Returns: AppIcon pointer (guest) or NULL */
static void workbench_AddAppIcon(void)
{
    uint32_t id      = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t text    = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t msgPort = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t lock    = m68k_get_reg(NULL, M68K_REG_D1);
    uint32_t diskObj = m68k_get_reg(NULL, M68K_REG_A2);

    AppIconSlot *slot = alloc_app_icon();
    if (!slot) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }

    slot->id       = id;
    slot->msg_port = msgPort;
    slot->disk_obj = diskObj;
    guest_str(slot->label, text, ICON_MAX_LABEL);

    /* Return a synthetic guest pointer (not a real guest address, but
     * unique enough for the app to pass back later). */
    m68k_set_reg(M68K_REG_D0, (unsigned int)(0x01000000U + slot->id * 256));
}

/* RemoveAppIcon(appIcon)
 * A0 = appIcon pointer returned by AddAppIcon
 * Returns: success (BOOL) */
static void workbench_RemoveAppIcon(void)
{
    uint32_t ptr = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t id  = (ptr - 0x01000000U) / 256;

    AppIconSlot *slot = find_app_icon(id);
    if (slot) {
        free_app_icon(slot);
        m68k_set_reg(M68K_REG_D0, 1);
    } else {
        m68k_set_reg(M68K_REG_D0, 0);
    }
}

/* AddAppWindow(id, text, msgPort, window, tagList)
 * D0 = id, A0 = text, A1 = msgPort, A2 = window, A3 = tagList
 * Returns: AppWindow pointer (guest) or NULL */
static void workbench_AddAppWindow(void)
{
    uint32_t id      = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t text    = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t msgPort = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t window  = m68k_get_reg(NULL, M68K_REG_A2);

    AppWindowSlot *slot = alloc_app_window();
    if (!slot) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }

    slot->id       = id;
    slot->msg_port = msgPort;
    slot->window   = window;

    m68k_set_reg(M68K_REG_D0, (unsigned int)(0x02000000U + slot->id * 256));
}

/* RemoveAppWindow(appWindow)
 * A0 = appWindow pointer returned by AddAppWindow
 * Returns: success (BOOL) */
static void workbench_RemoveAppWindow(void)
{
    uint32_t ptr = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t id  = (ptr - 0x02000000U) / 256;

    for (int i = 0; i < MAX_APP_WINDOWS; i++) {
        if (g_app_windows[i].active && g_app_windows[i].id == id) {
            free_app_window(&g_app_windows[i]);
            m68k_set_reg(M68K_REG_D0, 1);
            return;
        }
    }
    m68k_set_reg(M68K_REG_D0, 0);
}

static void workbench_BeginRefresh(void)
{
    /* BeginRefresh — stub, native WM handles its own refresh */
}

static void workbench_EndRefresh(void)
{
    /* EndRefresh — stub */
}

/* GetNextIcon(lock, buffer, size)
 * D0 = lock, A0 = buffer, D1 = size
 * Returns: 1 on success, 0 when no more icons */
static void workbench_GetNextIcon(void)
{
    /* Not yet implemented — no icon enumeration via library yet */
    m68k_set_reg(M68K_REG_D0, 0);
}

/* WorkbenchControl(tag, data)
 * D0 = tag, A0 = data
 * Returns: 1 on success, 0 on failure */
static void workbench_WorkbenchControl(void)
{
    uint32_t tag = m68k_get_reg(NULL, M68K_REG_D0);
    (void)tag;
    m68k_set_reg(M68K_REG_D0, 1);
}

/* OpenWorkbenchObject(name, tags)
 * A0 = name string, A1 = tag list
 * Returns: 1 on success, 0 on failure */
static void workbench_OpenWorkbenchObject(void)
{
    char path[128];
    uint32_t name_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    guest_str(path, name_ptr, sizeof(path));

    fprintf(stderr, "[WORKBENCH] OpenWorkbenchObject: %s\n", path);

    /* TODO: Launch via VFS / shell execute */
    m68k_set_reg(M68K_REG_D0, 1);
}

/* CloseWorkbenchObject(lock)
 * D0 = lock
 * Returns: 1 on success, 0 on failure */
static void workbench_CloseWorkbenchObject(void)
{
    m68k_set_reg(M68K_REG_D0, 1);
}

static void workbench_Iconify(void)
{
    /* Iconify — stub */
    m68k_set_reg(M68K_REG_D0, 0);
}

static void workbench_AddAppIconA(void)
{
    /* Tag-list variant — delegate to plain AddAppIcon for now */
    workbench_AddAppIcon();
}

static void workbench_AddAppWindowA(void)
{
    /* Tag-list variant — delegate to plain AddAppWindow for now */
    workbench_AddAppWindow();
}

/* =========================================================================
 * Function table (1-based indexing)
 * ========================================================================= */

static void *workbench_funcs[] = {
    workbench_OpenLibrary,        /* 1  */
    workbench_CloseLibrary,       /* 2  */
    workbench_AddAppIcon,         /* 3  */
    workbench_RemoveAppIcon,      /* 4  */
    workbench_AddAppWindow,       /* 5  */
    workbench_RemoveAppWindow,    /* 6  */
    workbench_BeginRefresh,       /* 7  */
    workbench_EndRefresh,         /* 8  */
    workbench_GetNextIcon,        /* 9  */
    workbench_WorkbenchControl,   /* 10 */
    workbench_OpenWorkbenchObject,  /* 11 */
    workbench_CloseWorkbenchObject, /* 12 */
    workbench_Iconify,            /* 13 */
    workbench_AddAppIconA,        /* 14 */
    workbench_AddAppWindowA,      /* 15 */
};

/* =========================================================================
 * Registration
 * ========================================================================= */

void UAOS_WORKBENCH_Register(void)
{
    UAOS_ROM_Register("workbench.library", 45, 0x00000000,
                      (uint16_t)(sizeof(workbench_funcs) / sizeof(workbench_funcs[0])),
                      workbench_funcs);
}

/* =========================================================================
 * AppIcon query API — used by desktop.c to render AppIcons on the desktop
 * ========================================================================= */

int WB_GetAppIconCount(void)
{
    int count = 0;
    for (int i = 0; i < MAX_APP_ICONS; i++) {
        if (g_app_icons[i].active)
            count++;
    }
    return count;
}

int WB_GetAppIcon(int index, AppIconInfo *out)
{
    if (!out) return 0;
    int seen = 0;
    for (int i = 0; i < MAX_APP_ICONS; i++) {
        if (!g_app_icons[i].active) continue;
        if (seen == index) {
            out->id       = g_app_icons[i].id;
            out->msg_port = g_app_icons[i].msg_port;
            out->disk_obj = g_app_icons[i].disk_obj;
            /* Copy label */
            for (int k = 0; k < APPICON_MAX_LABEL; k++) {
                out->label[k] = g_app_icons[i].label[k];
                if (g_app_icons[i].label[k] == '\0') break;
            }
            out->label[APPICON_MAX_LABEL - 1] = '\0';
            return 1;
        }
        seen++;
    }
    return 0;
}
