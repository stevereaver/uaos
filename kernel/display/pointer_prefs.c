/* pointer_prefs.c — UAOS Pointer Preferences Tool
 *
 * AmigaOS 3.1-style pointer preferences editor: cursor size, double-pixel
 * mode, and acceleration.  Declarative UI — the window's controls are a
 * UINode tree bound to a WM window by uibind (UAOS-125); no manual
 * coordinates remain in this file.
 */

#include "pointer_prefs.h"
#include "uibind.h"
#include "uitree.h"
#include "cursor.h"
#include "wm.h"

/* =========================================================================
 * Node ids
 * ========================================================================= */

enum {
    ID_SIZE_16 = 1, ID_SIZE_32, ID_SIZE_48,
    ID_DP_OFF, ID_DP_ON,
    ID_ACC_0, ID_ACC_25, ID_ACC_50, ID_ACC_75, ID_ACC_100,
    ID_APPLY, ID_CLOSE
};

/* =========================================================================
 * UI state
 * ========================================================================= */

static UIBind        *g_bind;
static CursorSettings g_current_settings;
static UIArena        g_arena;
static unsigned char  g_arena_buf[4096];

static const int g_size_ids[3] = { ID_SIZE_16, ID_SIZE_32, ID_SIZE_48 };
static const int g_dp_ids  [2] = { ID_DP_OFF,  ID_DP_ON };
static const int g_acc_ids [5] = { ID_ACC_0, ID_ACC_25, ID_ACC_50,
                                   ID_ACC_75, ID_ACC_100 };

/* =========================================================================
 * Helpers
 * ========================================================================= */

/* Radio-style exclusive highlight across a button group (the buttons
 * render pressed when they are the current selection). */
static void sel_group(UIBind *b, const int *ids, int n, int hit_id)
{
    for (int i = 0; i < n; i++) {
        Gad *g = uibind_gad(b, ids[i]);
        if (g)
            g->pressed = (ids[i] == hit_id);
    }
    uibind_repaint(b);
}

static void sync_pressed(UIBind *b)
{
    sel_group(b, g_size_ids, 3, g_size_ids[g_current_settings.size]);
    sel_group(b, g_dp_ids,   2,
              g_current_settings.double_pixel ? ID_DP_ON : ID_DP_OFF);
    sel_group(b, g_acc_ids,  5, g_acc_ids[g_current_settings.acceleration / 25]);
}

static void apply_settings(void)
{
    Cursor_SetSize(g_current_settings.size);
    Cursor_SetColors(g_current_settings.colors.body_color,
                     g_current_settings.colors.shadow_color);
    Cursor_SetAcceleration(g_current_settings.acceleration);
    Cursor_SetDoublePixel(g_current_settings.double_pixel);
    Cursor_ApplySettings();
}

/* =========================================================================
 * Event callback
 * ========================================================================= */

static int pointer_prefs_event(UIBind *b, UINode *n, int ev, int arg)
{
    switch (ev) {
    case UIEV_CLICK:
        switch (n->id) {
        case ID_SIZE_16: case ID_SIZE_32: case ID_SIZE_48:
            g_current_settings.size = (CursorSize)(n->id - ID_SIZE_16);
            sel_group(b, g_size_ids, 3, n->id);
            break;
        case ID_DP_OFF: case ID_DP_ON:
            g_current_settings.double_pixel = (n->id == ID_DP_ON);
            sel_group(b, g_dp_ids, 2, n->id);
            break;
        case ID_ACC_0: case ID_ACC_25: case ID_ACC_50:
        case ID_ACC_75: case ID_ACC_100:
            g_current_settings.acceleration = (n->id - ID_ACC_0) * 25;
            sel_group(b, g_acc_ids, 5, n->id);
            break;
        case ID_APPLY:
            apply_settings();
            break;
        case ID_CLOSE:
            uibind_close(b);
            g_bind = NULL;
            break;
        }
        return 0;
    case UIEV_KEY:
        if (arg == 27) {              /* ESC */
            uibind_close(b);
            g_bind = NULL;
        }
        return 0;
    case UIEV_CLOSE:
        g_bind = NULL;                /* uibind frees the slot on allow */
        return 1;
    default:
        return 0;
    }
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void PointerPrefs_Show(void)
{
    if (uibind_is_open(g_bind)) {
        WM_RaiseWindow(uibind_handle(g_bind));
        return;
    }

    g_current_settings = Cursor_GetSettings();
    ui_arena_reset(&g_arena);
    if (!g_arena.base)
        ui_arena_init(&g_arena, g_arena_buf, sizeof(g_arena_buf));

    /* UAOS-127: a .gui description under ENV:GUI/ or SYS:Prefs/GUI/
     * overrides the compiled-in tree — editing the file changes this
     * window's layout with no rebuild. */
    UINode *root = uibind_load_gui(&g_arena, "pointer", NULL);
    if (!root)
        root = ui_window(&g_arena, "Pointer Prefs",
            ui_vgroup(&g_arena,
                ui_label(&g_arena, "Cursor Size:"),
                ui_hgroup(&g_arena,
                    ui_w(ui_button(&g_arena, "16x16", ID_SIZE_16), 1),
                    ui_w(ui_button(&g_arena, "32x32", ID_SIZE_32), 1),
                    ui_w(ui_button(&g_arena, "48x48", ID_SIZE_48), 1),
                    NULL),
                ui_label(&g_arena, "Double Pixel:"),
                ui_hgroup(&g_arena,
                    ui_w(ui_button(&g_arena, "Off", ID_DP_OFF), 1),
                    ui_w(ui_button(&g_arena, "On",  ID_DP_ON),  1),
                    NULL),
                ui_label(&g_arena, "Acceleration:"),
                ui_hgroup(&g_arena,
                    ui_w(ui_button(&g_arena, "0%",   ID_ACC_0),   1),
                    ui_w(ui_button(&g_arena, "25%",  ID_ACC_25),  1),
                    ui_w(ui_button(&g_arena, "50%",  ID_ACC_50),  1),
                    ui_w(ui_button(&g_arena, "75%",  ID_ACC_75),  1),
                    ui_w(ui_button(&g_arena, "100%", ID_ACC_100), 1),
                    NULL),
                ui_spacer(&g_arena),
                ui_hgroup(&g_arena,
                    ui_spacer(&g_arena),
                    ui_min(ui_button(&g_arena, "Apply", ID_APPLY), 96, 0),
                    ui_min(ui_button(&g_arena, "Close", ID_CLOSE), 96, 0),
                    ui_spacer(&g_arena),
                    NULL),
                NULL));

    g_bind = uibind_open(root, pointer_prefs_event, NULL, -1, -1, 0, 0);
    if (g_bind)
        sync_pressed(g_bind);
}

void PointerPrefs_Hide(void)
{
    if (!uibind_is_open(g_bind))
        return;
    uibind_close(g_bind);
    g_bind = NULL;
}

int PointerPrefs_IsOpen(void)
{
    return uibind_is_open(g_bind);
}
