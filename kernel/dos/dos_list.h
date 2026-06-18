/* dos_list.h — AmigaDOS DosList global registry
 *
 * DosList is the system-wide list of devices, volumes, assigns,
 * and locks.  Handlers register their MsgPort here so dos.library
 * can route packets to the correct handler.
 */

#ifndef UAOS_DOS_LIST_H
#define UAOS_DOS_LIST_H

#include "amiga_dos_types.h"
#include <stdint.h>

/* -------------------------------------------------------------------------
 * DosList lifecycle
 * ------------------------------------------------------------------------- */

/* Initialise the global DosList.  Call once at boot. */
void DosList_Init(void);

/* Add a device entry to DosList.  Returns the new node or NULL. */
DosList *DosList_AddDevice(const char *name, MsgPort *handler_port,
                           uint32_t disk_type);

/* Add a volume entry to DosList.  Returns the new node or NULL. */
DosList *DosList_AddVolume(const char *name, MsgPort *handler_port,
                           uint32_t volume_date);

/* Add an assign entry to DosList.  Returns the new node or NULL. */
DosList *DosList_AddAssign(const char *name, BPTR lock);

/* Remove a DosList entry by name and type. */
void DosList_Remove(const char *name, uint8_t type);

/* Find a DosList entry by name (case-insensitive).  Any type. */
DosList *DosList_Find(const char *name);

/* Find a DosList entry by name and specific type. */
DosList *DosList_FindByType(const char *name, uint8_t type);

/* Get the handler MsgPort for a device or volume name.
 * Returns NULL if not found. */
MsgPort *DosList_FindHandlerPort(const char *name);

/* Iterate over all entries.  Call with NULL to start, then pass
 * the previous result to get the next one. */
DosList *DosList_Next(DosList *prev);

/* Return number of entries in the list. */
int DosList_Count(void);

#endif
