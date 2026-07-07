/*
 * boopsi_builtin.c — Built-in BOOPSI classes with native host dispatchers
 *
 * Standard AmigaOS classes implemented in host C so M68k programs can create
 * objects via NewObject() without shipping their own dispatcher code.
 * Currently implemented:
 *   - rootclass    (base OM_NEW/OM_DISPOSE/OM_SET/OM_GET/OM_ADDTAIL/OM_REMOVE)
 *   - gadgetclass  (GA_* attributes and stub GM_* methods)
 *   - imageclass   (IA_* attributes and IA_Data/IA_Left/IA_Top)
 *   - pointerclass (POINTERA_* attributes, decodes BitMap for WA_Pointer)
 *   - menuclass    (rootclass subclass, stub for menu support)
 *   - windowclass  (rootclass subclass, stub for window support)
 */

#include "exec/boopsi_builtin.h"
#include "exec/intuition_lib.h"
#include "exec/amiga_graphics.h"
#include "display/framebuffer.h"
#include <string.h>

/* Guest RAM access (mirrors intuition_lib.c helpers for fast local use) */
extern uint8_t *g_ram;
extern unsigned int m68k_get_reg(void *context, int reg);
extern void         m68k_set_reg(int reg, unsigned int value);
extern uint32_t     UAOS_InvokeM68kHook(uint32_t hook_ptr, uint32_t a0, uint32_t a1, uint32_t a2);
extern uint32_t     intu_alloc(uint32_t size);
extern void         intu_free(uint32_t user_addr);
extern void         FB_FillRect(int x, int y, int w, int h, uint32_t colour);
extern void         FB_DrawRect(int x, int y, int w, int h, uint32_t colour);
extern void         FB_PutStr(int x, int y, const char *s, uint32_t fg, uint32_t bg);
extern void         UAOS_Graphics_Dispatch(uint32_t fn);

#define GUEST_RAM_SIZE (2 * 1024 * 1024)

/* GM_* message offsets (AmigaOS 3.x) */
#define GMHT_OFF_GINFO   4
#define GMHT_OFF_MOUSEX  8
#define GMHT_OFF_MOUSEY  10
#define GMHT_OFF_FLAGS   12

#define GMR_OFF_GINFO    4
#define GMR_OFF_RPORT    8
#define GMR_OFF_REDRAW   12

#define GMI_OFF_GINFO    4
#define GMI_OFF_IEVENT   8
#define GMI_OFF_TERM     12
#define GMI_OFF_MOUSEX   16
#define GMI_OFF_MOUSEY   18
#define GMI_OFF_TABLET   20
#define GMI_OFF_FLAGS    24

#define GMGI_OFF_GINFO   4
#define GMGI_OFF_ABORT   8

#define GMR_NOREUSE   0
#define GMR_REUSE     1
#define GMR_MEACTIVE  2

static inline uint32_t mem_u32(uint32_t a)
{
    return ((uint32_t)g_ram[a] << 24) | ((uint32_t)g_ram[a + 1] << 16) |
           ((uint32_t)g_ram[a + 2] << 8) | (uint32_t)g_ram[a + 3];
}
static inline uint16_t mem_u16(uint32_t a)
{
    return (uint16_t)(((uint16_t)g_ram[a] << 8) | g_ram[a + 1]);
}
static inline int16_t mem_s16(uint32_t a) { return (int16_t)mem_u16(a); }
static inline void mem_w32(uint32_t a, uint32_t v)
{
    g_ram[a] = (uint8_t)(v >> 24); g_ram[a + 1] = (uint8_t)(v >> 16);
    g_ram[a + 2] = (uint8_t)(v >> 8); g_ram[a + 3] = (uint8_t)v;
}
static inline void mem_w16(uint32_t a, uint16_t v)
{
    g_ram[a] = (uint8_t)(v >> 8); g_ram[a + 1] = (uint8_t)v;
}

static void guest_str(char *dst, uint32_t src, int max)
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

static int same_str(const char *a, uint32_t b)
{
    if (!b || b >= GUEST_RAM_SIZE) return 0;
    int i = 0;
    while (a[i]) {
        if (g_ram[b + i] != (uint8_t)a[i]) return 0;
        i++;
    }
    return g_ram[b + i] == 0;
}

/* =========================================================================
 * Tag helpers
 * ========================================================================= */
static uint32_t find_tag(uint32_t taglist, uint32_t tag, uint32_t def)
{
    if (!taglist) return def;
    uint32_t p = taglist;
    while (p + 8 <= GUEST_RAM_SIZE) {
        uint32_t t = mem_u32(p);
        uint32_t d = mem_u32(p + 4);
        if (t == TAG_DONE) break;
        if (t == tag) return d;
        p += 8;
    }
    return def;
}

static int walk_tags(uint32_t taglist,
                     int (*cb)(uint32_t tag, uint32_t data, void *ctx),
                     void *ctx)
{
    if (!taglist) return 1;
    uint32_t p = taglist;
    while (p + 8 <= GUEST_RAM_SIZE) {
        uint32_t t = mem_u32(p);
        if (t == TAG_DONE) break;
        uint32_t d = mem_u32(p + 4);
        if (!cb(t, d, ctx)) return 0;
        p += 8;
    }
    return 1;
}

/* =========================================================================
 * rootclass
 * ========================================================================= */
static uint32_t rootclass_dispatch(uint32_t cls, uint32_t obj, uint32_t msg)
{
    (void)cls;
    uint32_t method = mem_u32(msg + MSG_OFF_METHODID);
    switch (method) {
        case OM_NEW:
            /* Object is already allocated; just succeed. */
            return 1;
        case OM_DISPOSE:
            return 1;
        case OM_SET:
            return 1;
        case OM_GET:
            return 0;
        case OM_ADDTAIL:
        case OM_REMOVE:
        case OM_ADDMEMBER:
        case OM_REMMEMBER:
        case OM_NOTIFY:
        case OM_UPDATE:
            return 1;
        default:
            return 0;
    }
}

/* =========================================================================
 * gadgetclass
 *
 * The gadgetclass object is laid out as a real AmigaOS Gadget structure so it
 * can be added directly to a window's FirstGadget list. The instance size is
 * GAD_SIZE (44) and the instance offset is 0.
 * ========================================================================= */

