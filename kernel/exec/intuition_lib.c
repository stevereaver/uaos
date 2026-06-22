/*
 * intuition_lib.c — UAOS intuition.library Implementation
 *
 * Basic window management stub backed by the native WM.
 * Guest programs open/close/move windows via standard AmigaOS calls;
 * each call is translated into the existing UAOS window manager API.
 */

#include "rom_modules.h"
#include "intuition_lib.h"
#include "amiga_graphics.h"
#include "../display/wm.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

/* =========================================================================
 * Guest RAM accessor (provided by uaos_m68k_glue.c)
 * ========================================================================= */

extern uint8_t *g_ram;
#define GUEST_RAM_SIZE  (2 * 1024 * 1024)

/* =========================================================================
 * Musashi register access
 * ========================================================================= */

extern unsigned int m68k_get_reg(void *context, int reg);
extern void         m68k_set_reg(int reg, unsigned int value);

#define M68K_REG_D0  0
#define M68K_REG_D1  1
#define M68K_REG_D2  2
#define M68K_REG_D3  3
#define M68K_REG_D4  4
#define M68K_REG_D5  5
#define M68K_REG_D6  6
#define M68K_REG_D7  7
#define M68K_REG_A0  8
#define M68K_REG_A1  9
#define M68K_REG_A2  10

/* =========================================================================
 * Guest memory helpers
 * ========================================================================= */

static inline uint8_t  mem_u8 (uint32_t addr) { return g_ram[addr]; }
static inline uint16_t mem_u16(uint32_t addr)
    { return (uint16_t)((g_ram[addr] << 8) | g_ram[addr + 1]); }
static inline int16_t  mem_s16(uint32_t addr)
    { return (int16_t)mem_u16(addr); }
