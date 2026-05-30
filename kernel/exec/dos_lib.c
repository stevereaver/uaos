/*
 * dos_lib.c — UAOS dos.library Implementation
 *
 * AmigaOS dos.library provides file system operations, process control,
 * and command-line interface functions. This is a native implementation
 * for UAOS using the existing VFS layer.
 */

#include "rom_modules.h"
#include "dos/vfs.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

/* =========================================================================
 * AmigaOS DOS Structures
 * ========================================================================= */

/* Lock structure - represents a locked directory */
typedef struct {
    RamFsNode *node;      /* Directory node */
    uint32_t    flags;    /* Lock flags (shared/exclusive) */
} DosLock;

/* FileInfoBlock structure - file/directory information */
typedef struct {
    uint64_t fib_DiskKey;      /* Disk key */
    uint32_t fib_DirEntryType; /* Entry type (file/directory) */
    char     fib_FileName[108];/* File name */
    uint32_t fib_Protection;   /* Protection bits */
    uint32_t fib_EntryType;    /* Entry type */
    uint32_t fib_Size;         /* File size */
    uint32_t fib_NumBlocks;    /* Number of blocks */
    uint32_t fib_Date;         /* Date stamp */
    uint32_t fib_Comment;      /* Comment */
    uint8_t  fib_Reserved[32]; /* Reserved */
} FileInfoBlock;

/* Entry types */
#define ST_ROOTDIR   1
#define ST_USERDIR   2
#define ST_FILE      -3

/* =========================================================================
 * Lock Management
 * ========================================================================= */

#define MAX_LOCKS 64
static DosLock g_locks[MAX_LOCKS];
static uint32_t g_next_lock_id = 1;

static uint32_t alloc_lock(RamFsNode *node, uint32_t flags)
{
    if (g_next_lock_id >= MAX_LOCKS)
        return 0; /* No more locks */
    
    uint32_t lock_id = g_next_lock_id++;
    g_locks[lock_id].node = node;
    g_locks[lock_id].flags = flags;
    return lock_id;
}

static void free_lock(uint32_t lock_id)
{
    if (lock_id > 0 && lock_id < MAX_LOCKS) {
        g_locks[lock_id].node = NULL;
        g_locks[lock_id].flags = 0;
    }
}

static DosLock *get_lock(uint32_t lock_id)
{
    if (lock_id > 0 && lock_id < MAX_LOCKS)
        return &g_locks[lock_id];
    return NULL;
}

/* =========================================================================
 * dos.library function indices (must match AmigaOS LVO offsets)
 * Note: dos.library has a very large API - this is a subset
 * ========================================================================= */

#define DOS_OPEN_LIBRARY   1
#define DOS_CLOSE_LIBRARY  2
#define DOS_OPEN           3
#define DOS_CLOSE          4
#define DOS_READ           5
#define DOS_WRITE          6
#define DOS_INPUT          7
#define DOS_OUTPUT         8
#define DOS_PRINT          9
#define DOS_VPRINTF        10
#define DOS_EXIT           11
#define DOS_IO_ERR         12
#define DOS_SET_IO_ERR     13
#define DOS_SELECT_INPUT   14
#define DOS_SELECT_OUTPUT  15
#define DOS_LOCK           16
#define DOS_UNLOCK         17
#define DOS_EXAMINE        18
#define DOS_EXAMINE_NEXT   19
#define DOS_OPEN_FROM_LOCK 20
#define DOS_CREATE_DIR     21
#define DOS_DELETE_FILE    22
#define DOS_RENAME         23
#define DOS_SET_PROTECTION 24
#define DOS_GET_VAR        25
#define DOS_SET_VAR        26
#define DOS_READ_ARGS      27
#define DOS_GET_ARG_STR    28
#define DOS_IS_INTERACTIVE 29
#define DOS_EXECUTE        30

/* =========================================================================
 * Stub implementations
 * ========================================================================= */

