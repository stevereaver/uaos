# 68020 and MMU Emulation Implementation Report

**Project:** UAOS (Ultimate Amiga OS)  
**Date:** 2026-06-17  
**Classification:** Technical Investigation & Implementation Roadmap

---

## Executive Summary

UAOS currently uses the **Musashi emulator** (v4.60) configured for **M68000** emulation only. The emulator already contains full implementations for **68020** and **68851/68030/68040 PMMU (Paged Memory Management Unit)**, but these are disabled in the UAOS build configuration.

This report details:
1. The current CPU emulation architecture
2. What 68020 features are already available in Musashi
3. What PMMU features are available
4. The specific changes required to enable 68020 + MMU support
5. The integration points between Musashi PMMU and UAOS's x86_64 MMU sandbox

---

## 1. Current Architecture

### 1.1 Musashi Integration

**Key Files:**
- `emulation/src/musashi/m68kcpu.c` - Main CPU execution engine
- `emulation/src/musashi/m68kcpu.h` - CPU state structures and internal macros
- `emulation/src/musashi/m68k_in.c` - Instruction definitions (518+ instructions)
- `emulation/src/musashi/m68kmmu.h` - **68851/68030/68040 PMMU implementation**
- `emulation/uaos_m68kconf.h` - **UAOS-specific configuration (currently 68000-only)**
- `emulation/uaos_m68k_glue.c` - UAOS integration layer

### 1.2 Current Configuration (68000-Only)

From `emulation/uaos_m68kconf.h`:

```c
#define M68K_EMULATE_010            M68K_OPT_OFF
#define M68K_EMULATE_EC020          M68K_OPT_OFF
#define M68K_EMULATE_020            M68K_OPT_OFF  // <- 68020 DISABLED
#define M68K_EMULATE_030            M68K_OPT_OFF
#define M68K_EMULATE_040            M68K_OPT_OFF
#define M68K_EMULATE_PMMU           M68K_OPT_OFF  // <- PMMU DISABLED
#define M68K_EMULATE_FPOINT         M68K_OPT_OFF  // <- FPU DISABLED
```

CPU initialization in `uaos_m68k_glue.c` (line 2151):
```c
m68k_set_cpu_type(M68K_CPU_TYPE_68000);  // Hardcoded to 68000
```

---

## 2. 68020 Emulation Requirements

### 2.1 68020-Specific Features Already in Musashi

The Musashi codebase contains complete implementations for the following 68020+ instructions, guarded by `CPU_TYPE_IS_EC020_PLUS`:

#### A. New Instructions (68020+)

| Instruction | Description | Implementation Status |
|-------------|-------------|----------------------|
| `DIVL` / `MULL` | 32-bit signed/unsigned divide/multiply | ✅ Complete |
| `CAS` / `CAS2` | Compare and swap (atomic) | ✅ Complete |
| `CHK2` / `CMP2` | Enhanced range check | ✅ Complete |
| `CALLM` / `RTM` | Call module / Return from module | ✅ Complete |
| `PACK` / `UNPK` | Pack/unpack BCD | ✅ Complete |
| `EXTB` | Sign-extend byte to long | ✅ Complete |
| Bitfield ops | BFCHG, BFCLR, BFEXTS, BFEXTU, BFFFO, BFINS, BFSET, BFTST | ✅ Complete |
| Scale factor | Index register scaling (1,2,4,8) | ✅ Complete |
| Long branches | Bcc.L, BRA.L, BSR.L (32-bit offset) | ✅ Complete |
| LINK.L | Long word stack link | ✅ Complete |
| `PUSH` / `POP` | Alias for -(An) and (An)+ | ✅ Complete |

#### B. Enhanced Addressing Modes (68020+)

- **Memory-indirect modes**: `([bd,An,Xn],od)`, `([bd,An],Xn,od)`
- **Scaled indexing**: `d8(An,Xn*scale)` where scale = 1,2,4,8
- **PC-relative with index**: Extended displacement sizes
- **Full 32-bit addressing**: 68000 limited to 24-bit; 68020 uses full 32-bit

