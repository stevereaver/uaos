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
#include "../display/framebuffer.h"
#include "../display/desktop.h"
#include "../display/cursor.h"
#include "task.h"
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

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
#define M68K_REG_A3  11

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

static void local_str_copy(char *dst, const char *src, int max)
{
    int i = 0;
    while (i < max - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static char *local_strchr(const char *s, int c)
{
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return NULL;
}

/* IntuiText structure offsets (packed AmigaOS layout) */
#define ITEXT_OFF_FRONTPEN   0
#define ITEXT_OFF_BACKPEN    1
#define ITEXT_OFF_DRAWMODE   2
#define ITEXT_OFF_LEFTEDGE   3
#define ITEXT_OFF_TOPEDGE    5
#define ITEXT_OFF_ITEXTFONT  7
#define ITEXT_OFF_ITEXT      11
#define ITEXT_OFF_NEXTTEXT   15
#define ITEXT_SIZE           19

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
    uint8_t  idcmp_sigbit;
    uint32_t user_port;
    uint32_t window_port;
    uint32_t gad_close;
    uint32_t gad_drag;
    uint32_t gad_depth;
    uint32_t gad_size;
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

static void free_intuition_signal(int sig);

static void free_slot(IntuitionSlot *slot)
{
    if (slot) {
        if (slot->idcmp_sigbit) free_intuition_signal(slot->idcmp_sigbit);
        slot->active    = 0;
        slot->guest_win = 0;
        slot->wm_handle = -1;
        slot->min_w     = 0;
        slot->min_h     = 0;
        slot->max_w     = 0;
        slot->max_h     = 0;
        slot->idcmp_sigbit = 0;
        slot->user_port = 0;
        slot->window_port = 0;
        slot->gad_close = 0;
        slot->gad_drag = 0;
        slot->gad_depth = 0;
        slot->gad_size = 0;
    }
}

/* =========================================================================
 * IDCMP message-port helpers
 * ========================================================================= */

static int alloc_intuition_signal(void)
{
    UaosTask *t = Task_Current();
    if (!t) return -1;
    uint32_t alloc_mask = t->tc_SigAlloc;
    for (int i = 0; i < 32; i++) {
        if ((alloc_mask >> i) & 1) {
            t->tc_SigAlloc &= ~(1U << i);
            return i;
        }
    }
    return -1;
}

static void free_intuition_signal(int sig)
{
    if (sig < 0 || sig >= 32) return;
    UaosTask *t = Task_Current();
    if (t) t->tc_SigAlloc |= (1U << sig);
}

static void init_guest_list(uint32_t list)
{
    mem_w32(list + LH_OFF_HEAD, list + LH_OFF_TAIL);
    mem_w32(list + LH_OFF_TAIL, 0);
    mem_w32(list + LH_OFF_TAILPRED, list + LH_OFF_HEAD);
    mem_w8(list + LH_OFF_TYPE, 0);
}

static uint32_t create_guest_msgport(uint8_t sigbit)
{
    uint32_t port = intu_alloc(MP_SIZE);
    if (!port) return 0;

    mem_w32(port + MP_OFF_LN_SUCC, port);
    mem_w32(port + MP_OFF_LN_PRED, port);
    mem_w8(port + MP_OFF_LN_TYPE, NT_MSGPORT);
    mem_w8(port + MP_OFF_LN_PRI, 0);
    mem_w32(port + MP_OFF_LN_NAME, 0);
    mem_w8(port + MP_OFF_FLAGS, 0);
    mem_w8(port + MP_OFF_SIGBIT, sigbit);

    UaosTask *t = Task_Current();
    uint32_t sigtask = t ? t->m68k_task_struct : 0;
    mem_w32(port + MP_OFF_SIGTASK, sigtask);

    init_guest_list(port + MP_OFF_MSGLIST);
    return port;
}

static void guest_list_add_tail(uint32_t list, uint32_t node)
{
    uint32_t tailpred = mem_u32(list + LH_OFF_TAILPRED);

    mem_w32(node + MSG_OFF_LN_SUCC, list + LH_OFF_TAIL);
    mem_w32(node + MSG_OFF_LN_PRED, tailpred);
    mem_w32(tailpred + MSG_OFF_LN_SUCC, node);
    mem_w32(list + LH_OFF_TAILPRED, node);
}

static uint32_t guest_list_remove_head(uint32_t list)
{
    uint32_t head = mem_u32(list + LH_OFF_HEAD);
    uint32_t tail = list + LH_OFF_TAIL;
    if (head == tail) return 0;

    uint32_t succ = mem_u32(head + MSG_OFF_LN_SUCC);
    uint32_t pred = mem_u32(head + MSG_OFF_LN_PRED);

    mem_w32(pred + MSG_OFF_LN_SUCC, succ);
    mem_w32(succ + MSG_OFF_LN_PRED, pred);
    return head;
}

static void init_guest_message(uint32_t msg, uint16_t length, uint32_t reply_port, uint32_t data)
{
    mem_w32(msg + MSG_OFF_LN_SUCC, 0);
    mem_w32(msg + MSG_OFF_LN_PRED, 0);
    mem_w8(msg + MSG_OFF_LN_TYPE, 0);
    mem_w8(msg + MSG_OFF_LN_PRI, 0);
    mem_w32(msg + MSG_OFF_LN_NAME, 0);
    mem_w16(msg + MSG_OFF_LENGTH, length);
    mem_w32(msg + MSG_OFF_REPLYPORT, reply_port);
    mem_w32(msg + MSG_OFF_DATA, data);
}

static uint32_t get_guest_window_from_handle(int wm_handle)
{
    for (int i = 0; i < MAX_INTUITION_WINS; i++) {
        if (g_intu_wins[i].active && g_intu_wins[i].wm_handle == wm_handle)
            return g_intu_wins[i].guest_win;
    }
    return 0;
}

static IntuitionSlot *get_slot_from_handle(int wm_handle)
{
    for (int i = 0; i < MAX_INTUITION_WINS; i++) {
        if (g_intu_wins[i].active && g_intu_wins[i].wm_handle == wm_handle)
            return &g_intu_wins[i];
    }
    return NULL;
}

static void post_intui_message(uint32_t win_ptr, uint32_t class, uint16_t code, uint16_t qualifier, int16_t mouse_x, int16_t mouse_y, uint32_t iaddress)
{
    if (!win_ptr) return;
    uint32_t idcmp = mem_u32(win_ptr + WIN_OFF_IDCMPFLAGS);
    if (!idcmp || !(idcmp & class)) return;

    uint32_t user_port = mem_u32(win_ptr + WIN_OFF_USERPORT);
    if (!user_port) return;

    uint32_t msg = intu_alloc(IM_SIZE);
    if (!msg) return;

    uint32_t window_port = mem_u32(win_ptr + WIN_OFF_WINDOWPORT);
    init_guest_message(msg, IM_SIZE, window_port, 0);

    mem_w32(msg + IM_OFF_CLASS, class);
    mem_w16(msg + IM_OFF_CODE, code);
    mem_w16(msg + IM_OFF_QUALIFIER, qualifier);
    mem_w32(msg + IM_OFF_IADDRESS, iaddress);
    mem_w16(msg + IM_OFF_MOUSEX, (uint16_t)mouse_x);
    mem_w16(msg + IM_OFF_MOUSEY, (uint16_t)mouse_y);
    mem_w32(msg + IM_OFF_SECONDS, 0);
    mem_w32(msg + IM_OFF_MICROS, 0);
    mem_w32(msg + IM_OFF_IDCMPWINDOW, win_ptr);
    mem_w32(msg + IM_OFF_SPECIALLINK, 0);

    guest_list_add_tail(user_port + MP_OFF_MSGLIST, msg);

    uint8_t sigbit = mem_u8(user_port + MP_OFF_SIGBIT);
    UaosTask *t = Task_Current();
    if (t) Signal(t, 1U << sigbit);
}

static uint32_t create_guest_gadget(uint32_t next, int16_t left, int16_t top,
                                     int16_t width, int16_t height,
                                     uint16_t gadget_type, uint16_t gadget_id)
{
    uint32_t gad = intu_alloc(GAD_SIZE);
    if (!gad) return 0;

    mem_w32(gad + GAD_OFF_NEXTGADGET, next);
    mem_w16(gad + GAD_OFF_LEFTEDGE,   left);
    mem_w16(gad + GAD_OFF_TOPEDGE,    top);
    mem_w16(gad + GAD_OFF_WIDTH,      width);
    mem_w16(gad + GAD_OFF_HEIGHT,     height);
    mem_w16(gad + GAD_OFF_FLAGS,      GFLG_SYSGADGET);
    mem_w16(gad + GAD_OFF_ACTIVATION, GACT_IMMEDIATE | GACT_RELVERIFY);
    mem_w16(gad + GAD_OFF_GADGETTYPE, GTYP_SYSGADGET | gadget_type);
    mem_w32(gad + GAD_OFF_GADGETRENDER, 0);
    mem_w32(gad + GAD_OFF_SELECTRENDER, 0);
    mem_w32(gad + GAD_OFF_GADGETTEXT, 0);
    mem_w32(gad + GAD_OFF_MUTUALEXCLUDE, 0);
    mem_w32(gad + GAD_OFF_SPECIALINFO, 0);
    mem_w16(gad + GAD_OFF_GADGETID, gadget_id);
    mem_w32(gad + GAD_OFF_USERDATA, 0);

    return gad;
}

static void create_system_gadgets(IntuitionSlot *slot, int16_t win_w, int16_t win_h)
{
    uint32_t win_ptr = slot->guest_win;
    if (!win_ptr) return;

    /* Link order: close -> drag -> depth -> size.
     * The WM already renders/hit-tests these system gadgets; here we expose
     * them as guest Gadget structures so IDCMP_GADGETDOWN/UP can report them. */
    uint32_t size_gad  = create_guest_gadget(0, win_w - WM_SCROLLBAR_W, win_h - WM_SCROLLBAR_W,
                                              WM_SCROLLBAR_W, WM_SCROLLBAR_W,
                                              GTYP_SIZER, SYSGAD_SIZE);
    uint32_t depth_gad = create_guest_gadget(size_gad, win_w - 15, 1, 13, 13,
                                              GTYP_WDEPTH, SYSGAD_DEPTH);
    uint32_t drag_gad  = create_guest_gadget(depth_gad, 0, 0, win_w, WM_TITLEBAR_H,
                                              GTYP_WDRAGGING, SYSGAD_DRAG);
    uint32_t close_gad = create_guest_gadget(drag_gad, 1, 1, 14, 14,
                                              GTYP_WCLOSE, SYSGAD_CLOSE);

    slot->gad_size  = size_gad;
    slot->gad_depth = depth_gad;
    slot->gad_drag  = drag_gad;
    slot->gad_close = close_gad;

    mem_w32(win_ptr + WIN_OFF_FIRSTGADGET, close_gad);
}

static uint32_t gadget_by_id(IntuitionSlot *slot, uint16_t id)
{
    switch (id) {
        case SYSGAD_CLOSE: return slot->gad_close;
        case SYSGAD_DRAG:  return slot->gad_drag;
        case SYSGAD_DEPTH: return slot->gad_depth;
        case SYSGAD_SIZE:  return slot->gad_size;
    }
    return 0;
}

static int intu_wm_event_handler(int wm_handle, int event_type, int p1, int p2, int p3)
{
    uint32_t win_ptr = get_guest_window_from_handle(wm_handle);
    if (!win_ptr) return 1;

    uint32_t idcmp = mem_u32(win_ptr + WIN_OFF_IDCMPFLAGS);
    if (!idcmp) return 1;

    int wx = 0, wy = 0, ww = 0, wh = 0;
    (void)ww; (void)wh;
    WM_GetWindowRect(wm_handle, &wx, &wy, &ww, &wh);

    switch (event_type) {
        case WM_EVT_CLOSE_REQUEST:
            if (idcmp & IDCMP_CLOSEWINDOW) {
                post_intui_message(win_ptr, IDCMP_CLOSEWINDOW, 0, 0, 0, 0, 0);
                return 0; /* veto WM close; guest will call CloseWindow */
            }
            return 1;

        case WM_EVT_GADGET_DOWN: {
            IntuitionSlot *slot = get_slot_from_handle(wm_handle);
            uint32_t gad = slot ? gadget_by_id(slot, (uint16_t)p1) : 0;
            if (idcmp & IDCMP_GADGETDOWN)
                post_intui_message(win_ptr, IDCMP_GADGETDOWN, 0, 0, 0, 0, gad);
            return 0;
        }

        case WM_EVT_GADGET_UP: {
            IntuitionSlot *slot = get_slot_from_handle(wm_handle);
            uint32_t gad = slot ? gadget_by_id(slot, (uint16_t)p1) : 0;
            if (idcmp & IDCMP_GADGETUP)
                post_intui_message(win_ptr, IDCMP_GADGETUP, 0, 0, 0, 0, gad);
            return 0;
        }

        case WM_EVT_MOUSE_DOWN:
            if (idcmp & IDCMP_MOUSEBUTTONS)
                post_intui_message(win_ptr, IDCMP_MOUSEBUTTONS, (uint16_t)p1, 0,
                                   (int16_t)(p2 - wx), (int16_t)(p3 - wy), 0);
            return 0;

        case WM_EVT_MOUSE_UP:
            if (idcmp & IDCMP_MOUSEBUTTONS)
                post_intui_message(win_ptr, IDCMP_MOUSEBUTTONS, (uint16_t)(0x100 + p1), 0,
                                   (int16_t)(p2 - wx), (int16_t)(p3 - wy), 0);
            return 0;

        case WM_EVT_MOUSE_MOVE:
            if (idcmp & IDCMP_MOUSEMOVE)
                post_intui_message(win_ptr, IDCMP_MOUSEMOVE, 0, 0,
                                   (int16_t)(p1 - wx), (int16_t)(p2 - wy), 0);
            return 0;

        case WM_EVT_KEY:
            if (idcmp & IDCMP_RAWKEY)
                post_intui_message(win_ptr, IDCMP_RAWKEY, (uint16_t)p1, 0, 0, 0, 0);
            if (idcmp & IDCMP_VANILLAKEY)
                post_intui_message(win_ptr, IDCMP_VANILLAKEY, (uint16_t)p1, 0, 0, 0, 0);
            return 0;

        case WM_EVT_RESIZE:
            if (idcmp & IDCMP_NEWSIZE)
                post_intui_message(win_ptr, IDCMP_NEWSIZE, 0, 0, 0, 0, 0);
            return 0;

        case WM_EVT_FOCUS:
            if (p1 && (idcmp & IDCMP_ACTIVEWINDOW))
                post_intui_message(win_ptr, IDCMP_ACTIVEWINDOW, 0, 0, 0, 0, 0);
            if (!p1 && (idcmp & IDCMP_INACTIVEWINDOW))
                post_intui_message(win_ptr, IDCMP_INACTIVEWINDOW, 0, 0, 0, 0, 0);
            return 0;
    }

    return 0;
}

/* Empty draw callback — m68k app should handle rendering via IDCMP */
static void intu_draw_fn(int win_x, int win_y, int win_w, int win_h)
{
    (void)win_x; (void)win_y; (void)win_w; (void)win_h;
}

/* =========================================================================
 * Requester support (AutoRequest / EasyRequest / BuildSysRequest / FreeSysRequest)
 * ========================================================================= */

#define REQ_MAX_BUTTONS  4
#define REQ_BTN_W       70
#define REQ_BTN_H       20

typedef struct {
    int      wm_handle;
    uint32_t guest_win;
    uint8_t  sigbit;
    uint32_t sigmask;
    UaosTask *task;
    int      result;
    uint8_t  active;
    int      num_buttons;
    char     button_labels[REQ_MAX_BUTTONS][32];
    char     body_text[256];
    char     title[32];
    int      btn_x[REQ_MAX_BUTTONS];
    int      btn_y[REQ_MAX_BUTTONS];
} ReqSlot;

static ReqSlot g_req_slot;

static void clear_req_slot(void)
{
    if (g_req_slot.active && g_req_slot.sigbit) {
        free_intuition_signal(g_req_slot.sigbit);
    }
    g_req_slot.active    = 0;
    g_req_slot.wm_handle = -1;
    g_req_slot.guest_win = 0;
    g_req_slot.sigbit    = 0;
    g_req_slot.sigmask   = 0;
    g_req_slot.task      = NULL;
    g_req_slot.result    = 0;
    g_req_slot.num_buttons = 0;
}

static void req_set_result(int result)
{
    g_req_slot.result = result;
    if (g_req_slot.task && g_req_slot.sigmask) {
        Signal(g_req_slot.task, g_req_slot.sigmask);
    }
}

static void req_draw_fn(int win_x, int win_y, int win_w, int win_h)
{
    if (!g_req_slot.active) return;

    FB_FillRect(win_x, win_y, win_w, win_h, WB_LIGHT_GREY);
    FB_DrawRect(win_x, win_y, win_w, win_h, WB_BLACK);

    FB_FillRect(win_x + 1, win_y + 1, win_w - 2, WM_TITLEBAR_H - 1, WB_BLUE);
    if (g_req_slot.title[0])
        FB_PutStrCentred(win_x + 1, win_y + 1, win_w - 2, WM_TITLEBAR_H - 1,
                         g_req_slot.title, WB_WHITE, WB_BLUE);

    if (g_req_slot.body_text[0])
        FB_PutStr(win_x + 10, win_y + WM_TITLEBAR_H + 10,
                  g_req_slot.body_text, WB_BLACK, WB_LIGHT_GREY);

    for (int i = 0; i < g_req_slot.num_buttons; i++) {
        int bx = win_x + g_req_slot.btn_x[i];
        int by = win_y + g_req_slot.btn_y[i];
        FB_FillRect(bx, by, REQ_BTN_W, REQ_BTN_H, WB_GREY);
        FB_DrawRect(bx, by, REQ_BTN_W, REQ_BTN_H, WB_BLACK);
        FB_PutStrCentred(bx, by, REQ_BTN_W, REQ_BTN_H,
                         g_req_slot.button_labels[i], WB_BLACK, WB_GREY);
    }
}

static int req_event_handler(int wh, int event_type, int p1, int p2, int p3)
{
    (void)p1;
    if (!g_req_slot.active || g_req_slot.wm_handle != wh) return 1;

    if (event_type == WM_EVT_CLOSE_REQUEST) {
        req_set_result(0);
        return 0;  /* Veto WM close; caller will call FreeSysRequest */
    }

    if (event_type == WM_EVT_MOUSE_DOWN) {
        int mx = p2, my = p3;
        for (int i = 0; i < g_req_slot.num_buttons; i++) {
            int bx = g_req_slot.btn_x[i];
            int by = g_req_slot.btn_y[i];
            if (mx >= bx && mx < bx + REQ_BTN_W &&
                my >= by && my < by + REQ_BTN_H) {
                req_set_result(i);
                return 0;
            }
        }
    }

    return 0;
}

static int create_requester_window(int x, int y, int w, int h, const char *title)
{
    int wh = WM_AddWindow(x, y, w, h, title, req_draw_fn, NULL);
    if (wh < 0) return -1;
    WM_SetEventHandler(wh, req_event_handler);
    WM_RequestFocus(wh);
    return wh;
}

static void build_req_guest_window(uint32_t win_ptr)
{
    if (!win_ptr) return;
    memset(&g_ram[win_ptr], 0, sizeof(AmigaWindow));
    mem_w32(win_ptr + WIN_OFF_FLAGS, WFLG_ACTIVATE | WFLG_CLOSEGADGET);
    mem_w16(win_ptr + WIN_OFF_WIDTH, 300);
    mem_w16(win_ptr + WIN_OFF_HEIGHT, 120);
    mem_w32(win_ptr + WIN_OFF_IDCMPFLAGS, IDCMP_GADGETUP | IDCMP_RAWKEY | IDCMP_CLOSEWINDOW);
}

static void guest_itext_text(char *out, uint32_t itext_ptr, size_t out_size)
{
    out[0] = '\0';
    if (!itext_ptr || !out_size) return;
    uint32_t text_ptr = mem_u32(itext_ptr + ITEXT_OFF_ITEXT);
    if (text_ptr) guest_str(out, text_ptr, out_size);
}

static uint32_t build_requester_internal(uint32_t parent_win, const char *title,
                                         const char *body, int num_buttons,
                                         const char *buttons[], int width, int height)
{
    if (g_req_slot.active) return 0;

    clear_req_slot();

    int sig = alloc_intuition_signal();
    if (sig < 0) return 0;

    int x = 100, y = 100;
    if (parent_win) {
        x = (int)mem_s16(parent_win + WIN_OFF_LEFTEDGE) + 20;
        y = (int)mem_s16(parent_win + WIN_OFF_TOPEDGE) + 20;
    }

    if (width < 200) width = 200;
    if (height < 80) height = 80;

    int wh = create_requester_window(x, y, width, height, title);
    if (wh < 0) {
        free_intuition_signal(sig);
        return 0;
    }

    uint32_t guest_win = intu_alloc(sizeof(AmigaWindow));
    if (!guest_win) {
        WM_CloseWindow(wh);
        free_intuition_signal(sig);
        return 0;
    }
    build_req_guest_window(guest_win);

    g_req_slot.active      = 1;
    g_req_slot.wm_handle   = wh;
    g_req_slot.guest_win   = guest_win;
    g_req_slot.sigbit      = (uint8_t)sig;
    g_req_slot.sigmask     = 1U << sig;
    g_req_slot.task        = Task_Current();
    g_req_slot.result      = 0;
    g_req_slot.num_buttons = num_buttons;

    local_str_copy(g_req_slot.title, title, sizeof(g_req_slot.title));
    local_str_copy(g_req_slot.body_text, body, sizeof(g_req_slot.body_text));

    int total_btn_w = num_buttons * REQ_BTN_W + (num_buttons - 1) * 10;
    int start_x = (width - total_btn_w) / 2;
    int by = height - REQ_BTN_H - 10;
    for (int i = 0; i < num_buttons; i++) {
        local_str_copy(g_req_slot.button_labels[i], buttons[i],
                       sizeof(g_req_slot.button_labels[i]));
        g_req_slot.btn_x[i] = start_x + i * (REQ_BTN_W + 10);
        g_req_slot.btn_y[i] = by;
    }

    WM_Redraw();
    return guest_win;
}

static void free_requester_internal(void)
{
    if (!g_req_slot.active) return;
    if (g_req_slot.wm_handle >= 0) WM_CloseWindow(g_req_slot.wm_handle);
    /* Guest window memory is not reclaimed by intu_alloc(). */
    clear_req_slot();
}

static int wait_requester_internal(void)
{
    if (!g_req_slot.active || !g_req_slot.task || !g_req_slot.sigmask) return 0;
    while (g_req_slot.active) {
        uint32_t sigs = Wait(g_req_slot.sigmask);
        if (sigs & g_req_slot.sigmask) break;
    }
    return g_req_slot.result;
}

/* =========================================================================
 * Screen support
 * ========================================================================= */

#define MAX_INTUITION_SCREENS 4

typedef struct {
    uint32_t guest_screen;
    uint8_t  active;
    char     title[64];
    char     pub_name[64];
    int16_t  left;
    int16_t  top;
    int16_t  width;
    int16_t  height;
    uint8_t  show_title;
    uint8_t  is_front;
    int      lock_count;
} ScreenSlot;

static ScreenSlot g_intu_screens[MAX_INTUITION_SCREENS];

static ScreenSlot *find_screen_slot(uint32_t screen_ptr)
{
    for (int i = 0; i < MAX_INTUITION_SCREENS; i++) {
        if (g_intu_screens[i].active && g_intu_screens[i].guest_screen == screen_ptr)
            return &g_intu_screens[i];
    }
    return NULL;
}

static ScreenSlot *alloc_screen_slot(void)
{
    for (int i = 0; i < MAX_INTUITION_SCREENS; i++) {
        if (!g_intu_screens[i].active) {
            g_intu_screens[i].active = 1;
            return &g_intu_screens[i];
        }
    }
    return NULL;
}

static void init_guest_screen(uint32_t scr, ScreenSlot *slot)
{
    memset(&g_ram[scr], 0, SCR_SIZE);
    mem_w16(scr + SCR_OFF_LEFTEDGE,  slot->left);
    mem_w16(scr + SCR_OFF_TOPEDGE,   slot->top);
    mem_w16(scr + SCR_OFF_WIDTH,     slot->width);
    mem_w16(scr + SCR_OFF_HEIGHT,    slot->height);
    mem_w32(scr + SCR_OFF_TITLE,     (uint32_t)0);
    mem_w32(scr + SCR_OFF_DEFAULTTITLE, (uint32_t)0);
    mem_w16(scr + SCR_OFF_FLAGS,     slot->show_title ? SHOWTITLE : 0);
    mem_w32(scr + SCR_OFF_FIRSTWINDOW, 0);
    mem_w32(scr + SCR_OFF_NEXTSCREEN,  0);
}

static void update_desktop_title(void)
{
    ScreenSlot *front = NULL;
    for (int i = 0; i < MAX_INTUITION_SCREENS; i++) {
        if (g_intu_screens[i].active && g_intu_screens[i].is_front) {
            front = &g_intu_screens[i];
            break;
        }
    }
    if (front && front->show_title && front->title[0]) {
        Desktop_SetScreenTitle(front->title, 1);
    } else {
        Desktop_SetScreenTitle(NULL, 0);
    }
}

static uint32_t open_screen_internal(uint32_t new_screen_ptr, uint32_t tag_list_ptr)
{
    int16_t left = 0, top = 0, width = 0, height = 0, depth = 2;
    uint8_t detail_pen = 0, block_pen = 1;
    uint32_t title_ptr = 0, font_ptr = 0, pub_name_ptr = 0;
    uint16_t type = CUSTOMSCREEN;
    uint8_t show_title = 1;

    /* Parse NewScreen defaults if provided */
    if (new_screen_ptr) {
        left       = mem_s16(new_screen_ptr + NS_OFF_LEFTEDGE);
        top        = mem_s16(new_screen_ptr + NS_OFF_TOPEDGE);
        width      = mem_s16(new_screen_ptr + NS_OFF_WIDTH);
        height     = mem_s16(new_screen_ptr + NS_OFF_HEIGHT);
        depth      = mem_s16(new_screen_ptr + NS_OFF_DEPTH);
        detail_pen = mem_u8(new_screen_ptr + NS_OFF_DETAILPEN);
        block_pen  = mem_u8(new_screen_ptr + NS_OFF_BLOCKPEN);
        type       = mem_u16(new_screen_ptr + NS_OFF_TYPE);
        font_ptr   = mem_u32(new_screen_ptr + NS_OFF_FONT);
        title_ptr  = mem_u32(new_screen_ptr + NS_OFF_DEFAULTTITLE);
    }

    /* Override with SA_* tags if present */
    if (tag_list_ptr) {
        uint32_t p = tag_list_ptr;
        while (p + 8 <= GUEST_RAM_SIZE) {
            uint32_t tag  = mem_u32(p);
            uint32_t data = mem_u32(p + 4);
            if (tag == TAG_DONE) break;
            p += 8;
            switch (tag) {
                case SA_Left:       left       = (int16_t)(uint16_t)data; break;
                case SA_Top:        top        = (int16_t)(uint16_t)data; break;
                case SA_Width:      width      = (int16_t)(uint16_t)data; break;
                case SA_Height:     height     = (int16_t)(uint16_t)data; break;
                case SA_Depth:      depth      = (int16_t)(uint16_t)data; break;
                case SA_DetailPen:  detail_pen = (uint8_t)data; break;
                case SA_BlockPen:   block_pen  = (uint8_t)data; break;
                case SA_Title:      title_ptr  = data; break;
                case SA_Font:       font_ptr   = data; break;
                case SA_Type:       type       = (uint16_t)data; break;
                case SA_ShowTitle:  show_title = data ? 1 : 0; break;
                case SA_Quiet:      if (data) type |= QUIET; break;
                case SA_Behind:     if (data) type |= BEHIND; break;
                case SA_PubName:    pub_name_ptr = data; break;
                default: break;
            }
        }
    }

    if (width <= 0)  width  = (int16_t)g_fb.width;
    if (height <= 0) height = (int16_t)g_fb.height;

    ScreenSlot *slot = alloc_screen_slot();
    if (!slot) return 0;

    uint32_t guest_screen = intu_alloc(SCR_SIZE);
    if (!guest_screen) {
        slot->active = 0;
        return 0;
    }

    slot->guest_screen = guest_screen;
    slot->left         = left;
    slot->top          = top;
    slot->width        = width;
    slot->height       = height;
    slot->show_title   = show_title;
    slot->is_front     = 1;
    slot->lock_count   = 0;
    slot->title[0]     = '\0';
    slot->pub_name[0]  = '\0';
    if (title_ptr)
        guest_str(slot->title, title_ptr, sizeof(slot->title));
    if (pub_name_ptr)
        guest_str(slot->pub_name, pub_name_ptr, sizeof(slot->pub_name));

    init_guest_screen(guest_screen, slot);
    if (title_ptr)
        mem_w32(guest_screen + SCR_OFF_TITLE, title_ptr);
    if (font_ptr)
        mem_w32(guest_screen + SCR_OFF_FONT, font_ptr);
    (void)depth; (void)detail_pen; (void)block_pen;

    /* Mark this screen as the front screen; others go behind */
    for (int i = 0; i < MAX_INTUITION_SCREENS; i++) {
        if (g_intu_screens[i].active && &g_intu_screens[i] != slot)
            g_intu_screens[i].is_front = 0;
    }
    update_desktop_title();
    return guest_screen;
}

static uint32_t find_pub_screen_by_name(const char *name)
{
    if (!name || !name[0]) return 0;
    for (int i = 0; i < MAX_INTUITION_SCREENS; i++) {
        if (!g_intu_screens[i].active) continue;
        if (g_intu_screens[i].pub_name[0] == '\0') continue;
        int j = 0;
        while (name[j] && g_intu_screens[i].pub_name[j] &&
               name[j] == g_intu_screens[i].pub_name[j]) j++;
        if (name[j] == '\0' && g_intu_screens[i].pub_name[j] == '\0')
            return g_intu_screens[i].guest_screen;
    }
    return 0;
}

static uint32_t get_default_pub_screen(void)
{
    /* Return the frontmost active screen, or create a default Workbench screen. */
    for (int i = 0; i < MAX_INTUITION_SCREENS; i++) {
        if (g_intu_screens[i].active && g_intu_screens[i].is_front)
            return g_intu_screens[i].guest_screen;
    }
    for (int i = 0; i < MAX_INTUITION_SCREENS; i++) {
        if (g_intu_screens[i].active)
            return g_intu_screens[i].guest_screen;
    }
    /* Create a default Workbench screen. */
    return open_screen_internal(0, 0);
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
#define INTUITION_AUTO_REQUEST       19
#define INTUITION_BUILD_SYS_REQUEST  20
#define INTUITION_FREE_SYS_REQUEST   21
#define INTUITION_EASY_REQUEST_ARGS  22
#define INTUITION_OPEN_SCREEN        23
#define INTUITION_CLOSE_SCREEN       24
#define INTUITION_MOVE_SCREEN        25
#define INTUITION_SCREEN_TO_FRONT    26
#define INTUITION_SCREEN_TO_BACK     27
#define INTUITION_SHOW_TITLE         28
#define INTUITION_OPEN_SCREEN_TAGS   29
#define INTUITION_SET_MENU_STRIP     30
#define INTUITION_CLEAR_MENU_STRIP   31
#define INTUITION_RESET_MENU_STRIP   32
#define INTUITION_ITEM_ADDRESS       33
#define INTUITION_LOCK_PUB_SCREEN        34
#define INTUITION_UNLOCK_PUB_SCREEN      35
#define INTUITION_LOCK_PUB_SCREEN_LIST   36
#define INTUITION_UNLOCK_PUB_SCREEN_LIST 37
#define INTUITION_SET_POINTER            38
#define INTUITION_CLEAR_POINTER          39
#define INTUITION_SET_WINDOW_POINTER_A   40
#define INTUITION_GET_DEF_PREFS          41
#define INTUITION_GET_PREFS              42
#define INTUITION_SET_PREFS              43
#define INTUITION_LOCK_GUI_PREFS         44
#define INTUITION_UNLOCK_GUI_PREFS       45

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
    uint16_t *flags, uint32_t *idcmp, uint32_t *title_ptr,
    int16_t *min_w, int16_t *min_h, int16_t *max_w, int16_t *max_h,
    uint32_t *pub_screen_ptr, uint32_t *pub_screen_name_ptr,
    uint8_t *pub_screen_fallback)
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
            case WA_IDCMP:     *idcmp     = data; break;
            case WA_Title:     *title_ptr = data; break;
            case WA_MinWidth:  *min_w     = (int16_t)(int32_t)data; break;
            case WA_MinHeight: *min_h     = (int16_t)(int32_t)data; break;
            case WA_MaxWidth:  *max_w     = (int16_t)(int32_t)data; break;
            case WA_MaxHeight: *max_h     = (int16_t)(int32_t)data; break;
            case WA_PubScreen:       *pub_screen_ptr      = data; break;
            case WA_PubScreenName:   *pub_screen_name_ptr = data; break;
            case WA_PubScreenFallBack: *pub_screen_fallback = data ? 1 : 0; break;
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
    uint16_t flags = 0;
    uint32_t idcmp = 0;
    uint32_t title_ptr = 0;
    int16_t  min_w = 0, min_h = 0, max_w = 0, max_h = 0;
    uint32_t first_gadget = 0;
    uint32_t pub_screen_ptr = 0, pub_screen_name_ptr = 0;
    uint8_t  pub_screen_fallback = 0;

    if (nw_ptr) {
        left         = mem_s16(nw_ptr + 0);
        top          = mem_s16(nw_ptr + 2);
        width        = mem_s16(nw_ptr + 4);
        height       = mem_s16(nw_ptr + 6);
        idcmp        = mem_u16(nw_ptr + 10);
        flags        = mem_u16(nw_ptr + 12);
        first_gadget = mem_u32(nw_ptr + 14);
        title_ptr    = mem_u32(nw_ptr + 22);
        min_w        = mem_s16(nw_ptr + 34);
        min_h        = mem_s16(nw_ptr + 36);
        max_w        = mem_s16(nw_ptr + 38);
        max_h        = mem_s16(nw_ptr + 40);
    }

    if (tag_list) {
        parse_window_tags(tag_list, &left, &top, &width, &height,
                          &flags, &idcmp, &title_ptr,
                          &min_w, &min_h, &max_w, &max_h,
                          &pub_screen_ptr, &pub_screen_name_ptr,
                          &pub_screen_fallback);
    }

    /* Resolve public screen for visitor windows. */
    uint32_t wscreen = 0;
    if (pub_screen_ptr) {
        wscreen = pub_screen_ptr;
    } else if (pub_screen_name_ptr) {
        char psname[64] = "";
        guest_str(psname, pub_screen_name_ptr, sizeof(psname));
        wscreen = find_pub_screen_by_name(psname);
        if (!wscreen && pub_screen_fallback)
            wscreen = get_default_pub_screen();
    } else {
        wscreen = get_default_pub_screen();
    }

    if (wscreen) {
        ScreenSlot *scr = find_screen_slot(wscreen);
        if (scr) {
            /* Position relative to screen; default visitor placement under menu bar. */
            if (left == 0 && top == 0) {
                top = 20;
            }
            left += scr->left;
            top  += scr->top;
        }
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

    /* Build an AmigaOS-compatible Window structure. */
    memset(&g_ram[win_ptr], 0, sizeof(AmigaWindow));
    mem_w32(win_ptr + WIN_OFF_NEXTWINDOW,   0);
    mem_w16(win_ptr + WIN_OFF_LEFTEDGE,     left);
    mem_w16(win_ptr + WIN_OFF_TOPEDGE,      top);
    mem_w16(win_ptr + WIN_OFF_WIDTH,        width);
    mem_w16(win_ptr + WIN_OFF_HEIGHT,       height);
    mem_w16(win_ptr + 16,                   min_w);        /* MinWidth     */
    mem_w16(win_ptr + 18,                   min_h);        /* MinHeight    */
    mem_w16(win_ptr + 20,                   (uint16_t)max_w); /* MaxWidth  */
    mem_w16(win_ptr + 22,                   (uint16_t)max_h); /* MaxHeight   */
    mem_w32(win_ptr + WIN_OFF_FLAGS,        flags);
    mem_w32(win_ptr + WIN_OFF_FIRSTGADGET,  first_gadget);
    mem_w32(win_ptr + WIN_OFF_TITLE,        title_ptr);
    mem_w32(win_ptr + WIN_OFF_FIRSTREQUEST, 0);
    mem_w16(win_ptr + WIN_OFF_REQCOUNT,     0);
    mem_w32(win_ptr + WIN_OFF_WSCREEN,      wscreen);
    mem_w32(win_ptr + WIN_OFF_RPORT,        rp_ptr);
    mem_w8 (win_ptr + 54,                   WM_BORDER);     /* BorderLeft   */
    mem_w8 (win_ptr + 55,                   WM_TITLEBAR_H);/* BorderTop    */
    mem_w8 (win_ptr + 56,                   WM_BORDER);     /* BorderRight  */
    mem_w8 (win_ptr + 57,                   WM_BORDER);     /* BorderBottom */
    mem_w32(win_ptr + WIN_OFF_IDCMPFLAGS,   idcmp);
    mem_w8 (win_ptr + WIN_OFF_DETAILPEN,   1);
    mem_w8 (win_ptr + WIN_OFF_BLOCKPEN,      0);

    slot->guest_win = win_ptr;
    slot->wm_handle = wh;
    slot->min_w     = min_w;
    slot->min_h     = min_h;
    slot->max_w     = max_w;
    slot->max_h     = max_h;

    if (idcmp) {
        int sig = alloc_intuition_signal();
        if (sig >= 0) {
            uint32_t user_port = create_guest_msgport((uint8_t)sig);
            uint32_t window_port = create_guest_msgport((uint8_t)sig);
            if (user_port && window_port) {
                mem_w32(win_ptr + WIN_OFF_USERPORT,   user_port);
                mem_w32(win_ptr + WIN_OFF_WINDOWPORT, window_port);
                slot->idcmp_sigbit = (uint8_t)sig;
                slot->user_port    = user_port;
                slot->window_port  = window_port;
            } else {
                free_intuition_signal(sig);
            }
        }
    }

    WM_SetEventHandler(wh, intu_wm_event_handler);
    create_system_gadgets(slot, width, height);

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
    uint32_t title_ptr = mem_u32(win_ptr + WIN_OFF_TITLE);
    if (title_ptr) guest_str(title, title_ptr, sizeof(title));

    int new_wh = WM_AddWindow(
        (int)mem_s16(win_ptr + 4),
        (int)mem_s16(win_ptr + 6),
        w, h, title, intu_draw_fn, NULL);

    if (new_wh >= 0) {
        slot->wm_handle = new_wh;
        mem_w16(win_ptr + 8,  (int16_t)w);
        mem_w16(win_ptr + 10, (int16_t)h);
        WM_SetEventHandler(new_wh, intu_wm_event_handler);
        create_system_gadgets(slot, (int16_t)w, (int16_t)h);

        uint32_t idcmp = mem_u32(win_ptr + WIN_OFF_IDCMPFLAGS);
        if (idcmp & IDCMP_NEWSIZE)
            post_intui_message(win_ptr, IDCMP_NEWSIZE, 0, 0, 0, 0, 0);
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
    uint32_t flags   = m68k_get_reg(NULL, M68K_REG_D0);
    if (win_ptr) {
        mem_w32(win_ptr + WIN_OFF_IDCMPFLAGS, flags);

        /* Allocate a UserPort if one doesn't exist and IDCMP is now wanted. */
        uint32_t user_port = mem_u32(win_ptr + WIN_OFF_USERPORT);
        if (flags && !user_port) {
            IntuitionSlot *slot = find_slot_by_guest(win_ptr);
            if (slot) {
                int sig = alloc_intuition_signal();
                if (sig >= 0) {
                    uint32_t up = create_guest_msgport((uint8_t)sig);
                    uint32_t wp = create_guest_msgport((uint8_t)sig);
                    if (up && wp) {
                        mem_w32(win_ptr + WIN_OFF_USERPORT,   up);
                        mem_w32(win_ptr + WIN_OFF_WINDOWPORT, wp);
                        slot->idcmp_sigbit = (uint8_t)sig;
                        slot->user_port    = up;
                        slot->window_port  = wp;
                    } else {
                        free_intuition_signal(sig);
                    }
                }
            }
        }
    }
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
        mem_w32(win_ptr + WIN_OFF_TITLE, title_ptr);

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

/* AutoRequest(window, bodyText, posText, negText, posFlags, negFlags, width, height)
 * A0/A1/A2/A3, D0/D1/D2/D3
 * Returns TRUE/FALSE in D0. */
static void intuition_AutoRequest(void)
{
    uint32_t win_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t body_ptr = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t pos_ptr  = m68k_get_reg(NULL, M68K_REG_A2);
    uint32_t neg_ptr  = m68k_get_reg(NULL, M68K_REG_A3);
    uint32_t pos_flags = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t neg_flags = m68k_get_reg(NULL, M68K_REG_D1);
    int16_t  width     = (int16_t)m68k_get_reg(NULL, M68K_REG_D2);
    int16_t  height    = (int16_t)m68k_get_reg(NULL, M68K_REG_D3);

    (void)pos_flags; (void)neg_flags;

    char body[256] = "";
    char pos[32]   = "OK";
    char neg[32]   = "Cancel";
    guest_itext_text(body, body_ptr, sizeof(body));
    if (pos_ptr) guest_itext_text(pos, pos_ptr, sizeof(pos));
    if (neg_ptr) guest_itext_text(neg, neg_ptr, sizeof(neg));

    const char *buttons[2];
    int num_buttons = 0;
    buttons[num_buttons++] = pos;
    if (neg_ptr) buttons[num_buttons++] = neg;

    uint32_t req_win = build_requester_internal(win_ptr, "Request", body, num_buttons, buttons,
                                                width, height);
    if (!req_win) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }

    int result = wait_requester_internal();
    free_requester_internal();
    m68k_set_reg(M68K_REG_D0, result ? 1 : 0);
}

/* BuildSysRequest(window, bodyText, posText, negText, flags, width, height)
 * A0/A1/A2/A3, D0/D1/D2
 * Returns requester Window* in D0. */
static void intuition_BuildSysRequest(void)
{
    uint32_t win_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t body_ptr = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t pos_ptr  = m68k_get_reg(NULL, M68K_REG_A2);
    uint32_t neg_ptr  = m68k_get_reg(NULL, M68K_REG_A3);
    uint32_t flags     = m68k_get_reg(NULL, M68K_REG_D0);
    int16_t  width     = (int16_t)m68k_get_reg(NULL, M68K_REG_D1);
    int16_t  height    = (int16_t)m68k_get_reg(NULL, M68K_REG_D2);

    (void)flags;

    char body[256] = "";
    char pos[32]   = "OK";
    char neg[32]   = "Cancel";
    guest_itext_text(body, body_ptr, sizeof(body));
    if (pos_ptr) guest_itext_text(pos, pos_ptr, sizeof(pos));
    if (neg_ptr) guest_itext_text(neg, neg_ptr, sizeof(neg));

    const char *buttons[2];
    int num_buttons = 0;
    buttons[num_buttons++] = pos;
    if (neg_ptr) buttons[num_buttons++] = neg;

    uint32_t req_win = build_requester_internal(win_ptr, "Request", body, num_buttons, buttons,
                                                width, height);
    m68k_set_reg(M68K_REG_D0, req_win);
}

/* FreeSysRequest(requesterWindow) — A0 = requester window */
static void intuition_FreeSysRequest(void)
{
    uint32_t req_win = m68k_get_reg(NULL, M68K_REG_A0);
    (void)req_win;
    free_requester_internal();
}

/* EasyRequestArgs(window, easyStruct, idcmpPtr, args)
 * A0/A1/A2/A3
 * Returns selected gadget index in D0 (0 for rightmost, 1..n-1 for others). */
static void intuition_EasyRequestArgs(void)
{
    uint32_t win_ptr  = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t easy_ptr = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t idcmp_ptr = m68k_get_reg(NULL, M68K_REG_A2);
    uint32_t args_ptr = m68k_get_reg(NULL, M68K_REG_A3);

    (void)idcmp_ptr; (void)args_ptr;

    char title[32]    = "Request";
    char body[256]    = "";
    char gad_fmt[128] = "OK";
    if (easy_ptr) {
        uint32_t t = mem_u32(easy_ptr + ES_OFF_TITLE);
        uint32_t b = mem_u32(easy_ptr + ES_OFF_TEXTFORMAT);
        uint32_t g = mem_u32(easy_ptr + ES_OFF_GADGETFORMAT);
        if (t) guest_str(title, t, sizeof(title));
        if (b) guest_str(body, b, sizeof(body));
        if (g) guest_str(gad_fmt, g, sizeof(gad_fmt));
    }

    const char *buttons[REQ_MAX_BUTTONS];
    int num_buttons = 0;
    char gad_buf[128];
    local_str_copy(gad_buf, gad_fmt, sizeof(gad_buf));

    char *p = gad_buf;
    while (num_buttons < REQ_MAX_BUTTONS && *p) {
        buttons[num_buttons++] = p;
        char *sep = local_strchr(p, '|');
        if (!sep) break;
        *sep = '\0';
        p = sep + 1;
    }
    if (num_buttons == 0) {
        buttons[0] = "OK";
        num_buttons = 1;
    }

    uint32_t req_win = build_requester_internal(win_ptr, title, body, num_buttons, buttons, 300, 120);
    if (!req_win) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }

    int result = wait_requester_internal();
    free_requester_internal();

    /* Amiga convention: rightmost gadget returns 0, others return 1..n-1. */
    int amiga_result = (num_buttons - 1) - result;
    m68k_set_reg(M68K_REG_D0, (uint32_t)amiga_result);
}

/* OpenScreen(newScreen) — A0 = NewScreen*; returns Screen* in D0 */
static void intuition_OpenScreen(void)
{
    uint32_t new_screen = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t screen = open_screen_internal(new_screen, 0);
    m68k_set_reg(M68K_REG_D0, screen);
}

/* OpenScreenTagList(newScreen, tagList) — A0/A1; returns Screen* in D0 */
static void intuition_OpenScreenTagList(void)
{
    uint32_t new_screen = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t tag_list   = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t screen = open_screen_internal(new_screen, tag_list);
    m68k_set_reg(M68K_REG_D0, screen);
}

/* CloseScreen(screen) — A0 = Screen*; returns BOOL in D0 */
static void intuition_CloseScreen(void)
{
    uint32_t screen_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    ScreenSlot *slot = find_screen_slot(screen_ptr);
    if (slot) {
        slot->active = 0;
        update_desktop_title();
    }
    m68k_set_reg(M68K_REG_D0, 1);  /* success */
}

/* MoveScreen(screen, dx, dy) — A0, D0/D1 */
static void intuition_MoveScreen(void)
{
    uint32_t screen_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    int16_t  dx         = (int16_t)m68k_get_reg(NULL, M68K_REG_D0);
    int16_t  dy         = (int16_t)m68k_get_reg(NULL, M68K_REG_D1);
    ScreenSlot *slot = find_screen_slot(screen_ptr);
    if (slot) {
        slot->left += dx;
        slot->top  += dy;
        mem_w16(screen_ptr + SCR_OFF_LEFTEDGE, slot->left);
        mem_w16(screen_ptr + SCR_OFF_TOPEDGE,  slot->top);
    }
}

/* ScreenToFront(screen) — A0 */
static void intuition_ScreenToFront(void)
{
    uint32_t screen_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    ScreenSlot *slot = find_screen_slot(screen_ptr);
    if (slot) {
        for (int i = 0; i < MAX_INTUITION_SCREENS; i++)
            g_intu_screens[i].is_front = 0;
        slot->is_front = 1;
        update_desktop_title();
    }
}

/* ScreenToBack(screen) — A0 */
static void intuition_ScreenToBack(void)
{
    uint32_t screen_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    ScreenSlot *slot = find_screen_slot(screen_ptr);
    if (slot) {
        slot->is_front = 0;
        /* Pick another active screen as front, if any */
        for (int i = 0; i < MAX_INTUITION_SCREENS; i++) {
            if (g_intu_screens[i].active && &g_intu_screens[i] != slot) {
                g_intu_screens[i].is_front = 1;
                break;
            }
        }
        update_desktop_title();
    }
}

/* ShowTitle(screen, showIt) — A0, D0 */
static void intuition_ShowTitle(void)
{
    uint32_t screen_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t show_it    = m68k_get_reg(NULL, M68K_REG_D0);
    ScreenSlot *slot = find_screen_slot(screen_ptr);
    if (slot) {
        slot->show_title = show_it ? 1 : 0;
        mem_w16(screen_ptr + SCR_OFF_FLAGS,
                (mem_u16(screen_ptr + SCR_OFF_FLAGS) & ~SHOWTITLE) |
                (slot->show_title ? SHOWTITLE : 0));
        update_desktop_title();
    }
}

/* SetMenuStrip(window, menu) — A0/A1 */
static void intuition_SetMenuStrip(void)
{
    uint32_t win_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t menu    = m68k_get_reg(NULL, M68K_REG_A1);
    if (win_ptr)
        mem_w32(win_ptr + WIN_OFF_MENUSTRIP, menu);
}

/* ClearMenuStrip(window) — A0 */
static void intuition_ClearMenuStrip(void)
{
    uint32_t win_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    if (win_ptr)
        mem_w32(win_ptr + WIN_OFF_MENUSTRIP, 0);
}

/* ResetMenuStrip(window, menu) — A0/A1 */
static void intuition_ResetMenuStrip(void)
{
    uint32_t win_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t menu    = m68k_get_reg(NULL, M68K_REG_A1);
    if (win_ptr)
        mem_w32(win_ptr + WIN_OFF_MENUSTRIP, menu);
}

/* ItemAddress(menuStrip, menuNumber) — A0, D0; returns MenuItem* in D0 */
static void intuition_ItemAddress(void)
{
    uint32_t menu_strip = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t menu_num   = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t result = 0;

    if (menu_strip && menu_num != MENUNULL) {
        int menu_idx = MENUNUM(menu_num);
        int item_idx = ITEMNUM(menu_num);
        int sub_idx  = SUBNUM(menu_num);

        uint32_t menu = menu_strip;
        for (int m = 0; menu && m < menu_idx; m++)
            menu = mem_u32(menu + MENU_OFF_NEXTMENU);

        if (menu) {
            uint32_t item = mem_u32(menu + MENU_OFF_FIRSTITEM);
            for (int i = 0; item && i < item_idx; i++)
                item = mem_u32(item + MENUITEM_OFF_NEXTITEM);

            if (item && sub_idx != NOSUB) {
                uint32_t sub = mem_u32(item + MENUITEM_OFF_SUBITEM);
                for (int s = 0; sub && s < sub_idx; s++)
                    sub = mem_u32(sub + MENUITEM_OFF_NEXTITEM);
                result = sub;
            } else {
                result = item;
            }
        }
    }

    m68k_set_reg(M68K_REG_D0, result);
}

/* LockPubScreen(name) — A0 = name; returns Screen* in D0 */
static void intuition_LockPubScreen(void)
{
    uint32_t name_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    char name[64] = "";
    if (name_ptr) guest_str(name, name_ptr, sizeof(name));

    uint32_t screen = 0;
    if (name[0]) {
        screen = find_pub_screen_by_name(name);
    } else {
        screen = get_default_pub_screen();
    }

    if (screen) {
        ScreenSlot *slot = find_screen_slot(screen);
        if (slot) slot->lock_count++;
    }

    m68k_set_reg(M68K_REG_D0, screen);
}

/* UnlockPubScreen(name, screen) — A0, A1 */
static void intuition_UnlockPubScreen(void)
{
    uint32_t name_ptr  = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t screen_ptr = m68k_get_reg(NULL, M68K_REG_A1);
    (void)name_ptr;
    ScreenSlot *slot = find_screen_slot(screen_ptr);
    if (slot && slot->lock_count > 0) slot->lock_count--;
}

/* LockPubScreenList() — returns List* in D0 */
static void intuition_LockPubScreenList(void)
{
    /* Allocate a minimal exec List; callers iterate with NextPubScreen(). */
    uint32_t list = intu_alloc(16);
    if (list) {
        mem_w32(list + 0, 0);  /* lh_Head */
        mem_w32(list + 4, 0);  /* lh_Tail */
        mem_w32(list + 8, 0);  /* lh_TailPred */
        mem_w16(list + 12, 0); /* lh_Type + pad */
    }
    m68k_set_reg(M68K_REG_D0, list);
}

/* UnlockPubScreenList() */
static void intuition_UnlockPubScreenList(void)
{
    /* no-op; list memory is not reclaimed by intu_alloc(). */
}

/* SetPointer(window, pointer, height, width, xOffset, yOffset)
 * A0, A1, D0, D1, D2, D3 */
static void intuition_SetPointer(void)
{
    uint32_t pointer = m68k_get_reg(NULL, M68K_REG_A1);
    int16_t  height  = (int16_t)m68k_get_reg(NULL, M68K_REG_D0);
    int16_t  width   = (int16_t)m68k_get_reg(NULL, M68K_REG_D1);

    if (width <= 0)  width = 16;
    if (width > 16)  width = 16;
    if (height <= 0) height = 16;
    if (height > 16) height = 16;

    uint8_t sprite[16 * 16];
    memset(sprite, 0, sizeof(sprite));

    if (pointer) {
        for (int row = 0; row < height; row++) {
            uint16_t p0 = mem_u16(pointer + row * 4);
            uint16_t p1 = mem_u16(pointer + row * 4 + 2);
            for (int col = 0; col < width; col++) {
                int bit = 15 - col;
                int v = 0;
                if (p0 & (1 << bit)) v |= 1;
                if (p1 & (1 << bit)) v |= 2;
                /* Map Amiga sprite colours to cursor palette:
                 * 0=transparent, 1=shadow, 2=body, 3=shadow */
                uint8_t px = 0;
                if (v == 1) px = 1;
                else if (v == 2) px = 2;
                else if (v == 3) px = 1;
                sprite[row * width + col] = px;
            }
        }
    }

    Cursor_SetCustomSprite(sprite, width, height);
}

/* ClearPointer(window) — A0 */
static void intuition_ClearPointer(void)
{
    (void)m68k_get_reg(NULL, M68K_REG_A0);
    Cursor_ClearCustomSprite();
}

/* SetWindowPointerA(window, tagList) — A0, A1 */
static void intuition_SetWindowPointerA(void)
{
    uint32_t tag_list = m68k_get_reg(NULL, M68K_REG_A1);
    int busy = 0;
    int got_busy = 0;

    if (tag_list) {
        uint32_t p = tag_list;
        while (p + 8 <= GUEST_RAM_SIZE) {
            uint32_t tag  = mem_u32(p);
            uint32_t data = mem_u32(p + 4);
            if (tag == TAG_DONE) break;
            if (tag == WA_BusyPointer) {
                got_busy = 1;
                busy = data ? 1 : 0;
            }
            p += 8;
        }
    }

    if (got_busy)
        Cursor_SetBusy(busy);
    else
        Cursor_ClearCustomSprite();
}

/* -------------------------------------------------------------------------
 * Preferences / defaults
 * ------------------------------------------------------------------------- */

static uint8_t g_intu_prefs[PREF_SIZE];
static uint8_t g_intu_def_prefs[PREF_SIZE];
static uint32_t g_gui_lock = 0xDEADBEEF;
static int      g_prefs_inited = 0;

static void write_host_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void write_host_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void init_prefs(void)
{
    if (g_prefs_inited) return;
    g_prefs_inited = 1;

    memset(g_intu_def_prefs, 0, PREF_SIZE);
    g_intu_def_prefs[PREF_OFF_FONTHEIGHT]    = 8;        /* Topaz 8 */
    g_intu_def_prefs[PREF_OFF_PRINTERPORT]   = 0;        /* Parallel */
    write_host_u16(&g_intu_def_prefs[PREF_OFF_BAUDRATE], 0x0005); /* BAUD_9600 */
    write_host_u16(&g_intu_def_prefs[PREF_OFF_POINTERTICKS], 60);
    write_host_u16(&g_intu_def_prefs[PREF_OFF_COLOR0], 0x0AAA); /* grey */
    write_host_u16(&g_intu_def_prefs[PREF_OFF_COLOR1], 0x0000); /* black */
    write_host_u16(&g_intu_def_prefs[PREF_OFF_COLOR2], 0x0FFF); /* white */
    write_host_u16(&g_intu_def_prefs[PREF_OFF_COLOR3], 0x00F0); /* blue */
    g_intu_def_prefs[PREF_OFF_VIEWXOFFSET]     = 0;
    g_intu_def_prefs[PREF_OFF_VIEWYOFFSET]     = 0;
    write_host_u16(&g_intu_def_prefs[PREF_OFF_VIEWINITX], 0);
    write_host_u16(&g_intu_def_prefs[PREF_OFF_VIEWINITY], 0);
    write_host_u32(&g_intu_def_prefs[PREF_OFF_ENABLECLI], 1);
    write_host_u16(&g_intu_def_prefs[PREF_OFF_WBWIDTH], 640);
    write_host_u16(&g_intu_def_prefs[PREF_OFF_WBHEIGHT], 200);
    g_intu_def_prefs[PREF_OFF_WBDEPTH]         = 2;

    memcpy(g_intu_prefs, g_intu_def_prefs, PREF_SIZE);
}

static void copy_prefs_to_guest(uint32_t dst, uint32_t size)
{
    init_prefs();
    if (!dst || size == 0) return;
    if (size > PREF_SIZE) size = PREF_SIZE;
    for (uint32_t i = 0; i < size; i++) {
        mem_w8(dst + i, g_intu_prefs[i]);
    }
}

/* GetDefPrefs(preferences, size) — A0, D0; returns buffer in D0 */
static void intuition_GetDefPrefs(void)
{
    uint32_t buf  = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t size = m68k_get_reg(NULL, M68K_REG_D0);
    init_prefs();
    if (buf && size) {
        if (size > PREF_SIZE) size = PREF_SIZE;
        for (uint32_t i = 0; i < size; i++)
            mem_w8(buf + i, g_intu_def_prefs[i]);
    }
    m68k_set_reg(M68K_REG_D0, buf);
}

/* GetPrefs(preferences, size) — A0, D0; returns buffer in D0 */
static void intuition_GetPrefs(void)
{
    uint32_t buf  = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t size = m68k_get_reg(NULL, M68K_REG_D0);
    copy_prefs_to_guest(buf, size);
    m68k_set_reg(M68K_REG_D0, buf);
}

/* SetPrefs(preferences, size, inform) — A0, D0, D1 */
static void intuition_SetPrefs(void)
{
    uint32_t buf  = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t size = m68k_get_reg(NULL, M68K_REG_D0);
    (void)m68k_get_reg(NULL, M68K_REG_D1);

    init_prefs();
    if (buf && size) {
        if (size > PREF_SIZE) size = PREF_SIZE;
        for (uint32_t i = 0; i < size; i++)
            g_intu_prefs[i] = mem_u8(buf + i);
    }
}

/* LockGUIPrefs(reserved) — D0; returns lock in D0 */
static void intuition_LockGUIPrefs(void)
{
    (void)m68k_get_reg(NULL, M68K_REG_D0);
    m68k_set_reg(M68K_REG_D0, g_gui_lock);
}

/* UnlockGUIPrefs(lock) — A0 */
static void intuition_UnlockGUIPrefs(void)
{
    (void)m68k_get_reg(NULL, M68K_REG_A0);
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
    intuition_AutoRequest,
    intuition_BuildSysRequest,
    intuition_FreeSysRequest,
    intuition_EasyRequestArgs,
    intuition_OpenScreen,
    intuition_CloseScreen,
    intuition_MoveScreen,
    intuition_ScreenToFront,
    intuition_ScreenToBack,
    intuition_ShowTitle,
    intuition_OpenScreenTagList,
    intuition_SetMenuStrip,
    intuition_ClearMenuStrip,
    intuition_ResetMenuStrip,
    intuition_ItemAddress,
    intuition_LockPubScreen,
    intuition_UnlockPubScreen,
    intuition_LockPubScreenList,
    intuition_UnlockPubScreenList,
    intuition_SetPointer,
    intuition_ClearPointer,
    intuition_SetWindowPointerA,
    intuition_GetDefPrefs,
    intuition_GetPrefs,
    intuition_SetPrefs,
    intuition_LockGUIPrefs,
    intuition_UnlockGUIPrefs,
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
