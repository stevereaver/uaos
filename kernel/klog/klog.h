/*
 * klog.h — Unified kernel logging (UAOS-64)
 *
 * Replaces the ad-hoc kprint + per-file UART helper culture with a single
 * logging path:
 *
 *   - KLOG(subsys, level, fmt, ...)  printf-style one-line log message
 *   - klog_puts/putc/appendf()       streaming API for multi-part lines
 *   - Per-subsystem runtime level mask, settable from the C:klog command
 *     (e.g. "klog vfs,net=debug", "klog all=off")
 *   - 48 KB ring buffer in RAM, dumped/filtered by the C:dmesg command
 *   - Write path is ring buffer + UART only — never the shell/WM paint
 *     path, so logging is safe inside packet dispatch and IRQ handlers.
 *
 * kprint() keeps working exactly as before (VGA + UART); it additionally
 * feeds the ring buffer so dmesg captures boot messages.
 */

#ifndef UAOS_KLOG_H
#define UAOS_KLOG_H

#include <stdint.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * UART (16550A, COM1 = 0x3F8) — the single canonical serial driver.
 * ------------------------------------------------------------------------- */
void uart_init(void);
void uart_putchar(char ch);                 /* '\n' auto-expanded to CRLF */
void uart_puts(const char *s);
void uart_write(const char *s, size_t len);

/* -------------------------------------------------------------------------
 * Log levels — lower number = more severe.  A message is emitted when
 * level <= the subsystem's configured threshold.  KLOG_OFF suppresses
 * everything.
 * ------------------------------------------------------------------------- */
#define KLOG_OFF    0
#define KLOG_ERR    1
#define KLOG_WARN   2
#define KLOG_INFO   3
#define KLOG_DEBUG  4
#define KLOG_TRACE  5

/* -------------------------------------------------------------------------
 * Subsystem identifiers — keep k_subsys_names[] in klog.c in sync.
 * ------------------------------------------------------------------------- */
enum {
    KLOG_KERN = 0,   /* generic kernel / boot */
    KLOG_EXEC,       /* Exec: tasks, memory, libraries, thunks */
    KLOG_DOS,        /* DOS handlers, packets */
    KLOG_VFS,        /* VFS / filesystems */
    KLOG_NET,        /* core IPv4 stack (eth/arp/ip/icmp/udp/tcp) */
    KLOG_DHCP,
    KLOG_DNS,
    KLOG_NTP,
    KLOG_NETDEV,     /* net_device layer */
    KLOG_E1000,      /* e1000 NIC driver */
    KLOG_VIRTIO,     /* virtio drivers */
    KLOG_IDE,        /* IDE/ATAPI */
    KLOG_FLOPPY,
    KLOG_DISP,       /* display / WM */
    KLOG_AUDIO,
    KLOG_CHIP,       /* AGA/ECS chipset emulator */
    KLOG_SHELL,
    KLOG_STRACE,
    KLOG_NSUBSYS
};

/* -------------------------------------------------------------------------
 * Emit API
 * ------------------------------------------------------------------------- */

/* Returns non-zero when (subsys, level) passes the current mask. */
int  klog_enabled(int subsys, int level);

/* Emit one complete formatted line ("[subsys] fmt").  Any trailing '\n'
 * in the formatted output is stripped; the line is committed atomically. */
void klog_emit(int subsys, int level, const char *fmt, ...);

/* KLOG(subsys, level, fmt, ...) — the preferred one-shot macro. */
#define KLOG(subsys, level, ...) \
    do { \
        if (klog_enabled((subsys), (level))) \
            klog_emit((subsys), (level), __VA_ARGS__); \
    } while (0)

/* Streaming API — for building a line across several calls (the pattern
 * the old _xx_puts/_xx_phex helpers used).  Text accumulates in a pending
 * line buffer that is committed when a '\n' is appended or when
 * klog_commit() is called.  All fragments of one logical line must use
 * the same subsys/level. */
void klog_puts(int subsys, int level, const char *s);
void klog_putc(int subsys, int level, char c);
void klog_appendf(int subsys, int level, const char *fmt, ...);
void klog_commit(void);   /* flush a partial pending line, if any */

/* Feed raw text into the ring buffer unconditionally (no mask check, no
 * UART write).  Used by kprint so boot messages land in dmesg while
 * keeping kprint's VGA+UART behaviour unchanged. */
void klog_raw_feed(int subsys, int level, const char *s);
void klog_raw_feedn(int subsys, int level, const char *s, size_t len);

/* -------------------------------------------------------------------------
 * Subsystem / level tables (for the C:klog and C:dmesg commands)
 * ------------------------------------------------------------------------- */
const char *klog_subsys_name(int idx);          /* NULL if out of range   */
int         klog_subsys_find(const char *name); /* index, or -1           */
int         klog_get_level(int subsys);
void        klog_set_level(int subsys, int level);  /* -1 => all subsys   */
const char *klog_level_name(int level);
int         klog_level_find(const char *name);      /* level, or -1       */

/* -------------------------------------------------------------------------
 * Ring buffer access (for C:dmesg)
 * ------------------------------------------------------------------------- */
uint32_t    klog_ring_count(void);                     /* entries present */
int         klog_ring_get(uint32_t idx, int *subsys, int *level,
                          const char **text);          /* oldest-first    */
void        klog_ring_clear(void);

#endif /* UAOS_KLOG_H */