#### C. New Control Registers (68020)

| Register | Description | Access |
|----------|-------------|--------|
| `CACR` | Cache Control Register | MOVEC |
| `CAAR` | Cache Address Register | MOVEC |
| `MSP` | Master Stack Pointer | MOVEC (supervisor) |
| `ISP` | Interrupt Stack Pointer | MOVEC (supervisor) |

#### D. Stack Frame Enhancements

68020 introduces new stack frame formats for exception handling (format $9, $A) that include more context for debugging and recovery.

### 2.2 Configuration Changes Required for 68020

**Step 1: Update `emulation/uaos_m68kconf.h`**

```c
// Change from:
#define M68K_EMULATE_020            M68K_OPT_OFF

// To:
#define M68K_EMULATE_020            M68K_OPT_ON
```

Optional: Also enable EC020 (embedded controller variant, no MMU):
```c
#define M68K_EMULATE_EC020          M68K_OPT_ON
```

**Step 2: Update CPU type initialization**

In `emulation/uaos_m68k_glue.c`, change:
```c
// From:
m68k_set_cpu_type(M68K_CPU_TYPE_68000);

// To:
m68k_set_cpu_type(M68K_CPU_TYPE_68020);  // Or M68K_CPU_TYPE_68EC020
```

**Step 3: Rebuild opcode table (if needed)**

If Musashi's `m68kops.c` was pre-generated for 68000 only, regenerate it:
```bash
cd emulation/src/musashi
gcc -o m68kmake m68kmake.c
./m68kmake . m68k_in.c  # Generates m68kops.c with 68020 instructions
```

### 2.3 68020 vs 68EC020 Decision

| Feature | 68020 | 68EC020 |
|---------|-------|---------|
| Address bus | 32-bit (4GB) | 24-bit (16MB) |
| PMMU (68851) | External (yes) | No |
| Typical use | Amiga 2500/3000 | Amiga 600/1200 (with accelerator) |

**Recommendation:** If implementing MMU support, use `M68K_CPU_TYPE_68020`. For cost-sensitive embedded-style emulation without MMU, use `M68K_CPU_TYPE_68EC020`.

---

## 3. PMMU (Paged MMU) Emulation Requirements

### 3.1 Existing PMMU Implementation in Musashi

**File:** `emulation/src/musashi/m68kmmu.h` (321 lines)

The PMMU implementation includes:

#### A. Address Translation: `pmmu_translate_addr()`

Implements full 68851/68030-style three-level page table walk:

```c
uint pmmu_translate_addr(uint addr_in)
{
    // 1. Select root pointer (SRP for supervisor, CRP for user)
    // 2. Extract IS (initial shift), A/B/C bits from TC register
    // 3. Walk Table A -> Table B -> Table C -> Page descriptor
    // 4. Handle early termination descriptors
    // 5. Return translated physical address
}
```

**Page Table Hierarchy:**
- **Table A (Root)**: Configurable bits (IS, abits)
- **Table B**: Middle level (bbits)
- **Table C**: Page level (cbits)
- **Page Descriptor**: Contains physical page base + protection bits

#### B. MMU Control Instructions: `m68881_mmu_ops()`

Handles all 68851 MMU coprocessor (COP 0) instructions:

| Instruction | Function | Status |
|-------------|----------|--------|
| `PMOVE` | Move to/from MMU registers | ✅ Implemented |
| `PLOAD` | Load entry into ATC | ⚠️ Stub (prints warning) |
| `PFLUSH` | Flush ATC entries | ⚠️ Stub (prints warning) |
| `PFLUSHR` | Flush ATC by root pointer | ⚠️ Stub (prints warning) |
| `PVALID` | Validate address | ⚠️ Stub (prints warning) |
| `PTEST` | Test address translation | ⚠️ Stub (prints warning) |
| `PBcc` | Branch on MMU condition | ⚠️ Stub (prints warning) |

#### C. MMU Registers in CPU State (`m68ki_cpu_core`)

