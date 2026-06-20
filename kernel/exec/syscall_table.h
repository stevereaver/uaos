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
#define SYSCALL_GETCWD      0x0C   /* sys_getcwd(buf, size)              */
#define SYSCALL_OPENDIR     0x0D   /* sys_opendir(path) -> dir fd          */
#define SYSCALL_READDIR     0x0E   /* sys_readdir(fd, ent)                 */
#define SYSCALL_CLOSEDIR    0x0F   /* sys_closedir(fd)                   */
#define SYSCALL_STAT        0x10   /* sys_stat(path, stat)               */

/* GUI / windowing syscalls for userspace tasks */
#define SYSCALL_GUI_CREATE_WINDOW  0x11   /* create_window(title, x, y, w, h) */
#define SYSCALL_GUI_DESTROY_WINDOW 0x12   /* destroy_window(handle)           */
#define SYSCALL_GUI_SET_SCROLL_INFO 0x13  /* set_scroll_info(handle, cw, ch)*/
#define SYSCALL_GUI_SET_SCROLL     0x14   /* set_scroll(handle, sx, sy)       */
#define SYSCALL_GUI_DRAW_TEXT      0x15   /* draw_text(handle, x, y, text, col)*/
#define SYSCALL_GUI_DRAW_RECT      0x16   /* draw_rect(handle, x, y, w, h, col)*/
#define SYSCALL_GUI_PRESENT        0x17   /* present(handle)                    */
#define SYSCALL_GUI_GET_EVENT      0x18   /* get_event(handle, event)         */

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

/* GPR block as saved by the page-fault ISR (uaos_page_fault_isr): GPRs are
 * pushed rax-last, so rax sits at the lowest address and this field order
 * matches the in-memory layout. */
typedef struct __attribute__((packed)) {
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11;
    uint64_t r12, r13, r14, r15;
} SavedRegs;

/* GPR block as saved by uaos_syscall_isr.  It uses the *same* push order and
 * frame layout as isr_common (GPRs pushed rax-first, so r15 ends up at the
 * lowest address, followed by vector + error_code padding).  Using an
 * identical layout is what allows a task switched out by the timer ISR to be
 * resumed by the syscall ISR (and vice-versa) without corrupting the frame.
 * The field order below matches that ascending-address memory layout. */
typedef struct __attribute__((packed)) {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
} SyscallRegs;

/* -------------------------------------------------------------------------
 * Dispatch entry
 * ------------------------------------------------------------------------- */
/* Assembly calls: rdi=SyscallRegs*, rsi=InterruptFrame* (SysV order) */
void Syscall_Dispatch(SyscallRegs *regs, InterruptFrame *frame);

#endif /* UAOS_SYSCALL_TABLE_H */
