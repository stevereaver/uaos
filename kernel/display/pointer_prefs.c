/* pointer_prefs.c — UAOS Pointer Preferences Tool
 *
 * AmigaOS 3.1-style pointer preferences editor.
 * Allows customization of cursor size, colors, and visibility options.
 */

#include "pointer_prefs.h"
#include "framebuffer.h"
#include "wm.h"
#include "cursor.h"
#include <stdint.h>
#include <string.h>

/* =========================================================================
 * Constants
 * ========================================================================= */

#define WIN_W  400
#define WIN_H  300
#define WIN_X  100
#define WIN_Y  100

#define BTN_W  120
#define BTN_H  25
#define BTN_Y_OFFSET 30

/* Color palette */
#define COL_BG         0xAAAAAA  /* Light grey background */
#define COL_TITLEBAR   0x000080  /* Dark blue title bar */
#define COL_BTN        0xCCCCCC  /* Button color */
#define COL_BTN_ACTIVE 0x999999  /* Active button color */
#define COL_TEXT       0x000000  /* Black text */
#define COL_BORDER     0x000000  /* Black border */

/* =========================================================================
 * UI State
 * ========================================================================= */

typedef enum {
    STATE_SIZE,
    STATE_COLORS,
    STATE_DOUBLE_PIXEL,
    STATE_ACCELERATION,
    STATE_COUNT
} UIState;

static int g_wm_handle = -1;
static UIState g_state = STATE_SIZE;
static CursorSettings g_current_settings;

/* Button definitions */
typedef struct {
    int x, y, w, h;
    const char *label;
    int active;
} Button;

static Button g_size_buttons[3];
static Button g_double_pixel_buttons[2];
static Button g_accel_buttons[5];
static Button g_apply_button;
static Button g_close_button;

/* =========================================================================
 * Helper functions
 * ========================================================================= */

static int slen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void draw_button(Button *btn, int active)
{
    uint32_t col = active ? COL_BTN_ACTIVE : COL_BTN;
    FB_FillRect(btn->x, btn->y, btn->w, btn->h, col);
    FB_DrawRect(btn->x, btn->y, btn->w, btn->h, COL_BORDER);
    
    /* Center text */
    int text_len = slen(btn->label) * 8; /* 8 pixels per char */
    int text_x = btn->x + (btn->w - text_len) / 2;
    int text_y = btn->y + (btn->h - 16) / 2;
    FB_PutStr(text_x, text_y, btn->label, COL_TEXT, col);
}

