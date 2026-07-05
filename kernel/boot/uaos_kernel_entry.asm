; uaos_kernel_entry.asm — UAOS Multiboot2 + ELF64 Kernel Entry Point
;
; Assembled with: nasm -f elf64 uaos_kernel_entry.asm -o uaos_kernel_entry.o
; Linked  with:   ld -T uaos_kernel.ld uaos_kernel_entry.o -o uaos-kernel.elf
;
; GRUB2 Multiboot2 entry contract (32-bit protected mode):
;   EAX = 0x36D76289  (multiboot2 magic)
;   EBX = 32-bit physical address of multiboot2 info structure
;
; Long-mode bootstrap sequence:
;   1. Build identity-map page tables in 32-bit mode (no paging yet)
;   2. Enable PAE, load CR3, set EFER.LME, enable paging → long mode
;   3. Far-jump to 64-bit code segment
;   4. Set up 64-bit stack, pass mb2 params in RDI/RSI, call C main

bits 32

; =========================================================================
; Multiboot2 Header  (must appear within first 32 KB of the binary)
; =========================================================================

MB2_MAGIC    equ  0xE85250D6
MB2_ARCH     equ  0                         ; i386 protected mode
MB2_HDRLEN   equ  (mb2_end - mb2_start)
MB2_CHECKSUM equ  (-(MB2_MAGIC + MB2_ARCH + MB2_HDRLEN)) & 0xFFFFFFFF

section .multiboot2 progbits alloc noexec nowrite align=8

mb2_start:
    dd  MB2_MAGIC
    dd  MB2_ARCH
    dd  MB2_HDRLEN
    dd  MB2_CHECKSUM

    ; ---------------------------------------------------------------
    ; Framebuffer request tag (type 5, optional) — 8-byte aligned
    ; size must be padded to next 8-byte boundary: 20 bytes → pad 4
    ; ---------------------------------------------------------------
    dw  5       ; tag type: framebuffer request
    dw  1       ; flags = optional (bit 0)
    dd  20      ; size (including this header: 4+4+4+4+4 = 20 bytes)
    dd  1024    ; preferred width
    dd  768     ; preferred height
    dd  32      ; preferred bpp (depth)
    dd  0       ; 4 bytes padding to align next tag to 8-byte boundary

    ; ---------------------------------------------------------------
    ; End tag (type 0, size 8) — mandatory terminator
    ; ---------------------------------------------------------------
    dw  0       ; tag type: end
    dw  0       ; flags
    dd  8       ; size
mb2_end:

; =========================================================================
; Static data — GDT for long-mode transition (in .data so linker places it)
; =========================================================================

section .data progbits alloc noexec write align=8

global gdt64_start
gdt64_start:
    dq  0x0000000000000000      ; 0x00 — null
    dq  0x00AF9A000000FFFF      ; 0x08 — 64-bit kernel code: P=1 DPL=0 L=1
    dq  0x00CF92000000FFFF      ; 0x10 — 64-bit kernel data: P=1 DPL=0
    dq  0x00AFFA000000FFFF      ; 0x18 — 64-bit user   code: P=1 DPL=3 L=1  (selector 0x1B with RPL=3)
    dq  0x00CFF2000000FFFF      ; 0x20 — 64-bit user   data: P=1 DPL=3      (selector 0x23 with RPL=3)
    ; 0x28/0x30 — TSS descriptor (16 bytes, two slots) — base filled at runtime by GDT_Init_TSS()
    dq  0x0000000000000000      ; TSS low  (type=0x89 = 64-bit available TSS, filled at runtime)
    dq  0x0000000000000000      ; TSS high (base[63:32], all zeros filled at runtime)
gdt64_end:

gdt64_ptr:
    dw  (gdt64_end - gdt64_start - 1)  ; limit
    dq  gdt64_start                     ; 64-bit base (used after LM entry)

gdt32_ptr:                              ; 32-bit descriptor used for lgdt
    dw  (gdt64_end - gdt64_start - 1)
    dd  gdt64_start                     ; 32-bit truncation — OK below 4 GB

; =========================================================================
; BSS — bootstrap stack + page tables (all zeroed by loader)
; =========================================================================

section .bss nobits alloc noexec write align=4096

pml4_table: resb 4096
pdpt_table: resb 4096
pd_table:   resb 4096           ; covers 0–1 GB with 512 × 2 MB pages

align 16
stack_bottom:
    resb    65536               ; 64 KB bootstrap stack
stack_top:

; =========================================================================
; 32-bit entry point
; =========================================================================

section .text progbits alloc exec nowrite align=16

extern __guest_ram_start
extern __guest_ram_end
extern __bss_start
extern __bss_end

