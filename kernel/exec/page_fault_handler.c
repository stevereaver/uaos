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
#include "chipset/chip_emu.h"
#include "task.h"

/* Forward declarations — defined in kernel/boot/uaos_kernel_main.c */
extern void kprint(const char *s);
extern void kprinthex(uint64_t v);

/* -----------------------------------------------------------------------
 * Amiga hardware register window boundaries
 * ----------------------------------------------------------------------- */

#define CHIP_WINDOW_START  0x00B00000ULL
#define CHIP_WINDOW_END    0x00DFFFFFULL  /* inclusive                       */

static inline int is_chip_address(uint64_t addr)
{
    return (addr >= CHIP_WINDOW_START) && (addr <= CHIP_WINDOW_END);
}

/* -----------------------------------------------------------------------
 * x86_64 instruction decoder
 *
 * Decodes the most common instruction patterns that hit the Amiga chip
 * window from both native x86_64 code and the Musashi M68k interpreter:
 *
 *   MOV  reg, [mem]        8A /r, 8B /r           (read)
 *   MOV  [mem], reg        88 /r, 89 /r           (write)
 *   MOV  [mem], imm        C6 /0, C7 /0           (write)
 *   OR   [mem], reg        08 /r, 09 /r           (RMW)
 *   OR   [mem], imm        80 /1, 81 /1, 83 /1    (RMW)
 *   AND  [mem], reg        20 /r, 21 /r           (RMW)
 *   AND  [mem], imm        80 /4, 81 /4, 83 /4    (RMW)
 *   XOR  [mem], reg        30 /r, 31 /r           (RMW)
 *   XOR  [mem], imm        80 /6, 81 /6, 83 /6    (RMW)
 *
 * The memory operand (ModRM r/m) is the one that faulted; we know its
 * address from CR2.  The register operand (ModRM reg) is decoded against
 * the saved register block to read source values or inject read results.
 * ----------------------------------------------------------------------- */

/* REX prefix bits */
#define REX_W  0x08
#define REX_R  0x04
#define REX_X  0x02
#define REX_B  0x01

/* Instruction categories */
#define INSN_UNKNOWN 0
#define INSN_MOV     1
#define INSN_OR      2
#define INSN_AND     3
#define INSN_XOR     4

/* Direction: memory operand is source or destination */
#define DIR_MEM_SRC  0
#define DIR_MEM_DST  1

/* Decoded instruction description */
typedef struct {
    int  category;       /* INSN_* */
    int  dir;          /* DIR_MEM_* */
    int  width;        /* 1, 2, 4 or 8 */
    int  length;       /* total bytes */
    int  src_reg;      /* ModRM reg field (0-15), or -1 for immediate */
    int  has_imm;      /* 1 if immediate operand present */
    int  imm_off;      /* offset of immediate within instruction */
    int  imm_size;     /* size of immediate in bytes */
} DecodedInsn;

static uint64_t *reg_slot(SavedRegs *regs, int reg)
{
    switch (reg) {
        case  0: return &regs->rax;
        case  1: return &regs->rcx;
        case  2: return &regs->rdx;
        case  3: return &regs->rbx;
        case  5: return &regs->rbp;
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
        default: return NULL; /* RSP (4) is not part of SavedRegs */
    }
}

static uint64_t reg_value(SavedRegs *regs, int reg, int width)
{
    uint64_t *slot = reg_slot(regs, reg);
    if (!slot) return 0;
    uint64_t v = *slot;
    if (width == 1) return v & 0xFFu;
    if (width == 2) return v & 0xFFFFu;
    if (width == 4) return v & 0xFFFFFFFFu;
    return v;
}

/* Sign-extend an N-bit value to 64 bits. */
static uint64_t sign_extend(uint64_t v, int bits)
{
    uint64_t sign = 1ULL << (bits - 1);
    return (v ^ sign) - sign;
}

/* Decode one x86_64 instruction.  Returns 1 if the instruction was one of
 * the supported forms, 0 otherwise. */