static uint32_t create_intuitext(const char *text, uint32_t text_attr)
{
    (void)text_attr;
    if (!text || !text[0]) return 0;
    uint32_t len = (uint32_t)strlen(text) + 1;
    uint32_t str = intu_alloc(len);
    if (!str) return 0;
    for (uint32_t i = 0; i < len; i++) g_ram[str + i] = (uint8_t)text[i];

    uint32_t it = intu_alloc(ITEXT_SIZE);
    if (!it) { intu_free(str); return 0; }
    for (int i = 0; i < ITEXT_SIZE; i++) g_ram[it + i] = 0;
    g_ram[it + ITEXT_OFF_FRONTPEN] = 1; /* text pen */
    g_ram[it + ITEXT_OFF_BACKPEN] = 0;  /* bg pen */
    g_ram[it + ITEXT_OFF_DRAWMODE] = 1; /* JAM1 */
    mem_w32(it + ITEXT_OFF_ITEXT, str);
    return it;
}

static int gadget_set_tag(uint32_t tag, uint32_t data, void *ctx)
{
    uint32_t obj = (uint32_t)(uintptr_t)ctx;
    switch (tag) {
        case GA_Left:       mem_w16(obj + GAD_OFF_LEFTEDGE, (uint16_t)(int16_t)data); break;
        case GA_RelRight:   break;
        case GA_Top:        mem_w16(obj + GAD_OFF_TOPEDGE, (uint16_t)(int16_t)data); break;
        case GA_RelBottom:  break;
        case GA_Width:      mem_w16(obj + GAD_OFF_WIDTH, (uint16_t)(int16_t)data); break;
        case GA_RelWidth:   break;
        case GA_Height:     mem_w16(obj + GAD_OFF_HEIGHT, (uint16_t)(int16_t)data); break;
        case GA_RelHeight:  break;
        case GA_Text:
        case GA_Label: {
            char text[80] = "";
            guest_str(text, data, sizeof(text));
            uint32_t it = create_intuitext(text, 0);
            if (it) {
                mem_w32(obj + GAD_OFF_GADGETTEXT, it);
                uint16_t f = mem_u16(obj + GAD_OFF_FLAGS);
                f &= ~(GFLG_LABELITEXT | GFLG_LABELSTRING | GFLG_LABELIMAGE);
                f |= GFLG_LABELITEXT;
                mem_w16(obj + GAD_OFF_FLAGS, f);
            }
            break;
        }
        case GA_Image: {
            mem_w32(obj + GAD_OFF_GADGETRENDER, data);
            break;
        }
        case GA_ID:         mem_w16(obj + GAD_OFF_GADGETID, (uint16_t)data); break;
        case GA_UserData:   mem_w32(obj + GAD_OFF_USERDATA, data); break;
        case GA_Disabled: {
            uint16_t f = mem_u16(obj + GAD_OFF_FLAGS);
            if (data) f |= GFLG_DISABLED;
            else f &= ~GFLG_DISABLED;
            mem_w16(obj + GAD_OFF_FLAGS, f);
            break;
        }
        case GA_Selected: {
            uint16_t f = mem_u16(obj + GAD_OFF_FLAGS);
            if (data) f |= GFLG_SELECTED;
            else f &= ~GFLG_SELECTED;
            mem_w16(obj + GAD_OFF_FLAGS, f);
            break;
        }
        case GA_Immediate: {
            uint16_t a = mem_u16(obj + GAD_OFF_ACTIVATION);
            if (data) a |= GACT_IMMEDIATE; else a &= ~GACT_IMMEDIATE;
            mem_w16(obj + GAD_OFF_ACTIVATION, a);
            break;
        }
        case GA_RelVerify: {
            uint16_t a = mem_u16(obj + GAD_OFF_ACTIVATION);
            if (data) a |= GACT_RELVERIFY; else a &= ~GACT_RELVERIFY;
            mem_w16(obj + GAD_OFF_ACTIVATION, a);
            break;
        }
        case GA_ToggleSelect: {
            uint16_t a = mem_u16(obj + GAD_OFF_ACTIVATION);
            if (data) a |= GACT_TOGGLESELECT; else a &= ~GACT_TOGGLESELECT;
            mem_w16(obj + GAD_OFF_ACTIVATION, a);
            break;
        }
    }
    return 1;
}

static int gadget_point_inside(uint32_t obj, int16_t mx, int16_t my)
{
    /* GM_HITTEST/GM_HANDLEINPUT mouse coordinates are relative to the
     * gadget's upper-left corner. */
    int16_t w = mem_s16(obj + GAD_OFF_WIDTH);
    int16_t h = mem_s16(obj + GAD_OFF_HEIGHT);
    return (mx >= 0 && mx < w && my >= 0 && my < h) ? 1 : 0;
}

static void rp_window_offset(uint32_t rport, int *wx, int *wy)
{
    *wx = 0; *wy = 0;
    if (!rport) return;
    uint32_t win = mem_u32(rport + RP_OFF_LAYER);
    if (!win) return;
    *wx = (int)mem_s16(win + WIN_OFF_LEFTEDGE);
    *wy = (int)mem_s16(win + WIN_OFF_TOPEDGE);
}

static void gadget_render(uint32_t obj, uint32_t rport, uint32_t redraw)
{
    (void)redraw;
    int16_t left = mem_s16(obj + GAD_OFF_LEFTEDGE);
    int16_t top  = mem_s16(obj + GAD_OFF_TOPEDGE);
    int16_t w    = mem_s16(obj + GAD_OFF_WIDTH);
    int16_t h    = mem_s16(obj + GAD_OFF_HEIGHT);
    uint16_t flags = mem_u16(obj + GAD_OFF_FLAGS);
    uint16_t activation = mem_u16(obj + GAD_OFF_ACTIVATION);

    int wx, wy;
    rp_window_offset(rport, &wx, &wy);
    int sx = wx + left;
    int sy = wy + top;

    uint32_t bg = (flags & GFLG_SELECTED) ? WB_BLUE : WB_GREY;
    uint32_t fg = (flags & GFLG_SELECTED) ? WB_WHITE : WB_BLACK;
    uint32_t dis = (flags & GFLG_DISABLED) ? 1 : 0;
    if (dis) { bg = WB_GREY; fg = WB_DARK_GREY; }

    FB_FillRect(sx, sy, w, h, bg);
    FB_DrawRect(sx, sy, w, h, fg);

    uint32_t label = mem_u32(obj + GAD_OFF_GADGETTEXT);
    if (label && (flags & GFLG_LABELITEXT)) {
        uint32_t text = mem_u32(label + ITEXT_OFF_ITEXT);
        if (text) {
            char buf[64] = "";
            guest_str(buf, text, sizeof(buf));
            FB_PutStr(sx + 4, sy + (h - 16) / 2, buf, fg, bg);
        }
    }

    if (activation & GACT_TOGGLESELECT) {
        int box = (h < 20) ? h - 4 : 16;
        int by = sy + (h - box) / 2;
        FB_DrawRect(sx + 2, by, box, box, fg);
        if (flags & GFLG_SELECTED)
            FB_FillRect(sx + 4, by + 2, box - 4, box - 4, fg);
    }
}

