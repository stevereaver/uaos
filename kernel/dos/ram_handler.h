/* ram_handler.h — RAMFS packet handler */

#ifndef UAOS_RAM_HANDLER_H
#define UAOS_RAM_HANDLER_H

#include "dos/handler.h"
#include "dos/ramfs.h"

/* Create a handler backed by an existing RamFsVol.
 * The volume must remain valid for the lifetime of the handler. */
Handler *RamHandler_Create(const char *name, RamFsVol *vol);

#endif