```c
typedef struct {
    // ... other fields ...
    int    has_pmmu;          // PMMU availability flag
    int    pmmu_enabled;      // Enabled via TC register E-bit
    
    // MMU registers
    uint mmu_crp_aptr, mmu_crp_limit;   // CPU Root Pointer
    uint mmu_srp_aptr, mmu_srp_limit;   // Supervisor Root Pointer  
    uint mmu_tc;                        // Translation Control
    uint16 mmu_sr;                      // MMU Status
} m68ki_cpu_core;
```

#### D. Translation Control Register (TC) Format

```
Bit 31 (E): Enable bit - enables/disables MMU
Bit 30 (S): Supervisor root pointer enable
Bits 27-25: Function code lookup bits (not used in basic mode)
Bits 23-16 (IS): Initial shift (ignore top IS bits)
Bits 15-12 (A): Table A bits (index size)
Bits 11-8 (B): Table B bits
Bits 7-4 (C): Table C bits
Bits 3-0: Page size (usually 12 for 4KB pages)
```

### 3.2 Enabling PMMU in UAOS

**Step 1: Configuration**

In `emulation/uaos_m68kconf.h`:
```c
// Enable PMMU emulation
#define M68K_EMULATE_PMMU           M68K_OPT_ON

// Must also enable 68020 (PMMU requires it)
#define M68K_EMULATE_020            M68K_OPT_ON
```

**Step 2: Ensure 68020 CPU type is selected**

Only 68020, 68030, and 68040 support PMMU. EC020 does NOT have PMMU.

### 3.3 PMMU Integration with UAOS Memory System

#### Current Memory Architecture (No MMU)

```
┌─────────────────────────────────────────────────────────────┐
│                    M68k Emulator                           │
│  ┌─────────────┐        ┌──────────────────────────────┐  │
│  │  Musashi    │───────>│  uaos_m68k_glue.c callbacks   │  │
│  │  CPU Core   │        │  m68k_read_memory_*()         │  │
│  └─────────────┘        │  m68k_write_memory_*()        │  │
└─────────────────────────┼──────────────────────────────────┘
                          │
                          ▼ direct array access
                   ┌───────────────┐
                   │  g_ram[addr]  │  2MB per-task buffer
                   └───────────────┘
```

#### With PMMU Enabled (Address Translation Required)

```
┌─────────────────────────────────────────────────────────────┐
│                    M68k Emulator                           │
│  ┌─────────────┐        ┌────────────────────────────────┐  │
│  │  Musashi    │───────>│  m68ki_read/write_*_fc()       │  │
│  │  CPU Core   │        │  (in m68kcpu.c)                │  │
│  └─────────────┘        └────────────────────────────────┘  │
│           │                           │                      │
│           │                           ▼                      │
│           │              ┌───────────────────────┐           │
│           │              │  pmmu_translate_addr()│           │
│           │              │  (if PMMU enabled)    │           │
│           │              └───────────────────────┘           │
│           │                           │                      │
│           │                           ▼                      │
│           │              ┌───────────────────────┐           │
│           └─────────────>│  m68k_read_memory_*() │           │
│                          │  (glue callbacks)     │           │
│                          └───────────────────────┘           │
│                                       │                      │
└───────────────────────────────────────┼──────────────────────┘
                                          │
                                          ▼
                                   ┌───────────────┐
                                   │  g_ram[phys]  │  Physical memory
                                   └───────────────┘
```

#### Critical Integration Point

When `M68K_EMULATE_PMMU` is enabled, Musashi calls `pmmu_translate_addr()` before every memory access. The glue layer at `m68k_read_memory_*()` receives the **translated physical address**, not the virtual address.

**Current glue code (direct access):**
```c
unsigned int m68k_read_memory_8(unsigned int addr) {
    if (addr < GUEST_RAM_SIZE) return g_ram[addr];
    return 0xFF;
}
```

**With PMMU (same interface, addr is already translated):**
```c
unsigned int m68k_read_memory_8(unsigned int addr) {
    // addr is already PHYSICAL after pmmu_translate_addr()
    if (addr < GUEST_RAM_SIZE) return g_ram[addr];
    return 0xFF;
}
```

