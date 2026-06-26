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
#include "irq/rtc.h"
#include "irq/ps2kbd.h"
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern volatile uint64_t g_pit_ticks;

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
extern uint32_t UAOS_InvokeM68kHook(uint32_t hook_ptr, uint32_t a0, uint32_t a1, uint32_t a2);

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
#define M68K_REG_A7  15
#define M68K_REG_SP  15

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
static inline int32_t mem_s32(uint32_t addr)
    { return (int32_t)mem_u32(addr); }
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

/* IntuiText structure offsets are defined in intuition_lib.h. */

/* BitMap structure offsets (used by SA_Interleaved and custom pointers). */
#define BM_OFF_BYTESPERROW 0
#define BM_OFF_ROWS        2
#define BM_OFF_FLAGS       4
#define BM_OFF_DEPTH       5
#define BM_OFF_PLANES      8

/* =========================================================================
 * Intuition heap allocator (dedicated region below stack)
 * ========================================================================= */

#define INTUITION_HEAP_BASE  0x1E0000
#define INTUITION_HEAP_SIZE  0x010000

/* Each allocation is preceded by an 8-byte header:
 *   +0: total block size (including header)
 *   +4: 0 when in use, otherwise next free block address
 * The data pointer returned to callers is block + 8. */
static uint32_t intu_heap_top = INTUITION_HEAP_BASE;
static uint32_t intu_free_list = 0;

uint32_t intu_alloc(uint32_t size)
{
    size = (size + 7) & ~7u;
    if (size < 8) size = 8;
    uint32_t total = size + 8;

    /* Search the free list for a block that fits. */
    uint32_t prev = 0;
    uint32_t cur = intu_free_list;
    while (cur) {
        uint32_t bsize = mem_u32(cur);
        uint32_t next = mem_u32(cur + 4);
        if (bsize >= total) {
            if (prev) mem_w32(prev + 4, next);
            else      intu_free_list = next;

            /* Split if the remainder would be useful. */
            if (bsize >= total + 16) {
                uint32_t rem = cur + total;
                uint32_t rem_size = bsize - total;
                mem_w32(rem, rem_size);
                mem_w32(rem + 4, intu_free_list);
                intu_free_list = rem;
                bsize = total;
            }
            mem_w32(cur, bsize);
            mem_w32(cur + 4, 0);
            for (uint32_t i = 8; i < bsize; i++) g_ram[cur + i] = 0;
            return cur + 8;
        }
        prev = cur;
        cur = next;
    }

    if (intu_heap_top + total > INTUITION_HEAP_BASE + INTUITION_HEAP_SIZE)
        return 0;
    uint32_t block = intu_heap_top;
    intu_heap_top += total;
    mem_w32(block, total);
    mem_w32(block + 4, 0);
    for (uint32_t i = 8; i < total; i++) g_ram[block + i] = 0;
    return block + 8;
}

void intu_free(uint32_t user_addr)
{
    if (!user_addr) return;
    if (user_addr < INTUITION_HEAP_BASE + 8 ||
        user_addr >= INTUITION_HEAP_BASE + INTUITION_HEAP_SIZE)
        return;
    uint32_t block = user_addr - 8;
    uint32_t size = mem_u32(block);
    if (size < 16 || size > INTUITION_HEAP_SIZE) return;

    /* If this is the topmost allocated block, lower the heap top instead
     * of adding to the free list. */
    if (block + size == intu_heap_top) {
        intu_heap_top = block;
        return;
    }

    /* Otherwise, add it to the free list. */
    mem_w32(block + 4, intu_free_list);
    intu_free_list = block;
}

static void init_guest_rastport(uint32_t rp, uint32_t win_ptr)
{
    for (int i = 0; i < RP_SIZE_MIN; i++) g_ram[rp + i] = 0;
    g_ram[rp + RP_OFF_FGPEN]    = 1;   /* white */
    g_ram[rp + RP_OFF_BGPEN]    = 0;   /* black */
    g_ram[rp + RP_OFF_DRAWMODE] = JAM2;
    mem_w32(rp + RP_OFF_LAYER, win_ptr);
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
    /* Tag storage for SetWindowAttrsA / GetWindowAttrsA */
    uint32_t pub_screen;
    uint32_t super_bitmap;
    uint32_t zoom;
    uint8_t  zoomed;
    uint32_t backfill;
    uint32_t colors;
    uint32_t checkmark;
    uint32_t window_name;
    uint32_t error_code_ptr;
    uint32_t help_group;
    uint32_t help_group_window;
    uint16_t mouse_queue;
    uint16_t rpt_queue;
    uint8_t  new_look_menus;
    uint8_t  tablet_messages;
    uint8_t  auto_adjust;
    uint8_t  menu_help;
    uint32_t screen_title;
    uint32_t pub_screen_name;
    uint8_t  pub_screen_fallback;
    uint16_t inner_width;
    uint16_t inner_height;
    uint32_t amiga_key;
    uint8_t  notify_depth;
    uint32_t pointer_delay;     /* WA_PointerDelay: stored but ignored */
    uint8_t  help_enabled;      /* HC_GADGETHELP state for this window */

    /* Refresh / damage state */
    uint8_t  refreshing;        /* inside BeginRefresh ... EndRefresh pair */
    uint8_t  simple_refresh;    /* 1 = SimpleRefresh, 0 = SmartRefresh */
    int16_t  damage_x, damage_y, damage_w, damage_h;

    /* GimmeZeroZero border offsets */
    uint8_t  gimme_zero_zero;
    int16_t  border_left, border_top, border_right, border_bottom;

    /* Active string gadget for host-side keyboard input */
    uint32_t active_string_gad;
    uint16_t active_string_cursor;
    uint16_t active_string_sel_start;
    uint16_t active_string_sel_end;

    /* Drag state for proportional gadget knobs / listview scrollbars */
    uint32_t drag_gad;
    uint8_t  drag_kind;     /* 1 = prop knob, 2 = listview scrollbar */
    int16_t  drag_start_x, drag_start_y;
    uint16_t drag_start_hpot, drag_start_vpot;
    int16_t  drag_start_top;
} IntuitionSlot;

static IntuitionSlot g_intu_wins[MAX_INTUITION_WINS];
static uint32_t      g_intu_view = 0;      /* single guest View for ViewAddress() */

/* Pending WA_PointerDelay pointer change.  The cursor is global, so only one
 * delayed change can be outstanding at a time. */
static uint32_t g_pending_pointer = 0;
static uint32_t g_pending_pointer_xoff = 0;
static uint32_t g_pending_pointer_yoff = 0;
static uint64_t g_pending_pointer_target = 0;
static uint8_t  g_pending_pointer_active = 0;
static uint8_t  g_pending_pointer_busy = 0;
static uint8_t  g_pending_pointer_busy_state = 0;

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
        slot->pub_screen = 0;
        slot->super_bitmap = 0;
        slot->zoom = 0;
        slot->zoomed = 0;
        slot->backfill = 0;
        slot->colors = 0;
        slot->checkmark = 0;
        slot->window_name = 0;
        slot->error_code_ptr = 0;
        slot->help_group = 0;
        slot->help_group_window = 0;
        slot->mouse_queue = 0;
        slot->rpt_queue = 0;
        slot->new_look_menus = 0;
        slot->tablet_messages = 0;
        slot->auto_adjust = 0;
        slot->menu_help = 0;
        slot->screen_title = 0;
        slot->pub_screen_name = 0;
        slot->pub_screen_fallback = 0;
        slot->inner_width = 0;
        slot->inner_height = 0;
        slot->amiga_key = 0;
        slot->notify_depth = 0;
        slot->pointer_delay = 0;
        slot->help_enabled = 0;
        slot->refreshing = 0;
        slot->simple_refresh = 0;
        slot->damage_x = 0;
        slot->damage_y = 0;
        slot->damage_w = 0;
        slot->damage_h = 0;
        slot->gimme_zero_zero = 0;
        slot->border_left = 0;
        slot->border_top = 0;
        slot->border_right = 0;
        slot->border_bottom = 0;
        slot->active_string_gad = 0;
        slot->active_string_cursor = 0;
        slot->active_string_sel_start = 0;
        slot->active_string_sel_end = 0;
        slot->drag_gad = 0;
        slot->drag_kind = 0;
        slot->drag_start_x = 0;
        slot->drag_start_y = 0;
        slot->drag_start_hpot = 0;
        slot->drag_start_vpot = 0;
        slot->drag_start_top = 0;
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

static void free_port_messages(uint32_t port)
{
    if (!port) return;
    uint32_t list = port + MP_OFF_MSGLIST;
    while (1) {
        uint32_t head = mem_u32(list + LH_OFF_HEAD);
        uint32_t tail = list + LH_OFF_TAIL;
        if (head == tail) break;
        uint32_t msg = guest_list_remove_head(list);
        if (!msg) break;
        intu_free(msg);
    }
}

static void free_window_idcmp(uint32_t win_ptr)
{
    if (!win_ptr) return;
    uint32_t user_port = mem_u32(win_ptr + WIN_OFF_USERPORT);
    uint32_t window_port = mem_u32(win_ptr + WIN_OFF_WINDOWPORT);
    free_port_messages(user_port);
    free_port_messages(window_port);
    intu_free(user_port);
    intu_free(window_port);
    mem_w32(win_ptr + WIN_OFF_USERPORT, 0);
    mem_w32(win_ptr + WIN_OFF_WINDOWPORT, 0);
}

