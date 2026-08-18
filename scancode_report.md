# ScanCode Report — UAOS

_Generated 2026-08-18 22:55 from `scancode_report.json`_

## Scan Metadata

- **Tool:** ScanCode Toolkit v32.5.0
- **Output format version:** 4.1.0
- **SPDX license list version:** 3.27
- **Scan start:** 2026-08-18T124820.633209
- **Scan end:** 2026-08-18T124918.474984
- **Duration:** 57.841796875s
- **Platform:** Linux-6.18.33.2-microsoft-standard-WSL2-x86_64-with-glibc2.39 (linux)
- **Python:** 3.12.3 (main, Jun 19 2026, 12:46:00) [GCC 13.3.0]
- **Errors:** 0  |  **Warnings:** 0

### Scan options
- Scans: --license, --copyright, --package, --info, --email, --url
- Post-scan: --classify, --summary, --tallies
- Ignored paths: build, sys-root, emulation/binaries, .git
- Processes: default

## Executive Summary

- **Declared license:** `mit AND unknown-license-reference`
- **Declared copyright holder:** UAOS Development Team
- **Primary language:** C
- **License clarity score:** 80/100
  - (declared_license=True, identification_precision=True, has_license_text=True, declared_copyrights=True, conflicting_license_categories=True, ambiguous_compound_licensing=False)

- **Files scanned:** 533
- **Directories scanned:** 111
- **Total size:** 8.22 MB
- **Files with license detections:** 19 (3.6%)
- **Files with copyright detections:** 21 (3.9%)
- **Files with package data:** 1
- **Files with emails:** 3
- **Files with URLs:** 17
- **Files with scan errors:** 0

## Tallies

### Detected license expressions

| License expression | Count |
| --- | ---: |
| _(none)_ | 513 |
| mit | 20 |
| softfloat AND proprietary-license | 7 |
| bsd-new | 4 |
| unknown-license-reference | 1 |

### Copyrights

| Copyright | Count |
| --- | ---: |
| _(none)_ | 512 |
| Copyright (c) Karl Stenerud | 12 |
| Copyright (c) UAOS Development Team | 5 |
| Copyright Nicola Salmoria and the MAME Team | 3 |
| Copyright Karl Stenerud (kstenerud@gmail.com) | 2 |
| (c) Table C | 1 |
| (c) UAOS Project | 1 |
| Copyright extcopyright UAOS Development Team | 1 |

### Holders

| Holder | Count |
| --- | ---: |
| _(none)_ | 512 |
| Karl Stenerud | 14 |
| UAOS Development Team | 5 |
| Nicola Salmoria and the MAME Team | 3 |
| Table | 1 |
| UAOS Project | 1 |
| extcopyright UAOS Development Team | 1 |

### Authors

| Author | Count |
| --- | ---: |
| _(none)_ | 524 |
| John R. Hauser | 6 |
| GfxNew'. GfxAssociate Implemented Stores | 1 |
| Henk Kelder | 1 |
| John R. Hauser (International Computer Science Institute) | 1 |
| Karl Stenerud | 1 |
| R. Belmont | 1 |
| me, John R. Hauser | 1 |

### Programming languages

| Language | Count |
| --- | ---: |
| C | 285 |
| GAS | 83 |
| Bash | 6 |
| NASM | 3 |
| HTML | 2 |
| CSS | 1 |
| verilog | 1 |

## Files with License Detections