**No changes needed to glue layer!** Musashi handles the translation internally.

---

## 4. Integration with UAOS x86_64 MMU Sandbox

### 4.1 Current x86_64 MMU Sandbox

**File:** `kernel/exec/mmu_sandbox.c`

UAOS has a complete x86_64 paging implementation that maps the 4GB Amiga guest address space:
- 4-level paging (PML4 → PDPT → PD → PT)
- 2MB huge pages for efficiency
- Special handling for CIA/custom chip window (0x00B00000-0x00DFFFFF)
- Page fault handler for chip register emulation

### 4.2 Two-Layer MMU Architecture

With 68020 PMMU + UAOS sandbox, there would be **two layers** of address translation:

```
┌──────────────────────────────────────────────────────────────┐
│ Layer 1: M68k PMMU (Musashi)                                 │
│                                                              │
│  Virtual Address (32-bit)                                     │
│       │                                                      │
│       ▼                                                      │
│  ┌─────────────────────────────────────┐                    │
│  │ pmmu_translate_addr()               │                    │
│  │ 68851-style page table walk         │                    │
│  │  VA -> Intermediate Physical Address│                    │
│  └─────────────────────────────────────┘                    │
│       │                                                      │
│       ▼                                                      │
│  Intermediate Physical Address (within 4GB window)          │
└──────────────────────────────────────────────────────────────┘
                          │
                          ▼ g_ram[addr]
┌──────────────────────────────────────────────────────────────┐
│ Layer 2: x86_64 Host Paging (Optional enhancement)           │
│                                                              │
│  Currently: Direct array access (simple, fast)                │
│                                                              │
│  Optional: Could use x86_64 page tables for:                │
│  - Per-task memory isolation at hardware level                │
│  - Copy-on-write for exec.library-shared memory               │
│  - Demand paging from Amiga disk images                     │
└──────────────────────────────────────────────────────────────┘
```

### 4.3 Memory Map with PMMU Enabled

```
Amiga Virtual Address Space (as seen by M68k programs):
├─ 0x00000000-0x001FFFFF: Chip RAM (mapped by PMMU)
├─ 0x00200000-0x009FFFFF: Fast RAM (mapped by PMMU)  
├─ 0x00B00000-0x00DFFFFF: Custom chip registers (special handling)
├─ 0x00F80000-0x00FFFFFF: Kickstart ROM (mapped by PMMU)
└─ 0x01000000-0xFFFFFFFF: Extended RAM (mapped by PMMU)

M68k PMMU Page Tables (in Amiga memory):
├─ Root Pointer (SRP/CRP) -> Table A -> Table B -> Table C -> Page
└─ Each page descriptor contains physical address + protection bits

Physical Address (as seen by UAOS glue layer):
└─ Direct index into g_ram[] array (2MB per-task buffer)
```

---

## 5. Implementation Roadmap

### Phase 1: Basic 68020 Support (1-2 days)

**Objective:** Enable 68020 instruction set without PMMU.

1. **Configuration changes:**
   - Edit `emulation/uaos_m68kconf.h`:
     ```c
     #define M68K_EMULATE_020            M68K_OPT_ON
     #define M68K_EMULATE_EC020          M68K_OPT_ON  // Optional
     ```

2. **CPU type change:**
   - Edit `emulation/uaos_m68k_glue.c` line 2151:
     ```c
     m68k_set_cpu_type(M68K_CPU_TYPE_68EC020);  // Start with EC020 (no MMU)
     ```

3. **Regenerate opcode table:**
   ```bash
   cd emulation/src/musashi
   gcc -o m68kmake m68kmake.c
   ./m68kmake . m68k_in.c
   ```

4. **Test with 68020-specific programs:**
   - Test DIVL/MULL instructions
   - Test bitfield operations (BFEXT, BFINS)
   - Test scaled indexing modes

**Risk:** Low. Musashi already contains all 68020 code; just enabling it.

### Phase 2: PMMU Foundation (2-3 days)

**Objective:** Enable PMMU emulation with simple test case.

