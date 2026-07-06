/*
 * dos_lib.c — UAOS dos.library Implementation
 *
 * AmigaOS dos.library provides file system operations, process control,
 * and command-line interface functions.
 *
 * This implementation is dispatched via the ROM module system
 * (UAOS_ROM_NativeFunc) and receives M68kCPUState so it can be called
 * from any emulator backend without backend-specific APIs.
 */

#include "rom_modules.h"
#include "uaos_emu.h"
#include "dos/vfs.h"
#include "dos/handler.h"
#include "dos/handle_table.h"
#include "dos/dospacket.h"
#include "dos/amiga_dos_types.h"
#include "exec/task.h"
#include "exec/amiga_task.h"
#include <stdint.h>
#include <stddef.h>
#include "irq/rtc.h"
#include "net/ntp.h"

extern volatile uint64_t g_pit_ticks;

/* =========================================================================
 * Console output helpers
 * ========================================================================= */
extern void kprint(const char *s);

/* =========================================================================
 * BSTR / path helpers
 * ========================================================================= */

/* Convert BSTR BPTR to native C string into dst[max].
 * Returns length or 0 if invalid. */
static int bstr_to_c(uint32_t bptr_bptr, char *dst, int max)
{
    uint32_t addr = bptr_bptr << 2;
    if (addr >= GUEST_RAM_SIZE || max < 2) return 0;
    uint8_t len = g_ram[addr];
    if (len > (uint8_t)(max - 1)) len = (uint8_t)(max - 1);
    for (int i = 0; i < (int)len; i++) dst[i] = (char)g_ram[addr + 1 + i];
    dst[len] = '\0';
    return (int)len;
}

/* Extract volume name from a path like "RAM:dir/file" into dst[max].
 * Returns length or 0 if no colon. */
static int extract_vol_name(const char *path, char *dst, int max)
{
    int i = 0;
    while (path[i] && path[i] != ':' && i < max - 1) { dst[i] = path[i]; i++; }
    dst[i] = '\0';
    return (path[i] == ':') ? i : 0;
}

/* =========================================================================
 * Guest-visible FileLock helpers
 * ========================================================================= */

static void guest_write_be32(uint32_t addr, uint32_t val)
{
    g_ram[addr + 0] = (uint8_t)(val >> 24);
    g_ram[addr + 1] = (uint8_t)(val >> 16);
    g_ram[addr + 2] = (uint8_t)(val >>  8);
    g_ram[addr + 3] = (uint8_t)(val      );
}

static uint32_t guest_read_be32(uint32_t addr)
{
    return ((uint32_t)g_ram[addr + 0] << 24)
         | ((uint32_t)g_ram[addr + 1] << 16)
         | ((uint32_t)g_ram[addr + 2] <<  8)
         | ((uint32_t)g_ram[addr + 3]      );
}

/* =========================================================================
 * Guest heap — free-list allocator
 *
 * Each free block has an 8-byte header in guest RAM:
 *   [0..3] size of block (including header), big-endian uint32
 *   [4..7] BPTR to next free block (0 = end of list), big-endian uint32
 *
 * Allocated blocks have only the size word at [0..3] (magic bit 31 set).
 * The rest of the block is available to the caller.
 *
 * The free-list is built lazily: the first time we need to free we
 * initialise it to cover all RAM above the current bump pointer.
 * ========================================================================= */

/* AmigaOS exec.library AllocMem memory-type requirements */
#define MEMF_PUBLIC     0x00000001u
#define MEMF_CHIP       0x00000002u
#define MEMF_FAST       0x00000004u
#define MEMF_LOCAL      0x00000008u
#define MEMF_24BITDMA   0x00000010u
#define MEMF_DMA        0x00000010u
#define MEMF_CLEAR      0x00010000u
#define MEMF_EXPUNGE    0x80000000u

#define HEAP_HDR        8u           /* header size in bytes             */
#define HEAP_MAGIC      0x80000000u  /* marks an allocated block         */
#define HEAP_FREE_BASE  0x020000u    /* permanent start of free-list pool */

/* Guest RAM layout: 8 MB chip (0x000000–0x7FFFFF) + 8 MB fast (0x800000–0xFFFFFF).
 * The free-list pools keep a 64 KB guard at the top of each region. */
#define HEAP_CHIP_END   0x007F0000u
#define HEAP_FAST_START 0x00800000u
#define HEAP_FAST_END   0x00FF0000u
#define HEAP_ANY_END    HEAP_FAST_END

/* Address in guest RAM of the free-list heads (BPTR words) in reserved 0x200-0x2FF */
#define HEAP_LIST_SLOT_CHIP 0x0200u
#define HEAP_LIST_SLOT_FAST 0x0204u

static int g_heap_ready = 0;
static int g_fast_heap_ready = 0;

static void heap_freelist_init_pool(uint32_t list_slot, uint32_t pool_start, uint32_t pool_end,
                                    int *ready_flag)
{
    if (*ready_flag) return;
    *ready_flag = 1;

    uint32_t start = pool_start;
    start = (start + 3u) & ~3u;
    if (start < HEAP_FREE_BASE) start = HEAP_FREE_BASE;
    if (start + HEAP_HDR >= pool_end) return; /* no room */

    uint32_t blk_size = pool_end - start;

    /* Write the single initial free block */
    guest_write_be32(start + 0, blk_size); /* size (no magic) */
    guest_write_be32(start + 4, 0);         /* next = NULL     */

    /* Store its BPTR in the list-head slot */
    guest_write_be32(list_slot, start >> 2);
}

static void heap_freelist_init(void)
{
    /* Chip pool covers the remaining bump-pointer space up to the 8 MB line. */
    heap_freelist_init_pool(HEAP_LIST_SLOT_CHIP, g_uaos_heap_ptr, HEAP_CHIP_END,
                            &g_heap_ready);
}

static void heap_freelist_init_fast(void)
{
    /* Fast pool covers the upper 8 MB of guest RAM. */
    heap_freelist_init_pool(HEAP_LIST_SLOT_FAST, HEAP_FAST_START, HEAP_FAST_END,
                            &g_fast_heap_ready);
}

/* Allocate 'size' bytes from a specific free list.  Returns guest addr or 0. */
static uint32_t heap_alloc_fl_pool(uint32_t size, uint32_t list_slot)
{
    size = (size + 3u) & ~3u;
    uint32_t need = size + HEAP_HDR;

    uint32_t prev_slot = list_slot;  /* address of the pointer to current */
    uint32_t cur_bptr  = guest_read_be32(list_slot);

    while (cur_bptr) {
        uint32_t cur = cur_bptr << 2;
        uint32_t blk_size = guest_read_be32(cur + 0) & ~HEAP_MAGIC;
        uint32_t next_bptr = guest_read_be32(cur + 4);

        if (blk_size >= need) {
            /* Split if there's enough room for another free block */
            if (blk_size >= need + HEAP_HDR + 4u) {
                uint32_t rem = cur + need;
                uint32_t rem_size = blk_size - need;
                guest_write_be32(rem + 0, rem_size);
                guest_write_be32(rem + 4, next_bptr);
                /* Link split block in place of cur */
                guest_write_be32(prev_slot, rem >> 2);
            } else {
                /* Use whole block — unlink cur */
                guest_write_be32(prev_slot, next_bptr);
            }

            /* Mark allocated */
            guest_write_be32(cur + 0, blk_size | HEAP_MAGIC);

            /* Zero payload */
            for (uint32_t i = HEAP_HDR; i < blk_size && cur + i < GUEST_RAM_SIZE; i++)
                g_ram[cur + i] = 0;

            return cur + HEAP_HDR;  /* return pointer past header */
        }

        prev_slot = cur + 4;
        cur_bptr  = next_bptr;
    }

    return 0; /* out of memory */
}

/* Free a block previously returned by heap_alloc_fl_pool.
 * Returns it to the front of the given free list. */
static void heap_free_fl_pool(uint32_t addr, uint32_t list_slot)
{
    if (addr < HEAP_HDR || addr >= GUEST_RAM_SIZE) return;

    uint32_t blk = addr - HEAP_HDR;
    uint32_t magic_size = guest_read_be32(blk + 0);
    if (!(magic_size & HEAP_MAGIC)) return;   /* not a valid alloc header */

    uint32_t blk_size = magic_size & ~HEAP_MAGIC;
    guest_write_be32(blk + 0, blk_size);                        /* clear magic */
    guest_write_be32(blk + 4, guest_read_be32(list_slot));     /* prepend     */
    guest_write_be32(list_slot, blk >> 2);
}

/* Allocate 'size' bytes from the chip free list. */
static uint32_t heap_alloc_fl_chip(uint32_t size)
{
    heap_freelist_init();
    return heap_alloc_fl_pool(size, HEAP_LIST_SLOT_CHIP);
}

/* Allocate 'size' bytes from the fast free list. */
static uint32_t heap_alloc_fl_fast(uint32_t size)
{
    heap_freelist_init_fast();
    return heap_alloc_fl_pool(size, HEAP_LIST_SLOT_FAST);
}

/* Legacy single-pool allocator: defaults to fast with chip fallback.
 * Preserves old callers until they are updated to pass requirements. */
static uint32_t heap_alloc_fl(uint32_t size)
{
    uint32_t addr = heap_alloc_fl_fast(size);
    if (!addr) addr = heap_alloc_fl_chip(size);
    return addr;
}

/* Free a block previously returned by any heap_alloc_fl_* variant.
 * Returns it to the correct pool based on its guest address. */
static void heap_free_fl(uint32_t addr)
{
    if (addr >= HEAP_FAST_START)
        heap_free_fl_pool(addr, HEAP_LIST_SLOT_FAST);
    else
        heap_free_fl_pool(addr, HEAP_LIST_SLOT_CHIP);
}

/* Legacy bump allocator — still used for internal structures that are never
 * freed (FileLocks, library tables, etc.) and for the initial program load.
 * Program hunks must live in chip RAM, so cap the bump pointer there. */
static uint32_t heap_alloc(uint32_t size)
{
    size = (size + 3) & ~3u;
    if (g_uaos_heap_ptr + size > HEAP_CHIP_END) return 0;
    uint32_t addr = g_uaos_heap_ptr;
    g_uaos_heap_ptr += size;
    for (uint32_t i = 0; i < size; i++) g_ram[addr + i] = 0;
    return addr;
}

/* =========================================================================
 * exec.library — AllocMem / FreeMem
 * These are the high-level dos_lib entry points called from the ROM module
 * dispatcher when M68k code calls exec.library AllocMem/FreeMem via the
 * extended ILLEGAL dispatch (lib == LIB_EXEC, fn == EXEC_ALLOC_MEM /
 * EXEC_FREE_MEM).  They are also exported to dos.library callers that need
 * to dynamically allocate guest memory from within C code.
 * ========================================================================= */

static void dos_AllocMem(M68kCPUState *cpu)
{
    /* AmigaOS: D0=byteSize, D1=requirements → D0=APTR or NULL */
    uint32_t size = cpu->d[0];
    uint32_t reqs = cpu->d[1];
    if (size == 0) { cpu->d[0] = 0; return; }

    uint32_t addr = 0;
    if (reqs & (MEMF_CHIP | MEMF_DMA | MEMF_24BITDMA)) {
        addr = heap_alloc_fl_chip(size);
    } else if (reqs & MEMF_FAST) {
        addr = heap_alloc_fl_fast(size);
    } else {
        /* Default / MEMF_PUBLIC: prefer fast, fallback to chip. */
        addr = heap_alloc_fl_fast(size);
        if (!addr) addr = heap_alloc_fl_chip(size);
    }

    if (!addr) SetIoErr(ERROR_NO_FREE_STORE);
    cpu->d[0] = addr;
}

static void dos_FreeMem(M68kCPUState *cpu)
{
    /* AmigaOS: A1=memoryBlock, D0=byteSize */
    uint32_t addr = cpu->a[1];
    heap_free_fl(addr);
}

