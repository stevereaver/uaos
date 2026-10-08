/*
 * icon_lib.c — icon.library ROM module + real .info format (UAOS-253)
 *
 * Classic Amiga .info files begin with a 78-byte DiskObject header
 * (magic 0xE310 + version + embedded 44-byte Gadget + presence-flag
 * pointer fields + position + stack size).  Optional sections follow in
 * fixed order: DrawerData (56 B), normal Image record, selected Image
 * record, default-tool string, tooltype block, tool-window string and —
 * for OS 2.x+ icons (gadget UserData bit 0) — a 6-byte DrawerData2 tail.
 * Strings are ULONG-length-prefixed (length includes the NUL); the
 * tooltype block starts with a ULONG holding (count+1)*4.
 *
 * The guest-visible DiskObject uses the identical 78-byte header layout
 * (#pragma pack(2) in the NDK), so the file header is copied verbatim
 * and its stale pointer fields are patched to real guest addresses.
 */

#include "icon_lib.h"
#include "rom_modules.h"
#include "icon_def.h"
#include "../dos/vfs.h"
#include "../../emulation/uaos_emu.h"
#include <stddef.h>
#include <string.h>

/* From dos_lib.c — guest heap (free-list) allocators shared by all
 * guest-visible allocations so FreeDiskObject can really free them. */
extern void dos_AllocMem_glue(uint32_t size, uint32_t reqs, uint32_t *out);
extern void dos_FreeMem_glue(uint32_t addr, uint32_t size);
extern const char *m68k_cur_cwd(void);
#define MEMF_PUBLIC_G 1u

extern void kprint(const char *);

/* -----------------------------------------------------------------------
 * Big-endian helpers — host buffers
 * ----------------------------------------------------------------------- */
static uint16_t rb16(const uint8_t *p, uint32_t o)
{ return (uint16_t)((p[o] << 8) | p[o + 1]); }
static uint32_t rb32(const uint8_t *p, uint32_t o)
{ return ((uint32_t)p[o] << 24) | ((uint32_t)p[o + 1] << 16) |
         ((uint32_t)p[o + 2] << 8) | (uint32_t)p[o + 3]; }
static void wb16(uint8_t *p, uint32_t o, uint16_t v)
{ p[o] = (uint8_t)(v >> 8); p[o + 1] = (uint8_t)v; }
static void wb32(uint8_t *p, uint32_t o, uint32_t v)
{ p[o] = (uint8_t)(v >> 24); p[o + 1] = (uint8_t)(v >> 16);
  p[o + 2] = (uint8_t)(v >> 8); p[o + 3] = (uint8_t)v; }

/* Guest RAM helpers */
static uint32_t gr32(uint32_t a)
{ return ((uint32_t)g_ram[a] << 24) | ((uint32_t)g_ram[a + 1] << 16) |
         ((uint32_t)g_ram[a + 2] << 8) | (uint32_t)g_ram[a + 3]; }
static void gw32(uint32_t a, uint32_t v)
{ g_ram[a] = (uint8_t)(v >> 24); g_ram[a + 1] = (uint8_t)(v >> 16);
  g_ram[a + 2] = (uint8_t)(v >> 8); g_ram[a + 3] = (uint8_t)v; }

/* -----------------------------------------------------------------------
 * Icon_ParseBuf — positional parse of a real .info buffer
 * ----------------------------------------------------------------------- */
static int read_text(const uint8_t *d, uint32_t size, uint32_t *pos,
                     char *out, uint32_t max)
{
    if (*pos + 4 > size) return 0;
    uint32_t len = rb32(d, *pos);
    *pos += 4;
    if (len > size - *pos || len > 4096) return 0;
    uint32_t n = len;
    if (n && d[*pos + n - 1] == 0) n--;       /* len includes NUL */
    if (n >= max) n = max - 1;
    for (uint32_t i = 0; i < n; i++) out[i] = (char)d[*pos + i];
    out[n] = '\0';
    *pos += len;
    return 1;
}

/* Returns plane-data size for an Image header at file offset `off`, or
 * 0 on invalid geometry. */
