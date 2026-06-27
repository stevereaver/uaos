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

## Future Work (Tier 4+)

- Real sprite DMA rendering using `SPR0PT`–`SPR7PT`.
- Per-scanline copper execution for mid-frame effects (current emulator runs the whole list once per frame).
- Accurate `VPOSR`/`VHPOSR` beam counters and vertical-blank interrupt timing.
- Interleaved bitplane layout and precise modulo handling.
