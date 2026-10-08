/* exec_file.c — Generic file launcher (no shell instance required)
 *
 * Opens a VFS file, reads the 32-byte UAOS header, then:
 *   NATIVE  -> NativeCmd_Run(name, minimal_ctx, args)
 *   M68K    -> UAOS_Emu_LoadAndRun with payload bytes
 *   No header -> return -2 (not executed)
 */

#include "exec_file.h"
#include "native_cmd.h"
#include "../exec/uaos_binary.h"
#include "../exec/elf64_loader.h"
#include "../exec/icon_lib.h"
#include "../exec/icon_def.h"
#include "../../emulation/uaos_emu.h"
#include "../exec/task.h"
#include "../dos/vfs.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* No-op print callback for commands that don't need output */
static void exec_nop_print(void *shell, const char *line)
{
    (void)shell;
    (void)line;
}

/* Fallback line printer for X64 binaries launched without a shell instance. */
static void exec_kprint_line(void *shell, const char *line)
{
    (void)shell;
    extern void kprintbuf(const char *s, size_t len);
    int i = 0;
    while (line[i]) i++;
    kprintbuf(line, (size_t)i);
}

/* Build a minimal NativeCmdCtx for non-shell callers.
 * Safe for commands that only open windows or don't use ctx at all. */
static NativeCmdCtx exec_make_ctx(void)
{
    NativeCmdCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.shell          = NULL;
    ctx.print          = exec_nop_print;
    ctx.cwd            = "";
    ctx.path           = "";
    ctx.shell_extra    = NULL;
    ctx.set_fdisk_mode = NULL;
    ctx.set_vim_mode   = NULL;
    ctx.loadwb         = NULL;
    ctx.clear_history  = NULL;
    ctx.is_builtin     = NULL;
    ctx.dispatch_line  = NULL;
    ctx.run_script     = NULL;
    ctx.yield_ms       = NULL;
    ctx.read_key       = NULL;
    ctx.read_line      = NULL;
    ctx.set_ask_mode   = NULL;
    ctx.visible_rows   = 0;
    ctx.enum_tasks     = NULL;
    ctx.set_env        = NULL;
    ctx.change_task_pri = NULL;
    ctx.break_pending  = NULL;
    return ctx;
}

/* Static payload buffer for M68K binaries loaded from VFS (max 2 MB) */
#define EXEC_MAX_PAYLOAD (2 * 1024 * 1024)
static uint8_t g_exec_payload[EXEC_MAX_PAYLOAD];

/* Extract a program name from a VFS path (last component, strip extension). */
static void exec_basename(const char *path, char *out, int max)
{
    const char *name = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == ':' || *p == '\\') name = p + 1;
    }
    int i = 0;
    while (i < max - 1 && name[i] && name[i] != '.') {
        out[i] = name[i];
        i++;
    }
    out[i] = '\0';
}

/* Build a NULL-terminated argv from a program name and raw argument string.
 * Both the pointer array and the token storage are written to the caller's
 * static buffers. */
static void exec_build_argv(const char *name, const char *args,
                            const char **argv, char *argstore, int argstore_max)
{
    argv[0] = name;
    int argc = 1;
    if (args && *args) {
        int ai = 0;
        while (*args && ai < argstore_max - 1)
            argstore[ai++] = *args++;
        argstore[ai] = '\0';
        char *tok = argstore;
        while (*tok && argc < 16) {
            while (*tok == ' ') tok++;
            if (!*tok) break;
            argv[argc++] = tok;
            while (*tok && *tok != ' ') tok++;
            if (*tok == ' ') *tok++ = '\0';
        }
    }
    argv[argc] = NULL;
}

