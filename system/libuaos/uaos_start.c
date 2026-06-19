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
 * _start reads argc/argv, calls main(), then invokes sys_exit().
 */

#include "uaos_syscall.h"

extern int main(int argc, const char **argv);

__attribute__((noreturn)) void _start(void)
{
    long *sp;
    __asm__ volatile("movq %%rsp, %0" : "=r"(sp));

    int argc = (int)sp[0];
    const char **argv = (const char **)(sp + 1);

    int rc = main(argc, argv);
    uaos_exit(rc);
    __builtin_unreachable();
}