static void free_gadget_list(uint32_t gad)
{
    while (gad) {
        uint32_t next = mem_u32(gad + GAD_OFF_NEXTGADGET);
        intu_free(gad);
        gad = next;
    }
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

static int count_port_messages_by_class(uint32_t port, uint32_t class_mask)
{
    if (!port) return 0;
    uint32_t list = port + MP_OFF_MSGLIST;
    int count = 0;
    uint32_t node = mem_u32(list + LH_OFF_HEAD);
    uint32_t tail = list + LH_OFF_TAIL;
    while (node && node != tail) {
        uint32_t node_class = mem_u32(node + IM_OFF_CLASS);
        if (node_class & class_mask) count++;
        node = mem_u32(node + MSG_OFF_LN_SUCC);
    }
    return count;
}

static void remove_oldest_port_message_by_class(uint32_t port, uint32_t class_mask)
{
    if (!port) return;
    uint32_t list = port + MP_OFF_MSGLIST;
    uint32_t node = mem_u32(list + LH_OFF_HEAD);
    uint32_t tail = list + LH_OFF_TAIL;
    while (node && node != tail) {
        uint32_t node_class = mem_u32(node + IM_OFF_CLASS);
        if (node_class & class_mask) {
            uint32_t pred = mem_u32(node + MSG_OFF_LN_PRED);
            uint32_t succ = mem_u32(node + MSG_OFF_LN_SUCC);
            mem_w32(pred + MSG_OFF_LN_SUCC, succ);
            mem_w32(succ + MSG_OFF_LN_PRED, pred);
            intu_free(node);
            return;
        }
        node = mem_u32(node + MSG_OFF_LN_SUCC);
    }
}

static void post_intui_message(uint32_t win_ptr, uint32_t class, uint16_t code, uint16_t qualifier, int16_t mouse_x, int16_t mouse_y, uint32_t iaddress)
{
    if (!win_ptr) return;
    uint32_t idcmp = mem_u32(win_ptr + WIN_OFF_IDCMPFLAGS);
    if (!idcmp || !(idcmp & class)) return;

    uint32_t user_port = mem_u32(win_ptr + WIN_OFF_USERPORT);
    if (!user_port) return;

    /* Enforce WA_MouseQueue / WA_RptQueue limits.  Mouse moves are limited by
     * mouse_queue; mouse button events are limited by rpt_queue. */
    IntuitionSlot *slot = find_slot_by_guest(win_ptr);
    if (slot) {
        if (class == IDCMP_MOUSEMOVE && slot->mouse_queue > 0) {
            while (count_port_messages_by_class(user_port, IDCMP_MOUSEMOVE) >= (int)slot->mouse_queue)
                remove_oldest_port_message_by_class(user_port, IDCMP_MOUSEMOVE);
        } else if (class == IDCMP_MOUSEBUTTONS && slot->rpt_queue > 0) {
            while (count_port_messages_by_class(user_port, IDCMP_MOUSEBUTTONS) >= (int)slot->rpt_queue)
                remove_oldest_port_message_by_class(user_port, IDCMP_MOUSEBUTTONS);
        }
    }

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

/* Convert window-relative mouse coordinates into content-relative
 * coordinates for GimmeZeroZero windows.  System gadgets remain in the
 * border area and are handled separately by the WM. */
static void gzz_mouse_to_content(IntuitionSlot *slot, int *x, int *y)
{
    if (slot && slot->gimme_zero_zero) {
        *x -= slot->border_left;
        *y -= slot->border_top;
    }
}

/* Determine the gadget type id and whether it behaves as a checkbox or
 * radio button. */
static int gad_type_id(uint32_t gad)
{
    return (int)(mem_u16(gad + GAD_OFF_GADGETTYPE) & 0x000F);
}

static int gad_is_checkbox_or_radio(uint32_t gad)
{
    if (gad_type_id(gad) != GTYP_BOOLGADGET) return 0;
    uint16_t activation = mem_u16(gad + GAD_OFF_ACTIVATION);
    if (activation & GACT_TOGGLESELECT) return 1;
    if (mem_u32(gad + GAD_OFF_MUTUALEXCLUDE)) return 1;
    return 0;
}

static int gad_is_listview(uint32_t gad)
{
    return gad_type_id(gad) == GTYP_LISTVIEW;
}

static int gad_is_string_gadget(uint32_t gad)
{
    int type = gad_type_id(gad);
    return type == GTYP_STRGADGET || type == GTYP_INTGADGET;
}

/* For a radio-button gadget, clear GFLG_SELECTED on all other gadgets that
 * share any mutual-exclude bit. */
static void radio_clear_others(uint32_t win_ptr, uint32_t sel_gad)
{
    uint32_t mutual = mem_u32(sel_gad + GAD_OFF_MUTUALEXCLUDE);
    if (!mutual) return;
    uint32_t gad = mem_u32(win_ptr + WIN_OFF_FIRSTGADGET);
    while (gad) {
        if (gad != sel_gad) {
            uint32_t m = mem_u32(gad + GAD_OFF_MUTUALEXCLUDE);
            if (m & mutual) {
                uint16_t flags = mem_u16(gad + GAD_OFF_FLAGS);
                if (flags & GFLG_SELECTED) {
                    flags &= ~GFLG_SELECTED;
                    mem_w16(gad + GAD_OFF_FLAGS, flags);
                }
            }
        }
        gad = mem_u32(gad + GAD_OFF_NEXTGADGET);
    }
}

/* Hit-test a listview: return the clicked item index or -1. */
static int listview_hit(uint32_t gad, int mx, int my)
{
    uint32_t lv = mem_u32(gad + GAD_OFF_SPECIALINFO);
    if (!lv) return -1;
    int top = (int)mem_u32(lv + LV_OFF_TOP);
    int row_h = 16;
    int row = (my - 2) / row_h + top;
    int count = (int)mem_u32(lv + LV_OFF_COUNT);
    if (row < 0 || row >= count) return -1;
    return row;
}

/* Hit-test the listview scrollbar thumb. Returns 1 if (mx,my) is inside the
 * thumb, 0 otherwise. */
static int listview_scrollbar_hit(uint32_t gad, int mx, int my)
{
    uint32_t lv = mem_u32(gad + GAD_OFF_SPECIALINFO);
    if (!lv) return 0;
    int count = (int)mem_u32(lv + LV_OFF_COUNT);
    int visible = (int)mem_u32(lv + LV_OFF_VISIBLE);
    if (visible <= 0) visible = 4;
    if (count <= visible) return 0;

    int16_t w = mem_s16(gad + GAD_OFF_WIDTH);
    int16_t h = mem_s16(gad + GAD_OFF_HEIGHT);
    int scroll_w = 12;
    int sx = w - scroll_w - 1;
    if (mx < sx || mx >= w - 1) return 0;
    if (my < 1 || my >= h - 1) return 0;

    int top = (int)mem_u32(lv + LV_OFF_TOP);
    int sh = (h - 4) * visible / count;
    if (sh < 4) sh = 4;
    int sy = 2 + (h - 4 - sh) * top / (count - visible);
    return (my >= sy && my < sy + sh);
}

/* Hit-test a proportional gadget knob. Returns 1 if the click is inside the
 * draggable knob area, 0 otherwise. */
static int prop_knob_hit(uint32_t gad, int mx, int my)
{
    uint32_t prop = mem_u32(gad + GAD_OFF_SPECIALINFO);
    if (!prop) return 0;
    int16_t w = mem_s16(gad + GAD_OFF_WIDTH);
    int16_t h = mem_s16(gad + GAD_OFF_HEIGHT);
    uint16_t hpot = mem_u16(prop + PROP_OFF_HORIZPOT);
    uint16_t vpot = mem_u16(prop + PROP_OFF_VERTPOT);
    uint16_t hbody = mem_u16(prop + PROP_OFF_HORIZBODY);
    uint16_t vbody = mem_u16(prop + PROP_OFF_VERTBODY);

    if (w > h) {
        int kw = (int)(w * hbody / 0xFFFF);
        if (kw < 4) kw = 4;
        int kx = (int)(w * hpot / 0xFFFF) - kw / 2;
        if (kx < 0) kx = 0;
        if (kx + kw > w) kx = w - kw;
        return (my >= 2 && my < h - 2 && mx >= kx && mx < kx + kw);
    } else {
        int kh = (int)(h * vbody / 0xFFFF);
        if (kh < 4) kh = 4;
        int ky = (int)(h * vpot / 0xFFFF) - kh / 2;
        if (ky < 0) ky = 0;
        if (ky + kh > h) ky = h - kh;
        return (mx >= 2 && mx < w - 2 && my >= ky && my < ky + kh);
    }
}

/* Update a proportional gadget's pot from the current mouse position during
 * a drag.  Returns 1 if the pot changed, 0 otherwise. */
static int prop_update_from_drag(uint32_t gad, int mx, int my)
{
    uint32_t prop = mem_u32(gad + GAD_OFF_SPECIALINFO);
    if (!prop) return 0;
    int16_t w = mem_s16(gad + GAD_OFF_WIDTH);
    int16_t h = mem_s16(gad + GAD_OFF_HEIGHT);
    uint16_t hbody = mem_u16(prop + PROP_OFF_HORIZBODY);
    uint16_t vbody = mem_u16(prop + PROP_OFF_VERTBODY);
    int changed = 0;

    if (w > h) {
        int kw = (int)(w * hbody / 0xFFFF);
        if (kw < 4) kw = 4;
        long num = (long)(mx - kw / 2) * 0xFFFF;
        long den = (long)(w - kw);
        uint16_t hpot;
        if (den <= 0) hpot = 0;
        else if (num <= 0) hpot = 0;
        else if (num >= den * 0xFFFF) hpot = 0xFFFF;
        else hpot = (uint16_t)(num / den);
        if (hpot != mem_u16(prop + PROP_OFF_HORIZPOT)) {
            mem_w16(prop + PROP_OFF_HORIZPOT, hpot);
            changed = 1;
        }
    } else {
        int kh = (int)(h * vbody / 0xFFFF);
        if (kh < 4) kh = 4;
        long num = (long)(my - kh / 2) * 0xFFFF;
        long den = (long)(h - kh);
        uint16_t vpot;
        if (den <= 0) vpot = 0;
        else if (num <= 0) vpot = 0;
        else if (num >= den * 0xFFFF) vpot = 0xFFFF;
        else vpot = (uint16_t)(num / den);
        if (vpot != mem_u16(prop + PROP_OFF_VERTPOT)) {
            mem_w16(prop + PROP_OFF_VERTPOT, vpot);
            changed = 1;
        }
    }
    return changed;
}

/* Update a listview's top index from the current mouse position during a
 * scrollbar thumb drag.  Returns 1 if top changed, 0 otherwise. */
static int listview_update_scroll_from_drag(uint32_t gad, int my)
{
    uint32_t lv = mem_u32(gad + GAD_OFF_SPECIALINFO);
    if (!lv) return 0;
    int16_t h = mem_s16(gad + GAD_OFF_HEIGHT);
    int count = (int)mem_u32(lv + LV_OFF_COUNT);
    int visible = (int)mem_u32(lv + LV_OFF_VISIBLE);
    if (visible <= 0) visible = 4;
    if (count <= visible) return 0;

    int track_h = h - 4;
    int sh = track_h * visible / count;
    if (sh < 4) sh = 4;
    int usable_h = track_h - sh;
    if (usable_h <= 0) return 0;

    long num = (long)(my - 2 - sh / 2) * (count - visible);
    int top = (int)(num / usable_h);
    if (top < 0) top = 0;
    if (top > count - visible) top = count - visible;
    if (top != (int)mem_u32(lv + LV_OFF_TOP)) {
        mem_w32(lv + LV_OFF_TOP, (uint32_t)top);
        return 1;
    }
    return 0;
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

/* Clamp a window's left/top so it fits inside its screen (or the framebuffer
 * if no screen is supplied).  Used for WA_AutoAdjust. */
static void auto_adjust_window_geometry(int16_t *left, int16_t *top, int16_t width, int16_t height, uint32_t wscreen)
{
    int screen_left = 0, screen_top = 0;
    int screen_width = (int)g_fb.width;
    int screen_height = (int)g_fb.height;

    if (wscreen && wscreen + SCR_OFF_HEIGHT + 2 <= GUEST_RAM_SIZE) {
        screen_left  = mem_s16(wscreen + SCR_OFF_LEFTEDGE);
        screen_top   = mem_s16(wscreen + SCR_OFF_TOPEDGE);
        screen_width = mem_s16(wscreen + SCR_OFF_WIDTH);
        screen_height = mem_s16(wscreen + SCR_OFF_HEIGHT);
    }

    if (width > screen_width) width = (int16_t)screen_width;
    if (height > screen_height) height = (int16_t)screen_height;

    if (*left < screen_left) *left = (int16_t)screen_left;
    if (*top < screen_top) *top = (int16_t)screen_top;
    if (*left + width > screen_left + screen_width)
        *left = (int16_t)(screen_left + screen_width - width);
    if (*top + height > screen_top + screen_height)
        *top = (int16_t)(screen_top + screen_height - height);

    if (*left < screen_left) *left = (int16_t)screen_left;
    if (*top < screen_top) *top = (int16_t)screen_top;
}

/* Forward declaration for custom gadget hit-testing */
static uint32_t gadget_at(uint32_t win_ptr, int mx, int my);

/* Forward declarations for string gadget helpers */
static uint32_t string_gadget_si(uint32_t gad);
static int string_gadget_handle_key(uint32_t gad, uint16_t *cursor,
                                    uint16_t *sel_start, uint16_t *sel_end, char c);
static int string_gadget_cursor_from_click(uint32_t gad, int relx);
static void deactivate_string_gadget(IntuitionSlot *slot);
static void post_help_message(IntuitionSlot *slot, uint32_t win_ptr);
static void int_gadget_commit(uint32_t gad);

/* Toggle the window geometry using the WA_Zoom array stored in the slot.
 * The array is 8 int16_t values: normal left/top/width/height followed by
 * zoomed left/top/width/height. */
static void apply_window_zoom(IntuitionSlot *slot, int wm_handle, uint32_t win_ptr)
{
    if (!slot || !slot->zoom || wm_handle < 0) return;

    int16_t z[8];
    for (int i = 0; i < 8; i++)
        z[i] = mem_s16(slot->zoom + i * 2);

    int idx = slot->zoomed ? 0 : 4;
    int16_t nx = z[idx + 0];
    int16_t ny = z[idx + 1];
    int16_t nw = z[idx + 2];
    int16_t nh = z[idx + 3];

    if (nw <= 0 || nh <= 0) return;

    slot->zoomed = slot->zoomed ? 0 : 1;
    WM_SetWindowGeometry(wm_handle, nx, ny, nw, nh);
    WM_SetWindowZoomed(wm_handle, slot->zoomed);

    mem_w16(win_ptr + WIN_OFF_LEFTEDGE, nx);
    mem_w16(win_ptr + WIN_OFF_TOPEDGE, ny);
    mem_w16(win_ptr + WIN_OFF_WIDTH, nw);
    mem_w16(win_ptr + WIN_OFF_HEIGHT, nh);

    uint32_t idcmp = mem_u32(win_ptr + WIN_OFF_IDCMPFLAGS);
    if (idcmp & IDCMP_NEWSIZE)
        post_intui_message(win_ptr, IDCMP_NEWSIZE, 0, 0, 0, 0, 0);
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
    IntuitionSlot *slot = get_slot_from_handle(wm_handle);

    switch (event_type) {
        case WM_EVT_CLOSE_REQUEST:
            if (idcmp & IDCMP_CLOSEWINDOW) {
                post_intui_message(win_ptr, IDCMP_CLOSEWINDOW, 0, 0, 0, 0, 0);
                return 0; /* veto WM close; guest will call CloseWindow */
            }
            return 1;

        case WM_EVT_GADGET_DOWN: {
            if (p1 == WM_GADGET_ZOOM) {
                apply_window_zoom(slot, wm_handle, win_ptr);
                return 0;
            }
            uint32_t gad = slot ? gadget_by_id(slot, (uint16_t)p1) : 0;
            if (idcmp & IDCMP_GADGETDOWN)
                post_intui_message(win_ptr, IDCMP_GADGETDOWN, 0, 0, 0, 0, gad);
            return 0;
        }

        case WM_EVT_GADGET_UP: {
            uint32_t gad = slot ? gadget_by_id(slot, (uint16_t)p1) : 0;
            if (idcmp & IDCMP_GADGETUP)
                post_intui_message(win_ptr, IDCMP_GADGETUP, 0, 0, 0, 0, gad);
            return 0;
        }

        case WM_EVT_MOUSE_DOWN: {
            int relx = p2 - wx;
            int rely = p3 - wy;
            gzz_mouse_to_content(slot, &relx, &rely);
            uint32_t gad = gadget_at(win_ptr, relx, rely);
            if (gad && !(mem_u16(gad + GAD_OFF_FLAGS) & GFLG_DISABLED)) {
                if (gad_is_listview(gad)) {
                    int gx = relx - mem_s16(gad + GAD_OFF_LEFTEDGE);
                    int gy = rely - mem_s16(gad + GAD_OFF_TOPEDGE);
                    if (listview_scrollbar_hit(gad, gx, gy)) {
                        if (slot) {
                            slot->drag_gad = gad;
                            slot->drag_kind = 2;
                            slot->drag_start_y = (int16_t)gy;
                            slot->drag_start_top = (int16_t)mem_u32(
                                mem_u32(gad + GAD_OFF_SPECIALINFO) + LV_OFF_TOP);
                        }
                    } else {
                        int item = listview_hit(gad, gx, gy);
                        if (item >= 0) {
                            uint32_t lv = mem_u32(gad + GAD_OFF_SPECIALINFO);
                            if (lv) {
                                int multi = (int)mem_u32(lv + LV_OFF_MULTI_SELECT);
                                uint32_t mask = mem_u32(lv + LV_OFF_SELECTED_MASK);
                                if (multi) {
                                    uint32_t bit = (uint32_t)(1U << item);
                                    if (g_kbd_mods.shift || g_kbd_mods.ctrl) {
                                        /* Toggle selection */
                                        if (mask & bit) mask &= ~bit;
                                        else mask |= bit;
                                    } else {
                                        /* Single-select only if not already selected */
                                        if (mask == bit) {
                                            /* keep it selected */
                                        } else {
                                            mask = bit;
                                        }
                                    }
                                    mem_w32(lv + LV_OFF_SELECTED_MASK, mask);
                                }
                                mem_w32(lv + LV_OFF_SELECTED, (uint32_t)item);
                            }
                        }
                    }
                } else if (gad_is_string_gadget(gad)) {
                    if (slot) {
                        int new_pos = string_gadget_cursor_from_click(
                            gad, relx - mem_s16(gad + GAD_OFF_LEFTEDGE));
                        if (slot->active_string_gad == gad && g_kbd_mods.shift) {
                            slot->active_string_sel_end = (uint16_t)new_pos;
                            slot->active_string_cursor = (uint16_t)new_pos;
                        } else {
                            slot->active_string_gad = gad;
                            slot->active_string_cursor = (uint16_t)new_pos;
                            slot->active_string_sel_start = (uint16_t)new_pos;
                            slot->active_string_sel_end = (uint16_t)new_pos;
                        }
                    }
                } else if (gad_type_id(gad) == GTYP_PROPGADGET) {
                    uint32_t prop = mem_u32(gad + GAD_OFF_SPECIALINFO);
                    if (prop) {
                        int gx = relx - mem_s16(gad + GAD_OFF_LEFTEDGE);
                        int gy = rely - mem_s16(gad + GAD_OFF_TOPEDGE);
                        if (prop_knob_hit(gad, gx, gy)) {
                            if (slot) {
                                slot->drag_gad = gad;
                                slot->drag_kind = 1;
                                slot->drag_start_x = (int16_t)gx;
                                slot->drag_start_y = (int16_t)gy;
                                slot->drag_start_hpot = mem_u16(prop + PROP_OFF_HORIZPOT);
                                slot->drag_start_vpot = mem_u16(prop + PROP_OFF_VERTPOT);
                            }
                        } else {
                            /* Click on the track: jump the knob to the click position. */
                            if (prop_update_from_drag(gad, gx, gy))
                                WM_Redraw();
                        }
                    }
                } else if (!gad_is_checkbox_or_radio(gad)) {
                    uint16_t flags = mem_u16(gad + GAD_OFF_FLAGS);
                    flags |= GFLG_SELECTED;
                    mem_w16(gad + GAD_OFF_FLAGS, flags);
                }
                if (idcmp & IDCMP_GADGETDOWN)
                    post_intui_message(win_ptr, IDCMP_GADGETDOWN, 0, 0, 0, 0, gad);
                WM_Redraw();
            } else if (slot && slot->active_string_gad) {
                deactivate_string_gadget(slot);
                WM_Redraw();
            } else if (idcmp & IDCMP_MOUSEBUTTONS) {
                post_intui_message(win_ptr, IDCMP_MOUSEBUTTONS, (uint16_t)p1, 0,
                                   (int16_t)relx, (int16_t)rely, 0);
            }
            return 0;
        }

        case WM_EVT_MOUSE_UP: {
            int relx = p2 - wx;
            int rely = p3 - wy;
            gzz_mouse_to_content(slot, &relx, &rely);

            if (slot && slot->drag_gad) {
                uint32_t drag_gad = slot->drag_gad;
                slot->drag_gad = 0;
                slot->drag_kind = 0;
                if (idcmp & IDCMP_GADGETUP)
                    post_intui_message(win_ptr, IDCMP_GADGETUP, 0, 0, 0, 0, drag_gad);
                WM_Redraw();
                return 0;
            }

            uint32_t gad = mem_u32(win_ptr + WIN_OFF_FIRSTGADGET);
            uint32_t hit_gad = gadget_at(win_ptr, relx, rely);
            int redraw = 0;

            if (hit_gad && gad_is_checkbox_or_radio(hit_gad) &&
                !(mem_u16(hit_gad + GAD_OFF_FLAGS) & GFLG_DISABLED)) {
                uint16_t flags = mem_u16(hit_gad + GAD_OFF_FLAGS);
                uint32_t mutual = mem_u32(hit_gad + GAD_OFF_MUTUALEXCLUDE);
                if (mutual) {
                    /* Radio button: select this one and clear the group. */
                    if (!(flags & GFLG_SELECTED)) {
                        flags |= GFLG_SELECTED;
                        mem_w16(hit_gad + GAD_OFF_FLAGS, flags);
                        radio_clear_others(win_ptr, hit_gad);
                        redraw = 1;
                    }
                } else {
                    /* Checkbox: toggle the selected state. */
                    flags ^= GFLG_SELECTED;
                    mem_w16(hit_gad + GAD_OFF_FLAGS, flags);
                    redraw = 1;
                }
                if (idcmp & IDCMP_GADGETUP)
                    post_intui_message(win_ptr, IDCMP_GADGETUP, 0, 0, 0, 0, hit_gad);
            }

            while (gad) {
                uint16_t flags = mem_u16(gad + GAD_OFF_FLAGS);
                if (flags & GFLG_SELECTED) {
                    flags &= ~GFLG_SELECTED;
                    mem_w16(gad + GAD_OFF_FLAGS, flags);
                    redraw = 1;
                    if (hit_gad == gad && (idcmp & IDCMP_GADGETUP))
                        post_intui_message(win_ptr, IDCMP_GADGETUP, 0, 0, 0, 0, gad);
                }
                gad = mem_u32(gad + GAD_OFF_NEXTGADGET);
            }
            if (!hit_gad && (idcmp & IDCMP_MOUSEBUTTONS))
                post_intui_message(win_ptr, IDCMP_MOUSEBUTTONS, (uint16_t)(0x100 + p1), 0,
                                   (int16_t)relx, (int16_t)rely, 0);
            if (redraw) WM_Redraw();
            return 0;
        }

        case WM_EVT_MOUSE_MOVE: {
            int relx = p1 - wx;
            int rely = p2 - wy;
            gzz_mouse_to_content(slot, &relx, &rely);
            if (slot && slot->drag_gad) {
                uint32_t gad = slot->drag_gad;
                int gx = relx - mem_s16(gad + GAD_OFF_LEFTEDGE);
                int gy = rely - mem_s16(gad + GAD_OFF_TOPEDGE);
                int changed = 0;
                if (slot->drag_kind == 1 && gad_type_id(gad) == GTYP_PROPGADGET)
                    changed = prop_update_from_drag(gad, gx, gy);
                else if (slot->drag_kind == 2 && gad_is_listview(gad))
                    changed = listview_update_scroll_from_drag(gad, gy);
                if (changed) WM_Redraw();
                return 0;
            }
            if (idcmp & IDCMP_MOUSEMOVE)
                post_intui_message(win_ptr, IDCMP_MOUSEMOVE, 0, 0,
                                   (int16_t)relx, (int16_t)rely, 0);
            return 0;
        }

        case WM_EVT_KEY:
            if (slot && slot->active_string_gad) {
                uint32_t gad = slot->active_string_gad;
                uint32_t si = string_gadget_si(gad);
                if (si) {
                    char c = (char)(unsigned char)p1;
                    if (c == '\n' || c == '\r' || c == '\t') {
                        if (gad_type_id(gad) == GTYP_INTGADGET)
                            int_gadget_commit(gad);
                        deactivate_string_gadget(slot);
                        if (idcmp & IDCMP_GADGETUP)
                            post_intui_message(win_ptr, IDCMP_GADGETUP, 0, 0, 0, 0, gad);
                        WM_Redraw();
                    } else if (string_gadget_handle_key(gad, &slot->active_string_cursor,
                                                         &slot->active_string_sel_start,
                                                         &slot->active_string_sel_end, c)) {
                        WM_Redraw();
                    }
                    return 0;
                }
            }
            /* 0x1C is the F1 / Help key mapped by the PS/2 keyboard driver.
             * Route it to the focused window or its WA_HelpGroupWindow. */
            if ((unsigned char)p1 == 0x1C) {
                if (idcmp & IDCMP_HELP) {
                    post_help_message(slot, win_ptr);
                }
                return 0;
            }
            if (idcmp & IDCMP_RAWKEY)
                post_intui_message(win_ptr, IDCMP_RAWKEY, (uint16_t)p1, 0, 0, 0, 0);
            if (idcmp & IDCMP_VANILLAKEY)
                post_intui_message(win_ptr, IDCMP_VANILLAKEY, (uint16_t)p1, 0, 0, 0, 0);
            return 0;

        case WM_EVT_RESIZE:
            if (idcmp & IDCMP_NEWSIZE)
                post_intui_message(win_ptr, IDCMP_NEWSIZE, 0, 0, 0, 0, 0);
            if (slot && slot->simple_refresh && (idcmp & IDCMP_REFRESHWINDOW))
                post_intui_message(win_ptr, IDCMP_REFRESHWINDOW, 0, 0, 0, 0, 0);
            return 0;

        case WM_EVT_FOCUS:
            if (p1 && (idcmp & IDCMP_ACTIVEWINDOW))
                post_intui_message(win_ptr, IDCMP_ACTIVEWINDOW, 0, 0, 0, 0, 0);
            if (!p1 && (idcmp & IDCMP_INACTIVEWINDOW))
                post_intui_message(win_ptr, IDCMP_INACTIVEWINDOW, 0, 0, 0, 0, 0);
            if (!p1 && slot && slot->active_string_gad) {
                deactivate_string_gadget(slot);
                WM_Redraw();
            }
            return 0;
    }

    return 0;
}

/* Empty draw callback — m68k app should handle rendering via IDCMP */
static void intu_draw_fn(int win_x, int win_y, int win_w, int win_h);

/* =========================================================================
 * Custom gadget support
 * ========================================================================= */

static uint32_t insert_gadget_at(uint32_t first, uint32_t gad, int position)
{
    if (!gad) return first;
    if (!first || position <= 0) {
        mem_w32(gad + GAD_OFF_NEXTGADGET, first);
        return gad;
    }
    uint32_t prev = first;
    int pos = 1;
    while (pos < position && mem_u32(prev + GAD_OFF_NEXTGADGET)) {
        prev = mem_u32(prev + GAD_OFF_NEXTGADGET);
        pos++;
    }
    uint32_t next = mem_u32(prev + GAD_OFF_NEXTGADGET);
    mem_w32(prev + GAD_OFF_NEXTGADGET, gad);
    mem_w32(gad + GAD_OFF_NEXTGADGET, next);
    return first;
}

static int count_gadgets(uint32_t first)
{
    int n = 0;
    while (first) {
        n++;
        first = mem_u32(first + GAD_OFF_NEXTGADGET);
    }
    return n;
}

static uint32_t remove_gadget_from_list(uint32_t first, uint32_t gad)
{
    if (!first || !gad) return first;
    if (first == gad) {
        return mem_u32(first + GAD_OFF_NEXTGADGET);
    }
    uint32_t prev = first;
    while (prev) {
        uint32_t next = mem_u32(prev + GAD_OFF_NEXTGADGET);
        if (next == gad) {
            mem_w32(prev + GAD_OFF_NEXTGADGET, mem_u32(gad + GAD_OFF_NEXTGADGET));
            return first;
        }
        prev = next;
    }
    return first;
}

static uint32_t remove_gadgets_from_list(uint32_t first, uint32_t gad, int num)
{
    if (!first || !gad || num <= 0) return first;
    /* Find the predecessor of gad in the list. */
    uint32_t prev = 0;
    uint32_t cur = first;
    while (cur && cur != gad) {
        prev = cur;
        cur = mem_u32(cur + GAD_OFF_NEXTGADGET);
    }
    if (!cur) return first;

    /* Walk num gadgets starting from gad and detach the chain. */
    uint32_t end = gad;
    int removed = 1;
    while (removed < num && end) {
        end = mem_u32(end + GAD_OFF_NEXTGADGET);
        removed++;
    }
    uint32_t after = end ? mem_u32(end + GAD_OFF_NEXTGADGET) : 0;
    if (prev) {
        mem_w32(prev + GAD_OFF_NEXTGADGET, after);
        return first;
    }
    return after;
}

static int gadget_position_in_list(uint32_t first, uint32_t gad)
{
    int pos = 0;
    while (first) {
        if (first == gad) return pos;
        first = mem_u32(first + GAD_OFF_NEXTGADGET);
        pos++;
    }
    return -1;
}

static int add_gadget_list(uint32_t win_ptr, uint32_t first_gad, int position, int num)
{
    if (!win_ptr || !first_gad) return -1;
    uint32_t first = mem_u32(win_ptr + WIN_OFF_FIRSTGADGET);
    if (position < 0) {
        /* -1 means append. */
        position = count_gadgets(first);
    }
    if (num < 0) {
        /* Count gadgets in the linked list starting at first_gad. */
        num = count_gadgets(first_gad);
    }
    if (num <= 0) return position;

    /* Detach the first num gadgets from the source list. */
    uint32_t tail = first_gad;
    for (int i = 1; i < num && tail; i++)
        tail = mem_u32(tail + GAD_OFF_NEXTGADGET);
    if (tail) {
        uint32_t rest = mem_u32(tail + GAD_OFF_NEXTGADGET);
        (void)rest;
        mem_w32(tail + GAD_OFF_NEXTGADGET, 0);
    }

    first = insert_gadget_at(first, first_gad, position);
    mem_w32(win_ptr + WIN_OFF_FIRSTGADGET, first);
    return position;
}

/* AddGadget(window, gadget, position) — A0, A1, D0; returns position in D0 */
static void intuition_AddGadget(void)
{
    uint32_t win_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t gad     = m68k_get_reg(NULL, M68K_REG_A1);
    int      pos     = (int)m68k_get_reg(NULL, M68K_REG_D0);
    int result = add_gadget_list(win_ptr, gad, pos, 1);
    m68k_set_reg(M68K_REG_D0, (result < 0) ? 0 : (uint32_t)result);
}

/* AddGList(window, gadget, position, numGad, requester) — A0, A1, D0, D1, A2 */
static void intuition_AddGList(void)
{
    uint32_t win_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t gad     = m68k_get_reg(NULL, M68K_REG_A1);
    int      pos     = (int)m68k_get_reg(NULL, M68K_REG_D0);
    int      num     = (int)m68k_get_reg(NULL, M68K_REG_D1);
    (void)m68k_get_reg(NULL, M68K_REG_A2);
    int result = add_gadget_list(win_ptr, gad, pos, num);
    m68k_set_reg(M68K_REG_D0, (result < 0) ? 0 : (uint32_t)result);
}

/* RemoveGadget(window, gadget) — A0, A1; returns position in D0 */
static void intuition_RemoveGadget(void)
{
    uint32_t win_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t gad     = m68k_get_reg(NULL, M68K_REG_A1);
    int result = -1;
    if (win_ptr && gad) {
        uint32_t first = mem_u32(win_ptr + WIN_OFF_FIRSTGADGET);
        result = gadget_position_in_list(first, gad);
        first = remove_gadget_from_list(first, gad);
        mem_w32(win_ptr + WIN_OFF_FIRSTGADGET, first);
    }
    m68k_set_reg(M68K_REG_D0, (result < 0) ? 0xFFFF : (uint32_t)result);
}

/* RemoveGList(window, gadget, numGad) — A0, A1, D0; returns position in D0 */
static void intuition_RemoveGList(void)
{
    uint32_t win_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t gad     = m68k_get_reg(NULL, M68K_REG_A1);
    int      num     = (int)m68k_get_reg(NULL, M68K_REG_D0);
    int result = -1;
    if (win_ptr && gad) {
        uint32_t first = mem_u32(win_ptr + WIN_OFF_FIRSTGADGET);
        result = gadget_position_in_list(first, gad);
        first = remove_gadgets_from_list(first, gad, num);
        mem_w32(win_ptr + WIN_OFF_FIRSTGADGET, first);
    }
    m68k_set_reg(M68K_REG_D0, (result < 0) ? 0xFFFF : (uint32_t)result);
}

/* RefreshGList(gadgets, window, requester, numGad) — A0, A1, A2, D0 */
static void intuition_RefreshGList(void)
{
    (void)m68k_get_reg(NULL, M68K_REG_A0);
    (void)m68k_get_reg(NULL, M68K_REG_A1);
    (void)m68k_get_reg(NULL, M68K_REG_A2);
    (void)m68k_get_reg(NULL, M68K_REG_D0);
    WM_Redraw();
}

/* RefreshGadgets(gadgets, window, requester, numGad) — A0, A1, A2, D0
 * Older Intuition entry point that performs the same work as RefreshGList. */
static void intuition_RefreshGadgets(void)
{
    intuition_RefreshGList();
}

/* Static helper: read a gadget's state/value.  info_id selects the field:
 *   0 = type id, 1 = flags, 2 = selected, 3 = integer value,
 *   4 = string buffer, 5 = listview selected index,
 *   6 = prop HorizPot, 7 = prop VertPot. */
static uint32_t GetGadgetInfo(uint32_t gad, uint32_t info_id)
{
    if (!gad) return 0;
    uint16_t type = mem_u16(gad + GAD_OFF_GADGETTYPE);
    int type_id = type & 0x000F;
    uint16_t flags = mem_u16(gad + GAD_OFF_FLAGS);
    uint32_t special = mem_u32(gad + GAD_OFF_SPECIALINFO);

    switch (info_id) {
        case 0: return (uint32_t)type_id;
        case 1: return (uint32_t)flags;
        case 2: return (flags & GFLG_SELECTED) ? 1 : 0;
        case 3:
            if (type_id == GTYP_INTGADGET && special) {
                uint32_t buf = mem_u32(special + SI_OFF_BUFFER);
                if (buf) {
                    char text[32];
                    guest_str(text, buf, sizeof(text));
                    char *p = text;
                    while (*p == ' ') p++;
                    return (uint32_t)(int)strtol(p, NULL, 10);
                }
            }
            return 0;
        case 4:
            if ((type_id == GTYP_STRGADGET || type_id == GTYP_INTGADGET) && special)
                return mem_u32(special + SI_OFF_BUFFER);
            return 0;
        case 5:
            if (type_id == GTYP_LISTVIEW && special) {
                if (mem_u32(special + LV_OFF_MULTI_SELECT))
                    return mem_u32(special + LV_OFF_SELECTED_MASK);
                return mem_u32(special + LV_OFF_SELECTED);
            }
            return 0;
        case 6:
            if (type_id == GTYP_PROPGADGET && special)
                return mem_u16(special + PROP_OFF_HORIZPOT);
            return 0;
        case 7:
            if (type_id == GTYP_PROPGADGET && special)
                return mem_u16(special + PROP_OFF_VERTPOT);
            return 0;
    }
    return 0;
}

/* OnGadget(gadget, window, requester) — A0, A1, A2 */
static void intuition_OnGadget(void)
{
    uint32_t gad = m68k_get_reg(NULL, M68K_REG_A0);
    (void)m68k_get_reg(NULL, M68K_REG_A1);
    (void)m68k_get_reg(NULL, M68K_REG_A2);
    if (gad) {
        uint16_t flags = mem_u16(gad + GAD_OFF_FLAGS);
        flags &= ~GFLG_DISABLED;
        mem_w16(gad + GAD_OFF_FLAGS, flags);
    }
    WM_Redraw();
}

/* OffGadget(gadget, window, requester) — A0, A1, A2 */
static void intuition_OffGadget(void)
{
    uint32_t gad = m68k_get_reg(NULL, M68K_REG_A0);
    (void)m68k_get_reg(NULL, M68K_REG_A1);
    (void)m68k_get_reg(NULL, M68K_REG_A2);
    if (gad) {
        uint16_t flags = mem_u16(gad + GAD_OFF_FLAGS);
        flags |= GFLG_DISABLED;
        mem_w16(gad + GAD_OFF_FLAGS, flags);
    }
    WM_Redraw();
}

/* ModifyProp(gadget, window, requester, flags, horizPot, vertPot, horizBody, vertBody)
 * A0, A1, A2, D0, D1, D2, D3, D4 */
static void intuition_ModifyProp(void)
{
    uint32_t gad = m68k_get_reg(NULL, M68K_REG_A0);
    (void)m68k_get_reg(NULL, M68K_REG_A1);
    (void)m68k_get_reg(NULL, M68K_REG_A2);
    uint16_t flags = (uint16_t)m68k_get_reg(NULL, M68K_REG_D0);
    uint16_t hpot  = (uint16_t)m68k_get_reg(NULL, M68K_REG_D1);
    uint16_t vpot  = (uint16_t)m68k_get_reg(NULL, M68K_REG_D2);
    uint16_t hbody = (uint16_t)m68k_get_reg(NULL, M68K_REG_D3);
    uint16_t vbody = (uint16_t)m68k_get_reg(NULL, M68K_REG_D4);
    if (gad) {
        uint16_t type = mem_u16(gad + GAD_OFF_GADGETTYPE);
        if ((type & 0x000F) == GTYP_PROPGADGET) {
            uint32_t prop = mem_u32(gad + GAD_OFF_SPECIALINFO);
            if (prop) {
                mem_w16(prop + PROP_OFF_FLAGS, flags);
                mem_w16(prop + PROP_OFF_HORIZPOT, hpot);
                mem_w16(prop + PROP_OFF_VERTPOT, vpot);
                mem_w16(prop + PROP_OFF_HORIZBODY, hbody);
                mem_w16(prop + PROP_OFF_VERTBODY, vbody);
            }
        }
    }
    WM_Redraw();
}

/* NewModifyProp(gadget, window, requester, flags, attr, hpot, vpot, hbody, vbody)
 * A0, A1, A2, D0, D1, D2, D3, D4, A3
 * In OS3.0 this is the same as ModifyProp but with extra flags argument. */
static void intuition_NewModifyProp(void)
{
    (void)m68k_get_reg(NULL, M68K_REG_A3);
    intuition_ModifyProp();
}

/* ActivateGadget(gadget, window, requester) — A0, A1, A2; returns BOOL in D0 */
static void intuition_ActivateGadget(void)
{
    uint32_t gad     = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t win_ptr = m68k_get_reg(NULL, M68K_REG_A1);
    (void)m68k_get_reg(NULL, M68K_REG_A2);
    int ok = 0;
    if (gad && win_ptr) {
        uint16_t type = mem_u16(gad + GAD_OFF_GADGETTYPE);
        if ((type & 0x000F) == GTYP_STRGADGET) {
            /* Mark the gadget as the active string gadget and focus the window. */
            IntuitionSlot *slot = find_slot_by_guest(win_ptr);
            if (slot) WM_RequestFocus(slot->wm_handle);
            ok = 1;
        }
    }
    m68k_set_reg(M68K_REG_D0, ok ? 1 : 0);
}

/* Find the topmost custom gadget at window-relative coordinates. */
static uint32_t gadget_at(uint32_t win_ptr, int mx, int my)
{
    uint32_t gad = mem_u32(win_ptr + WIN_OFF_FIRSTGADGET);
    while (gad) {
        uint16_t flags = mem_u16(gad + GAD_OFF_FLAGS);
        if (flags & GFLG_DISABLED) {
            gad = mem_u32(gad + GAD_OFF_NEXTGADGET);
            continue;
        }
        int16_t left = mem_s16(gad + GAD_OFF_LEFTEDGE);
        int16_t top  = mem_s16(gad + GAD_OFF_TOPEDGE);
        int16_t w    = mem_s16(gad + GAD_OFF_WIDTH);
        int16_t h    = mem_s16(gad + GAD_OFF_HEIGHT);
        if (mx >= left && mx < left + w && my >= top && my < top + h)
            return gad;
        gad = mem_u32(gad + GAD_OFF_NEXTGADGET);
    }
    return 0;
}

/* Read a gadget's label text into buf (max bytes). Returns 1 if a label was found. */
static int gadget_label_text(uint32_t gad, char *buf, int max)
{
    uint16_t flags = mem_u16(gad + GAD_OFF_FLAGS);
    buf[0] = '\0';
    if (flags & GFLG_LABELITEXT) {
        uint32_t label = mem_u32(gad + GAD_OFF_GADGETTEXT);
        if (label) {
            uint32_t text_ptr = mem_u32(label + ITEXT_OFF_ITEXT);
            if (text_ptr) {
                guest_str(buf, text_ptr, max);
                return 1;
            }
        }
    }
    return 0;
}

/* Draw a small checkbox or radio button with an optional label.
 * mutual = non-zero => radio circle; otherwise checkbox square. */
static void draw_checkbox_or_radio(int gx, int gy, int w, int h, uint32_t fg, uint32_t bg,
                                   int selected, int mutual, const char *label)
{
    int box = (h < 20) ? h - 2 : 18;
    if (box < 8) box = 8;
    int by = gy + (h - box) / 2;

    if (mutual) {
        FB_FillRect(gx + 1, by + 1, box - 2, box - 2, bg);
        FB_DrawRect(gx, by, box, box, fg);
        if (selected) {
            int dot = box / 4;
            if (dot < 3) dot = 3;
            int cx = gx + box / 2;
            int cy = by + box / 2;
            FB_FillRect(cx - dot / 2, cy - dot / 2, dot, dot, fg);
        }
    } else {
        FB_FillRect(gx, by, box, box, bg);
        FB_DrawRect(gx, by, box, box, fg);
        if (selected) {
            int cx = gx + box / 2;
            int cy = by + box / 2;
            FB_DrawRect(cx - box / 4, cy - box / 4, box / 2, box / 2, fg);
            FB_DrawRect(cx - box / 4 + 1, cy - box / 4 + 1, box / 2 - 2, box / 2 - 2, fg);
        }
    }

    if (label && label[0]) {
        int tx = gx + box + 4;
        int ty = gy + (h - 16) / 2;
        FB_PutStr(tx, ty, label, fg, bg);
    }
}

/* String gadget helpers --------------------------------------------------- */

static uint32_t string_gadget_si(uint32_t gad)
{
    if (!gad_is_string_gadget(gad)) return 0;
    return mem_u32(gad + GAD_OFF_SPECIALINFO);
}

static uint16_t string_gadget_numchars(uint32_t gad)
{
    uint32_t si = string_gadget_si(gad);
    if (!si) return 0;
    return mem_u16(si + SI_OFF_NUMCHARS);
}

static int string_gadget_insert_char(uint32_t gad, int pos, char c)
{
    uint32_t si = string_gadget_si(gad);
    if (!si) return 0;
    uint32_t buf = mem_u32(si + SI_OFF_BUFFER);
    if (!buf) return 0;
    uint16_t maxchars = mem_u16(si + SI_OFF_MAXCHARS);
    uint16_t numchars = mem_u16(si + SI_OFF_NUMCHARS);
    if (numchars >= maxchars) return 0;
    if (pos < 0) pos = 0;
    if (pos > numchars) pos = numchars;
    for (int i = numchars; i > pos; i--)
        mem_w8(buf + i, mem_u8(buf + i - 1));
    mem_w8(buf + pos, (uint8_t)c);
    mem_w16(si + SI_OFF_NUMCHARS, numchars + 1);
    mem_w8(buf + numchars + 1, 0);  /* keep buffer NUL-terminated */
    return 1;
}

static int string_gadget_delete_char(uint32_t gad, int pos)
{
    uint32_t si = string_gadget_si(gad);
    if (!si) return 0;
    uint32_t buf = mem_u32(si + SI_OFF_BUFFER);
    if (!buf) return 0;
    uint16_t numchars = mem_u16(si + SI_OFF_NUMCHARS);
    if (pos < 0 || pos >= numchars) return 0;
    for (int i = pos; i < numchars - 1; i++)
        mem_w8(buf + i, mem_u8(buf + i + 1));
    mem_w16(si + SI_OFF_NUMCHARS, numchars - 1);
    mem_w8(buf + numchars - 1, 0);  /* keep buffer NUL-terminated */
    return 1;
}

static int string_gadget_delete_range(uint32_t gad, int start, int end)
{
    uint32_t si = string_gadget_si(gad);
    if (!si) return 0;
    uint32_t buf = mem_u32(si + SI_OFF_BUFFER);
    if (!buf) return 0;
    uint16_t numchars = mem_u16(si + SI_OFF_NUMCHARS);
    if (start < 0) start = 0;
    if (end > numchars) end = numchars;
    if (start >= end) return 0;
    int n = end - start;
    for (int i = end; i < numchars; i++)
        mem_w8(buf + i - n, mem_u8(buf + i));
    mem_w16(si + SI_OFF_NUMCHARS, numchars - n);
    mem_w8(buf + numchars - n, 0);  /* keep buffer NUL-terminated */
    return 1;
}

static int string_gadget_handle_key(uint32_t gad, uint16_t *cursor,
                                    uint16_t *sel_start, uint16_t *sel_end, char c)
{
    uint32_t si = string_gadget_si(gad);
    if (!si) return 0;
    uint16_t numchars = mem_u16(si + SI_OFF_NUMCHARS);
    int pos = *cursor;
    int sel_lo = (*sel_start < *sel_end) ? *sel_start : *sel_end;
    int sel_hi = (*sel_start < *sel_end) ? *sel_end : *sel_start;
    int has_sel = (sel_lo != sel_hi);

    if (c >= 32 && c < 127) {
        if (has_sel) {
            string_gadget_delete_range(gad, sel_lo, sel_hi);
            pos = sel_lo;
            numchars = mem_u16(si + SI_OFF_NUMCHARS);
        }
        if (string_gadget_insert_char(gad, pos, c)) {
            *cursor = pos + 1;
            *sel_start = *sel_end = *cursor;
            return 1;
        }
    } else if (c == '\b') {
        if (has_sel) {
            string_gadget_delete_range(gad, sel_lo, sel_hi);
            *cursor = sel_lo;
            *sel_start = *sel_end = *cursor;
            return 1;
        } else if (pos > 0) {
            string_gadget_delete_char(gad, pos - 1);
            *cursor = pos - 1;
            return 1;
        }
    } else if (c == 0x7F) {
        if (has_sel) {
            string_gadget_delete_range(gad, sel_lo, sel_hi);
            *cursor = sel_lo;
            *sel_start = *sel_end = *cursor;
            return 1;
        } else if (pos < numchars) {
            string_gadget_delete_char(gad, pos);
            return 1;
        }
    } else if (c == 0x01) { /* Ctrl+A -> select all */
        *cursor = numchars;
        *sel_start = 0;
        *sel_end = numchars;
        return 1;
    } else if (c == 0x05) { /* VKEY_LEFT */
        int shift = g_kbd_mods.shift;
        if (pos > 0) {
            *cursor = pos - 1;
            if (shift) {
                if (sel_lo == sel_hi) { *sel_start = pos; *sel_end = *cursor; }
                else { *sel_end = *cursor; }
            } else {
                *sel_start = *sel_end = *cursor;
            }
        }
        return 1;
    } else if (c == 0x06) { /* VKEY_RIGHT */
        int shift = g_kbd_mods.shift;
        if (pos < numchars) {
            *cursor = pos + 1;
            if (shift) {
                if (sel_lo == sel_hi) { *sel_start = pos; *sel_end = *cursor; }
                else { *sel_end = *cursor; }
            } else {
                *sel_start = *sel_end = *cursor;
            }
        }
        return 1;
    } else if (c == 0x03) { /* VKEY_UP -> Home */
        int shift = g_kbd_mods.shift;
        *cursor = 0;
        if (shift) {
            if (sel_lo == sel_hi) { *sel_start = pos; *sel_end = 0; }
            else { *sel_end = 0; }
        } else {
            *sel_start = *sel_end = 0;
        }
        return 1;
    } else if (c == 0x04) { /* VKEY_DOWN -> End */
        int shift = g_kbd_mods.shift;
        *cursor = numchars;
        if (shift) {
            if (sel_lo == sel_hi) { *sel_start = pos; *sel_end = numchars; }
            else { *sel_end = numchars; }
        } else {
            *sel_start = *sel_end = numchars;
        }
        return 1;
    }
    return 0;
}

static int string_gadget_cursor_from_click(uint32_t gad, int relx)
{
    int pos = (relx - 4) / 8;
    uint32_t si = string_gadget_si(gad);
    uint16_t numchars = si ? mem_u16(si + SI_OFF_NUMCHARS) : 0;
    if (pos < 0) pos = 0;
    if (pos > numchars) pos = numchars;
    return pos;
}

/* Commit an integer gadget: parse the buffer, clamp to min/max bounds, and
 * rewrite the buffer with the validated value. */
static void int_gadget_commit(uint32_t gad)
{
    uint32_t si = string_gadget_si(gad);
    if (!si) return;
    uint32_t buf = mem_u32(si + SI_OFF_BUFFER);
    if (!buf) return;

    char text[64];
    int numchars = mem_u16(si + SI_OFF_NUMCHARS);
    if (numchars > 63) numchars = 63;
    for (int i = 0; i < numchars; i++) text[i] = (char)mem_u8(buf + i);
    text[numchars] = '\0';

    /* Trim leading spaces for integer gadgets. */
    char *p = text;
    while (*p == ' ') p++;

    int32_t value = 0;
    if (*p == '-' || (*p >= '0' && *p <= '9')) {
        int sign = 1;
        if (*p == '-') { sign = -1; p++; }
        while (*p >= '0' && *p <= '9') {
            value = value * 10 + (*p - '0');
            p++;
        }
        value *= sign;
    }

    /* Read min/max bounds; if both are 0, default to the 16-bit signed range. */
    int32_t min = (int32_t)mem_s32(si + SI_OFF_MIN);
    int32_t max = (int32_t)mem_s32(si + SI_OFF_MAX);
    if (min == 0 && max == 0) {
        min = -32768;
        max = 32767;
    }
    if (min > max) {
        int32_t t = min; min = max; max = t;
    }
    if (value < min) value = min;
    if (value > max) value = max;

    /* Format back into the buffer. */
    char out[16];
    int len = 0;
    if (value < 0) { out[0] = '-'; len = 1; value = -value; }
    char tmp[16];
    int tlen = 0;
    if (value == 0) { tmp[tlen++] = '0'; }
    while (value > 0) { tmp[tlen++] = (char)('0' + (value % 10)); value /= 10; }
    for (int i = tlen - 1; i >= 0; i--) out[len++] = tmp[i];
    out[len] = '\0';

    int maxchars = mem_u16(si + SI_OFF_MAXCHARS);
    if (len > maxchars) len = maxchars;
    for (int i = 0; i < len; i++) mem_w8(buf + i, (uint8_t)out[i]);
    mem_w16(si + SI_OFF_NUMCHARS, (uint16_t)len);
    mem_w8(buf + len, 0);
}

static void deactivate_string_gadget(IntuitionSlot *slot)
{
    if (!slot) return;
    slot->active_string_gad = 0;
    slot->active_string_cursor = 0;
    slot->active_string_sel_start = 0;
    slot->active_string_sel_end = 0;
}

/* Post an IDCMP_HELP message to the focused window or, if WA_HelpGroupWindow is
 * set, to the designated help-group window. */
static void post_help_message(IntuitionSlot *slot, uint32_t win_ptr)
{
    uint32_t target = win_ptr;
    if (slot && slot->help_group_window)
        target = slot->help_group_window;
    post_intui_message(target, IDCMP_HELP, 0, 0, 0, 0, 0);
}

/* Draw a string or integer gadget input box showing the current buffer text.
 * When active, the current selection is highlighted and a caret is drawn. */
static void draw_string_like_gadget(int gx, int gy, int w, int h, uint32_t fg, uint32_t bg,
                                    uint32_t si, int is_int, int is_active,
                                    int cursor_pos, int sel_start, int sel_end)
{
    FB_FillRect(gx, gy, w, h, WB_WHITE);
    FB_DrawRect(gx, gy, w, h, fg);
    if (si) {
        uint32_t buf = mem_u32(si + SI_OFF_BUFFER);
        if (buf) {
            char text[64];
            int numchars = mem_u16(si + SI_OFF_NUMCHARS);
            if (numchars > 63) numchars = 63;
            for (int i = 0; i < numchars; i++) text[i] = (char)mem_u8(buf + i);
            text[numchars] = '\0';
            int trim = 0;
            if (is_int) {
                /* Integer gadgets may contain leading spaces or signs; trim. */
                while (text[trim] == ' ') trim++;
            }
            const char *disp = text + trim;
            int disp_len = numchars - trim;
            int text_x = gx + 4;
            int text_y = gy + (h - 16) / 2;

            if (is_active && sel_end != sel_start) {
                int s1 = sel_start - trim;
                if (s1 < 0) s1 = 0;
                int s2 = sel_end - trim;
                if (s2 > disp_len) s2 = disp_len;
                if (s2 > s1) {
                    int sx1 = text_x + s1 * 8;
                    int sw = (s2 - s1) * 8;
                    FB_FillRect(sx1, gy + 2, sw, h - 4, WB_BLUE);
                    for (int i = s1; i < s2; i++)
                        FB_PutChar(text_x + i * 8, text_y, disp[i], WB_WHITE, WB_BLUE);
                }
                if (s1 > 0) {
                    char prefix[64];
                    for (int i = 0; i < s1; i++) prefix[i] = disp[i];
                    prefix[s1] = '\0';
                    FB_PutStr(text_x, text_y, prefix, fg, bg);
                }
                if (s2 < disp_len)
                    FB_PutStr(text_x + s2 * 8, text_y, disp + s2, fg, bg);
            } else {
                FB_PutStr(text_x, text_y, disp, fg, bg);
            }
        }
    }
    if (is_active && cursor_pos >= 0) {
        int cx = gx + 4 + cursor_pos * 8;
        if (cx < gx + w - 1 && cx >= gx + 1)
            FB_DrawVLine(cx, gy + 2, h - 4, fg);
    }
}

/* Compute the intersection of two rectangles.  Returns 0 if they do not
 * overlap, otherwise fills the output rectangle and returns 1. */
static int rect_intersect(int x1, int y1, int w1, int h1,
                          int x2, int y2, int w2, int h2,
                          int *ox, int *oy, int *ow, int *oh)
{
    int x1e = x1 + w1, y1e = y1 + h1;
    int x2e = x2 + w2, y2e = y2 + h2;
    int ox1 = x1 > x2 ? x1 : x2;
    int oy1 = y1 > y2 ? y1 : y2;
    int ox2 = x1e < x2e ? x1e : x2e;
    int oy2 = y1e < y2e ? y1e : y2e;
    if (ox2 <= ox1 || oy2 <= oy1) return 0;
    if (ox) *ox = ox1;
    if (oy) *oy = oy1;
    if (ow) *ow = ox2 - ox1;
    if (oh) *oh = oy2 - oy1;
    return 1;
}

/* Draw a simple listview: a box with visible items and a scrollbar. */
static void draw_listview_gadget(int gx, int gy, int w, int h, uint32_t fg, uint32_t bg,
                                   uint32_t lv)
{
    FB_FillRect(gx, gy, w, h, WB_WHITE);
    FB_DrawRect(gx, gy, w, h, fg);
    if (!lv) return;

    int count    = (int)mem_u32(lv + LV_OFF_COUNT);
    int visible  = (int)mem_u32(lv + LV_OFF_VISIBLE);
    int top      = (int)mem_u32(lv + LV_OFF_TOP);
    int multi    = (int)mem_u32(lv + LV_OFF_MULTI_SELECT);
    uint32_t sel_mask = mem_u32(lv + LV_OFF_SELECTED_MASK);
    if (visible <= 0) visible = 4;
    if (top < 0) top = 0;
    if (top > count - visible) top = count - visible;
    if (top < 0) top = 0;

    uint32_t items = mem_u32(lv + LV_OFF_ITEMS);
    int row_h = 16;
    int inner_w = w - 2;
    int scroll_w = 12;
    if (count > visible) inner_w -= scroll_w;
    if (inner_w < 4) inner_w = 4;

    for (int i = 0; i < visible && (top + i) < count; i++) {
        int idx = top + i;
        uint32_t item_ptr = 0;
        if (items) item_ptr = mem_u32(items + (uint32_t)idx * 4);
        char text[64] = "";
        if (item_ptr) guest_str(text, item_ptr, sizeof(text));
        int ry = gy + 2 + i * row_h;
        int selected = (multi && (sel_mask & (1U << idx))) || (!multi && idx == (int)mem_u32(lv + LV_OFF_SELECTED));
        uint32_t row_bg = selected ? WB_BLUE : WB_WHITE;
        uint32_t row_fg = selected ? WB_WHITE : fg;
        FB_FillRect(gx + 1, ry, inner_w - 1, row_h - 1, row_bg);
        FB_PutStr(gx + 4, ry + (row_h - 16) / 2, text, row_fg, row_bg);
    }

    if (count > visible) {
        int sx = gx + w - scroll_w - 1;
        FB_DrawRect(sx, gy + 1, scroll_w, h - 2, fg);
        int sh = (h - 4) * visible / count;
        if (sh < 4) sh = 4;
        int sy = gy + 2 + (h - 4 - sh) * top / (count - visible);
        FB_FillRect(sx + 1, sy, scroll_w - 2, sh, WB_GREY);
    }
}

/* Render the window's custom gadget list.  Boolean gadgets are rendered as
 * buttons, checkboxes, or radio buttons depending on activation flags; integer
 * gadgets show their numeric value; listviews show a scrollable item list; and
 * string gadgets show their buffer text.  The guest application is still
 * expected to draw its own complex imagery via the RastPort and to call
 * RefreshGList() when gadget visuals change.
 * When clip_w/clip_h are non-zero, only gadgets that intersect the clip
 * rectangle are drawn; this is used during BeginRefresh/EndRefresh. */
static void render_custom_gadgets(int win_x, int win_y, int off_x, int off_y, uint32_t win_ptr,
                                  IntuitionSlot *slot,
                                  int clip_x, int clip_y, int clip_w, int clip_h)
{
    uint32_t gad = mem_u32(win_ptr + WIN_OFF_FIRSTGADGET);
    while (gad) {
        uint16_t flags = mem_u16(gad + GAD_OFF_FLAGS);
        int16_t left   = mem_s16(gad + GAD_OFF_LEFTEDGE);
        int16_t top    = mem_s16(gad + GAD_OFF_TOPEDGE);
        int16_t w      = mem_s16(gad + GAD_OFF_WIDTH);
        int16_t h      = mem_s16(gad + GAD_OFF_HEIGHT);
        uint16_t type  = mem_u16(gad + GAD_OFF_GADGETTYPE);
        int type_id = type & 0x000F;
        uint16_t activation = mem_u16(gad + GAD_OFF_ACTIVATION);
        uint32_t special = mem_u32(gad + GAD_OFF_SPECIALINFO);

        uint32_t bg = (flags & GFLG_SELECTED) ? WB_BLUE : WB_GREY;
        uint32_t fg = WB_BLACK;
        if (flags & GFLG_DISABLED) { fg = WB_DARK_GREY; bg = WB_LIGHT_GREY; }

        int gx = win_x + off_x + left;
        int gy = win_y + off_y + top;

        if (clip_w <= 0 || clip_h <= 0 ||
            !rect_intersect(gx, gy, w, h, clip_x, clip_y, clip_w, clip_h, NULL, NULL, NULL, NULL)) {
            gad = mem_u32(gad + GAD_OFF_NEXTGADGET);
            continue;
        }

        if (type_id == GTYP_PROPGADGET) {
            uint32_t prop = special;
            if (prop) {
                uint16_t hpot = mem_u16(prop + PROP_OFF_HORIZPOT);
                uint16_t vpot = mem_u16(prop + PROP_OFF_VERTPOT);
                uint16_t hbody = mem_u16(prop + PROP_OFF_HORIZBODY);
                uint16_t vbody = mem_u16(prop + PROP_OFF_VERTBODY);
                FB_FillRect(gx, gy, w, h, WB_LIGHT_GREY);
                FB_DrawRect(gx, gy, w, h, WB_BLACK);
                if (w > h) {
                    int kw = (int)(w * hbody / 0xFFFF);
                    if (kw < 4) kw = 4;
                    int kx = gx + (int)(w * hpot / 0xFFFF) - kw / 2;
                    if (kx < gx) kx = gx;
                    if (kx + kw > gx + w) kx = gx + w - kw;
                    FB_FillRect(kx, gy + 2, kw, h - 4, WB_BLUE);
                } else {
                    int kh = (int)(h * vbody / 0xFFFF);
                    if (kh < 4) kh = 4;
                    int ky = gy + (int)(h * vpot / 0xFFFF) - kh / 2;
                    if (ky < gy) ky = gy;
                    if (ky + kh > gy + h) ky = gy + h - kh;
                    FB_FillRect(gx + 2, ky, w - 4, kh, WB_BLUE);
                }
            }
        } else if (type_id == GTYP_STRGADGET) {
            int active = (slot && slot->active_string_gad == gad);
            draw_string_like_gadget(gx, gy, w, h, fg, bg, special, 0,
                                    active, active ? (int)slot->active_string_cursor : 0,
                                    active ? (int)slot->active_string_sel_start : 0,
                                    active ? (int)slot->active_string_sel_end : 0);
        } else if (type_id == GTYP_INTGADGET) {
            int active = (slot && slot->active_string_gad == gad);
            draw_string_like_gadget(gx, gy, w, h, fg, bg, special, 1,
                                    active, active ? (int)slot->active_string_cursor : 0,
                                    active ? (int)slot->active_string_sel_start : 0,
                                    active ? (int)slot->active_string_sel_end : 0);
        } else if (type_id == GTYP_LISTVIEW) {
            draw_listview_gadget(gx, gy, w, h, fg, bg, special);
        } else if (type_id == GTYP_BOOLGADGET) {
            char label[64];
            gadget_label_text(gad, label, sizeof(label));
            int mutual = mem_u32(gad + GAD_OFF_MUTUALEXCLUDE) ? 1 : 0;
            int toggle = (activation & GACT_TOGGLESELECT) ? 1 : 0;
            if (mutual || toggle) {
                draw_checkbox_or_radio(gx, gy, w, h, fg, bg,
                                       (flags & GFLG_SELECTED) ? 1 : 0,
                                       mutual, label);
            } else {
                /* Plain push button */
                FB_FillRect(gx, gy, w, h, bg);
                FB_DrawRect(gx, gy, w, h, fg);
                if (label[0])
                    FB_PutStr(gx + 4, gy + (h - 16) / 2, label, fg, bg);
            }
        } else {
            /* Custom / fallback gadget */
            FB_FillRect(gx, gy, w, h, bg);
            FB_DrawRect(gx, gy, w, h, fg);

            char label[64];
            if (gadget_label_text(gad, label, sizeof(label))) {
                FB_PutStr(gx + 4, gy + (h - 16) / 2, label, fg, bg);
            }
        }

        gad = mem_u32(gad + GAD_OFF_NEXTGADGET);
    }
}

/* Return the ColorMap of the screen a window lives on, or 0. */
static uint32_t get_window_colormap(uint32_t win_ptr)
{
    uint32_t screen = mem_u32(win_ptr + WIN_OFF_WSCREEN);
    if (!screen) return 0;
    uint32_t vp = mem_u32(screen + SCR_OFF_VIEWPORT);
    if (!vp) return 0;
    return mem_u32(vp + VP_OFF_COLORMAP);
}

/* Fill the window's client area with a solid pen colour.
 * Used as a simple fallback for stored WA_BackFill / SA_BackFill hooks. */
static void fill_window_client_area(int win_x, int win_y, int win_w, int win_h,
                                    uint32_t rgb)
{
    int cx = win_x + 1;
    int cy = win_y + WM_TITLEBAR_H;
    int cw = win_w - 1 - WM_SCROLLBAR_W;
    int ch = win_h - WM_TITLEBAR_H - WM_SCROLLBAR_W;
    if (cw > 0 && ch > 0) FB_FillRect(cx, cy, cw, ch, rgb);
}

/* Window draw callback: render custom gadgets on top of whatever the
 * guest application has drawn into the window.  For SimpleRefresh windows
 * an IDCMP_REFRESHWINDOW is posted so the guest can redraw damaged areas.
 * GimmeZeroZero content is offset by the border sizes.
 * During BeginRefresh/EndRefresh, gadget rendering is clipped to the damage
 * region so host-side gadgets don't redraw outside the damaged area.
 *
 * WA_SuperBitMap windows are rendered from their backing BitMap. WA_BackFill
 * hooks are dispatched via UAOS_InvokeM68kHook(); values below 256 still use
 * the solid-pen fallback. */
static void intu_draw_fn(int win_x, int win_y, int win_w, int win_h)
{
    int wh = WM_CurrentDrawHandle;
    if (wh < 0) return;
    uint32_t win_ptr = get_guest_window_from_handle(wh);
    if (!win_ptr) return;

    IntuitionSlot *slot = get_slot_from_handle(wh);
    int off_x = (slot && slot->gimme_zero_zero) ? slot->border_left : 0;
    int off_y = (slot && slot->gimme_zero_zero) ? slot->border_top : 0;

    /* WA_SuperBitMap: render the backing BitMap into the window client area.
     * The WM has already filled the body with WB_GREY; the super-bitmap
     * replaces it. */
    if (slot && slot->super_bitmap) {
        uint32_t cmap = get_window_colormap(win_ptr);
        render_bitmap_to_framebuffer(slot->super_bitmap, cmap,
                                     win_x + 1, win_y + WM_TITLEBAR_H,
                                     win_w - 1 - WM_SCROLLBAR_W,
                                     win_h - WM_TITLEBAR_H - WM_SCROLLBAR_W);
    } else if (slot && slot->backfill) {
        if (slot->backfill < 256) {
            /* Pen-index fallback for WA_BackFill. */
            fill_window_client_area(win_x, win_y, win_w, win_h,
                                    amiga_pen_to_rgb((uint8_t)slot->backfill));
        } else {
            /* Real m68k Hook callback: fill the window client area.
             * A0 = hook, A2 = window RastPort, A1 = Rectangle in RastPort coords. */
            uint32_t rport = mem_u32(win_ptr + WIN_OFF_RPORT);
            if (rport) {
                int cx = win_x + 1;
                int cy = win_y + WM_TITLEBAR_H;
                int cw = win_w - 1 - WM_SCROLLBAR_W;
                int ch = win_h - WM_TITLEBAR_H - WM_SCROLLBAR_W;
                if (cw > 0 && ch > 0) {
                    const uint32_t rect = 0x1EF100u;
                    mem_w16(rect + 0, 0);
                    mem_w16(rect + 2, 0);
                    mem_w16(rect + 4, (int16_t)(cw - 1));
                    mem_w16(rect + 6, (int16_t)(ch - 1));
                    UAOS_InvokeM68kHook(slot->backfill, slot->backfill, rect, rport);
                }
            }
        }
    }

    int clip_x = win_x, clip_y = win_y, clip_w = win_w, clip_h = win_h;
    if (slot && slot->refreshing) {
        if (!rect_intersect(win_x, win_y, win_w, win_h,
                            slot->damage_x, slot->damage_y,
                            slot->damage_w, slot->damage_h,
                            &clip_x, &clip_y, &clip_w, &clip_h)) {
            clip_w = 0;
            clip_h = 0;
        }
    }

    render_custom_gadgets(win_x, win_y, off_x, off_y, win_ptr, slot,
                          clip_x, clip_y, clip_w, clip_h);

    if (slot && slot->simple_refresh && !slot->refreshing) {
        uint32_t idcmp = mem_u32(win_ptr + WIN_OFF_IDCMPFLAGS);
        if (idcmp & IDCMP_REFRESHWINDOW) {
            slot->damage_x = win_x + off_x;
            slot->damage_y = win_y + off_y;
            slot->damage_w = win_w - off_x - (slot ? slot->border_right : 0);
            if (slot->damage_w < 0) slot->damage_w = 0;
            slot->damage_h = win_h - off_y - (slot ? slot->border_bottom : 0);
            if (slot->damage_h < 0) slot->damage_h = 0;
            post_intui_message(win_ptr, IDCMP_REFRESHWINDOW, 0, 0, 0, 0, 0);
        }
    }
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
    if (g_req_slot.guest_win) {
        free_window_idcmp(g_req_slot.guest_win);
        free_gadget_list(mem_u32(g_req_slot.guest_win + WIN_OFF_FIRSTGADGET));
        uint32_t rport = mem_u32(g_req_slot.guest_win + WIN_OFF_RPORT);
        intu_free(rport);
        intu_free(g_req_slot.guest_win);
    }
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
 * Alert support (DisplayAlert / TimedDisplayAlert)
 * ========================================================================= */

#define ALERT_MAX_SUBSTRINGS  8
#define ALERT_SUBSTRING_TEXT_SIZE 80

typedef struct {
    uint16_t x;
    uint16_t y;
    char     text[ALERT_SUBSTRING_TEXT_SIZE];
} AlertSubstring;

typedef struct {
    int      wm_handle;
    uint32_t guest_win;
    uint8_t  sigbit;
    uint32_t sigmask;
    UaosTask *task;
    int      result;
    uint8_t  active;
    uint32_t color;          /* border/text colour */
    uint32_t timeout_frames; /* 0 = no timeout */
    uint64_t timeout_start;  /* PIT tick at which alert was shown */
    int      num_substrings;
    AlertSubstring substrings[ALERT_MAX_SUBSTRINGS];
} AlertSlot;

static AlertSlot g_alert_slot;

static void clear_alert_slot(void)
{
    if (g_alert_slot.active && g_alert_slot.sigbit) {
        free_intuition_signal(g_alert_slot.sigbit);
    }
    g_alert_slot.active = 0;
    g_alert_slot.wm_handle = -1;
    g_alert_slot.guest_win = 0;
    g_alert_slot.sigbit = 0;
    g_alert_slot.sigmask = 0;
    g_alert_slot.task = NULL;
    g_alert_slot.result = 0;
    g_alert_slot.num_substrings = 0;
    g_alert_slot.timeout_frames = 0;
    g_alert_slot.timeout_start = 0;
}

static void alert_set_result(int result)
{
    g_alert_slot.result = result;
    if (g_alert_slot.task && g_alert_slot.sigmask) {
        Signal(g_alert_slot.task, g_alert_slot.sigmask);
    }
}

static void alert_draw_fn(int win_x, int win_y, int win_w, int win_h)
{
    if (!g_alert_slot.active) return;

    /* Black background */
    FB_FillRect(win_x, win_y, win_w, win_h, WB_BLACK);
    /* Coloured border */
    FB_DrawRect(win_x, win_y, win_w, win_h, g_alert_slot.color);
    FB_DrawRect(win_x + 1, win_y + 1, win_w - 2, win_h - 2, g_alert_slot.color);

    /* Render substrings */
    for (int i = 0; i < g_alert_slot.num_substrings; i++) {
        AlertSubstring *s = &g_alert_slot.substrings[i];
        int x = win_x + (int)s->x;
        int y = win_y + (int)s->y;
        if (s->text[0]) {
            FB_PutStr(x, y, s->text, WB_WHITE, WB_BLACK);
        }
    }
}

static int alert_event_handler(int wh, int event_type, int p1, int p2, int p3)
{
    (void)p2;
    (void)p3;
    if (!g_alert_slot.active || g_alert_slot.wm_handle != wh) return 1;

    if (event_type == WM_EVT_CLOSE_REQUEST) {
        alert_set_result(0);
        return 0;
    }

    if (event_type == WM_EVT_MOUSE_DOWN) {
        /* p1 = 0 for left button, 1 for right button */
        alert_set_result(p1 == 0 ? 1 : 0);
        return 0;
    }

    if (event_type == WM_EVT_MOUSE_UP) {
        /* p1 = 0 for left button, 1 for right button */
        alert_set_result(p1 == 0 ? 1 : 0);
        return 0;
    }

    return 0;
}

static int create_alert_window(int x, int y, int w, int h)
{
    int wh = WM_AddWindow(x, y, w, h, "Alert", alert_draw_fn, NULL);
    if (wh < 0) return -1;
    WM_SetEventHandler(wh, alert_event_handler);
    WM_RequestFocus(wh);
    return wh;
}

static void build_alert_guest_window(uint32_t win_ptr)
{
    if (!win_ptr) return;
    memset(&g_ram[win_ptr], 0, sizeof(AmigaWindow));
    mem_w32(win_ptr + WIN_OFF_FLAGS, WFLG_ACTIVATE);
    mem_w16(win_ptr + WIN_OFF_WIDTH, 400);
    mem_w16(win_ptr + WIN_OFF_HEIGHT, 150);
}

static int parse_alert_string(uint32_t str_ptr)
{
    g_alert_slot.num_substrings = 0;
    if (!str_ptr) return 0;

    while (g_alert_slot.num_substrings < ALERT_MAX_SUBSTRINGS) {
        if (str_ptr + 4 >= GUEST_RAM_SIZE) break;
        uint16_t x = (uint16_t)mem_u16(str_ptr);
        uint16_t y = (uint16_t)mem_u8(str_ptr + 2);
        str_ptr += 3;

        /* Read text until null terminator */
        char *dst = g_alert_slot.substrings[g_alert_slot.num_substrings].text;
        int i = 0;
        while (i < ALERT_SUBSTRING_TEXT_SIZE - 1 && str_ptr < GUEST_RAM_SIZE) {
            uint8_t c = mem_u8(str_ptr);
            str_ptr++;
            if (c == 0) break;
            dst[i++] = (char)c;
        }
        dst[i] = '\0';

        g_alert_slot.substrings[g_alert_slot.num_substrings].x = x;
        g_alert_slot.substrings[g_alert_slot.num_substrings].y = y;
        g_alert_slot.num_substrings++;

        if (str_ptr >= GUEST_RAM_SIZE) break;
        uint8_t cont = mem_u8(str_ptr);
        str_ptr++;
        if (cont == 0) break;
    }
    return g_alert_slot.num_substrings;
}

static uint32_t build_alert_internal(uint32_t alert_str, uint32_t alert_num,
                                     int height, int width, uint32_t timeout)
{
    if (g_alert_slot.active) return 0;

    clear_alert_slot();

    int sig = alloc_intuition_signal();
    if (sig < 0) return 0;

    /* Determine alert colour */
    if (alert_num & DEADEND_ALERT)
        g_alert_slot.color = WB_RED;
    else
        g_alert_slot.color = WB_ORANGE;

    /* Parse alert substrings */
    parse_alert_string(alert_str);

    /* Alert window size */
    if (width < 300) width = 300;
    if (height < 80) height = 80;
    if (width > (int)g_fb.width) width = (int)g_fb.width;
    if (height > (int)g_fb.height) height = (int)g_fb.height;

    int x = ((int)g_fb.width - width) / 2;
    int y = ((int)g_fb.height - height) / 2;

    int wh = create_alert_window(x, y, width, height);
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
    build_alert_guest_window(guest_win);

    g_alert_slot.active = 1;
    g_alert_slot.wm_handle = wh;
    g_alert_slot.guest_win = guest_win;
    g_alert_slot.sigbit = (uint8_t)sig;
    g_alert_slot.sigmask = 1U << sig;
    g_alert_slot.task = Task_Current();
    g_alert_slot.timeout_frames = timeout;
    g_alert_slot.timeout_start = g_pit_ticks;

    WM_Redraw();
    return guest_win;
}

static void free_alert_internal(void)
{
    if (!g_alert_slot.active) return;
    if (g_alert_slot.wm_handle >= 0) WM_CloseWindow(g_alert_slot.wm_handle);
    if (g_alert_slot.guest_win) {
        free_window_idcmp(g_alert_slot.guest_win);
        free_gadget_list(mem_u32(g_alert_slot.guest_win + WIN_OFF_FIRSTGADGET));
        uint32_t rport = mem_u32(g_alert_slot.guest_win + WIN_OFF_RPORT);
        intu_free(rport);
        intu_free(g_alert_slot.guest_win);
    }
    clear_alert_slot();
}

static int wait_alert_internal(void)
{
    if (!g_alert_slot.active || !g_alert_slot.task || !g_alert_slot.sigmask) return 0;
    while (g_alert_slot.active) {
        if (g_alert_slot.timeout_frames) {
            uint64_t elapsed = g_pit_ticks - g_alert_slot.timeout_start;
            /* Approximate 50 fps PAL: frames * 2 ticks */
            if (elapsed > (uint64_t)g_alert_slot.timeout_frames * 2) {
                alert_set_result(0);
                break;
            }
        }
        uint32_t sigs = Wait(g_alert_slot.sigmask);
        if (sigs & g_alert_slot.sigmask) break;
    }
    return g_alert_slot.result;
}

/* DisplayAlert(alertNumber, string, height)
 * D0, A0, D1; returns BOOL in D0 */
static void intuition_DisplayAlert(void)
{
    uint32_t alert_num = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t alert_str = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t height    = m68k_get_reg(NULL, M68K_REG_D1);
    int width = 400;

    uint32_t win = build_alert_internal(alert_str, alert_num, (int)height, width, 0);
    if (!win) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }

    if (alert_num & DEADEND_ALERT) {
        /* Deadend alerts display but return immediately with FALSE */
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }

    int result = wait_alert_internal();
    free_alert_internal();
    m68k_set_reg(M68K_REG_D0, result ? 1 : 0);
}

/* TimedDisplayAlert(alertNumber, string, height, time)
 * D0, A0, D1, A1; returns BOOL in D0 */
static void intuition_TimedDisplayAlert(void)
{
    uint32_t alert_num = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t alert_str = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t height    = m68k_get_reg(NULL, M68K_REG_D1);
    uint32_t timeout   = m68k_get_reg(NULL, M68K_REG_A1);
    int width = 400;

    uint32_t win = build_alert_internal(alert_str, alert_num, (int)height, width, timeout);
    if (!win) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }

    if (alert_num & DEADEND_ALERT) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }

    int result = wait_alert_internal();
    free_alert_internal();
    m68k_set_reg(M68K_REG_D0, result ? 1 : 0);
}

/* =========================================================================
 * Screen support
 * ========================================================================= */

#define MAX_INTUITION_SCREENS 4

typedef struct ScreenSlot {
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
    /* Tag storage for SetScreenAttrsA / GetScreenAttrsA */
    uint32_t display_id;
    uint32_t bitmap;
    uint32_t colormap;
    uint32_t colors;
    uint32_t colors32;
    uint32_t sys_font;
    uint32_t pens;
    uint32_t error_code_ptr;
    uint32_t parent;
    uint32_t backfill;
    uint32_t dclip;
    uint32_t overscan;
    uint32_t pub_sig;
    uint32_t pub_task;
    uint16_t color_map_entries;
    uint8_t  depth;
    uint8_t  full_palette;
    uint8_t  draggable;
    uint8_t  exclusive;
    uint8_t  share_pens;
    uint8_t  interleaved;
    uint8_t  like_workbench;
    uint8_t  minimize_isg;
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

static void init_guest_screen(uint32_t scr, ScreenSlot *slot, uint16_t type_flags)
{
    memset(&g_ram[scr], 0, SCR_SIZE);
    mem_w16(scr + SCR_OFF_LEFTEDGE,  slot->left);
    mem_w16(scr + SCR_OFF_TOPEDGE,   slot->top);
    mem_w16(scr + SCR_OFF_WIDTH,     slot->width);
    mem_w16(scr + SCR_OFF_HEIGHT,    slot->height);
    mem_w32(scr + SCR_OFF_TITLE,     (uint32_t)0);
    mem_w32(scr + SCR_OFF_DEFAULTTITLE, (uint32_t)0);
    mem_w16(scr + SCR_OFF_FLAGS,     type_flags);
    mem_w32(scr + SCR_OFF_FIRSTWINDOW, 0);
    mem_w32(scr + SCR_OFF_NEXTSCREEN,  0);
    mem_w8(scr + SCR_OFF_DEPTH,      slot->depth);
    mem_w32(scr + SCR_OFF_BITMA,     slot->bitmap);
    mem_w32(scr + SCR_OFF_DISPLAYID, slot->display_id);
    mem_w32(scr + SCR_OFF_COLORS,    slot->colors);
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

/* Render the front Intuition screen's custom BitMap into the host framebuffer.
 * Returns 1 if a screen bitmap was rendered, 0 otherwise (caller should fall
 * back to its own background). */
int UAOS_Intuition_RenderScreenBackdrop(void)
{
    for (int i = 0; i < MAX_INTUITION_SCREENS; i++) {
        ScreenSlot *slot = &g_intu_screens[i];
        if (!slot->active || !slot->is_front) continue;
        uint32_t screen = slot->guest_screen;
        if (!screen) continue;
        uint32_t bm = slot->bitmap;
        if (bm) {
            uint32_t cmap = 0;
            uint32_t vp = mem_u32(screen + SCR_OFF_VIEWPORT);
            if (vp) cmap = mem_u32(vp + VP_OFF_COLORMAP);
            render_bitmap_to_framebuffer(bm, cmap, slot->left, slot->top,
                                         slot->width, slot->height);
            return 1;
        }
        if (slot->backfill) {
            if (slot->backfill < 256) {
                /* Pen-index fallback for SA_BackFill. */
                uint32_t rgb = amiga_pen_to_rgb((uint8_t)slot->backfill);
                FB_FillRect(0, 0, (int)g_fb.width, (int)g_fb.height, rgb);
            } else {
                /* Real m68k Hook callback: fill the screen backdrop.
                 * A0 = hook, A2 = screen RastPort, A1 = full-screen Rectangle.
                 * Non-Workbench screens do not have a persistent RastPort, so
                 * allocate a temporary one for the hook call. */
                uint32_t rport = mem_u32(screen + SCR_OFF_RASTPORT);
                uint32_t tmp_rport = 0;
                if (!rport) {
                    tmp_rport = intu_alloc(RP_SIZE_MIN);
                    if (tmp_rport) init_guest_rastport(tmp_rport, 0);
                    rport = tmp_rport;
                }
                if (rport) {
                    const uint32_t rect = 0x1EF100u;
                    mem_w16(rect + 0, (int16_t)slot->left);
                    mem_w16(rect + 2, (int16_t)slot->top);
                    mem_w16(rect + 4, (int16_t)(slot->left + slot->width - 1));
                    mem_w16(rect + 6, (int16_t)(slot->top + slot->height - 1));
                    UAOS_InvokeM68kHook(slot->backfill, slot->backfill, rect, rport);
                    if (tmp_rport) intu_free(tmp_rport);
                }
            }
            return 1;
        }
        return 0;
    }
    return 0;
}

/* Compute the allowed display rectangle for a screen based on its stored
 * SA_DClip or SA_Overscan constraints.  Returns 1 if constraints exist,
 * 0 otherwise.  The rectangle is in host framebuffer coordinates. */
static int get_screen_constraints(ScreenSlot *slot,
                                  int *cx, int *cy, int *cw, int *ch)
{
    if (slot->dclip) {
        int16_t minx = mem_s16(slot->dclip + 0);
        int16_t miny = mem_s16(slot->dclip + 2);
        int16_t maxx = mem_s16(slot->dclip + 4);
        int16_t maxy = mem_s16(slot->dclip + 6);
        *cx = minx;
        *cy = miny;
        *cw = maxx - minx + 1;
        *ch = maxy - miny + 1;
        return 1;
    }
    if (slot->overscan) {
        int dw = (int)g_fb.width;
        int dh = (int)g_fb.height;
        *cx = 0;
        *cy = 0;
        *cw = dw;
        *ch = dh;
        switch (slot->overscan) {
            case OSCAN_TEXT:
                *cw = dw * 90 / 100;
                *ch = dh * 90 / 100;
                break;
            case OSCAN_STANDARD:
                *cw = dw * 95 / 100;
                *ch = dh * 95 / 100;
                break;
            case OSCAN_MAX:
                *cw = dw * 98 / 100;
                *ch = dh * 98 / 100;
                break;
            case OSCAN_VIDEO:
                *cw = dw * 105 / 100;
                *ch = dh * 105 / 100;
                break;
        }
        *cx = (dw - *cw) / 2;
        *cy = (dh - *ch) / 2;
        return 1;
    }
    return 0;
}

/* Clamp a screen's position and size so it lies within the constraint
 * rectangle (cx,cy,cw,ch).  Width and height are clipped to the rectangle,
 * and left/top are clamped so the screen stays inside. */
static void clamp_screen_to_constraints(ScreenSlot *slot, int cx, int cy, int cw, int ch)
{
    if (slot->width > cw) slot->width = (int16_t)cw;
    if (slot->height > ch) slot->height = (int16_t)ch;
    if (slot->left < cx) slot->left = (int16_t)cx;
    if (slot->top < cy) slot->top = (int16_t)cy;
    if (slot->left + slot->width > cx + cw)
        slot->left = (int16_t)(cx + cw - slot->width);
    if (slot->top + slot->height > cy + ch)
        slot->top = (int16_t)(cy + ch - slot->height);
}

/* Forward declaration — defined later near the SetPrefs implementation. */
static uint32_t scale_rgb(uint32_t rgb, int num, int den);

/* Determine how many palette entries a screen should expose, honouring
 * SA_FullPalette and SA_ColorMapEntries.  Capped at 32 entries. */
static int screen_palette_count(ScreenSlot *slot)
{
    int count = 16;
    if (slot->full_palette) count = 32;
    if (slot->color_map_entries > 0 && slot->color_map_entries < count)
        count = slot->color_map_entries;
    if (count > 32) count = 32;
    return count;
}

/* Extract the screen's palette from SA_Colors and SA_Colors32 into a
 * local RGB table.  Unspecified pens keep the default Amiga palette. */
static void extract_screen_palette(ScreenSlot *slot, uint32_t *palette, int max_colors)
{
    for (int i = 0; i < max_colors; i++)
        palette[i] = amiga_pen_to_rgb((uint8_t)i);

    if (slot->colors) {
        /* SA_Colors: ColorSpec array (cs_Buffer, cs_UnRed, cs_UnGreen, cs_UnBlue).
         * Terminated by cs_Buffer == -1. */
        uint32_t p = slot->colors;
        while (p + CS_SIZE <= GUEST_RAM_SIZE) {
            int16_t pen = mem_s16(p + CS_OFF_BUFFER);
            if (pen < 0 || pen >= max_colors) break;
            uint32_t r = mem_u16(p + CS_OFF_RED)   >> 8;
            uint32_t g = mem_u16(p + CS_OFF_GREEN) >> 8;
            uint32_t b = mem_u16(p + CS_OFF_BLUE)  >> 8;
            palette[pen] = FB_RGB(r, g, b);
            p += CS_SIZE;
        }
    }

    if (slot->colors32) {
        /* SA_Colors32: LoadRGB32-style table: count, then r/g/b triples.
         * Each component is in the high byte of a ULONG. */
        uint32_t p = slot->colors32;
        if (p + 4 <= GUEST_RAM_SIZE) {
            uint32_t count = mem_u32(p);
            p += 4;
            if (count > (uint32_t)max_colors) count = max_colors;
            for (uint32_t i = 0; i < count && p + 12 <= GUEST_RAM_SIZE; i++) {
                uint32_t r = mem_u32(p)     >> 24;
                uint32_t g = mem_u32(p + 4) >> 24;
                uint32_t b = mem_u32(p + 8) >> 24;
                palette[i] = FB_RGB(r, g, b);
                p += 12;
            }
        }
    }
}

/* Apply a screen's palette and SA_Pens to the runtime WB_* palette used by
 * the host desktop and WM chrome.  Pens override the default first-four-color
 * mapping when a valid SA_Pens array is supplied. */
static void apply_screen_palette(ScreenSlot *slot)
{
    if (!slot) return;

    int count = screen_palette_count(slot);
    uint32_t palette[32];
    extract_screen_palette(slot, palette, count);

    uint32_t grey  = palette[0];
    uint32_t black = palette[1];
    uint32_t white = palette[2];
    uint32_t blue  = palette[3];
    uint32_t dark_grey = scale_rgb(grey, 1, 2);

    if (slot->pens) {
        uint32_t p = slot->pens;
        uint32_t bg = mem_u16(p + DRI_BACKGROUNDPEN * 2);
        uint32_t text = mem_u16(p + DRI_TEXTPEN * 2);
        uint32_t shine = mem_u16(p + DRI_SHINEPEN * 2);
        uint32_t shadow = mem_u16(p + DRI_SHADOWPEN * 2);
        uint32_t fill = mem_u16(p + DRI_FILLPEN * 2);
        uint32_t filltext = mem_u16(p + DRI_FILLTEXTPEN * 2);
        if (bg < 16)       grey = palette[bg];
        if (text < 16)     black = palette[text];
        if (shine < 16)    white = palette[shine];
        if (shadow < 16)   dark_grey = palette[shadow];
        if (fill < 16)     blue = palette[fill];
        if (filltext < 16) white = palette[filltext];
    }

    WB_GREY       = grey;
    WB_BLACK      = black;
    WB_WHITE      = white;
    WB_BLUE       = blue;
    WB_DARK_GREY  = dark_grey;
    WB_LIGHT_GREY = scale_rgb(grey, 3, 2);
    WB_LIGHT_BLUE = scale_rgb(blue, 3, 2);
    WB_CREAM      = (white == FB_RGB(0,0,0)) ? FB_RGB(0xFF,0xFF,0xCC) : white;

    /* Keep distinctive defaults for accents not covered by Workbench pens. */
    WB_RED   = palette[5] ? palette[5] : FB_RGB(0xCC,0x00,0x00);
    WB_ORANGE = palette[6] ? palette[6] : FB_RGB(0xFF,0x88,0x00);
    WB_GREEN = palette[7] ? palette[7] : FB_RGB(0x00,0xAA,0x00);
}

/* Apply the frontmost screen's palette to the host desktop palette. */
void UAOS_Intuition_ApplyFrontScreenPalette(void)
{
    ScreenSlot *front = NULL;
    for (int i = 0; i < MAX_INTUITION_SCREENS; i++) {
        if (g_intu_screens[i].active && g_intu_screens[i].is_front) {
            front = &g_intu_screens[i];
            break;
        }
    }
    if (front && (front->colors || front->colors32 || front->pens))
        apply_screen_palette(front);
    else
        WB_InitPalette();
}

/* WM palette callback: apply the palette of the screen the window lives on. */
static void intuition_apply_window_palette(int wm_handle)
{
    uint32_t win_ptr = get_guest_window_from_handle(wm_handle);
    if (!win_ptr) return;
    uint32_t screen = mem_u32(win_ptr + WIN_OFF_WSCREEN);
    if (!screen) return;
    ScreenSlot *slot = find_screen_slot(screen);
    if (slot && (slot->colors || slot->colors32 || slot->pens))
        apply_screen_palette(slot);
    else
        WB_InitPalette();
}

static void signal_pub_screen(ScreenSlot *slot);
static void apply_parent_screen_defaults(uint32_t parent_screen,
                                         int16_t *width, int16_t *height,
                                         int16_t *depth,
                                         uint8_t *detail_pen, uint8_t *block_pen);
static uint32_t find_pub_screen_by_name(const char *name);

static uint32_t open_screen_internal(uint32_t new_screen_ptr, uint32_t tag_list_ptr)
{
    int16_t left = 0, top = 0, width = 0, height = 0, depth = 2;
    uint8_t detail_pen = 0, block_pen = 1;
    uint32_t title_ptr = 0, font_ptr = 0, pub_name_ptr = 0;
    uint32_t bitmap_ptr = 0, display_id = 0, colors_ptr = 0, colors32_ptr = 0;
    uint32_t sys_font = 0, pens_ptr = 0, error_code_ptr = 0, parent_ptr = 0;
    uint32_t backfill_ptr = 0, dclip_ptr = 0, overscan = 0;
    uint32_t pub_sig = 0, pub_task = 0;
    uint16_t color_map_entries = 0;
    uint8_t show_title = 1, full_palette = 0, draggable = 1, exclusive = 0;
    uint8_t share_pens = 0, interleaved = 0, like_workbench = 0, minimize_isg = 0;
    uint16_t type = CUSTOMSCREEN;

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
                case SA_AutoScroll: if (data) type |= AUTOSCROLL; break;
                case SA_PubName:    pub_name_ptr = data; break;
                case SA_BitMap:     bitmap_ptr = data; if (data) type |= CUSTOMBITMAP; break;
                case SA_DisplayID:  display_id = data; break;
                case SA_Colors:     colors_ptr = data; break;
                case SA_Colors32:   colors32_ptr = data; break;
                case SA_SysFont:    sys_font = data; break;
                case SA_Pens:       pens_ptr = data; break;
                case SA_ErrorCode:  error_code_ptr = data; break;
                case SA_Parent:     parent_ptr = data; break;
                case SA_BackFill:   backfill_ptr = data; break;
                case SA_DClip:      dclip_ptr = data; break;
                case SA_Overscan:   overscan = data; break;
                case SA_PubSig:     pub_sig = data; break;
                case SA_PubTask:    pub_task = data; break;
                case SA_FullPalette: full_palette = data ? 1 : 0; break;
                case SA_ColorMapEntries: color_map_entries = (uint16_t)data; break;
                case SA_Draggable:  draggable = data ? 1 : 0; break;
                case SA_Exclusive:  exclusive = data ? 1 : 0; break;
                case SA_SharePens:  share_pens = data ? 1 : 0; break;
                case SA_Interleaved: interleaved = data ? 1 : 0; break;
                case SA_LikeWorkbench: like_workbench = data ? 1 : 0; break;
                case SA_MinimizeISG: minimize_isg = data ? 1 : 0; break;
                default: break;
            }
        }
    }

    /* SA_DClip and SA_Overscan determine the screen's display rectangle.
     * They override explicit geometry and are later used to constrain moves
     * and position changes. */
    if (dclip_ptr || overscan) {
        int16_t cx = 0, cy = 0, cw = 0, ch = 0;
        if (dclip_ptr) {
            cx = mem_s16(dclip_ptr + 0);
            cy = mem_s16(dclip_ptr + 2);
            cw = mem_s16(dclip_ptr + 4) - cx + 1;
            ch = mem_s16(dclip_ptr + 6) - cy + 1;
        } else {
            int dw = (int)g_fb.width;
            int dh = (int)g_fb.height;
            cw = dw;
            ch = dh;
            switch (overscan) {
                case OSCAN_TEXT:     cw = dw * 90 / 100; ch = dh * 90 / 100; break;
                case OSCAN_STANDARD: cw = dw * 95 / 100; ch = dh * 95 / 100; break;
                case OSCAN_MAX:      cw = dw * 98 / 100; ch = dh * 98 / 100; break;
                case OSCAN_VIDEO:    cw = dw * 105 / 100; ch = dh * 105 / 100; break;
            }
            cx = (dw - cw) / 2;
            cy = (dh - ch) / 2;
        }
        if (cw > 0 && ch > 0) {
            left   = cx;
            top    = cy;
            width  = cw;
            height = ch;
        }
    }

    /* Inherit geometry and default pens from the parent screen if one was
     * supplied and the child did not explicitly provide them. */
    apply_parent_screen_defaults(parent_ptr, &width, &height, &depth,
                                 &detail_pen, &block_pen);

    if (width <= 0)  width  = (int16_t)g_fb.width;
    if (height <= 0) height = (int16_t)g_fb.height;

    /* Clamp explicit/parent/default geometry to DClip/Overscan constraints. */
    if (dclip_ptr || overscan) {
        int cx = 0, cy = 0, cw = 0, ch = 0;
        if (dclip_ptr) {
            cx = mem_s16(dclip_ptr + 0);
            cy = mem_s16(dclip_ptr + 2);
            cw = mem_s16(dclip_ptr + 4) - cx + 1;
            ch = mem_s16(dclip_ptr + 6) - cy + 1;
        } else {
            int dw = (int)g_fb.width;
            int dh = (int)g_fb.height;
            cw = dw;
            ch = dh;
            switch (overscan) {
                case OSCAN_TEXT:     cw = dw * 90 / 100; ch = dh * 90 / 100; break;
                case OSCAN_STANDARD: cw = dw * 95 / 100; ch = dh * 95 / 100; break;
                case OSCAN_MAX:      cw = dw * 98 / 100; ch = dh * 98 / 100; break;
                case OSCAN_VIDEO:    cw = dw * 105 / 100; ch = dh * 105 / 100; break;
            }
            cx = (dw - cw) / 2;
            cy = (dh - ch) / 2;
        }
        if (cw > 0 && ch > 0) {
            if (width > cw)  width  = (int16_t)cw;
            if (height > ch) height = (int16_t)ch;
            if (left < cx) left = (int16_t)cx;
            if (top < cy)  top  = (int16_t)cy;
            if (left + width > cx + cw)
                left = (int16_t)(cx + cw - width);
            if (top + height > cy + ch)
                top = (int16_t)(cy + ch - height);
        }
    }

    /* Reject a public-screen name that is already in use. */
    if (pub_name_ptr && pub_name_ptr < GUEST_RAM_SIZE) {
        char test_name[64] = "";
        guest_str(test_name, pub_name_ptr, sizeof(test_name));
        if (find_pub_screen_by_name(test_name)) {
            if (error_code_ptr && error_code_ptr + 4 <= GUEST_RAM_SIZE)
                mem_w32(error_code_ptr, 5); /* OSERR_PUBNOTUNIQUE */
            return 0;
        }
    }

    ScreenSlot *slot = alloc_screen_slot();
    if (!slot) {
        if (error_code_ptr && error_code_ptr + 4 <= GUEST_RAM_SIZE)
            mem_w32(error_code_ptr, 3); /* OSERR_NOMEM */
        return 0;
    }

    uint32_t guest_screen = intu_alloc(SCR_SIZE);
    if (!guest_screen) {
        slot->active = 0;
        if (error_code_ptr && error_code_ptr + 4 <= GUEST_RAM_SIZE)
            mem_w32(error_code_ptr, 3); /* OSERR_NOMEM */
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

    slot->display_id       = display_id;
    slot->bitmap           = bitmap_ptr;
    /* If SA_Interleaved is requested with a custom bitmap, set the
     * BMF_INTERLEAVED bit (0x01) in the guest BitMap flags. */
    if (bitmap_ptr && interleaved && bitmap_ptr + BM_OFF_FLAGS < GUEST_RAM_SIZE)
        mem_w8(bitmap_ptr + BM_OFF_FLAGS, (uint8_t)(mem_u8(bitmap_ptr + BM_OFF_FLAGS) | 1));
    slot->colormap         = 0;
    slot->colors           = colors_ptr;
    slot->colors32         = colors32_ptr;
    slot->sys_font         = sys_font;
    slot->pens             = pens_ptr;
    slot->error_code_ptr    = error_code_ptr;
    slot->parent            = parent_ptr;
    slot->backfill          = backfill_ptr;
    slot->dclip             = dclip_ptr;
    slot->overscan          = overscan;
    slot->pub_sig           = pub_sig;
    slot->pub_task          = pub_task;
    slot->color_map_entries = color_map_entries;
    slot->depth             = (uint8_t)depth;
    slot->full_palette      = full_palette;
    slot->draggable         = draggable;
    slot->exclusive         = exclusive;
    slot->share_pens        = share_pens;
    slot->interleaved       = interleaved;
    slot->like_workbench    = like_workbench;
    slot->minimize_isg      = minimize_isg;

    uint16_t screen_flags = type | (show_title ? SHOWTITLE : 0);
    init_guest_screen(guest_screen, slot, screen_flags);
    if (title_ptr)
        mem_w32(guest_screen + SCR_OFF_TITLE, title_ptr);
    if (font_ptr)
        mem_w32(guest_screen + SCR_OFF_FONT, font_ptr);
    mem_w8(guest_screen + SCR_OFF_DETAILPEN, detail_pen);
    mem_w8(guest_screen + SCR_OFF_BLOCKPEN, block_pen);

    /* Mark this screen as the front screen; others go behind */
    for (int i = 0; i < MAX_INTUITION_SCREENS; i++) {
        if (g_intu_screens[i].active && &g_intu_screens[i] != slot)
            g_intu_screens[i].is_front = 0;
    }
    update_desktop_title();
    if (pub_name_ptr) signal_pub_screen(slot);
    return guest_screen;
}

/* If a parent screen is supplied, inherit its geometry and default pens when
 * the child screen did not explicitly provide them. */
static void apply_parent_screen_defaults(uint32_t parent_screen,
                                         int16_t *width, int16_t *height,
                                         int16_t *depth,
                                         uint8_t *detail_pen, uint8_t *block_pen)
{
    if (!parent_screen) return;
    ScreenSlot *parent_slot = find_screen_slot(parent_screen);
    if (!parent_slot) return;

    if (*width <= 0)  *width  = mem_s16(parent_screen + SCR_OFF_WIDTH);
    if (*height <= 0) *height = mem_s16(parent_screen + SCR_OFF_HEIGHT);
    if (*depth <= 0)  *depth  = mem_s16(parent_screen + SCR_OFF_DEPTH);
    /* Only inherit pens if the child did not explicitly set them.
     * detail_pen==0/block_pen==1 are the built-in defaults; we treat them as
     * "not explicitly set" and inherit the parent's pens. */
    if (*detail_pen == 0) *detail_pen = mem_u8(parent_screen + SCR_OFF_DETAILPEN);
    if (*block_pen == 1) *block_pen = mem_u8(parent_screen + SCR_OFF_BLOCKPEN);
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

/* Signal the public screen's notification task when its status changes. */
static void signal_pub_screen(ScreenSlot *slot)
{
    if (!slot || !slot->pub_task || !slot->pub_sig) return;
    UaosTask *t = Task_FindByM68kAddr(slot->pub_task);
    if (t) Signal(t, slot->pub_sig);
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
#define INTUITION_QUERY_OVERSCAN         46
#define INTUITION_GET_DISPLAY_INFO_DATA  47
#define INTUITION_NEXT_DISPLAY_INFO      48
#define INTUITION_CURRENT_TIME           49
#define INTUITION_DOUBLE_CLICK           50
#define INTUITION_REPORT_MOUSE           51
#define INTUITION_DISPLAY_BEEP           52
#define INTUITION_INIT_REQUESTER         53
#define INTUITION_END_REQUEST            54
#define INTUITION_REQUEST                55
#define INTUITION_VIEW_ADDRESS           56
#define INTUITION_VIEW_PORT_ADDRESS      57
#define INTUITION_GET_SCREEN_DATA        58
#define INTUITION_NEXT_PUB_SCREEN        59
#define INTUITION_SET_DEFAULT_PUB_SCREEN 60
#define INTUITION_LOCK_IBASE             61
#define INTUITION_UNLOCK_IBASE           62
#define INTUITION_SHOW_WINDOW            63
#define INTUITION_HIDE_WINDOW            64
#define INTUITION_WINDOW_LIMITS          65
#define INTUITION_CHANGE_WINDOW_BOX      66
#define INTUITION_GET_SCREEN_DRAW_INFO   67
#define INTUITION_FREE_SCREEN_DRAW_INFO  68
#define INTUITION_DISPLAY_ALERT          69
#define INTUITION_TIMED_DISPLAY_ALERT    70
#define INTUITION_SCREEN_DEPTH           71
#define INTUITION_SCREEN_POSITION        72
#define INTUITION_ADD_GADGET             73
#define INTUITION_ADD_GLIST              74
#define INTUITION_REMOVE_GADGET          75
#define INTUITION_REMOVE_GLIST           76
#define INTUITION_REFRESH_GLIST          77
#define INTUITION_ON_GADGET              78
#define INTUITION_OFF_GADGET             79
#define INTUITION_MODIFY_PROP            80
#define INTUITION_NEW_MODIFY_PROP        81
#define INTUITION_ACTIVATE_GADGET        82
#define INTUITION_SET_WINDOW_ATTRS_A     83
#define INTUITION_GET_WINDOW_ATTRS_A     84
#define INTUITION_SET_SCREEN_ATTRS_A     85
#define INTUITION_GET_SCREEN_ATTRS_A     86
#define INTUITION_GET_VISUAL_INFO_A      87
#define INTUITION_FREE_VISUAL_INFO       88
#define INTUITION_BEGIN_REFRESH          89
#define INTUITION_END_REFRESH            90
#define INTUITION_REFRESH_GADGETS        91
#define INTUITION_ON_MENU                92
#define INTUITION_OFF_MENU               93
#define INTUITION_SYS_REQ_HANDLER        94
#define INTUITION_PUB_SCREEN_STATUS      95
#define INTUITION_GET_DEFAULT_PUB_SCREEN 96
#define INTUITION_MOVE_WINDOW_IN_FRONT_OF 97
#define INTUITION_SET_EDIT_HOOK          98
#define INTUITION_OBTAIN_GIR_PORT        99
#define INTUITION_RELEASE_GIR_PORT       100
#define INTUITION_STRIP_INTUI_MESSAGES   101
#define INTUITION_NEW_OBJECT_A           102
#define INTUITION_DISPOSE_OBJECT          103
#define INTUITION_SET_ATTRS_A            104
#define INTUITION_GET_ATTR               105
#define INTUITION_DO_METHOD_A            106
#define INTUITION_DO_SUPER_METHOD_A      107
#define INTUITION_COERCE_METHOD_A        108
#define INTUITION_MAKE_CLASS              109
#define INTUITION_FREE_CLASS              110

/* =========================================================================
 * Implementation
 * ========================================================================= */

static void intuition_OpenLibrary(void)
{
    /* Lazily allocate the single guest View returned by ViewAddress().
     * It is intentionally never freed; it persists for the library lifetime. */
    if (!g_intu_view)
        g_intu_view = intu_alloc(64);
}

static void intuition_CloseLibrary(void)
{
    /* no-op */
}

/* Parse OpenWindowTagList() tag list and update window parameters.
 * Handles the full WA_* tag set; boolean tags update the corresponding
 * WFLG_* bit in the accumulated flags word. */
static void parse_window_tags(uint32_t tag_list,
    int16_t *left, int16_t *top, int16_t *width, int16_t *height,
    uint32_t *flags, uint32_t *idcmp, uint32_t *title_ptr,
    int16_t *min_w, int16_t *min_h, int16_t *max_w, int16_t *max_h,
    uint32_t *pub_screen_ptr, uint32_t *pub_screen_name_ptr,
    uint8_t *pub_screen_fallback, uint32_t *super_bitmap_ptr,
    uint32_t *colors, uint32_t *checkmark, uint32_t *amiga_key,
    uint8_t *menu_help, uint8_t *tablet_messages, uint8_t *auto_adjust,
    uint8_t *notify_depth, uint32_t *pointer_delay)
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
            case WA_Flags:     *flags     = data; break;
            case WA_IDCMP:     *idcmp     = data; break;
            case WA_Title:     *title_ptr = data; break;
            case WA_MinWidth:  *min_w     = (int16_t)(int32_t)data; break;
            case WA_MinHeight: *min_h     = (int16_t)(int32_t)data; break;
            case WA_MaxWidth:  *max_w     = (int16_t)(int32_t)data; break;
            case WA_MaxHeight: *max_h     = (int16_t)(int32_t)data; break;
            case WA_PubScreen:
            case WA_CustomScreen:
                *pub_screen_ptr = data; break;
            case WA_PubScreenName:   *pub_screen_name_ptr = data; break;
            case WA_PubScreenFallBack: *pub_screen_fallback = data ? 1 : 0; break;
            case WA_SizeGadget:  if (data) *flags |= WFLG_SIZEGADGET;  else *flags &= ~WFLG_SIZEGADGET; break;
            case WA_DragBar:     if (data) *flags |= WFLG_DRAGBAR;     else *flags &= ~WFLG_DRAGBAR; break;
            case WA_DepthGadget: if (data) *flags |= WFLG_DEPTHGADGET; else *flags &= ~WFLG_DEPTHGADGET; break;
            case WA_CloseGadget: if (data) *flags |= WFLG_CLOSEGADGET; else *flags &= ~WFLG_CLOSEGADGET; break;
            case WA_Backdrop:    if (data) *flags |= WFLG_BACKDROP;    else *flags &= ~WFLG_BACKDROP; break;
            case WA_ReportMouse: if (data) *flags |= WFLG_REPORTMOUSE; else *flags &= ~WFLG_REPORTMOUSE; break;
            case WA_NoCareRefresh: if (data) *flags |= WFLG_NOCAREREFRESH; else *flags &= ~WFLG_NOCAREREFRESH; break;
            case WA_Borderless:  if (data) *flags |= WFLG_BORDERLESS;  else *flags &= ~WFLG_BORDERLESS; break;
            case WA_GimmeZeroZero: if (data) *flags |= WFLG_GIMMEZEROZERO; else *flags &= ~WFLG_GIMMEZEROZERO; break;
            case WA_Activate:    if (data) *flags |= WFLG_ACTIVATE;    else *flags &= ~WFLG_ACTIVATE; break;
            case WA_RMBTrap:     if (data) *flags |= WFLG_RMBTRAP;     else *flags &= ~WFLG_RMBTRAP; break;
            case WA_SimpleRefresh: if (data) *flags = (*flags & ~WFLG_REFRESHBITS) | WFLG_SIMPLE_REFRESH; break;
            case WA_SmartRefresh: if (data) *flags = (*flags & ~WFLG_REFRESHBITS) | WFLG_SMART_REFRESH; break;
            case WA_SuperBitMap:
                if (data) {
                    *flags = (*flags & ~WFLG_REFRESHBITS) | WFLG_SUPER_BITMAP;
                    *super_bitmap_ptr = data;
                }
                break;
            case WA_SizeBRight:  if (data) *flags |= WFLG_SIZEBRIGHT;  else *flags &= ~WFLG_SIZEBRIGHT; break;
            case WA_SizeBBottom: if (data) *flags |= WFLG_SIZEBBOTTOM; else *flags &= ~WFLG_SIZEBBOTTOM; break;
            case WA_NewLookMenus: if (data) *flags |= WFLG_NEWLOOKMENUS; else *flags &= ~WFLG_NEWLOOKMENUS; break;
            case WA_Gadgets:     /* not wired in this path */ break;

            /* Stored tag values for GetWindowAttrsA / SetWindowAttrsA */
            case WA_Colors:          if (colors)          *colors          = data; break;
            case WA_Checkmark:       if (checkmark)       *checkmark       = data; break;
            case WA_AmigaKey:        if (amiga_key)       *amiga_key       = data; break;
            case WA_MenuHelp:        if (menu_help)       *menu_help       = data ? 1 : 0; break;
            case WA_TabletMessages:  if (tablet_messages) *tablet_messages = data ? 1 : 0; break;
            case WA_AutoAdjust:      if (auto_adjust)     *auto_adjust     = data ? 1 : 0; break;
            case WA_NotifyDepth:     if (notify_depth)    *notify_depth    = data ? 1 : 0; break;
            case WA_PointerDelay:    if (pointer_delay)   *pointer_delay   = data; break;
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
    uint32_t flags = 0;
    uint32_t idcmp = 0;
    uint32_t title_ptr = 0;
    int16_t  min_w = 0, min_h = 0, max_w = 0, max_h = 0;
    uint32_t first_gadget = 0;
    uint32_t pub_screen_ptr = 0, pub_screen_name_ptr = 0;
    uint8_t  pub_screen_fallback = 0;
    uint32_t super_bitmap = 0;
    uint32_t colors = 0, checkmark = 0, amiga_key = 0, pointer_delay = 0;
    uint8_t  menu_help = 0, tablet_messages = 0, auto_adjust = 0, notify_depth = 0;

    if (nw_ptr) {
        left         = mem_s16(nw_ptr + 0);
        top          = mem_s16(nw_ptr + 2);
        width        = mem_s16(nw_ptr + 4);
        height       = mem_s16(nw_ptr + 6);
        idcmp        = mem_u16(nw_ptr + 10);
        flags        = mem_u16(nw_ptr + 12);
        first_gadget = mem_u32(nw_ptr + 14);
        checkmark    = mem_u32(nw_ptr + 18);
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
                          &pub_screen_fallback, &super_bitmap,
                          &colors, &checkmark, &amiga_key,
                          &menu_help, &tablet_messages, &auto_adjust,
                          &notify_depth, &pointer_delay);
    }
    if (tablet_messages) idcmp |= IDCMP_TABLET;
    if (menu_help) idcmp |= IDCMP_MENUHELP;

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

    if (auto_adjust) {
        auto_adjust_window_geometry(&left, &top, width, height, wscreen);
    }

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
    if (rp_ptr) {
        init_guest_rastport(rp_ptr, win_ptr);
        /* WA_SuperBitMap: the window renders into its own backing BitMap. */
        if (super_bitmap) {
            mem_w32(rp_ptr + RP_OFF_BITMAP, super_bitmap);
            slot->super_bitmap = super_bitmap;
        }
    }

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

    /* Store miscellaneous WA_* tag values for later GetWindowAttrsA queries. */
    slot->colors           = colors;
    slot->checkmark        = checkmark;
    slot->amiga_key        = amiga_key;
    slot->menu_help        = menu_help;
    slot->tablet_messages  = tablet_messages;
    slot->auto_adjust      = auto_adjust;
    slot->notify_depth     = notify_depth;
    slot->pointer_delay    = pointer_delay;

    /* Refresh mode: SmartRefresh is the default.  SuperBitMap disables
     * simple refresh, since the application provides its own bitmap. */
    slot->simple_refresh = (flags & WFLG_SIMPLE_REFRESH) ? 1 : 0;
    if (flags & WFLG_SUPER_BITMAP) slot->simple_refresh = 0;
    slot->gimme_zero_zero = (flags & WFLG_GIMMEZEROZERO) ? 1 : 0;
    if (flags & WFLG_BORDERLESS) {
        slot->border_left = slot->border_top = slot->border_right = slot->border_bottom = 0;
    } else {
        slot->border_left   = WM_BORDER;
        slot->border_top    = WM_TITLEBAR_H;
        slot->border_right  = WM_BORDER;
        slot->border_bottom = WM_BORDER;
    }

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
        int wm_handle = slot->wm_handle;
        free_window_idcmp(win_ptr);
        free_gadget_list(mem_u32(win_ptr + WIN_OFF_FIRSTGADGET));
        uint32_t rport = mem_u32(win_ptr + WIN_OFF_RPORT);
        intu_free(rport);
        intu_free(win_ptr);
        WM_CloseWindow(wm_handle);
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

/* BeginRefresh(window) — A0 = window
 * Marks the start of a refresh cycle.  The damage region is the window
 * content area; the guest can now redraw only the damaged portion. */
static void intuition_BeginRefresh(void)
{
    uint32_t win_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    IntuitionSlot *slot = find_slot_by_guest(win_ptr);
    if (!slot) return;

    slot->refreshing = 1;
    int wx = (int)mem_s16(win_ptr + WIN_OFF_LEFTEDGE);
    int wy = (int)mem_s16(win_ptr + WIN_OFF_TOPEDGE);
    int ww = (int)mem_s16(win_ptr + WIN_OFF_WIDTH);
    int wh = (int)mem_s16(win_ptr + WIN_OFF_HEIGHT);
    slot->damage_x = wx + slot->border_left;
    slot->damage_y = wy + slot->border_top;
    slot->damage_w = ww - slot->border_left - slot->border_right;
    if (slot->damage_w < 0) slot->damage_w = 0;
    slot->damage_h = wh - slot->border_top - slot->border_bottom;
    if (slot->damage_h < 0) slot->damage_h = 0;
}

/* EndRefresh(window, complete) — A0 = window, D0 = complete
 * Clears the refresh state.  If complete is non-zero the damage region
 * is discarded. */
static void intuition_EndRefresh(void)
{
    uint32_t win_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t complete = m68k_get_reg(NULL, M68K_REG_D0);
    IntuitionSlot *slot = find_slot_by_guest(win_ptr);
    if (!slot) return;

    slot->refreshing = 0;
    if (complete) {
        slot->damage_x = slot->damage_y = slot->damage_w = slot->damage_h = 0;
    }
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

/* Set/clear a single WFLG_* bit in Window.Flags from a boolean WA_* tag. */
static void wa_set_flag(uint32_t win_ptr, uint32_t data, uint32_t flag)
{
    uint32_t flags = mem_u32(win_ptr + WIN_OFF_FLAGS);
    if (data) flags |= flag;
    else      flags &= ~flag;
    mem_w32(win_ptr + WIN_OFF_FLAGS, flags);
}

/* SetWindowAttrsA(window, tagList) — A0, A1; returns success in D0 */
static void intuition_SetWindowAttrsA(void)
{
    uint32_t win_ptr  = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t tag_list = m68k_get_reg(NULL, M68K_REG_A1);
    if (!win_ptr) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }

    IntuitionSlot *slot = find_slot_by_guest(win_ptr);
    uint32_t idcmp = mem_u32(win_ptr + WIN_OFF_IDCMPFLAGS);
    int moved = 0, resized = 0, redraw = 0;

    for (uint32_t t = tag_list; t; t += 8) {
        uint32_t tag  = mem_u32(t + 0);
        uint32_t data = mem_u32(t + 4);
        if (tag == TAG_DONE) break;

        uint32_t flags = mem_u32(win_ptr + WIN_OFF_FLAGS);
        switch (tag) {
            case WA_Left:
                mem_w16(win_ptr + WIN_OFF_LEFTEDGE, (int16_t)data);
                moved = 1;
                break;
            case WA_Top:
                mem_w16(win_ptr + WIN_OFF_TOPEDGE, (int16_t)data);
                moved = 1;
                break;
            case WA_Width:
                mem_w16(win_ptr + WIN_OFF_WIDTH, (int16_t)data);
                resized = 1;
                break;
            case WA_Height:
                mem_w16(win_ptr + WIN_OFF_HEIGHT, (int16_t)data);
                resized = 1;
                break;
            case WA_Title:
                mem_w32(win_ptr + WIN_OFF_TITLE, data);
                if (slot && data) {
                    char title[32];
                    guest_str(title, data, sizeof(title));
                    WM_SetWindowTitle(slot->wm_handle, title);
                }
                break;
            case WA_ScreenTitle:
                if (slot) slot->screen_title = data;
                break;
            case WA_IDCMP:
                mem_w32(win_ptr + WIN_OFF_IDCMPFLAGS, data);
                break;
            case WA_Flags:
                mem_w32(win_ptr + WIN_OFF_FLAGS, data);
                break;
            case WA_MinWidth:
                if (slot) slot->min_w = (int16_t)data;
                break;
            case WA_MinHeight:
                if (slot) slot->min_h = (int16_t)data;
                break;
            case WA_MaxWidth:
                if (slot) slot->max_w = (int16_t)data;
                break;
            case WA_MaxHeight:
                if (slot) slot->max_h = (int16_t)data;
                break;
            case WA_Gadgets:
                mem_w32(win_ptr + WIN_OFF_FIRSTGADGET, data);
                redraw = 1;
                break;
            case WA_Checkmark:
                if (slot) slot->checkmark = data;
                break;
            case WA_DetailPen:
                mem_w8(win_ptr + WIN_OFF_DETAILPEN, (uint8_t)data);
                break;
            case WA_BlockPen:
                mem_w8(win_ptr + WIN_OFF_BLOCKPEN, (uint8_t)data);
                break;
            case WA_InnerWidth:
                if (slot) slot->inner_width = (uint16_t)data;
                break;
            case WA_InnerHeight:
                if (slot) slot->inner_height = (uint16_t)data;
                break;
            case WA_PubScreen:
            case WA_CustomScreen:
                mem_w32(win_ptr + WIN_OFF_WSCREEN, data);
                if (slot) slot->pub_screen = data;
                break;
            case WA_PubScreenName:
                if (slot && data) {
                    char name[64];
                    guest_str(name, data, sizeof(name));
                    slot->pub_screen_name = data;
                    uint32_t ps = find_pub_screen_by_name(name);
                    if (ps) {
                        mem_w32(win_ptr + WIN_OFF_WSCREEN, ps);
                        slot->pub_screen = ps;
                    } else if (slot->pub_screen_fallback) {
                        ps = get_default_pub_screen();
                        if (ps) {
                            mem_w32(win_ptr + WIN_OFF_WSCREEN, ps);
                            slot->pub_screen = ps;
                        }
                    }
                }
                break;
            case WA_PubScreenFallBack:
                if (slot) slot->pub_screen_fallback = data ? 1 : 0;
                break;
            case WA_WindowName:
                if (slot) slot->window_name = data;
                break;
            case WA_Colors:
                if (slot) slot->colors = data;
                break;
            case WA_Zoom:
                if (slot) slot->zoom = data;
                break;
            case WA_MouseQueue:
                if (slot) slot->mouse_queue = (uint16_t)data;
                break;
            case WA_BackFill:
                if (slot) slot->backfill = data;
                break;
            case WA_RptQueue:
                if (slot) slot->rpt_queue = (uint16_t)data;
                break;
            case WA_SizeGadget:
                wa_set_flag(win_ptr, data, WFLG_SIZEGADGET); redraw = 1; break;
            case WA_DragBar:
                wa_set_flag(win_ptr, data, WFLG_DRAGBAR); redraw = 1; break;
            case WA_DepthGadget:
                wa_set_flag(win_ptr, data, WFLG_DEPTHGADGET); redraw = 1; break;
            case WA_CloseGadget:
                wa_set_flag(win_ptr, data, WFLG_CLOSEGADGET); redraw = 1; break;
            case WA_Backdrop:
                wa_set_flag(win_ptr, data, WFLG_BACKDROP); redraw = 1; break;
            case WA_ReportMouse:
                wa_set_flag(win_ptr, data, WFLG_REPORTMOUSE); break;
            case WA_NoCareRefresh:
                wa_set_flag(win_ptr, data, WFLG_NOCAREREFRESH); break;
            case WA_Borderless:
                wa_set_flag(win_ptr, data, WFLG_BORDERLESS); redraw = 1;
                if (slot) {
                    if (data) {
                        slot->border_left = slot->border_top = slot->border_right = slot->border_bottom = 0;
                    } else {
                        slot->border_left = slot->border_right = slot->border_bottom = WM_BORDER;
                        slot->border_top = WM_TITLEBAR_H;
                    }
                }
                break;
            case WA_GimmeZeroZero:
                wa_set_flag(win_ptr, data, WFLG_GIMMEZEROZERO); redraw = 1;
                if (slot) slot->gimme_zero_zero = data ? 1 : 0;
                break;
            case WA_Activate:
                wa_set_flag(win_ptr, data, WFLG_ACTIVATE); break;
            case WA_RMBTrap:
                wa_set_flag(win_ptr, data, WFLG_RMBTRAP); break;
            case WA_WBenchWindow:
                wa_set_flag(win_ptr, data, WFLG_WBENCHWINDOW); break;
            case WA_SimpleRefresh:
                if (data) {
                    mem_w32(win_ptr + WIN_OFF_FLAGS, (flags & ~WFLG_REFRESHBITS) | WFLG_SIMPLE_REFRESH);
                    if (slot) slot->simple_refresh = 1;
                }
                break;
            case WA_SmartRefresh:
                if (data) {
                    mem_w32(win_ptr + WIN_OFF_FLAGS, (flags & ~WFLG_REFRESHBITS) | WFLG_SMART_REFRESH);
                    if (slot) slot->simple_refresh = 0;
                }
                break;
            case WA_SizeBRight:
                wa_set_flag(win_ptr, data, WFLG_SIZEBRIGHT); redraw = 1; break;
            case WA_SizeBBottom:
                wa_set_flag(win_ptr, data, WFLG_SIZEBBOTTOM); redraw = 1; break;
            case WA_AutoAdjust:
                if (slot) {
                    slot->auto_adjust = data ? 1 : 0;
                    if (slot->auto_adjust) {
                        int16_t wleft = mem_s16(win_ptr + WIN_OFF_LEFTEDGE);
                        int16_t wtop  = mem_s16(win_ptr + WIN_OFF_TOPEDGE);
                        int16_t wwidth = mem_s16(win_ptr + WIN_OFF_WIDTH);
                        int16_t wheight = mem_s16(win_ptr + WIN_OFF_HEIGHT);
                        uint32_t wscreen = mem_u32(win_ptr + WIN_OFF_WSCREEN);
                        auto_adjust_window_geometry(&wleft, &wtop, wwidth, wheight, wscreen);
                        if (wleft != mem_s16(win_ptr + WIN_OFF_LEFTEDGE) ||
                            wtop != mem_s16(win_ptr + WIN_OFF_TOPEDGE)) {
                            mem_w16(win_ptr + WIN_OFF_LEFTEDGE, wleft);
                            mem_w16(win_ptr + WIN_OFF_TOPEDGE, wtop);
                            WM_MoveWindow(slot->wm_handle, wleft, wtop);
                            redraw = 1;
                        }
                    }
                }
                break;
            case WA_MenuHelp:
                if (slot) {
                    slot->menu_help = data ? 1 : 0;
                    if (data) idcmp |= IDCMP_MENUHELP;
                    else      idcmp &= ~IDCMP_MENUHELP;
                    mem_w32(win_ptr + WIN_OFF_IDCMPFLAGS, idcmp);
                }
                break;
            case WA_NewLookMenus:
                wa_set_flag(win_ptr, data, WFLG_NEWLOOKMENUS); break;
            case WA_AmigaKey:
                if (slot) slot->amiga_key = data;
                break;
            case WA_NotifyDepth:
                if (slot) slot->notify_depth = data ? 1 : 0;
                break;
            case WA_SuperBitMap:
                if (slot) {
                    slot->super_bitmap = data;
                    slot->simple_refresh = 0;
                }
                mem_w32(win_ptr + WIN_OFF_FLAGS, (flags & ~WFLG_REFRESHBITS) | WFLG_SUPER_BITMAP);
                {
                    uint32_t rp = mem_u32(win_ptr + WIN_OFF_RPORT);
                    if (rp) mem_w32(rp + RP_OFF_BITMAP, data);
                }
                redraw = 1;
                break;
            case WA_Pointer:
            case WA_BusyPointer:
                /* Handled by SetWindowPointerA; SetWindowAttrsA accepts but ignores them. */
                break;
            case WA_PointerDelay:
                if (slot) slot->pointer_delay = data;
                break;
            case WA_TabletMessages:
                if (slot) {
                    slot->tablet_messages = data ? 1 : 0;
                    if (data) idcmp |= IDCMP_TABLET;
                    else      idcmp &= ~IDCMP_TABLET;
                    mem_w32(win_ptr + WIN_OFF_IDCMPFLAGS, idcmp);
                }
                break;
            case WA_HelpGroup:
                if (slot) slot->help_group = data;
                break;
            case WA_HelpGroupWindow:
                if (slot) slot->help_group_window = data;
                break;
        }
    }

    if (slot) {
        if (moved) {
            int x = (int)mem_s16(win_ptr + WIN_OFF_LEFTEDGE);
            int y = (int)mem_s16(win_ptr + WIN_OFF_TOPEDGE);
            WM_MoveWindow(slot->wm_handle, x, y);
        }
        if (resized) {
            int w = (int)mem_s16(win_ptr + WIN_OFF_WIDTH);
            int h = (int)mem_s16(win_ptr + WIN_OFF_HEIGHT);
            if (slot->min_w > 0 && w < slot->min_w) w = slot->min_w;
            if (slot->min_h > 0 && h < slot->min_h) h = slot->min_h;
            if (slot->max_w > 0 && w > slot->max_w) w = slot->max_w;
            if (slot->max_h > 0 && h > slot->max_h) h = slot->max_h;
            if (w < 32) w = 32;
            if (h < 32) h = 32;
            WM_CloseWindow(slot->wm_handle);
            char title[32] = "Window";
            uint32_t title_ptr = mem_u32(win_ptr + WIN_OFF_TITLE);
            if (title_ptr) guest_str(title, title_ptr, sizeof(title));
            int new_wh = WM_AddWindow(
                (int)mem_s16(win_ptr + WIN_OFF_LEFTEDGE),
                (int)mem_s16(win_ptr + WIN_OFF_TOPEDGE),
                w, h, title, intu_draw_fn, NULL);
            if (new_wh >= 0) {
                slot->wm_handle = new_wh;
                mem_w16(win_ptr + WIN_OFF_WIDTH,  (int16_t)w);
                mem_w16(win_ptr + WIN_OFF_HEIGHT, (int16_t)h);
                WM_SetEventHandler(new_wh, intu_wm_event_handler);
                create_system_gadgets(slot, (int16_t)w, (int16_t)h);
                if (mem_u32(win_ptr + WIN_OFF_IDCMPFLAGS) & IDCMP_NEWSIZE)
                    post_intui_message(win_ptr, IDCMP_NEWSIZE, 0, 0, 0, 0, 0);
            } else {
                free_slot(slot);
            }
        }
    }
    if (moved || resized || redraw) WM_Redraw();
    m68k_set_reg(M68K_REG_D0, 1);
}

/* GetWindowAttrsA(window, tagList) — A0, A1; returns success in D0 */
static void intuition_GetWindowAttrsA(void)
{
    uint32_t win_ptr  = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t tag_list = m68k_get_reg(NULL, M68K_REG_A1);
    if (!win_ptr || !tag_list) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }

    IntuitionSlot *slot = find_slot_by_guest(win_ptr);
    uint32_t flags = mem_u32(win_ptr + WIN_OFF_FLAGS);

    for (uint32_t t = tag_list; t; t += 8) {
        uint32_t tag = mem_u32(t + 0);
        if (tag == TAG_DONE) break;

        uint32_t data = 0;
        switch (tag) {
            case WA_Left:      data = (uint32_t)(uint16_t)mem_u16(win_ptr + WIN_OFF_LEFTEDGE); break;
            case WA_Top:       data = (uint32_t)(uint16_t)mem_u16(win_ptr + WIN_OFF_TOPEDGE); break;
            case WA_Width:     data = (uint32_t)(uint16_t)mem_u16(win_ptr + WIN_OFF_WIDTH); break;
            case WA_Height:    data = (uint32_t)(uint16_t)mem_u16(win_ptr + WIN_OFF_HEIGHT); break;
            case WA_Title:     data = mem_u32(win_ptr + WIN_OFF_TITLE); break;
            case WA_ScreenTitle: data = slot ? slot->screen_title : 0; break;
            case WA_IDCMP:     data = mem_u32(win_ptr + WIN_OFF_IDCMPFLAGS); break;
            case WA_Flags:     data = flags; break;
            case WA_Gadgets:   data = mem_u32(win_ptr + WIN_OFF_FIRSTGADGET); break;
            case WA_Checkmark: data = slot ? slot->checkmark : 0; break;
            case WA_DetailPen: data = mem_u8(win_ptr + WIN_OFF_DETAILPEN); break;
            case WA_BlockPen:  data = mem_u8(win_ptr + WIN_OFF_BLOCKPEN); break;
            case WA_MinWidth:  data = slot ? (uint32_t)(uint16_t)slot->min_w : 0; break;
            case WA_MinHeight: data = slot ? (uint32_t)(uint16_t)slot->min_h : 0; break;
            case WA_MaxWidth:  data = slot ? (uint32_t)(uint16_t)slot->max_w : 0; break;
            case WA_MaxHeight: data = slot ? (uint32_t)(uint16_t)slot->max_h : 0; break;
            case WA_InnerWidth: data = slot ? slot->inner_width : 0; break;
            case WA_InnerHeight: data = slot ? slot->inner_height : 0; break;
            case WA_PubScreen:
            case WA_CustomScreen: data = mem_u32(win_ptr + WIN_OFF_WSCREEN); break;
            case WA_PubScreenName: data = slot ? slot->pub_screen_name : 0; break;
            case WA_PubScreenFallBack: data = slot ? slot->pub_screen_fallback : 0; break;
            case WA_WindowName: data = slot ? slot->window_name : 0; break;
            case WA_Colors:     data = slot ? slot->colors : 0; break;
            case WA_Zoom:       data = slot ? slot->zoom : 0; break;
            case WA_MouseQueue: data = slot ? slot->mouse_queue : 0; break;
            case WA_BackFill:   data = slot ? slot->backfill : 0; break;
            case WA_RptQueue:   data = slot ? slot->rpt_queue : 0; break;
            case WA_SizeGadget: data = (flags & WFLG_SIZEGADGET) ? 1 : 0; break;
            case WA_DragBar:    data = (flags & WFLG_DRAGBAR) ? 1 : 0; break;
            case WA_DepthGadget: data = (flags & WFLG_DEPTHGADGET) ? 1 : 0; break;
            case WA_CloseGadget: data = (flags & WFLG_CLOSEGADGET) ? 1 : 0; break;
            case WA_Backdrop:   data = (flags & WFLG_BACKDROP) ? 1 : 0; break;
            case WA_ReportMouse: data = (flags & WFLG_REPORTMOUSE) ? 1 : 0; break;
            case WA_NoCareRefresh: data = (flags & WFLG_NOCAREREFRESH) ? 1 : 0; break;
            case WA_Borderless: data = (flags & WFLG_BORDERLESS) ? 1 : 0; break;
            case WA_GimmeZeroZero: data = (flags & WFLG_GIMMEZEROZERO) ? 1 : 0; break;
            case WA_Activate:   data = (flags & WFLG_ACTIVATE) ? 1 : 0; break;
            case WA_RMBTrap:    data = (flags & WFLG_RMBTRAP) ? 1 : 0; break;
            case WA_WBenchWindow: data = (flags & WFLG_WBENCHWINDOW) ? 1 : 0; break;
            case WA_SimpleRefresh: data = ((flags & WFLG_REFRESHBITS) == WFLG_SIMPLE_REFRESH) ? 1 : 0; break;
            case WA_SmartRefresh: data = ((flags & WFLG_REFRESHBITS) == WFLG_SMART_REFRESH) ? 1 : 0; break;
            case WA_SizeBRight: data = (flags & WFLG_SIZEBRIGHT) ? 1 : 0; break;
            case WA_SizeBBottom: data = (flags & WFLG_SIZEBBOTTOM) ? 1 : 0; break;
            case WA_AutoAdjust: data = slot ? slot->auto_adjust : 0; break;
            case WA_MenuHelp:   data = slot ? slot->menu_help : 0; break;
            case WA_NewLookMenus: data = (flags & WFLG_NEWLOOKMENUS) ? 1 : 0; break;
            case WA_AmigaKey:   data = slot ? slot->amiga_key : 0; break;
            case WA_NotifyDepth: data = slot ? slot->notify_depth : 0; break;
            case WA_SuperBitMap: data = slot ? slot->super_bitmap : 0; break;
            case WA_Pointer:
            case WA_BusyPointer:
                data = 0; break;
            case WA_PointerDelay: data = slot ? slot->pointer_delay : 0; break;
            case WA_TabletMessages: data = slot ? slot->tablet_messages : 0; break;
            case WA_HelpGroup:  data = slot ? slot->help_group : 0; break;
            case WA_HelpGroupWindow: data = slot ? slot->help_group_window : 0; break;
        }
        mem_w32(t + 4, data);
    }
    m68k_set_reg(M68K_REG_D0, 1);
}

/* Set/clear a single screen flag bit in Screen.Flags from a boolean SA_* tag. */
static void sa_set_flag(uint32_t screen_ptr, uint32_t data, uint32_t flag)
{
    uint32_t flags = mem_u16(screen_ptr + SCR_OFF_FLAGS);
    if (data) flags |= flag;
    else      flags &= ~flag;
    mem_w16(screen_ptr + SCR_OFF_FLAGS, (uint16_t)flags);
}

/* SetScreenAttrsA(screen, tagList) — A0, A1; returns success in D0 */
static void intuition_SetScreenAttrsA(void)
{
    uint32_t screen_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t tag_list   = m68k_get_reg(NULL, M68K_REG_A1);
    if (!screen_ptr) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }

    ScreenSlot *slot = find_screen_slot(screen_ptr);
    int redraw = 0;
    int recompute_constraints = 0;

    for (uint32_t t = tag_list; t; t += 8) {
        uint32_t tag  = mem_u32(t + 0);
        uint32_t data = mem_u32(t + 4);
        if (tag == TAG_DONE) break;

        switch (tag) {
            case SA_Left:
                mem_w16(screen_ptr + SCR_OFF_LEFTEDGE, (int16_t)data);
                if (slot) slot->left = (int16_t)data;
                break;
            case SA_Top:
                mem_w16(screen_ptr + SCR_OFF_TOPEDGE, (int16_t)data);
                if (slot) slot->top = (int16_t)data;
                break;
            case SA_Width:
                mem_w16(screen_ptr + SCR_OFF_WIDTH, (int16_t)data);
                if (slot) slot->width = (int16_t)data;
                break;
            case SA_Height:
                mem_w16(screen_ptr + SCR_OFF_HEIGHT, (int16_t)data);
                if (slot) slot->height = (int16_t)data;
                break;
            case SA_Depth:
                mem_w8(screen_ptr + SCR_OFF_DEPTH, (uint8_t)data);
                if (slot) slot->depth = (uint8_t)data;
                break;
            case SA_DetailPen:
                mem_w8(screen_ptr + SCR_OFF_DETAILPEN, (uint8_t)data);
                break;
            case SA_BlockPen:
                mem_w8(screen_ptr + SCR_OFF_BLOCKPEN, (uint8_t)data);
                break;
            case SA_Title:
                mem_w32(screen_ptr + SCR_OFF_TITLE, data);
                if (slot && data) {
                    char title[64];
                    guest_str(title, data, sizeof(title));
                    local_str_copy(slot->title, title, sizeof(slot->title));
                    update_desktop_title();
                }
                break;
            case SA_Font:
                mem_w32(screen_ptr + SCR_OFF_FONT, data);
                if (slot) slot->sys_font = 0; /* explicit font overrides sys font */
                break;
            case SA_SysFont:
                if (slot) slot->sys_font = data;
                break;
            case SA_Type:
                mem_w16(screen_ptr + SCR_OFF_FLAGS, (uint16_t)data);
                break;
            case SA_BitMap:
                mem_w32(screen_ptr + SCR_OFF_BITMA, data);
                if (slot) slot->bitmap = data;
                if (data) sa_set_flag(screen_ptr, 1, CUSTOMBITMAP);
                redraw = 1;
                break;
            case SA_DisplayID:
                mem_w32(screen_ptr + SCR_OFF_DISPLAYID, data);
                if (slot) slot->display_id = data;
                break;
            case SA_Colors:
                mem_w32(screen_ptr + SCR_OFF_COLORS, data);
                if (slot) slot->colors = data;
                redraw = 1;
                break;
            case SA_Colors32:
                if (slot) slot->colors32 = data;
                redraw = 1;
                break;
            case SA_Pens:
                if (slot) slot->pens = data;
                redraw = 1;
                break;
            case SA_ErrorCode:
                if (slot) slot->error_code_ptr = data;
                break;
            case SA_Parent:
                if (slot) slot->parent = data;
                break;
            case SA_BackFill:
                if (slot) slot->backfill = data;
                redraw = 1;
                break;
            case SA_DClip:
                if (slot) {
                    slot->dclip = data;
                    recompute_constraints = 1;
                }
                break;
            case SA_Overscan:
                if (slot) {
                    slot->overscan = data;
                    recompute_constraints = 1;
                }
                break;
            case SA_PubSig:
                if (slot) slot->pub_sig = data;
                break;
            case SA_PubTask:
                if (slot) slot->pub_task = data;
                break;
            case SA_ShowTitle:
                sa_set_flag(screen_ptr, data, SHOWTITLE);
                if (slot) {
                    slot->show_title = data ? 1 : 0;
                    update_desktop_title();
                }
                break;
            case SA_Behind:
                sa_set_flag(screen_ptr, data, BEHIND);
                break;
            case SA_Quiet:
                sa_set_flag(screen_ptr, data, QUIET);
                break;
            case SA_AutoScroll:
                sa_set_flag(screen_ptr, data, AUTOSCROLL);
                break;
            case SA_FullPalette:
                if (slot) {
                    slot->full_palette = data ? 1 : 0;
                    redraw = 1;
                }
                break;
            case SA_ColorMapEntries:
                if (slot) {
                    slot->color_map_entries = (uint16_t)data;
                    redraw = 1;
                }
                break;
            case SA_Draggable:
                if (slot) slot->draggable = data ? 1 : 0;
                break;
            case SA_Exclusive:
                if (slot) slot->exclusive = data ? 1 : 0;
                break;
            case SA_SharePens:
                if (slot) slot->share_pens = data ? 1 : 0;
                break;
            case SA_Interleaved:
                if (slot) {
                    slot->interleaved = data ? 1 : 0;
                    uint32_t bm = mem_u32(screen_ptr + SCR_OFF_BITMA);
                    if (bm && bm + BM_OFF_FLAGS < GUEST_RAM_SIZE) {
                        uint8_t flags = mem_u8(bm + BM_OFF_FLAGS);
                        if (data) flags |= 1;
                        else      flags &= ~1;
                        mem_w8(bm + BM_OFF_FLAGS, flags);
                    }
                }
                break;
            case SA_LikeWorkbench:
                if (slot) slot->like_workbench = data ? 1 : 0;
                break;
            case SA_MinimizeISG:
                if (slot) slot->minimize_isg = data ? 1 : 0;
                break;
            case SA_PubName:
                if (slot && data) {
                    char name[64];
                    guest_str(name, data, sizeof(name));
                    local_str_copy(slot->pub_name, name, sizeof(slot->pub_name));
                }
                break;
        }
    }
    /* Apply SA_DClip / SA_Overscan constraints: if the tag list changed the
     * constraint rectangle, recompute the screen geometry from it; otherwise
     * just clamp any explicit geometry changes to the existing constraint. */
    if (slot && (slot->dclip || slot->overscan)) {
        int cx, cy, cw, ch;
        if (get_screen_constraints(slot, &cx, &cy, &cw, &ch) && cw > 0 && ch > 0) {
            if (recompute_constraints) {
                slot->left   = (int16_t)cx;
                slot->top    = (int16_t)cy;
                slot->width  = (int16_t)cw;
                slot->height = (int16_t)ch;
            }
            clamp_screen_to_constraints(slot, cx, cy, cw, ch);
            mem_w16(screen_ptr + SCR_OFF_LEFTEDGE, slot->left);
            mem_w16(screen_ptr + SCR_OFF_TOPEDGE,  slot->top);
            mem_w16(screen_ptr + SCR_OFF_WIDTH,    slot->width);
            mem_w16(screen_ptr + SCR_OFF_HEIGHT,   slot->height);
            redraw = 1;
        }
    }

    if (redraw) {
        if (slot && slot->is_front)
            apply_screen_palette(slot);
        WM_Redraw();
    }
    m68k_set_reg(M68K_REG_D0, 1);
}

