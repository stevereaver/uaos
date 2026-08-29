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
#define UAOS_SYSCALL_GETCWD      0x0C
#define UAOS_SYSCALL_OPENDIR     0x0D
#define UAOS_SYSCALL_READDIR     0x0E
#define UAOS_SYSCALL_CLOSEDIR    0x0F
#define UAOS_SYSCALL_STAT        0x10

/* VFS open flags (must stay in sync with kernel/dos/vfs.h) */
#define UAOS_O_RDONLY 1
#define UAOS_O_WRONLY 2
#define UAOS_O_RDWR   3
#define UAOS_O_CREAT  4
#define UAOS_O_TRUNC  8

/* GUI / windowing syscalls (must stay in sync with kernel/exec/syscall_table.h) */
#define UAOS_SYSCALL_GUI_CREATE_WINDOW  0x11
#define UAOS_SYSCALL_GUI_DESTROY_WINDOW 0x12
#define UAOS_SYSCALL_GUI_SET_SCROLL_INFO 0x13
#define UAOS_SYSCALL_GUI_SET_SCROLL     0x14
#define UAOS_SYSCALL_GUI_DRAW_TEXT      0x15
#define UAOS_SYSCALL_GUI_DRAW_RECT      0x16
#define UAOS_SYSCALL_GUI_PRESENT        0x17
#define UAOS_SYSCALL_GUI_GET_EVENT      0x18

/* Extended GUI drawing syscalls */
#define UAOS_SYSCALL_GUI_DRAW_LINE      0x30
#define UAOS_SYSCALL_GUI_FILL_RECT      0x31
#define UAOS_SYSCALL_GUI_DRAW_3DBORDER  0x32
#define UAOS_SYSCALL_GUI_DRAW_PIXEL     0x33
#define UAOS_SYSCALL_GUI_DRAW_TEXT_BG   0x34
#define UAOS_SYSCALL_GUI_GET_WINSIZE    0x35
#define UAOS_SYSCALL_GUI_SET_TITLE      0x36
#define UAOS_SYSCALL_GUI_DRAW_ELLIPSE   0x37

/* Filesystem mutation / metadata syscalls (must stay in sync with
 * kernel/exec/syscall_table.h) */
#define UAOS_SYSCALL_MKDIR          0x20
#define UAOS_SYSCALL_DELETE         0x21
#define UAOS_SYSCALL_RENAME         0x22
#define UAOS_SYSCALL_SETPROTECTION  0x23
#define UAOS_SYSCALL_GETPROTECTION  0x24
#define UAOS_SYSCALL_GETCOMMENT     0x25
#define UAOS_SYSCALL_SETCOMMENT     0x26
#define UAOS_SYSCALL_GETVOLUMEINFO  0x27
#define UAOS_SYSCALL_READKEY        0x28
#define UAOS_SYSCALL_GETATTRS       0x29
#define UAOS_SYSCALL_SETATTRS       0x2A
#define UAOS_SYSCALL_GETMOUNTCOUNT  0x2B
#define UAOS_SYSCALL_GETMOUNTNAME   0x2C
#define UAOS_SYSCALL_MEMINFO        0x2D

#define UAOS_SYSCALL_SCHEDULE           0xFF

/* GUI event types (must stay in sync with kernel/display/user_window.h) */
#define UAOS_GUI_EVENT_NONE     0
#define UAOS_GUI_EVENT_KEY      1
#define UAOS_GUI_EVENT_CLICK    2
#define UAOS_GUI_EVENT_RELEASE  3
#define UAOS_GUI_EVENT_MOVE     4
#define UAOS_GUI_EVENT_SCROLL   5

/* Workbench palette */
#define UAOS_WB_GREY       0xAAAAAA
#define UAOS_WB_LIGHT_GREY 0xCCCCCC
#define UAOS_WB_DARK_GREY  0x555555
#define UAOS_WB_BLACK      0x000000
#define UAOS_WB_WHITE      0xFFFFFF
#define UAOS_WB_BLUE       0x0055AA
#define UAOS_WB_LIGHT_BLUE 0x0088FF
#define UAOS_WB_ORANGE     0xFF8800
#define UAOS_WB_CREAM      0xFFFFCC
#define UAOS_WB_RED        0xCC0000
#define UAOS_WB_GREEN      0x00AA00

