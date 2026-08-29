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
        out->image.has_selected = 0;

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
                out->image.has_selected = 1;
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

/* =========================================================================
 * Big-endian writers (for .info serialization)
 * ========================================================================= */

static inline void put_u16(uint8_t *p, int off, uint16_t v)
{
    p[off]     = (uint8_t)(v >> 8);
    p[off + 1] = (uint8_t)(v & 0xFF);
}

static inline void put_u32(uint8_t *p, int off, uint32_t v)
{
    p[off]     = (uint8_t)(v >> 24);
    p[off + 1] = (uint8_t)(v >> 16);
    p[off + 2] = (uint8_t)(v >> 8);
    p[off + 3] = (uint8_t)(v & 0xFF);
}

/* =========================================================================
 * ARGB to planar bitplane conversion
 *
 * Reverse of planar_to_argb().  Maps ARGB pixels back to Amiga pens:
 *   0 = transparent (alpha 0)
 *   1 = white  (0xFFFFFFFF)
 *   2 = black  (0xFF000000)
 *   3 = grey   (0xFF888888)
 * ========================================================================= */

static uint8_t argb_to_pen(uint32_t argb)
{
    if ((argb & 0xFF000000) == 0) return 0; /* transparent */
    uint32_t r = (argb >> 16) & 0xFF;
    uint32_t g = (argb >> 8)  & 0xFF;
    uint32_t b =  argb        & 0xFF;
    if (r > 200 && g > 200 && b > 200) return 1; /* white */
    if (r < 50  && g < 50  && b < 50)  return 2; /* black */
    return 3; /* grey */
}

static uint16_t argb_to_planar(const uint32_t *src, uint8_t *dst,
                               uint16_t width, uint16_t height, uint16_t depth)
{
    uint16_t bpr = ((width + 15) >> 4) << 1;  /* bytes per row, word-aligned */
    uint16_t plane_size = bpr * height;
    uint16_t total = plane_size * depth;

    /* Clear output */
    for (int i = 0; i < total; i++) dst[i] = 0;

    for (uint16_t y = 0; y < height; y++) {
        for (uint16_t x = 0; x < width; x++) {
            uint8_t pen = argb_to_pen(src[y * ICON_MAX_WIDTH + x]);
            if (pen == 0) continue;

            uint16_t byte_idx = y * bpr + (x >> 3);
            uint8_t  bit_mask = 0x80 >> (x & 7);

            for (uint16_t d = 0; d < depth; d++) {
                if (pen & (1 << d))
                    dst[byte_idx + d * plane_size] |= bit_mask;
            }
        }
    }

    return total;
}

/* =========================================================================
 * Icon_Save — serialize ParsedIcon to .info binary
 * ========================================================================= */

/* Build .info path from a base path */
static void make_info_path(const char *path, char *out, int max)
{
    int plen = 0;
    while (path[plen] && plen < max - 6) {
        out[plen] = path[plen];
        plen++;
    }
    const char *suffix = ".info";
    for (int i = 0; i < 5; i++) out[plen++] = suffix[i];
    out[plen] = '\0';
}

