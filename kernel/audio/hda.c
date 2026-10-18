/*
 * hda.c — UAOS Intel High Definition Audio (HDA / "Azalia") output backend
 *
 * PCI-based driver for class 0x04/0x03 controllers (ICH6/ICH8/ICH9 HDA,
 * QEMU intel-hda/ich9-intel-hda).  Presents itself as an AudioBackend:
 * the mixer's 48 kHz stereo ring is streamed through a 4-entry BDL on the
 * first output stream descriptor, refilled from the 100 Hz tick via LPIB
 * polling — no IRQs, same pattern as ac97.c.
 *
 * Codec path setup does a bounded DFS from each usable output pin complex
 * through mixers/selectors to a DAC, then powers/unmutes/enables every hop
 * and starts one stream on the (shared) DAC.
 *
 * Apple MacBookPro4,1 quirk implemented: Linux ALC889_FIXUP_MBP_VREF
 * (chained ALC882_FIXUP_GPIO1) — GPIO1 driven high on the AFG, and
 * VREF_80 set on the headphone-output pin among nids 0x14/0x15/0x19.
 * See sound/pci/hda/patch_realtek.c alc882_fixup_tbl[] /
 * alc889_fixup_mbp_vref().
 */

#include "audio/audio.h"
#include "../boot/kprint.h"
#include "../klog/klog.h"
#include <stdint.h>

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

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


static uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg)
{
    outl(PCI_CONFIG_ADDR, 0x80000000u | ((uint32_t)bus << 16) |
         ((uint32_t)dev << 11) | ((uint32_t)fn << 8) | (reg & 0xFCu));
    return inl(PCI_CONFIG_DATA);
}
static void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg,
                        uint32_t val)
{
    outl(PCI_CONFIG_ADDR, 0x80000000u | ((uint32_t)bus << 16) |
         ((uint32_t)dev << 11) | ((uint32_t)fn << 8) | (reg & 0xFCu));
    outl(PCI_CONFIG_DATA, val);
}

/* -------------------------------------------------------------------------
 * Timing — hda_backend_init runs before the scheduler/PIT exist, same as
 * AHCI.  Calibrate the TSC once against PIT channel 2 (50 ms one-shot) so
 * every hardware wait below is bounded in wall-clock time.
 * ------------------------------------------------------------------------- */

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t g_tsc_khz = 0;   /* TSC ticks per ms, 0 = uncalibrated */

static void calibrate_tsc(void)
{
    if (g_tsc_khz) return;

    uint8_t old;
    __asm__ volatile("inb $0x61,%0" : "=a"(old));
    __asm__ volatile("outb %0,$0x61" :: "a"((uint8_t)((old & ~0x02) | 0x01)));
    __asm__ volatile("outb %0,$0x43" :: "a"((uint8_t)0xB0));
    __asm__ volatile("outb %0,$0x42" :: "a"((uint8_t)(59659 & 0xFF)));
    __asm__ volatile("outb %0,$0x42" :: "a"((uint8_t)(59659 >> 8)));
    __asm__ volatile("outb %0,$0x61" :: "a"((uint8_t)((old & ~0x02) | 0x01)));

    uint64_t t0 = rdtsc();
    uint8_t s = 0;
    /* Bounded: if the i8254 never toggles (no PIT), bail out and let the
     * port-0x80 fallback in us_spin() carry the timing. */
    while (!(s & 0x20)) {
        __asm__ volatile("inb $0x61,%0" : "=a"(s));
        if (rdtsc() - t0 > 200000000ull) {
            __asm__ volatile("outb %0,$0x61" :: "a"(old));
            return;                         /* g_tsc_khz stays 0 */
        }
    }
    uint64_t t1 = rdtsc();

    __asm__ volatile("outb %0,$0x61" :: "a"(old));

    g_tsc_khz = ((t1 - t0) * 20) / 1000;
    if (!g_tsc_khz) g_tsc_khz = 1;
}

/* Bounded busy-wait; ~ms/us scale.  Port-0x80 fallback if TSC is dead. */
static void us_spin(uint32_t us)
{
    if (!g_tsc_khz) {
        for (uint32_t i = 0; i < us; i++)
            __asm__ volatile ("inb $0x80, %%al" ::: "eax");
        return;
    }
    uint64_t tpu = g_tsc_khz / 1000;
    if (!tpu) tpu = 1;
    uint64_t end = rdtsc() + tpu * us;
    while (rdtsc() < end) __asm__ volatile ("pause");
}
static void ms_spin(uint32_t ms) { us_spin(ms * 1000); }

/* -------------------------------------------------------------------------
 * HDA controller registers (offsets into BAR0)
 * ------------------------------------------------------------------------- */

#define REG_GCAP      0x00   /* u16 */
#define REG_GCTL      0x08   /* u32, bit0 = CRST */
#define REG_STATESTS  0x0E   /* u16, codec present bits, write-clear */
#define REG_CORBLBASE 0x40   /* u32 */
#define REG_CORBUBASE 0x44   /* u32 */
#define REG_CORBWP    0x48   /* u16 */
#define REG_CORBRP    0x4A   /* u16, bit15 = reset */
#define REG_CORBCTL   0x4C   /* u8,  bit1 = DMA run */
#define REG_CORBSIZE  0x4E   /* u8,  bits1:0 sel, bits7:4 capability */
#define REG_RIRBLBASE 0x50   /* u32 */
#define REG_RIRBUBASE 0x54   /* u32 */
#define REG_RIRBWP    0x58   /* u16, bit15 = write-ptr reset */
#define REG_RINTCNT   0x5A   /* u16 */
#define REG_RIRBCTL   0x5C   /* u8,  bit0 = IRQ en, bit1 = DMA run */
#define REG_RIRBSTS   0x5D   /* u8,  W1C: bit0 RINTFL, bit2 OIS */
#define REG_RIRBSIZE  0x5E   /* u8 */
#define REG_ICOI      0x60   /* u32 immediate command out */
#define REG_IRI       0x64   /* u32 immediate response in */
#define REG_ICS       0x68   /* u16, bit0 = ICB, bit1 = IRV */

