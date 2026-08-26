/* mem_info.c — UAOS kernel-exported memory query API
 *
 * Implements Mem_GetInfo(), a single snapshot query that gathers memory
 * statistics from the x86-64 userspace heap, the emulated M68k guest RAM
 * slots, and the scheduler task table.  Consumed by the in-kernel C:mem
 * command and by the SYSCALL_MEMINFO handler exposed to x86-64 userspace
 * programs (e.g. the on-disk C:avail command).
 */

#include "mem_info.h"
#include "elf64_loader.h"
#include "task.h"
#include "../../emulation/uaos_emu.h"   /* GUEST_RAM_SIZE */
#include <stdint.h>

void Mem_GetInfo(struct UaosMemInfo *out)
{
    if (!out)
        return;

    /* x86-64 userspace heap (bump arena; reclaimed when no X64 tasks live) */
    uint32_t heap_total = ELF64_HeapSize();
    uint32_t heap_used  = ELF64_HeapUsed();
    out->x64_total = heap_total;
    out->x64_used  = heap_used;
    out->x64_free  = (heap_total > heap_used) ? (heap_total - heap_used) : 0u;

    /* Emulated M68k guest RAM slots */
    int slots_total = 0, slots_used = 0;
    Task_M68kSlotCount(&slots_total, &slots_used);
    out->m68k_ram_total  = GUEST_RAM_SIZE;
    out->m68k_slots_total = (uint16_t)slots_total;
    out->m68k_slots_used  = (uint16_t)slots_used;

    /* Scheduler task table */
    int tt = 0, tr = 0, tw = 0;
    Task_GetCounts(&tt, &tr, &tw);
    out->tasks_total   = (uint16_t)tt;
    out->tasks_running = (uint16_t)tr;
    out->tasks_waiting = (uint16_t)tw;
    out->reserved      = 0;
}
