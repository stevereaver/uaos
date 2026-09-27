/* uaos_start.c — UAOS userspace x86-64 minimal startup
 *
 * Phase 5 ABI: linked into every native x86-64 UAOS program with
 *   -nostdlib -fPIE
 *
 * The ELF loader builds the initial user stack in System V AMD64 ABI layout:
 *   [rsp]      = argc
 *   [rsp+8]    = argv[0]
 *   ...
 *   [rsp+8n]   = argv[argc-1]
 *   [rsp+8n+8] = NULL
 *
 * _start is a naked entry point: the kernel jumps to us with the user stack
 * already 16-byte aligned, so we must not touch the stack before the call to
 * main().  That call pushes the return address, giving main() the expected
 * 16-byte-aligned-minus-8 stack layout and avoiding SSE alignment faults.
 */

#include "uaos_syscall.h"

extern int main(int argc, const char **argv);

/* -------------------------------------------------------------------------
 * Buffered stdout
 *
 * Backing store for the put_s/put_c/put_line helpers in uaos_cmd.h.
 * Keeping the buffer here (instead of a static in the header) lets
 * _start flush whatever remains after main() returns, no matter which
 * translation unit produced the output — one INT 0x80 per line instead
 * of one per fragment.
 * ------------------------------------------------------------------------- */
#define UAOS_STDOUT_BUF_SIZE 512

static char g_uaos_stdout_buf[UAOS_STDOUT_BUF_SIZE];
static int  g_uaos_stdout_len;

void uaos_stdout_flush(void)
{
    if (g_uaos_stdout_len > 0) {
        uaos_syscall3(UAOS_SYSCALL_WRITE, 1,
                      (long)g_uaos_stdout_buf, (long)g_uaos_stdout_len);
        g_uaos_stdout_len = 0;
    }
}

void uaos_stdout_write(const void *buf, long len)
{
    const char *p = (const char *)buf;
    for (long i = 0; i < len; i++) {
        g_uaos_stdout_buf[g_uaos_stdout_len++] = p[i];
        if (p[i] == '\n' || g_uaos_stdout_len == UAOS_STDOUT_BUF_SIZE)
            uaos_stdout_flush();
    }
}

/* -------------------------------------------------------------------------
 * Read-ahead cache behind uaos_read_file()
 *
 * Byte-at-a-time readers (type, more, grep, wc, cat, ...) would otherwise
 * pay one INT 0x80 per byte — a 244-byte file cost 244 sys.read_file
 * calls.  A small per-fd cache refills in 4K blocks so the per-byte
 * callers stay byte-simple while the syscall count collapses.
 *
 * Each slot is keyed to one fd and only ever returns bytes it fetched
 * itself, so interleaved readers on different fds stay independent.  A
 * slot is dropped on close/write/open-reuse of that fd (there is no
 * userspace seek syscall, so no seek invalidation is needed).
 * ------------------------------------------------------------------------- */
#define UAOS_RD_SLOTS 8
#define UAOS_RD_BLOCK 4096

static struct {
    int  fd;
    long len;
    long idx;
    char buf[UAOS_RD_BLOCK];
} g_rd[UAOS_RD_SLOTS] = { [0 ... UAOS_RD_SLOTS - 1] = { .fd = -1 } };

void uaos_rd_invalidate(int fd)
{
    for (int i = 0; i < UAOS_RD_SLOTS; i++)
        if (g_rd[i].fd == fd) g_rd[i].fd = -1;
}

/* Serve up to len pending cached bytes for fd into buf (0 if none).  Lets
 * uaos_read() on a file fd stay in-order after read-ahead. */
long uaos_rd_drain(int fd, void *buf, long len)
{
    for (int i = 0; i < UAOS_RD_SLOTS; i++) {
        if (g_rd[i].fd != fd) continue;
        long avail = g_rd[i].len - g_rd[i].idx;
        if (avail <= 0) return 0;
        long take = (len < avail) ? len : avail;
        for (long j = 0; j < take; j++)
            ((char *)buf)[j] = g_rd[i].buf[g_rd[i].idx + j];
        g_rd[i].idx += take;
        return take;
    }
    return 0;
}

long uaos_read_file(int fd, void *buf, long len)
{
    if (len <= 0)
        return uaos_syscall3(UAOS_SYSCALL_READ_FILE, (long)fd, (long)buf, len);

    int s = -1, free_s = -1;
    for (int i = 0; i < UAOS_RD_SLOTS; i++) {
        if (g_rd[i].fd == fd) { s = i; break; }
        if (free_s < 0 && g_rd[i].fd < 0) free_s = i;
    }
    if (s < 0) {
        s = free_s >= 0 ? free_s : 0;
        g_rd[s].fd = fd; g_rd[s].len = 0; g_rd[s].idx = 0;
    }

    /* Large request with nothing pending: bypass the cache entirely. */
    if (len >= UAOS_RD_BLOCK && g_rd[s].idx >= g_rd[s].len)
        return uaos_syscall3(UAOS_SYSCALL_READ_FILE, (long)fd, (long)buf, len);

    long done = 0;
    while (done < len) {
        if (g_rd[s].idx >= g_rd[s].len) {
            long n = uaos_syscall3(UAOS_SYSCALL_READ_FILE, (long)fd,
                                   (long)g_rd[s].buf, UAOS_RD_BLOCK);
            if (n <= 0) {
                g_rd[s].len = 0;
                g_rd[s].idx = 0;
                return done ? done : n;
            }
            g_rd[s].len = n;
            g_rd[s].idx = 0;
        }
        long avail = g_rd[s].len - g_rd[s].idx;
        long take = (len - done < avail) ? len - done : avail;
        for (long i = 0; i < take; i++)
            ((char *)buf)[done + i] = g_rd[s].buf[g_rd[s].idx + i];
        g_rd[s].idx += take;
        done += take;
    }
    return done;
}

__attribute__((naked, noreturn)) void _start(void)
{
    __asm__ volatile(
        "movq  %%rsp, %%rax\n\t"        /* RAX = initial user stack pointer */
        "movl  (%%rax), %%edi\n\t"       /* RDI = argc */
        "leaq  8(%%rax), %%rsi\n\t"      /* RSI = argv */
        "call  main\n\t"                 /* call main(argc, argv) */
        "movl  %%eax, %%ebx\n\t"         /* save rc across the flush */
        "call  uaos_stdout_flush\n\t"    /* drain buffered stdout */
        "movslq %%ebx, %%rdi\n\t"        /* RDI = main return code */
        "movq  $7, %%rax\n\t"            /* RAX = SYSCALL_EXIT */
        "int   $0x80\n\t"                /* uaos_exit(rc) */
        ::: "rax", "rbx", "rdi", "rsi", "rcx", "rdx", "memory");
    __builtin_unreachable();
}
