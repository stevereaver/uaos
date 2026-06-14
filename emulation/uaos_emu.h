/* uaos_emu.h — UAOS M68k emulator public API */

#ifndef UAOS_EMU_H
#define UAOS_EMU_H

#include <stdint.h>

/* Callback type for printing output to the shell history */
typedef void (*UAOS_PrintFn)(void *shell, const char *line);

/* Run an embedded Amiga binary by name (e.g. "Lha", "Calculator").
 * shell is the ShellInstance* passed back to print_fn.
 * Returns 0 on success, -1 if binary not found.                    */
int UAOS_Emu_RunByName(const char *name, void *shell, UAOS_PrintFn print_fn);

/* Lower-level: run a raw binary image directly */
int UAOS_Emu_LoadAndRun(const uint8_t *binary, uint32_t bin_size,
                         const char **argv, void *shell,
                         UAOS_PrintFn print_fn);

/* Set the current working directory used to resolve relative paths in
 * dos_Open calls from emulated programs. Call before RunByName/LoadAndRun. */
void UAOS_Emu_SetCwd(const char *cwd);

/* Register a loadable library binary for installation into guest RAM.
 * Call at boot after scanning LIBS:.  The binary pointer must remain
 * valid until the first M68k program starts (install_library_tables
 * copies it into g_ram).  out_base receives the assigned guest address. */
void UAOS_Emu_RegisterLoadableLib(const char *name, const uint8_t *data,
                                  uint32_t size, uint32_t *out_base);

#endif /* UAOS_EMU_H */