/* GetScreenAttrsA(screen, tagList) — A0, A1; returns success in D0 */
static void intuition_GetScreenAttrsA(void)
{
    uint32_t screen_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t tag_list   = m68k_get_reg(NULL, M68K_REG_A1);
    if (!screen_ptr || !tag_list) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }

    ScreenSlot *slot = find_screen_slot(screen_ptr);
    uint16_t flags = mem_u16(screen_ptr + SCR_OFF_FLAGS);

    for (uint32_t t = tag_list; t; t += 8) {
        uint32_t tag = mem_u32(t + 0);
        if (tag == TAG_DONE) break;

        uint32_t data = 0;
        switch (tag) {
            case SA_Left:     data = (uint32_t)(uint16_t)mem_u16(screen_ptr + SCR_OFF_LEFTEDGE); break;
            case SA_Top:      data = (uint32_t)(uint16_t)mem_u16(screen_ptr + SCR_OFF_TOPEDGE); break;
            case SA_Width:    data = (uint32_t)(uint16_t)mem_u16(screen_ptr + SCR_OFF_WIDTH); break;
            case SA_Height:   data = (uint32_t)(uint16_t)mem_u16(screen_ptr + SCR_OFF_HEIGHT); break;
            case SA_Depth:    data = mem_u8(screen_ptr + SCR_OFF_DEPTH); break;
            case SA_DetailPen: data = mem_u8(screen_ptr + SCR_OFF_DETAILPEN); break;
            case SA_BlockPen: data = mem_u8(screen_ptr + SCR_OFF_BLOCKPEN); break;
            case SA_Title:    data = mem_u32(screen_ptr + SCR_OFF_TITLE); break;
            case SA_Font:     data = mem_u32(screen_ptr + SCR_OFF_FONT); break;
            case SA_SysFont:  data = slot ? slot->sys_font : 0; break;
            case SA_Type:     data = flags; break;
            case SA_BitMap:   data = mem_u32(screen_ptr + SCR_OFF_BITMA); break;
            case SA_DisplayID: data = mem_u32(screen_ptr + SCR_OFF_DISPLAYID); break;
            case SA_Colors:   data = mem_u32(screen_ptr + SCR_OFF_COLORS); break;
            case SA_Colors32: data = slot ? slot->colors32 : 0; break;
            case SA_Pens:     data = slot ? slot->pens : 0; break;
            case SA_ErrorCode: data = slot ? slot->error_code_ptr : 0; break;
            case SA_Parent:   data = slot ? slot->parent : 0; break;
            case SA_BackFill: data = slot ? slot->backfill : 0; break;
            case SA_DClip:    data = slot ? slot->dclip : 0; break;
            case SA_Overscan: data = slot ? slot->overscan : 0; break;
            case SA_PubSig:   data = slot ? slot->pub_sig : 0; break;
            case SA_PubTask:  data = slot ? slot->pub_task : 0; break;
            case SA_ShowTitle: data = (flags & SHOWTITLE) ? 1 : 0; break;
            case SA_Behind:   data = (flags & BEHIND) ? 1 : 0; break;
            case SA_Quiet:    data = (flags & QUIET) ? 1 : 0; break;
            case SA_AutoScroll: data = (flags & AUTOSCROLL) ? 1 : 0; break;
            case SA_FullPalette: data = slot ? slot->full_palette : 0; break;
            case SA_ColorMapEntries: data = slot ? slot->color_map_entries : 0; break;
            case SA_Draggable: data = slot ? slot->draggable : 0; break;
            case SA_Exclusive: data = slot ? slot->exclusive : 0; break;
            case SA_SharePens: data = slot ? slot->share_pens : 0; break;
            case SA_Interleaved: data = slot ? slot->interleaved : 0; break;
            case SA_LikeWorkbench: data = slot ? slot->like_workbench : 0; break;
            case SA_MinimizeISG: data = slot ? slot->minimize_isg : 0; break;
            case SA_PubName:  data = 0; break; /* host-side string; not exposed as guest pointer */
        }
        mem_w32(t + 4, data);
    }
    m68k_set_reg(M68K_REG_D0, 1);
}

