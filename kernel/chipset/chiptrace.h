/* chiptrace — custom-chip / CIA access tracer (UAOS-68)
 *
 * Instruments chip_emu_read()/chip_emu_write() and optionally samples the
 * M68k program counter using the Musashi disassembler.  All output goes
 * through klog (KLOG_CHIP subsystem) — never the WM paint path.
 *
 * Classes (bitmask argument to Chiptrace_Enable):
 */
#ifndef UAOS_CHIPTRACE_H
#define UAOS_CHIPTRACE_H

#include <stdint.h>

#define CT_CIA    0x01u    /* CIA-A/B registers (0xBFD000-0xBFE0FF region)  */
#define CT_PAULA  0x02u    /* Paula audio regs AUD0-3 (regoff 0x0A0-0x0DF)  */
#define CT_DISK   0x04u    /* Floppy/disk regs (DSK*, incl. DSKBYTR/DSKSYNC)*/
#define CT_CHIP   0x08u    /* All other custom-chip registers               */
#define CT_ALLREG (CT_CIA | CT_PAULA | CT_DISK | CT_CHIP)

#define CT_PC     0x100u   /* Sampled M68k PC disassembly (rate-limited)    */

/* Enable tracing of the given classes.  Replaces the current mask. */
void Chiptrace_Enable(uint32_t mask);
void Chiptrace_Disable(void);
int  Chiptrace_Enabled(void);

/* Stats / status for C:chiptrace. */
void     Chiptrace_Stats(uint32_t *mask, uint32_t *emitted,
                         uint32_t *dropped, uint32_t *pc_lines);
void     Chiptrace_ClearStats(void);
void     Chiptrace_SetPcRate(uint32_t ticks_per_line); /* 0 disables CT_PC */
uint32_t Chiptrace_GetPcRate(void);

/* Hot path — called from chip_emu_read()/chip_emu_write().
 * Returns immediately when tracing is off or the class is masked out.
 * 'offset' is the chip-window offset (relative to 0xB00000). */
void Chiptrace_Hit(uint32_t offset, uint32_t value, int width, int is_write);

/* Called after each m68k_execute() slice; emits one disassembled instruction
 * at the current guest PC at most once per g_pit_ticks interval. */
void Chiptrace_PcSample(void);

#endif /* UAOS_CHIPTRACE_H */
