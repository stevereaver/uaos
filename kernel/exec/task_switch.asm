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
    mov     rdi, rsi            ; first argument (SysV ABI: RDI)
    mov     r11, r8             ; entry point (keep in callee-saved reg)
    sti
    call    r11
    ; Task returned — should not happen for Phase 1, but halt just in case.
    cli
    hlt
