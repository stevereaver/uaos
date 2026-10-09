/*
 * timer_device.c — UAOS timer.device Implementation
 *
 * AmigaOS timer.device provides timing functions including
 * system time, delays, and interval timers. This is a native
 * implementation for UAOS using the NTP RTC driver and the PIT tick
 * (~100 Hz) as the timing source for all units.
 *
 * Guest-visible structures live in the requester's guest RAM window and
 * are big-endian: every field is accessed through the tr_r / tr_w helpers
 * below, never via native structs (UAOS-240 — the original code aliased
 * native structs onto guest memory, which is both endian- and
 * layout-wrong: io_Command is at +28, not +20, and guests are BE).
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include "rom_modules.h"
#include "task.h"
#include "chipset/chip_emu.h"
#include "chipset/floppy.h"
#include "audio/audio.h"
#include "../../emulation/uaos_emu.h"

/* =========================================================================
 * NTP/RTC interface
 * ========================================================================= */
extern uint32_t ntp_get_epoch(void);

/* =========================================================================
 * AmigaOS-compatible guest structure offsets (big-endian guest RAM)
 * ========================================================================= */

/* struct IORequest (32 bytes) */
#define IO_MN_REPLYPORT  14
#define IO_MN_LENGTH     18
#define IO_DEVICE        20
#define IO_UNIT          24
#define IO_COMMAND       28
#define IO_FLAGS         30
#define IO_ERROR         31
#define IOSTD_SIZE       48

/* struct TimeRequest: IORequest tr_node + timeval tr_time */
#define TR_SECS          32
#define TR_MICRO         36

/* =========================================================================
 * Timer Device Commands (AmigaOS compatible — devices/timer.h)
 * ========================================================================= */

#define CMD_INVALID      0x0000
#define CMD_RESET        0x0001
#define CMD_READ         0x0002
#define CMD_WRITE        0x0003
#define CMD_FLUSH        0x0008
#define CMD_NONSTD       0x0009

#define TR_ADDREQUEST    (CMD_NONSTD + 0)   /* 9  — queue a delay         */
#define TR_GETSYSTIME    (CMD_NONSTD + 1)   /* 10 — wall clock            */
#define TR_SETSYSTIME    (CMD_NONSTD + 2)   /* 11 — set wall clock        */

/* exec/io.h result codes */
#define IOERR_ABORTED    (-2)
#define IOERR_NOCMD      (-3)

/* =========================================================================
 * Global State
 * ========================================================================= */

static uint64_t g_eclock_value = 0;  /* E-clock counter in microseconds */
static uint32_t g_tick_counter = 0;  /* Global tick counter (~100Hz) */

/* Timer request queue entry (host-side tracking).
 * `ram` is the *requester's* guest window — timer expiry runs in PIT-ISR
 * context where g_ram may be bound to a different M68k task, so the
 * window is captured at BeginIO time. */
typedef struct TimerQueueEntry {
    struct TimerQueueEntry *next;
    uint32_t               request_addr;  /* Guest address of TimeRequest */
    uint8_t               *ram;           /* Requester's guest RAM window */
    uint32_t               target_ticks;  /* Target tick count */
    UaosTask              *task;          /* Task that issued the request */
    uint8_t                active;        /* Request is active */
} TimerQueueEntry_t;

/* Timer request queue (simple linked list) */
#define MAX_TIMER_ENTRIES 32
static TimerQueueEntry_t g_timer_entries[MAX_TIMER_ENTRIES];
static TimerQueueEntry_t *g_timer_queue_head = NULL;

/* =========================================================================
 * Guest memory access helpers — big-endian, any task's window
 * ========================================================================= */

static uint8_t tr_r8(uint8_t *ram, uint32_t a) { return ram[a]; }
static void    tr_w8(uint8_t *ram, uint32_t a, uint8_t v) { ram[a] = v; }

static uint16_t tr_r16(uint8_t *ram, uint32_t a)
{
    return (uint16_t)(((uint16_t)ram[a] << 8) | ram[a + 1]);
}

