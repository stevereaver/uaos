/*
 * dbgcon.c — On-screen kernel debug console over the linear framebuffer.
 *
 * Machines like the MacBookPro4,1 have no serial port, so kprint/klog
 * boot output is invisible until telnetd comes up.  When the kernel
 * command line contains "fbcon", every completed klog line is also
 * rendered into the framebuffer with the 8x16 system font — the last
 * screenful of boot messages remains visible at any hang point.
 *
 * Writes go straight to VRAM (never the GUI back buffer), so output
 * survives regardless of what the desktop is doing.
 */

#include "klog.h"
#include "../display/framebuffer.h"

static int      g_dbgcon_on;
static int      g_vram_ready;            /* 4GB map installed (safe VRAM) */
static uint32_t dbg_cx, dbg_cy;          /* cursor, char cells          */
static uint32_t dbg_cols, dbg_rows;      /* console geometry            */

/* Called once the MMU sandbox's full 4 GB identity map is active.  The
 * bootstrap page tables only cover 1 GB and the framebuffer BAR sits
 * above that — drawing before this point triple-faults.  Replays the
 * last screenful of klog lines captured while VRAM was unreachable. */
void Dbgcon_VramReady(void)
{
    g_vram_ready = 1;
    if (!g_dbgcon_on || !g_fb.valid) return;

    dbg_cols = g_fb.width  / 8;
    dbg_rows = g_fb.height / 16;
    if (!dbg_cols || !dbg_rows) return;

    uint32_t n = klog_ring_count();
    for (uint32_t i = (n > dbg_rows ? n - dbg_rows : 0); i < n; i++) {
        int sub = 0, lvl = 0;
        const char *t = 0;
        if (!klog_ring_get(i, &sub, &lvl, &t) || !t) continue;
        uint32_t l = 0;
        while (t[l] && l < 200) l++;
        Dbgcon_Write(t, l);
        Dbgcon_Write("\n", 1);
    }
}

/* Scan the multiboot2 info block for the command-line tag (type 1) and
 * enable the console if it contains the token "fbcon". */
void Dbgcon_Init(uint32_t mb2_info_phys)
{
    if (!mb2_info_phys) return;
    const uint8_t *base = (const uint8_t *)(uintptr_t)mb2_info_phys;
    uint32_t total = *(const uint32_t *)base;
    const uint8_t *p   = base + 8;
    const uint8_t *end = base + total;
    while (p + 8 <= end) {
        uint32_t type = *(const uint32_t *)p;
        uint32_t size = *(const uint32_t *)(p + 4);
        if (type == 0 || size < 8) break;
        if (type == 1) {                     /* boot command line */
            const char *s = (const char *)(p + 8);
            for (uint32_t i = 0; i + 5 <= size - 8; i++)
                if (s[i] == 'f' && s[i+1] == 'b' && s[i+2] == 'c' &&
                    s[i+3] == 'o' && s[i+4] == 'n')
                    g_dbgcon_on = 1;
        }
        p += (size + 7) & ~7U;
    }
}

int Dbgcon_Enabled(void) { return g_dbgcon_on; }

static void dbg_put_glyph(uint32_t px, uint32_t py, uint8_t ch)
{
    if (ch < 0x20 || ch > 0x7E) ch = '?';
    const uint8_t *gl = g_font8x16[ch - 0x20];
    uint8_t *fb = (uint8_t *)(uintptr_t)g_fb.phys_addr;
    if (g_fb.bpp == 32) {
        for (int r = 0; r < 16; r++) {
            uint8_t  bits = gl[r];
            uint32_t *row = (uint32_t *)(fb + (py + r) * g_fb.pitch);
            for (int c = 0; c < 8; c++)
                row[px + c] = (bits & (0x80 >> c)) ? 0x00FFFFFFu : 0;
        }
    } else {                                        /* 24 bpp */
        for (int r = 0; r < 16; r++) {
            uint8_t  bits = gl[r];
            uint8_t *row  = fb + (py + r) * g_fb.pitch;
            for (int c = 0; c < 8; c++) {
                uint8_t v = (bits & (0x80 >> c)) ? 0xFF : 0;
                uint8_t *q = row + (px + c) * 3;
                q[0] = q[1] = q[2] = v;
            }
        }
    }
}

static void dbg_scroll(void)
{
    uint8_t  *fb  = (uint8_t *)(uintptr_t)g_fb.phys_addr;
    uint32_t  one = 16 * g_fb.pitch;
    uint32_t  tot = g_fb.height * g_fb.pitch;
    for (uint32_t i = 0; i < tot - one; i++) fb[i] = fb[i + one];
    for (uint32_t i = tot - one; i < tot; i++) fb[i] = 0;
}

void Dbgcon_Write(const char *s, uint32_t len)
{
    if (!g_dbgcon_on || !g_vram_ready || !g_fb.valid || !s) return;
    if (!dbg_cols) {
        dbg_cols = g_fb.width  / 8;
        dbg_rows = g_fb.height / 16;
        if (!dbg_cols || !dbg_rows) return;
    }
    for (uint32_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == '\r') continue;
        if (c != '\n') {
            dbg_put_glyph(dbg_cx * 8, dbg_cy * 16, (uint8_t)c);
            if (++dbg_cx >= dbg_cols) dbg_cx = 0;
        } else {
            dbg_cx = 0;
        }
        if (dbg_cx == 0 && ++dbg_cy >= dbg_rows) {
            dbg_scroll();
            dbg_cy = dbg_rows - 1;
        }
    }
}
