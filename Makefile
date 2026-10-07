# ============================================================================
# Makefile — Ultimate Amiga OS
#
# Replaces the old sequential scripts/build_iso.sh pipeline with a real
# dependency graph so the build parallelises with `make -j` and rebuilds
# only what changed.
#
#   make              build build/Ultimate_Amiga_OS.iso
#   make -j$(nproc)   parallel build
#   make kernel       build/uaos-kernel.elf only
#   make check        build + run the uitree layout self-test
#   make clean        rm -rf build/
#   make distclean    clean + remove generated in-tree files
#   make V=1          echo full commands instead of CC/LD tags
#
# scripts/build_iso.sh remains as a thin compatibility wrapper.
# ============================================================================

SHELL           := /bin/bash
.DEFAULT_GOAL   := all
.DELETE_ON_ERROR:
# Keep intermediates (demo .o/.hunk etc.) like the old script did, so make
# doesn't re-assemble them on every incremental run.
.SECONDARY:

ifeq ($(V),1)
Q :=
else
Q := @
endif

# --- Tools -------------------------------------------------------------------

CC      := gcc
LD      := ld
NASM    := nasm
AR      := ar
PYTHON  := python3
WGET    := wget -q --timeout=60 --tries=2
LHA     := lha

# Download cache: when UAOS_CACHE_DIR contains the file, it is copied instead
# of fetched (the vasm/vlink host, sun.hasenbraten.de, is often unreachable).
UAOS_CACHE_DIR ?= /tmp/uaos-dl-cache

# $(call FETCH,<url>) — copy from the cache if present, else download.
define FETCH
	cached=""; \
	if [ -d "$(UAOS_CACHE_DIR)" ]; then \
	    cached=$$(find "$(UAOS_CACHE_DIR)" -name '$(notdir $@)' -type f \
	        -size +0c 2>/dev/null | head -1); \
	fi; \
	if [ -n "$$cached" ]; then \
	    echo "  CACHE   $(notdir $@)"; \
	    cp "$$cached" $@; \
	else \
	    echo "  FETCH   $(notdir $@)"; \
	    $(WGET) -O $@ '$(1)'; \
	fi
endef

XORRISO := $(firstword $(foreach t,xorriso genisoimage mkisofs,\
             $(notdir $(shell command -v $t 2>/dev/null))))
ifeq ($(XORRISO),xorriso)
MKISOFS := xorriso -as mkisofs
else
MKISOFS := $(XORRISO)
endif

GRUB_MKRESCUE := $(firstword $(foreach t,grub-mkrescue grub2-mkrescue,\
                   $(notdir $(shell command -v $t 2>/dev/null))))
HAVE_EFI      := $(and $(shell command -v grub-mkstandalone 2>/dev/null),\
                   $(wildcard /usr/lib/grub/x86_64-efi))

# --- Layout ------------------------------------------------------------------

BUILD       := build
OBJ         := $(BUILD)/obj
USPOBJ      := $(BUILD)/userspace
BOBJ        := $(OBJ)/bearssl
STAMPS      := $(BUILD)/.stamps
ISO_STAGING := $(BUILD)/iso-staging
SYSROOT     := $(ISO_STAGING)/SYS_ROOT
ISO         := $(BUILD)/Ultimate_Amiga_OS.iso
KERNEL_ELF  := $(BUILD)/uaos-kernel.elf
KERNEL_LD   := kernel/boot/uaos_kernel.ld

MUSASHI_DIR  := emulation/musashi
BINARIES_DIR := emulation/binaries
GNUSRC       := system/gnusrc
USERSPACE    := system/userspace
LIBUAOS      := system/libuaos
BEARSSL_DIR  := system/bearssl
BEARSSL_LIB  := $(BUILD)/libbearssl.a
VASM_DIR     := $(BUILD)/vasm
VASM_BIN     := $(VASM_DIR)/vasmm68k_mot
VLINK_BIN    := $(VASM_DIR)/vlink/vlink

GEN_NATIVE  := $(BUILD)/gen_uaos_native
GEN_M68K    := $(BUILD)/gen_uaos_m68k
GEN_X64     := $(BUILD)/gen_uaos_x64
GEN_M68KLIB := $(BUILD)/gen_m68k_library

# --- Flags -------------------------------------------------------------------

# Freestanding kernel flags (verbatim from build_iso.sh) + auto header deps.
KCFLAGS := -ffreestanding -fno-stack-protector -fno-pie -fno-PIE \
           -mno-red-zone -nostdlib -m64 -O2 -std=c11 -g \
           -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
           -Wall -Wextra \
           -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter \
           -Wno-address-of-packed-member \
           -MMD -MP
KINC    := -Iemulation -Ikernel

MUSASHI_DEF := -DMUSASHI_CNF='"uaos_m68kconf.h"'

# Userspace PIE flags — -mno-red-zone is mandatory: tasks run in ring 0, so
# INT 0x80 / IRQ entry frames land on the user stack and would clobber the
# 128-byte SysV red zone.
UCFLAGS := -ffreestanding -fno-stack-protector -nostdlib -fPIE -pie \
           -mno-red-zone -fcf-protection=none -m64 -O2 -std=c11 -MMD -MP
ULINK   := -nostdlib -fPIE -pie -m64 -fcf-protection=none

BCFLAGS := -ffreestanding -fno-stack-protector -nostdlib -fPIE \
           -mno-red-zone -fcf-protection=none -m64 -O2 -std=c11 -MMD -MP \
           -I$(BEARSSL_DIR)/inc -I$(BEARSSL_DIR)/src -I$(BEARSSL_DIR)/compat