/* Allocate a FileLock in guest RAM and return its BPTR */
static uint32_t guest_alloc_filelock(uint32_t handle, int32_t access)
{
    uint32_t addr = heap_alloc(16);
    if (!addr) return 0;
    guest_write_be32(addr + 0, handle);
    guest_write_be32(addr + 4, (uint32_t)access);
    guest_write_be32(addr + 8, 1);
    guest_write_be32(addr + 12, 0);
    return addr >> 2;
}

/* Read a FileLock from guest RAM.  Returns 0 if lock_bptr invalid. */
static int guest_read_filelock(uint32_t lock_bptr,
                                uint32_t *out_handle,
                                int32_t  *out_access)
{
    uint32_t addr = lock_bptr << 2;
    if (addr + 16 > GUEST_RAM_SIZE) return 0;
    if (out_handle) *out_handle = guest_read_be32(addr + 0);
    if (out_access) *out_access = (int32_t)guest_read_be32(addr + 4);
    return 1;
}

/* =========================================================================
 * Fake file handle BPTRs
 * ========================================================================= */
#define FAKE_STDOUT_ADDR   0x0500
#define FAKE_STDIN_ADDR    0x0504
#define DOS_STDOUT_BPTR    (FAKE_STDOUT_ADDR >> 2)
#define DOS_STDIN_BPTR     (FAKE_STDIN_ADDR  >> 2)

/* =========================================================================
 * dos.library function implementations
 * All functions receive M68kCPUState* so they are backend-agnostic.
 * ========================================================================= */

static void dos_Output(M68kCPUState *cpu)
{
    cpu->d[0] = DOS_STDOUT_BPTR;
}

static void dos_Input(M68kCPUState *cpu)
{
    cpu->d[0] = DOS_STDIN_BPTR;
}

static void dos_VFPrintf(M68kCPUState *cpu)
{
    uint32_t fh  = cpu->d[1];
    uint32_t fmt = cpu->a[0];
    uint32_t arr = cpu->a[1];
    (void)fh; (void)arr;
    if (fmt < GUEST_RAM_SIZE) {
        char tmp[256];
        int i = 0;
        while (i < 255 && fmt + i < GUEST_RAM_SIZE && g_ram[fmt + i]) {
            tmp[i] = (char)g_ram[fmt + i]; i++;
        }
        tmp[i] = '\0';
        kprint(tmp);
    }
    cpu->d[0] = 0;
}

static void dos_FPuts(M68kCPUState *cpu)
{
    uint32_t sp = cpu->d[2];
    if (sp < GUEST_RAM_SIZE) {
        char tmp[256];
        int i = 0;
        while (i < 255 && sp + i < GUEST_RAM_SIZE && g_ram[sp + i]) {
            tmp[i] = (char)g_ram[sp + i]; i++;
        }
        tmp[i] = '\0';
        kprint(tmp);
    }
    cpu->d[0] = 0;
}

static void dos_PutStr(M68kCPUState *cpu)
{
    uint32_t sp = cpu->d[1];
    if (sp < GUEST_RAM_SIZE) {
        char tmp[256];
        int i = 0;
        while (i < 255 && sp + i < GUEST_RAM_SIZE && g_ram[sp + i]) {
            tmp[i] = (char)g_ram[sp + i]; i++;
        }
        tmp[i] = '\0';
        kprint(tmp);
    }
    cpu->d[0] = 0;
}

static void dos_VPrintf(M68kCPUState *cpu)
{
    uint32_t fmt = cpu->d[1];
    uint32_t arr = cpu->d[2];
    (void)arr;
    if (fmt < GUEST_RAM_SIZE) {
        char tmp[256];
        int i = 0;
        while (i < 255 && fmt + i < GUEST_RAM_SIZE && g_ram[fmt + i]) {
            tmp[i] = (char)g_ram[fmt + i]; i++;
        }
        tmp[i] = '\0';
        kprint(tmp);
    }
    cpu->d[0] = 0;
}

static void dos_VFWritef(M68kCPUState *cpu)
{
    uint32_t fmt = cpu->d[2];
    char tmp[256];
    bstr_to_c(fmt, tmp, sizeof(tmp));
    kprint(tmp);
    cpu->d[0] = 0;
}

static void dos_ReadArgs(M68kCPUState *cpu)
{
    (void)cpu;
    cpu->d[0] = 1;
}

static void dos_GetArgStr(M68kCPUState *cpu)
{
    cpu->d[0] = 0;
}

static void dos_IsInteractive(M68kCPUState *cpu)
{
    uint32_t fh = cpu->d[1];
    if (fh == DOS_STDOUT_BPTR || fh == DOS_STDIN_BPTR) {
        cpu->d[0] = (uint32_t)DOSTRUE;
    } else {
        cpu->d[0] = (uint32_t)DOSFALSE;
    }
}

static void dos_Exit(M68kCPUState *cpu)
{
    (void)cpu;
    g_emu_halted = 1;
}

static void dos_IoErr(M68kCPUState *cpu)
{
    cpu->d[0] = (uint32_t)IoErr();
}


/* =========================================================================
 * Packet-based file operations (the core DOS logic)
 * ========================================================================= */

static void dos_Open(M68kCPUState *cpu)
{
    uint32_t bptr = cpu->d[1];
    char name[128];
    int blen = bstr_to_c(bptr, name, sizeof(name));
    if (blen == 0) {
        cpu->d[0] = DOS_STDOUT_BPTR;
        return;
    }
    if (name[0] == '*' ||
        (name[0]=='C' && name[1]=='O' && name[2]=='N') ||
        (name[0]=='N' && name[1]=='I' && name[2]=='L') ||
        (name[0]=='R' && name[1]=='A' && name[2]=='W') ||
        (name[0]=='A' && name[1]=='U' && name[2]=='X')) {
        cpu->d[0] = DOS_STDOUT_BPTR;
        return;
    }

    char full_name[128];
    int has_device = 0;
    for (int i = 0; i < blen; i++) if (name[i] == ':') { has_device = 1; break; }
    if (has_device) {
        int i = 0; while (i < blen) { full_name[i] = name[i]; i++; }
        full_name[i] = '\0';
    } else {
        int cwd_len = 0; while (g_uaos_cwd[cwd_len] && cwd_len < 63) cwd_len++;
        int i = 0; while (i < cwd_len) { full_name[i] = g_uaos_cwd[i]; i++; }
        if (cwd_len > 0 && g_uaos_cwd[cwd_len-1] != ':' && g_uaos_cwd[cwd_len-1] != '/')
            full_name[i++] = '/';
        int j = 0; while (j < blen && i < 127) { full_name[i++] = name[j++]; }
        full_name[i] = '\0';
    }

    char vol_name[16];
    extract_vol_name(full_name, vol_name, sizeof(vol_name));
    MsgPort *port = VFS_GetHandlerPort(vol_name);
    if (!port) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_DEVICE_NOT_MOUNTED);
        return;
    }

    uint32_t mode = cpu->d[2];
    int32_t action = (mode == 1006) ? ACTION_FINDOUTPUT : ACTION_FINDINPUT;
    int32_t handle = DoPkt(port, action, (int32_t)(intptr_t)full_name, (int32_t)mode, 0, 0, 0);
    if (handle == 0) {
        cpu->d[0] = 0;
        SetIoErr(IoErr());
    } else {
        cpu->d[0] = (uint32_t)handle;
    }
}

static void dos_Close(M68kCPUState *cpu)
{
    uint32_t fh = cpu->d[1];
    if (fh == DOS_STDOUT_BPTR || fh == DOS_STDIN_BPTR) {
        cpu->d[0] = 0;
        return;
    }
    HandleEntry *ent = HandleTable_Get(fh);
    if (ent && ent->type == HTYPE_FILE && ent->u.file.fh.node) {
        VFS_Close(&ent->u.file.fh);
    }
    HandleTable_Free(fh);
    cpu->d[0] = 0;
}

static void dos_Read(M68kCPUState *cpu)
{
    uint32_t fh  = cpu->d[1];
    uint32_t buf = cpu->d[2];
    uint32_t len = cpu->d[3];

    if (fh == DOS_STDIN_BPTR) {
        cpu->d[0] = 0;
        return;
    }
    if (fh == DOS_STDOUT_BPTR) {
        cpu->d[0] = (uint32_t)-1;
        SetIoErr(ERROR_ACTION_NOT_KNOWN);
        return;
    }

    HandleEntry *ent = HandleTable_Get(fh);
    if (!ent || ent->type != HTYPE_FILE || !ent->u.file.fh.node) {
        cpu->d[0] = (uint32_t)-1;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }
    if (buf + len >= GUEST_RAM_SIZE) {
        cpu->d[0] = (uint32_t)-1;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }
    cpu->d[0] = VFS_Read(&ent->u.file.fh, g_ram + buf, len);
}

static void dos_Write(M68kCPUState *cpu)
{
    uint32_t fh  = cpu->d[1];
    uint32_t buf = cpu->d[2];
    uint32_t len = cpu->d[3];

    if (fh == DOS_STDOUT_BPTR || fh == DOS_STDIN_BPTR) {
        if (buf + len < GUEST_RAM_SIZE) {
            char tmp[4096];
            uint32_t i;
            for (i = 0; i < len && buf + i < GUEST_RAM_SIZE; i++)
                tmp[i] = (char)g_ram[buf + i];
            tmp[i] = '\0';
            kprint(tmp);
            cpu->d[0] = len;
        } else {
            cpu->d[0] = (uint32_t)-1;
        }
        return;
    }

    HandleEntry *ent = HandleTable_Get(fh);
    if (!ent || ent->type != HTYPE_FILE || !ent->u.file.fh.node) {
        cpu->d[0] = (uint32_t)-1;
        return;
    }
    if (buf + len >= GUEST_RAM_SIZE) {
        cpu->d[0] = (uint32_t)-1;
        return;
    }
    cpu->d[0] = VFS_Write(&ent->u.file.fh, g_ram + buf, len);
}