static void dos_OpenLibrary(void)
{
    /* OpenLibrary - return library base */
    fprintf(stderr, "[DOS] OpenLibrary called\n");
}

static void dos_CloseLibrary(void)
{
    /* CloseLibrary - no-op for ROM library */
    fprintf(stderr, "[DOS] CloseLibrary called\n");
}

static void dos_Open(void)
{
    /* Open - open file */
    fprintf(stderr, "[DOS] Open called\n");
}

static void dos_Close(void)
{
    /* Close - close file */
    fprintf(stderr, "[DOS] Close called\n");
}

static void dos_Read(void)
{
    /* Read - read from file */
    fprintf(stderr, "[DOS] Read called\n");
}

static void dos_Write(void)
{
    /* Write - write to file */
    fprintf(stderr, "[DOS] Write called\n");
}

static void dos_Input(void)
{
    /* Input - get input file handle */
    fprintf(stderr, "[DOS] Input called\n");
}

static void dos_Output(void)
{
    /* Output - get output file handle */
    fprintf(stderr, "[DOS] Output called\n");
}

static void dos_Print(void)
{
    /* Print - print string to output */
    fprintf(stderr, "[DOS] Print called\n");
}

static void dos_VPrintf(void)
{
    /* VPrintf - formatted output */
    fprintf(stderr, "[DOS] VPrintf called\n");
}

static void dos_Exit(void)
{
    /* Exit - exit process */
    fprintf(stderr, "[DOS] Exit called\n");
}

static void dos_IoErr(void)
{
    /* IoErr - get last I/O error */
    fprintf(stderr, "[DOS] IoErr called\n");
}

static void dos_SetIoErr(void)
{
    /* SetIoErr - set I/O error code */
    fprintf(stderr, "[DOS] SetIoErr called\n");
}

static void dos_SelectInput(void)
{
    /* SelectInput - change input handle */
    fprintf(stderr, "[DOS] SelectInput called\n");
}

static void dos_SelectOutput(void)
{
    /* SelectOutput - change output handle */
    fprintf(stderr, "[DOS] SelectOutput called\n");
}

static void dos_Lock(void)
{
    /* Lock - lock a directory
     * D1 = path string
     * D2 = access mode (SHARED_LOCK/EXCLUSIVE_LOCK)
     * Returns: lock handle (BPTR) or 0 on failure */
    fprintf(stderr, "[DOS] Lock called\n");
    /* TODO: Implement with M68k memory access
     * Implementation would be:
     * 1. Read path string from guest memory
     * 2. Use VFS_ResolveDir to find the directory
     * 3. Allocate a lock with alloc_lock()
     * 4. Return lock ID to guest
     */
}

static void dos_Unlock(void)
{
    /* Unlock - unlock a directory
     * D1 = lock handle to release */
    fprintf(stderr, "[DOS] Unlock called\n");
    /* TODO: Implement with M68k memory access
     * Implementation would be:
     * 1. Read lock ID from D1
     * 2. Call free_lock(lock_id)
     */
}

static void dos_Examine(void)
{
    /* Examine - examine file/directory info
     * D1 = lock handle
     * D2 = pointer to FileInfoBlock structure
     * Returns: 1 on success, 0 on failure */
    fprintf(stderr, "[DOS] Examine called\n");
    /* TODO: Implement with M68k memory access
     * Implementation would be:
     * 1. Read lock ID from D1
     * 2. Get DosLock from lock ID
     * 3. Fill FileInfoBlock with node information:
     *    - fib_DirEntryType = ST_USERDIR or ST_FILE
     *    - fib_FileName = node name
     *    - fib_Size = node size
     *    - fib_Protection = default protection
     * 4. Write FileInfoBlock to guest memory
     */
}