static void init_buttons(void)
{
    /* Size buttons */
    g_size_buttons[0].x = WIN_X + 20; g_size_buttons[0].y = WIN_Y + 50;
    g_size_buttons[0].w = BTN_W; g_size_buttons[0].h = BTN_H;
    g_size_buttons[0].label = "16x16";
    g_size_buttons[0].active = (g_current_settings.size == CURSOR_SIZE_16x16);
    
    g_size_buttons[1].x = WIN_X + 150; g_size_buttons[1].y = WIN_Y + 50;
    g_size_buttons[1].w = BTN_W; g_size_buttons[1].h = BTN_H;
    g_size_buttons[1].label = "32x32";
    g_size_buttons[1].active = (g_current_settings.size == CURSOR_SIZE_32x32);
    
    g_size_buttons[2].x = WIN_X + 280; g_size_buttons[2].y = WIN_Y + 50;
    g_size_buttons[2].w = BTN_W; g_size_buttons[2].h = BTN_H;
    g_size_buttons[2].label = "48x48";
    g_size_buttons[2].active = (g_current_settings.size == CURSOR_SIZE_48x48);
    
    /* Double pixel buttons */
    g_double_pixel_buttons[0].x = WIN_X + 20; g_double_pixel_buttons[0].y = WIN_Y + 150;
    g_double_pixel_buttons[0].w = BTN_W; g_double_pixel_buttons[0].h = BTN_H;
    g_double_pixel_buttons[0].label = "Off";
    g_double_pixel_buttons[0].active = !g_current_settings.double_pixel;
    
    g_double_pixel_buttons[1].x = WIN_X + 150; g_double_pixel_buttons[1].y = WIN_Y + 150;
    g_double_pixel_buttons[1].w = BTN_W; g_double_pixel_buttons[1].h = BTN_H;
    g_double_pixel_buttons[1].label = "On";
    g_double_pixel_buttons[1].active = g_current_settings.double_pixel;
    
    /* Acceleration buttons */
    for (int i = 0; i < 5; i++) {
        g_accel_buttons[i].x = WIN_X + 20 + i * 75;
        g_accel_buttons[i].y = WIN_Y + 200;
        g_accel_buttons[i].w = 70; g_accel_buttons[i].h = BTN_H;
        static const char *labels[] = {"0%", "25%", "50%", "75%", "100%"};
        g_accel_buttons[i].label = labels[i];
        g_accel_buttons[i].active = (g_current_settings.acceleration == i * 25);
    }
    
    /* Apply button */
    g_apply_button.x = WIN_X + 20; g_apply_button.y = WIN_Y + 250;
    g_apply_button.w = 100; g_apply_button.h = BTN_H;
    g_apply_button.label = "Apply";
    g_apply_button.active = 0;
    
    /* Close button */
    g_close_button.x = WIN_X + 280; g_close_button.y = WIN_Y + 250;
    g_close_button.w = 100; g_close_button.h = BTN_H;
    g_close_button.label = "Close";
    g_close_button.active = 0;
}

/* =========================================================================
 * Draw callback
 * ========================================================================= */

static void pointer_prefs_draw(int x, int y, int w, int h)
{
    (void)w; (void)h;
    
    /* Update button positions to be relative to window position */
    /* Size buttons */
    g_size_buttons[0].x = x + 20; g_size_buttons[0].y = y + 50;
    g_size_buttons[1].x = x + 150; g_size_buttons[1].y = y + 50;
    g_size_buttons[2].x = x + 280; g_size_buttons[2].y = y + 50;
    
    /* Double pixel buttons */
    g_double_pixel_buttons[0].x = x + 20; g_double_pixel_buttons[0].y = y + 150;
    g_double_pixel_buttons[1].x = x + 150; g_double_pixel_buttons[1].y = y + 150;
    
    /* Acceleration buttons */
    for (int i = 0; i < 5; i++) {
        g_accel_buttons[i].x = x + 20 + i * 75;
        g_accel_buttons[i].y = y + 200;
    }
    
    /* Apply button */
    g_apply_button.x = x + 20; g_apply_button.y = y + 250;
    
    /* Close button */
    g_close_button.x = x + 280; g_close_button.y = y + 250;
    
    /* Title bar */
    FB_FillRect(x, y, w, WM_TITLEBAR_H, COL_TITLEBAR);
    FB_PutStr(x + 10, y + 2, "Pointer Preferences", 0xFFFFFF, COL_TITLEBAR);
    
    /* Background */
    FB_FillRect(x, y + WM_TITLEBAR_H, w, h - WM_TITLEBAR_H, COL_BG);
    
    /* Labels */
    FB_PutStr(x + 20, y + 35, "Cursor Size:", COL_TEXT, COL_BG);
    FB_PutStr(x + 20, y + 135, "Double Pixel:", COL_TEXT, COL_BG);
    FB_PutStr(x + 20, y + 185, "Acceleration:", COL_TEXT, COL_BG);
    
    /* Draw buttons based on current state */
    switch (g_state) {
        case STATE_SIZE:
            for (int i = 0; i < 3; i++)
                draw_button(&g_size_buttons[i], g_size_buttons[i].active);
            break;
        case STATE_DOUBLE_PIXEL:
            for (int i = 0; i < 2; i++)
                draw_button(&g_double_pixel_buttons[i], g_double_pixel_buttons[i].active);
            break;
        case STATE_ACCELERATION:
            for (int i = 0; i < 5; i++)
                draw_button(&g_accel_buttons[i], g_accel_buttons[i].active);
            break;
        default:
            break;
    }
    
    /* Always draw Apply and Close buttons */
    draw_button(&g_apply_button, 0);
    draw_button(&g_close_button, 0);
}