static void dos_Seek(M68kCPUState *cpu)
{
    uint32_t fh    = cpu->d[1];
    int32_t  offset = (int32_t)cpu->d[2];
    int32_t  mode   = (int32_t)cpu->d[3];

    if (fh == DOS_STDOUT_BPTR || fh == DOS_STDIN_BPTR) {
        cpu->d[0] = (uint32_t)-1;
        SetIoErr(ERROR_ACTION_NOT_KNOWN);
        return;
    }

    HandleEntry *ent = HandleTable_Get(fh);
    if (!ent || ent->type != HTYPE_FILE || !ent->u.file.fh.node) {
        cpu->d[0] = (uint32_t)-1;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    VfsFile *f = &ent->u.file.fh;
    uint32_t new_pos = 0;
    uint32_t size = VFS_Size(f);
    if (mode == OFFSET_CURRENT)      new_pos = f->pos + (uint32_t)offset;
    else if (mode == OFFSET_END)       new_pos = size + (uint32_t)offset;
    else if (mode == OFFSET_BEGINNING) new_pos = (uint32_t)offset;
    else                               new_pos = (uint32_t)offset;
    cpu->d[0] = f->pos;
    VFS_Seek(f, new_pos);
}

static void dos_DeleteFile(M68kCPUState *cpu)
{
    uint32_t bptr = cpu->d[1];
    char name[128];
    int blen = bstr_to_c(bptr, name, sizeof(name));
    if (blen == 0) {
        cpu->d[0] = (uint32_t)DOSFALSE;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    char full_name[128];
    int has_device = 0;
    for (int i = 0; i < blen; i++) if (name[i] == ':') { has_device = 1; break; }
    if (has_device) {
        int i = 0; while (i < blen) { full_name[i] = name[i]; i++; }
        full_name[i] = '\0';
    } else {
        int cwd_len = 0; while (g_uaos_cwd[cwd_len] && cwd_len < 63) cwd_len++;
        int i = 0; while (i < cwd_len) { full_name[i] = g_uaos_cwd[i]; i++; }
        if (cwd_len > 0 && g_uaos_cwd[cwd_len-1] != ':' && g_uaos_cwd[cwd_len-1] != '/')
            full_name[i++] = '/';
        int j = 0; while (j < blen && i < 127) { full_name[i++] = name[j++]; }
        full_name[i] = '\0';
    }

    char vol_name[16];
    extract_vol_name(full_name, vol_name, sizeof(vol_name));
    MsgPort *port = VFS_GetHandlerPort(vol_name);
    if (!port) {
        cpu->d[0] = (uint32_t)DOSFALSE;
        SetIoErr(ERROR_DEVICE_NOT_MOUNTED);
        return;
    }

    int32_t res = DoPkt(port, ACTION_DELETE_OBJECT, (int32_t)(intptr_t)full_name, 0, 0, 0, 0);
    cpu->d[0] = (uint32_t)res;
}

static void dos_Rename(M68kCPUState *cpu)
{
    uint32_t old_bptr = cpu->d[1];
    uint32_t new_bptr = cpu->d[2];
    char old_name[128], new_name[128];
    bstr_to_c(old_bptr, old_name, sizeof(old_name));
    bstr_to_c(new_bptr, new_name, sizeof(new_name));

    char old_full[128], new_full[128];
    int o_dev = 0, n_dev = 0;
    for (int i = 0; old_name[i]; i++) if (old_name[i] == ':') { o_dev = 1; break; }
    for (int i = 0; new_name[i]; i++) if (new_name[i] == ':') { n_dev = 1; break; }

    if (o_dev) {
        int i = 0; while (old_name[i]) { old_full[i] = old_name[i]; i++; }
        old_full[i] = '\0';
    } else {
        int cwd_len = 0; while (g_uaos_cwd[cwd_len] && cwd_len < 63) cwd_len++;
        int i = 0; while (i < cwd_len) { old_full[i] = g_uaos_cwd[i]; i++; }
        if (cwd_len > 0 && g_uaos_cwd[cwd_len-1] != ':' && g_uaos_cwd[cwd_len-1] != '/')
            old_full[i++] = '/';
        int j = 0; while (old_name[j] && i < 127) { old_full[i++] = old_name[j++]; }
        old_full[i] = '\0';
    }

    if (n_dev) {
        int i = 0; while (new_name[i]) { new_full[i] = new_name[i]; i++; }
        new_full[i] = '\0';
    } else {
        int cwd_len = 0; while (g_uaos_cwd[cwd_len] && cwd_len < 63) cwd_len++;
        int i = 0; while (i < cwd_len) { new_full[i] = g_uaos_cwd[i]; i++; }
        if (cwd_len > 0 && g_uaos_cwd[cwd_len-1] != ':' && g_uaos_cwd[cwd_len-1] != '/')
            new_full[i++] = '/';
        int j = 0; while (new_name[j] && i < 127) { new_full[i++] = new_name[j++]; }
        new_full[i] = '\0';
    }

    char vol_name[16];
    extract_vol_name(old_full, vol_name, sizeof(vol_name));
    MsgPort *port = VFS_GetHandlerPort(vol_name);
    if (!port) {
        cpu->d[0] = (uint32_t)DOSFALSE;
        SetIoErr(ERROR_DEVICE_NOT_MOUNTED);
        return;
    }

    int32_t res = DoPkt(port, ACTION_RENAME_OBJECT,
                        (int32_t)(intptr_t)old_full, (int32_t)(intptr_t)new_full, 0, 0, 0);
    cpu->d[0] = (uint32_t)res;
}

static void dos_SetProtection(M68kCPUState *cpu)
{
    uint32_t bptr = cpu->d[1];
    int32_t mask  = (int32_t)cpu->d[2];
    char name[128];
    bstr_to_c(bptr, name, sizeof(name));

    char full_name[128];
    int has_device = 0;
    for (int i = 0; name[i]; i++) if (name[i] == ':') { has_device = 1; break; }
    if (has_device) {
        int i = 0; while (name[i]) { full_name[i] = name[i]; i++; }
        full_name[i] = '\0';
    } else {
        int cwd_len = 0; while (g_uaos_cwd[cwd_len] && cwd_len < 63) cwd_len++;
        int i = 0; while (i < cwd_len) { full_name[i] = g_uaos_cwd[i]; i++; }
        if (cwd_len > 0 && g_uaos_cwd[cwd_len-1] != ':' && g_uaos_cwd[cwd_len-1] != '/')
            full_name[i++] = '/';
        int j = 0; while (name[j] && i < 127) { full_name[i++] = name[j++]; }
        full_name[i] = '\0';
    }

    char vol_name[16];
    extract_vol_name(full_name, vol_name, sizeof(vol_name));
    MsgPort *port = VFS_GetHandlerPort(vol_name);
    if (!port) {
        cpu->d[0] = (uint32_t)DOSFALSE;
        SetIoErr(ERROR_DEVICE_NOT_MOUNTED);
        return;
    }

    int32_t res = DoPkt(port, ACTION_SET_PROTECT, (int32_t)(intptr_t)full_name, mask, 0, 0, 0);
    cpu->d[0] = (uint32_t)res;
}

static void dos_GetVar(M68kCPUState *cpu)
{
    (void)cpu;
    cpu->d[0] = (uint32_t)-1;
    SetIoErr(ERROR_ACTION_NOT_KNOWN);
}

static void dos_SetVar(M68kCPUState *cpu)
{
    (void)cpu;
    cpu->d[0] = 0;
    SetIoErr(ERROR_ACTION_NOT_KNOWN);
}


static void dos_Lock(M68kCPUState *cpu)
{
    uint32_t bptr  = cpu->d[1];
    int32_t  mode  = (int32_t)cpu->d[2];
    char name[128];
    int blen = bstr_to_c(bptr, name, sizeof(name));

    char full_name[128];
    int has_device = 0;
    for (int i = 0; i < blen; i++) if (name[i] == ':') { has_device = 1; break; }
    if (has_device) {
        int i = 0; while (i < blen) { full_name[i] = name[i]; i++; }
        full_name[i] = '\0';
    } else {
        int cwd_len = 0; while (g_uaos_cwd[cwd_len] && cwd_len < 63) cwd_len++;
        int i = 0; while (i < cwd_len) { full_name[i] = g_uaos_cwd[i]; i++; }
        if (cwd_len > 0 && g_uaos_cwd[cwd_len-1] != ':' && g_uaos_cwd[cwd_len-1] != '/')
            full_name[i++] = '/';
        int j = 0; while (j < blen && i < 127) { full_name[i++] = name[j++]; }
        full_name[i] = '\0';
    }

    char vol_name[16];
    extract_vol_name(full_name, vol_name, sizeof(vol_name));
    MsgPort *port = VFS_GetHandlerPort(vol_name);
    if (!port) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_DEVICE_NOT_MOUNTED);
        return;
    }

    int32_t handle = DoPkt(port, ACTION_LOCATE_OBJECT, (int32_t)(intptr_t)full_name, mode, 0, 0, 0);
    if (handle == 0) {
        cpu->d[0] = 0;
        SetIoErr(IoErr());
        return;
    }

    uint32_t lock_bptr = guest_alloc_filelock((uint32_t)handle, mode);
    if (lock_bptr == 0) {
        HandleTable_Free((uint32_t)handle);
        cpu->d[0] = 0;
        SetIoErr(ERROR_NO_FREE_STORE);
        return;
    }
    cpu->d[0] = lock_bptr;
}

static void dos_Unlock(M68kCPUState *cpu)
{
    uint32_t lock = cpu->d[1];
    if (lock == 0) { cpu->d[0] = (uint32_t)DOSTRUE; return; }

    uint32_t handle = 0;
    guest_read_filelock(lock, &handle, NULL);
    if (handle) HandleTable_Free(handle);
    cpu->d[0] = (uint32_t)DOSTRUE;
}

static void dos_DupLock(M68kCPUState *cpu)
{
    uint32_t lock = cpu->d[1];
    if (lock == 0) { cpu->d[0] = 0; return; }

    uint32_t handle = 0;
    if (!guest_read_filelock(lock, &handle, NULL) || handle == 0) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    HandleEntry *ent = HandleTable_GetLockEntry(handle, NULL);
    if (!ent || ent->type != HTYPE_LOCK) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    char vol_name[16];
    extract_vol_name(ent->path, vol_name, sizeof(vol_name));
    MsgPort *port = VFS_GetHandlerPort(vol_name);
    if (!port) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_DEVICE_NOT_MOUNTED);
        return;
    }

    int32_t dup = DoPkt(port, ACTION_COPY_DIR, (int32_t)handle, 0, 0, 0, 0);
    if (dup == 0) {
        cpu->d[0] = 0;
        SetIoErr(IoErr());
        return;
    }

    uint32_t dup_bptr = guest_alloc_filelock((uint32_t)dup, -2);
    if (dup_bptr == 0) {
        HandleTable_Free((uint32_t)dup);
        cpu->d[0] = 0;
        SetIoErr(ERROR_NO_FREE_STORE);
        return;
    }
    cpu->d[0] = dup_bptr;
}

static void dos_Parent(M68kCPUState *cpu)
{
    uint32_t lock = cpu->d[1];
    if (lock == 0) { cpu->d[0] = 0; return; }

    uint32_t handle = 0;
    if (!guest_read_filelock(lock, &handle, NULL) || handle == 0) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    HandleEntry *ent = HandleTable_GetLockEntry(handle, NULL);
    if (!ent || ent->type != HTYPE_LOCK) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    char vol_name[16];
    extract_vol_name(ent->path, vol_name, sizeof(vol_name));
    MsgPort *port = VFS_GetHandlerPort(vol_name);
    if (!port) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_DEVICE_NOT_MOUNTED);
        return;
    }

    int32_t ph = DoPkt(port, ACTION_PARENT, (int32_t)handle, 0, 0, 0, 0);
    if (ph == 0) {
        cpu->d[0] = 0;
        SetIoErr(IoErr());
        return;
    }

    uint32_t parent_bptr = guest_alloc_filelock((uint32_t)ph, -2);
    if (parent_bptr == 0) {
        HandleTable_Free((uint32_t)ph);
        cpu->d[0] = 0;
        SetIoErr(ERROR_NO_FREE_STORE);
        return;
    }
    cpu->d[0] = parent_bptr;
}

static void dos_Examine(M68kCPUState *cpu)
{
    uint32_t lock = cpu->d[1];
    uint32_t fib_ptr = cpu->d[2];

    if (fib_ptr >= GUEST_RAM_SIZE) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    uint32_t handle = 0;
    if (!guest_read_filelock(lock, &handle, NULL) || handle == 0) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    HandleEntry *ent = HandleTable_GetLockEntry(handle, NULL);
    if (!ent || ent->type != HTYPE_LOCK) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    char vol_name[16];
    extract_vol_name(ent->path, vol_name, sizeof(vol_name));
    MsgPort *port = VFS_GetHandlerPort(vol_name);
    if (!port) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_DEVICE_NOT_MOUNTED);
        return;
    }

    int32_t res = DoPkt(port, ACTION_EXAMINE_OBJECT, (int32_t)handle,
                        (int32_t)(intptr_t)(g_ram + fib_ptr), 0, 0, 0);
    cpu->d[0] = (uint32_t)res;
}

static void dos_ExamineNext(M68kCPUState *cpu)
{
    uint32_t lock = cpu->d[1];
    uint32_t fib_ptr = cpu->d[2];

    if (fib_ptr >= GUEST_RAM_SIZE) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    uint32_t handle = 0;
    if (!guest_read_filelock(lock, &handle, NULL) || handle == 0) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    HandleEntry *ent = HandleTable_GetLockEntry(handle, NULL);
    if (!ent || ent->type != HTYPE_LOCK) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    char vol_name[16];
    extract_vol_name(ent->path, vol_name, sizeof(vol_name));
    MsgPort *port = VFS_GetHandlerPort(vol_name);
    if (!port) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_DEVICE_NOT_MOUNTED);
        return;
    }

    int32_t res = DoPkt(port, ACTION_EXAMINE_NEXT, (int32_t)handle,
                        (int32_t)(intptr_t)(g_ram + fib_ptr), 0, 0, 0);
    cpu->d[0] = (uint32_t)res;
}

