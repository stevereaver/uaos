/* vmmouse.h — VMware/QEMU absolute mouse interface */

#ifndef UAOS_VMMOUSE_H
#define UAOS_VMMOUSE_H

#include <stdint.h>

/* Returns 1 if running inside QEMU/VMware with vmmouse available */
int VMMouse_Detect(void);

/* Initialise vmmouse absolute mode */
void VMMouse_Init(void);

/* Poll for new position. Returns 1 if position updated, 0 if no data.
 * out_x and out_y are set to absolute screen coordinates (already scaled). */
int VMMouse_Poll(int *out_x, int *out_y, int *out_buttons);

#endif
