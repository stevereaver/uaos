/*
 * gadtools_lib.c — UAOS gadtools.library implementation
 *
 * Provides AmigaOS-compatible GadTools gadget creation, layout helpers,
 * VisualInfo management, and menu stubs.  Gadgets are built as standard
 * Intuition gadgets so the existing intuition_lib rendering and event
 * paths handle them without extra per-type code.
 */

#include "exec/gadtools_lib.h"
#include "exec/rom_modules.h"
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

/* Guest RAM (provided by uaos_m68k_glue.c) */
extern uint8_t *g_ram;
#define GUEST_RAM_SIZE (2 * 1024 * 1024)

/* Musashi register access (provided by uaos_m68k_glue.c) */
extern unsigned int m68k_get_reg(void *context, int reg);
extern void         m68k_set_reg(int reg, unsigned int value);

#define M68K_REG_D0  0
#define M68K_REG_D1  1
#define M68K_REG_D2  2
#define M68K_REG_D3  3
#define M68K_REG_A0  8
#define M68K_REG_A1  9
#define M68K_REG_A2  10
#define M68K_REG_A7  15

/* =========================================================================
 * Local guest memory helpers
 * ========================================================================= */
static inline uint8_t  gt_u8 (uint32_t addr) { return g_ram[addr]; }
static inline uint16_t gt_u16(uint32_t addr)
    { return (uint16_t)((g_ram[addr] << 8) | g_ram[addr + 1]); }
static inline int16_t  gt_s16(uint32_t addr)
    { return (int16_t)gt_u16(addr); }
static inline uint32_t gt_u32(uint32_t addr)
{
    return ((uint32_t)g_ram[addr]     << 24) |
           ((uint32_t)g_ram[addr + 1] << 16) |
           ((uint32_t)g_ram[addr + 2] <<  8) |
           ((uint32_t)g_ram[addr + 3]      );
}
static inline int32_t gt_s32(uint32_t addr)
    { return (int32_t)gt_u32(addr); }
static inline void gt_w8 (uint32_t addr, uint8_t  v) { g_ram[addr] = v; }
static inline void gt_w16(uint32_t addr, uint16_t v)
{
    g_ram[addr]     = (uint8_t)(v >> 8);
    g_ram[addr + 1] = (uint8_t)v;
}
static inline void gt_w32(uint32_t addr, uint32_t v)
{
    g_ram[addr]     = (uint8_t)(v >> 24);
    g_ram[addr + 1] = (uint8_t)(v >> 16);
    g_ram[addr + 2] = (uint8_t)(v >>  8);
    g_ram[addr + 3] = (uint8_t)v;
}

static void gt_guest_str(char *dst, uint32_t src, int max)
{
    int i = 0;
    while (i < max - 1 && src + i < GUEST_RAM_SIZE) {
        uint8_t c = g_ram[src + i];
        if (c == 0) break;
        dst[i] = (char)c;
        i++;
    }
    dst[i] = '\0';
}

static uint32_t count_label_array(uint32_t labels)
{
    uint32_t count = 0;
    if (!labels) return 0;
    while (labels + count * 4 < GUEST_RAM_SIZE) {
        uint32_t p = gt_u32(labels + count * 4);
        if (!p) break;
        count++;
        if (count > 256) break;
    }
    return count;
}

/* =========================================================================
 * Small libc helpers (freestanding kernel has no snprintf/strtol)
 * ========================================================================= */
static int gt_itoa_decimal(int32_t val, char *buf, int max)
{
    if (max < 2) return 0;
    if (val == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return 1;
    }
    int neg = 0;
    if (val < 0) { neg = 1; val = -val; }
    char tmp[16];
    int n = 0;
    while (val > 0 && n < 16) {
        tmp[n++] = (char)('0' + (val % 10));
        val /= 10;
    }
    int pos = 0;
    if (neg) {
        if (pos < max - 1) buf[pos++] = '-';
    }
    for (int i = n - 1; i >= 0 && pos < max - 1; i--) buf[pos++] = tmp[i];
    buf[pos] = '\0';
    return pos;
}

static int32_t gt_strtol(const char *s)
{
    int32_t sign = 1;
    int32_t val = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; }
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (int32_t)(*s - '0');
        s++;
    }
    return sign * val;
}

/* =========================================================================
 * Tag parsing
 * ========================================================================= */
static uint32_t find_tag_data(uint32_t tag_list, uint32_t tag, uint32_t def)
{
    if (!tag_list) return def;
    uint32_t p = tag_list;
    while (p + 8 <= GUEST_RAM_SIZE) {
        uint32_t t = gt_u32(p);
        uint32_t d = gt_u32(p + 4);
        if (t == TAG_DONE) break;
        if (t == tag) return d;
        p += 8;
    }
    return def;
}