static inline uint32_t mem_u32(uint32_t addr)
{
    return ((uint32_t)g_ram[addr]     << 24) |
           ((uint32_t)g_ram[addr + 1] << 16) |
           ((uint32_t)g_ram[addr + 2] <<  8) |
           ((uint32_t)g_ram[addr + 3]      );
}
static inline void mem_w8 (uint32_t addr, uint8_t  v) { g_ram[addr] = v; }
static inline void mem_w16(uint32_t addr, uint16_t v)
{
    g_ram[addr]     = (uint8_t)(v >> 8);
    g_ram[addr + 1] = (uint8_t)v;
}
static inline void mem_w32(uint32_t addr, uint32_t v)
{
    g_ram[addr]     = (uint8_t)(v >> 24);
    g_ram[addr + 1] = (uint8_t)(v >> 16);
    g_ram[addr + 2] = (uint8_t)(v >>  8);
    g_ram[addr + 3] = (uint8_t)v;
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

/* =========================================================================
 * Intuition heap allocator (dedicated region below stack)
 * ========================================================================= */

#define INTUITION_HEAP_BASE  0x1E0000
#define INTUITION_HEAP_SIZE  0x010000
static uint32_t intu_heap_ptr = INTUITION_HEAP_BASE;

static uint32_t intu_alloc(uint32_t size)
{
    size = (size + 3) & ~3u;
    if (intu_heap_ptr + size > INTUITION_HEAP_BASE + INTUITION_HEAP_SIZE)
        return 0;
    uint32_t addr = intu_heap_ptr;
    intu_heap_ptr += size;
    for (uint32_t i = 0; i < size; i++) g_ram[addr + i] = 0;
    return addr;
}

static void init_guest_rastport(uint32_t rp)
{
    for (int i = 0; i < RP_SIZE_MIN; i++) g_ram[rp + i] = 0;
    g_ram[rp + RP_OFF_FGPEN]    = 1;   /* white */
    g_ram[rp + RP_OFF_BGPEN]    = 0;   /* black */
    g_ram[rp + RP_OFF_DRAWMODE] = JAM2;
}

/* =========================================================================
 * Window tracking — map guest Window pointer to WM handle
 * ========================================================================= */

#define MAX_INTUITION_WINS  8

typedef struct {
    uint32_t guest_win;
    int      wm_handle;
    uint8_t  active;
    int16_t  min_w, min_h;
    int16_t  max_w, max_h;
} IntuitionSlot;

static IntuitionSlot g_intu_wins[MAX_INTUITION_WINS];

static IntuitionSlot *alloc_slot(void)
{
    for (int i = 0; i < MAX_INTUITION_WINS; i++) {
        if (!g_intu_wins[i].active) {
            g_intu_wins[i].active = 1;
            return &g_intu_wins[i];
        }
    }
    return NULL;
}

static IntuitionSlot *find_slot_by_guest(uint32_t guest_win)
{
    for (int i = 0; i < MAX_INTUITION_WINS; i++) {
        if (g_intu_wins[i].active && g_intu_wins[i].guest_win == guest_win)
            return &g_intu_wins[i];
    }
    return NULL;
}

static void free_slot(IntuitionSlot *slot)
{
    if (slot) {
        slot->active    = 0;
        slot->guest_win = 0;
        slot->wm_handle = -1;
        slot->min_w     = 0;
        slot->min_h     = 0;
        slot->max_w     = 0;
        slot->max_h     = 0;
    }
}

/* Empty draw callback — m68k app should handle rendering via IDCMP */
static void intu_draw_fn(int win_x, int win_y, int win_w, int win_h)
{
    (void)win_x; (void)win_y; (void)win_w; (void)win_h;
}

/* =========================================================================
 * intuition.library function indices (must match LVO offsets in glue)
 * ========================================================================= */

#define INTUITION_OPEN_LIBRARY       1
#define INTUITION_CLOSE_LIBRARY      2
#define INTUITION_OPEN_WINDOW        3
#define INTUITION_CLOSE_WINDOW       4
#define INTUITION_WINDOW_TO_FRONT    5
#define INTUITION_WINDOW_TO_BACK     6
#define INTUITION_ACTIVATE_WINDOW    7
#define INTUITION_MOVE_WINDOW        8
#define INTUITION_SIZE_WINDOW        9
#define INTUITION_REFRESH_WINDOW     10
#define INTUITION_MODIFY_IDCMP       11
#define INTUITION_SET_WINDOW_TITLES  12
#define INTUITION_OPEN_WINDOW_TAGS   13
#define INTUITION_OPEN_WORKBENCH     14
#define INTUITION_CLOSE_WORKBENCH    15
#define INTUITION_DRAW_BORDER        16
#define INTUITION_DRAW_IMAGE         17
#define INTUITION_PRINT_I_TEXT       18

/* =========================================================================
 * Implementation
 * ========================================================================= */

static void intuition_OpenLibrary(void)
{
    /* no-op */
}

static void intuition_CloseLibrary(void)
{
    /* no-op */
}

/* Parse OpenWindowTagList() tag list and update window parameters.
 * Only the basic WA_* tags requested are handled. */
static void parse_window_tags(uint32_t tag_list,
    int16_t *left, int16_t *top, int16_t *width, int16_t *height,
    uint16_t *flags, uint16_t *idcmp, uint32_t *title_ptr,
    int16_t *min_w, int16_t *min_h, int16_t *max_w, int16_t *max_h)
{
    while (tag_list && tag_list + 8 <= GUEST_RAM_SIZE) {
        uint32_t tag  = mem_u32(tag_list);
        uint32_t data = mem_u32(tag_list + 4);
        if (tag == TAG_DONE) break;
        switch (tag) {
            case WA_Left:      *left      = (int16_t)(int32_t)data; break;
            case WA_Top:       *top       = (int16_t)(int32_t)data; break;
            case WA_Width:     *width     = (int16_t)(int32_t)data; break;
            case WA_Height:    *height    = (int16_t)(int32_t)data; break;
            case WA_Flags:     *flags     = (uint16_t)data; break;
            case WA_IDCMP:     *idcmp     = (uint16_t)data; break;
            case WA_Title:     *title_ptr = data; break;
            case WA_MinWidth:  *min_w     = (int16_t)(int32_t)data; break;
            case WA_MinHeight: *min_h     = (int16_t)(int32_t)data; break;
            case WA_MaxWidth:  *max_w     = (int16_t)(int32_t)data; break;
            case WA_MaxHeight: *max_h     = (int16_t)(int32_t)data; break;
        }
        tag_list += sizeof(AmigaTagItem);
    }
}

/* OpenWindowTagList(newWindow, tagList)
 * A0 = newWindow, A1 = tagList
 * Returns Window* in D0 */
static void intuition_OpenWindowTagList(void)
{
    uint32_t nw_ptr    = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t tag_list  = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t win_ptr   = 0;

    int16_t  left = 0, top = 0, width = 320, height = 200;
    uint16_t flags = 0, idcmp = 0;
    uint32_t title_ptr = 0;
    int16_t  min_w = 0, min_h = 0, max_w = 0, max_h = 0;
    uint32_t first_gadget = 0, check_mark = 0;

    if (nw_ptr) {
        left        = mem_s16(nw_ptr + 0);
        top         = mem_s16(nw_ptr + 2);
        width       = mem_s16(nw_ptr + 4);
        height      = mem_s16(nw_ptr + 6);
        idcmp       = mem_u16(nw_ptr + 10);
        flags       = mem_u16(nw_ptr + 12);
        first_gadget = mem_u32(nw_ptr + 14);
        check_mark   = mem_u32(nw_ptr + 18);
        title_ptr    = mem_u32(nw_ptr + 22);
        min_w        = mem_s16(nw_ptr + 34);
        min_h        = mem_s16(nw_ptr + 36);
        max_w        = mem_s16(nw_ptr + 38);
        max_h        = mem_s16(nw_ptr + 40);
    }

    if (tag_list) {
        parse_window_tags(tag_list, &left, &top, &width, &height,
                          &flags, &idcmp, &title_ptr,
                          &min_w, &min_h, &max_w, &max_h);
    }

    char title[32] = "Window";
    if (title_ptr) guest_str(title, title_ptr, sizeof(title));

    if (min_w > 0 && width < min_w) width = min_w;
    if (min_h > 0 && height < min_h) height = min_h;
    if (max_w > 0 && width > max_w) width = max_w;
    if (max_h > 0 && height > max_h) height = max_h;

    if (width < 64) width = 64;
    if (height < 32) height = 32;

    IntuitionSlot *slot = alloc_slot();
    if (!slot) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }

    int wh = WM_AddWindow(left, top, width, height, title, intu_draw_fn, NULL);
    if (wh < 0) {
        free_slot(slot);
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }

    win_ptr = intu_alloc(sizeof(AmigaWindow));
    if (!win_ptr) {
        WM_CloseWindow(wh);
        free_slot(slot);
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }

    uint32_t rp_ptr = intu_alloc(RP_SIZE_MIN);
    if (rp_ptr) init_guest_rastport(rp_ptr);

    mem_w32(win_ptr + 0,  0);                 /* NextWindow    */
    mem_w16(win_ptr + 4,  left);              /* LeftEdge      */
    mem_w16(win_ptr + 6,  top);               /* TopEdge       */
    mem_w16(win_ptr + 8,  width);             /* Width         */
    mem_w16(win_ptr + 10, height);            /* Height        */
    mem_w8 (win_ptr + 12, 1);                /* DetailPen     */
    mem_w8 (win_ptr + 13, 0);                /* BlockPen      */
    mem_w16(win_ptr + 14, idcmp);             /* IDCMPFlags    */
    mem_w16(win_ptr + 16, flags);             /* Flags         */
    mem_w32(win_ptr + 18, first_gadget);        /* FirstGadget   */
    mem_w32(win_ptr + 22, check_mark);          /* CheckMark     */
    mem_w32(win_ptr + 26, title_ptr);         /* Title         */
    mem_w32(win_ptr + 30, 0);                 /* FirstRequest  */
    mem_w16(win_ptr + 34, 0);                 /* ReqCount      */
    mem_w32(win_ptr + 38, 0);                 /* WScreen       */
    /* Pad1 at 42-49 */
    mem_w32(win_ptr + 50, rp_ptr);            /* RPort         */

    slot->guest_win = win_ptr;
    slot->wm_handle = wh;
    slot->min_w     = min_w;
    slot->min_h     = min_h;
    slot->max_w     = max_w;
    slot->max_h     = max_h;

    if (flags & WFLG_ACTIVATE)
        WM_RequestFocus(wh);

    m68k_set_reg(M68K_REG_D0, win_ptr);
}