/* Stream descriptor block: 0x80 + index*0x20 */
#define SD_BASE   0x80
#define SD_STRIDE 0x20
#define SD_CTL    0x00   /* u32: bit0 SRST, bit1 RUN, bits23:20 tag */
#define SD_STS    0x03   /* u8  */
#define SD_LPIB   0x04   /* u32 */
#define SD_CBL    0x08   /* u32 */
#define SD_LVI    0x0C   /* u16 */
#define SD_FMT    0x12   /* u16 */
#define SD_BDPL   0x18   /* u32 */
#define SD_BDPU   0x1C   /* u32 */

static volatile uint8_t *g_mmio = 0;

static inline uint8_t  rb(uint32_t o) { return *(volatile uint8_t  *)(g_mmio + o); }
static inline uint16_t rw(uint32_t o) { return *(volatile uint16_t *)(g_mmio + o); }
static inline uint32_t rl(uint32_t o) { return *(volatile uint32_t *)(g_mmio + o); }
static inline void wb(uint32_t o, uint8_t  v) { *(volatile uint8_t  *)(g_mmio + o) = v; }
static inline void ww(uint32_t o, uint16_t v) { *(volatile uint16_t *)(g_mmio + o) = v; }
static inline void wl(uint32_t o, uint32_t v) { *(volatile uint32_t *)(g_mmio + o) = v; }

/* -------------------------------------------------------------------------
 * DMA buffers — static, physically contiguous, identity-mapped (<4 GB).
 * ------------------------------------------------------------------------- */

#define CORB_ENTRIES  256
#define RIRB_ENTRIES  256
#define HDA_BDL_COUNT 4
#define HDA_SEG_FRAMES 1024                    /* ~21 ms at 48 kHz */
#define HDA_BUF_FRAMES (HDA_BDL_COUNT * HDA_SEG_FRAMES)
#define HDA_BUF_BYTES  (HDA_BUF_FRAMES * 4)    /* s16 stereo */

static uint32_t g_corb[CORB_ENTRIES] __attribute__((aligned(128)));
static uint64_t g_rirb[RIRB_ENTRIES] __attribute__((aligned(128)));
static uint8_t  g_bdl[HDA_BDL_COUNT * 16]  __attribute__((aligned(128)));
static int16_t  g_pcm[HDA_BUF_FRAMES * 2]  __attribute__((aligned(128)));

static uint32_t host_phys(void *p) { return (uint32_t)(uintptr_t)p; }

static uint8_t  g_cad = 0;            /* codec address in use */
static uint8_t  g_afg = 0;            /* audio function group nid */
static int      g_dac = -1;           /* converter nid feeding the stream */
static uint16_t g_rirb_rp = 0;        /* our RIRB read pointer */
static int      g_corb_ok = 0;        /* CORB/RIRB engine usable */
static int      g_sd_index = -1;      /* output stream descriptor index */
static int      g_next_seg = 0;       /* next BDL segment to refill */
static int      g_ready = 0;

/* -------------------------------------------------------------------------
 * Verb transport
 * ------------------------------------------------------------------------- */

/* Poll the Immediate Command interface.  Returns response or -1. */
static int hda_verb_immediate(uint32_t cmd, uint32_t *resp)
{
    /* Wait for the interface to go idle (ICB/IRV clear), bounded. */
    uint64_t end = rdtsc() + g_tsc_khz * 10;
    while (rw(REG_ICS) & 0x3) {
        if (rdtsc() > end) return -1;
        __asm__ volatile ("pause");
    }
    wl(REG_ICOI, cmd);
    ww(REG_ICS, 0x1);                        /* ICB: issue */

    end = rdtsc() + g_tsc_khz * 10;          /* 10 ms bound */
    while (!(rw(REG_ICS) & 0x2)) {
        if (rdtsc() > end) return -1;
        __asm__ volatile ("pause");
    }
    *resp = rl(REG_IRI);
    ww(REG_ICS, 0x2);                        /* clear IRV */
    return 0;
}

static int hda_verb_corb(uint32_t cmd, uint32_t *resp)
{
    uint16_t wp = (uint16_t)(rw(REG_CORBWP) + 1) & (CORB_ENTRIES - 1);
    g_corb[wp] = cmd;
    __asm__ volatile ("sfence" ::: "memory");
    ww(REG_CORBWP, wp);

    uint64_t end = rdtsc() + g_tsc_khz * 10;
    while (rw(REG_RIRBWP) == g_rirb_rp) {
        if (rdtsc() > end) return -1;
        __asm__ volatile ("pause");
    }
    __asm__ volatile ("lfence" ::: "memory");
    g_rirb_rp = (uint16_t)(g_rirb_rp + 1) & (RIRB_ENTRIES - 1);
    *resp = (uint32_t)(g_rirb[g_rirb_rp] & 0xFFFFFFFFull);
    /* ACK the response-interrupt status so the controller keeps the CORB
     * engine running (RINTCNT-gated on some models, e.g. QEMU stalls once
     * rirb_count reaches RINTCNT until RIRBSTS is cleared).  INTCTL is
     * never enabled, so this raises no interrupt. */
    wb(REG_RIRBSTS, 0x05);
    return 0;
}