struct uaos_gui_event {
    uint8_t  type;
    uint8_t  button;
    int16_t  x;
    int16_t  y;
};

/* -------------------------------------------------------------------------
 * Directory entry returned by uaos_readdir()
 * ------------------------------------------------------------------------- */
struct uaos_dirent {
    char     name[32];   /* entry name (NUL-terminated) */
    uint32_t size;       /* file size in bytes (0 for directories) */
    uint8_t  is_dir;     /* 1 = directory, 0 = file */
    uint8_t  attrs;      /* volume attribute flags */
    uint16_t protection; /* AmigaDOS FIBF_* protection bits */
    uint32_t mtime;      /* modification time (Unix epoch seconds) */
};

/* -------------------------------------------------------------------------
 * File status returned by uaos_stat()
 * ------------------------------------------------------------------------- */
struct uaos_stat {
    uint32_t size;       /* file size in bytes (0 for directories) */
    uint8_t  is_dir;     /* 1 = directory, 0 = file */
    uint8_t  attrs;      /* volume attribute flags */
    uint16_t protection; /* AmigaDOS FIBF_* protection bits */
    uint32_t mtime;      /* modification time (Unix epoch seconds) */
};

/* -------------------------------------------------------------------------
 * Memory statistics returned by uaos_meminfo()
 *
 * Layout must stay in sync with struct UaosMemInfo in
 * kernel/exec/mem_info.h.
 * ------------------------------------------------------------------------- */
struct uaos_meminfo {
    uint32_t x64_total;        /* x86-64 userspace heap total (bytes)  */
    uint32_t x64_used;         /* x86-64 userspace heap used  (bytes)  */
    uint32_t x64_free;         /* x64_total - x64_used                 */
    uint32_t m68k_ram_total;   /* one M68k guest RAM slot  (bytes)     */
    uint16_t m68k_slots_total; /* total M68k RAM slots                  */
    uint16_t m68k_slots_used;  /* M68k RAM slots in use                 */
    uint16_t tasks_total;      /* registered tasks                      */
    uint16_t tasks_running;    /* tasks in RUN/READY state              */
    uint16_t tasks_waiting;    /* tasks blocked on a signal             */
    uint16_t reserved;
};

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

static inline long uaos_yield(void)
{
    return uaos_syscall0(UAOS_SYSCALL_SCHEDULE);
}

static inline void *uaos_alloc(long size)
{
    return (void *)(uintptr_t)uaos_syscall1(UAOS_SYSCALL_ALLOC, size);
}

static inline long uaos_getcwd(char *buf, long max)
{
    return uaos_syscall2(UAOS_SYSCALL_GETCWD, (long)buf, max);
}

static inline long uaos_opendir(const char *path)
{
    return uaos_syscall1(UAOS_SYSCALL_OPENDIR, (long)path);
}

static inline long uaos_readdir(int dd, struct uaos_dirent *ent)
{
    return uaos_syscall2(UAOS_SYSCALL_READDIR, (long)dd, (long)ent);
}

static inline long uaos_closedir(int dd)
{
    return uaos_syscall1(UAOS_SYSCALL_CLOSEDIR, (long)dd);
}

static inline long uaos_stat(const char *path, struct uaos_stat *st)
{
    return uaos_syscall2(UAOS_SYSCALL_STAT, (long)path, (long)st);
}

/* -------------------------------------------------------------------------
 * Filesystem mutation / metadata syscall wrappers
 * ------------------------------------------------------------------------- */

/* AmigaDOS FIBF_* protection bits (must stay in sync with
 * kernel/dos/amiga_dos_types.h).  Note the inverted logic: a bit SET
 * means the corresponding action is DISALLOWED. */
#define UAOS_FIBF_SCRIPT     (1 << 0)
#define UAOS_FIBF_PURE       (1 << 1)
#define UAOS_FIBF_ARCHIVE    (1 << 2)
#define UAOS_FIBF_READ       (1 << 3)
#define UAOS_FIBF_WRITE      (1 << 4)
#define UAOS_FIBF_EXECUTE    (1 << 5)
#define UAOS_FIBF_DELETE     (1 << 6)
#define UAOS_FIBF_HOLD       (1 << 7)

