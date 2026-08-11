/*
 * boopsi_builtin.c — Built-in BOOPSI classes with native host dispatchers
 *
 * Standard AmigaOS classes implemented in host C so M68k programs can create
 * objects via NewObject() without shipping their own dispatcher code.
 * Currently implemented:
 *   - rootclass       (base OM_NEW/OM_DISPOSE/OM_SET/OM_GET/OM_ADDTAIL/OM_REMOVE)
 *   - gadgetclass     (GA_* attributes and stub GM_* methods)
 *   - imageclass      (IA_* attributes and IA_Data/IA_Left/IA_Top)
 *   - pointerclass    (POINTERA_* attributes, decodes BitMap for WA_Pointer)
 *   - menuclass       (rootclass subclass, stub for menu support)
 *   - windowclass     (rootclass subclass, stub for window support)
 *   - modelclass      (ICA_TARGET/ICA_MAP, OM_NOTIFY broadcast)
 *   - frbuttonclass   (BOOLGADGET_* boolean button)
 *   - bevelbox        (BVS_* bevel box image)
 *   - menuitemclass   (menuclass subclass for menu items)
 *   - fillrectclass   (FILLRECT_* fill rectangle)
 *   - sysgclass       (system image gadget)
 *   - groupgclass     (GROUPG_* group layout + mutual exclusion)
 *   - propgclass      (PGA_* proportional gadget)
 *   - strgclass       (STRINGA_* string gadget)
 *   - icclass         (ICA_TARGET/ICA_MAP, OM_NOTIFY/ICM_INVOKE broadcast)
 *   - buttongclass    (BTNF_ and BOOLGADGET_ button gadget)
 *   - slidergclass    (SLIDER_* slider gadget with PropInfo)
 *   - scrollbarclass  (SCROLLER_* scroll bar, subclass of propgclass)
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
 * modelclass — broadcasts OM_NOTIFY to dependent gadgets via ICA_TARGET
 * ========================================================================= */
#define BMODEL_OFF_TARGET    0    /* ICA_TARGET: object to notify */
#define BMODEL_OFF_MAP       4    /* ICA_MAP: tag-mapping table (unused) */
#define BMODEL_OFF_CHILDREN  8    /* head of dependent list */
#define BMODEL_OFF_NEXT     12    /* next in model chain */
#define BMODEL_INST_SIZE    16

static int model_set_tag(uint32_t tag, uint32_t data, void *ctx)
{
    uint32_t obj = (uint32_t)(uintptr_t)ctx;
    switch (tag) {
        case ICA_TARGET: mem_w32(obj + BMODEL_OFF_TARGET, data); break;
        case ICA_MAP:    mem_w32(obj + BMODEL_OFF_MAP, data); break;
        default: break;
    }
    return 1;
}

static uint32_t modelclass_dispatch(uint32_t cls, uint32_t obj, uint32_t msg)
{
    uint32_t method = mem_u32(msg + MSG_OFF_METHODID);
    switch (method) {
        case OM_NEW: {
            uint32_t tags = mem_u32(msg + OPNEW_OFF_ATTRLIST);
            memset(&g_ram[obj], 0, BMODEL_INST_SIZE);
            walk_tags(tags, model_set_tag, (void*)(uintptr_t)obj);
            return 1;
        }
        case OM_SET: {
            uint32_t tags = mem_u32(msg + OPSET_OFF_ATTRLIST);
            walk_tags(tags, model_set_tag, (void*)(uintptr_t)obj);
            return 1;
        }
        case OM_NOTIFY: {
            /* Broadcast OM_UPDATE to the ICA_TARGET object (if any).
             * The real AmigaOS modelclass also applies ICA_MAP to
             * translate tag IDs.  We forward the message unchanged. */
            uint32_t target = mem_u32(obj + BMODEL_OFF_TARGET);
            if (target) {
                /* Rewrite MethodID to OM_UPDATE and dispatch to target. */
                mem_w32(msg + MSG_OFF_METHODID, OM_UPDATE);
                UAOS_BOOPSI_Dispatch(target, OM_UPDATE, msg, 0);
                mem_w32(msg + MSG_OFF_METHODID, OM_NOTIFY);
            }
            return 1;
        }
        case OM_GET: {
            uint32_t attr = mem_u32(msg + OPGET_OFF_ATTRID);
            uint32_t store = mem_u32(msg + OPGET_OFF_STORAGE);
            if (!store) return 0;
            switch (attr) {
                case ICA_TARGET: mem_w32(store, mem_u32(obj + BMODEL_OFF_TARGET)); return 1;
                case ICA_MAP:    mem_w32(store, mem_u32(obj + BMODEL_OFF_MAP)); return 1;
                default: return 0;
            }
        }
        default:
            return gadgetclass_dispatch(cls, obj, msg);
    }
}

/* =========================================================================
 * frbuttonclass — boolean button gadget (subclass of gadgetclass)
 * ========================================================================= */
/* Instance data piggybacks on the gadget's own GAD_OFF_* fields.
 * We store the checked state in GFLG_SELECTED, and the label/image
 * in GAD_OFF_GADGETTEXT / GAD_OFF_GADGETRENDER (already defined). */
#define BFRBTN_INST_SIZE  0  /* no extra instance data beyond gadgetclass */

static int frbutton_set_tag(uint32_t tag, uint32_t data, void *ctx)
{
    uint32_t obj = (uint32_t)(uintptr_t)ctx;
    switch (tag) {
        case BOOLGADGET_Checked: {
            uint16_t flags = mem_u16(obj + GAD_OFF_FLAGS);
            if (data) flags |= GFLG_SELECTED;
            else      flags &= ~GFLG_SELECTED;
            mem_w16(obj + GAD_OFF_FLAGS, flags);
            break;
        }
        case BOOLGADGET_Toggle: {
            /* BOOLGADGET_Toggle sets the GACT_TOGGLESELECT activation flag. */
            uint16_t act = mem_u16(obj + GAD_OFF_ACTIVATION);
            if (data) act |= GACT_TOGGLESELECT;
            else      act &= ~GACT_TOGGLESELECT;
            mem_w16(obj + GAD_OFF_ACTIVATION, act);
            break;
        }
        case BOOLGADGET_Text:
            /* Store as gadget label (IntuiText pointer). */
            mem_w32(obj + GAD_OFF_GADGETTEXT, data);
            break;
        case BOOLGADGET_Image:
            mem_w32(obj + GAD_OFF_GADGETRENDER, data);
            break;
        case BOOLGADGET_SelImage:
            mem_w32(obj + GAD_OFF_SELECTRENDER, data);
            break;
        default:
            break;
    }
    return 1;
}

static uint32_t frbuttonclass_dispatch(uint32_t cls, uint32_t obj, uint32_t msg)
{
    uint32_t method = mem_u32(msg + MSG_OFF_METHODID);
    switch (method) {
        case OM_NEW: {
            uint32_t tags = mem_u32(msg + OPNEW_OFF_ATTRLIST);
            /* Let gadgetclass handle GA_* first. */
            uint32_t r = gadgetclass_dispatch(cls, obj, msg);
            if (!r) return 0;
            /* Apply BOOLGADGET_* tags. */
            walk_tags(tags, frbutton_set_tag, (void*)(uintptr_t)obj);
            mem_w16(obj + GAD_OFF_GADGETTYPE, GTYP_BOOLGADGET);
            return r;
        }
        case OM_SET: {
            uint32_t tags = mem_u32(msg + OPSET_OFF_ATTRLIST);
            walk_tags(tags, frbutton_set_tag, (void*)(uintptr_t)obj);
            /* Also let gadgetclass handle GA_* tags. */
            return gadgetclass_dispatch(cls, obj, msg);
        }
        case OM_GET: {
            uint32_t attr = mem_u32(msg + OPGET_OFF_ATTRID);
            uint32_t store = mem_u32(msg + OPGET_OFF_STORAGE);
            if (!store) return 0;
            switch (attr) {
                case BOOLGADGET_Checked:
                    mem_w32(store, (mem_u16(obj + GAD_OFF_FLAGS) & GFLG_SELECTED) ? 1 : 0);
                    return 1;
                case BOOLGADGET_Text:
                    mem_w32(store, mem_u32(obj + GAD_OFF_GADGETTEXT));
                    return 1;
                default:
                    return gadgetclass_dispatch(cls, obj, msg);
            }
        }
        default:
            return gadgetclass_dispatch(cls, obj, msg);
    }
}

/* =========================================================================
 * bevelbox — bevel box image (subclass of imageclass)
 * ========================================================================= */
#define BBBOX_OFF_DRAWINFO  0
#define BBBOX_OFF_FRAME     4   /* IBox: Left, Top, Width, Height (8 bytes) */
#define BBBOX_OFF_RECESSED 12
#define BBBOX_OFF_SUNKEN   13
#define BBBOX_OFF_OFFSET   14   /* BVS_OFFSET: bevel rendering offset */
#define BBBOX_INST_SIZE   16

static int bevelbox_set_tag(uint32_t tag, uint32_t data, void *ctx)
{
    uint32_t obj = (uint32_t)(uintptr_t)ctx;
    switch (tag) {
        case BVS_DRAWINFO: mem_w32(obj + BBBOX_OFF_DRAWINFO, data); break;
        case BVS_RECESSED: g_ram[obj + BBBOX_OFF_RECESSED] = (uint8_t)(data ? 1 : 0); break;
        case BVS_SUNKEN:   g_ram[obj + BBBOX_OFF_SUNKEN]   = (uint8_t)(data ? 1 : 0); break;
        case BVS_OFFSET:   mem_w16(obj + BBBOX_OFF_OFFSET, (uint16_t)(int16_t)data); break;
        case BVS_LEFT:     mem_w16(obj + BBBOX_OFF_FRAME, (uint16_t)(int16_t)data); break;
        case BVS_TOP:      mem_w16(obj + BBBOX_OFF_FRAME + 2, (uint16_t)(int16_t)data); break;
        case BVS_WIDTH:    mem_w16(obj + BBBOX_OFF_FRAME + 4, (uint16_t)(int16_t)data); break;
        case BVS_HEIGHT:   mem_w16(obj + BBBOX_OFF_FRAME + 6, (uint16_t)(int16_t)data); break;
        default:
            /* Let imageclass handle IA_* tags. */
            image_set_tag(tag, data, ctx);
            break;
    }
    return 1;
}

