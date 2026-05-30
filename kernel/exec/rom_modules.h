/*
 * rom_modules.h — UAOS Thunk Library Registration Engine
 *
 * Manages the internal registry of native library modules that back the
 * m68k Exec library jump table.
 */

#ifndef UAOS_ROM_MODULES_H
#define UAOS_ROM_MODULES_H

#include <stdint.h>

/* -----------------------------------------------------------------------
 * ROM module descriptor
 * ----------------------------------------------------------------------- */

typedef struct UaosRomModule {
    const char *name;           /* AmigaOS library name, e.g. "exec.library" */
    uint16_t    version;        /* library version                            */
    uint32_t    amiga_base;     /* 32-bit Amiga address of the library base   */
    uint16_t    func_count;     /* number of exported jump table vectors      */
    void      **native_funcs;   /* array of native function pointers          */
} UaosRomModule;

/* Register a ROM module at boot time */
int UAOS_ROM_Register(const char *name, uint16_t version,
                      uint32_t amiga_base,
                      uint16_t func_count, void **native_funcs);

/* Find a module by name (internal use) */
UaosRomModule *UAOS_ROM_Find(const char *name);

/* Resolve function index to native handler */
void *UAOS_ROM_NativeFunc(const char *lib_name, uint16_t func_idx);

/* List all registered modules */
int UAOS_ROM_ListAll(char *names[], uint16_t versions[], int max_count);

/* Register all built-in ROM modules at boot time */
void UAOS_ROM_RegisterAll(void);

/* Register utility.library */
void UAOS_UTILITY_Register(void);

/* Register console.device */
void UAOS_CONSOLE_Register(void);

/* Register mathffp.library */
void UAOS_MATHFFP_Register(void);

/* Register locale.library */
void UAOS_LOCALE_Register(void);

#endif /* UAOS_ROM_MODULES_H */