int ExecFile_Run(const char *path, const char *args)
{
    if (!path || !*path) return -1;

    VfsFile fh;
    if (!VFS_Open(&fh, path, VFS_READ)) {
        return -1;
    }

    uint32_t file_size = VFS_Size(&fh);
    if (file_size < 4) {
        VFS_Close(&fh);
        return -2;
    }

    /* Read the first 4 bytes to detect either a UAOS wrapper or raw Hunk. */
    uint8_t first4[4];
    if (VFS_Read(&fh, first4, 4) != 4) {
        VFS_Close(&fh);
        return -2;
    }
    uint32_t magic = uaos_bin_u32(first4);

    if (magic == UAOS_BIN_MAGIC) {
        if (file_size < UAOS_BIN_HEADER_SIZE) {
            VFS_Close(&fh);
            return -2;
        }

        uint8_t hdr[UAOS_BIN_HEADER_SIZE];
        memcpy(hdr, first4, 4);
        if (VFS_Read(&fh, hdr + 4, UAOS_BIN_HEADER_SIZE - 4) != UAOS_BIN_HEADER_SIZE - 4) {
            VFS_Close(&fh);
            return -2;
        }

        uint16_t type         = uaos_bin_u16(hdr + 4);
        uint32_t payload_size = uaos_bin_u32(hdr + 8);

        /* Extract name from header (NUL-padded, 16 bytes at offset 12) */
        char bin_name[17];
        int ni = 0;
        while (ni < 16 && hdr[12 + ni]) {
            bin_name[ni] = (char)hdr[12 + ni];
            ni++;
        }
        bin_name[ni] = '\0';

        if (type == UAOS_BIN_TYPE_NATIVE) {
            VFS_Close(&fh);
            NativeCmdCtx ctx = exec_make_ctx();
            if (NativeCmd_Run(bin_name, &ctx, args ? args : "") == 0) {
                return 0;
            }
            return -2;
        }

        if (type == UAOS_BIN_TYPE_M68K) {
            if (payload_size == 0 || payload_size > EXEC_MAX_PAYLOAD) {
                VFS_Close(&fh);
                return -2;
            }
            uint32_t n = VFS_Read(&fh, g_exec_payload, payload_size);
            VFS_Close(&fh);
            if (n != payload_size) return -2;

            static const char *m68k_argv[18];
            static char m68k_argstore[256];
            exec_build_argv(bin_name, args, m68k_argv, m68k_argstore, 256);

            UAOS_Emu_SetCwd("");
            UaosTask *t = Task_CreateM68k(bin_name, 0,
                                            g_exec_payload, payload_size,
                                            m68k_argv, NULL);
            (void)t;  /* task is now running in background */
            return 0;
        }

        if (type == UAOS_BIN_TYPE_X64) {
            if (payload_size == 0 || payload_size > EXEC_MAX_PAYLOAD) {
                VFS_Close(&fh);
                return -2;
            }
            uint32_t n = VFS_Read(&fh, g_exec_payload, payload_size);
            VFS_Close(&fh);
            if (n != payload_size) return -2;

            static const char *x64_argv[18];
            static char x64_argstore[256];
            exec_build_argv(bin_name, args, x64_argv, x64_argstore, 256);

            ELF64Result result;
            if (ELF64_Load(g_exec_payload, payload_size, x64_argv, &result) != 0) {
                return -2;
            }
            int8_t pri = 0;
            UaosTask *cur = Task_Current();
            if (cur) pri = cur->ln_Pri;
            UaosTask *t = Task_CreateX64(bin_name, pri,
                                         result.entry_rip, result.initial_rsp,
                                         "",
                                         exec_kprint_line,
                                         NULL);
            if (t) {
                /* Hand the loaded image and user stack to the task so
                 * Task_Exit reclaims them when it dies. */
                ELF64_HeapOwn(t, (void *)(uintptr_t)result.image_base);
                ELF64_HeapOwn(t, (void *)(uintptr_t)result.initial_rsp);
            } else {
                ELF64_HeapFreeRange(result.image_base, result.image_size);
                ELF64_HeapFreeRange(result.initial_rsp, 1);
            }
            return 0;
        }

        /* Unknown type */
        VFS_Close(&fh);
        return -2;
    }

    if (magic == UAOS_BIN_HUNK_MAGIC) {
        /* Raw Amiga Hunk binary — wrap it as a background M68k task. */
        if (file_size > EXEC_MAX_PAYLOAD) {
            VFS_Close(&fh);
            return -2;
        }
        VFS_Seek(&fh, 0);
        uint32_t n = VFS_Read(&fh, g_exec_payload, file_size);
        VFS_Close(&fh);
        if (n != file_size) return -2;

        static char name_store[16];
        static const char *m68k_argv[18];
        static char m68k_argstore[256];
        exec_basename(path, name_store, sizeof(name_store));
        exec_build_argv(name_store, args, m68k_argv, m68k_argstore, 256);

        UAOS_Emu_SetCwd("");
        UaosTask *t = Task_CreateM68k(name_store, 0,
                                        g_exec_payload, file_size,
                                        m68k_argv, NULL);
        (void)t;
        return 0;
    }

    VFS_Close(&fh);
    return -2;
}

