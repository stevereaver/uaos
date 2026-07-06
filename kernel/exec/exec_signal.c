/* exec_signal.c — UAOS Exec-compatible signal and critical-section primitives
 *
 * The core Signal/Wait/SetSignal/Forbid/Permit/Disable/Enable are
 * implemented in task.c because they need deep scheduler access.
 * This file provides M68k-facing helpers and additional exec-level
 * wrappers used by the thunk handler and ROM module stubs.
 */

#include "task.h"
#include <stdint.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * M68k task lookup — map guest RAM Process/Task address to host UaosTask
 * ------------------------------------------------------------------------- */

extern UaosTask g_tasks[];
extern int      g_task_count;

UaosTask *Task_FindByM68kAddr(uint32_t guest_addr)
{
    for (int i = 0; i < g_task_count; i++) {
        if (g_tasks[i].type == TASK_TYPE_M68K &&
            g_tasks[i].m68k_task_struct == guest_addr &&
            g_tasks[i].tc_State != TASK_REMOVED)
            return &g_tasks[i];
    }
    return NULL;
}

UaosTask *Task_FindByName(const char *name)
{
    if (!name || !*name) return Task_Current();
    for (int i = 0; i < g_task_count; i++) {
        if (g_tasks[i].ln_Name && g_tasks[i].tc_State != TASK_REMOVED) {
            const char *a = g_tasks[i].ln_Name;
            const char *b = name;
            while (*a && *b && *a == *b) { a++; b++; }
            if (*a == *b) return &g_tasks[i];
        }
    }
    return NULL;
}
