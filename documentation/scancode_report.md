# ScanCode Report — UAOS

_Generated 2026-08-18 23:52 from `scancode_report.json`_

## Scan Metadata

- **Tool:** ScanCode Toolkit v32.5.0
- **Output format version:** 4.1.0
- **SPDX license list version:** 3.27
- **Scan start:** 2026-08-18T135134.227433
- **Scan end:** 2026-08-18T135231.439733
- **Duration:** 57.212321281433105s
- **Platform:** Linux-6.18.33.2-microsoft-standard-WSL2-x86_64-with-glibc2.39 (linux)
- **Python:** 3.12.3 (main, Jun 19 2026, 12:46:00) [GCC 13.3.0]
- **Errors:** 0  |  **Warnings:** 0

### Scan options
- Scans: --license, --copyright, --package, --info, --email, --url
- Post-scan: --classify, --summary, --tallies
- Ignored paths: build, sys-root, emulation/binaries, .git
- Processes: default

## Executive Summary

- **Declared license:** `(mit AND gpl-3.0-plus) AND unknown-license-reference`
- **Declared copyright holder:** UAOS Development Team
- **Primary language:** C
- **License clarity score:** 100/100
  - (declared_license=True, identification_precision=True, has_license_text=True, declared_copyrights=True, conflicting_license_categories=False, ambiguous_compound_licensing=False)

- **Files scanned:** 527
- **Directories scanned:** 110
- **Total size:** 8.01 MB
- **Files with license detections:** 13 (2.5%)
- **Files with copyright detections:** 20 (3.8%)
- **Files with package data:** 1
- **Files with emails:** 3
- **Files with URLs:** 11
- **Files with scan errors:** 0

## Tallies

### Detected license expressions

| License expression | Count |
| --- | ---: |
| _(none)_ | 513 |
| mit | 16 |
| bsd-new | 3 |
| commercial-license | 2 |
| bsd-new AND proprietary-license AND gpl-2.0-plus | 1 |
| gpl-3.0 AND gpl-2.0 AND proprietary-license | 1 |
| gpl-3.0 WITH gcc-exception-3.1 AND bsd-simplified AND gpl-3.0 AND proprietary-license | 1 |
| gpl-3.0-plus | 1 |
| gpl-3.0-plus AND gpl-1.0-plus AND gpl-3.0 AND mit | 1 |
| gpl-3.0-plus AND gpl-3.0 AND mit | 1 |
| mit AND gpl-3.0-plus | 1 |
| mit AND proprietary-license AND gpl-3.0-plus | 1 |
| proprietary-license | 1 |
| unknown-license-reference | 1 |

### Copyrights

| Copyright | Count |
| --- | ---: |
| _(none)_ | 507 |
| Copyright (c) Karl Stenerud | 12 |
| Copyright (c) UAOS Development Team | 5 |
| Copyright Karl Stenerud (kstenerud@gmail.com) | 2 |
| Copyright Nicola Salmoria and the MAME Team | 2 |
| (c) Table C | 1 |
| (c) UAOS Project | 1 |
| Copyright extcopyright UAOS Development Team | 1 |

### Holders

| Holder | Count |
| --- | ---: |
| _(none)_ | 507 |
| Karl Stenerud | 14 |
| UAOS Development Team | 5 |
| Nicola Salmoria and the MAME Team | 2 |
| Table | 1 |
| UAOS Project | 1 |
| extcopyright UAOS Development Team | 1 |

### Authors

| Author | Count |
| --- | ---: |
| _(none)_ | 524 |
| Free Software Foundation | 1 |
| GfxNew'. GfxAssociate Implemented Stores | 1 |
| Henk Kelder | 1 |
| John R. Hauser | 1 |
| John R. Hauser (International Computer Science Institute) | 1 |
| Karl Stenerud | 1 |
| R. Belmont | 1 |

### Programming languages

| Language | Count |
| --- | ---: |
| C | 280 |
| GAS | 83 |
| Bash | 6 |
| NASM | 3 |
| HTML | 2 |
| CSS | 1 |
| Python | 1 |
| verilog | 1 |

