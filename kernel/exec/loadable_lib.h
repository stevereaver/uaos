/*
 * loadable_lib.h — UAOS Loadable Library System
 *
 * Scans LIBS: for .library files at boot, reads the full M68k binary,
 * and registers it with the emulation layer for loading into guest RAM.
 */

#ifndef UAOS_LOADABLE_LIB_H
#define UAOS_LOADABLE_LIB_H

#include <stdint.h>

/* Boot-time scan — call after Workbench: is mounted. */
void UAOS_LoadableLib_Init(void);

/* List scanned loadable libraries.  Returns count.
 * If names/versions are non-NULL they are filled up to max_count. */
int UAOS_LoadableLib_ListAll(char *names[], uint16_t versions[], int max_count);

#endif /* UAOS_LOADABLE_LIB_H */
