/* resident_cmd.h — UAOS Resident Command Registry
 *
 * Keeps frequently-used command binaries resident in memory to avoid
 * repeated disk access. Commands can be "pure" (permanent) or flushable.
 *
 * This is similar to the AmigaDOS resident command feature.
 */

#ifndef UAOS_RESIDENT_CMD_H
#define UAOS_RESIDENT_CMD_H

#include <stdint.h>

/* Forward declaration - defined in native_cmd.h */
struct NativeCmdCtx;

/* Maximum number of resident commands */
#define MAX_RESIDENT_CMDS 16
#define MAX_RESIDENT_NAME 32
#define MAX_RESIDENT_SIZE (64 * 1024)  /* Max 64KB per resident command */

/* Command types */
#define RESIDENT_TYPE_UNKNOWN 0
#define RESIDENT_TYPE_NATIVE  1  /* Native x86_64 command */
#define RESIDENT_TYPE_M68K    2  /* M68K binary */
#define RESIDENT_TYPE_SCRIPT  3  /* Text script */

typedef struct {
    char     name[MAX_RESIDENT_NAME];  /* Command name (without path) */
    uint8_t *data;                     /* Loaded binary data (NULL if not loaded) */
    uint32_t size;                     /* Size of data in bytes */
    int      type;                     /* RESIDENT_TYPE_* */
    int      permanent;                /* 1 = pure (never flushed), 0 = flushable */
    int      in_use;                   /* 1 = slot is occupied */
} ResidentEntry;

/* -------------------------------------------------------------------------
 * Initialization
 * ------------------------------------------------------------------------- */

/* Initialize the resident command registry. Called at boot. */
void Resident_Init(void);

/* -------------------------------------------------------------------------
 * Command management
 * ------------------------------------------------------------------------- */

/* Add a command to the resident list.
 * Loads the command from the given path into memory.
 * If 'permanent' is 1, the command is marked as "pure" and won't be flushed.
 * Returns 0 on success, -1 on error (file not found, out of slots, etc.). */
int Resident_Add(const char *name, const char *path, int permanent);

/* Remove a command from the resident list.
 * Frees its memory. Returns 0 on success, -1 if not found. */
int Resident_Remove(const char *name);

/* Remove all flushable (non-permanent) resident commands.
 * Pure commands are kept. Returns number of commands removed. */
int Resident_Flush(void);

/* -------------------------------------------------------------------------
 * Query and execution
 * ------------------------------------------------------------------------- */

/* Check if a command is resident. Returns 1 if yes, 0 if no. */
int Resident_Exists(const char *name);

/* Get info about a resident command.
 * Fills in the provided entry structure (only metadata, not data pointer).
 * Returns 0 on success, -1 if not found. */
int Resident_GetInfo(const char *name, ResidentEntry *info);

/* List all resident commands, calling print_fn for each.
 * Format: "name type size [P]" where [P] indicates pure/permanent. */
void Resident_List(void (*print_fn)(const char *line));

/* Execute a resident command.
 * This bypasses disk access and runs the command from memory.
 * Returns 0 on success, -1 if command not resident or execution failed. */
int Resident_Run(const char *name, struct NativeCmdCtx *ctx, const char *args);

/* -------------------------------------------------------------------------
 * Internal helpers (exposed for cmd_resident.c)
 * ------------------------------------------------------------------------- */

/* Get the internal registry array for iteration. */
ResidentEntry* Resident_GetRegistry(void);

#endif /* UAOS_RESIDENT_CMD_H */
