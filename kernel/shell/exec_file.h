/* exec_file.h — Generic file launcher (no shell instance required)
 *
 * Provides ExecFile_Run(), which opens a VFS file, inspects its UAOS binary
 * header, and dispatches to the appropriate runner:
 *   NATIVE  -> NativeCmd_Run with a minimal context
 *   M68K    -> Task_CreateM68k with the payload (background task)
 *   raw hunk -> Task_CreateM68k for raw Amiga Hunk files (0x000003F3)
 *   X64     -> recognised but not yet executed (reserved for Phase 3)
 *
 * This allows any subsystem (file browser, desktop, etc.) to launch
 * executables without needing a ShellInstance.
 */

#ifndef UAOS_EXEC_FILE_H
#define UAOS_EXEC_FILE_H

/* Launch an executable file by path.
 * Returns 0 on success, -1 if file not found, -2 on bad format/error.
 */
int ExecFile_Run(const char *path, const char *args);

/* Launch a file with Workbench semantics (UAOS-253):
 *   - tool icons run the tool directly;
 *   - project icons resolve do_DefaultTool and pass the project as
 *     WBArg[1] (M68k) or the first argument (native);
 *   - bare default-tool names are searched in the project drawer, then
 *     C:, SYS:Utilities/, SYS:Tools/, SYS:System/, SYS: (UAOS-15);
 *   - extra_paths (shift-clicked multi-selection) become WBArg[2..].
 * The guest receives a WBStartup message on pr_MsgPort with pr_CLI = 0.
 * Returns 0 on success,
 *   -1 tool/file not found,
 *   -2 bad format/load error,
 *   -3 project icon has no default tool.
 * After a failure ExecFile_WBErrTool() names the unresolved default
 * tool ("" when not applicable) and ExecFile_WBErrMsg() fills buf with
 * a user-facing reason line. */
int ExecFile_RunWB(const char *path,
                   const char **extra_paths, int num_extra);
const char *ExecFile_WBErrTool(void);
const char *ExecFile_WBErrMsg(int rc, char *buf, int max);

#endif
