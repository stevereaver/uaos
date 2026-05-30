/*
 * keyboard_device.c — UAOS keyboard.device Implementation
 *
 * AmigaOS keyboard.device provides keyboard input operations.
 * This is a native implementation for UAOS using the existing
 * PS/2 keyboard driver.
 */

#include "rom_modules.h"
#include "../irq/ps2kbd.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

/* =========================================================================
 * AmigaOS Input Event Structure
 * ========================================================================= */

typedef struct {
    uint16_t ie_Class;      /* Event class */
    uint16_t ie_Code;       /* Event code */
    uint16_t ie_Qualifier;  /* Qualifiers (shift, ctrl, alt, etc) */
    uint16_t ie_Position;   /* Mouse position (for keyboard events) */
} InputEvent;

/* Event classes */
#define IECLASS_RAWKEY     0x11
#define IECLASS_RAWMOUSE   0x12
#define IECLASS_TIMER      0x1F

/* =========================================================================
 * keyboard.device function indices (must match AmigaOS LVO offsets)
 * ========================================================================= */

#define KEYBOARD_OPEN_DEVICE   1
#define KEYBOARD_CLOSE_DEVICE  2
#define KEYBOARD_BEGIN_IO      3
#define KEYBOARD_ABORT_IO      4
#define KEYBOARD_READ          5
#define KEYBOARD_WRITE         6
#define KEYBOARD_RAW_KEY       7
#define KEYBOARD_KBD_READ      8
#define KEYBOARD_KBD_WRITE     9
#define KEYBOARD_KBD_REMAP     10

/* =========================================================================
 * Stub implementations
 * ========================================================================= */

static void keyboard_OpenDevice(void)
{
    /* OpenDevice - open keyboard device */
    fprintf(stderr, "[KEYBOARD] OpenDevice called\n");
}

static void keyboard_CloseDevice(void)
{
    /* CloseDevice - close keyboard device */
    fprintf(stderr, "[KEYBOARD] CloseDevice called\n");
}

static void keyboard_BeginIO(void)
{
    /* BeginIO - start I/O operation */
    fprintf(stderr, "[KEYBOARD] BeginIO called\n");
}

static void keyboard_AbortIO(void)
{
    /* AbortIO - abort I/O operation */
    fprintf(stderr, "[KEYBOARD] AbortIO called\n");
}

static void keyboard_Read(void)
{
    /* Read - read character from keyboard
     * D1 = pointer to buffer to fill
     * Returns character read or -1 if no data */
    if (PS2Kbd_HasChar()) {
        char c = PS2Kbd_GetChar();
        fprintf(stderr, "[KEYBOARD] Read: '%c' (0x%02X)\n", c, (unsigned char)c);
        /* TODO: Write to guest memory via M68k glue */
    } else {
        fprintf(stderr, "[KEYBOARD] Read: no data\n");
    }
}

static void keyboard_Write(void)
{
    /* Write - write to keyboard (LED control, etc)
     * D1 = command byte */
    fprintf(stderr, "[KEYBOARD] Write called (LED control)\n");
    /* TODO: Implement LED control via PS/2 controller */
}

static void keyboard_RawKey(void)
{
    /* RawKey - read raw key codes
     * D1 = pointer to InputEvent structure to fill */
    if (PS2Kbd_HasChar()) {
        char c = PS2Kbd_GetChar();
        fprintf(stderr, "[KEYBOARD] RawKey: 0x%02X\n", (unsigned char)c);
        /* TODO: Fill InputEvent structure with raw key code */
    } else {
        fprintf(stderr, "[KEYBOARD] RawKey: no data\n");
    }
}

static void keyboard_KbdRead(void)
{
    /* KbdRead - read keyboard events
     * D1 = pointer to InputEvent structure to fill */
    if (PS2Kbd_HasChar()) {
        char c = PS2Kbd_GetChar();
        fprintf(stderr, "[KEYBOARD] KbdRead: 0x%02X\n", (unsigned char)c);
        /* TODO: Fill InputEvent structure with class, code, qualifiers */
    } else {
        fprintf(stderr, "[KEYBOARD] KbdRead: no data\n");
    }
}

static void keyboard_KbdWrite(void)
{
    /* KbdWrite - write keyboard events (inject keypresses)
     * D1 = pointer to InputEvent structure */
    fprintf(stderr, "[KEYBOARD] KbdWrite called\n");
    /* TODO: Implement key injection */
}

static void keyboard_KbdRemap(void)
{
    /* KbdRemap - remap keyboard codes
     * D1 = mapping table */
    fprintf(stderr, "[KEYBOARD] KbdRemap called\n");
    /* TODO: Implement key remapping */
}

/* =========================================================================
 * Function table
 * ========================================================================= */

static void *keyboard_funcs[] = {
    keyboard_OpenDevice,   /* index 1  */
    keyboard_CloseDevice,  /* index 2  */
    keyboard_BeginIO,      /* index 3  */
    keyboard_AbortIO,      /* index 4  */
    keyboard_Read,         /* index 5  */
    keyboard_Write,        /* index 6  */
    keyboard_RawKey,       /* index 7  */
    keyboard_KbdRead,      /* index 8  */
    keyboard_KbdWrite,     /* index 9  */
    keyboard_KbdRemap,     /* index 10 */
};

/* =========================================================================
 * Registration function
 * ========================================================================= */

void UAOS_KEYBOARD_Register(void)
{
    UAOS_ROM_Register("keyboard.device", 40, 0x000000B0,
                      (uint16_t)(sizeof(keyboard_funcs) / sizeof(keyboard_funcs[0])),
                      keyboard_funcs);
}