static uint32_t tr_r32(uint8_t *ram, uint32_t a)
{
    return ((uint32_t)ram[a + 0] << 24)
         | ((uint32_t)ram[a + 1] << 16)
         | ((uint32_t)ram[a + 2] <<  8)
         | ((uint32_t)ram[a + 3]      );
}

static void tr_w32(uint8_t *ram, uint32_t a, uint32_t v)
{
    ram[a + 0] = (uint8_t)(v >> 24);
    ram[a + 1] = (uint8_t)(v >> 16);
    ram[a + 2] = (uint8_t)(v >>  8);
    ram[a + 3] = (uint8_t)(v      );
}

/* Current-context convenience wrappers (BeginIO/Open run inside the
 * issuing task's emulation slice, so g_ram is the requester's window). */
extern uint8_t *g_ram;
static uint32_t timer_r32(uint32_t a)            { return tr_r32(g_ram, a); }
static void     timer_w32(uint32_t a, uint32_t v){ tr_w32(g_ram, a, v); }

/* =========================================================================
 * Timer Queue Management
 * ========================================================================= */

static TimerQueueEntry_t *timer_alloc_entry(void)
{
    for (int i = 0; i < MAX_TIMER_ENTRIES; i++) {
        if (!g_timer_entries[i].active) {
            g_timer_entries[i].active = 1;
            g_timer_entries[i].next = NULL;
            return &g_timer_entries[i];
        }
    }
    return NULL;
}

static void timer_free_entry(TimerQueueEntry_t *entry)
{
    if (entry) {
        entry->active = 0;
        entry->next = NULL;
    }
}

static void timer_enqueue(TimerQueueEntry_t *entry)
{
    if (!g_timer_queue_head) {
        g_timer_queue_head = entry;
    } else {
        TimerQueueEntry_t *current = g_timer_queue_head;
        while (current->next) {
            current = current->next;
        }
        current->next = entry;
    }
}

static int timer_dequeue(TimerQueueEntry_t *entry)
{
    if (!g_timer_queue_head || !entry) return 0;

    if (g_timer_queue_head == entry) {
        g_timer_queue_head = entry->next;
        entry->next = NULL;
        return 1;
    }

    TimerQueueEntry_t *current = g_timer_queue_head;
    while (current->next) {
        if (current->next == entry) {
            current->next = entry->next;
            entry->next = NULL;
            return 1;
        }
        current = current->next;
    }
    return 0;
}

/* Find queue entry by guest request — the same numeric address can exist
 * in two tasks' windows, so match the window pointer too. */
static TimerQueueEntry_t *timer_find_by_request(uint32_t request_addr,
                                                uint8_t *ram)
{
    TimerQueueEntry_t *current = g_timer_queue_head;
    while (current) {
        if (current->request_addr == request_addr && current->ram == ram)
            return current;
        current = current->next;
    }
    return NULL;
}

/* =========================================================================
 * Task-exit purge (UAOS-240): a dead guest's RAM window is recycled, so
 * its queued timer requests must never complete into it.
 * ========================================================================= */
void IODev_DropTaskRequests(UaosTask *t)
{
    if (!t) return;
    TimerQueueEntry_t *cur = g_timer_queue_head;
    while (cur) {
        TimerQueueEntry_t *next = cur->next;
        if (cur->task == t) {
            timer_dequeue(cur);
            timer_free_entry(cur);
        }
        cur = next;
    }
}

/* =========================================================================
 * Timer Tick Processing — called from the PIT ISR periodically
 * ========================================================================= */

