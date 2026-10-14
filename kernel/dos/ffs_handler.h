/* ffs_handler.h — AmigaDOS packet handler for OFS/FFS volumes */

#ifndef UAOS_FFS_HANDLER_H
#define UAOS_FFS_HANDLER_H

#include "ffs.h"
#include "handler.h"

/* Create a read/write OFS/FFS handler bound to a mounted volume. */
Handler *FfsHandler_Create(const char *name, FfsVolume *vol);

/* Identity test for direct-dispatch optimizations. */
int FfsHandler_Is(const Handler *handler);

/* Count open file handles + lock nodes bound to the handler's volume. */
int FfsHandler_BusyCount(const Handler *h);

#endif /* UAOS_FFS_HANDLER_H */
