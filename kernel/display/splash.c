/* splash.c — UAOS Boot Splash Screen
 *
 * Paints the embedded splash artwork onto the linear framebuffer during
 * early kernel init, before the scheduler and WM exist.  The artwork is
 * converted from assets/splash.jpg at build time (tools/make_splash.py) into a
 * self-describing RGB24 blob and linked into the kernel image with
 * `ld -r -b binary`, which exports the _binary_*_start/_end symbols.
 *
 * Blob layout (little-endian):
 *   [0:4]   "SPL0" magic
 *   [4:8]   u32 width
 *   [8:12]  u32 height
 *   [12:16] u32 border colour 0xRRGGBB (screen fill around the image)
 *   [16:]   RGB24 pixel data, row-major, top-down
 *
 * Must only be called after UAOS_MMU_Init() — the Multiboot2 framebuffer
 * can live above the bootstrap 1 GB identity map.
 */

#include "splash.h"
#include "framebuffer.h"
#include <stdint.h>
#include <stddef.h>

extern const uint8_t _binary_splash_rgb_start[];
extern const uint8_t _binary_splash_rgb_end[];

#define SPLASH_MAGIC  0x304C5053u   /* "SPL0" as stored little-endian */
#define SPLASH_HDR    16
#define SPLASH_MAX_DIM 4096

/* Row conversion buffer — g_fb.width is clamped to BB_MAX_W (1440). */
static uint32_t s_row[1440];

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0]         | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void Splash_Show(void)
{
    if (!g_fb.valid) return;

    const uint8_t *blob = _binary_splash_rgb_start;
    size_t blob_sz = (size_t)(_binary_splash_rgb_end - _binary_splash_rgb_start);

    if (blob_sz < SPLASH_HDR || rd32(blob) != SPLASH_MAGIC) return;

    uint32_t iw = rd32(blob + 4);
    uint32_t ih = rd32(blob + 8);
    uint32_t bg = rd32(blob + 12) & 0x00FFFFFFu;
    const uint8_t *pix = blob + SPLASH_HDR;

    if (!iw || iw > SPLASH_MAX_DIM || !ih || ih > SPLASH_MAX_DIM) return;
    if (blob_sz < SPLASH_HDR + (size_t)iw * ih * 3) return;

    int fw = (int)g_fb.width;
    int fh = (int)g_fb.height;

    /* Fill the whole screen with the image's border colour so the
     * letterbox bands blend seamlessly. */
    FB_FillRect(0, 0, fw, fh, bg);

    /* Centred blit — centre-cropped when the artwork exceeds the mode. */
    int dst_x = (fw - (int)iw) / 2;  if (dst_x < 0) dst_x = 0;
    int dst_y = (fh - (int)ih) / 2;  if (dst_y < 0) dst_y = 0;
    int src_x = ((int)iw > fw) ? ((int)iw - fw) / 2 : 0;
    int src_y = ((int)ih > fh) ? ((int)ih - fh) / 2 : 0;
    int cw = (int)iw - src_x;  if (cw > fw - dst_x) cw = fw - dst_x;
    int ch = (int)ih - src_y;  if (ch > fh - dst_y) ch = fh - dst_y;
    if (cw <= 0 || ch <= 0) return;
    if (cw > (int)(sizeof(s_row) / sizeof(s_row[0])))
        cw = (int)(sizeof(s_row) / sizeof(s_row[0]));

    for (int y = 0; y < ch; y++) {
        const uint8_t *s = pix + ((size_t)(src_y + y) * iw + (size_t)src_x) * 3;
        for (int x = 0; x < cw; x++) {
            /* alpha byte non-zero so FB_BlitARGB doesn't skip the pixel */
            s_row[x] = 0xFF000000u | ((uint32_t)s[0] << 16) |
                       ((uint32_t)s[1] << 8) | (uint32_t)s[2];
            s += 3;
        }
        FB_BlitARGB(dst_x, dst_y + y, cw, s_row, 0);
    }
}

void Splash_Dwell(void)
{
    /* Coarse busy-wait (~1 s class).  Called before the PIT is
     * programmed, so g_pit_ticks can't be used — spin on pause like
     * early_startup.c's key-wait loop. */
    for (volatile unsigned long i = 0; i < 20000000UL; i++)
        __asm__ volatile ("pause");
}
