/*
 * boopsi_builtin.h — Built-in BOOPSI class registration
 *
 * Provides native host dispatchers for the standard AmigaOS BOOPSI classes
 * (rootclass, gadgetclass, imageclass, pointerclass, menuclass, windowclass)
 * so M68k programs can use NewObject/DisposeObject/SetAttrs/GetAttr/DoMethod
 * without requiring M68k dispatcher code.
 */

#ifndef UAOS_BOOPSI_BUILTIN_H
#define UAOS_BOOPSI_BUILTIN_H

#include <stdint.h>

/* Register all built-in BOOPSI classes with the public class registry. */
void UAOS_BOOPSI_RegisterBuiltinClasses(void);

/* Public helper to register a single class with the BOOPSI registry. */
void UAOS_BOOPSI_RegisterClass(uint32_t cls);

/* Public BOOPSI dispatch helper (used by built-in class disposal). */
uint32_t UAOS_BOOPSI_Dispatch(uint32_t object, uint32_t method, uint32_t msg, uint32_t start_class);

#endif /* UAOS_BOOPSI_BUILTIN_H */
