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

#endif /* UAOS_ICON_LOADER_H */