/* =========================================================================
 * Workbench screen
 * ========================================================================= */

static uint32_t g_workbench_screen = 0;

static uint32_t open_workbench_internal(void)
{
    ScreenSlot *slot = alloc_screen_slot();
    if (!slot) return 0;

    uint32_t guest_screen = intu_alloc(SCR_SIZE);
    uint32_t rport = intu_alloc(RP_SIZE_MIN);
    if (!guest_screen || !rport) {
        intu_free(guest_screen);
        intu_free(rport);
        slot->active = 0;
        return 0;
    }

    init_guest_rastport(rport, 0);
    memset(&g_ram[guest_screen], 0, SCR_SIZE);
    mem_w16(guest_screen + SCR_OFF_LEFTEDGE, 0);
    mem_w16(guest_screen + SCR_OFF_TOPEDGE, 0);
    mem_w16(guest_screen + SCR_OFF_WIDTH, (int16_t)g_fb.width);
    mem_w16(guest_screen + SCR_OFF_HEIGHT, (int16_t)g_fb.height);
    mem_w16(guest_screen + SCR_OFF_FLAGS, (uint16_t)(WBENCHSCREEN | PUBLICSCREEN | SHOWTITLE));
    mem_w32(guest_screen + SCR_OFF_TITLE, 0);
    mem_w32(guest_screen + SCR_OFF_DEFAULTTITLE, 0);
    mem_w32(guest_screen + SCR_OFF_FONT, 0);
    mem_w32(guest_screen + SCR_OFF_RASTPORT, rport);
    mem_w32(guest_screen + SCR_OFF_VIEWPORT, 0);
    mem_w8(guest_screen + SCR_OFF_DETAILPEN, 0);
    mem_w8(guest_screen + SCR_OFF_BLOCKPEN, 1);

    slot->guest_screen = guest_screen;
    slot->left = 0;
    slot->top = 0;
    slot->width = (int16_t)g_fb.width;
    slot->height = (int16_t)g_fb.height;
    slot->show_title = 1;
    slot->is_front = 1;
    slot->lock_count = 0;
    slot->title[0] = '\0';
    local_str_copy(slot->pub_name, "Workbench", sizeof(slot->pub_name));
    slot->depth = 2;
    slot->display_id = 0;
    slot->bitmap = 0;
    slot->colormap = 0;
    slot->colors = 0;
    slot->colors32 = 0;
    slot->sys_font = 0;
    slot->pens = 0;
    slot->error_code_ptr = 0;
    slot->parent = 0;
    slot->backfill = 0;
    slot->dclip = 0;
    slot->overscan = 0;
    slot->pub_sig = 0;
    slot->pub_task = 0;
    slot->color_map_entries = 0;
    slot->full_palette = 0;
    slot->draggable = 1;
    slot->exclusive = 0;
    slot->share_pens = 0;
    slot->interleaved = 0;
    slot->like_workbench = 1;
    slot->minimize_isg = 0;
    mem_w8(guest_screen + SCR_OFF_DEPTH, slot->depth);
    mem_w32(guest_screen + SCR_OFF_BITMA, 0);
    mem_w32(guest_screen + SCR_OFF_DISPLAYID, 0);
    mem_w32(guest_screen + SCR_OFF_COLORS, 0);

    for (int i = 0; i < MAX_INTUITION_SCREENS; i++) {
        if (g_intu_screens[i].active && &g_intu_screens[i] != slot)
            g_intu_screens[i].is_front = 0;
    }
    update_desktop_title();
    return guest_screen;
}