static uint32_t bevelbox_dispatch(uint32_t cls, uint32_t obj, uint32_t msg)
{
    uint32_t method = mem_u32(msg + MSG_OFF_METHODID);
    switch (method) {
        case OM_NEW: {
            uint32_t tags = mem_u32(msg + OPNEW_OFF_ATTRLIST);
            memset(&g_ram[obj], 0, BBBOX_INST_SIZE);
            walk_tags(tags, bevelbox_set_tag, (void*)(uintptr_t)obj);
            return 1;
        }
        case OM_SET: {
            uint32_t tags = mem_u32(msg + OPSET_OFF_ATTRLIST);
            walk_tags(tags, bevelbox_set_tag, (void*)(uintptr_t)obj);
            return 1;
        }
        case OM_GET: {
            uint32_t attr = mem_u32(msg + OPGET_OFF_ATTRID);
            uint32_t store = mem_u32(msg + OPGET_OFF_STORAGE);
            if (!store) return 0;
            switch (attr) {
                case BVS_DRAWINFO: mem_w32(store, mem_u32(obj + BBBOX_OFF_DRAWINFO)); return 1;
                case BVS_LEFT:     mem_w32(store, (uint32_t)(int32_t)mem_s16(obj + BBBOX_OFF_FRAME)); return 1;
                case BVS_TOP:      mem_w32(store, (uint32_t)(int32_t)mem_s16(obj + BBBOX_OFF_FRAME + 2)); return 1;
                case BVS_WIDTH:    mem_w32(store, (uint32_t)(int32_t)mem_s16(obj + BBBOX_OFF_FRAME + 4)); return 1;
                case BVS_HEIGHT:   mem_w32(store, (uint32_t)(int32_t)mem_s16(obj + BBBOX_OFF_FRAME + 6)); return 1;
                case BVS_OFFSET:   mem_w32(store, (uint32_t)(int32_t)mem_s16(obj + BBBOX_OFF_OFFSET)); return 1;
                case BVS_RECESSED: mem_w32(store, g_ram[obj + BBBOX_OFF_RECESSED]); return 1;
                case BVS_SUNKEN:   mem_w32(store, g_ram[obj + BBBOX_OFF_SUNKEN]); return 1;
                default: return imageclass_dispatch(cls, obj, msg);
            }
        }
        case IM_DRAW:
        case IM_DRAWFRAME: {
            /* Draw a raised or sunken bevel rectangle using FB_DrawRect. */
            uint32_t rport = (method == IM_DRAW) ?
                mem_u32(msg + IMDRAW_OFF_RPORT) :
                mem_u32(msg + IMDRAWFRAME_OFF_RPORT);
            (void)rport;
            int32_t xoff = (method == IM_DRAW) ?
                (int32_t)mem_u32(msg + IMDRAW_OFF_OFFSETX) :
                (int32_t)mem_u32(msg + IMDRAWFRAME_OFF_OFFSETX);
            int32_t yoff = (method == IM_DRAW) ?
                (int32_t)mem_u32(msg + IMDRAW_OFF_OFFSETY) :
                (int32_t)mem_u32(msg + IMDRAWFRAME_OFF_OFFSETY);

            int16_t left   = mem_s16(obj + BBBOX_OFF_FRAME);
            int16_t top    = mem_s16(obj + BBBOX_OFF_FRAME + 2);
            int16_t width  = mem_s16(obj + BBBOX_OFF_FRAME + 4);
            int16_t height = mem_s16(obj + BBBOX_OFF_FRAME + 6);
            int sunken = g_ram[obj + BBBOX_OFF_SUNKEN];

            if (width > 0 && height > 0) {
                int x = xoff + left;
                int y = yoff + top;
                /* Draw outer rect (dark) and inner rect (light) for raised;
                 * reverse for sunken.  Use WB palette colours. */
                extern uint32_t WB_DARK_GREY, WB_LIGHT_GREY;
                FB_DrawRect(x, y, width, height, sunken ? WB_LIGHT_GREY : WB_DARK_GREY);
                FB_DrawRect(x + 1, y + 1, width - 2, height - 2,
                            sunken ? WB_DARK_GREY : WB_LIGHT_GREY);
            }
            return 1;
        }
        case IM_ERASE:
        case IM_ERASEFRAME: {
            /* Fill the bevel box area with the background pen. */
            int32_t xoff = (method == IM_ERASE) ?
                (int32_t)mem_u32(msg + IMERASE_OFF_OFFSETX) :
                (int32_t)mem_u32(msg + IMERASEFRAME_OFF_OFFSETX);
            int32_t yoff = (method == IM_ERASE) ?
                (int32_t)mem_u32(msg + IMERASE_OFF_OFFSETY) :
                (int32_t)mem_u32(msg + IMERASEFRAME_OFF_OFFSETY);
            int16_t left   = mem_s16(obj + BBBOX_OFF_FRAME);
            int16_t top    = mem_s16(obj + BBBOX_OFF_FRAME + 2);
            int16_t width  = mem_s16(obj + BBBOX_OFF_FRAME + 4);
            int16_t height = mem_s16(obj + BBBOX_OFF_FRAME + 6);
            if (width > 0 && height > 0) {
                extern uint32_t WB_GREY;
                FB_FillRect(xoff + left, yoff + top, width, height, WB_GREY);
            }
            return 1;
        }
        default:
            return imageclass_dispatch(cls, obj, msg);
    }
}

/* =========================================================================
 * menuitemclass — companion to menuclass for individual menu items
 * ========================================================================= */
#define BMITEM_OFF_LABEL    0    /* MA_Label string/IntuiText */
#define BMITEM_OFF_COMMAND  4    /* command key */
#define BMITEM_OFF_CHECKED  8    /* checked state */
#define BMITEM_OFF_DISABLED 9
#define BMITEM_OFF_NEXT    10    /* next item in chain */
#define BMITEM_OFF_SUB     14    /* submenu (menuclass child) */
#define BMITEM_OFF_PARENT  18
#define BMITEM_INST_SIZE   22

static int menuitem_set_tag(uint32_t tag, uint32_t data, void *ctx)
{
    uint32_t obj = (uint32_t)(uintptr_t)ctx;
    switch (tag) {
        case MA_Label:     mem_w32(obj + BMITEM_OFF_LABEL, data); break;
        case MA_Command:   g_ram[obj + BMITEM_OFF_COMMAND] = (uint8_t)data; break;
        case MA_Checked:   g_ram[obj + BMITEM_OFF_CHECKED] = (uint8_t)(data ? 1 : 0); break;
        case MA_Disabled:  g_ram[obj + BMITEM_OFF_DISABLED] = (uint8_t)(data ? 1 : 0); break;
        case MA_Sub:       mem_w32(obj + BMITEM_OFF_SUB, data); break;
        default: break;
    }
    return 1;
}

static uint32_t menuitemclass_dispatch(uint32_t cls, uint32_t obj, uint32_t msg)
{
    uint32_t method = mem_u32(msg + MSG_OFF_METHODID);
    switch (method) {
        case OM_NEW: {
            uint32_t tags = mem_u32(msg + OPNEW_OFF_ATTRLIST);
            memset(&g_ram[obj], 0, BMITEM_INST_SIZE);
            walk_tags(tags, menuitem_set_tag, (void*)(uintptr_t)obj);
            return 1;
        }
        case OM_SET: {
            uint32_t tags = mem_u32(msg + OPSET_OFF_ATTRLIST);
            walk_tags(tags, menuitem_set_tag, (void*)(uintptr_t)obj);
            return 1;
        }
        case OM_GET: {
            uint32_t attr = mem_u32(msg + OPGET_OFF_ATTRID);
            uint32_t store = mem_u32(msg + OPGET_OFF_STORAGE);
            if (!store) return 0;
            switch (attr) {
                case MA_Label:    mem_w32(store, mem_u32(obj + BMITEM_OFF_LABEL)); return 1;
                case MA_Command:  mem_w32(store, g_ram[obj + BMITEM_OFF_COMMAND]); return 1;
                case MA_Checked:  mem_w32(store, g_ram[obj + BMITEM_OFF_CHECKED] ? 1 : 0); return 1;
                case MA_Disabled: mem_w32(store, g_ram[obj + BMITEM_OFF_DISABLED] ? 1 : 0); return 1;
                case MA_Sub:      mem_w32(store, mem_u32(obj + BMITEM_OFF_SUB)); return 1;
                default: return 0;
            }
        }
        default:
            return menuclass_dispatch(cls, obj, msg);
    }
}

/* =========================================================================
 * fillrectclass — backfill hook class (subclass of rootclass)
 * ========================================================================= */
#define BFILL_OFF_HOOK   0    /* guest Hook* for backfill */
#define BFILL_OFF_TYPE   4    /* fill type */
#define BFILL_INST_SIZE  8

static int fillrect_set_tag(uint32_t tag, uint32_t data, void *ctx)
{
    uint32_t obj = (uint32_t)(uintptr_t)ctx;
    switch (tag) {
        case FILLRECT_FillHook: mem_w32(obj + BFILL_OFF_HOOK, data); break;
        case FILLRECT_FillType: mem_w32(obj + BFILL_OFF_TYPE, data); break;
        default: break;
    }
    return 1;
}

static uint32_t fillrectclass_dispatch(uint32_t cls, uint32_t obj, uint32_t msg)
{
    uint32_t method = mem_u32(msg + MSG_OFF_METHODID);
    switch (method) {
        case OM_NEW: {
            uint32_t tags = mem_u32(msg + OPNEW_OFF_ATTRLIST);
            memset(&g_ram[obj], 0, BFILL_INST_SIZE);
            walk_tags(tags, fillrect_set_tag, (void*)(uintptr_t)obj);
            return 1;
        }
        case OM_SET: {
            uint32_t tags = mem_u32(msg + OPSET_OFF_ATTRLIST);
            walk_tags(tags, fillrect_set_tag, (void*)(uintptr_t)obj);
            return 1;
        }
        case OM_GET: {
            uint32_t attr = mem_u32(msg + OPGET_OFF_ATTRID);
            uint32_t store = mem_u32(msg + OPGET_OFF_STORAGE);
            if (!store) return 0;
            switch (attr) {
                case FILLRECT_FillHook: mem_w32(store, mem_u32(obj + BFILL_OFF_HOOK)); return 1;
                case FILLRECT_FillType: mem_w32(store, mem_u32(obj + BFILL_OFF_TYPE)); return 1;
                default: return 0;
            }
        }
        default:
            return rootclass_dispatch(cls, obj, msg);
    }
}

/* =========================================================================
 * sysgclass — system gadget class (subclass of gadgetclass)
 * ========================================================================= */
/* SYSIA_Which is stored in the gadget's SpecialInfo field.
 * No extra instance data needed. */
#define BSYSG_INST_SIZE  0

static int sysg_set_tag(uint32_t tag, uint32_t data, void *ctx)
{
    uint32_t obj = (uint32_t)(uintptr_t)ctx;
    switch (tag) {
        case SYSIA_Which:
            mem_w32(obj + GAD_OFF_SPECIALINFO, data);
            break;
        case SYSIA_DrawInfo:
            /* DrawInfo is not stored per-gadget in UAOS; ignore. */
            break;
        default:
            break;
    }
    return 1;
}