void timer_ProcessTicks(void)
{
    g_tick_counter++;

    /* Keep PIT only for host audio/video timing.  Beam and subsystems are
     * advanced from the M68k cycle counter via chip_emu_beam_tick(); audio
     * DMA is advanced by the scheduler and mixed by the 48 kHz audio path. */
    chip_emu_beam_tick(g_tick_counter);
    chip_emu_cia_tick();
    floppy_tick();
    audio_tick();
    chip_emu_poll_ps2_keyboard();
    /* USB interrupt-transfer completion scan — plain memory reads on the
     * TD chain, safe at 100 Hz and complements the INTx IRQ path. */
    {
        extern void USB_Poll(void);
        USB_Poll();
    }
    chip_emu_serial_poll();

    /* Generate a PAL-equivalent VBlank interrupt every 2 ticks (~50 Hz). */
    if ((g_tick_counter & 1u) == 0) {
        chip_emu_vblank();
        /* Wake any M68k task blocked inside graphics.library/WaitTOF(). */
        extern UaosTask *g_wait_tof_task;
        if (g_wait_tof_task && g_wait_tof_task->m68k_vblank_sig >= 0) {
            Signal(g_wait_tof_task,
                   (1u << (unsigned int)g_wait_tof_task->m68k_vblank_sig));
        }
    }

    TimerQueueEntry_t *current = g_timer_queue_head;
    TimerQueueEntry_t *prev = NULL;

    while (current) {
        TimerQueueEntry_t *next = current->next;

        if (g_tick_counter >= current->target_ticks) {
            /* Timer expired — complete the request in the *requester's*
             * guest window and deliver the reply-port message + signal
             * (UAOS-240).  io_Error is left as BeginIO wrote it (0). */
            uint8_t *ram = current->ram;
            uint32_t io  = current->request_addr;
            if (ram)
                tr_w8(ram, io + IO_ERROR, 0);
            UAOS_Emu_IOReply(ram, io);

            /* Remove from queue */
            if (prev) {
                prev->next = next;
            } else {
                g_timer_queue_head = next;
            }

            timer_free_entry(current);
        } else {
            prev = current;
        }

        current = next;
    }
}

/* =========================================================================
 * C:timers — pending timer-request dump (UAOS-201)
 * ========================================================================= */
#include "../dbg/diag.h"

void TimerDevice_DiagDump(void *ctx, void (*emit)(void *, const char *))
{
    DiagLine l;
    int n = 0;

    dl_reset(&l);
    dl_add(&l, " tick_counter="); dl_dec(&l, g_tick_counter);
    dl_add(&l, "  (PIT rate ~100 Hz)");
    dl_emit(&l, ctx, emit);

    emit(ctx, " req#  target-tick  delta  task             req-addr");
    for (int i = 0; i < MAX_TIMER_ENTRIES; i++) {
        TimerQueueEntry_t *e = &g_timer_entries[i];
        if (!e->active) continue;
        n++;
        dl_reset(&l);
        dl_ch(&l, ' ');
        dl_dec(&l, (uint64_t)i); dl_pad(&l, 6);
        dl_dec(&l, e->target_ticks); dl_pad(&l, 19);
        int32_t delta = (int32_t)e->target_ticks - (int32_t)g_tick_counter;
        if (delta < 0) { dl_add(&l, "OVERDUE by "); dl_dec(&l, (uint64_t)(-delta)); }
        else           { dl_dec(&l, (uint64_t)delta); }
        dl_pad(&l, 26);
        dl_add(&l, (e->task && e->task->ln_Name) ? e->task->ln_Name : "-");
        if (e->task && e->task->tc_State == TASK_REMOVED)
            dl_add(&l, " *dead*");
        dl_pad(&l, 45);
        dl_hex(&l, e->request_addr);
        dl_emit(&l, ctx, emit);
    }

    dl_reset(&l);
    dl_add(&l, " "); dl_dec(&l, (uint64_t)n);
    dl_add(&l, " pending timer request(s)");
    dl_emit(&l, ctx, emit);
}

#define TIMER_OPEN_DEVICE   1
#define TIMER_CLOSE_DEVICE  2
#define TIMER_BEGIN_IO      3
#define TIMER_ABORT_IO      4
#define TIMER_GET_SYSTIME   5
#define TIMER_ECLOCK_UPDATE 6
#define TIMER_READ_ECLOCK   7
#define TIMER_ADD_TIME      8
#define TIMER_SUB_TIME      9
#define TIMER_CMP_TIME      10

