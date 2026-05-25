/*
 * uaos_kernel_main.c — UAOS C-level Kernel Entry Point
 *
 * Called from uaos_kernel_entry.asm after long-mode transition.
 * Receives the Multiboot2 magic value and info-structure pointer.
 *
 * This is a freestanding (no-libc) translation unit.  All I/O is done via
 * direct port writes to the VGA text-mode framebuffer and the 16550 UART.
 */

#include <stdint.h>
#include <stddef.h>
#include "../display/framebuffer.h"
#include "../display/desktop.h"
#include "../display/cursor.h"
#include "../display/shell_win.h"
#include "../irq/idt.h"
#include "../irq/ps2mouse.h"
#include "../irq/ps2kbd.h"
#include "../irq/vmmouse.h"
#include "../irq/rtc.h"
#include "../display/wm.h"
#include "dos/vfs.h"

/* -----------------------------------------------------------------------
 * Multiboot2 constants
 * ----------------------------------------------------------------------- */
#define MB2_MAGIC_EXPECTED  0x36D76289U

/* -----------------------------------------------------------------------
 * Minimal VGA text-mode console (80×25, port-mapped at 0xB8000)
 * ----------------------------------------------------------------------- */

#define VGA_BASE   ((volatile uint16_t *)0xB8000)
#define VGA_COLS   80
#define VGA_ROWS   25
#define VGA_ATTR   0x0F00   /* white on black */

static int vga_col = 0;
static int vga_row = 0;

static void vga_clear(void)
{
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++)
        VGA_BASE[i] = VGA_ATTR | ' ';
    vga_col = 0;
    vga_row = 0;
}

static void vga_scroll(void)
{
    for (int r = 0; r < VGA_ROWS - 1; r++)
        for (int c = 0; c < VGA_COLS; c++)
            VGA_BASE[r * VGA_COLS + c] = VGA_BASE[(r+1) * VGA_COLS + c];
    for (int c = 0; c < VGA_COLS; c++)
        VGA_BASE[(VGA_ROWS-1) * VGA_COLS + c] = VGA_ATTR | ' ';
    vga_row = VGA_ROWS - 1;
}

static void vga_putchar(char ch)
{
    if (ch == '\n') {
        vga_col = 0;
        vga_row++;
    } else if (ch == '\r') {
        vga_col = 0;
    } else {
        VGA_BASE[vga_row * VGA_COLS + vga_col] = (uint16_t)(VGA_ATTR | (uint8_t)ch);
        vga_col++;
        if (vga_col >= VGA_COLS) { vga_col = 0; vga_row++; }
    }
    if (vga_row >= VGA_ROWS) vga_scroll();
}

static void vga_puts(const char *s)
{
    while (*s) vga_putchar(*s++);
}

static void vga_puthex(uint64_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    vga_puts("0x");
    for (int i = 60; i >= 0; i -= 4)
        vga_putchar(hex[(v >> i) & 0xF]);
}

/* -----------------------------------------------------------------------
 * Serial UART (16550A, COM1 = 0x3F8)
 * ----------------------------------------------------------------------- */

#define UART_BASE  0x3F8

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

static void uart_init(void)
{
    outb(UART_BASE + 1, 0x00);  /* Disable interrupts                      */
    outb(UART_BASE + 3, 0x80);  /* Enable DLAB                             */
    outb(UART_BASE + 0, 0x03);  /* 38400 baud (divisor lo)                 */
    outb(UART_BASE + 1, 0x00);  /* divisor hi                              */
    outb(UART_BASE + 3, 0x03);  /* 8N1                                     */
    outb(UART_BASE + 2, 0xC7);  /* FIFO enable, clear, 14-byte threshold   */
    outb(UART_BASE + 4, 0x0B);  /* RTS/DSR                                 */
}

static void uart_putchar(char ch)
{
    while ((inb(UART_BASE + 5) & 0x20) == 0) {}
    outb(UART_BASE, (uint8_t)ch);
    if (ch == '\n') uart_putchar('\r');
}

static void uart_puts(const char *s)
{
    while (*s) uart_putchar(*s++);
}

/* -----------------------------------------------------------------------
 * Combined console output
 * ----------------------------------------------------------------------- */

static void kprint(const char *s)
{
    vga_puts(s);
    uart_puts(s);
}

static void kprinthex(uint64_t v)
{
    vga_puthex(v);
    static const char hex[] = "0123456789ABCDEF";
    uart_puts("0x");
    char buf[17]; buf[16] = 0;
    for (int i = 0; i < 16; i++)
        buf[15-i] = hex[(v >> (i*4)) & 0xF];
    uart_puts(buf);
}

/* -----------------------------------------------------------------------
 * UAOS Banner
 * ----------------------------------------------------------------------- */

static void print_banner(void)
{
    kprint("\n");
    kprint("  +----------------------------------------------------------+\n");
    kprint("  |         ULTIMATE AMIGA OS  (UAOS)  v0.1.0-dev           |\n");
    kprint("  |    x86_64 AROS-derived microkernel + M68k JIT sandbox    |\n");
    kprint("  +----------------------------------------------------------+\n");
    kprint("\n");
}

/* -----------------------------------------------------------------------
 * Subsystem forward declarations
 * ----------------------------------------------------------------------- */

extern void UAOS_MMU_Init(void);
extern void UAOS_ROM_RegisterAll(void);
extern int  UAOS_Bridge_Init(void);
extern void FB_Init(uint32_t mb2_info_phys);
extern void Desktop_Draw(void);
/* screen-size globals used by PS/2 mouse clamp (defined in stubs.c) */
extern unsigned int g_fb_width_irq;
extern unsigned int g_fb_height_irq;