/* RAMFS attribute flags (must stay in sync with kernel/dos/ramfs.h) */
#define UAOS_ATTR_READONLY   0x01
#define UAOS_ATTR_HIDDEN     0x02

static inline long uaos_mkdir(const char *path)
{
    return uaos_syscall1(UAOS_SYSCALL_MKDIR, (long)path);
}

static inline long uaos_delete(const char *path)
{
    return uaos_syscall1(UAOS_SYSCALL_DELETE, (long)path);
}

static inline long uaos_rename(const char *oldp, const char *newp)
{
    return uaos_syscall2(UAOS_SYSCALL_RENAME, (long)oldp, (long)newp);
}

static inline long uaos_setprotection(const char *path, uint16_t prot)
{
    return uaos_syscall2(UAOS_SYSCALL_SETPROTECTION, (long)path, (long)prot);
}

static inline long uaos_getprotection(const char *path)
{
    return uaos_syscall1(UAOS_SYSCALL_GETPROTECTION, (long)path);
}

static inline long uaos_getcomment(const char *path, char *buf, long max)
{
    return uaos_syscall3(UAOS_SYSCALL_GETCOMMENT, (long)path, (long)buf, max);
}

static inline long uaos_setcomment(const char *path, const char *comment)
{
    return uaos_syscall2(UAOS_SYSCALL_SETCOMMENT, (long)path, (long)comment);
}

static inline long uaos_getvolumeinfo(const char *path, uint32_t *total, uint32_t *used)
{
    return uaos_syscall3(UAOS_SYSCALL_GETVOLUMEINFO, (long)path, (long)total, (long)used);
}

static inline long uaos_readkey(void)
{
    return uaos_syscall0(UAOS_SYSCALL_READKEY);
}

static inline long uaos_getattrs(const char *path)
{
    return uaos_syscall1(UAOS_SYSCALL_GETATTRS, (long)path);
}

static inline long uaos_setattrs(const char *path, uint8_t attrs)
{
    return uaos_syscall2(UAOS_SYSCALL_SETATTRS, (long)path, (long)attrs);
}

static inline long uaos_getmountcount(void)
{
    return uaos_syscall0(UAOS_SYSCALL_GETMOUNTCOUNT);
}

static inline long uaos_getmountname(int idx, char *buf, long max)
{
    return uaos_syscall3(UAOS_SYSCALL_GETMOUNTNAME, (long)idx, (long)buf, max);
}

/* Query a snapshot of system memory statistics.  Returns 0 on success,
 * negative on error.  The kernel fills *info in place. */
static inline long uaos_meminfo(struct uaos_meminfo *info)
{
    return uaos_syscall1(UAOS_SYSCALL_MEMINFO, (long)info);
}

/* -------------------------------------------------------------------------
 * GUI / windowing syscall wrappers
 * ------------------------------------------------------------------------- */
#define UAOS_PACK_I16_4(x, y, w, h) \
    ( (((uint64_t)((uint16_t)(int16_t)(x)))      ) | \
      (((uint64_t)((uint16_t)(int16_t)(y))) << 16) | \
      (((uint64_t)((uint16_t)(int16_t)(w))) << 32) | \
      (((uint64_t)((uint16_t)(int16_t)(h))) << 48) )

#define UAOS_PACK_XY_COL(x, y, col) \
    ( (((uint64_t)((uint16_t)(int16_t)(x)))      ) | \
      (((uint64_t)((uint16_t)(int16_t)(y))) << 16) | \
      (((uint64_t)(uint32_t)(col)) << 32) )

static inline long uaos_gui_create_window(const char *title, int x, int y, int w, int h)
{
    uint64_t geo = UAOS_PACK_I16_4(x, y, w, h);
    return uaos_syscall2(UAOS_SYSCALL_GUI_CREATE_WINDOW, (long)title, (long)geo);
}

static inline long uaos_gui_destroy_window(int handle)
{
    return uaos_syscall1(UAOS_SYSCALL_GUI_DESTROY_WINDOW, (long)handle);
}

static inline long uaos_gui_set_scroll_info(int handle, int content_w, int content_h)
{
    uint64_t sz = UAOS_PACK_I16_4(content_w, content_h, 0, 0);
    return uaos_syscall2(UAOS_SYSCALL_GUI_SET_SCROLL_INFO, (long)handle, (long)sz);
}

