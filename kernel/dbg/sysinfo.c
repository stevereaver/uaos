/* sysinfo.c — live hardware inventory (UAOS-278)
 *
 * showconfig/version originally printed QEMU-era string literals: VirtIO
 * VGA, 10 Hz PIT, PS/2 input, 8259A PIC and a fixed 512 MB of RAM — all
 * wrong on real hardware like the MacBookPro4,1.  Everything below is
 * built from probed state instead: the Multiboot2 memory map, CPUID,
 * PCI config space, the IRQ routing mode, the PIT rate constant, and
 * the BlockDev/netdev/UHCI driver registries.
 *
 * SysInfo_Init() snapshots the boot-time-only inputs (the mb2 info
 * pointer is not retained anywhere else); the dump itself reads live
 * driver state so later device registration is reflected too.
 */

#include "diag.h"
#include "../irq/irq.h"
#include "../irq/pit.h"
#include "../irq/acpi.h"
#include "../irq/ps2mouse.h"
#include "../display/framebuffer.h"
#include "../boot/mb2mod.h"
#include "../dos/blockdev.h"
#include "../drivers/usb.h"
#include "../drivers/cpufreq.h"
#include "../net/net_device.h"
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Boot-time snapshot
 * ------------------------------------------------------------------------- */

static uint64_t g_ram_bytes;

static int      g_disp_found;
static uint16_t g_disp_vid, g_disp_did;
static uint8_t  g_disp_bus, g_disp_dev, g_disp_fn;

/* First PCI function with class 0x03 (display).  IRQ_PciRead32 is a raw
 * CF8/CFC access, safe before IRQ_Init. */
static void scan_display_device(void)
{
    for (int bus = 0; bus < 256 && !g_disp_found; bus++) {
        for (int dev = 0; dev < 32 && !g_disp_found; dev++) {
            for (int fn = 0; fn < 8; fn++) {
                uint32_t id = IRQ_PciRead32((uint8_t)bus, (uint8_t)dev,
                                            (uint8_t)fn, 0x00);
                if ((id & 0xFFFF) == 0xFFFF || id == 0) {
                    if (!fn) break;
                    continue;
                }
                uint8_t htr = IRQ_PciRead8((uint8_t)bus, (uint8_t)dev,
                                           (uint8_t)fn, 0x0E);
                uint32_t cls = IRQ_PciRead32((uint8_t)bus, (uint8_t)dev,
                                             (uint8_t)fn, 0x08);
                if ((uint8_t)(cls >> 24) == 0x03) {
                    g_disp_found = 1;
                    g_disp_vid   = (uint16_t)(id & 0xFFFF);
                    g_disp_did   = (uint16_t)(id >> 16);
                    g_disp_bus   = (uint8_t)bus;
                    g_disp_dev   = (uint8_t)dev;
                    g_disp_fn    = (uint8_t)fn;
                    return;
                }
                if (fn == 0 && !(htr & 0x80)) break;
            }
        }
    }
}

void SysInfo_Init(uint32_t mb2_phys)
{
    g_ram_bytes = Mb2_TotalRAM(mb2_phys);
    scan_display_device();
}

uint64_t SysInfo_RamBytes(void) { return g_ram_bytes; }

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static inline void si_outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint8_t si_inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void si_cpuid(uint32_t leaf, uint32_t *a, uint32_t *b,
                            uint32_t *c, uint32_t *d)
{
    __asm__ volatile ("cpuid"
                      : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                      : "a"(leaf), "c"(0));
}

/* dl_hex prints lowercase 0x-prefixed; ShowConfig style wants $-prefixed
 * fixed-width fields. */
static void dl_hexw(DiagLine *l, uint64_t v, int digits)
{
    char h[17];
    dl_ch(l, '$');
    for (int i = digits - 1; i >= 0; i--) {
        h[i] = "0123456789ABCDEF"[v & 15];
        v >>= 4;
    }
    h[digits] = 0;
    dl_add(l, h);
}

