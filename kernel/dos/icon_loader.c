/*
 * icon_loader.c — Amiga .info Icon File Parser
 *
 * Reads classic planar .info icons from the VFS and converts them
 * into native ARGB bitmaps suitable for the linear framebuffer.
 */

#include "icon_loader.h"
#include "vfs.h"
#include "ramfs.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

/* Maximum raw planar read size */
#define ICON_MAX_PLANE_BYTES  (ICON_MAX_WIDTH * ICON_MAX_HEIGHT / 8)

/* =========================================================================
 * Helpers: guest memory reading stubs (native VFS uses host pointers)
 * ========================================================================= */

static inline uint8_t  get_u8 (const uint8_t *p, int off) { return p[off]; }
static inline uint16_t get_u16(const uint8_t *p, int off)
{
    return (uint16_t)((p[off] << 8) | p[off + 1]);
}
static inline uint32_t get_u32(const uint8_t *p, int off)
{
    return ((uint32_t)p[off] << 24) | ((uint32_t)p[off + 1] << 16) |
           ((uint32_t)p[off + 2] << 8) | (uint32_t)p[off + 3];
}

/* =========================================================================
 * Planar to chunky ARGB conversion
 *
 * Amiga icons are stored as interleaved bitplanes.
 * Each scanline is padded to a multiple of 16 bits (2 bytes).
 * For depth > 1, planes are stored sequentially (plane 0, plane 1, ...).
 * ========================================================================= */

static void planar_to_argb(const uint8_t *src, uint32_t *dst,
                           uint16_t width, uint16_t height,
                           uint16_t depth, uint32_t transparent_pen)
{
    uint16_t bpr = ((width + 15) >> 4) << 1;  /* bytes per row, word-aligned */
    uint16_t plane_size = bpr * height;

    /* Default Amiga Workbench palette for icons (pens 0-3):
     * 0 = transparent (blue-ish on WB but we treat as transparent)
     * 1 = white
     * 2 = black
     * 3 = grey/selected
     */
    static const uint32_t pens[8] = {
        0x00000000,  /* 0: transparent (ARGB) */
        0xFFFFFFFF,  /* 1: white */
        0xFF000000,  /* 2: black */
        0xFF888888,  /* 3: grey */
        0xFFFFFFFF,  /* 4-7: repeated for safety */
        0xFFFFFFFF,
        0xFFFFFFFF,
        0xFFFFFFFF,
    };

    /* Clear output */
    for (int i = 0; i < ICON_MAX_WIDTH * ICON_MAX_HEIGHT; i++)
        dst[i] = transparent_pen;

    if (depth == 0 || depth > ICON_MAX_PLANES) return;

    for (uint16_t y = 0; y < height; y++) {
        for (uint16_t x = 0; x < width; x++) {
            uint16_t byte_idx = y * bpr + (x >> 3);
            uint8_t  bit_mask = 0x80 >> (x & 7);
            uint8_t  pen = 0;

            for (uint16_t d = 0; d < depth; d++) {
                if (src[byte_idx + d * plane_size] & bit_mask)
                    pen |= (1 << d);
            }

            if (pen != 0) {
                dst[y * ICON_MAX_WIDTH + x] = pens[pen];
            }
        }
    }
}

/* =========================================================================
 * Public API
 * ========================================================================= */

