/* system_reboot.h — shared warm-reboot routine
 *
 * Used by C:reboot (cmd_reboot.c) and the Workbench ▸ Quit menu action.
 * Resets the CPU via the keyboard controller (8042 command 0xFE).
 */

#ifndef UAOS_SYSTEM_REBOOT_H
#define UAOS_SYSTEM_REBOOT_H

static inline void System_Reboot(void)
{
    __asm__ volatile (
        "1: inb  $0x64, %%al\n"
        "   testb $0x02, %%al\n"
        "   jnz 1b\n"
        "   movb $0xFE, %%al\n"
        "   outb %%al, $0x64\n"
        :: : "eax"
    );
    for (;;) __asm__ volatile ("hlt");
}

#endif /* UAOS_SYSTEM_REBOOT_H */
