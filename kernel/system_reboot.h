/* system_reboot.h — shared warm-reboot routine
 *
 * Used by C:reboot (cmd_reboot.c) and the Workbench ▸ Quit menu action.
 * Resets the CPU via the keyboard controller (8042 command 0xFE).
 */

#ifndef UAOS_SYSTEM_REBOOT_H
#define UAOS_SYSTEM_REBOOT_H

static inline void System_Reboot(void)
{
    /* Interrupts off first — an IRQ landing mid-sequence would vector
     * the CPU away (seen in the wild: IRQ42 parked the reboot task
     * before the 0xCF9 write ever executed). */
    __asm__ volatile ("cli");

    /* 1. ACPI/PCI reset control (0xCF9) — the reliable path on q35/PCI
     *    chipsets.  0x0E = SYS_RST + CPU_RST + full reset.
     *    NOTE: ports above 0xFF must go through %dx — the immediate
     *    `out` form silently truncates the port to a byte (0xCF9 -> 0xF9). */
    __asm__ volatile (
        "movw $0xCF9, %%dx\n"
        "movb $0x0E, %%al\n"
        "outb %%al, %%dx\n"
        :: : "eax", "edx"
    );

    /* 2. Keyboard controller pulse (8042 command 0xFE) — for chipsets
     *    without 0xCF9.  Don't wait forever on IBF: under TCG/emulation
     *    the status bit can stay set, which used to hang the reboot. */
    __asm__ volatile (
        "movl $0x100000, %%ecx\n"
        "1: inb  $0x64, %%al\n"
        "   testb $0x02, %%al\n"
        "   jz 2f\n"
        "   loop 1b\n"
        "2: movb $0xFE, %%al\n"
        "   outb %%al, $0x64\n"
        :: : "eax", "ecx"
    );

    /* 3. Last resort: triple fault via an empty IDT + a guaranteed
     *    faulting instruction.  Built in C so the descriptor is a
     *    plain memory operand (no asm stack tricks). */
    {
        struct __attribute__((packed)) {
            uint16_t limit;
            uint64_t base;
        } idt_zero = { 0, 0 };
        __asm__ volatile ("lidt %0" :: "m"(idt_zero) : "memory");
        __asm__ volatile ("ud2");
    }

    for (;;) __asm__ volatile ("hlt");
}

#endif /* UAOS_SYSTEM_REBOOT_H */
