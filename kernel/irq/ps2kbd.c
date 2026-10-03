/* ps2kbd.c — UAOS PS/2 Keyboard Driver
 *
 * Receives PS/2 scan code set 1 bytes on IRQ1 (IDT vector 33).
 * Translates make-codes to ASCII, handles shift/ctrl/caps-lock,
 * stores results in a 256-byte ring buffer for polling by the shell.
 */

#include "ps2kbd.h"
#include "idt.h"
#include "irq.h"
#include "../exec/task.h"
#include "../chipset/chip_emu.h"
#include <stdint.h>

/* =========================================================================
 * I/O
 * ========================================================================= */

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_STAT_OBF 0x01

/* =========================================================================
 * Scan code set 1 → ASCII translation tables
 * Index = scan code (0x00–0x58).
 * 0x00 means "no printable character" (handled specially below).
 * ========================================================================= */

static const char sc_normal[89] = {
/*00*/  0,
/*01*/  27,     /* Esc          */
/*02*/  '1','2','3','4','5','6','7','8','9','0','-','=',
/*0E*/  '\b',
/*0F*/  '\t',
/*10*/  'q','w','e','r','t','y','u','i','o','p','[',']',
/*1C*/  '\n',
/*1D*/  0,      /* L-Ctrl       */
/*1E*/  'a','s','d','f','g','h','j','k','l',';','\'','`',
/*2A*/  0,      /* L-Shift      */
/*2B*/  '\\',
/*2C*/  'z','x','c','v','b','n','m',',','.','/',
/*36*/  0,      /* R-Shift      */
/*37*/  '*',
/*38*/  0,      /* L-Alt        */
/*39*/  ' ',
/*3A*/  0,      /* Caps Lock    */
/*3B*/  0x1C,0,0,0,0,0,0,0,0,0, /* F1-F10 (F1 mapped to 0x1C for help) */
/*45*/  0,      /* Num Lock     */
/*46*/  0,      /* Scroll Lock  */
/*47*/  '7','8','9','-','4','5','6','+','1','2','3','0','.',
/*54*/  0,0,
/*56*/  0,      /* non-US \|    */
/*57*/  0,      /* F11          */
/*58*/  0,      /* F12          */
};

static const char sc_shifted[89] = {
/*00*/  0,
/*01*/  0,
/*02*/  '!','@','#','$','%','^','&','*','(',')','_','+',
/*0E*/  '\b',
/*0F*/  '\t',
/*10*/  'Q','W','E','R','T','Y','U','I','O','P','{','}',
/*1C*/  '\n',
/*1D*/  0,
/*1E*/  'A','S','D','F','G','H','J','K','L',':','"','~',
/*2A*/  0,
/*2B*/  '|',
/*2C*/  'Z','X','C','V','B','N','M','<','>','?',
/*36*/  0,
/*37*/  '*',
/*38*/  0,
/*39*/  ' ',
/*3A*/  0,
/*3B*/  0,0,0,0,0,0,0,0,0,0,
/*45*/  0,
/*46*/  0,
/*47*/  '7','8','9','-','4','5','6','+','1','2','3','0','.',
/*54*/  0,0,0,0,0,
};

/* =========================================================================
 * Ring buffer — 256 bytes, power-of-2 so masking works
 * ========================================================================= */

#define KBUF_SIZE 256
#define KBUF_MASK (KBUF_SIZE - 1)

static volatile char   kbuf[KBUF_SIZE];
static volatile uint8_t kbuf_head = 0;
static volatile uint8_t kbuf_tail = 0;

static void kbuf_push(char c)
{
    uint8_t next = (kbuf_tail + 1) & KBUF_MASK;
    if (next != kbuf_head) {   /* drop if full */
        kbuf[kbuf_tail] = c;
        kbuf_tail = next;
        /* Wake the event pump (IRQ-safe Signal + IRQ-exit reschedule) so
         * keys are dispatched to the WM without waiting for a tick. */
        EventPump_Wake();
    }
}

/* Public hook so the USB HID driver can feed the same ring buffer. */
void PS2Kbd_PushChar(char c)
{
    kbuf_push(c);
}

/* =========================================================================
 * Rawkey event ring — Amiga rawkey transitions (code | up<<7) for every
 * physical key event, including modifiers and key releases which produce
 * no cooked character.  Drained by the event pump and posted to the
 * focused window as IDCMP_RAWKEY so m68k guests can track qualifier
 * state themselves the way real Amiga programs do.
 * ========================================================================= */

