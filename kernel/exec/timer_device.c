/*
 * timer_device.c — UAOS timer.device Implementation
 *
 * AmigaOS timer.device provides timing functions including
 * system time, delays, and interval timers. This is a native
 * implementation for UAOS using the NTP RTC driver.
 */

#include "rom_modules.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

/* =========================================================================
 * NTP/RTC interface
 * ========================================================================= */
extern uint32_t ntp_get_epoch(void);

/* =========================================================================
 * AmigaOS Time Structures (compatible with timeval)
 * ========================================================================= */

typedef struct timeval {
    uint32_t tv_sec;   /* Seconds */
    uint32_t tv_usec;  /* Microseconds */
} timeval_t;

/* EClockVal structure for ReadEClock */
typedef struct EClockVal {
    uint32_t ev_hi;    /* High 32 bits */
    uint32_t ev_lo;    /* Low 32 bits */
} EClockVal_t;

/* =========================================================================
 * Global State
 * ========================================================================= */

static uint64_t g_eclock_value = 0;  /* E-clock counter in microseconds */

/* =========================================================================
 * Memory access helper
 * ========================================================================= */
extern uint8_t *g_ram;
#define M68K_TO_HOST(addr) ((void *)(g_ram + (addr)))

/* =========================================================================
 * timer.device function indices (must match AmigaOS LVO offsets)
 * ========================================================================= */

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
     * A1 = IORequest pointer */
    (void)cpu;
}

static void timer_AbortIO(M68kCPUState *cpu)
{
    /* AbortIO - abort I/O operation
     * A1 = IORequest pointer */
    (void)cpu;
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
    timeval_t *tv = (timeval_t *)M68K_TO_HOST(time_ptr);

    tv->tv_sec = epoch;
    tv->tv_usec = 0;  /* We only have second precision from ntp_get_epoch */

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

    EClockVal_t *ev = (EClockVal_t *)M68K_TO_HOST(eclock_ptr);
    ev->ev_hi = (uint32_t)(g_eclock_value >> 32);
    ev->ev_lo = (uint32_t)g_eclock_value;

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

    timeval_t *dst = (timeval_t *)M68K_TO_HOST(dst_ptr);
    timeval_t *src = (timeval_t *)M68K_TO_HOST(src_ptr);

    dst->tv_sec += src->tv_sec;
    dst->tv_usec += src->tv_usec;

    /* Normalize microseconds */
    if (dst->tv_usec >= 1000000) {
        dst->tv_sec += 1;
        dst->tv_usec -= 1000000;
    }

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

    timeval_t *dst = (timeval_t *)M68K_TO_HOST(dst_ptr);
    timeval_t *src = (timeval_t *)M68K_TO_HOST(src_ptr);

    /* Handle microseconds underflow */
    if (dst->tv_usec < src->tv_usec) {
        dst->tv_sec -= 1;
        dst->tv_usec += 1000000;
    }

    dst->tv_usec -= src->tv_usec;
    dst->tv_sec -= src->tv_sec;

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

    timeval_t *t1 = (timeval_t *)M68K_TO_HOST(t1_ptr);
    timeval_t *t2 = (timeval_t *)M68K_TO_HOST(t2_ptr);

    if (t1->tv_sec < t2->tv_sec) {
        cpu->d[0] = (uint32_t)-1;
    } else if (t1->tv_sec > t2->tv_sec) {
        cpu->d[0] = 1;
    } else {
        /* Seconds equal, compare microseconds */
        if (t1->tv_usec < t2->tv_usec) {
            cpu->d[0] = (uint32_t)-1;
        } else if (t1->tv_usec > t2->tv_usec) {
            cpu->d[0] = 1;
        } else {
            cpu->d[0] = 0;
        }
    }
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

/* =========================================================================
 * Registration function
 * ========================================================================= */

void UAOS_TIMER_Register(void)
{
    UAOS_ROM_Register("timer.device", 40, 0x000000A0,
                      (uint16_t)(sizeof(timer_funcs) / sizeof(timer_funcs[0])),
                      timer_funcs);
}
