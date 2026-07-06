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

/* Guest RAM helpers used by the floppy DMA path and other chipset subsystems. */
uint8_t  chip_read_u8(uint32_t addr);
void     chip_write_u8(uint32_t addr, uint8_t v);
uint16_t chip_read_u16(uint32_t addr);
void     chip_write_u16(uint32_t addr, uint16_t v);

/* Raise INTREQ bits and update the M68k IRQ level. */
void     chip_emu_raise_intreq(uint16_t bits);

/* Tier 3: execute a copper list and render the resulting display state.
 * chip_emu_copper_jump(list, addr): if addr != 0 it becomes the active
 * copper list for that list, then the copper is executed. */
void     chip_emu_copper_jump(int list, uint32_t addr);
void     chip_emu_render_frame(void);

/* Reset and timing */
void     chip_emu_reset(void);
int      chip_emu_power_led(void);
void     chip_emu_poll_ps2_keyboard(void);
void     chip_emu_serial_poll(void);
void     chip_emu_set_keyboard_route(int to_cia);
uint64_t chip_emu_m68k_cycles(void);
int      chip_emu_dma_test(void);
int      chip_emu_line_test(void);
int      chip_emu_fill_test(void);
int      chip_emu_fill_complex_test(void);
int      chip_emu_blitter_busy_test(void);
int      chip_emu_blitter_desc_test(void);
int      chip_emu_raster_test(void);
int      chip_emu_sprite_test(void);
int      chip_emu_sprite_border_test(void);
int      chip_emu_sprite_priority_test(void);
int      chip_emu_sprite_superhires_test(void);
int      chip_emu_sprite_subpixel_test(void);
int      chip_emu_parallel_test(void);
int      chip_emu_serial_test(void);
int      chip_emu_agnus_slot_test(void);
int      chip_emu_timing_contention_test(void);
int      chip_emu_hblank_test(void);
int      chip_emu_ham8_test(void);
int      chip_emu_64color_test(void);
int      chip_emu_diwhigh_test(void);

/* Tier 4: VBlank timing and cycle-driven scheduler */
void     chip_emu_vblank(void);
uint32_t chip_emu_vblank_count(void);
void     chip_emu_beam_tick(uint32_t tick_counter);
void     chip_emu_run_to_cycle(uint64_t target_cycles);
uint64_t chip_emu_stolen_cycles(void);
void     chip_emu_cpu_chipram_access(uint32_t addr, int is_write);
int      chip_emu_timing_lock_test(void);

/* Tier 5: CIA timers */
void     chip_emu_cia_tick(void);

/* Tier 5: Paula audio */
void     chip_emu_audio_tick(void);          /* legacy one-tick stub */
void     chip_emu_audio_advance(uint32_t amiga_cycles);
int8_t   chip_emu_audio_sample_8bit(int ch);
uint8_t  chip_emu_audio_volume(int ch);
void     chip_emu_audio_set_channel(int ch, uint32_t ptr, uint16_t len, uint16_t per, uint16_t vol);
void     chip_emu_audio_set_dmacon(uint16_t dmacon);

/* Tier 6: Hardware sprite management.
 *
 * GetSprite() reserves a hardware sprite slot (0..7) and points its DMA
 * pointer at the supplied sprite data.  Returns the slot number on success
 * or -1 if the slot is already in use.  Pass SPRITE_RESERVED to mark a slot
 * as in-use without installing new data (used by GetSprite's "steal" path).
 * FreeSprite() releases a previously reserved slot.
 * MoveSprite() updates a sprite's vertical/horizontal position registers.
 * ChangeSprite() swaps the sprite data pointer for an already-reserved slot.
 * The sprite pointer is a guest RAM address of a 16-word sprite image:
 *   [0]=VSTART<<8|HSTART, [1]=VSTOP<<8|control, [2..]=DATA/DATB pairs. */
#define SPRITE_COUNT 8
int      chip_emu_get_sprite(int slot, uint32_t sprite_ptr);
void     chip_emu_free_sprite(int slot);
void     chip_emu_move_sprite(int slot, int16_t x, int16_t y);
void     chip_emu_change_sprite(int slot, uint32_t sprite_ptr);
int      chip_emu_sprite_in_use(int slot);

/* Tier 6: FMODE / AGA fetch mode.  Returns the current FMODE register value
 * and exposes the bitplane/sprite fetch width in bytes (2, 4, or 8). */
uint16_t chip_emu_fmode(void);
int      chip_emu_bpl_fetch_width(void);
int      chip_emu_spr_fetch_width(void);

#endif /* UAOS_CHIP_EMU_H */