/* Issue one codec verb.  Returns response, or -1 on timeout. */
static int hda_verb(int cad, int nid, uint32_t verb, uint32_t parm,
                    uint32_t *resp)
{
    uint32_t cmd = ((uint32_t)cad << 28) | ((uint32_t)nid << 20)
                 | (verb << 8) | parm;
    uint32_t r = 0xFFFFFFFFu;
    int rc;
    if (g_corb_ok) {
        rc = hda_verb_corb(cmd, &r);
        if (rc == 0) goto out;
        /* CORB wedged — drop to the immediate interface for good. */
        KLOG(KLOG_AUDIO, KLOG_DEBUG,
             "hda: CORB timeout on nid %x verb %x, switching to ICOI\n",
             (uint32_t)nid, verb);
        g_corb_ok = 0;
    }
    rc = hda_verb_immediate(cmd, &r);
out:
    if (resp) *resp = r;
    return rc;
}

static uint32_t hda_get_param(int nid, uint32_t param)
{
    uint32_t r = 0;
    if (hda_verb(g_cad, nid, 0xF00 /*GET_PARAMETER*/, param, &r) != 0)
        return 0;
    return r;
}
static void hda_set(int nid, uint32_t verb, uint32_t parm)
{
    uint32_t r;
    hda_verb(g_cad, nid, verb, parm, &r);
}
/* Config default is its own verb (0xF1C), not a GET_PARAMETER index —
 * real codecs don't answer parameter 0x1C. */
static uint32_t hda_get_defcfg(int nid)
{
    uint32_t r = 0;
    hda_verb(g_cad, nid, 0xF1C /*GET_CONFIG_DEFAULT*/, 0, &r);
    return r;
}

/* -------------------------------------------------------------------------
 * Codec/widget walk
 * ------------------------------------------------------------------------- */

#define WIDGET_TYPE(caps)   (((caps) >> 20) & 0xF)
#define WCAP_DAC      0x0
#define WCAP_ADC      0x1
#define WCAP_MIXER    0x2
#define WCAP_SELECTOR 0x3
#define WCAP_PIN      0x4

/* Pin default-config fields */
#define DEFCFG_CONN(v)  (((v) >> 30) & 0x3)   /* 1 = no physical connection */
#define DEFCFG_DEV(v)   (((v) >> 20) & 0xF)   /* 0 LO,1 Spk,2 HP,... */

static uint8_t hda_connlist(int nid, uint8_t *out, int max)
{
    uint32_t cl = hda_get_param(nid, 0x0E);
    int n = (int)(cl & 0x7F);
    int longform = cl & 0x80;
    if (n <= 0) return 0;
    if (n > max) n = max;

    int got = 0;
    if (longform) {
        for (int i = 0; i < n; i += 2) {
            uint32_t r;
            if (hda_verb(g_cad, nid, 0xF02, i, &r) != 0) break;
            for (int k = 0; k < 2 && got < n; k++) {
                uint32_t e = (r >> (k * 16)) & 0xFFFF;
                out[got++] = (uint8_t)(e & 0x7F);
                /* range flag (bit15) — next entry is a range end; expand */
                if ((e & 0x8000) && got < n)
                    out[got - 1] = (uint8_t)(e & 0x7F); /* keep start only */
            }
        }
    } else {
        for (int i = 0; i < n; i += 4) {
            uint32_t r;
            if (hda_verb(g_cad, nid, 0xF02, i, &r) != 0) break;
            for (int k = 0; k < 4 && got < n; k++) {
                uint32_t e = (r >> (k * 8)) & 0xFF;
                out[got++] = (uint8_t)(e & 0x7F);
            }
        }
    }
    return (uint8_t)got;
}

struct hda_path {
    uint8_t nids[6];   /* pin, ..., dac */
    uint8_t idx[6];    /* connection index used at each hop */
    int     n;
};

/* DFS from `nid` looking for an Audio Output converter (DAC).  On success
 * fills *path (deepest-first: nids[0] is `nid`).  Returns path length. */
static int hda_find_dac(int nid, int depth, struct hda_path *path)
{
    if (depth >= 6) return -1;
    uint32_t wcaps = hda_get_param(nid, 0x09);
    int type = WIDGET_TYPE(wcaps);
    if (type == WCAP_DAC) {
        path->nids[depth] = (uint8_t)nid;
        path->idx[depth] = 0;
        path->n = depth + 1;
        return depth + 1;
    }
    if (!(wcaps & (1u << 8))) return -1;      /* no connection list */

    uint8_t conns[16];
    int nc = hda_connlist(nid, conns, 16);
    for (int i = 0; i < nc; i++) {
        /* Skip our own parent to avoid trivial back-cycles; also skip
         * nodes already in the path. */
        int dup = 0;
        for (int d = 0; d < depth; d++)
            if (path->nids[d] == conns[i]) dup = 1;
        if (dup) continue;
        path->nids[depth] = (uint8_t)nid;
        path->idx[depth] = (uint8_t)i;
        if (hda_find_dac(conns[i], depth + 1, path) > 0)
            return 1;
    }
    return -1;
}

/* 0 dB gain position from amp caps (offset field, clamped to step count). */
static uint32_t amp_0db_gain(uint32_t caps)
{
    uint32_t steps  = caps & 0x7F;
    uint32_t offset = (caps >> 16) & 0x7F;
    return offset <= steps ? offset : steps;
}

