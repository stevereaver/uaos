/*
 * klog.c — Unified kernel logging (UAOS-64)
 *
 * Write path: pending line buffer -> 48 KB ring buffer + UART.
 * Never touches VGA, the shell, or the WM paint path, so it is safe to
 * call from packet dispatch, IRQ handlers, and deep in the net stack.
 *
 * A message is emitted when its level <= the subsystem's runtime
 * threshold (default KLOG_DEBUG for all subsystems, i.e. everything that
 * previously went straight to the UART still does).
 */

#include "klog.h"
#include <stdarg.h>

/* -------------------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------------------- */
#define KLOG_LINE_MAX      160   /* max bytes stored per log line         */
#define KLOG_RING_ENTRIES  288   /* 288 * ~168 B = ~47 KB ring            */

/* -------------------------------------------------------------------------
 * Subsystem / level tables
 * ------------------------------------------------------------------------- */
static const char *const k_subsys_names[KLOG_NSUBSYS] = {
    "kern", "exec", "dos", "vfs", "net", "dhcp", "dns", "ntp",
    "netdev", "e1000", "virtio", "ide", "floppy", "disp", "audio",
    "chip", "shell", "strace",
};

static const char *const k_level_names[] = {
    "off", "error", "warn", "info", "debug", "trace",
};

/* Runtime per-subsystem threshold.  Everything enabled at DEBUG by
 * default so the serial boot/debug output is unchanged from the old
 * _xx_puts days. */
static uint8_t g_threshold[KLOG_NSUBSYS] =
    { [0 ... KLOG_NSUBSYS - 1] = KLOG_DEBUG };

int klog_enabled(int subsys, int level)
{
    if (subsys < 0 || subsys >= KLOG_NSUBSYS) return 0;
    return level <= g_threshold[subsys];
}

/* -------------------------------------------------------------------------
 * Minimal printf-style formatter (no libc)
 *
 * Supported: %s %c %d %i %u %x %X %p %%
 * Optional zero-pad width:  %02x %08x %3d ...
 * Optional length modifier: %ld %lu %lx %llu %llx  (l == ll == 64-bit)
 * ------------------------------------------------------------------------- */
static void kfmt_char(char *dst, int cap, int *pos, char c)
{
    if (*pos < cap - 1) dst[*pos] = c;
    (*pos)++;
}

static void kfmt_str(char *dst, int cap, int *pos, const char *s)
{
    if (!s) s = "(null)";
    while (*s) kfmt_char(dst, cap, pos, *s++);
}

static void kfmt_uint(char *dst, int cap, int *pos,
                      uint64_t v, unsigned base, int upper,
                      int width, char pad)
{
    const char *h = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v && n < (int)sizeof(tmp)) {
        tmp[n++] = h[v % base];
        v /= base;
    }
    while (n < width) { tmp[n++] = pad; }
    while (n > 0) kfmt_char(dst, cap, pos, tmp[--n]);
}

static int kfmt(char *dst, int cap, const char *fmt, va_list ap)
{
    int pos = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            kfmt_char(dst, cap, &pos, *p);
            continue;
        }
        p++;
        if (!*p) break;
        if (*p == '%') {
            kfmt_char(dst, cap, &pos, '%');
            continue;
        }

        char pad = ' ';
        int width = 0;
        if (*p == '0') { pad = '0'; p++; }
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }

        int is64 = 0;
        while (*p == 'l') { is64 = 1; p++; }
        if (!*p) break;

        switch (*p) {
        case 's':
            kfmt_str(dst, cap, &pos, va_arg(ap, const char *));
            break;
        case 'c':
            kfmt_char(dst, cap, &pos, (char)va_arg(ap, int));
            break;
        case 'd':
        case 'i': {
            int64_t v = is64 ? va_arg(ap, int64_t) : (int64_t)va_arg(ap, int32_t);
            if (v < 0) {
                kfmt_char(dst, cap, &pos, '-');
                kfmt_uint(dst, cap, &pos, (uint64_t)(-v), 10, 0, width, pad);
            } else {
                kfmt_uint(dst, cap, &pos, (uint64_t)v, 10, 0, width, pad);
            }
            break;
        }
        case 'u':
            kfmt_uint(dst, cap, &pos,
                      is64 ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, uint32_t),
                      10, 0, width, pad);
            break;
        case 'x':
        case 'X':
            kfmt_uint(dst, cap, &pos,
                      is64 ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, uint32_t),
                      16, (*p == 'X'), width, pad);
            break;
        case 'p':
            kfmt_str(dst, cap, &pos, "0x");
            kfmt_uint(dst, cap, &pos, (uint64_t)(uintptr_t)va_arg(ap, void *),
                      16, 0, 0, '0');
            break;
        default:
            kfmt_char(dst, cap, &pos, '%');
            kfmt_char(dst, cap, &pos, *p);
            break;
        }
    }
    if (pos >= cap) pos = cap - 1;
    dst[pos] = '\0';
    return pos;
}

