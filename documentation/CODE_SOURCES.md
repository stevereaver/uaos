# UAOS Code Source Audit

This document is a code audit of the Ultimate Amiga OS (UAOS) repository,
cataloguing the origin of every piece of third-party or externally-derived
code, and flagging discrepancies between the documentation claims and what
the source tree actually contains.

The audit was performed by inspecting copyright headers, license blocks,
README files, build scripts, and the actual content of every source file
in the repository.

---

## Summary

| Component | Origin | License | Location |
|---|---|---|---|
| Musashi M68k CPU core | Karl Stenerud (upstream) | MIT | `emulation/src/musashi/` |
| SoftFloat 2b | John R. Hauser (via MAME) | Hauser/ICSI non-warranty | `emulation/src/musashi/softfloat/` |
| M68k PMMU (`m68kmmu.h`) | R. Belmont / MAME Team | MAME license | `emulation/src/musashi/m68kmmu.h` |
| Musashi example/test harness | Karl Stenerud (upstream) | MIT | `emulation/src/musashi/example/`, `emulation/src/musashi/test/` |
| UAOS kernel + AmigaOS libraries | UAOS Development Team (clean-room) | None declared (see §"License Discrepancies") | `kernel/**`, `emulation/uaos_*.c` |
| `powerpacker.library` | UAOS-generated stub wrapper (not real PowerPacker) | UAOS | `system/LIBS/powerpacker.library` |
| Build scripts, tools, docs | UAOS Development Team | UAOS | `scripts/`, `tools/`, `documentation/` |

**No source code from the AROS project is present in this repository.**
**No source code from original AmigaOS 3.1 (Commodore) is present.**
**No source code from UAE or FS-UAE is present** (despite a comment that
claims otherwise — see §"Discrepancies").

---

## 1. Third-Party Code Present in the Repository

### 1.1 Musashi M68k CPU Emulator

The M68k CPU emulator is the **Musashi** engine by Karl Stenerud, taken
from the upstream project at <https://github.com/kstenerud/Musashi>.

**Files (all under `emulation/src/musashi/`):**

| File | Upstream version noted in header |
|---|---|
| `m68kcpu.c` | Version 4.60 |
| `m68kcpu.h` | Version 4.5 |
| `m68k.h` | Version 3.32 |
| `m68k_in.c` | Version 3.32 (opcode handler source, fed to `m68kmake`) |
| `m68kdasm.c` | Version 3.32 |
| `m68kmake.c` | Version 4.60 (opcode-table generator) |
| `m68kconf.h` | Upstream default config |
| `readme.txt` | Upstream readme (describes "Version 4.10") |
| `history.txt` | Upstream changelog |
| `Makefile` | Upstream Makefile |
| `.gitignore` | Upstream |
| `example/` | Upstream example simulator (`sim.c`, `osd_linux.c`, `osd_dos.c`, `program.bin`, `README.md`, `m68kconf.h`, `osd.h`, `Makefile`) |
| `test/` | Upstream test suite (`entry.s`, `mc68000/*.s` + `*.bin`, `mc68040/`, `Makefile`, `README.md`) |

**License:** MIT (changed to MIT on 15-Jul-2013 per `history.txt`).
Copyright © 1998-2001 Karl Stenerud. The full MIT notice is preserved at
the top of `m68kcpu.c`, `m68k.h`, `m68k_in.c`, `m68kdasm.c`, and
`m68kmake.c`.

**UAOS integration:** UAOS does **not** modify the Musashi source. It
supplies a project-specific configuration via
`emulation/uaos_m68kconf.h` (included through Musashi's
`-DMUSASHI_CNF` mechanism documented in the upstream readme). The UAOS
config targets plain M68000 only, disables FPU/PMMU/trace/breakpoint
support, and enables ILLEGAL/TRAP callbacks so the UAOS glue layer can
intercept library calls.

The generated opcode table (`m68kops.c` / `m68kops.h`) is produced at
build time by running `m68kmake` against `m68k_in.c`, exactly as the
upstream Makefile does. It is listed in `.gitignore` and is not checked
in.

> **Note:** The `example/` and `test/` subdirectories are the original
> upstream Musashi demo simulator and test harness. They are **not used**
> by the UAOS build (`scripts/build_iso.sh` only compiles `m68kcpu.c`,
> `m68kdasm.c`, `softfloat/softfloat.c`, and the generated `m68kops.c`).
> They are retained as upstream reference material.

