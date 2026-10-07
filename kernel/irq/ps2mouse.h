/* ps2mouse.h — UAOS PS/2 Mouse driver */

#ifndef UAOS_PS2MOUSE_H
#define UAOS_PS2MOUSE_H

#include <stdint.h>

/* i8042 presence probe — runs the controller self-test (0xAA -> 0x55)
 * with bounded waits and caches the result.  On machines with no i8042
 * (USB-only boards like the MacBookPro4,1) the status port floats at
 * 0xFF and the self-test never answers.  Call once at boot before the
 * port drivers are initialised. */
int  PS2Ctl_Detect(void);

/* Cached result of PS2Ctl_Detect() — 0 when no i8042 was found. */
int  PS2_Present(void);

/* Initialise PS/2 controller, enable aux port, set stream mode */
void PS2Mouse_Init(void);

/* IRQ12 handler — call from IDT vector 44 (IRQ12 = vector 32+12 = 44) */
void PS2Mouse_IRQHandler(uint64_t vector, uint64_t error_code);

/* Current mouse state (updated by IRQ handler) */
typedef struct {
    int x, y;          /* current position (clamped to screen)  */
    int btn_left;       /* 1 if held                             */
    int btn_right;
    int btn_middle;
} MouseState;

extern MouseState g_mouse;

#endif
