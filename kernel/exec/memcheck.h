/* memcheck.h — Mungwall-style heap debugging for exec.library (UAOS-69)
 *
 * When enabled, AllocMem wraps payloads with guard words, FreeMem
 * validates them and poison-fills the freed block, and every live
 * allocation is tracked (addr/size/allocating task) so violations can be
 * attributed.  Implemented in exec/dos_lib.c where the guest free-list
 * allocator lives.
 */
#ifndef UAOS_MEMCHECK_H
#define UAOS_MEMCHECK_H

#include <stdint.h>

int      Memcheck_IsEnabled(void);
void     Memcheck_SetEnabled(int on);
int      Memcheck_LiveCount(void);

/* Walk live tracked allocs + both free lists; violations go to klog.
 * Returns the number of violations found. */
uint32_t Memcheck_Scan(void);

/* Deliberate tail-guard overwrite; returns violations found (>=1). */
uint32_t Memcheck_SelfTest(void);

/* Dump a free list chain (0 = chip pool slot, 1 = fast pool slot)
 * through emit().  For C:memcheck dump. */
void     Memcheck_DumpList(uint32_t which, void (*emit)(const char *line));

#endif /* UAOS_MEMCHECK_H */