static void dos_ExamineNext(void)
{
    /* ExamineNext - get next directory entry
     * D1 = lock handle
     * D2 = pointer to FileInfoBlock structure
     * Returns: 1 on success, 0 on failure (no more entries) */
    fprintf(stderr, "[DOS] ExamineNext called\n");
    /* TODO: Implement with M68k memory access
     * Implementation would be:
     * 1. Read lock ID from D1
     * 2. Get DosLock from lock ID
     * 3. Use VFS_OpenDir to get directory entries
     * 4. Fill FileInfoBlock with next entry information
     * 5. Write FileInfoBlock to guest memory
     */
}

static void dos_OpenFromLock(void)
{
    /* OpenFromLock - open file from lock */
    fprintf(stderr, "[DOS] OpenFromLock called\n");
}

static void dos_CreateDir(void)
{
    /* CreateDir - create directory */
    fprintf(stderr, "[DOS] CreateDir called\n");
}

static void dos_DeleteFile(void)
{
    /* DeleteFile - delete file */
    fprintf(stderr, "[DOS] DeleteFile called\n");
}

static void dos_Rename(void)
{
    /* Rename - rename file */
    fprintf(stderr, "[DOS] Rename called\n");
}

static void dos_SetProtection(void)
{
    /* SetProtection - set file protection bits */
    fprintf(stderr, "[DOS] SetProtection called\n");
}

static void dos_GetVar(void)
{
    /* GetVar - get environment variable */
    fprintf(stderr, "[DOS] GetVar called\n");
}

static void dos_SetVar(void)
{
    /* SetVar - set environment variable */
    fprintf(stderr, "[DOS] SetVar called\n");
}

static void dos_ReadArgs(void)
{
    /* ReadArgs - parse command line arguments */
    fprintf(stderr, "[DOS] ReadArgs called\n");
}

static void dos_GetArgStr(void)
{
    /* GetArgStr - get command line string */
    fprintf(stderr, "[DOS] GetArgStr called\n");
}

static void dos_IsInteractive(void)
{
    /* IsInteractive - check if handle is interactive */
    fprintf(stderr, "[DOS] IsInteractive called\n");
}

static void dos_Execute(void)
{
    /* Execute - execute a command */
    fprintf(stderr, "[DOS] Execute called\n");
}

/* =========================================================================
 * Function table
 * ========================================================================= */

static void *dos_funcs[] = {
    dos_OpenLibrary,   /* index 1  */
    dos_CloseLibrary,  /* index 2  */
    dos_Open,           /* index 3  */
    dos_Close,          /* index 4  */
    dos_Read,           /* index 5  */
    dos_Write,          /* index 6  */
    dos_Input,          /* index 7  */
    dos_Output,         /* index 8  */
    dos_Print,          /* index 9  */
    dos_VPrintf,        /* index 10 */
    dos_Exit,           /* index 11 */
    dos_IoErr,          /* index 12 */
    dos_SetIoErr,       /* index 13 */
    dos_SelectInput,    /* index 14 */
    dos_SelectOutput,   /* index 15 */
    dos_Lock,           /* index 16 */
    dos_Unlock,         /* index 17 */
    dos_Examine,        /* index 18 */
    dos_ExamineNext,    /* index 19 */
    dos_OpenFromLock,   /* index 20 */
    dos_CreateDir,      /* index 21 */
    dos_DeleteFile,     /* index 22 */
    dos_Rename,         /* index 23 */
    dos_SetProtection,  /* index 24 */
    dos_GetVar,         /* index 25 */
    dos_SetVar,         /* index 26 */
    dos_ReadArgs,       /* index 27 */
    dos_GetArgStr,      /* index 28 */
    dos_IsInteractive,  /* index 29 */
    dos_Execute,        /* index 30 */
};

/* =========================================================================
 * Registration function
 * ========================================================================= */

void UAOS_DOS_Register(void)
{
    UAOS_ROM_Register("dos.library", 40, 0x000000D0,
                      (uint16_t)(sizeof(dos_funcs) / sizeof(dos_funcs[0])),
                      dos_funcs);
}