1. **Enable PMMU in config:**
   - Edit `emulation/uaos_m68kconf.h`:
     ```c
     #define M68K_EMULATE_020            M68K_OPT_ON
     #define M68K_EMULATE_PMMU           M68K_OPT_ON
     ```

2. **Switch to full 68020:**
   - Edit `uaos_m68k_glue.c`:
     ```c
     m68k_set_cpu_type(M68K_CPU_TYPE_68020);
     ```

3. **Rebuild and test basic operation:**
   - Most programs should still work (PMMU starts disabled)
   - Test that PMOVE to TC register works
   - Verify `pmmu_enabled` flag gets set when TC.E=1

4. **Create simple MMU test:**
   - Write M68k assembly that:
     1. Sets up a simple page table (identity mapping)
     2. Enables MMU via TC register
     3. Accesses memory through MMU
     4. Disables MMU

**Risk:** Medium. PMMU translation logic is complex; requires testing.

### Phase 3: Page Table Management (3-5 days)

**Objective:** Implement Amiga-compatible page table allocation and management.

1. **Allocate page table memory:**
   - Page tables must be in M68k-addressable memory
   - Add allocator for 4KB-aligned blocks in guest RAM

2. **Page table manipulation helpers:**
   ```c
   // New functions to add
   void pmmu_create_table(uint32_t parent_entry, int level, uint32_t addr);
   void pmmu_map_page(uint32_t table_entry, uint32_t phys_page, uint32_t flags);
   void pmmu_unmap_page(uint32_t table_entry);
   ```

3. **Integration with exec.library:**
   - AmigaOS uses MMU for:
     - Memory protection (read/write/execute)
     - Copy-on-write for library data
     - Memory tracking and statistics
   - Add MMU-related fields to exec.library structures

4. **Implement ATC flush operations:**
   - Currently `PFLUSH` is a stub in Musashi
   - Add ATC (Address Translation Cache) simulation
   - ATC is essentially a TLB (Translation Lookaside Buffer)

**Risk:** Medium-High. Requires understanding AmigaOS MMU usage patterns.

### Phase 4: Full AmigaOS MMU Compatibility (1-2 weeks)

**Objective:** Support AmigaOS 3.x MMU features (Enforcer, MuGuardianAngel, etc.)

1. **MMU exception handling:**
   - Bus error exceptions for page faults
   - Stack frame format for MMU faults
   - `MMU_SSR` (MMU Status Register) reporting

2. **Protection features:**
   - Read-only pages for code segments
   - Guard pages for stack overflow detection
   - NULL pointer protection (page 0 not mapped)

3. **Copy-on-write implementation:**
   - Write-protect pages shared between tasks
   - Page fault handler duplicates page on write
   - Essential for efficient fork()/spawn()

4. **Testing with MMU-aware software:**
   - Enforcer (memory protection tool)
   - MuGuardianAngel (post-mortem debugger)
   - Later AmigaOS 3.x exec.library functions

**Risk:** High. Requires deep AmigaOS internals knowledge.

---

## 6. Key Files and Line Numbers

### Configuration
- `emulation/uaos_m68kconf.h` (lines 16-21, 78) - CPU type and PMMU enable
- `emulation/uaos_m68k_glue.c` (line 2151) - CPU type initialization

### Musashi Core
- `emulation/src/musashi/m68kcpu.h` (lines 938-1017) - CPU state structure with PMMU fields
- `emulation/src/musashi/m68kcpu.h` (lines 975-999) - PMMU register definitions
- `emulation/src/musashi/m68kmmu.h` (all) - PMMU implementation
- `emulation/src/musashi/m68k_in.c` - 68020+ instruction handlers (search for CPU_TYPE_IS_EC020_PLUS)

### UAOS Memory System
- `kernel/exec/mmu_sandbox.c` - x86_64 paging sandbox (independent of M68k PMMU)
- `kernel/exec/exec_task.c` (lines 43-65) - Per-task RAM allocation
- `emulation/uaos_m68k_glue.c` (lines 210-274) - Memory callbacks

---

## 7. Open Questions and Considerations

### 7.1 Performance Impact