/* OpenWorkbench() — returns Workbench screen pointer in D0 */
static void intuition_OpenWorkbench(void)
{
    if (!g_workbench_screen)
        g_workbench_screen = open_workbench_internal();
    m68k_set_reg(M68K_REG_D0, g_workbench_screen);
}

/* CloseWorkBench() — returns BOOL in D0 */
static void intuition_CloseWorkbench(void)
{
    if (g_workbench_screen) {
        ScreenSlot *slot = find_screen_slot(g_workbench_screen);
        if (slot) {
            uint32_t rport = mem_u32(g_workbench_screen + SCR_OFF_RASTPORT);
            intu_free(rport);
            intu_free(g_workbench_screen);
            slot->active = 0;
            update_desktop_title();
        }
        g_workbench_screen = 0;
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
#define GFX_SLOT_FINDDISPLAYINFO   121
#define GFX_SLOT_NEXTDISPLAYINFO   122
#define GFX_SLOT_GETDISPLAYINFODATA 126
#define GFX_SLOT_MODENOTAVAILABLE   133
#define GFX_SLOT_BESTMODEIDA        175

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
        uint32_t font_attr = mem_u32(itext + ITEXT_OFF_FONT);
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

/* EasyRequest(window, easyStruct, idcmpPtr, arg1, ...)
 * A0/A1/A2 plus varargs passed as a pointer in A3.
 * Varargs wrapper for EasyRequestArgs(); on m68k the compiler stub
 * packages the arguments and passes the first argument's address in A3,
 * so we can simply forward to the EasyRequestArgs implementation. */
static void intuition_EasyRequest(void)
{
    intuition_EasyRequestArgs();
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
        int cx, cy, cw, ch;
        if (get_screen_constraints(slot, &cx, &cy, &cw, &ch))
            clamp_screen_to_constraints(slot, cx, cy, cw, ch);
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

/* ScreenDepth(screen, flags, reserved) — A0, D0, A1
 * V39 depth-arrangement: SDEPTH_TOFRONT / SDEPTH_TOBACK */
static void intuition_ScreenDepth(void)
{
    uint32_t screen_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t flags      = m68k_get_reg(NULL, M68K_REG_D0);
    (void)m68k_get_reg(NULL, M68K_REG_A1);

    ScreenSlot *slot = find_screen_slot(screen_ptr);
    if (!slot) return;

    if (flags & SDEPTH_TOBACK) {
        slot->is_front = 0;
        for (int i = 0; i < MAX_INTUITION_SCREENS; i++) {
            if (g_intu_screens[i].active && &g_intu_screens[i] != slot) {
                g_intu_screens[i].is_front = 1;
                break;
            }
        }
    } else {
        for (int i = 0; i < MAX_INTUITION_SCREENS; i++)
            g_intu_screens[i].is_front = 0;
        slot->is_front = 1;
    }
    update_desktop_title();
}

/* ScreenPosition(screen, flags, x1, y1, x2, y2) — A0, D0, D1, D2, D3, D4
 * V39 screen positioning: SPOS_RELATIVE / SPOS_ABSOLUTE / SPOS_MAKEVISIBLE */
static void intuition_ScreenPosition(void)
{
    uint32_t screen_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t flags      = m68k_get_reg(NULL, M68K_REG_D0);
    int32_t  x1         = (int32_t)m68k_get_reg(NULL, M68K_REG_D1);
    int32_t  y1         = (int32_t)m68k_get_reg(NULL, M68K_REG_D2);
    int32_t  x2         = (int32_t)m68k_get_reg(NULL, M68K_REG_D3);
    int32_t  y2         = (int32_t)m68k_get_reg(NULL, M68K_REG_D4);
    (void)x2; (void)y2;

    ScreenSlot *slot = find_screen_slot(screen_ptr);
    if (!slot) return;

    if (flags & SPOS_ABSOLUTE) {
        slot->left = (int16_t)x1;
        slot->top  = (int16_t)y1;
    } else if (flags & SPOS_MAKEVISIBLE) {
        /* Simple autoscroll: keep (x1,y1)-(x2,y2) visible by clamping screen
         * position so the rectangle is inside the display. */
        int rect_w = (x2 > x1) ? (x2 - x1) : 0;
        int rect_h = (y2 > y1) ? (y2 - y1) : 0;
        if (x1 + slot->left < 0) slot->left = (int16_t)(-x1);
        if (y1 + slot->top  < 0) slot->top  = (int16_t)(-y1);
        if (x1 + slot->left + rect_w > (int)g_fb.width)
            slot->left = (int16_t)((int)g_fb.width - x1 - rect_w);
        if (y1 + slot->top  + rect_h > (int)g_fb.height)
            slot->top  = (int16_t)((int)g_fb.height - y1 - rect_h);
    } else {
        /* SPOS_RELATIVE (default) */
        slot->left += (int16_t)x1;
        slot->top  += (int16_t)y1;
    }

    /* Clamp to SA_DClip / SA_Overscan constraints if present. */
    int cx, cy, cw, ch;
    if (get_screen_constraints(slot, &cx, &cy, &cw, &ch))
        clamp_screen_to_constraints(slot, cx, cy, cw, ch);

    mem_w16(screen_ptr + SCR_OFF_LEFTEDGE, slot->left);
    mem_w16(screen_ptr + SCR_OFF_TOPEDGE,  slot->top);
    update_desktop_title();
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

/* Minimal VisualInfo state used by GadTools-style menu/layout code.
 * In real AmigaOS these are in gadtools.library; here they are provided
 * through intuition.library for convenience. */
#define MAX_VISUAL_INFOS 8

/* Allocate a DrawInfo for a screen.  This is a helper reused by
 * GetScreenDrawInfo and GetVisualInfoA. */
static uint32_t alloc_screen_draw_info(uint32_t screen)
{
    if (!screen) return 0;
    uint32_t dri = intu_alloc(DRINFO_SIZE);
    if (!dri) return 0;
    for (int i = 0; i < DRINFO_SIZE; i++) mem_w8(dri + i, 0);

    uint8_t detail = mem_u8(screen + SCR_OFF_DETAILPEN);
    uint8_t block  = mem_u8(screen + SCR_OFF_BLOCKPEN);
    uint32_t font  = mem_u32(screen + SCR_OFF_FONT);

    static const uint16_t default_pens[DRINFO_PEN_COUNT] = {
        0, 1, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 1, 1
    };

    mem_w16(dri + DRINFO_OFF_VERSION, 1);
    mem_w16(dri + DRINFO_OFF_NUMPENS, DRINFO_PEN_COUNT);
    for (int i = 0; i < DRINFO_PEN_COUNT; i++) {
        uint16_t pen = default_pens[i];
        if (pen == 0) pen = detail;
        else if (pen == 1) pen = block;
        mem_w16(dri + DRINFO_OFF_PENS + i * 2, pen);
    }
    mem_w32(dri + DRINFO_OFF_FONT, font);
    mem_w8(dri + DRINFO_OFF_DEPTH, 2);
    mem_w16(dri + DRINFO_OFF_RESX, 72);
    mem_w16(dri + DRINFO_OFF_RESY, 72);
    mem_w32(dri + DRINFO_OFF_FLAGS, 0);
    return dri;
}

/* GetVisualInfoA(screen, tagList) — A0, A1; returns VisualInfo* in D0
 * Allocates a real guest VisualInfo structure tied to the supplied screen. */
#define VI_SIZE        16
#define VI_OFF_SCREEN  0
#define VI_OFF_DRAWINFO 4
#define VI_OFF_FONT     8
#define VI_OFF_FLAGS   12

static void intuition_GetVisualInfoA(void)
{
    uint32_t screen_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    (void)m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t result = 0;
    if (screen_ptr) {
        uint32_t vi = intu_alloc(VI_SIZE);
        if (vi) {
            for (int i = 0; i < VI_SIZE; i++) mem_w8(vi + i, 0);
            mem_w32(vi + VI_OFF_SCREEN, screen_ptr);
            uint32_t dri = alloc_screen_draw_info(screen_ptr);
            mem_w32(vi + VI_OFF_DRAWINFO, dri);
            mem_w32(vi + VI_OFF_FONT, mem_u32(screen_ptr + SCR_OFF_FONT));
            result = vi;
        }
    }
    m68k_set_reg(M68K_REG_D0, result);
}

/* FreeVisualInfo(visualInfo) — A0 */
static void intuition_FreeVisualInfo(void)
{
    uint32_t vi = m68k_get_reg(NULL, M68K_REG_A0);
    if (vi) {
        uint32_t dri = mem_u32(vi + VI_OFF_DRAWINFO);
        if (dri) intu_free(dri);
        intu_free(vi);
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

/* Enable or disable a menu item addressed by a packed menu number. */
static void set_menu_item_enabled(uint32_t menu_strip, uint32_t menu_number, int enabled)
{
    if (!menu_strip || menu_number == MENUNULL) return;
    int menu_idx = MENUNUM(menu_number);
    int item_idx = ITEMNUM(menu_number);
    int sub_idx  = SUBNUM(menu_number);

    uint32_t menu = menu_strip;
    for (int m = 0; menu && m < menu_idx; m++)
        menu = mem_u32(menu + MENU_OFF_NEXTMENU);
    if (!menu) return;

    uint32_t item = mem_u32(menu + MENU_OFF_FIRSTITEM);
    for (int i = 0; item && i < item_idx; i++)
        item = mem_u32(item + MENUITEM_OFF_NEXTITEM);
    if (!item) return;

    if (sub_idx != NOSUB) {
        uint32_t sub = mem_u32(item + MENUITEM_OFF_SUBITEM);
        for (int s = 0; sub && s < sub_idx; s++)
            sub = mem_u32(sub + MENUITEM_OFF_NEXTITEM);
        item = sub;
    }

    if (item) {
        uint16_t flags = mem_u16(item + MENUITEM_OFF_FLAGS);
        if (enabled) flags |= ITEMENABLED;
        else         flags &= ~ITEMENABLED;
        mem_w16(item + MENUITEM_OFF_FLAGS, flags);
    }
}

/* OnMenu(window, menuNumber) — A0, D0 */
static void intuition_OnMenu(void)
{
    uint32_t win_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t menu_num = m68k_get_reg(NULL, M68K_REG_D0);
    if (win_ptr)
        set_menu_item_enabled(mem_u32(win_ptr + WIN_OFF_MENUSTRIP), menu_num, 1);
}

/* OffMenu(window, menuNumber) — A0, D0 */
static void intuition_OffMenu(void)
{
    uint32_t win_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t menu_num = m68k_get_reg(NULL, M68K_REG_D0);
    if (win_ptr)
        set_menu_item_enabled(mem_u32(win_ptr + WIN_OFF_MENUSTRIP), menu_num, 0);
}

/* -------------------------------------------------------------------------
 * Menu helpers exported to desktop.c
 * ------------------------------------------------------------------------- */

/* Return the menu strip pointer of the currently focused WM window, or 0. */
uint32_t Intuition_GetActiveWindowMenuStrip(void)
{
    int focus = WM_GetFocus();
    if (focus < 0) return 0;
    uint32_t win_ptr = get_guest_window_from_handle(focus);
    if (!win_ptr) return 0;
    return mem_u32(win_ptr + WIN_OFF_MENUSTRIP);
}

/* Post an IDCMP_MENUPICK message to the focused WM window. */
void Intuition_PostMenuPick(uint32_t menu_number)
{
    int focus = WM_GetFocus();
    if (focus < 0) return;
    uint32_t win_ptr = get_guest_window_from_handle(focus);
    if (!win_ptr) return;
    post_intui_message(win_ptr, IDCMP_MENUPICK, (uint16_t)menu_number, 0, 0, 0, 0);
}

/* Search the parsed menu strip for an enabled item whose command key matches
 * the given character.  Returns 1 and fills the invocation parameters if a
 * match is found, otherwise 0. */
static int find_command_key_in_menu(HostMenu *menus, int menu_count, char key,
                                    uint32_t *out_menu_number,
                                    uint32_t *out_guest_item, int *out_toggle)
{
    for (int m = 0; m < menu_count; m++) {
        for (int i = 0; i < menus[m].item_count; i++) {
            HostMenuItem *mi = &menus[m].items[i];
            if (mi->enabled && mi->command_key) {
                char cmd = mi->command_key;
                if (cmd == key || (cmd >= 'A' && cmd <= 'Z' && cmd + 32 == key) ||
                    (cmd >= 'a' && cmd <= 'z' && cmd - 32 == key)) {
                    *out_menu_number = (uint32_t)((m & 0x1F) | ((i & 0x3F) << 5));
                    *out_guest_item = mi->guest_item;
                    *out_toggle = mi->toggle;
                    return 1;
                }
            }
            if (mi->has_submenu && mi->submenu) {
                for (int s = 0; s < mi->submenu->item_count; s++) {
                    HostMenuItem *smi = &mi->submenu->items[s];
                    if (smi->enabled && smi->command_key) {
                        char cmd = smi->command_key;
                        if (cmd == key || (cmd >= 'A' && cmd <= 'Z' && cmd + 32 == key) ||
                            (cmd >= 'a' && cmd <= 'z' && cmd - 32 == key)) {
                            *out_menu_number = (uint32_t)((m & 0x1F) |
                                                          ((i & 0x3F) << 5) |
                                                          ((s & 0x1F) << 11));
                            *out_guest_item = smi->guest_item;
                            *out_toggle = smi->toggle;
                            return 1;
                        }
                    }
                }
            }
        }
    }
    return 0;
}

/* If the active window has a menu item with a matching command key, invoke
 * it by updating its check state and posting an IDCMP_MENUPICK message.
 * Returns 1 if a menu item was invoked, 0 otherwise. */
int Intuition_InvokeCommandKey(char c)
{
    uint32_t strip = Intuition_GetActiveWindowMenuStrip();
    if (!strip) return 0;

    HostMenu menus[HOST_MENU_MAX];
    int count = Intuition_GetHostMenuStrip(strip, menus, HOST_MENU_MAX);
    if (count <= 0) return 0;

    /* Convert a control code (Ctrl+A..Ctrl+Z) to the corresponding uppercase
     * letter, so command keys can be triggered with a Ctrl modifier. */
    char key = c;
    if ((unsigned char)c >= 1 && (unsigned char)c <= 26)
        key = (char)('A' + c - 1);

    uint32_t menu_number = 0, guest_item = 0;
    int toggle = 0;
    if (find_command_key_in_menu(menus, count, key, &menu_number, &guest_item, &toggle)) {
        Intuition_UpdateMenuItemCheck(guest_item, toggle);
        Intuition_PostMenuPick(menu_number);
        return 1;
    }
    return 0;
}

/* Small pool of host submenus used while parsing a guest menu strip.
 * Submenus are ephemeral and reparsed whenever the focused window changes. */
#define HOST_MENU_SUB_MAX 32
static HostMenu g_submenu_pool[HOST_MENU_SUB_MAX];
static int      g_submenu_pool_count = 0;

static HostMenu *alloc_host_submenu(void)
{
    if (g_submenu_pool_count >= HOST_MENU_SUB_MAX) return NULL;
    HostMenu *m = &g_submenu_pool[g_submenu_pool_count++];
    m->item_count = 0;
    m->label[0] = '\0';
    return m;
}

static void reset_host_submenu_pool(void)
{
    g_submenu_pool_count = 0;
}

static int parse_host_menu_items(uint32_t first_item, HostMenuItem *items, int max_items)
{
    int count = 0;
    uint32_t item = first_item;
    while (item && count < max_items) {
        HostMenuItem *mi = &items[count];
        mi->label[0] = '\0';
        mi->enabled = 1;
        mi->has_checkmark = 0;
        mi->checked = 0;
        mi->toggle = 0;
        mi->command_key = 0;
        mi->has_submenu = 0;
        mi->submenu = NULL;
        mi->guest_item = item;

        uint16_t flags = mem_u16(item + MENUITEM_OFF_FLAGS);
        if (!(flags & ITEMENABLED)) mi->enabled = 0;
        if (flags & CHECKIT) { mi->has_checkmark = 1; mi->checked = 1; }
        if (flags & MENUTOGGLE) mi->toggle = 1;
        if (flags & COMMSEQ) mi->command_key = (char)mem_u8(item + MENUITEM_OFF_COMMAND);

        uint32_t item_fill = mem_u32(item + MENUITEM_OFF_ITEMFILL);
        if (item_fill && (flags & ITEMTEXT)) {
            uint32_t text_ptr = mem_u32(item_fill + ITEXT_OFF_ITEXT);
            guest_str(mi->label, text_ptr, HOST_MENU_LABEL_SIZE);
        }

        uint32_t subitem = mem_u32(item + MENUITEM_OFF_SUBITEM);
        if (subitem) {
            HostMenu *sm = alloc_host_submenu();
            if (sm) {
                sm->item_count = parse_host_menu_items(subitem, sm->items, HOST_MENU_ITEM_MAX);
                mi->submenu = sm;
                mi->has_submenu = 1;
            }
        }

        count++;
        item = mem_u32(item + MENUITEM_OFF_NEXTITEM);
    }
    return count;
}

/* Parse the guest menu strip into host-friendly HostMenu structures.
 * Returns the number of top-level menus parsed. */
int Intuition_GetHostMenuStrip(uint32_t menu_strip, HostMenu *menus, int max_menus)
{
    if (!menus || max_menus <= 0) return 0;
    if (!menu_strip) return 0;

    reset_host_submenu_pool();

    int count = 0;
    uint32_t menu = menu_strip;
    while (menu && count < max_menus) {
        uint32_t name_ptr = mem_u32(menu + MENU_OFF_MENUNAME);
        guest_str(menus[count].label, name_ptr, HOST_MENU_LABEL_SIZE);
        menus[count].item_count = 0;

        uint32_t item = mem_u32(menu + MENU_OFF_FIRSTITEM);
        menus[count].item_count = parse_host_menu_items(item, menus[count].items, HOST_MENU_ITEM_MAX);

        count++;
        menu = mem_u32(menu + MENU_OFF_NEXTMENU);
    }
    return count;
}

/* Toggle/set a guest MenuItem's CHECKIT state after it has been selected.
 * For MENUTOGGLE items the state is toggled; for plain CHECKIT items it is set. */
void Intuition_UpdateMenuItemCheck(uint32_t guest_item, int toggle)
{
    if (!guest_item) return;
    uint16_t flags = mem_u16(guest_item + MENUITEM_OFF_FLAGS);
    if (flags & CHECKIT) {
        if (toggle) flags ^= CHECKIT;
        else        flags |= CHECKIT;
        mem_w16(guest_item + MENUITEM_OFF_FLAGS, flags);
    }
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

/* LockPubScreenList() — returns List* in D0
 * Builds a real exec List containing a node for every active public screen.
 * Each node has ln_Name pointing to the screen's public name and the guest
 * Screen* stored after the standard Node header. */
#define PSNODE_OFF_SCREEN 14
#define PSNODE_SIZE       20

static void free_pub_screen_list(uint32_t list)
{
    if (!list) return;
    uint32_t node = mem_u32(list + LH_OFF_HEAD);
    uint32_t tail = list + LH_OFF_TAIL;
    while (node && node != tail) {
        uint32_t succ = mem_u32(node + MSG_OFF_LN_SUCC);
        uint32_t name = mem_u32(node + MSG_OFF_LN_NAME);
        if (name) intu_free(name);
        intu_free(node);
        node = succ;
    }
    intu_free(list);
}

static void intuition_LockPubScreenList(void)
{
    uint32_t list = intu_alloc(16);
    if (!list) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }
    init_guest_list(list);
    mem_w8(list + LH_OFF_TYPE, 0);

    for (int i = 0; i < MAX_INTUITION_SCREENS; i++) {
        if (!g_intu_screens[i].active) continue;
        if (g_intu_screens[i].pub_name[0] == '\0') continue;

        const char *name = g_intu_screens[i].pub_name;
        int name_len = 0;
        while (name[name_len] && name_len < 63) name_len++;
        uint32_t guest_name = intu_alloc(name_len + 1);
        if (!guest_name) continue;
        for (int j = 0; j <= name_len; j++)
            mem_w8(guest_name + j, (uint8_t)name[j]);

        uint32_t node = intu_alloc(PSNODE_SIZE);
        if (!node) {
            intu_free(guest_name);
            continue;
        }
        for (int j = 0; j < PSNODE_SIZE; j++) mem_w8(node + j, 0);
        mem_w32(node + MSG_OFF_LN_NAME, guest_name);
        mem_w32(node + PSNODE_OFF_SCREEN, g_intu_screens[i].guest_screen);

        guest_list_add_tail(list, node);
    }

    m68k_set_reg(M68K_REG_D0, list);
}

/* UnlockPubScreenList() */
static void intuition_UnlockPubScreenList(void)
{
    uint32_t list = m68k_get_reg(NULL, M68K_REG_A0);
    free_pub_screen_list(list);
}

/* Decode a SetPointer-style sprite data buffer into a cursor sprite.
 * Amiga sprite data layout: 2 reserved words, then height rows of 2 words
 * (one per bitplane), then 2 reserved words. */
static void decode_pointer_sprite(uint32_t pointer, int height, int width,
                                  int xoff, int yoff)
{
    if (width <= 0)  width = 16;
    if (width > 48)  width = 48;
    if (height <= 0) height = 16;
    if (height > 48) height = 48;

    uint8_t sprite[48 * 48];
    memset(sprite, 0, sizeof(sprite));

    if (pointer) {
        for (int row = 0; row < height; row++) {
            uint16_t p0 = mem_u16(pointer + 4 + row * 4);
            uint16_t p1 = mem_u16(pointer + 4 + row * 4 + 2);
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

    Cursor_SetCustomSprite(sprite, width, height, xoff, yoff);
}

/* Check if a guest pointer looks like a valid struct BitMap. */
static int is_valid_bitmap(uint32_t bm)
{
    if (!bm || bm + 16 > GUEST_RAM_SIZE) return 0;
    uint16_t bpr = mem_u16(bm + BM_OFF_BYTESPERROW);
    uint16_t rows = mem_u16(bm + BM_OFF_ROWS);
    uint8_t  depth = mem_u8(bm + BM_OFF_DEPTH);
    return (bpr > 0 && bpr <= 8 && rows > 0 && rows <= 48 &&
            (depth == 1 || depth == 2));
}

/* Decode a graphics.library BitMap (up to 2 planes) into a cursor sprite. */
static void decode_pointer_bitmap(uint32_t bm, int xoff, int yoff)
{
    if (!is_valid_bitmap(bm)) {
        Cursor_ClearCustomSprite();
        return;
    }

    uint16_t bpr   = mem_u16(bm + BM_OFF_BYTESPERROW);
    uint16_t rows  = mem_u16(bm + BM_OFF_ROWS);
    uint8_t  depth = mem_u8(bm + BM_OFF_DEPTH);
    int width = bpr * 8;
    int height = rows;

    if (width > 48)  width = 48;
    if (height > 48) height = 48;

    uint32_t plane0 = mem_u32(bm + BM_OFF_PLANES);
    uint32_t plane1 = mem_u32(bm + BM_OFF_PLANES + 4);

    uint8_t sprite[48 * 48];
    memset(sprite, 0, sizeof(sprite));

    for (int row = 0; row < height; row++) {
        for (int col = 0; col < width; col++) {
            int byte = col / 8;
            int bit  = 7 - (col % 8);
            int v = 0;
            if (plane0 && plane0 + row * bpr + byte < GUEST_RAM_SIZE) {
                if (mem_u8(plane0 + row * bpr + byte) & (1 << bit)) v |= 1;
            }
            if (depth > 1 && plane1 && plane1 + row * bpr + byte < GUEST_RAM_SIZE) {
                if (mem_u8(plane1 + row * bpr + byte) & (1 << bit)) v |= 2;
            }
            uint8_t px = 0;
            if (v == 1) px = 1;
            else if (v == 2) px = 2;
            else if (v == 3) px = 1;
            sprite[row * width + col] = px;
        }
    }

    Cursor_SetCustomSprite(sprite, width, height, xoff, yoff);
}

/* Try to extract a pointer image from a WA_Pointer value. The value may be:
 *  - a SetPointer-style sprite data buffer
 *  - a pointer to a struct BitMap
 *  - a pointerclass BOOPSI object (explicit class check first)
 */
extern int UAOS_BOOPSI_IsPointerClass(uint32_t obj);
extern uint32_t UAOS_BOOPSI_PointerBitMap(uint32_t obj);
extern uint32_t UAOS_BOOPSI_PointerXOffset(uint32_t obj);
extern uint32_t UAOS_BOOPSI_PointerYOffset(uint32_t obj);

static void set_pointer_from_wa_pointer(uint32_t ptr, int xoff, int yoff)
{
    if (!ptr) {
        Cursor_ClearCustomSprite();
        return;
    }

    /* Sprite data buffer: first two words are reserved zeros. */
    if (ptr + 4 < GUEST_RAM_SIZE && mem_u32(ptr) == 0) {
        decode_pointer_sprite(ptr, 16, 16, xoff, yoff);
        return;
    }

    /* Direct struct BitMap pointer. */
    if (is_valid_bitmap(ptr)) {
        decode_pointer_bitmap(ptr, xoff, yoff);
        return;
    }

    /* pointerclass BOOPSI object: use the explicit class fields. */
    if (UAOS_BOOPSI_IsPointerClass(ptr)) {
        uint32_t bm = UAOS_BOOPSI_PointerBitMap(ptr);
        if (bm && is_valid_bitmap(bm)) {
            int16_t px = (int16_t)UAOS_BOOPSI_PointerXOffset(ptr);
            int16_t py = (int16_t)UAOS_BOOPSI_PointerYOffset(ptr);
            decode_pointer_bitmap(bm, xoff + px, yoff + py);
            return;
        }
    }

    /* Final fallback: scan likely offsets for a BitMap pointer. */
    for (int off = 0; off <= 32; off += 4) {
        uint32_t cand = mem_u32(ptr + off);
        if (is_valid_bitmap(cand)) {
            decode_pointer_bitmap(cand, xoff, yoff);
            return;
        }
    }

    Cursor_ClearCustomSprite();
}

/* SetPointer(window, pointer, height, width, xOffset, yOffset)
 * A0, A1, D0, D1, D2, D3 */
static void intuition_SetPointer(void)
{
    uint32_t pointer = m68k_get_reg(NULL, M68K_REG_A1);
    int16_t  height  = (int16_t)m68k_get_reg(NULL, M68K_REG_D0);
    int16_t  width   = (int16_t)m68k_get_reg(NULL, M68K_REG_D1);
    int16_t  xoff    = (int16_t)m68k_get_reg(NULL, M68K_REG_D2);
    int16_t  yoff    = (int16_t)m68k_get_reg(NULL, M68K_REG_D3);

    decode_pointer_sprite(pointer, height, width, xoff, yoff);
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
    uint32_t win_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t tag_list = m68k_get_reg(NULL, M68K_REG_A1);
    int busy = 0;
    int got_busy = 0;
    uint32_t wa_pointer = 0;
    int got_pointer = 0;
    uint32_t delay = 0;
    int got_delay = 0;

    if (tag_list) {
        uint32_t p = tag_list;
        while (p + 8 <= GUEST_RAM_SIZE) {
            uint32_t tag  = mem_u32(p);
            uint32_t data = mem_u32(p + 4);
            if (tag == TAG_DONE) break;
            if (tag == WA_BusyPointer) {
                got_busy = 1;
                busy = data ? 1 : 0;
            } else if (tag == WA_Pointer) {
                got_pointer = 1;
                wa_pointer = data;
            } else if (tag == WA_PointerDelay) {
                got_delay = 1;
                delay = data;
            }
            p += 8;
        }
    }

    if (got_delay) {
        IntuitionSlot *slot = find_slot_by_guest(win_ptr);
        if (slot) slot->pointer_delay = delay;
    }

    if (got_busy || got_pointer) {
        if (delay > 0) {
            /* Schedule the pointer change after the requested delay.
             * g_pit_ticks runs at 100 Hz, so 1 tick = 10 ms. */
            g_pending_pointer_active = 1;
            g_pending_pointer_target = g_pit_ticks + (delay + 9) / 10;
            g_pending_pointer_busy = got_busy ? 1 : 0;
            g_pending_pointer_busy_state = busy ? 1 : 0;
            g_pending_pointer = wa_pointer;
            g_pending_pointer_xoff = 0;
            g_pending_pointer_yoff = 0;
        } else {
            g_pending_pointer_active = 0;
            if (got_busy) {
                Cursor_SetBusy(busy);
            } else {
                set_pointer_from_wa_pointer(wa_pointer, 0, 0);
            }
        }
    } else {
        g_pending_pointer_active = 0;
        Cursor_ClearCustomSprite();
    }
}

/* Apply any delayed pointer change whose time has arrived.  Called from the
 * cursor module on mouse movement / redraw so that WA_PointerDelay is honoured. */
void UAOS_Intuition_CheckPendingPointer(void)
{
    if (!g_pending_pointer_active) return;
    if (g_pit_ticks < g_pending_pointer_target) return;

    g_pending_pointer_active = 0;
    if (g_pending_pointer_busy) {
        Cursor_SetBusy(g_pending_pointer_busy_state);
    } else {
        set_pointer_from_wa_pointer(g_pending_pointer,
                                  (int)g_pending_pointer_xoff,
                                  (int)g_pending_pointer_yoff);
    }
}

/* Post IDCMP_NEWSIZE to a window if it has WA_NotifyDepth set and the IDCMP
 * flag is enabled.  Called from the WM whenever the window's z-order changes. */
void UAOS_Intuition_NotifyDepthChange(int wm_handle)
{
    uint32_t win_ptr = get_guest_window_from_handle(wm_handle);
    if (!win_ptr) return;
    IntuitionSlot *slot = get_slot_from_handle(wm_handle);
    if (!slot || !slot->notify_depth) return;
    uint32_t idcmp = mem_u32(win_ptr + WIN_OFF_IDCMPFLAGS);
    if (idcmp & IDCMP_NEWSIZE)
        post_intui_message(win_ptr, IDCMP_NEWSIZE, 0, 0, 0, 0, 0);
}

/* -------------------------------------------------------------------------
 * Preferences / defaults
 * ------------------------------------------------------------------------- */

static uint8_t g_intu_prefs[PREF_SIZE];
static uint8_t g_intu_def_prefs[PREF_SIZE];
static uint32_t g_gui_lock = 0xDEADBEEF;
static int      g_prefs_inited = 0;

static void apply_prefs(void);

static uint16_t read_host_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

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

    apply_prefs();
}

/* Convert a 12-bit Amiga colour word (O RGB) to 24-bit RGB. */
static uint32_t amiga_color_to_rgb(uint16_t amiga)
{
    uint8_t r = ((amiga >> 8) & 0x0F) * 17;
    uint8_t g = ((amiga >> 4) & 0x0F) * 17;
    uint8_t b = (amiga        & 0x0F) * 17;
    return FB_RGB(r, g, b);
}

/* Scale an RGB colour by num/den (integer, no floating point). */
static uint32_t scale_rgb(uint32_t rgb, int num, int den)
{
    uint32_t r = (rgb >> 16) & 0xFF;
    uint32_t g = (rgb >> 8)  & 0xFF;
    uint32_t b = rgb         & 0xFF;
    r = (r * num) / den; if (r > 255) r = 255;
    g = (g * num) / den; if (g > 255) g = 255;
    b = (b * num) / den; if (b > 255) b = 255;
    return FB_RGB(r, g, b);
}

/* Globals holding the live values from SetPrefs. */
static int  g_intu_font_height = 8;
static int  g_intu_wb_width    = 640;
static int  g_intu_wb_height   = 200;
static int  g_intu_wb_depth    = 2;

/* Apply the current internal Preferences snapshot to the running system.
 * Colours are mapped to the Workbench palette, font/dimensions are stored
 * for callers, and the desktop is repainted. */
static void apply_prefs(void)
{
    uint16_t c0 = read_host_u16(&g_intu_prefs[PREF_OFF_COLOR0]);
    uint16_t c1 = read_host_u16(&g_intu_prefs[PREF_OFF_COLOR1]);
    uint16_t c2 = read_host_u16(&g_intu_prefs[PREF_OFF_COLOR2]);
    uint16_t c3 = read_host_u16(&g_intu_prefs[PREF_OFF_COLOR3]);

    WB_GREY       = amiga_color_to_rgb(c0);
    WB_BLACK      = amiga_color_to_rgb(c1);
    WB_WHITE      = amiga_color_to_rgb(c2);
    WB_BLUE       = amiga_color_to_rgb(c3);
    WB_DARK_GREY  = scale_rgb(WB_GREY, 1, 2);
    WB_LIGHT_GREY = scale_rgb(WB_GREY, 3, 2);
    WB_LIGHT_BLUE = scale_rgb(WB_BLUE, 3, 2);
    WB_CREAM      = (WB_WHITE == FB_RGB(0,0,0)) ? FB_RGB(0xFF,0xFF,0xCC) : WB_WHITE;

    g_intu_font_height = (int)g_intu_prefs[PREF_OFF_FONTHEIGHT];
    if (g_intu_font_height <= 0) g_intu_font_height = 8;

    g_intu_wb_width  = (int)read_host_u16(&g_intu_prefs[PREF_OFF_WBWIDTH]);
    g_intu_wb_height = (int)read_host_u16(&g_intu_prefs[PREF_OFF_WBHEIGHT]);
    g_intu_wb_depth  = (int)g_intu_prefs[PREF_OFF_WBDEPTH];
    if (g_intu_wb_width  <= 0) g_intu_wb_width  = 640;
    if (g_intu_wb_height <= 0) g_intu_wb_height = 200;
    if (g_intu_wb_depth  <= 0) g_intu_wb_depth  = 2;

    extern void WM_Redraw(void);
    WM_Redraw();
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

    apply_prefs();
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

/* QueryOverscan(displayID, rect, oScanType) — D0, A0, D1; returns 0 in D0 */
static void intuition_QueryOverscan(void)
{
    uint32_t display_id = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t rect_ptr   = m68k_get_reg(NULL, M68K_REG_A0);
    int16_t  oscan_type = (int16_t)m68k_get_reg(NULL, M68K_REG_D1);
    (void)display_id; (void)oscan_type;

    if (rect_ptr) {
        mem_w16(rect_ptr + 0, 0);
        mem_w16(rect_ptr + 2, 0);
        mem_w16(rect_ptr + 4, (int16_t)(g_fb.width  - 1));
        mem_w16(rect_ptr + 6, (int16_t)(g_fb.height - 1));
    }
    m68k_set_reg(M68K_REG_D0, 0);
}

/* GetDisplayInfoData(handle, buf, size, tagID, displayID)
 * A0, A1, D0, D1, A2 — delegates to graphics.library. */
static void intuition_GetDisplayInfoData(void)
{
    UAOS_Graphics_Dispatch(GFX_SLOT_GETDISPLAYINFODATA);
}

/* NextDisplayInfo(displayID) — D0; delegates to graphics.library. */
static void intuition_NextDisplayInfo(void)
{
    UAOS_Graphics_Dispatch(GFX_SLOT_NEXTDISPLAYINFO);
}

/* -------------------------------------------------------------------------
 * Missing utility functions
 * ------------------------------------------------------------------------- */

static char     g_default_pub_screen[32] = "Workbench";

/* CurrentTime(seconds, micros) — A0, A1
 * Returns the host wall-clock time in AmigaOS format:
 * seconds since 1978-01-01 00:00:00 and microseconds. */
static int current_time_is_leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static int32_t current_time_days_since_1978(uint16_t year, uint8_t month, uint8_t day)
{
    int32_t days = 0;
    for (int y = 1978; y < year; y++)
        days += current_time_is_leap(y) ? 366 : 365;
    static const int mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    for (int m = 1; m < month; m++) {
        days += mdays[m - 1];
        if (m == 2 && current_time_is_leap(year)) days++;
    }
    days += day - 1;
    return days;
}

static void intuition_CurrentTime(void)
{
    uint32_t sec_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t mic_ptr = m68k_get_reg(NULL, M68K_REG_A1);

    RtcDateTime dt = RTC_ReadDateTime();
    int32_t days = current_time_days_since_1978(dt.year, dt.month, dt.day);
    uint32_t seconds = (uint32_t)(days * 86400LL +
                                  dt.hour * 3600LL +
                                  dt.min   * 60LL +
                                  dt.sec);
    /* PIT ticks at 100 Hz give sub-second resolution: 1 tick = 10 ms. */
    uint32_t micros = (uint32_t)((g_pit_ticks % 100) * 10000ULL);

    if (sec_ptr) mem_w32(sec_ptr, seconds);
    if (mic_ptr) mem_w32(mic_ptr, micros);
}

/* DoubleClick(sSeconds, sMicros, cSeconds, cMicros) — D0/D1/D2/D3; returns BOOL in D0 */
static void intuition_DoubleClick(void)
{
    uint32_t s_sec = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t s_mic = m68k_get_reg(NULL, M68K_REG_D1);
    uint32_t c_sec = m68k_get_reg(NULL, M68K_REG_D2);
    uint32_t c_mic = m68k_get_reg(NULL, M68K_REG_D3);

    uint32_t diff_sec  = (c_sec > s_sec) ? (c_sec - s_sec) : (s_sec - c_sec);
    uint32_t diff_mic  = (c_mic > s_mic) ? (c_mic - s_mic) : (s_mic - c_mic);
    uint32_t diff      = diff_sec * 1000000 + diff_mic;

    m68k_set_reg(M68K_REG_D0, diff < 500000 ? 1 : 0);
}

/* ReportMouse(flag, window) — D0, A0 */
static void intuition_ReportMouse(void)
{
    uint32_t win_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t flag    = m68k_get_reg(NULL, M68K_REG_D0);
    if (!win_ptr) return;

    uint32_t flags = mem_u32(win_ptr + WIN_OFF_FLAGS);
    if (flag)
        flags |= WFLG_REPORTMOUSE;
    else
        flags &= ~WFLG_REPORTMOUSE;
    mem_w32(win_ptr + WIN_OFF_FLAGS, flags);
}

/* DisplayBeep(screen) — A0
 * Flash the active desktop background briefly.  The screen argument is
 * ignored because UAOS maps all screens onto a single desktop backdrop. */
static void intuition_DisplayBeep(void)
{
    (void)m68k_get_reg(NULL, M68K_REG_A0);

    Desktop_DisplayBeepFlash(WB_WHITE);
    WM_Redraw();
    uint64_t until = g_pit_ticks + 5;  /* 50 ms flash at 100 Hz */
    while (g_pit_ticks < until) __asm__ __volatile__("pause");
    Desktop_DisplayBeepFlash(0);
    WM_Redraw();
}

/* InitRequester(requester) — A0 */
static void intuition_InitRequester(void)
{
    uint32_t req = m68k_get_reg(NULL, M68K_REG_A0);
    if (req) {
        for (int i = 0; i < REQ_SIZE; i++) mem_w8(req + i, 0);
    }
}

/* EndRequest(requester, window) — A0, A1
 * The requester window is already closed when Request() returns, so this
 * only has to ensure no active requester is left behind. */
static void intuition_EndRequest(void)
{
    (void)m68k_get_reg(NULL, M68K_REG_A0);
    (void)m68k_get_reg(NULL, M68K_REG_A1);
    if (g_req_slot.active) free_requester_internal();
}

/* Request(requester, window) — A0, A1; returns BOOL in D0
 * Implements a synchronous modal requester.  Body text is read from
 * req_Text and the caller's gadget list is used for button labels. */
static void intuition_Request(void)
{
    uint32_t req = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t win = m68k_get_reg(NULL, M68K_REG_A1);
    (void)win;

    if (!req) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }

    if (g_req_slot.active) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }

    char body[256] = "";
    uint32_t text_ptr = mem_u32(req + REQ_OFF_REQTEXT);
    if (text_ptr) {
        /* Treat it as an IntuiText* first, then fall back to raw text. */
        uint32_t itext = mem_u32(text_ptr + ITEXT_OFF_ITEXT);
        if (itext) guest_str(body, itext, sizeof(body));
        else       guest_str(body, text_ptr, sizeof(body));
    }

    const char *buttons[REQ_MAX_BUTTONS];
    int num_buttons = 0;
    char btn_labels[REQ_MAX_BUTTONS][32];

    uint32_t gad = mem_u32(req + REQ_OFF_REQGADGETS);
    while (gad && num_buttons < REQ_MAX_BUTTONS) {
        btn_labels[num_buttons][0] = '\0';
        uint32_t label = mem_u32(gad + GAD_OFF_GADGETTEXT);
        if (label) {
            uint32_t label_text = mem_u32(label + ITEXT_OFF_ITEXT);
            if (label_text)
                guest_str(btn_labels[num_buttons], label_text, sizeof(btn_labels[num_buttons]));
        }
        if (!btn_labels[num_buttons][0])
            local_str_copy(btn_labels[num_buttons], "OK", sizeof(btn_labels[num_buttons]));
        buttons[num_buttons] = btn_labels[num_buttons];
        num_buttons++;
        gad = mem_u32(gad + GAD_OFF_NEXTGADGET);
    }
    if (num_buttons == 0) {
        local_str_copy(btn_labels[0], "OK", sizeof(btn_labels[0]));
        buttons[0] = btn_labels[0];
        num_buttons = 1;
    }

    uint32_t req_win = build_requester_internal(win, "Request", body, num_buttons, buttons,
                                               (int16_t)mem_u16(req + REQ_OFF_WIDTH),
                                               (int16_t)mem_u16(req + REQ_OFF_HEIGHT));
    if (!req_win) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }

    int result = wait_requester_internal();
    free_requester_internal();

    /* Rightmost gadget is FALSE (0), others are TRUE (1). */
    m68k_set_reg(M68K_REG_D0, result == (num_buttons - 1) ? 0 : 1);
}

/* ViewAddress() — returns the single guest View in D0 */
static void intuition_ViewAddress(void)
{
    if (!g_intu_view)
        g_intu_view = intu_alloc(64);
    m68k_set_reg(M68K_REG_D0, g_intu_view);
}

/* ViewPortAddress(window) — A0; returns ViewPort* in D0 */
static void intuition_ViewPortAddress(void)
{
    uint32_t win_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    if (!win_ptr) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }
    uint32_t screen = mem_u32(win_ptr + WIN_OFF_WSCREEN);
    if (screen) {
        m68k_set_reg(M68K_REG_D0, screen + SCR_OFF_VIEWPORT);
    } else {
        m68k_set_reg(M68K_REG_D0, 0);
    }
}

/* GetScreenData(buffer, size, type, screen) — A0, D0, D1, A1
 *
 * Type is an AmigaOS screen type flag:
 *   CUSTOMSCREEN (0)     -> use the supplied Screen pointer
 *   WBENCHSCREEN (1)     -> copy from the Workbench screen
 *   PUBLICSCREEN (2)     -> copy from the default public screen
 *   Other type values    -> use the supplied Screen pointer if active,
 *                           otherwise zero the buffer. */
static void intuition_GetScreenData(void)
{
    uint32_t buf   = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t size  = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t type  = m68k_get_reg(NULL, M68K_REG_D1);
    uint32_t screen = m68k_get_reg(NULL, M68K_REG_A1);

    if (!buf) return;
    if (size > 256) size = 256;

    uint32_t src = 0;
    switch (type) {
        case CUSTOMSCREEN:
            src = screen;
            break;
        case WBENCHSCREEN:
            src = find_pub_screen_by_name("Workbench");
            break;
        case PUBLICSCREEN:
            src = get_default_pub_screen();
            break;
        default:
            /* Additional type values: fall back to the supplied screen
             * if it looks like a valid guest screen. */
            if (screen && screen + SCR_SIZE <= GUEST_RAM_SIZE)
                src = screen;
            break;
    }

    if (src) {
        for (uint32_t i = 0; i < size; i++)
            mem_w8(buf + i, mem_u8(src + i));
    } else {
        memset(g_ram + buf, 0, size);
    }
}

/* NextPubScreen(screen, namebuf) — A0, A1; returns next Screen* in D0.
 * Walks the public-screen list in order.  A0 = NULL starts at the first
 * public screen; A0 = a previously returned screen returns the one after it. */
static void intuition_NextPubScreen(void)
{
    uint32_t screen = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t namebuf = m68k_get_reg(NULL, M68K_REG_A1);

    int start = 0;
    if (screen) {
        ScreenSlot *slot = find_screen_slot(screen);
        if (!slot) {
            m68k_set_reg(M68K_REG_D0, 0);
            return;
        }
        start = (int)(slot - g_intu_screens) + 1;
    }

    uint32_t next = 0;
    const char *name = NULL;
    for (int i = start; i < MAX_INTUITION_SCREENS; i++) {
        if (g_intu_screens[i].active && g_intu_screens[i].pub_name[0]) {
            next = g_intu_screens[i].guest_screen;
            name = g_intu_screens[i].pub_name;
            break;
        }
    }

    if (namebuf && name) {
        int i = 0;
        while (i < 64 && name[i]) {
            mem_w8(namebuf + i, (uint8_t)name[i]);
            i++;
        }
        mem_w8(namebuf + i, 0);
    }

    m68k_set_reg(M68K_REG_D0, next);
}

/* SetDefaultPubScreen(name) — A0 */
static void intuition_SetDefaultPubScreen(void)
{
    uint32_t name_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    if (name_ptr) {
        guest_str(g_default_pub_screen, name_ptr, sizeof(g_default_pub_screen));
    }
}

/* GetDefaultPubScreen(nameBuffer) — A0; returns Screen* in D0 */
static void intuition_GetDefaultPubScreen(void)
{
    uint32_t namebuf = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t screen = get_default_pub_screen();
    const char *name = "Workbench";

    if (screen) {
        ScreenSlot *slot = find_screen_slot(screen);
        if (slot && slot->pub_name[0]) name = slot->pub_name;
    }

    if (namebuf) {
        int i = 0;
        while (i < 31 && name[i]) {
            mem_w8(namebuf + i, (uint8_t)name[i]);
            i++;
        }
        mem_w8(namebuf + i, 0);
    }

    m68k_set_reg(M68K_REG_D0, screen);
}

/* PubScreenStatus(screen, statusFlags) — A0, D0
 * statusFlags: PS_OPENED, PS_PUBLIC, PS_PRIVATE, PS_NOTIFY. */
#define PS_OPENED  1
#define PS_PUBLIC  2
#define PS_PRIVATE 4
#define PS_NOTIFY  8

static void intuition_PubScreenStatus(void)
{
    uint32_t screen = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t status = m68k_get_reg(NULL, M68K_REG_D0);
    int result = 0;

    if (screen) {
        ScreenSlot *slot = find_screen_slot(screen);
        if (slot) {
            if (status & PS_PUBLIC) {
                if (slot->pub_name[0] == '\0') {
                    uint32_t title_ptr = mem_u32(screen + SCR_OFF_TITLE);
                    if (title_ptr) {
                        guest_str(slot->pub_name, title_ptr, sizeof(slot->pub_name));
                    }
                    if (slot->pub_name[0] == '\0') {
                        const char *defname = "PubScreen";
                        int dn = 0;
                        while (dn < (int)sizeof(slot->pub_name) - 1 && defname[dn]) {
                            slot->pub_name[dn] = defname[dn];
                            dn++;
                        }
                        slot->pub_name[dn] = '\0';
                    }
                }
                signal_pub_screen(slot);
            } else if (status & PS_PRIVATE) {
                slot->pub_name[0] = '\0';
                signal_pub_screen(slot);
            } else if (status & PS_NOTIFY) {
                signal_pub_screen(slot);
            }
            result = 1;
        }
    }
    m68k_set_reg(M68K_REG_D0, result ? 1 : 0);
}

/* SysReqHandler(window, idcmpPtr, waitInput) — A0, A1, D0
 * Returns the IDCMP class of the next requester message in the window's
 * UserPort.  If waitInput is non-zero, polls briefly for a message to arrive.
 * The caller is responsible for retrieving and freeing the actual message. */
static void intuition_SysReqHandler(void)
{
    uint32_t win_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t idcmp_ptr = m68k_get_reg(NULL, M68K_REG_A1);
    int wait_input = (int)m68k_get_reg(NULL, M68K_REG_D0);

    if (!win_ptr) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }

    uint32_t user_port = mem_u32(win_ptr + WIN_OFF_USERPORT);
    if (!user_port) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }

    uint32_t list = user_port + MP_OFF_MSGLIST;
    uint32_t msg = 0;

    if (wait_input) {
        for (int i = 0; i < 1000 && !msg; i++) {
            uint32_t head = mem_u32(list + LH_OFF_HEAD);
            uint32_t tail = list + LH_OFF_TAIL;
            if (head != tail) msg = guest_list_remove_head(list);
            if (!msg) {
                Task_Yield();
            }
        }
    } else {
        uint32_t head = mem_u32(list + LH_OFF_HEAD);
        uint32_t tail = list + LH_OFF_TAIL;
        if (head != tail) msg = guest_list_remove_head(list);
    }

    uint32_t result = 0;
    if (msg) {
        result = mem_u32(msg + IM_OFF_CLASS);
        if (idcmp_ptr) mem_w32(idcmp_ptr, result);
        intu_free(msg);
    }
    m68k_set_reg(M68K_REG_D0, result);
}

/* LockIBase(dontknow) — D0; returns lock in D0
 * A real recursive-style lock: a non-zero counter is returned and incremented.
 * The caller passes the previous value to UnlockIBase. */
static uint32_t g_ibase_lock_count = 0;

static void intuition_LockIBase(void)
{
    (void)m68k_get_reg(NULL, M68K_REG_D0);
    g_ibase_lock_count++;
    m68k_set_reg(M68K_REG_D0, g_ibase_lock_count);
}

/* UnlockIBase(ibLock) — A0 */
static void intuition_UnlockIBase(void)
{
    (void)m68k_get_reg(NULL, M68K_REG_A0);
    if (g_ibase_lock_count > 0) g_ibase_lock_count--;
}

/* ShowWindow(window) — A0 */
static void intuition_ShowWindow(void)
{
    uint32_t win_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    IntuitionSlot *slot = find_slot_by_guest(win_ptr);
    if (slot) WM_RaiseWindow(slot->wm_handle);
}

/* HideWindow(window) — A0 */
static void intuition_HideWindow(void)
{
    uint32_t win_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    IntuitionSlot *slot = find_slot_by_guest(win_ptr);
    if (slot) WM_LowerWindow(slot->wm_handle);
}

/* WindowLimits(window, widthMin, heightMin, widthMax, heightMax)
 * A0, D0, D1, D2, D3; returns BOOL in D0 */
static void intuition_WindowLimits(void)
{
    uint32_t win_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    int16_t  min_w   = (int16_t)m68k_get_reg(NULL, M68K_REG_D0);
    int16_t  min_h   = (int16_t)m68k_get_reg(NULL, M68K_REG_D1);
    int16_t  max_w   = (int16_t)m68k_get_reg(NULL, M68K_REG_D2);
    int16_t  max_h   = (int16_t)m68k_get_reg(NULL, M68K_REG_D3);

    IntuitionSlot *slot = find_slot_by_guest(win_ptr);
    if (!slot) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }
    slot->min_w = min_w;
    slot->min_h = min_h;
    slot->max_w = max_w;
    slot->max_h = max_h;
    m68k_set_reg(M68K_REG_D0, 1);
}

/* ChangeWindowBox(window, left, top, width, height)
 * A0, D0, D1, D2, D3 */
static void intuition_ChangeWindowBox(void)
{
    uint32_t win_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    int16_t  left    = (int16_t)m68k_get_reg(NULL, M68K_REG_D0);
    int16_t  top     = (int16_t)m68k_get_reg(NULL, M68K_REG_D1);
    int16_t  width   = (int16_t)m68k_get_reg(NULL, M68K_REG_D2);
    int16_t  height  = (int16_t)m68k_get_reg(NULL, M68K_REG_D3);

    IntuitionSlot *slot = find_slot_by_guest(win_ptr);
    if (!slot) return;

    if (width  < 64) width  = 64;
    if (height < 32) height = 32;

    mem_w16(win_ptr + WIN_OFF_LEFTEDGE, left);
    mem_w16(win_ptr + WIN_OFF_TOPEDGE,  top);
    mem_w16(win_ptr + WIN_OFF_WIDTH,    width);
    mem_w16(win_ptr + WIN_OFF_HEIGHT,   height);

    WM_MoveWindow(slot->wm_handle, left, top);
    WM_RepaintWindow(slot->wm_handle);
}

/* GetScreenDrawInfo(screen) — A0; returns DrawInfo* in D0
 * Builds a real DrawInfo that mirrors the screen's font and pens. */
static void intuition_GetScreenDrawInfo(void)
{
    uint32_t screen = m68k_get_reg(NULL, M68K_REG_A0);
    m68k_set_reg(M68K_REG_D0, alloc_screen_draw_info(screen));
}

/* FreeScreenDrawInfo(screen, drawInfo) — A0, A1 */
static void intuition_FreeScreenDrawInfo(void)
{
    (void)m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t dri = m68k_get_reg(NULL, M68K_REG_A1);
    intu_free(dri);
}

/* MoveWindowInFrontOf(window, behind) — A0, A1
 * Moves 'window' directly in front of 'behind'.  If behind is NULL, move to front. */
static void intuition_MoveWindowInFrontOf(void)
{
    uint32_t win_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t behind_ptr = m68k_get_reg(NULL, M68K_REG_A1);
    IntuitionSlot *src_slot = find_slot_by_guest(win_ptr);
    IntuitionSlot *behind_slot = behind_ptr ? find_slot_by_guest(behind_ptr) : NULL;
    if (src_slot) {
        int src_handle = src_slot->wm_handle;
        int behind_handle = behind_slot ? behind_slot->wm_handle : -1;
        WM_MoveWindowInFrontOf(src_handle, behind_handle);
    }
}

/* SetEditHook(gadget, hook) — A0, A1; returns old hook in D0 */
static void intuition_SetEditHook(void)
{
    uint32_t gad = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t hook = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t old = 0;
    if (gad) {
        old = mem_u32(gad + GAD_OFF_USERDATA);
        mem_w32(gad + GAD_OFF_USERDATA, hook);
    }
    m68k_set_reg(M68K_REG_D0, old);
}

/* ObtainGIRPort(gadgetInfo) — A0; returns temporary RastPort* in D0 */
static void intuition_ObtainGIRPort(void)
{
    (void)m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t rp = intu_alloc(RP_SIZE_MIN);
    if (rp) init_guest_rastport(rp, 0);
    m68k_set_reg(M68K_REG_D0, rp);
}

/* ReleaseGIRPort(rastPort) — A0 */
static void intuition_ReleaseGIRPort(void)
{
    uint32_t rp = m68k_get_reg(NULL, M68K_REG_A0);
    intu_free(rp);
}

/* StripIntuiMessages(idcmpMask, msgPort) — D0, A0; returns count in D0 */
static void intuition_StripIntuiMessages(void)
{
    uint32_t mask = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t port = m68k_get_reg(NULL, M68K_REG_A0);
    if (!port || !mask) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }
    uint32_t list = port + MP_OFF_MSGLIST;
    uint32_t node = mem_u32(list + LH_OFF_HEAD);
    uint32_t tail = list + LH_OFF_TAIL;
    int removed = 0;
    while (node && node != tail) {
        uint32_t next = mem_u32(node + MSG_OFF_LN_SUCC);
        uint32_t msg_class = mem_u32(node + IM_OFF_CLASS);
        if (msg_class & mask) {
            uint32_t pred = mem_u32(node + MSG_OFF_LN_PRED);
            mem_w32(pred + MSG_OFF_LN_SUCC, next);
            mem_w32(next + MSG_OFF_LN_PRED, pred);
            intu_free(node);
            removed++;
        }
        node = next;
    }
    m68k_set_reg(M68K_REG_D0, (uint32_t)removed);
}

