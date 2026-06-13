/*
 * ntp.c — SNTP client (RFC 4330)
 *
 * SNTP wire format (48 bytes):
 *
 *   Byte 0    : LI (2 bits) | VN (3 bits) | Mode (3 bits)
 *               Client request: LI=0, VN=4, Mode=3  → 0x23
 *   Bytes 1–3 : Stratum, Poll, Precision (we send 0)
 *   Bytes 4–7 : Root Delay        (we send 0)
 *   Bytes 8–11: Root Dispersion   (we send 0)
 *   Bytes 12–15: Reference ID     (we send 0)
 *   Bytes 16–23: Reference Timestamp (we send 0)
 *   Bytes 24–31: Originate Timestamp (we send 0)
 *   Bytes 32–39: Receive Timestamp   (filled by server)
 *   Bytes 40–47: Transmit Timestamp  (filled by server — this is what we read)
 *
 * The Transmit Timestamp is a 64-bit NTP timestamp:
 *   high 32 bits = seconds since 1900-01-01
 *   low  32 bits = fractional seconds (we ignore)
 *
 * We subtract NTP_EPOCH_DELTA (2208988800) to get Unix time, then
 * use integer arithmetic to decompose into Y/M/D H:M:S.
 */
#include "ntp.h"
#include "udp.h"
#include "stack.h"
#include "net.h"

/* -------------------------------------------------------------------------
 * Serial debug (COM1)
 * ------------------------------------------------------------------------- */
static inline void _nt_outb(uint16_t p, uint8_t v)
{ __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p)); }
static inline uint8_t _nt_inb(uint16_t p)
{ uint8_t v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static void _nt_putc(char c) {
    while ((_nt_inb(0x3FD) & 0x20) == 0) {}
    _nt_outb(0x3F8, (uint8_t)c);
    if (c == '\n') { while ((_nt_inb(0x3FD) & 0x20) == 0) {} _nt_outb(0x3F8, '\r'); }
}
static void _nt_puts(const char *s) { while (*s) _nt_putc(*s++); }
static void _nt_phex32(uint32_t v) {
    static const char h[] = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4) _nt_putc(h[(v >> i) & 0xF]);
}

/* -------------------------------------------------------------------------
 * Epoch keeper — live UTC Unix seconds, ticked by RTC IRQ
 *
 * ntp_tick_epoch() is called from the RTC UIE IRQ handler once per second.
 * However QEMU (and real hardware) can queue multiple UIE interrupts and
 * deliver them in a burst after the CPU was briefly unresponsive (e.g.
 * during a cli window, or a heavy repaint loop).  A burst of rapid ticks
 * makes the clock display jump ahead visibly.
 *
 * Guard: record the TSC at the last tick and refuse to advance the epoch
 * if less than ~900 ms of real time has elapsed since the previous one.
 * This absorbs any burst without losing real seconds (the RTC counter in
 * CMOS is the ground truth; we re-derive from it if we get too far behind).
 * ------------------------------------------------------------------------- */

static volatile uint32_t g_epoch     = 0;
static volatile uint64_t g_last_tick_tsc = 0;   /* TSC at last epoch advance */

static inline uint64_t _ntp_rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Estimate TSC ticks per second at first call (lightweight PIT-free method:
 * just use a fixed conservative estimate for QEMU — 1 GHz = 1e9 ticks/s).
 * For real hardware the guard is slightly loose but still safe: at 3 GHz a
 * "second" is 3e9 ticks, so 900 ms = 2.7e9 ticks. */
#define NTP_MIN_TICK_TSC  900000000ULL   /* 900 ms @ ~1 GHz, conservative */

void ntp_set_epoch(uint32_t unix_utc)
{
    g_epoch         = unix_utc;
    g_last_tick_tsc = _ntp_rdtsc();   /* reset guard on explicit set */
}

uint32_t ntp_get_epoch(void) { return g_epoch; }

void ntp_tick_epoch(void)
{
    if (!g_epoch) return;

    uint64_t now = _ntp_rdtsc();
    uint64_t elapsed = now - g_last_tick_tsc;

    /* Ignore the tick if it arrived too soon after the previous one */
    if (g_last_tick_tsc && elapsed < NTP_MIN_TICK_TSC) return;

    g_epoch++;
    g_last_tick_tsc = now;
}

/* -------------------------------------------------------------------------
 * Calendar decomposition (Unix timestamp → UTC date/time)
 *
 * Uses the algorithm from:
 *   http://howardhinnant.github.io/date_algorithms.html
 * "days_from_civil" inverse — civil_from_days.
 * Valid for dates from 1970 onwards (sufficient for NTP).
 * ------------------------------------------------------------------------- */
