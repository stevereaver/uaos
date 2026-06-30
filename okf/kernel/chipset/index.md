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

**Line mode and area-fill mode** — `blitter_execute()` now has a stateful Bresenham line drawer for `BLTCON1` bit 0 (`LINE`) and a per-row fill state for `BLTCON1` bits 3–4 (`IFEFE`/`EFE`).

- Line mode uses `BLTAPTL` as the Bresenham accumulator, applies the B-channel texture (`BLTBDAT`) through the `BLTCON0` minterm, honours the `SING` bit, and draws exactly one pixel per allocated Blitter DMA slot.
- Area-fill mode now processes each word from MSB (bit 15) to LSB (bit 0), matching the Amiga's left-to-right screen layout, and carries the fill state across words within the same row.
- A dedicated `chip_emu_fill_complex_test()` exercises a self-intersecting polygon (bowtie) to verify that the fill state toggles at each edge and carries across word boundaries.


### Hardware Sprites

Sprites are now fetched from `SPRxPT` by DMA each scanline rather than drawn from manually loaded data registers.  The sprite DMA pointer is reset to `SPRxPT` at the start of each frame and advances by the line's data size.  AGA 64-pixel wide sprites are supported when `BPLCON3` bit 10 is set; otherwise 16-pixel OCS/ECS sprites are used.  Attached pairs give 32 colours (palette entries 16–31).  AGA 32-colour palette banking is selected via `BPLCON3` bits 13–15; the attached pair index is expanded to 5 bits in this mode, giving sprite colours 16–47.

Sprite rendering uses a fixed-point low-resolution coordinate system (`SPR_FP_UNIT = 1/8` lores pixel) so that the `SPRxPOS` horizontal value (in colour clocks) is converted correctly to lores pixels.  This gives exact superhires scaling (64 superhires pixels span 16 lores pixels) and sub-pixel horizontal positioning to half-lores-pixel precision.

Lower-numbered sprites have higher priority: they are rendered after higher-numbered sprites so that their pixels overwrite the overlapping region.  Sprites are clipped to the `DIWSTRT`/`DIWSTOP` display window, so border sprites that start partially outside the window are drawn only where they are visible.  Sprite colours are always read directly from the AGA palette entries and never reinterpreted through HAM/EHB decode, even when the playfield is in HAM or EHB mode.

Sprite collision detection is now implemented: `CLXDAT` (`0x00E`) is read-and-clear, and `CLXCON` (`0x016`) accepts the control mask.  A frame-level bounding-box test detects overlapping sprite pairs and sets the corresponding `CLXDAT` bits.  Bitplane-sprite collisions are detected during scanline rendering, and bitplane-bitplane collisions are detected in the bitplane render path.

### Beam Timing and Display Window

`VPOSR` and `VHPOSR` are now derived from the M68k cycle counter.  `chip_emu_beam_tick()` is still called from the 100 Hz PIT path, but it only runs the scheduler to the current M68k cycle count and updates the beam registers from that result; the PIT no longer drives chipset advancement directly.  PAL mode uses 312 lines per frame and NTSC mode uses 262 lines, with the beam position computed from the color-clock cycle count.

The horizontal blanking period is now modeled in the DMA slot table: slots 0 through 18 are outside the active display, and bitplane DMA is never allocated there.  The fixed refresh, disk, audio and sprite slots continue to occupy the start of the line, so CPU access during HBLANK sees less contention from bitplane DMA.

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

**Serial port** — `SERDAT` (`0xDFF030`) writes queue a byte in the Paula serial state and immediately transmit it to the host COM1 UART (`0x3F8`).  `SERPER` (`0xDFF032`) is converted to a 16550 divisor from the PAL master clock (baud = 3,546,895 / period; divisor = 1,843,200 / baud) and the COM1 line control register is reprogrammed for 8N1.  `chip_emu_serial_poll()` is called every PIT tick; it reads the COM1 line status register and, if data is ready, stores the received byte in `SERDATR` (`0xDFF018`), raises `INTREQ` bit 11, and sets the RBF (receive-buffer-full) status bit.  `SERDATR` also reports break detection (LSR bit 4), overrun/framing errors (LSR bits 2/3), and transmit-buffer/shift-register empty flags.  If COM1 is absent (LSR returns `0xFF`), the byte is passed through to the kernel console as `[SER] 0xXX`.

**Keyboard routing** — The PS/2 keyboard interrupt handler stores translated ASCII characters in the host PS/2 ring buffer (`kernel/irq/ps2kbd.c`).  The 100 Hz PIT tick path drains that buffer into the CIA-A SDR queue only when the M68k emulation bridge is active (`chip_emu_set_keyboard_route(1)`).  When the bridge is unavailable, the routing flag is left at 0 so the native shell/idle task keeps keyboard input instead of losing it to the unused CIA-A SDR queue.