| File | Detected License | SPDX | Detections |
| --- | --- | --- | ---: |
| `uaos/LICENSE` | mit | MIT | 1 |
| `uaos/README.md` | mit AND unknown-license-reference | MIT AND LicenseRef-scancode-unknown-license-reference | 2 |
| `uaos/THIRD_PARTY_NOTICES.md` | mit AND (softfloat AND proprietary-license) AND bsd-new | MIT AND (LicenseRef-scancode-softfloat AND LicenseRef-scancode-proprietary-license) AND BSD-3-Clause | 7 |
| `uaos/documentation/CODE_SOURCES.md` | mit AND bsd-new | MIT AND BSD-3-Clause | 6 |
| `uaos/emulation/src/musashi/example/m68kconf.h` | mit | MIT | 1 |
| `uaos/emulation/src/musashi/m68k.h` | mit | MIT | 1 |
| `uaos/emulation/src/musashi/m68k_in.c` | mit | MIT | 1 |
| `uaos/emulation/src/musashi/m68kconf.h` | mit | MIT | 1 |
| `uaos/emulation/src/musashi/m68kcpu.c` | mit | MIT | 1 |
| `uaos/emulation/src/musashi/m68kcpu.h` | mit | MIT | 1 |
| `uaos/emulation/src/musashi/m68kdasm.c` | mit | MIT | 1 |
| `uaos/emulation/src/musashi/m68kmake.c` | mit | MIT | 1 |
| `uaos/emulation/src/musashi/readme.txt` | mit | MIT | 1 |
| `uaos/emulation/src/musashi/softfloat/README.txt` | softfloat AND proprietary-license | LicenseRef-scancode-softfloat AND LicenseRef-scancode-proprietary-license | 1 |
| `uaos/emulation/src/musashi/softfloat/milieu.h` | softfloat AND proprietary-license | LicenseRef-scancode-softfloat AND LicenseRef-scancode-proprietary-license | 1 |
| `uaos/emulation/src/musashi/softfloat/softfloat-macros` | softfloat AND proprietary-license | LicenseRef-scancode-softfloat AND LicenseRef-scancode-proprietary-license | 1 |
| `uaos/emulation/src/musashi/softfloat/softfloat-specialize` | softfloat AND proprietary-license | LicenseRef-scancode-softfloat AND LicenseRef-scancode-proprietary-license | 1 |
| `uaos/emulation/src/musashi/softfloat/softfloat.c` | softfloat AND proprietary-license | LicenseRef-scancode-softfloat AND LicenseRef-scancode-proprietary-license | 1 |
| `uaos/emulation/src/musashi/softfloat/softfloat.h` | softfloat AND proprietary-license | LicenseRef-scancode-softfloat AND LicenseRef-scancode-proprietary-license | 1 |

### License detection details

#### `uaos/LICENSE`

- **Expression:** `mit`  
  - SPDX: `MIT`  
  - Match: `mit` (lines 1-1, score 100.0, coverage 100.0%, rule `mit_14.RULE`, relevance 100)
  - Match: `mit` (lines 5-21, score 100.0, coverage 100.0%, rule `mit.LICENSE`, relevance 100)

#### `uaos/README.md`

- **Expression:** `mit`  
  - SPDX: `MIT`  
  - Match: `mit` (lines 402-402, score 100.0, coverage 100.0%, rule `mit_43.RULE`, relevance 100)
- **Expression:** `unknown-license-reference`  
  - SPDX: `LicenseRef-scancode-unknown-license-reference`  
  - Match: `unknown-license-reference` (lines 405-405, score 50.0, coverage 100.0%, rule `license-intro_2.RULE`, relevance 50)

#### `uaos/THIRD_PARTY_NOTICES.md`

- **Expression:** `mit`  
  - SPDX: `MIT`  
  - Match: `mit` (lines 8-8, score 56.25, coverage 56.25%, rule `mit_478.RULE`, relevance 100)
- **Expression:** `mit`  
  - SPDX: `MIT`  
  - Match: `unknown-license-reference` (lines 9-9, score 50.0, coverage 100.0%, rule `license-intro_2.RULE`, relevance 50)
  - Match: `mit` (lines 10-10, score 100.0, coverage 100.0%, rule `mit_27.RULE`, relevance 100)
- **Expression:** `mit`  
  - SPDX: `MIT`  
  - Match: `mit` (lines 18-18, score 100.0, coverage 100.0%, rule `mit_30.RULE`, relevance 100)
- **Expression:** `mit`  
  - SPDX: `MIT`  
  - Match: `mit` (lines 31-47, score 100.0, coverage 100.0%, rule `mit.LICENSE`, relevance 100)
- **Expression:** `softfloat AND proprietary-license`  
  - SPDX: `LicenseRef-scancode-softfloat AND LicenseRef-scancode-proprietary-license`  
  - Match: `softfloat AND proprietary-license` (lines 67-84, score 84.29, coverage 84.29%, rule `softfloat_and_proprietary-license_1.RULE`, relevance 100)
- **Expression:** `bsd-new`  
  - SPDX: `BSD-3-Clause`  
  - Match: `bsd-new` (lines 96-96, score 90.0, coverage 100.0%, rule `bsd-new_417.RULE`, relevance 90)
- **Expression:** `bsd-new`  
  - SPDX: `BSD-3-Clause`  
  - Match: `bsd-new` (lines 111-111, score 90.0, coverage 100.0%, rule `bsd-new_625.RULE`, relevance 90)
  - Match: `bsd-new` (lines 111-111, score 90.0, coverage 100.0%, rule `bsd-new_required_phrase_12.RULE`, relevance 90)

#### `uaos/documentation/CODE_SOURCES.md`

