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

/* -----------------------------------------------------------------------
 * layout.gadget (UAOS-128)
 *
 * A layout gadget is a transparent container: once attached to a window
 * its children are spliced into the window's gadget list (so drawing,
 * hit-testing and IDCMP_GADGETUP see real Gadget structures) and the
 * shared uitree engine assigns their rects on open and on every resize.
 * ----------------------------------------------------------------------- */

/* Non-zero when the gadget pointer is a layout.gadget object. */
int  UAOS_BOOPSI_IsLayoutGadget(uint32_t obj);

/* Splice layout children into the window's gadget list and run the
 * initial layout.  Call after AddGadget/AddGList and after window open
 * when WA_Gadgets/NewWindow carried a gadget list.  Idempotent. */
void UAOS_Layout_AttachWindow(uint32_t win_ptr);

/* Re-run the layout of every layout gadget in the window (resize/zoom). */
void UAOS_Layout_ReflowWindow(uint32_t win_ptr);

/* Unlink a layout container's spliced children before the container is
 * removed from the window's gadget list (RemoveGadget/RemoveGList). */
void UAOS_Layout_DetachGadget(uint32_t win_ptr, uint32_t lay);

/* Draw the container chrome (optional labelled group bevel).  Called by
 * the intuition gadget draw walk in place of the default gadget body. */
void UAOS_Layout_DrawChrome(uint32_t lay, int gx, int gy, int w, int h);

#endif /* UAOS_BOOPSI_BUILTIN_H */