## Files with License Detections

| File | Detected License | SPDX | Detections |
| --- | --- | --- | ---: |
| `uaos/LICENSE` | mit | MIT | 1 |
| `uaos/README.md` | (mit AND gpl-3.0-plus) AND unknown-license-reference | (MIT AND GPL-3.0-or-later) AND LicenseRef-scancode-unknown-license-reference | 2 |
| `uaos/THIRD_PARTY_NOTICES.md` | mit AND bsd-new AND (bsd-new AND proprietary-license AND gpl-2.0-plus) AND gpl-3.0-plus AND (gpl-3.0-plus AND gpl-1.0-plus AND gpl-3.0 AND mit) AND (gpl-3.0 WITH gcc-exception-3.1 AND bsd-simplified AND gpl-3.0 AND proprietary-license) AND proprietary-license AND (gpl-3.0 AND gpl-2.0 AND proprietary-license) | MIT AND BSD-3-Clause AND (BSD-3-Clause AND LicenseRef-scancode-proprietary-license AND GPL-2.0-or-later) AND GPL-3.0-or-later AND (GPL-3.0-or-later AND GPL-1.0-or-later AND GPL-3.0-only AND MIT) AND (GPL-3.0-only WITH GCC-exception-3.1 AND BSD-2-Clause AND GPL-3.0-only AND LicenseRef-scancode-proprietary-license) AND LicenseRef-scancode-proprietary-license AND (GPL-3.0-only AND GPL-2.0-only AND LicenseRef-scancode-proprietary-license) | 10 |
| `uaos/documentation/CODE_SOURCES.md` | mit AND bsd-new AND (gpl-3.0-plus AND gpl-3.0 AND mit) AND (mit AND proprietary-license AND gpl-3.0-plus) | MIT AND BSD-3-Clause AND (GPL-3.0-or-later AND GPL-3.0-only AND MIT) AND (MIT AND LicenseRef-scancode-proprietary-license AND GPL-3.0-or-later) | 6 |
| `uaos/emulation/src/musashi/example/m68kconf.h` | mit | MIT | 1 |
| `uaos/emulation/src/musashi/m68k.h` | mit | MIT | 1 |
| `uaos/emulation/src/musashi/m68k_in.c` | mit | MIT | 1 |
| `uaos/emulation/src/musashi/m68kconf.h` | mit | MIT | 1 |
| `uaos/emulation/src/musashi/m68kcpu.c` | mit | MIT | 1 |
| `uaos/emulation/src/musashi/m68kcpu.h` | mit | MIT | 1 |
| `uaos/emulation/src/musashi/m68kdasm.c` | mit | MIT | 1 |
| `uaos/emulation/src/musashi/m68kmake.c` | mit | MIT | 1 |
| `uaos/emulation/src/musashi/readme.txt` | mit | MIT | 1 |

### License detection details

#### `uaos/LICENSE`

- **Expression:** `mit`  
  - SPDX: `MIT`  
  - Match: `mit` (lines 1-1, score 100.0, coverage 100.0%, rule `mit_14.RULE`, relevance 100)
  - Match: `mit` (lines 5-21, score 100.0, coverage 100.0%, rule `mit.LICENSE`, relevance 100)

#### `uaos/README.md`

- **Expression:** `mit AND gpl-3.0-plus`  
  - SPDX: `MIT AND GPL-3.0-or-later`  
  - Match: `mit` (lines 402-402, score 100.0, coverage 100.0%, rule `mit_43.RULE`, relevance 100)
  - Match: `mit` (lines 404-404, score 100.0, coverage 100.0%, rule `mit_126.RULE`, relevance 100)
  - Match: `gpl-3.0-plus` (lines 406-406, score 50.0, coverage 100.0%, rule `spdx_license_id_gpl-3.0-or-later_for_gpl-3.0-plus.RULE`, relevance 50)
- **Expression:** `unknown-license-reference`  
  - SPDX: `LicenseRef-scancode-unknown-license-reference`  
  - Match: `unknown-license-reference` (lines 410-410, score 50.0, coverage 100.0%, rule `license-intro_2.RULE`, relevance 50)

