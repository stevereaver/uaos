---
type: Kernel Subsystem
title: AGA/ECS Custom Chipset Emulator
description: Sparse register emulator for the Amiga AGA/ECS custom chip and CIA window at 0xDFF000.
resource: /kernel/chipset/
tags: [aga, ecs, chipset, chip_emu, copper, dma, palette]
timestamp: 2026-06-26T17:00:00Z
---

# AGA/ECS Custom Chipset Emulator

The chipset emulator (`kernel/chipset/chip_emu.c`) is reached from the x86_64 page fault handler whenever M68k code touches the Amiga hardware register window at guest physical `0x00B00000–0x00DFFFFF`.  It presents the classic 0xDFF000 register block to the guest and keeps enough state to keep boot code, ROM sets, and Copper-list setup happy without yet performing real DMA rendering.

## Entry Points

- `chip_emu_read(offset, width_bytes)` — emulates a read from a chip register.
- `chip_emu_write(offset, value, width_bytes)` — emulates a write to a chip register.

`offset` is relative to the start of the chip window (`0x00B00000`).  Widths 1, 2 and 4 are handled.  Amiga custom registers are 16-bit, so 32-bit accesses fill two consecutive 16-bit registers in big-endian order.

## Tier 2 Register Behaviour

### Control state machines

| Register | Offset | Behaviour |
|---|---|---|
| `DMACON`  | `0x096` | SET/CLR state machine (bit 15 controls set vs clear). |
| `INTENA`  | `0x09A` | SET/CLR interrupt enable state. |
| `INTREQ`  | `0x09C` | SET/CLR interrupt request state. |
| `ADKCON`  | `0x09E` | SET/CLR audio/disk control state. |

Reads of these registers return the current state.  Writes with bit 15 set enable the bits in the lower 15 bits; writes with bit 15 clear disable them.

### Copper control

| Register | Offset | Behaviour |
|---|---|---|
| `COP1LC`   | `0x080` | 32-bit copper list 1 pointer. |
| `COP2LC`   | `0x084` | 32-bit copper list 2 pointer. |
| `COPJMP1`  | `0x088` | Strobe — execute copper list 1. |
| `COPJMP2`  | `0x08A` | Strobe — execute copper list 2. |
| `COPCON`   | `0x02E` | Copper control (dangerous/blitter-access bits). |

The copper emulator fetches instructions from `COP1LC`/`COP2LC`, executes `MOVE` to chipset registers, advances past `WAIT` targets, and skips the next instruction on `SKIP`.  An impossible `WAIT $FFFEFFFE` terminates the list.

### Beam position (read-only)

| Register | Offset | Behaviour |
|---|---|---|
| `VPOSR`  | `0x004` | Returns 0 for now (vertical beam position). |
| `VHPOSR` | `0x006` | Returns 0 for now (horizontal beam position). |

### Display/Bitplane registers

| Register | Offset | Purpose |
|---|---|---|
| `BPLCON0` | `0x100` | Bitplane depth, HIRES, HAM, DBLPF. |
| `BPLCON1` | `0x102` | Horizontal scroll. |
| `BPLCON2` | `0x104` | Playfield priorities. |
| `BPLCON3` | `0x106` | AGA bank/LOCT / sprite resolution. |
| `BPLCON4` | `0x10C` | AGA color-bank lower bits. |
| `BPL1MOD` | `0x108` | Bitplane modulo (odd planes). |
| `BPL2MOD` | `0x10A` | Bitplane modulo (even planes). |
| `DIWSTART`| `0x08E` | Display window start. |
| `DIWSTOP` | `0x090` | Display window stop. |
| `DDFSTART`| `0x092` | Data fetch start. |
| `DDFSTOP` | `0x094` | Data fetch stop. |
| `BPL1PT`–`BPL6PT` | `0x0E0`–`0x0F4` | Bitplane DMA pointers (OCS/ECS). |
| `BPL7PT`–`BPL8PT` | `0x0F8`–`0x0FC` | Additional bitplane pointers (AGA). |
| `SPR0PT`–`SPR7PT` | `0x120`–`0x13C` | Sprite DMA pointers (stored, not rendered). |