void ntp_unix_to_datetime(uint32_t unix_ts,
                           uint16_t *year, uint8_t *month, uint8_t *day,
                           uint8_t *hour, uint8_t *min,   uint8_t *sec)
{
    /* Time-of-day */
    uint32_t tod = unix_ts % 86400U;
    uint32_t days = unix_ts / 86400U;   /* days since 1970-01-01 */

    *sec  = (uint8_t)(tod % 60);
    *min  = (uint8_t)((tod / 60) % 60);
    *hour = (uint8_t)(tod / 3600);

    /* Civil date from days (Gregorian, proleptic) */
    /* Shift so epoch is 0000-03-01 to make leap-day handling trivial */
    uint32_t z  = days + 719468UL;
    uint32_t era = z / 146097UL;
    uint32_t doe = z - era * 146097UL;                   /* day of era [0,146096] */
    uint32_t yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365; /* year of era [0,399] */
    uint32_t y   = yoe + era * 400UL;
    uint32_t doy = doe - (365*yoe + yoe/4 - yoe/100);   /* day of year [0,365] */
    uint32_t mp  = (5*doy + 2) / 153;                   /* month prime [0,11]  */
    uint32_t d   = doy - (153*mp + 2) / 5 + 1;          /* day [1,31]          */
    uint32_t m   = mp < 10 ? mp + 3 : mp - 9;           /* month [1,12]        */
    if (m <= 2) y++;

    *year  = (uint16_t)y;
    *month = (uint8_t)m;
    *day   = (uint8_t)d;
}

/* -------------------------------------------------------------------------
 * SNTP query
 * ------------------------------------------------------------------------- */
int ntp_query(ipv4_t server_ip, uint32_t *out_unix,
              uint32_t timeout_ms,
              NtpPollFn poll_fn, void *poll_arg)
{
    /* Build 48-byte client request */
    uint8_t pkt[NTP_PACKET_LEN];
    for (int i = 0; i < NTP_PACKET_LEN; i++) pkt[i] = 0;
    pkt[0] = 0x23;   /* LI=0, VN=4, Mode=3 (client) */

    int sock = udp_open(0);
    if (sock < 0) {
        _nt_puts("[NTP] no UDP socket\n");
        return 0;
    }

    static const uint32_t RETRY_MS = 2000;
    static const uint32_t SLICE_MS = 50;
    uint32_t elapsed = 0;
    int result = 0;

    while (elapsed < timeout_ms && !result) {
        _nt_puts("[NTP] sending query to "); _nt_phex32(server_ip); _nt_putc('\n');
        udp_send(sock, server_ip, NTP_PORT, pkt, NTP_PACKET_LEN);

        uint32_t waited = 0;
        while (waited < RETRY_MS && elapsed < timeout_ms && !result) {
            if (poll_fn)
                poll_fn(poll_arg, SLICE_MS);
            else {
                volatile uint32_t n = 5000000UL;
                while (n--) __asm__ volatile("pause");
                net_stack_poll();
            }
            waited  += SLICE_MS;
            elapsed += SLICE_MS;

            uint8_t  rbuf[NTP_PACKET_LEN + 16];
            ipv4_t   src_ip   = 0;
            uint16_t src_port = 0;
            int rlen = udp_recv(sock, rbuf, (uint16_t)sizeof(rbuf),
                                &src_ip, &src_port);

            if (rlen >= NTP_PACKET_LEN && src_port == NTP_PORT) {
                /* Mode field of response should be 4 (server) */
                uint8_t mode = rbuf[0] & 0x07;
                if (mode != 4) continue;

                /* Transmit Timestamp: bytes 40–43 = seconds (big-endian) */
                uint32_t ntp_secs = ((uint32_t)rbuf[40] << 24) |
                                    ((uint32_t)rbuf[41] << 16) |
                                    ((uint32_t)rbuf[42] <<  8) |
                                     (uint32_t)rbuf[43];

                _nt_puts("[NTP] rx ntp_secs="); _nt_phex32(ntp_secs); _nt_putc('\n');

                if (ntp_secs < NTP_EPOCH_DELTA) {
                    _nt_puts("[NTP] bad timestamp (before 1970)\n");
                    continue;
                }

                *out_unix = ntp_secs - (uint32_t)NTP_EPOCH_DELTA;
                _nt_puts("[NTP] unix="); _nt_phex32(*out_unix); _nt_putc('\n');
                result = 1;
            }
        }
    }

    udp_close(sock);
    if (!result) _nt_puts("[NTP] timed out\n");
    return result;
}
