/* ps2kbd.c — UAOS PS/2 Keyboard Driver
 *
 * Receives PS/2 scan code set 1 bytes on IRQ1 (IDT vector 33).
 * Translates make-codes to ASCII, handles shift/ctrl/caps-lock,
 * stores results in a 256-byte ring buffer for polling by the shell.
 */

#include "ps2kbd.h"
#include "idt.h"
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
    }
}

/* =========================================================================
 * Modifier state
 * ========================================================================= */

KbdMods g_kbd_mods = { 0, 0, 0, 0 };

/* =========================================================================
 * PS2Kbd_Init
 * ========================================================================= */

void PS2Kbd_Init(void)
{
    /* Flush output buffer */
    while (inb(PS2_STATUS) & PS2_STAT_OBF)
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
        PIC_SendEOI(1);
        return;
    }

    uint8_t sc = inb(PS2_DATA);

    /* 0xE0 extended prefix — handle Page Up/Down for scrollback */
    static int extended = 0;
    if (sc == 0xE0) { extended = 1; PIC_SendEOI(1); return; }

    int is_break = (sc & 0x80) != 0;
    uint8_t key  = sc & 0x7F;

    if (extended) {
        extended = 0;
        /* Left Super/Windows key → LAmiga (E0 5B make / E0 DB break) */
        if (key == 0x5B) {
            g_kbd_mods.super_left = !is_break;
            PIC_SendEOI(1); return;
        }
        /* Right Super/Windows key → RAmiga (E0 5C make / E0 DC break) */
        if (key == 0x5C) {
            g_kbd_mods.super_right = !is_break;
            PIC_SendEOI(1); return;
        }
        if (!is_break) {
            if (key == 0x49) { kbuf_push(0x01); } /* Page Up   → VKEY_PGUP */
            if (key == 0x51) { kbuf_push(0x02); } /* Page Down → VKEY_PGDN */
            if (key == 0x48) { kbuf_push(0x03); } /* Up arrow  → VKEY_UP    */
            if (key == 0x50) { kbuf_push(0x04); } /* Down arrow→ VKEY_DOWN  */
            if (key == 0x4B) { kbuf_push(0x05); } /* Left arrow→ VKEY_LEFT  */
            if (key == 0x4D) { kbuf_push(0x06); } /* Right arrow→VKEY_RIGHT */
        }
        PIC_SendEOI(1); return;
    }

    /* Update modifiers on both make and break */
    if (key == 0x2A || key == 0x36) { /* L/R Shift */
        g_kbd_mods.shift = !is_break;
        PIC_SendEOI(1); return;
    }
    if (key == 0x1D) { /* Ctrl */
        g_kbd_mods.ctrl = !is_break;
        PIC_SendEOI(1); return;
    }
    if (key == 0x38) { /* Alt */
        g_kbd_mods.alt = !is_break;
        PIC_SendEOI(1); return;
    }
    if (key == 0x3A && !is_break) { /* Caps Lock toggle */
        g_kbd_mods.caps_lock ^= 1;
        PIC_SendEOI(1); return;
    }

    /* Only process make codes past this point */
    if (is_break) { PIC_SendEOI(1); return; }

    if (key >= 89) { PIC_SendEOI(1); return; }

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
                PIC_SendEOI(1); return;
            }
        }
        if (g_kbd_mods.super_left) {
            /* LAmiga + V/B/M/N → special requester/screen codes */
            char upper = ascii;
            if (ascii >= 'a' && ascii <= 'z') upper = (char)(ascii - 'a' + 'A');
            switch (upper) {
                case 'V': kbuf_push(AMIGA_LV); PIC_SendEOI(1); return;
                case 'B': kbuf_push(AMIGA_LB); PIC_SendEOI(1); return;
                case 'M': kbuf_push(AMIGA_LM); PIC_SendEOI(1); return;
                case 'N': kbuf_push(AMIGA_LN); PIC_SendEOI(1); return;
            }
        }
        kbuf_push(ascii);
    }

    PIC_SendEOI(1);
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