/* =========================================================================
 * timer.device function implementations
 * ========================================================================= */

static void timer_OpenDevice(M68kCPUState *cpu)
{
    /* Device Open vector (-6): A1 = IORequest, D0 = unit, D1 = flags.
     * Every unit (MICROHZ/VBLANK/ECLOCK…) shares the PIT tick. */
    (void)cpu;
    cpu->d[0] = 0;  /* Success */
}

static void timer_CloseDevice(M68kCPUState *cpu)
{
    /* Device Close vector (-12): A1 = IORequest */
    (void)cpu;
}

static void timer_BeginIO(M68kCPUState *cpu)
{
    /* BeginIO vector (-42): A1 = IORequest.
     *
     * TR_ADDREQUEST queues the delay and leaves IOF_QUEUED set — the PIT
     * tick path replies the request when it expires.  TR_GETSYSTIME and
     * the rest complete synchronously; the glue turns them into a
     * reply-port message for non-QUICK callers. */
    uint32_t io = cpu->a[1];
    if (!io || io + IOSTD_SIZE >= GUEST_RAM_SIZE) {
        cpu->d[0] = (uint32_t)-1;
        return;
    }

    uint16_t cmd = tr_r16(g_ram, io + IO_COMMAND);

    switch (cmd) {
        case TR_ADDREQUEST: {
            /* timeval at tr_node+32 */
            uint32_t seconds = timer_r32(io + TR_SECS);
            uint32_t micros  = timer_r32(io + TR_MICRO);

            /* ~100 Hz tick: 100 ticks/s, 10 ms each */
            uint32_t ticks = (seconds * 100) + (micros / 10000);
            if (ticks == 0) ticks = 1;  /* Minimum 1 tick */

            TimerQueueEntry_t *entry = timer_alloc_entry();
            if (!entry) {
                tr_w8(g_ram, io + IO_ERROR, (uint8_t)-1);  /* table full */
                cpu->d[0] = (uint32_t)-1;
                return;
            }

            entry->request_addr = io;
            entry->ram          = g_ram;
            entry->target_ticks = g_tick_counter + ticks;
            entry->task         = Task_Current();
            timer_enqueue(entry);

            /* In-flight: device holds it until expiry.  Clearing QUICK is
             * the Amiga contract for "this could not complete inline". */
            tr_w8(g_ram, io + IO_FLAGS,
                  (uint8_t)((tr_r8(g_ram, io + IO_FLAGS) | UIOF_QUEUED)
                            & ~UIOF_QUICK));
            g_ram[io + 8] = 5;             /* ln_Type = NT_MESSAGE */
            tr_w8(g_ram, io + IO_ERROR, 0);

            cpu->d[0] = 0;  /* Accepted */
            break;
        }

        case TR_GETSYSTIME: {
            uint32_t epoch = ntp_get_epoch();
            timer_w32(io + TR_SECS,  epoch);
            timer_w32(io + TR_MICRO, 0);  /* only second precision from RTC */
            tr_w8(g_ram, io + IO_ERROR, 0);
            cpu->d[0] = 0;
            break;
        }

        case TR_SETSYSTIME:
            /* Read-only wall clock for now. */
            tr_w8(g_ram, io + IO_ERROR, (uint8_t)IOERR_NOCMD);
            cpu->d[0] = (uint32_t)IOERR_NOCMD;
            break;

        case CMD_RESET:
            /* Drop every pending request this task queued. */
            IODev_DropTaskRequests(Task_Current());
            tr_w8(g_ram, io + IO_ERROR, 0);
            cpu->d[0] = 0;
            break;

        default:
            tr_w8(g_ram, io + IO_ERROR, (uint8_t)IOERR_NOCMD);
            cpu->d[0] = (uint32_t)IOERR_NOCMD;
            break;
    }
}

