/* resident_cmd.c — UAOS Resident Command Registry Implementation
 *
 * Manages in-memory storage of frequently-used command binaries.
 */

#include "resident_cmd.h"
#include "native_cmd.h"
#include "cmd_internal.h"
#include "../dos/vfs.h"
#include "../exec/uaos_binary.h"
#include "../../emulation/uaos_emu.h"
#include <string.h>

/* MAX_SCRIPT_SIZE is defined in shell_win.c; define here for standalone use */
#ifndef MAX_SCRIPT_SIZE
#define MAX_SCRIPT_SIZE 4096
#endif

/* Static pool of memory for resident commands (total 256KB for all residents) */
#define RESIDENT_POOL_SIZE (256 * 1024)
static uint8_t g_resident_pool[RESIDENT_POOL_SIZE];
static uint32_t g_resident_pool_used = 0;

/* Registry of resident commands */
static ResidentEntry g_registry[MAX_RESIDENT_CMDS];

/* Internal helpers */
static int resident_strcpy(char *dst, const char *src, int max)
{
    int i = 0;
    while (i < max - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
    return i;
}

static int resident_streql(const char *a, const char *b)
{
    int i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == b[i];
}

static int resident_streql_ci(const char *a, const char *b)
{
    int i = 0;
    while (a[i] && b[i]) {
        char ac = a[i];
        char bc = b[i];
        if (ac >= 'A' && ac <= 'Z') ac += 32;
        if (bc >= 'A' && bc <= 'Z') bc += 32;
        if (ac != bc) return 0;
        i++;
    }
    char ac = a[i];
    char bc = b[i];
    if (ac >= 'A' && ac <= 'Z') ac += 32;
    if (bc >= 'A' && bc <= 'Z') bc += 32;
    return ac == bc;
}

/* -------------------------------------------------------------------------
 * Initialization
 * ------------------------------------------------------------------------- */

void Resident_Init(void)
{
    for (int i = 0; i < MAX_RESIDENT_CMDS; i++) {
        g_registry[i].in_use = 0;
        g_registry[i].data = NULL;
        g_registry[i].size = 0;
    }
    g_resident_pool_used = 0;
}

/* -------------------------------------------------------------------------
 * Command management
 * ------------------------------------------------------------------------- */

int Resident_Add(const char *name, const char *path, int permanent)
{
    if (!name || !path || !*name || !*path) return -1;

    /* Check if already resident */
    for (int i = 0; i < MAX_RESIDENT_CMDS; i++) {
        if (g_registry[i].in_use && resident_streql_ci(g_registry[i].name, name)) {
            /* Already resident - update if path is different */
            /* For now, just return success */
            return 0;
        }
    }

    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < MAX_RESIDENT_CMDS; i++) {
        if (!g_registry[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return -1;  /* No free slots */

    /* Open the file */
    VfsFile fh;
    if (!VFS_Open(&fh, path, VFS_READ)) return -1;

    uint32_t file_size = VFS_Size(&fh);
    if (file_size == 0 || file_size > MAX_RESIDENT_SIZE) {
        VFS_Close(&fh);
        return -1;
    }

    /* Check if we have enough pool memory */
    if (g_resident_pool_used + file_size > RESIDENT_POOL_SIZE) {
        VFS_Close(&fh);
        return -1;
    }

    /* Allocate from pool and read file */
    uint8_t *data = &g_resident_pool[g_resident_pool_used];
    uint32_t nread = VFS_Read(&fh, data, file_size);
    VFS_Close(&fh);

    if (nread != file_size) return -1;

    /* Determine type from header */
    int type = RESIDENT_TYPE_UNKNOWN;
    if (file_size >= UAOS_BIN_HEADER_SIZE) {
        if (uaos_bin_check_magic(data)) {
            uint8_t type_byte = data[6];  /* Type byte at offset 6 */
            if (type_byte == UAOS_BIN_TYPE_NATIVE) {
                type = RESIDENT_TYPE_NATIVE;
            } else if (type_byte == UAOS_BIN_TYPE_M68K) {
                type = RESIDENT_TYPE_M68K;
            }
        }
    }
    /* If no valid header or unknown type, treat as script */
    if (type == RESIDENT_TYPE_UNKNOWN) {
        type = RESIDENT_TYPE_SCRIPT;
    }

    /* Fill registry entry */
    g_registry[slot].in_use = 1;
    resident_strcpy(g_registry[slot].name, name, MAX_RESIDENT_NAME);
    g_registry[slot].data = data;
    g_registry[slot].size = file_size;
    g_registry[slot].type = type;
    g_registry[slot].permanent = permanent ? 1 : 0;

    g_resident_pool_used += file_size;

    return 0;
}

int Resident_Remove(const char *name)
{
    if (!name) return -1;

    for (int i = 0; i < MAX_RESIDENT_CMDS; i++) {
        if (g_registry[i].in_use && resident_streql_ci(g_registry[i].name, name)) {
            /* Mark as unused */
            g_registry[i].in_use = 0;
            /* Note: We don't reclaim pool memory for simplicity */
            return 0;
        }
    }
    return -1;  /* Not found */
}

int Resident_Flush(void)
{
    int removed = 0;
    for (int i = 0; i < MAX_RESIDENT_CMDS; i++) {
        if (g_registry[i].in_use && !g_registry[i].permanent) {
            g_registry[i].in_use = 0;
            removed++;
        }
    }
    /* Note: Pool memory is not reclaimed for simplicity */
    return removed;
}

/* -------------------------------------------------------------------------
 * Query and execution
 * ------------------------------------------------------------------------- */

int Resident_Exists(const char *name)
{
    if (!name) return 0;

    for (int i = 0; i < MAX_RESIDENT_CMDS; i++) {
        if (g_registry[i].in_use && resident_streql_ci(g_registry[i].name, name)) {
            return 1;
        }
    }
    return 0;
}

int Resident_GetInfo(const char *name, ResidentEntry *info)
{
    if (!name || !info) return -1;

    for (int i = 0; i < MAX_RESIDENT_CMDS; i++) {
        if (g_registry[i].in_use && resident_streql_ci(g_registry[i].name, name)) {
            resident_strcpy(info->name, g_registry[i].name, MAX_RESIDENT_NAME);
            info->size = g_registry[i].size;
            info->type = g_registry[i].type;
            info->permanent = g_registry[i].permanent;
            info->in_use = 1;
            info->data = NULL;  /* Don't expose actual data pointer */
            return 0;
        }
    }
    return -1;
}

void Resident_List(void (*print_fn)(const char *line))
{
    char buf[128];

    for (int i = 0; i < MAX_RESIDENT_CMDS; i++) {
        if (!g_registry[i].in_use) continue;

        /* Format: "name type size [P]" */
        int bi = 0;

        /* Name */
        int ni = 0;
        while (g_registry[i].name[ni] && bi < 31) {
            buf[bi++] = g_registry[i].name[ni++];
        }
        while (bi < 32) buf[bi++] = ' ';

        /* Type */
        const char *type_str = "UNKNOWN";
        switch (g_registry[i].type) {
            case RESIDENT_TYPE_NATIVE: type_str = "NATIVE "; break;
            case RESIDENT_TYPE_M68K:   type_str = "M68K   "; break;
            case RESIDENT_TYPE_SCRIPT: type_str = "SCRIPT "; break;
        }
        int ti = 0;
        while (type_str[ti] && bi < 40) {
            buf[bi++] = type_str[ti++];
        }

        /* Size */
        bi += 8;  /* Space for size field */

        /* Permanent flag */
        if (g_registry[i].permanent) {
            buf[bi++] = ' ';
            buf[bi++] = 'P';
        }

        buf[bi] = '\0';
        print_fn(buf);
    }
}

int Resident_Run(const char *name, struct NativeCmdCtx *ctx, const char *args)
{
    if (!name || !ctx) return -1;

    /* Find the resident command */
    ResidentEntry *entry = NULL;
    for (int i = 0; i < MAX_RESIDENT_CMDS; i++) {
        if (g_registry[i].in_use && resident_streql_ci(g_registry[i].name, name)) {
            entry = &g_registry[i];
            break;
        }
    }
    if (!entry) return -1;

    /* Execute based on type */
    switch (entry->type) {
        case RESIDENT_TYPE_NATIVE: {
            /* For native commands, run directly from native command table */
            /* The command name in the registry is the native command name */
            return NativeCmd_Run(entry->name, ctx, args);
        }

        case RESIDENT_TYPE_M68K: {
            /* Run M68K binary using emulator */
            if (entry->size > UAOS_BIN_HEADER_SIZE) {
                const uint8_t *payload = entry->data + UAOS_BIN_HEADER_SIZE;
                uint32_t payload_size = entry->size - UAOS_BIN_HEADER_SIZE;
                /* Parse args into argv array */
                const char *argv[16];
                int argc = 0;
                argv[argc++] = entry->name;
                /* Simple args parsing - just pass args as single argument for now */
                if (args && *args) {
                    argv[argc++] = args;
                }
                argv[argc] = NULL;
                return UAOS_Emu_LoadAndRun(payload, payload_size, argv, ctx->shell, (UAOS_PrintFn)ctx->print);
            }
            return -1;
        }

        case RESIDENT_TYPE_SCRIPT: {
            /* Run as a text script */
            if (ctx->run_script) {
                /* Copy script to a null-terminated buffer */
                static char script_buf[MAX_SCRIPT_SIZE];
                uint32_t copy_size = entry->size;
                if (copy_size >= MAX_SCRIPT_SIZE) copy_size = MAX_SCRIPT_SIZE - 1;
                for (uint32_t i = 0; i < copy_size; i++) {
                    script_buf[i] = (char)entry->data[i];
                }
                script_buf[copy_size] = '\0';
                ctx->run_script(ctx->shell_extra, script_buf);
                return 0;
            }
            return -1;
        }

        default:
            return -1;
    }
}

/* -------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

ResidentEntry* Resident_GetRegistry(void)
{
    return g_registry;
}