static uint32_t gadgetclass_dispatch(uint32_t cls, uint32_t obj, uint32_t msg)
{
    uint32_t method = mem_u32(msg + MSG_OFF_METHODID);
    switch (method) {
        case OM_NEW: {
            uint32_t tags = mem_u32(msg + OPNEW_OFF_ATTRLIST);
            memset(&g_ram[obj], 0, GAD_SIZE);
            /* Default to a boolean push-button gadget. */
            mem_w16(obj + GAD_OFF_GADGETTYPE, GTYP_BOOLGADGET);
            mem_w16(obj + GAD_OFF_ACTIVATION, GACT_IMMEDIATE | GACT_RELVERIFY);
            walk_tags(tags, gadget_set_tag, (void*)(uintptr_t)obj);
            return 1;
        }
        case OM_DISPOSE: {
            uint32_t label = mem_u32(obj + GAD_OFF_GADGETTEXT);
            if (label) {
                uint32_t text = mem_u32(label + ITEXT_OFF_ITEXT);
                if (text) intu_free(text);
                intu_free(label);
            }
            return 1;
        }
        case OM_SET: {
            uint32_t tags = mem_u32(msg + OPSET_OFF_ATTRLIST);
            walk_tags(tags, gadget_set_tag, (void*)(uintptr_t)obj);
            return 1;
        }
        case OM_GET: {
            uint32_t attr = mem_u32(msg + OPGET_OFF_ATTRID);
            uint32_t store = mem_u32(msg + OPGET_OFF_STORAGE);
            if (!store) return 0;
            uint32_t value = 0;
            switch (attr) {
                case GA_Left:      value = (uint32_t)(int32_t)mem_s16(obj + GAD_OFF_LEFTEDGE); break;
                case GA_Top:       value = (uint32_t)(int32_t)mem_s16(obj + GAD_OFF_TOPEDGE); break;
                case GA_Width:     value = (uint32_t)(int32_t)mem_s16(obj + GAD_OFF_WIDTH); break;
                case GA_Height:    value = (uint32_t)(int32_t)mem_s16(obj + GAD_OFF_HEIGHT); break;
                case GA_Text:      value = mem_u32(obj + GAD_OFF_GADGETTEXT); break;
                case GA_Label:     value = mem_u32(obj + GAD_OFF_GADGETTEXT); break;
                case GA_Image:     value = mem_u32(obj + GAD_OFF_GADGETRENDER); break;
                case GA_ID:        value = mem_u16(obj + GAD_OFF_GADGETID); break;
                case GA_UserData:  value = mem_u32(obj + GAD_OFF_USERDATA); break;
                case GA_Disabled:  value = (mem_u16(obj + GAD_OFF_FLAGS) & GFLG_DISABLED) ? 1 : 0; break;
                case GA_Selected:  value = (mem_u16(obj + GAD_OFF_FLAGS) & GFLG_SELECTED) ? 1 : 0; break;
                default: return 0;
            }
            mem_w32(store, value);
            return 1;
        }
        case GM_HITTEST: {
            int16_t mx = mem_s16(msg + GMHT_OFF_MOUSEX);
            int16_t my = mem_s16(msg + GMHT_OFF_MOUSEY);
            return gadget_point_inside(obj, mx, my) ? 1 : 0;
        }
        case GM_RENDER: {
            uint32_t rport = mem_u32(msg + GMR_OFF_RPORT);
            uint32_t redraw = mem_u32(msg + GMR_OFF_REDRAW);
            gadget_render(obj, rport, redraw);
            return 1;
        }
        case GM_GOACTIVE: {
            uint16_t flags = mem_u16(obj + GAD_OFF_FLAGS);
            uint16_t activation = mem_u16(obj + GAD_OFF_ACTIVATION);
            if (activation & GACT_TOGGLESELECT) {
                flags ^= GFLG_SELECTED;
            } else {
                flags |= GFLG_SELECTED;
            }
            mem_w16(obj + GAD_OFF_FLAGS, flags);
            return GMR_MEACTIVE;
        }
        case GM_HANDLEINPUT: {
            int16_t mx = mem_s16(msg + GMI_OFF_MOUSEX);
            int16_t my = mem_s16(msg + GMI_OFF_MOUSEY);
            if (!gadget_point_inside(obj, mx, my)) {
                uint16_t flags = mem_u16(obj + GAD_OFF_FLAGS);
                flags &= ~GFLG_SELECTED;
                mem_w16(obj + GAD_OFF_FLAGS, flags);
                return GMR_NOREUSE;
            }
            return GMR_MEACTIVE;
        }
        case GM_GOINACTIVE: {
            uint16_t flags = mem_u16(obj + GAD_OFF_FLAGS);
            uint16_t activation = mem_u16(obj + GAD_OFF_ACTIVATION);
            if (!(activation & GACT_TOGGLESELECT)) {
                flags &= ~GFLG_SELECTED;
            }
            mem_w16(obj + GAD_OFF_FLAGS, flags);
            return GMR_NOREUSE;
        }
        case GM_LAYOUT: {
            /* Layout the gadget after a window resize.  For the base
             * gadgetclass there is nothing to do — subclasses with
             * GFLG_RELRIGHT/RELBOTTOM/RELWIDTH/RELHEIGHT would recompute
             * their geometry here.  Return 1 for success. */
            return 1;
        }
        case GM_DOMAIN: {
            /* Return the gadget's domain rectangle in the IBox* at
             * GMDOMAIN_OFF_DOMAIN.  For the base gadgetclass the domain
             * is the gadget's own LeftEdge/TopEdge/Width/Height. */
            uint32_t domain = mem_u32(msg + GMDOMAIN_OFF_DOMAIN);
            if (domain) {
                int16_t left  = mem_s16(obj + GAD_OFF_LEFTEDGE);
                int16_t top   = mem_s16(obj + GAD_OFF_TOPEDGE);
                int16_t width = mem_s16(obj + GAD_OFF_WIDTH);
                int16_t height = mem_s16(obj + GAD_OFF_HEIGHT);
                mem_w16(domain + IBOX_OFF_LEFT,   (uint16_t)left);
                mem_w16(domain + IBOX_OFF_TOP,    (uint16_t)top);
                mem_w16(domain + IBOX_OFF_WIDTH,  (uint16_t)width);
                mem_w16(domain + IBOX_OFF_HEIGHT, (uint16_t)height);
            }
            return 1;
        }
        default:
            return 0;
    }
    (void)cls;
}

