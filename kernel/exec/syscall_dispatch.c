/* syscall_dispatch.c — UAOS x86-64 INT 0x80 syscall dispatch
 *
 * Phase 4 ABI: dispatches native x86-64 syscalls from the saved interrupt
 * frame, writes the return value back into RAX, and returns to the
 * generic ISR epilogue (which may perform a task switch).
 */

#include "syscall_table.h"
#include "task.h"
#include "mem_info.h"
#include "../dos/vfs.h"
#include "elf64_loader.h"
#include "../irq/ps2kbd.h"
#include "../boot/kprint.h"
#include "../display/user_window.h"
#include <stdint.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * Console output helper
 *
 * stdout/stderr from X64 userspace tasks are routed to the task's
 * native_print_fn if one was provided at creation.  Output is accumulated
 * in task_out and flushed line-by-line so the shell can display each line
 * in its history buffer.
 * ------------------------------------------------------------------------- */
static void task_flush_line(UaosTask *t)
{
    if (!t || t->task_out_len <= 0)
        return;

    char line[256];
    int i;
    for (i = 0; i < t->task_out_len && i < 255; i++)
        line[i] = t->task_out[i];
    line[i] = '\0';

    if (t->native_print_fn)
        t->native_print_fn(t->native_print_ctx, line);
    else
        kprintbuf(line, (size_t)i);

    t->task_out_len = 0;
}

static void task_flush_all(UaosTask *t)
{
    if (!t || t->task_out_len <= 0)
        return;
    task_flush_line(t);
}

static void task_write_output(const char *s, size_t len)
{
    UaosTask *t = Task_Current();
    if (!t) {
        kprintbuf(s, len);
        return;
    }

    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == '\n') {
            task_flush_line(t);
        } else if (t->task_out_len < (int)sizeof(t->task_out) - 1) {
            t->task_out[t->task_out_len++] = c;
        } else {
            /* Buffer full without newline — flush as a partial line. */
            task_flush_line(t);
            if (t->task_out_len < (int)sizeof(t->task_out) - 1)
                t->task_out[t->task_out_len++] = c;
        }
    }
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
 * Directory handle table
 * ------------------------------------------------------------------------- */
#define MAX_DIR_FD  16

static RamFsNode *g_dir_table[MAX_DIR_FD];
static int        g_dir_used[MAX_DIR_FD];

static int dir_alloc(void)
{
    for (int i = 0; i < MAX_DIR_FD; i++) {
        if (!g_dir_used[i]) {
            g_dir_used[i] = 1;
            g_dir_table[i] = NULL;
            return i;
        }
    }
    return -1;
}

static void dir_free(int dd)
{
    if (dd >= 0 && dd < MAX_DIR_FD) {
        g_dir_used[dd] = 0;
        g_dir_table[dd] = NULL;
    }
}

/* -------------------------------------------------------------------------
 * Path helper — resolve a user path against the task's cwd
 *
 * AmigaDOS paths are absolute if they contain a volume/assign prefix (e.g.
 * "C:foo" or "RAM:T/foo").  A bare argument like "lha" is relative to the
 * calling task's current working directory, so prepend cwd + '/'.
 * ------------------------------------------------------------------------- */
#define UAOS_PATH_MAX 256

