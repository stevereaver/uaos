/*
 * chip_emu.h — UAOS AGA/ECS custom chip emulator public interface
 *
 * Exposes the two entry points used by the x86_64 page fault handler when
 * an emulated M68k instruction touches the Amiga hardware register window.
 */

#ifndef UAOS_CHIP_EMU_H
#define UAOS_CHIP_EMU_H

#include <stdint.h>

void     chip_emu_write(uint32_t offset, uint32_t value, int width_bytes);
uint32_t chip_emu_read (uint32_t offset, int width_bytes);

/* Tier 3: execute a copper list and render the resulting display state.
 * chip_emu_copper_jump(list, addr): if addr != 0 it becomes the active
 * copper list for that list, then the copper is executed. */
void     chip_emu_copper_jump(int list, uint32_t addr);
void     chip_emu_render_frame(void);

#endif /* UAOS_CHIP_EMU_H */
