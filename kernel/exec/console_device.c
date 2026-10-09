/*
 * console_device.c — UAOS console.device Implementation
 *
 * AmigaOS console.device provides console I/O operations for
 * character-based input/output (UAOS-240).
 *
 * CMD_WRITE bytes are emitted through the task's g_print hook (the shell
 * window printer each M68k task installs at launch), so a guest that
 * opens console.device for a CON:-style stream sees output where its
 * DOS Write() output would go.
 *
 * CMD_READ queues pending when no cooked input is buffered; the event
 * pump feeds typed characters via ConDev_FeedChar() and the request is
 * replied through the normal IORequest completion path
 * (UAOS_Emu_IOReply — port PutMsg + task signal).
 *
 * Guest structures are big-endian guest RAM — accessed via the cd_r and
 * cd_w helpers only.
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
#define IO_MN_REPLYPORT  14
#define IO_COMMAND       28
#define IO_FLAGS         30
#define IO_ERROR         31
#define IO_ACTUAL        32    /* IOStdReq.io_Actual */
#define IO_LENGTH        36    /* IOStdReq.io_Length */
#define IO_DATA          40    /* IOStdReq.io_Data   */
#define IOSTD_SIZE       48

/* =========================================================================
 * console.device commands (exec/io.h + devices/console.h)
 * ========================================================================= */

#define CMD_READ              2
#define CMD_WRITE             3
#define CMD_FLUSH             8
#define CMD_NONSTD            9
#define CD_ASKKEYMAP         (CMD_NONSTD + 0)   /* 9  */
#define CD_SETKEYMAP         (CMD_NONSTD + 1)   /* 10 */
#define CD_ASKDEFAULTKEYMAP  (CMD_NONSTD + 2)   /* 11 */
#define CD_SETDEFAULTKEYMAP  (CMD_NONSTD + 3)   /* 12 */

#define IOERR_ABORTED  (-2)
#define IOERR_NOCMD    (-3)

/* =========================================================================
 * Guest memory helpers — big-endian
 * ========================================================================= */

extern uint8_t *g_ram;

static uint16_t cd_r16(uint8_t *ram, uint32_t a)
{
    return (uint16_t)(((uint16_t)ram[a] << 8) | ram[a + 1]);
}

static uint32_t cd_r32(uint8_t *ram, uint32_t a)
{
    return ((uint32_t)ram[a] << 24) | ((uint32_t)ram[a + 1] << 16) |
           ((uint32_t)ram[a + 2] <<  8) |  (uint32_t)ram[a + 3];
}

static void cd_w32(uint8_t *ram, uint32_t a, uint32_t v)
{
    ram[a] = (uint8_t)(v >> 24); ram[a + 1] = (uint8_t)(v >> 16);
    ram[a + 2] = (uint8_t)(v >> 8); ram[a + 3] = (uint8_t)v;
}

/* =========================================================================
 * Cooked input buffer + pending reads
 *
 * The event pump feeds every typed cooked char here (it still goes to the
 * focused window's IDCMP too — console.device read and VANILLAKEY are
 * separate consumers on real AmigaOS).  Chars buffer even with no pending
 * read, like a real console line discipline.
 * ========================================================================= */

#define CON_INBUF_SIZE 64
static uint8_t  g_con_inbuf[CON_INBUF_SIZE];
static uint16_t g_con_in_head, g_con_in_tail;

typedef struct {
    uint8_t   active;
    uint32_t  io;      /* guest IOStdReq address      */
    uint8_t  *ram;     /* requester's guest window    */
    UaosTask *task;    /* owning task (exit purge)    */
    uint32_t  data;    /* io_Data buffer              */
    uint32_t  len;     /* io_Length                   */
    uint32_t  used;    /* bytes filled so far         */
} ConPendingRead;

#define CON_MAX_PENDING 8
static ConPendingRead g_con_pending[CON_MAX_PENDING];
static uint32_t g_con_open_cnt;