#### `uaos/THIRD_PARTY_NOTICES.md`

- **Expression:** `mit`  
  - SPDX: `MIT`  
  - Match: `mit` (lines 8-8, score 100.0, coverage 100.0%, rule `mit_1336.RULE`, relevance 100)
  - Match: `mit` (lines 1-1, score 100.0, coverage 100.0%, rule `mit_14.RULE`, relevance 100)
  - Match: `mit` (lines 5-21, score 100.0, coverage 100.0%, rule `mit.LICENSE`, relevance 100)
- **Expression:** `mit`  
  - SPDX: `MIT`  
  - Match: `unknown-license-reference` (lines 9-9, score 50.0, coverage 100.0%, rule `license-intro_2.RULE`, relevance 50)
  - Match: `mit` (lines 20-20, score 100.0, coverage 100.0%, rule `mit_30.RULE`, relevance 100)
- **Expression:** `mit`  
  - SPDX: `MIT`  
  - Match: `mit` (lines 36-52, score 100.0, coverage 100.0%, rule `mit.LICENSE`, relevance 100)
- **Expression:** `bsd-new`  
  - SPDX: `BSD-3-Clause`  
  - Match: `bsd-new` (lines 104-104, score 90.0, coverage 100.0%, rule `bsd-new_417.RULE`, relevance 90)
- **Expression:** `bsd-new AND proprietary-license AND gpl-2.0-plus`  
  - SPDX: `BSD-3-Clause AND LicenseRef-scancode-proprietary-license AND GPL-2.0-or-later`  
  - Match: `bsd-new` (lines 121-121, score 99.0, coverage 100.0%, rule `bsd-new_1102.RULE`, relevance 99)
  - Match: `proprietary-license` (lines 121-122, score 100.0, coverage 100.0%, rule `proprietary_non-commercial4.RULE`, relevance 100)
  - Match: `bsd-new` (lines 122-122, score 100.0, coverage 100.0%, rule `bsd-new_10.RULE`, relevance 100)
  - Match: `gpl-2.0-plus` (lines 122-122, score 100.0, coverage 100.0%, rule `spdx_license_id_gpl-2.0+_for_gpl-2.0-plus.RULE`, relevance 100)
- **Expression:** `gpl-3.0-plus`  
  - SPDX: `GPL-3.0-or-later`  
  - Match: `gpl-3.0-plus` (lines 134-134, score 100.0, coverage 100.0%, rule `gpl-3.0-plus_525.RULE`, relevance 100)
- **Expression:** `gpl-3.0-plus AND gpl-1.0-plus AND gpl-3.0 AND mit`  
  - SPDX: `GPL-3.0-or-later AND GPL-1.0-or-later AND GPL-3.0-only AND MIT`  
  - Match: `gpl-3.0-plus` (lines 145-148, score 100.0, coverage 100.0%, rule `gpl-3.0-plus_98.RULE`, relevance 100)
  - Match: `gpl-1.0-plus` (lines 150-150, score 90.0, coverage 100.0%, rule `gpl-1.0-plus_350.RULE`, relevance 90)
  - Match: `gpl-3.0` (lines 150-150, score 100.0, coverage 100.0%, rule `gpl-3.0_396.RULE`, relevance 100)
  - Match: `gpl-3.0` (lines 151-151, score 100.0, coverage 100.0%, rule `gpl-3.0_218.RULE`, relevance 100)
  - Match: `gpl-1.0-plus` (lines 153-153, score 50.0, coverage 100.0%, rule `gpl_bare_word_only.RULE`, relevance 50)
  - Match: `mit` (lines 153-154, score 100.0, coverage 100.0%, rule `mit_258.RULE`, relevance 100)
  - Match: `gpl-3.0` (lines 157-157, score 100.0, coverage 100.0%, rule `gpl-3.0_396.RULE`, relevance 100)
  - Match: `mit` (lines 158-158, score 100.0, coverage 100.0%, rule `mit_27.RULE`, relevance 100)
