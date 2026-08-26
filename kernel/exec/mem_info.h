/* mem_info.h — UAOS kernel-exported memory query API
 *
 * Provides a single, centralised query for system memory statistics that
 * can be consumed both by in-kernel callers (e.g. the resident C:mem
 * command) and by the SYSCALL_MEMINFO handler exposed to x86-64 userspace
 * programs (e.g. the on-disk C:avail command).
 *
 * The reported figures are point-in-time snapshots of the live arenas:
 *   - The x86-64 userspace heap (a bump arena used for ELF64 loading and
 *     sys_alloc).  It is reclaimed only when no X64 tasks are alive.
 *   - The per-task emulated M68k guest RAM slots.
 *   - The scheduler task table.
 */

#ifndef UAOS_MEM_INFO_H
#define UAOS_MEM_INFO_H

#include <stdint.h>

/* Memory statistics snapshot shared between kernel and userspace.
 * Layout must stay in sync with `struct uaos_meminfo` in
 * system/libuaos/uaos_syscall.h. */
struct UaosMemInfo {
    /* x86-64 userspace heap (ELF64 loader / sys_alloc arena) */
    uint32_t x64_total;       /* total arena size in bytes          */
    uint32_t x64_used;        /* bytes currently allocated          */
    uint32_t x64_free;        /* x64_total - x64_used               */

    /* Emulated M68k guest RAM slots (per-task RAM pools) */
    uint32_t m68k_ram_total;  /* size of one slot in bytes          */
    uint16_t m68k_slots_total;/* total slot count                   */
    uint16_t m68k_slots_used; /* slots currently in use             */

    /* Scheduler task table */
    uint16_t tasks_total;     /* registered tasks                   */
    uint16_t tasks_running;   /* tasks in RUN/READY state           */
    uint16_t tasks_waiting;   /* tasks blocked on a signal          */
    uint16_t reserved;        /* alignment / future use             */
};

/* Fill *out with a current memory statistics snapshot.
 * Safe to call from any kernel context; does not allocate. */
void Mem_GetInfo(struct UaosMemInfo *out);

#endif /* UAOS_MEM_INFO_H */
