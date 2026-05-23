/* uaos_kernel.ld — UAOS Kernel Linker Script
 *
 * Produces a flat ELF64 binary loaded by GRUB2 via multiboot2.
 * The Multiboot2 header section is placed first so it falls within
 * the first 32 KB of the file (GRUB2 requirement).
 * Load address: 1 MB physical (0x100000) — standard bare-metal target.
 */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS
{
    /* Load the kernel at the 1 MB mark */
    . = 0x100000;

    /* Multiboot2 header must appear first */
    .multiboot2 : ALIGN(8)
    {
        *(.multiboot2)
    }

    /* Executable text */
    .text : ALIGN(0x1000)
    {
        *(.text)
        *(.text.*)
    }

    /* Read-only data */
    .rodata : ALIGN(0x1000)
    {
        *(.rodata)
        *(.rodata.*)
    }

    /* Initialised data */
    .data : ALIGN(0x1000)
    {
        *(.data)
        *(.data.*)
    }

    /* Uninitialised data (BSS) — zeroed by the entry stub */
    .bss : ALIGN(0x1000)
    {
        *(COMMON)
        *(.bss)
        *(.bss.*)
    }

    /* Discard everything else */
    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame)
        *(.note.*)
    }
}