static uint32_t image_planes_size(const uint8_t *d, uint32_t size,
                                  uint32_t off)
{
    if (off + ICON_IMAGE_HDR > size) return 0;
    int32_t w = (int16_t)rb16(d, off + 4);
    int32_t h = (int16_t)rb16(d, off + 6);
    int32_t depth = (int16_t)rb16(d, off + 8);
    if (w <= 0 || h <= 0 || depth <= 0 || depth > 8 ||
        w > 512 || h > 512) return 0;
    uint32_t row = ((uint32_t)w + 15u) / 16u * 2u;
    uint32_t need = ICON_IMAGE_HDR + row * (uint32_t)h * (uint32_t)depth;
    if (off + need > size) return 0;
    return need;
}

int Icon_ParseBuf(const uint8_t *d, uint32_t size, IconMeta *m)
{
    memset(m, 0, sizeof(*m));
    if (!d || size < ICON_HDR_SIZE) return 0;
    if (rb16(d, 0) != ICON_MAGIC) return 0;

    m->type          = d[ICON_TYPE_OFF];
    m->default_tool[0] = 0;
    uint32_t has_deftool  = rb32(d, ICON_DEFTOOL_OFF);
    uint32_t has_ttypes   = rb32(d, ICON_TTYPES_OFF);
    m->cur_x         = (int32_t)rb32(d, ICON_CURX_OFF);
    m->cur_y         = (int32_t)rb32(d, ICON_CURY_OFF);
    uint32_t has_dd  = rb32(d, ICON_DRAWER_OFF);
    uint32_t has_tw  = rb32(d, ICON_TOOLWIN_OFF);
    m->stack_size    = (int32_t)rb32(d, ICON_STACK_OFF);
    m->gadget_render = rb32(d, ICON_GAD_RENDER);
    m->select_render = rb32(d, ICON_GAD_SELREN);
    m->userdata      = rb32(d, ICON_GAD_USERDATA);

    uint32_t pos = ICON_HDR_SIZE;

    if (has_dd) {
        if (pos + ICON_DRAWERDATA_FILE > size) return 0;
        m->has_drawer = 1;
        m->drawer_off = pos;
        pos += ICON_DRAWERDATA_FILE;
    }

    if (m->gadget_render) {
        uint32_t n = image_planes_size(d, size, pos);
        if (!n) return m->ok = 1, 1;          /* tolerate bad image */
        m->img_off = pos;
        m->img_end = pos + n;
        pos += n;
    }
    if (m->select_render) {
        uint32_t n = image_planes_size(d, size, pos);
        if (!n) { m->select_render = 0; goto tail; }
        m->sel_off = pos;
        m->sel_end = pos + n;
        pos += n;
    }
tail:
    if (has_deftool && pos + 4 <= size)
        read_text(d, size, &pos, m->default_tool, sizeof(m->default_tool));

    if (has_ttypes && pos + 4 <= size) {
        uint32_t count_field = rb32(d, pos);
        pos += 4;
        int entries = (count_field >= 4) ? (int)(count_field / 4) - 1 : 0;
        for (int i = 0; i < entries && i < ICON_MAX_TT; i++) {
            if (!read_text(d, size, &pos,
                           m->tooltypes[m->tooltype_count],
                           ICON_MAX_TT_LEN))
                break;
            m->tooltype_count++;
        }
    }

    if (has_tw && pos + 4 <= size)
        read_text(d, size, &pos, m->tool_window, sizeof(m->tool_window));

    /* DrawerData2 tail: flags(4) + viewmodes(2) — OS 2.x drawers only */
    if (has_dd && (m->userdata & 0xFF) && pos + 6 <= size) {
        m->has_dd2      = 1;
        m->dd2_flags    = rb32(d, pos);
        m->dd2_viewmodes = rb16(d, pos + 4);
    }

    m->ok = 1;
    return 1;
}

/* -----------------------------------------------------------------------
 * VFS helpers
 * ----------------------------------------------------------------------- */

