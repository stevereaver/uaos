/* nv50bl.c — NVIDIA NV50+ panel backlight (SOR PWM)
 *
 * Drives the LVDS backlight on EFI-booted NVIDIA GPUs where the firmware
 * leaves no other control path (MBP4,1 / G84M 8600M GT — UAOS-139).
 * Register contract mirrors nouveau's nv50 backlight ops, verified on
 * metal via the mbp_nv50_bl Linux module:
 *
 *   BAR0 + 0x61c080 + i*0x800   PWM_DIV  — firmware-programmed divisor
 *   BAR0 + 0x61c084 + i*0x800   PWM_CTL  — bit31 NEW commits a new duty,
 *                                        bits 10:0 duty value 0..1025
 *
 * Duty scale is linear 0..1025 -> 0..100 %.  Without a parsed VBIOS DCB
 * we can't know which SOR owns LVDS, so (like mbp_nv50_bl) we scan
 * SORs 0..3 and take the first whose CTL reads nonzero — LVDS is SOR0
 * on the 8600M GT.
 *
 * Safety: only ever writes `NEW | duty` to CTL — never DIV or any other
 * BAR0 register (arbitrary BAR0 writes can glitch the GPU).  The probe
 * is read-only, so this is a no-op on non-NVIDIA hardware.
 */

#include "../klog/klog.h"
#include <stdint.h>

/* -------------------------------------------------------------------------
 * PCI config access (CF8/CFC) — same pattern as ahci.c / sky2.c
 * ------------------------------------------------------------------------- */
static inline void outl(uint16_t p, uint32_t v)
{
    __asm__ volatile("outl %0,%1" :: "a"(v), "Nd"(p));
}
static inline uint32_t inl(uint16_t p)
{
    uint32_t v;
    __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}
static inline void outw(uint16_t p, uint16_t v)
{
    __asm__ volatile("outw %0,%1" :: "a"(v), "Nd"(p));
}

#define PCI_ADDR 0xCF8
#define PCI_DATA 0xCFC

static uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg)
{
    outl(PCI_ADDR, (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)dev << 11)
         | ((uint32_t)fn << 8) | (reg & 0xFC));
    return inl(PCI_DATA);
}
static void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg,
                        uint32_t v)
{
    outl(PCI_ADDR, (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)dev << 11)
         | ((uint32_t)fn << 8) | (reg & 0xFC));
    outl(PCI_DATA, v);
}
static uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg)
{
    return (uint16_t)(pci_read32(bus, dev, fn, reg) >> ((reg & 2) * 8));
}
static void pci_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg,
                        uint16_t v)
{
    uint32_t shift = (reg & 2) * 8;
    uint32_t d = pci_read32(bus, dev, fn, reg);
    d = (d & ~(0xFFFFu << shift)) | ((uint32_t)v << shift);
    pci_write32(bus, dev, fn, reg, d);
}

/* -------------------------------------------------------------------------
 * NV50 SOR PWM registers (nouveau_reg.h names kept for grep-ability)
 * ------------------------------------------------------------------------- */
#define NV50_PDISP_SOR_PWM_DIV(i)   (0x0061C080u + (i) * 0x800u)
#define NV50_PDISP_SOR_PWM_CTL(i)   (0x0061C084u + (i) * 0x800u)
#define NV50_PDISP_SOR_PWM_CTL_NEW  0x80000000u
#define NV50_PDISP_SOR_PWM_CTL_VAL  0x000007FFu   /* NV50; NVA3+ is wider */
#define NV50_BL_DUTY_MAX            1025u
#define NV50_BL_MAX_SOR             4

static volatile uint8_t *g_bl_regs = 0;  /* GPU BAR0 */
static int                 g_bl_sor  = -1;

static inline uint32_t bl_r32(uint32_t r)
{
    return *(volatile uint32_t *)(g_bl_regs + r);
}
static inline void bl_w32(uint32_t r, uint32_t v)
{
    *(volatile uint32_t *)(g_bl_regs + r) = v;
}

int NV50BL_Present(void)
{
    return g_bl_regs != 0 && g_bl_sor >= 0;
}

