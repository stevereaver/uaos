/*
 * workbench_lib.h — UAOS workbench.library Registration
 */

#ifndef UAOS_WORKBENCH_LIB_H
#define UAOS_WORKBENCH_LIB_H

#include <stdint.h>

/* Register workbench.library in the ROM module registry */
void UAOS_WORKBENCH_Register(void);

/* =========================================================================
 * AppIcon query API — used by desktop.c to render AppIcons on the desktop
 * ========================================================================= */

#define APPICON_MAX_LABEL  32

typedef struct {
    uint32_t id;          /* unique app-provided ID */
    uint32_t msg_port;    /* guest pointer to MsgPort */
    uint32_t disk_obj;    /* guest pointer to DiskObject */
    char     label[APPICON_MAX_LABEL];
} AppIconInfo;

/* Get the number of active AppIcons. */
int WB_GetAppIconCount(void);

/* Get info for an active AppIcon by index (0..count-1).
 * Returns 1 on success, 0 if index out of range. */
int WB_GetAppIcon(int index, AppIconInfo *out);

#endif /* UAOS_WORKBENCH_LIB_H */