### 1.2 SoftFloat 2b (FPU support library)

Bundled inside the Musashi directory at `emulation/src/musashi/softfloat/`.

**Files:**

| File | Origin |
|---|---|
| `softfloat.c` | SoftFloat Release 2b by John R. Hauser |
| `softfloat.h` | SoftFloat Release 2b |
| `softfloat-macros` | SoftFloat Release 2b |
| `softfloat-specialize` | SoftFloat Release 2b |
| `milieu.h` | SoftFloat Release 2b |
| `mamesf.h` | MAME-specific repackaging shim |
| `README.txt` | MAME note explaining the repackaging |

**License:** The Hauser/ICSI non-warranty license (not MIT). The
`README.txt` explicitly states: *"this package is derived from the
following original SoftFloat package and has been 're-packaged' to work
with MAME's conventions and build system. The source files come from
bits64/ and bits64/templates in the original distribution."*

The four-paragraph legal notice from John R. Hauser is preserved verbatim
at the top of `softfloat.c`, `softfloat.h`, `milieu.h`, and
`softfloat-specialize`. Derivative works are permitted provided the
source includes prominent notice that the work is derivative and retains
those four paragraphs.

**UAOS integration:** SoftFloat is compiled into the kernel as part of
the Musashi build, although the UAOS Musashi config
(`uaos_m68kconf.h`) sets `M68K_EMULATE_FPOINT` to `OFF`, so the FPU
paths are not actively exercised in the current configuration. The code
is still linked because `m68kcpu.c` includes `softfloat.h` and
references SoftFloat symbols conditionally.

### 1.3 M68k PMMU (`m68kmmu.h`)

`emulation/src/musashi/m68kmmu.h` implements the 68851/68030/68040
PMMU address translation.

**Origin:** By R. Belmont. Header reads:
> *"m68kmmu.h - PMMU implementation for 68851/68030/68040 — By R.
> Belmont — Copyright Nicola Salmoria and the MAME Team. Visit
> http://mamedev.org for licensing and usage restrictions."*

**License:** MAME license (BSD-like). This file ships as part of the
Musashi distribution from the MAME fork lineage.

**UAOS integration:** Like SoftFloat, this is compiled but not actively
used — `uaos_m68kconf.h` disables 030/040 emulation and PMMU is not
enabled.

---

## 2. Code Claimed to be Derived from AROS — But Not Present

### 2.1 Documentation claims

Several places in the repository claim AROS derivation:

- `documentation/manual.tex` (lines 61, 66, 84, 100, 105, 115):
  > *"UAOS is a standalone, bare-metal 64-bit operating system… It is
  > derived from the AROS open-source Amiga replacement kernel and
  > features transparent M68k JIT emulation…"*
  >
  > *"Released under the AROS Public License (APL)."*
  >
  > *"An extended AROS microkernel providing Exec, DOS, and Intuition
  > semantics on 64-bit hardware."*
  >
  > *"No Linux, no POSIX kernel layer… UAOS boots directly into its own
  > x86_64 AROS-derived microkernel."*
  >
  > *"AROS Kickstart firmware. The open-source AROS replacement ROM is
  > used as the modular firmware base with vectors patched at build
  > time."*

- `kernel/display/about_win.c` (lines 149, 153):
  > *"Copyright 2026 UAOS Development Team"*
  > *"Released under the AROS Public License (APL)"*

- `emulation/rom_patches/aros_kickstart.conf`:
  > *"Controls which AROS ROM image is loaded into the M68k guest
  > address space"*
  >
  > `rom_image = SYS/aros-amiga-m68k-rom.bin`

- `scripts/grub.cfg` loads `aros_kickstart.conf` as a multiboot2 module.

- `README.md` (line 138): the build script *"Injects `grub.cfg` and the
  AROS kickstart configuration"*.

### 2.2 What the source actually shows

After inspecting every C file under `kernel/` and `emulation/`:

- **No AROS source files are present.** There is no `aros-amiga-m68k-rom.bin`
  in the repository (the `aros_kickstart.conf` points to
  `SYS/aros-amiga-m68k-rom.bin`, which is not checked in and not
  generated by the build).
