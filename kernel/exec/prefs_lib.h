/*
 * prefs_lib.h — UAOS Preferences Persistence Library
 *
 * Implements AmigaOS IFF PREF file format reader/writer for
 * ENV: and ENVARC: preference files.
 *
 * IFF PREF format:
 *   FORM xxxx PREF
 *     PRHD (Preference Header chunk)
 *       0: do_Type    (uint8)  — prefs type (0=WB, 1=Screen, etc.)
 *       1: reservered (3 bytes)
 *     <type-specific chunks>
 *
 * Each chunk: TYPE (4 bytes) + length (4 bytes BE) + data (padded to even)
 */

#ifndef UAOS_PREFS_LIB_H
#define UAOS_PREFS_LIB_H

#include <stdint.h>

/* ------------------------------------------------------------------------- */
/* Constants                                                                 */
/* ------------------------------------------------------------------------- */

/* IFF tags */
#define IFF_FORM  0x464F524D  /* "FORM" */
#define IFF_PREF  0x50524546  /* "PREF" */

/* Preference types (do_Type in PRHD) */
#define PREFS_WB        0   /* Workbench */
#define PREFS_SCREEN    1   /* ScreenMode */
#define PREFS_PALETTE   2   /* Palette */
#define PREFS_POINTER   3   /* Pointer */
#define PREFS_INPUT     4   /* Input */
#define PREFS_FONT      5   /* Font */
#define PREFS_TIME      6   /* Time */
#define PREFS_IControl  7   /* IControl */
#define PREFS_SERIAL    8   /* Serial */
#define PREFS_SOUND     9   /* Sound */
#define PREFS_OVERSCAN  10  /* Overscan */
#define PREFS_PRINTER   11  /* Printer */
#define PREFS_PGFX      12  /* PrinterGfx */
#define PREFS_PPS       13  /* PrinterPS */
#define PREFS_LOCALE    14  /* Locale */
#define PREFS_WBPATTERN 15  /* WBPattern */

/* Max chunks per prefs file */
#define PREFS_MAX_CHUNKS   32
#define PREFS_MAX_CHUNK_DATA 4096
#define PREFS_MAX_NAME     32

/* ------------------------------------------------------------------------- */
/* Data structures                                                           */
/* ------------------------------------------------------------------------- */

/* A single IFF chunk within a PREF file */
typedef struct {
    char     type[4];       /* 4-char chunk type (e.g. "PRHD", "PGMS") */
    uint32_t size;          /* data size in bytes */
    uint8_t  data[PREFS_MAX_CHUNK_DATA];
} PrefsChunk;

/* A parsed prefs file */
typedef struct {
    uint8_t     prefs_type;     /* from PRHD chunk */
    PrefsChunk  chunks[PREFS_MAX_CHUNKS];
    int         chunk_count;
} PrefsFile;

/* ------------------------------------------------------------------------- */
/* API                                                                       */
/* ------------------------------------------------------------------------- */

/* Load a prefs file from ENV: or ENVARC:.
 * path should be like "ENVARC:Sys/WB.prefs"
 * Returns 1 on success, 0 on failure. */
int Prefs_Load(const char *path, PrefsFile *out);

/* Save a prefs file to the given path.
 * Returns 1 on success, 0 on failure. */
int Prefs_Save(const char *path, const PrefsFile *pf);

/* Find a chunk by 4-char type within a PrefsFile.
 * Returns pointer to the chunk, or NULL if not found. */
const PrefsChunk *Prefs_FindChunk(const PrefsFile *pf, const char *type);

/* Add or replace a chunk in a PrefsFile.
 * Returns 1 on success, 0 if chunk table or data is full. */
int Prefs_SetChunk(PrefsFile *pf, const char *type,
                   const uint8_t *data, uint32_t size);

/* Remove a chunk by type. Returns 1 if found, 0 if not. */
int Prefs_RemoveChunk(PrefsFile *pf, const char *type);

/* Set the prefs type (PRHD do_Type field). */
void Prefs_SetType(PrefsFile *pf, uint8_t type);

/* Copy a prefs file from ENVARC: to ENV: (load persistent → runtime).
 * Returns 1 on success, 0 if source doesn't exist. */
int Prefs_LoadToEnv(const char *name);

/* Copy a prefs file from ENV: to ENVARC: (save runtime → persistent).
 * Returns 1 on success, 0 on failure. */
int Prefs_SaveToEnvarc(const char *name);

/* Broadcast a prefs change notification.
 * Currently just a hook point; will be wired to intuition.library
 * to trigger screen/window redraws. */
void Prefs_NotifyChange(uint8_t prefs_type);

/* Register a callback to be called when any prefs file changes.
 * The callback receives the prefs type (PREFS_WB, PREFS_PALETTE, etc.) */
void Prefs_RegisterNotify(void (*cb)(uint8_t prefs_type));

#endif /* UAOS_PREFS_LIB_H */