**Disk DMA** — A real MFM floppy controller is implemented in `kernel/chipset/floppy.c`:
- ADF images are loaded into an 880 KiB buffer (`80 cylinders × 2 heads × 11 sectors × 512 bytes`).
- Tracks are encoded on demand to raw MFM bitstreams using the Amiga sector format: header sync (`0x4489`), 4 bytes of info + 16 bytes of label + 4 bytes of header CRC, data sync (`0x4489`), 512 bytes of data + 4 bytes of data CRC.  All bytes are MFM-encoded; sync words are inserted raw.
- `DSKSYNC` sets the sync pattern.  When `DSKLEN` is written with `DMAEN`, the controller waits for the next sync mark, then transfers `DSKLEN` words into chip RAM at `DSKPT` as the virtual disk rotates.
- Disk rotation is modelled at one track per ~1/11 second.  `floppy_tick()` is called from the 100 Hz PIT path to advance the bit position and continue any active DMA transfer.
- When the transfer completes, `INTREQ` bit 1 (`DSKBLK`) is raised.
- `DSKDAT` single-word reads also stream from the current MFM track position.
- A DOS handler integration wraps the floppy as a block device (`floppy0` / `DF0:`) in `kernel/drivers/floppy_blk.c`, so higher-level filesystem handlers can use it.  `floppy0` now supports sector writes via `floppy_write_sector()`.
- The DMA write path streams raw MFM words from chip RAM back onto the current track.  After a write completes the modified track is re-decoded and the ADF buffer is updated.
- CRCs are recalculated automatically when the track is regenerated on the next read or write to the same cylinder/head.
- Write-protected disks reject DMA writes, `DSKDAT` writes, and direct sector writes; `floppy_set_write_protect()` toggles the virtual write-protect tab.
- Boot-time tests `chip_emu_disk_dma_test()` and `floppy_block_device_test()` verify both the Paula DMA path and the DOS block-device path.  Additional tests `floppy_block_device_write_test()`, `floppy_write_protect_test()`, and `floppy_dma_write_test()` cover the new write paths.

**Parallel port** — CIA-B `PRB` (`0xBFD001`) is wired to the host LPT1 data port (`0x378`).  Writes to `PRB` only drive the bits that are marked as outputs in `DDRB` (`0xBFD003`); input bits are ignored.  Reads of `PRB` combine the last value written for output bits with the value sampled from the host data port for input bits.  `DDRB` writes that change bits from output to input trigger a fresh host port read.  At reset `lpt1_probe()` checks the LPT1 status port (`0x379`); if it reads `0xFF` the port is treated as absent and a loopback value is used so software tests still pass.  Boot-time tests `chip_emu_parallel_test()` and `chip_emu_serial_test()` verify both I/O paths.

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

Sprite rendering now uses a fixed-point low-resolution coordinate system for exact AGA super-hires scaling (64 superhires pixels = 16 lores pixels) and sub-pixel horizontal positioning via `SPRxPOS` colour-clock precision.  The 32-colour bank selection and 64-pixel wide modes remain in place.  Sprite-vs-playfield priority is implemented per pair through `BPLCON2` SP bits (bits 8–12): when a sprite pair's SP bit is set, non-transparent pixels are drawn only where the bitplanes are transparent.  Border sprites are clipped to the `DIWSTRT`/`DIWSTOP` display window, and sprite-to-sprite priority is enforced so lower-numbered sprites overwrite higher-numbered ones.  Sprites always bypass HAM/EHB decode and read their colours directly from the AGA palette bank selected by `BPLCON3`.

### CPU Cache Synchronization and Cycle Timing

The guest-write barrier in `emulation/uaos_m68k_glue.c` has been upgraded from a plain compiler barrier to a full x86 `mfence` memory fence.  This flushes the store buffer and makes M68k writes visible to the chipset emulator on multi-core hosts.  A non-x86 build would need to replace the `mfence` with the appropriate `dmb`/`sync` primitive for that host CPU.

A global M68k cycle counter (`g_m68k_cycles`) is accumulated after each `m68k_execute()` slice.  `chip_emu_m68k_cycles()` exposes the running total.  The chipset scheduler is now driven exclusively from this cycle counter via `chip_emu_run_to_cycle()`: every time the M68k advances, the scheduler processes any scanlines that have elapsed, allocates DMA slots, advances the Copper and Blitter, and schedules audio DMA.  The legacy 100 Hz PIT path only keeps host audio/video timing and calls `chip_emu_beam_tick()`, which in turn runs the scheduler to the current M68k cycle count, but it does not advance the chipset on its own.

CPU wait states for chip RAM accesses are now computed exactly from the stored DMA slot table.  `chip_emu_cpu_chipram_access()` maps the current M68k cycle to a slot within the current line, and if that slot is not free for the CPU it waits until the next CPU slot, adding roughly four cycles per slot.  The finalized slot table is copied into `g_dma_slots_current` after each scheduler line so the CPU hook can inspect it without rebuilding the table.  Fast RAM accesses at `0x800000` and above bypass the Agnus bus entirely and therefore incur no wait cycles.  Boot-time tests `chip_emu_timing_lock_test()`, `chip_emu_timing_contention_test()` and `chip_emu_hblank_test()` verify the scheduler lock, chip-versus-Fast RAM contention, and reduced contention during horizontal blanking.

