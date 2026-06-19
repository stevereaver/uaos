/* uaos_syscall.h — UAOS userspace x86-64 syscall helpers
 *
 * Phase 5 ABI: header-only inline wrappers for the INT 0x80 syscall table.
 * Include this in any UAOS native program; no standard library is required.
 *
 * Calling convention matches the kernel's syscall_table.h:
 *   RAX = syscall number
 *   RDI = arg 1, RSI = arg 2, RDX = arg 3
 *   Return value in RAX
 */

#ifndef UAOS_SYSCALL_H
#define UAOS_SYSCALL_H

#include <stdint.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * Syscall numbers (must stay in sync with kernel/exec/syscall_table.h)
 * ------------------------------------------------------------------------- */
#define UAOS_SYSCALL_WRITE       0x01
#define UAOS_SYSCALL_READ        0x02
#define UAOS_SYSCALL_OPEN        0x03
#define UAOS_SYSCALL_CLOSE       0x04
#define UAOS_SYSCALL_READ_FILE   0x05
#define UAOS_SYSCALL_WRITE_FILE  0x06
#define UAOS_SYSCALL_EXIT        0x07
#define UAOS_SYSCALL_GETARGS     0x08
#define UAOS_SYSCALL_SPAWN       0x09
#define UAOS_SYSCALL_WAIT        0x0A
#define UAOS_SYSCALL_ALLOC       0x0B

/* -------------------------------------------------------------------------
 * Generic raw syscall helpers
 * ------------------------------------------------------------------------- */
static inline long uaos_syscall0(long n)
{
    long ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(n)
        : "memory");
    return ret;
}

static inline long uaos_syscall1(long n, long a1)
{
    long ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(n), "D"(a1)
        : "memory");
    return ret;
}

static inline long uaos_syscall2(long n, long a1, long a2)
{
    long ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(n), "D"(a1), "S"(a2)
        : "memory");
    return ret;
}

static inline long uaos_syscall3(long n, long a1, long a2, long a3)
{
    long ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(n), "D"(a1), "S"(a2), "d"(a3)
        : "memory");
    return ret;
}

/* -------------------------------------------------------------------------
 * Convenience wrappers
 * ------------------------------------------------------------------------- */
static inline long uaos_write(int fd, const void *buf, long len)
{
    return uaos_syscall3(UAOS_SYSCALL_WRITE, (long)fd, (long)buf, len);
}

static inline long uaos_read(int fd, void *buf, long len)
{
    return uaos_syscall3(UAOS_SYSCALL_READ, (long)fd, (long)buf, len);
}

static inline long uaos_open(const char *path, int flags)
{
    return uaos_syscall2(UAOS_SYSCALL_OPEN, (long)path, (long)flags);
}

static inline long uaos_close(int fd)
{
    return uaos_syscall1(UAOS_SYSCALL_CLOSE, (long)fd);
}

static inline long uaos_read_file(int fd, void *buf, long len)
{
    return uaos_syscall3(UAOS_SYSCALL_READ_FILE, (long)fd, (long)buf, len);
}

static inline long uaos_write_file(int fd, const void *buf, long len)
{
    return uaos_syscall3(UAOS_SYSCALL_WRITE_FILE, (long)fd, (long)buf, len);
}

__attribute__((noreturn)) static inline void uaos_exit(int code)
{
    __asm__ volatile(
        "int $0x80"
        :
        : "a"((long)UAOS_SYSCALL_EXIT), "D"((long)code)
        : "memory");
    __builtin_unreachable();
}

static inline long uaos_getargs(const char **buf, long max)
{
    return uaos_syscall2(UAOS_SYSCALL_GETARGS, (long)buf, max);
}

static inline long uaos_spawn(const char *path, const char **args)
{
    return uaos_syscall2(UAOS_SYSCALL_SPAWN, (long)path, (long)args);
}

static inline long uaos_wait(void)
{
    return uaos_syscall0(UAOS_SYSCALL_WAIT);
}

static inline void *uaos_alloc(long size)
{
    return (void *)(uintptr_t)uaos_syscall1(UAOS_SYSCALL_ALLOC, size);
}

#endif /* UAOS_SYSCALL_H */