/* -----------------------------------------------------------------------
 * uaos_kernel_main — C entry point
 *
 * Parameters (passed by uaos_kernel_entry.asm via SysV ABI):
 *   edi = multiboot2 magic
 *   esi = multiboot2 info structure physical address (32-bit)
 * ----------------------------------------------------------------------- */

void uaos_kernel_main(uint32_t mb2_magic, uint32_t mb2_info_phys)
{
    uart_init();
    vga_clear();
    print_banner();

    /* Validate Multiboot2 handoff */
    if (mb2_magic != MB2_MAGIC_EXPECTED) {
        kprint("[BOOT] FATAL: Invalid Multiboot2 magic: ");
        kprinthex(mb2_magic);
        kprint("\n[BOOT] Halting.\n");
        goto halt;
    }

    kprint("[BOOT] Multiboot2 magic OK.  Info struct @ ");
    kprinthex((uint64_t)mb2_info_phys);
    kprint("\n");

    /* Initialise framebuffer from Multiboot2 info */
    kprint("[BOOT] Initialising framebuffer...\n");
    FB_Init(mb2_info_phys);
    if (g_fb.valid) {
        kprint("[BOOT] Framebuffer ready: ");
        kprinthex((uint64_t)g_fb.width);
        kprint("x");
        kprinthex((uint64_t)g_fb.height);
        kprint(" ");
        kprinthex((uint64_t)g_fb.bpp);
        kprint("bpp\n");
    } else {
        kprint("[BOOT] WARNING: No framebuffer from bootloader.\n");
    }

    /* Initialise MMU sandbox — bare-metal only */
    kprint("[BOOT] Initialising MMU sandbox...\n");
    UAOS_MMU_Init();
    kprint("[BOOT] MMU sandbox active.\n");

    /* Register all built-in ROM library modules */
    kprint("[BOOT] Registering ROM modules...\n");
    UAOS_ROM_RegisterAll();
    kprint("[BOOT] ROM modules registered.\n");

    /* Initialise the UAE emulation bridge (allocates 4 GB guest RAM) */
    kprint("[BOOT] Initialising M68k emulation bridge...\n");
    int rc = UAOS_Bridge_Init();
    if (rc != 0) {
        kprint("[BOOT] WARNING: Bridge init returned ");
        kprinthex((uint64_t)rc);
        kprint(" — emulation unavailable.\n");
    } else {
        kprint("[BOOT] M68k emulation bridge ready.\n");
    }

    kprint("\n[BOOT] UAOS kernel initialisation complete.\n");

    /* Initialise virtual filesystem + RAM: disk */
    kprint("[BOOT] Initialising VFS + RAM disk...\n");
    VFS_Init();
    kprint("[BOOT] RAM: mounted.\n");

    /* Draw the Workbench-style desktop */
    if (g_fb.valid) {
        kprint("[BOOT] Drawing desktop...\n");
        Desktop_Draw();
        kprint("[BOOT] Desktop rendered.\n");
    }

    /* Set up interrupts — IDT must be loaded before STI */
    kprint("[BOOT] Initialising IDT...\n");
    IDT_Init();
    kprint("[BOOT] Initialising PIC...\n");
    PIC_Init();

    /* Initialise PS/2 mouse (needs IRQ12 = vector 44) */
    if (g_fb.valid) {
        g_fb_width_irq  = g_fb.width;
        g_fb_height_irq = g_fb.height;

        kprint("[BOOT] Initialising PS/2 mouse...\n");
        IDT_SetHandler(44, PS2Mouse_IRQHandler);
        PS2Mouse_Init();
        PIC_UnmaskIRQ(12);
        Cursor_Init(g_mouse.x, g_mouse.y);
        kprint("[BOOT] PS/2 mouse active.\n");

        kprint("[BOOT] Initialising PS/2 keyboard...\n");
        IDT_SetHandler(33, PS2Kbd_IRQHandler);
        PS2Kbd_Init();
        PIC_UnmaskIRQ(1);
        kprint("[BOOT] PS/2 keyboard active.\n");

        kprint("[BOOT] Opening shell window...\n");
        ShellWin_Init();

        kprint("[BOOT] Detecting vmmouse...\n");
        VMMouse_Init();
        if (VMMouse_Detect())
            kprint("[BOOT] vmmouse active (absolute mode).\n");
        else
            kprint("[BOOT] vmmouse not found, using PS/2 relative.\n");

        kprint("[BOOT] Initialising RTC clock...\n");
        IDT_SetHandler(40, RTC_IRQHandler);  /* IRQ8 = vector 40 */
        RTC_Init();
        PIC_UnmaskIRQ(8);
        Desktop_UpdateClock();               /* initial draw from CMOS */
        kprint("[BOOT] RTC active.\n");
    }

    kprint("[BOOT] Enabling interrupts — entering event loop.\n");
    __asm__ volatile ("sti");

    /* Event loop — halt until IRQ fires, then dispatch input */
    int last_mx = -1, last_my = -1, last_btn = -1;
    for (;;) {
        __asm__ volatile ("hlt");
        /* Only dispatch mouse event if state actually changed */
        if (g_fb.valid) {
            int mx = g_mouse.x, my = g_mouse.y, btn = g_mouse.btn_left;
            if (mx != last_mx || my != last_my || btn != last_btn) {
                last_mx = mx; last_my = my; last_btn = btn;
                WM_MouseEvent(mx, my, btn);
            }
        }
        /* Keyboard -> focused window via WM */
        while (PS2Kbd_HasChar())
            WM_KeyEvent(PS2Kbd_GetChar());
    }
    return;

halt:
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}