static void hda_unmute_out(int nid, uint32_t wcaps)
{
    if (!(wcaps & (1u << 2))) return;         /* no output amp */
    uint32_t caps = (wcaps & (1u << 3)) ? hda_get_param(nid, 0x12) : 0;
    uint32_t gain = amp_0db_gain(caps);
    /* set out amp, both channels, index 0, unmuted */
    hda_set(nid, 0x300 /*SET_AMP_GAIN_MUTE*/, 0xB000 | gain);
}

static void hda_unmute_in(int nid, uint32_t wcaps, int idx)
{
    if (!(wcaps & (1u << 1))) return;         /* no input amp */
    uint32_t caps = (wcaps & (1u << 3)) ? hda_get_param(nid, 0x0D) : 0;
    uint32_t gain = amp_0db_gain(caps);
    hda_set(nid, 0x300, 0x7000 | ((uint32_t)idx << 8) | gain);
}

/* Configured pin→DAC paths + the widget that carries the master volume in
 * each (DAC preferred so attenuation doesn't compound across hops).
 * Kept for runtime set_volume and for C:hdadump diagnostics. */
#define HDA_MAX_PATHS 8
static struct {
    struct hda_path p;
    uint8_t  vol_nid;      /* widget with the controlled out-amp, 0 = none */
    uint32_t vol_caps;     /* its output-amp capabilities (param 0x12) */
} g_out[HDA_MAX_PATHS];
static int g_nout = 0;

static void hda_vol_register(const struct hda_path *p)
{
    if (g_nout >= HDA_MAX_PATHS) return;
    g_out[g_nout].p = *p;
    g_out[g_nout].vol_nid = 0;
    g_out[g_nout].vol_caps = 0;
    for (int i = p->n - 1; i >= 0; i--) {
        int nid = p->nids[i];
        uint32_t wcaps = hda_get_param(nid, 0x09);
        if (!(wcaps & (1u << 2))) continue;          /* no output amp */
        uint32_t caps = (wcaps & (1u << 3)) ? hda_get_param(nid, 0x12) : 0;
        if (!(caps & 0x7F)) continue;               /* no steps to attenuate */
        g_out[g_nout].vol_nid = (uint8_t)nid;
        g_out[g_nout].vol_caps = caps;
        break;
    }
    g_nout++;
}

/* Master volume → codec out-amps.  Gain is linear in the widget's step
 * range (offset counts as steps down from max); mute uses the amp mute bit. */
static void hda_backend_set_volume(int pct, int muted)
{
    if (!g_ready) return;
    for (int i = 0; i < g_nout; i++) {
        if (!g_out[i].vol_nid) continue;
        uint32_t steps = g_out[i].vol_caps & 0x7F;
        uint32_t gain = muted ? 0 : (steps * (uint32_t)pct) / 100;
        uint32_t pay = 0xB000 | gain;               /* out amp, L+R, idx 0 */
        if (muted || pct == 0) pay |= 0x80;         /* SET_AMP mute bit */
        hda_set(g_out[i].vol_nid, 0x300, pay);
    }
}

/* Apply the Apple fixup: ALC889_FIXUP_MBP_VREF (GPIO1 + VREF_80 on the HP
 * pin among 0x14/0x15/0x19).  Also used for the other Apple ALC885/889
 * subsystems that share the MBP_VREF fixup in Linux's quirk table. */
static int g_apple_quirk = 0;

static void hda_apple_fixup(void)
{
    /* ALC882_FIXUP_GPIO1: mask=dir=data=1 on the AFG */
    hda_set(g_afg, 0x716 /*SET_GPIO_MASK*/, 0x01);
    hda_set(g_afg, 0x717 /*SET_GPIO_DIRECTION*/, 0x01);
    ms_spin(1);                 /* alc_write_gpio: macpro needs a delay */
    hda_set(g_afg, 0x715 /*SET_GPIO_DATA*/, 0x01);
    g_apple_quirk = 1;
    KLOG(KLOG_AUDIO, KLOG_DEBUG, "hda: Apple SSID -- GPIO1 fixup applied\n");
}

/* Configure one pin→DAC path: power, connselects, amps, pin ctl, EAPD. */
static void hda_enable_path(const struct hda_path *p)
{
    int pin = p->nids[0];
    int pin_is_hp = (DEFCFG_DEV(hda_get_defcfg(pin)) == 2);

    for (int i = 0; i < p->n; i++) {
        int nid = p->nids[i];
        uint32_t wcaps = hda_get_param(nid, 0x09);
        int type = WIDGET_TYPE(wcaps);

        hda_set(nid, 0x705 /*SET_POWER_STATE*/, 0);   /* D0 */
        /* Any non-mixer widget with >1 connection honours CONN_SEL —
         * this covers selectors and multi-source pin complexes. */
        if (type != WCAP_MIXER && i + 1 < p->n)
            hda_set(nid, 0x701 /*SET_CONNSEL*/, p->idx[i]);
        if (type == WCAP_MIXER || type == WCAP_DAC)
            hda_unmute_in(nid, wcaps, p->idx[i]);
        hda_unmute_out(nid, wcaps);
    }

    /* Pin widget control: OUT enable; HP head for headphone jacks. */
    uint32_t ctl = 0x40;
    if (pin_is_hp) ctl |= 0x80;
    /* MBP_VREF: Apple codecs need VREF_80 on the HP output pin
     * (nids 0x14/0x15/0x19 per alc889_fixup_mbp_vref). */
    if (g_apple_quirk && pin_is_hp &&
        (pin == 0x14 || pin == 0x15 || pin == 0x19))
        ctl |= 0x04;            /* AC_PINCTL_VREF_80 */
    hda_set(pin, 0x707 /*SET_PIN_WIDGET_CONTROL*/, ctl);

    /* EAPD when the pin advertises it (external amp power). */
    uint32_t pcaps = hda_get_param(pin, 0x0C);
    if (pcaps & (1u << 16)) {
        uint32_t e;
        if (hda_verb(g_cad, pin, 0xF0C /*GET_EAPD*/, 0, &e) == 0)
            hda_set(pin, 0x70C /*SET_EAPD*/, e | 0x02);
    }
}