/* Build "<path>.info" into dst. */
static int info_path(const char *path, char *dst, int max)
{
    int i = 0;
    while (path && path[i] && i < max - 6) { dst[i] = path[i]; i++; }
    dst[i] = '\0';
    /* Don't double-append when the caller already named the .info */
    if (i >= 5 && dst[i - 5] == '.' &&
        (dst[i - 4] == 'i' || dst[i - 4] == 'I') &&
        (dst[i - 3] == 'n' || dst[i - 3] == 'N') &&
        (dst[i - 2] == 'f' || dst[i - 2] == 'F') &&
        (dst[i - 1] == 'o' || dst[i - 1] == 'O'))
        return 1;
    const char *s = ".info";
    for (int j = 0; j < 5; j++) dst[i++] = s[j];
    dst[i] = '\0';
    return 1;
}

/* Resolve a possibly relative/assign'd path into a full VFS path.
 * Returns dst (or NULL when nothing could be produced). */
static const char *resolve_info_path(const char *name, char *dst, int max)
{
    if (!name || !name[0]) return NULL;
    /* Bare leaf names resolve against the current task cwd. */
    int has_colon = 0;
    for (int i = 0; name[i]; i++) if (name[i] == ':') { has_colon = 1; break; }
    if (!has_colon) {
        const char *cwd = m68k_cur_cwd();
        int i = 0;
        while (cwd && cwd[i] && i < max - 2) { dst[i] = cwd[i]; i++; }
        if (i && dst[i - 1] != ':' && dst[i - 1] != '/' && i < max - 1)
            dst[i++] = '/';
        int j = 0;
        while (name[j] && i < max - 1) dst[i++] = name[j++];
        dst[i] = '\0';
        name = dst;
    }
    return name;
}

uint32_t Icon_ReadInfo(const char *path, uint8_t *buf, uint32_t max)
{
    char ipath[160];
    if (!info_path(path, ipath, sizeof(ipath))) return 0;

    char full[160];
    const char *resolved = VFS_ExpandAssigns(ipath, full, sizeof(full));
    if (resolved) { /* resolved already written to full */ }
    else { int i = 0; while (ipath[i] && i < 159) { full[i] = ipath[i]; i++; } full[i] = 0; }

    VfsFile fh;
    if (!VFS_Open(&fh, full, VFS_READ)) return 0;
    uint32_t size = VFS_Size(&fh);
    if (!size || size > max) { VFS_Close(&fh); return 0; }
    uint32_t n = VFS_Read(&fh, buf, size);
    VFS_Close(&fh);
    return n;
}

int Icon_LoadMeta(const char *path, IconMeta *out)
{
    static uint8_t buf[32768];
    uint32_t n = Icon_ReadInfo(path, buf, sizeof(buf));
    if (!n) return 0;
    return Icon_ParseBuf(buf, n, out);
}

/* -----------------------------------------------------------------------
 * Guest DiskObject construction
 *
 * One guest allocation holds everything so FreeDiskObject is a single
 * free:  [78B DiskObject][DrawerData 62B][img rec][sel rec]
 *        [tooltype ptr array][C strings...]
 * ----------------------------------------------------------------------- */

/* Track allocations per window so FreeDiskObject can release them. */
typedef struct { uint8_t *win; uint32_t addr; uint32_t size; } DobjAlloc;
static DobjAlloc g_dobjs[32];
static int       g_dobj_count = 0;

static void dobj_track(uint32_t addr, uint32_t size)
{
    if (g_dobj_count >= 32) return;
    DobjAlloc *e = &g_dobjs[g_dobj_count++];
    e->win = g_ram; e->addr = addr; e->size = size;
}

static int dobj_untrack(uint32_t addr)
{
    for (int i = 0; i < g_dobj_count; i++)
        if (g_dobjs[i].win == g_ram && g_dobjs[i].addr == addr) {
            g_dobjs[i] = g_dobjs[--g_dobj_count];
            return 1;
        }
    return 0;
}

static uint32_t guest_cstr_write(uint32_t at, const char *s)
{
    uint32_t i = 0;
    while (s && s[i]) { g_ram[at + i] = (uint8_t)s[i]; i++; }
    g_ram[at + i] = 0;
    return at;
}