/* =========================================================================
 * VisualInfo / DrawInfo helpers
 * ========================================================================= */
static uint32_t alloc_screen_draw_info(uint32_t screen)
{
    if (!screen) return 0;
    uint32_t dri = intu_alloc(DRINFO_SIZE);
    if (!dri) return 0;
    for (int i = 0; i < DRINFO_SIZE; i++) gt_w8(dri + i, 0);

    uint8_t detail = gt_u8(screen + SCR_OFF_DETAILPEN);
    uint8_t block  = gt_u8(screen + SCR_OFF_BLOCKPEN);
    uint32_t font  = gt_u32(screen + SCR_OFF_FONT);

    static const uint16_t default_pens[DRINFO_PEN_COUNT] = {
        0, 1, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 1, 1
    };

    gt_w16(dri + DRINFO_OFF_VERSION, 1);
    gt_w16(dri + DRINFO_OFF_NUMPENS, DRINFO_PEN_COUNT);
    for (int i = 0; i < DRINFO_PEN_COUNT; i++) {
        uint16_t pen = default_pens[i];
        if (pen == 0) pen = detail;
        else if (pen == 1) pen = block;
        gt_w16(dri + DRINFO_OFF_PENS + i * 2, pen);
    }
    gt_w32(dri + DRINFO_OFF_FONT, font);
    gt_w8(dri + DRINFO_OFF_DEPTH, 2);
    gt_w16(dri + DRINFO_OFF_RESX, 72);
    gt_w16(dri + DRINFO_OFF_RESY, 72);
    gt_w32(dri + DRINFO_OFF_FLAGS, 0);
    return dri;
}

/* =========================================================================
 * Gadget allocation helper
 * ========================================================================= */
static uint32_t alloc_gadget(uint32_t next, int16_t left, int16_t top,
                             int16_t width, int16_t height,
                             uint16_t type, uint16_t id,
                             uint16_t flags, uint16_t activation,
                             uint32_t special, uint32_t text,
                             uint32_t userdata)
{
    uint32_t gad = intu_alloc(GAD_SIZE);
    if (!gad) return 0;

    gt_w32(gad + GAD_OFF_NEXTGADGET,  next);
    gt_w16(gad + GAD_OFF_LEFTEDGE,    (uint16_t)left);
    gt_w16(gad + GAD_OFF_TOPEDGE,     (uint16_t)top);
    gt_w16(gad + GAD_OFF_WIDTH,       (uint16_t)width);
    gt_w16(gad + GAD_OFF_HEIGHT,      (uint16_t)height);
    gt_w16(gad + GAD_OFF_FLAGS,       flags);
    gt_w16(gad + GAD_OFF_ACTIVATION,  activation);
    gt_w16(gad + GAD_OFF_GADGETTYPE,  type);
    gt_w32(gad + GAD_OFF_GADGETRENDER, 0);
    gt_w32(gad + GAD_OFF_SELECTRENDER, 0);
    gt_w32(gad + GAD_OFF_GADGETTEXT,  text);
    gt_w32(gad + GAD_OFF_MUTUALEXCLUDE, 0);
    gt_w32(gad + GAD_OFF_SPECIALINFO, special);
    gt_w16(gad + GAD_OFF_GADGETID,    id);
    gt_w32(gad + GAD_OFF_USERDATA,    userdata);
    return gad;
}

/* =========================================================================
 * Label handling: create an IntuiText label from a NewGadget string.
 * ========================================================================= */
static uint32_t create_label_itext(const char *text, uint32_t text_attr,
                                   uint8_t front, uint8_t back)
{
    (void)text_attr;
    if (!text || !text[0]) return 0;
    uint32_t len = (uint32_t)strlen(text) + 1;
    uint32_t str = intu_alloc(len);
    if (!str) return 0;
    for (uint32_t i = 0; i < len; i++) gt_w8(str + i, (uint8_t)text[i]);

    uint32_t it = intu_alloc(ITEXT_SIZE);
    if (!it) { intu_free(str); return 0; }
    for (int i = 0; i < ITEXT_SIZE; i++) gt_w8(it + i, 0);
    gt_w8(it + ITEXT_OFF_FRONTPEN, front);
    gt_w8(it + ITEXT_OFF_BACKPEN, back);
    gt_w8(it + ITEXT_OFF_DRAWMODE, 1); /* JAM1 */
    gt_w32(it + ITEXT_OFF_FONT, 0);
    gt_w32(it + ITEXT_OFF_ITEXT, str);
    gt_w32(it + ITEXT_OFF_NEXTTEXT, 0);
    return it;
}

