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
    int super_left;   /* Left Super/Windows key → LAmiga */
    int super_right;  /* Right Super/Windows key → RAmiga */
} KbdMods;

extern KbdMods g_kbd_mods;

/* -------------------------------------------------------------------------
 * Amiga key mapping — Super/Windows key → Amiga key
 *
 * When Right Super (RAmiga) is held with a letter, the byte pushed into
 * the ring buffer is 0x80 | UPPERCASE_LETTER (e.g. RAmiga+A = 0xC1).
 *
 * When Left Super (LAmiga) is held with V/B/M/N, a single special byte
 * is pushed:
 * ------------------------------------------------------------------------- */
#define AMIGA_LV   ((char)0xF5)   /* LAmiga+V — requester Verify/OK   */
#define AMIGA_LB   ((char)0xF6)   /* LAmiga+B — requester Cancel      */
#define AMIGA_LM   ((char)0xF7)   /* LAmiga+M — next screen           */
#define AMIGA_LN   ((char)0xF8)   /* LAmiga+N — previous screen       */

/* Mask to extract the letter from a RAmiga+letter byte */
#define AMIGA_RMASK  0x80
#define AMIGA_RLETTER(c) ((char)((unsigned char)(c) & 0x7F))
#define IS_AMIGA_RKEY(c) (((unsigned char)(c) & 0x80) && (unsigned char)(c) < 0xF0)

#endif
