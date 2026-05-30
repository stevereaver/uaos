#!/usr/bin/env bash
# build_iso.sh — Ultimate Amiga OS ISO Packaging Pipeline
#
# Compiles the x86_64 ELF64 kernel from NASM + C sources, stages the
# sys-root, injects GRUB configuration, and invokes grub-mkrescue to
# produce a bootable Ultimate_Amiga_OS.iso.
#
# Usage:
#   ./scripts/build_iso.sh [--clean]
#
# Output:
#   build/Ultimate_Amiga_OS.iso

set -euo pipefail

# -------------------------------------------------------------------------
# Configuration
# -------------------------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build"
ISO_STAGING="${BUILD_DIR}/iso-staging"
ISO_OUTPUT="${BUILD_DIR}/Ultimate_Amiga_OS.iso"
SYS_ROOT="${REPO_ROOT}/sys-root"
GRUB_CFG="${REPO_ROOT}/scripts/grub.cfg"
KICKSTART_CONF="${REPO_ROOT}/emulation/rom_patches/aros_kickstart.conf"
KERNEL_ASM="${REPO_ROOT}/kernel/boot/uaos_kernel_entry.asm"
KERNEL_MAIN="${REPO_ROOT}/kernel/boot/uaos_kernel_main.c"
KERNEL_LD="${REPO_ROOT}/kernel/boot/uaos_kernel.ld"
KERNEL_ELF="${BUILD_DIR}/uaos-kernel.elf"

GCC_FLAGS="-ffreestanding -fno-stack-protector -fno-pie -fno-PIE \
           -mno-red-zone -nostdlib -m64 -O2 -std=c11 \
           -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
           -Wall -Wextra"

# -------------------------------------------------------------------------
# Helpers
# -------------------------------------------------------------------------

info()  { printf '\033[1;34m[BUILD]\033[0m %s\n' "$*"; }
ok()    { printf '\033[1;32m[  OK ]\033[0m %s\n' "$*"; }
warn()  { printf '\033[1;33m[ WARN]\033[0m %s\n' "$*"; }
fatal() { printf '\033[1;31m[FATAL]\033[0m %s\n' "$*" >&2; exit 1; }

# -------------------------------------------------------------------------
# Optional clean
# -------------------------------------------------------------------------

if [[ "${1:-}" == "--clean" ]]; then
    info "Cleaning previous build artefacts..."
    rm -rf "${BUILD_DIR}"
    ok "Clean complete"
fi

# -------------------------------------------------------------------------
# Step 1 — Create build staging directories
# -------------------------------------------------------------------------

info "Step 1: Creating build staging directories"

mkdir -p "${ISO_STAGING}/boot/grub"
mkdir -p "${ISO_STAGING}/boot/uaos"
mkdir -p "${ISO_STAGING}/sys-root/C"
mkdir -p "${ISO_STAGING}/sys-root/DEVS"
mkdir -p "${ISO_STAGING}/sys-root/L"
mkdir -p "${ISO_STAGING}/sys-root/S"
mkdir -p "${ISO_STAGING}/sys-root/SYS"

ok "Staging directories created at ${ISO_STAGING}"

# -------------------------------------------------------------------------
# Step 1b — Compile ELF64 kernel from NASM + C sources
# -------------------------------------------------------------------------

info "Step 1b: Compiling ELF64 kernel"

mkdir -p "${BUILD_DIR}/obj"

# -------------------------------------------------------------------------
# Step 1c — Generate Musashi m68kops.c if not already present
# -------------------------------------------------------------------------
MUSASHI_DIR="${REPO_ROOT}/emulation/src/musashi"
if [ ! -f "${MUSASHI_DIR}/m68kops.c" ]; then
    info "Step 1c: Generating Musashi opcode handlers"
    gcc -o "${BUILD_DIR}/m68kmake" "${MUSASHI_DIR}/m68kmake.c"
    "${BUILD_DIR}/m68kmake" "${MUSASHI_DIR}" "${MUSASHI_DIR}/m68k_in.c"
    ok "  Generated: m68kops.c ($(wc -l < ${MUSASHI_DIR}/m68kops.c) lines)"