static uint32_t build_guest_dobj(const uint8_t *raw, uint32_t raw_size,
                                 const IconMeta *m)
{
    uint32_t img_len  = (m->img_off && m->img_end > m->img_off)
                        ? m->img_end - m->img_off : 0;
    uint32_t sel_len  = (m->sel_off && m->sel_end > m->sel_off)
                        ? m->sel_end - m->sel_off : 0;
    uint32_t dd_len   = m->has_drawer ? ICON_DRAWERDATA_MEM : 0;
    uint32_t tt_arr   = ((uint32_t)m->tooltype_count + 1) * 4;

    uint32_t str_bytes = 0;
    if (m->default_tool[0]) str_bytes += strlen(m->default_tool) + 1;
    for (int i = 0; i < m->tooltype_count; i++)
        str_bytes += strlen(m->tooltypes[i]) + 1;
    if (m->tool_window[0]) str_bytes += strlen(m->tool_window) + 1;

    uint32_t total = ICON_HDR_SIZE + dd_len + img_len + sel_len +
                     tt_arr + str_bytes + 4;
    uint32_t base = 0;
    dos_AllocMem_glue(total, MEMF_PUBLIC_G, &base);
    if (!base) return 0;

    /* DiskObject header — copy the file header verbatim, then patch the
     * presence/stale-pointer fields with real guest addresses. */
    for (uint32_t i = 0; i < ICON_HDR_SIZE; i++)
        g_ram[base + i] = raw[i];

    uint32_t p = base + ICON_HDR_SIZE;

    /* DrawerData: file keeps OldDrawerData(56); memory adds flags+view */
    if (m->has_drawer) {
        for (uint32_t i = 0; i < ICON_DRAWERDATA_FILE; i++)
            g_ram[p + i] = raw[m->drawer_off + i];
        gw32(p + 56, m->has_dd2 ? m->dd2_flags : 0);
        g_ram[p + 60] = (uint8_t)(m->dd2_viewmodes >> 8);
        g_ram[p + 61] = (uint8_t)(m->dd2_viewmodes);
        gw32(base + ICON_DRAWER_OFF, p);
        p += dd_len;
    } else {
        gw32(base + ICON_DRAWER_OFF, 0);
    }

    /* Images: 20-byte Image header + plane data.  Patch ImageData (+10)
     * and NextImage (+16) in the guest copy. */
    if (img_len) {
        for (uint32_t i = 0; i < img_len; i++)
            g_ram[p + i] = raw[m->img_off + i];
        gw32(p + 10, p + ICON_IMAGE_HDR);    /* ImageData -> planes */
        gw32(p + 16, 0);                     /* NextImage */
        gw32(base + ICON_GAD_RENDER, p);     /* ga_GadgetRender */
        p += img_len;
    } else {
        gw32(base + ICON_GAD_RENDER, 0);
    }
    if (sel_len) {
        for (uint32_t i = 0; i < sel_len; i++)
            g_ram[p + i] = raw[m->sel_off + i];
        gw32(p + 10, p + ICON_IMAGE_HDR);
        gw32(p + 16, 0);
        gw32(base + ICON_GAD_SELREN, p);
        p += sel_len;
    } else {
        gw32(base + ICON_GAD_SELREN, 0);
    }

    /* ToolTypes: array of STRPTR + NULL terminator, then strings. */
    uint32_t tt_base = p;
    p += tt_arr;
    for (int i = 0; i < m->tooltype_count; i++) {
        gw32(tt_base + (uint32_t)i * 4, p);
        uint32_t n = strlen(m->tooltypes[i]) + 1;
        for (uint32_t j = 0; j < n; j++)
            g_ram[p + j] = (uint8_t)m->tooltypes[i][j];
        p += n;
    }
    gw32(tt_base + (uint32_t)m->tooltype_count * 4, 0);
    gw32(base + ICON_TTYPES_OFF,
         m->tooltype_count ? tt_base : 0);

    gw32(base + ICON_DEFTOOL_OFF,
         m->default_tool[0] ? guest_cstr_write(p, m->default_tool) : 0);
    if (m->default_tool[0]) p += strlen(m->default_tool) + 1;

    gw32(base + ICON_TOOLWIN_OFF,
         m->tool_window[0] ? guest_cstr_write(p, m->tool_window) : 0);
    if (m->tool_window[0]) p += strlen(m->tool_window) + 1;

    /* NextGadget/GadgetText/SpecialInfo must not carry stale file junk —
     * zero the ones that would alias guest pointers. */
    gw32(base + ICON_GADGET_OFF + 0, 0);      /* ga_NextGadget */
    gw32(base + ICON_GADGET_OFF + 26, 0);     /* ga_GadgetText */
    gw32(base + ICON_GADGET_OFF + 34, 0);     /* ga_SpecialInfo */

    dobj_track(base, total);
    return base;
}

