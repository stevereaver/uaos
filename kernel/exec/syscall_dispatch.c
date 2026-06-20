/* syscall_dispatch.c — UAOS x86-64 INT 0x80 syscall dispatch
 *
 * Phase 4 ABI: dispatches native x86-64 syscalls from the saved interrupt
 * frame, writes the return value back into RAX, and returns to the
 * generic ISR epilogue (which may perform a task switch).
 */

#include "syscall_table.h"
#include "task.h"
#include "../dos/vfs.h"
#include "elf64_loader.h"
#include "../irq/ps2kbd.h"
#include "../boot/kprint.h"
#include <stdint.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * Console output helper
 * ------------------------------------------------------------------------- */
static void console_write(const char *s, size_t len)
{
    kprintbuf(s, len);
}

/* -------------------------------------------------------------------------
 * File descriptor table
 * ------------------------------------------------------------------------- */
#define MAX_FD  16

static VfsFile g_fd_table[MAX_FD];
static int     g_fd_used[MAX_FD];

static int fd_alloc(void)
{
    for (int i = 3; i < MAX_FD; i++) {
        if (!g_fd_used[i]) {
            g_fd_used[i] = 1;
            g_fd_table[i].node = NULL;
            g_fd_table[i].pos = 0;
            g_fd_table[i].nil = 0;
            g_fd_table[i].handle_id = 0;
            return i;
        }
    }
    return -1;
}

static void fd_free(int fd)
{
    if (fd >= 0 && fd < MAX_FD) {
        g_fd_used[fd] = 0;
        g_fd_table[fd].node = NULL;
    }
}

/* -------------------------------------------------------------------------
 * Syscall implementations
 * ------------------------------------------------------------------------- */

static int sys_write(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    int fd = (int)rdi;
    const char *buf = (const char *)(uintptr_t)rsi;
    size_t len = (size_t)rdx;

    if (fd != 1 && fd != 2)
        return -1;   /* EBADF */

    console_write(buf, len);
    return (int)len;
}

static int sys_read(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    int fd = (int)rdi;
    char *buf = (char *)(uintptr_t)rsi;
    size_t max = (size_t)rdx;

    if (fd != 0)
        return -1;   /* EBADF */

    size_t i = 0;
    for (;;) {
        if (PS2Kbd_HasChar()) {
            char c = PS2Kbd_GetChar();
            if (c == '\n' || c == '\r') {
                if (i < max)
                    buf[i] = '\0';
                return (int)i;
            }
            if (c == '\b' || c == 0x7F) {
                if (i > 0) i--;
                continue;
            }
            if (c >= 32 && c < 127 && i + 1 < max) {
                buf[i++] = c;
            }
        }
        __asm__ volatile ("pause");
    }
}

static int sys_open(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    const char *path = (const char *)(uintptr_t)rdi;
    int flags = (int)rsi;
    (void)rdx;

    int fd = fd_alloc();
    if (fd < 0)
        return -1;   /* EMFILE */

    if (!VFS_Open(&g_fd_table[fd], path, flags)) {
        fd_free(fd);
        return -1;   /* ENOENT / other */
    }

    return fd;
}

static int sys_close(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    int fd = (int)rdi;
    (void)rsi; (void)rdx;

    if (fd < 0 || fd >= MAX_FD || !g_fd_used[fd])
        return -1;

    VFS_Close(&g_fd_table[fd]);
    fd_free(fd);
    return 0;
}

static int sys_read_file(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    int fd = (int)rdi;
    uint8_t *buf = (uint8_t *)(uintptr_t)rsi;
    uint32_t len = (uint32_t)rdx;

    if (fd < 0 || fd >= MAX_FD || !g_fd_used[fd])
        return -1;

    return (int)VFS_Read(&g_fd_table[fd], buf, len);
}

static int sys_write_file(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    int fd = (int)rdi;
    const uint8_t *buf = (const uint8_t *)(uintptr_t)rsi;
    uint32_t len = (uint32_t)rdx;

    if (fd < 0 || fd >= MAX_FD || !g_fd_used[fd])
        return -1;

    return (int)VFS_Write(&g_fd_table[fd], buf, len);
}

static int sys_exit(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    int code = (int)rdi;
    (void)rsi; (void)rdx;
    (void)code;

    /* Does not return. */
    Task_Exit();
    __builtin_unreachable();
}

