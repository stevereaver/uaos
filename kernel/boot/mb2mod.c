/*
 * mb2mod.c — Multiboot2 module helpers (see mb2mod.h)
 */

#include "mb2mod.h"
#include "dos/blockdev.h"
#include "kprint.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t size;
} Mb2TagHdr;

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t size;
    uint32_t mod_start;
    uint32_t mod_end;
    char     cmdline[];          /* NUL-terminated */
} Mb2TagModule;

typedef struct __attribute__((packed)) {
    uint32_t total_size;
    uint32_t reserved;
} Mb2InfoHdr;

int Mb2_FindModule(uint32_t mb2_info_phys, const char *name,
                   uint64_t *start, uint64_t *size)
{
    if (!mb2_info_phys || !name) return 0;

    Mb2InfoHdr *hdr = (Mb2InfoHdr *)(uintptr_t)mb2_info_phys;
    uint8_t *p   = (uint8_t *)(uintptr_t)(mb2_info_phys + 8);
    uint8_t *end = (uint8_t *)(uintptr_t)(mb2_info_phys + hdr->total_size);

    while (p < end) {
        Mb2TagHdr *tag = (Mb2TagHdr *)p;
        if (tag->type == 0) break;

        if (tag->type == 3) {
            Mb2TagModule *m = (Mb2TagModule *)p;
            if (strcmp(m->cmdline, name) == 0 && m->mod_end > m->mod_start) {
                if (start) *start = m->mod_start;
                if (size)  *size  = (uint64_t)m->mod_end - m->mod_start;
                return 1;
            }
        }

        uint32_t aligned = (tag->size + 7) & ~7U;
        p += aligned ? aligned : 8;
    }
    return 0;
}

int Mb2_CmdlineHas(uint32_t mb2_info_phys, const char *tok)
{
    if (!mb2_info_phys || !tok) return 0;

    Mb2InfoHdr *hdr = (Mb2InfoHdr *)(uintptr_t)mb2_info_phys;
    const uint8_t *p   = (const uint8_t *)(uintptr_t)(mb2_info_phys + 8);
    const uint8_t *end = (const uint8_t *)(uintptr_t)(mb2_info_phys + hdr->total_size);

    while (p < end) {
        Mb2TagHdr *tag = (Mb2TagHdr *)p;
        if (tag->type == 0 || tag->size < 8) break;
        if (tag->type == 1) {                /* boot command line */
            const char *s = (const char *)(p + 8);
            uint32_t n = tag->size - 8;
            uint32_t tl = (uint32_t)strlen(tok);
            for (uint32_t i = 0; i + tl <= n; i++)
                if (memcmp(s + i, tok, tl) == 0) return 1;
            return 0;
        }
        p += (tag->size + 7) & ~7U;
    }
    return 0;
}

/* ------------------------------------------------------------------ */

/* Copy the value of a "name=value" cmdline token into out[max].
 * Returns 1 when found.  Stops at whitespace or end of cmdline. */
int Mb2_CmdlineParam(uint32_t mb2_info_phys, const char *name,
                     char *out, int max)
{
    if (!mb2_info_phys || !name || !out || max < 2) return 0;

    Mb2InfoHdr *hdr = (Mb2InfoHdr *)(uintptr_t)mb2_info_phys;
    const uint8_t *p   = (const uint8_t *)(uintptr_t)(mb2_info_phys + 8);
    const uint8_t *end = (const uint8_t *)(uintptr_t)(mb2_info_phys + hdr->total_size);

    while (p < end) {
        Mb2TagHdr *tag = (Mb2TagHdr *)p;
        if (tag->type == 0 || tag->size < 8) break;
        if (tag->type == 1) {                /* boot command line */
            const char *s = (const char *)(p + 8);
            uint32_t n = tag->size - 8;
            uint32_t nl = (uint32_t)strlen(name);
            for (uint32_t i = 0; i + nl <= n; i++) {
                if (memcmp(s + i, name, nl) != 0) continue;
                /* token must start at a word boundary */
                if (i > 0 && s[i-1] != ' ' && s[i-1] != '\t') continue;
                uint32_t v = i + nl;
                int oi = 0;
                while (v < n && s[v] && s[v] != ' ' && s[v] != '\t'
                       && oi < max - 1)
                    out[oi++] = s[v++];
                out[oi] = '\0';
                return 1;
            }
            return 0;
        }
        p += (tag->size + 7) & ~7U;
    }
    return 0;
}

typedef struct {
    uint8_t *base;
    uint64_t bytes;
} RamBDevCtx;

static int ram_bdev_read(BlockDev *dev, uint64_t sector, void *buffer,
                         uint32_t num_sectors)
{
    RamBDevCtx *c = (RamBDevCtx *)dev->private_data;
    uint64_t off = sector * dev->sector_size;
    uint64_t len = (uint64_t)num_sectors * dev->sector_size;
    if (off + len > c->bytes) return -1;
    memcpy(buffer, c->base + off, (size_t)len);
    return 0;
}

static int ram_bdev_write(BlockDev *dev, uint64_t sector,
                          const void *buffer, uint32_t num_sectors)
{
    (void)dev; (void)sector; (void)buffer; (void)num_sectors;
    return -1;
}

static uint64_t ram_bdev_capacity(BlockDev *dev)
{
    RamBDevCtx *c = (RamBDevCtx *)dev->private_data;
    return c->bytes / dev->sector_size;
}

static const BlockDevOps g_ram_bdev_ops = {
    .read         = ram_bdev_read,
    .write        = ram_bdev_write,
    .get_capacity = ram_bdev_capacity,
};

static BlockDev   g_sysroot_bdev;
static RamBDevCtx g_sysroot_ctx;

BlockDev *Mb2Mod_RegisterSysroot(uint32_t mb2_info_phys)
{
    uint64_t start = 0, size = 0;
    if (!Mb2_FindModule(mb2_info_phys, "uaos-sysroot", &start, &size))
        return NULL;

    g_sysroot_ctx.base  = (uint8_t *)(uintptr_t)start;
    g_sysroot_ctx.bytes = size;

    memset(&g_sysroot_bdev, 0, sizeof(g_sysroot_bdev));
    g_sysroot_bdev.name         = "sysroot0";
    g_sysroot_bdev.display_name = "SYSROOT";
    g_sysroot_bdev.sector_size  = 2048;
    g_sysroot_bdev.num_sectors  = size / 2048;
    g_sysroot_bdev.private_data = &g_sysroot_ctx;
    g_sysroot_bdev.ops          = &g_ram_bdev_ops;

    if (BlockDev_Register(&g_sysroot_bdev) != 0) {
        kprint("[MB2MOD] sysroot blockdev register failed\n");
        return NULL;
    }

    kprint("[MB2MOD] sysroot module @ ");
    kprinthex(start);
    kprint(" size ");
    kprinthex(size);
    kprint("\n");
    return &g_sysroot_bdev;
}