/* Current brightness 0..100, or -1 when unavailable. */
int NV50BL_GetBrightness(void)
{
    if (!NV50BL_Present()) return -1;
    uint32_t duty = bl_r32(NV50_PDISP_SOR_PWM_CTL(g_bl_sor))
                    & NV50_PDISP_SOR_PWM_CTL_VAL;
    if (duty > NV50_BL_DUTY_MAX) duty = NV50_BL_DUTY_MAX;
    return (int)(duty * 100 / NV50_BL_DUTY_MAX);
}

/* Set brightness 0..100; returns the duty readback, or -1 when
 * unavailable. */
int NV50BL_SetBrightness(int pct)
{
    if (!NV50BL_Present()) return -1;
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    uint32_t duty = (uint32_t)pct * NV50_BL_DUTY_MAX / 100;
    bl_w32(NV50_PDISP_SOR_PWM_CTL(g_bl_sor),
           NV50_PDISP_SOR_PWM_CTL_NEW | duty);
    return (int)(bl_r32(NV50_PDISP_SOR_PWM_CTL(g_bl_sor))
                 & NV50_PDISP_SOR_PWM_CTL_VAL);
}

/* Find an NVIDIA display-class device, map BAR0, pick the SOR that owns
 * the panel PWM.  Returns 1 when a usable PWM was found. */
int NV50BL_Init(void)
{
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t fn = 0; fn < 8; fn++) {
                uint32_t id = pci_read32((uint8_t)bus, dev, fn, 0x00);
                if (id == 0xFFFFFFFF) { if (fn == 0) break; continue; }
                if ((id & 0xFFFF) != 0x10DE) continue;

                /* Display class only (base class 0x03) — the SOR PWM
                 * offsets are meaningless on other NVIDIA functions. */
                uint32_t classreg = pci_read32((uint8_t)bus, dev, fn, 0x08);
                if (((classreg >> 24) & 0xFF) != 0x03) continue;

                /* BAR0: 32- or 64-bit MMIO; must sit below 4G so the
                 * identity-mapped kernel can reach it directly. */
                uint32_t lo = pci_read32((uint8_t)bus, dev, fn, 0x10);
                if (lo & 1) continue;                    /* I/O BAR */
                uint64_t bar = lo & ~0xFULL;
                if ((lo & 0x6) == 0x4) {                 /* 64-bit BAR */
                    uint32_t hi = pci_read32((uint8_t)bus, dev, fn, 0x14);
                    bar |= (uint64_t)hi << 32;
                }
                if (!bar || bar >= 0x100000000ULL) continue;

                /* PCI command: enable memory space decoding. */
                uint16_t cmd = pci_read16((uint8_t)bus, dev, fn, 0x04);
                if (!(cmd & 0x2))
                    pci_write16((uint8_t)bus, dev, fn, 0x04,
                                (uint16_t)(cmd | 0x2));

                g_bl_regs = (volatile uint8_t *)(uintptr_t)bar;

                /* First SOR with a nonzero duty owns the panel. */
                for (int sor = 0; sor < NV50_BL_MAX_SOR; sor++) {
                    uint32_t ctl = bl_r32(NV50_PDISP_SOR_PWM_CTL(sor));
                    if (ctl & NV50_PDISP_SOR_PWM_CTL_VAL) {
                        g_bl_sor = sor;
                        KLOG(KLOG_DISP, KLOG_INFO,
                             "nv50bl: GPU %04x @%02x:%02x.%d bar0=0x%x "
                             "SOR%d duty=%u div=0x%x",
                             (unsigned)(id >> 16), bus, dev, fn,
                             (unsigned)bar, sor,
                             (unsigned)(ctl & NV50_PDISP_SOR_PWM_CTL_VAL),
                             (unsigned)bl_r32(NV50_PDISP_SOR_PWM_DIV(sor)));
                        return 1;
                    }
                }

                KLOG(KLOG_DISP, KLOG_INFO,
                     "nv50bl: GPU %04x @%02x:%02x.%d bar0=0x%x — "
                     "no live SOR PWM",
                     (unsigned)(id >> 16), bus, dev, fn, (unsigned)bar);
                g_bl_regs = 0;
            }
        }
    }
    return 0;
}
