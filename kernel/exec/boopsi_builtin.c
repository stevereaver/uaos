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
#include <string.h>

/* Guest RAM access (mirrors intuition_lib.c helpers for fast local use) */
extern uint8_t *g_ram;
extern unsigned int m68k_get_reg(void *context, int reg);
extern void         m68k_set_reg(int reg, unsigned int value);
extern uint32_t     UAOS_InvokeM68kHook(uint32_t hook_ptr, uint32_t a0, uint32_t a1, uint32_t a2);
extern uint32_t     intu_alloc(uint32_t size);
extern void         intu_free(uint32_t user_addr);

#define GUEST_RAM_SIZE (2 * 1024 * 1024)

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
 * ========================================================================= */
#define BGAD_OFF_LEFT        0
#define BGAD_OFF_TOP         4
#define BGAD_OFF_WIDTH       8
#define BGAD_OFF_HEIGHT      12
#define BGAD_OFF_FLAGS       16
#define BGAD_OFF_ACTIVATION  20
#define BGAD_OFF_TEXT        24
#define BGAD_OFF_ID          28
#define BGAD_OFF_USERDATA    32
#define BGAD_OFF_SPECIALINFO 36
#define BGAD_OFF_TYPE        40
#define BGAD_OFF_MUTUAL      44
#define BGAD_OFF_IMAGE       48
#define BGAD_OFF_LABEL       52
#define BGAD_INST_SIZE       56

static int gadget_set_tag(uint32_t tag, uint32_t data, void *ctx)
{
    uint32_t obj = (uint32_t)(uintptr_t)ctx;
    switch (tag) {
        case GA_Left:       mem_w32(obj + BGAD_OFF_LEFT, (uint32_t)(int32_t)(int16_t)data); break;
        case GA_RelRight:   break;
        case GA_Top:        mem_w32(obj + BGAD_OFF_TOP, (uint32_t)(int32_t)(int16_t)data); break;
        case GA_RelBottom:  break;
        case GA_Width:      mem_w32(obj + BGAD_OFF_WIDTH, (uint32_t)(int32_t)(int16_t)data); break;
        case GA_RelWidth:   break;
        case GA_Height:     mem_w32(obj + BGAD_OFF_HEIGHT, (uint32_t)(int32_t)(int16_t)data); break;
        case GA_RelHeight:  break;
        case GA_Text:       mem_w32(obj + BGAD_OFF_TEXT, data); break;
        case GA_Label:      mem_w32(obj + BGAD_OFF_LABEL, data); break;
        case GA_Image:      mem_w32(obj + BGAD_OFF_IMAGE, data); break;
        case GA_ID:         mem_w32(obj + BGAD_OFF_ID, data); break;
        case GA_UserData:   mem_w32(obj + BGAD_OFF_USERDATA, data); break;
        case GA_Disabled:   mem_w32(obj + BGAD_OFF_FLAGS, data ? 0x00000200 : 0); break;
        case GA_Selected:   {
            uint32_t f = mem_u32(obj + BGAD_OFF_FLAGS);
            if (data) f |= 0x00000001;
            else f &= ~0x00000001;
            mem_w32(obj + BGAD_OFF_FLAGS, f);
            break;
        }
        case GA_Immediate:  {
            uint32_t a = mem_u32(obj + BGAD_OFF_ACTIVATION);
            if (data) a |= 0x0002; else a &= ~0x0002;
            mem_w32(obj + BGAD_OFF_ACTIVATION, a);
            break;
        }
        case GA_RelVerify:  {
            uint32_t a = mem_u32(obj + BGAD_OFF_ACTIVATION);
            if (data) a |= 0x0001; else a &= ~0x0001;
            mem_w32(obj + BGAD_OFF_ACTIVATION, a);
            break;
        }
        case GA_ToggleSelect: {
            uint32_t a = mem_u32(obj + BGAD_OFF_ACTIVATION);
            if (data) a |= 0x0004; else a &= ~0x0004;
            mem_w32(obj + BGAD_OFF_ACTIVATION, a);
            break;
        }
    }
    return 1;
}

static uint32_t gadgetclass_dispatch(uint32_t cls, uint32_t obj, uint32_t msg)
{
    uint32_t method = mem_u32(msg + MSG_OFF_METHODID);
    switch (method) {
        case OM_NEW: {
            uint32_t tags = mem_u32(msg + OPNEW_OFF_ATTRLIST);
            memset(&g_ram[obj], 0, BGAD_INST_SIZE);
            walk_tags(tags, gadget_set_tag, (void*)(uintptr_t)obj);
            return 1;
        }
        case OM_DISPOSE:
            return 1;
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
                case GA_Left:      value = mem_u32(obj + BGAD_OFF_LEFT); break;
                case GA_Top:       value = mem_u32(obj + BGAD_OFF_TOP); break;
                case GA_Width:     value = mem_u32(obj + BGAD_OFF_WIDTH); break;
                case GA_Height:    value = mem_u32(obj + BGAD_OFF_HEIGHT); break;
                case GA_Text:      value = mem_u32(obj + BGAD_OFF_TEXT); break;
                case GA_Label:     value = mem_u32(obj + BGAD_OFF_LABEL); break;
                case GA_Image:     value = mem_u32(obj + BGAD_OFF_IMAGE); break;
                case GA_ID:        value = mem_u32(obj + BGAD_OFF_ID); break;
                case GA_UserData:  value = mem_u32(obj + BGAD_OFF_USERDATA); break;
                case GA_Disabled:  value = (mem_u32(obj + BGAD_OFF_FLAGS) & 0x00000200) ? 1 : 0; break;
                case GA_Selected:  value = (mem_u32(obj + BGAD_OFF_FLAGS) & 0x00000001) ? 1 : 0; break;
                default: return 0;
            }
            mem_w32(store, value);
            return 1;
        }
        case GM_HITTEST:
            return 1; /* inside */
        case GM_RENDER:
        case GM_GOACTIVE:
        case GM_HANDLEINPUT:
        case GM_GOINACTIVE:
            return 1;
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
 * menuclass / windowclass — rootclass subclasses with no extra data
 * ========================================================================= */
static uint32_t menuclass_dispatch(uint32_t cls, uint32_t obj, uint32_t msg)
{
    return rootclass_dispatch(cls, obj, msg);
}
static uint32_t windowclass_dispatch(uint32_t cls, uint32_t obj, uint32_t msg)
{
    return rootclass_dispatch(cls, obj, msg);
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

    g_gadgetclass = make_builtin_class("gadgetclass", "rootclass", g_rootclass, 0, BGAD_INST_SIZE, gadgetclass_dispatch);
    UAOS_BOOPSI_RegisterClass(g_gadgetclass);

    g_imageclass = make_builtin_class("imageclass", "rootclass", g_rootclass, 0, BIMG_INST_SIZE, imageclass_dispatch);
    UAOS_BOOPSI_RegisterClass(g_imageclass);

    g_pointerclass = make_builtin_class("pointerclass", "imageclass", g_imageclass, 0, BPTR_INST_SIZE, pointerclass_dispatch);
    UAOS_BOOPSI_RegisterClass(g_pointerclass);

    g_menuclass = make_builtin_class("menuclass", "rootclass", g_rootclass, 0, 0, menuclass_dispatch);
    UAOS_BOOPSI_RegisterClass(g_menuclass);

    g_windowclass = make_builtin_class("windowclass", "rootclass", g_rootclass, 0, 0, windowclass_dispatch);
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