- **Expression:** `mit`  
  - SPDX: `MIT`  
  - Match: `mit` (lines 57-57, score 100.0, coverage 100.0%, rule `mit_30.RULE`, relevance 100)
- **Expression:** `bsd-new`  
  - SPDX: `BSD-3-Clause`  
  - Match: `bsd-new` (lines 126-126, score 90.0, coverage 100.0%, rule `bsd-new_417.RULE`, relevance 90)
- **Expression:** `mit`  
  - SPDX: `MIT`  
  - Match: `mit` (lines 186-186, score 100.0, coverage 100.0%, rule `mit_43.RULE`, relevance 100)
- **Expression:** `mit`  
  - SPDX: `MIT`  
  - Match: `unknown-license-reference` (lines 190-190, score 50.0, coverage 100.0%, rule `license-intro_2.RULE`, relevance 50)
  - Match: `mit` (lines 191-191, score 100.0, coverage 100.0%, rule `mit_27.RULE`, relevance 100)
- **Expression:** `mit`  
  - SPDX: `MIT`  
  - Match: `mit` (lines 202-202, score 100.0, coverage 100.0%, rule `mit_14.RULE`, relevance 100)
- **Expression:** `mit`  
  - SPDX: `MIT`  
  - Match: `mit` (lines 231-231, score 100.0, coverage 100.0%, rule `mit_1336.RULE`, relevance 100)
  - Match: `mit` (lines 1-1, score 100.0, coverage 100.0%, rule `mit_14.RULE`, relevance 100)
  - Match: `mit` (lines 5-21, score 100.0, coverage 100.0%, rule `mit.LICENSE`, relevance 100)

#### `uaos/emulation/src/musashi/example/m68kconf.h`

- **Expression:** `mit`  
  - SPDX: `MIT`  
  - Match: `mit` (lines 11-27, score 100.0, coverage 100.0%, rule `mit.LICENSE`, relevance 100)

#### `uaos/emulation/src/musashi/m68k.h`

- **Expression:** `mit`  
  - SPDX: `MIT`  
  - Match: `mit` (lines 11-27, score 100.0, coverage 100.0%, rule `mit.LICENSE`, relevance 100)

#### `uaos/emulation/src/musashi/m68k_in.c`

- **Expression:** `mit`  
  - SPDX: `MIT`  
  - Match: `mit` (lines 16-32, score 100.0, coverage 100.0%, rule `mit.LICENSE`, relevance 100)

#### `uaos/emulation/src/musashi/m68kconf.h`

- **Expression:** `mit`  
  - SPDX: `MIT`  
  - Match: `mit` (lines 11-27, score 100.0, coverage 100.0%, rule `mit.LICENSE`, relevance 100)

#### `uaos/emulation/src/musashi/m68kcpu.c`

- **Expression:** `mit`  
  - SPDX: `MIT`  
  - Match: `mit` (lines 11-27, score 100.0, coverage 100.0%, rule `mit.LICENSE`, relevance 100)

#### `uaos/emulation/src/musashi/m68kcpu.h`

- **Expression:** `mit`  
  - SPDX: `MIT`  
  - Match: `mit` (lines 11-27, score 100.0, coverage 100.0%, rule `mit.LICENSE`, relevance 100)

#### `uaos/emulation/src/musashi/m68kdasm.c`

- **Expression:** `mit`  
  - SPDX: `MIT`  
  - Match: `mit` (lines 11-27, score 100.0, coverage 100.0%, rule `mit.LICENSE`, relevance 100)

#### `uaos/emulation/src/musashi/m68kmake.c`

- **Expression:** `mit`  
  - SPDX: `MIT`  
  - Match: `mit` (lines 12-28, score 100.0, coverage 100.0%, rule `mit.LICENSE`, relevance 100)

#### `uaos/emulation/src/musashi/readme.txt`

- **Expression:** `mit`  
  - SPDX: `MIT`  
  - Match: `mit` (lines 31-47, score 100.0, coverage 100.0%, rule `mit.LICENSE`, relevance 100)

#### `uaos/emulation/src/musashi/softfloat/README.txt`

- **Expression:** `softfloat AND proprietary-license`  
  - SPDX: `LicenseRef-scancode-softfloat AND LicenseRef-scancode-proprietary-license`  
  - Match: `softfloat AND proprietary-license` (lines 48-68, score 92.86, coverage 92.86%, rule `softfloat_and_proprietary-license_1.RULE`, relevance 100)

#### `uaos/emulation/src/musashi/softfloat/milieu.h`