/* OpenWindow(newWindow) — A0 = newWindow, returns Window* in D0 */
static void intuition_OpenWindow(void)
{
    uint32_t saved_a1 = m68k_get_reg(NULL, M68K_REG_A1);
    m68k_set_reg(M68K_REG_A1, 0);
    intuition_OpenWindowTagList();
    m68k_set_reg(M68K_REG_A1, saved_a1);
}

/* CloseWindow(window) — A0 = window */
static void intuition_CloseWindow(void)
{
    uint32_t win_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    if (!win_ptr) return;

    IntuitionSlot *slot = find_slot_by_guest(win_ptr);
    if (slot) {
        WM_CloseWindow(slot->wm_handle);
        free_slot(slot);
    }
}

/* WindowToFront(window) — A0 = window */
static void intuition_WindowToFront(void)
{
    uint32_t win_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    IntuitionSlot *slot = find_slot_by_guest(win_ptr);
    if (slot) WM_RaiseWindow(slot->wm_handle);
}

/* WindowToBack(window) — A0 = window */
static void intuition_WindowToBack(void)
{
    uint32_t win_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    IntuitionSlot *slot = find_slot_by_guest(win_ptr);
    if (slot) WM_LowerWindow(slot->wm_handle);
}

/* ActivateWindow(window) — A0 = window */
static void intuition_ActivateWindow(void)
{
    uint32_t win_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    IntuitionSlot *slot = find_slot_by_guest(win_ptr);
    if (slot) WM_RequestFocus(slot->wm_handle);
}