/* "N.N meg" — bytes -> decimal megabytes with one fractional digit. */
static void dl_meg(DiagLine *l, uint64_t bytes)
{
    uint64_t t = (bytes * 10) / (1024 * 1024);
    dl_dec(l, t / 10);
    dl_ch(l, '.');
    dl_dec(l, t % 10);
    dl_add(l, " meg");
}

static const char *pci_vendor_name(uint16_t v)
{
    switch (v) {
    case 0x8086: return "Intel";
    case 0x10DE: return "NVIDIA";
    case 0x1002: return "ATI/AMD";
    case 0x1234: return "Bochs/QEMU";
    case 0x1AF4: return "VirtIO";
    case 0x15AD: return "VMware";
    case 0x102B: return "Matrox";
    case 0x5333: return "S3";
    default:     return "PCI";
    }
}

/* Append "<vendor> <vid>:<did>, PCI b:d.f" describing the display
 * adapter found at boot, or a fallback when no display-class device
 * enumerated (should not happen — the LFB always comes from one). */
static void dl_display_name(DiagLine *l)
{
    if (!g_disp_found) { dl_add(l, "bootloader framebuffer"); return; }
    dl_add(l, pci_vendor_name(g_disp_vid));
    dl_ch(l, ' ');
    char h[5];
    for (int i = 3; i >= 0; i--) h[3 - i] = "0123456789abcdef"[(g_disp_vid >> (i * 4)) & 15];
    h[4] = 0; dl_add(l, h); dl_ch(l, ':');
    for (int i = 3; i >= 0; i--) h[3 - i] = "0123456789abcdef"[(g_disp_did >> (i * 4)) & 15];
    h[4] = 0; dl_add(l, h);
    dl_add(l, " @ ");
    dl_dec(l, g_disp_bus); dl_ch(l, ':');
    dl_dec(l, g_disp_dev); dl_ch(l, '.');
    dl_dec(l, g_disp_fn);
}

/* -------------------------------------------------------------------------
 * CPU brand string (CPUID 0x80000002-4)
 * ------------------------------------------------------------------------- */

void SysInfo_CpuBrand(char *out, int max)
{
    out[0] = '\0';
    if (!out || max < 2) return;

    uint32_t a, b, c, d;
    si_cpuid(0x80000000u, &a, &b, &c, &d);
    if (a < 0x80000004u) return;

    uint32_t w[12];
    for (int i = 0; i < 3; i++) {
        si_cpuid(0x80000002u + (uint32_t)i,
                 &w[i * 4], &w[i * 4 + 1], &w[i * 4 + 2], &w[i * 4 + 3]);
    }

    /* Copy 48 chars, squeezing the brand string's padding runs down to
     * single spaces and stripping leading/trailing whitespace. */
    const char *raw = (const char *)w;
    int oi = 0, pending_space = 0, seen = 0;
    for (int i = 0; i < 48 && oi < max - 1; i++) {
        char ch = raw[i];
        if (!ch) break;
        if (ch == ' ') { if (seen) pending_space = 1; continue; }
        if (pending_space) { out[oi++] = ' '; pending_space = 0; }
        out[oi++] = ch;
        seen = 1;
    }
    out[oi] = '\0';
}

/* -------------------------------------------------------------------------
 * Input description — shared by C:version and the About window
 * ------------------------------------------------------------------------- */