static void con_in_push(uint8_t c)
{
    uint16_t next = (uint16_t)(g_con_in_tail + 1) % CON_INBUF_SIZE;
    if (next == g_con_in_head) return;   /* full — drop oldest? just drop */
    g_con_inbuf[g_con_in_tail] = c;
    g_con_in_tail = next;
}

static int con_in_pop(void)
{
    if (g_con_in_head == g_con_in_tail) return -1;
    int c = g_con_inbuf[g_con_in_head];
    g_con_in_head = (uint16_t)(g_con_in_head + 1) % CON_INBUF_SIZE;
    return c;
}

/* Complete a pending read that has at least one byte. */
static void con_finish_read(ConPendingRead *r)
{
    if (!r->used) return;
    cd_w32(r->ram, r->io + IO_ACTUAL, r->used);
    r->ram[r->io + IO_ERROR] = 0;
    UAOS_Emu_IOReply(r->ram, r->io);
    r->active = 0;
}

/* Event-pump feed: cooked char destined for the focused console.
 * Buffers for later reads and completes any request a char satisfies. */
void ConDev_FeedChar(char c)
{
    con_in_push((uint8_t)c);
    for (int i = 0; i < CON_MAX_PENDING; i++) {
        ConPendingRead *r = &g_con_pending[i];
        if (!r->active) continue;
        while (r->used < r->len && r->data + r->used < GUEST_RAM_SIZE) {
            int ch = con_in_pop();
            if (ch < 0) break;
            r->ram[r->data + r->used] = (uint8_t)ch;
            r->used++;
        }
        if (r->used) con_finish_read(r);
    }
}

/* Task-exit purge (UAOS-240): never complete into a recycled window. */
void ConDev_DropTaskRequests(UaosTask *t)
{
    if (!t) return;
    for (int i = 0; i < CON_MAX_PENDING; i++)
        if (g_con_pending[i].active && g_con_pending[i].task == t)
            g_con_pending[i].active = 0;
}

/* =========================================================================
 * Device vectors (M68kCPUState marshalled by the glue's emu_rom_call)
 * ========================================================================= */

static void console_OpenDevice(M68kCPUState *cpu)
{
    /* Open (-6): A1 = IORequest, D0 = unit, D1 = flags.
     * io_Data may carry a Window* for RAW:/CON: binding — we ignore it;
     * output goes to the task's own console printer. */
    g_con_open_cnt++;
    g_ram[cpu->a[1] + IO_ERROR] = 0;
    cpu->d[0] = 0;
}

static void console_CloseDevice(M68kCPUState *cpu)
{
    /* Close (-12): A1 = IORequest */
    if (g_con_open_cnt) g_con_open_cnt--;
    /* Drop pending reads belonging to this IORequest's owner window. */
    for (int i = 0; i < CON_MAX_PENDING; i++)
        if (g_con_pending[i].active && g_con_pending[i].ram == g_ram)
            g_con_pending[i].active = 0;
    cpu->d[0] = 0;
}