static int decode_insn(const uint8_t *ip, DecodedInsn *out)
{
    int pos = 0;
    uint8_t rex = 0;
    int operand_size = 4;  /* default in 64-bit mode (no REX.W) */
    int has_66 = 0;

    /* Parse prefixes: REX, 0x66 operand-size override. */
    for (;;) {
        uint8_t b = ip[pos];
        if ((b & 0xF0) == 0x40) {
            rex = b;
            pos++;
        } else if (b == 0x66) {
            has_66 = 1;
            pos++;
        } else {
            break;
        }
    }

    if (rex & REX_W) operand_size = 8;
    if (has_66)      operand_size = 2;

    uint8_t opcode = ip[pos++];
    int category = INSN_UNKNOWN;
    int dir = DIR_MEM_DST;
    int src_reg = -1;
    int has_imm = 0;
    int imm_size = 0;
    int group_op = -1;

    switch (opcode) {
        /* MOV: [mem], reg / reg, [mem] */
        case 0x88: category = INSN_MOV; operand_size = 1; dir = DIR_MEM_DST; break;
        case 0x8A: category = INSN_MOV; operand_size = 1; dir = DIR_MEM_SRC; break;
        case 0x89: category = INSN_MOV; dir = DIR_MEM_DST; break;
        case 0x8B: category = INSN_MOV; dir = DIR_MEM_SRC; break;

        /* MOV: [mem], imm */
        case 0xC6: category = INSN_MOV; operand_size = 1; dir = DIR_MEM_DST; has_imm = 1; imm_size = 1; break;
        case 0xC7: category = INSN_MOV; dir = DIR_MEM_DST; has_imm = 1; imm_size = (operand_size == 2) ? 2 : 4; break;

        /* OR/AND/XOR: [mem], reg / reg, [mem] */
        case 0x08: category = INSN_OR;  operand_size = 1; dir = DIR_MEM_DST; break;
        case 0x09: category = INSN_OR;  dir = DIR_MEM_DST; break;
        case 0x0A: category = INSN_OR;  operand_size = 1; dir = DIR_MEM_SRC; break;
        case 0x0B: category = INSN_OR;  dir = DIR_MEM_SRC; break;

        case 0x20: category = INSN_AND; operand_size = 1; dir = DIR_MEM_DST; break;
        case 0x21: category = INSN_AND; dir = DIR_MEM_DST; break;
        case 0x22: category = INSN_AND; operand_size = 1; dir = DIR_MEM_SRC; break;
        case 0x23: category = INSN_AND; dir = DIR_MEM_SRC; break;

        case 0x30: category = INSN_XOR; operand_size = 1; dir = DIR_MEM_DST; break;
        case 0x31: category = INSN_XOR; dir = DIR_MEM_DST; break;
        case 0x32: category = INSN_XOR; operand_size = 1; dir = DIR_MEM_SRC; break;
        case 0x33: category = INSN_XOR; dir = DIR_MEM_SRC; break;

        /* Group 1: immediate ALU */
        case 0x80: has_imm = 1; imm_size = 1; group_op = (ip[pos] >> 3) & 7; break;
        case 0x81: has_imm = 1; imm_size = (operand_size == 2) ? 2 : 4; group_op = (ip[pos] >> 3) & 7; break;
        case 0x83: has_imm = 1; imm_size = 1; group_op = (ip[pos] >> 3) & 7; break;

        default:
            return 0;
    }

    if (group_op >= 0) {
        switch (group_op) {
            case 1: category = INSN_OR;  break;
            case 4: category = INSN_AND; break;
            case 6: category = INSN_XOR; break;
            default: return 0; /* ADD/SUB/ADC/SBB/CMP not supported */
        }
        dir = DIR_MEM_DST;
    }

    if (category == INSN_UNKNOWN)
        return 0;

    /* Parse ModRM. */
    uint8_t modrm = ip[pos++];
    uint8_t mod   = (modrm >> 6) & 0x3;
    uint8_t reg   = (modrm >> 3) & 0x7;
    uint8_t rm    = (modrm >> 0) & 0x7;

    if (!has_imm)
        src_reg = reg | ((rex & REX_R) ? 8 : 0);

    /* SIB byte */
    if (mod != 3 && rm == 4) {
        /* SIB: base, index, scale.  We don't need the values, just skip it. */
        pos++;
    }

    /* Displacement */
    if (mod == 0 && rm == 5) {
        pos += 4; /* RIP-relative or disp32 */
    } else if (mod == 1) {
        pos += 1;
    } else if (mod == 2) {
        pos += 4;
    }

    /* Immediate follows for group instructions and MOV [mem], imm.
     * The ModRM reg field for C6/C7 must be 0 (MOV). */
    if (has_imm) {
        if ((opcode == 0xC6 || opcode == 0xC7) && reg != 0)
            return 0;
        out->imm_off = pos;
        out->imm_size = imm_size;
        pos += imm_size;
    }

    out->category = category;
    out->dir = dir;
    out->width = operand_size;
    out->length = pos;
    out->src_reg = src_reg;
    out->has_imm = has_imm;
    return 1;
}

