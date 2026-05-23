/* ps2kbd.h — UAOS PS/2 Keyboard driver */

#ifndef UAOS_PS2KBD_H
#define UAOS_PS2KBD_H

#include <stdint.h>

/* Initialise keyboard: flush controller, enable scanning */
void PS2Kbd_Init(void);

/* IRQ1 handler — register with IDT vector 33 */
void PS2Kbd_IRQHandler(uint64_t vector, uint64_t error_code);

/* Read one ASCII character from the key ring buffer.
 * Returns 0 if the buffer is empty (non-blocking). */
char PS2Kbd_GetChar(void);

/* Returns 1 if there is a character waiting in the buffer */
int PS2Kbd_HasChar(void);

/* Modifier state */
typedef struct {
    int shift;
    int ctrl;
    int alt;
    int caps_lock;
} KbdMods;

extern KbdMods g_kbd_mods;

#endif
