/* syscall_table.h — UAOS x86-64 syscall numbers and interrupt frame types
 *
 * Phase 4 ABI: defines the INT 0x80 syscall table for native x86-64 tasks.
 * Calling convention matches the Linux x86-64 syscall ABI:
 *   RAX = syscall number
 *   RDI = arg 1, RSI = arg 2, RDX = arg 3
 *   Return value in RAX
 */

#ifndef UAOS_SYSCALL_TABLE_H
#define UAOS_SYSCALL_TABLE_H

#include <stdint.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * Syscall numbers
 * ------------------------------------------------------------------------- */
#define SYSCALL_WRITE       0x01   /* sys_write(fd, buf, len)            */
#define SYSCALL_READ        0x02   /* sys_read(fd, buf, len)             */
#define SYSCALL_OPEN        0x03   /* sys_open(path, flags)              */
#define SYSCALL_CLOSE       0x04   /* sys_close(fd)                      */
#define SYSCALL_READ_FILE   0x05   /* sys_read_file(fd, buf, len)        */
#define SYSCALL_WRITE_FILE  0x06   /* sys_write_file(fd, buf, len)       */
#define SYSCALL_EXIT        0x07   /* sys_exit(code)                     */
#define SYSCALL_GETARGS     0x08   /* sys_getargs(buf, max)              */
#define SYSCALL_SPAWN       0x09   /* sys_spawn(path, args)              */
#define SYSCALL_WAIT        0x0A   /* sys_wait()                         */
#define SYSCALL_ALLOC       0x0B   /* sys_alloc(size)                    */
#define SYSCALL_SCHEDULE    0xFF   /* reserved: yield/reschedule         */

/* -------------------------------------------------------------------------
 * Interrupt frame exposed to syscall / page-fault handlers.
 *
 * CPU exceptions with an error code (e.g. #PF) push error_code, then
 * RIP/CS/RFLAGS/RSP/SS.  The custom INT 0x80 entry pushes a dummy error_code
 * so the CPU frame (RIP/CS/RFLAGS/RSP/SS) sits at the same offsets, allowing
 * the same InterruptFrame definition to be reused.
 * ------------------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    uint64_t error_code;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} InterruptFrame;

/* GPR block saved by isr_common in idt_stubs.asm. */
typedef struct __attribute__((packed)) {
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11;
    uint64_t r12, r13, r14, r15;
} SavedRegs;

/* -------------------------------------------------------------------------
 * Dispatch entry
 * ------------------------------------------------------------------------- */
/* Assembly calls: rdi=SavedRegs*, rsi=InterruptFrame* (SysV order) */
void Syscall_Dispatch(SavedRegs *regs, InterruptFrame *frame);

#endif /* UAOS_SYSCALL_TABLE_H */
