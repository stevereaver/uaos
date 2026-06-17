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
#include "../../emulation/uaos_emu.h"
#include "../exec/task.h"
#include "../dos/vfs.h"
#include <stdint.h>
#include <stddef.h>

/* No-op print callback for commands that don't need output */
static void exec_nop_print(void *shell, const char *line)
{
    (void)shell;
    (void)line;
}

/* Build a minimal NativeCmdCtx for non-shell callers.
 * Safe for commands that only open windows or don't use ctx at all. */
static NativeCmdCtx exec_make_ctx(void)
{
    NativeCmdCtx ctx;
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
    return ctx;
}

/* Static payload buffer for M68K binaries loaded from VFS (max 512 KB) */
#define EXEC_MAX_PAYLOAD (512 * 1024)
static uint8_t g_exec_payload[EXEC_MAX_PAYLOAD];

int ExecFile_Run(const char *path, const char *args)
{
    if (!path || !*path) return -1;

    VfsFile fh;
    if (!VFS_Open(&fh, path, VFS_READ)) return -1;

    uint32_t file_size = VFS_Size(&fh);

    /* --- Try to read UAOS header --- */
    if (file_size >= UAOS_BIN_HEADER_SIZE) {
        uint8_t hdr[UAOS_BIN_HEADER_SIZE];
        if (VFS_Read(&fh, hdr, UAOS_BIN_HEADER_SIZE) == UAOS_BIN_HEADER_SIZE
            && uaos_bin_check_magic(hdr)) {

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
                if (NativeCmd_Run(bin_name, &ctx, args ? args : "") == 0)
                    return 0;
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

                /* Build argv from bin_name + args */
                static const char *m68k_argv[18];
                static char m68k_argstore[256];
                m68k_argv[0] = bin_name;
                int argc = 1;
                if (args && *args) {
                    int ai = 0;
                    while (*args && ai < 254)
                        m68k_argstore[ai++] = *args++;
                    m68k_argstore[ai] = '\0';
                    char *tok = m68k_argstore;
                    while (*tok && argc < 16) {
                        while (*tok == ' ') tok++;
                        if (!*tok) break;
                        m68k_argv[argc++] = tok;
                        while (*tok && *tok != ' ') tok++;
                        if (*tok == ' ') *tok++ = '\0';
                    }
                }
                m68k_argv[argc] = NULL;
                UAOS_Emu_SetCwd("");
                UaosTask *t = Task_CreateM68k(bin_name, 0,
                                                g_exec_payload, payload_size,
                                                m68k_argv, NULL);
                (void)t;  /* task is now running in background */
                return 0;
            }

            /* Unknown type */
            VFS_Close(&fh);
            return -2;
        }
        /* Not a UAOS binary */
    }

    VFS_Close(&fh);
    return -2;
}