/* =========================================================================
 * CreateGadgetA — the main GadTools gadget factory.
 * ========================================================================= */
static uint32_t create_boolean_kind(uint32_t prev, uint32_t ng, uint32_t tags,
                                    uint16_t gadget_type, uint32_t mutual)
{
    int16_t left  = gt_s16(ng + NG_OFF_LEFTEDGE);
    int16_t top   = gt_s16(ng + NG_OFF_TOPEDGE);
    int16_t w     = gt_s16(ng + NG_OFF_WIDTH);
    int16_t h     = gt_s16(ng + NG_OFF_HEIGHT);
    uint16_t id   = gt_u16(ng + NG_OFF_GADGETID);
    uint32_t ng_flags = gt_u32(ng + NG_OFF_FLAGS);
    uint32_t text_ptr = gt_u32(ng + NG_OFF_GADGETTEXT);

    uint16_t activation = GACT_IMMEDIATE | GACT_RELVERIFY;
    if (ng_flags & NG_TOGGLE) activation |= GACT_TOGGLESELECT;
    if (ng_flags & NG_LIVE) activation |= GACT_INTUITICKS;

    uint16_t flags = 0;
    if (ng_flags & NG_DISABLED) flags |= GFLG_DISABLED;

    uint32_t label = 0;
    if (text_ptr) {
        char text[80];
        gt_guest_str(text, text_ptr, sizeof(text));
        label = create_label_itext(text, gt_u32(ng + NG_OFF_TEXTATTR), 1, 0);
        if (label) flags |= GFLG_LABELITEXT;
    }

    uint32_t gad = alloc_gadget(prev, left, top, w, h,
                                gadget_type, id, flags, activation, 0, label, 0);
    if (gad && mutual) gt_w32(gad + GAD_OFF_MUTUALEXCLUDE, 1);
    return gad;
}

static uint32_t create_string_kind(uint32_t prev, uint32_t ng, uint32_t tags,
                                   int integer)
{
    int16_t left  = gt_s16(ng + NG_OFF_LEFTEDGE);
    int16_t top   = gt_s16(ng + NG_OFF_TOPEDGE);
    int16_t w     = gt_s16(ng + NG_OFF_WIDTH);
    int16_t h     = gt_s16(ng + NG_OFF_HEIGHT);
    uint16_t id   = gt_u16(ng + NG_OFF_GADGETID);
    uint32_t ng_flags = gt_u32(ng + NG_OFF_FLAGS);

    uint16_t maxchars = (uint16_t)find_tag_data(tags,
        integer ? GTIN_MaxChars : GTST_MaxChars, 32);
    if (maxchars < 4) maxchars = 4;
    if (maxchars > 256) maxchars = 256;

    uint32_t si = intu_alloc(SI_SIZE);
    if (!si) return 0;
    for (int i = 0; i < SI_SIZE; i++) gt_w8(si + i, 0);

    uint32_t buf = intu_alloc(maxchars + 1);
    if (!buf) { intu_free(si); return 0; }
    for (uint32_t i = 0; i <= maxchars; i++) gt_w8(buf + i, 0);

    gt_w32(si + SI_OFF_BUFFER, buf);
    gt_w32(si + SI_OFF_UNDOBUFFER, 0);
    gt_w16(si + SI_OFF_BUFFERPOS, 0);
    gt_w16(si + SI_OFF_MAXCHARS, maxchars);
    gt_w16(si + SI_OFF_DISPPOS, 0);
    gt_w16(si + SI_OFF_NUMCHARS, 0);
    gt_w32(si + SI_OFF_MIN, 0);
    gt_w32(si + SI_OFF_MAX, 0);

    if (integer) {
        int32_t num = (int32_t)find_tag_data(tags, GTIN_Number, 0);
        gt_w32(si + SI_OFF_MIN, -32768);
        gt_w32(si + SI_OFF_MAX,  32767);
        /* Convert initial number to text buffer. */
        char tmp[16];
        int n = gt_itoa_decimal(num, tmp, sizeof(tmp));
        if (n < 0) n = 0;
        if ((uint16_t)n > maxchars) n = maxchars;
        for (int i = 0; i < n; i++) gt_w8(buf + i, (uint8_t)tmp[i]);
        gt_w8(buf + n, 0);
        gt_w16(si + SI_OFF_NUMCHARS, (uint16_t)n);
    } else {
        uint32_t str_tag = find_tag_data(tags, GTST_String, 0);
        if (str_tag) {
            char tmp[256];
            gt_guest_str(tmp, str_tag, sizeof(tmp));
            int n = (int)strlen(tmp);
            if ((uint16_t)n > maxchars) n = maxchars;
            for (int i = 0; i < n; i++) gt_w8(buf + i, (uint8_t)tmp[i]);
            gt_w8(buf + n, 0);
            gt_w16(si + SI_OFF_NUMCHARS, (uint16_t)n);
        }
    }

    uint16_t activation = GACT_IMMEDIATE | GACT_RELVERIFY;
    uint16_t flags = 0;
    if (ng_flags & NG_DISABLED) flags |= GFLG_DISABLED;

    uint16_t type = integer ? GTYP_INTGADGET : GTYP_STRGADGET;
    return alloc_gadget(prev, left, top, w, h, type, id, flags, activation, si, 0, 0);
}

