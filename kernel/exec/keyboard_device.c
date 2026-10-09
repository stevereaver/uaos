/*
 * keyboard_device.c — UAOS keyboard.device Implementation
 *
 * AmigaOS keyboard.device provides keyboard input operations (UAOS-240).
 *
 * KBD_READEVENT queues an IORequest that completes when the next Amiga
 * rawkey transition arrives.  The event pump feeds transitions through
 * KbdDev_OnRawKey() — the same stream that drives IDCMP_RAWKEY, so a
 * pending guest read sees events without stealing them from Intuition —
 * and the request is replied via the normal IORequest completion path
 * (UAOS_Emu_IOReply — port PutMsg + task signal).
 *
 * Guest structures are big-endian guest RAM — accessed via the kb_r and
 * kb_w helpers only.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include "rom_modules.h"
#include "task.h"
#include "../../emulation/uaos_emu.h"

/* =========================================================================
 * Guest structure offsets (big-endian)
 * ========================================================================= */

/* struct IORequest / IOStdReq */
#define IO_COMMAND       28
#define IO_FLAGS         30
#define IO_ERROR         31
#define IO_ACTUAL        32    /* IOStdReq.io_Actual */
#define IO_LENGTH        36    /* IOStdReq.io_Length */
#define IO_DATA          40    /* IOStdReq.io_Data   */
#define IOSTD_SIZE       48

/* struct InputEvent (devices/inputevent.h — real Amiga layout, NOT the
 * internal intuition_lib.h IE_OFF_* convention used for gadget msgs) */
#define IE_NEXT           0    /* struct InputEvent *ie_Next     */
#define IE_CLASS          4    /* UBYTE  ie_Class                */
#define IE_SUBCLASS       5    /* UBYTE  ie_SubClass             */
#define IE_CODE           6    /* UWORD  ie_Code                 */
#define IE_QUAL           8    /* UWORD  ie_Qualifier            */
#define IE_POS_X         10    /* WORD   ie_position.ie_x        */
#define IE_POS_Y         12    /* WORD   ie_position.ie_y        */
#define IE_TIMESTAMP     14    /* struct timeval ie_TimeStamp    */
#define IE_SIZE          22

#define IECLASS_RAWKEY   0x01  /* devices/inputevent.h           */

/* =========================================================================
 * keyboard.device commands (exec/io.h + devices/keyboard.h)
 * ========================================================================= */

#define CMD_RESET             1
#define CMD_FLUSH             8
#define CMD_NONSTD            9
#define KBD_ADDRESETHANDLER     (CMD_NONSTD + 0)   /* 9  */
#define KBD_RESETHANDLERDONE    (CMD_NONSTD + 1)   /* 10 */
#define KBD_RESETHANDLER        (CMD_NONSTD + 2)   /* 11 */
#define KBD_READEVENT           (CMD_NONSTD + 3)   /* 12 */
#define KBD_READMATRIX          (CMD_NONSTD + 4)   /* 13 */
#define KBD_ADDRESETHANDLERDONE (CMD_NONSTD + 5)   /* 14 */
#define KBD_REMRESETHANDLER     (CMD_NONSTD + 6)   /* 15 */
#define KBD_REMRESETHANDLERDONE (CMD_NONSTD + 7)   /* 16 */

#define IOERR_ABORTED  (-2)
#define IOERR_NOCMD    (-3)

/* =========================================================================
 * Guest memory helpers — big-endian
 * ========================================================================= */

extern uint8_t *g_ram;

static void kb_w16(uint8_t *ram, uint32_t a, uint16_t v)
{
    ram[a] = (uint8_t)(v >> 8);
    ram[a + 1] = (uint8_t)v;
}

static uint16_t kb_r16(uint8_t *ram, uint32_t a)
{
    return (uint16_t)(((uint16_t)ram[a] << 8) | ram[a + 1]);
}

static uint32_t kb_r32(uint8_t *ram, uint32_t a)
{
    return ((uint32_t)ram[a] << 24) | ((uint32_t)ram[a + 1] << 16) |
           ((uint32_t)ram[a + 2] <<  8) |  (uint32_t)ram[a + 3];
}

static void kb_w32(uint8_t *ram, uint32_t a, uint32_t v)
{
    ram[a] = (uint8_t)(v >> 24); ram[a + 1] = (uint8_t)(v >> 16);
    ram[a + 2] = (uint8_t)(v >> 8); ram[a + 3] = (uint8_t)v;
}