# --- Kernel sources ----------------------------------------------------------
#
# Kernel sources are discovered by wildcard — every kernel/**/*.c is compiled
# AND linked.  The old script kept separate compile and link lists that
# repeatedly drifted apart (objects compiled but never linked); with one list
# that bug class is gone.  Add a path to KERNEL_EXCLUDE to opt a file out.

KERNEL_EXCLUDE :=
KERNEL_SRCS := $(filter-out $(KERNEL_EXCLUDE),\
                 $(shell find kernel -name '*.c' | sort)) \
               emulation/uaos_uae_bridge.c
KERNEL_OBJS := $(addprefix $(OBJ)/,$(notdir $(KERNEL_SRCS:.c=.o)))

ASM_SRCS := kernel/boot/uaos_kernel_entry.asm \
            kernel/irq/idt_stubs.asm \
            kernel/exec/task_switch.asm
ASM_OBJS := $(addprefix $(OBJ)/,$(notdir $(ASM_SRCS:.asm=.o)))

# softfloat.c and m68kfpu.c are intentionally absent — FPU emulation is
# disabled in uaos_m68kconf.h (M68K_EMULATE_FPOINT = OFF).
MUSASHI_OBJS := $(addprefix $(OBJ)/,m68kcpu.o m68kdasm.o m68kops.o)
MUSASHI_GEN  := $(MUSASHI_DIR)/m68kops.c $(MUSASHI_DIR)/m68kops.h

GLUE_OBJ     := $(OBJ)/uaos_m68k_glue.o
REGISTRY_GEN := $(OBJ)/uaos_emu_registry_gen.c
REGISTRY_OBJ := $(OBJ)/uaos_emu_registry.o
SPLASH_OBJ   := $(OBJ)/splash_img.o

vpath %.c   $(sort $(dir $(KERNEL_SRCS)))
vpath %.asm $(sort $(dir $(ASM_SRCS)))

