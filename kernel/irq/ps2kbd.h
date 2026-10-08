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

/* Push a translated ASCII/virtual-key byte into the ring buffer.
 * Used by the USB HID keyboard driver so both paths share the queue. */
void PS2Kbd_PushChar(char c);

/* Rawkey event ring — Amiga rawkey transitions for every physical key
 * event (code in low 7 bits, bit7 set on release), including modifiers
 * and releases that produce no cooked character.  The event pump drains
 * this and posts IDCMP_RAWKEY to the focused window. */
int  PS2Kbd_HasRawKey(void);
int  PS2Kbd_GetRawKey(void);   /* returns code|0x80*up, or -1 if empty */

/* Push an Amiga rawkey transition (code 0x00-0x7F, up=1 on release) into
 * the rawkey ring — used by the USB HID driver so both paths share the
 * queue. */
void PS2Kbd_PushRawKey(int amiga_code, int up);

/* ie_Qualifier bits (devices/inputevent.h) — carried in IntuiMessage
 * .Qualifier for RAWKEY/VANILLAKEY/MOUSEBUTTONS/MENUPICK messages. */
#define IEQUALIFIER_LSHIFT     0x0001
#define IEQUALIFIER_RSHIFT     0x0002
#define IEQUALIFIER_CAPSLOCK   0x0004
#define IEQUALIFIER_CONTROL    0x0008
#define IEQUALIFIER_LALT       0x0010
#define IEQUALIFIER_RALT       0x0020
#define IEQUALIFIER_LCOMMAND   0x0040   /* Left Amiga  */
#define IEQUALIFIER_RCOMMAND   0x0080   /* Right Amiga */
#define IEQUALIFIER_NUMERICPAD 0x0100
#define IEQUALIFIER_REPEAT     0x0200
#define IEQUALIFIER_INTERRUPT  0x0400
#define IEQUALIFIER_MULTIBCAST 0x0800
#define IEQUALIFIER_MIDBUTTON  0x1000
#define IEQUALIFIER_RBUTTON    0x2000
#define IEQUALIFIER_LBUTTON    0x4000

/* Live ie_Qualifier snapshot: current modifier + mouse-button state.
 * NUMERICPAD/REPEAT are per-key attributes and only ever set on the
 * rawkey ring. */
uint16_t PS2Kbd_IEQualifier(void);

/* Modifier state */
typedef struct {
    int shift;        /* any Shift held (L or R) */
    int ctrl;         /* any Ctrl held */
    int alt;          /* any Alt held (L or R) */
    int caps_lock;
    int super_left;   /* Left Super/Windows key → LAmiga */
    int super_right;  /* Right Super/Windows key → RAmiga */
    int lshift;       /* per-side tracking for ie_Qualifier */
    int rshift;
    int lalt;
    int ralt;
} KbdMods;

extern KbdMods g_kbd_mods;

/* Amiga three-finger reset: reboot when Ctrl + LAmiga + RAmiga are all
 * held.  Call after every g_kbd_mods update from any input source
 * (PS/2 IRQ handler, USB HID reports) so the last key of the chord
 * triggers it. */
void PS2Kbd_CheckResetChord(void);

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

/* Virtual cursor/function keys pushed for E0-prefixed scan codes
 * (arrows, PageUp/PageDown).  They live above the ASCII/control range
 * and outside the Amiga-key byte space so real control bytes — above
 * all Ctrl-C (0x03) — always reach consumers as data, never as cursor
 * keys.  Same codes a telnet session feeds via SHELL_VKEY_*. */
#define KBD_VKEY_PGUP   ((char)0xF9)
#define KBD_VKEY_PGDN   ((char)0xFA)
#define KBD_VKEY_UP     ((char)0xFB)
#define KBD_VKEY_DOWN   ((char)0xFC)
#define KBD_VKEY_LEFT   ((char)0xFD)
#define KBD_VKEY_RIGHT  ((char)0xFE)

/* Editing keys the PS/2 driver does not currently emit but remote
 * (telnet) sessions can feed via SHELL_VKEY_* — decoded from ANSI CSI
 * sequences (ESC [ H/F/1~/4~/3~).  Kept in the same non-ASCII space,
 * above the RAmiga-letter range (0x80..0xEF). */
#define KBD_VKEY_HOME   ((char)0xF2)
#define KBD_VKEY_END    ((char)0xF3)
#define KBD_VKEY_DEL    ((char)0xF4)

/* Mask to extract the letter from a RAmiga+letter byte */
#define AMIGA_RMASK  0x80
#define AMIGA_RLETTER(c) ((char)((unsigned char)(c) & 0x7F))
#define IS_AMIGA_RKEY(c) (((unsigned char)(c) & 0x80) && (unsigned char)(c) < 0xF0)

#endif
