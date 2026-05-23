/* ps2mouse.h — UAOS PS/2 Mouse driver */

#ifndef UAOS_PS2MOUSE_H
#define UAOS_PS2MOUSE_H

#include <stdint.h>

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
