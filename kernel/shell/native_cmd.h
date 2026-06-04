/* native_cmd.h — UAOS Native Command Registry
 *
 * Provides a table of native (x86 kernel) command implementations that
 * correspond to discrete binaries found in C:.  When the shell locates a
 * file in the PATH it calls NativeCmd_Run() which dispatches to the
 * matching handler here instead of attempting M68k emulation.
 *
 * Adding a new binary command:
 *   1. Create kernel/shell/cmd_<name>.c implementing:
 *        void Cmd_<Name>(NativeCmdCtx *ctx, const char *args);
 *   2. Declare it extern here and add an entry to k_native_cmds[] in
 *      native_cmd.c.
 *   3. Add a stub marker file to sys-root/C/<name>.
 *   4. Add the .c file to the build in scripts/build_iso.sh.
 */

#ifndef UAOS_NATIVE_CMD_H
#define UAOS_NATIVE_CMD_H

/* -------------------------------------------------------------------------
 * Context passed to every native command
 * -------------------------------------------------------------------------
 * shell        : opaque ShellInstance* passed back to print_fn
 * print        : function to emit a line of text to the shell window
 * cwd          : current working directory of the calling shell (read-only)
 * shell_extra  : opaque pointer for commands that need shell internal state
 *                (e.g. fdisk needs to set fdisk_mode on the shell instance)
 * set_fdisk    : optional callback — set fdisk interactive mode on the shell.
 *                Called by Cmd_Fdisk with the block device pointer and a
 *                pointer to the print-wrapper so fdisk_handle_cmd can use it.
 *                Pass NULL if the shell does not support this.
 * loadwb       : optional callback — launch Workbench desktop.
 * ------------------------------------------------------------------------- */

#include <stdint.h>

struct BlockDev;   /* forward-declare to avoid pulling blockdev.h here */
struct PartitionTable;

typedef struct NativeCmdCtx {
    void       *shell;
    void      (*print)(void *shell, const char *line);
    const char *cwd;
    const char *path;          /* shell's command search path (space-separated) */

    /* For fdisk: lets Cmd_Fdisk set the shell into interactive mode */
    void       *shell_extra;   /* ShellInstance* */
    void      (*set_fdisk_mode)(void *shell_extra,
                                struct BlockDev *dev);
    /* For loadwb: launch the desktop */
    void      (*loadwb)(void);
    /* For clear: wipe shell history */
    void      (*clear_history)(void *shell_extra);

    /* For which: check if a name is a shell builtin */
    int       (*is_builtin)(const char *name);

    /* For execute: dispatch a command line through the shell */
    void      (*dispatch_line)(void *shell_extra, const char *line);
} NativeCmdCtx;

/* Convenience macro — emit one line from inside a Cmd_* function */
#define CMD_PRINT(ctx, msg)  (ctx)->print((ctx)->shell, (msg))

/* -------------------------------------------------------------------------
 * Command function type
 * ------------------------------------------------------------------------- */
typedef void (*NativeCmdFn)(NativeCmdCtx *ctx, const char *args);

/* -------------------------------------------------------------------------
 * Look up and execute a native command by name.
 * Returns 0 if a handler was found and invoked, -1 if not found.
 * ------------------------------------------------------------------------- */
int NativeCmd_Run(const char *name, NativeCmdCtx *ctx, const char *args);

/* Check whether a name is a registered native command (1=yes, 0=no) */
int NativeCmd_Exists(const char *name);

/* -------------------------------------------------------------------------
 * Forward declarations — one per cmd_*.c source file
 * ------------------------------------------------------------------------- */
void Cmd_Version (NativeCmdCtx *ctx, const char *args);
void Cmd_Mem     (NativeCmdCtx *ctx, const char *args);
void Cmd_Libs    (NativeCmdCtx *ctx, const char *args);
void Cmd_Clear   (NativeCmdCtx *ctx, const char *args);
void Cmd_Reboot  (NativeCmdCtx *ctx, const char *args);
void Cmd_Dir     (NativeCmdCtx *ctx, const char *args);
void Cmd_Makedir (NativeCmdCtx *ctx, const char *args);
void Cmd_Delete  (NativeCmdCtx *ctx, const char *args);
void Cmd_Type    (NativeCmdCtx *ctx, const char *args);
void Cmd_Copy    (NativeCmdCtx *ctx, const char *args);
void Cmd_Rename  (NativeCmdCtx *ctx, const char *args);
void Cmd_Pwd     (NativeCmdCtx *ctx, const char *args);
void Cmd_Echo    (NativeCmdCtx *ctx, const char *args);
void Cmd_Protect (NativeCmdCtx *ctx, const char *args);
void Cmd_Attr    (NativeCmdCtx *ctx, const char *args);
void Cmd_Info    (NativeCmdCtx *ctx, const char *args);
void Cmd_Date    (NativeCmdCtx *ctx, const char *args);
void Cmd_Which   (NativeCmdCtx *ctx, const char *args);
void Cmd_Disks   (NativeCmdCtx *ctx, const char *args);
void Cmd_Fdisk   (NativeCmdCtx *ctx, const char *args);
void Cmd_Format  (NativeCmdCtx *ctx, const char *args);
void Cmd_Pointer (NativeCmdCtx *ctx, const char *args);
void Cmd_Run     (NativeCmdCtx *ctx, const char *args);
void Cmd_Assign  (NativeCmdCtx *ctx, const char *args);
void Cmd_Execute (NativeCmdCtx *ctx, const char *args);
void Cmd_LoadWB  (NativeCmdCtx *ctx, const char *args);
void Cmd_CalcWin (NativeCmdCtx *ctx, const char *args);

#endif /* UAOS_NATIVE_CMD_H */
