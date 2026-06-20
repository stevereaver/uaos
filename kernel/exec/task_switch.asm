; task_switch.asm — UAOS x86_64 context switch
;
; Called with interrupts disabled from timer ISR to switch between
; native x86_64 tasks.  Both tasks have a synthetic interrupt frame
; built by Task_CreateNative in task.c.
;
; Stack frame layout (built by Task_CreateNative, ascending addresses):
;   [native_rsp+0]    = RAX   (popped last)
;   [native_rsp+8]    = RBX
;   [native_rsp+16]   = RCX
;   [native_rsp+24]   = RDX
;   [native_rsp+32]   = RSI
;   [native_rsp+40]   = RDI
;   [native_rsp+48]   = RBP
;   [native_rsp+56]   = R8
;   [native_rsp+64]   = R9
;   [native_rsp+72]   = R10
;   [native_rsp+80]   = R11
;   [native_rsp+88]   = R12
;   [native_rsp+96]   = R13
;   [native_rsp+104]  = R14
;   [native_rsp+112]  = R15   (popped first)
;   [native_rsp+120]  = error_code
;   [native_rsp+128]  = vector
;   [native_rsp+136]  = RFLAGS
;   [native_rsp+144]  = CS
;   [native_rsp+152]  = RIP   (entry point)
;
; void Task_SwitchContext(UaosTask *old_task, UaosTask *new_task)
;   SysV ABI: RDI = old_task, RSI = new_task
;
; native_rsp offset within UaosTask = 136 (see task.h layout)

bits 64
section .text

global Task_SwitchContext
global Task_RunNew
global Task_RunNewX64

Task_SwitchContext:
    ; Save current RSP into old_task->native_rsp
    mov     rax, rsp
    mov     [rdi + 136], rax        ; offset of native_rsp

    ; Load new_task->native_rsp
    mov     rsp, [rsi + 136]

    ; Execute isr_common epilogue on new_task's stack.
    ; Pop order must match isr_common exactly: R15 first, RAX last.
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
    add     rsp, 16             ; discard error_code + vector
    iretq                       ; pop RFLAGS, CS, RIP

Task_RunNew:
    ; Start the very first task.  Called with interrupts off.
    ; SysV ABI: RDI = task
    ; We simply set RSP to the top of the task's stack and call the entry
    ; function.  The timer ISR will create the proper interrupt frame for
    ; future context switches.
    mov     r8,  [rdi + 168]    ; task->native_entry
    mov     rsi, [rdi + 176]    ; task->native_arg
    mov     rcx, [rdi + 152]    ; task->native_stack_base
    mov     eax, [rdi + 160]    ; task->native_stack_size
    mov     rsp, rcx
    add     rsp, rax            ; rsp = stack top
    and     rsp, -16            ; align to 16 bytes before call (SysV ABI)
    mov     rdi, rsi            ; first argument (SysV ABI: RDI)
    mov     r11, r8             ; entry point (keep in callee-saved reg)
    sti
    call    r11
    ; Task returned — should not happen for Phase 1, but halt just in case.
    cli
    hlt

Task_RunNewX64:
    ; First launch of an ELF64-loaded task (ring-0 → ring-3 transition).
    ; RDI = task (already set by Task_RunNew, and native_arg == task)
    ;
    ; We build an iretq frame on the current (kernel) stack and execute iretq,
    ; which atomically loads CS/RIP/RFLAGS/SS/RSP and switches to ring 3.
    ;
    ; iretq frame layout (pushed in reverse order, high → low addresses):
    ;   SS      (user data selector = 0x23)
    ;   RSP     (user stack top)
    ;   RFLAGS  (IF=1)
    ;   CS      (user code selector = 0x1B)
    ;   RIP     (ELF entry point)
    ;
    mov     rax, [rdi + 144]    ; task->native_rip  (ELF entry point)
    mov     rcx, [rdi + 184]    ; task->native_initial_rsp

    ; Build iretq frame on the current kernel stack
    push    qword 0x23          ; SS  — user data (0x20 | RPL=3)
    push    rcx                 ; RSP — user stack top
    pushfq
    or      qword [rsp], (1 << 9)   ; ensure IF=1
    push    qword 0x1B          ; CS  — user code (0x18 | RPL=3)
    push    rax                 ; RIP — ELF entry point

    ; Zero most GPRs so userspace starts with a clean state
    xor     rax, rax
    xor     rbx, rbx
    xor     rcx, rcx
    xor     rdx, rdx
    xor     rsi, rsi
    xor     rdi, rdi
    xor     rbp, rbp
    xor     r8,  r8
    xor     r9,  r9
    xor     r10, r10
    xor     r11, r11
    xor     r12, r12
    xor     r13, r13
    xor     r14, r14
    xor     r15, r15

    iretq
    ; Should never reach here
    cli
    hlt