static void dos_CreateDir(M68kCPUState *cpu)
{
    uint32_t bptr = cpu->d[1];
    char name[128];
    int blen = bstr_to_c(bptr, name, sizeof(name));

    char full_name[128];
    int has_device = 0;
    for (int i = 0; i < blen; i++) if (name[i] == ':') { has_device = 1; break; }
    if (has_device) {
        int i = 0; while (i < blen) { full_name[i] = name[i]; i++; }
        full_name[i] = '\0';
    } else {
        int cwd_len = 0; while (g_uaos_cwd[cwd_len] && cwd_len < 63) cwd_len++;
        int i = 0; while (i < cwd_len) { full_name[i] = g_uaos_cwd[i]; i++; }
        if (cwd_len > 0 && g_uaos_cwd[cwd_len-1] != ':' && g_uaos_cwd[cwd_len-1] != '/')
            full_name[i++] = '/';
        int j = 0; while (j < blen && i < 127) { full_name[i++] = name[j++]; }
        full_name[i] = '\0';
    }

    char vol_name[16];
    extract_vol_name(full_name, vol_name, sizeof(vol_name));
    MsgPort *port = VFS_GetHandlerPort(vol_name);
    if (!port) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_DEVICE_NOT_MOUNTED);
        return;
    }

    int32_t res = DoPkt(port, ACTION_CREATE_DIR, (int32_t)(intptr_t)full_name, 0, 0, 0, 0);
    cpu->d[0] = (uint32_t)res;
}

/* =========================================================================
 * Date / time helpers
 * ========================================================================= */

static int is_leap_year(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static int32_t days_since_1978(uint16_t year, uint8_t month, uint8_t day)
{
    int32_t days = 0;
    for (int y = 1978; y < year; y++) {
        days += is_leap_year(y) ? 366 : 365;
    }
    static const int mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    for (int m = 1; m < month; m++) {
        days += mdays[m - 1];
        if (m == 2 && is_leap_year(year)) days++;
    }
    days += day - 1;
    return days;
}

static void uint_to_str(uint32_t v, char *buf, int max)
{
    if (max <= 1) { if (max == 1) buf[0] = '\0'; return; }
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return; }

    char tmp[12];
    int i = 0;
    while (v > 0 && i < 11) {
        tmp[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    int j = 0;
    while (j < i && j < max - 1) {
        buf[j] = tmp[i - 1 - j];
        j++;
    }
    buf[j] = '\0';
}

static void uint_to_str_2d(uint32_t v, char *buf)
{
    buf[0] = (char)('0' + (v / 10));
    buf[1] = (char)('0' + (v % 10));
    buf[2] = '\0';
}

/* =========================================================================
 * Pattern matching helpers — shared by ParsePattern / MatchPattern
 * ========================================================================= */

static int pattern_match(const char *name, const char *pat)
{
    const char *n = name;
    const char *p = pat;
    const char *star_n = NULL;
    const char *star_p = NULL;

    while (*n) {
        char pc = *p;
        char nc = *n;
        if (pc >= 'A' && pc <= 'Z') pc += 32;
        if (nc >= 'A' && nc <= 'Z') nc += 32;

        if (pc == '%') {
            /* % matches the empty (NULL) string */
            p++;
            continue;
        } else if (pc == '*' || (pc == '#' && p[1] == '?')) {
            if (pc == '#') p++;
            star_p = ++p;
            star_n = n;
            continue;
        } else if (pc == '?') {
            p++;
            n++;
            continue;
        } else if (pc == nc) {
            p++;
            n++;
            continue;
        }

        if (star_p) {
            p = star_p;
            n = ++star_n;
            continue;
        }
        return 0;
    }

    while (*p == '*' || (*p == '#' && p[1] == '?') || *p == '%') {
        if (*p == '#') p++;
        p++;
    }
    return *p == '\0';
}

/* =========================================================================
 * DateStamp — fill guest DateStamp with current date/time
 * ========================================================================= */

static void dos_DateStamp(M68kCPUState *cpu)
{
    uint32_t addr = cpu->a[0];
    if (addr && addr + 12 <= GUEST_RAM_SIZE) {
        RtcDateTime dt = RTC_ReadDateTime();
        int32_t days = days_since_1978(dt.year, dt.month, dt.day);
        int32_t minutes = (int32_t)dt.hour * 60 + (int32_t)dt.min;
        int32_t ticks = (int32_t)dt.sec * 50;  /* PAL: 50 ticks/sec */

        guest_write_be32(addr + 0, (uint32_t)days);
        guest_write_be32(addr + 4, (uint32_t)minutes);
        guest_write_be32(addr + 8, (uint32_t)ticks);
    }
    cpu->d[0] = cpu->a[0];
}

/* =========================================================================
 * Delay — busy-wait using a spin loop
 * ========================================================================= */

static void dos_Delay(M68kCPUState *cpu)
{
    uint32_t ticks = cpu->d[0];
    if (ticks == 0) return;

    /* Conservative busy-wait: ~20 ms per Amiga tick.
     * This is approximate; real timing depends on CPU frequency. */
    volatile uint64_t n = (uint64_t)ticks * 4000000ULL;
    while (n--) {
        __asm__ __volatile__("pause");
    }
}

/* =========================================================================
 * DateToStr — convert DateStamp to formatted strings
 * ========================================================================= */

static void dos_DateToStr(M68kCPUState *cpu)
{
    uint32_t dt_addr = cpu->a[0];
    if (!dt_addr || dt_addr + 28 > GUEST_RAM_SIZE) {
        cpu->d[0] = (uint32_t)DOSFALSE;
        return;
    }

    /* Read DateTime struct from guest RAM (big-endian) */
    int32_t ds_days    = (int32_t)guest_read_be32(dt_addr + 0);
    int32_t ds_minute  = (int32_t)guest_read_be32(dt_addr + 4);
    int32_t ds_tick    = (int32_t)guest_read_be32(dt_addr + 8);
    uint8_t format     = g_ram[dt_addr + 12];
    uint32_t str_day   = guest_read_be32(dt_addr + 16);
    uint32_t str_date  = guest_read_be32(dt_addr + 20);
    uint32_t str_time  = guest_read_be32(dt_addr + 24);

    /* Convert DateStamp to calendar fields.
     * Amiga epoch (1978-01-01) to Unix epoch (1970-01-01) = 2922 days. */
    uint32_t unix_ts = (uint32_t)((ds_days + 2922) * 86400LL +
                                   ds_minute * 60LL + ds_tick / 50);
    uint16_t year; uint8_t month, day, hour, min, sec;
    ntp_unix_to_datetime(unix_ts, &year, &month, &day, &hour, &min, &sec);

    static const char *mon_name[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };

    /* Write day string (empty for now) */
    if (str_day && str_day < GUEST_RAM_SIZE) {
        g_ram[str_day] = '\0';
    }

    /* Write date string */
    if (str_date && str_date + 16 < GUEST_RAM_SIZE) {
        char buf[16];
        uint32_t yr = year % 100;
        switch (format) {
            case 2: /* FORMAT_USA */
                uint_to_str_2d(month, buf);
                buf[2] = '-';
                uint_to_str_2d(day, buf + 3);
                buf[5] = '-';
                uint_to_str_2d(yr, buf + 6);
                buf[8] = '\0';
                break;
            case 3: /* FORMAT_CDN */
                uint_to_str_2d(yr, buf);
                buf[2] = '-';
                uint_to_str_2d(month, buf + 3);
                buf[5] = '-';
                uint_to_str_2d(day, buf + 6);
                buf[8] = '\0';
                break;
            case 1: /* FORMAT_INTL */
                uint_to_str_2d(day, buf);
                buf[2] = '-';
                buf[3] = mon_name[month - 1][0];
                buf[4] = mon_name[month - 1][1];
                buf[5] = mon_name[month - 1][2];
                buf[6] = '-';
                uint_to_str(year, buf + 7, 6);
                break;
            default: /* FORMAT_DOS, FORMAT_DEF */
                uint_to_str_2d(day, buf);
                buf[2] = '-';
                buf[3] = mon_name[month - 1][0];
                buf[4] = mon_name[month - 1][1];
                buf[5] = mon_name[month - 1][2];
                buf[6] = '-';
                uint_to_str_2d(yr, buf + 7);
                buf[9] = '\0';
                break;
        }
        int i = 0;
        while (buf[i] && str_date + i < GUEST_RAM_SIZE) {
            g_ram[str_date + i] = (uint8_t)buf[i];
            i++;
        }
        g_ram[str_date + i] = '\0';
    }

    /* Write time string */
    if (str_time && str_time + 10 < GUEST_RAM_SIZE) {
        char buf[10];
        uint_to_str_2d(hour, buf);
        buf[2] = ':';
        uint_to_str_2d(min, buf + 3);
        buf[5] = ':';
        uint_to_str_2d(sec, buf + 6);
        buf[8] = '\0';
        int i = 0;
        while (buf[i] && str_time + i < GUEST_RAM_SIZE) {
            g_ram[str_time + i] = (uint8_t)buf[i];
            i++;
        }
        g_ram[str_time + i] = '\0';
    }

    cpu->d[0] = (uint32_t)DOSTRUE;
}

/* =========================================================================
 * ParsePattern / ParsePatternNoCase — copy pattern and detect wildcards
 *
 * Recognised wildcards: ?, *, #? (and convenience alias *), plus % (matches
 * the empty string, as in AmigaDOS).  ParsePattern returns 1 if the
 * pattern contains wildcards, 0 if it is a plain literal, and -1 on error.
 * ========================================================================= */

static void dos_ParsePattern(M68kCPUState *cpu)
{
    uint32_t src = cpu->d[1];
    uint32_t dst = cpu->a[0];
    int32_t dst_len = (int32_t)cpu->d[2];

    if (dst_len < 1 || src >= GUEST_RAM_SIZE || dst >= GUEST_RAM_SIZE) {
        cpu->d[0] = (uint32_t)-1;
        return;
    }

    int i = 0;
    int has_wild = 0;

    while (i < dst_len - 1 && src + i < GUEST_RAM_SIZE && g_ram[src + i]) {
        char c = (char)g_ram[src + i];
        g_ram[dst + i] = (uint8_t)c;
        if (c == '?' || c == '*' || c == '#' || c == '%') has_wild = 1;
        i++;
    }

    if (src + i < GUEST_RAM_SIZE && g_ram[src + i]) {
        cpu->d[0] = (uint32_t)-1;
        return;
    }

    g_ram[dst + i] = 0;
    cpu->d[0] = has_wild ? 1 : 0;
}

static void dos_ParsePatternNoCase(M68kCPUState *cpu)
{
    /* Same behaviour as ParsePattern — our MatchPattern is case-insensitive */
    dos_ParsePattern(cpu);
}

/* =========================================================================
 * MatchPattern / MatchPatternNoCase — match string against parsed pattern
 * ========================================================================= */

static void dos_MatchPattern(M68kCPUState *cpu)
{
    uint32_t pat = cpu->d[1];
    uint32_t str = cpu->a[0];

    if (pat >= GUEST_RAM_SIZE || str >= GUEST_RAM_SIZE) {
        cpu->d[0] = (uint32_t)DOSFALSE;
        return;
    }

    char pat_buf[128];
    char str_buf[128];
    int i = 0;
    while (i < 127 && pat + i < GUEST_RAM_SIZE && g_ram[pat + i]) {
        pat_buf[i] = (char)g_ram[pat + i]; i++;
    }
    pat_buf[i] = '\0';

    i = 0;
    while (i < 127 && str + i < GUEST_RAM_SIZE && g_ram[str + i]) {
        str_buf[i] = (char)g_ram[str + i]; i++;
    }
    str_buf[i] = '\0';

    cpu->d[0] = pattern_match(str_buf, pat_buf) ? (uint32_t)DOSTRUE : (uint32_t)DOSFALSE;
}

static void dos_MatchPatternNoCase(M68kCPUState *cpu)
{
    /* Same behaviour as MatchPattern — our pattern_match is already case-insensitive */
    dos_MatchPattern(cpu);
}

/* =========================================================================
 * Segment loading (LoadSeg / UnLoadSeg)
 * ========================================================================= */

#define LS_HUNK_HEADER   0x3F3
#define LS_HUNK_CODE     0x3E9
#define LS_HUNK_DATA     0x3EA
#define LS_HUNK_BSS      0x3EB
#define LS_HUNK_RELOC32  0x3EC
#define LS_HUNK_END      0x3F2
#define LS_HUNK_SYMBOL   0x3F0
#define LS_HUNK_DEBUG    0x3F1

#define MAX_LOADSEG_HUNKS  32

static uint8_t g_loadseg_buf[524288]; /* 512KB temp for hunk loading */

/* Forward declarations for seglist tracking */
static void seglist_track(uint32_t bptr, uint32_t addr);
static uint32_t seglist_untrack(uint32_t bptr);

static uint32_t ls_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) | ((uint32_t)p[3]);
}

