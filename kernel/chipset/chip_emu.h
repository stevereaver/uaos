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

#endif /* UAOS_CHIP_EMU_H */