### AGA 256-entry color palette

The classic `COLOR00`–`COLOR31` addresses (`0x180`–`0x1BE`) are reused by AGA to access 256 24-bit colors.  The bank is selected by bits 13–15 of `BPLCON3` (`0x106`), and the `LOCT` bit (bit 9) selects whether a write updates the high or low nibbles of each color component:

- `LOCT=0`: bits 11–8 = R7–R4, bits 7–4 = G7–G4, bits 3–0 = B7–B4.  The high nibble is duplicated into the low byte for OCS compatibility, producing a 24-bit color.
- `LOCT=1`: bits 11–8 = R3–R0, bits 7–4 = G3–G0, bits 3–0 = B3–B0.  Only the low nibbles are updated, leaving the high nibbles untouched.

The resulting 256-entry palette is stored internally as host `0x00RRGGBB` values.

## Tier 3 — Bitplane Rendering

`chip_emu_render_frame()` executes the primary copper list (if any), then renders the active chipset state to the host linear framebuffer using `FB_PutPixel`.  It supports:

- 1–8 bitplanes with palette lookup.
- HAM6 (6 bitplanes, `HOMOD=1`): upper 2 bits select palette/modify-red/modify-green/modify-blue.
- EHB (6 bitplanes, `HOMOD=0`, `DBLPF=0`): colours 32–63 are half-brightness versions of colours 0–31.
- HAM8 and AGA 8-bit/24-bit palette modes via the 256-entry palette.

`BPLCON0` selects the depth and mode, `DIWSTART`/`DIWSTOP` define the display window, and `DDFSTART`/`DDFSTOP` help derive the fetch width.

## Page Fault Decoder

The page fault handler (`kernel/exec/page_fault_handler.c`) decodes the x86_64 instruction that caused the chip-window access and forwards it to `chip_emu_read`/`chip_emu_write`.  Tier 2 decoding supports:

- `MOV reg, [mem]` and `MOV [mem], reg` (8/16/32/64-bit).
- `MOV [mem], imm` (`0xC6`/`0xC7`).
- Read-modify-write `OR`, `AND`, and `XOR` with register or immediate operands.

For unknown instructions, the handler falls back to a 32-bit read/write and advances past the instruction so the guest does not crash.

## Integration with graphics.library

`graphics.library` now builds real copper lists:

- `MakeVPort(view, vp)` allocates a chip-RAM copper list for a `ViewPort` and populates it with `MOVE`s for `DMACON`, `BPLCON0`–`BPLCON4`, display window, bitplane pointers, and the first 32 `COLORxx` registers from the `ColorMap`.
- `MrgCop(view)` merges per-`ViewPort` copper lists into one master list attached to the `View`.
- `LoadView(view)` passes the merged copper list to `chip_emu_copper_jump()` and then calls `chip_emu_render_frame()`.  Views without a merged copper list still use the CPU-drawn bitmap fallback.

The display-mode database (`graphics_lib.c`) includes AGA variants of each mode (bit 31 set) so `MakeVPort` can pick AGA depth and timing parameters.

## Tier 1 — Accuracy for Games and Productivity

### Per-scanline Copper

The copper no longer runs only once at the start of `chip_emu_render_frame()`.  Instead, the render loop resets the copper PC to `COP1LC` at the top of each frame and then runs `copper_run_to_beam(vpos, 0)` before rendering every scanline.  `WAIT` instructions compare the masked beam position and stall the copper until the target line is reached, so raster-bar colour changes, mid-frame pointer updates, and other copper effects now take effect on the correct line.  `SKIP` is evaluated using the same beam comparison.

