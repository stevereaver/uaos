/* ps2mouse.c — UAOS PS/2 Mouse driver
 *
 * Communicates with the PS/2 controller (i8042) to enable the auxiliary
 * (mouse) port.  Receives 3-byte standard PS/2 packets on IRQ12 (vector 44)
 * and updates a shared MouseState struct.  Calls Cursor_Move() to redraw
 * the hardware pointer on the framebuffer.
 */

#include "ps2mouse.h"
#include "idt.h"
#include "../display/cursor.h"
#include <stdint.h>

/* P4: set to 1 to re-enable the per-packet serial dump in the IRQ handler.
 * Default 0 — the dump floods the serial log and costs ~40 UART busy-waits
 * per mouse packet at IRQ time (IF=0), skewing PIT ticks. */
#ifndef MOUSE_DEBUG
#define MOUSE_DEBUG 0
#endif

/* =========================================================================
 * i8042 port constants
 * ========================================================================= */

#define PS2_DATA    0x60    /* read/write data                  */
#define PS2_STATUS  0x64    /* read  status register            */
#define PS2_CMD     0x64    /* write command register           */

/* Status bits */
#define PS2_STAT_OBF  0x01  /* output buffer full (data ready to read) */
#define PS2_STAT_IBF  0x02  /* input buffer full  (busy, do not write) */
#define PS2_STAT_AUX  0x20  /* data came from aux (mouse) port         */
#define PS2_AUX       0x20  /* alias used in IRQ handler                */

/* =========================================================================
 * I/O helpers
 * ========================================================================= */

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* =========================================================================
 * Minimal serial debug output (COM1 = 0x3F8) — defined after inb/outb
 * ========================================================================= */
static void _ser_putc(char c) {
    while ((inb(0x3F8 + 5) & 0x20) == 0) {}
    outb(0x3F8, (uint8_t)c);
    if (c == '\n') _ser_putc('\r');
}
static void _ser_puts(const char *s) { while (*s) _ser_putc(*s++); }
static void _ser_hex8(uint8_t v) {
    const char *h = "0123456789ABCDEF";
    _ser_putc(h[v >> 4]); _ser_putc(h[v & 0xF]);
}
static void _ser_int(int v) {
    if (v < 0) { _ser_putc('-'); v = -v; }
    if (v >= 100) _ser_putc('0' + (v / 100) % 10);
    if (v >= 10)  _ser_putc('0' + (v / 10)  % 10);
    _ser_putc('0' + v % 10);
}

static void ps2_wait_write(void)
{
    int timeout = 100000;
    while ((inb(PS2_STATUS) & PS2_STAT_IBF) && --timeout) {}
}

static void ps2_wait_read(void)
{
    int timeout = 100000;
    while (!(inb(PS2_STATUS) & PS2_STAT_OBF) && --timeout) {}
}

static void ps2_cmd(uint8_t cmd)
{
    ps2_wait_write();
    outb(PS2_CMD, cmd);
}

static void ps2_write_mouse(uint8_t byte)
{
    ps2_cmd(0xD4);          /* route next byte to aux port      */
    ps2_wait_write();
    outb(PS2_DATA, byte);
}

static uint8_t ps2_read(void)
{
    ps2_wait_read();
    return inb(PS2_DATA);
}

/* =========================================================================
 * Global mouse state
 * ========================================================================= */

MouseState g_mouse = { 0, 0, 0, 0, 0 };

static uint8_t  pkt[3];
static int      pkt_idx = 0;

/* Screen boundary — set during init from g_fb */
extern unsigned int g_fb_width_irq;
extern unsigned int g_fb_height_irq;

/* =========================================================================
 * PS2Mouse_Init
 * ========================================================================= */

/* Drain without blocking — discard up to 128 pending bytes */
static void ps2_drain(void)
{
    int n = 128;
    while ((inb(PS2_STATUS) & PS2_STAT_OBF) && --n)
        inb(PS2_DATA);
}

/* Write a byte to the mouse and drain any ACK/response without blocking */
static void ps2_mouse_cmd(uint8_t byte)
{
    ps2_write_mouse(byte);
    /* Small busy-wait for ACK to arrive, then drain */
    for (int i = 0; i < 10000; i++) {
        if (inb(PS2_STATUS) & PS2_STAT_OBF) break;
    }
    ps2_drain();
}