static int sys_getargs(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    const char **buf = (const char **)(uintptr_t)rdi;
    size_t max = (size_t)rsi;
    (void)rdx;

    UaosTask *t = Task_Current();
    if (!t)
        return 0;

    /* Initial stack layout built by ELF64 loader:
     *   [rsp]       = argc
     *   [rsp+8]     = argv[0]
     *   ...
     *   [rsp+8n]    = argv[argc-1]
     *   [rsp+8(n+1)] = NULL
     */
    uint64_t *sp = (uint64_t *)(uintptr_t)t->native_initial_rsp;
    if (!sp)
        return 0;

    uint64_t argc = sp[0];
    if (argc > 64)
        argc = 64;

    size_t n = 0;
    for (n = 0; n < max && n < argc; n++)
        buf[n] = (const char *)(uintptr_t)sp[1 + n];

    return (int)n;
}

static int sys_spawn(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    const char *path = (const char *)(uintptr_t)rdi;
    const char **args = (const char **)(uintptr_t)rsi;
    (void)rdx;

    /* Open the binary file. */
    VfsFile fh;
    if (!VFS_Open(&fh, path, VFS_READ))
        return -1;

    uint32_t size = VFS_Size(&fh);
    if (size == 0) {
        VFS_Close(&fh);
        return -1;
    }

    /* Load the whole file into the x64 heap using the public bump allocator. */
    uint8_t *data = (uint8_t *)ELF64_HeapAlloc(size, 16);
    if (!data) {
        VFS_Close(&fh);
        return -1;
    }

    if (VFS_Read(&fh, data, size) != size) {
        VFS_Close(&fh);
        return -1;
    }
    VFS_Close(&fh);

    ELF64Result res;
    if (ELF64_Load(data, size, args, &res) != 0) {
        return -1;
    }

    UaosTask *child = Task_CreateX64(path, 0, res.entry_rip, res.initial_rsp);
    if (!child)
        return -1;

    return (int)(child - g_tasks);   /* simple task index as PID */
}

static int sys_wait(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    (void)rdi; (void)rsi; (void)rdx;
    return (int)Wait(SIGF_CHILD);
}

static int sys_alloc(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    uint32_t size = (uint32_t)rdi;
    (void)rsi; (void)rdx;

    if (size == 0)
        return 0;

    void *p = ELF64_HeapAlloc(size, 16);
    return (int)(intptr_t)p;
}

/* -------------------------------------------------------------------------
 * Dispatcher
 * ------------------------------------------------------------------------- */
void Syscall_Dispatch(SavedRegs *regs, InterruptFrame *frame)
{
    (void)frame;

    uint64_t n = regs->rax;
    uint64_t rdi = regs->rdi;
    uint64_t rsi = regs->rsi;
    uint64_t rdx = regs->rdx;
    int64_t ret;

    kprint("[SYSCALL] rip="); kprinthex(frame->rip);
    kprint(" rax="); kprinthex(n);
    kprint(" rdi="); kprinthex(rdi);
    kprint(" rsi="); kprinthex(rsi);
    kprint(" rdx="); kprinthex(rdx);
    kprint("\n");

    switch (n) {
    case SYSCALL_WRITE:      ret = sys_write(rdi, rsi, rdx); break;
    case SYSCALL_READ:     ret = sys_read(rdi, rsi, rdx); break;
    case SYSCALL_OPEN:     ret = sys_open(rdi, rsi, rdx); break;
    case SYSCALL_CLOSE:    ret = sys_close(rdi, rsi, rdx); break;
    case SYSCALL_READ_FILE:  ret = sys_read_file(rdi, rsi, rdx); break;
    case SYSCALL_WRITE_FILE: ret = sys_write_file(rdi, rsi, rdx); break;
    case SYSCALL_EXIT:     ret = sys_exit(rdi, rsi, rdx); break;
    case SYSCALL_GETARGS:  ret = sys_getargs(rdi, rsi, rdx); break;
    case SYSCALL_SPAWN:    ret = sys_spawn(rdi, rsi, rdx); break;
    case SYSCALL_WAIT:     ret = sys_wait(rdi, rsi, rdx); break;
    case SYSCALL_ALLOC:    ret = sys_alloc(rdi, rsi, rdx); break;
    case SYSCALL_SCHEDULE:
    default:
        /* Reserved / legacy voluntary yield. */
        Task_ScheduleFromSyscall();
        ret = 0;
        break;
    }

    /* sys_exit does not return; all others write their return value back
     * into the saved RAX slot so the interrupted task sees it. */
    regs->rax = (uint64_t)ret;
}