static uint32_t loadseg_hunk_load(const uint8_t *bin, uint32_t bin_size)
{
    if (bin_size < 8) return 0;
    const uint8_t *p = bin;
    const uint8_t *end = bin + bin_size;

    if (ls_be32(p) != LS_HUNK_HEADER) return 0;
    p += 4;

    while (p + 4 <= end) {
        uint32_t cnt = ls_be32(p); p += 4;
        if (!cnt) break;
        p += cnt * 4;
    }

    if (p + 12 > end) return 0;
    uint32_t table_size = ls_be32(p); p += 4;
    uint32_t first_hunk = ls_be32(p); p += 4;
    uint32_t last_hunk  = ls_be32(p); p += 4;
    (void)table_size;

    uint32_t n_hunks = last_hunk - first_hunk + 1;
    if (n_hunks > MAX_LOADSEG_HUNKS) return 0;

    uint32_t hunk_base[MAX_LOADSEG_HUNKS];
    uint32_t allocated[MAX_LOADSEG_HUNKS];

    for (uint32_t i = 0; i < n_hunks; i++) {
        if (p + 4 > end) return 0;
        uint32_t words = ls_be32(p) & 0x3FFFFFFF; p += 4;
        uint32_t bytes = words * 4;
        uint32_t seg_size = 4 + (bytes ? bytes : 4);
        seg_size = (seg_size + 3) & ~3u;
        allocated[i] = heap_alloc_fl(seg_size);
        if (!allocated[i]) return 0;
        hunk_base[i] = allocated[i] + 4;
    }

    /* Write SegList next pointers */
    for (uint32_t i = 0; i < n_hunks; i++) {
        guest_write_be32(allocated[i], (i + 1 < n_hunks) ? (allocated[i + 1] >> 2) : 0);
    }

    int cur = 0;
    while (p + 4 <= end && cur < (int)n_hunks) {
        uint32_t type = ls_be32(p) & 0x3FFFFFFF; p += 4;

        if (type == LS_HUNK_CODE || type == LS_HUNK_DATA) {
            if (p + 4 > end) break;
            uint32_t words = ls_be32(p); p += 4;
            uint32_t bytes = words * 4;
            if (p + bytes > end) return 0;
            for (uint32_t i = 0; i < bytes; i++)
                g_ram[hunk_base[cur] + i] = p[i];
            p += bytes;
        } else if (type == LS_HUNK_BSS) {
            if (p + 4 > end) break;
            p += 4;
        } else if (type == LS_HUNK_RELOC32) {
            while (p + 4 <= end) {
                uint32_t n_offsets = ls_be32(p); p += 4;
                if (!n_offsets) break;
                if (p + 4 > end) break;
                uint32_t ref_hunk = ls_be32(p); p += 4;
                if (ref_hunk >= n_hunks) { p += n_offsets * 4; continue; }
                uint32_t base = hunk_base[ref_hunk];
                for (uint32_t r = 0; r < n_offsets; r++) {
                    if (p + 4 > end) break;
                    uint32_t offset = ls_be32(p); p += 4;
                    uint32_t patch_addr = hunk_base[cur] + offset;
                    if (patch_addr + 4 <= GUEST_RAM_SIZE) {
                        uint32_t old_val = guest_read_be32(patch_addr);
                        guest_write_be32(patch_addr, old_val + base);
                    }
                }
            }
            continue;
        } else if (type == LS_HUNK_SYMBOL || type == LS_HUNK_DEBUG) {
            while (p + 4 <= end) {
                uint32_t len = ls_be32(p); p += 4;
                if (!len) break;
                p += len * 4 + 4;
            }
            continue;
        } else if (type == LS_HUNK_END) {
            cur++;
            continue;
        } else {
            break;
        }
    }

    return allocated[0];
}