`COPCON` bit 0 (the "dangerous" bit) is now enforced: the copper can only write to the Blitter register block (`0x040`–`0x058`) when the dangerous bit is set, matching hardware behavior.

### Bitplane Layout, BPLMOD, and Horizontal Scrolling

`bpl_line_ptr()` now uses the real hardware fetch width and the correct per-plane modulo:

- Odd-numbered planes (1, 3, 5, 7) use `BPL1MOD`.
- Even-numbered planes (2, 4, 6, 8) use `BPL2MOD`.
- The interleaved/non-interleaved distinction is handled by the separate plane base pointers and moduli exactly as real Amiga software sets them up.

`render_scanline()` applies the horizontal scroll offsets from `BPLCON1`: `PF1H` (bits 3–0) and `PF2H` (bits 7–4).  In dual-playfield mode the even/odd planes use the appropriate playfield scroll; in single-playfield mode all planes use `PF1H`.

### Blitter

The AGA Blitter is triggered by writes to `BLTSIZE` (`0x058`).  It supports area-mode rectangle copies/fills with:

- A/B/C/D channel enable bits from `BLTCON0`.
- 8-bit minterms from `BLTCON0`/`BLTCON1`.
- A/B barrel shifts (`ASH`/`BSH` in `BLTCON0`/`BLTCON1`).
- First/last word masks (`BLTAFWM`/`BLTALWM`).
- Source and destination modulo (`BLTAMOD`/`BLTBMOD`/`BLTCMOD`/`BLTDMOD`).
- Descending address mode (`BLTCON1` `DESC`).

Blitter busy timing is now implemented: `BLTSIZE` sets the busy flag and a PIT-tick counter, and `DMACONR` (register `0x002`) reflects `BLITZ` (bit 14) while the blitter is active.  The busy duration is scaled to the blit size (clamped to a few PIT ticks) so that software polling `DMACONR` sees the expected delay.  True slot-by-slot DMA contention is not yet modelled.

**Line mode and area-fill mode** — `blitter_execute()` now has a basic Bresenham line drawer for `BLTCON1` bit 0 (`LINE`) and a per-row fill state for `BLTCON1` bits 3–4 (`IFEFE`/`EFE`).  These are approximations: the real line-mode delta/octant encoding and polygon edge-fill semantics are simplified.


### Hardware Sprites

Sprites are now fetched from `SPRxPT` by DMA each scanline rather than drawn from manually loaded data registers.  The sprite DMA pointer is reset to `SPRxPT` at the start of each frame and advances by the line's data size.  AGA 64-pixel wide sprites are supported when `BPLCON3` bit 10 is set; otherwise 16-pixel OCS/ECS sprites are used.  Attached pairs give 32 colours (palette entries 16–31).  AGA 32-colour palette banking is selected via `BPLCON3` bits 13–15; the attached pair index is expanded to 5 bits in this mode, giving sprite colours 16–47.

Sprite collision detection is now implemented: `CLXDAT` (`0x00E`) is read-and-clear, and `CLXCON` (`0x016`) accepts the control mask.  A frame-level bounding-box test detects overlapping sprite pairs and sets the corresponding `CLXDAT` bits.  Bitplane-sprite and bitplane-bitplane collisions are not yet detected.

### Beam Timing and Display Window

`VPOSR` and `VHPOSR` are now updated each PIT tick by `chip_emu_beam_tick()`.  PAL mode uses 312 lines per frame and NTSC mode uses 262 lines, with a fractional advance approximation tied to the 100 Hz PIT.

The display window derivation has been improved:

- `DIWSTRT`/`DIWSTOP` horizontal values are in low-resolution pixels with the usual `$80` origin.
- `DIWSTOP` horizontal stop is written with the high bit stripped; the hardware implies `$100` for the right-hand side.
- `DIWSTOP` vertical stop uses the hardware quirk that forces bit 8 to the complement of bit 7, allowing PAL wrap-around without `DIWHIGH`.
- `DDFSTART`/`DDFSTOP` derive the fetch width using the hardware formula: words = `(DDFSTOP - DDFSTART) / 4 + 2`, giving 8 lores pixels or 16 hires pixels per word.  This matches the standard `$38`–`$D0` fetch producing a 320-pixel low-res line.