int Icon_Save(const char *path, const ParsedIcon *icon)
{
    if (!path || !icon) return 0;

    char info_path[128];
    make_info_path(path, info_path, (int)sizeof(info_path));

    /* Determine image parameters */
    uint16_t w = icon->image.width;
    uint16_t h = icon->image.height;
    uint16_t depth = icon->image.depth;
    if (w == 0 || h == 0 || depth == 0) {
        w = 0; h = 0; depth = 0;
    }

    uint16_t bpr = (w > 0) ? (((w + 15) >> 4) << 1) : 0;
    uint16_t plane_size = bpr * h;
    uint16_t img_data_size = plane_size * depth;

    /* Layout:
     *   0-31:  DiskObject header (32 bytes)
     *   32-45: Gadget (14 bytes)
     *   46-57: Normal Image header (12 bytes)
     *   58-69: Selected Image header (12 bytes)
     *   70+:   Normal image data (img_data_size bytes)
     *          Selected image data (img_data_size bytes)
     *          Default tool string (null-terminated)
     *          Tool type BPTR array (4 * (count+1) bytes)
     *          Tool type strings (null-terminated, sequential)
     */
    uint32_t data_off = 70;
    uint32_t sel_data_off = data_off + img_data_size;
    uint32_t def_tool_off = sel_data_off + img_data_size;
    uint32_t tt_array_off = def_tool_off;

    /* Include default tool string in layout */
    if (icon->default_tool[0]) {
        tt_array_off += strlen(icon->default_tool) + 1;
    }

    uint32_t tt_count = (uint32_t)icon->tool_type_count;
    if (tt_count > ICON_MAX_TOOLTYPES) tt_count = ICON_MAX_TOOLTYPES;

    uint32_t tt_strings_off = tt_array_off + 4 * (tt_count + 1);

    /* Compute total size */
    uint32_t total_size = tt_strings_off;
    for (uint32_t i = 0; i < tt_count; i++) {
        total_size += strlen(icon->tool_types[i]) + 1;
    }

    /* Allocate buffer (on heap via VFS pool or static) */
    static uint8_t buf[4096];
    if (total_size > sizeof(buf)) return 0;
    memset(buf, 0, total_size);

    /* DiskObject header */
    put_u16(buf, 0, WB_DISKOBJECT_MAGIC);
    put_u16(buf, 2, WB_DISKVERSION);
    put_u32(buf, 4, 0);              /* gadget ptr (unused) */
    buf[8] = icon->type;
    buf[9] = 0;                       /* pad */
    put_u32(buf, 10, icon->default_tool[0] ? def_tool_off : 0);
    put_u32(buf, 14, tt_count > 0 ? tt_array_off : 0);
    put_u16(buf, 18, (uint16_t)icon->pos_x);
    put_u16(buf, 20, (uint16_t)icon->pos_y);
    put_u32(buf, 22, 0);             /* drawer data */
    put_u32(buf, 26, 0);             /* tool window */
    put_u16(buf, 30, 4096);          /* stack size */

    /* Gadget (14 bytes) */
    put_u16(buf, 32, GTYP_CUSTOM);   /* gadgetType */
    put_u16(buf, 34, 0);             /* render flags */
    put_u32(buf, 36, w > 0 ? 46 : 0); /* gadgetRender → normal image */
    put_u32(buf, 40, w > 0 ? 58 : 0); /* selectRender → selected image */
    put_u16(buf, 44, 0);             /* leftEdge */

    /* Normal Image header (12 bytes) */
    if (w > 0) {
        put_u16(buf, 46, w);
        put_u16(buf, 48, h);
        put_u16(buf, 50, depth);
        put_u16(buf, 52, img_data_size);
        put_u32(buf, 54, data_off);  /* image data offset */
    }

    /* Selected Image header (12 bytes) */
    if (w > 0) {
        put_u16(buf, 58, w);
        put_u16(buf, 60, h);
        put_u16(buf, 62, depth);
        put_u16(buf, 64, img_data_size);
        put_u32(buf, 66, sel_data_off);
    }

    /* Normal image planar data */
    if (w > 0 && img_data_size > 0) {
        argb_to_planar(icon->image.normal, buf + data_off, w, h, depth);
    }

    /* Selected image planar data */
    if (w > 0 && img_data_size > 0) {
        if (icon->image.has_selected) {
            argb_to_planar(icon->image.selected, buf + sel_data_off, w, h, depth);
        } else {
            memcpy(buf + sel_data_off, buf + data_off, img_data_size);
        }
    }

    /* Default tool string */
    if (icon->default_tool[0]) {
        int dl = strlen(icon->default_tool);
        memcpy(buf + def_tool_off, icon->default_tool, dl);
        buf[def_tool_off + dl] = '\0';
    }

    /* Tool type BPTR array + strings */
    uint32_t str_off = tt_strings_off;
    for (uint32_t i = 0; i < tt_count; i++) {
        put_u32(buf, tt_array_off + i * 4, str_off);
        int sl = strlen(icon->tool_types[i]);
        memcpy(buf + str_off, icon->tool_types[i], sl);
        buf[str_off + sl] = '\0';
        str_off += sl + 1;
    }
    /* Terminator BPTR (NULL) */
    put_u32(buf, tt_array_off + tt_count * 4, 0);

    /* Write to VFS */
    VfsFile fh;
    if (!VFS_Open(&fh, info_path, VFS_WRITE | VFS_CREATE | VFS_TRUNC)) {
        return 0;
    }
    VFS_Write(&fh, buf, total_size);
    VFS_Close(&fh);
    return 1;
}

/* =========================================================================
 * Icon_SavePosition — update do_CurrentX/Y in an existing .info file
 * ========================================================================= */

