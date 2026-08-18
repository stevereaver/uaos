# Third-Party Notices

This file lists third-party code bundled with UAOS, along with the
licenses and upstream sources for each component.  The in-file license
notices are preserved verbatim in each source file; this document serves
as a top-level summary.

UAOS-original code is licensed under the MIT License (see `LICENSE`).
The third-party components below are licensed under their own terms.
Only components actually compiled into the UAOS kernel are listed as
"compiled in" below; others are retained in the source tree for
reference but excluded from the build.

---

## 1. Musashi M68k CPU Emulator

- **Upstream:** <https://github.com/kstenerud/Musashi>
- **Author:** Karl Stenerud
- **License:** MIT
- **Location:** `emulation/src/musashi/`

A portable Motorola M680x0 processor emulation engine.  Used as the
M68k CPU core for UAOS's M68k emulation subsystem.  UAOS supplies a
project-specific configuration via `emulation/uaos_m68kconf.h` using
Musashi's `-DMUSASHI_CNF` mechanism.  The following changes have been
made to the upstream Musashi source:

- `#if M68K_EMULATE_FPOINT` guards added to `m68kcpu.c` and `m68kcpu.h`
  to exclude FPU and PMMU code from the build
- SoftFloat 2b source files (`softfloat/` directory) **deleted** — see §2
- MAME `m68kmmu.h` **deleted** — see §3
- `int8`/`int16`/`int32` typedefs (previously provided by SoftFloat's
  `milieu.h`) defined locally in `m68kcpu.h`

All other Musashi files are unmodified.

### License

```
Copyright © 1998-2001 Karl Stenerud

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

---

## 2. SoftFloat 2b (removed)

- **Upstream:** <http://www.cs.berkeley.edu/~jhauser/arithmetic/SoftFloat.html>
- **Author:** John R. Hauser (International Computer Science Institute)
- **License:** Hauser/ICSI non-warranty license (non-OSI, GPL-incompatible)
- **Status:** **Removed from the repository.**

SoftFloat 2b was previously bundled as part of the Musashi distribution.
Its license includes a use restriction and an indemnification clause that
makes it GPL-incompatible (per the FSF) and not OSI-approved.  The
SoftFloat source files have been **deleted** from the repository.  FPU
emulation is disabled in `uaos_m68kconf.h` (`M68K_EMULATE_FPOINT = OFF`),
and the `int8`/`int16`/`int32` typedefs that SoftFloat provided are now
defined locally in `m68kcpu.h`.

---

## 3. M68k PMMU (removed)

- **Upstream:** MAME (<http://mamedev.org>)
- **Author:** R. Belmont
- **License:** Pre-2016 MAME license (non-commercial restriction)
- **Status:** **Removed from the repository.**

The `m68kmmu.h` file was previously part of the Musashi distribution.
Its header referenced "usage restrictions," the phrasing of the
pre-2016 MAME license which included a non-commercial use clause
("Redistributions may not be sold, nor may they be used in a
commercial product or activity").  The FSF and Debian classified this
as non-free.  The file has been **deleted** from the repository.  The
`#include "m68kmmu.h"` in `m68kcpu.c` is guarded by
`#if M68K_EMULATE_FPOINT` (disabled in `uaos_m68kconf.h`).

---

## 4. GNU GRUB (bootloader)

- **Upstream:** <https://www.gnu.org/software/grub/>
- **Author:** Free Software Foundation, Inc.
- **License:** GPL-3.0-or-later
- **Location:** Distributed in the bootable ISO image (`/boot/grub/`,
  `/EFI/BOOT/BOOTX64.EFI`, `/System/Library/CoreServices/boot.efi`)

GRUB 2.12 is used as the bootloader for the UAOS hybrid BIOS+EFI
bootable ISO.  `grub-mkrescue` generates the boot image, GRUB modules,
and EFI bootloader at ISO build time.  These GRUB binaries are
distributed inside the ISO image.

### License

GRUB is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation, either version 3 of the License, or (at your
option) any later version.

The complete text of the GPL-3.0 is available at
<https://www.gnu.org/licenses/gpl-3.0.txt>.

**Note on GPL and the UAOS kernel:** The UAOS kernel itself is MIT-
licensed and is *not* a derivative work of GRUB.  GRUB loads the UAOS
kernel as a multiboot2 payload via the standard multiboot protocol.
The two are separate works combined on the ISO for booting
convenience.  The GPL-3.0 applies to the GRUB bootloader components
only; the MIT license applies to the UAOS kernel and system files.

---

## 5. Build Tools (not distributed in the ISO)

The following tools are used to build the UAOS ISO but are **not
included in the distributed image**.  They are listed here for
completeness.

| Tool | Version | License | Role |
|---|---|---|---|
| GCC | 13.x | GPL-3.0-with-GCC-exception | C compiler (kernel + userspace) |
| NASM | 2.16.01 | BSD-2-Clause | x86-64 assembler (kernel entry, IDT, task switch) |
| GNU ld (binutils) | 2.42 | GPL-3.0 | Linker |
| xorriso | 1.5.6 | GPL-3.0 | ISO image creation |
| vasm | 2.0f | vasm non-commercial license (M68k/AmigaOS commercial exception) | M68k assembler (Amiga demos) |
| vlink | 1.9.x | vasm non-commercial license (M68k/AmigaOS commercial exception) | M68k linker (Amiga demos) |
| objcopy (binutils) | 2.42 | GPL-3.0 | Binary embedding |
| xxd | (vim) | GPL-2.0 | Binary-to-C header conversion |

**vasm/vlink license note:** vasm and vlink are non-commercial software
with an exception: "An exception for commercial usage is granted,
provided that the target CPU is M68k and the target OS is AmigaOS."
UAOS uses vasm/vlink exclusively to assemble M68k AmigaOS demo
binaries, so the commercial exception applies.  These tools are build-
time only and are not distributed in the ISO.
