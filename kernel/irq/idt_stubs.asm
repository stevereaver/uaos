; idt_stubs.asm — UAOS x86_64 IDT ISR entry stubs
;
; Generates 256 ISR entry points.  Each stub pushes a dummy error code
; (0) for vectors that don't push one, then pushes the vector number,
; then jumps to the common dispatch trampoline.
;
; Assembled with: nasm -f elf64 idt_stubs.asm -o idt_stubs.o

bits 64
section .text

; -------------------------------------------------------------------------
; isr_common — saves all GPRs, calls C dispatcher, restores, iretq
; C prototype: void ISR_Dispatch(uint64_t vector, uint64_t error_code,
;                                uint64_t rip, uint64_t cs,
;                                uint64_t rflags, uint64_t rsp, uint64_t ss)
; We pass the vector and error code in RDI/RSI (SysV ABI).
; -------------------------------------------------------------------------

extern ISR_Dispatch
extern Task_SwitchNext
extern Task_SwitchPrev

isr_common:
    ; Stack at entry:
    ;   [rsp+0]  = vector  (pushed by stub)
    ;   [rsp+8]  = error_code (pushed by stub or 0)
    ;   [rsp+16] = rip
    ;   [rsp+24] = cs
    ;   [rsp+32] = rflags
    ;   [rsp+40] = rsp_prev
    ;   [rsp+48] = ss

    push    rax
    push    rbx
    push    rcx
    push    rdx
    push    rsi
    push    rdi
    push    rbp
    push    r8
    push    r9
    push    r10
    push    r11
    push    r12
    push    r13
    push    r14
    push    r15

    ; The frame above the saved GPRs is:
    ;   rsp+15*8 = vector
    ;   rsp+16*8 = error_code
    ;   rsp+17*8 = rip
    ;   rsp+18*8 = cs
    ;   rsp+19*8 = rflags
    ;   rsp+20*8 = rsp_prev
    ;   rsp+21*8 = ss

    mov     rdi, [rsp + 15*8]   ; vector
    mov     rsi, [rsp + 16*8]   ; error_code
    mov     rdx, [rsp + 17*8]   ; rip
    call    ISR_Dispatch

    ; -----------------------------------------------------------------
    ; Task switch requested by scheduler?
    ; -----------------------------------------------------------------
    mov     rax, [rel Task_SwitchNext]
    test    rax, rax
    jz      .no_switch

    ; Save current RSP (points to R15 slot on interrupted task's stack)
    ; into the old task's native_rsp.
    mov     rbx, [rel Task_SwitchPrev]
    mov     [rbx + 136], rsp    ; offset of native_rsp in UaosTask

    ; -----------------------------------------------------------------
    ; All task types (including X64 ELF64) use the same iretq-based
    ; restore path.  Task_CreateX64 builds a synthetic interrupt frame
    ; with user-mode CS (0x1B) and SS (0x23) so iretq automatically
    ; transitions to ring 3.  No special first-launch branch needed.
    ; -----------------------------------------------------------------

.normal_switch:
    ; Load new task's RSP and clear switch request.
    mov     rsp, [rax + 136]
    mov     qword [rel Task_SwitchNext], 0
    mov     qword [rel Task_SwitchPrev], 0

.no_switch:
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     r11
    pop     r10
    pop     r9
    pop     r8
    pop     rbp
    pop     rdi
    pop     rsi
    pop     rdx
    pop     rcx
    pop     rbx
    pop     rax
    add     rsp, 16             ; discard vector + error_code
    iretq

; -------------------------------------------------------------------------
; Macro: ISR stub
;   has_error = 1 if CPU pushes an error code (exceptions 8,10-14,17,21)
;   has_error = 0 otherwise (push dummy 0 first)
; -------------------------------------------------------------------------

%macro ISR_STUB 2       ; %1 = vector number, %2 = has_error_code
global isr_stub_%1
isr_stub_%1:
%if %2 == 0
    push    qword 0     ; dummy error code
%endif
    push    qword %1    ; vector number
    jmp     isr_common
%endmacro

; -------------------------------------------------------------------------
; 256 stubs
; -------------------------------------------------------------------------

; CPU exceptions 0-7 (no error code)
ISR_STUB  0, 0
ISR_STUB  1, 0
ISR_STUB  2, 0
ISR_STUB  3, 0
ISR_STUB  4, 0
ISR_STUB  5, 0
ISR_STUB  6, 0
ISR_STUB  7, 0
; Exception 8 — double fault (error code)
ISR_STUB  8, 1
ISR_STUB  9, 0
; Exceptions 10-14 (error code)
ISR_STUB 10, 1
ISR_STUB 11, 1
ISR_STUB 12, 1
ISR_STUB 13, 1
ISR_STUB 14, 1
; 15-16 no error
ISR_STUB 15, 0
ISR_STUB 16, 0
; 17 alignment check (error code)
ISR_STUB 17, 1
; 18-20 no error
ISR_STUB 18, 0
ISR_STUB 19, 0
ISR_STUB 20, 0
; 21 control protection (error code)
ISR_STUB 21, 1
; 22-31 reserved
ISR_STUB 22, 0
ISR_STUB 23, 0
ISR_STUB 24, 0
ISR_STUB 25, 0
ISR_STUB 26, 0
ISR_STUB 27, 0
ISR_STUB 28, 0
ISR_STUB 29, 0
ISR_STUB 30, 0
ISR_STUB 31, 0

; IRQ vectors 32-47  (PIC remapped: IRQ0-15 → vectors 32-47)
%assign v 32
%rep 16
    ISR_STUB v, 0
    %assign v v+1
%endrep

; Generic stubs 48-255
%assign v 48
%rep 208
    ISR_STUB v, 0
    %assign v v+1
%endrep

; -------------------------------------------------------------------------
; Syscall vector 0x80 — direct dispatch to Syscall_Dispatch(frame, regs)
;
; Saves GPRs in the same order as the page-fault handler so the shared
; SavedRegs struct (rax..r15) matches memory layout.
;
; INT 0x80 does not push an error code; we push a dummy one so the CPU frame
; (RIP/CS/RFLAGS/RSP/SS) aligns with the shared InterruptFrame struct.
;
; The C handler receives:
;   rdi = pointer to SavedRegs (rax slot at the top of saved GPRs)
;   rsi = pointer to InterruptFrame (error_code slot)
; -------------------------------------------------------------------------

extern Syscall_Dispatch

global uaos_syscall_isr
uaos_syscall_isr:
    push    qword 0             ; dummy error code

    push    r15
    push    r14
    push    r13
    push    r12
    push    r11
    push    r10
    push    r9
    push    r8
    push    rbp
    push    rdi
    push    rsi
    push    rdx
    push    rcx
    push    rbx
    push    rax

    ; RDI = &SavedRegs (rax slot), RSI = &InterruptFrame (error_code slot)
    mov     rdi, rsp
    mov     rsi, rsp
    add     rsi, 15 * 8
    call    Syscall_Dispatch

    ; Task switch requested by scheduler?
    mov     rax, [rel Task_SwitchNext]
    test    rax, rax
    jz      .no_switch

    ; Save current RSP into the old task's native_rsp.
    mov     rbx, [rel Task_SwitchPrev]
    mov     [rbx + 136], rsp    ; offset of native_rsp in UaosTask

    ; First-launch handling for X64 ELF64 tasks (same as in isr_common).
    ; See the detailed comments in isr_common above.
    mov     rbx, [rax + 128]    ; type
    cmp     rbx, 2              ; TASK_TYPE_X64
    jne     .normal_switch

    mov     rbx, [rax + 152]    ; native_stack_base
    mov     rcx, [rax + 160]    ; native_stack_size
    add     rcx, rbx            ; native_stack_base + native_stack_size
    mov     rdx, [rax + 136]    ; native_rsp
    cmp     rdx, rbx
    jb      .normal_switch      ; native_rsp < stack_base
    cmp     rdx, rcx
    jae     .normal_switch      ; native_rsp >= stack_top

    ; First launch of an X64 task from a syscall context.
    mov     rsp, [rax + 184]    ; native_initial_rsp (user stack)
    mov     rbx, [rax + 144]    ; native_rip (entry point)
    mov     qword [rel Task_SwitchNext], 0
    mov     qword [rel Task_SwitchPrev], 0
    sti
    jmp     rbx

.normal_switch:
    ; Load new task's RSP and clear switch request.
    mov     rsp, [rax + 136]
    mov     qword [rel Task_SwitchNext], 0
    mov     qword [rel Task_SwitchPrev], 0

.no_switch:
    pop     rax
    pop     rbx
    pop     rcx
    pop     rdx
    pop     rsi
    pop     rdi
    pop     rbp
    pop     r8
    pop     r9
    pop     r10
    pop     r11
    pop     r12
    pop     r13
    pop     r14
    pop     r15
    add     rsp, 8              ; discard dummy error_code
    iretq
