/*
 * ac97.c — UAOS Intel ICH AC97 host audio PCM backend
 *
 * PCI-based driver for the Intel 82801AA AC97 controller (class 0x04/0x01/0x00).
 * Presents itself as an AudioBackend.  Uses double-buffered DMA descriptors so
 * the mixer can fill one half while the hardware plays the other.
 */

#include "audio/ac97.h"
#include <stdint.h>

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

/* DMA buffer: 4096 stereo 16-bit frames = 16384 bytes, split into two halves. */
#define AC97_BUF_SAMPLES 4096
#define AC97_BUF_BYTES   (AC97_BUF_SAMPLES * 4)
#define AC97_HALF_SAMPLES (AC97_BUF_SAMPLES / 2)
#define AC97_HALF_BYTES   (AC97_BUF_BYTES / 2)

/* Two descriptors in the buffer descriptor list. */
#define AC97_DESC_COUNT 2

static uint8_t g_ac97_buf[AC97_BUF_BYTES] __attribute__((aligned(4096)));

/* Each descriptor is 8 bytes: 4 bytes address, 2 bytes length, 1 byte control,
 * 1 byte reserved.  Align the whole list to 8 bytes. */
static uint8_t g_ac97_desc[AC97_DESC_COUNT * 8] __attribute__((aligned(16)));

static uint16_t g_namba = 0;   /* mixer I/O base */
static uint16_t g_nabmba = 0;  /* bus master I/O base */
static uint32_t g_buf_phys = 0;
static uint32_t g_desc_phys = 0;
static int      g_ac97_ready = 0;
static int      g_ac97_last_civ = -1;

static inline void outb(uint16_t p, uint8_t v)
{
    __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p));
}
static inline uint8_t inb(uint16_t p)
{
    uint8_t v;
    __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}
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
static inline uint16_t inw(uint16_t p)
{
    uint16_t v;
    __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}

static uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg)
{
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11)
                  | ((uint32_t)fn << 8) | (reg & 0xFCu);
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

static void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint32_t val)
{
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11)
                  | ((uint32_t)fn << 8) | (reg & 0xFCu);
    outl(PCI_CONFIG_ADDR, addr);
    outl(PCI_CONFIG_DATA, val);
}

static uint32_t host_phys(void *p)
{
    return (uint32_t)(uintptr_t)p;
}

static void ac97_codec_write(uint8_t reg, uint16_t val)
{
    outw(g_namba + reg, val);
}

static int ac97_find_pci(uint8_t *bus, uint8_t *dev, uint8_t *fn)
{
    for (uint16_t b = 0; b < 256; b++) {
        for (uint8_t d = 0; d < 32; d++) {
            uint32_t id = pci_read32((uint8_t)b, d, 0, 0);
            if (id == 0xFFFFFFFFu) continue;
            uint32_t cls = pci_read32((uint8_t)b, d, 0, 8);
            uint8_t base_class = (cls >> 24) & 0xFFu;
            uint8_t sub_class  = (cls >> 16) & 0xFFu;
            uint8_t prog_if    = (cls >> 8) & 0xFFu;
            if (base_class == 0x04u && sub_class == 0x01u && prog_if == 0x00u) {
                *bus = (uint8_t)b;
                *dev = d;
                *fn = 0;
                return 1;
            }
        }
    }
    return 0;
}

/* Build the buffer descriptor list for two half-buffers. */
static void ac97_build_descriptors(void)
{
    for (int i = 0; i < AC97_DESC_COUNT; i++) {
        uint32_t  *addr  = (uint32_t  *)(g_ac97_desc + i * 8 + 0);
        uint16_t  *len   = (uint16_t  *)(g_ac97_desc + i * 8 + 4);
        uint8_t   *ctl   = (uint8_t   *)(g_ac97_desc + i * 8 + 6);
        *addr = g_buf_phys + i * AC97_HALF_BYTES;
        *len  = (uint16_t)AC97_HALF_SAMPLES;
        /* IOC on every descriptor, LVI on the last one. */
        *ctl = 0x01u | ((i == AC97_DESC_COUNT - 1) ? 0x80u : 0x00u);
    }
}

/* Fill one half of the DMA buffer with stereo frames from the ring buffer.
 * Any unfilled slots are zeroed so the hardware does not replay old data. */