/* -------------------------------------------------------------------------
 * Ring buffer
 * ------------------------------------------------------------------------- */
typedef struct {
    uint32_t seq;
    uint8_t  subsys;
    uint8_t  level;
    uint16_t len;
    char     text[KLOG_LINE_MAX];
} KLogEntry;

static KLogEntry g_ring[KLOG_RING_ENTRIES];
static uint32_t  g_ring_head;   /* next slot to write                  */
static uint32_t  g_ring_used;   /* entries present (<= RING_ENTRIES)   */
static uint32_t  g_ring_seq;    /* monotonically increasing sequence   */

static void ring_push(int subsys, int level, const char *text, uint16_t len)
{
    KLogEntry *e = &g_ring[g_ring_head];
    e->seq    = g_ring_seq++;
    e->subsys = (uint8_t)subsys;
    e->level  = (uint8_t)level;
    e->len    = len;
    for (uint16_t i = 0; i < len; i++)
        e->text[i] = text[i];
    e->text[len] = '\0';

    g_ring_head = (g_ring_head + 1) % KLOG_RING_ENTRIES;
    if (g_ring_used < KLOG_RING_ENTRIES)
        g_ring_used++;
}

uint32_t klog_ring_count(void)
{
    return g_ring_used;
}

int klog_ring_get(uint32_t idx, int *subsys, int *level, const char **text)
{
    if (idx >= g_ring_used) return 0;
    uint32_t oldest = (g_ring_head + KLOG_RING_ENTRIES - g_ring_used)
                      % KLOG_RING_ENTRIES;
    const KLogEntry *e = &g_ring[(oldest + idx) % KLOG_RING_ENTRIES];
    if (subsys) *subsys = e->subsys;
    if (level)  *level  = e->level;
    if (text)   *text   = e->text;
    return 1;
}

void klog_ring_clear(void)
{
    g_ring_head = 0;
    g_ring_used = 0;
}

/* -------------------------------------------------------------------------
 * Pending-line assembler
 *
 * Streaming calls append to g_pend; the line is committed to the ring
 * buffer and (optionally) the UART on '\n' or klog_commit().
 * ------------------------------------------------------------------------- */
static char    g_pend[KLOG_LINE_MAX];
static uint16_t g_pend_len;
static int     g_pend_subsys;
static int     g_pend_level;
static int     g_pend_uart;
static int     g_pend_on;

static void line_commit(void)
{
    if (!g_pend_on) return;
    if (g_pend_len > 0) {
        ring_push(g_pend_subsys, g_pend_level, g_pend, g_pend_len);
        if (g_pend_uart) {
            uart_write(g_pend, g_pend_len);
            uart_putchar('\n');
        }
    }
    g_pend_on  = 0;
    g_pend_len = 0;
}

static void line_begin(int subsys, int level, int to_uart)
{
    /* Different attribution mid-line: flush what we have first. */
    if (g_pend_on &&
        (g_pend_subsys != subsys || g_pend_level != level ||
         g_pend_uart != to_uart)) {
        line_commit();
    }
    if (g_pend_on) return;

    g_pend_on     = 1;
    g_pend_subsys = subsys;
    g_pend_level  = level;
    g_pend_uart   = to_uart;
    g_pend_len    = 0;

    /* "[name] " prefix */
    g_pend[g_pend_len++] = '[';
    const char *n = klog_subsys_name(subsys);
    if (n) while (*n && g_pend_len < KLOG_LINE_MAX - 1)
        g_pend[g_pend_len++] = *n++;
    g_pend[g_pend_len++] = ']';
    g_pend[g_pend_len++] = ' ';
}