/* -----------------------------------------------------------------------
 * Guest string / path helpers
 * ----------------------------------------------------------------------- */
static int gstr(uint32_t addr, char *out, uint32_t max)
{
    if (!addr || addr >= GUEST_RAM_SIZE) return 0;
    uint32_t i = 0;
    while (i < max - 1 && addr + i < GUEST_RAM_SIZE && g_ram[addr + i]) {
        out[i] = (char)g_ram[addr + i];
        i++;
    }
    out[i] = '\0';
    return (int)i;
}

static int ci_eq_n(const char *a, const char *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        if (!ca) return 1;
    }
    return 1;
}

/* -----------------------------------------------------------------------
 * icon.library LVO handlers (dispatched via LIB_ROM / emu_rom_call)
 * ----------------------------------------------------------------------- */

/* GetDiskObject(name=A0) → D0 DiskObject* */
static void icon_GetDiskObject(M68kCPUState *cpu)
{
    char name[160], resolved[160], ipath[176];
    gstr(cpu->a[0], name, sizeof(name));
    static uint8_t raw[32768];

    const char *p = resolve_info_path(name, resolved, sizeof(resolved));
    uint32_t n = p ? Icon_ReadInfo(p, raw, sizeof(raw)) : 0;
    if (!n) {
        /* Guest may hand us an already-suffixed or cwd-relative name the
         * first pass missed — try the raw name too. */
        if (info_path(name, ipath, sizeof(ipath)))
            n = Icon_ReadInfo(ipath, raw, sizeof(raw));
    }
    if (!n) { cpu->d[0] = 0; return; }

    IconMeta m;
    if (!Icon_ParseBuf(raw, n, &m)) { cpu->d[0] = 0; return; }
    cpu->d[0] = build_guest_dobj(raw, n, &m);
}

/* FreeDiskObject(diskobj=A0) */
static void icon_FreeDiskObject(M68kCPUState *cpu)
{
    uint32_t d = cpu->a[0];
    if (!d || !dobj_untrack(d)) return;
    dos_FreeMem_glue(d, 0);
}

/* FindToolType(toolTypeArray=A0, typeName=A1) → D0 STRPTR or 0 */
static void icon_FindToolType(M68kCPUState *cpu)
{
    uint32_t arr = cpu->a[0];
    char key[80];
    if (!arr || !gstr(cpu->a[1], key, sizeof(key))) { cpu->d[0] = 0; return; }
    uint32_t klen = strlen(key);

    for (uint32_t i = 0; i < 256; i++) {
        uint32_t sp = gr32(arr + i * 4);
        if (!sp) break;
        char tt[ICON_MAX_TT_LEN];
        gstr(sp, tt, sizeof(tt));
        /* Match the key part: "NAME", "NAME=v" — '=' or NUL ends the key.
         * Leading/trailing spaces are skipped per icon autodoc examples. */
        const char *t = tt;
        while (*t == ' ') t++;
        uint32_t kl = 0;
        while (t[kl] && t[kl] != '=') kl++;
        /* Trim trailing spaces in the key */
        while (kl && t[kl - 1] == ' ') kl--;
        if (kl == klen && ci_eq_n(t, key, klen)) {
            cpu->d[0] = sp;
            return;
        }
    }
    cpu->d[0] = 0;
}