/* MoveWindow(window, dx, dy) — A0 = window, D0 = dx, D1 = dy */
static void intuition_MoveWindow(void)
{
    uint32_t win_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    int dx = (int)m68k_get_reg(NULL, M68K_REG_D0);
    int dy = (int)m68k_get_reg(NULL, M68K_REG_D1);

    IntuitionSlot *slot = find_slot_by_guest(win_ptr);
    if (!slot) return;

    int left = (int)mem_s16(win_ptr + 4);
    int top  = (int)mem_s16(win_ptr + 6);
    int new_x = left + dx;
    int new_y = top  + dy;

    WM_MoveWindow(slot->wm_handle, new_x, new_y);

    mem_w16(win_ptr + 4, (int16_t)new_x);
    mem_w16(win_ptr + 6, (int16_t)new_y);
}

/* SizeWindow(window, dx, dy) — A0 = window, D0 = dx, D1 = dy */
static void intuition_SizeWindow(void)
{
    uint32_t win_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    int dw = (int)m68k_get_reg(NULL, M68K_REG_D0);
    int dh = (int)m68k_get_reg(NULL, M68K_REG_D1);

    IntuitionSlot *slot = find_slot_by_guest(win_ptr);
    if (!slot) return;

    int w = (int)mem_s16(win_ptr + 8)  + dw;
    int h = (int)mem_s16(win_ptr + 10) + dh;

    if (slot->min_w > 0 && w < slot->min_w) w = slot->min_w;
    if (slot->min_h > 0 && h < slot->min_h) h = slot->min_h;
    if (slot->max_w > 0 && w > slot->max_w) w = slot->max_w;
    if (slot->max_h > 0 && h > slot->max_h) h = slot->max_h;

    if (w < 32) w = 32;
    if (h < 32) h = 32;

    /* WM has no direct resize, so close and reopen at new size */
    WM_CloseWindow(slot->wm_handle);

    char title[32] = "Window";
    uint32_t title_ptr = mem_u32(win_ptr + 26);
    if (title_ptr) guest_str(title, title_ptr, sizeof(title));

    int new_wh = WM_AddWindow(
        (int)mem_s16(win_ptr + 4),
        (int)mem_s16(win_ptr + 6),
        w, h, title, intu_draw_fn, NULL);

    if (new_wh >= 0) {
        slot->wm_handle = new_wh;
        mem_w16(win_ptr + 8,  (int16_t)w);
        mem_w16(win_ptr + 10, (int16_t)h);
    } else {
        free_slot(slot);
    }
}