/* =========================================================================
 * Pending KBD_READEVENT requests
 * ========================================================================= */

typedef struct {
    uint8_t   active;
    uint32_t  io;      /* guest IOStdReq address   */
    uint8_t  *ram;     /* requester's guest window */
    UaosTask *task;    /* owning task (exit purge) */
    uint32_t  data;    /* io_Data — InputEvent buf */
    uint32_t  len;     /* io_Length                */
} KbdPendingRead;

#define KBD_MAX_PENDING 8
static KbdPendingRead g_kbd_pending[KBD_MAX_PENDING];
static uint32_t g_kbd_open_cnt;

/* Event-pump feed: one Amiga rawkey transition (code | 0x80*up in the low
 * byte, full ie_Qualifier snapshot) — completes every pending
 * KBD_READEVENT with a copy, mirroring input.device's broadcast. */
void KbdDev_OnRawKey(int code, uint16_t qual)
{
    for (int i = 0; i < KBD_MAX_PENDING; i++) {
        KbdPendingRead *r = &g_kbd_pending[i];
        if (!r->active) continue;
        if (r->data && r->data + IE_SIZE <= GUEST_RAM_SIZE &&
            r->len >= IE_SIZE) {
            uint8_t *ram = r->ram;
            uint32_t ie = r->data;
            kb_w32(ram, ie + IE_NEXT, 0);
            ram[ie + IE_CLASS]    = IECLASS_RAWKEY;
            ram[ie + IE_SUBCLASS] = 0;
            kb_w16(ram, ie + IE_CODE, (uint16_t)(code & 0xFF));
            kb_w16(ram, ie + IE_QUAL, qual);
            kb_w32(ram, ie + IE_POS_X, 0);      /* position: none for keys */
            kb_w32(ram, ie + IE_TIMESTAMP, 0);  /* timestamp: not tracked  */
            kb_w32(ram, ie + IE_TIMESTAMP + 4, 0);
            kb_w32(ram, r->io + IO_ACTUAL, IE_SIZE);
        }
        r->ram[r->io + IO_ERROR] = 0;
        UAOS_Emu_IOReply(r->ram, r->io);
        r->active = 0;
    }
}

/* Task-exit purge (UAOS-240): never complete into a recycled window. */
void KbdDev_DropTaskRequests(UaosTask *t)
{
    if (!t) return;
    for (int i = 0; i < KBD_MAX_PENDING; i++)
        if (g_kbd_pending[i].active && g_kbd_pending[i].task == t)
            g_kbd_pending[i].active = 0;
}

/* =========================================================================
 * Device vectors (M68kCPUState marshalled by the glue's emu_rom_call)
 * ========================================================================= */

static void keyboard_OpenDevice(M68kCPUState *cpu)
{
    /* Open (-6): A1 = IORequest, D0 = unit, D1 = flags */
    g_kbd_open_cnt++;
    g_ram[cpu->a[1] + IO_ERROR] = 0;
    cpu->d[0] = 0;
}

static void keyboard_CloseDevice(M68kCPUState *cpu)
{
    /* Close (-12): A1 = IORequest */
    if (g_kbd_open_cnt) g_kbd_open_cnt--;
    for (int i = 0; i < KBD_MAX_PENDING; i++)
        if (g_kbd_pending[i].active && g_kbd_pending[i].ram == g_ram)
            g_kbd_pending[i].active = 0;
    cpu->d[0] = 0;
}