### Cycle-Accurate DMA Arbitration (Agnus Slot Table)

`chip_emu.c` now models the real Agnus DMA slot layout rather than an abstract priority table.  PAL and NTSC lines both use 227 DMA slots; one slot is four color clocks and corresponds to one chip-RAM word transfer.

Fixed per-line slots are position-specific:

- slots 0–3: memory refresh (4 slots)
- slots 4–6: disk DMA (3 slots)
- slots 7–10: audio DMA (one slot per channel)
- slots 11–26: sprite DMA (two slots per sprite for eight sprites)

Refresh cannot be disabled; the other fixed slots are freed when their DMACON enable bit is clear (`dma_slot_release_if_disabled()`).  Bitplane DMA starts at `max(DDFSTRT/4, HBLANK_END_SLOT, 24)` and runs until `DDFSTOP/4`, using the low-resolution 8-slot fetch pattern `[free], bp4, bp6, bp2, [free], bp3, bp5, bp1` or the high-resolution 4-slot pattern `bp4, bp2, bp3, bp1`.  The `HBLANK_END_SLOT` constant (18) prevents bitplane DMA from being allocated during the horizontal blanking period.  Copper DMA uses only odd-numbered free slots.  The Blitter uses remaining free slots and yields every fourth free slot to the CPU when DMACON BLTPRI is clear; the CPU takes whatever is left.  The cycle-driven `chip_emu_run_to_cycle()` rebuilds the table every scanline; the finalized table is copied into `g_dma_slots_current` so that the CPU chip-RAM access hook can inspect it without rebuilding it.  `chip_emu_render_frame()` no longer builds its own slot table; it relies on the scheduler having already advanced the chipset state.

A helper API is provided:

- `dma_slot_reset()` — clears the line table and allocates the fixed refresh/disk/audio/sprite slots.
- `dma_slot_alloc_bitplanes()` — reserves bitplane slots from DDFSTRT/DDFSTOP.
- `dma_slot_alloc_copper(max_slots)` — reserves odd-numbered free slots for the Copper.
- `dma_slot_alloc_blitter()` — reserves free slots for the Blitter, yielding to the CPU when BLTPRI is clear.
- `dma_slot_release_if_disabled(channel, dmacon_bit)` — frees a channel's fixed slots if its DMACON enable bit is clear.
- `dma_slot_alloc(channel, start, count)` / `dma_slot_release(channel)` / `dma_slot_count(channel)` — retained as general-purpose helpers.

The copper only advances when it has been granted slots: each instruction consumes 2 slots, and `copper_run_to_beam()` stalls when the budget is exhausted.  The blitter tracks `g_blitter_words_remaining` and decrements it per allocated slot; the busy flag is cleared when the remaining word count reaches zero.  Audio DMA is advanced by the scheduler through `chip_emu_audio_advance()` once per scanline when master audio DMA is enabled.  Remaining slots are marked as CPU slots, and the number of non-CPU slots is accumulated into `g_cpu_stolen_cycles` as a proxy for CPU wait states.  `chip_emu_stolen_cycles()` exposes this counter.

Two boot-time tests are invoked from `uaos_kernel_main()`:

- `chip_emu_dma_test()` builds a 120-instruction copper list at guest RAM offset `0x10000`, runs one frame, and verifies that the copper advanced but stalled before the end-of-list `WAIT`.
- `chip_emu_agnus_slot_test()` verifies the position-specific layout: refresh/disk/audio/sprite fixed positions, bitplane fetch starting no earlier than slot 24, and DMA-off cycles freeing the bitplane region.

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

- **Fine-grained DMA slot timing** such as exact bitplane/Copper/sprite/audio/disk slot timing relative to color-clock positions, DMA-off cycles for all channels, and per-revision Agnus/Alice differences.
- **Blitter line/area-fill remaining edge cases** such as real line-mode texture/B channel usage, exact `BLTAPTL` accumulator loading, and per-pixel DMA slot timing for the actual data transfer.
- ~~**AGA sprite fine details** such as exact superhires pixel scaling, border sprites, sprite-to-sprite priority ordering, sub-pixel positioning, and HAM/EHB bypass.~~ Implemented.
- ~~**CPU/chipset timing lock** that advances the beam and subsystems from the M68k cycle counter rather than from PIT ticks, with cycle-accurate memory contention and horizontal blanking modelling.~~ Implemented.
- **Undocumented register behaviors, hardware quirks, and chipset revisions** (OCS/ECS/AGA differences, Alice/Lisa variants, A1200 vs A4000).
- **Zorro III / AutoConfig**, A4000 Gayle IDE, RTC (MSM6242/RP5C01), PCMCIA, and full genlock.