static void dos_LoadSeg(M68kCPUState *cpu)
{
    uint32_t bptr = cpu->d[1];
    char name[128];
    int blen = bstr_to_c(bptr, name, sizeof(name));
    if (blen == 0) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    char full_name[128];
    int has_device = 0;
    for (int i = 0; i < blen; i++) if (name[i] == ':') { has_device = 1; break; }
    if (has_device) {
        int i = 0; while (i < blen) { full_name[i] = name[i]; i++; }
        full_name[i] = '\0';
    } else {
        int cwd_len = 0; while (g_uaos_cwd[cwd_len] && cwd_len < 63) cwd_len++;
        int i = 0; while (i < cwd_len) { full_name[i] = g_uaos_cwd[i]; i++; }
        if (cwd_len > 0 && g_uaos_cwd[cwd_len - 1] != ':' && g_uaos_cwd[cwd_len - 1] != '/')
            full_name[i++] = '/';
        int j = 0; while (j < blen && i < 127) { full_name[i++] = name[j++]; }
        full_name[i] = '\0';
    }

    VfsFile fh;
    if (!VFS_Open(&fh, full_name, VFS_READ)) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    uint32_t size = VFS_Size(&fh);
    if (size == 0 || size > sizeof(g_loadseg_buf)) {
        VFS_Close(&fh);
        cpu->d[0] = 0;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    uint32_t read = VFS_Read(&fh, g_loadseg_buf, size);
    VFS_Close(&fh);
    if (read != size) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    uint32_t seglist = loadseg_hunk_load(g_loadseg_buf, size);
    if (seglist == 0) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    /* Track the seglist so UnLoadSeg can free it */
    seglist_track(seglist >> 2, seglist);

    cpu->d[0] = seglist >> 2;
}

/* =========================================================================
 * Seglist tracking — map BPTR → allocation address so UnLoadSeg can free
 * ========================================================================= */

#define MAX_SEGLISTS 32

typedef struct {
    uint32_t bptr;   /* BPTR returned to caller (allocated[0] >> 2)        */
    uint32_t addr;   /* raw guest address of first segment (allocated[0])   */
} SeglistEntry;

static SeglistEntry g_seglists[MAX_SEGLISTS];
static int          g_seglist_count = 0;

static void seglist_track(uint32_t bptr, uint32_t addr)
{
    if (g_seglist_count >= MAX_SEGLISTS) return;
    g_seglists[g_seglist_count].bptr = bptr;
    g_seglists[g_seglist_count].addr = addr;
    g_seglist_count++;
}

static uint32_t seglist_untrack(uint32_t bptr)
{
    for (int i = 0; i < g_seglist_count; i++) {
        if (g_seglists[i].bptr == bptr) {
            uint32_t addr = g_seglists[i].addr;
            g_seglists[i] = g_seglists[--g_seglist_count];
            return addr;
        }
    }
    return 0;
}

static void dos_UnLoadSeg(M68kCPUState *cpu)
{
    /* AmigaOS: D1=BPTR seglist */
    uint32_t seg_bptr = cpu->d[1];
    if (!seg_bptr) return;

    /* Walk the seglist chain and free each segment via the free-list allocator.
     * Each segment header layout (set up by loadseg_hunk_load):
     *   [0..3] BPTR to next segment (or 0)
     *   [4..]  segment data (code / data / bss)
     * All segments allocated via heap_alloc_fl are tracked in g_seglists.
     * We walk the chain and free each segment through the free-list allocator. */

    uint32_t tracked_addr = seglist_untrack(seg_bptr);
    if (tracked_addr) {
        /* Walk the chain and free each segment's payload through free-list */
        uint32_t cur = tracked_addr;
        while (cur && cur < GUEST_RAM_SIZE) {
            uint32_t next_bptr = guest_read_be32(cur + 0);
            heap_free_fl(cur + 4);  /* payload starts at +4 */
            cur = next_bptr ? (next_bptr << 2) : 0;
        }
    }
    /* If not tracked, silently no-op (should not happen with properly loaded seglists) */
}

/* =========================================================================
 * Process control
 * ========================================================================= */

/* =========================================================================
 * Process helpers — forward declarations
 * ========================================================================= */

/* From exec_task.c */
extern UaosTask *Task_CreateM68k(const char *name, int8_t pri,
                                  const uint8_t *binary, uint32_t bin_size,
                                  const char **argv,
                                  void (*print_fn)(const char *));
extern UaosTask *Task_Current(void);

/* From uaos_m68k_glue.c */
extern int m68k_execute(int num_cycles);
extern void m68k_end_timeslice(void);
extern void m68k_set_reg(int reg, unsigned int val);
extern unsigned int m68k_get_reg(void *context, int reg);

/* M68k register IDs (Musashi) */
#define M68K_REG_D0  0
#define M68K_REG_D1  1
#define M68K_REG_D2  2
#define M68K_REG_D3  3
#define M68K_REG_A0  8
#define M68K_REG_A6 14
#define M68K_REG_A7 15
#define M68K_REG_PC 16

/* =========================================================================
 * Helpers to build a minimal in-RAM M68k binary that jumps to a loaded
 * seglist entry point.  RunCommand uses this to call a seglist without
 * having a real Hunk file on disk.
 *
 * Layout in the temp buffer (big-endian M68k instructions):
 *   JSR  <entry_addr>.L   (4EBx absolute long)
 *   MOVEQ #rc, D0
 *   ILLEGAL (triggers dos_Exit via glue)
 *
 * We can't use the normal "binary" path because that requires a Hunk header.
 * Instead we call the entry directly via m68k_execute after manually setting
 * up the registers and PC.
 * ========================================================================= */

/* Allocate & build a minimal Process struct in guest RAM.
 * Returns the guest address or 0. */
static uint32_t build_process_struct(uint8_t pri)
{
    uint32_t proc_addr = heap_alloc_fl(PROCESS_SIZE + CLI_SIZE + 32);
    if (!proc_addr) return 0;

    uint32_t cli_addr = proc_addr + PROCESS_SIZE;

    /* Zero the whole block */
    for (uint32_t i = 0; i < PROCESS_SIZE + CLI_SIZE + 32; i++)
        g_ram[proc_addr + i] = 0;

    /* Task node header */
    g_ram[proc_addr + TASK_LN_TYPE] = NT_PROCESS;
    g_ram[proc_addr + TASK_LN_PRI]  = (uint8_t)pri;

    /* tc_SigAlloc: all bits free */
    guest_write_be32(proc_addr + TASK_TC_SIGALLOC, 0xFFFFFFFFu);

    /* pr_CLI = BPTR to CLI */
    guest_write_be32(proc_addr + PR_CLI_OFFSET, cli_addr >> 2);
    /* pr_CIS, pr_COS — fake console handles */
    guest_write_be32(proc_addr + PR_CIS_OFFSET, DOS_STDIN_BPTR);
    guest_write_be32(proc_addr + PR_COS_OFFSET, DOS_STDOUT_BPTR);

    /* Minimal CLI struct */
    g_ram[cli_addr] = 0x01;

    return proc_addr;
}

static void dos_CreateProc(M68kCPUState *cpu)
{
    /* AmigaOS: D1=BSTR name, D2=LONG pri, D3=BPTR seglist, D4=ULONG stackSize
     * → D0=struct MsgPort * (process port) or NULL */
    uint32_t name_bptr = cpu->d[1];
    int8_t   pri       = (int8_t)(cpu->d[2] & 0xFF);
    uint32_t seg_bptr  = cpu->d[3];
    uint32_t stacksize = cpu->d[4];
    (void)stacksize;

    /* Decode name */
    char proc_name[64];
    bstr_to_c(name_bptr, proc_name, sizeof(proc_name));
    if (!proc_name[0]) {
        int i = 0;
        const char *dflt = "NewProc";
        while (dflt[i]) { proc_name[i] = dflt[i]; i++; }
        proc_name[i] = '\0';
    }

    /* Validate seglist */
    if (!seg_bptr) {
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        cpu->d[0] = 0;
        return;
    }

    /* Resolve entry point: first code word after the seglist header.
     * Seglist BPTR points to (seg_bptr<<2) which holds the next-BPTR at [0]
     * and code/data starting at [4].  The entry point is seg_base+4. */
    uint32_t seg_addr  = seg_bptr << 2;
    if (seg_addr + 8 > GUEST_RAM_SIZE) {
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        cpu->d[0] = 0;
        return;
    }
    uint32_t entry = seg_addr + 4;  /* standard AmigaOS calling convention */

    /* Build a minimal Process struct in the current task's guest RAM */
    uint32_t proc_addr = build_process_struct(pri);
    if (!proc_addr) {
        SetIoErr(ERROR_NO_FREE_STORE);
        cpu->d[0] = 0;
        return;
    }

    /* Store process struct in ExecBase so it is visible to FindTask */
    guest_write_be32(0x0300 + 0x114, proc_addr);

    /* Calculate total seglist size by walking the hunks.
     * Read actual segment sizes from heap allocation metadata. */
    uint32_t total_size = 0;
    uint32_t cur_seg = seg_addr;
    while (cur_seg && cur_seg >= HEAP_HDR && cur_seg < GUEST_RAM_SIZE) {
        uint32_t next_bptr = guest_read_be32(cur_seg);
        /* Read segment size from heap header (at cur_seg - HEAP_HDR) */
        uint32_t seg_size = 4096;  /* Default fallback */
        if (cur_seg >= HEAP_HDR + 4) {
            uint32_t blk = cur_seg - HEAP_HDR;
            uint32_t magic_size = guest_read_be32(blk);
            if (magic_size & HEAP_MAGIC) {
                seg_size = (magic_size & ~HEAP_MAGIC) - HEAP_HDR;
            }
        }
        total_size += seg_size;
        if (next_bptr == 0) break;
        cur_seg = next_bptr << 2;
    }

    /* Create M68k task to run the seglist
     * We pass the first segment's address as the binary pointer
     * The entry point is seg_addr + 4 as per AmigaOS convention
     * The task wrapper will handle setting up the execution context */
    if (total_size > 0 && total_size < GUEST_RAM_SIZE) {
        /* Copy seglist data to a temporary buffer for Task_CreateM68k */
        static uint8_t seglist_buf[256 * 1024];  /* 256KB max for CreateProc */
        if (total_size > sizeof(seglist_buf)) total_size = sizeof(seglist_buf);

        /* Copy first segment - this is what the task will execute from */
        uint32_t first_seg_size = total_size;
        if (first_seg_size > sizeof(seglist_buf)) first_seg_size = sizeof(seglist_buf);
        for (uint32_t i = 0; i < first_seg_size && (seg_addr + i) < GUEST_RAM_SIZE; i++) {
            seglist_buf[i] = g_ram[seg_addr + i];
        }

        /* Create the task - it will use the seglist buffer */
        UaosTask *new_task = Task_CreateM68k(proc_name, pri, seglist_buf, first_seg_size, NULL, NULL);
        if (new_task) {
            /* Store the entry point for the task */
            new_task->m68k_entry = entry;
            /* Link the process struct to the task */
            new_task->m68k_task_struct = proc_addr;
            kprint("[dos] CreateProc: launched task '");
        } else {
            kprint("[dos] CreateProc: failed to create task '");
        }
    } else {
        kprint("[dos] CreateProc: invalid seglist size '");
    }
    kprint(proc_name);
    kprint("'\n");

    /* Return the MsgPort BPTR (pr_MsgPort starts at PROCESS + PR_PORT = 0x5A) */
    cpu->d[0] = (proc_addr + PR_PORT) >> 2;
}

static void dos_RunCommand(M68kCPUState *cpu)
{
    /* AmigaOS: D1=BPTR seglist, D2=ULONG stackSize, A0=STRPTR argPtr,
     *          D3=ULONG argSize → D0=LONG returnCode
     *
     * RunCommand executes a loaded seglist synchronously in the context of
     * the current process, then returns the result code.
     *
     * Implementation:
     *   1. Resolve the entry point from the seglist.
     *   2. Push the argument string + length onto the M68k stack.
     *   3. JSR to the entry — we stay inside the current m68k_execute loop
     *      because we just adjust PC and SP then continue execution.
     *      The segment's startup code will eventually call dos.library Exit()
     *      which sets g_emu_halted=1; we detect that and return the RC.
     *
     * Caveat: because we redirect execution we cannot truly "return" from
     * RunCommand until the sub-program calls Exit().  This is exactly what
     * real AmigaOS does — RunCommand is a call/return via the stack.
     */
    uint32_t seg_bptr  = cpu->d[1];
    uint32_t arg_ptr   = cpu->a[0];
    uint32_t arg_size  = cpu->d[3];
    (void)cpu->d[2];   /* stackSize — we use the current stack */

    if (!seg_bptr) {
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        cpu->d[0] = (uint32_t)-1;
        return;
    }

    uint32_t seg_addr = seg_bptr << 2;
    if (seg_addr + 8 > GUEST_RAM_SIZE) {
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        cpu->d[0] = (uint32_t)-1;
        return;
    }

    /* Entry point is 4 bytes past the seglist next-pointer */
    uint32_t entry = seg_addr + 4;

    /* Set up the M68k state to call the entry point.
     *   D0 = argument size (number of characters)
     *   A0 = argument pointer (C string or BSTR depending on startup)
     * We do a virtual JSR by pushing the current PC on the M68k stack so
     * RTS at the end of the program returns here.  Then we set PC = entry.
     *
     * However, in practice most programs call dos.library Exit() rather than
     * RTS, which sets g_emu_halted.  We handle both paths:
     *   - If the program does RTS, PC returns to a small trap stub we install.
     *   - If the program calls Exit(), g_emu_halted stops execution.
     *
     * We install a 4-byte "return stub" just below the current SP:
     *   ILLEGAL 0x4AFC + dispatch word (DOS_EXIT via lib 2, fn 6)
     */
    uint32_t sp = m68k_get_reg(NULL, M68K_REG_A7);

    /* Place return stub at a known location in upper RAM */
    const uint32_t ret_stub = 0x1EF800u;
    if (ret_stub + 4 <= GUEST_RAM_SIZE) {
        g_ram[ret_stub + 0] = 0x4A; g_ram[ret_stub + 1] = 0xFC; /* ILLEGAL */
        g_ram[ret_stub + 2] = 0x02; g_ram[ret_stub + 3] = 0x06; /* LIB_DOS, DOS_EXIT */
    }

    /* Push return address (ret_stub) onto M68k stack */
    sp -= 4;
    if (sp + 4 <= GUEST_RAM_SIZE) {
        guest_write_be32(sp, ret_stub);
    }
    m68k_set_reg(M68K_REG_A7, sp);

    /* Set entry arguments per AmigaOS calling convention */
    m68k_set_reg(M68K_REG_D0, arg_size);
    m68k_set_reg(M68K_REG_A0, arg_ptr);

    /* Jump to entry */
    m68k_set_reg(M68K_REG_PC, entry);

    /* Signal the glue that we don't want to stop yet */
    g_emu_halted = 0;

    /* Execute until Exit() is called (or until something else terminates) */
    while (!g_emu_halted) {
        m68k_execute(10000);
    }

    /* Retrieve the return code that Exit() was called with.
     * dos_Exit stores it in D1 (Amiga convention); after Exit sets
     * g_emu_halted the last D1 value is still in the Musashi register set. */
    uint32_t rc = m68k_get_reg(NULL, M68K_REG_D1);

    /* Reset halted flag — the outer loop can continue */
    g_emu_halted = 0;

    cpu->d[0] = rc;
}

/* Tag constants for SystemTagList (from dos/dosextens.h) */
#define NP_Seglist      (0x80000000 + 2)   /* BPTR seglist for new process */
#define NP_FreeSeglist  (0x80000000 + 3)   /* BOOL: free seglist on exit */
#define NP_CopyArgs     (0x80000000 + 4)   /* BOOL: copy args */
#define NP_ArgPtr       (0x80000000 + 5)   /* STRPTR: argument pointer */
#define NP_ArgLen       (0x80000000 + 6)   /* ULONG: argument length */
#define NP_Priority     (0x80000000 + 7)   /* BYTE: initial priority */
#define NP_Name         (0x80000000 + 9)   /* STRPTR: process name */
#define NP_Cwd          (0x80000000 + 12)  /* BPTR: current working directory */
#define NP_StackSize    (0x80000000 + 13)  /* ULONG: stack size */

/* Tag constants for SystemTagList (SYS_*) */
#define SYS_Input       (0x80000000 + 100) /* BPTR: input filehandle */
#define SYS_Output      (0x80000000 + 101) /* BPTR: output filehandle */
#define SYS_Asynch      (0x80000000 + 102) /* BOOL: run asynchronously */

/* Tag list control tags */
#define TAG_DONE        0
#define TAG_MORE        0x80000001
#define TAG_IGNORE      0x80000002
#define TAG_JUMP        0x80000003

typedef struct TagItem {
    uint32_t ti_Tag;
    uint32_t ti_Data;
} TagItem_t;

static uint32_t parse_tag_item(uint32_t *tag_ptr, uint32_t tag_to_find)
{
    if (!tag_ptr) return 0;

    while (1) {
        uint32_t tag = guest_read_be32((uint32_t)(uintptr_t)tag_ptr);
        uint32_t data = guest_read_be32((uint32_t)(uintptr_t)tag_ptr + 4);

        if (tag == TAG_DONE) {
            break;
        }

        if (tag == TAG_MORE || tag == TAG_JUMP) {
            tag_ptr = (uint32_t *)(uintptr_t)data;
            continue;
        }

        if (tag == TAG_IGNORE) {
            tag_ptr += 2;
            continue;
        }

        if (tag == tag_to_find) {
            return data;
        }

        tag_ptr += 2;  /* Each tag item is 8 bytes */
    }

    return 0;  /* Tag not found */
}

static void dos_SystemTagList(M68kCPUState *cpu)
{
    /* AmigaOS: D1=STRPTR command, A1=struct TagItem *tags → D0=LONG result
     *
     * Create and run a new process with configuration from tag list.
     * Tags extracted:
     *   NP_Name       - process name
     *   NP_Seglist    - BPTR seglist to run
     *   NP_StackSize  - stack size for process
     *   NP_Priority   - task priority (-128 to 127)
     *   NP_ArgPtr     - pointer to argument string
     *   NP_ArgLen     - length of argument string
     *   NP_Cwd        - current working directory
     *   SYS_Input     - input filehandle
     *   SYS_Output    - output filehandle
     *   SYS_Asynch    - if true, run asynchronously
     */
    uint32_t cmd_ptr = cpu->d[1];
    uint32_t tags_ptr = cpu->a[1];

    /* Extract tag values */
    uint32_t np_name = parse_tag_item((uint32_t *)(uintptr_t)tags_ptr, NP_Name);
    uint32_t np_seglist = parse_tag_item((uint32_t *)(uintptr_t)tags_ptr, NP_Seglist);
    uint32_t np_stacksize = parse_tag_item((uint32_t *)(uintptr_t)tags_ptr, NP_StackSize);
    uint32_t np_priority = parse_tag_item((uint32_t *)(uintptr_t)tags_ptr, NP_Priority);
    uint32_t np_argptr = parse_tag_item((uint32_t *)(uintptr_t)tags_ptr, NP_ArgPtr);
    uint32_t np_arglen = parse_tag_item((uint32_t *)(uintptr_t)tags_ptr, NP_ArgLen);
    uint32_t np_cwd = parse_tag_item((uint32_t *)(uintptr_t)tags_ptr, NP_Cwd);
    uint32_t sys_asynch = parse_tag_item((uint32_t *)(uintptr_t)tags_ptr, SYS_Asynch);

    /* If no seglist provided, try to load from command string */
    if (!np_seglist && cmd_ptr && cmd_ptr < GUEST_RAM_SIZE) {
        /* Read the command string */
        char cmd[256];
        int ci = 0;
        while (ci < 255 && cmd_ptr + ci < GUEST_RAM_SIZE && g_ram[cmd_ptr + ci]) {
            cmd[ci] = (char)g_ram[cmd_ptr + ci]; ci++;
        }
        cmd[ci] = '\0';

        /* Split into command name + arguments */
        char cmd_name[128];
        int ni = 0;
        while (cmd[ni] && cmd[ni] != ' ' && ni < 127) {
            cmd_name[ni] = cmd[ni]; ni++;
        }
        cmd_name[ni] = '\0';

        /* Look for binary and load seglist */
        static const char * const search_paths[] = {
            "SYS:C/",
            "RAM:",
            NULL
        };

        for (int pi = 0; search_paths[pi]; pi++) {
            char full_path[128];
            int fi = 0;
            const char *pfx = search_paths[pi];
            while (pfx[fi]) { full_path[fi] = pfx[fi]; fi++; }
            int ni2 = 0;
            while (cmd_name[ni2] && fi < 127) { full_path[fi++] = cmd_name[ni2++]; }
            full_path[fi] = '\0';

            VfsFile fh;
            if (!VFS_Open(&fh, full_path, VFS_READ)) continue;

            uint32_t bin_size = VFS_Size(&fh);
            if (!bin_size || bin_size > sizeof(g_loadseg_buf)) {
                VFS_Close(&fh);
                continue;
            }

            uint32_t rd = VFS_Read(&fh, g_loadseg_buf, bin_size);
            VFS_Close(&fh);
            if (rd != bin_size) continue;

            np_seglist = loadseg_hunk_load(g_loadseg_buf, bin_size);
            if (np_seglist) {
                seglist_track(np_seglist >> 2, np_seglist);
                break;
            }
        }
    }

    if (!np_seglist) {
        cpu->d[0] = (uint32_t)DOSFALSE;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    /* Use defaults for optional parameters */
    if (!np_stacksize) np_stacksize = 8192;  /* Default 8KB stack */
    if (!np_priority) np_priority = 0;       /* Normal priority */

    /* Read process name */
    char proc_name[64] = "NewProc";
    if (np_name) {
        int ni = 0;
        while (ni < 63 && np_name + ni < GUEST_RAM_SIZE && g_ram[np_name + ni]) {
            proc_name[ni] = (char)g_ram[np_name + ni];
            ni++;
        }
        proc_name[ni] = '\0';
    }

    /* Build process struct */
    uint32_t proc_addr = build_process_struct((int8_t)np_priority);
    if (!proc_addr) {
        cpu->d[0] = (uint32_t)DOSFALSE;
        SetIoErr(ERROR_NO_FREE_STORE);
        return;
    }

    /* Store process struct in ExecBase */
    guest_write_be32(0x0300 + 0x114, proc_addr);

    /* Calculate total seglist size for task creation */
    uint32_t total_size = 0;
    uint32_t cur_seg = np_seglist;
    while (cur_seg && cur_seg < GUEST_RAM_SIZE) {
        uint32_t next_bptr = guest_read_be32(cur_seg);
        total_size += 4096;  /* Estimate 4KB per segment */
        if (next_bptr == 0) break;
        cur_seg = next_bptr << 2;
    }

    /* Copy seglist data to buffer for task creation */
    static uint8_t seglist_buf[256 * 1024];
    if (total_size > sizeof(seglist_buf)) total_size = sizeof(seglist_buf);

    uint32_t first_seg_size = total_size;
    if (first_seg_size > sizeof(seglist_buf)) first_seg_size = sizeof(seglist_buf);
    for (uint32_t i = 0; i < first_seg_size && (np_seglist + i) < GUEST_RAM_SIZE; i++) {
        seglist_buf[i] = g_ram[np_seglist + i];
    }

    /* Create M68k task */
    UaosTask *new_task = Task_CreateM68k(proc_name, (int8_t)np_priority,
                                          seglist_buf, first_seg_size, NULL, NULL);
    if (new_task) {
        uint32_t entry = np_seglist + 4;
        new_task->m68k_entry = entry;
        new_task->m68k_task_struct = proc_addr;

        /* Set up arguments if provided */
        if (np_argptr && np_arglen) {
            uint32_t arg_buf = heap_alloc_fl(np_arglen + 1);
            if (arg_buf) {
                for (uint32_t i = 0; i < np_arglen && (np_argptr + i) < GUEST_RAM_SIZE; i++) {
                    g_ram[arg_buf + i] = g_ram[np_argptr + i];
                }
                g_ram[arg_buf + np_arglen] = '\0';
                /* Arguments are now in guest RAM, task will access via A0 */
                (void)arg_buf;  /* Suppress unused warning - used by guest code */
            }
        }

        kprint("[dos] SystemTagList: launched process '\"");
        kprint(proc_name);
        kprint("\"'\n");
        cpu->d[0] = (uint32_t)DOSTRUE;
    } else {
        kprint("[dos] SystemTagList: failed to create process '\"");
        kprint(proc_name);
        kprint("\"'\n");
        cpu->d[0] = (uint32_t)DOSFALSE;
        SetIoErr(ERROR_NO_FREE_STORE);
    }

    /* Return process message port BPTR */
    cpu->a[0] = (proc_addr + 0x5A) >> 2;
    (void)sys_asynch;  /* Synchronous execution for now */
    (void)np_cwd;      /* CWD handling not yet implemented */
}

/* =========================================================================
 * Packets (SendPkt / WaitPkt / ReplyPkt)
 * ========================================================================= */

static void dos_SendPkt(M68kCPUState *cpu)
{
    /* Amiga: A0=DosPacket*, A1=MsgPort* port, D1=MsgPort* replyport → D0=res */
    DosPacket *dp = (DosPacket *)(uintptr_t)cpu->a[0];
    MsgPort *port = (MsgPort *)(uintptr_t)cpu->a[1];
    (void)cpu->d[1];
    if (!dp || !port) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_ACTION_NOT_KNOWN);
        return;
    }
    /* Synchronous dispatch */
    int32_t res = DoPkt(port, dp->dp_Type,
                        dp->dp_Arg1, dp->dp_Arg2, dp->dp_Arg3,
                        dp->dp_Arg4, dp->dp_Arg5);
    dp->dp_Res1 = res;
    dp->dp_Res2 = IoErr();
    cpu->d[0] = (uint32_t)res;
}