- **Expression:** `softfloat AND proprietary-license`  
  - SPDX: `LicenseRef-scancode-softfloat AND LicenseRef-scancode-proprietary-license`  
  - Match: `softfloat AND proprietary-license` (lines 7-26, score 84.29, coverage 84.29%, rule `softfloat_and_proprietary-license_1.RULE`, relevance 100)

#### `uaos/emulation/src/musashi/softfloat/softfloat-macros`

- **Expression:** `softfloat AND proprietary-license`  
  - SPDX: `LicenseRef-scancode-softfloat AND LicenseRef-scancode-proprietary-license`  
  - Match: `softfloat AND proprietary-license` (lines 7-26, score 83.81, coverage 83.81%, rule `softfloat_and_proprietary-license_1.RULE`, relevance 100)

#### `uaos/emulation/src/musashi/softfloat/softfloat-specialize`

- **Expression:** `softfloat AND proprietary-license`  
  - SPDX: `LicenseRef-scancode-softfloat AND LicenseRef-scancode-proprietary-license`  
  - Match: `softfloat AND proprietary-license` (lines 7-26, score 84.29, coverage 84.29%, rule `softfloat_and_proprietary-license_1.RULE`, relevance 100)

#### `uaos/emulation/src/musashi/softfloat/softfloat.c`

- **Expression:** `softfloat AND proprietary-license`  
  - SPDX: `LicenseRef-scancode-softfloat AND LicenseRef-scancode-proprietary-license`  
  - Match: `softfloat AND proprietary-license` (lines 7-26, score 84.29, coverage 84.29%, rule `softfloat_and_proprietary-license_1.RULE`, relevance 100)

#### `uaos/emulation/src/musashi/softfloat/softfloat.h`

- **Expression:** `softfloat AND proprietary-license`  
  - SPDX: `LicenseRef-scancode-softfloat AND LicenseRef-scancode-proprietary-license`  
  - Match: `softfloat AND proprietary-license` (lines 7-26, score 84.29, coverage 84.29%, rule `softfloat_and_proprietary-license_1.RULE`, relevance 100)

## Files with Copyright Detections

| File | Copyright | Lines |
| --- | --- | --- |
| `uaos/LICENSE` | Copyright (c) 2026 UAOS Development Team | 3-3 |
| `uaos/THIRD_PARTY_NOTICES.md` | Copyright (c) 1998-2001 Karl Stenerud | 29-29 |
| `uaos/THIRD_PARTY_NOTICES.md` | Copyright Nicola Salmoria and the MAME Team | 107-107 |
| `uaos/documentation/CODE_SOURCES.md` | Copyright (c) 1998-2001 Karl Stenerud | 58-58 |
| `uaos/documentation/CODE_SOURCES.md` | Copyright Nicola Salmoria and the MAME Team | 123-123 |
| `uaos/documentation/manual.html` | Copyright (c) 2026 UAOS Development Team | 2483-2483 |
| `uaos/documentation/manual.md` | Copyright (c) 2026 UAOS Development Team | 1257-1257 |
| `uaos/documentation/manual.pdf` | Copyright 2026 UAOS Development Team | 13-13 |
| `uaos/documentation/manual.tex` | Copyright extcopyright 2026 UAOS Development Team | 64-64 |
| `uaos/documentation/uaos.guide` | (c) 2026 UAOS Project | 4-4 |
| `uaos/emulation/src/musashi/example/m68kconf.h` | Copyright Karl Stenerud | 9-9 |
| `uaos/emulation/src/musashi/m68k.h` | Copyright Karl Stenerud | 9-9 |
| `uaos/emulation/src/musashi/m68k_in.c` | Copyright Karl Stenerud | 14-14 |
| `uaos/emulation/src/musashi/m68kconf.h` | Copyright Karl Stenerud | 9-9 |
| `uaos/emulation/src/musashi/m68kcpu.c` | Copyright Karl Stenerud | 9-9 |
| `uaos/emulation/src/musashi/m68kcpu.h` | Copyright Karl Stenerud | 9-9 |
| `uaos/emulation/src/musashi/m68kdasm.c` | Copyright Karl Stenerud | 9-9 |
| `uaos/emulation/src/musashi/m68kmake` | Copyright Karl Stenerud (kstenerud@gmail.com) | 88-88 |
| `uaos/emulation/src/musashi/m68kmake.c` | Copyright Karl Stenerud | 9-9 |
| `uaos/emulation/src/musashi/m68kmake.c` | Copyright Karl Stenerud (kstenerud@gmail.com) | 1242-1242 |
| `uaos/emulation/src/musashi/m68kmmu.h` | Copyright Nicola Salmoria and the MAME Team | 6-6 |
| `uaos/emulation/src/musashi/readme.txt` | Copyright 1998-2002 Karl Stenerud | 7-7 |
| `uaos/emulation/src/musashi/readme.txt` | Copyright (c) 1998-2001 Karl Stenerud | 29-29 |
| `uaos/kernel/display/about_win.c` | Copyright 2026 UAOS Development Team | 149-149 |
| `uaos/plans/68020_mmu_investigation_report.md` | (c) Table C | 214-214 |

