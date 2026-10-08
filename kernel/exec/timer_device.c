/*
 * timer_device.c — UAOS timer.device Implementation
 *
 * AmigaOS timer.device provides timing functions including
 * system time, delays, and interval timers. This is a native
 * implementation for UAOS using the NTP RTC driver.
 */

#include "rom_modules.h"
#include "task.h"
#include "chipset/chip_emu.h"
#include "chipset/floppy.h"
#include "audio/audio.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

/* =========================================================================
 * NTP/RTC interface
 * ========================================================================= */
extern uint32_t ntp_get_epoch(void);

/* =========================================================================
 * AmigaOS-compatible Device I/O Structures
 * ========================================================================= */

/* timeval - matches AmigaOS structure */
typedef struct timeval {
    uint32_t tv_sec;   /* Seconds */
    uint32_t tv_usec;  /* Microseconds */
} timeval_t;

/* EClockVal structure for ReadEClock */
typedef struct EClockVal {
    uint32_t ev_hi;    /* High 32 bits */
    uint32_t ev_lo;    /* Low 32 bits */
} EClockVal_t;

/* Message structure (minimal) */
typedef struct Message {
    struct Message *mn_Next;
    uint32_t        mn_ReplyPort;  /* MsgPort pointer */
    uint16_t        mn_Length;
    uint8_t         mn_Data[0];
} Message_t;

/* IORequest structure (AmigaOS compatible) */
typedef struct IORequest {
    Message_t       io_Message;
    uint32_t        io_Device;      /* Device pointer */
    uint32_t        io_Unit;        /* Unit pointer */
    uint16_t        io_Command;
    uint8_t         io_Flags;
    int8_t          io_Error;
} IORequest_t;

/* TimeRequest structure for timer.device */
typedef struct TimeRequest {
    IORequest_t     tr_node;
    timeval_t       tr_time;
} TimeRequest_t;

/* Timer request queue entry (host-side tracking) */
typedef struct TimerQueueEntry {
    struct TimerQueueEntry *next;
    uint32_t               request_addr;  /* Guest address of TimeRequest */
    uint32_t               target_ticks;  /* Target tick count */
    uint32_t               sigmask;       /* Signal mask to send */
    UaosTask              *task;          /* Task to signal */
    uint8_t                active;        /* Request is active */
} TimerQueueEntry_t;

/* =========================================================================
 * Timer Device Commands (AmigaOS compatible)
 * ========================================================================= */

#define TR_ADDREQUEST   0x0001  /* Add a timer request */
#define TR_SETSYSTIME   0x0002  /* Set system time */
#define TR_GETSYSTIME   0x0003  /* Get system time */

/* =========================================================================
 * Global State
 * ========================================================================= */

static uint64_t g_eclock_value = 0;  /* E-clock counter in microseconds */
static uint32_t g_tick_counter = 0;  /* Global tick counter (~100Hz) */

/* Timer request queue (simple linked list) */
#define MAX_TIMER_ENTRIES 32
static TimerQueueEntry_t g_timer_entries[MAX_TIMER_ENTRIES];
static TimerQueueEntry_t *g_timer_queue_head = NULL;

/* =========================================================================
 * Memory access helper
 * ========================================================================= */
extern uint8_t *g_ram;
#define M68K_TO_HOST(addr) ((void *)(g_ram + (addr)))

/* Guest memory is big-endian — read/write 32-bit words byte-wise. */
static uint32_t timer_r32(uint32_t addr)
{
    return ((uint32_t)g_ram[addr + 0] << 24)
         | ((uint32_t)g_ram[addr + 1] << 16)
         | ((uint32_t)g_ram[addr + 2] <<  8)
         | ((uint32_t)g_ram[addr + 3]      );
}

static void timer_w32(uint32_t addr, uint32_t val)
{
    g_ram[addr + 0] = (uint8_t)(val >> 24);
    g_ram[addr + 1] = (uint8_t)(val >> 16);
    g_ram[addr + 2] = (uint8_t)(val >>  8);
    g_ram[addr + 3] = (uint8_t)(val      );
}

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