- **Expression:** `gpl-3.0 WITH gcc-exception-3.1 AND bsd-simplified AND gpl-3.0 AND proprietary-license`  
  - SPDX: `GPL-3.0-only WITH GCC-exception-3.1 AND BSD-2-Clause AND GPL-3.0-only AND LicenseRef-scancode-proprietary-license`  
  - Match: `gpl-3.0 WITH gcc-exception-3.1` (lines 170-170, score 100.0, coverage 100.0%, rule `gpl-3.0_with_gcc-exception-3.1_9.RULE`, relevance 100)
  - Match: `bsd-simplified` (lines 171-171, score 100.0, coverage 100.0%, rule `spdx_license_id_bsd-2-clause_for_bsd-simplified.RULE`, relevance 100)
  - Match: `gpl-3.0` (lines 172-172, score 100.0, coverage 100.0%, rule `gpl-3.0_6.RULE`, relevance 100)
  - Match: `gpl-3.0` (lines 173-173, score 100.0, coverage 100.0%, rule `gpl-3.0_6.RULE`, relevance 100)
  - Match: `proprietary-license` (lines 174-174, score 100.0, coverage 100.0%, rule `proprietary-license_544.RULE`, relevance 100)
- **Expression:** `proprietary-license`  
  - SPDX: `LicenseRef-scancode-proprietary-license`  
  - Match: `proprietary-license` (lines 175-175, score 100.0, coverage 100.0%, rule `proprietary-license_544.RULE`, relevance 100)
- **Expression:** `gpl-3.0 AND gpl-2.0 AND proprietary-license`  
  - SPDX: `GPL-3.0-only AND GPL-2.0-only AND LicenseRef-scancode-proprietary-license`  
  - Match: `gpl-3.0` (lines 176-176, score 100.0, coverage 100.0%, rule `gpl-3.0_6.RULE`, relevance 100)
  - Match: `gpl-2.0` (lines 177-177, score 100.0, coverage 100.0%, rule `gpl-2.0_52.RULE`, relevance 100)
  - Match: `proprietary-license` (lines 179-179, score 100.0, coverage 100.0%, rule `proprietary-license_544.RULE`, relevance 100)
  - Match: `proprietary-license` (lines 180-180, score 100.0, coverage 100.0%, rule `proprietary-license_271.RULE`, relevance 100)

#### `uaos/documentation/CODE_SOURCES.md`

- **Expression:** `mit`  
  - SPDX: `MIT`  
  - Match: `mit` (lines 66-66, score 100.0, coverage 100.0%, rule `mit_30.RULE`, relevance 100)
- **Expression:** `bsd-new`  
  - SPDX: `BSD-3-Clause`  
  - Match: `bsd-new` (lines 135-135, score 90.0, coverage 100.0%, rule `bsd-new_417.RULE`, relevance 90)
- **Expression:** `mit`  
  - SPDX: `MIT`  
  - Match: `mit` (lines 195-195, score 100.0, coverage 100.0%, rule `mit_43.RULE`, relevance 100)
  - Match: `mit` (lines 199-199, score 100.0, coverage 100.0%, rule `mit_126.RULE`, relevance 100)
- **Expression:** `gpl-3.0-plus AND gpl-3.0 AND mit`  
  - SPDX: `GPL-3.0-or-later AND GPL-3.0-only AND MIT`  
  - Match: `gpl-3.0-plus` (lines 216-216, score 50.0, coverage 100.0%, rule `spdx_license_id_gpl-3.0-or-later_for_gpl-3.0-plus.RULE`, relevance 50)
  - Match: `gpl-3.0` (lines 218-219, score 100.0, coverage 100.0%, rule `gpl-3.0_396.RULE`, relevance 100)
  - Match: `mit` (lines 219-219, score 100.0, coverage 100.0%, rule `mit_27.RULE`, relevance 100)
