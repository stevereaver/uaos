/* cmd_crash.c — C:crash — deliberate kernel fault for panic-path testing
 *
 * Triggers an unhandled kernel-mode #PF so the exception dump in
 * ISR_Dispatch (GPRs, CR2, stack, task context) can be verified against
 * tools/symbolize.sh on the host.  This kills the kernel — only use it to
 * test the panic path.
 */

#include "cmd_internal.h"

void Cmd_Crash(NativeCmdCtx *ctx, const char *args)
{
    (void)ctx; (void)args;
    PRINT("crash: triggering deliberate kernel fault...");
    /* Write to a non-canonical address — always faults, never mapped. */
    *(volatile uint64_t *)0xDEADBEEF00000000ULL = 0xDEADBEEFCAFEBABEULL;
    PRINT("crash: write did not fault?!");
}
