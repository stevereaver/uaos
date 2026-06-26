/* uaos_emu.h — UAOS M68k emulator public API */

#ifndef UAOS_EMU_H
#define UAOS_EMU_H

#include <stdint.h>

/* Guest RAM layout: 8 MB chip RAM at guest address 0x00000000 followed by
 * 8 MB fast RAM.  Total guest RAM visible to the M68k emulation layer. */
#define GUEST_RAM_SIZE (16 * 1024 * 1024)

/* Guest RAM base — shared with ROM stubs so dos.library (and future
 * libraries) can read/write guest memory without backend-specific APIs.
 * This is a pointer so each M68k task can have its own guest RAM.
 *
 * When the UAE bridge is available, the bridge initialisation calls
 * UAOS_Glue_SetRamBase() to make g_ram point into the 4 GB guest physical
 * window at offset 0.  Otherwise it falls back to the static default RAM
 * buffer in uaos_m68k_glue.c. */
extern uint8_t *g_ram;

/* Print callback for M68k stdout output */
typedef void (*GluePrintFn)(const char *s);
extern GluePrintFn g_print;

/* Bump allocator pointer — shared so ROM stubs can allocate guest
 * FileLock structs, BSTRs, etc. */
extern uint32_t g_uaos_heap_ptr;

/* BPTR to the CLI argument BSTR, set at startup for GetArgStr() */
extern uint32_t g_cmdline_bptr;

/* Current working directory for resolving relative paths */
extern char g_uaos_cwd[64];

/* Emulation halt flag — set by dos_Exit to break the execute loop */
extern int g_emu_halted;

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

/* Set the guest RAM base used by the M68k glue layer.  Called by the UAE
 * bridge after allocating the 4 GB guest physical window.  Passing NULL
 * leaves the current base unchanged. */
void UAOS_Glue_SetRamBase(uint8_t *base);

/* Internal helpers used by the M68k task wrapper (exec_task.c) */
void install_library_tables(void);
uint32_t hunk_load(const uint8_t *bin, uint32_t bin_size);

#endif /* UAOS_EMU_H */
