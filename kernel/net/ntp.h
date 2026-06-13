/*
 * ntp.h — SNTP client (RFC 4330 / RFC 5905)
 *
 * Sends a single SNTPv4 request to the configured server and returns
 * the server's time as a Unix timestamp (seconds since 1970-01-01 UTC).
 * The caller is responsible for writing the result to the RTC.
 */
#ifndef UAOS_NTP_H
#define UAOS_NTP_H

#include "net.h"
#include <stdint.h>

#define NTP_PORT        123
#define NTP_PACKET_LEN  48

/* NTP epoch offset: seconds between 1900-01-01 and 1970-01-01 */
#define NTP_EPOCH_DELTA  2208988800UL

/* Poll/yield callback — same signature as DnsPollFn */
typedef void (*NtpPollFn)(void *arg, uint32_t ms);

/*
 * Perform a single SNTP query.
 *
 * server_ip  : NTP server (host byte order), e.g. from dns_resolve()
 * out_unix   : filled with Unix timestamp (seconds since 1970) on success
 * timeout_ms : how long to wait for a reply
 * poll_fn    : called each ~50 ms slice to pump network / UI; NULL = busy-poll
 * poll_arg   : forwarded to poll_fn
 *
 * Returns 1 on success, 0 on timeout or error.
 */
int ntp_query(ipv4_t server_ip, uint32_t *out_unix,
              uint32_t timeout_ms,
              NtpPollFn poll_fn, void *poll_arg);

/*
 * Decompose a Unix timestamp into calendar fields (UTC, proleptic Gregorian).
 * Fills year (e.g. 2026), month (1–12), day (1–31), hour, min, sec.
 */
void ntp_unix_to_datetime(uint32_t unix_ts,
                           uint16_t *year, uint8_t *month, uint8_t *day,
                           uint8_t *hour, uint8_t *min,   uint8_t *sec);

/*
 * Epoch keeper — updated by ntpd after a successful sync.
 * The RTC IRQ increments this by 1 each second so it stays live.
 * Value is UTC Unix seconds (0 = not yet synced).
 */
void     ntp_set_epoch(uint32_t unix_utc);
uint32_t ntp_get_epoch(void);
void     ntp_tick_epoch(void);   /* call from RTC IRQ (adds 1 second) */

#endif /* UAOS_NTP_H */