/* =========================================================================
 * BOOPSI — real object/class registry, attribute and method dispatch
 * ========================================================================= */

#define MAX_BOOPSI_CLASSES 32
#define MAX_BOOPSI_NEST    8

typedef struct {
    uint32_t class_ptr;
    uint8_t  active;
} BoopsiClassEntry;

static BoopsiClassEntry g_boopsi_classes[MAX_BOOPSI_CLASSES];
static uint32_t         g_boopsi_class_stack[MAX_BOOPSI_NEST];
static int              g_boopsi_nest = 0;

/* Get the class pointer from an object's header. The _Object header is
 * immediately before the object (instance-data) pointer. */
static uint32_t boopsi_object_class(uint32_t object)
{
    if (!object || object < OBJ_HEADER_SIZE) return 0;
    return mem_u32(object - OBJ_HEADER_SIZE + OBJ_OFF_CLASS);
}

static void boopsi_set_object_class(uint32_t object, uint32_t cls)
{
    if (object >= OBJ_HEADER_SIZE)
        mem_w32(object - OBJ_HEADER_SIZE + OBJ_OFF_CLASS, cls);
}

static int class_id_matches(uint32_t wanted, uint32_t cls_id)
{
    if (!wanted || !cls_id) return 0;
    if (wanted == cls_id) return 1;
    /* ClassID may be a string pointer; compare the string contents. */
    if (wanted < GUEST_RAM_SIZE && cls_id < GUEST_RAM_SIZE) {
        char a[64], b[64];
        guest_str(a, wanted, sizeof(a));
        guest_str(b, cls_id, sizeof(b));
        int i = 0;
        while (a[i] && b[i] && a[i] == b[i]) i++;
        if (a[i] == '\0' && b[i] == '\0') return 1;
    }
    return 0;
}