/* -------------------------------------------------------------------------
 * Init
 * ------------------------------------------------------------------------- */

static int hda_find_pci(uint8_t *bus, uint8_t *dev, uint8_t *fn)
{
    for (uint16_t b = 0; b < 256; b++) {
        for (uint8_t d = 0; d < 32; d++) {
            uint32_t id = pci_read32((uint8_t)b, d, 0, 0);
            if (id == 0xFFFFFFFFu) continue;
            uint32_t cls = pci_read32((uint8_t)b, d, 0, 8);
            if ((cls >> 24) == 0x04 && ((cls >> 16) & 0xFF) == 0x03) {
                *bus = (uint8_t)b; *dev = d; *fn = 0;
                return 1;
            }
        }
    }
    return 0;
}

static int hda_controller_reset(void)
{
    wl(REG_GCTL, rl(REG_GCTL) & ~1u);
    uint64_t end = rdtsc() + g_tsc_khz * 10;
    while (rl(REG_GCTL) & 1) {
        if (rdtsc() > end) return -1;
        __asm__ volatile ("pause");
    }
    us_spin(200);
    wl(REG_GCTL, rl(REG_GCTL) | 1);
    end = rdtsc() + g_tsc_khz * 10;
    while (!(rl(REG_GCTL) & 1)) {
        if (rdtsc() > end) return -1;
        __asm__ volatile ("pause");
    }
    us_spin(600);               /* spec: codecs need >= 521 us after CRST */
    return 0;
}

static int hda_corb_rirb_init(void)
{
    /* Stop both DMA engines. */
    wb(REG_CORBCTL, 0);
    wb(REG_RIRBCTL, 0);
    us_spin(100);

    /* CORB: 256 entries if supported. */
    uint8_t szcap = rb(REG_CORBSIZE) >> 4;
    wb(REG_CORBSIZE, (szcap & 0x4) ? 0x02 : (szcap & 0x2) ? 0x01 : 0x00);
    wl(REG_CORBLBASE, host_phys(g_corb));
    wl(REG_CORBUBASE, 0);

    /* CORBRP reset: set bit15, wait, clear, wait. */
    ww(REG_CORBRP, 0x8000);
    uint64_t end = rdtsc() + g_tsc_khz * 10;
    while (!(rw(REG_CORBRP) & 0x8000)) {
        if (rdtsc() > end) return -1;
        __asm__ volatile ("pause");
    }
    ww(REG_CORBRP, 0);
    end = rdtsc() + g_tsc_khz * 10;
    while (rw(REG_CORBRP) & 0x8000) {
        if (rdtsc() > end) return -1;
        __asm__ volatile ("pause");
    }
    ww(REG_CORBWP, 0);

    /* RIRB: 256 entries, WP reset, run. */
    szcap = rb(REG_RIRBSIZE) >> 4;
    wb(REG_RIRBSIZE, (szcap & 0x4) ? 0x02 : (szcap & 0x2) ? 0x01 : 0x00);
    wl(REG_RIRBLBASE, host_phys(g_rirb));
    wl(REG_RIRBUBASE, 0);
    ww(REG_RIRBWP, 0x8000);
    g_rirb_rp = 0;
    ww(REG_RINTCNT, 1);
    wb(REG_RIRBCTL, 0x03);      /* RIRB DMA run + resp-IRQ status (per-verb ACK) */

    /* CORB run last. */
    wb(REG_CORBCTL, 0x02);
    us_spin(100);

    /* Sanity: issue GET vendor-id on codec 0; a real response via RIRB
     * proves the engine works end to end. */
    uint32_t r;
    if (hda_verb_corb((0 << 28) | (0xF00 << 8) | 0, &r) != 0)
        return -1;
    (void)r;
    return 0;
}