/* -------------------------------------------------------------------------
 * Workbench launch path (UAOS-253)
 * ------------------------------------------------------------------------- */

/* Leaf name of a VFS path, preserving the extension. */
static void wb_leaf(const char *path, char *out, int max)
{
    const char *name = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == ':' || *p == '\\') name = p + 1;
    }
    int i = 0;
    while (i < max - 1 && name[i]) { out[i] = name[i]; i++; }
    out[i] = '\0';
}

/* Directory portion of a VFS path, keeping the trailing '/' or ':'.
 * "SYS:Tools/Foo" -> "SYS:Tools/"; "SYS:Foo" -> "SYS:". */
static void wb_dirname(const char *path, char *out, int max)
{
    const char *last = NULL;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == ':') last = p;
    }
    int i = 0;
    if (last) {
        const char *p = path;
        while (p <= last && i < max - 1) { out[i++] = *p++; }
    } else {
        /* Bare name — use the volume root as the drawer. */
        out[i++] = 'S'; if (i < max - 1) out[i++] = 'Y';
        if (i < max - 1) out[i++] = 'S';
        if (i < max - 1) out[i++] = ':';
    }
    out[i] = '\0';
}

/* Launch `path` with Workbench semantics.  extra_paths are additional
 * selected icons (shift-click) that become WBArg[1..n] after any project
 * argument. */