static uint64_t decode_immediate(const uint8_t *ip, int off, int size)
{
    if (size == 1) return ip[off];
    if (size == 2) return (uint16_t)(ip[off] | (ip[off + 1] << 8));
    uint32_t v = (uint32_t)(ip[off] | (ip[off + 1] << 8) |
                            (ip[off + 2] << 16) | (ip[off + 3] << 24));
    return v;
}

/* -----------------------------------------------------------------------
 * UAOS_PageFaultHandler — C body called from the ISR assembly wrapper
 * ----------------------------------------------------------------------- */

void UAOS_PageFaultHandler(InterruptFrame *frame, SavedRegs *regs)
{
    uint64_t fault_addr;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(fault_addr));

    if (!is_chip_address(fault_addr)) {
        /* Non-chip page fault.
         *
         * If the faulting task is an X64 userspace task, kill it gracefully
         * instead of halting the entire system.  This prevents a single
         * buggy userspace command from locking up the OS.
         *
         * For kernel-mode faults (no current task, or current task is not
         * an X64 task), treat it as a fatal kernel panic. */
        UaosTask *cur = Task_Current();
        if (cur && cur->type == TASK_TYPE_X64) {
            kprint("[PF] page fault in X64 task '");
            kprint(cur->ln_Name ? cur->ln_Name : "(null)");
            kprint("' at rip=");
            kprinthex(frame->rip);
            kprint(" fault_addr=");
            kprinthex(fault_addr);
            kprint(" — killing task\n");
            Task_Exit();
            __builtin_unreachable();
        }
        /* Kernel-mode fault — panic */
        kprint("[PF] kernel page fault at rip=");
        kprinthex(frame->rip);
        kprint(" fault_addr=");
        kprinthex(fault_addr);
        kprint("\n");
        __asm__ volatile ("cli; hlt");
        __builtin_unreachable();
    }

    uint32_t chip_offset = (uint32_t)(fault_addr - CHIP_WINDOW_START);
    const uint8_t *ip = (const uint8_t *)(uintptr_t)frame->rip;

    DecodedInsn insn;
    if (!decode_insn(ip, &insn)) {
        /* Unknown instruction: emulate a harmless 32-bit read/write so we
         * can still advance past it without crashing. */
        int is_write = (frame->error_code & 0x2) != 0;
        if (is_write) {
            chip_emu_write(chip_offset, (uint32_t)regs->rax, 4);
        } else {
            uint64_t *dst = reg_slot(regs, 0);
            if (dst) *dst = chip_emu_read(chip_offset, 4);
        }
        frame->rip += 4;
        return;
    }

    if (insn.dir == DIR_MEM_SRC) {
        /* Memory read: load value from chip emulator and inject into dest register. */
        uint64_t value = chip_emu_read(chip_offset, insn.width);
        uint64_t *dst = reg_slot(regs, insn.src_reg);
        if (dst) {
            /* Preserve upper bits for partial-width writes to 64-bit registers. */
            if (insn.width == 1) *dst = (*dst & ~0xFFULL) | (value & 0xFFULL);
            else if (insn.width == 2) *dst = (*dst & ~0xFFFFULL) | (value & 0xFFFFULL);
            else if (insn.width == 4) *dst = (uint32_t)value;
            else *dst = value;
        }
    } else {
        /* Memory write or RMW: compute value and write back. */
        uint64_t value;
        if (insn.has_imm) {
            value = decode_immediate(ip, insn.imm_off, insn.imm_size);
            if (insn.imm_size == 1 && insn.width > 1)
                value = sign_extend(value, 8); /* 0x83 sign-extended immediate */
        } else if (insn.src_reg >= 0) {
            value = reg_value(regs, insn.src_reg, insn.width);
        } else {
            value = 0;
        }

        if (insn.category == INSN_MOV) {
            chip_emu_write(chip_offset, (uint32_t)value, insn.width);
        } else {
            /* Read-modify-write: read current, apply op, write back. */
            uint64_t cur = chip_emu_read(chip_offset, insn.width);
            uint64_t mask;
            if (insn.width == 8) mask = ~0ULL;
            else mask = (1ULL << (insn.width * 8)) - 1;

            switch (insn.category) {
                case INSN_OR:  value = (cur | value) & mask; break;
                case INSN_AND: value = (cur & value) & mask; break;
                case INSN_XOR: value = (cur ^ value) & mask; break;
                default: value = cur; break;
            }
            chip_emu_write(chip_offset, (uint32_t)value, insn.width);
        }
    }

    /* Advance RIP past the faulting instruction. */
    frame->rip += (uint64_t)insn.length;
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