- **PMMU translation overhead:** Every memory access requires table walk (3-4 memory reads)
- **Mitigation:** Musashi has ATC (TLB) but it's not fully implemented
- **Benchmark:** Compare M68k instruction throughput with/without PMMU

### 7.2 Memory Layout Compatibility

- Real Amiga with 68851: MMU tables in dedicated fast RAM
- UAOS: Page tables in per-task g_ram[] buffer
- **Question:** Does this affect AmigaOS behavior?

### 7.3 Integration with Existing x86_64 Sandbox

- Current sandbox maps entire 4GB as identity-mapped
- With PMMU: Should x86_64 tables change per-task?
- **Options:**
  1. Keep identity mapping, let PMMU handle all translation (simplest)
  2. Use x86_64 tables for per-task isolation (more complex, more secure)

### 7.4 68851 vs 68030/040 PMMU Differences

- 68851: External chip, more complex, used in Amiga 2500/3000
- 68030: Integrated, slightly different register layout
- 68040: Simplified "paged" MMU, different again
- **Musashi implements 68851/68030-style PMMU**
- **Recommendation:** Target 68030 PMMU model for broader compatibility

---

## 8. Summary

### What Exists (Ready to Use)

| Component | Status | Location |
|-----------|--------|----------|
| Musashi 68020 core | ✅ Complete | `m68kcpu.c`, `m68k_in.c` |
| 68020 instructions | ✅ Complete | `m68k_in.c` (518+ instrs) |
| PMMU translation | ✅ Complete | `m68kmmu.h` |
| PMMU control ops | ⚠️ Partial | `m68kmmu.h` (PMOVE works, others stubbed) |
| UAOS glue layer | ✅ Complete | `uaos_m68k_glue.c` |
| x86_64 sandbox | ✅ Complete | `mmu_sandbox.c` |

### What Needs to be Done

| Task | Effort | Priority |
|------|--------|----------|
| Enable 68020 in config | 1 hour | HIGH |
| Regenerate opcode table | 30 min | HIGH |
| Enable PMMU in config | 1 hour | HIGH |
| Test basic 68020 operation | 1 day | HIGH |
| Test PMMU with identity map | 1-2 days | MEDIUM |
| Implement ATC/TLB flush | 2-3 days | MEDIUM |
| Page table allocator | 2-3 days | MEDIUM |
| AmigaOS MMU integration | 1-2 weeks | LOW |
| Copy-on-write support | 3-5 days | LOW |

### Recommended First Steps

1. **Start with 68EC020** (no MMU) to validate 68020 instruction set
2. **Enable PMMU** and test with simple identity-mapped page table
3. **Add PMMU register dump** to debug page table walks
4. **Implement PFLUSH** (ATC flush) before other coprocessor ops
5. **Profile performance** to understand translation overhead

### Risk Assessment

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| Musashi PMMU bugs | Medium | High | Test with real Amiga MMU code |
| Performance degradation | High | Medium | Implement software TLB cache |
| AmigaOS incompatibility | Medium | High | Test with actual Amiga MMU software |
| Memory layout issues | Low | Medium | Validate against Amiga hardware behavior |

---

## Appendix A: 68020 Instruction Quick Reference

### New/Enhanced Instructions