- The AmigaOS-compatible libraries (`exec.library`, `dos.library`,
  `graphics.library`, `intuition.library`, `workbench.library`,
  `bsdsocket.library`, `utility.library`, `console.device`,
  `mathffp.library`, `locale.library`, `ixemul.library`,
  `timer.device`, `keyboard.device`, `gadtools.library`) are
  **clean-room C re-implementations** written by the UAOS Development
  Team. They implement the AmigaOS public API (LVO offsets, BPTR/BSTR
  conventions, `FileLock` / `FileInfoBlock` / `DosList` struct layouts)
  but the code is original UAOS code, not copied from AROS.
- The struct layouts in `kernel/dos/amiga_dos_types.h` match the
  published AmigaOS 3.x ABI (this is a public binary interface, not
  copyrighted source), but the definitions are UAOS-written.
- The `rom_traps.s` file is UAOS-written M68k assembly (Vasm/Devpac
  syntax) that emits 6-byte `ILLEGAL + 0x414D + index + RTS` breakout
  stubs. Its header comment says the stubs are *"inserted into the AROS
  replacement ROM jump table"*, but no such ROM exists in the tree —
  the stubs are actually written directly into guest RAM by
  `uaos_m68k_glue.c`'s `install_library_tables()`.

### 2.3 Conclusion on AROS

The AROS references in `manual.tex`, `about_win.c`, and
`aros_kickstart.conf` describe an **intended or aspirational
architecture** (loading a real AROS m68k replacement ROM and patching
its vectors) that is **not implemented** in the current codebase. The
actual implementation uses UAOS-native C library stubs registered
through `rom_modules.c` and dispatched via the `ILLEGAL`-opcode thunk
handler. **No AROS code is compiled into the kernel.**

The claim that UAOS is *"derived from the AROS open-source Amiga
replacement kernel"* is **not supported by the source tree**. The
kernel is an original UAOS implementation.

---

## 3. Code Claimed to be Derived from UAE — But Not Present

`emulation/uaos_uae_bridge.c` (line 4) contains the comment:

> *"This module sits between the headless M68k emulator core (derived
> from UAE / FS-UAE) and the UAOS native kernel subsystems."*

After inspection:

- The actual M68k emulator core used is **Musashi** (see §1.1), not
  UAE or FS-UAE.
- `uaos_uae_bridge.c` itself is UAOS-written glue code. It defines a
  `M68kCPUState` struct and forward-declares `UAOS_HandleThunk` — it
  does not include or link any UAE source.
- No UAE or FS-UAE source files are present anywhere in the repository.

The "derived from UAE / FS-UAE" comment is **inaccurate**. The bridge
module is UAOS-original and interfaces with Musashi, not UAE. The
"UAE-compatible" naming refers only to the conceptual interface
contract (an ILLEGAL-opcode callback), not to derived code.

---

## 4. Code from Original AmigaOS 3.1 (Commodore)

**None.**

No binary or source from Commodore AmigaOS 3.1 is present. The
AmigaOS-compatible libraries are clean-room re-implementations of the
**public API** (library vector offsets, function signatures, struct
layouts as published in the Amiga ROM Kernel Manual). The API itself is
a published binary interface; re-implementing it does not require
Commodore source.

No Amiga-era binaries are present in the repository.

---

## 5. UAOS-Original Code

The following is original UAOS Development Team code, written from
scratch for this project:

- **Kernel core** (`kernel/boot/`, `kernel/irq/`, `kernel/exec/`,
  `kernel/dos/`, `kernel/net/`, `kernel/drivers/`, `kernel/display/`,
  `kernel/shell/`, `kernel/chipset/`, `kernel/audio/`) — the entire
  x86_64 bare-metal kernel.
- **AmigaOS-compatible libraries** (`kernel/exec/exec_task.c`,
  `dos_lib.c`, `graphics_lib.c`, `intuition_lib.c`,
  `workbench_lib.c`, `bsdsocket_lib.c`, `utility_lib.c`,
  `console_device.c`, `mathffp_lib.c`, `locale_lib.c`,
  `ixemul_lib.c`, `timer_device.c`, `keyboard_device.c`,
  `gadtools_lib.c`, `boopsi_builtin.c`, `loadable_lib.c`) —
  clean-room re-implementations.