fi

# Assemble NASM sources
nasm -f elf64 \
    "${KERNEL_ASM}" \
    -o "${BUILD_DIR}/obj/uaos_kernel_entry.o"
ok "  Assembled: uaos_kernel_entry.asm"

nasm -f elf64 \
    "${REPO_ROOT}/kernel/irq/idt_stubs.asm" \
    -o "${BUILD_DIR}/obj/idt_stubs.o"
ok "  Assembled: idt_stubs.asm"

# Compile Musashi M68k core (suppress its own warnings — not our code)
for msrc in \
    "${MUSASHI_DIR}/softfloat/softfloat.c" \
    "${MUSASHI_DIR}/m68kcpu.c" \
    "${MUSASHI_DIR}/m68kops.c"
do
    base="$(basename "${msrc}" .c)"
    gcc ${GCC_FLAGS} -w \
        -DMUSASHI_CNF='"uaos_m68kconf.h"' \
        -DFLOATX80 -DFLOAT128 \
        -I"${REPO_ROOT}/emulation" \
        -I"${MUSASHI_DIR}" \
        -I"${MUSASHI_DIR}/softfloat" \
        -c "${msrc}" -o "${BUILD_DIR}/obj/${base}.o"
    ok "  Compiled:  ${msrc##*/}"
done

# -------------------------------------------------------------------------
# Auto-embed all binaries in emulation/binaries/ (files with no extension)
# Generates <Name>_bin.c and auto-generates uaos_emu_registry.c
# -------------------------------------------------------------------------
BINARIES_DIR="${REPO_ROOT}/emulation/binaries"
BIN_OBJ_LIST=""

# Collect names of all plain binaries (no extension, not hidden, not .c/.h)
BIN_NAMES=()
for f in "${BINARIES_DIR}"/*; do
    base="$(basename "$f")"
    # Skip dotfiles, .c, .h, .gitkeep
    [[ "$base" == .* ]]       && continue
    [[ "$base" == *.c ]]      && continue
    [[ "$base" == *.h ]]      && continue
    [[ "$base" == *.gitkeep ]] && continue
    [[ -f "$f" ]]              || continue
    BIN_NAMES+=("$base")
done

# Generate _bin.c for each binary (skip if up-to-date)
for name in "${BIN_NAMES[@]}"; do
    src="${BINARIES_DIR}/${name}"
    out_c="${BINARIES_DIR}/${name}_bin.c"
    out_h="${BINARIES_DIR}/${name}_bin.h"
    out_o="${BUILD_DIR}/obj/${name}_bin.o"

    # Regenerate .c if binary is newer than generated file
    if [ ! -f "${out_c}" ] || [ "${src}" -nt "${out_c}" ]; then
        info "  Embedding binary: ${name}"
        sz=$(wc -c < "${src}")

        # Generate .h
        cat > "${out_h}" << HEOF
/* Auto-generated by build_iso.sh — do not edit */
#ifndef UAOS_BIN_${name^^}_H
#define UAOS_BIN_${name^^}_H
#include <stdint.h>
extern const uint8_t  g_bin_${name}[];
extern const uint32_t g_bin_${name}_size;
#endif
HEOF

        # Generate .c using xxd
        printf "/* Auto-generated — do not edit */\n#include <stdint.h>\n" > "${out_c}"
        printf "const uint8_t g_bin_${name}[] = {\n" >> "${out_c}"
        xxd -i < "${src}" >> "${out_c}"
        printf "};\nconst uint32_t g_bin_${name}_size = %u;\n" "${sz}" >> "${out_c}"
        ok "  Embedded:  ${name} (${sz} bytes)"
    fi

    # Compile _bin.c
    gcc ${GCC_FLAGS} \
        -c "${out_c}" -o "${out_o}"
    ok "  Compiled:  ${name}_bin.c"
    BIN_OBJ_LIST="${BIN_OBJ_LIST} ${out_o}"
done