static void dos_WaitPkt(M68kCPUState *cpu)
{
    /* Amiga: → D0=DosPacket*  (async queue empty in single-threaded mode) */
    cpu->d[0] = 0;
}

static void dos_ReplyPkt(M68kCPUState *cpu)
{
    /* Amiga: A0=DosPacket*, D0=res1, D1=res2 */
    DosPacket *dp = (DosPacket *)(uintptr_t)cpu->a[0];
    if (dp) {
        dp->dp_Res1 = (int32_t)cpu->d[0];
        dp->dp_Res2 = (int32_t)cpu->d[1];
    }
}

/* =========================================================================
 * Path handling (AddPart / CompareNames)
 * ========================================================================= */

static void dos_AddPart(M68kCPUState *cpu)
{
    /* Amiga: D1=STRPTR dirname, D2=STRPTR filename, D3=ULONG size → D0=BOOL */
    uint32_t dir_ptr = cpu->d[1];
    uint32_t file_ptr = cpu->d[2];
    uint32_t max_size = cpu->d[3];
    if (dir_ptr >= GUEST_RAM_SIZE || file_ptr >= GUEST_RAM_SIZE || max_size < 2) {
        cpu->d[0] = (uint32_t)DOSFALSE;
        return;
    }

    /* Read dirname */
    char dir[128];
    int i = 0;
    while (i < 127 && dir_ptr + i < GUEST_RAM_SIZE && g_ram[dir_ptr + i]) {
        dir[i] = (char)g_ram[dir_ptr + i]; i++;
    }
    dir[i] = '\0';

    /* Read filename */
    char file[128];
    i = 0;
    while (i < 127 && file_ptr + i < GUEST_RAM_SIZE && g_ram[file_ptr + i]) {
        file[i] = (char)g_ram[file_ptr + i]; i++;
    }
    file[i] = '\0';

    /* If dirname doesn't end with / or :, append / */
    int dir_len = 0;
    while (dir[dir_len]) dir_len++;
    int need_slash = 0;
    if (dir_len > 0 && dir[dir_len - 1] != ':' && dir[dir_len - 1] != '/')
        need_slash = 1;

    int file_len = 0;
    while (file[file_len]) file_len++;

    if ((uint32_t)(dir_len + need_slash + file_len + 1) > max_size) {
        cpu->d[0] = (uint32_t)DOSFALSE;
        return;
    }

    uint32_t out_ptr = dir_ptr;
    if (need_slash) {
        g_ram[out_ptr + dir_len] = '/';
        dir_len++;
    }
    for (int j = 0; j < file_len; j++)
        g_ram[out_ptr + dir_len + j] = (uint8_t)file[j];
    g_ram[out_ptr + dir_len + file_len] = '\0';
    cpu->d[0] = (uint32_t)DOSTRUE;
}

static void dos_CompareNames(M68kCPUState *cpu)
{
    /* Amiga: D1=LONG type, D2=STRPTR name1, D3=STRPTR name2 → D0=LONG */
    int32_t type = (int32_t)cpu->d[1];
    uint32_t n1_ptr = cpu->d[2];
    uint32_t n2_ptr = cpu->d[3];

    char name1[128], name2[128];
    int i = 0;
    while (i < 127 && n1_ptr + i < GUEST_RAM_SIZE && g_ram[n1_ptr + i]) {
        name1[i] = (char)g_ram[n1_ptr + i]; i++;
    }
    name1[i] = '\0';

    i = 0;
    while (i < 127 && n2_ptr + i < GUEST_RAM_SIZE && g_ram[n2_ptr + i]) {
        name2[i] = (char)g_ram[n2_ptr + i]; i++;
    }
    name2[i] = '\0';

    int ci = (type != 0);
    int j = 0;
    while (name1[j] && name2[j]) {
        char c1 = name1[j];
        char c2 = name2[j];
        if (ci) {
            if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
            if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        }
        if (c1 != c2) {
            cpu->d[0] = (c1 < c2) ? (uint32_t)-1 : 1;
            return;
        }
        j++;
    }
    if (name1[j] == name2[j]) {
        cpu->d[0] = 0;
    } else if (name1[j] == '\0') {
        cpu->d[0] = (uint32_t)-1;
    } else {
        cpu->d[0] = 1;
    }
}

/* =========================================================================
 * Date/Time (StrToDate)
 * ========================================================================= */