/* MatchToolValue(typeString=A0, value=A1) → D0 BOOL
 * typeString may be "KEY" or "KEY=v1|v2|v3". */
static void icon_MatchToolValue(M68kCPUState *cpu)
{
    char tt[ICON_MAX_TT_LEN], val[96];
    gstr(cpu->a[0], tt, sizeof(tt));
    gstr(cpu->a[1], val, sizeof(val));
    cpu->d[0] = 0;

    const char *s = tt;
    while (*s && *s != '=') s++;
    if (*s == '=') s++;
    /* Walk |-separated alternatives */
    while (*s) {
        const char *e = s;
        while (*e && *e != '|') e++;
        uint32_t alen = (uint32_t)(e - s);
        uint32_t vlen = strlen(val);
        if (alen == vlen && ci_eq_n(s, val, vlen)) {
            cpu->d[0] = 1;
            return;
        }
        s = (*e == '|') ? e + 1 : e;
    }
}

/* -----------------------------------------------------------------------
 * PutDiskObject — write a guest DiskObject back to disk in real format
 * ----------------------------------------------------------------------- */

static uint32_t wr_str(uint8_t *b, uint32_t p, uint32_t gstr_addr)
{
    uint32_t len = 0;
    while (gstr_addr + len < GUEST_RAM_SIZE && g_ram[gstr_addr + len] &&
           len < 4095) len++;
    len++;                                   /* include NUL */
    wb32(b, p, len);
    for (uint32_t i = 0; i < len; i++)
        b[p + 4 + i] = g_ram[gstr_addr + i];
    return p + 4 + len;
}

/* Serialise one guest Image record (20B header + plane data). */
static uint32_t wr_image(uint8_t *b, uint32_t p, uint32_t img)
{
    int32_t w = (int16_t)((g_ram[img + 4] << 8) | g_ram[img + 5]);
    int32_t h = (int16_t)((g_ram[img + 6] << 8) | g_ram[img + 7]);
    int32_t depth = (int16_t)((g_ram[img + 8] << 8) | g_ram[img + 9]);
    if (w <= 0 || h <= 0 || depth <= 0 || depth > 8 || w > 512 || h > 512)
        return p;
    for (int i = 0; i < (int)ICON_IMAGE_HDR; i++)
        b[p + i] = g_ram[img + i];
    p += ICON_IMAGE_HDR;
    uint32_t planes = gr32(img + 10);
    uint32_t psz = ((uint32_t)w + 15u) / 16u * 2u * (uint32_t)h *
                   (uint32_t)depth;
    for (uint32_t i = 0; i < psz && planes + i < GUEST_RAM_SIZE; i++)
        b[p + i] = g_ram[planes + i];
    return p + psz;
}