/* RefreshWindowFrame(window) — A0 = window */
static void intuition_RefreshWindowFrame(void)
{
    uint32_t win_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    IntuitionSlot *slot = find_slot_by_guest(win_ptr);
    if (slot) WM_RepaintWindow(slot->wm_handle);
}

/* ModifyIDCMP(window, flags) — A0 = window, D0 = flags */
static void intuition_ModifyIDCMP(void)
{
    uint32_t win_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    uint16_t flags   = (uint16_t)m68k_get_reg(NULL, M68K_REG_D0);
    if (win_ptr) mem_w16(win_ptr + 14, flags);
}

/* SetWindowTitles(window, title, screenTitle)
 * A0 = window, A1 = title, A2 = screenTitle */
static void intuition_SetWindowTitles(void)
{
    uint32_t win_ptr   = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t title_ptr = m68k_get_reg(NULL, M68K_REG_A1);

    if (!win_ptr) return;

    if (title_ptr) {
        char title[32];
        guest_str(title, title_ptr, sizeof(title));
        mem_w32(win_ptr + 26, title_ptr);

        IntuitionSlot *slot = find_slot_by_guest(win_ptr);
        if (slot) WM_SetWindowTitle(slot->wm_handle, title);
    }
}

/* =========================================================================
 * Workbench screen stubs
 * ========================================================================= */

static uint8_t g_workbench_open = 0;

/* OpenWorkbench() — returns Workbench screen pointer in D0 */
static void intuition_OpenWorkbench(void)
{
    g_workbench_open = 1;
    /* Return a synthetic non-zero screen pointer; UAOS has no guest Screen. */
    m68k_set_reg(M68K_REG_D0, 0x00001000);
}

/* CloseWorkBench() — returns BOOL in D0 */
static void intuition_CloseWorkbench(void)
{
    if (g_workbench_open) {
        g_workbench_open = 0;
        m68k_set_reg(M68K_REG_D0, 1);
    } else {
        m68k_set_reg(M68K_REG_D0, 0);
    }
}

/* =========================================================================
 * Thin drawing wrappers — delegate to graphics.library primitives
 * ========================================================================= */

extern void UAOS_Graphics_Dispatch(uint32_t fn);

/* Graphics LVO slots used by the wrappers (|LVO| / 6). */
#define GFX_SLOT_TEXT                10
#define GFX_SLOT_SETFONT             11
#define GFX_SLOT_OPENFONT            12
#define GFX_SLOT_BLTTEMPLATE          6
#define GFX_SLOT_MOVE                40
#define GFX_SLOT_DRAW                41
#define GFX_SLOT_RECTFILL            51
#define GFX_SLOT_SETAPEN             57
#define GFX_SLOT_SETBPEN             58
#define GFX_SLOT_SETDRMD             59
#define GFX_SLOT_BLTBITMAPRASTPORT  101

/* Image structure offsets */
#define IMG_OFF_LEFTEDGE    0
#define IMG_OFF_TOPEDGE     2
#define IMG_OFF_WIDTH       4
#define IMG_OFF_HEIGHT      6
#define IMG_OFF_DEPTH       8
#define IMG_OFF_IMAGEDATA  10
#define IMG_OFF_PLANEPICK  14
#define IMG_OFF_PLANEONOFF 15
#define IMG_OFF_NEXTIMAGE  16
#define IMG_SIZE           20

/* Border structure offsets */
#define BORDER_OFF_LEFTEDGE    0
#define BORDER_OFF_TOPEDGE     2
#define BORDER_OFF_FRONTPEN    4
#define BORDER_OFF_BACKPEN     5
#define BORDER_OFF_DRAWMODE     6
#define BORDER_OFF_COUNT       7
#define BORDER_OFF_XY           8
#define BORDER_OFF_NEXTBORDER  12
#define BORDER_SIZE            16