/* Build the 4-entry BDL over g_pcm and start the output stream. */
static int hda_stream_start(uint16_t gcap)
{
    int n_iss = (gcap >> 8) & 0xF;
    int n_oss = (gcap >> 12) & 0xF;
    if (!n_oss) {
        kprint("[HDA] no output streams\n");
        return -1;
    }
    g_sd_index = n_iss;         /* input SDs first, then output */
    uint32_t sd = SD_BASE + g_sd_index * SD_STRIDE;

    /* Reset the stream descriptor. */
    wl(sd + SD_CTL, rl(sd + SD_CTL) | 1);
    uint64_t end = rdtsc() + g_tsc_khz * 10;
    while (!(rl(sd + SD_CTL) & 1)) {
        if (rdtsc() > end) return -1;
        __asm__ volatile ("pause");
    }
    wl(sd + SD_CTL, rl(sd + SD_CTL) & ~1u);
    end = rdtsc() + g_tsc_khz * 10;
    while (rl(sd + SD_CTL) & 1) {
        if (rdtsc() > end) return -1;
        __asm__ volatile ("pause");
    }

    /* BDL: 4 segments of HDA_SEG_FRAMES frames. */
    uint32_t base = host_phys(g_pcm);
    for (int i = 0; i < HDA_BDL_COUNT; i++) {
        uint8_t *e = g_bdl + i * 16;
        *(uint32_t *)(e + 0)  = base + i * HDA_SEG_FRAMES * 4;
        *(uint32_t *)(e + 4)  = 0;                    /* addr hi */
        *(uint32_t *)(e + 8)  = HDA_SEG_FRAMES * 4;   /* bytes */
        *(uint32_t *)(e + 12) = 0;                    /* no IOC */
    }
    wl(sd + SD_BDPL, host_phys(g_bdl));
    wl(sd + SD_BDPU, 0);
    wl(sd + SD_CBL, HDA_BUF_BYTES);
    ww(sd + SD_LVI, HDA_BDL_COUNT - 1);
    ww(sd + SD_FMT, 0x0011);    /* 48 kHz, 16-bit, 2ch */

    /* Converter: 48 kHz format, stream tag 1, channel 0. */
    hda_set(g_dac, 0x200 /*SET_CONVERTER_FORMAT*/, 0x0011);
    hda_set(g_dac, 0x706 /*SET_CHANNEL_STREAMID*/, 0x10);

    /* Prefill all four segments, then run. */
    for (int i = 0; i < HDA_BDL_COUNT; i++) {
        int16_t *dst = g_pcm + i * HDA_SEG_FRAMES * 2;
        unsigned int got = audio_ring_read(dst, HDA_SEG_FRAMES);
        for (unsigned int f = got; f < HDA_SEG_FRAMES; f++) {
            dst[f * 2] = 0; dst[f * 2 + 1] = 0;
        }
    }
    __asm__ volatile ("sfence" ::: "memory");
    g_next_seg = 0;

    wl(sd + SD_CTL, (rl(sd + SD_CTL) & ~0x00F00000u)
                    | (1u << 20)      /* stream tag 1 */
                    | 0x02);          /* RUN */
    return 0;
}