/* PutDiskObject(name=A0, diskobj=A1) → D0 BOOL */
static void icon_PutDiskObject(M68kCPUState *cpu)
{
    char name[160], resolved[160], ipath[176], full[192];
    gstr(cpu->a[0], name, sizeof(name));
    uint32_t d = cpu->a[1];
    cpu->d[0] = 0;
    if (!d || d + ICON_HDR_SIZE > GUEST_RAM_SIZE) return;

    const char *p = resolve_info_path(name, resolved, sizeof(resolved));
    if (!p) return;
    info_path(p, ipath, sizeof(ipath));
    const char *fp = VFS_ExpandAssigns(ipath, full, sizeof(full));
    if (!fp) return;

    static uint8_t buf[24576];

    /* Header verbatim; presence flags written as the file section
     * offsets so downstream parsers see non-zero markers. */
    for (uint32_t i = 0; i < ICON_HDR_SIZE; i++) buf[i] = g_ram[d + i];
    uint32_t p_img  = gr32(d + ICON_GAD_RENDER);
    uint32_t p_sel  = gr32(d + ICON_GAD_SELREN);
    uint32_t p_dd   = gr32(d + ICON_DRAWER_OFF);
    uint32_t p_tt   = gr32(d + ICON_TTYPES_OFF);
    uint32_t p_def  = gr32(d + ICON_DEFTOOL_OFF);
    uint32_t p_tw   = gr32(d + ICON_TOOLWIN_OFF);
    uint32_t ud     = gr32(d + ICON_GAD_USERDATA);

    uint32_t w = ICON_HDR_SIZE;
    wb32(buf, ICON_GAD_RENDER,  p_img ? 1 : 0);
    wb32(buf, ICON_GAD_SELREN,  p_sel ? 1 : 0);
    wb32(buf, ICON_DEFTOOL_OFF, p_def ? 1 : 0);
    wb32(buf, ICON_TTYPES_OFF,  p_tt  ? 1 : 0);
    wb32(buf, ICON_DRAWER_OFF,  p_dd  ? 1 : 0);
    wb32(buf, ICON_TOOLWIN_OFF, p_tw  ? 1 : 0);

    if (p_dd) {
        for (uint32_t i = 0; i < ICON_DRAWERDATA_FILE; i++)
            buf[w + i] = g_ram[p_dd + i];
        w += ICON_DRAWERDATA_FILE;
    }
    /* wr_image writes header + plane data contiguously and returns the
     * offset past the record — images serialise back-to-back. */
    if (p_img) w = wr_image(buf, w, p_img);
    if (p_sel) w = wr_image(buf, w, p_sel);

    if (p_def) w = wr_str(buf, w, p_def);

    if (p_tt) {
        uint32_t count = 0;
        while (count < 256 && gr32(p_tt + count * 4)) count++;
        wb32(buf, w, (count + 1) * 4);
        w += 4;
        for (uint32_t i = 0; i < count; i++)
            w = wr_str(buf, w, gr32(p_tt + i * 4));
    }
    if (p_tw) w = wr_str(buf, w, p_tw);
    if (p_dd && (ud & 0xFF)) {
        wb32(buf, w, gr32(p_dd + 56));
        wb16(buf, w + 4,
             (uint16_t)((g_ram[p_dd + 60] << 8) | g_ram[p_dd + 61]));
        w += 6;
    }

    VfsFile fh;
    if (!VFS_Open(&fh, fp, VFS_WRITE | VFS_CREATE | VFS_TRUNC)) return;
    uint32_t n = VFS_Write(&fh, buf, w);
    VFS_Close(&fh);
    cpu->d[0] = (n == w) ? 1u : 0u;
}

/* GetDefDiskObject(type=D0) → D0 — minimal guest DiskObject, no image. */
static void icon_GetDefDiskObject(M68kCPUState *cpu)
{
    uint32_t total = ICON_HDR_SIZE + 8;
    uint32_t base = 0;
    dos_AllocMem_glue(total, MEMF_PUBLIC_G, &base);
    if (!base) { cpu->d[0] = 0; return; }
    for (uint32_t i = 0; i < total; i++) g_ram[base + i] = 0;
    wb16(g_ram, base + 0, ICON_MAGIC);
    wb16(g_ram, base + 2, 1);
    g_ram[base + ICON_TYPE_OFF] = (uint8_t)cpu->d[0];
    wb16(g_ram, base + ICON_GAD_TYPE, 1);
    wb32(g_ram, base + ICON_STACK_OFF, 4096);
    dobj_track(base, total);
    cpu->d[0] = base;
}

/* PutDefDiskObject(diskobj=A0) — persist as the default for its type.
 * UAOS keeps defaults procedural; accept and discard. */
static void icon_PutDefDiskObject(M68kCPUState *cpu)
{
    (void)cpu;
    cpu->d[0] = 1;
}

/* GetDiskObjectNew(name=A0) — V36+: same lookup as GetDiskObject; the
 * missing-icon default-image fallback is cosmetic, guests handle NULL. */
static void icon_GetDiskObjectNew(M68kCPUState *cpu)
{
    icon_GetDiskObject(cpu);
}