global _start
_start:
    cli

    ; ---- Save multiboot2 parameters in callee-saved registers ----
    ; EAX = mb2 magic, EBX = mb2 info ptr (set by GRUB)
    mov     ebp, eax            ; save magic  in EBP (survives BSS zeroing)
    ; EBX already holds the info pointer

    ; ---- Load our 32-bit GDT so segments are definitely flat ----
    lgdt    [gdt32_ptr]
    mov     ax, 0x10
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    xor     ax, ax
    mov     fs, ax
    mov     gs, ax

    ; ================================================================
    ; ZERO THE BSS SECTION (GRUB may not zero all 32+ MB of it)
    ; Must happen BEFORE setting up the stack — the stack and page
    ; tables live in .bss and will be (re)initialised below.
    ; rep stosd clobbers EAX, ECX, EDI — EBP and EBX are safe.
    ; ================================================================
    mov     edi, __bss_start
    mov     ecx, __bss_end
    sub     ecx, edi
    shr     ecx, 2              ; dwords
    xor     eax, eax
    rep     stosd

    ; ================================================================
    ; ZERO THE GUEST RAM SECTION (NOLOAD — bootloader may not clear it)
    ; ================================================================
    mov     edi, __guest_ram_start
    mov     ecx, __guest_ram_end
    sub     ecx, edi
    shr     ecx, 2              ; dwords
    xor     eax, eax
    rep     stosd

    ; ---- Restore multiboot2 parameters ----
    mov     edi, ebp            ; magic  → EDI (survives mode switch)
    mov     esi, ebx            ; info   → ESI

    ; ---- Set up the bootstrap stack (in .bss, now zeroed) ----
    mov     esp, stack_top

    ; ================================================================
    ; BUILD IDENTITY-MAP PAGE TABLES (in 32-bit mode, paging OFF)
    ;
    ; PML4[0]  → pdpt_table   (0–512 GB)
    ; PDPT[0]  → pd_table     (0–1 GB)
    ; PD[0..N] → 2 MB huge pages identity-mapped
    ; ================================================================

    ; PML4[0] = &pdpt_table | PRESENT | WRITABLE
    mov     eax, pdpt_table
    or      eax, 0x03
    mov     dword [pml4_table],     eax
    mov     dword [pml4_table + 4], 0

    ; PDPT[0] = &pd_table | PRESENT | WRITABLE
    mov     eax, pd_table
    or      eax, 0x03
    mov     dword [pdpt_table],     eax
    mov     dword [pdpt_table + 4], 0

    ; PD[0..511] = i<<21 | PRESENT | WRITABLE | HUGE (PS)
    mov     ecx, 0
.fill_pd:
    mov     eax, ecx
    shl     eax, 21             ; physical address of 2 MB page
    or      eax, 0x83           ; PRESENT | WRITABLE | HUGE
    mov     dword [pd_table + ecx*8],     eax
    mov     dword [pd_table + ecx*8 + 4], 0
    inc     ecx
    cmp     ecx, 512
    jl      .fill_pd

    ; ================================================================
    ; ENTER LONG MODE
    ; ================================================================

    ; 1. Enable PAE (CR4.PAE = bit 5) and SSE (CR4.OSFXSR = bit 9)
    mov     eax, cr4
    or      eax, (1 << 5) | (1 << 9)
    mov     cr4, eax

    ; 2. Load PML4 into CR3
    mov     eax, pml4_table
    mov     cr3, eax

    ; 3. Set EFER.LME (MSR 0xC0000080 bit 8)
    mov     ecx, 0xC0000080
    rdmsr
    or      eax, (1 << 8)
    wrmsr

    ; 4. Enable paging (CR0.PG = bit 31) — activates long mode
    mov     eax, cr0
    or      eax, (1 << 31)
    mov     cr0, eax

    ; 5. Far jump into 64-bit code segment (selector 0x08)
    jmp     0x08:long_mode_entry

; =========================================================================
; 64-bit entry point
; =========================================================================

bits 64
long_mode_entry:
    ; Reload data segments with 64-bit data selector
    mov     ax, 0x10
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    xor     ax, ax
    mov     fs, ax
    mov     gs, ax

    ; Reload GDT with full 64-bit pointer
    lgdt    [rel gdt64_ptr]

    ; Set up 64-bit stack
    mov     rsp, stack_top

    ; Align stack to 16 bytes (SysV ABI requirement before call)
    and     rsp, ~0xF

    ; Pass multiboot2 params to C main (SysV ABI: RDI=arg1, RSI=arg2)
    ; EDI and ESI were set in 32-bit mode and are zero-extended in 64-bit
    mov     edi, edi            ; zero-extend EDI → RDI  (mb2 magic)
    mov     esi, esi            ; zero-extend ESI → RSI  (mb2 info phys)

    ; Call C kernel entry
    extern  uaos_kernel_main
    call    uaos_kernel_main

    ; Should never return
kernel_halt:
    cli
    hlt
    jmp     kernel_halt
