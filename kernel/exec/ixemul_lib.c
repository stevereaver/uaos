/*
 * ixemul_lib.c — UAOS ixemul.library Implementation
 *
 * AmigaOS ixemul.library is a Unix compatibility layer that provides
 * POSIX-style functions for Unix ports to Amiga. This is a basic stub
 * implementation for UAOS to provide compatibility.
 */

#include "rom_modules.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

/* =========================================================================
 * ixemul.library function indices (must match AmigaOS LVO offsets)
 * Note: ixemul has a very large API - this is a subset of common functions
 * ========================================================================= */

#define IXEMUL_OPEN_LIBRARY   1
#define IXEMUL_CLOSE_LIBRARY  2
#define IXEMUL_OPEN           3
#define IXEMUL_CLOSE          4
#define IXEMUL_READ           5
#define IXEMUL_WRITE          6
#define IXEMUL_LSEEK          7
#define IXEMUL_IOCTL          8
#define IXEMUL_FSTAT          9
#define IXEMUL_STAT           10
#define IXEMUL_ACCESS         11
#define IXEMUL_UNLINK         12
#define IXEMUL_RENAME         13
#define IXEMUL_MKDIR          14
#define IXEMUL_RMDIR          15
#define IXEMUL_OPENDIR        16
#define IXEMUL_CLOSEDIR       17
#define IXEMUL_READDIR        18
#define IXEMUL_CHDIR          19
#define IXEMUL_GETCWD         20
#define IXEMUL_MALLOC         21
#define IXEMUL_FREE           22
#define IXEMUL_CALLOC         23
#define IXEMUL_REALLOC        24
#define IXEMUL_STRDUP         25
#define IXEMUL_GETENV         26
#define IXEMUL_SETENV         27
#define IXEMUL_PUTENV         28
#define IXEMUL_EXIT           29
#define IXEMUL__EXIT          30
#define IXEMUL_ABORT          31
#define IXEMUL_FORK           32
#define IXEMUL_EXECVE         33
#define IXEMUL_SYSTEM         34
#define IXEMUL_WAIT           35
#define IXEMUL_WAITPID        36
#define IXEMUL_KILL           37
#define IXEMUL_SIGNAL         38
#define IXEMUL_PIPE           39
#define IXEMUL_DUP            40
#define IXEMUL_DUP2           41
#define IXEMUL_FCNTL          42
#define IXEMUL_ISATTY         43
#define IXEMUL_FILENO         44
#define IXEMUL_STUB_45        45
#define IXEMUL_STUB_46        46
#define IXEMUL_GETTIMEOFDAY   47
#define IXEMUL_GETPID         48
#define IXEMUL_GETUID         49
#define IXEMUL_GETGID         50

/* =========================================================================
 * Stub implementations
 * ========================================================================= */

static void ixemul_OpenLibrary(void)
{
    /* OpenLibrary - return library base */
    fprintf(stderr, "[IXEMUL] OpenLibrary called\n");
}

static void ixemul_CloseLibrary(void)
{
    /* CloseLibrary - no-op for ROM library */
    fprintf(stderr, "[IXEMUL] CloseLibrary called\n");
}

static void ixemul_open(void)
{
    /* open - open file (Unix style) */
    fprintf(stderr, "[IXEMUL] open called\n");
}

static void ixemul_close(void)
{
    /* close - close file descriptor */
    fprintf(stderr, "[IXEMUL] close called\n");
}

static void ixemul_read(void)
{
    /* read - read from file descriptor */
    fprintf(stderr, "[IXEMUL] read called\n");
}

static void ixemul_write(void)
{
    /* write - write to file descriptor */
    fprintf(stderr, "[IXEMUL] write called\n");
}

static void ixemul_lseek(void)
{
    /* lseek - seek in file */
    fprintf(stderr, "[IXEMUL] lseek called\n");
}

