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

/* OpenWindowTagList(newWindow, tagList)
 * A0 = newWindow, A1 = tagList
 * Returns Window* in D0 */
static void intuition_OpenWindowTagList(void)
{
    uint32_t nw_ptr  = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t win_ptr = 0;

    if (nw_ptr) {
        int16_t left = mem_s16(nw_ptr + 0);
        int16_t top  = mem_s16(nw_ptr + 2);
        int16_t w    = mem_s16(nw_ptr + 4);
        int16_t h    = mem_s16(nw_ptr + 6);
        uint32_t title_ptr = mem_u32(nw_ptr + 22);
        uint16_t flags = mem_u16(nw_ptr + 12);

        char title[32] = "Window";
        if (title_ptr) guest_str(title, title_ptr, sizeof(title));

        if (w < 64) w = 64;
        if (h < 32) h = 32;

        IntuitionSlot *slot = alloc_slot();
        if (!slot) {
            m68k_set_reg(M68K_REG_D0, 0);
            return;
        }

        int wh = WM_AddWindow(left, top, w, h, title, intu_draw_fn, NULL);
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
        mem_w16(win_ptr + 8,  w);                 /* Width         */
        mem_w16(win_ptr + 10, h);                 /* Height        */
        mem_w8 (win_ptr + 12, 1);                /* DetailPen     */
        mem_w8 (win_ptr + 13, 0);                /* BlockPen      */
        mem_w16(win_ptr + 14, mem_u16(nw_ptr + 10)); /* IDCMPFlags */
        mem_w16(win_ptr + 16, flags);             /* Flags         */
        mem_w32(win_ptr + 18, mem_u32(nw_ptr + 14)); /* FirstGadget */
        mem_w32(win_ptr + 22, mem_u32(nw_ptr + 18)); /* CheckMark   */
        mem_w32(win_ptr + 26, title_ptr);         /* Title         */
        mem_w32(win_ptr + 30, 0);                 /* FirstRequest  */
        mem_w16(win_ptr + 34, 0);                 /* ReqCount      */
        mem_w32(win_ptr + 38, 0);                 /* WScreen       */
        /* Pad1 at 42-49 */
        mem_w32(win_ptr + 50, rp_ptr);            /* RPort         */

        slot->guest_win = win_ptr;
        slot->wm_handle = wh;

        if (flags & WFLG_ACTIVATE)
            WM_RequestFocus(wh);
    }

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
    /* WM has no explicit to-back; stub for now */
    (void)m68k_get_reg(NULL, M68K_REG_A0);
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
    /* Native WM handles its own chrome repainting */
    (void)m68k_get_reg(NULL, M68K_REG_A0);
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

    if (win_ptr && title_ptr)
        mem_w32(win_ptr + 26, title_ptr);
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
};

void UAOS_Intuition_Dispatch(uint32_t fn)
{
    if (fn == 0 || fn > 13) {
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