/* =========================================================================
 * imageclass
 * ========================================================================= */
#define BIMG_OFF_WIDTH   0
#define BIMG_OFF_HEIGHT  4
#define BIMG_OFF_FGPEN   8
#define BIMG_OFF_BGPEN   12
#define BIMG_OFF_DATA    16
#define BIMG_OFF_LEFT    20
#define BIMG_OFF_TOP     24
#define BIMG_OFF_BITMAP  28
#define BIMG_INST_SIZE   32

static int image_set_tag(uint32_t tag, uint32_t data, void *ctx)
{
    uint32_t obj = (uint32_t)(uintptr_t)ctx;
    switch (tag) {
        case IA_Width:      mem_w32(obj + BIMG_OFF_WIDTH, (uint32_t)(int32_t)(int16_t)data); break;
        case IA_Height:     mem_w32(obj + BIMG_OFF_HEIGHT, (uint32_t)(int32_t)(int16_t)data); break;
        case IA_FGPen:      mem_w32(obj + BIMG_OFF_FGPEN, data); break;
        case IA_BGPen:      mem_w32(obj + BIMG_OFF_BGPEN, data); break;
        case IA_Data:       mem_w32(obj + BIMG_OFF_DATA, data); break;
        case IA_Left:       mem_w32(obj + BIMG_OFF_LEFT, (uint32_t)(int32_t)(int16_t)data); break;
        case IA_Top:        mem_w32(obj + BIMG_OFF_TOP, (uint32_t)(int32_t)(int16_t)data); break;
        case IA_SupportsDisable: break;
        case IA_Mode:       break;
        /* imageclass also accepts a BitMap via IM_BitMap extension */
        default: {
            if (tag == IM_BitMap) {
                mem_w32(obj + BIMG_OFF_BITMAP, data);
            }
            break;
        }
    }
    return 1;
}