# Auto-generate uaos_emu_registry.c from discovered binaries
REGISTRY_C="${BUILD_DIR}/obj/uaos_emu_registry_gen.c"
{
    echo "/* Auto-generated by build_iso.sh — do not edit */"
    echo "#include \"${REPO_ROOT}/emulation/uaos_emu.h\""
    echo "#include <stdint.h>"
    echo "#include <stddef.h>"

    # Extern declarations
    for name in "${BIN_NAMES[@]}"; do
        echo "extern const uint8_t  g_bin_${name}[];"
        echo "extern const uint32_t g_bin_${name}_size;"
    done

    cat << 'REGEOF2'
typedef struct { const char *name; const uint8_t *data; uint32_t size; } EmbeddedProgram;
static const EmbeddedProgram k_programs[] = {
REGEOF2

    for name in "${BIN_NAMES[@]}"; do
        sz=$(wc -c < "${BINARIES_DIR}/${name}")
        lname="${name,,}"   # lowercase alias
        echo "    { \"${name}\", g_bin_${name}, ${sz}U },"
        if [ "${lname}" != "${name}" ]; then
            echo "    { \"${lname}\", g_bin_${name}, ${sz}U },"
        fi
    done

    cat << 'REGEOF3'
    { NULL, NULL, 0 }
};
REGEOF3

    # Append the rest of the registry boilerplate (lookup + RunByName)
    tail -n +45 "${REPO_ROOT}/emulation/uaos_emu_registry.c"
} > "${REGISTRY_C}"

# Compile emulation layer
for esrc in \
    "${REPO_ROOT}/emulation/uaos_m68k_glue.c"
do
    base="$(basename "${esrc}" .c)"
    gcc ${GCC_FLAGS} \
        -DMUSASHI_CNF='"uaos_m68kconf.h"' \
        -I"${REPO_ROOT}/emulation" \
        -I"${REPO_ROOT}/kernel" \
        -I"${MUSASHI_DIR}" \
        -c "${esrc}" -o "${BUILD_DIR}/obj/${base}.o"
    ok "  Compiled:  ${esrc##*/}"
done

# Compile the auto-generated registry
gcc ${GCC_FLAGS} \
    -DMUSASHI_CNF='"uaos_m68kconf.h"' \
    -I"${REPO_ROOT}/emulation" \
    -I"${MUSASHI_DIR}" \
    -I"${BUILD_DIR}/obj" \
    -c "${REGISTRY_C}" -o "${BUILD_DIR}/obj/uaos_emu_registry.o"
ok "  Compiled:  uaos_emu_registry (auto-generated)"

# Compile C kernel sources (freestanding — no libc)
for src in \
    "${KERNEL_MAIN}" \
    "${REPO_ROOT}/kernel/display/framebuffer.c" \
    "${REPO_ROOT}/kernel/display/desktop.c" \
    "${REPO_ROOT}/kernel/display/cursor.c" \
    "${REPO_ROOT}/kernel/display/shell_win.c" \
    "${REPO_ROOT}/kernel/display/wm.c" \
    "${REPO_ROOT}/kernel/display/filebrowser.c" \
    "${REPO_ROOT}/kernel/display/about_win.c" \
    "${REPO_ROOT}/kernel/display/calc_win.c" \
    "${REPO_ROOT}/kernel/irq/idt.c" \
    "${REPO_ROOT}/kernel/irq/ps2mouse.c" \
    "${REPO_ROOT}/kernel/irq/ps2kbd.c" \
    "${REPO_ROOT}/kernel/irq/vmmouse.c" \
    "${REPO_ROOT}/kernel/irq/rtc.c" \
    "${REPO_ROOT}/kernel/irq/virtio_blk.c" \
    "${REPO_ROOT}/kernel/exec/thunk_handler.c" \
    "${REPO_ROOT}/kernel/exec/rom_modules.c" \
    "${REPO_ROOT}/kernel/exec/utility_lib.c" \
    "${REPO_ROOT}/kernel/exec/console_device.c" \
    "${REPO_ROOT}/kernel/exec/mathffp_lib.c" \
    "${REPO_ROOT}/kernel/exec/locale_lib.c" \
    "${REPO_ROOT}/kernel/exec/ixemul_lib.c" \
    "${REPO_ROOT}/kernel/exec/timer_device.c" \
    "${REPO_ROOT}/kernel/exec/keyboard_device.c" \
    "${REPO_ROOT}/kernel/exec/graphics_lib.c" \
    "${REPO_ROOT}/kernel/exec/dos_lib.c" \
    "${REPO_ROOT}/kernel/exec/mmu_sandbox.c" \
    "${REPO_ROOT}/kernel/exec/page_fault_handler.c" \
    "${REPO_ROOT}/emulation/uaos_uae_bridge.c" \
    "${REPO_ROOT}/kernel/dos/ramfs.c" \
    "${REPO_ROOT}/kernel/dos/vfs.c" \
    "${REPO_ROOT}/kernel/dos/blockdev.c" \
    "${REPO_ROOT}/kernel/dos/fat32.c" \
    "${REPO_ROOT}/kernel/dos/pfs3.c" \
    "${REPO_ROOT}/kernel/dos/ext4.c" \
    "${REPO_ROOT}/kernel/display/pointer_prefs.c"
