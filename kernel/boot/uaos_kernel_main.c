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
#include "../irq/virtio_blk.h"
#include "../drivers/virtio_net.h"
#include "../net/stack.h"
#include "../exec/bsdsocket_lib.h"
#include "../display/wm.h"
#include "dos/vfs.h"
#include "dos/blockdev.h"
#include "dos/partition.h"
#include "drivers/ide.h"
#include "dos/iso9660.h"

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

/* -----------------------------------------------------------------------
 * Local APIC helpers — q35 enables LAPIC by default but LINT0 may need
 * explicit ExtINT configuration for the 8259A PIC to deliver IRQs.
 * ----------------------------------------------------------------------- */

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val)
{
    uint32_t lo = (uint32_t)val;
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ volatile ("wrmsr" :: "a"(lo), "d"(hi), "c"(msr));
}

/* Forward declarations (defined further down) */
void kprint(const char *s);
void kprinthex(uint64_t v);
void kprintdec(uint32_t v);

static void APIC_Init(void)
{
    /* Read APIC base from MSR 0x1B.  Bits 35:12 are the physical base. */
    uint64_t apic_base = rdmsr(0x1B);
    kprint("[APIC] MSR 0x1B base="); kprinthex(apic_base); kprint("\n");

    /* Default base is 0xFEE00000; bail if the APIC is not enabled. */
    if (!(apic_base & 0x800)) {
        kprint("[APIC] WARNING: APIC not enabled in MSR!\n");
        return;
    }

    uint32_t *lapic = (uint32_t *)(apic_base & 0xFFFFF000);

    /* Spurious Interrupt Vector Register (SVR) at offset 0x0F0 */
    uint32_t svr = lapic[0x0F0 / 4];
    kprint("[APIC] SVR old="); kprinthex(svr); kprint("\n");
    /* Ensure APIC software enable (bit 8) and set spurious vector 0xFF */
    svr = (svr & ~0xFF) | 0xFF | 0x100;
    lapic[0x0F0 / 4] = svr;

    /* LVT LINT0 at offset 0x350 — configure for ExtINT (delivery mode 111) */
    uint32_t lint0 = lapic[0x350 / 4];
    kprint("[APIC] LINT0 old="); kprinthex(lint0); kprint("\n");
    lint0 = (lint0 & ~0x10700) | 0x700;  /* ExtINT, not masked, edge trig */
    lapic[0x350 / 4] = lint0;

    kprint("[APIC] LINT0 new="); kprinthex(lapic[0x350 / 4]); kprint("\n");
    kprint("[APIC] APIC initialised.\n");
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

/* Simple VGA text-mode console output */
void kprint(const char *s)
{
    vga_puts(s);
    uart_puts(s);
}

void kprinthex(uint64_t v)
{
    vga_puthex(v);
    static const char hex[] = "0123456789ABCDEF";
    uart_puts("0x");
    char buf[17]; buf[16] = 0;
    for (int i = 0; i < 16; i++)
        buf[15-i] = hex[(v >> (i*4)) & 0xF];
    uart_puts(buf);
}

void kprintdec(uint32_t v)
{
    char buf[12];
    int i = 0;
    if (v == 0) {
        buf[0] = '0'; buf[1] = '\0';
        kprint(buf);
        return;
    }
    while (v > 0 && i < 11) {
        buf[i++] = '0' + (v % 10);
        v /= 10;
    }
    buf[i] = '\0';
    /* Reverse in place for output */
    for (int j = 0; j < i / 2; j++) {
        char t = buf[j]; buf[j] = buf[i - 1 - j]; buf[i - 1 - j] = t;
    }
    kprint(buf);
}

/* -----------------------------------------------------------------------
 * PIT timer test handler — prints a dot to verify interrupt delivery
 * ----------------------------------------------------------------------- */
volatile uint64_t g_pit_ticks = 0;

void PIT_IRQHandler(uint64_t vector, uint64_t error_code)
{
    (void)vector; (void)error_code;
    g_pit_ticks++;
    if ((g_pit_ticks % 10) == 0) {
        kprint("[PIT] tick="); kprintdec((uint32_t)g_pit_ticks); kprint("\n");
    }
    net_stack_tick();
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
extern void UAOS_LoadableLib_Init(void);
extern void UAOS_POWERPACKER_Register(void);
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

    /* Initialise block device layer */
    kprint("[BOOT] Initialising block device layer...\n");
    BlockDev_Init();
    kprint("[BOOT] Block device layer initialised.\n");

    /* Initialise VirtIO block device driver */
    kprint("[BOOT] Scanning for VirtIO block devices...\n");
    if (virtio_blk_init() == 0) {
        kprint("[BOOT] VirtIO block device detected and registered.\n");
        /* Auto-detect partitions on virtio0 */
        BlockDev *vdev = BlockDev_Find("virtio0");
        if (vdev) {
            PartitionTable pt;
            if (partition_read(vdev, &pt) == 0 && pt.valid && pt.scheme == PART_SCHEME_MBR) {
                for (int i = 0; i < MBR_PART_COUNT; i++) {
                    if (pt.mbr.partitions[i].type_code != PART_TYPE_EMPTY) {
                        char namebuf[16];
                        const char *dname = uaos_meta_get_name(&pt.uaos_meta, i, namebuf, sizeof(namebuf));
                        BlockDev *pdev = BlockDev_RegisterPartition(vdev, i + 1,
                            pt.mbr.partitions[i].lba_start,
                            pt.mbr.partitions[i].sector_count, dname);
                        if (pdev && BlockDev_CheckFormatted(pdev)) {
                            /* Strip trailing colon for VFS mount name */
                            char mnt_name[16];
                            int ni = 0, si = 0;
                            while (si < 15 && dname[si] && dname[si] != ':')
                                mnt_name[ni++] = dname[si++];
                            mnt_name[ni] = '\0';

                            /* Mount by FAT32 volume label if available,
                             * otherwise fall back to the device name (DH0 etc.) */
                            char fat_label[16];
                            if (BlockDev_ReadVolLabel(pdev, fat_label, sizeof(fat_label))) {
                                VFS_MountPartition(fat_label);
                            } else if (mnt_name[0]) {
                                VFS_MountPartition(mnt_name);
                            }
                        }
                    }
                }
            }
        }
    } else {
        kprint("[BOOT] No VirtIO block device found (this is OK if no disk attached).\n");
    }

    /* Initialise IDE/ATAPI controller and register CD-ROM block devices */
    kprint("[BOOT] Initialising IDE controller...\n");
    IDE_Init();
    IDE_RegisterBlockDevs();

    /* Scan for ATAPI CD-ROMs and mount ISO 9660 volumes */
    {
        extern int g_virtio_irq_line;
        extern unsigned int g_canary_before;
        extern unsigned int g_canary_after;
        kprint("[BOOT] virtio_irq_line before ISO9660 = "); kprinthex(g_virtio_irq_line);
        kprint(" canary_before="); kprinthex(g_canary_before);
        kprint(" canary_after="); kprinthex(g_canary_after); kprint("\n");
    }
    kprint("[BOOT] Scanning for ATAPI CD-ROMs...\n");
    for (int ch = 0; ch < IDE_GetChannelCount(); ch++) {
        for (int dev = 0; dev < 2; dev++) {
            const IdeDeviceInfo *info = IDE_GetDeviceInfo(ch, dev);
            if (info && info->present && info->type == IDE_DEV_ATAPI) {
                char bdev_name[16];
                int idx = ch * 2 + dev;
                bdev_name[0] = 'a'; bdev_name[1] = 't'; bdev_name[2] = 'a';
                bdev_name[3] = 'p'; bdev_name[4] = 'i'; bdev_name[5] = '0' + idx;
                bdev_name[6] = '\0';
                BlockDev *cd_dev = BlockDev_Find(bdev_name);
                if (cd_dev) {
                    kprint("[BOOT] Mounting ISO 9660 from ");
                    kprint(bdev_name);
                    kprint("...\n");
                    if (ISO9660_MountCD(cd_dev, "Workbench") == 0) {
                        kprint("[BOOT] Workbench: mounted.\n");
                    } else {
                        kprint("[BOOT] ISO 9660 mount failed.\n");
                    }
                }
            }
        }
    }
    {
        extern int g_virtio_irq_line;
        extern unsigned int g_canary_before;
        extern unsigned int g_canary_after;
        kprint("[BOOT] virtio_irq_line after ISO9660 = "); kprinthex(g_virtio_irq_line);
        kprint(" canary_before="); kprinthex(g_canary_before);
        kprint(" canary_after="); kprinthex(g_canary_after); kprint("\n");
    }

    /* Note: Desktop is NOT drawn here - it starts via LoadWB from Startup-Sequence */

    /* Set up interrupts — IDT must be loaded before STI */
    kprint("[BOOT] Initialising IDT...\n");
    IDT_Init();
    kprint("[BOOT] Initialising PIC...\n");
    PIC_Init();
    /* PIT is programmed and unmasked later, after its handler is registered */

    /* Register VirtIO interrupt handler (must be after IDT/PIC init) */
    kprint("[BOOT] Registering VirtIO IRQ...\n");
    virtio_blk_setup_irq();

    /* Initialise network stack (e1000 or virtio-net, auto-detected).
     * DHCP is tried first (3-second timeout).  The static fallback of
     * 10.0.2.15/24 gw 10.0.2.2 matches the QEMU/VirtualBox NAT default
     * and is only used when DHCP receives no reply at all (e.g. no DHCP
     * server on a bridged network segment with no router). */
    kprint("[BOOT] Initialising network stack...\n");
    if (net_stack_init(IPV4(10,0,2,15), IPV4(10,0,2,2), IPV4(255,255,255,0))) {
        if (net_stack_dhcp_used()) {
            char ipbuf[20];
            net_ip_to_str(net_stack_get_ip(), ipbuf);
            kprint("[BOOT] Network up via DHCP: ");
            kprint(ipbuf);
            kprint("\n");
        } else {
            kprint("[BOOT] Network up: 10.0.2.15/24 gw 10.0.2.2 (static fallback)\n");
        }
    } else {
        kprint("[BOOT] No network card found.\n");
    }
    BsdSocket_Init();

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

        kprint("[BOOT] Setting up Workbench assigns...\n");
        VFS_SetupWorkbenchAssigns();

        kprint("[BOOT] Scanning LIBS: for loadable libraries...\n");
        UAOS_LoadableLib_Init();
        UAOS_POWERPACKER_Register();

        kprint("[BOOT] Opening shell window...\n");
        ShellWin_Init();

        kprint("[BOOT] Running Startup-Sequence...\n");
        ShellWin_RunStartupSequence();

        kprint("[BOOT] Detecting vmmouse...\n");
        VMMouse_Init();
        if (VMMouse_Detect())
            kprint("[BOOT] vmmouse active (absolute mode).\n");
        else
            kprint("[BOOT] vmmouse not found, using PS/2 relative.\n");

        /* Program PIT at 10 Hz BEFORE enabling RTC so that g_pit_ticks is
         * already ticking when the first RTC UIE fires.  ntp_tick_epoch()
         * gates on g_pit_ticks, so it must be running first. */
        kprint("[BOOT] Programming PIT (10 Hz)...\n");
        IDT_SetHandler(32, PIT_IRQHandler);
        {
            uint16_t divisor = (uint16_t)(1193180UL / 10UL);
            outb(0x43, 0x36);
            outb(0x40, (uint8_t)(divisor & 0xFF));
            outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
        }
        PIC_UnmaskIRQ(0);
        kprint("[BOOT] PIT active.\n");

        kprint("[BOOT] Initialising RTC clock...\n");
        IDT_SetHandler(40, RTC_IRQHandler);  /* IRQ8 = vector 40 */
        RTC_Init();
        PIC_UnmaskIRQ(8);
        Desktop_UpdateClock();               /* initial draw from CMOS */
        kprint("[BOOT] RTC active.\n");
    }

    /* Initialise local APIC so q35 forwards 8259A PIC interrupts */
    APIC_Init();

    kprint("[BOOT] Enabling interrupts — entering event loop.\n");
    __asm__ volatile ("sti");

    /* Event loop — yield until IRQ fires, then dispatch input */
    int last_mx = -1, last_my = -1, last_btn = -1;
    uint64_t loop_count = 0;
    for (;;) {
        /* "memory" clobber forces compiler to reload g_mouse / kbd state
         * from memory on every iteration (ISRs write them).            */
        __asm__ volatile ("pause" ::: "memory");

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

        /* Flush any pending clock redraw requested by the RTC IRQ */
        if (g_fb.valid) Desktop_FlushClockRedraw();

        /* Poll network stack for incoming packets */
        net_stack_poll();

        /* Periodic heartbeat so we know the loop hasn't hung */
        loop_count++;
        if ((loop_count & 0x7FFFFFF) == 0) {

            volatile uint32_t *mbox = (volatile uint32_t *)0x90000;
            kprint("[EVT] loop="); kprinthex(loop_count);
            kprint(" pit="); kprinthex(g_pit_ticks);
            kprint(" mbox="); kprinthex(mbox[0]);
            kprint(" v="); kprinthex(mbox[1]);
            kprint(" seq="); kprinthex(mbox[2]); kprint("\n");
        }
    }
    return;

halt:
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}