static void feed_char(int subsys, int level, int to_uart, char c)
{
    if (c == '\r') return;                    /* drop stray CRs          */
    if (c == '\n') { line_commit(); return; }
    line_begin(subsys, level, to_uart);       /* flushes on attr change  */
    if (g_pend_len < KLOG_LINE_MAX - 1)
        g_pend[g_pend_len++] = c;
    /* Overflowing characters are silently truncated — the line stays a
     * single ring entry. */
}

static void feed_str(int subsys, int level, int to_uart, const char *s)
{
    for (; *s; s++)
        feed_char(subsys, level, to_uart, *s);
}

/* -------------------------------------------------------------------------
 * Public emit API
 * ------------------------------------------------------------------------- */
void klog_emit(int subsys, int level, const char *fmt, ...)
{
    if (!klog_enabled(subsys, level)) return;

    char body[KLOG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    kfmt(body, sizeof(body), fmt, ap);
    va_end(ap);

    feed_str(subsys, level, 1, body);
    line_commit();
}

void klog_puts(int subsys, int level, const char *s)
{
    if (!klog_enabled(subsys, level)) return;
    feed_str(subsys, level, 1, s);
}

void klog_putc(int subsys, int level, char c)
{
    if (!klog_enabled(subsys, level)) return;
    feed_char(subsys, level, 1, c);
}

void klog_appendf(int subsys, int level, const char *fmt, ...)
{
    if (!klog_enabled(subsys, level)) return;

    char body[KLOG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    kfmt(body, sizeof(body), fmt, ap);
    va_end(ap);

    feed_str(subsys, level, 1, body);
}

void klog_commit(void)
{
    line_commit();
}

void klog_raw_feed(int subsys, int level, const char *s)
{
    feed_str(subsys, level, 0, s);
}

void klog_raw_feedn(int subsys, int level, const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++)
        feed_char(subsys, level, 0, s[i]);
}

/* -------------------------------------------------------------------------
 * Subsystem / level lookup + runtime control
 * ------------------------------------------------------------------------- */
static int klog_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        char ac = *a; if (ac >= 'A' && ac <= 'Z') ac += 32;
        char bc = *b; if (bc >= 'A' && bc <= 'Z') bc += 32;
        if (ac != bc) return 0;
        a++; b++;
    }
    return *a == *b;
}

const char *klog_subsys_name(int idx)
{
    if (idx < 0 || idx >= KLOG_NSUBSYS) return 0;
    return k_subsys_names[idx];
}

int klog_subsys_find(const char *name)
{
    for (int i = 0; i < KLOG_NSUBSYS; i++) {
        if (klog_ieq(k_subsys_names[i], name))
            return i;
    }
    return -1;
}

int klog_get_level(int subsys)
{
    if (subsys < 0 || subsys >= KLOG_NSUBSYS) return KLOG_OFF;
    return g_threshold[subsys];
}

void klog_set_level(int subsys, int level)
{
    if (level < KLOG_OFF || level > KLOG_TRACE) return;
    if (subsys < 0) {
        for (int i = 0; i < KLOG_NSUBSYS; i++)
            g_threshold[i] = (uint8_t)level;
        return;
    }
    if (subsys >= KLOG_NSUBSYS) return;
    g_threshold[subsys] = (uint8_t)level;
}

const char *klog_level_name(int level)
{
    if (level < KLOG_OFF || level > KLOG_TRACE) return "?";
    return k_level_names[level];
}

int klog_level_find(const char *name)
{
    for (int i = KLOG_OFF; i <= KLOG_TRACE; i++) {
        if (klog_ieq(k_level_names[i], name))
            return i;
    }
    /* Convenience aliases */
    if (klog_ieq(name, "err"))   return KLOG_ERR;
    if (klog_ieq(name, "dbg"))   return KLOG_DEBUG;
    if (klog_ieq(name, "none"))  return KLOG_OFF;
    return -1;
}
