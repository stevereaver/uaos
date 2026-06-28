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

/* Reset and timing */
void     chip_emu_reset(void);
int      chip_emu_power_led(void);
void     chip_emu_poll_ps2_keyboard(void);
uint64_t chip_emu_m68k_cycles(void);
int      chip_emu_dma_test(void);
int      chip_emu_line_test(void);
int      chip_emu_fill_test(void);
int      chip_emu_raster_test(void);
int      chip_emu_sprite_test(void);

/* Tier 4: VBlank timing */
void     chip_emu_vblank(void);
uint32_t chip_emu_vblank_count(void);
void     chip_emu_beam_tick(uint32_t tick_counter);

/* Tier 5: CIA timers */
void     chip_emu_cia_tick(void);

/* Tier 5: Paula audio */
void     chip_emu_audio_tick(void);          /* legacy one-tick stub */
void     chip_emu_audio_advance(uint32_t amiga_cycles);
int8_t   chip_emu_audio_sample_8bit(int ch);
uint8_t  chip_emu_audio_volume(int ch);
void     chip_emu_audio_set_channel(int ch, uint32_t ptr, uint16_t len, uint16_t per, uint16_t vol);
void     chip_emu_audio_set_dmacon(uint16_t dmacon);

#endif /* UAOS_CHIP_EMU_H */