static void timer_AbortIO(M68kCPUState *cpu)
{
    /* AbortIO vector (-48): A1 = IORequest.
     *
     * Removes a pending TR_ADDREQUEST from the queue; the glue completes
     * it with IOERR_ABORTED and the usual reply.  D0 = -1 when the
     * request wasn't ours (already done or never queued). */
    uint32_t io = cpu->a[1];
    if (!io || io + IOSTD_SIZE >= GUEST_RAM_SIZE) {
        cpu->d[0] = (uint32_t)-1;
        return;
    }

    TimerQueueEntry_t *entry = timer_find_by_request(io, g_ram);
    if (entry) {
        timer_dequeue(entry);
        timer_free_entry(entry);
        tr_w8(g_ram, io + IO_ERROR, (uint8_t)IOERR_ABORTED);
        cpu->d[0] = 0;
    } else {
        cpu->d[0] = (uint32_t)-1;  /* Already done / not queued */
    }
}

static void timer_GetSysTime(M68kCPUState *cpu)
{
    /* GetSysTime - get current system time
     * A0 = pointer to timeval structure to fill
     * Fills: tv_sec, tv_usec with current time */
    uint32_t time_ptr = cpu->a[0];
    if (!time_ptr) {
        cpu->d[0] = (uint32_t)-1;
        return;
    }

    uint32_t epoch = ntp_get_epoch();
    timer_w32(time_ptr,     epoch);
    timer_w32(time_ptr + 4, 0);  /* only second precision from ntp_get_epoch */

    cpu->d[0] = 0;  /* Success */
}

static void timer_ECLOCK_UPDATE(M68kCPUState *cpu)
{
    /* ECLOCK_UPDATE - update E-clock value
     * Called periodically by the system to advance the E-clock
     * Each call adds approximately 40 microseconds (typical Amiga E-clock period) */
    (void)cpu;
    g_eclock_value += 40;  /* ~40 microseconds per E-clock tick */
}

static void timer_ReadEClock(M68kCPUState *cpu)
{
    /* ReadEClock - read E-clock value
     * A0 = pointer to EClockVal structure to fill
     * Returns: D0 = E-clock frequency in ticks/second */
    uint32_t eclock_ptr = cpu->a[0];
    if (!eclock_ptr) {
        cpu->d[0] = 0;
        return;
    }

    /* Advance the free-running eclock by elapsed PIT ticks so it ticks
     * even when nobody calls ECLOCK_UPDATE (UAOS-240). */
    static uint32_t eclock_last_tick = 0;
    if (g_tick_counter != eclock_last_tick) {
        g_eclock_value += (uint64_t)(g_tick_counter - eclock_last_tick)
                          * 10000;               /* 10 ms per tick */
        eclock_last_tick = g_tick_counter;
    }

    timer_w32(eclock_ptr,     (uint32_t)(g_eclock_value >> 32));
    timer_w32(eclock_ptr + 4, (uint32_t)g_eclock_value);

    /* Return E-clock frequency (~709379 ticks per second on PAL Amiga;
     * we drive it in microseconds — callers only use the value for
     * elapsed-time scaling, so a rounded PAL rate keeps intervals sane) */
    cpu->d[0] = 709379;
}

static void timer_AddTime(M68kCPUState *cpu)
{
    /* AddTime - add two time values
     * A0 = destination timeval, A1 = source timeval to add
     * destination = destination + source */
    uint32_t dst_ptr = cpu->a[0];
    uint32_t src_ptr = cpu->a[1];

    if (!dst_ptr || !src_ptr) {
        cpu->d[0] = (uint32_t)-1;
        return;
    }

    uint32_t dsec  = timer_r32(dst_ptr);
    uint32_t dusec = timer_r32(dst_ptr + 4);
    uint32_t ssec  = timer_r32(src_ptr);
    uint32_t susec = timer_r32(src_ptr + 4);

    dsec  += ssec;
    dusec += susec;

    /* Normalize microseconds */
    if (dusec >= 1000000) {
        dsec  += 1;
        dusec -= 1000000;
    }

    timer_w32(dst_ptr,     dsec);
    timer_w32(dst_ptr + 4, dusec);

    cpu->d[0] = 0;  /* Success */
}