static void ixemul_ioctl(void)
{
    /* ioctl - I/O control */
    fprintf(stderr, "[IXEMUL] ioctl called\n");
}

static void ixemul_fstat(void)
{
    /* fstat - get file status */
    fprintf(stderr, "[IXEMUL] fstat called\n");
}

static void ixemul_stat(void)
{
    /* stat - get file status by path */
    fprintf(stderr, "[IXEMUL] stat called\n");
}

static void ixemul_access(void)
{
    /* access - check file accessibility */
    fprintf(stderr, "[IXEMUL] access called\n");
}

static void ixemul_unlink(void)
{
    /* unlink - delete file */
    fprintf(stderr, "[IXEMUL] unlink called\n");
}

static void ixemul_rename(void)
{
    /* rename - rename file */
    fprintf(stderr, "[IXEMUL] rename called\n");
}

static void ixemul_mkdir(void)
{
    /* mkdir - create directory */
    fprintf(stderr, "[IXEMUL] mkdir called\n");
}

static void ixemul_rmdir(void)
{
    /* rmdir - remove directory */
    fprintf(stderr, "[IXEMUL] rmdir called\n");
}

static void ixemul_opendir(void)
{
    /* opendir - open directory */
    fprintf(stderr, "[IXEMUL] opendir called\n");
}

static void ixemul_closedir(void)
{
    /* closedir - close directory */
    fprintf(stderr, "[IXEMUL] closedir called\n");
}

static void ixemul_readdir(void)
{
    /* readdir - read directory entry */
    fprintf(stderr, "[IXEMUL] readdir called\n");
}

static void ixemul_chdir(void)
{
    /* chdir - change current directory */
    fprintf(stderr, "[IXEMUL] chdir called\n");
}

static void ixemul_getcwd(void)
{
    /* getcwd - get current working directory */
    fprintf(stderr, "[IXEMUL] getcwd called\n");
}

static void ixemul_malloc(void)
{
    /* malloc - allocate memory */
    fprintf(stderr, "[IXEMUL] malloc called\n");
}

static void ixemul_free(void)
{
    /* free - free memory */
    fprintf(stderr, "[IXEMUL] free called\n");
}

static void ixemul_calloc(void)
{
    /* calloc - allocate and zero memory */
    fprintf(stderr, "[IXEMUL] calloc called\n");
}

static void ixemul_realloc(void)
{
    /* realloc - reallocate memory */
    fprintf(stderr, "[IXEMUL] realloc called\n");
}

static void ixemul_strdup(void)
{
    /* strdup - duplicate string */
    fprintf(stderr, "[IXEMUL] strdup called\n");
}

static void ixemul_getenv(void)
{
    /* getenv - get environment variable */
    fprintf(stderr, "[IXEMUL] getenv called\n");
}

static void ixemul_setenv(void)
{
    /* setenv - set environment variable */
    fprintf(stderr, "[IXEMUL] setenv called\n");
}

static void ixemul_putenv(void)
{
    /* putenv - put environment variable */
    fprintf(stderr, "[IXEMUL] putenv called\n");
}

static void ixemul_exit(void)
{
    /* exit - exit process */
    fprintf(stderr, "[IXEMUL] exit called\n");
}

static void ixemul__exit(void)
{
    /* _exit - exit without cleanup */
    fprintf(stderr, "[IXEMUL] _exit called\n");
}

static void ixemul_abort(void)
{
    /* abort - abort process */
    fprintf(stderr, "[IXEMUL] abort called\n");
}

static void ixemul_fork(void)
{
    /* fork - create child process */
    fprintf(stderr, "[IXEMUL] fork called\n");
}

static void ixemul_execve(void)
{
    /* execve - execute program */
    fprintf(stderr, "[IXEMUL] execve called\n");
}

static void ixemul_system(void)
{
    /* system - execute shell command */
    fprintf(stderr, "[IXEMUL] system called\n");
}

