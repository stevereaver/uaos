/*
 * loadable_lib.h — UAOS Loadable Library System
 *
 * Manages libraries loaded from .library descriptor files in LIBS:.
 * At boot the kernel scans Workbench:LIBS/ for *.library files and
 * registers them.  Built-in native implementations then bind to these
 * entries by name.
 */

#ifndef UAOS_LOADABLE_LIB_H
#define UAOS_LOADABLE_LIB_H

#include <stdint.h>

/* -------------------------------------------------------------------------
 * .library file format constants
 * ------------------------------------------------------------------------- */
#define UAOS_LIB_MAGIC_0 'U'
#define UAOS_LIB_MAGIC_1 'A'
#define UAOS_LIB_MAGIC_2 'O'
#define UAOS_LIB_MAGIC_3 'S'

/* -------------------------------------------------------------------------
 * Limits
 * ------------------------------------------------------------------------- */
#define UAOS_MAX_LOADABLE_LIBS  16
#define UAOS_MAX_LIB_NAME       64
#define UAOS_MAX_LIB_FUNCS      32

/* -------------------------------------------------------------------------
 * Public info structure (used by glue layer for stub install + dispatch)
 * ------------------------------------------------------------------------- */
typedef struct {
    const char *name;
    uint16_t    version;
    uint16_t    func_count;
    uint32_t    base_addr;
    uint8_t     lib_id;
    void      (*dispatch)(uint32_t fn_idx);
} LoadableLibInfo;

/* -------------------------------------------------------------------------
 * Boot-time scan — call after Workbench: is mounted.
 * ------------------------------------------------------------------------- */
void UAOS_LoadableLib_Init(void);

/* -------------------------------------------------------------------------
 * Look-up
 * ------------------------------------------------------------------------- */

/* Find by AmigaOS library name (e.g. "powerpacker.library"). */
LoadableLibInfo *UAOS_LoadableLib_FindByName(const char *name);

/* Get info by lib_id (used by ILLEGAL handler). Returns NULL if not found. */
LoadableLibInfo *UAOS_LoadableLib_GetById(uint8_t lib_id);

/* Get info by index (0..count-1). Returns 1 on success, 0 if out of range. */
int UAOS_LoadableLib_GetInfo(int idx, LoadableLibInfo *out);

/* -------------------------------------------------------------------------
 * Registration of built-in implementations
 * ------------------------------------------------------------------------- */

/* Bind a native dispatch function to an already-scanned loadable library.
 * Call after UAOS_LoadableLib_Init() and after the native impl is ready.
 * Returns 0 on success, -1 if library not found. */
int UAOS_LoadableLib_BindDispatch(const char *name,
                                  void (*dispatch)(uint32_t fn_idx));

/* -------------------------------------------------------------------------
 * Listing (used by the C:libs shell command)
 * ------------------------------------------------------------------------- */

/* List all active loadable libraries.  Returns total count.
 * If names/versions are non-NULL they are filled up to max_count. */
int UAOS_LoadableLib_ListAll(char *names[], uint16_t versions[], int max_count);

/* Return total number of registered loadable libraries. */
int UAOS_LoadableLib_Count(void);

#endif /* UAOS_LOADABLE_LIB_H */