static uint32_t find_public_class(uint32_t class_id)
{
    if (!class_id) return 0;
    for (int i = 0; i < MAX_BOOPSI_CLASSES; i++) {
        if (!g_boopsi_classes[i].active || !g_boopsi_classes[i].class_ptr)
            continue;
        uint32_t id = mem_u32(g_boopsi_classes[i].class_ptr + CLASS_OFF_ID);
        if (class_id_matches(class_id, id))
            return g_boopsi_classes[i].class_ptr;
    }
    return 0;
}

static void push_stack_msg(uint32_t addr, uint32_t method,
                           uint32_t p1, uint32_t p2)
{
    mem_w32(addr + 0, method);
    mem_w32(addr + 4, p1);
    mem_w32(addr + 8, p2);
}

static uint32_t alloc_stack_msg(uint32_t method, uint32_t p1, uint32_t p2)
{
    uint32_t sp = m68k_get_reg(NULL, M68K_REG_A7);
    sp -= 12;
    push_stack_msg(sp, method, p1, p2);
    m68k_set_reg(M68K_REG_A7, sp);
    return sp;
}

static uint32_t alloc_stack_msg_4(uint32_t method)
{
    uint32_t sp = m68k_get_reg(NULL, M68K_REG_A7);
    sp -= 4;
    mem_w32(sp, method);
    m68k_set_reg(M68K_REG_A7, sp);
    return sp;
}