### Dual-Playfield and Interlace

**Dual-playfield** rendering (`BPLCON0` `DBLPF`) is now implemented.  Even planes form playfield 1 and odd planes form playfield 2.  Each playfield uses its own `BPLCON1` horizontal scroll (`PF1H`/`PF2H`), and `BPLCON2` priority bits (`PF2P0-PF2P2` vs `PF1P0-PF1P2`) decide which playfield is drawn when both are non-transparent.  Playfield 1 indexes palette entries 0–15; playfield 2 uses palette entries 8–15 for 6-plane modes or 16–31 for 8-plane AGA modes.

**Interlace** mode (`BPLCON0` `LACE`) is approximated by doubling the vertical display range and fetching bitplanes from line `y/2`.  Long/short fields are not modelled yet, but the vertical resolution doubling is in place.

### Interrupt Delivery

The chip emulator now computes the highest enabled M68k interrupt level from `INTREQ` & `INTENA` and calls `m68k_set_irq()`:

- INTREQ bits 0–4 → level 1
- INTREQ bits 5–8 → level 2
- INTREQ bits 9–12 → level 3
- INTREQ bits 13–14 → level 4

VBlank, CIA timers, and audio DMA all set the correct `INTREQ` bits and drive the Musashi interrupt line.  CIA-A timers map to `TIMERA`/`TIMERB` (bits 13/14); CIA-B timers are mapped to `EXTER` (level 1) until a dedicated level-6 path is added.

### Reset State and LEDs

`chip_emu_reset()` is called from `uaos_kernel_main()` at boot and initializes all chipset registers to hardware-correct defaults, including `DIWSTRT`/`DIWSTOP`, `DDFSTART`/`DDFSTOP`, `BPLCON4`, first/last word masks, and CIAA port A pull-ups.  The power LED state is tracked through CIAA `PRA` bit 3 (active low) and exposed via `chip_emu_power_led()`.

## Tier 5 — Audio, CIA, and Disk

### Paula Audio

Four audio channels are tracked with `AUDxLCH/LCL`, `AUDxLEN`, `AUDxPER`, `AUDxVOL`, and `AUDxDAT`.  `chip_emu_audio_advance()` advances the DMA pointer by a configurable number of Amiga master-clock cycles, fetching the next 16-bit word when the period counter expires so the high byte is played first, then the low byte.

A host audio subsystem in `kernel/audio/` provides a 48 kHz stereo mixer and a pluggable backend interface (`AudioBackend`).

- `audio_init()` probes backends in order: AC97 first, PC speaker as a fallback.
- `audio_tick()` (called every PIT tick) generates 480 stereo frames per tick and writes them to a lock-free ring buffer.  The active backend's `service()` routine consumes from the ring buffer.
- The mixer resamples the four Paula channels to a fixed 48 kHz output, applies per-channel `AUDxVOL` (0–64), applies the one-pole LED-style low-pass filter, and pans channels 0/2 to the left and 1/3 to the right.  Output is clamped to signed 16-bit.
- The AC97 backend (`kernel/audio/ac97.c`) uses the Intel ICH AC97 controller (PCI class 0x04/0x01/0x00), double-buffered DMA descriptors, and a 48 kHz 16-bit stereo stream.
- The PC speaker backend (`kernel/audio/pc_speaker.c`) remains as a low-fidelity fallback.
- Boot-time tests `audio_sine_test()` and `audio_pattern_test()` set up a Paula channel with a sine wave and verify the mixer path is active.

### CIA-A and CIA-B

`CIA-A` (`0xBFE001`) and `CIA-B` (`0xBFD000`) are now decoded by the chip emulator.  Implemented:

- Timer A and Timer B with latches, counters, and `CRA`/`CRB` control (start/stop, one-shot/continuous, force-load).
- `ICR` read clears pending interrupt status; writing `ICR` sets/clear interrupt masks.
- Basic 24-bit TOD counter registers.
- Port A/B data and direction registers are maintained; power LED is tracked through CIAA `PRA` bit 3.
- CIA-B interrupts now drive M68k level 6 (the real Amiga routing).  CIA-A timers continue to use `INTREQ` bits 13/14 (level 4); keyboard serial data uses `INTREQ` bit 3 (level 1).
- CIA-A `SDR` is wired to the PS/2 keyboard driver: `chip_emu_poll_ps2_keyboard()` drains the PS/2 ring buffer every PIT tick and pushes translated bytes into the CIA-A SDR queue, raising the keyboard interrupt.  Reading CIAA `SDR` pops the next byte.  CIA-A SDR writes (keyboard commands) are accepted and ignored for now.

### Paula Disk/Serial/Parallel

**Serial port** — `SERDAT` writes now transmit the low byte to the host COM1 UART (`0x3F8`).  `SERDATR` reads poll COM1 for received bytes and return the byte with the receive-buffer-full bit set.  `SERPER` is stored but does not affect the baud rate yet.

**Parallel port** — CIA-B `PRB` writes are forwarded to the host LPT1 data port (`0x378`) with a brief strobe pulse on the control port (`0x37A`).

**Disk DMA** — `DSKLEN` now performs a basic DMA burst when its DMA-enable bit (bit 15) is set.  `DSKPT` (registers `0x020`/`0x022`) is the chip-RAM transfer address, `DSKLEN` bits 0–13 give the word count, and bit 14 selects read vs write.  A synthetic 880 KiB ADF buffer is copied to or from chip RAM, and the `DSKBLK` interrupt (`INTREQ` bit 1) is raised on completion.  `DSKSYNC` is stored but not yet used as a sync trigger.  `DSKDAT` single-word reads/writes also advance through the synthetic buffer.  This is enough for software that only needs data to appear after issuing a disk DMA command, but it is not an accurate MFM floppy controller.

### Sprite Collision Detection

`CLXDAT` now detects three kinds of overlap on each scanline:
- **Bit 0** — even bitplane vs odd bitplane collision (or playfield 1 vs playfield 2 in dual-playfield mode).
- **Bits 1–8** — bitplane vs sprite 0–7 collisions.
- **Bits 8+** — sprite-sprite bounding-box collisions (existing coarse frame-level test).

`CLXCON` is still accepted but not yet used to mask individual collision classes.

## Tier 4 — Expert / UAE-Level Compatibility

A subset of the hardest items has been implemented; the rest is the long tail that makes UAE-level accuracy a multi-year project.

### CPU / Chip RAM Coherency

`m68k_write_memory_8/16/32()` in `emulation/uaos_m68k_glue.c` now issue a compiler barrier (`GUEST_WRITE_BARRIER()`) after every guest RAM write.  This prevents the compiler from reordering stores and ensures that self-modifying code and CPU-to-chipset writes are visible to subsequent reads as soon as possible.

### Copper Interrupts

`copper_run_to_beam()` now raises the `COPER` interrupt (`INTREQ` bit 6) when the copper reaches the end-of-list `WAIT $FFFEFFFE`.  Copper `MOVE` instructions that write to `INTREQ` already drive `chip_emu_update_irq()` through the normal register path.

### Chipset Identification

- `VPOSR` (`0x004`) reads return the dynamic beam position with the AGA identifier bit (bit 15) set.
- `DENISEID` (`0x07C`) reads return `0x00F8` (AGA Denise/Lisa ID).

### Genlock and Sprite Priority

`BPLCON2` genlock/external-sync bits are preserved in the register state.  The dual-playfield priority logic uses `BPLCON2` bits 0–5 (`PF2P`/`PF1P`).  Full genlock video-mixing and external sync are not implemented because there is no external video input.

