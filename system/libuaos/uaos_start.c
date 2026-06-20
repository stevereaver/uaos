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

__attribute__((naked, noreturn)) void _start(void)
{
    __asm__ volatile(
        "movw  $0x3F8, %%dx\n\t"
        "movb  $'S', %%al\n\t"
        "outb  %%al, %%dx\n\t"
        "movq  %%rsp, %%rax\n\t"        /* RAX = initial user stack pointer */
        "movl  (%%rax), %%edi\n\t"       /* RDI = argc */
        "leaq  8(%%rax), %%rsi\n\t"      /* RSI = argv */
        "call  main\n\t"                 /* call main(argc, argv) */
        "movslq %%eax, %%rdi\n\t"        /* RDI = main return code */
        "movq  $7, %%rax\n\t"            /* RAX = SYSCALL_EXIT */
        "int   $0x80\n\t"                /* uaos_exit(rc) */
        ::: "rax", "rdi", "rsi", "rcx", "rdx", "memory");
    __builtin_unreachable();
}