static uint32_t sysgclass_dispatch(uint32_t cls, uint32_t obj, uint32_t msg)
{
    uint32_t method = mem_u32(msg + MSG_OFF_METHODID);
    switch (method) {
        case OM_NEW: {
            uint32_t tags = mem_u32(msg + OPNEW_OFF_ATTRLIST);
            /* Let gadgetclass handle GA_* first. */
            uint32_t r = gadgetclass_dispatch(cls, obj, msg);
            if (!r) return 0;
            walk_tags(tags, sysg_set_tag, (void*)(uintptr_t)obj);
            mem_w16(obj + GAD_OFF_GADGETTYPE, GTYP_SYSGADGET | GTYP_BOOLGADGET);
            return r;
        }
        case OM_SET: {
            uint32_t tags = mem_u32(msg + OPSET_OFF_ATTRLIST);
            walk_tags(tags, sysg_set_tag, (void*)(uintptr_t)obj);
            return gadgetclass_dispatch(cls, obj, msg);
        }
        case OM_GET: {
            uint32_t attr = mem_u32(msg + OPGET_OFF_ATTRID);
            uint32_t store = mem_u32(msg + OPGET_OFF_STORAGE);
            if (!store) return 0;
            if (attr == SYSIA_Which) {
                mem_w32(store, mem_u32(obj + GAD_OFF_SPECIALINFO));
                return 1;
            }
            return gadgetclass_dispatch(cls, obj, msg);
        }
        default:
            return gadgetclass_dispatch(cls, obj, msg);
    }
}

/* =========================================================================
 * groupgclass — group gadget for layout + mutual exclusion
 * (subclass of gadgetclass, V36)
 * ========================================================================= */
#define BGRP_OFF_CHILDREN  0    /* head of child gadget list */
#define BGRP_OFF_ACTIVE    4    /* currently active child */
#define BGRP_OFF_LABELS    8    /* label array pointer */
#define BGRP_OFF_ACTIVEKEY 12   /* keyboard shortcut for active child */
#define BGRP_INST_SIZE    16

static int groupg_set_tag(uint32_t tag, uint32_t data, void *ctx)
{
    uint32_t obj = (uint32_t)(uintptr_t)ctx;
    switch (tag) {
        case GROUPG_Active:     mem_w32(obj + BGRP_OFF_ACTIVE, data); break;
        case GROUPG_Labels:     mem_w32(obj + BGRP_OFF_LABELS, data); break;
        case GROUPG_ActiveKey:  mem_w32(obj + BGRP_OFF_ACTIVEKEY, data); break;
        default: break;
    }
    return 1;
}

static uint32_t groupgclass_dispatch(uint32_t cls, uint32_t obj, uint32_t msg)
{
    uint32_t method = mem_u32(msg + MSG_OFF_METHODID);
    switch (method) {
        case OM_NEW: {
            uint32_t tags = mem_u32(msg + OPNEW_OFF_ATTRLIST);
            uint32_t r = gadgetclass_dispatch(cls, obj, msg);
            if (!r) return 0;
            walk_tags(tags, groupg_set_tag, (void*)(uintptr_t)obj);
            return r;
        }
        case OM_SET: {
            uint32_t tags = mem_u32(msg + OPSET_OFF_ATTRLIST);
            walk_tags(tags, groupg_set_tag, (void*)(uintptr_t)obj);
            return gadgetclass_dispatch(cls, obj, msg);
        }
        case OM_GET: {
            uint32_t attr = mem_u32(msg + OPGET_OFF_ATTRID);
            uint32_t store = mem_u32(msg + OPGET_OFF_STORAGE);
            if (!store) return 0;
            switch (attr) {
                case GROUPG_Children:  mem_w32(store, mem_u32(obj + BGRP_OFF_CHILDREN)); return 1;
                case GROUPG_Active:    mem_w32(store, mem_u32(obj + BGRP_OFF_ACTIVE)); return 1;
                case GROUPG_Labels:    mem_w32(store, mem_u32(obj + BGRP_OFF_LABELS)); return 1;
                case GROUPG_ActiveKey: mem_w32(store, mem_u32(obj + BGRP_OFF_ACTIVEKEY)); return 1;
                default: return gadgetclass_dispatch(cls, obj, msg);
            }
        }
        case OM_ADDMEMBER: {
            /* Add a child gadget to the group's child list. */
            uint32_t child = mem_u32(msg + 4);
            if (!child) return 0;
            uint32_t old = mem_u32(obj + BGRP_OFF_CHILDREN);
            mem_w32(obj + BGRP_OFF_CHILDREN, child);
            /* Link child to next.  We store the next pointer in the
             * gadget's GA_Next field (GAD_OFF_NEXTGADGET = 0). */
            mem_w32(child + GAD_OFF_NEXTGADGET, old);
            return 1;
        }
        case OM_REMMEMBER: {
            uint32_t child = mem_u32(msg + 4);
            if (!child) return 0;
            uint32_t prev = 0;
            uint32_t cur = mem_u32(obj + BGRP_OFF_CHILDREN);
            while (cur) {
                uint32_t next = mem_u32(cur + GAD_OFF_NEXTGADGET);
                if (cur == child) {
                    if (prev) mem_w32(prev + GAD_OFF_NEXTGADGET, next);
                    else      mem_w32(obj + BGRP_OFF_CHILDREN, next);
                    mem_w32(cur + GAD_OFF_NEXTGADGET, 0);
                    return 1;
                }
                prev = cur;
                cur = next;
            }
            return 0;
        }
        case OM_NOTIFY: {
            /* Broadcast OM_UPDATE to all children in the group. */
            uint32_t child = mem_u32(obj + BGRP_OFF_CHILDREN);
            while (child) {
                uint32_t next = mem_u32(child + GAD_OFF_NEXTGADGET);
                mem_w32(msg + MSG_OFF_METHODID, OM_UPDATE);
                UAOS_BOOPSI_Dispatch(child, OM_UPDATE, msg, 0);
                mem_w32(msg + MSG_OFF_METHODID, OM_NOTIFY);
                child = next;
            }
            return 1;
        }
        default:
            return gadgetclass_dispatch(cls, obj, msg);
    }
}

/* =========================================================================
 * propgclass — full BOOPSI proportional gadget
 * (subclass of gadgetclass, V36)
 * ========================================================================= */
/* The prop gadget instance data stores the PropInfo fields that in classic
 * AmigaOS live in Gadget.SpecialInfo.  We allocate a PropInfo block in
 * guest RAM and point SpecialInfo at it. */
#define BPROP_OFF_PROPINFO  0    /* pointer to guest PropInfo struct */
#define BPROP_INST_SIZE     4

static int propg_set_tag(uint32_t tag, uint32_t data, void *ctx)
{
    uint32_t obj = (uint32_t)(uintptr_t)ctx;
    uint32_t pi = mem_u32(obj + BPROP_OFF_PROPINFO);
    if (!pi) return 1;
    switch (tag) {
        case PGA_Top: {
            /* PGA_Top sets both HorizPot and VertPot (simplified). */
            uint16_t pot = (uint16_t)data;
            mem_w16(pi + PROP_OFF_HORIZPOT, pot);
            mem_w16(pi + PROP_OFF_VERTPOT, pot);
            break;
        }
        case PGA_Total: {
            uint16_t body_base = (uint16_t)data;
            /* Body = (Visible / Total) * MAX_BODY; we set a simplified
             * body proportional to visible/total.  For now store total
             * so the gadget can compute body later. */
            (void)body_base;
            break;
        }
        case PGA_Visible: {
            uint16_t vis = (uint16_t)data;
            /* Store visible as the body value if total is known.
             * Simplified: body = vis * 0xFFFF / total (deferred). */
            (void)vis;
            break;
        }
        case PGA_Freedom: {
            uint16_t flags = mem_u16(pi + PROP_OFF_FLAGS);
            flags &= ~(0x0003);  /* clear FREEHORIZ/FREEVERT */
            if (data == PGA_FREEHORIZ || data == PGA_FREEBOTH) flags |= 0x0001;
            if (data == PGA_FREEVERT  || data == PGA_FREEBOTH) flags |= 0x0002;
            mem_w16(pi + PROP_OFF_FLAGS, flags);
            break;
        }
        case PGA_NewLook: {
            uint16_t flags = mem_u16(pi + PROP_OFF_FLAGS);
            if (data) flags |= 0x0004;  /* PROPNEWLOOK */
            mem_w16(pi + PROP_OFF_FLAGS, flags);
            break;
        }
        case PGA_Borderless: {
            uint16_t flags = mem_u16(pi + PROP_OFF_FLAGS);
            if (data) flags |= 0x0010;  /* PROPBORDERLESS */
            mem_w16(pi + PROP_OFF_FLAGS, flags);
            break;
        }
        case PGA_TopBorder: {
            /* PGA_TopBorder: draw a top border line for the prop gadget.
             * Stored as a flag in the gadget's Flags field (bit 12). */
            uint16_t flags = mem_u16(obj + GAD_OFF_FLAGS);
            if (data) flags |= 0x1000;
            else      flags &= ~0x1000;
            mem_w16(obj + GAD_OFF_FLAGS, flags);
            break;
        }
        case PGA_HorizPot:  mem_w16(pi + PROP_OFF_HORIZPOT, (uint16_t)data); break;
        case PGA_VertPot:   mem_w16(pi + PROP_OFF_VERTPOT,  (uint16_t)data); break;
        case PGA_HorizBody: mem_w16(pi + PROP_OFF_HORIZBODY, (uint16_t)data); break;
        case PGA_VertBody:  mem_w16(pi + PROP_OFF_VERTBODY,  (uint16_t)data); break;
        default: break;
    }
    return 1;
}

static uint32_t propgclass_dispatch(uint32_t cls, uint32_t obj, uint32_t msg)
{
    uint32_t method = mem_u32(msg + MSG_OFF_METHODID);
    switch (method) {
        case OM_NEW: {
            uint32_t tags = mem_u32(msg + OPNEW_OFF_ATTRLIST);
            uint32_t r = gadgetclass_dispatch(cls, obj, msg);
            if (!r) return 0;
            /* Allocate a PropInfo structure in guest RAM. */
            uint32_t pi = intu_alloc(32);  /* PropInfo is ~18 bytes, round up */
            if (pi) {
                for (int i = 0; i < 32; i++) g_ram[pi + i] = 0;
                mem_w32(obj + BPROP_OFF_PROPINFO, pi);
                mem_w32(obj + GAD_OFF_SPECIALINFO, pi);
            }
            mem_w16(obj + GAD_OFF_GADGETTYPE, GTYP_PROPGADGET);
            walk_tags(tags, propg_set_tag, (void*)(uintptr_t)obj);
            return r;
        }
        case OM_DISPOSE: {
            uint32_t pi = mem_u32(obj + BPROP_OFF_PROPINFO);
            if (pi) intu_free(pi);
            return gadgetclass_dispatch(cls, obj, msg);
        }
        case OM_SET: {
            uint32_t tags = mem_u32(msg + OPSET_OFF_ATTRLIST);
            walk_tags(tags, propg_set_tag, (void*)(uintptr_t)obj);
            return gadgetclass_dispatch(cls, obj, msg);
        }
        case OM_GET: {
            uint32_t attr = mem_u32(msg + OPGET_OFF_ATTRID);
            uint32_t store = mem_u32(msg + OPGET_OFF_STORAGE);
            if (!store) return 0;
            uint32_t pi = mem_u32(obj + BPROP_OFF_PROPINFO);
            switch (attr) {
                case PGA_HorizPot:  mem_w32(store, mem_u16(pi + PROP_OFF_HORIZPOT)); return 1;
                case PGA_VertPot:   mem_w32(store, mem_u16(pi + PROP_OFF_VERTPOT)); return 1;
                case PGA_HorizBody: mem_w32(store, mem_u16(pi + PROP_OFF_HORIZBODY)); return 1;
                case PGA_VertBody:  mem_w32(store, mem_u16(pi + PROP_OFF_VERTBODY)); return 1;
                default: return gadgetclass_dispatch(cls, obj, msg);
            }
        }
        default:
            return gadgetclass_dispatch(cls, obj, msg);
    }
}