static void console_BeginIO(M68kCPUState *cpu)
{
    /* BeginIO (-42): A1 = IOStdReq */
    uint32_t io = cpu->a[1];
    if (!io || io + IOSTD_SIZE > GUEST_RAM_SIZE) {
        cpu->d[0] = (uint32_t)-1;
        return;
    }

    uint16_t cmd = cd_r16(g_ram, io + IO_COMMAND);
    uint32_t data = cd_r32(g_ram, io + IO_DATA);
    uint32_t len  = cd_r32(g_ram, io + IO_LENGTH);

    switch (cmd) {
    case CMD_WRITE: {
        /* Emit the buffer through the task's console printer in
         * NUL-terminated chunks (g_print takes a C string). */
        if (data + len > GUEST_RAM_SIZE) len = GUEST_RAM_SIZE - data;
        char chunk[129];
        uint32_t off = 0;
        while (off < len) {
            uint32_t n = len - off;
            if (n > sizeof(chunk) - 1) n = sizeof(chunk) - 1;
            for (uint32_t i = 0; i < n; i++)
                chunk[i] = (char)g_ram[data + off + i];
            chunk[n] = '\0';
            if (g_print) g_print(chunk);
            off += n;
        }
        cd_w32(g_ram, io + IO_ACTUAL, len);
        g_ram[io + IO_ERROR] = 0;
        cpu->d[0] = 0;
        break;
    }

    case CMD_READ: {
        /* Buffered cooked input completes inline; otherwise the request
         * queues until the event pump feeds a typed char. */
        uint32_t n = 0;
        if (data + len > GUEST_RAM_SIZE) len = GUEST_RAM_SIZE - data;
        while (n < len) {
            int ch = con_in_pop();
            if (ch < 0) break;
            g_ram[data + n] = (uint8_t)ch;
            n++;
        }
        if (n) {
            cd_w32(g_ram, io + IO_ACTUAL, n);
            g_ram[io + IO_ERROR] = 0;
            cpu->d[0] = 0;
            break;
        }
        for (int i = 0; i < CON_MAX_PENDING; i++) {
            if (g_con_pending[i].active) continue;
            g_con_pending[i].active = 1;
            g_con_pending[i].io   = io;
            g_con_pending[i].ram  = g_ram;
            g_con_pending[i].task = Task_Current();
            g_con_pending[i].data = data;
            g_con_pending[i].len  = len;
            g_con_pending[i].used = 0;
            g_ram[io + IO_FLAGS] = (uint8_t)((g_ram[io + IO_FLAGS] |
                                              UIOF_QUEUED) & ~UIOF_QUICK);
            g_ram[io + IO_ERROR] = 0;
            cpu->d[0] = 0;
            return;
        }
        /* No free pending slot — fail rather than silently losing it. */
        g_ram[io + IO_ERROR] = (uint8_t)IOERR_NOCMD;
        cpu->d[0] = (uint32_t)IOERR_NOCMD;
        break;
    }

    case CMD_FLUSH:
        g_con_in_head = g_con_in_tail = 0;
        g_ram[io + IO_ERROR] = 0;
        cpu->d[0] = 0;
        break;

    default:
        /* Keymap queries and anything else we don't model. */
        g_ram[io + IO_ERROR] = (uint8_t)IOERR_NOCMD;
        cpu->d[0] = (uint32_t)IOERR_NOCMD;
        break;
    }
}

static void console_AbortIO(M68kCPUState *cpu)
{
    /* AbortIO (-48): A1 = IORequest */
    uint32_t io = cpu->a[1];
    for (int i = 0; i < CON_MAX_PENDING; i++) {
        ConPendingRead *r = &g_con_pending[i];
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

static void *console_funcs[] = {
    console_OpenDevice,   /* index 1  */
    console_CloseDevice,  /* index 2  */
    console_BeginIO,      /* index 3  */
    console_AbortIO,      /* index 4  */
};

/* Device vectors -> console_funcs[] indices (Open -6, Close -12,
 * BeginIO -42, AbortIO -48).  All command I/O flows through BeginIO. */
static const UaosRomLvo console_lvo_map[] = {
    {  -6, 1 },   /* Open     */
    { -12, 2 },   /* Close    */
    { -42, 3 },   /* BeginIO  */
    { -48, 4 },   /* AbortIO  */
};

/* =========================================================================
 * Registration function
 * ========================================================================= */

void UAOS_CONSOLE_Register(void)
{
    UAOS_ROM_Register("console.device", 40, 0x00000060,
                      (uint16_t)(sizeof(console_funcs) / sizeof(console_funcs[0])),
                      console_funcs);
    UAOS_ROM_BindLvoMap("console.device", console_lvo_map,
                        (uint16_t)(sizeof(console_lvo_map) / sizeof(console_lvo_map[0])));
}