# Embedded Amiga binaries: every extensionless file in emulation/binaries/
# gets a generated _bin.c/_bin.h pair and a wrapped C: command.
BIN_NAMES := $(filter-out .gitkeep,\
               $(notdir \
                 $(filter-out %.c %.h,$(wildcard $(BINARIES_DIR)/*))))
BIN_SRCS  := $(addprefix $(BINARIES_DIR)/,$(BIN_NAMES))
BIN_OBJS  := $(addprefix $(OBJ)/,$(addsuffix _bin.o,$(BIN_NAMES)))
lc = $(shell printf '%s' '$(1)' | tr '[:upper:]' '[:lower:]')
BIN_LC    := $(foreach b,$(BIN_NAMES),$(call lc,$b))

# --- Userspace / GNU / BearSSL ------------------------------------------------

USERSPACE_SRCS  := $(wildcard $(USERSPACE)/*.c)
USERSPACE_PROGS := $(filter-out guide,$(basename $(notdir $(USERSPACE_SRCS))))
USER_ELFS       := $(addprefix $(USPOBJ)/,$(USERSPACE_PROGS))
USER_OBJS       := $(addprefix $(USPOBJ)/,$(addsuffix .o,$(USERSPACE_PROGS)))
USER_BINS       := $(addprefix $(SYSROOT)/C/,$(USERSPACE_PROGS))
HAVE_GUIDE_SRC  := $(wildcard $(USERSPACE)/guide.c)
ifneq ($(HAVE_GUIDE_SRC),)
GUIDE_ELF       := $(USPOBJ)/guide
GUIDE_BIN       := $(SYSROOT)/Tools/Guide
endif

GNU_PROGS := $(basename $(notdir $(wildcard $(GNUSRC)/*.c)))
GNU_OBJS  := $(addprefix $(USPOBJ)/gnu_,$(addsuffix .o,$(GNU_PROGS)))
GNU_ELFS  := $(addprefix $(USPOBJ)/gnu_,$(GNU_PROGS))
GNU_BINS  := $(addprefix $(SYSROOT)/gnu/usr/bin/,$(GNU_PROGS))
GNU_TLS   := wget curl

BEARSSL_SRCS := $(wildcard $(BEARSSL_DIR)/src/*.c $(BEARSSL_DIR)/src/*/*.c \
                  $(BEARSSL_DIR)/ta_roots.c)
BEARSSL_OBJS := $(addprefix $(BOBJ)/,$(notdir $(BEARSSL_SRCS:.c=.o)))
vpath %.c $(sort $(dir $(BEARSSL_SRCS)))

# --- C: native-command stubs --------------------------------------------------
#
# Stage-write order in the old script was: natives < wrapped hunks <
# userspace < bas download < system/ copies — later writers win.  Make needs
# one owner per file, so each name is assigned to the last writer.

NATIVE_C_ALL := version mem cpu libs clear reboot pwd info date which disks fdisk \
    format fsck pointer run assign execute loadwb ifconfig ping route \
    nslookup ntpd netstart netstop vim ed ps netinfo wait prompt stack why \
    failat quit endcli relabel getenv unset jobs install diskchange \
    addbuffers requestchoice requestfile changetaskpri status rx telnetd \
    strace print crossdos guide klog debug dmesg irqstat usbdiag crash \
    memcheck chiptrace taskdump taskstat watchdog ports timers handles \
    netstat diskdiag pciscan irqroute peek poke irqaudit sercon tickcheck \
    etrace prof failalloc pktmon screenshot runback alias unalias path skip \
    lab resload

C_FROM_USER   := $(USERSPACE_PROGS)
C_FROM_WRAP   := $(filter-out $(C_FROM_USER) bas,$(BIN_LC))
C_FROM_NATIVE := $(filter-out $(C_FROM_USER) $(BIN_LC) bas,$(NATIVE_C_ALL))
NEED_BAS      := $(if $(filter bas,$(C_FROM_USER) $(BIN_LC)),,yes)

NATIVE_BINS := $(addprefix $(SYSROOT)/C/,$(C_FROM_NATIVE))
WRAP_BINS   := $(addprefix $(SYSROOT)/C/,$(C_FROM_WRAP))
BAS_BIN     := $(if $(NEED_BAS),$(SYSROOT)/C/bas,)

TOOL_BINS := $(SYSROOT)/Tools/NetInfo $(SYSROOT)/Tools/Exchange \
             $(SYSROOT)/Tools/Blanker
UTIL_BINS := $(SYSROOT)/Utilities/Calculator $(SYSROOT)/Utilities/Clock
PREF_BINS := $(SYSROOT)/Prefs/Pointer $(SYSROOT)/Prefs/ScreenMode \
             $(SYSROOT)/Prefs/Font $(SYSROOT)/Prefs/IControl \
             $(SYSROOT)/Prefs/Input $(SYSROOT)/Prefs/Palette \
             $(SYSROOT)/Prefs/WBPattern $(SYSROOT)/Prefs/Serial \
             $(SYSROOT)/Prefs/Printer $(SYSROOT)/Prefs/Time \
             $(SYSROOT)/Prefs/Locale

# --- M68k demos / downloads ---------------------------------------------------

DEMOS     := $(basename $(notdir $(wildcard system/Demos/*.s)))
DEMO_BINS := $(addprefix $(SYSROOT)/Demos/,$(DEMOS))

VASM_STAMP    := $(STAMPS)/vasm-built
REGINA_LHA    := $(BUILD)/Regina.lha
REGINA_DIR    := $(BUILD)/regina-0.08i
REGINA_STAMP  := $(STAMPS)/regina-extracted
REXX_BIN      := $(SYSROOT)/REXX/rexx
ACE_LHA       := $(BUILD)/ace-basic.lha
ACE_DIR       := $(BUILD)/ace-basic
ACE_XSTAMP    := $(STAMPS)/ace-extracted
ACE_STAMP     := $(STAMPS)/ace-staged

# --- Staging ------------------------------------------------------------------

DIRS_STAMP    := $(STAMPS)/dirs
SYSROOT_STAMP := $(STAMPS)/sysroot-copied
SYSROOT_IMG   := $(ISO_STAGING)/boot/uaos-sysroot.img
KERNEL_BIN    := $(ISO_STAGING)/boot/uaos-kernel.bin
GRUB_STAGED   := $(ISO_STAGING)/boot/grub/grub.cfg
KICK_STAGED   := $(ISO_STAGING)/boot/kickstart.conf
SPLASH_STAGED := $(ISO_STAGING)/boot/splash.jpg
EFI_STAGED    := $(ISO_STAGING)/EFI/BOOT/BOOTX64.EFI
DOC_STAGED    := $(ISO_STAGING)/documentation/uaos.guide
GUIDE_DB      := $(SYSROOT)/Tools/uaos.guide

KICK_SRC   := $(wildcard emulation/rom_patches/kickstart.conf)
SPLASH_SRC := $(wildcard assets/splash.jpg)
GUIDE_SRC  := $(wildcard documentation/uaos.guide)

# Optional staging items — only requested when their source exists.
OPT_STAGED :=
ifneq ($(SPLASH_SRC),)
OPT_STAGED += $(SPLASH_STAGED)
endif
ifneq ($(GUIDE_SRC),)
OPT_STAGED += $(DOC_STAGED) $(GUIDE_DB)
endif
ifneq ($(HAVE_EFI),)
OPT_STAGED += $(EFI_STAGED)
endif

# system/ subtrees copied wholesale into SYS_ROOT (Demos handled separately —
# *.s sources are excluded).  SYS_FILES tracks their contents for rebuilds.
SYS_SUBDIRS := S C LIBS DEVS L SYS Tools Utilities Prefs Classes Fonts \
               Locale Storage gnu
SYS_FILES   := $(shell find $(addprefix system/,$(SYS_SUBDIRS)) \
                 -type f 2>/dev/null) \
               system/Startup-Sequence
DEMO_EXTRAS := $(shell find system/Demos -type f ! -name '*.s' 2>/dev/null)

SYSROOT_CONTENTS := $(NATIVE_BINS) $(WRAP_BINS) $(USER_BINS) $(BAS_BIN) \
                    $(TOOL_BINS) $(UTIL_BINS) $(PREF_BINS) $(GUIDE_BIN) \
                    $(GNU_BINS) $(DEMO_BINS) $(REXX_BIN) $(ACE_STAMP) \
                    $(SYSROOT)/LIBS/powerpacker.library
ifneq ($(GUIDE_SRC),)
SYSROOT_CONTENTS += $(GUIDE_DB)
endif

# ============================================================================
# Phony targets
# ============================================================================

.PHONY: all iso kernel tools sysroot userspace gnusrc demos check \
        clean distclean help FORCE

# Always-out-of-date prerequisite: anything depending on FORCE rebuilds on
# every make invocation.  Used so the os-release serial is stamped with the
# actual ISO build time rather than the last time sysroot contents changed.
FORCE:

all: iso
iso: $(ISO)
kernel: $(KERNEL_ELF)
tools: $(BUILD)/gen_uaos_native $(BUILD)/gen_uaos_m68k $(BUILD)/gen_uaos_x64 \
       $(BUILD)/gen_m68k_library $(BUILD)/m68kmake $(BUILD)/ui_layout_test
sysroot: $(SYSROOT_IMG)
userspace: $(USER_BINS) $(GUIDE_BIN)
gnusrc: $(GNU_BINS)
demos: $(DEMO_BINS)
check: $(STAMPS)/uitest

help:
	@echo "UAOS build targets:"
	@echo "  make / make iso   build $(ISO)"
	@echo "  make kernel       build $(KERNEL_ELF)"
	@echo "  make sysroot      build the SYS_ROOT module image only"
	@echo "  make tools        host-side generator tools only"
	@echo "  make check        build + run the uitree layout self-test"
	@echo "  make clean        remove build/"
	@echo "  make distclean    clean + generated in-tree files (_bin.*, m68kops.*)"
	@echo "  make V=1          verbose commands"

# --- Build directories --------------------------------------------------------

$(BUILD) $(OBJ) $(USPOBJ) $(BOBJ) $(VASM_DIR) $(STAMPS):
	$(Q)mkdir -p $@

$(DIRS_STAMP): | $(BUILD) $(STAMPS)
	$(Q)mkdir -p $(ISO_STAGING)/boot/grub $(ISO_STAGING)/boot/uaos \
	  $(SYSROOT)/C $(SYSROOT)/DEVS/Printers $(SYSROOT)/DEVS/Keymaps \
	  $(SYSROOT)/L $(SYSROOT)/LIBS $(SYSROOT)/S $(SYSROOT)/SYS \
	  $(SYSROOT)/Tools $(SYSROOT)/Utilities $(SYSROOT)/Prefs \
	  $(SYSROOT)/Classes $(SYSROOT)/Fonts $(SYSROOT)/Locale \
	  $(SYSROOT)/Storage $(SYSROOT)/Demos $(SYSROOT)/REXX \
	  $(SYSROOT)/ACE/bin $(SYSROOT)/ACE/lib $(SYSROOT)/ACE/bmaps \
	  $(SYSROOT)/ACE/include $(SYSROOT)/ACE/submods \
	  $(SYSROOT)/gnu/bin $(SYSROOT)/gnu/usr/bin \
	  $(SYSROOT)/gnu/usr/local/bin \
	  $(ISO_STAGING)/documentation $(ISO_STAGING)/EFI/BOOT
	$(Q)touch $@

# --- Host-side generator tools ------------------------------------------------

$(BUILD)/gen_uaos_native $(BUILD)/gen_uaos_m68k $(BUILD)/gen_uaos_x64 \
$(BUILD)/gen_m68k_library: $(BUILD)/%: tools/%.c | $(BUILD)
	@echo "  HOSTCC  $@"
	$(Q)$(CC) -O2 -o $@ $<

$(BUILD)/m68kmake: $(MUSASHI_DIR)/m68kmake.c | $(BUILD)
	@echo "  HOSTCC  $@"
	$(Q)$(CC) -o $@ $<

# UI layout engine self-test — uitree.c is shared with the kernel, so a
# failure here means the kernel layout code is broken; it gates the ELF link.
$(BUILD)/ui_layout_test: tools/ui_layout_test.c kernel/display/uitree.c \
    kernel/display/uiformat.c $(wildcard kernel/display/*.h) | $(BUILD)
	@echo "  HOSTCC  $@"
	$(Q)$(CC) -O2 -Ikernel/display -o $@ tools/ui_layout_test.c \
	    kernel/display/uitree.c kernel/display/uiformat.c

$(STAMPS)/uitest: $(BUILD)/ui_layout_test | $(STAMPS)
	@echo "  TEST    ui_layout_test"
	$(Q)$(BUILD)/ui_layout_test
	$(Q)touch $@

# --- Musashi M68k core ---------------------------------------------------------
# m68kmake emits both m68kops.c and m68kops.h in one run — grouped target.

$(MUSASHI_GEN) &: $(MUSASHI_DIR)/m68k_in.c $(BUILD)/m68kmake
	@echo "  GEN     m68kops"
	$(Q)$(BUILD)/m68kmake $(MUSASHI_DIR) $(MUSASHI_DIR)/m68k_in.c

$(MUSASHI_OBJS): $(OBJ)/%.o: $(MUSASHI_DIR)/%.c | $(OBJ) $(MUSASHI_DIR)/m68kops.h
	@echo "  CC      $<"
	$(Q)$(CC) $(KCFLAGS) -w $(MUSASHI_DEF) -Iemulation -I$(MUSASHI_DIR) \
	    -c $< -o $@

$(GLUE_OBJ): emulation/uaos_m68k_glue.c | $(OBJ) $(MUSASHI_DIR)/m68kops.h
	@echo "  CC      $<"
	$(Q)$(CC) $(KCFLAGS) $(MUSASHI_DEF) -Iemulation -Ikernel -I$(MUSASHI_DIR) \
	    -c $< -o $@

# --- Kernel objects ------------------------------------------------------------

$(ASM_OBJS): $(OBJ)/%.o: %.asm | $(OBJ)
	@echo "  NASM    $<"
	$(Q)$(NASM) -f elf64 $< -o $@

$(KERNEL_OBJS): $(OBJ)/%.o: %.c | $(OBJ)
	@echo "  CC      $<"
	$(Q)$(CC) $(KCFLAGS) $(KINC) $(KEXTRA) -c $< -o $@

# ntp.c uses doubles; SSE is not context-switched so it must not be used.
$(OBJ)/ntp.o: KEXTRA := -mno-sse

# Embedded binaries: <name> -> <name>_bin.c/.h generated next to the binary
# (gitignored), then compiled into build/obj/<name>_bin.o.
$(BINARIES_DIR)/%_bin.c $(BINARIES_DIR)/%_bin.h: $(BINARIES_DIR)/%
	@echo "  EMBED   $*"
	$(Q)sz=$$(wc -c < $<); \
	guard=UAOS_BIN_$$(printf '%s' '$*' | tr '[:lower:]' '[:upper:]')_H; \
	{ \
	  echo '/* Auto-generated by Makefile — do not edit */'; \
	  echo "#ifndef $$guard"; \
	  echo "#define $$guard"; \
	  echo '#include <stdint.h>'; \
	  echo "extern const uint8_t  g_bin_$*[];"; \
	  echo "extern const uint32_t g_bin_$*_size;"; \
	  echo '#endif'; \
	} > $(BINARIES_DIR)/$*_bin.h; \
	{ \
	  printf '/* Auto-generated by Makefile — do not edit */\n#include <stdint.h>\n'; \
	  printf 'const uint8_t g_bin_%s[] = {\n' '$*'; \
	  xxd -i < $<; \
	  printf '};\nconst uint32_t g_bin_%s_size = %u;\n' '$*' "$$sz"; \
	} > $(BINARIES_DIR)/$*_bin.c

$(BIN_OBJS): $(OBJ)/%_bin.o: $(BINARIES_DIR)/%_bin.c | $(OBJ)
	@echo "  CC      $<"
	$(Q)$(CC) $(KCFLAGS) -c $< -o $@

# The first 45 lines of emulation/uaos_emu_registry.c are a template header;
# the registry is regenerated from whatever binaries exist plus the file's
# tail (line 46 onward: helpers + lookup + RunByName).
# $(BINARIES_DIR) itself is a prerequisite so that adding/removing a binary
# (which changes the directory mtime) regenerates the table; the cmp+move
# keeps the output mtime stable when nothing actually changed.
$(REGISTRY_GEN): emulation/uaos_emu_registry.c $(BINARIES_DIR) $(BIN_SRCS) | $(OBJ)
	@echo "  GEN     $@"
	$(Q){ \
	  echo '/* Auto-generated by Makefile — do not edit */'; \
	  echo '#include "$(CURDIR)/emulation/uaos_emu.h"'; \
	  echo '#include <stdint.h>'; \
	  echo '#include <stddef.h>'; \
	  $(foreach n,$(BIN_NAMES),\
	    echo 'extern const uint8_t  g_bin_$(n)[];'; \
	    echo 'extern const uint32_t g_bin_$(n)_size;';) \
	  echo 'typedef struct { const char *name; const uint8_t *data; uint32_t size; } EmbeddedProgram;'; \
	  echo 'static const EmbeddedProgram k_programs[] = {'; \
	  $(foreach n,$(BIN_NAMES),\
	    printf '    { "%s", g_bin_%s, %sU },\n' '$(n)' '$(n)' \
	      "$$$$(wc -c < $(BINARIES_DIR)/$(n))";) \
	  $(foreach n,$(BIN_NAMES),\
	    $(if $(filter-out $(call lc,$n),$n),\
	      printf '    { "%s", g_bin_%s, %sU },\n' '$(call lc,$n)' '$(n)' \
	        "$$$$(wc -c < $(BINARIES_DIR)/$(n))";)) \
	  echo '    { NULL, NULL, 0 }'; \
	  echo '};'; \
	  tail -n +46 emulation/uaos_emu_registry.c; \
	} > $@.tmp
	$(Q)if cmp -s $@.tmp $@ 2>/dev/null; then rm -f $@.tmp; else mv $@.tmp $@; fi

$(REGISTRY_OBJ): $(REGISTRY_GEN) | $(OBJ)
	@echo "  CC      $<"
	$(Q)$(CC) $(KCFLAGS) $(MUSASHI_DEF) -Iemulation -I$(MUSASHI_DIR) \
	    -I$(OBJ) -c $< -o $@

# Boot splash — RGB blob wrapped via ld -r -b binary; the cd keeps the symbol
# name _binary_splash_rgb_* that kernel/display/splash.c references.  Falls
# back to a 1x1 navy placeholder when the artwork or Pillow is unavailable.
$(OBJ)/splash.rgb: tools/make_splash.py $(SPLASH_SRC) | $(OBJ)
	@echo "  SPLASH  $@"
	$(Q)if [ -f assets/splash.jpg ] && \
	    $(PYTHON) tools/make_splash.py assets/splash.jpg $@; then \
	    :; \
	else \
	    printf 'SPL0\001\000\000\000\001\000\000\000\040\016\012\000\012\016\040' > $@; \
	fi

$(SPLASH_OBJ): $(OBJ)/splash.rgb | $(OBJ)
	@echo "  LDR     $@"
	$(Q)cd $(OBJ) && $(LD) -r -b binary splash.rgb -o splash_img.o

# --- Kernel link ----------------------------------------------------------------

KERNEL_ALL_OBJS := $(ASM_OBJS) $(KERNEL_OBJS) $(MUSASHI_OBJS) $(GLUE_OBJ) \
                   $(REGISTRY_OBJ) $(BIN_OBJS) $(SPLASH_OBJ)

$(KERNEL_ELF): $(KERNEL_LD) $(KERNEL_ALL_OBJS) | $(STAMPS)/uitest
	@echo "  LD      $@"
	$(Q)$(LD) -z noexecstack -T $< $(filter %.o,$^) -o $@
	@file $@ | grep -q 'ELF 64-bit' \
	    || { echo "[FATAL] kernel is not a valid ELF64"; exit 1; }
	@echo "  kernel: $$(du -h $@ | cut -f1)"

# --- SYS_ROOT generated binaries ------------------------------------------------

$(SYSROOT)/LIBS/powerpacker.library: $(GEN_M68KLIB) | $(DIRS_STAMP)
	@echo "  GEN     LIBS:powerpacker.library"
	$(Q)$(GEN_M68KLIB) powerpacker.library 1 4 $@

$(NATIVE_BINS): $(SYSROOT)/C/%: $(GEN_NATIVE) | $(DIRS_STAMP)
	$(Q)$(GEN_NATIVE) $* $@

$(TOOL_BINS): $(SYSROOT)/Tools/%: $(GEN_NATIVE) | $(DIRS_STAMP)
	$(Q)$(GEN_NATIVE) $$(printf '%s' '$*' | tr '[:upper:]' '[:lower:]') $@

$(UTIL_BINS): $(SYSROOT)/Utilities/%: $(GEN_NATIVE) | $(DIRS_STAMP)
	$(Q)$(GEN_NATIVE) $$(printf '%s' '$*' | tr '[:upper:]' '[:lower:]') $@

$(PREF_BINS): $(SYSROOT)/Prefs/%: $(GEN_NATIVE) | $(DIRS_STAMP)
	$(Q)$(GEN_NATIVE) $$(printf '%s' '$*' | tr '[:upper:]' '[:lower:]') $@

# Wrapped Amiga Hunk binaries -> C:<lowercase name> (skipped when a userspace
# program or the bas download produces the same name — they win).
define M68K_WRAP_RULE
$(SYSROOT)/C/$(call lc,$(1)): $(BINARIES_DIR)/$(1) $(GEN_M68K) | $(DIRS_STAMP)
	@echo "  WRAP    C:$(call lc,$(1))"
	$(Q)$$(GEN_M68K) $(1) $$< $$@
endef
$(foreach b,$(BIN_NAMES),\
  $(if $(filter $(call lc,$b),$(C_FROM_WRAP)),\
    $(eval $(call M68K_WRAP_RULE,$b))))

# bas script (ACE Basic driver) — downloaded once, like the old script's
# "only if missing" check.
ifdef NEED_BAS
$(SYSROOT)/C/bas: | $(DIRS_STAMP)
	$(Q)$(call FETCH,https://raw.githubusercontent.com/mdbergmann/ACEBasic/master/bin/bas)
endif

# --- Userspace programs ---------------------------------------------------------

$(OBJ)/uaos_start.o: $(LIBUAOS)/uaos_start.c $(wildcard $(LIBUAOS)/*.h) | $(OBJ)
	@echo "  CC      $<"
	$(Q)$(CC) $(filter-out -pie,$(UCFLAGS)) -I$(LIBUAOS) -c $< -o $@

# uitree/uiformat are shared with the kernel — rebuilt PIE for userspace.
$(USPOBJ)/uitree.o: kernel/display/uitree.c $(wildcard kernel/display/*.h) | $(USPOBJ)
	@echo "  CC      $< (userspace)"
	$(Q)$(CC) $(UCFLAGS) -Ikernel/display -c $< -o $@

$(USPOBJ)/uiformat.o: kernel/display/uiformat.c $(wildcard kernel/display/*.h) | $(USPOBJ)
	@echo "  CC      $< (userspace)"
	$(Q)$(CC) $(UCFLAGS) -Ikernel/display -c $< -o $@

$(USER_OBJS): $(USPOBJ)/%.o: $(USERSPACE)/%.c $(wildcard $(LIBUAOS)/*.h) | $(USPOBJ)
	@echo "  CC      $< (userspace)"
	$(Q)$(CC) $(UCFLAGS) -I$(LIBUAOS) -Ikernel/display -c $< -o $@

ifneq ($(HAVE_GUIDE_SRC),)
$(USPOBJ)/guide.o: $(USERSPACE)/guide.c $(wildcard $(LIBUAOS)/*.h) | $(USPOBJ)
	@echo "  CC      $< (userspace)"
	$(Q)$(CC) $(UCFLAGS) -I$(LIBUAOS) -c $< -o $@
endif

$(USER_ELFS): $(USPOBJ)/%: $(USPOBJ)/%.o $(OBJ)/uaos_start.o \
              $(USPOBJ)/uitree.o $(USPOBJ)/uiformat.o
	@echo "  ULD     $@"
	$(Q)$(CC) $(ULINK) -o $@ $^

$(USER_BINS): $(SYSROOT)/C/%: $(USPOBJ)/% $(GEN_X64) | $(DIRS_STAMP)
	@echo "  WRAP    C:$*"
	$(Q)$(GEN_X64) $* $< $@

ifneq ($(HAVE_GUIDE_SRC),)
# Guide links without uitree/uiformat.
$(GUIDE_ELF): $(USPOBJ)/guide.o $(OBJ)/uaos_start.o
	@echo "  ULD     $@"
	$(Q)$(CC) $(ULINK) -o $@ $^

$(GUIDE_BIN): $(GUIDE_ELF) $(GEN_X64) | $(DIRS_STAMP)
	@echo "  WRAP    Tools:Guide"
	$(Q)$(GEN_X64) Guide $< $@
endif

ifneq ($(GUIDE_SRC),)
$(GUIDE_DB): documentation/uaos.guide | $(DIRS_STAMP)
	$(Q)cp $< $@
endif

# --- BearSSL (TLS client subset for wget/curl) ---------------------------------

ifneq ($(BEARSSL_SRCS),)
$(BEARSSL_OBJS): $(BOBJ)/%.o: %.c | $(BOBJ)
	@echo "  CC      $< (bearssl)"
	$(Q)$(CC) $(BCFLAGS) -c $< -o $@

$(BEARSSL_LIB): $(BEARSSL_OBJS)
	@echo "  AR      $@"
	$(Q)$(AR) rcs $@ $^

$(USPOBJ)/gnu_wget.o $(USPOBJ)/gnu_curl.o: GEXTRA_INC := -I$(BEARSSL_DIR)/inc
$(USPOBJ)/gnu_wget $(USPOBJ)/gnu_curl: $(BEARSSL_LIB)
$(USPOBJ)/gnu_wget $(USPOBJ)/gnu_curl: GEXTRA_LIB := $(BEARSSL_LIB)
endif

# --- GNU coreutils -------------------------------------------------------------

$(GNU_OBJS): $(USPOBJ)/gnu_%.o: $(GNUSRC)/%.c $(wildcard $(LIBUAOS)/*.h) | $(USPOBJ)
	@echo "  CC      $< (gnu)"
	$(Q)$(CC) $(UCFLAGS) -I$(LIBUAOS) $(GEXTRA_INC) -c $< -o $@

$(GNU_ELFS): $(USPOBJ)/gnu_%: $(USPOBJ)/gnu_%.o $(OBJ)/uaos_start.o
	@echo "  ULD     $@"
	$(Q)$(CC) $(ULINK) -o $@ $(filter %.o,$^) $(GEXTRA_LIB)

$(GNU_BINS): $(SYSROOT)/gnu/usr/bin/%: $(USPOBJ)/gnu_% $(GEN_X64) | $(DIRS_STAMP)
	$(Q)$(GEN_X64) $* $< $@

# --- vasm/vlink toolchain + M68k demos -------------------------------------------
# Toolchain is fetched and built lazily — only when a demo source exists.

$(VASM_DIR)/vasm.tar.gz: | $(VASM_DIR)
	$(Q)$(call FETCH,http://sun.hasenbraten.de/vasm/release/vasm.tar.gz)

$(VASM_DIR)/vlink.tar.gz: | $(VASM_DIR)
	$(Q)$(call FETCH,http://sun.hasenbraten.de/vlink/release/vlink.tar.gz)

$(VASM_STAMP): $(VASM_DIR)/vasm.tar.gz $(VASM_DIR)/vlink.tar.gz | $(STAMPS)
	@echo "  TOOLS   vasm-m68k + vlink"
	$(Q)cd $(VASM_DIR) && tar xzf vasm.tar.gz && tar xzf vlink.tar.gz
	$(Q)$(MAKE) -C $(VASM_DIR)/vasm CPU=m68k SYNTAX=mot >/dev/null 2>&1
	$(Q)$(MAKE) -C $(VASM_DIR)/vlink >/dev/null 2>&1
	$(Q)cp $(VASM_DIR)/vasm/vasmm68k_mot $(VASM_BIN)
	$(Q)touch $@

ifneq ($(DEMOS),)
$(BUILD)/%.o: system/Demos/%.s $(VASM_STAMP)
	@echo "  VASM    $<"
	$(Q)$(VASM_BIN) -Fhunk -o $@ $<

$(BUILD)/%.hunk: $(BUILD)/%.o
	@echo "  VLINK   $@"
	$(Q)$(VLINK_BIN) -bamigahunk -o $@ $<

$(DEMO_BINS): $(SYSROOT)/Demos/%: $(BUILD)/%.hunk $(GEN_M68K) | $(DIRS_STAMP)
	@echo "  WRAP    Demos:$*"
	$(Q)$(GEN_M68K) $* $< $@
endif

# --- Regina Rexx ------------------------------------------------------------------

$(REGINA_LHA): | $(BUILD)
	$(Q)$(call FETCH,http://aminet.net/dev/lang/Regina.lha)

$(REGINA_STAMP): $(REGINA_LHA) | $(STAMPS)
	@echo "  EXTRACT Regina.lha"
	$(Q)cd $(BUILD) && $(LHA) x Regina.lha >/dev/null 2>&1
	$(Q)touch $@

$(REXX_BIN): $(REGINA_STAMP) $(GEN_M68K) | $(DIRS_STAMP)
	@echo "  WRAP    REXX:rexx"
	$(Q)bin=$$(find $(REGINA_DIR) -name rexx -type f | head -1); \
	[ -n "$$bin" ] || { echo "[FATAL] rexx binary not found in $(REGINA_DIR)"; exit 1; }; \
	$(GEN_M68K) rexx "$$bin" $@

# --- ACE Basic --------------------------------------------------------------------

$(ACE_LHA): | $(BUILD)
	$(Q)$(call FETCH,https://github.com/mdbergmann/ACEBasic/releases/download/3.0.1/ace-basic.lha)

$(ACE_XSTAMP): $(ACE_LHA) | $(STAMPS)
	@echo "  EXTRACT ace-basic.lha"
	$(Q)cd $(BUILD) && $(LHA) x ace-basic.lha >/dev/null 2>&1
	$(Q)touch $@

$(ACE_STAMP): $(ACE_XSTAMP) $(GEN_M68K) | $(STAMPS) $(DIRS_STAMP)
	@echo "  STAGE   ACE Basic"
	$(Q)for subdir in lib bmaps include submods; do \
	    if [ -d "$(ACE_DIR)/$$subdir" ]; then \
	        cp -r "$(ACE_DIR)/$$subdir/"* "$(SYSROOT)/ACE/$$subdir/" 2>/dev/null || true; \
	    fi; \
	done
	$(Q)for tool in ace yap vasmm68k_mot vlink parseusing; do \
	    bin=$$(find "$(ACE_DIR)" -name "$$tool" -type f | head -1); \
	    if [ -n "$$bin" ]; then \
	        $(GEN_M68K) "$$tool" "$$bin" "$(SYSROOT)/ACE/bin/$$tool"; \
	    fi; \
	done
	$(Q)for lic in LICENSE COPYING COPYING-LIB; do \
	    if [ -f "$(ACE_DIR)/$$lic" ]; then \
	        cp "$(ACE_DIR)/$$lic" "$(SYSROOT)/ACE/$$lic"; break; \
	    fi; \
	done
	$(Q)touch $@

# --- system/ file copies -----------------------------------------------------------
# One ordered recipe: it must run after every generated SYS_ROOT file exists
# so that system/ overlays keep their old "last writer wins" semantics.

$(SYSROOT_STAMP): $(SYSROOT_CONTENTS) $(SYS_FILES) $(DEMO_EXTRAS) | $(DIRS_STAMP) $(STAMPS)
	@echo "  STAGE   system/ -> SYS_ROOT"
	$(Q)cp system/Startup-Sequence $(SYSROOT)/S/Startup-Sequence
	$(Q)for d in $(SYS_SUBDIRS); do \
	    if [ -d "system/$$d" ]; then \
	        cp -r "system/$$d/"* "$(SYSROOT)/$$d/" 2>/dev/null || true; \
	    fi; \
	done
	$(Q)if [ -d system/Demos ]; then \
	    for f in system/Demos/*; do \
	        if [[ -f "$$f" && "$$f" != *.s ]]; then \
	            cp "$$f" "$(SYSROOT)/Demos/"; \
	        fi; \
	    done; \
	fi
	$(Q)touch $@

# --- sysroot multiboot module (ISO9660 image of SYS_ROOT) -------------------------
# s:os-release carries the build serial (UAOS-ddmmyyhhmmss) so a running
# system can be matched to the ISO that produced it (UAOS-283).  FORCE keeps
# the serial fresh: the sysroot image — and therefore the ISO — is repacked
# on every `make` run, and the stamp lands in both the module image and the
# ISO-visible SYS_ROOT/S/ (the same staged tree feeds both).

$(SYSROOT_IMG): $(SYSROOT_STAMP) FORCE
	@echo "  MKISOFS $@"
	@test -n "$(MKISOFS)" || { echo "[FATAL] xorriso/genisoimage/mkisofs not found"; exit 1; }
	$(Q)printf 'UAOS-%s\n' "$$(date '+%d%m%y%H%M%S')" > $(SYSROOT)/S/os-release
	$(Q)$(MKISOFS) -o $@ -V UAOS_SYSROOT -r -J -iso-level 3 -graft-points \
	    SYS_ROOT=$(SYSROOT)
	@du -h $@ | sed 's/^/  /'

# --- ISO staging files -------------------------------------------------------------

$(KERNEL_BIN): $(KERNEL_ELF) | $(DIRS_STAMP)
	$(Q)cp $< $@

$(GRUB_STAGED): scripts/grub.cfg | $(DIRS_STAMP)
	$(Q)cp $< $@

$(KICK_STAGED): $(KICK_SRC) | $(DIRS_STAMP)
	$(Q)if [ -n "$(KICK_SRC)" ]; then cp $(KICK_SRC) $@; \
	    else echo '# placeholder' > $@; fi

ifneq ($(SPLASH_SRC),)
$(SPLASH_STAGED): $(SPLASH_SRC) | $(DIRS_STAMP)
	$(Q)cp $< $@
endif

ifneq ($(GUIDE_SRC),)
$(DOC_STAGED): $(GUIDE_SRC) | $(DIRS_STAMP)
	$(Q)cp $< $@
endif

# Self-contained EFI GRUB image with multiboot2 baked in — avoids the EFI
# GRUB needing to load .mod files from the ISO at runtime.
ifneq ($(HAVE_EFI),)
$(EFI_STAGED): scripts/grub.cfg | $(DIRS_STAMP)
	@echo "  EFI     $@"
	$(Q)grub-mkstandalone --format=x86_64-efi \
	    --output=$(BUILD)/bootx64.efi \
	    --modules="normal multiboot2 ls cat echo all_video serial" \
	    "boot/grub/grub.cfg=$(CURDIR)/scripts/grub.cfg"
	$(Q)cp $(BUILD)/bootx64.efi $@
endif

# --- Final hybrid ISO ----------------------------------------------------------------

$(ISO): $(KERNEL_BIN) $(GRUB_STAGED) $(KICK_STAGED) $(SYSROOT_IMG) $(OPT_STAGED)
	@echo "  CHECK   staging tree"
	$(Q)missing=0; \
	for f in boot/grub/grub.cfg boot/uaos-kernel.bin boot/uaos-sysroot.img \
	         boot/kickstart.conf SYS_ROOT/S/Startup-Sequence; do \
	    if [ ! -f "$(ISO_STAGING)/$$f" ]; then \
	        echo "  MISSING: $$f"; missing=1; \
	    fi; \
	done; \
	[ $$missing -eq 0 ] || exit 1
	@test -n "$(GRUB_MKRESCUE)" || { \
	    echo "[FATAL] grub-mkrescue not found (apt install grub-pc-bin grub-common xorriso)"; \
	    exit 1; }
ifeq ($(HAVE_EFI),)
	@echo "  WARN    grub-mkstandalone/x86_64-efi unavailable — EFI boot disabled"
endif
	@echo "  MKRESCUE $@"
	$(Q)$(GRUB_MKRESCUE) \
	    --output=$@ \
	    --modules="normal multiboot2 iso9660 ls cat echo gfxterm all_video serial" \
	    --compress=no \
	    $(ISO_STAGING)
	@echo "────────────────────────────────────────────────"
	@echo "  ISO build successful: $@ ($$(du -h $@ | cut -f1))"
	@echo "  Test: qemu-system-x86_64 -cdrom $@ -m 512M -boot d"

# --- Housekeeping --------------------------------------------------------------------

clean:
	rm -rf $(BUILD)

distclean: clean
	rm -f $(BINARIES_DIR)/*_bin.c $(BINARIES_DIR)/*_bin.h
	rm -f $(MUSASHI_DIR)/m68kops.c $(MUSASHI_DIR)/m68kops.h

# Auto-generated header dependency files (-MMD -MP).
DEPFILES := $(KERNEL_ALL_OBJS:.o=.d) $(USER_OBJS:.o=.d) $(GNU_OBJS:.o=.d) \
            $(BEARSSL_OBJS:.o=.d) $(OBJ)/uaos_start.d \
            $(USPOBJ)/uitree.d $(USPOBJ)/uiformat.d $(USPOBJ)/guide.d
-include $(DEPFILES)