static uint32_t create_slider_kind(uint32_t prev, uint32_t ng, uint32_t tags)
{
    int16_t left  = gt_s16(ng + NG_OFF_LEFTEDGE);
    int16_t top   = gt_s16(ng + NG_OFF_TOPEDGE);
    int16_t w     = gt_s16(ng + NG_OFF_WIDTH);
    int16_t h     = gt_s16(ng + NG_OFF_HEIGHT);
    uint16_t id   = gt_u16(ng + NG_OFF_GADGETID);
    uint32_t ng_flags = gt_u32(ng + NG_OFF_FLAGS);

    int32_t min = (int32_t)find_tag_data(tags, GTSL_Min, 0);
    int32_t max = (int32_t)find_tag_data(tags, GTSL_Max, 100);
    int32_t lvl = (int32_t)find_tag_data(tags, GTSL_Level, 0);
    if (max < min) max = min;
    if (lvl < min) lvl = min;
    if (lvl > max) lvl = max;

    uint32_t pi = intu_alloc(PROP_SIZE);
    if (!pi) return 0;
    for (int i = 0; i < PROP_SIZE; i++) gt_w8(pi + i, 0);

    uint16_t pot = 0;
    if (max > min) pot = (uint16_t)(((lvl - min) * 65535) / (max - min));
    gt_w16(pi + PROP_OFF_HORIZPOT, pot);
    gt_w16(pi + PROP_OFF_VERTPOT, 0);
    gt_w16(pi + PROP_OFF_HORIZBODY, 0xFFFF);
    gt_w16(pi + PROP_OFF_VERTBODY, 0xFFFF);
    gt_w16(pi + PROP_OFF_WIDTH, 0);
    gt_w16(pi + PROP_OFF_HEIGHT, 0);
    gt_w16(pi + PROP_OFF_HORIZSIG, 0);
    gt_w16(pi + PROP_OFF_VERTSIG, 0);

    uint16_t activation = GACT_IMMEDIATE | GACT_RELVERIFY | GACT_INTUITICKS;
    uint16_t flags = 0;
    if (ng_flags & NG_DISABLED) flags |= GFLG_DISABLED;

    return alloc_gadget(prev, left, top, w, h, GTYP_PROPGADGET, id,
                        flags, activation, pi, 0, 0);
}

static uint32_t create_listview_kind(uint32_t prev, uint32_t ng, uint32_t tags)
{
    int16_t left  = gt_s16(ng + NG_OFF_LEFTEDGE);
    int16_t top   = gt_s16(ng + NG_OFF_TOPEDGE);
    int16_t w     = gt_s16(ng + NG_OFF_WIDTH);
    int16_t h     = gt_s16(ng + NG_OFF_HEIGHT);
    uint16_t id   = gt_u16(ng + NG_OFF_GADGETID);
    uint32_t ng_flags = gt_u32(ng + NG_OFF_FLAGS);

    uint32_t labels = find_tag_data(tags, GTLV_Labels, 0);
    uint32_t count = count_label_array(labels);
    uint32_t top_idx = find_tag_data(tags, GTLV_Top, 0);
    uint32_t selected = find_tag_data(tags, GTLV_Selected, 0);

    uint32_t lv = intu_alloc(LV_SIZE);
    if (!lv) return 0;
    for (int i = 0; i < LV_SIZE; i++) gt_w8(lv + i, 0);

    int visible = (h - 4) / 16;
    if (visible < 1) visible = 1;

    gt_w32(lv + LV_OFF_ITEMS, labels);
    gt_w32(lv + LV_OFF_COUNT, count);
    gt_w32(lv + LV_OFF_SELECTED, selected);
    gt_w32(lv + LV_OFF_VISIBLE, (uint32_t)visible);
    gt_w32(lv + LV_OFF_TOP, top_idx);
    gt_w32(lv + LV_OFF_MULTI_SELECT, 0);
    gt_w32(lv + LV_OFF_SELECTED_MASK, 0);

    uint16_t activation = GACT_IMMEDIATE | GACT_RELVERIFY;
    uint16_t flags = 0;
    if (ng_flags & NG_DISABLED) flags |= GFLG_DISABLED;

    return alloc_gadget(prev, left, top, w, h, GTYP_LISTVIEW, id,
                        flags, activation, lv, 0, 0);
}