### Advanced Sprite Features

Sprite rendering now supports an AGA super-hires heuristic (selected via `BPLCON3` bit 9), where each sprite pixel maps to one low-resolution framebuffer pixel instead of two.  The 32-colour bank selection and 64-pixel wide modes remain in place.  Sprite-vs-playfield priority is now implemented: when any `BPLCON2` SP bit (bits 8–12) is set, non-transparent sprite pixels are drawn only where the bitplanes are transparent.  Exact per-sprite SP bits, HAM sprite interactions, and full AGA sprite resolution selection are not yet implemented.

### CPU Cache Synchronization and Cycle Timing

The guest-write barrier in `emulation/uaos_m68k_glue.c` has been upgraded from a plain compiler barrier to a full x86 `mfence` memory fence.  This flushes the store buffer and makes M68k writes visible to the chipset emulator on multi-core hosts.  A non-x86 build would need to replace the `mfence` with the appropriate `dmb`/`sync` primitive for that host CPU.

A global M68k cycle counter (`g_m68k_cycles`) is now accumulated after each `m68k_execute()` slice.  `chip_emu_m68k_cycles()` exposes the running total.  This is the foundation for a future CPU/chipset timing lock; for now the chipset is still advanced from the PIT tick rather than from the M68k cycle count.

### Cycle-Accurate DMA Arbitration (First Pass)

A simplified DMA slot table has been added to `chip_emu.c`.  Each PAL line has 226 abstract slots, NTSC has 227, and 4 slots per line are reserved for refresh.  A helper API is provided:

- `dma_slot_reset()` — clears the line table and allocates refresh slots.
- `dma_slot_alloc(channel, start, count)` — reserves consecutive free slots.
- `dma_slot_release(channel)` / `dma_slot_count(channel)` — introspection helpers.

Priority is refresh > bitplane > copper > sprite > audio > disk > blitter > CPU.  `chip_emu_render_frame()` now builds a fresh table every scanline and allocates slots to bitplanes, sprites, the copper, and (if busy) the blitter.  The copper only advances when it has been granted slots: each instruction consumes 2 slots, and `copper_run_to_beam()` stalls when the budget is exhausted.  The blitter tracks `g_blitter_words_remaining` and consumes leftover slots word-by-word; the busy flag is cleared only when the remaining word count reaches zero.  Remaining slots are marked as CPU slots, and the number of non-CPU slots is accumulated into `g_cpu_stolen_cycles` as a proxy for CPU wait states.

A boot-time test, `chip_emu_dma_test()`, builds a 120-instruction copper list at guest RAM offset `0x10000`, runs one frame, and verifies that the copper advanced but stalled before the end-of-list `WAIT`.  The test is invoked from `uaos_kernel_main()` and prints `PASSED` or `FAILED`.

### Interlace Timing

Interlace (`BPLCON0` `LACE`) now alternates long and short fields: PAL alternates between 312 and 313 lines, NTSC between 262 and 263.  The field index is tracked in `g_interlace_field` and toggles every VBlank.  Bitplane fetches use the field index to select the correct alternating line (`(y + field) / 2`).

### Blitter Exact Semantics

The Blitter has been upgraded to use real Amiga line-mode and fill semantics:

- **Line mode** now decodes `BLTCON1` bits 4-7 (octant) and bits 1-2 (SING/sign).  `BLTAMOD` and `BLTBMOD` are interpreted as the Bresenham deltas (`2*minor - 2*major` and `2*minor`), and `BLTAPTL` supplies the starting `(x,y)`.  The line drawer respects the octant-derived major/minor axis and sign bits, and it plots pixels directly into `BLTDPT` using `BLTDMOD` as the row stride.
- **Area fill** now implements the correct inclusive (`IFEFE`, bit 3) and exclusive (`EFE`, bit 4) polygon rules.  The fill state toggles on every A-channel edge pixel; inclusive mode draws the edge pixel with the new state, while exclusive mode draws it with the old state.
- **Descending mode (`DESC`)** is now handled correctly for overlapping source/destination copies: the start pointers are moved to the end of the row, the per-word stride is reversed, and the first/last word masks (`BLTAFWM`/`BLTALWM`) are swapped.  The modulo is no longer negated; the programmer is expected to set the correct full-row modulo for reverse transfers.
- **BLTAPTL/BLTAFWM/BLTALWM** interactions are verified: line mode uses `BLTAPTL` for the starting pixel, while area mode applies `BLTAFWM` to the first word and `BLTALWM` to the last word (with the mask ends swapped in `DESC` mode).
- The Blitter busy flag and `g_blitter_words_remaining` are driven by the DMA slot table, so the blitter consumes slots word-by-word across scanlines.

Two boot-time tests exercise the new Blitter code:
- `chip_emu_line_test()` draws a diagonal line from (10,10) to (20,18) using octant 0 and verifies that destination pixels are set.
- `chip_emu_fill_test()` performs an exclusive fill between two vertical edges and checks that the interior bits are filled while the edge bits are not.

Both tests are invoked from `uaos_kernel_main()` and print `PASSED`/`FAILED`.

### Beam Position Derived from Color Clock

`VPOSR`/`VHPOSR` reads and the Copper `WAIT` logic now derive the beam position from the M68k cycle counter, which is treated as the Amiga color clock:

- PAL color clock is ~7.09 MHz, NTSC is ~7.16 MHz.  These are the same frequencies as the M68k CPU on real Amigas.
- `beam_cycles_now()` returns the elapsed M68k cycles since the start of the current frame.
- `beam_position_from_cycles()` converts the elapsed color-clock ticks into a vertical line (`VPOSR`) and horizontal counter (`VHPOSR`).
- `VPOSR` reads return the vertical line with the AGA identifier in bit 15.
- `VHPOSR` reads return the horizontal counter and the extra vertical bit.
- `copper_run_to_beam()` now queries the color-clock beam position internally, so `WAIT` comparisons happen against the real color-clock beam rather than the 100 Hz PIT approximation.
- `chip_emu_vblank()` recomputes the color-clock parameters after any field change and resets the frame-start cycle counter, so the next frame is aligned to the color-clock boundary.
- Horizontal blanking and display-region boundaries are not yet modelled; the beam counter runs continuously across the entire line.

A boot-time test, `chip_emu_raster_test()`, builds a small copper list that `WAIT`s for line 100 and line 110, changes `COLOR00` each time, and verifies that the second color was written.  The test is invoked from `uaos_kernel_main()` and prints `PASSED` or `FAILED`.

### Full AGA Sprite Control

Sprite rendering has been upgraded to support AGA resolution, colour bank, and priority semantics:

- **Sprite resolution** is decoded from `BPLCON3` bits 10-12 (`SPRES`):
  - `000` = ECS/OCS lores (16 sprite pixels, 2 lores pixels each)
  - `001` = lores (same as ECS/OCS)
  - `010` = hires (32 sprite pixels, 1 lores pixel each)
  - `011` = superhires (64 sprite pixels, 1 lores pixel each)
- **Sprite colour bank** is selected by `BPLCON3` bits 13-15, giving the sprite palette base `16 + bank * 16`.
- **Per-sprite playfield priority** is implemented from `BPLCON2` bits 8-12, mapped to sprite pairs (SP0 -> pair 0, SP1 -> pair 1, ...).  When a pair's SP bit is set, those sprites are drawn behind the bitplanes.
- **Attached-pair colour expansion** now uses the correct even/odd pair member (e.g. sprite 0 attaches to sprite 1, sprite 2 to sprite 3, etc.).  In AGA superhires/attached mode, a fifth bit is added from the pair index to reach 32 colours.
- **Sprite transparency** is honoured: pixels with `DATA`/`DATB` index `0` are skipped (transparent) unless the sprite is attached.  Sprite pixels are drawn with the normal palette lookup, so they remain visible against HAM and EHB backgrounds.
- **Sprite DMA fetch control** reads `DATA`/`DATB` words only between the sprite's vertical start and vertical stop.  The DMA pointer is reset to `SPRnPT` at the start of each frame.
- **Display-window clipping** clips sprite pixels to the DIW horizontal bounds.