```
Bitfield Operations (all 68020+):
  BFCHG Dn,<ea>{o:w}     - Bit field change
  BFCLR Dn,<ea>{o:w}     - Bit field clear  
  BFEXTS Dn,<ea>{o:w}    - Bit field extract signed
  BFEXTU Dn,<ea>{o:w}    - Bit field extract unsigned
  BFFFO Dn,<ea>{o:w}     - Bit field find first one
  BFINS Dn,<ea>{o:w}     - Bit field insert
  BFSET Dn,<ea>{o:w}     - Bit field set
  BFTST Dn,<ea>{o:w}     - Bit field test

  Where {o:w} = {offset:width}, each can be immediate (0-31) or data register

Enhanced Arithmetic (68020+):
  DIVS.L Dn,Dm           - 32-bit signed divide
  DIVU.L Dn,Dm           - 32-bit unsigned divide
  MULS.L Dn,Dm           - 32-bit signed multiply
  MULU.L Dn,Dm           - 32-bit unsigned multiply

Atomic Operations (68020+):
  CAS Dc,Du,<ea>         - Compare and swap
  CAS2 Dc1:Dc2,Du1:Du2  - Compare and swap dual

Range Checking (68020+):
  CHK2 <ea>,Rn           - Check register against bounds
  CMP2 <ea>,Rn           - Compare register against bounds

Module Operations (68020+, rarely used):
  CALLM #data,<ea>       - Call module
  RTM Rn                 - Return from module

Data Conversion (68020+):
  EXTB.L Dn              - Sign-extend byte to long
  PACK -(Ax),-(Ay),#adj  - Pack BCD
  UNPK -(Ax),-(Ay),#adj  - Unpack BCD

Enhanced Control (68020+):
  LINK.L An,#disp        - Long displacement link
  Bcc.L <label>          - Long branch (32-bit offset)
  BRA.L <label>          - Unconditional long branch
  BSR.L <label>          - Long branch to subroutine
```

### New Addressing Modes

```
Scaled Indexing (68020+):
  d8(An,Xn*scale)        - scale = 1, 2, 4, or 8
  d8(PC,Xn*scale)        - PC-relative with scaled index

Memory Indirect (68020+):
  ([bd,An,Xn],od)        - Pre-indexed indirect
  ([bd,An],Xn,od)        - Post-indexed indirect
  
  Where bd = base displacement (0, 16, or 32 bit)
        od = outer displacement (0, 16, or 32 bit)
```

---

## Appendix B: PMMU Register Reference

### Translation Control Register (TC)

```
Bit 31 (E):  Enable - 1=MMU on, 0=MMU off
Bit 30 (S):  SRP enable - 1=use SRP in supervisor mode
Bits 27-25: FCL - Function code lookup (advanced)
Bits 23-16: IS - Initial shift (bits to ignore at top of VA)
Bits 15-12: A - Table A index bits
Bits 11-8:  B - Table B index bits  
Bits 7-4:   C - Table C index bits
Bits 3-0:   Page size (PS)
  0x0 = 256 bytes
  0x1 = 512 bytes
  0x2 = 1 KB
  0x3 = 2 KB
  0x4 = 4 KB (typical)
  0x5 = 8 KB
  0x6 = 16 KB
  0x7 = 32 KB
```

### Root Pointer Registers (SRP, CRP)

```
Upper 32 bits (limit):  Descriptor limit value
Lower 32 bits (aptr):   Address of root table (Table A)
Bit 1-0 of lower:       Table type (0=invalid, 1=early term, 2=4-byte, 3=8-byte)
```

### MMU Status Register (MMUSR)

```
Bits 15-14: Bus error type
Bit 13:     Write protect violation
Bit 12:     Invalid descriptor
Bit 11:     Limit violation
Bit 10:     Supervisor violation
Bits 9-0:   Reserved
```

---

## Appendix C: Build Configuration Summary

### Current Build (68000 Only)

```bash
# GCC flags in scripts/build_iso.sh
gcc ${GCC_FLAGS} -w \
    -DMUSASHI_CNF='"uaos_m68kconf.h"' \
    -I"${REPO_ROOT}/emulation" \
    -c m68kcpu.c m68kops.c softfloat.c
```

### 68020 + PMMU Build

```bash
# 1. Update uaos_m68kconf.h:
#    M68K_EMULATE_020 = M68K_OPT_ON
#    M68K_EMULATE_PMMU = M68K_OPT_ON

# 2. Regenerate opcode table:
cd emulation/src/musashi
gcc -o m68kmake m68kmake.c
./m68kmake . m68k_in.c

# 3. Update uaos_m68k_glue.c:
#    m68k_set_cpu_type(M68K_CPU_TYPE_68020)

# 4. Build as normal
./scripts/build_iso.sh
```

---

*Report generated by Devin AI - Technical Investigation*
*For questions or clarifications, see the implementation files referenced above.*