/* Find queue entry by guest request address */
static TimerQueueEntry_t *timer_find_by_request(uint32_t request_addr)
{
    TimerQueueEntry_t *current = g_timer_queue_head;
    while (current) {
        if (current->request_addr == request_addr) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

/* =========================================================================
 * Timer Tick Processing — called from timer ISR periodically
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
            /* Timer expired - signal the task and remove from queue */
            if (current->task) {
                Signal(current->task, current->sigmask);
            }

            /* Update IORequest io_Error to 0 (success) */
            IORequest_t *ior = (IORequest_t *)M68K_TO_HOST(current->request_addr);
            ior->io_Error = 0;

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

    emit(ctx, " req#  target-tick  delta  sigmask    task             req-addr");
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
        dl_hex(&l, e->sigmask); dl_pad(&l, 37);
        dl_add(&l, (e->task && e->task->ln_Name) ? e->task->ln_Name : "-");
        if (e->task && e->task->tc_State == TASK_REMOVED)
            dl_add(&l, " *dead*");
        dl_pad(&l, 54);
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
    /* OpenDevice - open timer device
     * A0 = IORequest pointer, D0 = unit number, D1 = flags
     * Returns: D0 = 0 for success, non-zero for error */
    (void)cpu;
    cpu->d[0] = 0;  /* Success */
}

static void timer_CloseDevice(M68kCPUState *cpu)
{
    /* CloseDevice - close timer device
     * A1 = IORequest pointer */
    (void)cpu;
}

static void timer_BeginIO(M68kCPUState *cpu)
{
    /* BeginIO - start I/O operation
     * A1 = IORequest pointer
     *
     * For timer.device, this queues a TR_ADDREQUEST for the specified time.
     * When the time expires, the requesting task is signaled.
     */
    uint32_t ior_addr = cpu->a[1];
    if (!ior_addr) {
        cpu->d[0] = (uint32_t)-1;
        return;
    }

    IORequest_t *ior = (IORequest_t *)M68K_TO_HOST(ior_addr);

    switch (ior->io_Command) {
        case TR_ADDREQUEST: {
            /* Read the timeval from the TimeRequest */
            TimeRequest_t *tr = (TimeRequest_t *)ior;
            uint32_t seconds = tr->tr_time.tv_sec;
            uint32_t micros = tr->tr_time.tv_usec;

            /* Convert to ticks (~100Hz = 10ms per tick, 100 ticks = 1 second) */
            uint32_t ticks = (seconds * 100) + (micros / 10000);
            if (ticks == 0) ticks = 1;  /* Minimum 1 tick */

            /* Allocate a timer queue entry */
            TimerQueueEntry_t *entry = timer_alloc_entry();
            if (!entry) {
                ior->io_Error = -1;  /* No free entries */
                cpu->d[0] = (uint32_t)-1;
                return;
            }

            /* Set up the timer request */
            entry->request_addr = ior_addr;
            entry->target_ticks = g_tick_counter + ticks;
            entry->task = Task_Current();

            /* Use signal bit from ReplyPort or default to SIGB_SINGLE */
            if (ior->io_Message.mn_ReplyPort) {
                /* For now, use a default signal - in full implementation,
                 * we'd read the MsgPort's signal bit */
                entry->sigmask = SIGF_SINGLE;
            } else {
                entry->sigmask = SIGF_SINGLE;
            }

            /* Queue the request */
            timer_enqueue(entry);

            /* Set io_Flags to indicate request is pending */
            ior->io_Flags |= 0x01;  /* IOF_QUEUED */
            ior->io_Error = 0;

            cpu->d[0] = 0;  /* Success - queued */
            break;
        }

        case TR_SETSYSTIME:
            /* Set system time - not implemented yet */
            ior->io_Error = -1;
            cpu->d[0] = (uint32_t)-1;
            break;

        case TR_GETSYSTIME:
            /* Get system time via IORequest - use same logic as GetSysTime */
            {
                uint32_t time_ptr = (uint32_t)(uintptr_t)&((TimeRequest_t *)ior)->tr_time;
                uint32_t epoch = ntp_get_epoch();
                timeval_t *tv = (timeval_t *)M68K_TO_HOST(time_ptr);
                tv->tv_sec = epoch;
                tv->tv_usec = 0;
                ior->io_Error = 0;
                cpu->d[0] = 0;
            }
            break;

        default:
            /* Unknown command */
            ior->io_Error = -1;
            cpu->d[0] = (uint32_t)-1;
            break;
    }
}

static void timer_AbortIO(M68kCPUState *cpu)
{
    /* AbortIO - abort I/O operation
     * A1 = IORequest pointer
     *
     * Attempts to remove a pending TR_ADDREQUEST from the timer queue.
     * Returns 0 if successful, non-zero if request already completed.
     */
    uint32_t ior_addr = cpu->a[1];
    if (!ior_addr) {
        cpu->d[0] = (uint32_t)-1;
        return;
    }

    IORequest_t *ior = (IORequest_t *)M68K_TO_HOST(ior_addr);

    /* Find the timer queue entry for this request */
    TimerQueueEntry_t *entry = timer_find_by_request(ior_addr);
    if (entry) {
        /* Found - remove from queue and mark as aborted */
        timer_dequeue(entry);
        timer_free_entry(entry);

        /* Mark IORequest as aborted */
        ior->io_Flags &= ~0x01;  /* Clear IOF_QUEUED */
        ior->io_Error = -2;      /* IOERR_ABORTED */

        cpu->d[0] = 0;  /* Success - aborted */
    } else {
        /* Request not found (already completed or never queued) */
        cpu->d[0] = (uint32_t)-1;  /* Already done */
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

    timer_w32(eclock_ptr,     (uint32_t)(g_eclock_value >> 32));
    timer_w32(eclock_ptr + 4, (uint32_t)g_eclock_value);

    /* Return E-clock frequency (~709379 ticks per second on PAL Amiga) */
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