static void ixemul_wait(void)
{
    /* wait - wait for child process */
    fprintf(stderr, "[IXEMUL] wait called\n");
}

static void ixemul_waitpid(void)
{
    /* waitpid - wait for specific child process */
    fprintf(stderr, "[IXEMUL] waitpid called\n");
}

static void ixemul_kill(void)
{
    /* kill - send signal to process */
    fprintf(stderr, "[IXEMUL] kill called\n");
}

static void ixemul_signal(void)
{
    /* signal - set signal handler */
    fprintf(stderr, "[IXEMUL] signal called\n");
}

static void ixemul_pipe(void)
{
    /* pipe - create pipe */
    fprintf(stderr, "[IXEMUL] pipe called\n");
}

static void ixemul_dup(void)
{
    /* dup - duplicate file descriptor */
    fprintf(stderr, "[IXEMUL] dup called\n");
}

static void ixemul_dup2(void)
{
    /* dup2 - duplicate file descriptor to specific fd */
    fprintf(stderr, "[IXEMUL] dup2 called\n");
}

static void ixemul_fcntl(void)
{
    /* fcntl - file control operations */
    fprintf(stderr, "[IXEMUL] fcntl called\n");
}

static void ixemul_isatty(void)
{
    /* isatty - test if file descriptor is a terminal */
    fprintf(stderr, "[IXEMUL] isatty called\n");
}

/* =========================================================================
 * Function table
 * ========================================================================= */

static void *ixemul_funcs[] = {
    ixemul_OpenLibrary,   /* index 1  */
    ixemul_CloseLibrary,  /* index 2  */
    ixemul_open,           /* index 3  */
    ixemul_close,          /* index 4  */
    ixemul_read,           /* index 5  */
    ixemul_write,          /* index 6  */
    ixemul_lseek,          /* index 7  */
    ixemul_ioctl,          /* index 8  */
    ixemul_fstat,          /* index 9  */
    ixemul_stat,           /* index 10 */
    ixemul_access,         /* index 11 */
    ixemul_unlink,         /* index 12 */
    ixemul_rename,         /* index 13 */
    ixemul_mkdir,          /* index 14 */
    ixemul_rmdir,          /* index 15 */
    ixemul_opendir,        /* index 16 */
    ixemul_closedir,       /* index 17 */
    ixemul_readdir,        /* index 18 */
    ixemul_chdir,          /* index 19 */
    ixemul_getcwd,         /* index 20 */
    ixemul_malloc,         /* index 21 */
    ixemul_free,           /* index 22 */
    ixemul_calloc,         /* index 23 */
    ixemul_realloc,        /* index 24 */
    ixemul_strdup,         /* index 25 */
    ixemul_getenv,         /* index 26 */
    ixemul_setenv,         /* index 27 */
    ixemul_putenv,         /* index 28 */
    ixemul_exit,           /* index 29 */
    ixemul__exit,          /* index 30 */
    ixemul_abort,          /* index 31 */
    ixemul_fork,           /* index 32 */
    ixemul_execve,         /* index 33 */
    ixemul_system,         /* index 34 */
    ixemul_wait,           /* index 35 */
    ixemul_waitpid,        /* index 36 */
    ixemul_kill,           /* index 37 */
    ixemul_signal,         /* index 38 */
    ixemul_pipe,           /* index 39 */
    ixemul_dup,            /* index 40 */
    ixemul_dup2,           /* index 41 */
    ixemul_fcntl,          /* index 42 */
    ixemul_isatty,         /* index 43 */
};

/* =========================================================================
 * Registration function
 * ========================================================================= */

void UAOS_IXEMUL_Register(void)
{
    UAOS_ROM_Register("ixemul.library", 53, 0x00000090,
                      (uint16_t)(sizeof(ixemul_funcs) / sizeof(ixemul_funcs[0])),
                      ixemul_funcs);
}