A boot-time test, `chip_emu_sprite_test()`, sets up a low-res sprite with a known `DATA`/`DATB` pattern, enables sprite DMA, renders one line, and verifies the DMA-fetched data.  It is invoked from `uaos_kernel_main()` and prints `PASSED` or `FAILED`.

### High-Quality Paula Audio Output

The PC speaker stop-gap has been replaced with a real PCM DAC backend and an abstracted audio pipeline:

- `kernel/audio/audio.h` defines the `AudioBackend` interface (`init`, `service`, `shutdown`, `name`).
- `audio_init()` probes `audio_backend_ac97` first; if the Intel ICH AC97 controller (PCI class 0x04/0x01/0x00) is not found, it falls back to `audio_backend_pcspk`.
- `audio.c` produces a fixed 48 kHz stereo mix into a 16384-frame ring buffer.  The mixer advances the Paula DMA state with the exact PAL Amiga master-clock delta per sample (≈73.9 cycles/sample) and applies volume, LED filtering, and panning.
- `kernel/audio/ac97.c` uses double-buffered bus-master descriptors (two 2048-sample halves) so the mixer can fill one half while the hardware plays the other.
- `kernel/audio/pc_speaker.c` remains as a low-fidelity fallback that drives the PIT channel 2 square wave at the 100 Hz tick rate.
- `audio_sine_test()` and `audio_pattern_test()` are invoked from `uaos_kernel_main()` after `audio_init()` to verify the Paula→mixer→backend path.

### What Remains for True 100% / UAE-Level Compatibility

- **Accurate DMA slot table** aligned with the real Agnus slot layout (bitplane/Copper/sprite/audio/disk slots at specific horizontal positions, refresh slot timing, and DMA-off cycles).
- **Blitter line/area-fill remaining edge cases** such as real line-mode texture/B channel usage, exact `BLTAPTL` accumulator loading, and per-pixel DMA slot timing for the actual data transfer.
- **AGA sprite fine details** such as exact superhires pixel scaling, border sprites, and sprite-to-sprite priority ordering.
- **Real MFM floppy controller** emulation, `DSKSYNC`-based transfer start, and loading ADF images from disk.
- **Host parallel port** bidirectional I/O and full Amiga serial port emulation (baud-rate control, break, status bits).
- **CPU/chipset timing lock** that advances the beam and subsystems from the M68k cycle counter rather than from PIT ticks, with cycle-accurate memory contention and horizontal blanking modelling.
- **Undocumented register behaviors, hardware quirks, and chipset revisions** (OCS/ECS/AGA differences, Alice/Lisa variants, A1200 vs A4000).
- **Zorro III / AutoConfig**, A4000 Gayle IDE, RTC (MSM6242/RP5C01), PCMCIA, and full genlock.
- **Real MFM floppy controller** emulation, `DSKSYNC`-based transfer start, and loading ADF images from disk.
- **Host parallel port** bidirectional I/O and full Amiga serial port emulation (baud-rate control, break, status bits).
- **CPU/chipset timing lock** that advances the beam and subsystems from the M68k cycle counter rather than from PIT ticks, with cycle-accurate memory contention and horizontal blanking modelling.
- **Undocumented register behaviors, hardware quirks, and chipset revisions** (OCS/ECS/AGA differences, Alice/Lisa variants, A1200 vs A4000).
- **Zorro III / AutoConfig**, A4000 Gayle IDE, RTC (MSM6242/RP5C01), PCMCIA, and full genlock.