static void ac97_fill_half(int half)
{
    int16_t tmp[AC97_HALF_SAMPLES * 2];
    unsigned int got = audio_ring_read(tmp, AC97_HALF_SAMPLES);

    /* Zero the remainder so we do not loop stale audio. */
    for (unsigned int i = got; i < AC97_HALF_SAMPLES; i++) {
        tmp[i * 2 + 0] = 0;
        tmp[i * 2 + 1] = 0;
    }

    uint8_t *base = g_ac97_buf + half * AC97_HALF_BYTES;
    for (unsigned int i = 0; i < AC97_HALF_SAMPLES; i++) {
        base[i * 4 + 0] = (uint8_t)(tmp[i * 2 + 0] & 0xFF);
        base[i * 4 + 1] = (uint8_t)((tmp[i * 2 + 0] >> 8) & 0xFF);
        base[i * 4 + 2] = (uint8_t)(tmp[i * 2 + 1] & 0xFF);
        base[i * 4 + 3] = (uint8_t)((tmp[i * 2 + 1] >> 8) & 0xFF);
    }
}

static int ac97_backend_init(void)
{
    uint8_t bus, dev, fn;
    if (!ac97_find_pci(&bus, &dev, &fn)) return 0;

    /* Enable I/O space and bus mastering. */
    uint32_t cmd = pci_read32(bus, dev, fn, 4);
    cmd |= 0x05u;
    pci_write32(bus, dev, fn, 4, cmd);

    g_namba  = (uint16_t)(pci_read32(bus, dev, fn, 0x10) & 0xFFFEu);
    g_nabmba = (uint16_t)(pci_read32(bus, dev, fn, 0x14) & 0xFFFEu);
    if (g_namba == 0 || g_nabmba == 0) return 0;

    /* Cold reset the AC97 codec. */
    ac97_codec_write(0x00, 0xFFFFu);
    for (volatile int i = 0; i < 100000; i++) {}

    /* Set master and PCM out volumes to 0 dB (unmuted). */
    ac97_codec_write(0x02, 0x0000u);
    ac97_codec_write(0x18, 0x0000u);

    /* Set sample rate to 48 kHz (default). */
    ac97_codec_write(0x2E, 48000u);

    g_buf_phys  = host_phys(g_ac97_buf);
    g_desc_phys = host_phys(g_ac97_desc);

    /* Silence the buffer. */
    for (int i = 0; i < AC97_BUF_BYTES; i++) g_ac97_buf[i] = 0;

    ac97_build_descriptors();

    /* Reset the PCM out bus master. */
    outb(g_nabmba + 0x1B, 0x02u);
    for (volatile int i = 0; i < 1000; i++) {}
    outb(g_nabmba + 0x1B, 0x00u);

    /* Set descriptor base address and last valid index. */
    outl(g_nabmba + 0x10, g_desc_phys);
    outb(g_nabmba + 0x15, (uint8_t)(AC97_DESC_COUNT - 1));

    /* Start DMA. */
    outb(g_nabmba + 0x1B, 0x01u);

    g_ac97_last_civ = -1;
    g_ac97_ready = 1;
    return 1;
}

static void ac97_backend_service(void)
{
    if (!g_ac97_ready) return;

    /* CIV is the index of the descriptor currently being played. */
    int civ = (int)(inb(g_nabmba + 0x14) & 0x1Fu);

    if (g_ac97_last_civ < 0) {
        /* First service call: fill the half that is not currently playing. */
        int inactive = (civ + 1) % AC97_DESC_COUNT;
        ac97_fill_half(inactive);
    } else if (civ != g_ac97_last_civ) {
        /* The hardware just moved to a new descriptor; the descriptor it
         * finished (the previous CIV) is now free to refill. */
        ac97_fill_half(g_ac97_last_civ);
    }

    g_ac97_last_civ = civ;
}

static void ac97_backend_shutdown(void)
{
    if (!g_ac97_ready) return;
    outb(g_nabmba + 0x1B, 0x00u); /* stop */
    g_ac97_ready = 0;
}

AudioBackend audio_backend_ac97 = {
    .init     = ac97_backend_init,
    .service  = ac97_backend_service,
    .shutdown = ac97_backend_shutdown,
    .name     = "AC97",
};