static void dos_StrToDate(M68kCPUState *cpu)
{
    /* Amiga: A0=struct DateTime *datetime → D0=BOOL
     *
     * DateTime layout:
     *  0 : DateStamp  (12 bytes)
     * 12 : ULONG dat_Format
     * 16 : ULONG dat_Flags
     * 20 : APTR dat_StrDay
     * 24 : APTR dat_StrDate
     * 28 : APTR dat_StrTime
     */
    uint32_t dt_ptr = cpu->a[0];
    if (dt_ptr + 32 > GUEST_RAM_SIZE) {
        cpu->d[0] = (uint32_t)DOSFALSE;
        return;
    }

    uint32_t str_date_ptr = guest_read_be32(dt_ptr + 24);
    uint32_t str_time_ptr = guest_read_be32(dt_ptr + 28);
    uint32_t format = guest_read_be32(dt_ptr + 12);

    int day = 1, month = 1, year = 78;
    int hour = 0, min = 0, sec = 0;

    /* Parse date string */
    if (str_date_ptr && str_date_ptr + 12 < GUEST_RAM_SIZE) {
        char ds[32];
        int k = 0;
        while (k < 31 && str_date_ptr + k < GUEST_RAM_SIZE && g_ram[str_date_ptr + k]) {
            ds[k] = (char)g_ram[str_date_ptr + k]; k++;
        }
        ds[k] = '\0';

        if (format == 1) { /* FORMAT_INTL / USA: MM-DD-YY */
            int v[3] = {0,0,0};
            int vi = 0, val = 0;
            for (int j = 0; ds[j] && vi < 3; j++) {
                char ch = ds[j];
                if (ch >= '0' && ch <= '9') {
                    val = val * 10 + (ch - '0');
                } else if (ch == '-' || ch == '/' || ch == '.') {
                    v[vi++] = val; val = 0;
                }
            }
            if (vi < 3) v[vi] = val;
            month = v[0]; day = v[1]; year = v[2];
        } else {
            /* FORMAT_DOS: DD-MMM-YY */
            day = (ds[0] - '0') * 10 + (ds[1] - '0');
            year = (ds[7] - '0') * 10 + (ds[8] - '0');
            char mon[4] = {0,0,0,0};
            mon[0] = ds[3]; mon[1] = ds[4]; mon[2] = ds[5];
            const char *mns[] = {"jan","feb","mar","apr","may","jun","jul","aug","sep","oct","nov","dec"};
            for (int m = 0; m < 12; m++) {
                char a = mon[0]; if (a >= 'A' && a <= 'Z') a += 32;
                char b = mns[m][0];
                if (a == b && mon[1] == mns[m][1] && mon[2] == mns[m][2]) {
                    month = m + 1; break;
                }
            }
        }
    }

    /* Parse time string */
    if (str_time_ptr && str_time_ptr + 10 < GUEST_RAM_SIZE) {
        char ts[16];
        int k = 0;
        while (k < 15 && str_time_ptr + k < GUEST_RAM_SIZE && g_ram[str_time_ptr + k]) {
            ts[k] = (char)g_ram[str_time_ptr + k]; k++;
        }
        ts[k] = '\0';
        hour = (ts[0] - '0') * 10 + (ts[1] - '0');
        min  = (ts[3] - '0') * 10 + (ts[4] - '0');
        sec  = (ts[6] - '0') * 10 + (ts[7] - '0');
    }

    /* Validate */
    if (day < 1 || day > 31 || month < 1 || month > 12 || year < 0 || year > 99 ||
        hour < 0 || hour > 23 || min < 0 || min > 59 || sec < 0 || sec > 59) {
        cpu->d[0] = (uint32_t)DOSFALSE;
        return;
    }

    /* Convert to DateStamp (days since 1-Jan-1978, minutes, ticks) */
    static const int16_t mdays[12] = {0,31,59,90,120,151,181,212,243,273,304,334};
    int y = year + 1900;
    if (y < 1978) y += 100;
    int leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 1 : 0;
    int days = (y - 1978) * 365;
    for (int ly = 1978; ly < y; ly++) {
        if ((ly % 4 == 0 && (ly % 100 != 0 || ly % 400 == 0))) days++;
    }
    days += mdays[month - 1];
    if (month > 2) days += leap;
    days += (day - 1);

    int total_ticks = ((hour * 60 + min) * 60 + sec) * 50;
    int minutes = total_ticks / 3000;
    int ticks = total_ticks % 3000;

    guest_write_be32(dt_ptr + 0, (uint32_t)days);
    guest_write_be32(dt_ptr + 4, (uint32_t)minutes);
    guest_write_be32(dt_ptr + 8, (uint32_t)ticks);

    cpu->d[0] = (uint32_t)DOSTRUE;
}

/* =========================================================================
 * Signals (CheckSignal / WaitForChar)
 * ========================================================================= */

static void dos_CheckSignal(M68kCPUState *cpu)
{
    /* AmigaOS: D0=ULONG mask → D0=ULONG received (and clears matched bits)
     *
     * Returns the subset of pending signals that intersect with mask, then
     * clears those bits from tc_SigRecvd so they are only reported once.
     * This is the non-blocking variant of exec.library Wait().
     */
    uint32_t mask = cpu->d[0];
    UaosTask *cur = Task_Current();
    if (!cur) { cpu->d[0] = 0; return; }

    /* Atomically read and clear matching bits */
    __asm__ volatile ("cli");
    uint32_t received = cur->tc_SigRecvd & mask;
    cur->tc_SigRecvd &= ~received;  /* Clear only the bits that were matched */
    __asm__ volatile ("sti");

    cpu->d[0] = received;
}

static void dos_WaitForChar(M68kCPUState *cpu)
{
    /* Amiga: D1=BPTR file, D2=ULONG timeout → D0=BOOL (0=timeout, -1=available) */
    uint32_t fh = cpu->d[1];
    (void)cpu->d[2];
    if (fh == DOS_STDIN_BPTR) {
        cpu->d[0] = (uint32_t)DOSTRUE;
    } else {
        cpu->d[0] = (uint32_t)DOSFALSE;
    }
}

/* =========================================================================
 * Advanced locks (NameFromLock / LockRecord / UnLockRecord)
 * ========================================================================= */

static void dos_NameFromLock(M68kCPUState *cpu)
{
    /* Amiga: D1=BPTR lock, D2=STRPTR buffer, D3=LONG len → D0=BOOL */
    uint32_t lock = cpu->d[1];
    uint32_t buf = cpu->d[2];
    int32_t len = (int32_t)cpu->d[3];

    if (buf >= GUEST_RAM_SIZE || len < 2) {
        cpu->d[0] = (uint32_t)DOSFALSE;
        return;
    }

    uint32_t handle = 0;
    if (!guest_read_filelock(lock, &handle, NULL) || handle == 0) {
        cpu->d[0] = (uint32_t)DOSFALSE;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    HandleEntry *ent = HandleTable_GetLockEntry(handle, NULL);
    if (!ent || ent->type != HTYPE_LOCK) {
        cpu->d[0] = (uint32_t)DOSFALSE;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    const char *path = ent->path;
    int i = 0;
    while (path[i] && i < len - 1 && buf + i < GUEST_RAM_SIZE) {
        g_ram[buf + i] = (uint8_t)path[i];
        i++;
    }
    g_ram[buf + i] = '\0';
    cpu->d[0] = (uint32_t)DOSTRUE;
}

static void dos_LockRecord(M68kCPUState *cpu)
{
    /* Amiga: D1=BPTR fh, D2=offset, D3=length, D4=mode, D5=timeout → D0=BOOL */
    /* Synchronous single-threaded system — records are never contested */
    (void)cpu;
    cpu->d[0] = (uint32_t)DOSTRUE;
}

static void dos_UnLockRecord(M68kCPUState *cpu)
{
    /* Amiga: D1=BPTR fh, D2=offset, D3=length */
    (void)cpu;
}

/* =========================================================================
 * CLI (GetConsoleTask / SetConsoleTask)
 * ========================================================================= */

static uint32_t g_console_task = 0;

static void dos_GetConsoleTask(M68kCPUState *cpu)
{
    /* Amiga: → D0=struct MsgPort* */
    cpu->d[0] = g_console_task;
}

static void dos_SetConsoleTask(M68kCPUState *cpu)
{
    /* Amiga: D1=struct MsgPort* */
    g_console_task = cpu->d[1];
}

/* =========================================================================
 * CreateSegList stub
 * ========================================================================= */

static void dos_CreateSegList(M68kCPUState *cpu)
{
    /* AmigaOS (3.9+): A0=APTR codeStart, D0=ULONG codeSize → D0=BPTR seglist
     *
     * Wraps a raw M68k code buffer in a single-segment seglist so it can be
     * passed to CreateProc() or RunCommand().  The seglist header (4 bytes)
     * holds the next-BPTR (0 = last) followed by the code.
     *
     * This is not part of the original AmigaOS 3.1 API (it was added later)
     * but several third-party tools try to call it.  We allocate a block from
     * the free-list allocator, copy the code, and return a BPTR.
     */
    uint32_t code_ptr  = cpu->a[0];
    uint32_t code_size = cpu->d[0];

    if (!code_ptr || !code_size || code_ptr + code_size > GUEST_RAM_SIZE) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    /* Allocate: 4-byte header + code */
    uint32_t total = 4 + code_size;
    uint32_t blk = heap_alloc_fl(total);
    if (!blk) {
        cpu->d[0] = 0;
        SetIoErr(ERROR_NO_FREE_STORE);
        return;
    }

    /* next-BPTR = 0 (single segment) */
    guest_write_be32(blk + 0, 0);

    /* Copy code into the block */
    for (uint32_t i = 0; i < code_size && blk + 4 + i < GUEST_RAM_SIZE; i++)
        g_ram[blk + 4 + i] = g_ram[code_ptr + i];

    /* Track so UnLoadSeg can free it */
    seglist_track(blk >> 2, blk);

    cpu->d[0] = blk >> 2;  /* return BPTR */
}

/* =========================================================================
 * Function table — indices must match the ILLEGAL handler's DOS_* constants
 * ========================================================================= */

static void *dos_funcs[] = {
    dos_Output,        /* index 1  */
    dos_Write,         /* index 2  */
    dos_Open,          /* index 3  */
    dos_Close,         /* index 4  */
    dos_Read,          /* index 5  */
    dos_Exit,          /* index 6  */
    dos_IoErr,         /* index 7  */
    dos_Input,         /* index 8  */
    dos_VFPrintf,      /* index 9  */
    dos_FPuts,         /* index 10 */
    dos_PutStr,        /* index 11 */
    dos_VPrintf,       /* index 12 */
    dos_VPrintf,       /* index 13 — alias */
    dos_VFWritef,      /* index 14 */
    dos_ReadArgs,      /* index 15 */
    dos_GetArgStr,     /* index 16 */
    dos_IsInteractive, /* index 17 */
    dos_DeleteFile,    /* index 18 */
    dos_Rename,        /* index 19 */
    dos_SetProtection, /* index 20 */
    dos_GetVar,        /* index 21 */
    dos_SetVar,        /* index 22 */
    dos_Seek,          /* index 23 */
    dos_Lock,          /* index 24 */
    dos_Unlock,        /* index 25 */
    dos_Examine,       /* index 26 */
    dos_ExamineNext,   /* index 27 */
    dos_CreateDir,     /* index 28 */
    dos_DupLock,       /* index 29 */
    dos_Parent,        /* index 30 */
    dos_DateStamp,     /* index 31 */
    dos_Delay,         /* index 32 */
    dos_DateToStr,     /* index 33 */
    dos_ParsePattern,       /* index 34 */
    dos_ParsePatternNoCase, /* index 35 */
    dos_MatchPattern,       /* index 36 */
    dos_MatchPatternNoCase, /* index 37 */
    dos_LoadSeg,            /* index 38 */
    dos_UnLoadSeg,          /* index 39 */
    dos_CreateProc,         /* index 40 */
    dos_SystemTagList,      /* index 41 */
    dos_RunCommand,         /* index 42 */
    dos_SendPkt,            /* index 43 */
    dos_WaitPkt,            /* index 44 */
    dos_ReplyPkt,           /* index 45 */
    dos_AddPart,            /* index 46 */
    dos_CompareNames,       /* index 47 */
    dos_StrToDate,          /* index 48 */
    dos_CheckSignal,        /* index 49 */
    dos_WaitForChar,        /* index 50 */
    dos_NameFromLock,       /* index 51 */
    dos_LockRecord,         /* index 52 */
    dos_UnLockRecord,       /* index 53 */
    dos_GetConsoleTask,     /* index 54 */
    dos_SetConsoleTask,     /* index 55 */
    dos_CreateSegList,      /* index 56 */
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

/* =========================================================================
 * Glue entry points called from uaos_m68k_glue.c for exec.library
 * AllocMem and FreeMem.  These bridge the two translation units.
 * ========================================================================= */

void dos_AllocMem_glue(uint32_t size, uint32_t reqs, uint32_t *out_addr)
{
    if (size == 0) { *out_addr = 0; return; }

    uint32_t addr = 0;
    if (reqs & (MEMF_CHIP | MEMF_DMA | MEMF_24BITDMA)) {
        addr = heap_alloc_fl_chip(size);
    } else if (reqs & MEMF_FAST) {
        addr = heap_alloc_fl_fast(size);
    } else {
        addr = heap_alloc_fl_fast(size);
        if (!addr) addr = heap_alloc_fl_chip(size);
    }

    if (!addr) SetIoErr(ERROR_NO_FREE_STORE);
    *out_addr = addr;
}

void dos_FreeMem_glue(uint32_t addr, uint32_t size)
{
    (void)size; /* our free-list tracks block sizes internally */
    heap_free_fl(addr);
}