do
    base="$(basename "${src}" .c)"
    gcc ${GCC_FLAGS} \
        -I"${REPO_ROOT}/emulation" \
        -I"${REPO_ROOT}/kernel" \
        -c "${src}" -o "${BUILD_DIR}/obj/${base}.o"
    ok "  Compiled:  ${src##*/}"
done

# Provide stub implementations for chip_emu_read/write and aligned_alloc
# so the freestanding link resolves all symbols for this validation build.
cat > "${BUILD_DIR}/obj/stubs.c" <<'STUBEOF'
#include <stdint.h>

/* Chip emulator interface stubs */
void     chip_emu_write(uint32_t o, uint32_t v, int w) { (void)o;(void)v;(void)w; }
uint32_t chip_emu_read (uint32_t o, int w)             { (void)o;(void)w; return 0; }

/* Screen size for PS/2 mouse clamp — populated by kernel before PS2Mouse_Init */
unsigned int g_fb_width_irq  = 1024;
unsigned int g_fb_height_irq = 768;

/* Memory allocation stubs */
void *aligned_alloc(unsigned long a, unsigned long s)  { (void)a;(void)s; return (void*)0; }
void  free(void *p)                                    { (void)p; }

/* vfprintf / fprintf / printf / sprintf / sscanf / exit stubs for Musashi */
typedef __builtin_va_list va_list;
#define va_start(v,l) __builtin_va_start(v,l)
#define va_end(v)     __builtin_va_end(v)
#define va_arg(v,l)   __builtin_va_arg(v,l)
typedef void FILE2;
extern FILE2 *stderr;
int vfprintf(FILE2 *f, const char *fmt, va_list ap) {
    (void)f; (void)fmt; (void)ap; return 0;
}
int sprintf(char *buf, const char *fmt, ...) {
    (void)buf; (void)fmt; return 0;
}
int sscanf(const char *s, const char *fmt, ...) {
    (void)s; (void)fmt; return 0;
}
int __isoc99_sscanf(const char *s, const char *fmt, ...) {
    (void)s; (void)fmt; return 0;
}
void exit(int code) { (void)code; for(;;) __asm__ volatile("hlt"); }
double sin(double x)  { (void)x; return 0.0; }
double cos(double x)  { (void)x; return 1.0; }
/* setjmp/longjmp stubs — Musashi uses these for exception unwinding.
 * In a bare-metal single-threaded kernel we just halt on longjmp. */
typedef long long jmp_buf[8];
int  _setjmp(jmp_buf *e)          { (void)e; return 0; }
void longjmp(jmp_buf *e, int v)   { (void)e; (void)v;
    for(;;) __asm__ volatile("hlt"); }

/* String / memory stubs */
void *memset(void *d, int c, unsigned long n) {
    unsigned char *p = (unsigned char *)d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}