static uint32_t imageclass_dispatch(uint32_t cls, uint32_t obj, uint32_t msg)
{
    uint32_t method = mem_u32(msg + MSG_OFF_METHODID);
    switch (method) {
        case OM_NEW: {
            uint32_t tags = mem_u32(msg + OPNEW_OFF_ATTRLIST);
            memset(&g_ram[obj], 0, BIMG_INST_SIZE);
            walk_tags(tags, image_set_tag, (void*)(uintptr_t)obj);
            return 1;
        }
        case OM_DISPOSE:
            return 1;
        case OM_SET: {
            uint32_t tags = mem_u32(msg + OPSET_OFF_ATTRLIST);
            walk_tags(tags, image_set_tag, (void*)(uintptr_t)obj);
            return 1;
        }
        case OM_GET: {
            uint32_t attr = mem_u32(msg + OPGET_OFF_ATTRID);
            uint32_t store = mem_u32(msg + OPGET_OFF_STORAGE);
            if (!store) return 0;
            uint32_t value = 0;
            switch (attr) {
                case IA_Width:   value = mem_u32(obj + BIMG_OFF_WIDTH); break;
                case IA_Height:  value = mem_u32(obj + BIMG_OFF_HEIGHT); break;
                case IA_FGPen:   value = mem_u32(obj + BIMG_OFF_FGPEN); break;
                case IA_BGPen:   value = mem_u32(obj + BIMG_OFF_BGPEN); break;
                case IA_Data:    value = mem_u32(obj + BIMG_OFF_DATA); break;
                case IA_Left:    value = mem_u32(obj + BIMG_OFF_LEFT); break;
                case IA_Top:     value = mem_u32(obj + BIMG_OFF_TOP); break;
                default: return 0;
            }
            mem_w32(store, value);
            return 1;
        }
        case IM_DRAW: {
            /* impDraw: { MethodID, RPort, OffsetX, OffsetY, State }
             * Build a temporary Image structure from the instance data
             * and blit it to the RastPort. */
            uint32_t rport   = mem_u32(msg + IMDRAW_OFF_RPORT);
            int32_t  xoff    = (int32_t)mem_u32(msg + IMDRAW_OFF_OFFSETX);
            int32_t  yoff    = (int32_t)mem_u32(msg + IMDRAW_OFF_OFFSETY);
            uint32_t state   = mem_u32(msg + IMDRAW_OFF_STATE);
            (void)state;

            int32_t w  = (int32_t)mem_u32(obj + BIMG_OFF_WIDTH);
            int32_t h  = (int32_t)mem_u32(obj + BIMG_OFF_HEIGHT);
            int32_t il = (int32_t)mem_u32(obj + BIMG_OFF_LEFT);
            int32_t it = (int32_t)mem_u32(obj + BIMG_OFF_TOP);
            uint32_t data = mem_u32(obj + BIMG_OFF_DATA);

            if (data && w > 0 && h > 0 && rport) {
                int bpr = ((w + 15) / 16) * 2;
                /* BltTemplate(src, xSrc, srcMod, destRP, xDest, yDest, xSize, ySize) */
                extern unsigned int m68k_get_reg(void *, int);
                extern void m68k_set_reg(int, unsigned int);
                m68k_set_reg(0 /* A0 */, data);
                m68k_set_reg(0 /* D0 */, 0);
                m68k_set_reg(1 /* D1 */, (unsigned int)bpr);
                m68k_set_reg(1 /* A1 */, rport);
                /* Use register constants matching the Musashi API:
                 * M68K_REG_A0=8, M68K_REG_A1=9, M68K_REG_D0=0, etc. */
                /* We need to use the real register enum values.  Since
                 * boopsi_builtin.c already uses m68k_get_reg/m68k_set_reg
                 * with raw int constants, match the Musashi numbering:
                 * D0=0..D7=7, A0=8..A7=15. */
                extern void m68k_set_reg(int reg, unsigned int value);
                /* A0 = 8, A1 = 9, D0 = 0, D1 = 1, D2 = 2, D3 = 3, D4 = 4, D5 = 5 */
                m68k_set_reg(8, data);        /* A0 = source */
                m68k_set_reg(0, 0);           /* D0 = xSrc */
                m68k_set_reg(1, (unsigned int)bpr); /* D1 = srcMod */
                m68k_set_reg(9, rport);       /* A1 = destRP */
                m68k_set_reg(2, (unsigned int)(xoff + il)); /* D2 = xDest */
                m68k_set_reg(3, (unsigned int)(yoff + it)); /* D3 = yDest */
                m68k_set_reg(4, (unsigned int)w);           /* D4 = xSize */
                m68k_set_reg(5, (unsigned int)h);           /* D5 = ySize */
                UAOS_Graphics_Dispatch(6);    /* GFX_SLOT_BLTTEMPLATE = 6 */
            }
            return 1;
        }
        case IM_ERASE: {
            /* impErase: { MethodID, RPort, OffsetX, OffsetY }
             * Fill the image rectangle with the RastPort's background pen. */
            uint32_t rport = mem_u32(msg + IMERASE_OFF_RPORT);
            int32_t  xoff  = (int32_t)mem_u32(msg + IMERASE_OFF_OFFSETX);
            int32_t  yoff  = (int32_t)mem_u32(msg + IMERASE_OFF_OFFSETY);
            int32_t w  = (int32_t)mem_u32(obj + BIMG_OFF_WIDTH);
            int32_t h  = (int32_t)mem_u32(obj + BIMG_OFF_HEIGHT);
            int32_t il = (int32_t)mem_u32(obj + BIMG_OFF_LEFT);
            int32_t it = (int32_t)mem_u32(obj + BIMG_OFF_TOP);
            if (w > 0 && h > 0 && rport) {
                /* RectFill(rp, xMin, yMin, xMax, yMax)
                 * A1=rp, D0=xMin, D1=yMin, D2=xMax, D3=yMax */
                extern void m68k_set_reg(int reg, unsigned int value);
                m68k_set_reg(9, rport);
                m68k_set_reg(0, (unsigned int)(xoff + il));
                m68k_set_reg(1, (unsigned int)(yoff + it));
                m68k_set_reg(2, (unsigned int)(xoff + il + w - 1));
                m68k_set_reg(3, (unsigned int)(yoff + it + h - 1));
                UAOS_Graphics_Dispatch(51);   /* GFX_SLOT_RECTFILL = 51 */
            }
            return 1;
        }
        case IM_DRAWFRAME: {
            /* impDrawFrame: { MethodID, RPort, IBox* Frame, OffsetX, OffsetY, State }
             * Like IM_DRAW but clipped to the frame rectangle.  We draw
             * the image normally; the caller is responsible for setting
             * up the clipping layer. */
            uint32_t rport = mem_u32(msg + IMDRAWFRAME_OFF_RPORT);
            int32_t  xoff  = (int32_t)mem_u32(msg + IMDRAWFRAME_OFF_OFFSETX);
            int32_t  yoff  = (int32_t)mem_u32(msg + IMDRAWFRAME_OFF_OFFSETY);
            uint32_t state = mem_u32(msg + IMDRAWFRAME_OFF_STATE);
            (void)state;
            (void)mem_u32(msg + IMDRAWFRAME_OFF_FRAME); /* clip frame — layers handle this */

            int32_t w  = (int32_t)mem_u32(obj + BIMG_OFF_WIDTH);
            int32_t h  = (int32_t)mem_u32(obj + BIMG_OFF_HEIGHT);
            int32_t il = (int32_t)mem_u32(obj + BIMG_OFF_LEFT);
            int32_t it = (int32_t)mem_u32(obj + BIMG_OFF_TOP);
            uint32_t data = mem_u32(obj + BIMG_OFF_DATA);

            if (data && w > 0 && h > 0 && rport) {
                int bpr = ((w + 15) / 16) * 2;
                extern void m68k_set_reg(int reg, unsigned int value);
                m68k_set_reg(8, data);
                m68k_set_reg(0, 0);
                m68k_set_reg(1, (unsigned int)bpr);
                m68k_set_reg(9, rport);
                m68k_set_reg(2, (unsigned int)(xoff + il));
                m68k_set_reg(3, (unsigned int)(yoff + it));
                m68k_set_reg(4, (unsigned int)w);
                m68k_set_reg(5, (unsigned int)h);
                UAOS_Graphics_Dispatch(6);    /* GFX_SLOT_BLTTEMPLATE */
            }
            return 1;
        }
        case IM_ERASEFRAME: {
            /* impEraseFrame: { MethodID, RPort, IBox* Frame, OffsetX, OffsetY }
             * Like IM_ERASE but clipped to the frame rectangle. */
            uint32_t rport = mem_u32(msg + IMERASEFRAME_OFF_RPORT);
            int32_t  xoff  = (int32_t)mem_u32(msg + IMERASEFRAME_OFF_OFFSETX);
            int32_t  yoff  = (int32_t)mem_u32(msg + IMERASEFRAME_OFF_OFFSETY);
            (void)mem_u32(msg + IMERASEFRAME_OFF_FRAME);

            int32_t w  = (int32_t)mem_u32(obj + BIMG_OFF_WIDTH);
            int32_t h  = (int32_t)mem_u32(obj + BIMG_OFF_HEIGHT);
            int32_t il = (int32_t)mem_u32(obj + BIMG_OFF_LEFT);
            int32_t it = (int32_t)mem_u32(obj + BIMG_OFF_TOP);
            if (w > 0 && h > 0 && rport) {
                extern void m68k_set_reg(int reg, unsigned int value);
                m68k_set_reg(9, rport);
                m68k_set_reg(0, (unsigned int)(xoff + il));
                m68k_set_reg(1, (unsigned int)(yoff + it));
                m68k_set_reg(2, (unsigned int)(xoff + il + w - 1));
                m68k_set_reg(3, (unsigned int)(yoff + it + h - 1));
                UAOS_Graphics_Dispatch(51);   /* GFX_SLOT_RECTFILL */
            }
            return 1;
        }
        default:
            return 0;
    }
    (void)cls;
}