/* IntuiText structure offsets */
#define ITEXT_OFF_FRONTPEN   0
#define ITEXT_OFF_BACKPEN    1
#define ITEXT_OFF_DRAWMODE   2
#define ITEXT_OFF_LEFTEDGE   3
#define ITEXT_OFF_TOPEDGE    5
#define ITEXT_OFF_ITEXTFONT  7
#define ITEXT_OFF_ITEXT      11
#define ITEXT_OFF_NEXTTEXT   15
#define ITEXT_SIZE           19

/* DrawBorder(rp, border, xOffset, yOffset)
 * A0 = rp, A1 = border, D0 = xOffset, D1 = yOffset */
static void intuition_DrawBorder(void)
{
    uint32_t rp     = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t border = m68k_get_reg(NULL, M68K_REG_A1);
    int xoff        = (int)m68k_get_reg(NULL, M68K_REG_D0);
    int yoff        = (int)m68k_get_reg(NULL, M68K_REG_D1);

    while (border && border + BORDER_SIZE <= GUEST_RAM_SIZE) {
        int16_t left      = mem_s16(border + BORDER_OFF_LEFTEDGE);
        int16_t top       = mem_s16(border + BORDER_OFF_TOPEDGE);
        uint8_t front_pen = mem_u8(border + BORDER_OFF_FRONTPEN);
        uint8_t draw_mode = mem_u8(border + BORDER_OFF_DRAWMODE);
        int     count     = (int)(int8_t)mem_u8(border + BORDER_OFF_COUNT);
        uint32_t xy       = mem_u32(border + BORDER_OFF_XY);

        int base_x = xoff + left;
        int base_y = yoff + top;

        /* SetAPen(rp, front_pen) */
        m68k_set_reg(M68K_REG_A1, rp);
        m68k_set_reg(M68K_REG_D0, front_pen);
        UAOS_Graphics_Dispatch(GFX_SLOT_SETAPEN);

        /* SetDrMd(rp, draw_mode) */
        m68k_set_reg(M68K_REG_A1, rp);
        m68k_set_reg(M68K_REG_D0, draw_mode);
        UAOS_Graphics_Dispatch(GFX_SLOT_SETDRMD);

        for (int i = 0; xy && i < count - 1 && xy + (i + 2) * 4 <= GUEST_RAM_SIZE; i++) {
            int x0 = base_x + mem_s16(xy + i * 4);
            int y0 = base_y + mem_s16(xy + i * 4 + 2);
            int x1 = base_x + mem_s16(xy + (i + 1) * 4);
            int y1 = base_y + mem_s16(xy + (i + 1) * 4 + 2);

            /* Move(rp, x0, y0) */
            m68k_set_reg(M68K_REG_A1, rp);
            m68k_set_reg(M68K_REG_D0, x0);
            m68k_set_reg(M68K_REG_D1, y0);
            UAOS_Graphics_Dispatch(GFX_SLOT_MOVE);

            /* Draw(rp, x1, y1) */
            m68k_set_reg(M68K_REG_A1, rp);
            m68k_set_reg(M68K_REG_D0, x1);
            m68k_set_reg(M68K_REG_D1, y1);
            UAOS_Graphics_Dispatch(GFX_SLOT_DRAW);
        }

        border = mem_u32(border + BORDER_OFF_NEXTBORDER);
    }
}

/* DrawImage(rp, image, xOffset, yOffset)
 * A0 = rp, A1 = image, D0 = xOffset, D1 = yOffset */