/* =========================================================================
 * Key callback
 * ========================================================================= */

static void pointer_prefs_key(char c)
{
    if (c == 27) { /* ESC */
        PointerPrefs_Hide();
    }
}

/* =========================================================================
 * Click callback
 * ========================================================================= */

static int is_inside_button(int mx, int my, Button *btn)
{
    return (mx >= btn->x && mx < btn->x + btn->w &&
            my >= btn->y && my < btn->y + btn->h);
}

static void pointer_prefs_click(int handle, int mx, int my)
{
    (void)handle;
    
    /* Check Apply button */
    if (is_inside_button(mx, my, &g_apply_button)) {
        Cursor_SetSize(g_current_settings.size);
        Cursor_SetColors(g_current_settings.colors.body_color, 
                         g_current_settings.colors.shadow_color);
        Cursor_SetAcceleration(g_current_settings.acceleration);
        Cursor_SetDoublePixel(g_current_settings.double_pixel);
        Cursor_ApplySettings();
        return;
    }
    
    /* Check Close button */
    if (is_inside_button(mx, my, &g_close_button)) {
        PointerPrefs_Hide();
        return;
    }
    
    /* Check size buttons */
    for (int i = 0; i < 3; i++) {
        if (is_inside_button(mx, my, &g_size_buttons[i])) {
            g_current_settings.size = (CursorSize)i;
            for (int j = 0; j < 3; j++)
                g_size_buttons[j].active = (j == i);
            WM_Redraw();
            return;
        }
    }
    
    /* Check double pixel buttons */
    for (int i = 0; i < 2; i++) {
        if (is_inside_button(mx, my, &g_double_pixel_buttons[i])) {
            g_current_settings.double_pixel = i;
            for (int j = 0; j < 2; j++)
                g_double_pixel_buttons[j].active = (j == i);
            WM_Redraw();
            return;
        }
    }
    
    /* Check acceleration buttons */
    for (int i = 0; i < 5; i++) {
        if (is_inside_button(mx, my, &g_accel_buttons[i])) {
            g_current_settings.acceleration = i * 25;
            for (int j = 0; j < 5; j++)
                g_accel_buttons[j].active = (j == i);
            WM_Redraw();
            return;
        }
    }
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void PointerPrefs_Show(void)
{
    if (g_wm_handle >= 0 && WM_GetDrawFn(g_wm_handle) == pointer_prefs_draw) {
        WM_RaiseWindow(g_wm_handle);
        WM_Redraw();
        return;
    }
    g_wm_handle = -1;
    
    g_current_settings = Cursor_GetSettings();
    init_buttons();
    
    /* Centre on screen */
    int wx = ((int)g_fb.width - WIN_W) / 2;
    int wy = ((int)g_fb.height - WIN_H) / 2;
    if (wy < WM_TITLEBAR_H + 4) wy = WM_TITLEBAR_H + 4;
    
    g_wm_handle = WM_AddWindow(wx, wy, WIN_W, WIN_H, "Pointer Prefs",
                               pointer_prefs_draw, pointer_prefs_key);
    if (g_wm_handle >= 0) {
        WM_SetClickHandler(g_wm_handle, pointer_prefs_click);
        WM_Redraw();
    }
}

void PointerPrefs_Hide(void)
{
    if (g_wm_handle < 0) return;
    
    WM_CloseWindow(g_wm_handle);
    g_wm_handle = -1;
}

int PointerPrefs_IsOpen(void)
{
    return (g_wm_handle >= 0 && WM_GetDrawFn(g_wm_handle) == pointer_prefs_draw);
}
