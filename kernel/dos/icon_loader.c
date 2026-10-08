/*
 * icon_loader.c — Amiga .info Icon File Parser
 *
 * Reads classic planar .info icons from the VFS and converts them
 * into native ARGB bitmaps suitable for the linear framebuffer.
 */

#include "icon_loader.h"
#include "vfs.h"
#include "ramfs.h"
#include "../display/framebuffer.h"
#include "../exec/icon_lib.h"
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

    /* Icon pens index the Workbench screen palette directly.  WB 3.x
     * screen order is pen 0 = grey backdrop (transparent for icons),
     * pen 1 = black, pen 2 = white, pen 3 = blue (#3B67A2).  Pens 4-7
     * use the WB 3.x eight-colour extension set for depth-3 images. */
    const uint32_t pens[8] = {
        0x00000000,                    /* 0: transparent           */
        0xFF000000,                    /* 1: black                 */
        0xFFFFFFFF,                    /* 2: white                 */
        0xFF000000u | WB_BLUE,         /* 3: WB blue (screen pen 3)*/
        0xFF7B7B7B,                    /* 4: dark grey  #7B7B7B    */
        0xFFAFAFAF,                    /* 5: light grey #AFAFAF    */
        0xFFAA907C,                    /* 6: brown/tan  #AA907C    */
        0xFFFFA997,                    /* 7: salmon     #FFA997    */
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

    /* Real classic .info format via the shared parser (UAOS-253):
     * 78-byte DiskObject header, optional DrawerData/Image sections at
     * computed file offsets, length-prefixed strings. */
    static uint8_t buf[32768];
    uint32_t size = Icon_ReadInfo(path, buf, sizeof(buf));
    if (!size) return 0;

    IconMeta m;
    if (!Icon_ParseBuf(buf, size, &m)) {
        fprintf(stderr, "[ICON] Parse failed for %s.info\n", path);
        return 0;
    }

    out->type        = m.type;
    out->pos_x       = (int16_t)m.cur_x;
    out->pos_y       = (int16_t)m.cur_y;
    out->stack_size  = m.stack_size;

    int i = 0;
    while (i < ICON_MAX_LABEL - 1 && m.default_tool[i]) {
        out->default_tool[i] = m.default_tool[i]; i++;
    }
    out->default_tool[i] = '\0';
    i = 0;
    while (i < ICON_MAX_LABEL - 1 && m.tool_window[i]) {
        out->tool_window[i] = m.tool_window[i]; i++;
    }
    out->tool_window[i] = '\0';

    int tt = m.tooltype_count;
    if (tt > ICON_MAX_TOOLTYPES) tt = ICON_MAX_TOOLTYPES;
    for (i = 0; i < tt; i++) {
        memcpy(out->tool_types[i], m.tooltypes[i], ICON_MAX_TOOLTYPE_LEN);
    }
    out->tool_type_count = tt;

    /* Image records: 20-byte Image header (LeftEdge/TopEdge/Width/
     * Height/Depth/ImageData/PlanePick/PlaneOnOff/NextImage) followed
     * directly by the planar data. */
    if (m.img_off) {
        uint16_t n_w     = get_u16(buf, (int)m.img_off + 4);
        uint16_t n_h     = get_u16(buf, (int)m.img_off + 6);
        uint16_t n_depth = get_u16(buf, (int)m.img_off + 8);
        uint32_t data_off = m.img_off + ICON_IMAGE_HDR;

        if (n_w > 0 && n_w <= ICON_MAX_WIDTH && n_h > 0 &&
            n_h <= ICON_MAX_HEIGHT && n_depth > 0 &&
            n_depth <= ICON_MAX_PLANES) {
            out->image.width  = n_w;
            out->image.height = n_h;
            out->image.depth  = n_depth;
            planar_to_argb(buf + data_off, out->image.normal,
                           n_w, n_h, n_depth, 0x00000000);

            if (m.sel_off) {
                uint16_t s_w     = get_u16(buf, (int)m.sel_off + 4);
                uint16_t s_h     = get_u16(buf, (int)m.sel_off + 6);
                uint16_t s_depth = get_u16(buf, (int)m.sel_off + 8);
                if (s_w == n_w && s_h == n_h && s_depth == n_depth) {
                    planar_to_argb(buf + m.sel_off + ICON_IMAGE_HDR,
                                   out->image.selected,
                                   s_w, s_h, s_depth, 0x00000000);
                    out->image.has_selected = 1;
                }
            }
            if (!out->image.has_selected)
                memcpy(out->image.selected, out->image.normal,
                       sizeof(out->image.normal));
        }
    }

    /* Derive label from base filename */
    const char *base = path;
    int last_slash = -1;
    for (i = 0; path[i]; i++) {
        if (path[i] == '/' || path[i] == ':') last_slash = i;
    }
    if (last_slash >= 0) base = path + last_slash + 1;

    int li = 0;
    while (li < ICON_MAX_LABEL - 1 && base[li] && base[li] != '.') {
        out->label[li] = base[li];
        li++;
    }
    out->label[li] = '\0';

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
 *   1 = black  (0xFF000000)
 *   2 = white  (0xFFFFFFFF)
 *   3 = detail (WB blue pen)
 * ========================================================================= */

static uint8_t argb_to_pen(uint32_t argb)
{
    if ((argb & 0xFF000000) == 0) return 0; /* transparent */
    uint32_t r = (argb >> 16) & 0xFF;
    uint32_t g = (argb >> 8)  & 0xFF;
    uint32_t b =  argb        & 0xFF;
    if (r > 200 && g > 200 && b > 200) return 2; /* white */
    if (r < 50  && g < 50  && b < 50)  return 1; /* black */
    return 3; /* blue/detail */
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

    uint32_t tt_count = (uint32_t)icon->tool_type_count;
    if (tt_count > ICON_MAX_TOOLTYPES) tt_count = ICON_MAX_TOOLTYPES;
    int has_deftool = icon->default_tool[0] != '\0';
    int has_toolwin = icon->tool_window[0] != '\0';

    /* Real classic .info layout (UAOS-253):
     *   0-77:  DiskObject header (magic+version+44B embedded Gadget+fields)
     *   78+:   normal Image (20B hdr + planes), selected Image likewise,
     *          default tool (u32 len incl. NUL + bytes),
     *          tooltypes (u32 (count+1)*4 + len-prefixed strings),
     *          tool window (len-prefixed)
     * Pointer fields in the header are presence flags on disk. */
    uint32_t img_rec = (w > 0) ? ICON_IMAGE_HDR + img_data_size : 0;
    uint32_t p = ICON_HDR_SIZE;
    uint32_t img1_off = 0, img2_off = 0, deftool_off = 0,
             tt_off = 0, tw_off = 0;
    if (w > 0)            { img1_off = p; p += img_rec; }
    if (w > 0)            { img2_off = p; p += img_rec; }
    if (has_deftool)      { deftool_off = p; p += 4 + strlen(icon->default_tool) + 1; }
    if (tt_count) {
        tt_off = p; p += 4;
        for (uint32_t i = 0; i < tt_count; i++)
            p += 4 + strlen(icon->tool_types[i]) + 1;
    }
    if (has_toolwin)      { tw_off = p; p += 4 + strlen(icon->tool_window) + 1; }
    uint32_t total_size = p;

    static uint8_t buf[8192];
    if (total_size > sizeof(buf)) return 0;
    memset(buf, 0, total_size);

    /* DiskObject header */
    put_u16(buf, 0, WB_DISKOBJECT_MAGIC);
    put_u16(buf, 2, WB_DISKVERSION);
    /* Embedded Gadget (44 bytes at offset 4) */
    put_u16(buf, ICON_GAD_LEFT,   0);
    put_u16(buf, ICON_GAD_TOP,    0);
    put_u16(buf, ICON_GAD_WIDTH,  w);
    put_u16(buf, ICON_GAD_HEIGHT, h);
    put_u16(buf, ICON_GAD_FLAGS,  0x0003);       /* GADGHIMAGE */
    put_u16(buf, ICON_GAD_TYPE,   GTYP_CUSTOM);
    put_u32(buf, ICON_GAD_RENDER, w > 0 ? 1 : 0);
    put_u32(buf, ICON_GAD_SELREN, w > 0 ? 1 : 0);
    /* Type / pad / presence flags / position / stack */
    buf[ICON_TYPE_OFF] = icon->type;
    buf[ICON_TYPE_OFF + 1] = 0;
    put_u32(buf, ICON_DEFTOOL_OFF, has_deftool ? 1 : 0);
    put_u32(buf, ICON_TTYPES_OFF,  tt_count ? 1 : 0);
    put_u32(buf, ICON_CURX_OFF, (uint32_t)(int32_t)icon->pos_x);
    put_u32(buf, ICON_CURY_OFF, (uint32_t)(int32_t)icon->pos_y);
    put_u32(buf, ICON_DRAWER_OFF, 0);
    put_u32(buf, ICON_TOOLWIN_OFF, has_toolwin ? 1 : 0);
    put_u32(buf, ICON_STACK_OFF,
            (uint32_t)(icon->stack_size ? icon->stack_size : 4096));

    /* Image records — 20B header (ImageData/NextImage = stale markers) */
    for (int which = 0; which < 2; which++) {
        uint32_t io = which ? img2_off : img1_off;
        if (!io) continue;
        put_u16(buf, io + 0, 0);                    /* LeftEdge  */
        put_u16(buf, io + 2, 0);                    /* TopEdge   */
        put_u16(buf, io + 4, w);
        put_u16(buf, io + 6, h);
        put_u16(buf, io + 8, depth);
        put_u32(buf, io + 10, 1);                   /* ImageData (marker) */
        buf[io + 14] = 0;                           /* PlanePick */
        buf[io + 15] = 0;                           /* PlaneOnOff */
        put_u32(buf, io + 16, 0);                   /* NextImage */
        const uint32_t *src = which ? icon->image.selected
                                    : icon->image.normal;
        if (which && !icon->image.has_selected)
            src = icon->image.normal;
        argb_to_planar(src, buf + io + ICON_IMAGE_HDR, w, h, depth);
    }

    /* Default tool — u32 length (incl NUL) + bytes */
    if (deftool_off) {
        uint32_t dl = strlen(icon->default_tool) + 1;
        put_u32(buf, deftool_off, dl);
        memcpy(buf + deftool_off + 4, icon->default_tool, dl);
    }

    /* Tool types — u32 (count+1)*4 then len-prefixed strings */
    if (tt_off) {
        put_u32(buf, tt_off, (tt_count + 1) * 4);
        uint32_t q = tt_off + 4;
        for (uint32_t i = 0; i < tt_count; i++) {
            uint32_t sl = strlen(icon->tool_types[i]) + 1;
            put_u32(buf, q, sl);
            memcpy(buf + q + 4, icon->tool_types[i], sl);
            q += 4 + sl;
        }
    }

    if (tw_off) {
        uint32_t tl = strlen(icon->tool_window) + 1;
        put_u32(buf, tw_off, tl);
        memcpy(buf + tw_off + 4, icon->tool_window, tl);
    }

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
    static uint8_t buf[32768];
    if (size > sizeof(buf) || size < ICON_HDR_SIZE) {
        VFS_Close(&fh);
        return 0;
    }
    uint32_t rd = VFS_Read(&fh, buf, size);
    VFS_Close(&fh);
    if (rd < ICON_HDR_SIZE) return 0;

    /* Update position fields (real format: s32 at 58/62) */
    put_u32(buf, ICON_CURX_OFF, (uint32_t)(int32_t)x);
    put_u32(buf, ICON_CURY_OFF, (uint32_t)(int32_t)y);

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
#define PEN_WHITE        (0xFF000000u | WB_WHITE)
#define PEN_BLACK        (0xFF000000u | WB_BLACK)
#define PEN_GREY         (0xFF000000u | WB_GREY)

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