static inline long uaos_gui_set_scroll(int handle, int scroll_x, int scroll_y)
{
    uint64_t sc = UAOS_PACK_I16_4(scroll_x, scroll_y, 0, 0);
    return uaos_syscall2(UAOS_SYSCALL_GUI_SET_SCROLL, (long)handle, (long)sc);
}

static inline long uaos_gui_draw_text(int handle, int x, int y, const char *text, uint32_t color)
{
    uint64_t args = UAOS_PACK_XY_COL(x, y, color);
    return uaos_syscall3(UAOS_SYSCALL_GUI_DRAW_TEXT, (long)handle, (long)text, (long)args);
}

static inline long uaos_gui_draw_rect(int handle, int x, int y, int w, int h, uint32_t color)
{
    uint64_t geo = UAOS_PACK_I16_4(x, y, w, h);
    return uaos_syscall3(UAOS_SYSCALL_GUI_DRAW_RECT, (long)handle, (long)geo, (long)color);
}

static inline long uaos_gui_present(int handle)
{
    return uaos_syscall1(UAOS_SYSCALL_GUI_PRESENT, (long)handle);
}

static inline long uaos_gui_get_event(int handle, struct uaos_gui_event *event)
{
    return uaos_syscall2(UAOS_SYSCALL_GUI_GET_EVENT, (long)handle, (long)event);
}

static inline long uaos_gui_draw_line(int handle, int x1, int y1, int x2, int y2, uint32_t color)
{
    uint64_t args = UAOS_PACK_I16_4(x1, y1, x2, y2);
    return uaos_syscall3(UAOS_SYSCALL_GUI_DRAW_LINE, (long)handle, (long)args, (long)color);
}

static inline long uaos_gui_fill_rect(int handle, int x, int y, int w, int h, uint32_t color)
{
    uint64_t geo = UAOS_PACK_I16_4(x, y, w, h);
    return uaos_syscall3(UAOS_SYSCALL_GUI_FILL_RECT, (long)handle, (long)geo, (long)color);
}

static inline long uaos_gui_draw_3d_border(int handle, int x, int y, int w, int h,
                                           int raised, uint32_t base_color)
{
    uint64_t geo = UAOS_PACK_I16_4(x, y, w, h);
    uint64_t extra = ((uint64_t)(uint32_t)base_color << 8) | (uint64_t)(uint8_t)raised;
    return uaos_syscall3(UAOS_SYSCALL_GUI_DRAW_3DBORDER, (long)handle, (long)geo, (long)extra);
}

static inline long uaos_gui_draw_pixel(int handle, int x, int y, uint32_t color)
{
    uint64_t args = UAOS_PACK_XY_COL(x, y, color);
    return uaos_syscall2(UAOS_SYSCALL_GUI_DRAW_PIXEL, (long)handle, (long)args);
}

static inline long uaos_gui_draw_text_bg(int handle, int x, int y,
                                         const char *text, uint32_t fg, uint32_t bg)
{
    uint64_t args = UAOS_PACK_XY_COL(x, y, fg);
    return uaos_syscall3(UAOS_SYSCALL_GUI_DRAW_TEXT_BG, (long)handle, (long)text, (long)args);
}

static inline long uaos_gui_get_winsize(int handle, int *w, int *h)
{
    struct { int w, h; } sz;
    long ret = uaos_syscall2(UAOS_SYSCALL_GUI_GET_WINSIZE, (long)handle, (long)&sz);
    if (ret == 0) { if (w) *w = sz.w; if (h) *h = sz.h; }
    return ret;
}

static inline long uaos_gui_set_title(int handle, const char *title)
{
    return uaos_syscall2(UAOS_SYSCALL_GUI_SET_TITLE, (long)handle, (long)title);
}

static inline long uaos_gui_draw_ellipse(int handle, int cx, int cy, int rx, int ry, uint32_t color)
{
    uint64_t args = UAOS_PACK_I16_4(cx, cy, rx, ry);
    return uaos_syscall3(UAOS_SYSCALL_GUI_DRAW_ELLIPSE, (long)handle, (long)args, (long)color);
}

#endif /* UAOS_SYSCALL_H */