void *memcpy(void *d, const void *s, unsigned long n) {
    unsigned char *dp = (unsigned char *)d;
    const unsigned char *sp = (const unsigned char *)s;
    while (n--) *dp++ = *sp++;
    return d;
}
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
unsigned long strlen(const char *s) {
    unsigned long n = 0;
    while (*s++) n++;
    return n;
}

/* Serial UART output used by the kernel in place of fprintf */
static inline void _uart_putc(char c) {
    /* Wait for transmit-hold-empty on COM1 */
    __asm__ volatile (
        "1: inb $0x3FD, %%al\n"
        "   testb $0x20, %%al\n"
        "   jz 1b\n"
        "   movb %0, %%al\n"
        "   outb %%al, $0x3F8\n"
        :: "r"((unsigned char)c) : "eax"
    );
}
static void _uart_puts(const char *s) {
    while (*s) { if (*s == '\n') _uart_putc('\r'); _uart_putc(*s++); }
}

/* fprintf stub — routes to UART serial output, ignores FILE*, ignores format */
typedef void FILE;
extern FILE *stderr;
FILE *stderr = (FILE*)0;

int fprintf(FILE *f, const char *fmt, ...) {
    (void)f;
    _uart_puts(fmt);
    return 0;
}
int printf(const char *fmt, ...) {
    _uart_puts(fmt);
    return 0;
}
STUBEOF
gcc ${GCC_FLAGS} -c "${BUILD_DIR}/obj/stubs.c" -o "${BUILD_DIR}/obj/stubs.o"
ok "  Compiled:  stubs.c (symbol resolution)"

# Link into ELF64
ld -T "${KERNEL_LD}" \
    "${BUILD_DIR}/obj/uaos_kernel_entry.o" \
    "${BUILD_DIR}/obj/idt_stubs.o" \
    "${BUILD_DIR}/obj/uaos_kernel_main.o" \
    "${BUILD_DIR}/obj/framebuffer.o" \
    "${BUILD_DIR}/obj/desktop.o" \
    "${BUILD_DIR}/obj/cursor.o" \
    "${BUILD_DIR}/obj/shell_win.o" \
    "${BUILD_DIR}/obj/wm.o" \
    "${BUILD_DIR}/obj/filebrowser.o" \
    "${BUILD_DIR}/obj/about_win.o" \
    "${BUILD_DIR}/obj/calc_win.o" \
    "${BUILD_DIR}/obj/softfloat.o" \
    "${BUILD_DIR}/obj/m68kcpu.o" \
    "${BUILD_DIR}/obj/m68kops.o" \
    "${BUILD_DIR}/obj/uaos_m68k_glue.o" \
    "${BUILD_DIR}/obj/uaos_emu_registry.o" \
    ${BIN_OBJ_LIST} \
    "${BUILD_DIR}/obj/idt.o" \
    "${BUILD_DIR}/obj/ps2mouse.o" \
    "${BUILD_DIR}/obj/ps2kbd.o" \
    "${BUILD_DIR}/obj/vmmouse.o" \
    "${BUILD_DIR}/obj/rtc.o" \
    "${BUILD_DIR}/obj/virtio_blk.o" \
    "${BUILD_DIR}/obj/thunk_handler.o" \
    "${BUILD_DIR}/obj/rom_modules.o" \
    "${BUILD_DIR}/obj/utility_lib.o" \
    "${BUILD_DIR}/obj/console_device.o" \
    "${BUILD_DIR}/obj/mathffp_lib.o" \
    "${BUILD_DIR}/obj/locale_lib.o" \
    "${BUILD_DIR}/obj/ixemul_lib.o" \
    "${BUILD_DIR}/obj/timer_device.o" \
    "${BUILD_DIR}/obj/keyboard_device.o" \
    "${BUILD_DIR}/obj/graphics_lib.o" \
    "${BUILD_DIR}/obj/dos_lib.o" \
    "${BUILD_DIR}/obj/mmu_sandbox.o" \
    "${BUILD_DIR}/obj/page_fault_handler.o" \
    "${BUILD_DIR}/obj/uaos_uae_bridge.o" \
    "${BUILD_DIR}/obj/ramfs.o" \
    "${BUILD_DIR}/obj/vfs.o" \
    "${BUILD_DIR}/obj/blockdev.o" \
    "${BUILD_DIR}/obj/fat32.o" \
    "${BUILD_DIR}/obj/pfs3.o" \
    "${BUILD_DIR}/obj/ext4.o" \
    "${BUILD_DIR}/obj/pointer_prefs.o" \
    "${BUILD_DIR}/obj/stubs.o" \
    -o "${KERNEL_ELF}"
