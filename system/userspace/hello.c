/* hello.c — UAOS x86-64 userspace proof-of-concept
 *
 * Phase 7 ABI: simple native program that uses the INT 0x80 syscall
 * wrappers in libuaos to print a greeting.  Verifies the full ELF64 load
 * -> syscall -> console output pipeline end to end.
 */

#include "uaos_syscall.h"

int main(int argc, const char **argv)
{
    (void)argc;
    (void)argv;

    static const char msg[] = "Hello from UAOS x86-64 userspace!\n";
    uaos_write(1, msg, sizeof(msg) - 1);

    return 0;
}