void SysInfo_InputDesc(char *out, int max)
{
    if (!out || max < 2) return;
    out[0] = '\0';

    extern int BCM5974_Present(void);   /* drivers/bcm5974.c */
    int ps2 = PS2_Present();
    int nk  = USBHid_KbdCount();
    int nm  = USBHid_MouseCount();
    int nh  = USBHid_DeviceCount();   /* non-boot-protocol HIDs too */
    int tp  = BCM5974_Present();      /* Wellspring isn't HID proto */

    int oi = 0;
    #define SI_ADD(s) do { const char *_p = (s); \
        while (*_p && oi < max - 1) out[oi++] = *_p++; } while (0)

    if (ps2) SI_ADD("PS/2 keyboard + mouse (IRQ1/IRQ12)");
    if (nk || nm) {
        if (oi) SI_ADD(", ");
        SI_ADD("USB HID ");
        if (nk && nm) SI_ADD("keyboard + mouse");
        else if (nk)  SI_ADD("keyboard");
        else          SI_ADD("mouse");
    } else if (!ps2 && nh > 0) {
        SI_ADD("USB HID x");
        out[oi++] = (char)('0' + (nh < 9 ? nh : 9));
    }
    if (tp) SI_ADD(oi ? " + bcm5974 trackpad" : "bcm5974 trackpad");
    if (!oi) SI_ADD("none detected");
    out[oi] = '\0';
    #undef SI_ADD
}

/* -------------------------------------------------------------------------
 * UART presence — 16550 scratch-register probe (port 7).  A floating bus
 * reads 0xFF back regardless of what was written.
 * ------------------------------------------------------------------------- */
static int g_uart_present = -1;

static int uart_present(void)
{
    if (g_uart_present < 0) {
        si_outb(0x3F8 + 7, 0xA5); uint8_t a = si_inb(0x3F8 + 7);
        si_outb(0x3F8 + 7, 0x5A); uint8_t b = si_inb(0x3F8 + 7);
        g_uart_present = (a == 0xA5 && b == 0x5A);
    }
    return g_uart_present;
}

/* -------------------------------------------------------------------------
 * SysInfo_DumpConfig — ShowConfig-style hardware report
 * ------------------------------------------------------------------------- */