int Icon_SavePosition(const char *path, int16_t x, int16_t y)
{
    if (!path) return 0;

    char info_path[128];
    make_info_path(path, info_path, (int)sizeof(info_path));

    /* If .info doesn't exist, create a minimal one with just position */
    VfsFile fh;
    if (!VFS_Open(&fh, info_path, VFS_READ)) {
        ParsedIcon icon;
        memset(&icon, 0, sizeof(icon));
        icon.type = WB_DISK;
        icon.pos_x = x;
        icon.pos_y = y;
        return Icon_Save(path, &icon);
    }

    /* Read existing file */
    uint32_t size = VFS_Size(&fh);
    static uint8_t buf[4096];
    if (size > sizeof(buf) || size < 32) {
        VFS_Close(&fh);
        return 0;
    }
    uint32_t rd = VFS_Read(&fh, buf, size);
    VFS_Close(&fh);
    if (rd < 32) return 0;

    /* Update position fields */
    put_u16(buf, 18, (uint16_t)x);
    put_u16(buf, 20, (uint16_t)y);

    /* Write back */
    if (!VFS_Open(&fh, info_path, VFS_WRITE | VFS_TRUNC)) {
        return 0;
    }
    VFS_Write(&fh, buf, size);
    VFS_Close(&fh);
    return 1;
}

/* =========================================================================
 * Tool type get/set/delete API
 * ========================================================================= */

static int tt_key_match(const char *tt, const char *key)
{
    int i = 0;
    while (key[i] && tt[i]) {
        if (tt[i] != key[i]) return 0;
        i++;
    }
    if (key[i] == '\0') {
        /* Full match — tt must be exactly key or key=value */
        return (tt[i] == '\0' || tt[i] == '=');
    }
    return 0;
}

const char *Icon_ToolTypeGet(const ParsedIcon *icon, const char *key)
{
    if (!icon || !key) return NULL;
    for (int i = 0; i < icon->tool_type_count; i++) {
        if (tt_key_match(icon->tool_types[i], key))
            return icon->tool_types[i];
    }
    return NULL;
}

int Icon_ToolTypeSet(ParsedIcon *icon, const char *key, const char *value)
{
    if (!icon || !key) return 0;

    /* Try to find and replace existing entry */
    for (int i = 0; i < icon->tool_type_count; i++) {
        if (tt_key_match(icon->tool_types[i], key)) {
            if (value) {
                /* Format as key=value */
                int kl = strlen(key);
                int vl = strlen(value);
                if (kl + 1 + vl >= ICON_MAX_TOOLTYPE_LEN) return 0;
                memcpy(icon->tool_types[i], key, kl);
                icon->tool_types[i][kl] = '=';
                memcpy(icon->tool_types[i] + kl + 1, value, vl);
                icon->tool_types[i][kl + 1 + vl] = '\0';
            } else {
                /* Key only */
                int kl = strlen(key);
                if (kl >= ICON_MAX_TOOLTYPE_LEN) return 0;
                memcpy(icon->tool_types[i], key, kl);
                icon->tool_types[i][kl] = '\0';
            }
            return 1;
        }
    }

    /* Append new entry */
    if (icon->tool_type_count >= ICON_MAX_TOOLTYPES) return 0;

    int idx = icon->tool_type_count;
    if (value) {
        int kl = strlen(key);
        int vl = strlen(value);
        if (kl + 1 + vl >= ICON_MAX_TOOLTYPE_LEN) return 0;
        memcpy(icon->tool_types[idx], key, kl);
        icon->tool_types[idx][kl] = '=';
        memcpy(icon->tool_types[idx] + kl + 1, value, vl);
        icon->tool_types[idx][kl + 1 + vl] = '\0';
    } else {
        int kl = strlen(key);
        if (kl >= ICON_MAX_TOOLTYPE_LEN) return 0;
        memcpy(icon->tool_types[idx], key, kl);
        icon->tool_types[idx][kl] = '\0';
    }
    icon->tool_type_count++;
    return 1;
}

int Icon_ToolTypeDelete(ParsedIcon *icon, const char *key)
{
    if (!icon || !key) return 0;
    for (int i = 0; i < icon->tool_type_count; i++) {
        if (tt_key_match(icon->tool_types[i], key)) {
            /* Shift remaining entries down */
            for (int j = i; j < icon->tool_type_count - 1; j++) {
                memcpy(icon->tool_types[j], icon->tool_types[j + 1],
                       ICON_MAX_TOOLTYPE_LEN);
            }
            icon->tool_types[icon->tool_type_count - 1][0] = '\0';
            icon->tool_type_count--;
            return 1;
        }
    }
    return 0;
}

