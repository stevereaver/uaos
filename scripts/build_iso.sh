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
KICKSTART_CONF="${REPO_ROOT}/emulation/rom_patches/kickstart.conf"
KERNEL_ASM="${REPO_ROOT}/kernel/boot/uaos_kernel_entry.asm"
KERNEL_MAIN="${REPO_ROOT}/kernel/boot/uaos_kernel_main.c"
KERNEL_LD="${REPO_ROOT}/kernel/boot/uaos_kernel.ld"
KERNEL_ELF="${BUILD_DIR}/uaos-kernel.elf"

GCC_FLAGS="-ffreestanding -fno-stack-protector -fno-pie -fno-PIE \
           -mno-red-zone -nostdlib -m64 -O2 -std=c11 \
           -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
           -Wall -Wextra \
           -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter \
           -Wno-address-of-packed-member -Wno-missing-braces"

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
mkdir -p "${ISO_STAGING}/SYS_ROOT/C"
mkdir -p "${ISO_STAGING}/SYS_ROOT/DEVS"
mkdir -p "${ISO_STAGING}/SYS_ROOT/L"
mkdir -p "${ISO_STAGING}/SYS_ROOT/LIBS"
mkdir -p "${ISO_STAGING}/SYS_ROOT/S"
mkdir -p "${ISO_STAGING}/SYS_ROOT/SYS"
mkdir -p "${ISO_STAGING}/SYS_ROOT/Tools"
mkdir -p "${ISO_STAGING}/documentation"

ok "Staging directories created at ${ISO_STAGING}"

# -------------------------------------------------------------------------
# Step 1d — Stage documentation files
# -------------------------------------------------------------------------

info "Step 1d: Staging documentation"
if [[ -f "${REPO_ROOT}/documentation/uaos.guide" ]]; then
    cp "${REPO_ROOT}/documentation/uaos.guide" "${ISO_STAGING}/documentation/uaos.guide"
    ok "  Copied: documentation/uaos.guide"
else
    warn "  uaos.guide not found in documentation/"
fi

# -------------------------------------------------------------------------
# Step 1a — Build host-side UAOS binary tools
# -------------------------------------------------------------------------

info "Step 1a: Building UAOS binary generation tools"

TOOLS_DIR="${REPO_ROOT}/tools"
gcc -O2 -o "${BUILD_DIR}/gen_uaos_native" "${TOOLS_DIR}/gen_uaos_native.c"
ok "  Built: gen_uaos_native"
gcc -O2 -o "${BUILD_DIR}/gen_uaos_m68k"   "${TOOLS_DIR}/gen_uaos_m68k.c"
ok "  Built: gen_uaos_m68k"
gcc -O2 -o "${BUILD_DIR}/gen_uaos_x64"    "${TOOLS_DIR}/gen_uaos_x64.c"
ok "  Built: gen_uaos_x64"
gcc -O2 -o "${BUILD_DIR}/gen_m68k_library" "${TOOLS_DIR}/gen_m68k_library.c"
ok "  Built: gen_m68k_library"

# -------------------------------------------------------------------------
# Step 1b — Generate real M68k binary .library files
# -------------------------------------------------------------------------

info "Step 1b: Generating M68k binary .library files"

mkdir -p "${REPO_ROOT}/system/LIBS"
"${BUILD_DIR}/gen_m68k_library" "powerpacker.library" 1 4 \
    "${REPO_ROOT}/system/LIBS/powerpacker.library"
ok "  Generated: system/LIBS/powerpacker.library"

# -------------------------------------------------------------------------
# Step 1c — Compile ELF64 kernel from NASM + C sources
# -------------------------------------------------------------------------

info "Step 1c: Compiling ELF64 kernel"

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

nasm -f elf64 \
    "${REPO_ROOT}/kernel/exec/task_switch.asm" \
    -o "${BUILD_DIR}/obj/task_switch.o"
ok "  Assembled: task_switch.asm"

# Compile Musashi M68k core (suppress its own warnings — not our code)
# Note: softfloat.c and m68kfpu.c are excluded — FPU emulation is disabled
# in uaos_m68kconf.h (M68K_EMULATE_FPOINT = OFF), so the SoftFloat 2b
# library is not compiled into the kernel.
for msrc in \
    "${MUSASHI_DIR}/m68kcpu.c" \
    "${MUSASHI_DIR}/m68kops.c"
