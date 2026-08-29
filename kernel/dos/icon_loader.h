/*
 * icon_loader.h — .info Icon File Loader
 */

#ifndef UAOS_ICON_LOADER_H
#define UAOS_ICON_LOADER_H

#include "../exec/icon_def.h"

/* Load an Amiga .info file from VFS into a ParsedIcon.
 * Returns 1 on success, 0 on failure (not found, bad format, etc.).
 * On failure, *out is zeroed but safe to use. */
int Icon_Load(const char *path, ParsedIcon *out);

/* Free any dynamically allocated data inside a ParsedIcon.
 * Currently a no-op because everything is fixed-size. */
void Icon_Free(ParsedIcon *icon);

/* Check whether a .info file exists for a given path.
 * e.g. "RAM:MyFile" -> checks "RAM:MyFile.info" */
int Icon_ExistsFor(const char *path);

/* -------------------------------------------------------------------------
 * Icon writer — serialize ParsedIcon back to .info binary format
 * ------------------------------------------------------------------------- */

/* Save a ParsedIcon to a .info file at <path>.info.
 * Returns 1 on success, 0 on failure. */
int Icon_Save(const char *path, const ParsedIcon *icon);

/* Save only the icon position (do_CurrentX/Y) to an existing .info file.
 * If the .info doesn't exist, creates a minimal one.
 * Returns 1 on success, 0 on failure. */
int Icon_SavePosition(const char *path, int16_t x, int16_t y);

/* -------------------------------------------------------------------------
 * Tool type get/set/delete API
 * ------------------------------------------------------------------------- */

/* Find a tool type by key prefix (e.g. "STARTPRI" matches "STARTPRI=5").
 * Returns pointer to the full tool type string within icon, or NULL. */
const char *Icon_ToolTypeGet(const ParsedIcon *icon, const char *key);

/* Set or replace a tool type.  If key already exists, its value is updated.
 * If not, a new entry is appended.  value may be NULL for key-only entries.
 * Returns 1 on success, 0 if tool type table is full. */
int Icon_ToolTypeSet(ParsedIcon *icon, const char *key, const char *value);

/* Delete a tool type by key.  Returns 1 if found and removed, 0 if not found. */
int Icon_ToolTypeDelete(ParsedIcon *icon, const char *key);

/* -------------------------------------------------------------------------
 * Default icon generation (pseudo-icons)
 * ------------------------------------------------------------------------- */

/* Generate a default ParsedIcon for the given type (WB_DISK, WB_DRAWER, etc.)
 * with a simple procedural image.  label is copied to the icon label. */
void Icon_MakeDefault(ParsedIcon *out, uint8_t type, const char *label);

#endif /* UAOS_ICON_LOADER_H */