/* =========================================================================
 * pointerclass
 * ========================================================================= */
#define BPTR_OFF_XOFFSET     32
#define BPTR_OFF_YOFFSET     36
#define BPTR_OFF_WORDWIDTH   40
#define BPTR_OFF_FLAGS       44
#define BPTR_OFF_XRES        48
#define BPTR_OFF_YRES        52
#define BPTR_INST_SIZE       56

static int pointer_set_tag(uint32_t tag, uint32_t data, void *ctx)
{
    uint32_t obj = (uint32_t)(uintptr_t)ctx;
    image_set_tag(tag, data, ctx); /* handle IA_* tags first */
    switch (tag) {
        case POINTERA_BitMap:      mem_w32(obj + BIMG_OFF_BITMAP, data); break;
        case POINTERA_XOffset:     mem_w32(obj + BPTR_OFF_XOFFSET, (uint32_t)(int32_t)(int16_t)data); break;
        case POINTERA_YOffset:     mem_w32(obj + BPTR_OFF_YOFFSET, (uint32_t)(int32_t)(int16_t)data); break;
        case POINTERA_WordWidth:   mem_w32(obj + BPTR_OFF_WORDWIDTH, data); break;
        case POINTERA_XResolution: mem_w32(obj + BPTR_OFF_XRES, data); break;
        case POINTERA_YResolution: mem_w32(obj + BPTR_OFF_YRES, data); break;
        case POINTERA_Flags:       mem_w32(obj + BPTR_OFF_FLAGS, data); break;
    }
    return 1;
}

static uint32_t pointerclass_dispatch(uint32_t cls, uint32_t obj, uint32_t msg)
{
    uint32_t method = mem_u32(msg + MSG_OFF_METHODID);
    switch (method) {
        case OM_NEW: {
            uint32_t tags = mem_u32(msg + OPNEW_OFF_ATTRLIST);
            memset(&g_ram[obj], 0, BPTR_INST_SIZE);
            walk_tags(tags, pointer_set_tag, (void*)(uintptr_t)obj);
            return 1;
        }
        case OM_DISPOSE:
            return 1;
        case OM_SET: {
            uint32_t tags = mem_u32(msg + OPSET_OFF_ATTRLIST);
            walk_tags(tags, pointer_set_tag, (void*)(uintptr_t)obj);
            return 1;
        }
        case OM_GET: {
            uint32_t attr = mem_u32(msg + OPGET_OFF_ATTRID);
            uint32_t store = mem_u32(msg + OPGET_OFF_STORAGE);
            if (!store) return 0;
            uint32_t value = 0;
            switch (attr) {
                case POINTERA_BitMap:      value = mem_u32(obj + BIMG_OFF_BITMAP); break;
                case POINTERA_XOffset:     value = mem_u32(obj + BPTR_OFF_XOFFSET); break;
                case POINTERA_YOffset:     value = mem_u32(obj + BPTR_OFF_YOFFSET); break;
                case POINTERA_WordWidth:   value = mem_u32(obj + BPTR_OFF_WORDWIDTH); break;
                case POINTERA_XResolution: value = mem_u32(obj + BPTR_OFF_XRES); break;
                case POINTERA_YResolution: value = mem_u32(obj + BPTR_OFF_YRES); break;
                case POINTERA_Flags:       value = mem_u32(obj + BPTR_OFF_FLAGS); break;
                default: return imageclass_dispatch(cls, obj, msg); /* IA_* */
            }
            mem_w32(store, value);
            return 1;
        }
        default:
            return imageclass_dispatch(cls, obj, msg);
    }
    (void)cls;
}

/* =========================================================================
 * menuclass — real menu node with children and sibling list
 * ========================================================================= */
#define BMENU_OFF_TYPE      0
#define BMENU_OFF_LABEL     4
#define BMENU_OFF_KEY       8
#define BMENU_OFF_DISABLED  12
#define BMENU_OFF_CHECKED   16
#define BMENU_OFF_CHILDREN  20
#define BMENU_OFF_NEXT      24
#define BMENU_OFF_PARENT    28
#define BMENU_INST_SIZE     32

static int menu_set_tag(uint32_t tag, uint32_t data, void *ctx)
{
    uint32_t obj = (uint32_t)(uintptr_t)ctx;
    switch (tag) {
        case MA_Type:       mem_w32(obj + BMENU_OFF_TYPE, data); break;
        case MA_Label:      mem_w32(obj + BMENU_OFF_LABEL, data); break;
        case MA_Key:        mem_w32(obj + BMENU_OFF_KEY, data); break;
        case MA_Disabled:   mem_w32(obj + BMENU_OFF_DISABLED, data ? 1 : 0); break;
        case MA_Checked:    mem_w32(obj + BMENU_OFF_CHECKED, data ? 1 : 0); break;
        default:
            /* Unknown menu tag: store as a user-data slot. */
            break;
    }
    return 1;
}