do
    base="$(basename "${msrc}" .c)"
    gcc ${GCC_FLAGS} -w \
        -DMUSASHI_CNF='"uaos_m68kconf.h"' \
        -I"${REPO_ROOT}/emulation" \
        -I"${MUSASHI_DIR}" \
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
    tail -n +46 "${REPO_ROOT}/emulation/uaos_emu_registry.c"
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
    "${REPO_ROOT}/kernel/display/icon_render.c" \
    "${REPO_ROOT}/kernel/display/cursor.c" \
    "${REPO_ROOT}/kernel/display/shell_win.c" \
    "${REPO_ROOT}/kernel/display/wm.c" \
    "${REPO_ROOT}/kernel/display/filebrowser.c" \
    "${REPO_ROOT}/kernel/display/about_win.c" \
    "${REPO_ROOT}/kernel/display/calc_win.c" \
    "${REPO_ROOT}/kernel/display/clock_win.c" \
    "${REPO_ROOT}/kernel/display/netinfo_win.c" \
    "${REPO_ROOT}/kernel/display/user_window.c" \
    "${REPO_ROOT}/kernel/display/vim_win.c" \
    "${REPO_ROOT}/kernel/irq/idt.c" \
    "${REPO_ROOT}/kernel/irq/ps2mouse.c" \
    "${REPO_ROOT}/kernel/irq/ps2kbd.c" \
    "${REPO_ROOT}/kernel/irq/vmmouse.c" \
    "${REPO_ROOT}/kernel/irq/rtc.c" \
    "${REPO_ROOT}/kernel/irq/virtio_blk.c" \
    "${REPO_ROOT}/kernel/drivers/virtio_net.c" \
    "${REPO_ROOT}/kernel/drivers/e1000.c" \
    "${REPO_ROOT}/kernel/net/eth.c" \
    "${REPO_ROOT}/kernel/net/arp.c" \
    "${REPO_ROOT}/kernel/net/ip.c" \
    "${REPO_ROOT}/kernel/net/icmp.c" \
    "${REPO_ROOT}/kernel/net/udp.c" \
    "${REPO_ROOT}/kernel/net/tcp.c" \
    "${REPO_ROOT}/kernel/net/dhcp.c" \
    "${REPO_ROOT}/kernel/net/dns.c" \
    "${REPO_ROOT}/kernel/net/ntp.c" \
    "${REPO_ROOT}/kernel/net/timezone.c" \
    "${REPO_ROOT}/kernel/net/stack.c" \
    "${REPO_ROOT}/kernel/net/net_device.c" \
    "${REPO_ROOT}/kernel/exec/thunk_handler.c" \
    "${REPO_ROOT}/kernel/exec/rom_modules.c" \
    "${REPO_ROOT}/kernel/exec/task.c" \
    "${REPO_ROOT}/kernel/exec/elf64_loader.c" \
    "${REPO_ROOT}/kernel/exec/syscall_dispatch.c" \
    "${REPO_ROOT}/kernel/exec/mem_info.c" \
    "${REPO_ROOT}/kernel/exec/exec_task.c" \
    "${REPO_ROOT}/kernel/exec/exec_signal.c" \
    "${REPO_ROOT}/kernel/exec/exec_ipc.c" \
    "${REPO_ROOT}/kernel/exec/utility_lib.c" \
    "${REPO_ROOT}/kernel/exec/console_device.c" \
    "${REPO_ROOT}/kernel/exec/mathffp_lib.c" \
    "${REPO_ROOT}/kernel/exec/locale_lib.c" \
    "${REPO_ROOT}/kernel/exec/ixemul_lib.c" \
    "${REPO_ROOT}/kernel/exec/timer_device.c" \
    "${REPO_ROOT}/kernel/exec/bsdsocket_lib.c" \
    "${REPO_ROOT}/kernel/exec/keyboard_device.c" \
    "${REPO_ROOT}/kernel/exec/graphics_lib.c" \
    "${REPO_ROOT}/kernel/exec/dos_lib.c" \
    "${REPO_ROOT}/kernel/exec/workbench_lib.c" \
    "${REPO_ROOT}/kernel/exec/intuition_lib.c" \
    "${REPO_ROOT}/kernel/exec/gadtools_lib.c" \
    "${REPO_ROOT}/kernel/exec/boopsi_builtin.c" \
    "${REPO_ROOT}/kernel/exec/loadable_lib.c" \
    "${REPO_ROOT}/kernel/exec/mmu_sandbox.c" \
    "${REPO_ROOT}/kernel/exec/page_fault_handler.c" \
    "${REPO_ROOT}/kernel/chipset/chip_emu.c" \
    "${REPO_ROOT}/kernel/chipset/floppy.c" \
    "${REPO_ROOT}/kernel/audio/audio.c" \
    "${REPO_ROOT}/kernel/audio/pc_speaker.c" \
    "${REPO_ROOT}/kernel/audio/ac97.c" \
    "${REPO_ROOT}/kernel/audio/audio_test.c" \
    "${REPO_ROOT}/emulation/uaos_uae_bridge.c" \
    "${REPO_ROOT}/kernel/dos/ramfs.c" \
    "${REPO_ROOT}/kernel/dos/vfs.c" \
    "${REPO_ROOT}/kernel/dos/handle_table.c" \
    "${REPO_ROOT}/kernel/dos/handler.c" \
    "${REPO_ROOT}/kernel/dos/handler_loader.c" \
    "${REPO_ROOT}/kernel/dos/dos_list.c" \
    "${REPO_ROOT}/kernel/dos/device_handler.c" \
    "${REPO_ROOT}/kernel/dos/aux_handler.c" \
    "${REPO_ROOT}/kernel/dos/port_handler.c" \
    "${REPO_ROOT}/kernel/dos/ram_handler.c" \
    "${REPO_ROOT}/kernel/dos/fat_handler.c" \
    "${REPO_ROOT}/kernel/dos/blockdev.c" \
    "${REPO_ROOT}/kernel/dos/partition.c" \
    "${REPO_ROOT}/kernel/dos/dma.c" \
    "${REPO_ROOT}/kernel/dos/fat32.c" \
    "${REPO_ROOT}/kernel/dos/pfs3.c" \
    "${REPO_ROOT}/kernel/dos/ext4.c" \
    "${REPO_ROOT}/kernel/dos/iso9660.c" \
    "${REPO_ROOT}/kernel/dos/icon_loader.c" \
    "${REPO_ROOT}/kernel/drivers/ide.c" \
    "${REPO_ROOT}/kernel/drivers/floppy_blk.c" \
    "${REPO_ROOT}/kernel/display/pointer_prefs.c" \
    "${REPO_ROOT}/kernel/shell/native_cmd.c" \
    "${REPO_ROOT}/kernel/shell/cmd_template.c" \
    "${REPO_ROOT}/kernel/shell/exec_file.c" \
    "${REPO_ROOT}/kernel/shell/cmd_version.c" \
    "${REPO_ROOT}/kernel/shell/cmd_mem.c" \
    "${REPO_ROOT}/kernel/shell/cmd_libs.c" \
    "${REPO_ROOT}/kernel/shell/cmd_clear.c" \
    "${REPO_ROOT}/kernel/shell/cmd_reboot.c" \
    "${REPO_ROOT}/kernel/shell/cmd_pwd.c" \
    "${REPO_ROOT}/kernel/shell/cmd_info.c" \
    "${REPO_ROOT}/kernel/shell/cmd_date.c" \
    "${REPO_ROOT}/kernel/shell/cmd_which.c" \
    "${REPO_ROOT}/kernel/shell/cmd_disks.c" \
    "${REPO_ROOT}/kernel/shell/cmd_fdisk.c" \
    "${REPO_ROOT}/kernel/shell/cmd_format.c" \
    "${REPO_ROOT}/kernel/shell/cmd_pointer.c" \
    "${REPO_ROOT}/kernel/shell/cmd_run.c" \
    "${REPO_ROOT}/kernel/shell/cmd_assign.c" \
    "${REPO_ROOT}/kernel/shell/cmd_execute.c" \
    "${REPO_ROOT}/kernel/shell/cmd_loadwb.c" \
    "${REPO_ROOT}/kernel/shell/cmd_calc.c" \
    "${REPO_ROOT}/kernel/shell/cmd_ifconfig.c" \
    "${REPO_ROOT}/kernel/shell/cmd_ping.c" \
    "${REPO_ROOT}/kernel/shell/cmd_route.c" \
    "${REPO_ROOT}/kernel/shell/cmd_nslookup.c" \
    "${REPO_ROOT}/kernel/shell/cmd_ntpd.c" \
    "${REPO_ROOT}/kernel/shell/cmd_netstart.c" \
    "${REPO_ROOT}/kernel/shell/cmd_netstop.c" \
    "${REPO_ROOT}/kernel/shell/cmd_clock.c" \
    "${REPO_ROOT}/kernel/shell/cmd_netinfo.c" \
    "${REPO_ROOT}/kernel/shell/cmd_vim.c" \
    "${REPO_ROOT}/kernel/shell/cmd_newcli.c" \
    "${REPO_ROOT}/kernel/shell/cmd_ask.c" \
    "${REPO_ROOT}/kernel/shell/resident_cmd.c" \
    "${REPO_ROOT}/kernel/shell/cmd_resident.c" \
    "${REPO_ROOT}/kernel/shell/cmd_ps.c" \
    "${REPO_ROOT}/kernel/shell/cmd_wait.c" \
    "${REPO_ROOT}/kernel/shell/cmd_prompt.c" \
    "${REPO_ROOT}/kernel/shell/cmd_stack.c" \
    "${REPO_ROOT}/kernel/shell/cmd_why.c" \
    "${REPO_ROOT}/kernel/shell/cmd_failat.c" \
    "${REPO_ROOT}/kernel/shell/cmd_quit.c" \
    "${REPO_ROOT}/kernel/shell/cmd_endcli.c" \
    "${REPO_ROOT}/kernel/shell/cmd_relabel.c" \
    "${REPO_ROOT}/kernel/shell/cmd_mount.c" \
    "${REPO_ROOT}/kernel/shell/cmd_getenv.c" \
    "${REPO_ROOT}/kernel/shell/cmd_unset.c" \
    "${REPO_ROOT}/kernel/shell/cmd_jobs.c" \
    "${REPO_ROOT}/kernel/shell/cmd_install.c" \
    "${REPO_ROOT}/kernel/shell/cmd_diskchange.c" \
    "${REPO_ROOT}/kernel/shell/cmd_addbuffers.c" \
    "${REPO_ROOT}/kernel/shell/cmd_requestchoice.c" \
    "${REPO_ROOT}/kernel/shell/cmd_requestfile.c" \
    "${REPO_ROOT}/kernel/shell/cmd_changetaskpri.c" \
    "${REPO_ROOT}/kernel/shell/cmd_status.c" \
    "${REPO_ROOT}/kernel/shell/cmd_strace.c"
