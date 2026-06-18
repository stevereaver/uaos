/* handler_loader.h — Amiga L: handler dynamic loader
 *
 * Scans the L: directory for handler binaries, loads them via the
 * existing LoadSeg / SystemTagList infrastructure, and maintains a
 * registry of running handlers.
 */

#ifndef UAOS_HANDLER_LOADER_H
#define UAOS_HANDLER_LOADER_H

#include "dos/handler.h"
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Handler entry
 * ------------------------------------------------------------------------- */

typedef struct LHandlerEntry {
    char      name[32];        /* handler binary name, e.g. "aux-handler" */
    char      device_name[16]; /* device name, e.g. "AUX:" */
    uint32_t  seglist_bptr;    /* BPTR to loaded seglist (0 if not loaded) */
    Handler  *handler;         /* NULL if not running */
    int       is_running;      /* 1 = process active */
    int       is_filesystem;   /* 1 = filesystem handler, 0 = device handler */
    MsgPort  *port;            /* handler's message port (when running) */
    void     *device_ref;      /* BlockDev* for filesystems, NULL for devices */
} LHandlerEntry;

#define MAX_L_HANDLERS 16

/* -------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

/* Initialise handler loader — call once at boot after VFS_Init. */
void HandlerLoader_Init(void);

/* Scan L: directory for handler binaries and register them.
 * Returns number of handlers discovered. */
int HandlerLoader_ScanLDirectory(void);

/* Load and start a handler by name (e.g. "aux-handler").
 * If the binary does not exist in L:, a built-in native handler is
 * created when one is known (aux-handler, port-handler).
 * Returns entry pointer on success, NULL on failure. */
LHandlerEntry *HandlerLoader_Load(const char *name);

/* Unload and stop a handler by name. */
void HandlerLoader_Unload(const char *name);

/* Find handler entry by device name (e.g. "AUX:"). */
LHandlerEntry *HandlerLoader_FindByDevice(const char *device_name);

/* Find handler entry by handler binary name (e.g. "aux-handler"). */
LHandlerEntry *HandlerLoader_FindByName(const char *name);

/* List all registered handlers into out[] (up to max_count).
 * Returns number of entries written. */
int HandlerLoader_ListAll(LHandlerEntry *out[], int max_count);

/* Check if a handler is a filesystem. */
int HandlerLoader_IsFilesystem(const char *name);

/* -------------------------------------------------------------------------
 * Built-in native handler registration
 * ------------------------------------------------------------------------- */

/* Register a built-in native handler (no L: binary required).
 * Used for aux-handler, port-handler, etc. */
LHandlerEntry *HandlerLoader_RegisterNative(const char *name,
                                            const char *device_name,
                                            int is_filesystem,
                                            Handler *handler);

#endif