/* =========================================================================
 * strgclass — full BOOPSI string gadget
 * (subclass of gadgetclass, V36)
 * ========================================================================= */
/* The string gadget instance data stores a StringInfo structure in guest
 * RAM, pointed to by Gadget.SpecialInfo.  The StringInfo holds the buffer,
 * undo buffer, cursor position, max chars, etc. */
#define BSTR_OFF_STRINFO   0    /* pointer to guest StringInfo struct */
#define BSTR_INST_SIZE     4

/* StringInfo structure size (from NDK): ~26 bytes, round to 32 */
#define BSTR_SI_SIZE       32

/* Helper: write a 16-bit big-endian value to guest RAM. */
static void mem_u16_set(uint32_t addr, uint16_t val)
{
    g_ram[addr]     = (uint8_t)(val >> 8);
    g_ram[addr + 1] = (uint8_t)(val & 0xFF);
}

static int strg_set_tag(uint32_t tag, uint32_t data, void *ctx)
{
    uint32_t obj = (uint32_t)(uintptr_t)ctx;
    uint32_t si = mem_u32(obj + BSTR_OFF_STRINFO);
    switch (tag) {
        case STRINGA_TextVal: {
            /* Copy the guest string into the buffer. */
            if (si) {
                uint16_t maxchars = mem_u16(si + SI_OFF_MAXCHARS);
                uint32_t buf = mem_u32(si + SI_OFF_BUFFER);
                if (buf && maxchars > 0) {
                    int i = 0;
                    while (i < maxchars - 1 && data + i < GUEST_RAM_SIZE &&
                           g_ram[data + i] != 0) {
                        g_ram[buf + i] = g_ram[data + i];
                        i++;
                    }
                    g_ram[buf + i] = 0;
                    mem_u16_set(si + SI_OFF_NUMCHARS, (uint16_t)i);
                    mem_u16_set(si + SI_OFF_BUFFERPOS, 0);
                }
            }
            break;
        }
        case STRINGA_MaxChars: {
            if (si) {
                mem_u16_set(si + SI_OFF_MAXCHARS, (uint16_t)data);
                /* Allocate buffer if not already present. */
                uint32_t buf = mem_u32(si + SI_OFF_BUFFER);
                if (!buf && data > 0) {
                    buf = intu_alloc((uint32_t)data);
                    if (buf) {
                        for (int i = 0; i < (int)data; i++) g_ram[buf + i] = 0;
                        mem_w32(si + SI_OFF_BUFFER, buf);
                        uint32_t undo = intu_alloc((uint32_t)data);
                        if (undo) {
                            for (int i = 0; i < (int)data; i++) g_ram[undo + i] = 0;
                            mem_w32(si + SI_OFF_UNDOBUFFER, undo);
                        }
                    }
                }
            }
            break;
        }
        case STRINGA_BufferPos: {
            if (si) mem_u16_set(si + SI_OFF_BUFFERPOS, (uint16_t)data);
            break;
        }
        case STRINGA_DispPos: {
            if (si) mem_u16_set(si + SI_OFF_DISPPOS, (uint16_t)data);
            break;
        }
        case STRINGA_EditHook: {
            /* Store edit hook in the gadget's UserData for now. */
            mem_w32(obj + GAD_OFF_USERDATA, data);
            break;
        }
        default: break;
    }
    return 1;
}

static uint32_t strgclass_dispatch(uint32_t cls, uint32_t obj, uint32_t msg)
{
    uint32_t method = mem_u32(msg + MSG_OFF_METHODID);
    switch (method) {
        case OM_NEW: {
            uint32_t tags = mem_u32(msg + OPNEW_OFF_ATTRLIST);
            uint32_t r = gadgetclass_dispatch(cls, obj, msg);
            if (!r) return 0;
            /* Allocate a StringInfo structure in guest RAM. */
            uint32_t si = intu_alloc(BSTR_SI_SIZE);
            if (si) {
                for (int i = 0; i < BSTR_SI_SIZE; i++) g_ram[si + i] = 0;
                mem_w32(obj + BSTR_OFF_STRINFO, si);
                mem_w32(obj + GAD_OFF_SPECIALINFO, si);
            }
            mem_w16(obj + GAD_OFF_GADGETTYPE, GTYP_STRGADGET);
            walk_tags(tags, strg_set_tag, (void*)(uintptr_t)obj);
            return r;
        }
        case OM_DISPOSE: {
            uint32_t si = mem_u32(obj + BSTR_OFF_STRINFO);
            if (si) {
                uint32_t buf  = mem_u32(si + SI_OFF_BUFFER);
                uint32_t undo = mem_u32(si + SI_OFF_UNDOBUFFER);
                if (buf)  intu_free(buf);
                if (undo) intu_free(undo);
                intu_free(si);
            }
            return gadgetclass_dispatch(cls, obj, msg);
        }
        case OM_SET: {
            uint32_t tags = mem_u32(msg + OPSET_OFF_ATTRLIST);
            walk_tags(tags, strg_set_tag, (void*)(uintptr_t)obj);
            return gadgetclass_dispatch(cls, obj, msg);
        }
        case OM_GET: {
            uint32_t attr = mem_u32(msg + OPGET_OFF_ATTRID);
            uint32_t store = mem_u32(msg + OPGET_OFF_STORAGE);
            if (!store) return 0;
            uint32_t si = mem_u32(obj + BSTR_OFF_STRINFO);
            switch (attr) {
                case STRINGA_BufferPos:
                    if (si) { mem_w32(store, mem_u16(si + SI_OFF_BUFFERPOS)); return 1; }
                    return 0;
                case STRINGA_DispPos:
                    if (si) { mem_w32(store, mem_u16(si + SI_OFF_DISPPOS)); return 1; }
                    return 0;
                case STRINGA_MaxChars:
                    if (si) { mem_w32(store, mem_u16(si + SI_OFF_MAXCHARS)); return 1; }
                    return 0;
                case STRINGA_TextVal:
                    if (si) { mem_w32(store, mem_u32(si + SI_OFF_BUFFER)); return 1; }
                    return 0;
                default: return gadgetclass_dispatch(cls, obj, msg);
            }
        }
        case GM_GOACTIVE: {
            /* Activate the string gadget: set GFLG_SELECTED and return
             * GMR_MEACTIVE so Intuition keeps sending input events. */
            uint16_t flags = mem_u16(obj + GAD_OFF_FLAGS);
            flags |= GFLG_SELECTED;
            mem_w16(obj + GAD_OFF_FLAGS, flags);
            return GMR_MEACTIVE;
        }
        case GM_HANDLEINPUT: {
            /* Process keyboard input for the string gadget.
             * Return SGA_* action codes OR'd with GMR_* flags. */
            uint32_t ievent = mem_u32(msg + GMI_OFF_IEVENT);
            if (ievent && ievent + 12 <= GUEST_RAM_SIZE) {
                uint16_t ie_class = mem_u16(ievent + IE_OFF_CLASS);
                uint16_t ie_code  = mem_u16(ievent + IE_OFF_CODE);
                if (ie_class == IECLASS_RAWKEY) {
                    /* Key press (bit 0 of ie_Code clear = pressed). */
                    if (!(ie_code & 0x8000)) {
                        uint16_t key = ie_code & 0x7F;
                        if (key == IEKEY_RETURN) {
                            /* Commit and exit. */
                            uint16_t flags = mem_u16(obj + GAD_OFF_FLAGS);
                            flags &= ~GFLG_SELECTED;
                            mem_w16(obj + GAD_OFF_FLAGS, flags);
                            return GMR_NOREUSE | SGA_EXIT | SGA_END;
                        }
                        if (key == IEKEY_TAB) {
                            /* Move to next string gadget. */
                            uint16_t flags = mem_u16(obj + GAD_OFF_FLAGS);
                            flags &= ~GFLG_SELECTED;
                            mem_w16(obj + GAD_OFF_FLAGS, flags);
                            return GMR_NOREUSE | SGA_NEXT | SGA_END;
                        }
                        if (key == IEKEY_ESCAPE) {
                            /* Cancel and exit. */
                            uint16_t flags = mem_u16(obj + GAD_OFF_FLAGS);
                            flags &= ~GFLG_SELECTED;
                            mem_w16(obj + GAD_OFF_FLAGS, flags);
                            return GMR_NOREUSE | SGA_EXIT | SGA_END;
                        }
                        /* Any other key: request a redraw. */
                        return GMR_MEACTIVE | SGA_REDISPLAY;
                    }
                }
            }
            return GMR_MEACTIVE;
        }
        case GM_GOINACTIVE: {
            /* Deactivate the string gadget. */
            uint16_t flags = mem_u16(obj + GAD_OFF_FLAGS);
            flags &= ~GFLG_SELECTED;
            mem_w16(obj + GAD_OFF_FLAGS, flags);
            return GMR_NOREUSE | SGA_USE;
        }
        default:
            return gadgetclass_dispatch(cls, obj, msg);
    }
}

/* =========================================================================
 * icclass — interconnection class (subclass of rootclass)
 *
 * Provides the OM_NOTIFY → OM_UPDATE broadcast infrastructure used by
 * modelclass and other notification sources.  Stores an ICA_TARGET
 * object pointer and an ICA_MAP tag-translation table.  When OM_NOTIFY
 * is received, the class rewrites the MethodID to OM_UPDATE and
 * dispatches it to the target.  ICM_INVOKE dispatches an arbitrary
 * method to the target.  OM_ADDMEMBER/OM_REMMEMBER manage a simple
 * dependent list so multiple targets can be notified.
 * ========================================================================= */
#define BIC_OFF_TARGET    0    /* ICA_TARGET: primary notification target */
#define BIC_OFF_MAP       4    /* ICA_MAP: tag-mapping table (stored, unused) */
#define BIC_OFF_DEPS      8    /* head of dependent list */
#define BIC_INST_SIZE    12