/* =========================================================================
 * Default icon generation (pseudo-icons)
 *
 * Generates simple 4-color (depth=2) planar icons procedurally.
 * Palette: 0=transparent, 1=white, 2=black, 3=grey
 * ========================================================================= */

static void set_pixel(uint32_t *buf, int x, int y, uint32_t argb)
{
    if (x >= 0 && x < ICON_MAX_WIDTH && y >= 0 && y < ICON_MAX_HEIGHT)
        buf[y * ICON_MAX_WIDTH + x] = argb;
}

static void fill_rect_px(uint32_t *buf, int x, int y, int w, int h, uint32_t argb)
{
    for (int dy = 0; dy < h; dy++)
        for (int dx = 0; dx < w; dx++)
            set_pixel(buf, x + dx, y + dy, argb);
}

static void draw_rect_px(uint32_t *buf, int x, int y, int w, int h, uint32_t argb)
{
    for (int dx = 0; dx < w; dx++) {
        set_pixel(buf, x + dx, y, argb);
        set_pixel(buf, x + dx, y + h - 1, argb);
    }
    for (int dy = 0; dy < h; dy++) {
        set_pixel(buf, x, y + dy, argb);
        set_pixel(buf, x + w - 1, y + dy, argb);
    }
}

#define PEN_TRANSPARENT  0x00000000
#define PEN_WHITE        0xFFFFFFFF
#define PEN_BLACK        0xFF000000
#define PEN_GREY         0xFF888888

/* Default icon image size */
#define DEF_ICON_W  32
#define DEF_ICON_H  32

static void draw_default_disk(uint32_t *buf)
{
    /* Floppy disk shape: white rectangle with black border, grey label area */
    fill_rect_px(buf, 4, 2, 24, 28, PEN_WHITE);
    draw_rect_px(buf, 4, 2, 24, 28, PEN_BLACK);
    /* Metal slider area (top) */
    fill_rect_px(buf, 16, 2, 12, 8, PEN_GREY);
    draw_rect_px(buf, 16, 2, 12, 8, PEN_BLACK);
    /* Label area */
    fill_rect_px(buf, 7, 14, 18, 12, PEN_GREY);
    draw_rect_px(buf, 7, 14, 18, 12, PEN_BLACK);
    /* Label lines */
    for (int i = 0; i < 3; i++)
        fill_rect_px(buf, 9, 16 + i * 3, 14, 1, PEN_BLACK);
}

static void draw_default_drawer(uint32_t *buf)
{
    /* Folder shape: grey body with black border, tab on top-left */
    /* Tab */
    fill_rect_px(buf, 4, 4, 10, 4, PEN_GREY);
    draw_rect_px(buf, 4, 4, 10, 4, PEN_BLACK);
    /* Body */
    fill_rect_px(buf, 4, 8, 24, 20, PEN_GREY);
    draw_rect_px(buf, 4, 8, 24, 20, PEN_BLACK);
    /* Inner highlight */
    draw_rect_px(buf, 6, 10, 20, 16, PEN_BLACK);
}

static void draw_default_tool(uint32_t *buf)
{
    /* Generic tool: white page with black border and folded corner */
    fill_rect_px(buf, 6, 2, 20, 28, PEN_WHITE);
    draw_rect_px(buf, 6, 2, 20, 28, PEN_BLACK);
    /* Folded corner (top-right) */
    fill_rect_px(buf, 20, 2, 6, 6, PEN_GREY);
    /* Fold line */
    for (int i = 0; i < 6; i++)
        set_pixel(buf, 20 + i, 2 + i, PEN_BLACK);
    /* Gear icon in center */
    fill_rect_px(buf, 12, 12, 8, 8, PEN_BLACK);
    fill_rect_px(buf, 14, 14, 4, 4, PEN_WHITE);
}

static void draw_default_project(uint32_t *buf)
{
    /* Project file: white page with folded corner, lines */
    fill_rect_px(buf, 8, 2, 16, 28, PEN_WHITE);
    draw_rect_px(buf, 8, 2, 16, 28, PEN_BLACK);
    /* Folded corner (bottom-right) */
    fill_rect_px(buf, 20, 24, 4, 6, PEN_GREY);
    for (int i = 0; i < 6; i++)
        set_pixel(buf, 20 + i - 2, 24 + i, PEN_BLACK);
    /* Text lines */
    for (int i = 0; i < 4; i++)
        fill_rect_px(buf, 11, 6 + i * 4, 8, 1, PEN_BLACK);
}