static void free_stack_msg(uint32_t size)
{
    uint32_t sp = m68k_get_reg(NULL, M68K_REG_A7);
    sp += size;
    m68k_set_reg(M68K_REG_A7, sp);
}

/* Dispatch a BOOPSI method. start_class is the class whose dispatcher is
 * invoked; if zero the object's own class is used. Returns the dispatcher's
 * D0 result. */
static uint32_t boopsi_dispatch(uint32_t object, uint32_t method,
                                uint32_t msg, uint32_t start_class)
{
    if (!object) return 0;
    uint32_t cls = start_class ? start_class : boopsi_object_class(object);
    if (!cls) return 0;

    if (g_boopsi_nest >= MAX_BOOPSI_NEST) return 0;
    g_boopsi_class_stack[g_boopsi_nest++] = cls;

    uint32_t use_msg = msg;
    if (!use_msg) {
        /* Method with no parameters: push a one-longword message. */
        use_msg = alloc_stack_msg_4(method);
    }

    uint32_t result = 0;
    uint32_t flags = mem_u32(cls + CLASS_OFF_FLAGS);
    int native_dispatcher = (flags & CLASS_FLAG_NATIVE) ? 1 : 0;
    if (native_dispatcher) {
        uint32_t native = mem_u32(cls + CLASS_OFF_NATIVE_DISPATCHER);
        if (native) {
            typedef uint32_t (*NativeDispatcher)(uint32_t cls, uint32_t obj, uint32_t msg);
            result = ((NativeDispatcher)(uintptr_t)native)(cls, object, use_msg);
        }
    } else {
        uint32_t entry = mem_u32(cls + CLASS_OFF_DISPATCHER_ENTRY);
        if (entry) {
            result = UAOS_InvokeM68kHook(entry, cls, object, use_msg);
        }
    }

    /* The auto-allocated one-longword message must be freed only for native
     * dispatchers. M68k dispatchers restore the entire CPU context, so the
     * stack pointer is already back to the pre-allocation value. */
    if (!msg && native_dispatcher) free_stack_msg(4);

    g_boopsi_nest--;
    return result;
}

/* Call the superclass of the class currently handling a method. */
static uint32_t boopsi_super_dispatch(uint32_t object, uint32_t method,
                                      uint32_t msg)
{
    if (g_boopsi_nest <= 0) return 0;
    uint32_t current = g_boopsi_class_stack[g_boopsi_nest - 1];
    uint32_t super = mem_u32(current + CLASS_OFF_SUPER);
    if (!super) return 0;
    return boopsi_dispatch(object, method, msg, super);
}

static uint32_t alloc_boopsi_object(uint32_t cls)
{
    if (!cls) return 0;
    uint16_t inst_offset = mem_u16(cls + CLASS_OFF_INST_OFFSET);
    uint16_t inst_size = mem_u16(cls + CLASS_OFF_INST_SIZE);
    uint32_t total = OBJ_HEADER_SIZE + inst_offset + inst_size;
    if (total < OBJ_HEADER_SIZE) return 0;
    uint32_t base = intu_alloc(total);
    if (!base) return 0;
    memset(&g_ram[base], 0, total);
    return base + OBJ_HEADER_SIZE;
}

static void free_boopsi_object(uint32_t object)
{
    if (!object || object < OBJ_HEADER_SIZE) return;
    intu_free(object - OBJ_HEADER_SIZE);
}

/* NewObjectA(classPtr, classID, tagList) — A0, A1, A2
 * Returns object pointer in D0. */
static void intuition_NewObjectA(void)
{
    uint32_t class_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t class_id  = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t tag_list  = m68k_get_reg(NULL, M68K_REG_A2);

    uint32_t cls = class_ptr ? class_ptr : find_public_class(class_id);
    if (!cls) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }

    uint32_t object = alloc_boopsi_object(cls);
    if (!object) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }
    boopsi_set_object_class(object, cls);

    uint32_t msg = alloc_stack_msg(OM_NEW, tag_list, 0);
    uint32_t result = boopsi_dispatch(object, OM_NEW, msg, cls);
    free_stack_msg(12);

    if (!result) {
        free_boopsi_object(object);
        object = 0;
    }
    m68k_set_reg(M68K_REG_D0, object);
}

/* DisposeObject(object) — A0 */
static void intuition_DisposeObject(void)
{
    uint32_t object = m68k_get_reg(NULL, M68K_REG_A0);
    if (!object) return;
    boopsi_dispatch(object, OM_DISPOSE, 0, 0);
    free_boopsi_object(object);
}

/* SetAttrsA(object, tagList, ginfo) — A0, A1, A2 */
static void intuition_SetAttrsA(void)
{
    uint32_t object   = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t tag_list = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t ginfo    = m68k_get_reg(NULL, M68K_REG_A2);
    if (!object) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }
    uint32_t msg = alloc_stack_msg(OM_SET, tag_list, ginfo);
    uint32_t result = boopsi_dispatch(object, OM_SET, msg, 0);
    free_stack_msg(12);
    m68k_set_reg(M68K_REG_D0, result);
}

/* GetAttr(attrID, object, storagePtr) — D0, A0, A1 */
static void intuition_GetAttr(void)
{
    uint32_t attr_id = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t object  = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t storage = m68k_get_reg(NULL, M68K_REG_A1);
    if (!object || !storage) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }
    uint32_t msg = alloc_stack_msg(OM_GET, attr_id, storage);
    uint32_t result = boopsi_dispatch(object, OM_GET, msg, 0);
    free_stack_msg(12);
    m68k_set_reg(M68K_REG_D0, result);
}

/* GetAttrsA(object, tagList) — A0, A1 */
static void intuition_GetAttrsA(void)
{
    uint32_t object   = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t tag_list = m68k_get_reg(NULL, M68K_REG_A1);
    if (!object || !tag_list) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }
    uint32_t success = 1;
    uint32_t p = tag_list;
    while (p + 8 <= GUEST_RAM_SIZE) {
        uint32_t tag = mem_u32(p);
        if (tag == TAG_DONE) break;
        uint32_t storage = mem_u32(p + 4);
        if (storage) {
            uint32_t msg = alloc_stack_msg(OM_GET, tag, storage);
            uint32_t rc = boopsi_dispatch(object, OM_GET, msg, 0);
            free_stack_msg(12);
            if (!rc) success = 0;
        }
        p += 8;
    }
    m68k_set_reg(M68K_REG_D0, success);
}

/* SetSuperAttrsA(object, tagList, ginfo) — A0, A1, A2 */
static void intuition_SetSuperAttrsA(void)
{
    uint32_t object   = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t tag_list = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t ginfo    = m68k_get_reg(NULL, M68K_REG_A2);
    if (!object) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }
    uint32_t msg = alloc_stack_msg(OM_SET, tag_list, ginfo);
    uint32_t result = boopsi_super_dispatch(object, OM_SET, msg);
    free_stack_msg(12);
    m68k_set_reg(M68K_REG_D0, result);
}

/* DoMethodA(object, method, msg) — A0, D0, A1 */
static void intuition_DoMethodA(void)
{
    uint32_t object = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t method = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t msg    = m68k_get_reg(NULL, M68K_REG_A1);
    if (!object) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }
    uint32_t result = boopsi_dispatch(object, method, msg, 0);
    m68k_set_reg(M68K_REG_D0, result);
}

/* DoSuperMethodA(object, method, msg) — A0, D0, A1 */
static void intuition_DoSuperMethodA(void)
{
    uint32_t object = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t method = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t msg    = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t result = boopsi_super_dispatch(object, method, msg);
    m68k_set_reg(M68K_REG_D0, result);
}

/* CoerceMethodA(class, object, method, msg) — A0, A1, D0, A2 */
static void intuition_CoerceMethodA(void)
{
    uint32_t cls    = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t object = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t method = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t msg    = m68k_get_reg(NULL, M68K_REG_A2);
    if (!cls || !object) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }
    uint32_t result = boopsi_dispatch(object, method, msg, cls);
    m68k_set_reg(M68K_REG_D0, result);
}

/* DoGadgetMethodA(gadget, window, requester, method, msg) — A0, A1, A2, D0, A3 */
static void intuition_DoGadgetMethodA(void)
{
    uint32_t gad    = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t method = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t msg    = m68k_get_reg(NULL, M68K_REG_A3);
    (void)m68k_get_reg(NULL, M68K_REG_A1);
    (void)m68k_get_reg(NULL, M68K_REG_A2);
    if (!gad) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }
    uint32_t result = boopsi_dispatch(gad, method, msg, 0);
    m68k_set_reg(M68K_REG_D0, result);
}

/* MakeClass(classID, superClassID, superClassPtr, instanceSize, flags)
 * A0, A1, A2, D0, D1 — returns class pointer in D0. */
static void intuition_MakeClass(void)
{
    uint32_t class_id = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t super_id = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t super_ptr = m68k_get_reg(NULL, M68K_REG_A2);
    uint32_t inst_size = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t flags = m68k_get_reg(NULL, M68K_REG_D1);

    if (!super_ptr && super_id) {
        super_ptr = find_public_class(super_id);
        /* If no public class matched, try matching against the raw ClassID or
         * a private class pointer. */
        if (!super_ptr) {
            for (int i = 0; i < MAX_BOOPSI_CLASSES; i++) {
                if (!g_boopsi_classes[i].active) continue;
                uint32_t ptr = g_boopsi_classes[i].class_ptr;
                if (ptr == super_id || class_id_matches(super_id, mem_u32(ptr + CLASS_OFF_ID))) {
                    super_ptr = ptr;
                    break;
                }
            }
        }
    }

    uint32_t cls = intu_alloc(CLASS_SIZE);
    if (!cls) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }
    memset(&g_ram[cls], 0, CLASS_SIZE);

    uint16_t inst_offset = 0;
    if (super_ptr) {
        uint16_t super_offset = mem_u16(super_ptr + CLASS_OFF_INST_OFFSET);
        uint16_t super_size = mem_u16(super_ptr + CLASS_OFF_INST_SIZE);
        inst_offset = super_offset + super_size;
    }

    mem_w32(cls + CLASS_OFF_ID, class_id);
    mem_w32(cls + CLASS_OFF_SUPER, super_ptr);
    mem_w16(cls + CLASS_OFF_INST_OFFSET, inst_offset);
    mem_w16(cls + CLASS_OFF_INST_SIZE, (uint16_t)inst_size);
    mem_w32(cls + CLASS_OFF_FLAGS, flags);

    m68k_set_reg(M68K_REG_D0, cls);
}

/* FreeClass(class) — A0; returns BOOL in D0. */
static void intuition_FreeClass(void)
{
    uint32_t cls = m68k_get_reg(NULL, M68K_REG_A0);
    if (!cls) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }
    for (int i = 0; i < MAX_BOOPSI_CLASSES; i++) {
        if (g_boopsi_classes[i].active && g_boopsi_classes[i].class_ptr == cls) {
            g_boopsi_classes[i].active = 0;
            g_boopsi_classes[i].class_ptr = 0;
        }
    }
    intu_free(cls);
    m68k_set_reg(M68K_REG_D0, 1);
}

extern void UAOS_BOOPSI_RegisterClass(uint32_t cls);

/* AddClass(class) — A0; returns nothing useful. */
static void intuition_AddClass(void)
{
    uint32_t cls = m68k_get_reg(NULL, M68K_REG_A0);
    UAOS_BOOPSI_RegisterClass(cls);
}

/* Public dispatch helper for built-in classes. */
uint32_t UAOS_BOOPSI_Dispatch(uint32_t object, uint32_t method, uint32_t msg, uint32_t start_class)
{
    return boopsi_dispatch(object, method, msg, start_class);
}

/* Public registration helper for built-in classes. */
void UAOS_BOOPSI_RegisterClass(uint32_t cls)
{
    if (!cls) return;
    for (int i = 0; i < MAX_BOOPSI_CLASSES; i++) {
        if (!g_boopsi_classes[i].active) {
            g_boopsi_classes[i].active = 1;
            g_boopsi_classes[i].class_ptr = cls;
            return;
        }
    }
}

/* RemoveClass(class) — A0; returns nothing useful. */
static void intuition_RemoveClass(void)
{
    uint32_t cls = m68k_get_reg(NULL, M68K_REG_A0);
    if (!cls) return;
    for (int i = 0; i < MAX_BOOPSI_CLASSES; i++) {
        if (g_boopsi_classes[i].active && g_boopsi_classes[i].class_ptr == cls) {
            g_boopsi_classes[i].active = 0;
            g_boopsi_classes[i].class_ptr = 0;
        }
    }
}

/* NextObject(objectPtrPtr) — A0; returns next object in D0. */
static void intuition_NextObject(void)
{
    uint32_t ptrptr = m68k_get_reg(NULL, M68K_REG_A0);
    if (!ptrptr || ptrptr + 4 > GUEST_RAM_SIZE) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }
    uint32_t cur = mem_u32(ptrptr);
    if (!cur) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }
    uint32_t next = mem_u32(cur + OBJ_OFF_LN_SUCC);
    mem_w32(ptrptr, next);
    m68k_set_reg(M68K_REG_D0, next);
}

/* -------------------------------------------------------------------------
 * Helpers for singular-attribute and varargs wrapper LVOs
 * ------------------------------------------------------------------------- */

/* Build a two-item TagItem array on the guest stack for singular Attr calls. */
static uint32_t alloc_stack_taglist(uint32_t tag, uint32_t data)
{
    uint32_t sp = m68k_get_reg(NULL, M68K_REG_A7);
    sp -= 16;
    if (sp + 16 > GUEST_RAM_SIZE) return 0;
    mem_w32(sp, tag);
    mem_w32(sp + 4, data);
    mem_w32(sp + 8, TAG_DONE);
    mem_w32(sp + 12, 0);
    m68k_set_reg(M68K_REG_A7, sp);
    return sp;
}

static void restore_stack(uint32_t sp)
{
    m68k_set_reg(M68K_REG_A7, sp);
}

/* Build a variable-length TagItem array from the varargs on the guest stack.
 * Fixed arguments are in registers; the first vararg is at the return
 * address + 4.  The list is forcibly terminated with TAG_DONE.
 * Returns the new stack pointer pointing at the tag list, or 0 on failure. */
static uint32_t build_varargs_taglist(uint32_t old_sp, uint32_t max_pairs)
{
    uint32_t read = old_sp + 4; /* skip return address */
    uint32_t pairs = 0;
    int done = 0;

    while (read + 8 <= GUEST_RAM_SIZE && pairs < max_pairs) {
        uint32_t tag = mem_u32(read);
        pairs++;
        if (tag == TAG_DONE) { done = 1; break; }
        read += 8;
    }

    if (!done) pairs++; /* force TAG_DONE termination */

    uint32_t size = pairs * 8;
    if (size > old_sp) return 0;
    uint32_t new_sp = old_sp - size;

    read = old_sp + 4;
    uint32_t write = new_sp;
    for (uint32_t i = 0; i < pairs; i++) {
        uint32_t tag  = mem_u32(read);
        uint32_t data = 0;
        if (tag == TAG_DONE) {
            data = 0;
        } else if (i == pairs - 1 && !done) {
            tag = TAG_DONE;
        } else {
            data = mem_u32(read + 4);
        }
        mem_w32(write, tag);
        mem_w32(write + 4, data);
        write += 8;
        read += 8;
    }

    m68k_set_reg(M68K_REG_A7, new_sp);
    return new_sp;
}

/* Build a 12-byte BOOPSI method message on the guest stack from the varargs
 * following the method ID. */
static uint32_t build_varargs_method_msg(uint32_t method, uint32_t old_sp)
{
    if (old_sp + 8 > GUEST_RAM_SIZE) return 0;
    uint32_t new_sp = old_sp - 12;
    if (new_sp + 12 > GUEST_RAM_SIZE) return 0;
    mem_w32(new_sp, method);
    mem_w32(new_sp + 4, mem_u32(old_sp + 4));
    mem_w32(new_sp + 8, mem_u32(old_sp + 8));
    m68k_set_reg(M68K_REG_A7, new_sp);
    return new_sp;
}

/* -------------------------------------------------------------------------
 * New LVOs: HelpControl, screen notify, singular attr calls, varargs wrappers
 * ------------------------------------------------------------------------- */

/* HelpControl(window, flags) — A0, D0
 * Enable/disable gadget help for the window and its help group. */
static void intuition_HelpControl(void)
{
    uint32_t win_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t flags   = m68k_get_reg(NULL, M68K_REG_D0);
    IntuitionSlot *slot = find_slot_by_guest(win_ptr);
    if (slot) slot->help_enabled = (flags & HC_GADGETHELP) ? 1 : 0;
    /* TODO: propagate HC_GADGETHELP to other windows in the same help-group. */
}

/* StartScreenNotifyTagList(tagList) — A0; returns handle in D0. */
static void intuition_StartScreenNotifyTagList(void)
{
    (void)m68k_get_reg(NULL, M68K_REG_A0);
    m68k_set_reg(M68K_REG_D0, 0);
}

/* EndScreenNotify(handle) — A0; returns success in D0. */
static void intuition_EndScreenNotify(void)
{
    (void)m68k_get_reg(NULL, M68K_REG_A0);
    m68k_set_reg(M68K_REG_D0, 1);
}

/* GetWindowAttr(window, attrID, data, size) — A0, D0, A1, D1 */
static void intuition_GetWindowAttr(void)
{
    uint32_t win_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t attr_id = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t storage = m68k_get_reg(NULL, M68K_REG_A1);
    (void)m68k_get_reg(NULL, M68K_REG_D1);

    uint32_t old_sp = m68k_get_reg(NULL, M68K_REG_A7);
    uint32_t tag_list = alloc_stack_taglist(attr_id, storage);
    if (!tag_list) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }
    m68k_set_reg(M68K_REG_A0, win_ptr);
    m68k_set_reg(M68K_REG_A1, tag_list);
    intuition_GetWindowAttrsA();
    restore_stack(old_sp);
}

/* SetWindowAttr(window, attrID, data, size) — A0, D0, A1, D1 */
static void intuition_SetWindowAttr(void)
{
    uint32_t win_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t attr_id = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t data    = m68k_get_reg(NULL, M68K_REG_A1);
    (void)m68k_get_reg(NULL, M68K_REG_D1);

    uint32_t old_sp = m68k_get_reg(NULL, M68K_REG_A7);
    uint32_t tag_list = alloc_stack_taglist(attr_id, data);
    if (!tag_list) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }
    m68k_set_reg(M68K_REG_A0, win_ptr);
    m68k_set_reg(M68K_REG_A1, tag_list);
    m68k_set_reg(M68K_REG_A2, 0);
    intuition_SetWindowAttrsA();
    restore_stack(old_sp);
}

/* GetScreenAttr(screen, attrID, data, size) — A0, D0, A1, D1 */
static void intuition_GetScreenAttr(void)
{
    uint32_t screen_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t attr_id    = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t storage    = m68k_get_reg(NULL, M68K_REG_A1);
    (void)m68k_get_reg(NULL, M68K_REG_D1);

    uint32_t old_sp = m68k_get_reg(NULL, M68K_REG_A7);
    uint32_t tag_list = alloc_stack_taglist(attr_id, storage);
    if (!tag_list) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }
    m68k_set_reg(M68K_REG_A0, screen_ptr);
    m68k_set_reg(M68K_REG_A1, tag_list);
    intuition_GetScreenAttrsA();
    restore_stack(old_sp);
}

/* SetScreenAttr(screen, attrID, data, size) — A0, D0, A1, D1 */
static void intuition_SetScreenAttr(void)
{
    uint32_t screen_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t attr_id    = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t data       = m68k_get_reg(NULL, M68K_REG_A1);
    (void)m68k_get_reg(NULL, M68K_REG_D1);

    uint32_t old_sp = m68k_get_reg(NULL, M68K_REG_A7);
    uint32_t tag_list = alloc_stack_taglist(attr_id, data);
    if (!tag_list) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }
    m68k_set_reg(M68K_REG_A0, screen_ptr);
    m68k_set_reg(M68K_REG_A1, tag_list);
    intuition_SetScreenAttrsA();
    restore_stack(old_sp);
}

/* SetGadgetAttrsA(gadget, window, requester, tagList) — A0, A1, A2, A3 */
static void intuition_SetGadgetAttrsA(void)
{
    uint32_t gadget    = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t window    = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t requester = m68k_get_reg(NULL, M68K_REG_A2);
    uint32_t tag_list  = m68k_get_reg(NULL, M68K_REG_A3);
    (void)requester;

    /* Minimal implementation: pass the window pointer as the GadgetInfo to
     * the generic OM_SET dispatcher. */
    m68k_set_reg(M68K_REG_A0, gadget);
    m68k_set_reg(M68K_REG_A1, tag_list);
    m68k_set_reg(M68K_REG_A2, window);
    intuition_SetAttrsA();
}

/* NewObject(classPtr, classID, tag1, ...) — A0, A1 + varargs on stack */
static void intuition_NewObject(void)
{
    uint32_t class_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t class_id  = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t old_sp    = m68k_get_reg(NULL, M68K_REG_A7);
    uint32_t tag_list  = build_varargs_taglist(old_sp, 32);
    if (!tag_list) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }
    m68k_set_reg(M68K_REG_A0, class_ptr);
    m68k_set_reg(M68K_REG_A1, class_id);
    m68k_set_reg(M68K_REG_A2, tag_list);
    intuition_NewObjectA();
    restore_stack(old_sp);
}

/* SetAttrs(object, tag1, ...) — A0 + varargs on stack */
static void intuition_SetAttrs(void)
{
    uint32_t object = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t old_sp = m68k_get_reg(NULL, M68K_REG_A7);
    uint32_t tag_list = build_varargs_taglist(old_sp, 32);
    if (!tag_list) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }
    m68k_set_reg(M68K_REG_A0, object);
    m68k_set_reg(M68K_REG_A1, tag_list);
    m68k_set_reg(M68K_REG_A2, 0);
    intuition_SetAttrsA();
    restore_stack(old_sp);
}

/* GetAttrs(object, tag1, ...) — A0 + varargs on stack */
static void intuition_GetAttrs(void)
{
    uint32_t object = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t old_sp = m68k_get_reg(NULL, M68K_REG_A7);
    uint32_t tag_list = build_varargs_taglist(old_sp, 32);
    if (!tag_list) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }
    m68k_set_reg(M68K_REG_A0, object);
    m68k_set_reg(M68K_REG_A1, tag_list);
    intuition_GetAttrsA();
    restore_stack(old_sp);
}

/* DoMethod(object, method, ...) — A0, D0 + varargs on stack */
static void intuition_DoMethod(void)
{
    uint32_t object = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t method = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t old_sp = m68k_get_reg(NULL, M68K_REG_A7);
    uint32_t msg = build_varargs_method_msg(method, old_sp);
    if (!msg) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }
    m68k_set_reg(M68K_REG_A0, object);
    m68k_set_reg(M68K_REG_A1, msg);
    intuition_DoMethodA();
    restore_stack(old_sp);
}

/* DoSuperMethod(object, method, ...) — A0, D0 + varargs on stack */
static void intuition_DoSuperMethod(void)
{
    uint32_t object = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t method = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t old_sp = m68k_get_reg(NULL, M68K_REG_A7);
    uint32_t msg = build_varargs_method_msg(method, old_sp);
    if (!msg) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }
    m68k_set_reg(M68K_REG_A0, object);
    m68k_set_reg(M68K_REG_A1, msg);
    intuition_DoSuperMethodA();
    restore_stack(old_sp);
}

/* CoerceMethod(class, object, method, ...) — A0, A1, D0 + varargs on stack */
static void intuition_CoerceMethod(void)
{
    uint32_t cls    = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t object = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t method = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t old_sp = m68k_get_reg(NULL, M68K_REG_A7);
    uint32_t msg = build_varargs_method_msg(method, old_sp);
    if (!msg) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }
    m68k_set_reg(M68K_REG_A0, cls);
    m68k_set_reg(M68K_REG_A1, object);
    m68k_set_reg(M68K_REG_A2, msg);
    intuition_CoerceMethodA();
    restore_stack(old_sp);
}

/* SetGadgetAttrs(gadget, window, requester, tag1, ...) — A0, A1, A2 + varargs */
static void intuition_SetGadgetAttrs(void)
{
    uint32_t gadget    = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t window    = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t requester = m68k_get_reg(NULL, M68K_REG_A2);
    uint32_t old_sp    = m68k_get_reg(NULL, M68K_REG_A7);
    uint32_t tag_list  = build_varargs_taglist(old_sp, 32);
    if (!tag_list) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }
    m68k_set_reg(M68K_REG_A0, gadget);
    m68k_set_reg(M68K_REG_A1, window);
    m68k_set_reg(M68K_REG_A2, requester);
    m68k_set_reg(M68K_REG_A3, tag_list);
    intuition_SetGadgetAttrsA();
    restore_stack(old_sp);
}

/* SetSuperAttrs(class, object, tag1, ...) — A0, A1 + varargs on stack */
static void intuition_SetSuperAttrs(void)
{
    uint32_t cls    = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t object = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t old_sp = m68k_get_reg(NULL, M68K_REG_A7);
    uint32_t tag_list = build_varargs_taglist(old_sp, 32);
    if (!tag_list) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }
    (void)cls;
    m68k_set_reg(M68K_REG_A0, object);
    m68k_set_reg(M68K_REG_A1, tag_list);
    m68k_set_reg(M68K_REG_A2, 0);
    intuition_SetSuperAttrsA();
    restore_stack(old_sp);
}

/* SetWindowPointer(window, tag1, ...) — A0 + varargs on stack */
static void intuition_SetWindowPointer(void)
{
    uint32_t win_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t old_sp  = m68k_get_reg(NULL, M68K_REG_A7);
    uint32_t tag_list = build_varargs_taglist(old_sp, 32);
    if (!tag_list) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }
    m68k_set_reg(M68K_REG_A0, win_ptr);
    m68k_set_reg(M68K_REG_A1, tag_list);
    intuition_SetWindowPointerA();
    restore_stack(old_sp);
}

/* OpenWindowTags(newWindow, tag1, ...) — A0 + varargs on stack */
static void intuition_OpenWindowTags(void)
{
    uint32_t new_window = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t old_sp     = m68k_get_reg(NULL, M68K_REG_A7);
    uint32_t tag_list   = build_varargs_taglist(old_sp, 32);
    if (!tag_list) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }
    m68k_set_reg(M68K_REG_A0, new_window);
    m68k_set_reg(M68K_REG_A1, tag_list);
    intuition_OpenWindowTagList();
    restore_stack(old_sp);
}

/* OpenScreenTags(newScreen, tag1, ...) — A0 + varargs on stack */
static void intuition_OpenScreenTags(void)
{
    uint32_t new_screen = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t old_sp     = m68k_get_reg(NULL, M68K_REG_A7);
    uint32_t tag_list   = build_varargs_taglist(old_sp, 32);
    if (!tag_list) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }
    m68k_set_reg(M68K_REG_A0, new_screen);
    m68k_set_reg(M68K_REG_A1, tag_list);
    intuition_OpenScreenTagList();
    restore_stack(old_sp);
}

/* DoGadgetMethod(gadget, window, requester, method, ...) — A0, A1, A2, D0 + varargs */
static void intuition_DoGadgetMethod(void)
{
    uint32_t gadget = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t method = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t old_sp = m68k_get_reg(NULL, M68K_REG_A7);
    uint32_t msg = build_varargs_method_msg(method, old_sp);
    if (!msg) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }
    m68k_set_reg(M68K_REG_A0, gadget);
    m68k_set_reg(M68K_REG_A3, msg);
    intuition_DoGadgetMethodA();
    restore_stack(old_sp);
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
    intuition_EasyRequest,
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
    intuition_QueryOverscan,
    intuition_GetDisplayInfoData,
    intuition_NextDisplayInfo,
    intuition_CurrentTime,
    intuition_DoubleClick,
    intuition_ReportMouse,
    intuition_DisplayBeep,
    intuition_InitRequester,
    intuition_EndRequest,
    intuition_Request,
    intuition_ViewAddress,
    intuition_ViewPortAddress,
    intuition_GetScreenData,
    intuition_NextPubScreen,
    intuition_SetDefaultPubScreen,
    intuition_LockIBase,
    intuition_UnlockIBase,
    intuition_ShowWindow,
    intuition_HideWindow,
    intuition_WindowLimits,
    intuition_ChangeWindowBox,
    intuition_GetScreenDrawInfo,
    intuition_FreeScreenDrawInfo,
    intuition_DisplayAlert,
    intuition_TimedDisplayAlert,
    intuition_ScreenDepth,
    intuition_ScreenPosition,
    intuition_AddGadget,
    intuition_AddGList,
    intuition_RemoveGadget,
    intuition_RemoveGList,
    intuition_RefreshGList,
    intuition_OnGadget,
    intuition_OffGadget,
    intuition_ModifyProp,
    intuition_NewModifyProp,
    intuition_ActivateGadget,
    intuition_SetWindowAttrsA,
    intuition_GetWindowAttrsA,
    intuition_SetScreenAttrsA,
    intuition_GetScreenAttrsA,
    intuition_GetVisualInfoA,
    intuition_FreeVisualInfo,
    intuition_BeginRefresh,
    intuition_EndRefresh,
    intuition_RefreshGadgets,
    intuition_OnMenu,
    intuition_OffMenu,
    intuition_SysReqHandler,
    intuition_PubScreenStatus,
    intuition_GetDefaultPubScreen,
    intuition_MoveWindowInFrontOf,
    intuition_SetEditHook,
    intuition_ObtainGIRPort,
    intuition_ReleaseGIRPort,
    intuition_StripIntuiMessages,
    intuition_NewObjectA,
    intuition_DisposeObject,
    intuition_SetAttrsA,
    intuition_GetAttr,
    intuition_DoMethodA,
    intuition_DoSuperMethodA,
    intuition_CoerceMethodA,
    intuition_MakeClass,
    intuition_FreeClass,
    intuition_AddClass,
    intuition_RemoveClass,
    intuition_NextObject,
    intuition_GetAttrsA,
    intuition_SetSuperAttrsA,
    intuition_DoGadgetMethodA,
    intuition_HelpControl,
    intuition_StartScreenNotifyTagList,
    intuition_EndScreenNotify,
    intuition_GetWindowAttr,
    intuition_SetWindowAttr,
    intuition_GetScreenAttr,
    intuition_SetScreenAttr,
    intuition_NewObject,
    intuition_SetAttrs,
    intuition_GetAttrs,
    intuition_DoMethod,
    intuition_DoSuperMethod,
    intuition_CoerceMethod,
    intuition_SetGadgetAttrsA,
    intuition_SetSuperAttrs,
    intuition_SetWindowPointer,
    intuition_OpenWindowTags,
    intuition_OpenScreenTags,
    intuition_DoGadgetMethod,
    intuition_SetGadgetAttrs,
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
    WM_SetPaletteFn(intuition_apply_window_palette);

    extern void UAOS_BOOPSI_RegisterBuiltinClasses(void);
    UAOS_BOOPSI_RegisterBuiltinClasses();
}