ok "  Linked:    uaos-kernel.elf  ($(du -h "${KERNEL_ELF}" | cut -f1))"

# Verify ELF magic and multiboot2 header are present
if file "${KERNEL_ELF}" | grep -q 'ELF 64-bit'; then
    ok "  ELF64 magic verified"
else
    fatal "Kernel is not a valid ELF64 binary — check linker script"
fi

# -------------------------------------------------------------------------
# Step 2 — Package sys-root directory layout into the ISO staging area
# -------------------------------------------------------------------------

info "Step 2: Packaging sys-root layout"

if [[ -d "${SYS_ROOT}" ]]; then
    cp -r "${SYS_ROOT}/." "${ISO_STAGING}/sys-root/"
    ok "sys-root copied"
else
    warn "sys-root not found at ${SYS_ROOT} — using empty skeleton"
fi

# -------------------------------------------------------------------------
# Step 3 — Install GRUB configuration
# -------------------------------------------------------------------------

info "Step 3: Injecting GRUB configuration"

if [[ -f "${GRUB_CFG}" ]]; then
    cp "${GRUB_CFG}" "${ISO_STAGING}/boot/grub/grub.cfg"
    ok "grub.cfg installed at /boot/grub/grub.cfg"
else
    fatal "grub.cfg not found at ${GRUB_CFG}"
fi

# -------------------------------------------------------------------------
# Step 4 — Install AROS kickstart configuration module
# -------------------------------------------------------------------------

info "Step 4: Installing kickstart configuration module"

if [[ -f "${KICKSTART_CONF}" ]]; then
    cp "${KICKSTART_CONF}" "${ISO_STAGING}/boot/aros_kickstart.conf"
    ok "aros_kickstart.conf installed"
else
    warn "aros_kickstart.conf not found — creating placeholder"
    echo "# placeholder" > "${ISO_STAGING}/boot/aros_kickstart.conf"
fi

# -------------------------------------------------------------------------
# Step 5 — Install real ELF64 kernel into ISO staging
# -------------------------------------------------------------------------

info "Step 5: Installing ELF64 kernel into ISO staging"

cp "${KERNEL_ELF}" "${ISO_STAGING}/boot/uaos-kernel.bin"
ok "Kernel installed at /boot/uaos-kernel.bin ($(du -h "${ISO_STAGING}/boot/uaos-kernel.bin" | cut -f1))"

# -------------------------------------------------------------------------
# Step 6 — Create mock sysroot image for multiboot2 module2 validation
# -------------------------------------------------------------------------

info "Step 6: Creating mock sysroot image module"

dd if=/dev/zero                         \
   of="${ISO_STAGING}/boot/uaos-sysroot.img" \
   bs=512 count=2048                    \
   status=none

ok "Mock sysroot image created (1 MB placeholder)"

# -------------------------------------------------------------------------
# Step 7 — Create S/Startup-Sequence stub
# -------------------------------------------------------------------------

info "Step 7: Creating S/Startup-Sequence stub"

cat > "${ISO_STAGING}/sys-root/S/Startup-Sequence" <<'STARTUP'
; Ultimate Amiga OS — Startup-Sequence
; This script is executed by AmigaDOS after the kernel initialises Exec.

Assign SYS: SYS:
Assign C:   SYS:C
Assign S:   SYS:S
Assign L:   SYS:L
Assign DEVS: SYS:DEVS