/* DeleteDiskObject(name=A0) → D0 BOOL */
static void icon_DeleteDiskObject(M68kCPUState *cpu)
{
    char name[160], resolved[160], ipath[176];
    gstr(cpu->a[0], name, sizeof(name));
    const char *p = resolve_info_path(name, resolved, sizeof(resolved));
    if (!p) { cpu->d[0] = 0; return; }
    info_path(p, ipath, sizeof(ipath));
    cpu->d[0] = VFS_Delete(ipath) == 0 ? 1u : 0u;
}

/* BumpRevision(newname=A0, oldname=A1) → D0 newname or 0.
 * Copies oldname's icon to "Copy_of_<newname>".info. */
static void icon_BumpRevision(M68kCPUState *cpu)
{
    char nn[128], on[160], res[160], ipath[176], npath[176];
    gstr(cpu->a[0], nn, sizeof(nn));
    gstr(cpu->a[1], on, sizeof(on));
    const char *p = resolve_info_path(on, res, sizeof(res));
    static uint8_t raw[32768];
    uint32_t n = p ? Icon_ReadInfo(p, raw, sizeof(raw)) : 0;
    if (!n) { cpu->d[0] = 0; return; }

    /* newname gets a "Copy_of_" prefix for its icon (classic behaviour:
     * the icon file created next to <newname>). */
    const char *np = resolve_info_path(nn, res, sizeof(res));
    if (!np) { cpu->d[0] = 0; return; }
    info_path(np, ipath, sizeof(ipath));
    /* Build "<dir>/Copy_of_<leaf>.info" */
    int i = 0, last = -1;
    while (ipath[i]) { if (ipath[i] == '/' || ipath[i] == ':') last = i; i++; }
    int head = last + 1, o = 0;
    for (int k = 0; k < head; k++) npath[o++] = ipath[k];
    const char *pre = "Copy_of_";
    for (int k = 0; pre[k]; k++) npath[o++] = pre[k];
    for (int k = head; ipath[k] && o < (int)sizeof(npath) - 1; k++)
        npath[o++] = ipath[k];
    npath[o] = '\0';

    VfsFile fh;
    if (!VFS_Open(&fh, npath, VFS_WRITE | VFS_CREATE | VFS_TRUNC)) {
        cpu->d[0] = 0; return;
    }
    VFS_Write(&fh, raw, n);
    VFS_Close(&fh);
    cpu->d[0] = cpu->a[0];
}

/* -----------------------------------------------------------------------
 * Registration — LVO map matches the classic icon_lib.fd (bias 30)
 * ----------------------------------------------------------------------- */

static void *icon_funcs[] = {
    icon_GetDiskObject,     /* 1 */
    icon_PutDiskObject,     /* 2 */
    icon_FreeDiskObject,    /* 3 */
    icon_FindToolType,      /* 4 */
    icon_MatchToolValue,    /* 5 */
    icon_BumpRevision,      /* 6 */
    icon_GetDefDiskObject,  /* 7 */
    icon_PutDefDiskObject,  /* 8 */
    icon_GetDiskObjectNew,  /* 9 */
    icon_DeleteDiskObject,  /* 10 */
};

static const UaosRomLvo icon_lvo_map[] = {
    { -78,  1 },   /* GetDiskObject    */
    { -84,  2 },   /* PutDiskObject    */
    { -90,  3 },   /* FreeDiskObject   */
    { -96,  4 },   /* FindToolType     */
    { -102, 5 },   /* MatchToolValue   */
    { -108, 6 },   /* BumpRevision     */
    { -120, 7 },   /* GetDefDiskObject */
    { -126, 8 },   /* PutDefDiskObject */
    { -132, 9 },   /* GetDiskObjectNew */
    { -138, 10 },  /* DeleteDiskObject */
};

void UAOS_ICON_Register(void)
{
    UAOS_ROM_Register("icon.library", 44, 0,
                      (uint16_t)(sizeof(icon_funcs) / sizeof(icon_funcs[0])),
                      icon_funcs);
    UAOS_ROM_BindLvoMap("icon.library", icon_lvo_map,
                        (uint16_t)(sizeof(icon_lvo_map) /
                                   sizeof(icon_lvo_map[0])));
}
