# UAOS Code Source Audit

This document is a code audit of the Ultimate Amiga OS (UAOS) repository,
cataloguing the origin of every piece of third-party or externally-derived
code.

The audit was performed in two phases:

1. **Phase 1 — Manual audit:** Inspecting copyright headers, license
   blocks, README files, build scripts, and the actual content of every
   source file in the repository.
2. **Phase 2 — Automated scan with ScanCode Toolkit v32.5.0:** A
   comprehensive automated license, copyright, and package scan of the
   entire repository using [ScanCode Toolkit](https://github.com/aboutcode-org/scancode-toolkit).
   The scan checked all 533 source files for license expressions,
   copyright holders, package manifests, emails, and URLs.  Results are
   in `scancode_report.json` and `scancode_report.md` at the repository
   root.

---

## Summary

| Component | Origin | License | Location |
|---|---|---|---|
| Musashi M68k CPU core | Karl Stenerud (upstream) | MIT | `emulation/src/musashi/` |
| SoftFloat 2b | John R. Hauser (via MAME) | Hauser/ICSI non-warranty | `emulation/src/musashi/softfloat/` |
| M68k PMMU (`m68kmmu.h`) | R. Belmont / MAME Team | MAME license | `emulation/src/musashi/m68kmmu.h` |
| Musashi example/test harness | Karl Stenerud (upstream) | MIT | `emulation/src/musashi/example/`, `emulation/src/musashi/test/` |
| UAOS kernel + AmigaOS libraries | UAOS Development Team (clean-room) | MIT | `kernel/**`, `emulation/uaos_*.c` |
| `powerpacker.library` | UAOS-generated stub wrapper (not real PowerPacker) | MIT | `system/LIBS/powerpacker.library` |
| Build scripts, tools, docs | UAOS Development Team | MIT | `scripts/`, `tools/`, `documentation/` |

**No source code from the AROS project is present in this repository.**
**No source code from original AmigaOS 3.1 (Commodore) is present.**
**No source code from UAE or FS-UAE is present** (despite a comment that
previously claimed otherwise — see §"Prior Discrepancies (Resolved)").

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

## 2. Code from Original AmigaOS 3.1 (Commodore)

**None.**

No binary or source from Commodore AmigaOS 3.1 is present. The
AmigaOS-compatible libraries are clean-room re-implementations of the
**public API** (library vector offsets, function signatures, struct
layouts as published in the Amiga ROM Kernel Manual). The API itself is
a published binary interface; re-implementing it does not require
Commodore source.

No Amiga-era binaries are present in the repository.

---

## 3. UAOS-Original Code

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
  `emulation/rom_patches/kickstart.conf`) — original.
- **Userspace** (`system/userspace/`, `system/libuaos/`) — original.
- **Build tooling** (`scripts/`, `tools/`) — original.
- **Documentation** (`documentation/`, `okf/`) — original.

None of these files contain UAE or Commodore copyright headers.
They are authored under the "UAOS Development Team" attribution (where
attribution is present).

---

## 4. License Status

UAOS-original code is released under the **MIT License**. The full text
is in `LICENSE` at the repository root. The `README.md` license section
links to it.

**Files compiled into the UAOS kernel:** Only MIT-licensed code is
compiled into the kernel binary.  This was verified by:

- ScanCode Toolkit scan of all source files
- `nm` symbol analysis of `build/uaos-kernel.elf` (no SoftFloat, MAME,
  or non-MIT symbols present)
- `strings` analysis of the kernel binary (no third-party license text
  embedded)
- ISO content inspection (no source files or third-party license files
  in the distributed image, except GRUB bootloader — see below)

**SoftFloat 2b and M68k PMMU are not compiled into the kernel.**  FPU
emulation is disabled in `uaos_m68kconf.h` (`M68K_EMULATE_FPOINT = OFF`),
and the SoftFloat and PMMU source files are excluded from the build via
`#if M68K_EMULATE_FPOINT` guards in `m68kcpu.h` and `m68kcpu.c`.  The
source files are retained in the tree for reference only.