- **Expression:** `mit AND proprietary-license AND gpl-3.0-plus`  
  - SPDX: `MIT AND LicenseRef-scancode-proprietary-license AND GPL-3.0-or-later`  
  - Match: `mit` (lines 237-237, score 100.0, coverage 100.0%, rule `mit_14.RULE`, relevance 100)
  - Match: `proprietary-license` (lines 241-241, score 100.0, coverage 100.0%, rule `proprietary-license_544.RULE`, relevance 100)
  - Match: `gpl-3.0-plus` (lines 244-244, score 85.71, coverage 100.0%, rule `gpl-3.0-plus_69.RULE`, relevance 100)
- **Expression:** `mit`  
  - SPDX: `MIT`  
  - Match: `mit` (lines 272-272, score 100.0, coverage 100.0%, rule `mit_1336.RULE`, relevance 100)
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

## Files with Copyright Detections

| File | Copyright | Lines |
| --- | --- | --- |
| `uaos/LICENSE` | Copyright (c) 2026 UAOS Development Team | 3-3 |
| `uaos/THIRD_PARTY_NOTICES.md` | Copyright (c) 1998-2001 Karl Stenerud | 34-34 |
| `uaos/THIRD_PARTY_NOTICES.md` | Copyright Nicola Salmoria and the MAME Team | 116-116 |
| `uaos/documentation/CODE_SOURCES.md` | Copyright (c) 1998-2001 Karl Stenerud | 67-67 |
| `uaos/documentation/CODE_SOURCES.md` | Copyright Nicola Salmoria and the MAME Team | 132-132 |
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
| `uaos/.gitmodules` | https://github.com/stevereaver/Musashi.git | 3-3 |
| `uaos/README.md` | https://github.com/user-attachments/assets/66433a49-276c-4408-a7e7-6b869220c57e | 398-398 |
| `uaos/README.md` | https://www.gnu.org/software/grub | 405-405 |
| `uaos/THIRD_PARTY_NOTICES.md` | https://github.com/kstenerud/Musashi | 18-18 |
| `uaos/THIRD_PARTY_NOTICES.md` | http://www.cs.berkeley.edu/~jhauser/arithmetic/SoftFloat.html | 59-59 |
| `uaos/THIRD_PARTY_NOTICES.md` | http://mamedev.org/ | 102-102 |
| `uaos/THIRD_PARTY_NOTICES.md` | https://www.gnu.org/software/grub/ | 132-132 |
| `uaos/THIRD_PARTY_NOTICES.md` | https://www.gnu.org/licenses/gpl-3.0.txt | 151-151 |
| `uaos/documentation/CODE_SOURCES.md` | https://github.com/aboutcode-org/scancode-toolkit | 14-14 |
| `uaos/documentation/CODE_SOURCES.md` | https://github.com/kstenerud/Musashi | 46-46 |
| `uaos/documentation/CODE_SOURCES.md` | http://mamedev.org/ | 133-133 |
| `uaos/documentation/index.html` | https://github.com/user-attachments/assets/66433a49-276c-4408-a7e7-6b869220c57e | 1040-1040 |
| `uaos/emulation/src/musashi/history.txt` | http://dynarec.com/~bart/files/68knotes.txt | 18-18 |
| `uaos/emulation/src/musashi/m68k_in.c` | http://dynarec.com/~bart/files/68knotes.txt | 38-38 |
| `uaos/emulation/src/musashi/readme.txt` | http://www.mame.net/ | 21-21 |
| `uaos/emulation/src/musashi/readme.txt` | https://github.com/kstenerud/Musashi | 54-54 |
| `uaos/kernel/net/dns.h` | http://www.google.com/ | 42-42 |
| `uaos/kernel/net/ntp.c` | http://howardhinnant.github.io/date_algorithms.html | 105-105 |
| `uaos/scripts/build_iso.sh` | http://sun.hasenbraten.de/vasm/release/vasm.tar.gz | 939-939 |
| `uaos/scripts/build_iso.sh` | http://sun.hasenbraten.de/vlink/release/vlink.tar.gz | 943-943 |

## Scan Errors

_No scan errors._

---

### Notice

Generated with ScanCode and provided on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. No content created from ScanCode should be considered or used as legal advice. Consult an Attorney for any legal advice. ScanCode is a free software code scanning tool from nexB Inc. and others. Visit https://github.com/nexB/scancode-toolkit/ for support and download.