static int ic_set_tag(uint32_t tag, uint32_t data, void *ctx)
{
    uint32_t obj = (uint32_t)(uintptr_t)ctx;
    switch (tag) {
        case ICA_TARGET: mem_w32(obj + BIC_OFF_TARGET, data); break;
        case ICA_MAP:    mem_w32(obj + BIC_OFF_MAP, data); break;
        default: break;
    }
    return 1;
}

static uint32_t icclass_dispatch(uint32_t cls, uint32_t obj, uint32_t msg)
{
    (void)cls;
    uint32_t method = mem_u32(msg + MSG_OFF_METHODID);
    switch (method) {
        case OM_NEW: {
            uint32_t tags = mem_u32(msg + OPNEW_OFF_ATTRLIST);
            memset(&g_ram[obj], 0, BIC_INST_SIZE);
            walk_tags(tags, ic_set_tag, (void*)(uintptr_t)obj);
            return 1;
        }
        case OM_DISPOSE:
            return 1;
        case OM_SET: {
            uint32_t tags = mem_u32(msg + OPSET_OFF_ATTRLIST);
            walk_tags(tags, ic_set_tag, (void*)(uintptr_t)obj);
            return 1;
        }
        case OM_GET: {
            uint32_t attr = mem_u32(msg + OPGET_OFF_ATTRID);
            uint32_t store = mem_u32(msg + OPGET_OFF_STORAGE);
            if (!store) return 0;
            switch (attr) {
                case ICA_TARGET: mem_w32(store, mem_u32(obj + BIC_OFF_TARGET)); return 1;
                case ICA_MAP:    mem_w32(store, mem_u32(obj + BIC_OFF_MAP)); return 1;
                default: return 0;
            }
        }
        case OM_ADDMEMBER: {
            /* Add a dependent to the linked list (via ln_Succ at offset 0). */
            uint32_t dep = mem_u32(msg + 4);
            if (!dep) return 0;
            uint32_t old = mem_u32(obj + BIC_OFF_DEPS);
            mem_w32(obj + BIC_OFF_DEPS, dep);
            mem_w32(dep, old);  /* link: dep->next = old head */
            return 1;
        }
        case OM_REMMEMBER: {
            uint32_t dep = mem_u32(msg + 4);
            if (!dep) return 0;
            uint32_t prev = 0;
            uint32_t cur  = mem_u32(obj + BIC_OFF_DEPS);
            while (cur) {
                uint32_t next = mem_u32(cur);
                if (cur == dep) {
                    if (prev) mem_w32(prev, next);
                    else      mem_w32(obj + BIC_OFF_DEPS, next);
                    mem_w32(cur, 0);
                    return 1;
                }
                prev = cur;
                cur  = next;
            }
            return 0;
        }
        case OM_NOTIFY: {
            /* Broadcast OM_UPDATE to the primary target and all dependents. */
            uint32_t target = mem_u32(obj + BIC_OFF_TARGET);
            if (target) {
                mem_w32(msg + MSG_OFF_METHODID, OM_UPDATE);
                UAOS_BOOPSI_Dispatch(target, OM_UPDATE, msg, 0);
                mem_w32(msg + MSG_OFF_METHODID, OM_NOTIFY);
            }
            uint32_t dep = mem_u32(obj + BIC_OFF_DEPS);
            while (dep) {
                uint32_t next = mem_u32(dep);
                mem_w32(msg + MSG_OFF_METHODID, OM_UPDATE);
                UAOS_BOOPSI_Dispatch(dep, OM_UPDATE, msg, 0);
                mem_w32(msg + MSG_OFF_METHODID, OM_NOTIFY);
                dep = next;
            }
            return 1;
        }
        case ICM_INVOKE: {
            /* Dispatch an arbitrary method to the target.  msg+4 = methodID,
             * msg+8 = message pointer. */
            uint32_t target = mem_u32(obj + BIC_OFF_TARGET);
            if (!target) return 0;
            uint32_t sub_method = mem_u32(msg + 4);
            uint32_t sub_msg    = mem_u32(msg + 8);
            return UAOS_BOOPSI_Dispatch(target, sub_method, sub_msg, 0);
        }
        case ICM_SET: {
            /* Forward OM_SET to the target. */
            uint32_t target = mem_u32(obj + BIC_OFF_TARGET);
            if (!target) return 0;
            return UAOS_BOOPSI_Dispatch(target, OM_SET, msg, 0);
        }
        case ICM_CHECKNOTIFY: {
            /* Check if this object should notify; always succeeds. */
            return 1;
        }
        default:
            return rootclass_dispatch(cls, obj, msg);
    }
}

/* =========================================================================
 * buttongclass — button gadget (subclass of gadgetclass, V36)
 *
 * A simple push-button gadget that uses BOOLGADGET_* / BTNF_* tags.
 * Unlike frbuttonclass (which is the "fast" boolean button), buttongclass
 * supports both text and image labels, highlight modes, and bevel styles.
 * The checked/pressed state is stored in GFLG_SELECTED.
 * ========================================================================= */
#define BBTN_INST_SIZE  0  /* no extra instance data beyond gadgetclass */

