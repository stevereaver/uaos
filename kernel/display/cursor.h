/* cursor.h — UAOS Framebuffer Hardware Cursor */

#ifndef UAOS_CURSOR_H
#define UAOS_CURSOR_H

#include <stdint.h>

/* Cursor size options */
typedef enum {
    CURSOR_SIZE_16x16 = 0,
    CURSOR_SIZE_32x32 = 1,
    CURSOR_SIZE_48x48 = 2,
    CURSOR_SIZE_COUNT
} CursorSize;

/* Cursor color scheme */
typedef struct {
    uint32_t body_color;      /* Main cursor body color */
    uint32_t shadow_color;    /* Shadow/outline color */
    uint32_t bg_color;        /* Background color (for inverted pixels) */
} CursorColors;

/* Cursor settings */
typedef struct {
    CursorSize size;
    CursorColors colors;
    int acceleration;         /* Mouse acceleration (0-100) */
    int double_pixel;         /* Double pixels for visibility (0/1) */
} CursorSettings;

/* Default cursor colors */
#define CURSOR_DEFAULT_BODY      0xFFFFFF    /* White */
#define CURSOR_DEFAULT_SHADOW    0x000000    /* Black */
#define CURSOR_DEFAULT_BG        0x000000    /* Black */

/* Initialise cursor state and draw at starting position */
void Cursor_Init(int x, int y);

/* Move cursor to (x, y): restore background, draw at new position */
void Cursor_Move(int x, int y);

/* Draw cursor at current position (call after desktop redraw) */
void Cursor_Redraw(void);

/* Remove cursor from screen (restore background) — call before any repaint */
void Cursor_Hide(void);

/* Cursor settings management */
void Cursor_SetSize(CursorSize size);
void Cursor_SetColors(uint32_t body, uint32_t shadow);
void Cursor_SetAcceleration(int accel);
void Cursor_SetDoublePixel(int enable);
CursorSettings Cursor_GetSettings(void);

/* Apply cursor settings (redraws cursor) */
void Cursor_ApplySettings(void);

/* Custom sprite / busy cursor support */
void Cursor_SetCustomSprite(const uint8_t *data, int w, int h);
void Cursor_ClearCustomSprite(void);
void Cursor_SetBusy(int busy);

#endif
