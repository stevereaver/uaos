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
Musashi's `-DMUSASHI_CNF` mechanism.  Two minor `#if` guards have been
added to the upstream source to exclude FPU and PMMU code from the
build (see sections 2 and 3 below); all other Musashi files are
unmodified.

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

## 2. SoftFloat 2b

- **Upstream:** <http://www.cs.berkeley.edu/~jhauser/arithmetic/SoftFloat.html>
- **Author:** John R. Hauser (International Computer Science Institute)
- **License:** Hauser/ICSI non-warranty license (see below)
- **Location:** `emulation/src/musashi/softfloat/`

An IEC/IEEE-conformant software floating-point arithmetic package.
Bundled as part of the Musashi distribution (MAME-repackaged version).
**Not compiled into the kernel.**  FPU emulation is disabled in
`uaos_m68kconf.h` (`M68K_EMULATE_FPOINT = OFF`), and the SoftFloat
sources are excluded from the build via `#if M68K_EMULATE_FPOINT`
guards in `m68kcpu.h` and `m68kcpu.c`.  The source files are retained
in the tree for reference only.

### License

```
Written by John R. Hauser.  This work was made possible in part by the
International Computer Science Institute, located at Suite 600, 1947 Center
Street, Berkeley, California 94704.  Funding was partially provided by the
National Science Foundation under grant MIP-9311980.  The original version
of this code was written as part of a project to build a fixed-point vector
processor in collaboration with the University of California at Berkeley,
overseen by Profs. Nelson Morgan and John Wawrzynek.

THIS SOFTWARE IS DISTRIBUTED AS IS, FOR FREE.  Although reasonable effort has
been made to avoid it, THIS SOFTWARE MAY CONTAIN FAULTS THAT WILL AT TIMES
RESULT IN INCORRECT BEHAVIOR.  USE OF THIS SOFTWARE IS RESTRICTED TO PERSONS
AND ORGANIZATIONS WHO CAN AND WILL TAKE FULL RESPONSIBILITY FOR ALL LOSSES,
COSTS, OR OTHER PROBLEMS THEY INCUR DUE TO THE SOFTWARE, AND WHO FURTHERMORE
EFFECTIVELY INDEMNIFY JOHN HAUSER AND THE INTERNATIONAL COMPUTER SCIENCE
INSTITUTE (possibly via similar legal warning) AGAINST ALL LOSSES, COSTS, OR
OTHER PROBLEMS INCURRED BY THEIR CUSTOMERS AND CLIENTS DUE TO THE SOFTWARE.

Derivative works are acceptable, even for commercial purposes, so long as
(1) the source code for the derivative work includes prominent notice that
the work is derivative, and (2) the source code includes prominent notice with
these four paragraphs for those parts of this code that are retained.
```

---

## 3. M68k PMMU

- **Upstream:** MAME (<http://mamedev.org>)
- **Author:** R. Belmont
- **License:** MAME license (BSD-like)
- **Location:** `emulation/src/musashi/m68kmmu.h`

PMMU implementation for 68851/68030/68040.  Ships as part of the
Musashi distribution from the MAME fork lineage.  **Not compiled into
the kernel.**  The `#include "m68kmmu.h"` in `m68kcpu.c` is guarded by
`#if M68K_EMULATE_FPOINT` and excluded from the build.  030/040
emulation and PMMU are disabled in `uaos_m68kconf.h`.

### License

```
Copyright Nicola Salmoria and the MAME Team.
Visit http://mamedev.org for licensing and usage restrictions.
```

The file header references "usage restrictions," which is the phrasing
of the pre-2016 MAME license (a BSD-like license with a non-commercial
use restriction).  MAME was re-licensed to BSD-3-Clause / GPL-2.0+ in
March 2016, but this file's header was never updated in the upstream
Musashi repository.  Because the file is **not compiled into the UAOS
kernel**, its license terms do not apply to the distributed binary.
The source is retained in the tree for reference only.

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