static int hda_backend_init(void)
{
    uint8_t bus, dev, fn;
    if (!hda_find_pci(&bus, &dev, &fn)) return 0;

    calibrate_tsc();

    uint32_t id = pci_read32(bus, dev, fn, 0);
    kprint("[HDA] controller at ");
    kprintdec(bus); kprint(":"); kprintdec(dev); kprint("."); kprintdec(fn);
    kprint(" id="); kprinthex(id); kprint("\n");

    /* Memory space + bus master. */
    uint32_t cmd = pci_read32(bus, dev, fn, 4);
    pci_write32(bus, dev, fn, 4, cmd | 0x6u);

    /* Intel controllers: TCSEL (cfg 0x44 bits 2:0) must be 0 so the codec
     * traffic uses TC0 — Linux hda_intel does the same. */
    if ((id & 0xFFFF) == 0x8086) {
        uint32_t tcsel = pci_read32(bus, dev, fn, 0x44);
        if (tcsel & 0x7)
            pci_write32(bus, dev, fn, 0x44, tcsel & ~0x7u);
    }

    /* BAR0: memory space, may be 64-bit. */
    uint32_t bar0 = pci_read32(bus, dev, fn, 0x10);
    if (bar0 & 1) {
        kprint("[HDA] BAR0 is I/O space -- not supported\n");
        return 0;
    }
    uint64_t mmio = bar0 & 0xFFFFFFF0u;
    if ((bar0 & 0x6) == 0x4)
        mmio |= (uint64_t)pci_read32(bus, dev, fn, 0x14) << 32;
    if (mmio == 0 || mmio > 0xFFFFFFFFull) {
        kprint("[HDA] BAR0 unusable\n");
        return 0;
    }
    g_mmio = (volatile uint8_t *)(uintptr_t)mmio;

    uint16_t gcap = rw(REG_GCAP);
    KLOG(KLOG_AUDIO, KLOG_DEBUG, "hda: mmio=%x gcap=%x\n",
         (uint32_t)mmio, (uint32_t)gcap);

    if (hda_controller_reset() != 0) {
        kprint("[HDA] controller reset failed\n");
        return 0;
    }

    /* Codec presence bits (one per codec address). */
    uint16_t state = rw(REG_STATESTS);
    ww(REG_STATESTS, state);    /* ack */
    if (!state) {
        kprint("[HDA] no codecs present\n");
        return 0;
    }
    KLOG(KLOG_AUDIO, KLOG_DEBUG, "hda: STATESTS=%x\n", (uint32_t)state);

    if (hda_corb_rirb_init() != 0) {
        KLOG(KLOG_AUDIO, KLOG_DEBUG,
             "hda: CORB/RIRB unusable, falling back to ICOI\n");
        g_corb_ok = 0;
    } else {
        g_corb_ok = 1;
    }

    /* First present codec. */
    int cad = -1;
    for (int i = 0; i < 15; i++) {
        if (!(state & (1u << i))) continue;
        g_cad = (uint8_t)i;
        uint32_t vid = hda_get_param(0, 0x00);
        if (vid && vid != 0xFFFFFFFFu) { cad = i; break; }
    }
    if (cad < 0) {
        kprint("[HDA] codecs visible but none respond\n");
        return 0;
    }
    g_cad = (uint8_t)cad;

    uint32_t vid = hda_get_param(0, 0x00);

    /* Find the Audio Function Group under the root node. */
    uint32_t nc = hda_get_param(0, 0x04);
    int fg_start = (nc >> 16) & 0xFF, fg_cnt = nc & 0xFF;
    for (int i = 0; i < fg_cnt; i++) {
        int nid = fg_start + i;
        uint32_t ft = hda_get_param(nid, 0x05);
        if (ft & 1) { g_afg = (uint8_t)nid; break; }
    }
    if (!g_afg) {
        kprint("[HDA] no audio function group\n");
        return 0;
    }
    KLOG(KLOG_AUDIO, KLOG_DEBUG, "hda: afg nid=%x\n", (uint32_t)g_afg);
    hda_set(g_afg, 0x705, 0);               /* AFG to D0 */

    /* GET_SUBSYSTEM_ID is an AFG verb — addressed to the function group,
     * not the codec root (nid 0 returns garbage on real codecs, which is
     * why the Apple quirk silently never fired on metal). */
    uint32_t ssid = 0;
    if (hda_verb(g_cad, g_afg, 0xF20 /*GET_SUBSYSTEM_ID*/, 0, &ssid) != 0)
        ssid = 0;
    uint32_t pci_ssid = pci_read32(bus, dev, fn, 0x2C);
    KLOG(KLOG_AUDIO, KLOG_DEBUG, "hda: codec %u vid=%x ssid=%x pci-ssid=%x\n",
         (uint32_t)g_cad, vid, ssid, pci_ssid);

    /* Apple quirk (patch_realtek.c ALC889_FIXUP_MBP_VREF /
     * ALC885_FIXUP_MACPRO_GPIO — both take the GPIO path; VREF applied
     * per-pin in hda_enable_path).  Apple shows up in either the codec
     * SSID or the controller's PCI subsystem vendor. */
    if ((ssid & 0xFFFF) == 0x106B || (ssid >> 16) == 0x106B ||
        (pci_ssid & 0xFFFF) == 0x106B)
        hda_apple_fixup();

    /* Enumerate AFG widgets; wire every usable output pin to a DAC. */
    nc = hda_get_param(g_afg, 0x04);
    int w_start = (nc >> 16) & 0xFF, w_cnt = nc & 0xFF;
    if (w_cnt > 64) w_cnt = 64;
    int npins = 0;
    for (int i = 0; i < w_cnt; i++) {
        int nid = w_start + i;
        uint32_t wcaps = hda_get_param(nid, 0x09);
        if (WIDGET_TYPE(wcaps) != WCAP_PIN) continue;
        uint32_t pcaps = hda_get_param(nid, 0x0C);
        if (!(pcaps & (1u << 4))) continue;   /* not output-capable */
        uint32_t def = hda_get_defcfg(nid);
        if (DEFCFG_CONN(def) == 1) continue;  /* no physical connection */
        uint32_t devt = DEFCFG_DEV(def);
        if (devt != 0 && devt != 1 && devt != 2) continue; /* LO/Spk/HP */

        struct hda_path path = {{0}, {0}, 0};
        if (hda_find_dac(nid, 0, &path) <= 0) {
            KLOG(KLOG_AUDIO, KLOG_DEBUG,
                 "hda: pin %x has no DAC path, skipped\n", (uint32_t)nid);
            continue;
        }
        hda_enable_path(&path);
        hda_vol_register(&path);
        KLOG(KLOG_AUDIO, KLOG_DEBUG,
             "hda: pin %x dev=%u -> dac %x path %x/%x/%x/%x\n",
             (uint32_t)nid, devt, (uint32_t)path.nids[path.n - 1],
             (uint32_t)path.nids[0], (uint32_t)path.nids[1],
             (uint32_t)path.nids[2], (uint32_t)path.nids[3]);
        if (g_dac < 0) g_dac = path.nids[path.n - 1];
        npins++;
    }

    if (!npins || g_dac < 0) {
        kprint("[HDA] no usable output pin->DAC path\n");
        return 0;
    }

    if (hda_stream_start(gcap) != 0) {
        kprint("[HDA] stream start failed\n");
        return 0;
    }

    kprint("[HDA] streaming via dac nid "); kprintdec((uint32_t)g_dac);
    kprint(", "); kprintdec((uint32_t)npins); kprint(" output pin(s)");
    kprint(g_corb_ok ? "" : " (icci verbs)");
    kprint("\n");
    g_ready = 1;
    return 1;
}

static void hda_backend_service(void)
{
    if (!g_ready) return;
    uint32_t sd = SD_BASE + g_sd_index * SD_STRIDE;

    /* Current segment the hardware is playing. */
    uint32_t lpib = rl(sd + SD_LPIB) % HDA_BUF_BYTES;
    int cur = (int)(lpib / (HDA_SEG_FRAMES * 4));

    /* Refill every segment between the write cursor and the play cursor —
     * those are the ones the hardware has finished with. */
    while (g_next_seg != cur) {
        int16_t *dst = g_pcm + g_next_seg * HDA_SEG_FRAMES * 2;
        unsigned int got = audio_ring_read(dst, HDA_SEG_FRAMES);
        for (unsigned int f = got; f < HDA_SEG_FRAMES; f++) {
            dst[f * 2] = 0; dst[f * 2 + 1] = 0;
        }
        g_next_seg = (g_next_seg + 1) % HDA_BDL_COUNT;
    }
}

static void hda_backend_shutdown(void)
{
    if (!g_ready) return;
    uint32_t sd = SD_BASE + g_sd_index * SD_STRIDE;
    wl(sd + SD_CTL, rl(sd + SD_CTL) & ~0x2u); /* clear RUN */
    g_ready = 0;
}

