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
#include "../audio/audio.h"
#include "../display/cursor.h"
#include "../display/shell_win.h"
#include "../display/user_window.h"
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
#include "drivers/floppy_blk.h"
#include "exec/task.h"
#include "exec/syscall_table.h"
#include "chipset/floppy.h"
#include "chipset/chip_emu.h"
#include "uaos_emu.h"

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

static void vga_write(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++)
        vga_putchar(s[i]);
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

static void uart_write(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++)
        uart_putchar(s[i]);
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

void kprintbuf(const char *s, size_t len)
{
    vga_write(s, len);
    uart_write(s, len);
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

extern void Task_ScheduleFromIRQ(void);
extern void timer_ProcessTicks(void);
extern int FloppyBlockDev_Init(void);
extern BlockDev *BlockDev_Find(const char *name);
extern int BlockDev_Read(BlockDev *dev, uint64_t sector, void *buffer, uint32_t num_sectors);

void PIT_IRQHandler(uint64_t vector, uint64_t error_code)
{
    (void)vector; (void)error_code;
    g_pit_ticks++;
    net_stack_tick();
    timer_ProcessTicks();
    Task_ScheduleFromIRQ();
}

/* -----------------------------------------------------------------------
 * Software interrupt handler (vector 0x80) — UAOS x86-64 syscall table
 * ----------------------------------------------------------------------- */
extern void uaos_syscall_isr(void);

/* -----------------------------------------------------------------------
 * UAOS Banner
 * ----------------------------------------------------------------------- */

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
extern void FB_Init(uint32_t mb2_info_phys);
extern void chip_emu_reset(void);
extern void audio_init(void);
extern void audio_sine_test(void);
extern void audio_pattern_test(void);
extern void floppy_make_test_adf(void);
extern int chip_emu_disk_dma_test(void);
extern int floppy_block_device_test(void);
extern int chip_emu_dma_test(void);
extern uint16_t g_intreq;
extern int chip_emu_line_test(void);
extern int chip_emu_fill_test(void);
extern int chip_emu_fill_complex_test(void);
extern int chip_emu_raster_test(void);
extern int chip_emu_sprite_test(void);
extern int chip_emu_parallel_test(void);
extern int chip_emu_serial_test(void);
extern int chip_emu_timing_lock_test(void);
extern int chip_emu_timing_contention_test(void);
extern int chip_emu_hblank_test(void);
extern int chip_emu_agnus_slot_test(void);
extern int chip_emu_sprite_border_test(void);
extern int chip_emu_sprite_priority_test(void);
extern int chip_emu_sprite_superhires_test(void);
extern int chip_emu_sprite_subpixel_test(void);
extern void Desktop_Draw(void);
extern void uaos_page_fault_isr(void);
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

    /* Reset AGA chipset state to hardware-correct defaults */
    kprint("[BOOT] Resetting AGA chipset state...\n");
    chip_emu_reset();

    /* Initialise MMU sandbox — bare-metal only.
     *
     * Must be active before any code touches the linear framebuffer, because
     * the Multiboot2-provided framebuffer address can sit above the first
     * 1 GB (e.g. VirtualBox maps the VGA LFB at 0x80000000) while the
     * bootstrap page tables only identity-map the first 1 GB.  Installing the
     * sandbox here gives us a full 4 GB identity map including the FB region. */
    kprint("[BOOT] Initialising MMU sandbox...\n");
    UAOS_MMU_Init();
    kprint("[BOOT] MMU sandbox active.\n");

    /* Initialise host audio subsystem */
    kprint("[BOOT] Initialising audio subsystem...\n");
    audio_init();
    const char *backend_name = audio_backend_name();
    if (backend_name) {
        kprint("[BOOT] Audio backend: ");
        kprint(backend_name);
        kprint("\n");
    } else {
        kprint("[BOOT] WARNING: no audio backend available.\n");
    }

    /* Run audio subsystem tests */
    kprint("[BOOT] Running audio sine-wave test...\n");
    audio_sine_test();
    kprint("[BOOT] Running audio pattern test...\n");
    audio_pattern_test();

    kprint("[BOOT] Initialising floppy subsystem...\n");
    floppy_make_test_adf();
    kprint("[BOOT] Running disk DMA test...\n");
    int disk_test = chip_emu_disk_dma_test();
    kprint(disk_test ? "[BOOT] Disk DMA test PASSED\n" : "[BOOT] Disk DMA test FAILED\n");

    /* Run DMA slot-arbitration test */
    kprint("[BOOT] Running DMA slot test...\n");
    int dma_test = chip_emu_dma_test();
    kprint(dma_test ? "[BOOT] DMA slot test PASSED\n" : "[BOOT] DMA slot test FAILED\n");

    /* Run Blitter line and fill tests */
    kprint("[BOOT] Running Blitter line test...\n");
    int line_test = chip_emu_line_test();
    kprint(line_test ? "[BOOT] Blitter line test PASSED\n" : "[BOOT] Blitter line test FAILED\n");
    kprint("[BOOT] Running Blitter fill test...\n");
    int fill_test = chip_emu_fill_test();
    kprint(fill_test ? "[BOOT] Blitter fill test PASSED\n" : "[BOOT] Blitter fill test FAILED\n");

    kprint("[BOOT] Running Blitter complex fill test...\n");
    int fill_complex_test = chip_emu_fill_complex_test();
    kprint(fill_complex_test ? "[BOOT] Blitter complex fill test PASSED\n" : "[BOOT] Blitter complex fill test FAILED\n");

    /* Run color-clock raster test */
    kprint("[BOOT] Running color-clock raster test...\n");
    int raster_test = chip_emu_raster_test();
    kprint(raster_test ? "[BOOT] Color-clock raster test PASSED\n" : "[BOOT] Color-clock raster test FAILED\n");

    /* Run AGA sprite tests */
    kprint("[BOOT] Running AGA sprite test...\n");
    int sprite_test = chip_emu_sprite_test();
    kprint(sprite_test ? "[BOOT] AGA sprite test PASSED\n" : "[BOOT] AGA sprite test FAILED\n");

    kprint("[BOOT] Running AGA sprite border test...\n");
    int sprite_border_test = chip_emu_sprite_border_test();
    kprint(sprite_border_test ? "[BOOT] AGA sprite border test PASSED\n" : "[BOOT] AGA sprite border test FAILED\n");

    kprint("[BOOT] Running AGA sprite priority test...\n");
    int sprite_priority_test = chip_emu_sprite_priority_test();
    kprint(sprite_priority_test ? "[BOOT] AGA sprite priority test PASSED\n" : "[BOOT] AGA sprite priority test FAILED\n");

    kprint("[BOOT] Running AGA sprite superhires test...\n");
    int sprite_superhires_test = chip_emu_sprite_superhires_test();
    kprint(sprite_superhires_test ? "[BOOT] AGA sprite superhires test PASSED\n" : "[BOOT] AGA sprite superhires test FAILED\n");

    kprint("[BOOT] Running AGA sprite subpixel test...\n");
    int sprite_subpixel_test = chip_emu_sprite_subpixel_test();
    kprint(sprite_subpixel_test ? "[BOOT] AGA sprite subpixel test PASSED\n" : "[BOOT] AGA sprite subpixel test FAILED\n");

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
        chip_emu_set_keyboard_route(0); /* native shell keeps keyboard input */
    } else {
        kprint("[BOOT] M68k emulation bridge ready.\n");
        chip_emu_set_keyboard_route(1); /* route to CIA-A SDR for M68k input */
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

    kprint("[BOOT] Registering floppy block device...\n");
    if (FloppyBlockDev_Init() == 0) {
        kprint("[BOOT] Floppy block device DF0: registered.\n");
    } else {
        kprint("[BOOT] Floppy block device registration failed.\n");
    }

    kprint("[BOOT] Running floppy block-device test...\n");
    int floppy_blk_test = floppy_block_device_test();
    kprint(floppy_blk_test ? "[BOOT] Floppy block-device test PASSED\n" : "[BOOT] Floppy block-device test FAILED\n");

    kprint("[BOOT] Running parallel port test...\n");
    int parallel_test = chip_emu_parallel_test();
    kprint(parallel_test ? "[BOOT] Parallel port test PASSED\n" : "[BOOT] Parallel port test FAILED\n");

    kprint("[BOOT] Running serial port test...\n");
    int serial_test = chip_emu_serial_test();
    kprint(serial_test ? "[BOOT] Serial port test PASSED\n" : "[BOOT] Serial port test FAILED\n");

    kprint("[BOOT] Running CPU/chipset timing-lock test...\n");
    int timing_lock_test = chip_emu_timing_lock_test();
    kprint(timing_lock_test ? "[BOOT] Timing-lock test PASSED\n" : "[BOOT] Timing-lock test FAILED\n");

    kprint("[BOOT] Running chip RAM timing-contention test...\n");
    int timing_contention_test = chip_emu_timing_contention_test();
    kprint(timing_contention_test ? "[BOOT] Timing-contention test PASSED\n" : "[BOOT] Timing-contention test FAILED\n");

    kprint("[BOOT] Running horizontal-blanking timing test...\n");
    int hblank_test = chip_emu_hblank_test();
    kprint(hblank_test ? "[BOOT] H-blanking test PASSED\n" : "[BOOT] H-blanking test FAILED\n");

    kprint("[BOOT] Running Agnus slot table test...\n");
    int agnus_test = chip_emu_agnus_slot_test();
    kprint(agnus_test ? "[BOOT] Agnus slot table test PASSED\n" : "[BOOT] Agnus slot table test FAILED\n");

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
    /* Probe both fixed IDE channels (primary + secondary).  IDE_GetChannelCount()
     * returns the number of *populated* channels, which is NOT a valid index
     * bound — e.g. an empty primary + a CD on the secondary yields a count of 1
     * but the CD lives at channel index 1, so it would be skipped.  Iterate all
     * channels and rely on info->present (matching IDE_RegisterBlockDevs). */
    for (int ch = 0; ch < 2; ch++) {
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

    /* -------------------------------------------------------------------
     * Phase 1 — ROM Fallback Assigns (Kickstart-style pre-assign)
     *
     * On Amiga, the ROM hooks up SYS:, LIBS:, C:, DEVS:, L:, S:, FONTS:
     * etc. to the boot volume *before* any startup script runs.  UAOS
     * mirrors that here: as soon as the boot device is identified and
     * Workbench: is mounted, we create the hardcoded fallback assigns.
     * The Startup-Sequence may later override or extend them (e.g.
     * Assign LIBS: SYS:Classes ADD).
     * ------------------------------------------------------------------- */
    kprint("[BOOT] Setting up ROM fallback assigns...\n");
    VFS_SetupWorkbenchAssigns();

    /* -------------------------------------------------------------------
     * Phase 1b — Handler loader initialisation
     *
     * Scan L: for handler binaries and register built-in native handlers.
     * This must happen after Workbench: assigns are set up so L: resolves.
     * ------------------------------------------------------------------- */
    extern void HandlerLoader_Init(void);
    extern int HandlerLoader_ScanLDirectory(void);
    extern void DosList_Init(void);
    kprint("[BOOT] Initialising handler loader...\n");
    DosList_Init();
    HandlerLoader_Init();
    HandlerLoader_ScanLDirectory();

    kprint("[BOOT] Scanning LIBS: for loadable libraries...\n");
    UAOS_LoadableLib_Init();

    /* Note: Desktop is NOT drawn here - it starts via LoadWB from Startup-Sequence */

    /* Set up interrupts — IDT must be loaded before STI */
    kprint("[BOOT] Initialising IDT...\n");
    IDT_Init();
    kprint("[BOOT] Initialising TSS/GDT (user segments)...\n");
    GDT_InitTSS();
    kprint("[BOOT] Initialising PIC...\n");
    PIC_Init();
    /* PIT is programmed and unmasked later, after its handler is registered */

    /* Register the custom chip-window page fault handler (vector 14).
     * The generic stub is replaced so M68k accesses to 0x00B00000-0x00DFFFFF
     * are decoded and forwarded to the AGA/ECS emulator. */
    IDT_SetRawHandler(14, uaos_page_fault_isr);

    /* Register VirtIO interrupt handler (must be after IDT/PIC init) */
    kprint("[BOOT] Registering VirtIO IRQ...\n");
    virtio_blk_setup_irq();

    /* Program PIT at 100 Hz unconditionally — g_pit_ticks is used for all
     * kernel timing (network poll pacing, yield_ms, ntp guards) and must
     * tick regardless of whether a framebuffer is present. */
    kprint("[BOOT] Programming PIT (100 Hz)...\n");
    IDT_SetHandler(32, PIT_IRQHandler);
    {
        uint16_t divisor = (uint16_t)(1193180UL / 100UL);
        outb(0x43, 0x36);
        outb(0x40, (uint8_t)(divisor & 0xFF));
        outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
    }
    PIC_UnmaskIRQ(0);
    kprint("[BOOT] PIT active.\n");

    /* Initialise PS/2 mouse/keyboard and RTC only when a display is present */
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

        kprint("[BOOT] Initialising RTC clock...\n");
        IDT_SetHandler(40, RTC_IRQHandler);  /* IRQ8 = vector 40 */
        RTC_Init();
        PIC_UnmaskIRQ(8);
        Desktop_UpdateClock();               /* initial draw from CMOS */
        kprint("[BOOT] RTC active.\n");
    }

    /* Initialise local APIC so q35 forwards 8259A PIC interrupts */
    APIC_Init();

    kprint("[BOOT] Detecting vmmouse...\n");
    VMMouse_Init();
    if (VMMouse_Detect())
        kprint("[BOOT] vmmouse active (absolute mode).\n");
    else
        kprint("[BOOT] vmmouse not found, using PS/2 relative.\n");

    kprint("[BOOT] Enabling interrupts — starting scheduler.\n");

    /* Init scheduler BEFORE creating any tasks */
    TaskScheduler_Init();

    /* Wire INT 0x80 to the syscall table dispatcher before the scheduler
     * starts.  DPL=3 so ring-3 userspace can execute INT 0x80 without #GP.
     * The raw assembly entry passes the full interrupt frame to
     * Syscall_Dispatch(); legacy Wait() yields use SYSCALL_SCHEDULE (0xFF). */
    IDT_SetRawHandlerDPL3(0x80, uaos_syscall_isr);

    /* Create system idle task (runs the former event loop) */
    extern void Task_IdleEntry(void *arg);
    Task_CreateNative("Idle", -128, Task_IdleEntry, NULL);

    kprint("[BOOT] Initialising userspace GUI windows...\n");
    UserWindow_Init();

    kprint("[BOOT] Opening shell window...\n");
    ShellWin_Init();

    kprint("[BOOT] Running Startup-Sequence...\n");
    ShellWin_RunStartupSequence();

    /* Start the first task with interrupts off */
    __asm__ volatile ("cli");
    Task_StartFirst();

halt:
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

int chip_emu_disk_dma_test(void)
{
    uint32_t dst = 0x18000u;
    if (dst + 1024 > GUEST_RAM_SIZE) return 0;

    /* Ensure the virtual drive is spinning. */
    floppy_set_motor(1);

    /* Set disk DMA pointer. */
    chip_emu_write(0x020, (dst >> 16) & 0xFFFFu, 2); /* DSKPTH */
    chip_emu_write(0x022, dst & 0xFFFFu, 2); /* DSKPTL */
    chip_emu_write(0x07E, 0x4489, 2); /* DSKSYNC */

    /* Start read of 32 words (one sector) with DMAEN. */
    chip_emu_write(0x024, 0x8020, 2); /* DSKLEN = 32 words, read, DMAEN */

    /* Wait up to 50 ticks for the DMA to complete. */
    for (int i = 0; i < 50; i++) {
        floppy_tick();
        if ((g_intreq & 0x0002u) != 0) break;
    }

    if ((g_intreq & 0x0002u) == 0) return 0;

    /* Check that the sector data contains the test signature. */
    for (int i = 0; i < 16; i++) {
        if (g_ram[dst + i] != (uint8_t)"UAOS ADF TEST BOOT"[i]) return 0;
    }
    return 1;
}

int floppy_block_device_test(void)
{
    BlockDev *dev = BlockDev_Find("floppy0");
    if (!dev) return 0;

    uint8_t buf[512];
    if (BlockDev_Read(dev, 1, buf, 1) != 0) return 0;

    for (int i = 0; i < 256; i++) {
        if (buf[i * 2 + 0] != (uint8_t)i) return 0;
        if (buf[i * 2 + 1] != (uint8_t)(0xFF - i)) return 0;
    }
    return 1;
}