void SysInfo_DumpConfig(void *ctx, DiagEmitFn emit)
{
    DiagLine l;

    /* PROCESSOR */
    char brand[64];
    SysInfo_CpuBrand(brand, sizeof(brand));
    dl_reset(&l);
    dl_add(&l, "PROCESSOR:    CPU ");
    dl_add(&l, brand[0] ? brand : "x86_64");
    {
        CpuFreqInfo cf;
        CpuFreq_GetInfo(&cf);
        uint32_t top = 0;
        for (int i = 0; i < cf.n_states && i < CPUFREQ_MAX_STATES; i++)
            if (cf.state_mhz[i] > top) top = cf.state_mhz[i];
        if (top) {
            dl_add(&l, " (fam ");
            dl_dec(&l, (uint32_t)cf.cpu_family);
            dl_add(&l, " model ");
            dl_hex(&l, (uint32_t)cf.cpu_model);
            dl_add(&l, ", max ");
            dl_dec(&l, top);
            dl_add(&l, " MHz)");
        }
    }
    dl_emit(&l, ctx, emit);

    /* DISPLAY */
    dl_add(&l, "DISPLAY:      ");
    dl_dec(&l, g_fb.width);
    dl_ch(&l, 'x');
    dl_dec(&l, g_fb.height);
    dl_add(&l, ", ");
    dl_dec(&l, g_fb.bpp);
    dl_add(&l, "bpp linear framebuffer (");
    dl_display_name(&l);
    dl_ch(&l, ')');
    dl_emit(&l, ctx, emit);

    /* VERSION */
    dl_add(&l, "VERS:         UAOS v0.1.0-dev, Kernel build 1, Exec 1.0");
    dl_emit(&l, ctx, emit);

    /* RAM — installed total from the Multiboot2 map, plus the emulated
     * guest regions in AmigaDOS ShowConfig node style. */
    dl_add(&l, "RAM:          ");
    if (g_ram_bytes) dl_meg(&l, g_ram_bytes);
    else             dl_add(&l, "size unknown (no MB2 memory map)");
    dl_add(&l, " installed");
    dl_emit(&l, ctx, emit);
    dl_add(&l, "      Node type $A, Attributes $703 (CHIP), at ");
    dl_hexw(&l, 0x00000000, 8); dl_ch(&l, '-'); dl_hexw(&l, 0x007FFFFF, 8);
    dl_add(&l, " (8.0 meg, M68k guest)");
    dl_emit(&l, ctx, emit);
    dl_add(&l, "      Node type $A, Attributes $005 (FAST), at ");
    dl_hexw(&l, 0x00800000, 8); dl_ch(&l, '-'); dl_hexw(&l, 0x00FFFFFF, 8);
    dl_add(&l, " (8.0 meg, M68k guest)");
    dl_emit(&l, ctx, emit);

    /* BOARDS */
    dl_add(&l, "BOARDS:");
    dl_emit(&l, ctx, emit);

    dl_add(&l, "  Board (display adapter):     ");
    dl_display_name(&l);
    dl_emit(&l, ctx, emit);

    dl_add(&l, "  Board (PS/2 controller):     ");
    if (PS2_Present())
        dl_add(&l, "IRQ1 keyboard, IRQ12 mouse");
    else
        dl_add(&l, "not present (USB HID input)");
    dl_emit(&l, ctx, emit);

    dl_add(&l, "  Board (interrupts):          ");
    if (IRQ_Mode() == IRQ_MODE_IOAPIC && !IRQ_PicFallbackActive()) {
        dl_add(&l, "IO-APIC, LAPIC ");
        dl_hexw(&l, ACPI_LapicBase(), 8);
    } else if (IRQ_PicFallbackActive()) {
        dl_add(&l, "8259A PIC (IO-APIC fallback)");
    } else {
        dl_add(&l, "8259A PIC");
    }
    dl_emit(&l, ctx, emit);

    dl_add(&l, "  Board (PIT timer):           8253/8254, ");
    dl_dec(&l, UAOS_PIT_HZ);
    dl_add(&l, " Hz, IRQ0");
    if (IRQ_Mode() == IRQ_MODE_IOAPIC) {
        int gsi = ACPI_IsaToGsi(0, NULL);
        if (gsi > 0) {
            dl_add(&l, "->GSI");
            dl_dec(&l, (uint32_t)gsi);
        }
    }
    dl_emit(&l, ctx, emit);

    dl_add(&l, "  Board (UART):                ");
    if (uart_present())
        dl_add(&l, "16550A COM1 $3F8, IRQ4");
    else
        dl_add(&l, "not present");
    dl_emit(&l, ctx, emit);

    dl_add(&l, "  Board (RTC):                 MC146818 CMOS RTC, IRQ8");
    dl_emit(&l, ctx, emit);

    int nhc = UHCI_DiagCount();
    if (nhc > 0) {
        dl_add(&l, "  Board (USB):                 UHCI host controller x");
        dl_dec(&l, (uint32_t)nhc);
        dl_add(&l, ", ");
        dl_dec(&l, (uint32_t)USB_DeviceCount());
        dl_add(&l, " device(s)");
        dl_emit(&l, ctx, emit);
    }

    if (netdev_is_up()) {
        dl_add(&l, "  Board (network):             ");
        dl_add(&l, netdev_name());
        dl_emit(&l, ctx, emit);
    }

    for (BlockDev *bd = BlockDev_GetList(); bd; bd = bd->next) {
        dl_add(&l, "  Board (disk ");
        dl_add(&l, bd->name ? bd->name : "?");
        dl_add(&l, "): ");
        dl_pad(&l, 32);
        uint64_t bytes = bd->num_sectors * (uint64_t)bd->sector_size;
        if (bytes >= 1024 * 1024) {
            dl_dec(&l, bytes / (1024 * 1024));
            dl_add(&l, " MB");
        } else {
            dl_dec(&l, bytes / 1024);
            dl_add(&l, " KB");
        }
        dl_emit(&l, ctx, emit);
    }
}