static void intuition_DrawImage(void)
{
    uint32_t rp    = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t image = m68k_get_reg(NULL, M68K_REG_A1);
    int xoff       = (int)m68k_get_reg(NULL, M68K_REG_D0);
    int yoff       = (int)m68k_get_reg(NULL, M68K_REG_D1);

    while (image && image + IMG_SIZE <= GUEST_RAM_SIZE) {
        int16_t  width      = mem_s16(image + IMG_OFF_WIDTH);
        int16_t  height     = mem_s16(image + IMG_OFF_HEIGHT);
        uint16_t depth      = mem_u16(image + IMG_OFF_DEPTH);
        int16_t  left       = mem_s16(image + IMG_OFF_LEFTEDGE);
        int16_t  top        = mem_s16(image + IMG_OFF_TOPEDGE);
        uint32_t image_data = mem_u32(image + IMG_OFF_IMAGEDATA);
        uint32_t next_image = mem_u32(image + IMG_OFF_NEXTIMAGE);

        int dst_x = xoff + left;
        int dst_y = yoff + top;

        if (image_data && width > 0 && height > 0) {
            int bpr = ((width + 15) / 16) * 2;

            if (depth == 1) {
                /* BltTemplate(source, xSrc, srcMod, destRP, xDest, yDest, xSize, ySize) */
                m68k_set_reg(M68K_REG_A0, image_data);
                m68k_set_reg(M68K_REG_D0, 0);
                m68k_set_reg(M68K_REG_D1, bpr);
                m68k_set_reg(M68K_REG_A1, rp);
                m68k_set_reg(M68K_REG_D2, dst_x);
                m68k_set_reg(M68K_REG_D3, dst_y);
                m68k_set_reg(M68K_REG_D4, width);
                m68k_set_reg(M68K_REG_D5, height);
                UAOS_Graphics_Dispatch(GFX_SLOT_BLTTEMPLATE);
            } else {
                /* Build a temporary guest BitMap and blit it to the RastPort. */
                uint32_t bm = intu_alloc(40);
                if (bm) {
                    mem_w16(bm + BM_OFF_BYTESPERROW, (uint16_t)bpr);
                    mem_w16(bm + BM_OFF_ROWS, (uint16_t)height);
                    mem_w8(bm + BM_OFF_FLAGS, 0);
                    mem_w8(bm + BM_OFF_DEPTH, (uint8_t)(depth > 8 ? 8 : depth));
                    for (int i = 0; i < 8; i++)
                        mem_w32(bm + BM_OFF_PLANES + i * 4, 0);
                    for (int i = 0; i < (int)depth; i++)
                        mem_w32(bm + BM_OFF_PLANES + i * 4,
                                image_data + (uint32_t)i * (uint32_t)height * (uint32_t)bpr);

                    /* BltBitMapRastPort(src, xSrc, ySrc, dstRP, xDst, yDst, xSize, ySize, minterm) */
                    m68k_set_reg(M68K_REG_A0, bm);
                    m68k_set_reg(M68K_REG_D0, 0);
                    m68k_set_reg(M68K_REG_D1, 0);
                    m68k_set_reg(M68K_REG_A1, rp);
                    m68k_set_reg(M68K_REG_D2, dst_x);
                    m68k_set_reg(M68K_REG_D3, dst_y);
                    m68k_set_reg(M68K_REG_D4, width);
                    m68k_set_reg(M68K_REG_D5, height);
                    m68k_set_reg(M68K_REG_D6, 0xC0);
                    UAOS_Graphics_Dispatch(GFX_SLOT_BLTBITMAPRASTPORT);
                }
            }
        }

        image = next_image;
    }
}

/* PrintIText(rp, iText, xOffset, yOffset)
 * A0 = rp, A1 = iText, D0 = xOffset, D1 = yOffset */