static uint32_t create_cycle_kind(uint32_t prev, uint32_t ng, uint32_t tags)
{
    /* A cycle gadget is a boolean gadget that displays the active label. */
    uint32_t gad = create_boolean_kind(prev, ng, tags, GTYP_BOOLGADGET, 0);
    if (gad) {
        uint32_t labels = find_tag_data(tags, GTCY_Labels, 0);
        uint32_t active = find_tag_data(tags, GTCY_Active, 0);
        uint32_t count = count_label_array(labels);
        if (active >= count) active = 0;
        gt_w32(gad + GAD_OFF_USERDATA, (labels << 16) | (active & 0xFFFF));
        gt_w16(gad + GAD_OFF_ACTIVATION,
               GACT_IMMEDIATE | GACT_RELVERIFY | GACT_TOGGLESELECT);
    }
    return gad;
}

static void gadtools_CreateGadgetA(void)
{
    uint32_t kind = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t prev = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t ng   = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t tags = m68k_get_reg(NULL, M68K_REG_A2);

    uint32_t result = 0;

    if (kind >= NUM_KINDS || !ng) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }

    switch (kind) {
        case BUTTON_KIND:
            result = create_boolean_kind(prev, ng, tags, GTYP_BOOLGADGET, 0);
            break;
        case CHECKBOX_KIND:
            result = create_boolean_kind(prev, ng, tags, GTYP_BOOLGADGET, 0);
            if (result) {
                uint16_t a = gt_u16(result + GAD_OFF_ACTIVATION) | GACT_TOGGLESELECT;
                gt_w16(result + GAD_OFF_ACTIVATION, a);
            }
            break;
        case CYCLE_KIND:
            result = create_cycle_kind(prev, ng, tags);
            break;
        case MX_KIND:
            result = create_boolean_kind(prev, ng, tags, GTYP_BOOLGADGET, 1);
            break;
        case SLIDER_KIND:
            result = create_slider_kind(prev, ng, tags);
            break;
        case STRING_KIND:
            result = create_string_kind(prev, ng, tags, 0);
            break;
        case INTEGER_KIND:
            result = create_string_kind(prev, ng, tags, 1);
            break;
        case LISTVIEW_KIND:
            result = create_listview_kind(prev, ng, tags);
            break;
        case NUMBER_KIND:
        case TEXT_KIND:
            /* Display-only gadgets: render as a boolean with a label, no events. */
            result = create_boolean_kind(prev, ng, tags, GTYP_BOOLGADGET, 0);
            if (result) {
                gt_w16(result + GAD_OFF_ACTIVATION, 0);
                uint16_t f = gt_u16(result + GAD_OFF_FLAGS);
                f |= GFLG_DISABLED; /* non-interactive */
                gt_w16(result + GAD_OFF_FLAGS, f);
            }
            break;
        default:
            break;
    }

    if (result && prev) {
        /* Link the new gadget after the previous one. */
        gt_w32(result + GAD_OFF_NEXTGADGET, gt_u32(prev + GAD_OFF_NEXTGADGET));
        gt_w32(prev + GAD_OFF_NEXTGADGET, result);
    }

    m68k_set_reg(M68K_REG_D0, result);
}

/* =========================================================================
 * CreateContext
 * ========================================================================= */
static uint32_t gadtools_CreateContext(void)
{
    uint32_t glistptr = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t gad = 0;
    if (glistptr) {
        gad = intu_alloc(GAD_SIZE);
        if (gad) {
            for (int i = 0; i < GAD_SIZE; i++) gt_w8(gad + i, 0);
            gt_w16(gad + GAD_OFF_WIDTH, 1);
            gt_w16(gad + GAD_OFF_HEIGHT, 1);
            gt_w16(gad + GAD_OFF_FLAGS, GFLG_SYSGADGET | GFLG_DISABLED);
            gt_w16(gad + GAD_OFF_ACTIVATION, 0);
            gt_w16(gad + GAD_OFF_GADGETTYPE, GTYP_SYSGADGET);
            gt_w32(glistptr, gad);
        }
    }
    m68k_set_reg(M68K_REG_D0, gad);
    return gad;
}