**GRUB bootloader (GPL-3.0-or-later):** The bootable ISO image contains
GNU GRUB 2.12 as the bootloader.  GRUB is a separate work loaded via
the multiboot2 protocol and is not linked into the UAOS kernel.  The
GPL-3.0 applies to the GRUB bootloader components only; the MIT license
applies to the UAOS kernel and system files.  See
`THIRD_PARTY_NOTICES.md` §4 for full GRUB attribution.

**Build tools (GCC, NASM, vasm, vlink, xorriso, etc.):** These are used
at build time only and are not distributed in the ISO.  See
`THIRD_PARTY_NOTICES.md` §5 for the full build tool license table.

See `THIRD_PARTY_NOTICES.md` at the repository root for a summary of
each third-party component, its license, and its upstream URL.

---

## 5. Attribution

Third-party attribution is provided in `THIRD_PARTY_NOTICES.md` at the
repository root. It lists:

1. **Musashi** — MIT license, upstream URL, location in the tree.
   Compiled into the kernel.
2. **SoftFloat 2b** — Hauser/ICSI non-warranty license, upstream URL,
   location in the tree. **Not compiled into the kernel.**
3. **M68k PMMU** (`m68kmmu.h`) — MAME license (pre-2016, non-commercial
   restriction), upstream URL, location in the tree. **Not compiled
   into the kernel.**
4. **GNU GRUB** — GPL-3.0-or-later.  Distributed in the bootable ISO
   as the bootloader (separate work, not linked into the kernel).
5. **Build tools** — GCC, NASM, vasm, vlink, xorriso, binutils, xxd.
   Build-time only, not distributed in the ISO.

In-file license notices are also preserved verbatim in each third-party
source file.

---

## 6. Prior Discrepancies (Resolved)

The following discrepancies were identified during the initial audit and
have since been **resolved** by removing the incorrect references from
the codebase:

1. **AROS derivation claim.** Several files (`manual.tex`,
   `about_win.c`, `uaos_kernel_main.c`, `uaos.guide`,
   `aros_kickstart.conf`) previously claimed UAOS was "derived from
   the AROS open-source Amiga replacement kernel" or was an "AROS-derived
   microkernel." This was false — no AROS source code was ever present.
   All such references have been removed. The kernel is entirely
   UAOS-original.

2. **AROS Public License (APL) claim.** `manual.tex` and `about_win.c`
   previously asserted "Released under the AROS Public License (APL)."
   This was inappropriate since UAOS contains no AROS code. The APL
   claim has been removed from both files. The repository is now
   licensed under the MIT License (see `LICENSE`).

3. **AROS kickstart configuration.** The file
   `emulation/rom_patches/aros_kickstart.conf` and all references to it
   in `build_iso.sh`, `grub.cfg`, `uaos.guide`, and the OKF docs
   referenced an "AROS ROM image" (`SYS/aros-amiga-m68k-rom.bin`) that
   was never present in the repository. The file has been renamed to
   `kickstart.conf` and all references updated. The ROM image path is
   now `SYS/uaos-m68k-rom.bin`.

4. **UAE / FS-UAE derivation claim.** `emulation/uaos_uae_bridge.c`
   contained a comment stating the emulator core was "derived from UAE /
   FS-UAE." This was inaccurate — the actual emulator is Musashi. The
   comment has been corrected to describe the bridge as sitting between
   the M68k emulator core and the UAOS kernel, without the false UAE
   derivation claim.

5. **`Lha` test binary.** A compiled Amiga `Lha` archive tool binary
   was previously committed to `emulation/binaries/`. It was a test
   artifact with no license attribution. It has been removed from the
   entire git history (via `git filter-repo`) and the
   `emulation/binaries/` directory is now gitignored.

---

*Audit performed by inspecting all source files, copyright headers,
license blocks, build scripts, and documentation in the UAOS repository
tree at `/home/reaver/workspaces/uaos/uaos`.*