#define KRAW_SIZE 64
static volatile uint16_t kraw[KRAW_SIZE];
static volatile uint8_t  kraw_head = 0;
static volatile uint8_t  kraw_tail = 0;

/* IEQUALIFIER_* snapshot taken at event time — the queue may sit for a
 * pump cycle during which later transitions would corrupt a live read. */
static uint16_t kraw_qual(void)
{
    uint16_t q = 0;
    if (g_kbd_mods.shift)       q |= 0x0001;
    if (g_kbd_mods.caps_lock)   q |= 0x0004;
    if (g_kbd_mods.ctrl)        q |= 0x0008;
    if (g_kbd_mods.alt)         q |= 0x0010;
    if (g_kbd_mods.super_left)  q |= 0x0040;
    if (g_kbd_mods.super_right) q |= 0x0080;
    return q;
}

static void kraw_push(int amiga_code, int up)
{
    if (amiga_code < 0 || amiga_code > 0x7F) return;
    uint8_t next = (kraw_tail + 1) & (KRAW_SIZE - 1);
    if (next == kraw_head) return;
    kraw[kraw_tail] = (uint16_t)((amiga_code | (up ? 0x80 : 0)) |
                                 ((int)kraw_qual() << 8));
    kraw_tail = next;
}

int PS2Kbd_HasRawKey(void)
{
    return kraw_head != kraw_tail;
}

int PS2Kbd_GetRawKey(void)
{
    if (kraw_head == kraw_tail) return -1;
    int v = kraw[kraw_head];
    kraw_head = (kraw_head + 1) & (KRAW_SIZE - 1);
    return v;   /* low byte: code|0x80*up, high byte: qualifier snapshot */
}

/* PS/2 set-1 scancode -> Amiga rawkey matrix code (-1 = unmapped). */
static const int8_t ps2_to_amiga[89] = {
/*00*/  -1,
/*01*/  0x45,                       /* Esc */
/*02*/  0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A, /* 1-0 */
/*0C*/  0x0B,0x0C,                /* - = */
/*0E*/  0x41,                     /* Backspace */
/*0F*/  0x42,                     /* Tab */
/*10*/  0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19, /* Q-P */
/*1A*/  0x1A,0x1B,                /* [ ] */
/*1C*/  0x44,                     /* Enter */
/*1D*/  0x63,                     /* L-Ctrl */
/*1E*/  0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x28,      /* A-L */
/*27*/  0x29,0x2A,                /* ; ' */
/*29*/  0x00,                     /* ` */
/*2A*/  0x60,                     /* L-Shift */
/*2B*/  0x0D,                     /* \ */
/*2C*/  0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38,0x39,0x3A, /* Z-M , . / */
/*36*/  0x61,                     /* R-Shift */
/*37*/  0x5D,                     /* KP * */
/*38*/  0x64,                     /* L-Alt */
/*39*/  0x40,                     /* Space */
/*3A*/  0x62,                     /* Caps Lock */
/*3B*/  0x50,0x51,0x52,0x53,0x54,0x55,0x56,0x57,0x58,0x59, /* F1-F10 */
/*45*/  -1,-1,                    /* NumLock, ScrollLock */
/*47*/  0x3D,0x3E,0x3F,0x4A,      /* KP 7 8 9 - */
/*4B*/  0x2D,0x2E,0x2F,0x5E,      /* KP 4 5 6 + */
/*4F*/  0x1D,0x1E,0x1F,0x0F,0x3C, /* KP 1 2 3 0 . */
/*54*/  -1,-1,
/*56*/  0x30,                     /* intl <> key */
/*57*/  -1,-1,                    /* F11, F12 */
};

/* =========================================================================
 * Modifier state
 * ========================================================================= */

KbdMods g_kbd_mods = { 0, 0, 0, 0, 0, 0 };

/* =========================================================================
 * PS2Kbd_Init
 * ========================================================================= */

void PS2Kbd_Init(void)
{
    /* Flush output buffer — bounded: machines without an i8042 (e.g.
     * MacBookPro4,1, USB-only input) leave port 0x64 reading 0xFF, which
     * would loop forever on OBF. */
    int n = 128;
    while ((inb(PS2_STATUS) & PS2_STAT_OBF) && --n)
        inb(PS2_DATA);
    /* Keyboard is already active after BIOS; just clear any pending data */
    kbuf_head = 0;
    kbuf_tail = 0;
}