/* =========================================================================
 * FreeGadgets
 * ========================================================================= */
static void free_gadget_chain(uint32_t gad)
{
    while (gad) {
        uint32_t next = gt_u32(gad + GAD_OFF_NEXTGADGET);
        uint32_t special = gt_u32(gad + GAD_OFF_SPECIALINFO);
        uint32_t label = gt_u32(gad + GAD_OFF_GADGETTEXT);

        if (label) {
            uint32_t text = gt_u32(label + ITEXT_OFF_ITEXT);
            if (text) intu_free(text);
            intu_free(label);
        }
        if (special) {
            uint16_t type = gt_u16(gad + GAD_OFF_GADGETTYPE) & 0x000F;
            if (type == GTYP_STRGADGET || type == GTYP_INTGADGET) {
                uint32_t buf = gt_u32(special + SI_OFF_BUFFER);
                if (buf) intu_free(buf);
            }
            intu_free(special);
        }
        intu_free(gad);
        gad = next;
    }
}

static void gadtools_FreeGadgets(void)
{
    uint32_t gad = m68k_get_reg(NULL, M68K_REG_A0);
    free_gadget_chain(gad);
}

/* =========================================================================
 * GT_SetGadgetAttrsA / GT_GetGadgetAttrsA
 * ========================================================================= */
static void gadtools_GT_SetGadgetAttrsA(void)
{
    uint32_t gad  = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t win  = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t req  = m68k_get_reg(NULL, M68K_REG_A2);
    uint32_t tags = m68k_get_reg(NULL, M68K_REG_D0);
    (void)win; (void)req;

    if (!gad) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }

    uint16_t type = gt_u16(gad + GAD_OFF_GADGETTYPE) & 0x000F;
    uint32_t special = gt_u32(gad + GAD_OFF_SPECIALINFO);

    uint32_t p = tags;
    while (p + 8 <= GUEST_RAM_SIZE) {
        uint32_t tag = gt_u32(p);
        uint32_t data = gt_u32(p + 4);
        if (tag == TAG_DONE) break;
        p += 8;

        switch (tag) {
            case GA_Disabled:
            case GT_Private0:
                if (data) {
                    gt_w16(gad + GAD_OFF_FLAGS,
                           gt_u16(gad + GAD_OFF_FLAGS) | GFLG_DISABLED);
                } else {
                    gt_w16(gad + GAD_OFF_FLAGS,
                           gt_u16(gad + GAD_OFF_FLAGS) & ~GFLG_DISABLED);
                }
                break;
            case GTST_String:
                if (type == GTYP_STRGADGET && special) {
                    uint32_t buf = gt_u32(special + SI_OFF_BUFFER);
                    uint16_t maxchars = gt_u16(special + SI_OFF_MAXCHARS);
                    if (buf && data) {
                        char tmp[256];
                        gt_guest_str(tmp, data, sizeof(tmp));
                        int n = (int)strlen(tmp);
                        if ((uint16_t)n > maxchars) n = maxchars;
                        for (int i = 0; i < n; i++)
                            gt_w8(buf + i, (uint8_t)tmp[i]);
                        gt_w8(buf + n, 0);
                        gt_w16(special + SI_OFF_NUMCHARS, (uint16_t)n);
                        gt_w16(special + SI_OFF_BUFFERPOS, (uint16_t)n);
                    }
                }
                break;
            case GTIN_Number:
                if (type == GTYP_INTGADGET && special) {
                    uint32_t buf = gt_u32(special + SI_OFF_BUFFER);
                    uint16_t maxchars = gt_u16(special + SI_OFF_MAXCHARS);
                    if (buf) {
                        char tmp[16];
                        int n = gt_itoa_decimal((int32_t)data, tmp, sizeof(tmp));
                        if (n < 0) n = 0;
                        if ((uint16_t)n > maxchars) n = maxchars;
                        for (int i = 0; i < n; i++)
                            gt_w8(buf + i, (uint8_t)tmp[i]);
                        gt_w8(buf + n, 0);
                        gt_w16(special + SI_OFF_NUMCHARS, (uint16_t)n);
                        gt_w16(special + SI_OFF_BUFFERPOS, (uint16_t)n);
                    }
                }
                break;
            case GTCY_Active:
                if (type == GTYP_BOOLGADGET) {
                    uint32_t userdata = gt_u32(gad + GAD_OFF_USERDATA);
                    uint32_t labels = userdata >> 16;
                    uint32_t count = count_label_array(labels);
                    if (data < count) {
                        gt_w32(gad + GAD_OFF_USERDATA, (labels << 16) | (data & 0xFFFF));
                    }
                }
                break;
            case GTLV_Selected:
                if (type == GTYP_LISTVIEW && special) {
                    uint32_t count = gt_u32(special + LV_OFF_COUNT);
                    if (data < count) gt_w32(special + LV_OFF_SELECTED, data);
                }
                break;
            case GTLV_Top:
                if (type == GTYP_LISTVIEW && special) {
                    gt_w32(special + LV_OFF_TOP, data);
                }
                break;
        }
    }

    m68k_set_reg(M68K_REG_D0, 1);
}