static void make_abs_path(const char *cwd, const char *arg, char *out, size_t max)
{
    const char *p = arg;
    while (*p && *p != ':')
        p++;
    if (*p == ':') {
        /* Already absolute. */
        size_t i = 0;
        while (i < max - 1 && arg[i]) {
            out[i] = arg[i];
            i++;
        }
        out[i] = '\0';
        return;
    }

    /* Relative to cwd. */
    size_t i = 0;
    while (i < max - 1 && cwd[i]) {
        out[i] = cwd[i];
        i++;
    }
    if (i > 0 && out[i - 1] != ':' && out[i - 1] != '/' && i < max - 1)
        out[i++] = '/';
    while (i < max - 1 && *arg)
        out[i++] = *arg++;
    out[i] = '\0';
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

    task_write_output(buf, len);
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
            if (c >= 32 && c < 127 && i < max) {
                buf[i++] = c;
            }
        }
        /* Halt the CPU until the next interrupt.  With the trap-gate
         * IDT entry for vector 0x80, interrupts remain enabled during
         * syscalls, so the timer ISR (100 Hz) fires here, calls
         * Task_ScheduleFromIRQ(), and switches to other tasks (shell,
         * idle/desktop, network poll).  When this task is scheduled
         * again, it resumes from the hlt and re-checks for input.
         *
         * We must NOT call Task_ScheduleFromSyscall() here: it changes
         * g_current without performing the actual RSP switch (that
         * only happens in the ISR epilogue), which would corrupt the
         * scheduler state if called in a loop. */
        __asm__ volatile ("hlt");
    }
}