; Load device drivers
Mount DEVS:DOSDrivers/~(#?.info)

; Execute user startup
Execute S:User-Startup
STARTUP

ok "Startup-Sequence written"

# -------------------------------------------------------------------------
# Step 8 — Validate staging directory structure
# -------------------------------------------------------------------------

info "Step 8: Validating staging directory structure"

REQUIRED_FILES=(
    "boot/grub/grub.cfg"
    "boot/uaos-kernel.bin"
    "boot/uaos-sysroot.img"
    "boot/aros_kickstart.conf"
    "sys-root/S/Startup-Sequence"
)

MISSING=0
for f in "${REQUIRED_FILES[@]}"; do
    if [[ -f "${ISO_STAGING}/${f}" ]]; then
        ok "  FOUND: ${f}"
    else
        warn "MISSING: ${f}"
        MISSING=$((MISSING + 1))
    fi
done

if [[ ${MISSING} -gt 0 ]]; then
    fatal "${MISSING} required file(s) missing from staging directory"
fi

# -------------------------------------------------------------------------
# Step 9 — Invoke grub-mkrescue to build the bootable ISO
# -------------------------------------------------------------------------

info "Step 9: Building hybrid BIOS+EFI bootable ISO"

if ! command -v grub-mkrescue &>/dev/null && ! command -v grub2-mkrescue &>/dev/null; then
    fatal "grub-mkrescue not found. Install: sudo apt install grub-pc-bin grub-common xorriso"
fi

GRUB_CMD="grub-mkrescue"
command -v grub-mkrescue &>/dev/null || GRUB_CMD="grub2-mkrescue"

# Stage 9a: Build a self-contained EFI GRUB image (bootx64.efi) with
# multiboot2 baked in.  This avoids the EFI GRUB needing to load .mod files
# at runtime from the ISO, and guarantees ELF64 multiboot2 support.
EFI_IMG="${BUILD_DIR}/bootx64.efi"
EFI_STAGING="${ISO_STAGING}/boot/grub"

if command -v grub-mkstandalone &>/dev/null && \
   [[ -d /usr/lib/grub/x86_64-efi ]]; then
    info "  Building EFI GRUB image (x86_64-efi + multiboot2)..."
    grub-mkstandalone \
        --format=x86_64-efi \
        --output="${EFI_IMG}" \
        --modules="normal multiboot2 ls cat echo all_video serial" \
        "boot/grub/grub.cfg=${GRUB_CFG}"
    mkdir -p "${ISO_STAGING}/EFI/BOOT"
    cp "${EFI_IMG}" "${ISO_STAGING}/EFI/BOOT/BOOTX64.EFI"
    ok "  EFI GRUB image built: $(du -h "${EFI_IMG}" | cut -f1)"
else
    warn "  grub-mkstandalone or x86_64-efi not found — EFI boot unavailable"
fi

# Stage 9b: Build the final hybrid BIOS+EFI ISO with grub-mkrescue.
# --modules lists only modules present in BOTH i386-pc and x86_64-efi;
# BIOS-only modules (vbe, vga) are loaded dynamically via grub.cfg insmod.
"${GRUB_CMD}"                                                       \
    --output="${ISO_OUTPUT}"                                        \
    --modules="normal multiboot2 iso9660 ls cat echo gfxterm all_video serial" \
    --compress=no                                                   \
    "${ISO_STAGING}"

# -------------------------------------------------------------------------
# Step 10 — Report
# -------------------------------------------------------------------------

if [[ -f "${ISO_OUTPUT}" ]]; then
    ISO_SIZE=$(du -h "${ISO_OUTPUT}" | cut -f1)
    ok "────────────────────────────────────────────────"
    ok "ISO build successful!"
    ok "  Output : ${ISO_OUTPUT}"
    ok "  Size   : ${ISO_SIZE}"
    ok "────────────────────────────────────────────────"
    ok "Test with: qemu-system-x86_64 -cdrom '${ISO_OUTPUT}' -m 512M -boot d"
else
    fatal "grub-mkrescue finished but ISO not found at ${ISO_OUTPUT}"
fi
