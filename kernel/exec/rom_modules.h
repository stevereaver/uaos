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
 * M68k CPU context block — shared between Musashi glue, thunk handler,
 * and ROM module stubs so that dos.library (and future libraries) can
 * be dispatched from any emulator backend without backend-specific APIs.
 * ----------------------------------------------------------------------- */

typedef struct {
    uint32_t d[8];   /* D0–D7 data registers                              */
    uint32_t a[8];   /* A0–A7 address registers (A7 = stack pointer)      */
    uint32_t pc;     /* Guest Program Counter                              */
    uint16_t sr;     /* Guest Status Register                              */
} M68kCPUState;

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

/* Register ixemul.library */
void UAOS_IXEMUL_Register(void);

/* Register timer.device */
void UAOS_TIMER_Register(void);

/* Register keyboard.device */
void UAOS_KEYBOARD_Register(void);

/* Register graphics.library */
void UAOS_GRAPHICS_Register(void);

/* Register dos.library */
void UAOS_DOS_Register(void);

/* Register bsdsocket.library */
void UAOS_BSDSOCKET_Register(void);

/* Register workbench.library */
void UAOS_WORKBENCH_Register(void);

/* Register intuition.library */
void UAOS_INTUITION_Register(void);

/* Global guest RAM base for Amiga address translation */
extern uint8_t *uaos_ram_base;

#endif /* UAOS_ROM_MODULES_H */