/* =========================================================================
 * PS2Kbd_IRQHandler — IDT vector 33 (IRQ1)
 * ========================================================================= */

void PS2Kbd_IRQHandler(uint64_t vector, uint64_t error_code)
{
    (void)vector; (void)error_code;

    if (!(inb(PS2_STATUS) & PS2_STAT_OBF)) {
        IRQ_EOI((int)vector);
        return;
    }

    uint8_t sc = inb(PS2_DATA);

    /* 0xE0 extended prefix — handle Page Up/Down for scrollback */
    static int extended = 0;
    if (sc == 0xE0) { extended = 1; IRQ_EOI((int)vector); return; }

    int is_break = (sc & 0x80) != 0;
    uint8_t key  = sc & 0x7F;

    if (extended) {
        extended = 0;
        /* Left Super/Windows key → LAmiga (E0 5B make / E0 DB break) */
        if (key == 0x5B) {
            g_kbd_mods.super_left = !is_break;
            chip_emu_push_keycode(0x66, is_break);
            kraw_push(0x66, is_break);
            IRQ_EOI((int)vector); return;
        }
        /* Right Super/Windows key → RAmiga (E0 5C make / E0 DC break) */
        if (key == 0x5C) {
            g_kbd_mods.super_right = !is_break;
            chip_emu_push_keycode(0x67, is_break);
            kraw_push(0x67, is_break);
            IRQ_EOI((int)vector); return;
        }
        /* Right Alt → Amiga RALT (E0 38) */
        if (key == 0x38) {
            g_kbd_mods.alt = !is_break;
            chip_emu_push_keycode(0x65, is_break);
            kraw_push(0x65, is_break);
            IRQ_EOI((int)vector); return;
        }
        /* Right Ctrl → Amiga CTRL (E0 1D) */
        if (key == 0x1D) {
            g_kbd_mods.ctrl = !is_break;
            chip_emu_push_keycode(0x63, is_break);
            kraw_push(0x63, is_break);
            IRQ_EOI((int)vector); return;
        }
        /* Extended keys with Amiga rawkey equivalents (cursor keys,
         * keypad enter/divide, delete). */
        {
            int ac = -1;
            switch (key) {
                case 0x48: ac = 0x4C; break; /* Up    */
                case 0x4B: ac = 0x4F; break; /* Left  */
                case 0x50: ac = 0x4E; break; /* Down  */
                case 0x4D: ac = 0x4D; break; /* Right */
                case 0x1C: ac = 0x43; break; /* KP Enter */
                case 0x35: ac = 0x5C; break; /* KP / */
                case 0x53: ac = 0x46; break; /* Delete */
                case 0x47: ac = 0x5F; break; /* Home -> Help */
            }
            if (ac >= 0) {
                kraw_push(ac, is_break);
                chip_emu_push_keycode(ac, is_break);
            }
        }
        if (!is_break) {
            if (key == 0x49) { kbuf_push(KBD_VKEY_PGUP);  } /* Page Up   */
            if (key == 0x51) { kbuf_push(KBD_VKEY_PGDN);  } /* Page Down */
            if (key == 0x48) { kbuf_push(KBD_VKEY_UP);    } /* Up arrow  */
            if (key == 0x50) { kbuf_push(KBD_VKEY_DOWN);  } /* Down arrow*/
            if (key == 0x4B) { kbuf_push(KBD_VKEY_LEFT);  } /* Left arrow*/
            if (key == 0x4D) { kbuf_push(KBD_VKEY_RIGHT); } /* Right arrow*/
        }
        IRQ_EOI((int)vector); return;
    }

    /* Update modifiers on both make and break; mirror each transition into
     * the emulated CIA-A keyboard stream so low-level guests see them. */
    if (key == 0x2A || key == 0x36) { /* L/R Shift */
        g_kbd_mods.shift = !is_break;
        chip_emu_push_keycode(key == 0x2A ? 0x60 : 0x61, is_break);
        kraw_push(key == 0x2A ? 0x60 : 0x61, is_break);
        IRQ_EOI((int)vector); return;
    }
    if (key == 0x1D) { /* Ctrl */
        g_kbd_mods.ctrl = !is_break;
        chip_emu_push_keycode(0x63, is_break);
        kraw_push(0x63, is_break);
        IRQ_EOI((int)vector); return;
    }
    if (key == 0x38) { /* Alt */
        g_kbd_mods.alt = !is_break;
        chip_emu_push_keycode(0x64, is_break);
        kraw_push(0x64, is_break);
        IRQ_EOI((int)vector); return;
    }
    if (key == 0x3A) { /* Caps Lock — down event only changes state */
        if (!is_break) g_kbd_mods.caps_lock ^= 1;
        chip_emu_push_keycode(0x62, is_break);
        kraw_push(0x62, is_break);
        IRQ_EOI((int)vector); return;
    }

    /* Rawkey transition for every regular key, on both make and break —
     * guests (OctaMED) need releases and self-tracked qualifiers.  Also
     * mirror the raw make/break into CIA-A SDR so low-level guests see
     * real transitions; the cooked-char feed (chip_emu_push_key) was a
     * racy second consumer of the same kbuf the event pump drains. */
    if (key < 89 && ps2_to_amiga[key] >= 0) {
        kraw_push(ps2_to_amiga[key], is_break);
        chip_emu_push_keycode(ps2_to_amiga[key], is_break);
    }

    /* Only process make codes past this point */
    if (is_break) { IRQ_EOI((int)vector); return; }

    if (key >= 89) { IRQ_EOI((int)vector); return; }

    int use_shift = g_kbd_mods.shift;
    /* Caps lock inverts shift for alpha keys only */
    if (g_kbd_mods.caps_lock) {
        char base = sc_normal[key];
        if (base >= 'a' && base <= 'z') use_shift ^= 1;
    }

    char ascii = use_shift ? sc_shifted[key] : sc_normal[key];

    /* Ctrl+key → control code */
    if (g_kbd_mods.ctrl && ascii >= 'a' && ascii <= 'z')
        ascii = (char)(ascii - 'a' + 1);
    else if (g_kbd_mods.ctrl && ascii >= 'A' && ascii <= 'Z')
        ascii = (char)(ascii - 'A' + 1);

    if (ascii) {
        /* Amiga key mapping: Super/Windows → Amiga */
        if (g_kbd_mods.super_right) {
            /* RAmiga + letter → menu shortcut (0x80 | uppercase) */
            char upper = ascii;
            if (ascii >= 'a' && ascii <= 'z') upper = (char)(ascii - 'a' + 'A');
            if (upper >= 'A' && upper <= 'Z') {
                kbuf_push((char)(0x80 | (unsigned char)upper));
                IRQ_EOI((int)vector); return;
            }
        }
        if (g_kbd_mods.super_left) {
            /* LAmiga + V/B/M/N → special requester/screen codes */
            char upper = ascii;
            if (ascii >= 'a' && ascii <= 'z') upper = (char)(ascii - 'a' + 'A');
            switch (upper) {
                case 'V': kbuf_push(AMIGA_LV); IRQ_EOI((int)vector); return;
                case 'B': kbuf_push(AMIGA_LB); IRQ_EOI((int)vector); return;
                case 'M': kbuf_push(AMIGA_LM); IRQ_EOI((int)vector); return;
                case 'N': kbuf_push(AMIGA_LN); IRQ_EOI((int)vector); return;
            }
            /* Other LAmiga+letters are command-key (menu shortcut)
             * candidates, same as RAmiga+letter — encode 0x80|upper so the
             * pump routes them to Intuition menu matching instead of
             * leaking a plain VANILLAKEY the real keymap would suppress. */
            if (upper >= 'A' && upper <= 'Z') {
                kbuf_push((char)(0x80 | (unsigned char)upper));
                IRQ_EOI((int)vector); return;
            }
        }
        kbuf_push(ascii);
    }

    IRQ_EOI((int)vector);
}

/* =========================================================================
 * Public polling API
 * ========================================================================= */

int PS2Kbd_HasChar(void)
{
    return kbuf_head != kbuf_tail;
}

char PS2Kbd_GetChar(void)
{
    if (kbuf_head == kbuf_tail) return 0;
    char c = kbuf[kbuf_head];
    kbuf_head = (kbuf_head + 1) & KBUF_MASK;
    return c;
}