static int buttong_set_tag(uint32_t tag, uint32_t data, void *ctx)
{
    uint32_t obj = (uint32_t)(uintptr_t)ctx;
    switch (tag) {
        case BTNF_Title:
        case BTNF_Text: {
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
        case BTNF_Image:     mem_w32(obj + GAD_OFF_GADGETRENDER, data); break;
        case BTNF_SelImage:  mem_w32(obj + GAD_OFF_SELECTRENDER, data); break;
        case BTNF_Disabled: {
            uint16_t f = mem_u16(obj + GAD_OFF_FLAGS);
            if (data) f |= GFLG_DISABLED; else f &= ~GFLG_DISABLED;
            mem_w16(obj + GAD_OFF_FLAGS, f);
            break;
        }
        case BTNF_Pushed:
        case BOOLGADGET_Checked: {
            uint16_t f = mem_u16(obj + GAD_OFF_FLAGS);
            if (data) f |= GFLG_SELECTED; else f &= ~GFLG_SELECTED;
            mem_w16(obj + GAD_OFF_FLAGS, f);
            break;
        }
        case BOOLGADGET_Text:
            mem_w32(obj + GAD_OFF_GADGETTEXT, data);
            break;
        case BOOLGADGET_Image:
            mem_w32(obj + GAD_OFF_GADGETRENDER, data);
            break;
        case BOOLGADGET_SelImage:
            mem_w32(obj + GAD_OFF_SELECTRENDER, data);
            break;
        /* BUTTONA_* / BTNG_* — 3.2 Reaction aliases for BTNF_*
         * (BTNG_* are #defined as BUTTONA_* so they share case labels) */
        case BUTTONA_Title:
        case BUTTONA_Text: {
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
        case BUTTONA_Image:
            mem_w32(obj + GAD_OFF_GADGETRENDER, data);
            break;
        case BUTTONA_SelImage:
            mem_w32(obj + GAD_OFF_SELECTRENDER, data);
            break;
        case BUTTONA_Disabled: {
            uint16_t f = mem_u16(obj + GAD_OFF_FLAGS);
            if (data) f |= GFLG_DISABLED; else f &= ~GFLG_DISABLED;
            mem_w16(obj + GAD_OFF_FLAGS, f);
            break;
        }
        case BUTTONA_Pushed: {
            uint16_t f = mem_u16(obj + GAD_OFF_FLAGS);
            if (data) f |= GFLG_SELECTED; else f &= ~GFLG_SELECTED;
            mem_w16(obj + GAD_OFF_FLAGS, f);
            break;
        }
        default: break;
    }
    return 1;
}

static uint32_t buttongclass_dispatch(uint32_t cls, uint32_t obj, uint32_t msg)
{
    uint32_t method = mem_u32(msg + MSG_OFF_METHODID);
    switch (method) {
        case OM_NEW: {
            uint32_t tags = mem_u32(msg + OPNEW_OFF_ATTRLIST);
            uint32_t r = gadgetclass_dispatch(cls, obj, msg);
            if (!r) return 0;
            walk_tags(tags, buttong_set_tag, (void*)(uintptr_t)obj);
            mem_w16(obj + GAD_OFF_GADGETTYPE, GTYP_BOOLGADGET);
            return r;
        }
        case OM_SET: {
            uint32_t tags = mem_u32(msg + OPSET_OFF_ATTRLIST);
            walk_tags(tags, buttong_set_tag, (void*)(uintptr_t)obj);
            return gadgetclass_dispatch(cls, obj, msg);
        }
        case OM_GET: {
            uint32_t attr = mem_u32(msg + OPGET_OFF_ATTRID);
            uint32_t store = mem_u32(msg + OPGET_OFF_STORAGE);
            if (!store) return 0;
            switch (attr) {
                case BTNF_Pushed:
                case BOOLGADGET_Checked:
                case BUTTONA_Pushed:
                    mem_w32(store, (mem_u16(obj + GAD_OFF_FLAGS) & GFLG_SELECTED) ? 1 : 0);
                    return 1;
                case BTNF_Text:
                case BOOLGADGET_Text:
                case BUTTONA_Text:
                    mem_w32(store, mem_u32(obj + GAD_OFF_GADGETTEXT));
                    return 1;
                case BTNF_Image:
                case BOOLGADGET_Image:
                case BUTTONA_Image:
                    mem_w32(store, mem_u32(obj + GAD_OFF_GADGETRENDER));
                    return 1;
                case BUTTONA_SelImage:
                    mem_w32(store, mem_u32(obj + GAD_OFF_SELECTRENDER));
                    return 1;
                case BUTTONA_Disabled:
                    mem_w32(store, (mem_u16(obj + GAD_OFF_FLAGS) & GFLG_DISABLED) ? 1 : 0);
                    return 1;
                default:
                    return gadgetclass_dispatch(cls, obj, msg);
            }
        }
        default:
            return gadgetclass_dispatch(cls, obj, msg);
    }
}

/* =========================================================================
 * slidergclass — slider gadget (subclass of gadgetclass, V36)
 *
 * A slider is a prop gadget with a fixed-size knob that the user drags
 * to set a level value between Min and Max.  It uses a PropInfo internally
 * (like propgclass) but exposes SLIDER_* integer attributes.
 * ========================================================================= */
#define BSLD_OFF_PROPINFO  0    /* pointer to guest PropInfo struct */
#define BSLD_OFF_MIN       4    /* SLIDER_Min (uint16) */
#define BSLD_OFF_MAX       6    /* SLIDER_Max (uint16) */
#define BSLD_OFF_LEVEL     8    /* SLIDER_Level / SLIDER_Top (uint16) */
#define BSLD_OFF_ORIENT   10    /* 0 = vertical, 1 = horizontal */
#define BSLD_OFF_KNOBPIX  12    /* SLIDER_KnobPixels */
#define BSLD_INST_SIZE    16

static void slider_recalc_body(uint32_t obj)
{
    uint32_t pi = mem_u32(obj + BSLD_OFF_PROPINFO);
    if (!pi) return;
    uint16_t mn = mem_u16(obj + BSLD_OFF_MIN);
    uint16_t mx = mem_u16(obj + BSLD_OFF_MAX);
    uint16_t lvl = mem_u16(obj + BSLD_OFF_LEVEL);
    uint16_t knob = mem_u16(obj + BSLD_OFF_KNOBPIX);
    uint32_t range = (uint32_t)(mx - mn);
    if (range == 0) range = 1;

    /* Body = knob / range * 0xFFFF (proportional knob size). */
    uint32_t body;
    if (knob > 0) {
        body = (knob * 0xFFFFU) / range;
    } else {
        /* Default knob = 1/10 of range. */
        body = 0xFFFFU / 10;
    }
    if (body > 0xFFFFU) body = 0xFFFFU;

    /* Pot = (level - min) / range * 0xFFFF. */
    uint32_t pot;
    if (lvl >= mx) pot = 0xFFFFU;
    else if (lvl <= mn) pot = 0;
    else pot = ((uint32_t)(lvl - mn) * 0xFFFFU) / range;

    uint16_t orient = mem_u16(obj + BSLD_OFF_ORIENT);
    if (orient == SLD_ORIENT_HORIZ) {
        mem_w16(pi + PROP_OFF_HORIZPOT, (uint16_t)pot);
        mem_w16(pi + PROP_OFF_HORIZBODY, (uint16_t)body);
        mem_w16(pi + PROP_OFF_VERTPOT, 0);
        mem_w16(pi + PROP_OFF_VERTBODY, 0xFFFFU);
    } else {
        mem_w16(pi + PROP_OFF_VERTPOT, (uint16_t)pot);
        mem_w16(pi + PROP_OFF_VERTBODY, (uint16_t)body);
        mem_w16(pi + PROP_OFF_HORIZPOT, 0);
        mem_w16(pi + PROP_OFF_HORIZBODY, 0xFFFFU);
    }
}

static int slider_set_tag(uint32_t tag, uint32_t data, void *ctx)
{
    uint32_t obj = (uint32_t)(uintptr_t)ctx;
    switch (tag) {
        case SLIDER_Min:         mem_w16(obj + BSLD_OFF_MIN, (uint16_t)data); break;
        case SLIDER_Max:         mem_w16(obj + BSLD_OFF_MAX, (uint16_t)data); break;
        case SLIDER_Level:
        case SLIDER_Top:         mem_w16(obj + BSLD_OFF_LEVEL, (uint16_t)data); break;
        case SLIDER_Orientation: mem_w16(obj + BSLD_OFF_ORIENT, (uint16_t)data); break;
        case SLIDER_KnobPixels:  mem_w16(obj + BSLD_OFF_KNOBPIX, (uint16_t)data); break;
        case SLIDER_Invisible: {
            uint16_t f = mem_u16(obj + GAD_OFF_FLAGS);
            if (data) f |= 0x0010;  /* PROPBORDERLESS hides border */
            mem_w16(obj + GAD_OFF_FLAGS, f);
            break;
        }
        default: break;
    }
    slider_recalc_body(obj);
    return 1;
}

static uint32_t slidergclass_dispatch(uint32_t cls, uint32_t obj, uint32_t msg)
{
    uint32_t method = mem_u32(msg + MSG_OFF_METHODID);
    switch (method) {
        case OM_NEW: {
            uint32_t tags = mem_u32(msg + OPNEW_OFF_ATTRLIST);
            uint32_t r = gadgetclass_dispatch(cls, obj, msg);
            if (!r) return 0;
            /* Allocate a PropInfo for the slider. */
            uint32_t pi = intu_alloc(32);
            if (pi) {
                for (int i = 0; i < 32; i++) g_ram[pi + i] = 0;
                mem_w32(obj + BSLD_OFF_PROPINFO, pi);
                mem_w32(obj + GAD_OFF_SPECIALINFO, pi);
            }
            /* Defaults: Min=0, Max=100, Level=0, vertical. */
            mem_w16(obj + BSLD_OFF_MIN, 0);
            mem_w16(obj + BSLD_OFF_MAX, 100);
            mem_w16(obj + BSLD_OFF_LEVEL, 0);
            mem_w16(obj + BSLD_OFF_ORIENT, SLD_ORIENT_VERT);
            mem_w16(obj + BSLD_OFF_KNOBPIX, 0);
            mem_w16(obj + GAD_OFF_GADGETTYPE, GTYP_PROPGADGET);
            walk_tags(tags, slider_set_tag, (void*)(uintptr_t)obj);
            return r;
        }
        case OM_DISPOSE: {
            uint32_t pi = mem_u32(obj + BSLD_OFF_PROPINFO);
            if (pi) intu_free(pi);
            return gadgetclass_dispatch(cls, obj, msg);
        }
        case OM_SET: {
            uint32_t tags = mem_u32(msg + OPSET_OFF_ATTRLIST);
            walk_tags(tags, slider_set_tag, (void*)(uintptr_t)obj);
            return gadgetclass_dispatch(cls, obj, msg);
        }
        case OM_GET: {
            uint32_t attr = mem_u32(msg + OPGET_OFF_ATTRID);
            uint32_t store = mem_u32(msg + OPGET_OFF_STORAGE);
            if (!store) return 0;
            switch (attr) {
                case SLIDER_Min:   mem_w32(store, mem_u16(obj + BSLD_OFF_MIN)); return 1;
                case SLIDER_Max:   mem_w32(store, mem_u16(obj + BSLD_OFF_MAX)); return 1;
                case SLIDER_Level:
                case SLIDER_Top:   mem_w32(store, mem_u16(obj + BSLD_OFF_LEVEL)); return 1;
                case SLIDER_Orientation: mem_w32(store, mem_u16(obj + BSLD_OFF_ORIENT)); return 1;
                case SLIDER_KnobPixels:  mem_w32(store, mem_u16(obj + BSLD_OFF_KNOBPIX)); return 1;
                default: return gadgetclass_dispatch(cls, obj, msg);
            }
        }
        default:
            return gadgetclass_dispatch(cls, obj, msg);
    }
}

/* =========================================================================
 * scrollbarclass — scroll bar gadget (subclass of propgclass, V36)
 *
 * A scroll bar is a prop gadget with SCROLLER_* integer attributes for
 * Top, Total, and Visible.  It converts these to PropInfo Pot/Body values
 * and delegates rendering/input to propgclass.
 * ========================================================================= */
#define BSCR_OFF_TOP      0    /* SCROLLER_Top (uint16) */
#define BSCR_OFF_TOTAL    2    /* SCROLLER_Total (uint16) */
#define BSCR_OFF_VISIBLE  4    /* SCROLLER_Visible (uint16) */
#define BSCR_OFF_ORIENT   6    /* 0 = vertical, 1 = horizontal */
#define BSCR_INST_SIZE    8

static void scroller_recalc(uint32_t obj)
{
    uint32_t pi = mem_u32(obj + BPROP_OFF_PROPINFO);
    if (!pi) return;
    uint16_t top     = mem_u16(obj + BSCR_OFF_TOP);
    uint16_t total   = mem_u16(obj + BSCR_OFF_TOTAL);
    uint16_t visible = mem_u16(obj + BSCR_OFF_VISIBLE);
    uint16_t orient  = mem_u16(obj + BSCR_OFF_ORIENT);

    if (total == 0) total = 1;
    if (visible > total) visible = total;

    /* Body = visible / total * 0xFFFF. */
    uint32_t body = ((uint32_t)visible * 0xFFFFU) / total;
    if (body > 0xFFFFU) body = 0xFFFFU;

    /* Pot = top / (total - visible) * 0xFFFF. */
    uint32_t pot;
    uint32_t scroll_range = (uint32_t)(total - visible);
    if (scroll_range == 0) {
        pot = 0;
    } else {
        if (top >= scroll_range) pot = 0xFFFFU;
        else pot = ((uint32_t)top * 0xFFFFU) / scroll_range;
    }

    if (orient == SCR_ORIENT_HORIZ) {
        mem_w16(pi + PROP_OFF_HORIZPOT, (uint16_t)pot);
        mem_w16(pi + PROP_OFF_HORIZBODY, (uint16_t)body);
        mem_w16(pi + PROP_OFF_VERTPOT, 0);
        mem_w16(pi + PROP_OFF_VERTBODY, 0xFFFFU);
        uint16_t flags = mem_u16(pi + PROP_OFF_FLAGS);
        flags |= 0x0001;  /* FREEHORIZ */
        flags &= ~0x0002; /* clear FREEVERT */
        mem_w16(pi + PROP_OFF_FLAGS, flags);
    } else {
        mem_w16(pi + PROP_OFF_VERTPOT, (uint16_t)pot);
        mem_w16(pi + PROP_OFF_VERTBODY, (uint16_t)body);
        mem_w16(pi + PROP_OFF_HORIZPOT, 0);
        mem_w16(pi + PROP_OFF_HORIZBODY, 0xFFFFU);
        uint16_t flags = mem_u16(pi + PROP_OFF_FLAGS);
        flags |= 0x0002;  /* FREEVERT */
        flags &= ~0x0001; /* clear FREEHORIZ */
        mem_w16(pi + PROP_OFF_FLAGS, flags);
    }
}

static int scroller_set_tag(uint32_t tag, uint32_t data, void *ctx)
{
    uint32_t obj = (uint32_t)(uintptr_t)ctx;
    switch (tag) {
        case SCROLLER_Top:         mem_w16(obj + BSCR_OFF_TOP, (uint16_t)data); break;
        case SCROLLER_Total:       mem_w16(obj + BSCR_OFF_TOTAL, (uint16_t)data); break;
        case SCROLLER_Visible:     mem_w16(obj + BSCR_OFF_VISIBLE, (uint16_t)data); break;
        case SCROLLER_Orientation: mem_w16(obj + BSCR_OFF_ORIENT, (uint16_t)data); break;
        /* SCROLLBARA_* — 3.2 Reaction aliases for SCROLLER_* */
        case SCROLLBARA_Top:          mem_w16(obj + BSCR_OFF_TOP, (uint16_t)data); break;
        case SCROLLBARA_Total:        mem_w16(obj + BSCR_OFF_TOTAL, (uint16_t)data); break;
        case SCROLLBARA_Visible:      mem_w16(obj + BSCR_OFF_VISIBLE, (uint16_t)data); break;
        case SCROLLBARA_Orientation:  mem_w16(obj + BSCR_OFF_ORIENT, (uint16_t)data); break;
        case SCROLLBARA_Invisible: {
            uint16_t f = mem_u16(obj + GAD_OFF_FLAGS);
            if (data) f |= 0x0010;  /* PROPBORDERLESS */
            mem_w16(obj + GAD_OFF_FLAGS, f);
            break;
        }
        case SCROLLER_Invisible: {
            uint16_t f = mem_u16(obj + GAD_OFF_FLAGS);
            if (data) f |= 0x0010;  /* PROPBORDERLESS */
            mem_w16(obj + GAD_OFF_FLAGS, f);
            break;
        }
        /* Also accept PGA_* tags for direct Pot/Body control. */
        case PGA_Top: {
            /* Translate PGA_Top to SCROLLER_Top. */
            mem_w16(obj + BSCR_OFF_TOP, (uint16_t)data);
            break;
        }
        default:
            /* Delegate PGA_* tags to propg_set_tag. */
            propg_set_tag(tag, data, ctx);
            break;
    }
    scroller_recalc(obj);
    return 1;
}

static uint32_t scrollbarclass_dispatch(uint32_t cls, uint32_t obj, uint32_t msg)
{
    uint32_t method = mem_u32(msg + MSG_OFF_METHODID);
    switch (method) {
        case OM_NEW: {
            uint32_t tags = mem_u32(msg + OPNEW_OFF_ATTRLIST);
            uint32_t r = propgclass_dispatch(cls, obj, msg);
            if (!r) return 0;
            /* Defaults: Top=0, Total=100, Visible=10, vertical. */
            mem_w16(obj + BSCR_OFF_TOP, 0);
            mem_w16(obj + BSCR_OFF_TOTAL, 100);
            mem_w16(obj + BSCR_OFF_VISIBLE, 10);
            mem_w16(obj + BSCR_OFF_ORIENT, SCR_ORIENT_VERT);
            walk_tags(tags, scroller_set_tag, (void*)(uintptr_t)obj);
            return r;
        }
        case OM_SET: {
            uint32_t tags = mem_u32(msg + OPSET_OFF_ATTRLIST);
            walk_tags(tags, scroller_set_tag, (void*)(uintptr_t)obj);
            return propgclass_dispatch(cls, obj, msg);
        }
        case OM_GET: {
            uint32_t attr = mem_u32(msg + OPGET_OFF_ATTRID);
            uint32_t store = mem_u32(msg + OPGET_OFF_STORAGE);
            if (!store) return 0;
            switch (attr) {
                case SCROLLER_Top:         mem_w32(store, mem_u16(obj + BSCR_OFF_TOP)); return 1;
                case SCROLLER_Total:       mem_w32(store, mem_u16(obj + BSCR_OFF_TOTAL)); return 1;
                case SCROLLER_Visible:     mem_w32(store, mem_u16(obj + BSCR_OFF_VISIBLE)); return 1;
                case SCROLLER_Orientation: mem_w32(store, mem_u16(obj + BSCR_OFF_ORIENT)); return 1;
                case SCROLLBARA_Top:          mem_w32(store, mem_u16(obj + BSCR_OFF_TOP)); return 1;
                case SCROLLBARA_Total:        mem_w32(store, mem_u16(obj + BSCR_OFF_TOTAL)); return 1;
                case SCROLLBARA_Visible:      mem_w32(store, mem_u16(obj + BSCR_OFF_VISIBLE)); return 1;
                case SCROLLBARA_Orientation:  mem_w32(store, mem_u16(obj + BSCR_OFF_ORIENT)); return 1;
                default: return propgclass_dispatch(cls, obj, msg);
            }
        }
        default:
            return propgclass_dispatch(cls, obj, msg);
    }
}

/* =========================================================================
 * pagerclass — pager/tab gadget (subclass of gadgetclass, V40)
 *
 * A pager gadget displays a row (or column) of tab labels and allows the
 * user to switch between pages of content by selecting a tab.  The active
 * page index is stored in the instance data and exposed via PAGERA_*.
 * ========================================================================= */
#define BPGR_OFF_ACTIVE     0    /* PAGERA_Active (uint16) */
#define BPGR_OFF_TOTAL      2    /* PAGERA_Total (uint16) */
#define BPGR_OFF_LABELS     4    /* PAGERA_Labels: guest pointer to label array */
#define BPGR_OFF_ORIENT     8    /* 0 = horizontal, 1 = vertical */
#define BPGR_OFF_STYLE     10    /* tab style */
#define BPGR_OFF_SPACING   12    /* spacing between tabs */
#define BPGR_INST_SIZE     16

static int pager_set_tag(uint32_t tag, uint32_t data, void *ctx)
{
    uint32_t obj = (uint32_t)(uintptr_t)ctx;
    switch (tag) {
        case PAGERA_Active: {
            uint16_t total = mem_u16(obj + BPGR_OFF_TOTAL);
            if (total > 0 && data < total)
                mem_w16(obj + BPGR_OFF_ACTIVE, (uint16_t)data);
            else
                mem_w16(obj + BPGR_OFF_ACTIVE, 0);
            break;
        }
        case PAGERA_Total:
            mem_w16(obj + BPGR_OFF_TOTAL, (uint16_t)data);
            break;
        case PAGERA_Labels:
            mem_w32(obj + BPGR_OFF_LABELS, data);
            break;
        case PAGERA_LabelType:
            /* Label type is stored in gadget flags area — not used in UAOS */
            break;
        case PAGERA_Orientation:
            mem_w16(obj + BPGR_OFF_ORIENT, (uint16_t)data);
            break;
        case PAGERA_Style:
            mem_w16(obj + BPGR_OFF_STYLE, (uint16_t)data);
            break;
        case PAGERA_Spacing:
            mem_w16(obj + BPGR_OFF_SPACING, (uint16_t)data);
            break;
        default: break;
    }
    return 1;
}

static uint32_t pagerclass_dispatch(uint32_t cls, uint32_t obj, uint32_t msg)
{
    uint32_t method = mem_u32(msg + MSG_OFF_METHODID);
    switch (method) {
        case OM_NEW: {
            uint32_t tags = mem_u32(msg + OPNEW_OFF_ATTRLIST);
            uint32_t r = gadgetclass_dispatch(cls, obj, msg);
            if (!r) return 0;
            /* Defaults: Active=0, Total=1, horizontal. */
            mem_w16(obj + BPGR_OFF_ACTIVE, 0);
            mem_w16(obj + BPGR_OFF_TOTAL, 1);
            mem_w16(obj + BPGR_OFF_ORIENT, PAGEORIENT_HORIZ);
            mem_w16(obj + BPGR_OFF_STYLE, 0);
            mem_w16(obj + BPGR_OFF_SPACING, 4);
            walk_tags(tags, pager_set_tag, (void*)(uintptr_t)obj);
            mem_w16(obj + GAD_OFF_GADGETTYPE, GTYP_CUSTOMGADGET);
            return r;
        }
        case OM_SET: {
            uint32_t tags = mem_u32(msg + OPSET_OFF_ATTRLIST);
            walk_tags(tags, pager_set_tag, (void*)(uintptr_t)obj);
            return gadgetclass_dispatch(cls, obj, msg);
        }
        case OM_GET: {
            uint32_t attr = mem_u32(msg + OPGET_OFF_ATTRID);
            uint32_t store = mem_u32(msg + OPGET_OFF_STORAGE);
            if (!store) return 0;
            switch (attr) {
                case PAGERA_Active:      mem_w32(store, mem_u16(obj + BPGR_OFF_ACTIVE)); return 1;
                case PAGERA_Total:       mem_w32(store, mem_u16(obj + BPGR_OFF_TOTAL)); return 1;
                case PAGERA_Labels:      mem_w32(store, mem_u32(obj + BPGR_OFF_LABELS)); return 1;
                case PAGERA_Orientation: mem_w32(store, mem_u16(obj + BPGR_OFF_ORIENT)); return 1;
                case PAGERA_Style:       mem_w32(store, mem_u16(obj + BPGR_OFF_STYLE)); return 1;
                case PAGERA_Spacing:     mem_w32(store, mem_u16(obj + BPGR_OFF_SPACING)); return 1;
                default: return gadgetclass_dispatch(cls, obj, msg);
            }
        }
        case GM_GOACTIVE: {
            /* On click, determine which tab was hit and set active. */
            int16_t mx = mem_s16(msg + GMI_OFF_MOUSEX);
            int16_t my = mem_s16(msg + GMI_OFF_MOUSEY);
            int16_t gw = mem_s16(obj + GAD_OFF_WIDTH);
            int16_t gh = mem_s16(obj + GAD_OFF_HEIGHT);
            uint16_t total = mem_u16(obj + BPGR_OFF_TOTAL);
            uint16_t orient = mem_u16(obj + BPGR_OFF_ORIENT);
            if (total > 0 && mx >= 0 && my >= 0) {
                if (orient == PAGEORIENT_HORIZ && gw > 0) {
                    int tab_w = gw / total;
                    if (tab_w > 0) {
                        int idx = mx / tab_w;
                        if (idx >= 0 && idx < total)
                            mem_w16(obj + BPGR_OFF_ACTIVE, (uint16_t)idx);
                    }
                } else if (orient == PAGEORIENT_VERT && gh > 0) {
                    int tab_h = gh / total;
                    if (tab_h > 0) {
                        int idx = my / tab_h;
                        if (idx >= 0 && idx < total)
                            mem_w16(obj + BPGR_OFF_ACTIVE, (uint16_t)idx);
                    }
                }
            }
            uint16_t f = mem_u16(obj + GAD_OFF_FLAGS);
            f |= GFLG_SELECTED;
            mem_w16(obj + GAD_OFF_FLAGS, f);
            return GMR_MEACTIVE;
        }
        case GM_HANDLEINPUT: {
            int16_t mx = mem_s16(msg + GMI_OFF_MOUSEX);
            int16_t my = mem_s16(msg + GMI_OFF_MOUSEY);
            if (!gadget_point_inside(obj, mx, my)) {
                uint16_t f = mem_u16(obj + GAD_OFF_FLAGS);
                f &= ~GFLG_SELECTED;
                mem_w16(obj + GAD_OFF_FLAGS, f);
                return GMR_NOREUSE;
            }
            return GMR_MEACTIVE;
        }
        case GM_GOINACTIVE: {
            uint16_t f = mem_u16(obj + GAD_OFF_FLAGS);
            f &= ~GFLG_SELECTED;
            mem_w16(obj + GAD_OFF_FLAGS, f);
            return GMR_NOREUSE;
        }
        default:
            return gadgetclass_dispatch(cls, obj, msg);
    }
}

/* =========================================================================
 * listviewgclass — BOOPSI listview gadget (subclass of gadgetclass, V40)
 *
 * A listview gadget displays a scrollable list of text items and allows
 * the user to select one.  It uses LVGA_* attributes for the item list,
 * top/visible/total counts, and the selected index.
 * ========================================================================= */
#define BLV_OFF_TOP        0    /* LVGA_Top (uint16) */
#define BLV_OFF_VISIBLE    2    /* LVGA_Visible (uint16) */
#define BLV_OFF_TOTAL      4    /* LVGA_Total (uint16) */
#define BLV_OFF_SELECTED   6    /* LVGA_Selected (int16, -1 = none) */
#define BLV_OFF_LABELS     8    /* LVGA_Labels: guest pointer to label array */
#define BLV_OFF_MULTI      12   /* LVGA_MultiSelect flag */
#define BLV_OFF_READONLY   13   /* LVGA_ReadOnly flag */
#define BLV_OFF_SPACING    14   /* LVGA_Spacing */
#define BLV_INST_SIZE      16

static int listview_set_tag(uint32_t tag, uint32_t data, void *ctx)
{
    uint32_t obj = (uint32_t)(uintptr_t)ctx;
    switch (tag) {
        case LVGA_Top:
            mem_w16(obj + BLV_OFF_TOP, (uint16_t)data);
            break;
        case LVGA_Visible:
            mem_w16(obj + BLV_OFF_VISIBLE, (uint16_t)data);
            break;
        case LVGA_Total:
            mem_w16(obj + BLV_OFF_TOTAL, (uint16_t)data);
            break;
        case LVGA_Selected:
            mem_w16(obj + BLV_OFF_SELECTED, (uint16_t)data);
            break;
        case LVGA_ItemText:
        case LVGA_ItemLabels:
        case LVGA_Labels:
            mem_w32(obj + BLV_OFF_LABELS, data);
            break;
        case LVGA_MultiSelect:
            g_ram[obj + BLV_OFF_MULTI] = (uint8_t)(data ? 1 : 0);
            break;
        case LVGA_ShowSelected:
            /* ShowSelected is the default behaviour in UAOS */
            break;
        case LVGA_ReadOnly:
            g_ram[obj + BLV_OFF_READONLY] = (uint8_t)(data ? 1 : 0);
            break;
        case LVGA_Spacing:
            mem_w16(obj + BLV_OFF_SPACING, (uint16_t)data);
            break;
        default: break;
    }
    return 1;
}

static uint32_t listviewgclass_dispatch(uint32_t cls, uint32_t obj, uint32_t msg)
{
    uint32_t method = mem_u32(msg + MSG_OFF_METHODID);
    switch (method) {
        case OM_NEW: {
            uint32_t tags = mem_u32(msg + OPNEW_OFF_ATTRLIST);
            uint32_t r = gadgetclass_dispatch(cls, obj, msg);
            if (!r) return 0;
            /* Defaults: Top=0, Visible=5, Total=0, Selected=-1. */
            mem_w16(obj + BLV_OFF_TOP, 0);
            mem_w16(obj + BLV_OFF_VISIBLE, 5);
            mem_w16(obj + BLV_OFF_TOTAL, 0);
            mem_w16(obj + BLV_OFF_SELECTED, 0xFFFF);  /* -1 = no selection */
            mem_w16(obj + BLV_OFF_SPACING, 0);
            walk_tags(tags, listview_set_tag, (void*)(uintptr_t)obj);
            mem_w16(obj + GAD_OFF_GADGETTYPE, GTYP_CUSTOMGADGET);
            return r;
        }
        case OM_SET: {
            uint32_t tags = mem_u32(msg + OPSET_OFF_ATTRLIST);
            walk_tags(tags, listview_set_tag, (void*)(uintptr_t)obj);
            return gadgetclass_dispatch(cls, obj, msg);
        }
        case OM_GET: {
            uint32_t attr = mem_u32(msg + OPGET_OFF_ATTRID);
            uint32_t store = mem_u32(msg + OPGET_OFF_STORAGE);
            if (!store) return 0;
            switch (attr) {
                case LVGA_Top:        mem_w32(store, mem_u16(obj + BLV_OFF_TOP)); return 1;
                case LVGA_Visible:    mem_w32(store, mem_u16(obj + BLV_OFF_VISIBLE)); return 1;
                case LVGA_Total:      mem_w32(store, mem_u16(obj + BLV_OFF_TOTAL)); return 1;
                case LVGA_Selected:   mem_w32(store, (uint32_t)(int16_t)mem_u16(obj + BLV_OFF_SELECTED)); return 1;
                case LVGA_ItemText:
                case LVGA_ItemLabels:
                case LVGA_Labels:     mem_w32(store, mem_u32(obj + BLV_OFF_LABELS)); return 1;
                case LVGA_MultiSelect:mem_w32(store, g_ram[obj + BLV_OFF_MULTI]); return 1;
                case LVGA_ReadOnly:   mem_w32(store, g_ram[obj + BLV_OFF_READONLY]); return 1;
                case LVGA_Spacing:    mem_w32(store, mem_u16(obj + BLV_OFF_SPACING)); return 1;
                default: return gadgetclass_dispatch(cls, obj, msg);
            }
        }
        case GM_GOACTIVE: {
            /* On click, determine which item was hit and select it. */
            int16_t my = mem_s16(msg + GMI_OFF_MOUSEY);
            int16_t gh = mem_s16(obj + GAD_OFF_HEIGHT);
            uint16_t visible = mem_u16(obj + BLV_OFF_VISIBLE);
            uint16_t top = mem_u16(obj + BLV_OFF_TOP);
            uint8_t  readonly = g_ram[obj + BLV_OFF_READONLY];
            if (!readonly && visible > 0 && gh > 0) {
                int item_h = gh / visible;
                if (item_h > 0) {
                    int idx = my / item_h;
                    if (idx >= 0 && idx < visible) {
                        mem_w16(obj + BLV_OFF_SELECTED,
                                (uint16_t)(top + idx));
                    }
                }
            }
            uint16_t f = mem_u16(obj + GAD_OFF_FLAGS);
            f |= GFLG_SELECTED;
            mem_w16(obj + GAD_OFF_FLAGS, f);
            return GMR_MEACTIVE;
        }
        case GM_HANDLEINPUT: {
            int16_t mx = mem_s16(msg + GMI_OFF_MOUSEX);
            int16_t my = mem_s16(msg + GMI_OFF_MOUSEY);
            if (!gadget_point_inside(obj, mx, my)) {
                uint16_t f = mem_u16(obj + GAD_OFF_FLAGS);
                f &= ~GFLG_SELECTED;
                mem_w16(obj + GAD_OFF_FLAGS, f);
                return GMR_NOREUSE;
            }
            return GMR_MEACTIVE;
        }
        case GM_GOINACTIVE: {
            uint16_t f = mem_u16(obj + GAD_OFF_FLAGS);
            f &= ~GFLG_SELECTED;
            mem_w16(obj + GAD_OFF_FLAGS, f);
            return GMR_NOREUSE;
        }
        default:
            return gadgetclass_dispatch(cls, obj, msg);
    }
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
static uint32_t g_rootclass, g_gadgetclass, g_imageclass, g_pointerclass,
                g_menuclass, g_windowclass,
                g_modelclass, g_frbuttonclass, g_bevelbox,
                g_menuitemclass, g_fillrectclass, g_sysgclass,
                g_groupgclass, g_propgclass, g_strgclass,
                g_icclass, g_buttongclass, g_slidergclass, g_scrollbarclass,
                g_pagerclass, g_listviewgclass;

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

    /* Tier 5 — additional built-in classes */
    g_modelclass = make_builtin_class("modelclass", "gadgetclass", g_gadgetclass,
                                       GAD_SIZE, BMODEL_INST_SIZE, modelclass_dispatch);
    UAOS_BOOPSI_RegisterClass(g_modelclass);

    g_frbuttonclass = make_builtin_class("frbuttonclass", "gadgetclass", g_gadgetclass,
                                          GAD_SIZE, BFRBTN_INST_SIZE, frbuttonclass_dispatch);
    UAOS_BOOPSI_RegisterClass(g_frbuttonclass);

    g_bevelbox = make_builtin_class("bevelbox", "imageclass", g_imageclass,
                                     BIMG_INST_SIZE, BBBOX_INST_SIZE, bevelbox_dispatch);
    UAOS_BOOPSI_RegisterClass(g_bevelbox);

    g_menuitemclass = make_builtin_class("menuitemclass", "menuclass", g_menuclass,
                                          BMENU_INST_SIZE, BMITEM_INST_SIZE, menuitemclass_dispatch);
    UAOS_BOOPSI_RegisterClass(g_menuitemclass);

    g_fillrectclass = make_builtin_class("fillrectclass", "rootclass", g_rootclass,
                                          0, BFILL_INST_SIZE, fillrectclass_dispatch);
    UAOS_BOOPSI_RegisterClass(g_fillrectclass);

    g_sysgclass = make_builtin_class("sysgclass", "gadgetclass", g_gadgetclass,
                                      GAD_SIZE, BSYSG_INST_SIZE, sysgclass_dispatch);
    UAOS_BOOPSI_RegisterClass(g_sysgclass);

    /* Tier 6 — group, prop, string gadget classes */
    g_groupgclass = make_builtin_class("groupgclass", "gadgetclass", g_gadgetclass,
                                        GAD_SIZE, BGRP_INST_SIZE, groupgclass_dispatch);
    UAOS_BOOPSI_RegisterClass(g_groupgclass);

    g_propgclass = make_builtin_class("propgclass", "gadgetclass", g_gadgetclass,
                                       GAD_SIZE, BPROP_INST_SIZE, propgclass_dispatch);
    UAOS_BOOPSI_RegisterClass(g_propgclass);

    g_strgclass = make_builtin_class("strgclass", "gadgetclass", g_gadgetclass,
                                      GAD_SIZE, BSTR_INST_SIZE, strgclass_dispatch);
    UAOS_BOOPSI_RegisterClass(g_strgclass);

    /* Tier 6b — icclass, buttongclass, slidergclass, scrollbarclass */
    g_icclass = make_builtin_class("icclass", "rootclass", g_rootclass,
                                    0, BIC_INST_SIZE, icclass_dispatch);
    UAOS_BOOPSI_RegisterClass(g_icclass);

    g_buttongclass = make_builtin_class("buttongclass", "gadgetclass", g_gadgetclass,
                                         GAD_SIZE, BBTN_INST_SIZE, buttongclass_dispatch);
    UAOS_BOOPSI_RegisterClass(g_buttongclass);

    g_slidergclass = make_builtin_class("slidergclass", "gadgetclass", g_gadgetclass,
                                         GAD_SIZE, BSLD_INST_SIZE, slidergclass_dispatch);
    UAOS_BOOPSI_RegisterClass(g_slidergclass);

    g_scrollbarclass = make_builtin_class("scrollbarclass", "propgclass", g_propgclass,
                                           GAD_SIZE + BPROP_INST_SIZE, BSCR_INST_SIZE,
                                           scrollbarclass_dispatch);
    UAOS_BOOPSI_RegisterClass(g_scrollbarclass);

    /* Tier 5b — pagerclass, listviewgclass (V40 Reaction gadgets) */
    g_pagerclass = make_builtin_class("pagerclass", "gadgetclass", g_gadgetclass,
                                       GAD_SIZE, BPGR_INST_SIZE, pagerclass_dispatch);
    UAOS_BOOPSI_RegisterClass(g_pagerclass);

    g_listviewgclass = make_builtin_class("listviewgclass", "gadgetclass", g_gadgetclass,
                                           GAD_SIZE, BLV_INST_SIZE, listviewgclass_dispatch);
    UAOS_BOOPSI_RegisterClass(g_listviewgclass);
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