static uint32_t menuclass_dispatch(uint32_t cls, uint32_t obj, uint32_t msg)
{
    uint32_t method = mem_u32(msg + MSG_OFF_METHODID);
    switch (method) {
        case OM_NEW: {
            uint32_t tags = mem_u32(msg + OPNEW_OFF_ATTRLIST);
            memset(&g_ram[obj], 0, BMENU_INST_SIZE);
            walk_tags(tags, menu_set_tag, (void*)(uintptr_t)obj);
            return 1;
        }
        case OM_DISPOSE: {
            /* Dispose children and siblings. */
            uint32_t child = mem_u32(obj + BMENU_OFF_CHILDREN);
            while (child) {
                uint32_t next = mem_u32(child + BMENU_OFF_NEXT);
                UAOS_BOOPSI_Dispatch(child, OM_DISPOSE, 0, 0);
                intu_free(child - OBJ_HEADER_SIZE);
                child = next;
            }
            uint32_t sibling = mem_u32(obj + BMENU_OFF_NEXT);
            while (sibling) {
                uint32_t next = mem_u32(sibling + BMENU_OFF_NEXT);
                UAOS_BOOPSI_Dispatch(sibling, OM_DISPOSE, 0, 0);
                intu_free(sibling - OBJ_HEADER_SIZE);
                sibling = next;
            }
            return 1;
        }
        case OM_SET: {
            uint32_t tags = mem_u32(msg + OPSET_OFF_ATTRLIST);
            walk_tags(tags, menu_set_tag, (void*)(uintptr_t)obj);
            return 1;
        }
        case OM_GET: {
            uint32_t attr = mem_u32(msg + OPGET_OFF_ATTRID);
            uint32_t store = mem_u32(msg + OPGET_OFF_STORAGE);
            if (!store) return 0;
            uint32_t value = 0;
            switch (attr) {
                case MA_Type:      value = mem_u32(obj + BMENU_OFF_TYPE); break;
                case MA_Label:     value = mem_u32(obj + BMENU_OFF_LABEL); break;
                case MA_Key:       value = mem_u32(obj + BMENU_OFF_KEY); break;
                case MA_Disabled:  value = mem_u32(obj + BMENU_OFF_DISABLED); break;
                case MA_Checked:   value = mem_u32(obj + BMENU_OFF_CHECKED); break;
                case MA_AddChild:  value = mem_u32(obj + BMENU_OFF_CHILDREN); break;
                default: return 0;
            }
            mem_w32(store, value);
            return 1;
        }
        case OM_ADDMEMBER: {
            uint32_t member = mem_u32(msg + 4); /* OM_ADDMEMBER passes member in message[1] */
            if (!member) return 0;
            /* Add to the head of the children list. */
            uint32_t old = mem_u32(obj + BMENU_OFF_CHILDREN);
            mem_w32(obj + BMENU_OFF_CHILDREN, member);
            mem_w32(member + BMENU_OFF_PARENT, obj);
            mem_w32(member + BMENU_OFF_NEXT, old);
            return 1;
        }
        case OM_REMMEMBER: {
            uint32_t member = mem_u32(msg + 4);
            if (!member) return 0;
            uint32_t prev = 0;
            uint32_t cur = mem_u32(obj + BMENU_OFF_CHILDREN);
            while (cur) {
                if (cur == member) {
                    uint32_t next = mem_u32(cur + BMENU_OFF_NEXT);
                    if (prev) mem_w32(prev + BMENU_OFF_NEXT, next);
                    else mem_w32(obj + BMENU_OFF_CHILDREN, next);
                    mem_w32(cur + BMENU_OFF_PARENT, 0);
                    mem_w32(cur + BMENU_OFF_NEXT, 0);
                    return 1;
                }
                prev = cur;
                cur = mem_u32(cur + BMENU_OFF_NEXT);
            }
            return 0;
        }
        default:
            return rootclass_dispatch(cls, obj, msg);
    }
    (void)cls;
}

/* =========================================================================
 * windowclass — real window attribute storage
 * ========================================================================= */
#define BWIN_OFF_LEFT       0
#define BWIN_OFF_TOP        4
#define BWIN_OFF_WIDTH      8
#define BWIN_OFF_HEIGHT     12
#define BWIN_OFF_TITLE      16
#define BWIN_OFF_FLAGS      20
#define BWIN_OFF_IDCMP      24
#define BWIN_OFF_SCREEN     28
#define BWIN_OFF_MINWIDTH   32
#define BWIN_OFF_MINHEIGHT  36
#define BWIN_OFF_MAXWIDTH   40
#define BWIN_OFF_MAXHEIGHT  44
#define BWIN_INST_SIZE      48

static int win_set_tag(uint32_t tag, uint32_t data, void *ctx)
{
    uint32_t obj = (uint32_t)(uintptr_t)ctx;
    switch (tag) {
        case WA_Left:        mem_w32(obj + BWIN_OFF_LEFT, (uint32_t)(int32_t)(int16_t)data); break;
        case WA_Top:         mem_w32(obj + BWIN_OFF_TOP, (uint32_t)(int32_t)(int16_t)data); break;
        case WA_Width:       mem_w32(obj + BWIN_OFF_WIDTH, (uint32_t)(int32_t)(int16_t)data); break;
        case WA_Height:      mem_w32(obj + BWIN_OFF_HEIGHT, (uint32_t)(int32_t)(int16_t)data); break;
        case WA_Title:       mem_w32(obj + BWIN_OFF_TITLE, data); break;
        case WA_Flags:       mem_w32(obj + BWIN_OFF_FLAGS, data); break;
        case WA_IDCMP:       mem_w32(obj + BWIN_OFF_IDCMP, data); break;
        case WA_CustomScreen:
        case WA_PubScreen:   mem_w32(obj + BWIN_OFF_SCREEN, data); break;
        case WA_MinWidth:    mem_w32(obj + BWIN_OFF_MINWIDTH, (uint32_t)(int32_t)(int16_t)data); break;
        case WA_MinHeight:   mem_w32(obj + BWIN_OFF_MINHEIGHT, (uint32_t)(int32_t)(int16_t)data); break;
        case WA_MaxWidth:    mem_w32(obj + BWIN_OFF_MAXWIDTH, (uint32_t)(int32_t)(int16_t)data); break;
        case WA_MaxHeight:   mem_w32(obj + BWIN_OFF_MAXHEIGHT, (uint32_t)(int32_t)(int16_t)data); break;
        default: break;
    }
    return 1;
}