void PS2Mouse_Init(void)
{
    ps2_drain();

    /* Disable both PS/2 ports while we reconfigure */
    ps2_cmd(0xAD);   /* disable keyboard port */
    ps2_cmd(0xA7);   /* disable aux port      */
    ps2_drain();

    /* Read controller config, enable aux interrupt, clear aux disable */
    ps2_cmd(0x20);
    uint8_t cfg = ps2_read();
    cfg |=  0x02;    /* bit 1: enable IRQ12 for aux port  */
    cfg &= ~0x20;    /* bit 5: clear aux clock disable    */
    ps2_cmd(0x60);
    ps2_wait_write();
    outb(PS2_DATA, cfg);

    /* Re-enable aux port */
    ps2_cmd(0xA8);
    ps2_drain();

    /* Enable data reporting — stream mode, don't wait for ACK to block */
    ps2_mouse_cmd(0xF4);

    /* Re-enable keyboard port */
    ps2_cmd(0xAE);
    ps2_drain();

    /* Initialise position to screen centre */
    g_mouse.x = (int)(g_fb_width_irq  >> 1);
    g_mouse.y = (int)(g_fb_height_irq >> 1);
    pkt_idx   = 0;
}

/* =========================================================================
 * PS2Mouse_IRQHandler — called by IDT vector 44 (IRQ12)
 *
 * Standard PS/2 mouse packet (3 bytes):
 *   Byte 0: flags  [Y-ovf | X-ovf | Y-sign | X-sign | 1 | M | R | L]
 *   Byte 1: X movement (2's complement, sign in byte 0 bit 4)
 *   Byte 2: Y movement (2's complement, sign in byte 0 bit 5, Y axis inverted)
 * ========================================================================= */

void PS2Mouse_IRQHandler(uint64_t vector, uint64_t error_code)
{
    (void)vector; (void)error_code;

    uint8_t status = inb(PS2_STATUS);

    /* No data ready — spurious IRQ */
    if (!(status & PS2_STAT_OBF)) {
        PIC_SendEOI(12);
        return;
    }
    /* IRQ12 is wired exclusively to the aux (mouse) port by the 8259A,
     * so we don't need to check the AUX status bit — just read the byte. */

    uint8_t data = inb(PS2_DATA);

    /* Sync: first byte must have bit 3 set (always 1 in standard packets).
     * If out of sync, reset and try to re-sync on this byte. */
    if (pkt_idx == 0 && !(data & 0x08)) {
        PIC_SendEOI(12);
        return;
    }

    pkt[pkt_idx++] = data;

    if (pkt_idx < 3) {
        PIC_SendEOI(12);
        return;
    }
    pkt_idx = 0;

    /* Decode packet */
    uint8_t flags = pkt[0];

    /* Overflow bits — discard packet if set */
    if ((flags & 0x40) || (flags & 0x80)) {
        PIC_SendEOI(12);
        return;
    }

    /* dx/dy are 9-bit 2's complement values.  The sign bit is flags[4/5].
     * Cast to int8_t first to get the sign from the byte itself (QEMU
     * always sets the byte correctly), then override sign if the flags
     * sign bit disagrees (real hardware edge case). */
    int dx = (int)(int8_t)pkt[1];
    int dy = (int)(int8_t)pkt[2];
    /* If flags says negative but byte looks positive, correct it */
    if ((flags & 0x10) && dx > 0) dx -= 256;
    if ((flags & 0x20) && dy > 0) dy -= 256;

    /* Y axis is inverted in PS/2 (positive = up on screen) */
    dy = -dy;

    int new_x = g_mouse.x + dx;
    int new_y = g_mouse.y + dy;

    /* Clamp to framebuffer */
    int max_x = (int)g_fb_width_irq  - 1;
    int max_y = (int)g_fb_height_irq - 1;
    if (new_x < 0)     new_x = 0;
    if (new_y < 0)     new_y = 0;
    if (new_x > max_x) new_x = max_x;
    if (new_y > max_y) new_y = max_y;

    g_mouse.x          = new_x;
    g_mouse.y          = new_y;
    g_mouse.btn_left   = (flags & 0x01) ? 1 : 0;
    g_mouse.btn_right  = (flags & 0x02) ? 1 : 0;
    g_mouse.btn_middle = (flags & 0x04) ? 1 : 0;

    /* P4: per-packet serial dump gated behind MOUSE_DEBUG (default 0).
     * Each char busy-waits on UART LSR and causes a TCG vmexit; with IF=0
     * this also skews PIT ticks.  Set MOUSE_DEBUG=1 to re-enable. */
#if MOUSE_DEBUG
    _ser_puts("M["); _ser_hex8(pkt[0]); _ser_putc(' ');
    _ser_hex8(pkt[1]); _ser_putc(' '); _ser_hex8(pkt[2]);
    _ser_puts("] dx="); _ser_int(dx);
    _ser_puts(" dy="); _ser_int(dy);
    _ser_puts(" x="); _ser_int(new_x);
    _ser_puts(" y="); _ser_int(new_y);
    _ser_putc('\n');
#endif

    Cursor_Move(new_x, new_y);

    PIC_SendEOI(12);
}
