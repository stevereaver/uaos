/* exec_file.h — Generic file launcher (no shell instance required)
 *
 * Provides ExecFile_Run(), which opens a VFS file, inspects its UAOS binary
 * header, and dispatches to the appropriate runner:
 *   NATIVE -> NativeCmd_Run with a minimal context
 *   M68K   -> UAOS_Emu_LoadAndRun with the payload
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

#endif