static uint32_t windowclass_dispatch(uint32_t cls, uint32_t obj, uint32_t msg)
{
    uint32_t method = mem_u32(msg + MSG_OFF_METHODID);
    switch (method) {
        case OM_NEW: {
            uint32_t tags = mem_u32(msg + OPNEW_OFF_ATTRLIST);
            memset(&g_ram[obj], 0, BWIN_INST_SIZE);
            walk_tags(tags, win_set_tag, (void*)(uintptr_t)obj);
            return 1;
        }
        case OM_DISPOSE:
            return 1;
        case OM_SET: {
            uint32_t tags = mem_u32(msg + OPSET_OFF_ATTRLIST);
            walk_tags(tags, win_set_tag, (void*)(uintptr_t)obj);
            return 1;
        }
        case OM_GET: {
            uint32_t attr = mem_u32(msg + OPGET_OFF_ATTRID);
            uint32_t store = mem_u32(msg + OPGET_OFF_STORAGE);
            if (!store) return 0;
            uint32_t value = 0;
            switch (attr) {
                case WA_Left:      value = mem_u32(obj + BWIN_OFF_LEFT); break;
                case WA_Top:       value = mem_u32(obj + BWIN_OFF_TOP); break;
                case WA_Width:     value = mem_u32(obj + BWIN_OFF_WIDTH); break;
                case WA_Height:    value = mem_u32(obj + BWIN_OFF_HEIGHT); break;
                case WA_Title:     value = mem_u32(obj + BWIN_OFF_TITLE); break;
                case WA_Flags:     value = mem_u32(obj + BWIN_OFF_FLAGS); break;
                case WA_IDCMP:     value = mem_u32(obj + BWIN_OFF_IDCMP); break;
                case WA_CustomScreen:
                case WA_PubScreen: value = mem_u32(obj + BWIN_OFF_SCREEN); break;
                case WA_MinWidth:  value = mem_u32(obj + BWIN_OFF_MINWIDTH); break;
                case WA_MinHeight: value = mem_u32(obj + BWIN_OFF_MINHEIGHT); break;
                case WA_MaxWidth:  value = mem_u32(obj + BWIN_OFF_MAXWIDTH); break;
                case WA_MaxHeight: value = mem_u32(obj + BWIN_OFF_MAXHEIGHT); break;
                default: return 0;
            }
            mem_w32(store, value);
            return 1;
        }
        default:
            return rootclass_dispatch(cls, obj, msg);
    }
    (void)cls;
}

/* =========================================================================
 * Class creation helper
 * ========================================================================= */
static uint32_t make_builtin_class(const char *id, const char *super_id,
                                   uint32_t super_ptr, uint32_t inst_offset,
                                   uint32_t inst_size,
                                   uint32_t (*native)(uint32_t, uint32_t, uint32_t))
{
    uint32_t cls = intu_alloc(CLASS_SIZE);
    if (!cls) return 0;
    for (int i = 0; i < CLASS_SIZE; i++) g_ram[cls + i] = 0;

    uint32_t id_len = (uint32_t)strlen(id) + 1;
    uint32_t id_addr = intu_alloc(id_len);
    if (!id_addr) { intu_free(cls); return 0; }
    for (uint32_t i = 0; i < id_len; i++) g_ram[id_addr + i] = (uint8_t)id[i];

    mem_w32(cls + CLASS_OFF_ID, id_addr);
    if (super_ptr) mem_w32(cls + CLASS_OFF_SUPER, super_ptr);
    if (super_id) {
        /* super_id string lookup will happen at registration time */
        (void)super_id;
    }
    mem_w16(cls + CLASS_OFF_INST_OFFSET, (uint16_t)inst_offset);
    mem_w16(cls + CLASS_OFF_INST_SIZE, (uint16_t)inst_size);
    mem_w32(cls + CLASS_OFF_FLAGS, CLASS_FLAG_NATIVE);
    mem_w32(cls + CLASS_OFF_NATIVE_DISPATCHER, (uint32_t)(uintptr_t)native);
    return cls;
}


/* =========================================================================
 * Public registration
 * ========================================================================= */
static uint32_t g_rootclass, g_gadgetclass, g_imageclass, g_pointerclass, g_menuclass, g_windowclass;

void UAOS_BOOPSI_RegisterBuiltinClasses(void)
{
    g_rootclass = make_builtin_class("rootclass", NULL, 0, 0, 0, rootclass_dispatch);
    UAOS_BOOPSI_RegisterClass(g_rootclass);

    g_gadgetclass = make_builtin_class("gadgetclass", "rootclass", g_rootclass, 0, GAD_SIZE, gadgetclass_dispatch);
    UAOS_BOOPSI_RegisterClass(g_gadgetclass);

    g_imageclass = make_builtin_class("imageclass", "rootclass", g_rootclass, 0, BIMG_INST_SIZE, imageclass_dispatch);
    UAOS_BOOPSI_RegisterClass(g_imageclass);

    g_pointerclass = make_builtin_class("pointerclass", "imageclass", g_imageclass, 0, BPTR_INST_SIZE, pointerclass_dispatch);
    UAOS_BOOPSI_RegisterClass(g_pointerclass);

    g_menuclass = make_builtin_class("menuclass", "rootclass", g_rootclass, 0, BMENU_INST_SIZE, menuclass_dispatch);
    UAOS_BOOPSI_RegisterClass(g_menuclass);

    g_windowclass = make_builtin_class("windowclass", "rootclass", g_rootclass, 0, BWIN_INST_SIZE, windowclass_dispatch);
    UAOS_BOOPSI_RegisterClass(g_windowclass);
}

/* =========================================================================
 * Helper used by the host pointer setter to decode pointerclass objects
 * ========================================================================= */
int UAOS_BOOPSI_IsPointerClass(uint32_t obj)
{
    if (!obj || obj < OBJ_HEADER_SIZE) return 0;
    uint32_t cls = mem_u32(obj - OBJ_HEADER_SIZE + OBJ_OFF_CLASS);
    return (cls == g_pointerclass) ? 1 : 0;
}

uint32_t UAOS_BOOPSI_PointerBitMap(uint32_t obj)
{
    if (!UAOS_BOOPSI_IsPointerClass(obj)) return 0;
    return mem_u32(obj + BIMG_OFF_BITMAP);
}

uint32_t UAOS_BOOPSI_PointerXOffset(uint32_t obj)
{
    if (!UAOS_BOOPSI_IsPointerClass(obj)) return 0;
    return mem_s16(obj + BPTR_OFF_XOFFSET);
}

uint32_t UAOS_BOOPSI_PointerYOffset(uint32_t obj)
{
    if (!UAOS_BOOPSI_IsPointerClass(obj)) return 0;
    return mem_s16(obj + BPTR_OFF_YOFFSET);
}