static void timer_SubTime(M68kCPUState *cpu)
{
    /* SubTime - subtract two time values
     * A0 = destination timeval, A1 = source timeval to subtract
     * destination = destination - source */
    uint32_t dst_ptr = cpu->a[0];
    uint32_t src_ptr = cpu->a[1];

    if (!dst_ptr || !src_ptr) {
        cpu->d[0] = (uint32_t)-1;
        return;
    }

    uint32_t dsec  = timer_r32(dst_ptr);
    uint32_t dusec = timer_r32(dst_ptr + 4);
    uint32_t ssec  = timer_r32(src_ptr);
    uint32_t susec = timer_r32(src_ptr + 4);

    /* Handle microseconds underflow */
    if (dusec < susec) {
        dsec  -= 1;
        dusec += 1000000;
    }

    dusec -= susec;
    dsec  -= ssec;

    timer_w32(dst_ptr,     dsec);
    timer_w32(dst_ptr + 4, dusec);

    cpu->d[0] = 0;  /* Success */
}

static void timer_CmpTime(M68kCPUState *cpu)
{
    /* CmpTime - compare two time values
     * A0 = first timeval, A1 = second timeval
     * Returns: D0 = -1 if t1 < t2, 0 if equal, 1 if t1 > t2 */
    uint32_t t1_ptr = cpu->a[0];
    uint32_t t2_ptr = cpu->a[1];

    if (!t1_ptr || !t2_ptr) {
        cpu->d[0] = 0;
        return;
    }

    uint32_t t1s = timer_r32(t1_ptr), t1u = timer_r32(t1_ptr + 4);
    uint32_t t2s = timer_r32(t2_ptr), t2u = timer_r32(t2_ptr + 4);

    if (t1s < t2s || (t1s == t2s && t1u < t2u))
        cpu->d[0] = (uint32_t)-1;
    else if (t1s > t2s || (t1s == t2s && t1u > t2u))
        cpu->d[0] = 1;
    else
        cpu->d[0] = 0;
}

/* =========================================================================
 * Function table
 * ========================================================================= */

static void *timer_funcs[] = {
    timer_OpenDevice,   /* index 1  */
    timer_CloseDevice,  /* index 2  */
    timer_BeginIO,      /* index 3  */
    timer_AbortIO,      /* index 4  */
    timer_GetSysTime,   /* index 5  */
    timer_ECLOCK_UPDATE, /* index 6  */
    timer_ReadEClock,   /* index 7  */
    timer_AddTime,      /* index 8  */
    timer_SubTime,      /* index 9  */
    timer_CmpTime,      /* index 10 */
};

/* timer.device LVOs -> timer_funcs[] indices.
 * -6/-12 are the device Open/Close vectors, -42/-48 BeginIO/AbortIO,
 * and the timer "library" calls live at -66..-90 (canonical timer.fd). */
static const UaosRomLvo timer_lvo_map[] = {
    {  -6, 1 },   /* Open        */
    { -12, 2 },   /* Close       */
    { -42, 3 },   /* BeginIO     */
    { -48, 4 },   /* AbortIO     */
    { -66, 8 },   /* AddTime     */
    { -72, 9 },   /* SubTime     */
    { -78, 10 },  /* CmpTime     */
    { -84, 7 },   /* ReadEClock  */
    { -90, 5 },   /* GetSysTime  */
};

/* =========================================================================
 * Registration function
 * ========================================================================= */

void UAOS_TIMER_Register(void)
{
    UAOS_ROM_Register("timer.device", 40, 0x000000A0,
                      (uint16_t)(sizeof(timer_funcs) / sizeof(timer_funcs[0])),
                      timer_funcs);
    UAOS_ROM_BindLvoMap("timer.device", timer_lvo_map,
                        (uint16_t)(sizeof(timer_lvo_map) / sizeof(timer_lvo_map[0])));
}
