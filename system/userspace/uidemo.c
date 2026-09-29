/* uidemo.c — declarative-UI demo for the userspace backend (UAOS-126)
 *
 * The entire window is a UINode tree driven by uaos_ui.h over the
 * uaos_gui_* syscalls — no absolute coordinates.  Demonstrates label,
 * cycle, checkbox, slider, string, integer, and button gadgets plus the
 * standard prefs row, with live resize reflow.
 */

#include "uaos_syscall.h"
#include "uaos_libc.h"
#include "uaos_ui.h"

enum {
    ID_MODE = 1,
    ID_SHADOW, ID_FX,
    ID_SPEED,
    ID_NAME, ID_LEVEL,
    ID_SAVE, ID_USE, ID_CANCEL,
    ID_STATUS
};

static const char * const k_modes[] = {
    "Workbench", "Screen", "Window"
};

static uaos_ui_t     g_ui;
static UIArena       g_arena;
static unsigned char g_arena_buf[8192];
static char          g_status[64];

static void set_status(const char *s)
{
    uaos_strncpy(g_status, s, sizeof(g_status) - 1);
    g_status[sizeof(g_status) - 1] = '\0';
    uaos_gui_set_str(&g_ui.gui, ID_STATUS, g_status);
    uaos_ui_redraw(&g_ui);
}

static int on_event(uaos_ui_t *ui, UINode *n, int ev, int arg)
{
    switch (ev) {
    case UAOS_UIEV_CLICK:
        if (n->id == ID_CANCEL) { uaos_ui_close(ui); return 0; }
        if (n->id == ID_SAVE)   set_status("Saved to ENVARC: (demo)");
        if (n->id == ID_USE)    set_status("Applied for session (demo)");
        break;
    case UAOS_UIEV_CHANGE:
        if (n->id == ID_MODE)   set_status("Mode changed");
        else if (n->id == ID_SHADOW)
            set_status(uaos_ui_val(ui, ID_SHADOW) ? "Shadow on" : "Shadow off");
        else if (n->id == ID_FX)
            set_status(uaos_ui_val(ui, ID_FX) ? "FX on" : "FX off");
        else if (n->id == ID_SPEED) set_status("Speed moved");
        else if (n->id == ID_NAME)  set_status("Name edited");
        else if (n->id == ID_LEVEL) set_status("Level edited");
        break;
    case UAOS_UIEV_KEY:
        if (arg == 27) {              /* ESC */
            uaos_ui_close(ui);
            return 0;
        }
        break;
    default:
        break;
    }
    return 0;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    ui_arena_init(&g_arena, g_arena_buf, sizeof(g_arena_buf));
    uaos_strcpy(g_status, "Ready.");

    /* UAOS-127: ENV:GUI/uidemo.gui or SYS:Prefs/GUI/uidemo.gui may
     * override this compiled-in tree at runtime. */
    UINode *root = uaos_ui_load(&g_arena, "uidemo", NULL);
    if (!root)
        root = ui_window(&g_arena, "UI Demo",
        ui_vgroup(&g_arena,
            ui_hgroup(&g_arena,
                ui_label(&g_arena, "Mode:"),
                ui_w(ui_cycle(&g_arena, k_modes, 3, ID_MODE), 1),
                NULL),
            ui_hgroup(&g_arena,
                ui_w(ui_checkbox(&g_arena, "Pointer shadow", ID_SHADOW), 1),
                ui_w(ui_checkbox(&g_arena, "Screen FX",      ID_FX),     1),
                NULL),
            ui_hgroup(&g_arena,
                ui_label(&g_arena, "Speed:"),
                ui_w(ui_slider(&g_arena, 0, 100, 50, ID_SPEED), 1),
                NULL),
            ui_hgroup(&g_arena,
                ui_label(&g_arena, "Name:"),
                ui_w(ui_string(&g_arena, "demo", 24, ID_NAME), 1),
                NULL),
            ui_hgroup(&g_arena,
                ui_label(&g_arena, "Level:"),
                ui_w(ui_integer(&g_arena, 42, 0, 99, ID_LEVEL), 0),
                ui_spacer(&g_arena),
                NULL),
            ui_id(ui_label(&g_arena, "Status: idle"), ID_STATUS),
            ui_spacer(&g_arena),
            uaos_ui_prefs_row(&g_arena, ID_SAVE, ID_USE, ID_CANCEL),
            NULL));

    if (uaos_ui_open(&g_ui, root, on_event, NULL, -1, -1, 420, 0) < 0) {
        uaos_stdout_write("uidemo: could not open window\n", 30);
        return 1;
    }

    while (uaos_ui_is_open(&g_ui))
        uaos_ui_poll(&g_ui);
    return 0;
}
