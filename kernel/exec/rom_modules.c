/*
 * rom_modules.c — UAOS Thunk Library Registration Engine
 *
 * Manages the internal registry of native library modules that back the
 * m68k Exec library jump table.  At startup the kernel calls
 * UAOS_ROM_RegisterAll() to populate the registry; the thunk dispatcher
 * in thunk_handler.c resolves calls against the registered module list.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* -----------------------------------------------------------------------
 * Maximum number of simultaneously registered ROM modules
 * ----------------------------------------------------------------------- */

#define UAOS_MAX_ROM_MODULES  64

/* -----------------------------------------------------------------------
 * ROM module descriptor
 * ----------------------------------------------------------------------- */

typedef struct {
    const char *name;           /* AmigaOS library name, e.g. "exec.library" */
    uint16_t    version;        /* library version                            */
    uint32_t    amiga_base;     /* 32-bit Amiga address of the library base   */
    uint16_t    func_count;     /* number of exported jump table vectors      */
    void      **native_funcs;   /* array of native function pointers          */
} UaosRomModule;

static UaosRomModule rom_registry[UAOS_MAX_ROM_MODULES];
static int           rom_count = 0;

/* -----------------------------------------------------------------------
 * UAOS_ROM_Register — add a module to the registry
 *
 * Returns 0 on success, -1 if the registry is full.
 * ----------------------------------------------------------------------- */

int UAOS_ROM_Register(const char *name, uint16_t version,
                      uint32_t amiga_base,
                      uint16_t func_count, void **native_funcs)
{
    if (rom_count >= UAOS_MAX_ROM_MODULES) {
        fprintf(stderr, "[ROM] Registry full — cannot register \"%s\"\n", name);
        return -1;
    }

    UaosRomModule *m = &rom_registry[rom_count++];
    m->name         = name;
    m->version      = version;
    m->amiga_base   = amiga_base;
    m->func_count   = func_count;
    m->native_funcs = native_funcs;

    fprintf(stderr, "[ROM] Registered \"%s\" v%u @ 0x%08X (%u vectors)\n",
            name, version, amiga_base, func_count);
    return 0;
}

/* -----------------------------------------------------------------------
 * UAOS_ROM_Find — look up a module by name
 *
 * Returns a pointer to the descriptor, or NULL if not found.
 * ----------------------------------------------------------------------- */

UaosRomModule *UAOS_ROM_Find(const char *name)
{
    for (int i = 0; i < rom_count; i++) {
        if (strcmp(rom_registry[i].name, name) == 0)
            return &rom_registry[i];
    }
    return NULL;
}

/* -----------------------------------------------------------------------
 * UAOS_ROM_NativeFunc — resolve function index to native handler
 *
 * func_idx is 1-based to match the rom_traps.s index assignments.
 * Returns NULL if the module is not found or the index is out of range.
 * ----------------------------------------------------------------------- */

void *UAOS_ROM_NativeFunc(const char *lib_name, uint16_t func_idx)
{
    UaosRomModule *m = UAOS_ROM_Find(lib_name);
    if (m == NULL) return NULL;
    if (func_idx == 0 || func_idx > m->func_count) return NULL;
    return m->native_funcs[func_idx - 1];
}

/* -----------------------------------------------------------------------
 * Built-in stub handlers for exec.library vectors
 * (These are thin wrappers; the real implementations live in thunk_handler.c)
 * ----------------------------------------------------------------------- */

static void exec_stub_OpenLibrary(void)    { /* forwarded via thunk */ }
static void exec_stub_AllocMem(void)       { /* forwarded via thunk */ }
static void exec_stub_FreeMem(void)        { /* forwarded via thunk */ }
static void exec_stub_CloseLibrary(void)   { /* forwarded via thunk */ }
static void exec_stub_FindTask(void)       { /* forwarded via thunk */ }
static void exec_stub_AddTask(void)        { /* forwarded via thunk */ }
static void exec_stub_RemTask(void)        { /* forwarded via thunk */ }
static void exec_stub_Wait(void)           { /* forwarded via thunk */ }
static void exec_stub_Signal(void)         { /* forwarded via thunk */ }
static void exec_stub_SetFunction(void)    { /* forwarded via thunk */ }

static void *exec_funcs[] = {
    exec_stub_OpenLibrary,   /* index 1  */
    exec_stub_AllocMem,      /* index 2  */
    exec_stub_FreeMem,       /* index 3  */
    exec_stub_CloseLibrary,  /* index 4  */
    exec_stub_FindTask,      /* index 5  */
    exec_stub_AddTask,       /* index 6  */
    exec_stub_RemTask,       /* index 7  */
    exec_stub_Wait,          /* index 8  */
    exec_stub_Signal,        /* index 9  */
    exec_stub_SetFunction,   /* index 10 */
};

/* -----------------------------------------------------------------------
 * UAOS_ROM_RegisterAll — register all built-in ROM modules at boot time
 * ----------------------------------------------------------------------- */

void UAOS_ROM_RegisterAll(void)
{
    UAOS_ROM_Register("exec.library",     45, 0x00000004,
                      (uint16_t)(sizeof(exec_funcs) / sizeof(exec_funcs[0])),
                      exec_funcs);

    /* Additional ROM modules (dos, intuition, graphics …) are registered
     * here as their native implementations are completed.                  */
}