static void gadtools_GT_GetGadgetAttrsA(void)
{
    uint32_t gad  = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t win  = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t req  = m68k_get_reg(NULL, M68K_REG_A2);
    uint32_t tags = m68k_get_reg(NULL, M68K_REG_D0);
    (void)win; (void)req;

    int ok = 0;
    if (gad && tags) {
        uint16_t type = gt_u16(gad + GAD_OFF_GADGETTYPE) & 0x000F;
        uint32_t special = gt_u32(gad + GAD_OFF_SPECIALINFO);

        uint32_t p = tags;
        while (p + 8 <= GUEST_RAM_SIZE) {
            uint32_t tag = gt_u32(p);
            uint32_t store = gt_u32(p + 4);
            if (tag == TAG_DONE) break;
            p += 8;
            if (!store) continue;

            uint32_t value = 0;
            int matched = 0;
            switch (tag) {
                case GTST_String:
                    if (type == GTYP_STRGADGET && special) {
                        value = gt_u32(special + SI_OFF_BUFFER);
                        matched = 1;
                    }
                    break;
                case GTIN_Number:
                    if (type == GTYP_INTGADGET && special) {
                        uint32_t buf = gt_u32(special + SI_OFF_BUFFER);
                        if (buf) {
                            char tmp[32];
                            gt_guest_str(tmp, buf, sizeof(tmp));
                            value = (uint32_t)gt_strtol(tmp);
                            matched = 1;
                        }
                    }
                    break;
                case GTCY_Active:
                    if (type == GTYP_BOOLGADGET) {
                        value = gt_u32(gad + GAD_OFF_USERDATA) & 0xFFFF;
                        matched = 1;
                    }
                    break;
                case GTLV_Selected:
                    if (type == GTYP_LISTVIEW && special) {
                        value = gt_u32(special + LV_OFF_SELECTED);
                        matched = 1;
                    }
                    break;
                case GA_Disabled:
                    value = (gt_u16(gad + GAD_OFF_FLAGS) & GFLG_DISABLED) ? 1 : 0;
                    matched = 1;
                    break;
            }
            if (matched) {
                gt_w32(store, value);
                ok++;
            }
        }
    }

    m68k_set_reg(M68K_REG_D0, (uint32_t)ok);
}

/* =========================================================================
 * VisualInfo
 * ========================================================================= */
static void gadtools_GetVisualInfoA(void)
{
    uint32_t screen_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    (void)m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t result = 0;
    if (screen_ptr) {
        uint32_t vi = intu_alloc(GTVI_SIZE);
        if (vi) {
            for (int i = 0; i < GTVI_SIZE; i++) gt_w8(vi + i, 0);
            gt_w32(vi + GTVI_OFF_SCREEN, screen_ptr);
            uint32_t dri = alloc_screen_draw_info(screen_ptr);
            gt_w32(vi + GTVI_OFF_DRAWINFO, dri);
            gt_w32(vi + GTVI_OFF_FONT, gt_u32(screen_ptr + SCR_OFF_FONT));
            result = vi;
        }
    }
    m68k_set_reg(M68K_REG_D0, result);
}

static void gadtools_FreeVisualInfo(void)
{
    uint32_t vi = m68k_get_reg(NULL, M68K_REG_A0);
    if (vi) {
        uint32_t dri = gt_u32(vi + GTVI_OFF_DRAWINFO);
        if (dri) intu_free(dri);
        intu_free(vi);
    }
}

/* =========================================================================
 * DrawBevelBoxA
 * ========================================================================= */
static void gadtools_DrawBevelBoxA(void)
{
    /* Stub: guests normally call this to draw recessed/raised boxes.
     * For now just return success. */
    m68k_set_reg(M68K_REG_D0, 1);
}

/* =========================================================================
 * Menu stubs
 * ========================================================================= */