do
    base="$(basename "${src}" .c)"
    if [ "${src##*/}" = "ntp.c" ]; then
        gcc ${GCC_FLAGS} -mno-sse \
            -I"${REPO_ROOT}/emulation" \
            -I"${REPO_ROOT}/kernel" \
            -c "${src}" -o "${BUILD_DIR}/obj/${base}.o"
    else
        gcc ${GCC_FLAGS} \
            -I"${REPO_ROOT}/emulation" \
            -I"${REPO_ROOT}/kernel" \
            -c "${src}" -o "${BUILD_DIR}/obj/${base}.o"
    fi
    ok "  Compiled:  ${src##*/}"
done

# Provide stub implementations for chip_emu_read/write and aligned_alloc
# so the freestanding link resolves all symbols for this validation build.
cat > "${BUILD_DIR}/obj/stubs.c" <<'STUBEOF'
#include <stdint.h>

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
int memcmp(const void *a, const void *b, unsigned long n) {
    const unsigned char *ap = (const unsigned char *)a;
    const unsigned char *bp = (const unsigned char *)b;
    while (n--) {
        if (*ap != *bp) return (int)*ap - (int)*bp;
        ap++; bp++;
    }
    return 0;
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
        "   movw $0x3FD, %%dx\n"
        "1: inb %%dx, %%al\n"
        "   testb $0x20, %%al\n"
        "   jz 1b\n"
        "   movb %0, %%al\n"
        "   movw $0x3F8, %%dx\n"
        "   outb %%al, %%dx\n"
        :: "r"((unsigned char)c) : "eax", "edx"
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
ld -z noexecstack -T "${KERNEL_LD}" \
    "${BUILD_DIR}/obj/uaos_kernel_entry.o" \
    "${BUILD_DIR}/obj/idt_stubs.o" \
    "${BUILD_DIR}/obj/task_switch.o" \
    "${BUILD_DIR}/obj/uaos_kernel_main.o" \
    "${BUILD_DIR}/obj/framebuffer.o" \
    "${BUILD_DIR}/obj/desktop.o" \
    "${BUILD_DIR}/obj/icon_render.o" \
    "${BUILD_DIR}/obj/cursor.o" \
    "${BUILD_DIR}/obj/shell_win.o" \
    "${BUILD_DIR}/obj/wm.o" \
    "${BUILD_DIR}/obj/filebrowser.o" \
    "${BUILD_DIR}/obj/about_win.o" \
    "${BUILD_DIR}/obj/calc_win.o" \
    "${BUILD_DIR}/obj/clock_win.o" \
    "${BUILD_DIR}/obj/netinfo_win.o" \
    "${BUILD_DIR}/obj/user_window.o" \
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
    "${BUILD_DIR}/obj/virtio_net.o" \
    "${BUILD_DIR}/obj/e1000.o" \
    "${BUILD_DIR}/obj/eth.o" \
    "${BUILD_DIR}/obj/arp.o" \
    "${BUILD_DIR}/obj/ip.o" \
    "${BUILD_DIR}/obj/icmp.o" \
    "${BUILD_DIR}/obj/udp.o" \
    "${BUILD_DIR}/obj/tcp.o" \
    "${BUILD_DIR}/obj/dhcp.o" \
    "${BUILD_DIR}/obj/dns.o" \
    "${BUILD_DIR}/obj/ntp.o" \
    "${BUILD_DIR}/obj/timezone.o" \
    "${BUILD_DIR}/obj/stack.o" \
    "${BUILD_DIR}/obj/net_device.o" \
    "${BUILD_DIR}/obj/thunk_handler.o" \
    "${BUILD_DIR}/obj/rom_modules.o" \
    "${BUILD_DIR}/obj/task.o" \
    "${BUILD_DIR}/obj/elf64_loader.o" \
    "${BUILD_DIR}/obj/syscall_dispatch.o" \
    "${BUILD_DIR}/obj/mem_info.o" \
    "${BUILD_DIR}/obj/exec_task.o" \
    "${BUILD_DIR}/obj/exec_signal.o" \
    "${BUILD_DIR}/obj/exec_ipc.o" \
    "${BUILD_DIR}/obj/utility_lib.o" \
    "${BUILD_DIR}/obj/console_device.o" \
    "${BUILD_DIR}/obj/mathffp_lib.o" \
    "${BUILD_DIR}/obj/locale_lib.o" \
    "${BUILD_DIR}/obj/ixemul_lib.o" \
    "${BUILD_DIR}/obj/timer_device.o" \
    "${BUILD_DIR}/obj/bsdsocket_lib.o" \
    "${BUILD_DIR}/obj/keyboard_device.o" \
    "${BUILD_DIR}/obj/graphics_lib.o" \
    "${BUILD_DIR}/obj/dos_lib.o" \
    "${BUILD_DIR}/obj/workbench_lib.o" \
    "${BUILD_DIR}/obj/intuition_lib.o" \
    "${BUILD_DIR}/obj/gadtools_lib.o" \
    "${BUILD_DIR}/obj/boopsi_builtin.o" \
    "${BUILD_DIR}/obj/loadable_lib.o" \
    "${BUILD_DIR}/obj/mmu_sandbox.o" \
    "${BUILD_DIR}/obj/page_fault_handler.o" \
    "${BUILD_DIR}/obj/chip_emu.o" \
    "${BUILD_DIR}/obj/floppy.o" \
    "${BUILD_DIR}/obj/audio.o" \
    "${BUILD_DIR}/obj/pc_speaker.o" \
    "${BUILD_DIR}/obj/ac97.o" \
    "${BUILD_DIR}/obj/audio_test.o" \
    "${BUILD_DIR}/obj/uaos_uae_bridge.o" \
    "${BUILD_DIR}/obj/ramfs.o" \
    "${BUILD_DIR}/obj/vfs.o" \
    "${BUILD_DIR}/obj/handle_table.o" \
    "${BUILD_DIR}/obj/handler.o" \
    "${BUILD_DIR}/obj/handler_loader.o" \
    "${BUILD_DIR}/obj/dos_list.o" \
    "${BUILD_DIR}/obj/device_handler.o" \
    "${BUILD_DIR}/obj/aux_handler.o" \
    "${BUILD_DIR}/obj/port_handler.o" \
    "${BUILD_DIR}/obj/ram_handler.o" \
    "${BUILD_DIR}/obj/fat_handler.o" \
    "${BUILD_DIR}/obj/blockdev.o" \
    "${BUILD_DIR}/obj/partition.o" \
    "${BUILD_DIR}/obj/dma.o" \
    "${BUILD_DIR}/obj/fat32.o" \
    "${BUILD_DIR}/obj/pfs3.o" \
    "${BUILD_DIR}/obj/ext4.o" \
    "${BUILD_DIR}/obj/iso9660.o" \
    "${BUILD_DIR}/obj/icon_loader.o" \
    "${BUILD_DIR}/obj/ide.o" \
    "${BUILD_DIR}/obj/floppy_blk.o" \
    "${BUILD_DIR}/obj/pointer_prefs.o" \
    "${BUILD_DIR}/obj/native_cmd.o" \
    "${BUILD_DIR}/obj/cmd_template.o" \
    "${BUILD_DIR}/obj/exec_file.o" \
    "${BUILD_DIR}/obj/cmd_version.o" \
    "${BUILD_DIR}/obj/cmd_mem.o" \
    "${BUILD_DIR}/obj/cmd_libs.o" \
    "${BUILD_DIR}/obj/cmd_clear.o" \
    "${BUILD_DIR}/obj/cmd_reboot.o" \
    "${BUILD_DIR}/obj/cmd_pwd.o" \
    "${BUILD_DIR}/obj/cmd_dir.o" \
    "${BUILD_DIR}/obj/cmd_makedir.o" \
    "${BUILD_DIR}/obj/cmd_delete.o" \
    "${BUILD_DIR}/obj/cmd_type.o" \
    "${BUILD_DIR}/obj/cmd_copy.o" \
    "${BUILD_DIR}/obj/cmd_rename.o" \
    "${BUILD_DIR}/obj/cmd_echo.o" \
    "${BUILD_DIR}/obj/cmd_protect.o" \
    "${BUILD_DIR}/obj/cmd_attr.o" \
    "${BUILD_DIR}/obj/cmd_info.o" \
    "${BUILD_DIR}/obj/cmd_date.o" \
    "${BUILD_DIR}/obj/cmd_which.o" \
    "${BUILD_DIR}/obj/cmd_disks.o" \
    "${BUILD_DIR}/obj/cmd_fdisk.o" \
    "${BUILD_DIR}/obj/cmd_format.o" \
    "${BUILD_DIR}/obj/cmd_pointer.o" \
    "${BUILD_DIR}/obj/cmd_run.o" \
    "${BUILD_DIR}/obj/cmd_assign.o" \
    "${BUILD_DIR}/obj/cmd_execute.o" \
    "${BUILD_DIR}/obj/cmd_loadwb.o" \
    "${BUILD_DIR}/obj/cmd_calc.o" \
    "${BUILD_DIR}/obj/cmd_ifconfig.o" \
    "${BUILD_DIR}/obj/cmd_ping.o" \
    "${BUILD_DIR}/obj/cmd_route.o" \
    "${BUILD_DIR}/obj/cmd_nslookup.o" \
    "${BUILD_DIR}/obj/cmd_ntpd.o" \
    "${BUILD_DIR}/obj/cmd_netstart.o" \
    "${BUILD_DIR}/obj/cmd_netstop.o" \
    "${BUILD_DIR}/obj/cmd_clock.o" \
    "${BUILD_DIR}/obj/cmd_netinfo.o" \
    "${BUILD_DIR}/obj/cmd_grep.o" \
    "${BUILD_DIR}/obj/cmd_more.o" \
    "${BUILD_DIR}/obj/cmd_vim.o" \
    "${BUILD_DIR}/obj/cmd_newcli.o" \
    "${BUILD_DIR}/obj/cmd_ask.o" \
    "${BUILD_DIR}/obj/resident_cmd.o" \
    "${BUILD_DIR}/obj/cmd_resident.o" \
    "${BUILD_DIR}/obj/cmd_ps.o" \
    "${BUILD_DIR}/obj/cmd_list.o" \
    "${BUILD_DIR}/obj/cmd_search.o" \
    "${BUILD_DIR}/obj/cmd_sort.o" \
    "${BUILD_DIR}/obj/cmd_join.o" \
    "${BUILD_DIR}/obj/cmd_wait.o" \
    "${BUILD_DIR}/obj/cmd_prompt.o" \
    "${BUILD_DIR}/obj/cmd_stack.o" \
    "${BUILD_DIR}/obj/cmd_why.o" \
    "${BUILD_DIR}/obj/cmd_failat.o" \
    "${BUILD_DIR}/obj/cmd_quit.o" \
    "${BUILD_DIR}/obj/cmd_endcli.o" \
    "${BUILD_DIR}/obj/cmd_filenote.o" \
    "${BUILD_DIR}/obj/cmd_relabel.o" \
    "${BUILD_DIR}/obj/cmd_mount.o" \
    "${BUILD_DIR}/obj/cmd_getenv.o" \
    "${BUILD_DIR}/obj/cmd_unset.o" \
    "${BUILD_DIR}/obj/cmd_jobs.o" \
    "${BUILD_DIR}/obj/cmd_install.o" \
    "${BUILD_DIR}/obj/cmd_diskchange.o" \
    "${BUILD_DIR}/obj/cmd_addbuffers.o" \
    "${BUILD_DIR}/obj/cmd_requestchoice.o" \
    "${BUILD_DIR}/obj/cmd_requestfile.o" \
    "${BUILD_DIR}/obj/cmd_changetaskpri.o" \
    "${BUILD_DIR}/obj/cmd_status.o" \
    "${BUILD_DIR}/obj/cmd_strace.o" \
    "${BUILD_DIR}/obj/vim_win.o" \
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

info "Step 2: Creating empty sys-root layout"

# sys-root is now built dynamically in Step 7, so we just create the directory structure
ok "sys-root directory structure created"

# -------------------------------------------------------------------------
# Step 2b — Generate real UAOS NATIVE binaries for C: commands
#
# Each C: command becomes a real 32-byte UAOS binary file with magic "UAOS",
# type NATIVE (0x0001), and the command name embedded in the header.
# The shell reads this header, looks up the native handler, and runs it.
# These files REPLACE the text stub files copied from sys-root/C/.
# -------------------------------------------------------------------------

info "Step 2b: Generating UAOS NATIVE binaries for C:"

C_STAGING="${ISO_STAGING}/SYS_ROOT/C"
GEN_NATIVE="${BUILD_DIR}/gen_uaos_native"

for cmd in version mem libs clear reboot \
           pwd \
           info date which disks fdisk format pointer \
           run assign execute loadwb ifconfig ping route nslookup ntpd netstart netstop vim ps netinfo \
           wait prompt stack why failat quit endcli relabel \
           getenv unset jobs \
           install diskchange addbuffers requestchoice requestfile changetaskpri status \
           strace; do
    "${GEN_NATIVE}" "${cmd}" "${C_STAGING}/${cmd}"
    ok "  Generated: C:${cmd}  (32-byte NATIVE binary)"
done

# Generate Tools: binaries
info "Step 2d: Generating Tools: binaries (Calculator, Clock, NetInfo, Pointer)"
TOOLS_STAGING="${ISO_STAGING}/SYS_ROOT/Tools"
"${GEN_NATIVE}" "calculator" "${TOOLS_STAGING}/Calculator"
ok "  Generated: Tools:Calculator  (32-byte NATIVE binary)"
"${GEN_NATIVE}" "clock"      "${TOOLS_STAGING}/Clock"
ok "  Generated: Tools:Clock       (32-byte NATIVE binary)"
"${GEN_NATIVE}" "netinfo"    "${TOOLS_STAGING}/NetInfo"
ok "  Generated: Tools:NetInfo     (32-byte NATIVE binary)"
"${GEN_NATIVE}" "pointer"    "${TOOLS_STAGING}/Pointer"
ok "  Generated: Tools:Pointer    (32-byte NATIVE binary)"

# -------------------------------------------------------------------------
# Step 2c — Wrap any Amiga Hunk binaries in emulation/binaries/ with UAOS header
#
# Each plain Amiga Hunk file gets a 32-byte UAOS header prepended.
# The wrapped file is placed in sys-root/C/ so it can be run transparently
# from the shell without the 'run' prefix.
# -------------------------------------------------------------------------

info "Step 2c: Wrapping M68k Amiga Hunk binaries with UAOS header"

GEN_M68K="${BUILD_DIR}/gen_uaos_m68k"

for f in "${BINARIES_DIR}"/*; do
    base="$(basename "$f")"
    [[ "$base" == .* ]]        && continue
    [[ "$base" == *.c ]]       && continue
    [[ "$base" == *.h ]]       && continue
    [[ "$base" == *.gitkeep ]] && continue
    [[ -f "$f" ]]              || continue
    lname="${base,,}"  # lowercase for C: filename
    "${GEN_M68K}" "${base}" "${f}" "${C_STAGING}/${lname}"
    ok "  Wrapped:   C:${lname}  (M68K Hunk, $(wc -c < "$f") bytes + 32 header)"
done

# -------------------------------------------------------------------------
# Step 2e — Build native x86-64 userspace programs from system/userspace/
# -------------------------------------------------------------------------
#
# Phase 7 ABI: compile any C programs in system/userspace/ with -nostdlib
# -fPIE -pie, link against the shared uaos_start.o, wrap the resulting
# ELF64 with gen_uaos_x64, and stage into SYS_ROOT/C/.
# -------------------------------------------------------------------------

info "Step 2e: Building native x86-64 userspace programs"

USERSPACE_DIR="${REPO_ROOT}/system/userspace"
if [[ -d "${USERSPACE_DIR}" ]]; then
    mkdir -p "${BUILD_DIR}/userspace"
    GEN_X64="${BUILD_DIR}/gen_uaos_x64"

    gcc -ffreestanding -fno-stack-protector -nostdlib -fPIE \
        -fcf-protection=none \
        -m64 -O2 -std=c11 \
        -I"${REPO_ROOT}/system/libuaos" \
        -c "${REPO_ROOT}/system/libuaos/uaos_start.c" \
        -o "${BUILD_DIR}/obj/uaos_start.o"
    ok "  Compiled: uaos_start.o"

    for src in "${USERSPACE_DIR}"/*.c; do
        [[ -f "${src}" ]] || continue
        base="$(basename "${src}" .c)"
        # Guide viewer is built separately for Tools:.
        [[ "${base}" == "guide" ]] && continue
        elf_out="${BUILD_DIR}/userspace/${base}"
        bin_out="${C_STAGING}/${base}"

        gcc -ffreestanding -fno-stack-protector -nostdlib -fPIE -pie \
            -fcf-protection=none \
            -m64 -O2 -std=c11 \
            -I"${REPO_ROOT}/system/libuaos" \
            -c "${src}" -o "${BUILD_DIR}/userspace/${base}.o"
        ok "  Compiled: userspace/${base}.c"

        gcc -nostdlib -fPIE -pie -m64 -fcf-protection=none \
            -o "${elf_out}" \
            "${BUILD_DIR}/obj/uaos_start.o" \
            "${BUILD_DIR}/userspace/${base}.o"
        ok "  Linked:   userspace/${base}"

        "${GEN_X64}" "${base}" "${elf_out}" "${bin_out}"
        ok "  Wrapped:  C:${base}  (x86-64 ELF64)"
    done

    # -------------------------------------------------------------------------
    # Step 2f — Build AmigaGuide help viewer for SYS:Tools
    # -------------------------------------------------------------------------
    info "Step 2f: Building AmigaGuide viewer for Tools:"
    GUIDE_SRC="${REPO_ROOT}/system/userspace/guide.c"
    if [[ -f "${GUIDE_SRC}" ]]; then
        mkdir -p "${BUILD_DIR}/userspace"
        mkdir -p "${ISO_STAGING}/SYS_ROOT/Tools"
        gcc -ffreestanding -fno-stack-protector -nostdlib -fPIE -pie \
            -fcf-protection=none \
            -m64 -O2 -std=c11 \
            -I"${REPO_ROOT}/system/libuaos" \
            -c "${GUIDE_SRC}" -o "${BUILD_DIR}/userspace/guide.o"
        ok "  Compiled: userspace/guide.c"

        gcc -nostdlib -fPIE -pie -m64 -fcf-protection=none \
            -o "${BUILD_DIR}/userspace/guide" \
            "${BUILD_DIR}/obj/uaos_start.o" \
            "${BUILD_DIR}/userspace/guide.o"
        ok "  Linked:   userspace/guide"

        "${GEN_X64}" "Guide" "${BUILD_DIR}/userspace/guide" "${ISO_STAGING}/SYS_ROOT/Tools/Guide"
        ok "  Wrapped:  Tools:Guide  (x86-64 ELF64)"

        # Keep a copy of the guide database in Tools: as well.
        if [[ -f "${REPO_ROOT}/documentation/uaos.guide" ]]; then
            cp "${REPO_ROOT}/documentation/uaos.guide" "${ISO_STAGING}/SYS_ROOT/Tools/uaos.guide"
            ok "  Copied:  Tools:uaos.guide"
        fi
    fi
else
    ok "  No userspace programs to build (system/userspace/ not found)"
fi

# -------------------------------------------------------------------------
# Step 2ga — Build GNU coreutils for SYS_ROOT/gnu/usr/bin
# -------------------------------------------------------------------------
#
# The gnu: assign (Workbench:gnu) provides a POSIX compatibility layer.
# Source files in system/gnusrc/ are compiled as x86-64 ELF64 PIE binaries
# using the same freestanding toolchain, linked against uaos_start.o, and
# wrapped with gen_uaos_x64 into SYS_ROOT/gnu/usr/bin/.
# These tools take GNU-style flags (parsed via uaos_getopt.h).
# The full GNU coreutils set is built: text utils, file utils, shell utils,
# system info utils, and user/group utils.
# -------------------------------------------------------------------------

info "Step 2ga: Building GNU coreutils"

GNUSRC_DIR="${REPO_ROOT}/system/gnusrc"
GNU_STAGING="${ISO_STAGING}/SYS_ROOT/gnu/usr/bin"
if [[ -d "${GNUSRC_DIR}" ]]; then
    mkdir -p "${GNU_STAGING}"
    mkdir -p "${ISO_STAGING}/SYS_ROOT/gnu/bin"
    mkdir -p "${ISO_STAGING}/SYS_ROOT/gnu/usr/local/bin"
    GEN_X64="${BUILD_DIR}/gen_uaos_x64"

    # uaos_start.o is already compiled in Step 2e; compile it if Step 2e was skipped.
    if [[ ! -f "${BUILD_DIR}/obj/uaos_start.o" ]]; then
        gcc -ffreestanding -fno-stack-protector -nostdlib -fPIE \
            -fcf-protection=none \
            -m64 -O2 -std=c11 \
            -I"${REPO_ROOT}/system/libuaos" \
            -c "${REPO_ROOT}/system/libuaos/uaos_start.c" \
            -o "${BUILD_DIR}/obj/uaos_start.o"
        ok "  Compiled: uaos_start.o"
    fi

    gnu_count=0
    for src in "${GNUSRC_DIR}"/*.c; do
        [[ -f "${src}" ]] || continue
        base="$(basename "${src}" .c)"
        elf_out="${BUILD_DIR}/userspace/gnu_${base}"
        bin_out="${GNU_STAGING}/${base}"

        gcc -ffreestanding -fno-stack-protector -nostdlib -fPIE -pie \
            -fcf-protection=none \
            -m64 -O2 -std=c11 \
            -I"${REPO_ROOT}/system/libuaos" \
            -c "${src}" -o "${BUILD_DIR}/userspace/gnu_${base}.o"
        ok "  Compiled: gnusrc/${base}.c"

        gcc -nostdlib -fPIE -pie -m64 -fcf-protection=none \
            -o "${elf_out}" \
            "${BUILD_DIR}/obj/uaos_start.o" \
            "${BUILD_DIR}/userspace/gnu_${base}.o"
        ok "  Linked:   gnu/${base}"

        "${GEN_X64}" "${base}" "${elf_out}" "${bin_out}"
        ok "  Wrapped:  gnu/usr/bin/${base}  (x86-64 ELF64)"
        gnu_count=$((gnu_count + 1))
    done
    ok "  Built ${gnu_count} GNU coreutils"
else
    ok "  No GNU coreutils sources found (system/gnusrc/ not found)"
fi

# -------------------------------------------------------------------------
# Step 2g — Build M68k assembly demos for SYS_ROOT/Demos
# -------------------------------------------------------------------------

info "Step 2g: Building M68k assembly demos"

DEMOS_STAGING="${ISO_STAGING}/SYS_ROOT/Demos"
mkdir -p "${DEMOS_STAGING}"

# List of demos to build (basename without extension). Each must have a
# corresponding source file at system/Demos/<name>.s.
M68K_DEMOS=(CopperBars AGATest HelloWorld)

VASM_DIR="${BUILD_DIR}/vasm"
VASM_BIN="${VASM_DIR}/vasmm68k_mot"
VLINK_BIN="${VASM_DIR}/vlink/vlink"

# Ensure vasm and vlink are available if any demo source exists.
need_toolchain=0
for demo in "${M68K_DEMOS[@]}"; do
    if [[ -f "${REPO_ROOT}/system/Demos/${demo}.s" ]]; then
        need_toolchain=1
        break
    fi
done

if [[ ${need_toolchain} -eq 1 ]]; then
    if [[ ! -f "${VASM_BIN}" || ! -f "${VLINK_BIN}" ]]; then
        info "  Downloading and building vasm-m68k + vlink"
        mkdir -p "${VASM_DIR}"
        if [[ ! -f "${VASM_DIR}/vasm.tar.gz" ]]; then
            wget -q -O "${VASM_DIR}/vasm.tar.gz" http://sun.hasenbraten.de/vasm/release/vasm.tar.gz \
                || fatal "Failed to download vasm source"
        fi
        if [[ ! -f "${VASM_DIR}/vlink.tar.gz" ]]; then
            wget -q -O "${VASM_DIR}/vlink.tar.gz" http://sun.hasenbraten.de/vlink/release/vlink.tar.gz \
                || fatal "Failed to download vlink source"
        fi
        cd "${VASM_DIR}"
        tar xzf vasm.tar.gz
        tar xzf vlink.tar.gz
        cd "${VASM_DIR}/vasm"
        make CPU=m68k SYNTAX=mot >/dev/null 2>&1 || fatal "Failed to build vasm"
        cd "${VASM_DIR}/vlink"
        make >/dev/null 2>&1 || fatal "Failed to build vlink"
        cp "${VASM_DIR}/vasm/vasmm68k_mot" "${VASM_BIN}"
        ok "  Built: vasm-m68k + vlink"
    fi

    for demo in "${M68K_DEMOS[@]}"; do
        DEMO_SRC="${REPO_ROOT}/system/Demos/${demo}.s"
        if [[ ! -f "${DEMO_SRC}" ]]; then
            ok "  No ${demo} source found (${DEMO_SRC})"
            continue
        fi
        "${VASM_BIN}" -Fhunk -o "${BUILD_DIR}/${demo}.o" "${DEMO_SRC}" \
            || fatal "Failed to assemble ${demo}.s"
        "${VLINK_BIN}" -bamigahunk -o "${BUILD_DIR}/${demo}.hunk" "${BUILD_DIR}/${demo}.o" \
            || fatal "Failed to link ${demo}"
        "${BUILD_DIR}/gen_uaos_m68k" "${demo}" "${BUILD_DIR}/${demo}.hunk" "${DEMOS_STAGING}/${demo}" \
            || fatal "Failed to wrap ${demo}"
        ok "  Built: SYS_ROOT/Demos/${demo}"
    done
else
    ok "  No M68k demo sources found"
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
# Step 4 — Install kickstart configuration module
# -------------------------------------------------------------------------

info "Step 4: Installing kickstart configuration module"

if [[ -f "${KICKSTART_CONF}" ]]; then
    cp "${KICKSTART_CONF}" "${ISO_STAGING}/boot/kickstart.conf"
    ok "kickstart.conf installed"
else
    warn "kickstart.conf not found — creating placeholder"
    echo "# placeholder" > "${ISO_STAGING}/boot/kickstart.conf"
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
# Step 7 — Copy system files to dynamic sys-root
# -------------------------------------------------------------------------

info "Step 7: Building dynamic sys-root from system files"

# Copy Startup-Sequence from the system directory
if [[ -f "${REPO_ROOT}/system/Startup-Sequence" ]]; then
    cp "${REPO_ROOT}/system/Startup-Sequence" "${ISO_STAGING}/SYS_ROOT/S/Startup-Sequence"
    ok "  Copied: system/Startup-Sequence -> sys-root/S/Startup-Sequence"
else
    fatal "  Startup-Sequence not found at ${REPO_ROOT}/system/Startup-Sequence"
fi

# Copy any extra files from system/S/ (e.g. ntp.conf, timezone.conf)
if [[ -d "${REPO_ROOT}/system/S" ]]; then
    cp -r "${REPO_ROOT}/system/S/"* "${ISO_STAGING}/SYS_ROOT/S/" 2>/dev/null || true
    ok "  Copied: system/S/ -> sys-root/S/"
fi

# Copy any additional system files if they exist
SYSTEM_DIR="${REPO_ROOT}/system"
if [[ -d "${SYSTEM_DIR}" ]]; then
    # Copy any shell scripts to C:
    if [[ -d "${SYSTEM_DIR}/C" ]]; then
        cp -r "${SYSTEM_DIR}/C/"* "${ISO_STAGING}/SYS_ROOT/C/" 2>/dev/null || true
        ok "  Copied: system/C/ -> sys-root/C/"
    fi
    
    # Copy any libraries to LIBS:
    if [[ -d "${SYSTEM_DIR}/LIBS" ]]; then
        cp -r "${SYSTEM_DIR}/LIBS/"* "${ISO_STAGING}/SYS_ROOT/LIBS/" 2>/dev/null || true
        ok "  Copied: system/LIBS/ -> sys-root/LIBS/"
    fi
    
    # Copy any devices to DEVS:
    if [[ -d "${SYSTEM_DIR}/DEVS" ]]; then
        cp -r "${SYSTEM_DIR}/DEVS/"* "${ISO_STAGING}/SYS_ROOT/DEVS/" 2>/dev/null || true
        ok "  Copied: system/DEVS/ -> sys-root/DEVS/"
    fi
    
    # Copy any L: files
    if [[ -d "${SYSTEM_DIR}/L" ]]; then
        cp -r "${SYSTEM_DIR}/L/"* "${ISO_STAGING}/SYS_ROOT/L/" 2>/dev/null || true
        ok "  Copied: system/L/ -> sys-root/L/"
    fi
    
    # Copy any SYS files
    if [[ -d "${SYSTEM_DIR}/SYS" ]]; then
        cp -r "${SYSTEM_DIR}/SYS/"* "${ISO_STAGING}/SYS_ROOT/SYS/" 2>/dev/null || true
        ok "  Copied: system/SYS/ -> sys-root/SYS/"
    fi
    
    # Copy any Tools files
    if [[ -d "${SYSTEM_DIR}/Tools" ]]; then
        cp -r "${SYSTEM_DIR}/Tools/"* "${ISO_STAGING}/SYS_ROOT/Tools/" 2>/dev/null || true
        ok "  Copied: system/Tools/ -> sys-root/Tools/"
    fi
    
    # Copy any Demos files (built binaries and icons), excluding assembly sources
    if [[ -d "${SYSTEM_DIR}/Demos" ]]; then
        mkdir -p "${ISO_STAGING}/SYS_ROOT/Demos"
        for f in "${SYSTEM_DIR}/Demos"/*; do
            [[ -f "$f" && "$f" != *.s ]] || continue
            cp "$f" "${ISO_STAGING}/SYS_ROOT/Demos/"
        done
        ok "  Copied: system/Demos/ -> SYS_ROOT/Demos/"
    fi

    # Copy GNU POSIX layer skeleton (static files only; binaries are built
    # in Step 2ga).  This ensures empty dirs like gnu/bin exist even if
    # no coreutils sources are present.
    if [[ -d "${SYSTEM_DIR}/gnu" ]]; then
        cp -r "${SYSTEM_DIR}/gnu/"* "${ISO_STAGING}/SYS_ROOT/gnu/" 2>/dev/null || true
        ok "  Copied: system/gnu/ -> sys-root/gnu/"
    fi
fi

ok "Dynamic sys-root populated from system files"

# -------------------------------------------------------------------------
# Step 8 — Validate staging directory structure
# -------------------------------------------------------------------------

info "Step 8: Validating staging directory structure"

REQUIRED_FILES=(
    "boot/grub/grub.cfg"
    "boot/uaos-kernel.bin"
    "boot/uaos-sysroot.img"
    "boot/kickstart.conf"
    "SYS_ROOT/S/Startup-Sequence"
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
