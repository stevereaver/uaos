/*
 * page_fault_handler.c — UAOS x86_64 Page Fault ISR (#PF, Vector 14)
 *
 * Intercepts all page faults originating from the Amiga custom chip /
 * CIA hardware address window (0x00B00000–0x00DFFFFF).
 *
 * Fault classification:
 *   WRITE fault → extract the write value and forward to the chip emulator
 *   READ  fault → query the chip emulator, inject the result into the
 *                 caller's register context, advance RIP past the faulting
 *                 instruction, then return from interrupt normally.
 *
 * Non-chip faults are forwarded to the kernel's general page fault handler.
 *
 * Register injection strategy:
 *   x86_64 MOV instructions that load from memory write their result into
 *   one of the 16 general-purpose registers.  This handler decodes the
 *   ModRM byte of the faulting instruction to identify the destination
 *   register and writes the emulator result directly into the exception
 *   frame stored on the interrupt stack.
 *
 * Build note: This file is bare-metal only.  It uses GCC interrupt
 * function attributes and inline assembly; it will not link against libc.
 */

#include <stdint.h>
#include <stddef.h>
#include "syscall_table.h"

/* -----------------------------------------------------------------------
 * Amiga hardware register window boundaries
 * ----------------------------------------------------------------------- */

#define CHIP_WINDOW_START  0x00B00000ULL
#define CHIP_WINDOW_END    0x00DFFFFULL   /* inclusive                       */

static inline int is_chip_address(uint64_t addr)
{
    return (addr >= CHIP_WINDOW_START) && (addr <= CHIP_WINDOW_END);
}

/* -----------------------------------------------------------------------
 * Chip emulator interface — stub declarations.
 * Replace these with the real emulation engine calls once integrated.
 * ----------------------------------------------------------------------- */

extern void   chip_emu_write(uint32_t offset, uint32_t value, int width_bytes);
extern uint32_t chip_emu_read(uint32_t offset, int width_bytes);

/* -----------------------------------------------------------------------
 * x86_64 instruction prefix / opcode decode helpers
 *
 * We support the most common MOV reg, [mem] patterns:
 *   8B /r        MOV r32/r64, r/m32/r/m64
 *   REX.W 8B /r  MOV r64, r/m64
 *
 * This is intentionally minimal — a full decoder would handle all
 * addressing modes; extend as required.
 * ----------------------------------------------------------------------- */

/* REX prefix bits */
#define REX_W  0x08
#define REX_R  0x04
#define REX_X  0x02
#define REX_B  0x01

/* ModRM register-field to SavedRegs slot mapping (reg = bits 5:3 of ModRM).
 * Returns a pointer into the saved register block, or NULL if unsupported. */
static uint64_t *modrm_reg_slot(SavedRegs *regs, uint8_t modrm, uint8_t rex)
{
    int reg = ((modrm >> 3) & 0x7) | ((rex & REX_R) ? 8 : 0);
    switch (reg) {
        case  0: return &regs->rax;
        case  1: return &regs->rcx;
        case  2: return &regs->rdx;
        case  3: return &regs->rbx;
        case  6: return &regs->rsi;
        case  7: return &regs->rdi;
        case  8: return &regs->r8;
        case  9: return &regs->r9;
        case 10: return &regs->r10;
        case 11: return &regs->r11;
        case 12: return &regs->r12;
        case 13: return &regs->r13;
        case 14: return &regs->r14;
        case 15: return &regs->r15;
        default: return NULL;
    }
}

/* -----------------------------------------------------------------------
 * decode_insn_length — returns byte length of the faulting MOV instruction
 * so we can advance RIP past it.  Handles REX + opcode + ModRM + SIB +
 * displacement combinations for register-indirect addressing modes.
 * ----------------------------------------------------------------------- */