int Icon_Load(const char *path, ParsedIcon *out)
{
    if (!out) return 0;
    memset(out, 0, sizeof(ParsedIcon));

    if (!path || path[0] == '\0') return 0;

    /* Build .info path: append ".info" */
    char info_path[128];
    int plen = 0;
    while (path[plen] && plen < (int)sizeof(info_path) - 6) {
        info_path[plen] = path[plen];
        plen++;
    }
    const char *suffix = ".info";
    for (int i = 0; i < 5; i++) info_path[plen++] = suffix[i];
    info_path[plen] = '\0';

    /* Try VFS read */
    VfsFile fh;
    if (!VFS_Open(&fh, info_path, VFS_READ)) {
        /* No .info file — caller should fall back to procedural icon */
        return 0;
    }

    RamFsNode *node = fh.node;
    if (!node || !node->data || node->size == 0) {
        VFS_Close(&fh);
        return 0;
    }

    const uint8_t *data = (const uint8_t *)node->data;
    size_t         size = node->size;

    if (size < 78) return 0;  /* Minimum DiskObject header size */

    /* Verify magic */
    uint16_t magic = get_u16(data, 0);
    if (magic != WB_DISKOBJECT_MAGIC) {
        fprintf(stderr, "[ICON] Bad magic 0x%04X for %s\n", magic, info_path);
        return 0;
    }

    uint16_t version = get_u16(data, 2);
    (void)version;

    /* DiskObject fields */
    uint32_t gadget_ptr = get_u32(data, 4);
    uint8_t  type       = get_u8 (data, 8);
    uint32_t def_tool   = get_u32(data, 10);
    uint32_t tool_types = get_u32(data, 14);
    int16_t  cur_x      = (int16_t)get_u16(data, 18);
    int16_t  cur_y      = (int16_t)get_u16(data, 20);
    (void)gadget_ptr;

    out->type = type;
    out->pos_x = cur_x;
    out->pos_y = cur_y;

    /* Default tool string */
    if (def_tool && def_tool < size) {
        int i = 0;
        while (i < ICON_MAX_LABEL - 1 && (def_tool + i) < size && data[def_tool + i]) {
            out->default_tool[i] = (char)data[def_tool + i];
            i++;
        }
        out->default_tool[i] = '\0';
    }

    /* Tool types array */
    if (tool_types && tool_types < size) {
        int tt_count = 0;
        uint32_t entry_off = tool_types;
        while (tt_count < ICON_MAX_TOOLTYPES && entry_off + 3 < size) {
            uint32_t str_ptr = get_u32(data, (int)entry_off);
            if (str_ptr == 0) break;
            if (str_ptr < size) {
                int j = 0;
                while (j < ICON_MAX_TOOLTYPE_LEN - 1 && (str_ptr + j) < size && data[str_ptr + j]) {
                    out->tool_types[tt_count][j] = (char)data[str_ptr + j];
                    j++;
                }
                out->tool_types[tt_count][j] = '\0';
                tt_count++;
            }
            entry_off += 4;
            /* safety break if we seem to be reading garbage */
            if (entry_off > size) break;
        }
        out->tool_type_count = tt_count;
    }

    /* Gadget / Image data starts after the fixed-size header.
     * For simplicity we parse the raw image from offsets following
     * the header rather than following the guest pointer chain.
     * Classic .info layout:
     *   0-3:   magic + version
     *   4-7:   gadget_ptr
     *   8:     type, pad
     *   10-13: default_tool
     *   14-17: tool_types
     *   18-21: current_x, current_y
     *   22-25: drawer_data
     *   26-29: tool_window
     *   30-31: stack_size
     *   32+:   Gadget struct (14 bytes)
     *   46+:   normal Image struct (12 bytes)
     *   58+:   selected Image struct (12 bytes)
     *   70+:   image data
     */

    if (size < 82) return 1;  /* enough for header but no images */

    int gadget_off = 32;
    int img_n_off  = gadget_off + 14;
    int img_s_off  = img_n_off  + 12;
    int data_off   = img_s_off  + 12;

    /* Normal image dimensions */
    uint16_t n_w     = get_u16(data, img_n_off + 0);
    uint16_t n_h     = get_u16(data, img_n_off + 2);
    uint16_t n_depth = get_u16(data, img_n_off + 4);
    uint16_t n_dsize = get_u16(data, img_n_off + 6);

    if (n_w > 0 && n_w <= ICON_MAX_WIDTH && n_h > 0 && n_h <= ICON_MAX_HEIGHT
        && n_depth > 0 && n_depth <= ICON_MAX_PLANES && data_off + n_dsize <= (int)size) {
        out->image.width  = n_w;
        out->image.height = n_h;
        out->image.depth  = n_depth;

        /* Convert normal image */
        planar_to_argb(data + data_off,
                       out->image.normal,
                       n_w, n_h, n_depth,
                       0x00000000);

        /* Selected image follows normal image data */
        int sel_off = data_off + n_dsize;
        if (sel_off + 12 <= (int)size) {
            uint16_t s_w     = get_u16(data, sel_off + 0);
            uint16_t s_h     = get_u16(data, sel_off + 2);
            uint16_t s_depth = get_u16(data, sel_off + 4);
            uint16_t s_dsize = get_u16(data, sel_off + 6);
            int s_data_off = sel_off + 12;

            if (s_w == n_w && s_h == n_h && s_depth == n_depth
                && s_data_off + s_dsize <= (int)size) {
                planar_to_argb(data + s_data_off,
                               out->image.selected,
                               s_w, s_h, s_depth,
                               0x00000000);
            } else {
                /* If selected image is missing or mismatched, use normal */
                memcpy(out->image.selected, out->image.normal,
                       sizeof(out->image.normal));
            }
        } else {
            memcpy(out->image.selected, out->image.normal,
                   sizeof(out->image.normal));
        }
    }

    /* Derive label from base filename */
    const char *base = path;
    int last_slash = -1;
    for (int i = 0; path[i]; i++) {
        if (path[i] == '/' || path[i] == ':') last_slash = i;
    }
    if (last_slash >= 0) base = path + last_slash + 1;

    int li = 0;
    while (li < ICON_MAX_LABEL - 1 && base[li] && base[li] != '.') {
        out->label[li] = base[li];
        li++;
    }
    out->label[li] = '\0';

    VFS_Close(&fh);
    return 1;
}

void Icon_Free(ParsedIcon *icon)
{
    (void)icon;
    /* Fixed-size structure — nothing to free dynamically */
}

int Icon_ExistsFor(const char *path)
{
    if (!path || path[0] == '\0') return 0;

    char info_path[128];
    int plen = 0;
    while (path[plen] && plen < (int)sizeof(info_path) - 6) {
        info_path[plen] = path[plen];
        plen++;
    }
    const char *suffix = ".info";
    for (int i = 0; i < 5; i++) info_path[plen++] = suffix[i];
    info_path[plen] = '\0';

    VfsFile fh;
    int exists = VFS_Open(&fh, info_path, VFS_READ);
    if (exists) VFS_Close(&fh);
    return exists;
}