static void keyboard_BeginIO(M68kCPUState *cpu)
{
    /* BeginIO (-42): A1 = IOStdReq */
    uint32_t io = cpu->a[1];
    if (!io || io + IOSTD_SIZE > GUEST_RAM_SIZE) {
        cpu->d[0] = (uint32_t)-1;
        return;
    }

    uint16_t cmd = kb_r16(g_ram, io + IO_COMMAND);

    switch (cmd) {
    case KBD_READEVENT: {
        /* Queue until the next rawkey transition.  We don't peek at the
         * driver's rawkey ring — that stream belongs to the IDCMP pump;
         * events are delivered to us by the pump's feed hook instead. */
        uint32_t data = kb_r32(g_ram, io + IO_DATA);
        uint32_t len  = kb_r32(g_ram, io + IO_LENGTH);
        for (int i = 0; i < KBD_MAX_PENDING; i++) {
            if (g_kbd_pending[i].active) continue;
            g_kbd_pending[i].active = 1;
            g_kbd_pending[i].io   = io;
            g_kbd_pending[i].ram  = g_ram;
            g_kbd_pending[i].task = Task_Current();
            g_kbd_pending[i].data = data;
            g_kbd_pending[i].len  = len;
            g_ram[io + IO_FLAGS] = (uint8_t)((g_ram[io + IO_FLAGS] |
                                              UIOF_QUEUED) & ~UIOF_QUICK);
            g_ram[io + IO_ERROR] = 0;
            cpu->d[0] = 0;
            return;
        }
        g_ram[io + IO_ERROR] = (uint8_t)IOERR_NOCMD;
        cpu->d[0] = (uint32_t)IOERR_NOCMD;
        break;
    }

    case KBD_READMATRIX: {
        /* io_Data <- 16-byte held-key matrix; we don't track the PS/2
         * held bitmap at Amiga matrix granularity, so report all-up. */
        uint32_t data = kb_r32(g_ram, io + IO_DATA);
        uint32_t len  = kb_r32(g_ram, io + IO_LENGTH);
        if (len > 16) len = 16;
        if (data + len > GUEST_RAM_SIZE) len = GUEST_RAM_SIZE - data;
        for (uint32_t i = 0; i < len; i++) g_ram[data + i] = 0;
        kb_w32(g_ram, io + IO_ACTUAL, len);
        g_ram[io + IO_ERROR] = 0;
        cpu->d[0] = 0;
        break;
    }

    case CMD_RESET:
        /* Drop this task's pending reads. */
        KbdDev_DropTaskRequests(Task_Current());
        g_ram[io + IO_ERROR] = 0;
        cpu->d[0] = 0;
        break;

    case KBD_ADDRESETHANDLER:
    case KBD_ADDRESETHANDLERDONE:
    case KBD_RESETHANDLER:
    case KBD_RESETHANDLERDONE:
    case KBD_REMRESETHANDLER:
    case KBD_REMRESETHANDLERDONE:
        /* Ctrl-Amiga-Amiga reset handlers — the driver forces reset
         * unconditionally (PS2Kbd_CheckResetChord), so these register
         * as successful no-ops. */
        g_ram[io + IO_ERROR] = 0;
        cpu->d[0] = 0;
        break;

    default:
        g_ram[io + IO_ERROR] = (uint8_t)IOERR_NOCMD;
        cpu->d[0] = (uint32_t)IOERR_NOCMD;
        break;
    }
}

static void keyboard_AbortIO(M68kCPUState *cpu)
{
    /* AbortIO (-48): A1 = IORequest */
    uint32_t io = cpu->a[1];
    for (int i = 0; i < KBD_MAX_PENDING; i++) {
        KbdPendingRead *r = &g_kbd_pending[i];
        if (r->active && r->io == io && r->ram == g_ram) {
            r->active = 0;
            g_ram[io + IO_ERROR] = (uint8_t)IOERR_ABORTED;
            cpu->d[0] = 0;
            return;
        }
    }
    cpu->d[0] = (uint32_t)-1;   /* not pending */
}

/* =========================================================================
 * Function table
 * ========================================================================= */

static void *keyboard_funcs[] = {
    keyboard_OpenDevice,   /* index 1  */
    keyboard_CloseDevice,  /* index 2  */
    keyboard_BeginIO,      /* index 3  */
    keyboard_AbortIO,      /* index 4  */
};

/* Device vectors -> keyboard_funcs[] indices (Open -6, Close -12,
 * BeginIO -42, AbortIO -48). */
static const UaosRomLvo keyboard_lvo_map[] = {
    {  -6, 1 },   /* Open     */
    { -12, 2 },   /* Close    */
    { -42, 3 },   /* BeginIO  */
    { -48, 4 },   /* AbortIO  */
};

/* =========================================================================
 * Registration function
 * ========================================================================= */

void UAOS_KEYBOARD_Register(void)
{
    UAOS_ROM_Register("keyboard.device", 40, 0x000000B0,
                      (uint16_t)(sizeof(keyboard_funcs) / sizeof(keyboard_funcs[0])),
                      keyboard_funcs);
    UAOS_ROM_BindLvoMap("keyboard.device", keyboard_lvo_map,
                        (uint16_t)(sizeof(keyboard_lvo_map) / sizeof(keyboard_lvo_map[0])));
}
