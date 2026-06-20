/* pwd.c — UAOS x86-64 userspace 'pwd' command
 *
 * Prints the current working directory of the calling shell by asking
 * the kernel for the per-task cwd via the INT 0x80 syscall interface.
 */

#include "uaos_syscall.h"

static void put_s(const char *s)
{
    while (*s)
        uaos_write(1, s++, 1);
}

int main(int argc, const char **argv)
{
    (void)argc;
    (void)argv;

    char buf[128];
    long n = uaos_getcwd(buf, sizeof(buf));
    if (n > 0) {
        put_s(buf);
    }
    put_s("\n");

    return 0;
}
