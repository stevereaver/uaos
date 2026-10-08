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
 * LVO map entry — binds a guest jump-table vector (negative offset from
 * the library base) to a 1-based native_funcs index.
 * ----------------------------------------------------------------------- */

typedef struct {
    int16_t  lvo;               /* e.g. -36                                   */
    uint16_t fn;                /* 1-based index into native_funcs            */
} UaosRomLvo;

/* -----------------------------------------------------------------------
 * ROM module descriptor
 * ----------------------------------------------------------------------- */

typedef struct UaosRomModule {
    const char *name;           /* AmigaOS library name, e.g. "exec.library" */
    uint16_t    version;        /* library version                            */
    uint32_t    amiga_base;     /* 32-bit Amiga address of the library base   */
    uint16_t    func_count;     /* number of exported jump table vectors      */
    void      **native_funcs;   /* array of native function pointers          */
    const UaosRomLvo *lvo_map;  /* optional guest LVO -> func index table     */
    uint16_t    lvo_count;      /* entries in lvo_map                         */
    uint8_t     lvo_slot_indexed; /* nonzero: native_funcs[i] serves LVO -6*i */
} UaosRomModule;

/* Register a ROM module at boot time */
int UAOS_ROM_Register(const char *name, uint16_t version,
                      uint32_t amiga_base,
                      uint16_t func_count, void **native_funcs);

/* Find a module by name (internal use) */
UaosRomModule *UAOS_ROM_Find(const char *name);

/* Resolve function index to native handler */
void *UAOS_ROM_NativeFunc(const char *lib_name, uint16_t func_idx);

/* Attach a guest LVO->func map to a registered module (0 on success) */
int UAOS_ROM_BindLvoMap(const char *name, const UaosRomLvo *map,
                        uint16_t count);

/* Mark a module whose native_funcs[] is indexed by LVO slot (|lvo|/6) */
int UAOS_ROM_MarkSlotIndexed(const char *name);

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

/* Register mathieeesingbas.library */
void UAOS_MATHIEEESINGBAS_Register(void);

/* Register mathtrans.library */
void UAOS_MATHTRANS_Register(void);

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

/* Register iffparse.library */
void UAOS_IFFPARSE_Register(void);

/* Global guest RAM base for Amiga address translation */
extern uint8_t *uaos_ram_base;

#endif /* UAOS_ROM_MODULES_H */