static void intuition_PrintIText(void)
{
    uint32_t rp     = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t itext  = m68k_get_reg(NULL, M68K_REG_A1);
    int xoff        = (int)m68k_get_reg(NULL, M68K_REG_D0);
    int yoff        = (int)m68k_get_reg(NULL, M68K_REG_D1);

    while (itext && itext + ITEXT_SIZE <= GUEST_RAM_SIZE) {
        uint8_t  front_pen = mem_u8(itext + ITEXT_OFF_FRONTPEN);
        uint8_t  back_pen  = mem_u8(itext + ITEXT_OFF_BACKPEN);
        uint8_t  draw_mode = mem_u8(itext + ITEXT_OFF_DRAWMODE);
        int16_t  left      = mem_s16(itext + ITEXT_OFF_LEFTEDGE);
        int16_t  top       = mem_s16(itext + ITEXT_OFF_TOPEDGE);
        uint32_t font_attr = mem_u32(itext + ITEXT_OFF_ITEXTFONT);
        uint32_t text      = mem_u32(itext + ITEXT_OFF_ITEXT);
        uint32_t next_text = mem_u32(itext + ITEXT_OFF_NEXTTEXT);

        int x = xoff + left;
        int y = yoff + top;

        if (font_attr) {
            /* OpenFont(textAttr) — A0 = textAttr, returns font in D0 */
            m68k_set_reg(M68K_REG_A0, font_attr);
            UAOS_Graphics_Dispatch(GFX_SLOT_OPENFONT);
            uint32_t font = m68k_get_reg(NULL, M68K_REG_D0);
            if (font) {
                /* SetFont(rp, font) — A1 = rp, A0 = font */
                m68k_set_reg(M68K_REG_A1, rp);
                m68k_set_reg(M68K_REG_A0, font);
                UAOS_Graphics_Dispatch(GFX_SLOT_SETFONT);
            }
        }

        /* SetAPen(rp, front_pen) */
        m68k_set_reg(M68K_REG_A1, rp);
        m68k_set_reg(M68K_REG_D0, front_pen);
        UAOS_Graphics_Dispatch(GFX_SLOT_SETAPEN);

        /* SetBPen(rp, back_pen) */
        m68k_set_reg(M68K_REG_A1, rp);
        m68k_set_reg(M68K_REG_D0, back_pen);
        UAOS_Graphics_Dispatch(GFX_SLOT_SETBPEN);

        /* SetDrMd(rp, draw_mode) */
        m68k_set_reg(M68K_REG_A1, rp);
        m68k_set_reg(M68K_REG_D0, draw_mode);
        UAOS_Graphics_Dispatch(GFX_SLOT_SETDRMD);

        /* Move(rp, x, y) */
        m68k_set_reg(M68K_REG_A1, rp);
        m68k_set_reg(M68K_REG_D0, x);
        m68k_set_reg(M68K_REG_D1, y);
        UAOS_Graphics_Dispatch(GFX_SLOT_MOVE);

        /* Text(rp, string, length) — A1 = rp, A0 = string, D0 = length */
        if (text) {
            int len = 0;
            while (len < 256 && text + len < GUEST_RAM_SIZE && g_ram[text + len] != 0)
                len++;
            m68k_set_reg(M68K_REG_A1, rp);
            m68k_set_reg(M68K_REG_A0, text);
            m68k_set_reg(M68K_REG_D0, len);
            UAOS_Graphics_Dispatch(GFX_SLOT_TEXT);
        }

        itext = next_text;
    }
}

/* =========================================================================
 * Dispatch entry point (called from uaos_m68k_glue.c)
 * ========================================================================= */

static void *intuition_funcs[] = {
    intuition_OpenLibrary,
    intuition_CloseLibrary,
    intuition_OpenWindow,
    intuition_CloseWindow,
    intuition_WindowToFront,
    intuition_WindowToBack,
    intuition_ActivateWindow,
    intuition_MoveWindow,
    intuition_SizeWindow,
    intuition_RefreshWindowFrame,
    intuition_ModifyIDCMP,
    intuition_SetWindowTitles,
    intuition_OpenWindowTagList,
    intuition_OpenWorkbench,
    intuition_CloseWorkbench,
    intuition_DrawBorder,
    intuition_DrawImage,
    intuition_PrintIText,
};

void UAOS_Intuition_Dispatch(uint32_t fn)
{
    if (fn == 0 || fn > (sizeof(intuition_funcs) / sizeof(intuition_funcs[0]))) {
        fprintf(stderr, "[INTUITION] unknown fn=%u\n", fn);
        return;
    }
    void (*f)(void) = (void (*)(void))intuition_funcs[fn - 1];
    f();
}

/* =========================================================================
 * ROM module registration
 * ========================================================================= */

void UAOS_INTUITION_Register(void)
{
    UAOS_ROM_Register("intuition.library", 40, 0x00005000,
                      (uint16_t)(sizeof(intuition_funcs) / sizeof(intuition_funcs[0])),
                      intuition_funcs);
}