/* -------------------------------------------------------------------------
 * Diagnostics — C:hdadump reads live widget state through the verb path.
 * ------------------------------------------------------------------------- */

static void hd_hex(char *l, int *i, uint32_t v, int digits)
{
    for (int d = digits - 1; d >= 0; d--)
        l[(*i)++] = "0123456789abcdef"[(v >> (d * 4)) & 0xF];
    l[*i] = '\0';
}
static void hd_dec(char *l, int *i, uint32_t v)
{
    char t[12]; int n = 0;
    if (!v) { l[(*i)++] = '0'; l[*i] = 0; return; }
    while (v && n < 11) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) l[(*i)++] = t[--n];
    l[*i] = '\0';
}
static void hd_str(char *l, int *i, const char *s)
{
    while (*s) l[(*i)++] = *s++;
    l[*i] = '\0';
}

static void hda_dump_line(void (*emit)(const char *, void *), void *ud,
                          const char *s)
{
    char line[120];
    int i = 0;
    hd_str(line, &i, s);
    emit(line, ud);
}

void HDA_DumpState(void (*emit)(const char *line, void *ud), void *ud)
{
    if (!g_ready) {
        hda_dump_line(emit, ud, "hda: not ready");
        return;
    }
    {
        char l[120]; int i = 0;
        hd_str(l, &i, "hda: cad=0x"); hd_hex(l, &i, g_cad, 2);
        hd_str(l, &i, " afg=0x"); hd_hex(l, &i, g_afg, 2);
        hd_str(l, &i, " dac=0x"); hd_hex(l, &i, (uint32_t)g_dac, 2);
        hd_str(l, &i, " corb="); hd_dec(l, &i, (uint32_t)g_corb_ok);
        hd_str(l, &i, " quirk="); hd_dec(l, &i, (uint32_t)g_apple_quirk);
        hd_str(l, &i, " gpio=0x");
        {
            uint32_t gd = 0;
            hda_verb(g_cad, g_afg, 0xF15 /*GET_GPIO_DATA*/, 0, &gd);
            hd_hex(l, &i, gd & 0xFF, 2);
        }
        hd_str(l, &i, " sd="); hd_dec(l, &i, (uint32_t)g_sd_index);
        hd_str(l, &i, " lpib=0x");
        hd_hex(l, &i, rl(SD_BASE + g_sd_index * SD_STRIDE + SD_LPIB), 8);
        hd_str(l, &i, " ctl=0x");
        hd_hex(l, &i, rl(SD_BASE + g_sd_index * SD_STRIDE + SD_CTL), 8);
        emit(l, ud);
    }
    for (int p = 0; p < g_nout; p++) {
        const struct hda_path *path = &g_out[p].p;
        char l[120]; int i = 0;
        hd_str(l, &i, "path"); hd_dec(l, &i, (uint32_t)p);
        hd_str(l, &i, " pin 0x"); hd_hex(l, &i, path->nids[0], 2);
        hd_str(l, &i, " -> dac 0x");
        hd_hex(l, &i, path->nids[path->n - 1], 2);
        hd_str(l, &i, " volnid=0x");
        hd_hex(l, &i, g_out[p].vol_nid, 2);
        emit(l, ud);
        for (int j = 0; j < path->n; j++) {
            int nid = path->nids[j];
            uint32_t wcaps = hda_get_param(nid, 0x09);
            int type = WIDGET_TYPE(wcaps);
            uint32_t pwr = 0, amp = 0, pct = 0, eapd = 0, def = 0, fmt = 0;
            hda_verb(g_cad, nid, 0xF05, 0, &pwr);
            if (wcaps & (1u << 2))
                hda_verb(g_cad, nid, 0xB00, 0x8000, &amp);
            if (type == WCAP_PIN) {
                hda_verb(g_cad, nid, 0xF07, 0, &pct);
                hda_verb(g_cad, nid, 0xF0C, 0, &eapd);
                def = hda_get_defcfg(nid);
            }
            if (type == WCAP_DAC) {
                hda_verb(g_cad, nid, 0xA00, 0, &fmt);
            }
            i = 0;
            hd_str(l, &i, "  nid 0x"); hd_hex(l, &i, (uint32_t)nid, 2);
            hd_str(l, &i, " t="); hd_dec(l, &i, (uint32_t)type);
            hd_str(l, &i, " pwr=0x"); hd_hex(l, &i, pwr & 0xFF, 2);
            if (wcaps & (1u << 2)) {
                hd_str(l, &i, " amp=0x"); hd_hex(l, &i, amp & 0xFF, 2);
            }
            if (type == WCAP_PIN) {
                hd_str(l, &i, " pinctl=0x"); hd_hex(l, &i, pct & 0xFF, 2);
                hd_str(l, &i, " eapd=0x"); hd_hex(l, &i, eapd & 0xFF, 2);
                hd_str(l, &i, " dev="); hd_dec(l, &i, DEFCFG_DEV(def));
            }
            if (type == WCAP_DAC) {
                hd_str(l, &i, " fmt=0x"); hd_hex(l, &i, fmt & 0xFFFF, 4);
            }
            emit(l, ud);
        }
    }
}

AudioBackend audio_backend_hda = {
    .init       = hda_backend_init,
    .service    = hda_backend_service,
    .shutdown   = hda_backend_shutdown,
    .set_volume = hda_backend_set_volume,
    .name       = "HDA",
};