static void draw_default_garbage(uint32_t *buf)
{
    /* Trashcan: grey can with black border, lid on top */
    /* Lid */
    fill_rect_px(buf, 6, 4, 20, 3, PEN_GREY);
    draw_rect_px(buf, 6, 4, 20, 3, PEN_BLACK);
    /* Handle */
    fill_rect_px(buf, 14, 2, 4, 3, PEN_GREY);
    draw_rect_px(buf, 14, 2, 4, 3, PEN_BLACK);
    /* Body */
    fill_rect_px(buf, 8, 7, 16, 22, PEN_GREY);
    draw_rect_px(buf, 8, 7, 16, 22, PEN_BLACK);
    /* Vertical lines on body */
    for (int i = 0; i < 4; i++)
        fill_rect_px(buf, 11 + i * 4, 9, 1, 18, PEN_BLACK);
}

static void draw_default_device(uint32_t *buf)
{
    /* Device: grey box with black border and indicator LED */
    fill_rect_px(buf, 4, 8, 24, 18, PEN_GREY);
    draw_rect_px(buf, 4, 8, 24, 18, PEN_BLACK);
    /* LED */
    fill_rect_px(buf, 22, 12, 3, 3, PEN_BLACK);
    /* Slot lines */
    for (int i = 0; i < 3; i++)
        fill_rect_px(buf, 7, 12 + i * 4, 12, 1, PEN_BLACK);
}

static void draw_default_kick(uint32_t *buf)
{
    /* Kickstart: chip shape */
    fill_rect_px(buf, 8, 8, 16, 16, PEN_WHITE);
    draw_rect_px(buf, 8, 8, 16, 16, PEN_BLACK);
    /* Pins */
    for (int i = 0; i < 4; i++) {
        fill_rect_px(buf, 10 + i * 4, 4, 2, 4, PEN_BLACK);
        fill_rect_px(buf, 10 + i * 4, 24, 2, 4, PEN_BLACK);
    }
    /* Dot */
    fill_rect_px(buf, 10, 10, 2, 2, PEN_BLACK);
}

void Icon_MakeDefault(ParsedIcon *out, uint8_t type, const char *label)
{
    if (!out) return;
    memset(out, 0, sizeof(ParsedIcon));

    out->type = type;
    out->pos_x = 0;
    out->pos_y = 0;
    out->image.width = DEF_ICON_W;
    out->image.height = DEF_ICON_H;
    out->image.depth = 2;
    out->image.has_selected = 0;

    /* Fill normal image with transparent */
    for (int i = 0; i < ICON_MAX_WIDTH * ICON_MAX_HEIGHT; i++) {
        out->image.normal[i] = PEN_TRANSPARENT;
        out->image.selected[i] = PEN_TRANSPARENT;
    }

    /* Draw type-specific icon */
    switch (type) {
        case WB_DISK:     draw_default_disk(out->image.normal); break;
        case WB_DRAWER:   draw_default_drawer(out->image.normal); break;
        case WB_TOOL:     draw_default_tool(out->image.normal); break;
        case WB_PROJECT:  draw_default_project(out->image.normal); break;
        case WB_GARBAGE:  draw_default_garbage(out->image.normal); break;
        case WB_DEVICE:   draw_default_device(out->image.normal); break;
        case WB_KICK:     draw_default_kick(out->image.normal); break;
        default:          draw_default_project(out->image.normal); break;
    }

    /* Selected = inverted (swap white/black, keep grey) */
    for (int i = 0; i < ICON_MAX_WIDTH * ICON_MAX_HEIGHT; i++) {
        uint32_t p = out->image.normal[i];
        if (p == PEN_WHITE)       out->image.selected[i] = PEN_BLACK;
        else if (p == PEN_BLACK)  out->image.selected[i] = PEN_WHITE;
        else                      out->image.selected[i] = p;
    }
    out->image.has_selected = 1;

    /* Copy label */
    if (label) {
        int i = 0;
        while (label[i] && i < ICON_MAX_LABEL - 1) {
            out->label[i] = label[i];
            i++;
        }
        out->label[i] = '\0';
    }
}
