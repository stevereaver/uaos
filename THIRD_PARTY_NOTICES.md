# Third-Party Notices

This file lists third-party code bundled with UAOS, along with the
licenses and upstream sources for each component.  The in-file license
notices are preserved verbatim in each source file; this document serves
as a top-level summary.

UAOS-original code is licensed under the MIT License (see `LICENSE`).
The third-party components below are licensed under their own terms,
which are compatible with the MIT License.

---

## 1. Musashi M68k CPU Emulator

- **Upstream:** <https://github.com/kstenerud/Musashi>
- **Author:** Karl Stenerud
- **License:** MIT
- **Location:** `emulation/src/musashi/`

A portable Motorola M680x0 processor emulation engine.  Used as the
M68k CPU core for UAOS's M68k emulation subsystem.  UAOS does not
modify the Musashi source; it supplies a project-specific configuration
via `emulation/uaos_m68kconf.h` using Musashi's `-DMUSASHI_CNF` mechanism.

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
Compiled into the kernel but not actively exercised in the current
configuration (FPU emulation is disabled in `uaos_m68kconf.h`).

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
Musashi distribution from the MAME fork lineage.  Compiled into the
kernel but not actively used (030/040 emulation and PMMU are disabled
in `uaos_m68kconf.h`).

### License

```
Copyright Nicola Salmoria and the MAME Team.
Visit http://mamedev.org for licensing and usage restrictions.
```

The MAME license is a BSD-like license.  The full text is available at
<http://mamedev.org/LICENSE.md>.