## Files with Email Detections

| File | Email | Lines |
| --- | --- | --- |
| `uaos/emulation/src/musashi/m68kmake` | `kstenerud@gmail.com` | 88-88 |
| `uaos/emulation/src/musashi/m68kmake.c` | `kstenerud@gmail.com` | 1242-1242 |
| `uaos/emulation/src/musashi/readme.txt` | `kstenerud@gmail.com` | 60-60 |

## Files with URL Detections

| File | URL | Lines |
| --- | --- | --- |
| `uaos/README.md` | https://github.com/user-attachments/assets/66433a49-276c-4408-a7e7-6b869220c57e | 398-398 |
| `uaos/THIRD_PARTY_NOTICES.md` | https://github.com/kstenerud/Musashi | 16-16 |
| `uaos/THIRD_PARTY_NOTICES.md` | http://www.cs.berkeley.edu/~jhauser/arithmetic/SoftFloat.html | 54-54 |
| `uaos/THIRD_PARTY_NOTICES.md` | http://mamedev.org/ | 94-94 |
| `uaos/THIRD_PARTY_NOTICES.md` | http://mamedev.org/LICENSE.md | 112-112 |
| `uaos/documentation/CODE_SOURCES.md` | https://github.com/kstenerud/Musashi | 37-37 |
| `uaos/documentation/CODE_SOURCES.md` | http://mamedev.org/ | 124-124 |
| `uaos/documentation/index.html` | https://github.com/user-attachments/assets/66433a49-276c-4408-a7e7-6b869220c57e | 1040-1040 |
| `uaos/emulation/src/musashi/history.txt` | http://dynarec.com/~bart/files/68knotes.txt | 18-18 |
| `uaos/emulation/src/musashi/m68k_in.c` | http://dynarec.com/~bart/files/68knotes.txt | 38-38 |
| `uaos/emulation/src/musashi/m68kmmu.h` | http://mamedev.org/ | 7-7 |
| `uaos/emulation/src/musashi/readme.txt` | http://www.mame.net/ | 21-21 |
| `uaos/emulation/src/musashi/readme.txt` | https://github.com/kstenerud/Musashi | 54-54 |
| `uaos/emulation/src/musashi/softfloat/README.txt` | http://www.cs.berkeley.edu/~jhauser/arithmetic/SoftFloat.html | 76-76 |
| `uaos/emulation/src/musashi/softfloat/milieu.h` | http://www.cs.berkeley.edu/~jhauser/ | 14-14 |
| `uaos/emulation/src/musashi/softfloat/softfloat-macros` | http://www.cs.berkeley.edu/~jhauser/ | 14-14 |
| `uaos/emulation/src/musashi/softfloat/softfloat-specialize` | http://www.cs.berkeley.edu/~jhauser/ | 14-14 |
| `uaos/emulation/src/musashi/softfloat/softfloat.c` | http://www.cs.berkeley.edu/~jhauser/ | 14-14 |
| `uaos/emulation/src/musashi/softfloat/softfloat.h` | http://www.cs.berkeley.edu/~jhauser/ | 14-14 |
| `uaos/kernel/net/dns.h` | http://www.google.com/ | 42-42 |
| `uaos/kernel/net/ntp.c` | http://howardhinnant.github.io/date_algorithms.html | 105-105 |
| `uaos/scripts/build_iso.sh` | http://sun.hasenbraten.de/vasm/release/vasm.tar.gz | 940-940 |
| `uaos/scripts/build_iso.sh` | http://sun.hasenbraten.de/vlink/release/vlink.tar.gz | 944-944 |

## Scan Errors

_No scan errors._

---

### Notice

Generated with ScanCode and provided on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. No content created from ScanCode should be considered or used as legal advice. Consult an Attorney for any legal advice. ScanCode is a free software code scanning tool from nexB Inc. and others. Visit https://github.com/nexB/scancode-toolkit/ for support and download.
