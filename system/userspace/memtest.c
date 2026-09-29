/* memtest.c — UAOS x86-64 userspace 'memtest' command
 *
 * Self-test for the x64 heap free-list allocator (UAOS-152): exercises
 * SYSCALL_ALLOC / SYSCALL_FREE, verifies block reuse through
 * SYSCALL_MEMINFO, and probes the kernel's rejection of double-free and
 * wild-pointer frees.
 *
 * Exit code 0 = all checks passed, 20 = at least one failed (AmigaDOS
 * failat convention).
 */

#include "uaos_cmd.h"

static int g_fails = 0;

static void check(const char *name, int ok)
{
    put_s(name);
    put_s(": ");
    put_line(ok ? "ok" : "FAIL");
    if (!ok)
        g_fails++;
}

static void put_hex(uintptr_t v)
{
    char buf[17];
    int i = 16;
    buf[i] = '\0';
    do {
        int d = (int)(v & 0xF);
        buf[--i] = (char)(d < 10 ? '0' + d : 'A' + d - 10);
        v >>= 4;
    } while (v);
    put_s("0x");
    put_s(buf + i);
}

int main(int argc, const char **argv)
{
    (void)argc; (void)argv;

    struct uaos_meminfo m0, m1, m2;
    if (uaos_meminfo(&m0) != 0) {
        put_line("memtest: meminfo query failed");
        return 20;
    }

    /* --- alloc / free / reuse ---------------------------------------- */
    uint8_t *p1 = (uint8_t *)uaos_alloc(4096);
    check("alloc returns a block", p1 != NULL);
    check("block is 16-aligned", ((uintptr_t)p1 & 15) == 0);
    if (!p1)
        goto out;

    put_s("p1 = "); put_hex((uintptr_t)p1); put_c('\n');

    for (int i = 0; i < 4096; i++)
        p1[i] = (uint8_t)i;
    int ok = 1;
    for (int i = 0; i < 4096; i++)
        if (p1[i] != (uint8_t)i) { ok = 0; break; }
    check("block is writable", ok);

    uaos_meminfo(&m1);
    check("x64_used grew after alloc", m1.x64_used > m0.x64_used);

    uaos_free(p1);
    uaos_meminfo(&m2);
    check("x64_used dropped after free", m2.x64_used < m1.x64_used);

    uint8_t *p2 = (uint8_t *)uaos_alloc(4096);
    put_s("p2 = "); put_hex((uintptr_t)p2); put_c('\n');
    check("freed block reused (first-fit)", p2 == p1);

    /* --- double-free / wild-free rejection ---------------------------- */
    /* The kernel logs rejections on serial; survival here is the test —
     * a heap clobbered by either call would fault the next alloc. */
    uaos_free(p2);
    uaos_free(p2);                      /* double free — must be rejected */
    uint8_t *p3 = (uint8_t *)uaos_alloc(4096);
    put_s("p3 = "); put_hex((uintptr_t)p3); put_c('\n');
    check("heap intact after double free", p3 != NULL);
    check("double-free released once", p3 == p2);

    uaos_free((void *)(uintptr_t)0x1000);   /* below the arena — reject */
    uaos_free(NULL);                        /* no-op */
    put_line("wild/NULL frees rejected (see serial log)");

    /* --- malloc/calloc/free names via uaos_libc ---------------------- */
    int *arr = (int *)calloc(16, sizeof(int));
    check("calloc returns block", arr != NULL);
    if (arr) {
        check("calloc memory is zeroed", arr[0] == 0 && arr[15] == 0);
        arr[7] = 42;
        check("calloc block is writable", arr[7] == 42);
        free(arr);
    }

    /* --- leave nothing behind ---------------------------------------- */
    uaos_free(p3);
    uaos_meminfo(&m2);
    check("heap back at baseline", m2.x64_used == m0.x64_used);

out:
    put_s("memtest: ");
    if (g_fails == 0) {
        put_line("all checks passed");
    } else {
        char num[24];
        uint_to_dec((uint32_t)g_fails, num, sizeof(num));
        put_s(num);
        put_line(" check(s) FAILED");
    }
    return g_fails ? 20 : 0;
}