static void gadtools_CreateMenusA(void)
{
    m68k_set_reg(M68K_REG_D0, 0);
}
static void gadtools_FreeMenus(void)
{
    (void)m68k_get_reg(NULL, M68K_REG_A0);
}
static void gadtools_LayoutMenuItemsA(void)
{
    m68k_set_reg(M68K_REG_D0, 0);
}
static void gadtools_LayoutMenusA(void)
{
    m68k_set_reg(M68K_REG_D0, 0);
}

/* =========================================================================
 * Message wrappers (forward to exec.library)
 * ========================================================================= */
extern void UAOS_Exec_Dispatch(uint32_t fn);

static void gadtools_GT_GetIMsg(void)
{
    /* Forward to GetMsg(userPort).  A0 = port. */
    uint32_t port = m68k_get_reg(NULL, M68K_REG_A0);
    m68k_set_reg(M68K_REG_A0, port);
    UAOS_Exec_Dispatch(12); /* EXEC_GET_MSG */
}
static void gadtools_GT_ReplyIMsg(void)
{
    uint32_t msg = m68k_get_reg(NULL, M68K_REG_A1);
    m68k_set_reg(M68K_REG_A1, msg);
    UAOS_Exec_Dispatch(13); /* EXEC_REPLY_MSG */
}
static void gadtools_GT_RefreshWindow(void)
{
    (void)m68k_get_reg(NULL, M68K_REG_A0);
    (void)m68k_get_reg(NULL, M68K_REG_A1);
    m68k_set_reg(M68K_REG_D0, 1);
}
static void gadtools_GT_BeginRefresh(void)
{
    uint32_t win = m68k_get_reg(NULL, M68K_REG_A0);
    m68k_set_reg(M68K_REG_A0, win);
    UAOS_Intuition_Dispatch(89); /* INTUITION_BEGIN_REFRESH */
}
static void gadtools_GT_EndRefresh(void)
{
    uint32_t win = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t complete = m68k_get_reg(NULL, M68K_REG_D0);
    m68k_set_reg(M68K_REG_A0, win);
    m68k_set_reg(M68K_REG_D0, complete);
    UAOS_Intuition_Dispatch(90); /* INTUITION_END_REFRESH */
}
static void gadtools_GT_FilterIMsg(void)
{
    /* No filtering; return the message unchanged. */
    uint32_t imsg = m68k_get_reg(NULL, M68K_REG_A1);
    m68k_set_reg(M68K_REG_D0, imsg);
}
static void gadtools_GT_PostFilterIMsg(void)
{
    /* No filtering; return the message unchanged. */
    uint32_t imsg = m68k_get_reg(NULL, M68K_REG_A1);
    m68k_set_reg(M68K_REG_D0, imsg);
}

/* =========================================================================
 * Library open/close
 * ========================================================================= */
static void gadtools_OpenLibrary(void)
{
    m68k_set_reg(M68K_REG_D0, 1);
}
static void gadtools_CloseLibrary(void)
{
    /* no-op */
}

/* =========================================================================
 * Dispatch table and ROM registration
 * ========================================================================= */
static void *gadtools_funcs[] = {
    gadtools_OpenLibrary,
    gadtools_CloseLibrary,
    gadtools_CreateGadgetA,
    gadtools_FreeGadgets,
    gadtools_GT_SetGadgetAttrsA,
    gadtools_CreateMenusA,
    gadtools_FreeMenus,
    gadtools_LayoutMenuItemsA,
    gadtools_LayoutMenusA,
    gadtools_GT_GetIMsg,
    gadtools_GT_ReplyIMsg,
    gadtools_GT_RefreshWindow,
    gadtools_GT_BeginRefresh,
    gadtools_GT_EndRefresh,
    gadtools_GT_FilterIMsg,
    gadtools_GT_PostFilterIMsg,
    gadtools_CreateContext,
    gadtools_DrawBevelBoxA,
    gadtools_GetVisualInfoA,
    gadtools_FreeVisualInfo,
    gadtools_GT_GetGadgetAttrsA,
};

void UAOS_GADTOOLS_Dispatch(uint32_t fn)
{
    if (fn >= 1 && fn <= GADTOOLS_MAX_FUNC) {
        void (*f)(void) = gadtools_funcs[fn - 1];
        if (f) f();
    }
}

void UAOS_GADTOOLS_Register(void)
{
    UAOS_ROM_Register("gadtools.library", 39, 0x0000A000,
                      (uint16_t)(sizeof(gadtools_funcs) / sizeof(gadtools_funcs[0])),
                      gadtools_funcs);
}
