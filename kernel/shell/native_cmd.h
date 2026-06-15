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
struct CmdTemplateResult;  /* forward-declare for template parsing */
struct PartitionTable;

typedef struct NativeCmdCtx {
    void       *shell;
    void      (*print)(void *shell, const char *line);
    void      (*print_raw)(void *shell, const char *text); /* no newline appended */
    const char *cwd;
    const char *path;          /* shell's command search path (space-separated) */

    /* For fdisk: lets Cmd_Fdisk set the shell into interactive mode */
    void       *shell_extra;   /* ShellInstance* */
    void      (*set_fdisk_mode)(void *shell_extra,
                                struct BlockDev *dev);
    /* For vim: open inline editor in the shell window */
    void      (*set_vim_mode)(void *shell_extra, const char *filename);
    /* For loadwb: launch the desktop */
    void      (*loadwb)(void);
    /* For clear: wipe shell history */
    void      (*clear_history)(void *shell_extra);

    /* For which: check if a name is a shell builtin */
    int       (*is_builtin)(const char *name);

    /* For execute: dispatch a command line through the shell */
    void      (*dispatch_line)(void *shell_extra, const char *line);

    /* For execute: run a script text with flow-control support */
    void      (*run_script)(void *shell_extra, const char *text);

    /* Cooperative yield — pump mouse/keyboard/WM/network for ~N ms without
     * blocking the UI.  Commands that busy-wait (ping, etc.) must call this
     * instead of raw delay loops so the desktop stays responsive. */
    void      (*yield_ms)(void *shell_extra, uint32_t ms);

    /* Blocking key read — pumps UI events and returns the next ASCII character
     * typed by the user.  Used by the pager (more) to wait for Space/Enter/q.
     * Returns 0 if the callback is not set. */
    char      (*read_key)(void *shell_extra);

    /* Blocking line read — pumps UI events and fills buffer until Enter.
     * Used by 'ask' command to get user input. Returns bytes read or 0. */
    int       (*read_line)(void *shell_extra, char *buf, int max);

    /* Set ask mode — sets a custom prompt for the next input line.
     * Used by 'ask' command to display a custom prompt to the user. */
    void      (*set_ask_mode)(void *shell_extra, const char *prompt);

    /* Shell window geometry — visible text rows in the history area.
     * Used by more to compute the page size without hard-coding a number.
     * 0 means unknown (fall back to a safe default). */
    int         visible_rows;

    /* For ps: enumerate running tasks.
     * idx starts at 0 and increments until the callback returns 0.
     * When active, fills out (up to max bytes) with a formatted task line
     * and returns 1.  Returns 0 when idx is past the end of the list. */
    int       (*enum_tasks)(void *shell_extra, int idx, char *out, int max);

    /* For prompt: set a custom shell prompt string (replaces volume>).
     * Pass empty string to reset to default. */
    void      (*set_prompt)(void *shell_extra, const char *prompt);

    /* For endcli: close the current shell window. */
    void      (*close_shell)(void *shell_extra);

    /* For why: get the last command return code. Returns 0 if unavailable. */
    int       (*get_last_rc)(void *shell_extra);

    /* For why: set the last command return code. */
    void      (*set_rc)(void *shell_extra, int rc);

    /* For failat: get/set the failure threshold (default 10). */
    int       (*get_failat)(void *shell_extra);
    void      (*set_failat)(void *shell_extra, int threshold);

    /* For getenv: read an environment variable value into buf[max].
     * Returns 1 if found, 0 if not found. */
    int       (*get_env)(void *shell_extra, const char *name, char *buf, int max);

    /* For quit: signal the script runner to stop at the next boundary.
     * Optional rc is stored as the script return code. */
    void      (*quit_script)(void *shell_extra, int rc);

    /* For piping: path to a temp file containing the previous command's
     * output.  Commands that read from a file should use this when no
     * explicit file argument is given. */
    const char *pipe_file;

    /* Set automatically by NativeCmd_Run when the command has a template.
     * Commands can query parsed arguments via the CmdTemplate_* helpers. */
    struct CmdTemplateResult *template;
} NativeCmdCtx;

/* Convenience macro — yield N ms from inside a Cmd_* function */
#define CMD_YIELD(ctx, ms)  do { if ((ctx)->yield_ms) (ctx)->yield_ms((ctx)->shell_extra, (ms)); } while(0)

/* Convenience macro — blocking read of one key from inside a Cmd_* function */
#define CMD_READ_KEY(ctx)  ((ctx)->read_key ? (ctx)->read_key((ctx)->shell_extra) : (char)0)

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
void Cmd_Ifconfig (NativeCmdCtx *ctx, const char *args);
void Cmd_Ping     (NativeCmdCtx *ctx, const char *args);
void Cmd_Route    (NativeCmdCtx *ctx, const char *args);
void Cmd_Nslookup (NativeCmdCtx *ctx, const char *args);
void Cmd_Ntpd     (NativeCmdCtx *ctx, const char *args);
void Cmd_ClockWin (NativeCmdCtx *ctx, const char *args);
void Cmd_Grep     (NativeCmdCtx *ctx, const char *args);
void Cmd_More     (NativeCmdCtx *ctx, const char *args);
void Cmd_Vim      (NativeCmdCtx *ctx, const char *args);
void Cmd_NewCLI   (NativeCmdCtx *ctx, const char *args);
void Cmd_Ask      (NativeCmdCtx *ctx, const char *args);
void Cmd_Resident (NativeCmdCtx *ctx, const char *args);
void Cmd_Ps       (NativeCmdCtx *ctx, const char *args);
void Cmd_NetInfo  (NativeCmdCtx *ctx, const char *args);
void Cmd_List     (NativeCmdCtx *ctx, const char *args);
void Cmd_Search   (NativeCmdCtx *ctx, const char *args);
void Cmd_Sort     (NativeCmdCtx *ctx, const char *args);
void Cmd_Join     (NativeCmdCtx *ctx, const char *args);
void Cmd_Wait     (NativeCmdCtx *ctx, const char *args);
void Cmd_Prompt   (NativeCmdCtx *ctx, const char *args);
void Cmd_Stack    (NativeCmdCtx *ctx, const char *args);
void Cmd_Why      (NativeCmdCtx *ctx, const char *args);
void Cmd_Failat   (NativeCmdCtx *ctx, const char *args);
void Cmd_Quit     (NativeCmdCtx *ctx, const char *args);
void Cmd_EndCLI   (NativeCmdCtx *ctx, const char *args);
void Cmd_Filenote (NativeCmdCtx *ctx, const char *args);
void Cmd_Relabel  (NativeCmdCtx *ctx, const char *args);
void Cmd_Avail    (NativeCmdCtx *ctx, const char *args);
void Cmd_GetEnv   (NativeCmdCtx *ctx, const char *args);
void Cmd_UnSet    (NativeCmdCtx *ctx, const char *args);

#endif /* UAOS_NATIVE_CMD_H */
