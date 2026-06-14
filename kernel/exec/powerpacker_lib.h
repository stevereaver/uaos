/*
 * powerpacker_lib.h — UAOS powerpacker.library Interface
 *
 * AmigaOS powerpacker.library provides data decompression and decryption
 * for PowerPacker-compressed files.  This is a native UAOS implementation
 * backed by the loadable library system.
 */

#ifndef UAOS_POWERPACKER_LIB_H
#define UAOS_POWERPACKER_LIB_H

#include <stdint.h>

/* Register powerpacker.library with the loadable library system.
 * Call after UAOS_LoadableLib_Init(). */
void UAOS_POWERPACKER_Register(void);

/* Dispatch function — called by the m68k ILLEGAL handler. */
void UAOS_POWERPACKER_Dispatch(uint32_t fn_idx);

#endif /* UAOS_POWERPACKER_LIB_H */