static int sys_open(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    const char *path = (const char *)(uintptr_t)rdi;
    int flags = (int)rsi;
    (void)rdx;

    UaosTask *t = Task_Current();
    char abs_path[UAOS_PATH_MAX];
    make_abs_path(t ? t->task_cwd : "", path, abs_path, sizeof(abs_path));

    int fd = fd_alloc();
    if (fd < 0)
        return -1;   /* EMFILE */

    if (!VFS_Open(&g_fd_table[fd], abs_path, flags)) {
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

    /* Flush any buffered stdout before terminating. */
    task_flush_all(Task_Current());

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
    UaosTask *parent = Task_Current();
    char abs_path[UAOS_PATH_MAX];
    make_abs_path(parent ? parent->task_cwd : "", path, abs_path, sizeof(abs_path));

    VfsFile fh;
    if (!VFS_Open(&fh, abs_path, VFS_READ))
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

    const char *cwd = "";
    void (*print_fn)(void *, const char *) = NULL;
    void *print_ctx = NULL;
    if (parent) {
        cwd = parent->task_cwd;
        print_fn = parent->native_print_fn;
        print_ctx = parent->native_print_ctx;
    }

    UaosTask *child = Task_CreateX64(path, 0, res.entry_rip, res.initial_rsp,
                                     cwd, print_fn, print_ctx);
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

static int sys_getcwd(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    char *buf = (char *)(uintptr_t)rdi;
    size_t max = (size_t)rsi;
    (void)rdx;

    if (!buf || max == 0)
        return 0;

    UaosTask *t = Task_Current();
    const char *cwd = t ? t->task_cwd : "";

    size_t i = 0;
    while (i < max - 1 && cwd[i]) {
        buf[i] = cwd[i];
        i++;
    }
    buf[i] = '\0';
    return (int)i;
}

/* Userspace-visible directory entry (matches uaos_syscall.h). */
struct UaosUserDirent {
    char     name[32];
    uint32_t size;
    uint8_t  is_dir;
    uint8_t  attrs;
    uint16_t protection;
    uint32_t mtime;
};

/* Userspace-visible stat result (matches uaos_syscall.h). */
struct UaosUserStat {
    uint32_t size;
    uint8_t  is_dir;
    uint8_t  attrs;
    uint16_t protection;
    uint32_t mtime;
};

static int sys_opendir(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    const char *path = (const char *)(uintptr_t)rdi;
    (void)rsi; (void)rdx;

    UaosTask *t = Task_Current();
    char abs_path[UAOS_PATH_MAX];
    make_abs_path(t ? t->task_cwd : "", path, abs_path, sizeof(abs_path));

    RamFsNode *dir = VFS_ResolveDir(abs_path);
    if (!dir)
        return -1;

    int dd = dir_alloc();
    if (dd < 0)
        return -1;

    g_dir_table[dd] = dir->first_child;
    return dd;
}

static int sys_readdir(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    int dd = (int)rdi;
    struct UaosUserDirent *ent = (struct UaosUserDirent *)(uintptr_t)rsi;
    (void)rdx;

    if (dd < 0 || dd >= MAX_DIR_FD || !g_dir_used[dd] || !ent)
        return -1;

    RamFsNode *node = g_dir_table[dd];
    if (!node)
        return 0;   /* end of directory */

    size_t n = 0;
    while (n < sizeof(ent->name) - 1 && node->name[n]) {
        ent->name[n] = node->name[n];
        n++;
    }
    ent->name[n] = '\0';
    ent->size    = node->size;
    ent->is_dir  = (node->type == RAMFS_TYPE_DIR) ? 1 : 0;
    ent->attrs   = node->attrs;
    ent->protection = node->protection;
    ent->mtime   = node->mtime;

    g_dir_table[dd] = node->next_sibling;
    return 1;
}

static int sys_closedir(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    int dd = (int)rdi;
    (void)rsi; (void)rdx;

    if (dd < 0 || dd >= MAX_DIR_FD || !g_dir_used[dd])
        return -1;

    dir_free(dd);
    return 0;
}

static int sys_stat(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    const char *path = (const char *)(uintptr_t)rdi;
    struct UaosUserStat *st = (struct UaosUserStat *)(uintptr_t)rsi;
    (void)rdx;

    if (!st)
        return -1;

    UaosTask *t = Task_Current();
    char abs_path[UAOS_PATH_MAX];
    make_abs_path(t ? t->task_cwd : "", path, abs_path, sizeof(abs_path));

    /* Directories first — VFS_Open may refuse them. */
    RamFsNode *dir = VFS_ResolveDir(abs_path);
    if (dir) {
        st->size    = 0;
        st->is_dir  = 1;
        st->attrs   = dir->attrs;
        st->protection = dir->protection;
        st->mtime   = dir->mtime;
        return 0;
    }

    /* Try as a regular file. */
    VfsFile fh;
    if (!VFS_Open(&fh, abs_path, VFS_READ))
        return -1;

    st->size    = VFS_Size(&fh);
    st->is_dir  = 0;
    st->attrs   = 0;
    st->protection = 0;
    st->mtime   = 0;
    VFS_Close(&fh);
    return 0;
}

/* -------------------------------------------------------------------------
 * Filesystem mutation / metadata syscalls
 * ------------------------------------------------------------------------- */
static int sys_mkdir(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    const char *path = (const char *)(uintptr_t)rdi;
    (void)rsi; (void)rdx;

    UaosTask *t = Task_Current();
    char abs_path[UAOS_PATH_MAX];
    make_abs_path(t ? t->task_cwd : "", path, abs_path, sizeof(abs_path));

    return VFS_MkDir(abs_path);
}

static int sys_delete(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    const char *path = (const char *)(uintptr_t)rdi;
    (void)rsi; (void)rdx;

    UaosTask *t = Task_Current();
    char abs_path[UAOS_PATH_MAX];
    make_abs_path(t ? t->task_cwd : "", path, abs_path, sizeof(abs_path));

    return VFS_Delete(abs_path);
}

static int sys_rename(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    const char *oldp = (const char *)(uintptr_t)rdi;
    const char *newp = (const char *)(uintptr_t)rsi;
    (void)rdx;

    UaosTask *t = Task_Current();
    const char *cwd = t ? t->task_cwd : "";
    char abs_old[UAOS_PATH_MAX], abs_new[UAOS_PATH_MAX];
    make_abs_path(cwd, oldp, abs_old, sizeof(abs_old));
    make_abs_path(cwd, newp, abs_new, sizeof(abs_new));

    return VFS_Rename(abs_old, abs_new);
}

static int sys_setprotection(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    const char *path = (const char *)(uintptr_t)rdi;
    uint16_t prot = (uint16_t)rsi;
    (void)rdx;

    UaosTask *t = Task_Current();
    char abs_path[UAOS_PATH_MAX];
    make_abs_path(t ? t->task_cwd : "", path, abs_path, sizeof(abs_path));

    return VFS_SetProtection(abs_path, prot);
}

static int sys_getprotection(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    const char *path = (const char *)(uintptr_t)rdi;
    (void)rsi; (void)rdx;

    UaosTask *t = Task_Current();
    char abs_path[UAOS_PATH_MAX];
    make_abs_path(t ? t->task_cwd : "", path, abs_path, sizeof(abs_path));

    return (int)VFS_GetProtection(abs_path);
}

static int sys_getcomment(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    const char *path = (const char *)(uintptr_t)rdi;
    char *buf = (char *)(uintptr_t)rsi;
    int max = (int)rdx;

    if (!buf || max <= 0)
        return -1;

    UaosTask *t = Task_Current();
    char abs_path[UAOS_PATH_MAX];
    make_abs_path(t ? t->task_cwd : "", path, abs_path, sizeof(abs_path));

    return VFS_GetComment(abs_path, buf, max);
}

static int sys_setcomment(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    const char *path = (const char *)(uintptr_t)rdi;
    const char *comment = (const char *)(uintptr_t)rsi;
    (void)rdx;

    UaosTask *t = Task_Current();
    char abs_path[UAOS_PATH_MAX];
    make_abs_path(t ? t->task_cwd : "", path, abs_path, sizeof(abs_path));

    return VFS_SetComment(abs_path, comment);
}

static int sys_getvolumeinfo(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    const char *path = (const char *)(uintptr_t)rdi;
    uint32_t *total = (uint32_t *)(uintptr_t)rsi;
    uint32_t *used  = (uint32_t *)(uintptr_t)rdx;

    if (!total || !used)
        return -1;

    UaosTask *t = Task_Current();
    char abs_path[UAOS_PATH_MAX];
    make_abs_path(t ? t->task_cwd : "", path, abs_path, sizeof(abs_path));

    return VFS_GetVolumeInfo(abs_path, total, used);
}

static int sys_readkey(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    (void)rdi; (void)rsi; (void)rdx;

    /* Block until a key is available.  With the trap-gate IDT entry
     * for vector 0x80, interrupts remain enabled during syscalls, so
     * hlt lets the timer ISR fire and switch to other tasks. */
    for (;;) {
        if (PS2Kbd_HasChar())
            return (int)(unsigned char)PS2Kbd_GetChar();
        __asm__ volatile ("hlt");
    }
}

static int sys_getattrs(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    const char *path = (const char *)(uintptr_t)rdi;
    (void)rsi; (void)rdx;

    UaosTask *t = Task_Current();
    char abs_path[UAOS_PATH_MAX];
    make_abs_path(t ? t->task_cwd : "", path, abs_path, sizeof(abs_path));

    return (int)VFS_GetAttrs(abs_path);
}

static int sys_setattrs(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    const char *path = (const char *)(uintptr_t)rdi;
    uint8_t attrs = (uint8_t)rsi;
    (void)rdx;

    UaosTask *t = Task_Current();
    char abs_path[UAOS_PATH_MAX];
    make_abs_path(t ? t->task_cwd : "", path, abs_path, sizeof(abs_path));

    return VFS_SetAttrs(abs_path, attrs);
}

static int sys_getmountcount(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    (void)rdi; (void)rsi; (void)rdx;
    return VFS_GetMountCount();
}

static int sys_getmountname(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    int idx = (int)rdi;
    char *buf = (char *)(uintptr_t)rsi;
    int max = (int)rdx;

    if (!buf || max <= 0)
        return 0;

    return VFS_GetMountName(idx, buf, max);
}

/* Userspace-visible memory info (matches struct uaos_meminfo in
 * system/libuaos/uaos_syscall.h and struct UaosMemInfo in mem_info.h). */
struct UaosMemInfoUser {
    uint32_t x64_total;
    uint32_t x64_used;
    uint32_t x64_free;
    uint32_t m68k_ram_total;
    uint16_t m68k_slots_total;
    uint16_t m68k_slots_used;
    uint16_t tasks_total;
    uint16_t tasks_running;
    uint16_t tasks_waiting;
    uint16_t reserved;
};

static int sys_meminfo(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    (void)rsi; (void)rdx;
    struct UaosMemInfoUser *out = (struct UaosMemInfoUser *)(uintptr_t)rdi;
    if (!out)
        return -1;

    struct UaosMemInfo info;
    Mem_GetInfo(&info);
    out->x64_total        = info.x64_total;
    out->x64_used         = info.x64_used;
    out->x64_free         = info.x64_free;
    out->m68k_ram_total   = info.m68k_ram_total;
    out->m68k_slots_total = info.m68k_slots_total;
    out->m68k_slots_used  = info.m68k_slots_used;
    out->tasks_total      = info.tasks_total;
    out->tasks_running    = info.tasks_running;
    out->tasks_waiting    = info.tasks_waiting;
    out->reserved         = 0;
    return 0;
}

/* -------------------------------------------------------------------------
 * GUI / user-window syscalls
 * ------------------------------------------------------------------------- */
static int16_t unpack_i16(uint64_t v, int shift)
{
    uint16_t u = (uint16_t)(v >> shift);
    if (u & 0x8000)
        return (int16_t)(u | 0xFFFF0000);
    return (int16_t)u;
}

static int sys_gui_create_window(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    (void)rdx;
    const char *title = (const char *)(uintptr_t)rdi;
    int x = unpack_i16(rsi, 0);
    int y = unpack_i16(rsi, 16);
    int w = unpack_i16(rsi, 32);
    int h = unpack_i16(rsi, 48);
    return UserWindow_Create(title, x, y, w, h);
}

static int sys_gui_destroy_window(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    (void)rsi; (void)rdx;
    return UserWindow_Destroy((int)rdi);
}

static int sys_gui_set_scroll_info(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    (void)rdx;
    int handle = (int)rdi;
    int cw = unpack_i16(rsi, 0);
    int ch = unpack_i16(rsi, 16);
    return UserWindow_SetScrollInfo(handle, cw, ch);
}

static int sys_gui_set_scroll(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    (void)rdx;
    int handle = (int)rdi;
    int sx = unpack_i16(rsi, 0);
    int sy = unpack_i16(rsi, 16);
    return UserWindow_SetScroll(handle, sx, sy);
}

static int sys_gui_draw_text(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    int handle = (int)rdi;
    const char *text = (const char *)(uintptr_t)rsi;
    int x = unpack_i16(rdx, 0);
    int y = unpack_i16(rdx, 16);
    uint32_t color = (uint32_t)(rdx >> 32);
    return UserWindow_DrawText(handle, x, y, text, color);
}

static int sys_gui_draw_rect(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    int handle = (int)rdi;
    int x = unpack_i16(rsi, 0);
    int y = unpack_i16(rsi, 16);
    int w = unpack_i16(rsi, 32);
    int h = unpack_i16(rsi, 48);
    uint32_t color = (uint32_t)rdx;
    return UserWindow_DrawRect(handle, x, y, w, h, color);
}

static int sys_gui_present(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    (void)rsi; (void)rdx;
    return UserWindow_Present((int)rdi);
}

static int sys_gui_get_event(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    (void)rdx;
    int handle = (int)rdi;
    struct uaos_gui_event *ev = (struct uaos_gui_event *)(uintptr_t)rsi;
    return UserWindow_GetEvent(handle, ev);
}

/* Extended GUI drawing syscalls */
static int sys_gui_draw_line(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    int handle = (int)rdi;
    int x1 = unpack_i16(rsi, 0);
    int y1 = unpack_i16(rsi, 16);
    int x2 = unpack_i16(rsi, 32);
    int y2 = unpack_i16(rsi, 48);
    uint32_t color = (uint32_t)rdx;
    return UserWindow_DrawLine(handle, x1, y1, x2, y2, color);
}

static int sys_gui_fill_rect(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    int handle = (int)rdi;
    int x = unpack_i16(rsi, 0);
    int y = unpack_i16(rsi, 16);
    int w = unpack_i16(rsi, 32);
    int h = unpack_i16(rsi, 48);
    uint32_t color = (uint32_t)rdx;
    return UserWindow_FillRect(handle, x, y, w, h, color);
}

static int sys_gui_draw_3d_border(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    int handle = (int)rdi;
    int x = unpack_i16(rsi, 0);
    int y = unpack_i16(rsi, 16);
    int w = unpack_i16(rsi, 32);
    int h = unpack_i16(rsi, 48);
    int raised = (int)(rdx & 0xFF);
    uint32_t base_color = (uint32_t)(rdx >> 8);
    return UserWindow_Draw3DBorder(handle, x, y, w, h, raised, base_color);
}

static int sys_gui_draw_pixel(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    (void)rdx;
    int handle = (int)rdi;
    int x = unpack_i16(rsi, 0);
    int y = unpack_i16(rsi, 16);
    uint32_t color = (uint32_t)(rsi >> 32);
    return UserWindow_DrawPixel(handle, x, y, color);
}

static int sys_gui_draw_text_bg(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    int handle = (int)rdi;
    const char *text = (const char *)(uintptr_t)rsi;
    int x = unpack_i16(rdx, 0);
    int y = unpack_i16(rdx, 16);
    uint32_t fg = (uint32_t)(rdx >> 32);
    /* bg is passed via a second call — we pack fg in high 32, bg in low 32
     * of rdx after x/y. But we only have 3 args. Use rdx bits 32-63 for fg
     * and pack bg into rsi high bits. Actually, let's use a simpler approach:
     * pack x/y in rsi low, fg in rsi high, text in rdx, bg in a separate
     * encoding. For now, use bg = white as default. */
    uint32_t bg = 0xFFFFFF;
    return UserWindow_DrawTextBg(handle, x, y, text, fg, bg);
}

static int sys_gui_get_winsize(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    (void)rdx;
    int handle = (int)rdi;
    int *w = (int *)(uintptr_t)rsi;
    int *h = (int *)(uintptr_t)(rsi + 4);
    return UserWindow_GetWinSize(handle, w, h);
}

static int sys_gui_set_title(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    (void)rdx;
    int handle = (int)rdi;
    const char *title = (const char *)(uintptr_t)rsi;
    return UserWindow_SetTitle(handle, title);
}

static int sys_gui_draw_ellipse(uint64_t rdi, uint64_t rsi, uint64_t rdx)
{
    int handle = (int)rdi;
    int cx = unpack_i16(rsi, 0);
    int cy = unpack_i16(rsi, 16);
    int rx = unpack_i16(rsi, 32);
    int ry = unpack_i16(rsi, 48);
    uint32_t color = (uint32_t)rdx;
    return UserWindow_DrawEllipse(handle, cx, cy, rx, ry, color);
}

/* -------------------------------------------------------------------------
 * Dispatcher
 * ------------------------------------------------------------------------- */
void Syscall_Dispatch(SyscallRegs *regs, InterruptFrame *frame)
{
    (void)frame;

    uint64_t n = regs->rax;
    uint64_t rdi = regs->rdi;
    uint64_t rsi = regs->rsi;
    uint64_t rdx = regs->rdx;
    int64_t ret;

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
    case SYSCALL_GETCWD:   ret = sys_getcwd(rdi, rsi, rdx); break;
    case SYSCALL_OPENDIR:  ret = sys_opendir(rdi, rsi, rdx); break;
    case SYSCALL_READDIR:  ret = sys_readdir(rdi, rsi, rdx); break;
    case SYSCALL_CLOSEDIR: ret = sys_closedir(rdi, rsi, rdx); break;
    case SYSCALL_STAT:     ret = sys_stat(rdi, rsi, rdx); break;
    case SYSCALL_GUI_CREATE_WINDOW:  ret = sys_gui_create_window(rdi, rsi, rdx); break;
    case SYSCALL_GUI_DESTROY_WINDOW: ret = sys_gui_destroy_window(rdi, rsi, rdx); break;
    case SYSCALL_GUI_SET_SCROLL_INFO: ret = sys_gui_set_scroll_info(rdi, rsi, rdx); break;
    case SYSCALL_GUI_SET_SCROLL:     ret = sys_gui_set_scroll(rdi, rsi, rdx); break;
    case SYSCALL_GUI_DRAW_TEXT:      ret = sys_gui_draw_text(rdi, rsi, rdx); break;
    case SYSCALL_GUI_DRAW_RECT:      ret = sys_gui_draw_rect(rdi, rsi, rdx); break;
    case SYSCALL_GUI_PRESENT:        ret = sys_gui_present(rdi, rsi, rdx); break;
    case SYSCALL_GUI_GET_EVENT:      ret = sys_gui_get_event(rdi, rsi, rdx); break;
    case SYSCALL_GUI_DRAW_LINE:      ret = sys_gui_draw_line(rdi, rsi, rdx); break;
    case SYSCALL_GUI_FILL_RECT:      ret = sys_gui_fill_rect(rdi, rsi, rdx); break;
    case SYSCALL_GUI_DRAW_3DBORDER:  ret = sys_gui_draw_3d_border(rdi, rsi, rdx); break;
    case SYSCALL_GUI_DRAW_PIXEL:     ret = sys_gui_draw_pixel(rdi, rsi, rdx); break;
    case SYSCALL_GUI_DRAW_TEXT_BG:   ret = sys_gui_draw_text_bg(rdi, rsi, rdx); break;
    case SYSCALL_GUI_GET_WINSIZE:    ret = sys_gui_get_winsize(rdi, rsi, rdx); break;
    case SYSCALL_GUI_SET_TITLE:      ret = sys_gui_set_title(rdi, rsi, rdx); break;
    case SYSCALL_GUI_DRAW_ELLIPSE:   ret = sys_gui_draw_ellipse(rdi, rsi, rdx); break;
    case SYSCALL_MKDIR:          ret = sys_mkdir(rdi, rsi, rdx); break;
    case SYSCALL_DELETE:         ret = sys_delete(rdi, rsi, rdx); break;
    case SYSCALL_RENAME:         ret = sys_rename(rdi, rsi, rdx); break;
    case SYSCALL_SETPROTECTION:  ret = sys_setprotection(rdi, rsi, rdx); break;
    case SYSCALL_GETPROTECTION:  ret = sys_getprotection(rdi, rsi, rdx); break;
    case SYSCALL_GETCOMMENT:     ret = sys_getcomment(rdi, rsi, rdx); break;
    case SYSCALL_SETCOMMENT:     ret = sys_setcomment(rdi, rsi, rdx); break;
    case SYSCALL_GETVOLUMEINFO:  ret = sys_getvolumeinfo(rdi, rsi, rdx); break;
    case SYSCALL_READKEY:        ret = sys_readkey(rdi, rsi, rdx); break;
    case SYSCALL_GETATTRS:       ret = sys_getattrs(rdi, rsi, rdx); break;
    case SYSCALL_SETATTRS:       ret = sys_setattrs(rdi, rsi, rdx); break;
    case SYSCALL_GETMOUNTCOUNT:  ret = sys_getmountcount(rdi, rsi, rdx); break;
    case SYSCALL_GETMOUNTNAME:   ret = sys_getmountname(rdi, rsi, rdx); break;
    case SYSCALL_MEMINFO:        ret = sys_meminfo(rdi, rsi, rdx); break;
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
