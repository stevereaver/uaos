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

#endif