int ExecFile_RunWB(const char *path,
                   const char **extra_paths, int num_extra)
{
    if (!path || !*path) return -1;

    /* Parse the clicked icon — controls type, stack, tool window, and
     * (for projects) the default tool. */
    static IconMeta meta;
    int has_icon = Icon_LoadMeta(path, &meta);
    if (!has_icon) memset(&meta, 0, sizeof(meta));

    /* Resolve the real tool: a project icon's do_DefaultTool, otherwise
     * the file itself.  Relative default tools resolve inside the
     * project's drawer (classic WB behaviour). */
    static char tool_path[160];
    int is_project = has_icon && meta.type == WB_PROJECT;
    if (is_project) {
        if (!meta.default_tool[0]) return -2;
        const char *dt = meta.default_tool;
        int has_vol = 0;
        for (const char *q = dt; *q; q++) if (*q == ':') { has_vol = 1; break; }
        int i = 0;
        if (!has_vol) {
            char dir[96];
            wb_dirname(path, dir, sizeof(dir));
            for (int j = 0; dir[j] && i < 158; j++) tool_path[i++] = dir[j];
        }
        for (int j = 0; dt[j] && i < 159; j++) tool_path[i++] = dt[j];
        tool_path[i] = '\0';
    } else {
        int i = 0;
        while (path[i] && i < 159) { tool_path[i] = path[i]; i++; }
        tool_path[i] = '\0';
    }

    /* The tool's own icon supplies stack size / tool window when the
     * clicked icon didn't specify them (projects rarely carry them). */
    static IconMeta tmeta;
    memset(&tmeta, 0, sizeof(tmeta));
    Icon_LoadMeta(tool_path, &tmeta);
    int32_t stack_size = meta.stack_size ? meta.stack_size : tmeta.stack_size;
    const char *toolwin = meta.tool_window[0] ? meta.tool_window
                                            : tmeta.tool_window;

    /* ---- Inspect the tool binary ---- */
    VfsFile fh;
    if (!VFS_Open(&fh, tool_path, VFS_READ)) return -1;
    uint32_t file_size = VFS_Size(&fh);
    if (file_size < 4 || file_size > EXEC_MAX_PAYLOAD) {
        VFS_Close(&fh);
        return -2;
    }
    uint8_t first4[4];
    if (VFS_Read(&fh, first4, 4) != 4) { VFS_Close(&fh); return -2; }
    uint32_t magic = uaos_bin_u32(first4);
    uint32_t payload_size = file_size;
    static char bin_name[17];
    if (magic == UAOS_BIN_MAGIC) {
        if (file_size < UAOS_BIN_HEADER_SIZE) { VFS_Close(&fh); return -2; }
        uint8_t hdr[UAOS_BIN_HEADER_SIZE];
        memcpy(hdr, first4, 4);
        if (VFS_Read(&fh, hdr + 4, UAOS_BIN_HEADER_SIZE - 4)
                != UAOS_BIN_HEADER_SIZE - 4) { VFS_Close(&fh); return -2; }
        uint16_t type = uaos_bin_u16(hdr + 4);
        payload_size  = uaos_bin_u32(hdr + 8);
        int ni = 0;
        while (ni < 16 && hdr[12 + ni]) { bin_name[ni] = (char)hdr[12 + ni]; ni++; }
        bin_name[ni] = '\0';
        if (type == UAOS_BIN_TYPE_NATIVE) {
            /* Native commands have no WB environment — launch normally. */
            VFS_Close(&fh);
            return ExecFile_Run(tool_path, "");
        }
        if (type != UAOS_BIN_TYPE_M68K) { VFS_Close(&fh); return -2; }
        if (!payload_size || payload_size > EXEC_MAX_PAYLOAD ||
            payload_size > file_size - UAOS_BIN_HEADER_SIZE) {
            VFS_Close(&fh); return -2;
        }
    } else if (magic == UAOS_BIN_HUNK_MAGIC) {
        wb_leaf(tool_path, bin_name, sizeof(bin_name));
    } else {
        VFS_Close(&fh);
        return -2;
    }

    VFS_Seek(&fh, (magic == UAOS_BIN_MAGIC) ? UAOS_BIN_HEADER_SIZE : 0);
    uint32_t n = VFS_Read(&fh, g_exec_payload, payload_size);
    VFS_Close(&fh);
    if (n != payload_size) return -2;

    /* ---- Fill the pending WB launch descriptor ---- */
    memset(&g_wb_pending, 0, sizeof(g_wb_pending));
    g_wb_pending.used = 1;
    wb_dirname(tool_path, g_wb_pending.tool_dir, sizeof(g_wb_pending.tool_dir));
    wb_leaf(tool_path, g_wb_pending.tool_name, sizeof(g_wb_pending.tool_name));
    {
        int i = 0;
        while (i < (int)sizeof(g_wb_pending.toolwindow) - 1 && toolwin[i]) {
            g_wb_pending.toolwindow[i] = toolwin[i];
            i++;
        }
        g_wb_pending.toolwindow[i] = '\0';
    }
    g_wb_pending.stack_size = stack_size;

    int e = 0;
    if (is_project) {
        wb_dirname(path, g_wb_pending.extra_dir[e],
                   sizeof(g_wb_pending.extra_dir[e]));
        wb_leaf(path, g_wb_pending.extra_name[e],
                sizeof(g_wb_pending.extra_name[e]));
        e++;
    }
    for (int i = 0; i < num_extra && e < WB_MAX_ARGS; i++) {
        if (!extra_paths[i] || !extra_paths[i][0]) continue;
        wb_dirname(extra_paths[i], g_wb_pending.extra_dir[e],
                   sizeof(g_wb_pending.extra_dir[e]));
        wb_leaf(extra_paths[i], g_wb_pending.extra_name[e],
                sizeof(g_wb_pending.extra_name[e]));
        e++;
    }
    g_wb_pending.num_extra = e;

    /* The launcher's cwd becomes the task cwd — the tool's drawer. */
    char cwd[96];
    wb_dirname(tool_path, cwd, sizeof(cwd));
    UAOS_Emu_SetCwd(cwd);

    static const char *wb_argv[2];
    wb_argv[0] = bin_name[0] ? bin_name : g_wb_pending.tool_name;
    wb_argv[1] = NULL;

    UaosTask *t = Task_CreateM68k(wb_argv[0], 0,
                                    g_exec_payload, payload_size,
                                    wb_argv, NULL);
    if (!t) { g_wb_pending.used = 0; return -2; }
    return 0;
}
