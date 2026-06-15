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

static uint32_t heap_alloc(uint32_t size)
{
    size = (size + 3) & ~3u;
    if (g_uaos_heap_ptr + size > GUEST_RAM_SIZE) return 0;
    uint32_t addr = g_uaos_heap_ptr;
    g_uaos_heap_ptr += size;
    for (uint32_t i = 0; i < size; i++) g_ram[addr + i] = 0;
    return addr;
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
    int32_t handle = DoPkt(port, action, (int32_t)full_name, (int32_t)mode, 0, 0, 0);
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

    int32_t res = DoPkt(port, ACTION_DELETE_OBJECT, (int32_t)full_name, 0, 0, 0, 0);
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
                        (int32_t)old_full, (int32_t)new_full, 0, 0, 0);
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

    int32_t res = DoPkt(port, ACTION_SET_PROTECT, (int32_t)full_name, mask, 0, 0, 0);
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

    int32_t handle = DoPkt(port, ACTION_LOCATE_OBJECT, (int32_t)full_name, mode, 0, 0, 0);
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
                        (int32_t)(g_ram + fib_ptr), 0, 0, 0);
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
                        (int32_t)(g_ram + fib_ptr), 0, 0, 0);
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

    int32_t res = DoPkt(port, ACTION_CREATE_DIR, (int32_t)full_name, 0, 0, 0, 0);
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