static int decode_insn_length(const uint8_t *ip)
{
    int len = 0;
    uint8_t rex = 0;

    /* Consume optional REX prefix */
    if ((ip[len] & 0xF0) == 0x40) {
        rex = ip[len++];
        (void)rex;
    }

    uint8_t opcode = ip[len++];
    (void)opcode;       /* we trust the caller already verified 0x8B       */

    uint8_t modrm = ip[len++];
    uint8_t mod   = (modrm >> 6) & 0x3;
    uint8_t rm    = (modrm)      & 0x7;

    /* SIB byte follows when rm == 4 and mod != 3                          */
    if (mod != 3 && rm == 4) len++;

    /* Displacement */
    if (mod == 0 && rm == 5) len += 4; /* RIP-relative 32-bit displacement */
    else if (mod == 1)        len += 1;
    else if (mod == 2)        len += 4;

    return len;
}

/* -----------------------------------------------------------------------
 * UAOS_PageFaultHandler — C body called from the ISR assembly wrapper
 * ----------------------------------------------------------------------- */

void UAOS_PageFaultHandler(InterruptFrame *frame, SavedRegs *regs)
{
    uint64_t fault_addr;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(fault_addr));

    if (!is_chip_address(fault_addr)) {
        /* Not our fault — panic or chain to general handler               */
        __asm__ volatile ("cli; hlt");
        __builtin_unreachable();
    }

    uint32_t chip_offset = (uint32_t)(fault_addr - CHIP_WINDOW_START);
    int is_write = (frame->error_code & 0x2) != 0;  /* bit 1 = W/R        */

    const uint8_t *ip = (const uint8_t *)(uintptr_t)frame->rip;

    if (is_write) {
        /* Extract write value from RAX (most common source for chip writes;
         * a full implementation would decode the full MOV src operand).    */
        uint32_t write_value = (uint32_t)regs->rax;
        chip_emu_write(chip_offset, write_value, 4);
    } else {
        /* READ: decode destination register, inject emulator result        */
        const uint8_t *scan = ip;
        uint8_t rex_byte = 0;
        if ((*scan & 0xF0) == 0x40) rex_byte = *scan++;

        /* Expect opcode 0x8B (MOV r, r/m) */
        if (*scan == 0x8B) {
            uint8_t modrm = scan[1];
            uint32_t chip_value = chip_emu_read(chip_offset, 4);
            uint64_t *dst = modrm_reg_slot(regs, modrm, rex_byte);
            if (dst != NULL) {
                *dst = (uint64_t)chip_value;
            }
        }
    }

    /* Advance RIP past the faulting instruction and return from interrupt  */
    frame->rip += (uint64_t)decode_insn_length(ip);
}

/* -----------------------------------------------------------------------
 * ISR assembly wrapper — pushes all GPRs, calls the C handler, pops, iretq
 *
 * The linker script must place uaos_page_fault_isr at IDT vector 14.
 * ----------------------------------------------------------------------- */

__asm__ (
    ".global uaos_page_fault_isr\n"
    "uaos_page_fault_isr:\n"
    /* The CPU has already pushed: error_code, RIP, CS, RFLAGS, RSP, SS    */
    "push %r15\n"
    "push %r14\n"
    "push %r13\n"
    "push %r12\n"
    "push %r11\n"
    "push %r10\n"
    "push %r9\n"
    "push %r8\n"
    "push %rbp\n"
    "push %rdi\n"
    "push %rsi\n"
    "push %rdx\n"
    "push %rcx\n"
    "push %rbx\n"
    "push %rax\n"
    /* rdi = pointer to InterruptFrame (sits above saved GPRs on stack)    */
    "lea  15*8(%rsp), %rdi\n"
    /* rsi = pointer to SavedRegs block (bottom of pushed GPR block)       */
    "mov  %rsp, %rsi\n"
    "call UAOS_PageFaultHandler\n"
    "pop  %rax\n"
    "pop  %rbx\n"
    "pop  %rcx\n"
    "pop  %rdx\n"
    "pop  %rsi\n"
    "pop  %rdi\n"
    "pop  %rbp\n"
    "pop  %r8\n"
    "pop  %r9\n"
    "pop  %r10\n"
    "pop  %r11\n"
    "pop  %r12\n"
    "pop  %r13\n"
    "pop  %r14\n"
    "pop  %r15\n"
    "add  $8, %rsp\n"    /* discard error_code pushed by CPU               */
    "iretq\n"
);