- **Chipset emulator** (`kernel/chipset/chip_emu.c`,
  `kernel/chipset/floppy.c`) — original AGA/ECS custom chip
  emulation.
- **M68k glue** (`emulation/uaos_m68k_glue.c`,
  `emulation/uaos_emu_registry.c`, `emulation/uaos_uae_bridge.c`,
  `emulation/uaos_m68kconf.h`) — original integration layer.
- **ROM trap stubs** (`emulation/rom_patches/rom_traps.s`,
  `emulation/rom_patches/aros_kickstart.conf`) — original.
- **Userspace** (`system/userspace/`, `system/libuaos/`) — original.
- **Build tooling** (`scripts/`, `tools/`) — original.
- **Documentation** (`documentation/`, `okf/`) — original.

None of these files contain AROS, UAE, or Commodore copyright headers.
They are authored under the "UAOS Development Team" attribution (where
attribution is present).

---

## 6. License Discrepancies

The repository has **inconsistent and conflicting license statements**:

| Location | Claim |
|---|---|
| `README.md` (line 402) | *"This project is a personal research and hobby project. No licence is currently applied."* |
| `documentation/manual.tex` (line 66) | *"Released under the AROS Public License (APL)."* |
| `kernel/display/about_win.c` (line 153) | *"Released under the AROS Public License (APL)"* (shown in the running OS's About window) |

There is **no `LICENSE` file** in the repository root.

The AROS Public License (APL) claim is problematic because:

1. APL is the license of the AROS project. UAOS contains no AROS code,
   so applying APL to UAOS-original code is a category error — APL is
   not a license the UAOS authors can retroactively impose on their own
   clean-room code unless they explicitly choose to (and even then, APL
   is an unusual choice for non-AROS-derived code).
2. The README explicitly contradicts the APL claim by saying *"No
   licence is currently applied."*
3. The About window shown to end users asserts APL, which is the most
   visible statement of all three.

**Recommendation:** Pick one license, apply it consistently, add a
`LICENSE` file to the repository root, and reconcile the README,
`manual.tex`, and `about_win.c` to agree. If UAOS is truly
independent of AROS, APL is the wrong license — MIT or BSD-2-Clause
would be more appropriate and would also be compatible with the Musashi
MIT license and the SoftFloat / MAME licenses of the bundled
third-party code.

---

## 7. Attribution Gaps

The following third-party material is present but lacks a top-level
attribution or license file:

1. **SoftFloat** — the in-file notices are preserved, but there is no
   top-level `THIRD_PARTY_NOTICES` or equivalent summarizing that
   SoftFloat 2b (Hauser/ICSI) is bundled.
2. **MAME PMMU** (`m68kmmu.h`) — same: in-file notice preserved, no
   top-level summary.
3. **Musashi** — in-file notices preserved, no top-level summary.

**Recommendation:** Add a `THIRD_PARTY_NOTICES.md` (or similar) to the
repository root listing Musashi, SoftFloat, and the MAME PMMU code with
their respective licenses and upstream URLs.

---

## 8. Discrepancy Summary

| Claim | Source | Reality |
|---|---|---|
| "derived from the AROS open-source Amiga replacement kernel" | `manual.tex` | False — no AROS code present; kernel is UAOS-original |
| "Released under the AROS Public License (APL)" | `manual.tex`, `about_win.c` | Conflicts with README ("No licence is currently applied"); APL is inappropriate for non-AROS-derived code |
| "AROS replacement ROM is used as the modular firmware base" | `manual.tex` | False — `aros-amiga-m68k-rom.bin` is not in the tree and not built; UAOS uses native C library stubs |
| "emulator core (derived from UAE / FS-UAE)" | `uaos_uae_bridge.c` comment | False — the emulator is Musashi, not UAE/FS-UAE |
| "built from scratch" | `README.md` | True for the kernel and libraries; the only non-scratch code is Musashi + SoftFloat |
| "No licence is currently applied" | `README.md` | Conflicts with the APL claims in `manual.tex` and `about_win.c` |

---

*Audit performed by inspecting all source files, copyright headers,
license blocks, build scripts, and documentation in the UAOS repository
tree at `/home/reaver/workspaces/uaos/uaos`.*
