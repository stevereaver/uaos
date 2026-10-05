/* uaos_m68k_glue.c — UAOS Musashi integration layer
 *
 * Provides:
 *   - Flat 16 MB guest RAM (8 MB chip + 8 MB fast) with a simple bump allocator
 *   - Musashi memory read/write callbacks
 *   - ILLEGAL opcode handler → AmigaOS library vector dispatch
 *   - TRAP #1 handler → DOS I/O (Write/Output etc.)
 *   - Amiga Hunk binary loader (HUNK_CODE/DATA/BSS/RELOC32)
 *   - Minimal exec.library stubs (AllocMem, FreeMem, OpenLibrary, CloseLibrary)
 *   - Minimal dos.library stubs (Output, Write, Open, Close, Read, Exit)
 *   - Public API: UAOS_Emu_LoadAndRun(binary, size, argv, shell_print_fn)
 *
 * Memory map (within the 16 MB guest window):
 *   0x000000–0x000100   Exception vectors (minimal: SSP at 0, PC at 4)
 *   0x000100–0x000200   Library jump table stubs (ILLEGAL + lib_id word)
 *   0x000200–0x001000   Stack (grows down from 0x001000)
 *   0x001000–0xFFFFFF   Program segments loaded by Hunk loader
 *                       First 8 MB = chip RAM, second 8 MB = fast RAM
 *
 * Library dispatch:
 *   Each library function is represented by a 4-byte stub at a fixed address:
 *     ILLEGAL  (0x4AFC)
 *     dc.w lib_id      (high byte = lib, low byte = func index)
 *   The ILLEGAL callback reads these two words to identify the call.
 */

#define MUSASHI_CNF "uaos_m68kconf.h"

#include "musashi/m68k.h"
#include <stdint.h>
#include <stddef.h>
#include "uaos_emu.h"
#include "chipset/chip_emu.h"
#include "chipset/chiptrace.h"
#include "dos/vfs.h"
#include "dos/handler.h"
#include "dos/handle_table.h"
#include "dos/dospacket.h"
#include "dos/amiga_dos_types.h"
#include "exec/rom_modules.h"
#include "exec/task.h"

/* strace hooks (kernel/shell/cmd_strace.c) — emit M68k libcall records into
 * klog when a trace window is active.  Cheap gate: IsEnabled() returns a
 * static flag, so this costs one function call per ILLEGAL dispatch. */
extern int  Strace_IsEnabled(void);
extern void Strace_M68kEntry(uint8_t lib, uint8_t fn, M68kCPUState *cpu);
extern void Strace_M68kExit(uint8_t lib, uint8_t fn, int32_t result);

/* Forward decls for the instr hook's low-reentry diagnostic (defined below). */
static void     emu_print(const char *s);
static void     u32_hex(uint32_t v, char *buf);
static uint32_t guest_read_be32(uint32_t addr);

/* Debug: control-flow edge ring (M68K_INSTRUCTION_HOOK).  Records only
 * discontinuities — the destination is stored with bit0 set (PCs are always
 * even) preceded by the source — so a crash-driven linear march through
 * data doesn't flush the useful edges.  The first PC that escapes all code
 * regions (<0x10000) is captured in g_m68k_first_wild_pc. */
#define M68K_PC_RING_SZ 256
uint32_t g_m68k_pc_ring[M68K_PC_RING_SZ];
int      g_m68k_pc_ring_idx = 0;
uint32_t g_m68k_first_wild_pc = 0;
uint32_t g_m68k_first_wild_prev = 0;
static int g_m68k_saw_app_code = 0;   /* pc >= 0x800000 seen (post-decrunch) */
static int g_m68k_low_reentry = 0;    /* first wild re-entry logged          */

/* Amiga custom chip/CIA register window — declared early for the instr
 * hook's wild-PC check (the full comment lives with the memory callbacks). */
#define CHIP_WINDOW_START 0x00B00000u
#define CHIP_WINDOW_END   0x00DFFFFFu

/* Wild-PC circuit breaker.  When the guest branches outside its 16 MB
 * window (or to address 0) the memory callbacks only return 0xFF — the CPU
 * then marches through data executing store instructions that shred the
 * whole arena (observed: OctaMED's decrunched image wiped to zeros inside
 * m68k_isr_call's 4M-cycle guard loop).  Ending the timeslice on the first
 * out-of-window instruction bounds the damage to a handful of cycles and —
 * inside host-invoked hook/ISR calls — lets the caller restore the saved
 * context instead of running wild until the guard expires. */
int g_m68k_wild_abort = 0;

/* Defined near the hook machinery below — used by the instr hook to decide
 * whether a wild PC is fatal (main execution) or just aborts the current
 * host-invoked hook/ISR call (context is restored on return). */
static int g_hook_nest_level;

/* Reset all instruction-hook state between M68k tasks — these globals are
 * shared by every task using the glue, so without this a second launch
 * inherits saw_app_code=1 and the decruncher's legitimate 0x2xxxx PCs
 * look like wild re-entries.  Also clears the hook's `prev` so task N+1's
 * first instruction doesn't record an edge from task N's last PC. */
static uint32_t g_m68k_hook_prev = 0;

void m68k_reset_wild_state(void)
{
    g_m68k_first_wild_pc   = 0;
    g_m68k_first_wild_prev = 0;
    g_m68k_pc_ring_idx     = 0;
    g_m68k_saw_app_code    = 0;
    g_m68k_low_reentry     = 0;
    g_m68k_wild_abort      = 0;
    g_m68k_hook_prev       = 0;
}

void uaos_m68k_instr_hook(unsigned int pc)
{
    uint32_t prev = g_m68k_hook_prev;
    /* Wild floor is the exception-vector page: with EXEC_BASE at 0x1000
     * the deepest LVO stub (-996) sits at 0xC1C, so no legitimate code
     * executes below 0x100.  (While EXEC_BASE was 0x300 the floor had to
     * be 0x40 — deep exec stubs lived inside the vector page itself.) */
    if (pc >= GUEST_RAM_SIZE || pc < 0x100u ||
        (pc >= CHIP_WINDOW_START && pc <= CHIP_WINDOW_END)) {
        g_m68k_wild_abort = 1;
        /* A PC outside the 16 MB window can never come back.  Inside a
         * host-invoked hook/ISR call, abort+context-restore recovers — but
         * in main execution there is no way back, so halt the task outright
         * instead of letting it march data-as-code between timeslice aborts
         * (that path previously shredded the arena and once panicked the
         * host with a corrupted Musashi context). */
        if (!g_hook_nest_level) g_emu_halted = 1;
        static int wild_logs = 0;
        if (wild_logs < 8) {
            wild_logs++;
            char b[64]; int i = 0;
            const char *t = "[m68k] WILD-PC pc=0x";
            while (t[i]) { b[i] = t[i]; i++; }
            char n[12]; u32_hex(pc, n); int j = 0;
            while (n[j] && i < 60) b[i++] = n[j++];
            t = " from=0x"; j = 0; while (t[j]) b[i++] = t[j++];
            u32_hex(prev, n); j = 0;
            while (n[j] && i < 60) b[i++] = n[j++];
            b[i++] = '\n'; b[i] = '\0';
            emu_print(b);
        }
        m68k_end_timeslice();
    }
    uint32_t d = pc - prev;
    if (d < 2 || d > 8) {
        int i = g_m68k_pc_ring_idx;
        g_m68k_pc_ring[i & (M68K_PC_RING_SZ - 1)] = prev;
        g_m68k_pc_ring[(i + 1) & (M68K_PC_RING_SZ - 1)] = pc | 1;
        g_m68k_pc_ring_idx = i + 2;
    }
    if (pc >= 0x800000u)
        g_m68k_saw_app_code = 1;
    else if (g_m68k_saw_app_code && g_m68k_low_reentry < 4 &&
             pc >= 0x20000u && pc < 0x80000u) {
        /* After the decrunched program is running in fast RAM, execution
         * must never return to [0x20000,0x80000): that band holds the
         * dead packed input hunks.  Pool/generation addresses above
         * 0x80000 (including the 0x1EF000 hook-return trap and OctaMED's
         * generated code in chip RAM) are legitimate code.  Catch the
         * entry edge and the guest stack to identify the caller that
         * branched into dead data. */
        g_m68k_low_reentry++;
        char b[64]; int i = 0;
        const char *t = "[m68k] LOW-REENTRY pc=0x";
        while (t[i]) { b[i] = t[i]; i++; }
        char n[12]; u32_hex(pc, n); int j = 0;
        while (n[j] && i < 60) b[i++] = n[j++];
        t = " from=0x"; j = 0; while (t[j]) b[i++] = t[j++];
        u32_hex(prev, n); j = 0;
        while (n[j] && i < 60) b[i++] = n[j++];
        t = " sp=0x"; j = 0; while (t[j]) b[i++] = t[j++];
        uint32_t sp = (uint32_t)m68k_get_reg(NULL, M68K_REG_SP);
        u32_hex(sp, n); j = 0;
        while (n[j] && i < 62) b[i++] = n[j++];
        b[i++] = '\n'; b[i] = '\0';
        emu_print(b);
        /* Top-of-stack dwords — return-address candidates. */
        for (int k = 0; k < 12; k++) {
            i = 0; t = "[m68k]   stk+";
            while (t[i]) { b[i] = t[i]; i++; }
            u32_hex((uint32_t)(k * 4), n); j = 0;
            while (n[j] && i < 56) b[i++] = n[j++];
            t = " = 0x"; j = 0; while (t[j]) b[i++] = t[j++];
            uint32_t v = (sp + (uint32_t)(k*4) + 4 <= GUEST_RAM_SIZE)
                       ? guest_read_be32(sp + (uint32_t)(k*4)) : 0;
            u32_hex(v, n); j = 0;
            while (n[j] && i < 60) b[i++] = n[j++];
            b[i++] = '\n'; b[i] = '\0';
            emu_print(b);
        }
        /* Post-decrunch there is no valid code in this band — the CPU is
         * wild.  Kill the task now (outside a hook call): letting it
         * continue marches through data-as-code and shreds the arena. */
        g_m68k_wild_abort = 1;
        if (!g_hook_nest_level) g_emu_halted = 1;
        m68k_end_timeslice();
    }
    if (pc >= 0x10000u && !g_m68k_first_wild_pc) {
        g_m68k_first_wild_pc = pc;
        g_m68k_first_wild_prev = prev;
        int i = g_m68k_pc_ring_idx;
        g_m68k_pc_ring[i & (M68K_PC_RING_SZ - 1)] = prev;
        g_m68k_pc_ring[(i + 1) & (M68K_PC_RING_SZ - 1)] = pc | 1;
        g_m68k_pc_ring_idx = i + 2;
    }
    g_m68k_hook_prev = pc;
}

/* =========================================================================
 * Shell output callback — set by UAOS_Emu_LoadAndRun_Internal
 * ========================================================================= */

typedef void (*GluePrintFn)(const char *s);
GluePrintFn g_print = (void*)0;

/* Current working directory for resolving relative paths */
char g_uaos_cwd[64] = "RAM:";

static void emu_print(const char *s)
{
    extern void kprint(const char *);
    /* Diagnostics are serial-only: echoing them to the task's console via
     * g_print floods the launching shell window with trace lines and stalls
     * the launch for seconds per print-heavy burst.  Real guest stdout
     * (dos PutStr/FPuts/Printf paths in dos_lib.c) calls g_print directly,
     * so nothing user-facing is lost. */
    kprint(s);
}

/* Console echo — for messages the launching user must see (load/launch
 * failures).  Serial + the task's print hook. */
static void emu_cprint(const char *s)
{
    extern void kprint(const char *);
    kprint(s);
    if (g_print) g_print(s);
}

/* Guest memory base for BPTR-to-native conversion */
extern uint8_t *g_ram;

/* Convert BSTR BPTR to native C string into dst[max].
 * Returns length or 0 if invalid. */
static int bstr_to_c(uint32_t bptr_bptr, char *dst, int max)
{
    uint32_t addr = bptr_bptr << 2;
    if (addr >= GUEST_RAM_SIZE || max < 2) return 0;
    uint8_t len = g_ram[addr];
    if (len > (uint8_t)(max - 1)) len = (uint8_t)(max - 1);
    for (int i = 0; i < (int)len; i++) dst[i] = (char)g_ram[addr + 1 + i];
    dst[len] = '\0';
    return (int)len;
}

/* Extract volume name from a path like "RAM:dir/file" into dst[max].
 * Returns length or 0 if no colon. */
static int extract_vol_name(const char *path, char *dst, int max)
{
    int i = 0;
    while (path[i] && path[i] != ':' && i < max - 1) { dst[i] = path[i]; i++; }
    dst[i] = '\0';
    return (path[i] == ':') ? i : 0;
}

/* Append src to dst (up to max-1 chars total) */
static void scat(char *dst, const char *src, int max)
{
    int i = 0;
    while (dst[i] && i < max - 1) i++;
    int j = 0;
    while (i < max - 1 && src[j]) { dst[i++] = src[j++]; }
    dst[i] = '\0';
}

/* =========================================================================
 * Minimal no-libc helpers
 * ========================================================================= */

static int emu_strlen(const char *s) { int n=0; while(s[n]) n++; return n; }

static void emu_memset(void *d, int c, unsigned int n) {
    unsigned char *p = (unsigned char *)d;
    while (n--) *p++ = (unsigned char)c;
}

static void emu_memcpy(void *d, const void *s, unsigned int n) {
    unsigned char *dp = (unsigned char *)d;
    const unsigned char *sp = (const unsigned char *)s;
    while (n--) *dp++ = *sp++;
}

/* Convert uint32 to hex string into buf (8 hex digits + NUL) */
static void u32_hex(uint32_t v, char *buf) {
    const char *h = "0123456789ABCDEF";
    for (int i = 7; i >= 0; i--) { buf[i] = h[v & 0xF]; v >>= 4; }
    buf[8] = '\0';
}

static void u32_dec(uint32_t v, char *buf, int max) {
    char tmp[12]; int i=0, j=0;
    if (!v) { buf[j++]='0'; buf[j]='\0'; return; }
    while (v && i<11) { tmp[i++]=(char)('0'+v%10); v/=10; }
    while (i-- && j<max-1) buf[j++]=tmp[i];
    buf[j]='\0';
}

/* =========================================================================
 * Guest RAM
 * ========================================================================= */

/* Guest RAM layout (GUEST_RAM_SIZE is defined in uaos_emu.h as 16 MB):
 *   0x000000–0x000100   Exception vectors (minimal: SSP at 0, SysBase at 4)
 *   0x000100–0x000C1C   Low system band: heap list slots (0x10-0x1F),
 *                       legacy jump table (0x100), fake DOS handles (0x400),
 *                       library name/idstring arena (0x800)
 *   0x000C1C–0x001000   exec.library jump-table stubs (EXEC_BASE = 0x1000)
 *   0x001000–0x001280   ExecBase (SysBase) struct
 *   0x001C1C–0x002000   dos.library jump-table stubs (DOS_BASE = 0x2000)
 *   0x002F28–0x003000   bsdsocket.library stubs (BSD_BASE = 0x3000)
 *   0x007BE0–0x00A000   graphics/intuition/gadtools stubs + fake bases
 *   0x00A000–0x01A000   loadable .library blobs (16 x 4 KB)
 *   0x01B000–0x01B400   Process environment (Process/CLI/RDArgs/ports),
 *                       dedicated region outside the program heap (UAOS-237)
 *   0x020000–0x7F0000   Program segments + AllocMem(MEMF_CHIP) pool
 *                       (chip RAM, accessible by custom chips)
 *   0x7F0000–0x800000   Generated-library stub arena (genlibs)
 *   0x800000–0xFF0000   AllocMem(MEMF_FAST) pool (fast RAM)
 */
#define STACK_TOP       0x1F0000  /* top of guest stack — grows downward */
/* Program hunks load above the reserved system zone: exec/dos jump tables
 * live at 0xC1C-0x2000, LVO stub tables for graphics/intuition/gadtools at
 * 0x7BDC-0xA000, the lib base structs at 0x8000-0xA100, and loadable
 * .library blobs at 0xA000-0x1A000 (16 x 4K).  Loading a program at the
 * old 0x1000 overwrote the stubs — OctaMED's OpenScreenTagList jumped
 * into packed hunk data and ran wild. */
#define PROG_BASE       0x020000  /* program hunks load here */

static uint8_t g_default_ram[GUEST_RAM_SIZE] __attribute__((section(".guest_ram"), aligned(4096)));
uint8_t *g_ram = g_default_ram;
/* The "system" guest RAM context — the buffer host-side code (Intuition
 * rendering, chipset DMA, console echo) may legitimately write to when no
 * M68k task is current.  do_schedule() rebinds g_ram to this when switching
 * to a non-M68k task so host writes never scribble on a suspended guest's
 * per-task address space. */
uint8_t *g_shared_ram = g_default_ram;
int      g_emu_halted   = 0;  /* set by dos_Exit to break the execute loop */
uint32_t g_cmdline_bptr = 0;  /* BPTR to CLI arg BSTR, set at startup */
uint64_t g_m68k_cycles  = 0;  /* cumulative M68k cycles executed */
int      g_chipset_sync_disabled = 0;  /* set during per-task M68k exec */

/* Bump allocator — starts after program load area.
 * Will be set to first free address after hunk loading. */
uint32_t g_uaos_heap_ptr = PROG_BASE;

/* Loaded-program hunk table — populated by hunk_load(), read by
 * UAOS_Emu_SetupProcess() to publish pr_SegList/cli_Module (UAOS-237). */
#define MAX_HUNKS  32
static uint32_t g_hunk_base[MAX_HUNKS];
static uint32_t g_hunk_blk[MAX_HUNKS];
static int      g_hunk_count = 0;

/* Allow the UAE bridge to point the glue layer at the 4 GB guest physical
 * window.  A NULL base is ignored so the static default RAM remains in use
 * when the bridge is unavailable (e.g. during freestanding validation builds). */
void UAOS_Glue_SetRamBase(uint8_t *base)
{
    if (base != NULL) {
        g_ram = base;
        g_shared_ram = base;
    }
}

static uint32_t heap_alloc(uint32_t size)
{
    /* Align to 4 bytes; program hunks stay in chip RAM (0x001000-0x7F0000). */
    size = (size + 3) & ~3u;
    if (g_uaos_heap_ptr + size > 0x007F0000u) return 0;
    uint32_t addr = g_uaos_heap_ptr;
    g_uaos_heap_ptr += size;
    emu_memset(g_ram + addr, 0, size);
    return addr;
}

/* =========================================================================
 * Guest-visible FileLock helpers
 * FileLock lives in guest RAM so M68k binaries can inspect / pass BPTRs.
 * Layout (16 bytes, big-endian, matching 32-bit AmigaOS):
 *   offset 0: fl_Key     (BPTR to handle table slot)
 *   offset 4: fl_Access  (SHARED_LOCK=-2 / EXCLUSIVE_LOCK=-1)
 *   offset 8: fl_Task    (handler marker, not a real guest pointer)
 *   offset 12: fl_Volume (DosList BPTR, 0 for now)
 * ========================================================================= */

static void guest_write_be32(uint32_t addr, uint32_t val)
{
    g_ram[addr + 0] = (uint8_t)(val >> 24);
    g_ram[addr + 1] = (uint8_t)(val >> 16);
    g_ram[addr + 2] = (uint8_t)(val >>  8);
    g_ram[addr + 3] = (uint8_t)(val      );
}

static uint32_t guest_read_be32(uint32_t addr)
{
    return ((uint32_t)g_ram[addr + 0] << 24)
         | ((uint32_t)g_ram[addr + 1] << 16)
         | ((uint32_t)g_ram[addr + 2] <<  8)
         | ((uint32_t)g_ram[addr + 3]      );
}

static void guest_write_be16(uint32_t addr, uint16_t val)
{
    g_ram[addr + 0] = (uint8_t)(val >> 8);
    g_ram[addr + 1] = (uint8_t)(val     );
}

static uint16_t guest_read_be16(uint32_t addr)
{
    return ((uint16_t)g_ram[addr + 0] << 8)
         | ((uint16_t)g_ram[addr + 1]     );
}

/* Allocate a FileLock in guest RAM and return its BPTR.
 * On failure returns 0 (no free store). */
static uint32_t guest_alloc_filelock(uint32_t handle, int32_t access)
{
    uint32_t addr = heap_alloc(sizeof(FileLock));
    if (!addr) return 0;
    guest_write_be32(addr + 0, handle);       /* fl_Key    */
    guest_write_be32(addr + 4, (uint32_t)access); /* fl_Access */
    guest_write_be32(addr + 8, 1);             /* fl_Task   = RAM handler marker */
    guest_write_be32(addr + 12, 0);            /* fl_Volume = 0 */
    return addr >> 2;  /* BPTR = addr >> 2 */
}

/* Read a FileLock from guest RAM.  Returns 0 if lock_bptr invalid. */
static int guest_read_filelock(uint32_t lock_bptr,
                                uint32_t *out_handle,
                                int32_t  *out_access)
{
    uint32_t addr = lock_bptr << 2;
    if (addr + sizeof(FileLock) > GUEST_RAM_SIZE) return 0;
    if (out_handle) *out_handle = guest_read_be32(addr + 0);
    if (out_access) *out_access = (int32_t)guest_read_be32(addr + 4);
    return 1;
}

/* =========================================================================
 * Musashi memory callbacks
 * ========================================================================= */

/* Route accesses to the Amiga custom chip/CIA window (0xB00000-0xDFFFFF —
 * CHIP_WINDOW_START/END defined near the top of this file) through the
 * chipset emulator.  This is the same range handled by the x86_64 page
 * fault handler for native code; M68k code must use it too, since the
 * Musashi emulator does not trigger host page faults. */
static inline int is_chip_window(unsigned int addr)
{
    return addr >= CHIP_WINDOW_START && addr <= CHIP_WINDOW_END;
}

unsigned int m68k_read_memory_8(unsigned int addr)
{
    if (is_chip_window(addr))
        return chip_emu_read(addr - CHIP_WINDOW_START, 1);
    if (addr < GUEST_RAM_SIZE) {
        if (addr < 0x00800000u && !g_chipset_sync_disabled) chip_emu_cpu_chipram_access(addr, 0);
        return g_ram[addr];
    }
    return 0xFF;
}

unsigned int m68k_read_memory_16(unsigned int addr)
{
    if (is_chip_window(addr) && addr + 1 <= CHIP_WINDOW_END)
        return chip_emu_read(addr - CHIP_WINDOW_START, 2);
    if (addr < GUEST_RAM_SIZE - 1) {
        if (addr < 0x00800000u && !g_chipset_sync_disabled) chip_emu_cpu_chipram_access(addr, 0);
        return ((unsigned int)g_ram[addr] << 8) | g_ram[addr+1];
    }
    return 0xFFFF;
}

unsigned int m68k_read_memory_32(unsigned int addr)
{
    if (is_chip_window(addr) && addr + 3 <= CHIP_WINDOW_END)
        return chip_emu_read(addr - CHIP_WINDOW_START, 4);
    if (addr < GUEST_RAM_SIZE - 3) {
        if (addr < 0x00800000u && !g_chipset_sync_disabled) chip_emu_cpu_chipram_access(addr, 0);
        return ((unsigned int)g_ram[addr]   << 24) |
               ((unsigned int)g_ram[addr+1] << 16) |
               ((unsigned int)g_ram[addr+2] <<  8) |
                (unsigned int)g_ram[addr+3];
    }
    return 0xFFFFFFFF;
}

/* Memory barrier to ensure M68k writes to guest RAM are visible to the
 * chipset emulator and instruction fetch before any subsequent reads.
 * On x86 this uses a full memory fence (mfence); for other host CPUs the
 * macro would need to be replaced with the appropriate cache-coherence
 * primitive. */
#define GUEST_WRITE_BARRIER() __asm__ volatile("mfence" ::: "memory")

void m68k_write_memory_8(unsigned int addr, unsigned int val)
{
    if (is_chip_window(addr)) {
        chip_emu_write(addr - CHIP_WINDOW_START, val, 1);
        GUEST_WRITE_BARRIER();
        return;
    }
    if (addr < GUEST_RAM_SIZE) {
        if (addr < 0x00800000u && !g_chipset_sync_disabled) chip_emu_cpu_chipram_access(addr, 1);
        g_ram[addr] = (uint8_t)val;
    }
    GUEST_WRITE_BARRIER();
}

void m68k_write_memory_16(unsigned int addr, unsigned int val)
{
    if (is_chip_window(addr) && addr + 1 <= CHIP_WINDOW_END) {
        chip_emu_write(addr - CHIP_WINDOW_START, val, 2);
        GUEST_WRITE_BARRIER();
        return;
    }
    if (addr < GUEST_RAM_SIZE - 1) {
        if (addr < 0x00800000u && !g_chipset_sync_disabled) chip_emu_cpu_chipram_access(addr, 1);
        g_ram[addr]   = (uint8_t)(val >> 8);
        g_ram[addr+1] = (uint8_t)(val);
    }
    GUEST_WRITE_BARRIER();
}

/* tar-binary compat hacks (UAOS legacy): SAS/C startup of the embedded
 * `tar` resident binary writes a bad stack limit to absolute 0x89EC and a
 * corrupt BPTR at PC 0x2770.  These rewrites corrupt ANY other m68k program
 * that happens to write those addresses — OctaMED's decruncher passes data
 * through 0x89EC — so they are gated to the tar binary by name. */
int g_m68k_tar_compat = 0;

static int m68k_name_is_tar(const char *n)
{
    if (!n) return 0;
    const char *base = n;
    for (const char *p = n; *p; p++)
        if (*p == '/' || *p == ':' || *p == '\\') base = p + 1;
    return (base[0] == 't' || base[0] == 'T') &&
           (base[1] == 'a' || base[1] == 'A') &&
           (base[2] == 'r' || base[2] == 'R') &&
           (base[3] == '\0' || base[3] == '.');
}

void UAOS_Emu_SetTarCompat(const char *name)
{
    g_m68k_tar_compat = m68k_name_is_tar(name);
}

void m68k_write_memory_32(unsigned int addr, unsigned int val)
{
    if (is_chip_window(addr) && addr + 3 <= CHIP_WINDOW_END) {
        chip_emu_write(addr - CHIP_WINDOW_START, val, 4);
        GUEST_WRITE_BARRIER();
        return;
    }
    if (addr < GUEST_RAM_SIZE - 3) {
        if (addr < 0x00800000u && !g_chipset_sync_disabled) chip_emu_cpu_chipram_access(addr, 1);
        if (g_m68k_tar_compat) {
            /* Fix: SAS/C startup writes a bad stack limit to PROG_BASE+0x79EC
             * (was 0x89EC when PROG_BASE was 0x1000) because the stack size
             * parameter on the stack is 0. Override with a safe limit. */
            if (addr == (PROG_BASE + 0x79ECu)) {
                uint32_t safe_limit = 0x1B0080; /* tc_SPLower (0x1B0000) + 128 */
                val = safe_limit;
            }
            /* Patch D1 at PC=PROG_BASE+0x1770 (was 0x2770) to use correct BPTR from _ufb[3] instead of corrupted 0x140 */
            uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC);
            if (pc == (PROG_BASE + 0x1770u)) {
                uint32_t d1 = m68k_get_reg(NULL, M68K_REG_D1);
                if (d1 == 0x00000140) {
                    /* Get correct BPTR from _ufb[3] */
                    uint32_t a4 = m68k_get_reg(NULL, M68K_REG_A4);
                    uint32_t ufb_base = a4 + 0x14F0;
                    uint32_t ufb_addr = ufb_base + 3 * 24;
                    uint16_t correct_bptr = m68k_read_memory_16(ufb_addr);
                    m68k_set_reg(M68K_REG_D1, correct_bptr);
                }
            }
        }
        g_ram[addr]   = (uint8_t)(val >> 24);
        g_ram[addr+1] = (uint8_t)(val >> 16);
        g_ram[addr+2] = (uint8_t)(val >>  8);
        g_ram[addr+3] = (uint8_t)(val);
    }
    GUEST_WRITE_BARRIER();
}

/* Disassembler uses these — just alias to the main ones */
unsigned int m68k_read_disassembler_8 (unsigned int addr) { return m68k_read_memory_8(addr); }
unsigned int m68k_read_disassembler_16(unsigned int addr) { return m68k_read_memory_16(addr); }
unsigned int m68k_read_disassembler_32(unsigned int addr) { return m68k_read_memory_32(addr); }

/* =========================================================================
 * Library jump table layout
 *
 * Address 0x100 + (lib_id * 64) + (func_idx * 4) holds a 4-byte stub:
 *   0x4AFC  ILLEGAL
 *   lib_id  (byte, high)  func_idx (byte, low) — packed as one 16-bit word
 *
 * lib_id values:
 *   1 = exec.library
 *   2 = dos.library
 * ========================================================================= */

#define JMPTAB_BASE     0x100
#define JMPTAB_LIB_SZ   80      /* 20 slots × 4 bytes per lib (DOS now has 17 fns) */

#define LIB_EXEC        1
#define LIB_DOS         2
#define LIB_BSDSOCKET   3
#define LIB_GRAPHICS    4
#define LIB_INTUITION   5
#define LIB_GADTOOLS    6
#define LIB_GENERIC     7   /* fake lib base — log lvo, return 0 */
#define LIB_UTILITY     8   /* utility.library → ROM dispatch */
#define LIB_AUDIODEV    9   /* audio.device → UAOS-240 arbitration */
#define LIB_IRQ        10   /* autovector dispatch stub — fn carries level */

#define AUDEV_LVO_OPEN    0   /* -6  */
#define AUDEV_LVO_CLOSE   1   /* -12 */
#define AUDEV_LVO_BEGINIO 2   /* -42 */
#define AUDEV_LVO_ABORTIO 3   /* -48 */

/* MEMF_* attribute bits (exec/memory.h) */
#define MEMF_PUBLIC      0x00000001u
#define MEMF_CHIP        0x00000002u
#define MEMF_FAST        0x00000004u
#define MEMF_24BITDMA    0x00020000u
#define MEMF_CLEAR_FLAG  0x00010000u

#define NT_LIBRARY       9

/* Allocator bridge implemented in kernel/exec/dos_lib.c */
extern void dos_AllocMem_glue(uint32_t size, uint32_t reqs, uint32_t *out_addr);
extern void dos_FreeMem_glue(uint32_t addr, uint32_t size);
extern void dos_AvailMem_glue(uint32_t attrs, uint32_t *total, uint32_t *largest);

/* Guest memory accessors (big-endian via Musashi callbacks) */
#define glue_r8(a)   ((uint32_t)m68k_read_memory_8(a))
#define glue_r16(a)  ((uint32_t)m68k_read_memory_16(a))
#define glue_r32(a)  ((uint32_t)m68k_read_memory_32(a))
#define glue_w8(a,v)  m68k_write_memory_8((a),(v))
#define glue_w16(a,v) m68k_write_memory_16((a),(v))
#define glue_w32(a,v) m68k_write_memory_32((a),(v))

/* exec.library function indices */
#define EXEC_OPEN_LIBRARY   1
#define EXEC_CLOSE_LIBRARY  2
#define EXEC_ALLOC_MEM      3
#define EXEC_FREE_MEM       4
#define EXEC_FIND_TASK      5
#define EXEC_WAIT           6
#define EXEC_SIGNAL         7
#define EXEC_SETSIGNAL      8
#define EXEC_ALLOC_SIGNAL   9
#define EXEC_FREE_SIGNAL   10
#define EXEC_PUT_MSG       11
#define EXEC_GET_MSG       12
#define EXEC_REPLY_MSG     13
#define EXEC_WAIT_PORT     14
#define EXEC_CACHE_CLEAR_U 15   /* CacheClearU — no-op (Musashi has no icache) */
#define EXEC_INIT_STRUCT       16
#define EXEC_DISABLE           17
#define EXEC_ENABLE            18
#define EXEC_FORBID            19
#define EXEC_PERMIT            20
#define EXEC_SUPER_STATE       21
#define EXEC_USER_STATE        22
#define EXEC_AVAIL_MEM         23
#define EXEC_ALLOC_ENTRY       24
#define EXEC_FREE_ENTRY        25
#define EXEC_INSERT            26
#define EXEC_ADD_HEAD          27
#define EXEC_ADD_TAIL          28
#define EXEC_REMOVE            29
#define EXEC_REM_HEAD          30
#define EXEC_REM_TAIL          31
#define EXEC_ENQUEUE           32
#define EXEC_FIND_NAME         33
#define EXEC_SET_TASK_PRI      34
#define EXEC_SET_EXCEPT        35
#define EXEC_ALLOC_TRAP        36
#define EXEC_FREE_TRAP         37
#define EXEC_ADD_PORT          38
#define EXEC_REM_PORT          39
#define EXEC_FIND_PORT         40
#define EXEC_OLD_OPEN_LIBRARY  41
#define EXEC_SET_FUNCTION      42
#define EXEC_OPEN_DEVICE       43
#define EXEC_CLOSE_DEVICE      44
#define EXEC_DO_IO             45
#define EXEC_SEND_IO           46
#define EXEC_CHECK_IO          47
#define EXEC_WAIT_IO           48
#define EXEC_ABORT_IO          49
#define EXEC_OPEN_RESOURCE     50
#define EXEC_GETCC             51
#define EXEC_TYPE_OF_MEM       52
#define EXEC_PROCURE           53
#define EXEC_VACATE            54
#define EXEC_INIT_SEMAPHORE    55
#define EXEC_OBTAIN_SEM        56
#define EXEC_RELEASE_SEM       57
#define EXEC_ATTEMPT_SEM       58
#define EXEC_COPY_MEM          59
#define EXEC_COPY_MEM_QUICK    60
#define EXEC_CACHE_CLEAR_E     61
#define EXEC_CACHE_CONTROL     62
#define EXEC_CREATE_IOREQUEST  63
#define EXEC_DELETE_IOREQUEST  64
#define EXEC_CREATE_MSGPORT    65
#define EXEC_DELETE_MSGPORT    66
#define EXEC_OBTAIN_SEM_SHARED 67
#define EXEC_ALLOC_VEC         68
#define EXEC_FREE_VEC          69
#define EXEC_OBTAIN_SEM_LIST   70
#define EXEC_RELEASE_SEM_LIST  71
#define EXEC_FIND_SEMAPHORE    72
#define EXEC_ADD_SEMAPHORE     73
#define EXEC_REM_SEMAPHORE     74
#define EXEC_SET_INT_VECTOR    75
#define EXEC_ADD_INT_SERVER    76
#define EXEC_REM_INT_SERVER    77
#define EXEC_CAUSE             78
#define EXEC_RAW_DO_FMT        79
#define EXEC_STACK_SWAP        80
#define EXEC_STUB_LVO      250  /* catch-all stub marker for unimplemented LVOs */

/* bsdsocket.library function indices */
#define BSD_FN_SOCKET        1
#define BSD_FN_BIND          2
#define BSD_FN_LISTEN        3
#define BSD_FN_ACCEPT        4
#define BSD_FN_CONNECT       5
#define BSD_FN_SEND          6
#define BSD_FN_SENDTO        7
#define BSD_FN_RECV          8
#define BSD_FN_RECVFROM      9
#define BSD_FN_CLOSESOCKET   10
#define BSD_FN_SETSOCKOPT    11
#define BSD_FN_GETSOCKOPT    12
#define BSD_FN_IOCTLSOCKET   13
#define BSD_FN_INET_ADDR     14
#define BSD_FN_INET_NTOA     15
#define BSD_FN_GETHOSTBYNAME 16

/* dos.library function indices */
#define DOS_OUTPUT          1
#define DOS_WRITE           2
#define DOS_OPEN            3
#define DOS_CLOSE           4
#define DOS_READ            5
#define DOS_EXIT            6
#define DOS_IO_ERR          7
#define DOS_INPUT           8
#define DOS_VFPRINTF        9
#define DOS_FPUTS          10
#define DOS_PUTSTR         11
#define DOS_VPRINTF        12
#define DOS_PRINTF         13
#define DOS_VFWRITEF       14
#define DOS_READARGS       15
#define DOS_GETARGSTR      16
#define DOS_ISINTERACTIVE  17
#define DOS_DELETEFILE     18
#define DOS_RENAME         19
#define DOS_SETPROTECTION  20
#define DOS_GETVAR         21
#define DOS_SETVAR         22
#define DOS_SEEK           23
#define DOS_LOCK           24
#define DOS_UNLOCK         25
#define DOS_EXAMINE        26
#define DOS_EXAMINE_NEXT   27
#define DOS_CREATE_DIR     28
#define DOS_DUPLOCK        29
#define DOS_PARENT         30
#define DOS_DATE_STAMP     31
#define DOS_DELAY          32
#define DOS_DATE_TO_STR    33
#define DOS_PARSE_PATTERN       34
#define DOS_MATCH_PATTERN       35
#define DOS_PARSE_PATTERN_NO_CASE 36
#define DOS_MATCH_PATTERN_NO_CASE 37
#define DOS_LOADSEG        38
#define DOS_UNLOADSEG      39
#define DOS_CREATE_PROC    40
#define DOS_SYSTEM_TAG_LIST 41
#define DOS_RUN_COMMAND    42
#define DOS_SEND_PKT       43
#define DOS_WAIT_PKT       44
#define DOS_REPLY_PKT      45
#define DOS_ADD_PART       46
#define DOS_COMPARE_NAMES  47
#define DOS_STR_TO_DATE    48
#define DOS_CHECK_SIGNAL   49
#define DOS_WAIT_FOR_CHAR  50
#define DOS_NAME_FROM_LOCK 51
#define DOS_LOCK_RECORD    52
#define DOS_UNLOCK_RECORD  53
#define DOS_GET_CONSOLE_TASK 54
#define DOS_SET_CONSOLE_TASK 55
#define DOS_CREATE_SEG_LIST 56
#define DOS_CURRENT_DIR    57
#define DOS_SET_PROGRAM_DIR 58
#define DOS_GET_PROGRAM_DIR 59
#define DOS_SET_IO_ERR     60
#define DOS_CLI            61
#define DOS_FIND_CLI_PROC  62
#define DOS_WRITE_CHARS    63
#define DOS_FREE_ARGS      64
#define DOS_FLUSH          65
#define DOS_SELECT_INPUT   66
#define DOS_SELECT_OUTPUT  67
#define DOS_EXECUTE        68
#define DOS_DEVICE_PROC    69
#define DOS_FAULT          70
#define DOS_STUB_LVO      250  /* catch-all stub marker for unimplemented LVOs */

/* intuition.library function indices */
#define INTUITION_OPEN_LIBRARY      1
#define INTUITION_CLOSE_LIBRARY     2
#define INTUITION_OPEN_WINDOW       3
#define INTUITION_CLOSE_WINDOW      4
#define INTUITION_WINDOW_TO_FRONT   5
#define INTUITION_WINDOW_TO_BACK    6
#define INTUITION_ACTIVATE_WINDOW   7
#define INTUITION_MOVE_WINDOW       8
#define INTUITION_SIZE_WINDOW       9
#define INTUITION_REFRESH_WINDOW    10
#define INTUITION_MODIFY_IDCMP      11
#define INTUITION_SET_WINDOW_TITLES 12
#define INTUITION_OPEN_WINDOW_TAGS  13
#define INTUITION_OPEN_WORKBENCH    14
#define INTUITION_CLOSE_WORKBENCH   15
#define INTUITION_DRAW_BORDER       16
#define INTUITION_DRAW_IMAGE        17
#define INTUITION_PRINT_I_TEXT      18
#define INTUITION_AUTO_REQUEST      19
#define INTUITION_BUILD_SYS_REQUEST 20
#define INTUITION_FREE_SYS_REQUEST  21
#define INTUITION_EASY_REQUEST      22
#define INTUITION_OPEN_SCREEN       23
#define INTUITION_CLOSE_SCREEN      24
#define INTUITION_MOVE_SCREEN       25
#define INTUITION_SCREEN_TO_FRONT   26
#define INTUITION_SCREEN_TO_BACK    27
#define INTUITION_SHOW_TITLE        28
#define INTUITION_OPEN_SCREEN_TAGS  29
#define INTUITION_SET_MENU_STRIP    30
#define INTUITION_CLEAR_MENU_STRIP  31
#define INTUITION_RESET_MENU_STRIP  32
#define INTUITION_ITEM_ADDRESS      33
#define INTUITION_LOCK_PUB_SCREEN     34
#define INTUITION_UNLOCK_PUB_SCREEN   35
#define INTUITION_LOCK_PUB_SCREEN_LIST   36
#define INTUITION_UNLOCK_PUB_SCREEN_LIST 37
#define INTUITION_SET_POINTER            38
#define INTUITION_CLEAR_POINTER          39
#define INTUITION_SET_WINDOW_POINTER_A   40
#define INTUITION_GET_DEF_PREFS          41
#define INTUITION_GET_PREFS              42
#define INTUITION_SET_PREFS              43
#define INTUITION_LOCK_GUI_PREFS         44
#define INTUITION_UNLOCK_GUI_PREFS       45
#define INTUITION_QUERY_OVERSCAN         46
#define INTUITION_GET_DISPLAY_INFO_DATA  47
#define INTUITION_NEXT_DISPLAY_INFO      48
#define INTUITION_CURRENT_TIME           49
#define INTUITION_DOUBLE_CLICK           50
#define INTUITION_REPORT_MOUSE           51
#define INTUITION_DISPLAY_BEEP           52
#define INTUITION_INIT_REQUESTER         53
#define INTUITION_END_REQUEST            54
#define INTUITION_REQUEST                55
#define INTUITION_VIEW_ADDRESS           56
#define INTUITION_VIEW_PORT_ADDRESS      57
#define INTUITION_GET_SCREEN_DATA        58
#define INTUITION_NEXT_PUB_SCREEN        59
#define INTUITION_SET_DEFAULT_PUB_SCREEN 60
#define INTUITION_LOCK_IBASE             61
#define INTUITION_UNLOCK_IBASE           62
#define INTUITION_SHOW_WINDOW            63
#define INTUITION_HIDE_WINDOW            64
#define INTUITION_WINDOW_LIMITS          65
#define INTUITION_CHANGE_WINDOW_BOX      66
#define INTUITION_GET_SCREEN_DRAW_INFO   67
#define INTUITION_FREE_SCREEN_DRAW_INFO  68
#define INTUITION_DISPLAY_ALERT          69
#define INTUITION_TIMED_DISPLAY_ALERT    70
#define INTUITION_SCREEN_DEPTH           71
#define INTUITION_SCREEN_POSITION        72
#define INTUITION_ADD_GADGET             73
#define INTUITION_ADD_GLIST              74
#define INTUITION_REMOVE_GADGET          75
#define INTUITION_REMOVE_GLIST           76
#define INTUITION_REFRESH_GLIST          77
#define INTUITION_ON_GADGET              78
#define INTUITION_OFF_GADGET             79
#define INTUITION_MODIFY_PROP            80
#define INTUITION_NEW_MODIFY_PROP        81
#define INTUITION_ACTIVATE_GADGET        82
#define INTUITION_SET_WINDOW_ATTRS       83
#define INTUITION_GET_WINDOW_ATTRS       84
#define INTUITION_SET_SCREEN_ATTRS       85
#define INTUITION_GET_SCREEN_ATTRS       86
#define INTUITION_GET_VISUAL_INFO        87
#define INTUITION_FREE_VISUAL_INFO       88
#define INTUITION_BEGIN_REFRESH          89
#define INTUITION_END_REFRESH            90
#define INTUITION_REFRESH_GADGETS        91
#define INTUITION_ON_MENU                92
#define INTUITION_OFF_MENU               93
#define INTUITION_SYS_REQ_HANDLER        94
#define INTUITION_PUB_SCREEN_STATUS      95
#define INTUITION_GET_DEFAULT_PUB_SCREEN 96
#define INTUITION_MOVE_WINDOW_IN_FRONT_OF 97
#define INTUITION_SET_EDIT_HOOK          98
#define INTUITION_OBTAIN_GIR_PORT        99
#define INTUITION_RELEASE_GIR_PORT       100
#define INTUITION_STRIP_INTUI_MESSAGES   101
#define INTUITION_NEW_OBJECT_A           102
#define INTUITION_DISPOSE_OBJECT         103
#define INTUITION_SET_ATTRS_A            104
#define INTUITION_GET_ATTR               105
#define INTUITION_DO_METHOD_A            106
#define INTUITION_DO_SUPER_METHOD_A      107
#define INTUITION_COERCE_METHOD_A        108
#define INTUITION_MAKE_CLASS             109
#define INTUITION_FREE_CLASS             110
#define INTUITION_ADD_CLASS              111
#define INTUITION_REMOVE_CLASS          112
#define INTUITION_NEXT_OBJECT           113
#define INTUITION_GET_ATTRS_A           114
#define INTUITION_SET_SUPER_ATTRS_A     115
#define INTUITION_DO_GADGET_METHOD_A    116
#define INTUITION_HELP_CONTROL          117
#define INTUITION_START_SCREEN_NOTIFY   118
#define INTUITION_END_SCREEN_NOTIFY     119
#define INTUITION_GET_WINDOW_ATTR       120
#define INTUITION_SET_WINDOW_ATTR       121
#define INTUITION_GET_SCREEN_ATTR       122
#define INTUITION_SET_SCREEN_ATTR       123
#define INTUITION_NEW_OBJECT            124
#define INTUITION_SET_ATTRS             125
#define INTUITION_GET_ATTRS             126
#define INTUITION_DO_METHOD             127
#define INTUITION_DO_SUPER_METHOD       128
#define INTUITION_COERCE_METHOD         129
#define INTUITION_SET_GADGET_ATTRS_A   130
#define INTUITION_SET_SUPER_ATTRS       131
#define INTUITION_SET_WINDOW_POINTER    132
#define INTUITION_OPEN_WINDOW_TAGS_V    133
#define INTUITION_OPEN_SCREEN_TAGS_V    134
#define INTUITION_DO_GADGET_METHOD       135
#define INTUITION_SET_GADGET_ATTRS     136
#define INTUITION_ALLOC_SCREEN_BUFFER  137
#define INTUITION_FREE_SCREEN_BUFFER   138
#define INTUITION_CHANGE_SCREEN_BUFFER 139
#define INTUITION_WBENCH_TO_BACK       140
#define INTUITION_WBENCH_TO_FRONT      141
#define INTUITION_MAKE_SCREEN          142
#define INTUITION_REMAKE_DISPLAY       143
#define INTUITION_RETHINK_DISPLAY      144
#define INTUITION_CLEAR_DMREQUEST      145
#define INTUITION_SET_DMREQUEST        146
#define INTUITION_SET_MOUSE_QUEUE      147
#define INTUITION_SET_PUB_SCREEN_MODES 148
#define INTUITION_LEND_MENUS           149
#define INTUITION_GADGET_MOUSE         150
#define INTUITION_INTUITEXT_LENGTH     151
#define INTUITION_POINT_IN_IMAGE       152
#define INTUITION_ERASE_IMAGE          153
#define INTUITION_ZIP_WINDOW           154
#define INTUITION_REFRESH_SET_GADGET_ATTRS_A 155
#define INTUITION_SCROLL_WINDOW_RASTER 156
#define INTUITION_BUILD_EASY_REQUEST_ARGS 157
#define INTUITION_DRAW_IMAGE_STATE       158
#define INTUITION_ALLOC_REMEMBER         159
#define INTUITION_FREE_REMEMBER          160
#define INTUITION_NEW_IMAGE_A            161
#define INTUITION_DISPOSE_IMAGE          162
#define INTUITION_SET_IPREFS             163
#define INTUITION_GET_HALF_PENS          164
#define INTUITION_GADGET_BOX             165
#define INTUITION_SET_GUI_ATTRS_A        166
#define INTUITION_GET_GUI_ATTRS_A        167
#define INTUITION_OPEN_CLASS             168
#define INTUITION_CLOSE_CLASS            169
#define INTUITION_IDO_METHOD_A           170
#define INTUITION_IDO_SUPER_METHOD_A     171
#define INTUITION_ICOERCE_METHOD_A       172
#define INTUITION_ISET_SUPER_ATTRS_A     173
#define INTUITION_LOCK_SCREEN            174
#define INTUITION_UNLOCK_SCREEN          175
#define INTUITION_LOCK_SCREEN_LIST       176
#define INTUITION_UNLOCK_SCREEN_LIST     177
#define INTUITION_LOCK_SCREEN_GI         178
#define INTUITION_UNLOCK_SCREEN_GI       179

/* gadtools.library function indices */
#define GADTOOLS_OPEN_LIBRARY          1
#define GADTOOLS_CLOSE_LIBRARY         2
#define GADTOOLS_CREATE_GADGET_A       3
#define GADTOOLS_FREE_GADGETS          4
#define GADTOOLS_GT_SET_GADGET_ATTRS_A 5
#define GADTOOLS_CREATE_MENUS_A        6
#define GADTOOLS_FREE_MENUS            7
#define GADTOOLS_LAYOUT_MENU_ITEMS_A   8
#define GADTOOLS_LAYOUT_MENUS_A        9
#define GADTOOLS_GT_GET_IMSG          10
#define GADTOOLS_GT_REPLY_IMSG         11
#define GADTOOLS_GT_REFRESH_WINDOW     12
#define GADTOOLS_GT_BEGIN_REFRESH      13
#define GADTOOLS_GT_END_REFRESH        14
#define GADTOOLS_GT_FILTER_IMSG       15
#define GADTOOLS_GT_POST_FILTER_IMSG   16
#define GADTOOLS_CREATE_CONTEXT        17
#define GADTOOLS_DRAW_BEVEL_BOX_A      18
#define GADTOOLS_GET_VISUAL_INFO_A     19
#define GADTOOLS_FREE_VISUAL_INFO       20
#define GADTOOLS_GT_GET_GADGET_ATTRS_A 21

/* Build the stub: ILLEGAL word followed by (lib<<8|func) word */
static void install_stub(int lib_id, int func_idx)
{
    uint32_t addr = JMPTAB_BASE + (uint32_t)(lib_id-1) * JMPTAB_LIB_SZ
                                + (uint32_t)(func_idx-1) * 4;
    g_ram[addr]   = 0x4A; g_ram[addr+1] = 0xFC; /* ILLEGAL */
    g_ram[addr+2] = (uint8_t)lib_id;
    g_ram[addr+3] = (uint8_t)func_idx;
}

/* =========================================================================
 * exec.library pseudo-base address
 * Amiga programs call exec via negative offsets from the exec base pointer
 * stored at address 4 in the Amiga memory map.  Each negative slot holds
 * a 4-byte ILLEGAL dispatch stub (with an RTS right after it).
 *
 * Layout (UAOS-252): every base keeps its full negative jump table clear
 * of the exception-vector page and of its neighbours' positive structs.
 * The old EXEC_BASE=0x300/DOS_BASE=0x800 placement overlapped ExecBase
 * fields with the DOS jump table and pushed deep exec LVOs (-516 and
 * beyond) into the vector page, tripping the WILD-PC breaker.
 * ========================================================================= */

#define EXEC_BASE    0x1000   /* stubs -6..-996 → 0xC1C-0xFFA, clear of vectors */
#define FAKE_LIB_BASE 0xF000   /* returned for unknown libraries — has RTS at LVO slots.
                                * LVO range: 0xED0C-0xEFFA, above LHA data hunk (ends ~0xDF88) */
#define AUDIO_DEV_BASE 0xE000  /* audio.device base — trapped LVO table (UAOS-240) */

/* Process struct layout (AmigaOS offsets — mirror kernel/exec/amiga_task.h;
 * keep the two in sync).  Task struct embedded at start, then Process
 * extensions.
 * pr_CLI  is at Process+0xAC (172) — non-zero means launched from CLI.
 * pr_CIS  is at Process+0x9C  — CLI input stream (we set to DOS_STDIN_BPTR).
 * pr_COS  is at Process+0xA0  — CLI output stream (we set to DOS_STDOUT_BPTR). */
#define PR_CLI_OFFSET      0xAC
#define PR_CIS_OFFSET      0x9C
#define PR_COS_OFFSET      0xA0

/* Embedded Task node + task fields */
#define PRC_LN_TYPE        0x08
#define PRC_LN_PRI         0x09
#define PRC_LN_NAME        0x0A
#define PRC_TC_FLAGS       0x0E
#define PRC_TC_STATE       0x0F
#define PRC_TC_IDNEST      0x10
#define PRC_TC_TDNEST      0x11
#define PRC_TC_SIGALLOC    0x12
#define PRC_TC_SPREG       0x36
#define PRC_TC_SPLOWER     0x3A
#define PRC_TC_SPUPPER     0x3E
#define PRC_MSGPORT        0x5C   /* embedded struct MsgPort (34 bytes) */
#define PRC_SEGLIST        0x80   /* BPTR */
#define PRC_STACKSIZE      0x84   /* LONG */
#define PRC_GLOBVEC        0x88   /* APTR — DOS library base */
#define PRC_TASKNUM        0x8C   /* LONG */
#define PRC_STACKBASE      0x90   /* BPTR */
#define PRC_RESULT2        0x94   /* LONG */
#define PRC_CONSOLETASK    0xA4   /* APTR MsgPort* */
#define PRC_WINDOWPTR      0xB8   /* APTR Window*; -1 suppresses requesters */
#define PRC_LOCALVARS      0xD0   /* embedded struct MinList (12 bytes) */

/* MsgPort sub-offsets (relative to PRC_MSGPORT / a standalone port) */
#define PMP_LN_TYPE        0x08
#define PMP_FLAGS          0x0E
#define PMP_SIGBIT         0x0F
#define PMP_SIGTASK        0x10
#define PMP_MSGLIST        0x14   /* embedded struct List (14 bytes) */

/* CommandLineInterface offsets (UAOS extended layout — amiga_task.h) */
#define CLI_SETNAME        0x04
#define CLI_COMMANDNAME    0x10
#define CLI_FAILLEVEL      0x14
#define CLI_PROMPT         0x18
#define CLI_DEFAULTINPUT   0x1C
#define CLI_DEFAULTOUTPUT  0x20
#define CLI_ERRORLEVEL     0x24
#define CLI_CURRENTINPUT   0x2C
#define CLI_CURRENTOUTPUT  0x30
#define CLI_CURRENTDIRNAME 0x38
#define CLI_INTERACTIVE    0x44
#define CLI_MODULE         0x58   /* BPTR seglist of running command */
#define CLI_MODULE_SC      0x3C   /* same slot, SAS/C + classic-NDK layout */

/* Node types / task state / port flags */
#define NT_MSGPORT_G       4
#define NT_LIBRARY_G       9
#define NT_DEVICE_G        10
#define NT_PROCESS_G       13
#define PA_SIGNAL_G        0
#define TS_RUN_G           2
#define TF_PROCTASK_G      0x01

/* Fake file handle BPTRs (defined here so install_library_tables can use them) */
#define FAKE_STDOUT_ADDR   0x0400   /* below the exec stub floor (0xC1C) */
#define FAKE_STDIN_ADDR    0x0404
#define DOS_STDOUT_BPTR    (FAKE_STDOUT_ADDR >> 2)
#define DOS_STDIN_BPTR     (FAKE_STDIN_ADDR  >> 2)

/* We patch the exec base JVT so that JSR -offset(A6) hits our stubs.
 * Standard AmigaOS exec LVO table (word offsets from base, all negative):
 *   FindTask     -294 = 0xFF7A
 *   OpenLibrary  -552 = 0xFDD8 ... too many to enumerate
 *
 * Simpler approach: programs that call dos.library go through OpenLibrary
 * first. We'll return a fake dos_base that also has stubs installed at the
 * standard LVO offsets. The key LVOs we implement:
 */

/* Fake library bases */
#define DOS_BASE       0x2000  /* stubs -6..-996 → 0x1C1C-0x1FFA, clear of ExecBase */
#define BSD_BASE       0x3000  /* bsdsocket.library base — clear of DOS range */
#define GRAPHICS_BASE  0x8000  /* graphics.library base — room for LVOs -30..-1056 */
#define INTUITION_BASE 0x9000  /* intuition.library base — clear of graphics range */
#define GADTOOLS_BASE  0xA000  /* gadtools.library base — clear of intuition range */

/* Autovector dispatch stubs (UAOS-241): each autovector (level 1-7) points
 * at a 4-byte ILLEGAL word tagged (LIB_IRQ, level).  Musashi pushes the
 * 68020 exception frame and enters the illg callback, which runs the
 * level's exec IntVects / cia.resource ICR servers and emulates RTE back
 * to the interrupted code.  Placed just above HOOK_RETURN_TRAP_ADDR
 * (0x1EF000) in the unused band below the guest stack. */
#define IRQ_STUB_BASE  0x1EF040u

/* bsdsocket.library LVO offsets (AmiTCP/IP standard) */
#define LVO_BSD_SOCKET        (-30)
#define LVO_BSD_BIND          (-36)
#define LVO_BSD_LISTEN        (-42)
#define LVO_BSD_ACCEPT        (-48)
#define LVO_BSD_CONNECT       (-54)
#define LVO_BSD_SEND          (-60)
#define LVO_BSD_SENDTO        (-66)
#define LVO_BSD_RECV          (-72)
#define LVO_BSD_RECVFROM      (-78)
#define LVO_BSD_CLOSESOCKET   (-84)
#define LVO_BSD_SETSOCKOPT    (-96)
#define LVO_BSD_GETSOCKOPT    (-102)
#define LVO_BSD_IOCTLSOCKET   (-108)
#define LVO_BSD_INET_ADDR     (-132)
#define LVO_BSD_INET_NTOA     (-138)
#define LVO_BSD_GETHOSTBYNAME (-210)

/* LVO (Library Vector Offset) — negative byte offset from lib base
 * These are the standard AmigaOS offsets. */
#define LVO_OPEN_LIBRARY   (-552)
#define LVO_CLOSE_LIBRARY  (-414)
#define LVO_ALLOC_MEM      (-198)
#define LVO_FREE_MEM       (-210)
#define LVO_FIND_TASK      (-294)

/* dos.library LVO offsets — canonical AmigaDOS (dos_lib.i V40). */
#define LVO_DOS_OPEN        (-30)
#define LVO_DOS_CLOSE       (-36)
#define LVO_DOS_READ        (-42)
#define LVO_DOS_WRITE       (-48)
#define LVO_DOS_INPUT       (-54)
#define LVO_DOS_OUTPUT      (-60)
#define LVO_DOS_SEEK        (-66)
#define LVO_DOS_DELETEFILE  (-72)
#define LVO_DOS_RENAME      (-78)
#define LVO_DOS_LOCK        (-84)
#define LVO_DOS_UNLOCK      (-90)
#define LVO_DOS_DUPLOCK     (-96)
#define LVO_DOS_EXAMINE     (-102)
#define LVO_DOS_EXAMINE_NEXT (-108)
#define LVO_DOS_INFO        (-114)
#define LVO_DOS_CREATE_DIR  (-120)
#define LVO_DOS_CURRENT_DIR (-126)
#define LVO_DOS_IO_ERR      (-132)
#define LVO_DOS_CREATE_PROC (-138)
#define LVO_DOS_EXIT        (-144)
#define LVO_DOS_LOADSEG     (-150)
#define LVO_DOS_UNLOADSEG   (-156)
#define LVO_DOS_GET_PACKET  (-162)
#define LVO_DOS_QUEUE_PKT   (-168)
#define LVO_DOS_DEVICE_PROC (-174)
#define LVO_DOS_SET_COMMENT (-180)
#define LVO_DOS_SETPROTECTION (-186)
#define LVO_DOS_DATE_STAMP  (-192)
#define LVO_DOS_DELAY       (-198)
#define LVO_DOS_WAIT_FOR_CHAR (-204)
#define LVO_DOS_PARENT      (-210)
#define LVO_DOS_ISINTERACTIVE (-216)
#define LVO_DOS_EXECUTE     (-222)
#define LVO_DOS_ALLOC_DOS_OBJECT (-228)
#define LVO_DOS_FREE_DOS_OBJECT (-234)
#define LVO_DOS_DO_PKT      (-240)
#define LVO_DOS_SEND_PKT    (-246)
#define LVO_DOS_WAIT_PKT    (-252)
#define LVO_DOS_REPLY_PKT   (-258)
#define LVO_DOS_ABORT_PKT   (-264)
#define LVO_DOS_LOCK_RECORD (-270)
#define LVO_DOS_LOCK_RECORDS (-276)
#define LVO_DOS_UNLOCK_RECORD (-282)
#define LVO_DOS_UNLOCK_RECORDS (-288)
#define LVO_DOS_SELECT_INPUT (-294)
#define LVO_DOS_SELECT_OUTPUT (-300)
#define LVO_DOS_FGETC       (-306)
#define LVO_DOS_FPUTC       (-312)
#define LVO_DOS_UNGETC      (-318)
#define LVO_DOS_FREAD       (-324)
#define LVO_DOS_FWRITE      (-330)
#define LVO_DOS_FGETS       (-336)
#define LVO_DOS_FPUTS       (-342)
#define LVO_DOS_VFWRITEF    (-348)
#define LVO_DOS_VFPRINTF    (-354)
#define LVO_DOS_FLUSH       (-360)
#define LVO_DOS_SET_VBUF    (-366)
#define LVO_DOS_DUP_LOCK_FROM_FH (-372)
#define LVO_DOS_OPEN_FROM_LOCK (-378)
#define LVO_DOS_PARENT_OF_FH (-384)
#define LVO_DOS_EXAMINE_FH  (-390)
#define LVO_DOS_SET_FILE_DATE (-396)
#define LVO_DOS_NAME_FROM_LOCK (-402)
#define LVO_DOS_NAME_FROM_FH (-408)
#define LVO_DOS_SPLIT_NAME  (-414)
#define LVO_DOS_SAME_LOCK   (-420)
#define LVO_DOS_SET_MODE    (-426)
#define LVO_DOS_EX_ALL      (-432)
#define LVO_DOS_READ_LINK   (-438)
#define LVO_DOS_MAKE_LINK   (-444)
#define LVO_DOS_CHANGE_MODE (-450)
#define LVO_DOS_SET_FILE_SIZE (-456)
#define LVO_DOS_SET_IO_ERR  (-462)
#define LVO_DOS_FAULT       (-468)
#define LVO_DOS_PRINT_FAULT (-474)
#define LVO_DOS_ERROR_REPORT (-480)
#define LVO_DOS_CLI         (-492)
#define LVO_DOS_CREATE_NEW_PROC (-498)
#define LVO_DOS_RUN_COMMAND (-504)
#define LVO_DOS_GET_CONSOLE_TASK (-510)
#define LVO_DOS_SET_CONSOLE_TASK (-516)
#define LVO_DOS_GET_ARG_STR (-534)
#define LVO_DOS_SET_ARG_STR (-540)
#define LVO_DOS_FIND_CLI_PROC (-546)
#define LVO_DOS_MAX_CLI     (-552)
#define LVO_DOS_SET_PROGRAM_NAME (-570)
#define LVO_DOS_GET_PROGRAM_NAME (-576)
#define LVO_DOS_SET_PROGRAM_DIR (-594)
#define LVO_DOS_GET_PROGRAM_DIR (-600)
#define LVO_DOS_SYSTEM_TAG_LIST (-606)
#define LVO_DOS_GET_DEVICE_PROC (-642)
#define LVO_DOS_LOCK_DOS_LIST (-654)
#define LVO_DOS_DATE_TO_STR (-744)
#define LVO_DOS_STR_TO_DATE (-750)
#define LVO_DOS_CHECK_SIGNAL (-792)
#define LVO_DOS_READARGS    (-798)
#define LVO_DOS_STR_TO_LONG (-816)
#define LVO_DOS_MATCH_FIRST (-822)
#define LVO_DOS_MATCH_NEXT  (-828)
#define LVO_DOS_MATCH_END   (-834)
#define LVO_DOS_PARSE_PATTERN (-840)
#define LVO_DOS_MATCH_PATTERN (-846)
#define LVO_DOS_FREE_ARGS   (-858)
#define LVO_DOS_FILE_PART   (-870)
#define LVO_DOS_PATH_PART   (-876)
#define LVO_DOS_ADD_PART    (-882)
#define LVO_DOS_SETVAR      (-900)
#define LVO_DOS_GETVAR      (-906)
#define LVO_DOS_CLI_INIT_NEWCLI (-930)
#define LVO_DOS_CLI_INIT_RUN (-936)
#define LVO_DOS_WRITE_CHARS (-942)
#define LVO_DOS_PUTSTR      (-948)
#define LVO_DOS_VPRINTF     (-954)
#define LVO_DOS_PRINTF      (-954)  /* alias VPrintf */
#define LVO_DOS_PARSE_PATTERN_NO_CASE (-966)
#define LVO_DOS_MATCH_PATTERN_NO_CASE (-972)

/* exec.library LVO offsets — canonical exec_lib.i (V37+). */
#define LVO_INIT_STRUCT       (-78)
#define LVO_DISABLE           (-120)
#define LVO_ENABLE            (-126)
#define LVO_FORBID            (-132)
#define LVO_PERMIT            (-138)
#define LVO_SUPER_STATE       (-150)
#define LVO_USER_STATE        (-156)
#define LVO_SET_INT_VECTOR    (-162)
#define LVO_ADD_INT_SERVER    (-168)
#define LVO_REM_INT_SERVER    (-174)
#define LVO_CAUSE             (-180)
#define LVO_AVAIL_MEM         (-216)
#define LVO_ALLOC_ENTRY       (-222)
#define LVO_FREE_ENTRY        (-228)
#define LVO_INSERT            (-234)
#define LVO_ADD_HEAD          (-240)
#define LVO_ADD_TAIL          (-246)
#define LVO_REMOVE            (-252)
#define LVO_REM_HEAD          (-258)
#define LVO_REM_TAIL          (-264)
#define LVO_ENQUEUE           (-270)
#define LVO_FIND_NAME         (-276)
#define LVO_ADD_TASK          (-282)
#define LVO_REM_TASK          (-288)
#define LVO_SET_TASK_PRI      (-300)
#define LVO_SET_EXCEPT        (-312)
#define LVO_WAIT              (-318)
#define LVO_SIGNAL            (-324)
#define LVO_SETSIGNAL         (-306)
#define LVO_ALLOC_SIGNAL      (-330)
#define LVO_FREE_SIGNAL       (-336)
#define LVO_ALLOC_TRAP        (-342)
#define LVO_FREE_TRAP         (-348)
#define LVO_ADD_PORT          (-354)
#define LVO_REM_PORT          (-360)
#define LVO_PUT_MSG           (-366)
#define LVO_GET_MSG           (-372)
#define LVO_REPLY_MSG         (-378)
#define LVO_WAIT_PORT         (-384)
#define LVO_FIND_PORT         (-390)
#define LVO_ADD_LIBRARY       (-396)
#define LVO_REM_LIBRARY       (-402)
#define LVO_OLD_OPEN_LIBRARY  (-408)
#define LVO_SET_FUNCTION      (-420)
#define LVO_ADD_DEVICE        (-432)
#define LVO_REM_DEVICE        (-438)
#define LVO_OPEN_DEVICE       (-444)
#define LVO_CLOSE_DEVICE      (-450)
#define LVO_DO_IO             (-456)
#define LVO_SEND_IO           (-462)
#define LVO_CHECK_IO          (-468)
#define LVO_WAIT_IO           (-474)
#define LVO_ABORT_IO          (-480)
#define LVO_ADD_RESOURCE      (-486)
#define LVO_REM_RESOURCE      (-492)
#define LVO_OPEN_RESOURCE     (-498)
#define LVO_RAW_DO_FMT        (-522)
#define LVO_GETCC             (-528)
#define LVO_TYPE_OF_MEM       (-534)
#define LVO_PROCURE           (-540)
#define LVO_VACATE            (-546)
#define LVO_INIT_SEMAPHORE    (-558)
#define LVO_OBTAIN_SEM        (-564)
#define LVO_RELEASE_SEM       (-570)
#define LVO_ATTEMPT_SEM       (-576)
#define LVO_OBTAIN_SEM_LIST   (-582)
#define LVO_RELEASE_SEM_LIST  (-588)
#define LVO_FIND_SEMAPHORE    (-594)
#define LVO_ADD_SEMAPHORE     (-600)
#define LVO_REM_SEMAPHORE     (-606)
#define LVO_SUM_KICK_DATA     (-612)
#define LVO_ADD_MEM_LIST      (-618)
#define LVO_COPY_MEM          (-624)
#define LVO_COPY_MEM_QUICK    (-630)
#define LVO_CACHE_CLEAR_U     (-636)
#define LVO_CACHE_CLEAR_E     (-642)
#define LVO_CACHE_CONTROL     (-648)
#define LVO_CREATE_IOREQUEST  (-654)
#define LVO_DELETE_IOREQUEST  (-660)
#define LVO_CREATE_MSGPORT    (-666)
#define LVO_DELETE_MSGPORT    (-672)
#define LVO_OBTAIN_SEM_SHARED (-678)
#define LVO_ALLOC_VEC         (-684)
#define LVO_FREE_VEC          (-690)
#define LVO_CREATE_POOL       (-696)  /* V39 */
#define LVO_DELETE_POOL       (-702)
#define LVO_ALLOC_POOLED      (-708)
#define LVO_FREE_POOLED       (-714)
#define LVO_ATTEMPT_SEM_SHARED (-720)
#define LVO_COLD_REBOOT       (-726)
#define LVO_STACK_SWAP        (-732)  /* V37 */

/* intuition.library LVO offsets — official AmigaOS 3.1 (V39) and V40 values.

 * Verified against the AmigaOS NDK intuition_lib.i include file.

 * Functions not in the official LVO table (amiga.lib link-time

 * functions, gadtools/graphics functions) have NO LVO stub installed. */
#define LVO_INTUITION_OPEN_LIBRARY                 (  -30)  /* OpenIntuition */
#define LVO_INTUITION_CLOSE_LIBRARY                (  -36)  /* Intuition */
#define LVO_INTUITION_OPEN_WINDOW                  ( -204)  /* OpenWindow */
#define LVO_INTUITION_CLOSE_WINDOW                 (  -72)  /* CloseWindow */
#define LVO_INTUITION_WINDOW_TO_FRONT              ( -312)  /* WindowToFront */
#define LVO_INTUITION_WINDOW_TO_BACK               ( -306)  /* WindowToBack */
#define LVO_INTUITION_ACTIVATE_WINDOW              ( -450)  /* ActivateWindow */
#define LVO_INTUITION_MOVE_WINDOW                  ( -168)  /* MoveWindow */
#define LVO_INTUITION_SIZE_WINDOW                  ( -288)  /* SizeWindow */
#define LVO_INTUITION_REFRESH_WINDOW               ( -456)  /* RefreshWindowFrame */
#define LVO_INTUITION_MODIFY_IDCMP                 ( -150)  /* ModifyIDCMP */
#define LVO_INTUITION_SET_WINDOW_TITLES            ( -276)  /* SetWindowTitles */
#define LVO_INTUITION_OPEN_WINDOW_TAGS             ( -606)  /* OpenWindowTagList */
#define LVO_INTUITION_OPEN_WORKBENCH               ( -210)  /* OpenWorkBench */
#define LVO_INTUITION_CLOSE_WORKBENCH              (  -78)  /* CloseWorkBench */
#define LVO_INTUITION_DRAW_BORDER                  ( -108)  /* DrawBorder */
#define LVO_INTUITION_DRAW_IMAGE                   ( -114)  /* DrawImage */
#define LVO_INTUITION_PRINT_I_TEXT                 ( -216)  /* PrintIText */
#define LVO_INTUITION_AUTO_REQUEST                 ( -348)  /* AutoRequest */
#define LVO_INTUITION_BUILD_SYS_REQUEST            ( -360)  /* BuildSysRequest */
#define LVO_INTUITION_FREE_SYS_REQUEST             ( -372)  /* FreeSysRequest */
#define LVO_INTUITION_EASY_REQUEST                 ( -588)  /* EasyRequestArgs */
#define LVO_INTUITION_OPEN_SCREEN                  ( -198)  /* OpenScreen */
#define LVO_INTUITION_CLOSE_SCREEN                 (  -66)  /* CloseScreen */
#define LVO_INTUITION_MOVE_SCREEN                  ( -162)  /* MoveScreen */
#define LVO_INTUITION_SCREEN_TO_FRONT              ( -252)  /* ScreenToFront */
#define LVO_INTUITION_SCREEN_TO_BACK               ( -246)  /* ScreenToBack */
#define LVO_INTUITION_SHOW_TITLE                   ( -282)  /* ShowTitle */
#define LVO_INTUITION_OPEN_SCREEN_TAGS             ( -612)  /* OpenScreenTagList */
#define LVO_INTUITION_SET_MENU_STRIP               ( -264)  /* SetMenuStrip */
#define LVO_INTUITION_CLEAR_MENU_STRIP             (  -54)  /* ClearMenuStrip */
#define LVO_INTUITION_RESET_MENU_STRIP             ( -702)  /* ResetMenuStrip */
#define LVO_INTUITION_ITEM_ADDRESS                 ( -144)  /* ItemAddress */
#define LVO_INTUITION_LOCK_PUB_SCREEN              ( -510)  /* LockPubScreen */
#define LVO_INTUITION_UNLOCK_PUB_SCREEN            ( -516)  /* UnlockPubScreen */
#define LVO_INTUITION_LOCK_PUB_SCREEN_LIST         ( -522)  /* LockPubScreenList */
#define LVO_INTUITION_UNLOCK_PUB_SCREEN_LIST       ( -528)  /* UnlockPubScreenList */
#define LVO_INTUITION_SET_POINTER                  ( -270)  /* SetPointer */
#define LVO_INTUITION_CLEAR_POINTER                (  -60)  /* ClearPointer */
#define LVO_INTUITION_SET_WINDOW_POINTER_A         ( -816)  /* SetWindowPointerA */
#define LVO_INTUITION_GET_DEF_PREFS                ( -126)  /* GetDefPrefs */
#define LVO_INTUITION_GET_PREFS                    ( -132)  /* GetPrefs */
#define LVO_INTUITION_SET_PREFS                    ( -324)  /* SetPrefs */
#define LVO_INTUITION_LOCK_GUI_PREFS               ( -852)  /* LockGUIPrefs */
#define LVO_INTUITION_UNLOCK_GUI_PREFS             ( -858)  /* UnlockGUIPrefs */
#define LVO_INTUITION_QUERY_OVERSCAN               ( -474)  /* QueryOverscan */
#define LVO_INTUITION_CURRENT_TIME                 (  -84)  /* CurrentTime */
#define LVO_INTUITION_DOUBLE_CLICK                 ( -102)  /* DoubleClick */
#define LVO_INTUITION_REPORT_MOUSE                 ( -234)  /* ReportMouse */
#define LVO_INTUITION_DISPLAY_BEEP                 (  -96)  /* DisplayBeep */
#define LVO_INTUITION_INIT_REQUESTER               ( -138)  /* InitRequester */
#define LVO_INTUITION_END_REQUEST                  ( -120)  /* EndRequest */
#define LVO_INTUITION_REQUEST                      ( -240)  /* Request */
#define LVO_INTUITION_VIEW_ADDRESS                 ( -294)  /* ViewAddress */
#define LVO_INTUITION_VIEW_PORT_ADDRESS            ( -300)  /* ViewPortAddress */
#define LVO_INTUITION_GET_SCREEN_DATA              ( -426)  /* GetScreenData */
#define LVO_INTUITION_NEXT_PUB_SCREEN              ( -534)  /* NextPubScreen */
#define LVO_INTUITION_SET_DEFAULT_PUB_SCREEN       ( -540)  /* SetDefaultPubScreen */
#define LVO_INTUITION_LOCK_IBASE                   ( -414)  /* LockIBase */
#define LVO_INTUITION_UNLOCK_IBASE                 ( -420)  /* UnlockIBase */
#define LVO_INTUITION_SHOW_WINDOW                  ( -834)  /* ShowWindow */
#define LVO_INTUITION_HIDE_WINDOW                  ( -840)  /* HideWindow */
#define LVO_INTUITION_WINDOW_LIMITS                ( -318)  /* WindowLimits */
#define LVO_INTUITION_CHANGE_WINDOW_BOX            ( -486)  /* ChangeWindowBox */
#define LVO_INTUITION_GET_SCREEN_DRAW_INFO         ( -690)  /* GetScreenDrawInfo */
#define LVO_INTUITION_FREE_SCREEN_DRAW_INFO        ( -696)  /* FreeScreenDrawInfo */
#define LVO_INTUITION_DISPLAY_ALERT                (  -90)  /* DisplayAlert */
#define LVO_INTUITION_TIMED_DISPLAY_ALERT          ( -822)  /* TimedDisplayAlert */
#define LVO_INTUITION_SCREEN_DEPTH                 ( -786)  /* ScreenDepth */
#define LVO_INTUITION_SCREEN_POSITION              ( -792)  /* ScreenPosition */
#define LVO_INTUITION_ADD_GADGET                   (  -42)  /* AddGadget */
#define LVO_INTUITION_ADD_GLIST                    ( -438)  /* AddGList */
#define LVO_INTUITION_REMOVE_GADGET                ( -228)  /* RemoveGadget */
#define LVO_INTUITION_REMOVE_GLIST                 ( -444)  /* RemoveGList */
#define LVO_INTUITION_REFRESH_GLIST                ( -432)  /* RefreshGList */
#define LVO_INTUITION_ON_GADGET                    ( -186)  /* OnGadget */
#define LVO_INTUITION_OFF_GADGET                   ( -174)  /* OffGadget */
#define LVO_INTUITION_MODIFY_PROP                  ( -156)  /* ModifyProp */
#define LVO_INTUITION_NEW_MODIFY_PROP              ( -468)  /* NewModifyProp */
#define LVO_INTUITION_ACTIVATE_GADGET              ( -462)  /* ActivateGadget */
#define LVO_INTUITION_SET_WINDOW_ATTRS             ( -954)  /* SetWindowAttrsA */
#define LVO_INTUITION_GET_WINDOW_ATTRS             ( -948)  /* GetWindowAttrsA */
#define LVO_INTUITION_SET_SCREEN_ATTRS             ( -996)  /* SetScreenAttrsA */
#define LVO_INTUITION_GET_SCREEN_ATTRS             ( -990)  /* GetScreenAttrsA */
#define LVO_INTUITION_BEGIN_REFRESH                ( -354)  /* BeginRefresh */
#define LVO_INTUITION_END_REFRESH                  ( -366)  /* EndRefresh */
#define LVO_INTUITION_REFRESH_GADGETS              ( -222)  /* RefreshGadgets */
#define LVO_INTUITION_ON_MENU                      ( -192)  /* OnMenu */
#define LVO_INTUITION_OFF_MENU                     ( -180)  /* OffMenu */
#define LVO_INTUITION_SYS_REQ_HANDLER              ( -600)  /* SysReqHandler */
#define LVO_INTUITION_PUB_SCREEN_STATUS            ( -552)  /* PubScreenStatus */
#define LVO_INTUITION_GET_DEFAULT_PUB_SCREEN       ( -582)  /* GetDefaultPubScreen */
#define LVO_INTUITION_MOVE_WINDOW_IN_FRONT_OF      ( -480)  /* MoveWindowInFrontOf */
#define LVO_INTUITION_SET_EDIT_HOOK                ( -492)  /* SetEditHook */
#define LVO_INTUITION_OBTAIN_GIR_PORT              ( -558)  /* ObtainGIRPort */
#define LVO_INTUITION_RELEASE_GIR_PORT             ( -564)  /* ReleaseGIRPort */
#define LVO_INTUITION_STRIP_INTUI_MESSAGES         ( -972)  /* StripIntuiMessages */
#define LVO_INTUITION_NEW_OBJECT_A                 ( -636)  /* NewObjectA */
#define LVO_INTUITION_DISPOSE_OBJECT               ( -642)  /* DisposeObject */
#define LVO_INTUITION_SET_ATTRS_A                  ( -648)  /* SetAttrsA */
#define LVO_INTUITION_GET_ATTR                     ( -654)  /* GetAttr */
#define LVO_INTUITION_MAKE_CLASS                   ( -678)  /* MakeClass */
#define LVO_INTUITION_FREE_CLASS                   ( -714)  /* FreeClass */
#define LVO_INTUITION_ADD_CLASS                    ( -684)  /* AddClass */
#define LVO_INTUITION_REMOVE_CLASS                 ( -708)  /* RemoveClass */
#define LVO_INTUITION_NEXT_OBJECT                  ( -666)  /* NextObject */
#define LVO_INTUITION_GET_ATTRS_A                  ( -846)  /* GetAttrsA */
#define LVO_INTUITION_DO_GADGET_METHOD_A           ( -810)  /* DoGadgetMethodA */
#define LVO_INTUITION_HELP_CONTROL                 ( -828)  /* HelpControl */
#define LVO_INTUITION_START_SCREEN_NOTIFY          (-1218)  /* StartScreenNotifyTagList */
#define LVO_INTUITION_END_SCREEN_NOTIFY            (-1224)  /* EndScreenNotify */
#define LVO_INTUITION_GET_WINDOW_ATTR              ( -960)  /* GetWindowAttr */
#define LVO_INTUITION_SET_WINDOW_ATTR              ( -966)  /* SetWindowAttr */
#define LVO_INTUITION_GET_SCREEN_ATTR              (-1002)  /* GetScreenAttr */
#define LVO_INTUITION_SET_SCREEN_ATTR              (-1008)  /* SetScreenAttr */
#define LVO_INTUITION_SET_GADGET_ATTRS_A           ( -660)  /* SetGadgetAttrsA */
#define LVO_INTUITION_ALLOC_SCREEN_BUFFER          ( -768)  /* AllocScreenBuffer */
#define LVO_INTUITION_FREE_SCREEN_BUFFER           ( -774)  /* FreeScreenBuffer */
#define LVO_INTUITION_CHANGE_SCREEN_BUFFER         ( -780)  /* ChangeScreenBuffer */
#define LVO_INTUITION_WBENCH_TO_BACK               ( -336)  /* WBenchToBack */
#define LVO_INTUITION_WBENCH_TO_FRONT              ( -342)  /* WBenchToFront */
#define LVO_INTUITION_MAKE_SCREEN                  ( -378)  /* MakeScreen */
#define LVO_INTUITION_REMAKE_DISPLAY               ( -384)  /* RemakeDisplay */
#define LVO_INTUITION_RETHINK_DISPLAY              ( -390)  /* RethinkDisplay */
#define LVO_INTUITION_CLEAR_DMREQUEST              (  -48)  /* ClearDMRequest */
#define LVO_INTUITION_SET_DMREQUEST                ( -258)  /* SetDMRequest */
#define LVO_INTUITION_SET_MOUSE_QUEUE              ( -498)  /* SetMouseQueue */
#define LVO_INTUITION_SET_PUB_SCREEN_MODES         ( -546)  /* SetPubScreenModes */
#define LVO_INTUITION_LEND_MENUS                   ( -804)  /* LendMenus */
#define LVO_INTUITION_GADGET_MOUSE                 ( -570)  /* GadgetMouse */
#define LVO_INTUITION_INTUITEXT_LENGTH             ( -330)  /* IntuiTextLength */
#define LVO_INTUITION_POINT_IN_IMAGE               ( -624)  /* PointInImage */
#define LVO_INTUITION_ERASE_IMAGE                  ( -630)  /* EraseImage */
#define LVO_INTUITION_ZIP_WINDOW                   ( -504)  /* ZipWindow */
#define LVO_INTUITION_REFRESH_SET_GADGET_ATTRS_A   ( -888)  /* RefreshSetGadgetAttrsA */
#define LVO_INTUITION_SCROLL_WINDOW_RASTER         ( -798)  /* ScrollWindowRaster */
#define LVO_INTUITION_BUILD_EASY_REQUEST_ARGS      ( -594)  /* BuildEasyRequestArgs */
#define LVO_INTUITION_DRAW_IMAGE_STATE             ( -618)  /* DrawImageState */
#define LVO_INTUITION_ALLOC_REMEMBER               ( -396)  /* AllocRemember */
#define LVO_INTUITION_FREE_REMEMBER                ( -408)  /* FreeRemember */
#define LVO_INTUITION_NEW_IMAGE_A                  ( -744)  /* NewImageA (V40) */
#define LVO_INTUITION_DISPOSE_IMAGE                ( -750)  /* DisposeImage (V40) */
#define LVO_INTUITION_SET_IPREFS                   ( -756)  /* SetIPrefs (V40) */
#define LVO_INTUITION_SET_GUI_ATTRS_A              ( -864)  /* SetGUIAttrsA (V39) */
#define LVO_INTUITION_GET_GUI_ATTRS_A              ( -870)  /* GetGUIAttrsA (V39) */
#define LVO_INTUITION_GET_HALF_PENS                ( -876)  /* GetHalfPens (V39) */
#define LVO_INTUITION_GADGET_BOX                   ( -882)  /* GadgetBox (V39) */
#define LVO_INTUITION_OPEN_CLASS                   ( -918)  /* OpenClass (V40) */
#define LVO_INTUITION_CLOSE_CLASS                  ( -924)  /* CloseClass (V40) */
#define LVO_INTUITION_LOCK_SCREEN                  ( -936)  /* LockScreen (V40) */
#define LVO_INTUITION_UNLOCK_SCREEN                ( -942)  /* UnlockScreen (V40) */
#define LVO_INTUITION_IDO_SUPER_METHOD_A           ( -894)  /* IDoSuperMethodA (V40) */
#define LVO_INTUITION_ISET_SUPER_ATTRS_A           ( -900)  /* ISetSuperAttrsA (V40) */
#define LVO_INTUITION_ICOERCE_METHOD_A             ( -906)  /* ICoerceMethodA (V40) */
#define LVO_INTUITION_IDO_METHOD_A                 ( -912)  /* IDoMethodA (V40) */
#define LVO_INTUITION_LOCK_SCREEN_LIST             ( -1014) /* LockScreenList (V40) */
#define LVO_INTUITION_UNLOCK_SCREEN_LIST           ( -1020) /* UnlockScreenList (V40) */
#define LVO_INTUITION_LOCK_SCREEN_GI               ( -1026) /* LockScreenGI (V40) */
#define LVO_INTUITION_UNLOCK_SCREEN_GI             ( -1032) /* UnlockScreenGI (V40) */

/* Non-LVO functions (amiga.lib / gadtools / graphics.library):
 *   GetDisplayInfoData(47), NextDisplayInfo(48) — graphics.library
 *   GetVisualInfoA(87), FreeVisualInfo(88) — gadtools.library
 *   DoMethodA(106), DoSuperMethodA(107), CoerceMethodA(108),
 *   SetSuperAttrsA(115) — amiga.lib (not intuition LVO)
 * Varargs stubs share LVO with A-suffix counterparts:
 *   NewObject(124)→NewObjectA, SetAttrs(125)→SetAttrsA,
 *   GetAttrs(126)→GetAttrsA, DoMethod(127)→DoMethodA,
 *   DoSuperMethod(128)→DoSuperMethodA, CoerceMethod(129)→CoerceMethodA,
 *   SetSuperAttrs(131)→SetSuperAttrsA, SetWindowPointer(132)→SetWindowPointerA,
 *   OpenWindowTags_V(133)→OpenWindowTagList, OpenScreenTags_V(134)→OpenScreenTagList,
 *   DoGadgetMethod(135)→DoGadgetMethodA, SetGadgetAttrs(136)→SetGadgetAttrsA */


/* gadtools.library LVO offsets (AmigaOS 3.x) */
#define LVO_GADTOOLS_CREATE_GADGET_A       (-30)
#define LVO_GADTOOLS_FREE_GADGETS            (-36)
#define LVO_GADTOOLS_GT_SET_GADGET_ATTRS_A   (-42)
#define LVO_GADTOOLS_CREATE_MENUS_A          (-48)
#define LVO_GADTOOLS_FREE_MENUS              (-54)
#define LVO_GADTOOLS_LAYOUT_MENU_ITEMS_A     (-60)
#define LVO_GADTOOLS_LAYOUT_MENUS_A           (-66)
#define LVO_GADTOOLS_GT_GET_IMSG             (-72)
#define LVO_GADTOOLS_GT_REPLY_IMSG            (-78)
#define LVO_GADTOOLS_GT_REFRESH_WINDOW        (-84)
#define LVO_GADTOOLS_GT_BEGIN_REFRESH         (-90)
#define LVO_GADTOOLS_GT_END_REFRESH            (-96)
#define LVO_GADTOOLS_GT_FILTER_IMSG          (-102)
#define LVO_GADTOOLS_GT_POST_FILTER_IMSG      (-108)
#define LVO_GADTOOLS_CREATE_CONTEXT          (-114)
#define LVO_GADTOOLS_DRAW_BEVEL_BOX_A         (-120)
#define LVO_GADTOOLS_GET_VISUAL_INFO_A         (-126)
#define LVO_GADTOOLS_FREE_VISUAL_INFO          (-132)
#define LVO_GADTOOLS_GT_GET_GADGET_ATTRS_A    (-174)

static uint32_t stub_addr(int lib_id, int func_idx)
{
    if (lib_id == LIB_EXEC) {
        switch (func_idx) {
            case EXEC_OPEN_LIBRARY:  return (uint32_t)((int)EXEC_BASE + LVO_OPEN_LIBRARY);
            case EXEC_CLOSE_LIBRARY: return (uint32_t)((int)EXEC_BASE + LVO_CLOSE_LIBRARY);
            case EXEC_ALLOC_MEM:     return (uint32_t)((int)EXEC_BASE + LVO_ALLOC_MEM);
            case EXEC_FREE_MEM:      return (uint32_t)((int)EXEC_BASE + LVO_FREE_MEM);
            case EXEC_FIND_TASK:     return (uint32_t)((int)EXEC_BASE + LVO_FIND_TASK);
            case EXEC_WAIT:          return (uint32_t)((int)EXEC_BASE + LVO_WAIT);
            case EXEC_SIGNAL:        return (uint32_t)((int)EXEC_BASE + LVO_SIGNAL);
            case EXEC_SETSIGNAL:     return (uint32_t)((int)EXEC_BASE + LVO_SETSIGNAL);
            case EXEC_ALLOC_SIGNAL:  return (uint32_t)((int)EXEC_BASE + LVO_ALLOC_SIGNAL);
            case EXEC_FREE_SIGNAL:   return (uint32_t)((int)EXEC_BASE + LVO_FREE_SIGNAL);
            case EXEC_PUT_MSG:       return (uint32_t)((int)EXEC_BASE + LVO_PUT_MSG);
            case EXEC_GET_MSG:       return (uint32_t)((int)EXEC_BASE + LVO_GET_MSG);
            case EXEC_REPLY_MSG:     return (uint32_t)((int)EXEC_BASE + LVO_REPLY_MSG);
            case EXEC_WAIT_PORT:     return (uint32_t)((int)EXEC_BASE + LVO_WAIT_PORT);
            case EXEC_CACHE_CLEAR_U: return (uint32_t)((int)EXEC_BASE + LVO_CACHE_CLEAR_U);
            case EXEC_INIT_STRUCT:   return (uint32_t)((int)EXEC_BASE + LVO_INIT_STRUCT);
            case EXEC_DISABLE:       return (uint32_t)((int)EXEC_BASE + LVO_DISABLE);
            case EXEC_ENABLE:        return (uint32_t)((int)EXEC_BASE + LVO_ENABLE);
            case EXEC_FORBID:        return (uint32_t)((int)EXEC_BASE + LVO_FORBID);
            case EXEC_PERMIT:        return (uint32_t)((int)EXEC_BASE + LVO_PERMIT);
            case EXEC_SUPER_STATE:   return (uint32_t)((int)EXEC_BASE + LVO_SUPER_STATE);
            case EXEC_USER_STATE:    return (uint32_t)((int)EXEC_BASE + LVO_USER_STATE);
            case EXEC_AVAIL_MEM:     return (uint32_t)((int)EXEC_BASE + LVO_AVAIL_MEM);
            case EXEC_ALLOC_ENTRY:   return (uint32_t)((int)EXEC_BASE + LVO_ALLOC_ENTRY);
            case EXEC_FREE_ENTRY:    return (uint32_t)((int)EXEC_BASE + LVO_FREE_ENTRY);
            case EXEC_INSERT:        return (uint32_t)((int)EXEC_BASE + LVO_INSERT);
            case EXEC_ADD_HEAD:      return (uint32_t)((int)EXEC_BASE + LVO_ADD_HEAD);
            case EXEC_ADD_TAIL:      return (uint32_t)((int)EXEC_BASE + LVO_ADD_TAIL);
            case EXEC_REMOVE:        return (uint32_t)((int)EXEC_BASE + LVO_REMOVE);
            case EXEC_REM_HEAD:      return (uint32_t)((int)EXEC_BASE + LVO_REM_HEAD);
            case EXEC_REM_TAIL:      return (uint32_t)((int)EXEC_BASE + LVO_REM_TAIL);
            case EXEC_ENQUEUE:       return (uint32_t)((int)EXEC_BASE + LVO_ENQUEUE);
            case EXEC_FIND_NAME:     return (uint32_t)((int)EXEC_BASE + LVO_FIND_NAME);
            case EXEC_SET_TASK_PRI:  return (uint32_t)((int)EXEC_BASE + LVO_SET_TASK_PRI);
            case EXEC_SET_EXCEPT:    return (uint32_t)((int)EXEC_BASE + LVO_SET_EXCEPT);
            case EXEC_ALLOC_TRAP:    return (uint32_t)((int)EXEC_BASE + LVO_ALLOC_TRAP);
            case EXEC_FREE_TRAP:     return (uint32_t)((int)EXEC_BASE + LVO_FREE_TRAP);
            case EXEC_ADD_PORT:      return (uint32_t)((int)EXEC_BASE + LVO_ADD_PORT);
            case EXEC_REM_PORT:      return (uint32_t)((int)EXEC_BASE + LVO_REM_PORT);
            case EXEC_FIND_PORT:     return (uint32_t)((int)EXEC_BASE + LVO_FIND_PORT);
            case EXEC_OLD_OPEN_LIBRARY: return (uint32_t)((int)EXEC_BASE + LVO_OLD_OPEN_LIBRARY);
            case EXEC_SET_FUNCTION:  return (uint32_t)((int)EXEC_BASE + LVO_SET_FUNCTION);
            case EXEC_OPEN_DEVICE:   return (uint32_t)((int)EXEC_BASE + LVO_OPEN_DEVICE);
            case EXEC_CLOSE_DEVICE:  return (uint32_t)((int)EXEC_BASE + LVO_CLOSE_DEVICE);
            case EXEC_DO_IO:         return (uint32_t)((int)EXEC_BASE + LVO_DO_IO);
            case EXEC_SEND_IO:       return (uint32_t)((int)EXEC_BASE + LVO_SEND_IO);
            case EXEC_CHECK_IO:      return (uint32_t)((int)EXEC_BASE + LVO_CHECK_IO);
            case EXEC_WAIT_IO:       return (uint32_t)((int)EXEC_BASE + LVO_WAIT_IO);
            case EXEC_ABORT_IO:      return (uint32_t)((int)EXEC_BASE + LVO_ABORT_IO);
            case EXEC_OPEN_RESOURCE: return (uint32_t)((int)EXEC_BASE + LVO_OPEN_RESOURCE);
            case EXEC_GETCC:         return (uint32_t)((int)EXEC_BASE + LVO_GETCC);
            case EXEC_TYPE_OF_MEM:   return (uint32_t)((int)EXEC_BASE + LVO_TYPE_OF_MEM);
            case EXEC_PROCURE:       return (uint32_t)((int)EXEC_BASE + LVO_PROCURE);
            case EXEC_VACATE:        return (uint32_t)((int)EXEC_BASE + LVO_VACATE);
            case EXEC_INIT_SEMAPHORE: return (uint32_t)((int)EXEC_BASE + LVO_INIT_SEMAPHORE);
            case EXEC_OBTAIN_SEM: return (uint32_t)((int)EXEC_BASE + LVO_OBTAIN_SEM);
            case EXEC_RELEASE_SEM: return (uint32_t)((int)EXEC_BASE + LVO_RELEASE_SEM);
            case EXEC_ATTEMPT_SEM: return (uint32_t)((int)EXEC_BASE + LVO_ATTEMPT_SEM);
            case EXEC_COPY_MEM:      return (uint32_t)((int)EXEC_BASE + LVO_COPY_MEM);
            case EXEC_COPY_MEM_QUICK: return (uint32_t)((int)EXEC_BASE + LVO_COPY_MEM_QUICK);
            case EXEC_CACHE_CONTROL: return (uint32_t)((int)EXEC_BASE + LVO_CACHE_CONTROL);
            case EXEC_CACHE_CLEAR_E: return (uint32_t)((int)EXEC_BASE + LVO_CACHE_CLEAR_E);
            case EXEC_CREATE_IOREQUEST: return (uint32_t)((int)EXEC_BASE + LVO_CREATE_IOREQUEST);
            case EXEC_DELETE_IOREQUEST: return (uint32_t)((int)EXEC_BASE + LVO_DELETE_IOREQUEST);
            case EXEC_CREATE_MSGPORT:   return (uint32_t)((int)EXEC_BASE + LVO_CREATE_MSGPORT);
            case EXEC_DELETE_MSGPORT:   return (uint32_t)((int)EXEC_BASE + LVO_DELETE_MSGPORT);
            case EXEC_OBTAIN_SEM_LIST:  return (uint32_t)((int)EXEC_BASE + LVO_OBTAIN_SEM_LIST);
            case EXEC_RELEASE_SEM_LIST: return (uint32_t)((int)EXEC_BASE + LVO_RELEASE_SEM_LIST);
            case EXEC_FIND_SEMAPHORE:   return (uint32_t)((int)EXEC_BASE + LVO_FIND_SEMAPHORE);
            case EXEC_ADD_SEMAPHORE:    return (uint32_t)((int)EXEC_BASE + LVO_ADD_SEMAPHORE);
            case EXEC_REM_SEMAPHORE:    return (uint32_t)((int)EXEC_BASE + LVO_REM_SEMAPHORE);
            case EXEC_OBTAIN_SEM_SHARED: return (uint32_t)((int)EXEC_BASE + LVO_OBTAIN_SEM_SHARED);
            case EXEC_ALLOC_VEC:     return (uint32_t)((int)EXEC_BASE + LVO_ALLOC_VEC);
            case EXEC_FREE_VEC:      return (uint32_t)((int)EXEC_BASE + LVO_FREE_VEC);
        }
    } else if (lib_id == LIB_DOS) {
        switch (func_idx) {
            case DOS_OUTPUT:   return (uint32_t)((int)DOS_BASE + LVO_DOS_OUTPUT);
            case DOS_INPUT:    return (uint32_t)((int)DOS_BASE + LVO_DOS_INPUT);
            case DOS_VFPRINTF:      return (uint32_t)((int)DOS_BASE + LVO_DOS_VFPRINTF);
            case DOS_FPUTS:          return (uint32_t)((int)DOS_BASE + LVO_DOS_FPUTS);
            case DOS_PUTSTR:         return (uint32_t)((int)DOS_BASE + LVO_DOS_PUTSTR);
            case DOS_VPRINTF:        return (uint32_t)((int)DOS_BASE + LVO_DOS_VPRINTF);
            case DOS_VFWRITEF:       return (uint32_t)((int)DOS_BASE + LVO_DOS_VFWRITEF);
            case DOS_READARGS:       return (uint32_t)((int)DOS_BASE + LVO_DOS_READARGS);
            case DOS_GETARGSTR:      return (uint32_t)((int)DOS_BASE + LVO_DOS_GET_ARG_STR);
            case DOS_ISINTERACTIVE:  return (uint32_t)((int)DOS_BASE + LVO_DOS_ISINTERACTIVE);
            case DOS_DELETEFILE:     return (uint32_t)((int)DOS_BASE + LVO_DOS_DELETEFILE);
            case DOS_RENAME:         return (uint32_t)((int)DOS_BASE + LVO_DOS_RENAME);
            case DOS_SETPROTECTION:  return (uint32_t)((int)DOS_BASE + LVO_DOS_SETPROTECTION);
            case DOS_GETVAR:         return (uint32_t)((int)DOS_BASE + LVO_DOS_GETVAR);
            case DOS_SETVAR:         return (uint32_t)((int)DOS_BASE + LVO_DOS_SETVAR);
            case DOS_SEEK:           return (uint32_t)((int)DOS_BASE + LVO_DOS_SEEK);
            case DOS_LOCK:           return (uint32_t)((int)DOS_BASE + LVO_DOS_LOCK);
            case DOS_UNLOCK:         return (uint32_t)((int)DOS_BASE + LVO_DOS_UNLOCK);
            case DOS_EXAMINE:        return (uint32_t)((int)DOS_BASE + LVO_DOS_EXAMINE);
            case DOS_EXAMINE_NEXT:   return (uint32_t)((int)DOS_BASE + LVO_DOS_EXAMINE_NEXT);
            case DOS_CREATE_DIR:     return (uint32_t)((int)DOS_BASE + LVO_DOS_CREATE_DIR);
            case DOS_DUPLOCK:        return (uint32_t)((int)DOS_BASE + LVO_DOS_DUPLOCK);
            case DOS_PARENT:         return (uint32_t)((int)DOS_BASE + LVO_DOS_PARENT);
            case DOS_DATE_STAMP:     return (uint32_t)((int)DOS_BASE + LVO_DOS_DATE_STAMP);
            case DOS_DELAY:          return (uint32_t)((int)DOS_BASE + LVO_DOS_DELAY);
            case DOS_DATE_TO_STR:    return (uint32_t)((int)DOS_BASE + LVO_DOS_DATE_TO_STR);
            case DOS_PARSE_PATTERN:       return (uint32_t)((int)DOS_BASE + LVO_DOS_PARSE_PATTERN);
            case DOS_MATCH_PATTERN:       return (uint32_t)((int)DOS_BASE + LVO_DOS_MATCH_PATTERN);
            case DOS_PARSE_PATTERN_NO_CASE: return (uint32_t)((int)DOS_BASE + LVO_DOS_PARSE_PATTERN_NO_CASE);
            case DOS_MATCH_PATTERN_NO_CASE: return (uint32_t)((int)DOS_BASE + LVO_DOS_MATCH_PATTERN_NO_CASE);
            case DOS_LOADSEG:  return (uint32_t)((int)DOS_BASE + LVO_DOS_LOADSEG);
            case DOS_UNLOADSEG: return (uint32_t)((int)DOS_BASE + LVO_DOS_UNLOADSEG);
            case DOS_WRITE:  return (uint32_t)((int)DOS_BASE + LVO_DOS_WRITE);
            case DOS_OPEN:   return (uint32_t)((int)DOS_BASE + LVO_DOS_OPEN);
            case DOS_CLOSE:  return (uint32_t)((int)DOS_BASE + LVO_DOS_CLOSE);
            case DOS_READ:   return (uint32_t)((int)DOS_BASE + LVO_DOS_READ);
            case DOS_EXIT:   return (uint32_t)((int)DOS_BASE + LVO_DOS_EXIT);
            case DOS_IO_ERR: return (uint32_t)((int)DOS_BASE + LVO_DOS_IO_ERR);
            case DOS_CREATE_PROC:    return (uint32_t)((int)DOS_BASE + LVO_DOS_CREATE_PROC);
            case DOS_SYSTEM_TAG_LIST: return (uint32_t)((int)DOS_BASE + LVO_DOS_SYSTEM_TAG_LIST);
            case DOS_RUN_COMMAND:    return (uint32_t)((int)DOS_BASE + LVO_DOS_RUN_COMMAND);
            case DOS_SEND_PKT:       return (uint32_t)((int)DOS_BASE + LVO_DOS_SEND_PKT);
            case DOS_WAIT_PKT:       return (uint32_t)((int)DOS_BASE + LVO_DOS_WAIT_PKT);
            case DOS_REPLY_PKT:      return (uint32_t)((int)DOS_BASE + LVO_DOS_REPLY_PKT);
            case DOS_ADD_PART:       return (uint32_t)((int)DOS_BASE + LVO_DOS_ADD_PART);
            /* DOS_COMPARE_NAMES has no public LVO — internal-only fn id */
            case DOS_STR_TO_DATE:    return (uint32_t)((int)DOS_BASE + LVO_DOS_STR_TO_DATE);
            case DOS_CHECK_SIGNAL:   return (uint32_t)((int)DOS_BASE + LVO_DOS_CHECK_SIGNAL);
            case DOS_WAIT_FOR_CHAR:  return (uint32_t)((int)DOS_BASE + LVO_DOS_WAIT_FOR_CHAR);
            case DOS_NAME_FROM_LOCK: return (uint32_t)((int)DOS_BASE + LVO_DOS_NAME_FROM_LOCK);
            case DOS_LOCK_RECORD:    return (uint32_t)((int)DOS_BASE + LVO_DOS_LOCK_RECORD);
            case DOS_UNLOCK_RECORD:  return (uint32_t)((int)DOS_BASE + LVO_DOS_UNLOCK_RECORD);
            case DOS_GET_CONSOLE_TASK: return (uint32_t)((int)DOS_BASE + LVO_DOS_GET_CONSOLE_TASK);
            case DOS_SET_CONSOLE_TASK: return (uint32_t)((int)DOS_BASE + LVO_DOS_SET_CONSOLE_TASK);
            case DOS_CURRENT_DIR:    return (uint32_t)((int)DOS_BASE + LVO_DOS_CURRENT_DIR);
            case DOS_SET_PROGRAM_DIR: return (uint32_t)((int)DOS_BASE + LVO_DOS_SET_PROGRAM_DIR);
            case DOS_GET_PROGRAM_DIR: return (uint32_t)((int)DOS_BASE + LVO_DOS_GET_PROGRAM_DIR);
            case DOS_SET_IO_ERR:     return (uint32_t)((int)DOS_BASE + LVO_DOS_SET_IO_ERR);
            case DOS_CLI:            return (uint32_t)((int)DOS_BASE + LVO_DOS_CLI);
            case DOS_FIND_CLI_PROC:  return (uint32_t)((int)DOS_BASE + LVO_DOS_FIND_CLI_PROC);
            case DOS_WRITE_CHARS:    return (uint32_t)((int)DOS_BASE + LVO_DOS_WRITE_CHARS);
            case DOS_FREE_ARGS:      return (uint32_t)((int)DOS_BASE + LVO_DOS_FREE_ARGS);
            case DOS_FLUSH:          return (uint32_t)((int)DOS_BASE + LVO_DOS_FLUSH);
            case DOS_SELECT_INPUT:   return (uint32_t)((int)DOS_BASE + LVO_DOS_SELECT_INPUT);
            case DOS_SELECT_OUTPUT:  return (uint32_t)((int)DOS_BASE + LVO_DOS_SELECT_OUTPUT);
            case DOS_EXECUTE:        return (uint32_t)((int)DOS_BASE + LVO_DOS_EXECUTE);
            case DOS_DEVICE_PROC:    return (uint32_t)((int)DOS_BASE + LVO_DOS_DEVICE_PROC);
            case DOS_FAULT:          return (uint32_t)((int)DOS_BASE + LVO_DOS_FAULT);
        }
    } else if (lib_id == LIB_INTUITION) {
        switch (func_idx) {
            case INTUITION_OPEN_LIBRARY: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_OPEN_LIBRARY);
            case INTUITION_CLOSE_LIBRARY: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_CLOSE_LIBRARY);
            case INTUITION_OPEN_WINDOW: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_OPEN_WINDOW);
            case INTUITION_CLOSE_WINDOW: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_CLOSE_WINDOW);
            case INTUITION_WINDOW_TO_FRONT: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_WINDOW_TO_FRONT);
            case INTUITION_WINDOW_TO_BACK: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_WINDOW_TO_BACK);
            case INTUITION_ACTIVATE_WINDOW: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_ACTIVATE_WINDOW);
            case INTUITION_MOVE_WINDOW: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_MOVE_WINDOW);
            case INTUITION_SIZE_WINDOW: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_SIZE_WINDOW);
            case INTUITION_REFRESH_WINDOW: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_REFRESH_WINDOW);
            case INTUITION_MODIFY_IDCMP: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_MODIFY_IDCMP);
            case INTUITION_SET_WINDOW_TITLES: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_SET_WINDOW_TITLES);
            case INTUITION_OPEN_WINDOW_TAGS: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_OPEN_WINDOW_TAGS);
            case INTUITION_OPEN_WORKBENCH: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_OPEN_WORKBENCH);
            case INTUITION_CLOSE_WORKBENCH: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_CLOSE_WORKBENCH);
            case INTUITION_DRAW_BORDER: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_DRAW_BORDER);
            case INTUITION_DRAW_IMAGE: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_DRAW_IMAGE);
            case INTUITION_PRINT_I_TEXT: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_PRINT_I_TEXT);
            case INTUITION_AUTO_REQUEST: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_AUTO_REQUEST);
            case INTUITION_BUILD_SYS_REQUEST: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_BUILD_SYS_REQUEST);
            case INTUITION_FREE_SYS_REQUEST: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_FREE_SYS_REQUEST);
            case INTUITION_EASY_REQUEST: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_EASY_REQUEST);
            case INTUITION_OPEN_SCREEN: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_OPEN_SCREEN);
            case INTUITION_CLOSE_SCREEN: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_CLOSE_SCREEN);
            case INTUITION_MOVE_SCREEN: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_MOVE_SCREEN);
            case INTUITION_SCREEN_TO_FRONT: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_SCREEN_TO_FRONT);
            case INTUITION_SCREEN_TO_BACK: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_SCREEN_TO_BACK);
            case INTUITION_SHOW_TITLE: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_SHOW_TITLE);
            case INTUITION_OPEN_SCREEN_TAGS: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_OPEN_SCREEN_TAGS);
            case INTUITION_SET_MENU_STRIP: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_SET_MENU_STRIP);
            case INTUITION_CLEAR_MENU_STRIP: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_CLEAR_MENU_STRIP);
            case INTUITION_RESET_MENU_STRIP: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_RESET_MENU_STRIP);
            case INTUITION_ITEM_ADDRESS: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_ITEM_ADDRESS);
            case INTUITION_LOCK_PUB_SCREEN: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_LOCK_PUB_SCREEN);
            case INTUITION_UNLOCK_PUB_SCREEN: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_UNLOCK_PUB_SCREEN);
            case INTUITION_LOCK_PUB_SCREEN_LIST: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_LOCK_PUB_SCREEN_LIST);
            case INTUITION_UNLOCK_PUB_SCREEN_LIST: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_UNLOCK_PUB_SCREEN_LIST);
            case INTUITION_SET_POINTER: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_SET_POINTER);
            case INTUITION_CLEAR_POINTER: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_CLEAR_POINTER);
            case INTUITION_SET_WINDOW_POINTER_A: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_SET_WINDOW_POINTER_A);
            case INTUITION_GET_DEF_PREFS: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_GET_DEF_PREFS);
            case INTUITION_GET_PREFS: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_GET_PREFS);
            case INTUITION_SET_PREFS: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_SET_PREFS);
            case INTUITION_LOCK_GUI_PREFS: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_LOCK_GUI_PREFS);
            case INTUITION_UNLOCK_GUI_PREFS: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_UNLOCK_GUI_PREFS);
            case INTUITION_QUERY_OVERSCAN: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_QUERY_OVERSCAN);
            case INTUITION_CURRENT_TIME: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_CURRENT_TIME);
            case INTUITION_DOUBLE_CLICK: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_DOUBLE_CLICK);
            case INTUITION_REPORT_MOUSE: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_REPORT_MOUSE);
            case INTUITION_DISPLAY_BEEP: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_DISPLAY_BEEP);
            case INTUITION_INIT_REQUESTER: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_INIT_REQUESTER);
            case INTUITION_END_REQUEST: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_END_REQUEST);
            case INTUITION_REQUEST: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_REQUEST);
            case INTUITION_VIEW_ADDRESS: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_VIEW_ADDRESS);
            case INTUITION_VIEW_PORT_ADDRESS: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_VIEW_PORT_ADDRESS);
            case INTUITION_GET_SCREEN_DATA: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_GET_SCREEN_DATA);
            case INTUITION_NEXT_PUB_SCREEN: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_NEXT_PUB_SCREEN);
            case INTUITION_SET_DEFAULT_PUB_SCREEN: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_SET_DEFAULT_PUB_SCREEN);
            case INTUITION_LOCK_IBASE: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_LOCK_IBASE);
            case INTUITION_UNLOCK_IBASE: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_UNLOCK_IBASE);
            case INTUITION_SHOW_WINDOW: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_SHOW_WINDOW);
            case INTUITION_HIDE_WINDOW: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_HIDE_WINDOW);
            case INTUITION_WINDOW_LIMITS: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_WINDOW_LIMITS);
            case INTUITION_CHANGE_WINDOW_BOX: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_CHANGE_WINDOW_BOX);
            case INTUITION_GET_SCREEN_DRAW_INFO: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_GET_SCREEN_DRAW_INFO);
            case INTUITION_FREE_SCREEN_DRAW_INFO: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_FREE_SCREEN_DRAW_INFO);
            case INTUITION_DISPLAY_ALERT: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_DISPLAY_ALERT);
            case INTUITION_TIMED_DISPLAY_ALERT: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_TIMED_DISPLAY_ALERT);
            case INTUITION_SCREEN_DEPTH: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_SCREEN_DEPTH);
            case INTUITION_SCREEN_POSITION: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_SCREEN_POSITION);
            case INTUITION_ADD_GADGET: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_ADD_GADGET);
            case INTUITION_ADD_GLIST: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_ADD_GLIST);
            case INTUITION_REMOVE_GADGET: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_REMOVE_GADGET);
            case INTUITION_REMOVE_GLIST: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_REMOVE_GLIST);
            case INTUITION_REFRESH_GLIST: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_REFRESH_GLIST);
            case INTUITION_ON_GADGET: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_ON_GADGET);
            case INTUITION_OFF_GADGET: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_OFF_GADGET);
            case INTUITION_MODIFY_PROP: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_MODIFY_PROP);
            case INTUITION_NEW_MODIFY_PROP: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_NEW_MODIFY_PROP);
            case INTUITION_ACTIVATE_GADGET: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_ACTIVATE_GADGET);
            case INTUITION_SET_WINDOW_ATTRS: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_SET_WINDOW_ATTRS);
            case INTUITION_GET_WINDOW_ATTRS: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_GET_WINDOW_ATTRS);
            case INTUITION_SET_SCREEN_ATTRS: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_SET_SCREEN_ATTRS);
            case INTUITION_GET_SCREEN_ATTRS: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_GET_SCREEN_ATTRS);
            case INTUITION_BEGIN_REFRESH: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_BEGIN_REFRESH);
            case INTUITION_END_REFRESH: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_END_REFRESH);
            case INTUITION_REFRESH_GADGETS: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_REFRESH_GADGETS);
            case INTUITION_ON_MENU: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_ON_MENU);
            case INTUITION_OFF_MENU: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_OFF_MENU);
            case INTUITION_SYS_REQ_HANDLER: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_SYS_REQ_HANDLER);
            case INTUITION_PUB_SCREEN_STATUS: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_PUB_SCREEN_STATUS);
            case INTUITION_GET_DEFAULT_PUB_SCREEN: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_GET_DEFAULT_PUB_SCREEN);
            case INTUITION_MOVE_WINDOW_IN_FRONT_OF: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_MOVE_WINDOW_IN_FRONT_OF);
            case INTUITION_SET_EDIT_HOOK: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_SET_EDIT_HOOK);
            case INTUITION_OBTAIN_GIR_PORT: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_OBTAIN_GIR_PORT);
            case INTUITION_RELEASE_GIR_PORT: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_RELEASE_GIR_PORT);
            case INTUITION_STRIP_INTUI_MESSAGES: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_STRIP_INTUI_MESSAGES);
            case INTUITION_NEW_OBJECT_A: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_NEW_OBJECT_A);
            case INTUITION_DISPOSE_OBJECT: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_DISPOSE_OBJECT);
            case INTUITION_SET_ATTRS_A: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_SET_ATTRS_A);
            case INTUITION_GET_ATTR: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_GET_ATTR);
            case INTUITION_MAKE_CLASS: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_MAKE_CLASS);
            case INTUITION_FREE_CLASS: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_FREE_CLASS);
            case INTUITION_ADD_CLASS: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_ADD_CLASS);
            case INTUITION_REMOVE_CLASS: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_REMOVE_CLASS);
            case INTUITION_NEXT_OBJECT: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_NEXT_OBJECT);
            case INTUITION_GET_ATTRS_A: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_GET_ATTRS_A);
            case INTUITION_DO_GADGET_METHOD_A: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_DO_GADGET_METHOD_A);
            case INTUITION_HELP_CONTROL: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_HELP_CONTROL);
            case INTUITION_START_SCREEN_NOTIFY: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_START_SCREEN_NOTIFY);
            case INTUITION_END_SCREEN_NOTIFY: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_END_SCREEN_NOTIFY);
            case INTUITION_GET_WINDOW_ATTR: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_GET_WINDOW_ATTR);
            case INTUITION_SET_WINDOW_ATTR: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_SET_WINDOW_ATTR);
            case INTUITION_GET_SCREEN_ATTR: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_GET_SCREEN_ATTR);
            case INTUITION_SET_SCREEN_ATTR: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_SET_SCREEN_ATTR);
            case INTUITION_SET_GADGET_ATTRS_A: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_SET_GADGET_ATTRS_A);
            case INTUITION_ALLOC_SCREEN_BUFFER: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_ALLOC_SCREEN_BUFFER);
            case INTUITION_FREE_SCREEN_BUFFER: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_FREE_SCREEN_BUFFER);
            case INTUITION_CHANGE_SCREEN_BUFFER: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_CHANGE_SCREEN_BUFFER);
            case INTUITION_WBENCH_TO_BACK: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_WBENCH_TO_BACK);
            case INTUITION_WBENCH_TO_FRONT: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_WBENCH_TO_FRONT);
            case INTUITION_MAKE_SCREEN: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_MAKE_SCREEN);
            case INTUITION_REMAKE_DISPLAY: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_REMAKE_DISPLAY);
            case INTUITION_RETHINK_DISPLAY: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_RETHINK_DISPLAY);
            case INTUITION_CLEAR_DMREQUEST: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_CLEAR_DMREQUEST);
            case INTUITION_SET_DMREQUEST: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_SET_DMREQUEST);
            case INTUITION_SET_MOUSE_QUEUE: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_SET_MOUSE_QUEUE);
            case INTUITION_SET_PUB_SCREEN_MODES: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_SET_PUB_SCREEN_MODES);
            case INTUITION_LEND_MENUS: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_LEND_MENUS);
            case INTUITION_GADGET_MOUSE: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_GADGET_MOUSE);
            case INTUITION_INTUITEXT_LENGTH: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_INTUITEXT_LENGTH);
            case INTUITION_POINT_IN_IMAGE: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_POINT_IN_IMAGE);
            case INTUITION_ERASE_IMAGE: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_ERASE_IMAGE);
            case INTUITION_ZIP_WINDOW: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_ZIP_WINDOW);
            case INTUITION_REFRESH_SET_GADGET_ATTRS_A: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_REFRESH_SET_GADGET_ATTRS_A);
            case INTUITION_SCROLL_WINDOW_RASTER: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_SCROLL_WINDOW_RASTER);
            case INTUITION_BUILD_EASY_REQUEST_ARGS: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_BUILD_EASY_REQUEST_ARGS);
            case INTUITION_DRAW_IMAGE_STATE: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_DRAW_IMAGE_STATE);
            case INTUITION_ALLOC_REMEMBER: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_ALLOC_REMEMBER);
            case INTUITION_FREE_REMEMBER: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_FREE_REMEMBER);
            case INTUITION_NEW_IMAGE_A: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_NEW_IMAGE_A);
            case INTUITION_DISPOSE_IMAGE: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_DISPOSE_IMAGE);
            case INTUITION_SET_IPREFS: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_SET_IPREFS);
            case INTUITION_GET_HALF_PENS: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_GET_HALF_PENS);
            case INTUITION_GADGET_BOX: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_GADGET_BOX);
            case INTUITION_SET_GUI_ATTRS_A: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_SET_GUI_ATTRS_A);
            case INTUITION_GET_GUI_ATTRS_A: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_GET_GUI_ATTRS_A);
            case INTUITION_OPEN_CLASS: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_OPEN_CLASS);
            case INTUITION_CLOSE_CLASS: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_CLOSE_CLASS);
            case INTUITION_IDO_METHOD_A: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_IDO_METHOD_A);
            case INTUITION_IDO_SUPER_METHOD_A: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_IDO_SUPER_METHOD_A);
            case INTUITION_ICOERCE_METHOD_A: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_ICOERCE_METHOD_A);
            case INTUITION_ISET_SUPER_ATTRS_A: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_ISET_SUPER_ATTRS_A);
            case INTUITION_LOCK_SCREEN: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_LOCK_SCREEN);
            case INTUITION_UNLOCK_SCREEN: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_UNLOCK_SCREEN);
            case INTUITION_LOCK_SCREEN_LIST: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_LOCK_SCREEN_LIST);
            case INTUITION_UNLOCK_SCREEN_LIST: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_UNLOCK_SCREEN_LIST);
            case INTUITION_LOCK_SCREEN_GI: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_LOCK_SCREEN_GI);
            case INTUITION_UNLOCK_SCREEN_GI: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_UNLOCK_SCREEN_GI);
        }
    } else if (lib_id == LIB_GADTOOLS) {
        switch (func_idx) {
            case GADTOOLS_OPEN_LIBRARY:          return (uint32_t)((int)GADTOOLS_BASE + LVO_GADTOOLS_CREATE_GADGET_A - 6);
            case GADTOOLS_CREATE_GADGET_A:       return (uint32_t)((int)GADTOOLS_BASE + LVO_GADTOOLS_CREATE_GADGET_A);
            case GADTOOLS_FREE_GADGETS:          return (uint32_t)((int)GADTOOLS_BASE + LVO_GADTOOLS_FREE_GADGETS);
            case GADTOOLS_GT_SET_GADGET_ATTRS_A: return (uint32_t)((int)GADTOOLS_BASE + LVO_GADTOOLS_GT_SET_GADGET_ATTRS_A);
            case GADTOOLS_CREATE_MENUS_A:        return (uint32_t)((int)GADTOOLS_BASE + LVO_GADTOOLS_CREATE_MENUS_A);
            case GADTOOLS_FREE_MENUS:            return (uint32_t)((int)GADTOOLS_BASE + LVO_GADTOOLS_FREE_MENUS);
            case GADTOOLS_LAYOUT_MENU_ITEMS_A:   return (uint32_t)((int)GADTOOLS_BASE + LVO_GADTOOLS_LAYOUT_MENU_ITEMS_A);
            case GADTOOLS_LAYOUT_MENUS_A:          return (uint32_t)((int)GADTOOLS_BASE + LVO_GADTOOLS_LAYOUT_MENUS_A);
            case GADTOOLS_GT_GET_IMSG:           return (uint32_t)((int)GADTOOLS_BASE + LVO_GADTOOLS_GT_GET_IMSG);
            case GADTOOLS_GT_REPLY_IMSG:         return (uint32_t)((int)GADTOOLS_BASE + LVO_GADTOOLS_GT_REPLY_IMSG);
            case GADTOOLS_GT_REFRESH_WINDOW:     return (uint32_t)((int)GADTOOLS_BASE + LVO_GADTOOLS_GT_REFRESH_WINDOW);
            case GADTOOLS_GT_BEGIN_REFRESH:      return (uint32_t)((int)GADTOOLS_BASE + LVO_GADTOOLS_GT_BEGIN_REFRESH);
            case GADTOOLS_GT_END_REFRESH:        return (uint32_t)((int)GADTOOLS_BASE + LVO_GADTOOLS_GT_END_REFRESH);
            case GADTOOLS_GT_FILTER_IMSG:        return (uint32_t)((int)GADTOOLS_BASE + LVO_GADTOOLS_GT_FILTER_IMSG);
            case GADTOOLS_GT_POST_FILTER_IMSG:   return (uint32_t)((int)GADTOOLS_BASE + LVO_GADTOOLS_GT_POST_FILTER_IMSG);
            case GADTOOLS_CREATE_CONTEXT:        return (uint32_t)((int)GADTOOLS_BASE + LVO_GADTOOLS_CREATE_CONTEXT);
            case GADTOOLS_DRAW_BEVEL_BOX_A:      return (uint32_t)((int)GADTOOLS_BASE + LVO_GADTOOLS_DRAW_BEVEL_BOX_A);
            case GADTOOLS_GET_VISUAL_INFO_A:     return (uint32_t)((int)GADTOOLS_BASE + LVO_GADTOOLS_GET_VISUAL_INFO_A);
            case GADTOOLS_FREE_VISUAL_INFO:      return (uint32_t)((int)GADTOOLS_BASE + LVO_GADTOOLS_FREE_VISUAL_INFO);
            case GADTOOLS_GT_GET_GADGET_ATTRS_A: return (uint32_t)((int)GADTOOLS_BASE + LVO_GADTOOLS_GT_GET_GADGET_ATTRS_A);
        }
    }
    return JMPTAB_BASE;
}

/* Install a stub at base + lvo (lvo is negative) */
static void install_lvo(uint32_t base, int lvo, int lib_id, int func_idx)
{
    uint32_t addr = (uint32_t)((int)base + lvo);
    if (addr >= GUEST_RAM_SIZE - 4) return;
    g_ram[addr]   = 0x4A; g_ram[addr+1] = 0xFC; /* ILLEGAL */
    g_ram[addr+2] = (uint8_t)lib_id;
    g_ram[addr+3] = (uint8_t)func_idx;
    /* RTS after the stub so execution can continue */
    g_ram[addr+4] = 0x4E; g_ram[addr+5] = 0x75; /* RTS */
}

/* Boot-time sanity check (UAOS-252): every fixed library keeps its whole
 * negative jump table (below the base) and positive base struct clear of
 * the exception-vector page and of every other library's range.  Runs
 * once — install_library_tables is also called per-task. */
static void libmap_selfcheck(void)
{
    static int done = 0;
    if (done) return;
    done = 1;
    static const struct { const char *n; uint32_t b, neg, pos; } r[] = {
        { "exec.library",      EXEC_BASE,       996,  0x280 },
        { "dos.library",       DOS_BASE,        996,  0x080 },
        { "bsdsocket.library", BSD_BASE,        216,  0x040 },
        { "graphics.library",  GRAPHICS_BASE,   1056, 0x040 },
        { "intuition.library", INTUITION_BASE,  1236, 0x040 },
        { "gadtools.library",  GADTOOLS_BASE,   174,  0x040 },
        { "audio.device",      AUDIO_DEV_BASE,  48,   0x040 },
        { "fakelib",           FAKE_LIB_BASE,   756,  0x040 },
    };
    int bad = 0;
    for (unsigned i = 0; i < sizeof(r)/sizeof(r[0]); i++) {
        uint32_t lo = r[i].b - r[i].neg, hi = r[i].b + r[i].pos;
        if (lo < 0x100u || hi > GUEST_RAM_SIZE) {
            bad++;
            char m[64]; int k = 0;
            const char *t = "[emu] libmap OUT-OF-RANGE ";
            while (t[k]) { m[k] = t[k]; k++; }
            for (int j = 0; r[i].n[j] && k < 60; j++) m[k++] = r[i].n[j];
            m[k++] = '\n'; m[k] = '\0';
            emu_print(m);
        }
        for (unsigned j = i + 1; j < sizeof(r)/sizeof(r[0]); j++) {
            uint32_t lo2 = r[j].b - r[j].neg, hi2 = r[j].b + r[j].pos;
            if (lo < hi2 && lo2 < hi) {
                bad++;
                char m[80]; int k = 0;
                const char *t = "[emu] libmap OVERLAP ";
                while (t[k]) { m[k] = t[k]; k++; }
                for (int q = 0; r[i].n[q] && k < 66; q++) m[k++] = r[i].n[q];
                m[k++] = ' '; m[k++] = '/'; m[k++] = ' ';
                for (int q = 0; r[j].n[q] && k < 76; q++) m[k++] = r[j].n[q];
                m[k++] = '\n'; m[k] = '\0';
                emu_print(m);
            }
        }
    }
    if (!bad) emu_print("[emu] libmap selfcheck OK\n");
}

/* Library name/idstring arena (UAOS-237): a small C-string pool in the
 * unused low band between the legacy jump table / fake handles and the
 * exec stub floor (0xC1C).  Install-time only — written per window. */
#define LIBSTR_ARENA   0x00000800u

static uint32_t libstr_write(uint32_t *next, const char *s)
{
    uint32_t a = *next, i = 0;
    while (s[i]) { g_ram[a + i] = (uint8_t)s[i]; i++; }
    g_ram[a + i] = 0;
    *next = a + i + 1;
    return a;
}

/* AddTail for guest List headers (same protocol as glue_list_add_tail but
 * usable at table-install time before that helper is defined). */
static void guest_list_add_tail(uint32_t list, uint32_t node)
{
    uint32_t tailpred = guest_read_be32(list + 8);
    guest_write_be32(node + 0, list + 4);
    guest_write_be32(node + 4, tailpred);
    guest_write_be32(tailpred + 0, node);
    guest_write_be32(list + 8, node);
}

static void install_loadable_libs(void);
static int  exec_alloc_sigbit(void);

/* Guest-visible Process/CLI/RDArgs addresses — fixed region published by
 * UAOS_Emu_SetupProcess() so they can never sit inside the program image. */
uint32_t g_guest_proc_addr   = 0;
uint32_t g_guest_cli_addr    = 0;
uint32_t g_guest_rdargs_addr = 0;

/* Dedicated process-environment region (UAOS-237): the whole per-launch
 * Process environment lives at fixed offsets between the loadable-library
 * arena (ends 0x1A000) and the hook-return trap (0x1EF000), well below
 * PROG_BASE.  Both the shared-context and per-task setup paths use these
 * addresses, so no program image — however large — can overlap them, and
 * they are outside every guest allocator (bump heap 0x20000+, chip/fast
 * free lists, genlib arena 0x7F0000+). */
#define PROC_ENV_BASE   0x0001B000u
#define PROC_PROC       (PROC_ENV_BASE + 0x000)  /* struct Process (0xE4)  */
#define PROC_CLI        (PROC_ENV_BASE + 0x100)  /* CommandLineInterface   */
#define PROC_RDARGS     (PROC_ENV_BASE + 0x180)  /* ReadArgs scratch block */
#define PROC_CONPORT    (PROC_ENV_BASE + 0x1C0)  /* console MsgPort        */
#define PROC_NAME_STR   (PROC_ENV_BASE + 0x200)  /* task ln_Name C string  */
#define PROC_SETNAME    (PROC_ENV_BASE + 0x240)  /* BSTR cli_SetName       */
#define PROC_PROMPT     (PROC_ENV_BASE + 0x250)  /* BSTR cli_Prompt        */
#define PROC_CURDIR     (PROC_ENV_BASE + 0x260)  /* BSTR current dir name  */
#define PROC_ENV_END    (PROC_ENV_BASE + 0x400)

/* Stack bounds advertised in the Process (tc_SPLower/tc_SPUpper,
 * pr_StackSize, pr_StackBase) — the real SP starts at STACK_TOP. */
#define PROC_STACK_SIZE 0x00010000u   /* 64 KB */

/* Write a BSTR (len byte + chars) into guest RAM. */
static void proc_env_bstr(uint32_t addr, const char *s, uint32_t max)
{
    uint32_t n = 0;
    while (s && s[n] && n < max - 1) n++;
    g_ram[addr] = (uint8_t)n;
    for (uint32_t i = 0; i < n; i++) g_ram[addr + 1 + i] = (uint8_t)s[i];
}

/* Initialise a MsgPort at `mp` owned by guest task `owner` with a real
 * allocated signal bit (PA_SIGNAL semantics). */
static void proc_env_msgport(uint32_t mp, uint32_t owner)
{
    int sb = exec_alloc_sigbit();
    if (sb < 0) sb = 0;
    g_ram[mp + PMP_LN_TYPE] = NT_MSGPORT_G;
    g_ram[mp + PMP_FLAGS]   = PA_SIGNAL_G;
    g_ram[mp + PMP_SIGBIT]  = (uint8_t)sb;
    guest_write_be32(mp + PMP_SIGTASK, owner);
    guest_write_be32(mp + PMP_MSGLIST + 0, mp + PMP_MSGLIST + 4);
    guest_write_be32(mp + PMP_MSGLIST + 4, 0);
    guest_write_be32(mp + PMP_MSGLIST + 8, mp + PMP_MSGLIST + 0);
}

/* Populate the guest-side Process/Task/CLI environment in the dedicated
 * PROC_ENV region.  Call AFTER hunk_load so pr_SegList/cli_Module can
 * point at the loaded seglist.  cmdname_bptr is the BSTR BPTR of the
 * command name.  Returns the Process struct address. */
uint32_t UAOS_Emu_SetupProcess(uint32_t cmdname_bptr)
{
    uint32_t proc = PROC_PROC;
    uint32_t cli  = PROC_CLI;
    emu_memset(g_ram + PROC_ENV_BASE, 0, PROC_ENV_END - PROC_ENV_BASE);
    g_guest_proc_addr   = proc;
    g_guest_cli_addr    = cli;
    g_guest_rdargs_addr = PROC_RDARGS;

    /* Task name = the command-name BSTR text as a C string. */
    {
        uint32_t nb = cmdname_bptr << 2;
        uint32_t n  = (nb + 1 < GUEST_RAM_SIZE) ? g_ram[nb] : 0;
        if (n > 31) n = 31;
        for (uint32_t i = 0; i < n; i++)
            g_ram[PROC_NAME_STR + i] = g_ram[nb + 1 + i];
        g_ram[PROC_NAME_STR + n] = 0;
    }

    /* Task node + fields — FindTask(NULL) returns this address, so make
     * it a real NT_PROCESS node for name/type/stack probes. */
    g_ram[proc + PRC_LN_TYPE]  = NT_PROCESS_G;
    g_ram[proc + PRC_LN_PRI]   = 0;
    guest_write_be32(proc + PRC_LN_NAME, PROC_NAME_STR);
    g_ram[proc + PRC_TC_FLAGS]  = TF_PROCTASK_G;
    g_ram[proc + PRC_TC_STATE]  = TS_RUN_G;
    g_ram[proc + PRC_TC_IDNEST] = (uint8_t)-1;
    g_ram[proc + PRC_TC_TDNEST] = (uint8_t)-1;
    guest_write_be32(proc + PRC_TC_SIGALLOC, 0xFFFFFFFFu);
    guest_write_be32(proc + PRC_TC_SPREG,   STACK_TOP);
    guest_write_be32(proc + PRC_TC_SPLOWER, STACK_TOP - PROC_STACK_SIZE);
    guest_write_be32(proc + PRC_TC_SPUPPER, STACK_TOP);

    /* pr_MsgPort (+0x5C) and the console port: real signal-allocated ports
     * so WaitPort/PutMsg on them behaves like AmigaOS.  pr_ConsoleTask is
     * what dos.library/GetConsoleTask returns and what startup code uses
     * to reach the console handler. */
    proc_env_msgport(proc + PRC_MSGPORT, proc);
    proc_env_msgport(PROC_CONPORT, proc);

    /* Seglist BPTR of the loaded program (hunk_load writes LoadSeg-style
     * [size][next][data] headers; the seglist is the first link field). */
    uint32_t seglist = (g_hunk_count > 0) ? ((g_hunk_blk[0] + 4) >> 2) : 0;

    guest_write_be32(proc + PRC_SEGLIST,     seglist);
    guest_write_be32(proc + PRC_STACKSIZE,   PROC_STACK_SIZE);
    guest_write_be32(proc + PRC_GLOBVEC,     DOS_BASE);
    guest_write_be32(proc + PRC_TASKNUM,     1);
    guest_write_be32(proc + PRC_STACKBASE,   (STACK_TOP - PROC_STACK_SIZE) >> 2);
    guest_write_be32(proc + PR_CIS_OFFSET,   DOS_STDIN_BPTR);   /* pr_CIS */
    guest_write_be32(proc + PR_COS_OFFSET,   DOS_STDOUT_BPTR);  /* pr_COS */
    guest_write_be32(proc + PRC_CONSOLETASK, PROC_CONPORT);
    /* pr_FileSystemTask (+0xA8) stays 0 — packets go through the host VFS. */
    guest_write_be32(proc + PR_CLI_OFFSET,   cli >> 2);
    guest_write_be32(proc + PRC_WINDOWPTR,   0xFFFFFFFFu); /* no req window */
    /* pr_LocalVars (+0xD0): initialise as an empty MinList. */
    guest_write_be32(proc + PRC_LOCALVARS + 0, proc + PRC_LOCALVARS + 4);
    guest_write_be32(proc + PRC_LOCALVARS + 4, 0);
    guest_write_be32(proc + PRC_LOCALVARS + 8, proc + PRC_LOCALVARS + 0);

    /* CommandLineInterface — the fields a CLI-launched binary checks for
     * Workbench-vs-CLI detection (pr_CLI), argument retrieval
     * (cli_CommandName/cli_CommandLine), module identity (cli_Module) and
     * console state. */
    proc_env_bstr(PROC_SETNAME, "SYS:", 16);
    proc_env_bstr(PROC_PROMPT,  "%N.%S>", 16);
    proc_env_bstr(PROC_CURDIR,  g_uaos_cwd[0] ? g_uaos_cwd : "SYS:", 160);
    guest_write_be32(cli + CLI_SETNAME,        PROC_SETNAME >> 2);
    guest_write_be32(cli + CLI_COMMANDNAME,    cmdname_bptr);
    guest_write_be32(cli + CLI_FAILLEVEL,      10);
    guest_write_be32(cli + CLI_PROMPT,         PROC_PROMPT >> 2);
    guest_write_be32(cli + CLI_DEFAULTINPUT,   DOS_STDIN_BPTR);
    guest_write_be32(cli + CLI_DEFAULTOUTPUT,  DOS_STDOUT_BPTR);
    guest_write_be32(cli + CLI_ERRORLEVEL,     10);
    /* cli_Background (+0x28) = 0 — foreground command. */
    guest_write_be32(cli + CLI_CURRENTINPUT,   g_cmdline_bptr); /* cli_CommandLine */
    guest_write_be32(cli + CLI_CURRENTOUTPUT,  DOS_STDOUT_BPTR);
    guest_write_be32(cli + CLI_CURRENTDIRNAME, PROC_CURDIR >> 2);
    guest_write_be32(cli + CLI_MODULE_SC,      seglist); /* classic/SAS-C slot */
    guest_write_be32(cli + CLI_INTERACTIVE,    1);
    guest_write_be32(cli + CLI_MODULE,         seglist); /* extended slot */

    /* ExecBase+0x114 = ThisTask — guest programs read their Process here. */
    guest_write_be32(EXEC_BASE + 0x114, proc);
    return proc;
}

void install_library_tables(void)
{
    /* Exception vectors: autovectors 1-7 -> IRQ dispatch stubs (UAOS-241).
     * Vector number 24+level at byte address 0x60+level*4; the stub is a
     * 4-byte ILLEGAL word tagged (LIB_IRQ, level).  Every context (shared
     * boot and per-task) gets these so real m68k_set_irq() delivery never
     * vectors to a zero entry. */
    for (int lvl = 1; lvl <= 7; lvl++) {
        uint32_t s = IRQ_STUB_BASE + (uint32_t)lvl * 4u;
        g_ram[s + 0] = 0x4A; g_ram[s + 1] = 0xFC;
        g_ram[s + 2] = LIB_IRQ; g_ram[s + 3] = (uint8_t)lvl;
        guest_write_be32(0x60u + (uint32_t)lvl * 4u, s);
    }

    /* Pre-fill ALL DOS LVO slots (-6 .. -996) with ILLEGAL catch-all stubs
     * so an unimplemented call logs "[dos] unimpl lvo=N" instead of silently
     * returning 0 or wandering off.  Real handlers install over these. */
    for (int lvo = -6; lvo >= -996; lvo -= 6) {
        uint32_t addr = (uint32_t)((int)DOS_BASE + lvo);
        if (addr < 0x100) break;
        install_lvo(DOS_BASE, lvo, LIB_DOS, DOS_STUB_LVO);
    }

    /* exec.library at EXEC_BASE — catch-all stubs for every LVO we do not
     * implement so a guest call logs "[exec] unimpl lvo=…" instead of
     * marching into zeroed RAM.  Stubs cover -6..-996 (all of V40) and now
     * land at 0xC1C+, clear of the exception-vector page (UAOS-252). */
    for (int lvo = -6; lvo >= -996; lvo -= 6) {
        uint32_t a = (uint32_t)((int)EXEC_BASE + lvo);
        if (a < 0x100) break;
        install_lvo(EXEC_BASE, lvo, LIB_EXEC, EXEC_STUB_LVO);
    }

    /* Dynamic system fields: nest counters start at -1 (enabled),
     * AttnFlags claims a 68020. */
    g_ram[EXEC_BASE + 0x126] = (uint8_t)-1;  /* IDNestCnt */
    g_ram[EXEC_BASE + 0x127] = (uint8_t)-1;  /* TDNestCnt */
    guest_write_be16(EXEC_BASE + 0x128, 0x0003); /* AttnFlags: 68010+68020 */
    g_ram[EXEC_BASE + 0x212] = 50;           /* VBlankFrequency  (V36) */
    g_ram[EXEC_BASE + 0x213] = 50;           /* PowerSupplyFreq  (V36) */

    /* System list headers — canonical execbase.i offsets.  An Amiga List
     * header is: lh_Head=&lh_Tail, lh_Tail=NULL, lh_TailPred=&lh_Head. */
    static const uint16_t eb_lists[] = {
        0x142, /* MemList */
        0x150, /* ResourceList */
        0x15E, /* DeviceList */
        0x16C, /* IntrList */
        0x17A, /* LibList */
        0x188, /* PortList */
        0x196, /* TaskReady */
        0x1A4, /* TaskWait */
        0x214, /* SemaphoreList (V36) */
    };
    for (unsigned li = 0; li < sizeof(eb_lists)/sizeof(eb_lists[0]); li++) {
        uint32_t h = EXEC_BASE + eb_lists[li];
        guest_write_be32(h + 0, h + 4);
        guest_write_be32(h + 4, 0);
        guest_write_be32(h + 8, h);
    }

    /* Real struct Library headers on every guest-visible base (UAOS-237):
     * ln_Type + ln_Name + lib_NegSize/PosSize + lib_Version/lib_Revision +
     * lib_IdString + lib_OpenCnt, so OpenLibrary("x.library", 37) version
     * checks and FindName(&SysBase->LibList) see plausible data instead of
     * zeros.  Names/idstrings live in the low-band string arena (0x800).
     * Report version 37 (Kickstart 2.04) — OctaMED requires WB 2.04+. */
    {
        uint32_t strp = LIBSTR_ARENA;
        static const struct {
            uint32_t base; const char *name;
            uint16_t neg, pos; uint8_t ntype; uint16_t list;
        } hdrs[] = {
            { EXEC_BASE,      "exec.library",      0x3EA, 0x280, NT_LIBRARY_G, 0x17A },
            { DOS_BASE,       "dos.library",       0x3EA, 0x080, NT_LIBRARY_G, 0x17A },
            { BSD_BASE,       "bsdsocket.library", 0x0D8, 0x040, NT_LIBRARY_G, 0x17A },
            { GRAPHICS_BASE,  "graphics.library",  0x420, 0x040, NT_LIBRARY_G, 0x17A },
            { INTUITION_BASE, "intuition.library", 0x4D4, 0x040, NT_LIBRARY_G, 0x17A },
            { GADTOOLS_BASE,  "gadtools.library",  0x0AE, 0x040, NT_LIBRARY_G, 0x17A },
            { AUDIO_DEV_BASE, "audio.device",      0x030, 0x040, NT_DEVICE_G,  0x15E },
            { FAKE_LIB_BASE,  "fakelib.library",   0x2F4, 0x040, NT_LIBRARY_G, 0x000 },
        };
        for (unsigned i = 0; i < sizeof(hdrs)/sizeof(hdrs[0]); i++) {
            uint32_t b   = hdrs[i].base;
            uint32_t np  = libstr_write(&strp, hdrs[i].name);
            uint32_t ids = libstr_write(&strp, hdrs[i].name);
            g_ram[b + 8] = hdrs[i].ntype;                /* ln_Type      */
            guest_write_be32(b + 10, np);                /* ln_Name      */
            guest_write_be16(b + 16, hdrs[i].neg);       /* lib_NegSize  */
            guest_write_be16(b + 18, hdrs[i].pos);       /* lib_PosSize  */
            guest_write_be16(b + 20, 37);                /* lib_Version  */
            guest_write_be16(b + 22, 3);                 /* lib_Revision */
            guest_write_be32(b + 24, ids);               /* lib_IdString */
            guest_write_be16(b + 32, 1);                 /* lib_OpenCnt  */
            if (hdrs[i].list)
                guest_list_add_tail(EXEC_BASE + hdrs[i].list, b);
        }
    }

    install_lvo(EXEC_BASE, LVO_OPEN_LIBRARY,  LIB_EXEC, EXEC_OPEN_LIBRARY);
    install_lvo(EXEC_BASE, LVO_CLOSE_LIBRARY, LIB_EXEC, EXEC_CLOSE_LIBRARY);
    install_lvo(EXEC_BASE, LVO_ALLOC_MEM,     LIB_EXEC, EXEC_ALLOC_MEM);
    install_lvo(EXEC_BASE, LVO_FREE_MEM,      LIB_EXEC, EXEC_FREE_MEM);
    install_lvo(EXEC_BASE, LVO_FIND_TASK,     LIB_EXEC, EXEC_FIND_TASK);
    install_lvo(EXEC_BASE, LVO_WAIT,          LIB_EXEC, EXEC_WAIT);
    install_lvo(EXEC_BASE, LVO_SIGNAL,        LIB_EXEC, EXEC_SIGNAL);
    install_lvo(EXEC_BASE, LVO_SETSIGNAL,     LIB_EXEC, EXEC_SETSIGNAL);
    install_lvo(EXEC_BASE, LVO_ALLOC_SIGNAL,  LIB_EXEC, EXEC_ALLOC_SIGNAL);
    install_lvo(EXEC_BASE, LVO_FREE_SIGNAL,   LIB_EXEC, EXEC_FREE_SIGNAL);
    install_lvo(EXEC_BASE, LVO_PUT_MSG,       LIB_EXEC, EXEC_PUT_MSG);
    install_lvo(EXEC_BASE, LVO_GET_MSG,       LIB_EXEC, EXEC_GET_MSG);
    install_lvo(EXEC_BASE, LVO_REPLY_MSG,     LIB_EXEC, EXEC_REPLY_MSG);
    install_lvo(EXEC_BASE, LVO_WAIT_PORT,     LIB_EXEC, EXEC_WAIT_PORT);
    install_lvo(EXEC_BASE, LVO_CACHE_CLEAR_U, LIB_EXEC, EXEC_CACHE_CLEAR_U);
    install_lvo(EXEC_BASE, LVO_INIT_STRUCT,   LIB_EXEC, EXEC_INIT_STRUCT);
    install_lvo(EXEC_BASE, LVO_DISABLE,       LIB_EXEC, EXEC_DISABLE);
    install_lvo(EXEC_BASE, LVO_ENABLE,        LIB_EXEC, EXEC_ENABLE);
    install_lvo(EXEC_BASE, LVO_FORBID,        LIB_EXEC, EXEC_FORBID);
    install_lvo(EXEC_BASE, LVO_PERMIT,        LIB_EXEC, EXEC_PERMIT);
    install_lvo(EXEC_BASE, LVO_SUPER_STATE,   LIB_EXEC, EXEC_SUPER_STATE);
    install_lvo(EXEC_BASE, LVO_USER_STATE,    LIB_EXEC, EXEC_USER_STATE);
    install_lvo(EXEC_BASE, LVO_SET_INT_VECTOR, LIB_EXEC, EXEC_SET_INT_VECTOR);
    install_lvo(EXEC_BASE, LVO_ADD_INT_SERVER, LIB_EXEC, EXEC_ADD_INT_SERVER);
    install_lvo(EXEC_BASE, LVO_REM_INT_SERVER, LIB_EXEC, EXEC_REM_INT_SERVER);
    install_lvo(EXEC_BASE, LVO_CAUSE,          LIB_EXEC, EXEC_CAUSE);
    install_lvo(EXEC_BASE, LVO_AVAIL_MEM,     LIB_EXEC, EXEC_AVAIL_MEM);
    install_lvo(EXEC_BASE, LVO_ALLOC_ENTRY,   LIB_EXEC, EXEC_ALLOC_ENTRY);
    install_lvo(EXEC_BASE, LVO_FREE_ENTRY,    LIB_EXEC, EXEC_FREE_ENTRY);
    install_lvo(EXEC_BASE, LVO_INSERT,        LIB_EXEC, EXEC_INSERT);
    install_lvo(EXEC_BASE, LVO_ADD_HEAD,      LIB_EXEC, EXEC_ADD_HEAD);
    install_lvo(EXEC_BASE, LVO_ADD_TAIL,      LIB_EXEC, EXEC_ADD_TAIL);
    install_lvo(EXEC_BASE, LVO_REMOVE,        LIB_EXEC, EXEC_REMOVE);
    install_lvo(EXEC_BASE, LVO_REM_HEAD,      LIB_EXEC, EXEC_REM_HEAD);
    install_lvo(EXEC_BASE, LVO_REM_TAIL,      LIB_EXEC, EXEC_REM_TAIL);
    install_lvo(EXEC_BASE, LVO_ENQUEUE,       LIB_EXEC, EXEC_ENQUEUE);
    install_lvo(EXEC_BASE, LVO_FIND_NAME,     LIB_EXEC, EXEC_FIND_NAME);
    install_lvo(EXEC_BASE, LVO_SET_TASK_PRI,  LIB_EXEC, EXEC_SET_TASK_PRI);
    install_lvo(EXEC_BASE, LVO_SET_EXCEPT,    LIB_EXEC, EXEC_SET_EXCEPT);
    install_lvo(EXEC_BASE, LVO_ALLOC_TRAP,    LIB_EXEC, EXEC_ALLOC_TRAP);
    install_lvo(EXEC_BASE, LVO_FREE_TRAP,     LIB_EXEC, EXEC_FREE_TRAP);
    install_lvo(EXEC_BASE, LVO_ADD_PORT,      LIB_EXEC, EXEC_ADD_PORT);
    install_lvo(EXEC_BASE, LVO_REM_PORT,      LIB_EXEC, EXEC_REM_PORT);
    install_lvo(EXEC_BASE, LVO_FIND_PORT,     LIB_EXEC, EXEC_FIND_PORT);
    install_lvo(EXEC_BASE, LVO_OLD_OPEN_LIBRARY, LIB_EXEC, EXEC_OLD_OPEN_LIBRARY);
    install_lvo(EXEC_BASE, LVO_SET_FUNCTION,  LIB_EXEC, EXEC_SET_FUNCTION);
    install_lvo(EXEC_BASE, LVO_OPEN_DEVICE,   LIB_EXEC, EXEC_OPEN_DEVICE);
    install_lvo(EXEC_BASE, LVO_CLOSE_DEVICE,  LIB_EXEC, EXEC_CLOSE_DEVICE);
    install_lvo(EXEC_BASE, LVO_DO_IO,         LIB_EXEC, EXEC_DO_IO);
    install_lvo(EXEC_BASE, LVO_SEND_IO,       LIB_EXEC, EXEC_SEND_IO);
    install_lvo(EXEC_BASE, LVO_CHECK_IO,      LIB_EXEC, EXEC_CHECK_IO);
    install_lvo(EXEC_BASE, LVO_WAIT_IO,       LIB_EXEC, EXEC_WAIT_IO);
    install_lvo(EXEC_BASE, LVO_ABORT_IO,      LIB_EXEC, EXEC_ABORT_IO);
    install_lvo(EXEC_BASE, LVO_OPEN_RESOURCE, LIB_EXEC, EXEC_OPEN_RESOURCE);
    install_lvo(EXEC_BASE, LVO_GETCC,         LIB_EXEC, EXEC_GETCC);
    install_lvo(EXEC_BASE, LVO_TYPE_OF_MEM,   LIB_EXEC, EXEC_TYPE_OF_MEM);
    install_lvo(EXEC_BASE, LVO_PROCURE,       LIB_EXEC, EXEC_PROCURE);
    install_lvo(EXEC_BASE, LVO_VACATE,        LIB_EXEC, EXEC_VACATE);
    install_lvo(EXEC_BASE, LVO_INIT_SEMAPHORE, LIB_EXEC, EXEC_INIT_SEMAPHORE);
    install_lvo(EXEC_BASE, LVO_OBTAIN_SEM, LIB_EXEC, EXEC_OBTAIN_SEM);
    install_lvo(EXEC_BASE, LVO_RELEASE_SEM, LIB_EXEC, EXEC_RELEASE_SEM);
    install_lvo(EXEC_BASE, LVO_ATTEMPT_SEM, LIB_EXEC, EXEC_ATTEMPT_SEM);
    install_lvo(EXEC_BASE, LVO_COPY_MEM,      LIB_EXEC, EXEC_COPY_MEM);
    install_lvo(EXEC_BASE, LVO_COPY_MEM_QUICK, LIB_EXEC, EXEC_COPY_MEM_QUICK);
    install_lvo(EXEC_BASE, LVO_CACHE_CONTROL, LIB_EXEC, EXEC_CACHE_CONTROL);
    install_lvo(EXEC_BASE, LVO_CACHE_CLEAR_E, LIB_EXEC, EXEC_CACHE_CLEAR_E);
    install_lvo(EXEC_BASE, LVO_RAW_DO_FMT,     LIB_EXEC, EXEC_RAW_DO_FMT);
    install_lvo(EXEC_BASE, LVO_CREATE_IOREQUEST, LIB_EXEC, EXEC_CREATE_IOREQUEST);
    install_lvo(EXEC_BASE, LVO_DELETE_IOREQUEST, LIB_EXEC, EXEC_DELETE_IOREQUEST);
    install_lvo(EXEC_BASE, LVO_CREATE_MSGPORT,   LIB_EXEC, EXEC_CREATE_MSGPORT);
    install_lvo(EXEC_BASE, LVO_DELETE_MSGPORT,   LIB_EXEC, EXEC_DELETE_MSGPORT);
    install_lvo(EXEC_BASE, LVO_OBTAIN_SEM_LIST,  LIB_EXEC, EXEC_OBTAIN_SEM_LIST);
    install_lvo(EXEC_BASE, LVO_RELEASE_SEM_LIST, LIB_EXEC, EXEC_RELEASE_SEM_LIST);
    install_lvo(EXEC_BASE, LVO_FIND_SEMAPHORE,   LIB_EXEC, EXEC_FIND_SEMAPHORE);
    install_lvo(EXEC_BASE, LVO_ADD_SEMAPHORE,    LIB_EXEC, EXEC_ADD_SEMAPHORE);
    install_lvo(EXEC_BASE, LVO_REM_SEMAPHORE,    LIB_EXEC, EXEC_REM_SEMAPHORE);
    install_lvo(EXEC_BASE, LVO_OBTAIN_SEM_SHARED, LIB_EXEC, EXEC_OBTAIN_SEM_SHARED);
    install_lvo(EXEC_BASE, LVO_ALLOC_VEC,     LIB_EXEC, EXEC_ALLOC_VEC);
    install_lvo(EXEC_BASE, LVO_FREE_VEC,      LIB_EXEC, EXEC_FREE_VEC);
    install_lvo(EXEC_BASE, LVO_STACK_SWAP,    LIB_EXEC, EXEC_STACK_SWAP);

    /* dos.library at DOS_BASE */
    install_lvo(DOS_BASE, LVO_DOS_OUTPUT,   LIB_DOS, DOS_OUTPUT);
    install_lvo(DOS_BASE, LVO_DOS_INPUT,    LIB_DOS, DOS_INPUT);
    install_lvo(DOS_BASE, LVO_DOS_VFPRINTF,     LIB_DOS, DOS_VFPRINTF);
    install_lvo(DOS_BASE, LVO_DOS_FPUTS,         LIB_DOS, DOS_FPUTS);
    install_lvo(DOS_BASE, LVO_DOS_PUTSTR,        LIB_DOS, DOS_PUTSTR);
    install_lvo(DOS_BASE, LVO_DOS_VPRINTF,       LIB_DOS, DOS_VPRINTF);
    install_lvo(DOS_BASE, LVO_DOS_VFWRITEF,      LIB_DOS, DOS_VFWRITEF);
    install_lvo(DOS_BASE, LVO_DOS_READARGS,      LIB_DOS, DOS_READARGS);
    install_lvo(DOS_BASE, LVO_DOS_GET_ARG_STR,   LIB_DOS, DOS_GETARGSTR);
    install_lvo(DOS_BASE, LVO_DOS_ISINTERACTIVE, LIB_DOS, DOS_ISINTERACTIVE);
    install_lvo(DOS_BASE, LVO_DOS_WRITE,  LIB_DOS, DOS_WRITE);
    install_lvo(DOS_BASE, LVO_DOS_OPEN,   LIB_DOS, DOS_OPEN);
    install_lvo(DOS_BASE, LVO_DOS_CLOSE,  LIB_DOS, DOS_CLOSE);
    install_lvo(DOS_BASE, LVO_DOS_READ,   LIB_DOS, DOS_READ);
    install_lvo(DOS_BASE, LVO_DOS_EXIT,   LIB_DOS, DOS_EXIT);
    install_lvo(DOS_BASE, LVO_DOS_IO_ERR, LIB_DOS, DOS_IO_ERR);
    install_lvo(DOS_BASE, LVO_DOS_DELETEFILE, LIB_DOS, DOS_DELETEFILE);
    install_lvo(DOS_BASE, LVO_DOS_RENAME,     LIB_DOS, DOS_RENAME);
    install_lvo(DOS_BASE, LVO_DOS_SETPROTECTION, LIB_DOS, DOS_SETPROTECTION);
    install_lvo(DOS_BASE, LVO_DOS_GETVAR,     LIB_DOS, DOS_GETVAR);
    install_lvo(DOS_BASE, LVO_DOS_SETVAR,     LIB_DOS, DOS_SETVAR);
    install_lvo(DOS_BASE, LVO_DOS_SEEK,        LIB_DOS, DOS_SEEK);
    install_lvo(DOS_BASE, LVO_DOS_LOCK,        LIB_DOS, DOS_LOCK);
    install_lvo(DOS_BASE, LVO_DOS_UNLOCK,      LIB_DOS, DOS_UNLOCK);
    install_lvo(DOS_BASE, LVO_DOS_EXAMINE,     LIB_DOS, DOS_EXAMINE);
    install_lvo(DOS_BASE, LVO_DOS_EXAMINE_NEXT, LIB_DOS, DOS_EXAMINE_NEXT);
    install_lvo(DOS_BASE, LVO_DOS_CREATE_DIR,  LIB_DOS, DOS_CREATE_DIR);
    install_lvo(DOS_BASE, LVO_DOS_DUPLOCK,     LIB_DOS, DOS_DUPLOCK);
    install_lvo(DOS_BASE, LVO_DOS_PARENT,      LIB_DOS, DOS_PARENT);
    install_lvo(DOS_BASE, LVO_DOS_DATE_STAMP,  LIB_DOS, DOS_DATE_STAMP);
    install_lvo(DOS_BASE, LVO_DOS_DELAY,       LIB_DOS, DOS_DELAY);
    install_lvo(DOS_BASE, LVO_DOS_DATE_TO_STR, LIB_DOS, DOS_DATE_TO_STR);
    install_lvo(DOS_BASE, LVO_DOS_PARSE_PATTERN,       LIB_DOS, DOS_PARSE_PATTERN);
    install_lvo(DOS_BASE, LVO_DOS_MATCH_PATTERN,       LIB_DOS, DOS_MATCH_PATTERN);
    install_lvo(DOS_BASE, LVO_DOS_PARSE_PATTERN_NO_CASE, LIB_DOS, DOS_PARSE_PATTERN_NO_CASE);
    install_lvo(DOS_BASE, LVO_DOS_MATCH_PATTERN_NO_CASE, LIB_DOS, DOS_MATCH_PATTERN_NO_CASE);
    install_lvo(DOS_BASE, LVO_DOS_LOADSEG,    LIB_DOS, DOS_LOADSEG);
    install_lvo(DOS_BASE, LVO_DOS_UNLOADSEG,  LIB_DOS, DOS_UNLOADSEG);
    install_lvo(DOS_BASE, LVO_DOS_CREATE_PROC,    LIB_DOS, DOS_CREATE_PROC);
    install_lvo(DOS_BASE, LVO_DOS_SYSTEM_TAG_LIST, LIB_DOS, DOS_SYSTEM_TAG_LIST);
    install_lvo(DOS_BASE, LVO_DOS_RUN_COMMAND,    LIB_DOS, DOS_RUN_COMMAND);
    install_lvo(DOS_BASE, LVO_DOS_SEND_PKT,       LIB_DOS, DOS_SEND_PKT);
    install_lvo(DOS_BASE, LVO_DOS_WAIT_PKT,       LIB_DOS, DOS_WAIT_PKT);
    install_lvo(DOS_BASE, LVO_DOS_REPLY_PKT,      LIB_DOS, DOS_REPLY_PKT);
    install_lvo(DOS_BASE, LVO_DOS_ADD_PART,       LIB_DOS, DOS_ADD_PART);
    install_lvo(DOS_BASE, LVO_DOS_CURRENT_DIR,   LIB_DOS, DOS_CURRENT_DIR);
    install_lvo(DOS_BASE, LVO_DOS_SET_PROGRAM_DIR, LIB_DOS, DOS_SET_PROGRAM_DIR);
    install_lvo(DOS_BASE, LVO_DOS_GET_PROGRAM_DIR, LIB_DOS, DOS_GET_PROGRAM_DIR);
    install_lvo(DOS_BASE, LVO_DOS_SET_IO_ERR,    LIB_DOS, DOS_SET_IO_ERR);
    install_lvo(DOS_BASE, LVO_DOS_CLI,           LIB_DOS, DOS_CLI);
    install_lvo(DOS_BASE, LVO_DOS_FIND_CLI_PROC, LIB_DOS, DOS_FIND_CLI_PROC);
    install_lvo(DOS_BASE, LVO_DOS_WRITE_CHARS,   LIB_DOS, DOS_WRITE_CHARS);
    install_lvo(DOS_BASE, LVO_DOS_FREE_ARGS,     LIB_DOS, DOS_FREE_ARGS);
    install_lvo(DOS_BASE, LVO_DOS_FLUSH,         LIB_DOS, DOS_FLUSH);
    install_lvo(DOS_BASE, LVO_DOS_SELECT_INPUT,  LIB_DOS, DOS_SELECT_INPUT);
    install_lvo(DOS_BASE, LVO_DOS_SELECT_OUTPUT, LIB_DOS, DOS_SELECT_OUTPUT);
    install_lvo(DOS_BASE, LVO_DOS_EXECUTE,       LIB_DOS, DOS_EXECUTE);
    install_lvo(DOS_BASE, LVO_DOS_DEVICE_PROC,   LIB_DOS, DOS_DEVICE_PROC);
    install_lvo(DOS_BASE, LVO_DOS_STR_TO_DATE,    LIB_DOS, DOS_STR_TO_DATE);
    install_lvo(DOS_BASE, LVO_DOS_CHECK_SIGNAL,   LIB_DOS, DOS_CHECK_SIGNAL);
    install_lvo(DOS_BASE, LVO_DOS_WAIT_FOR_CHAR,  LIB_DOS, DOS_WAIT_FOR_CHAR);
    install_lvo(DOS_BASE, LVO_DOS_NAME_FROM_LOCK, LIB_DOS, DOS_NAME_FROM_LOCK);
    install_lvo(DOS_BASE, LVO_DOS_LOCK_RECORD,    LIB_DOS, DOS_LOCK_RECORD);
    install_lvo(DOS_BASE, LVO_DOS_UNLOCK_RECORD,  LIB_DOS, DOS_UNLOCK_RECORD);
    install_lvo(DOS_BASE, LVO_DOS_GET_CONSOLE_TASK, LIB_DOS, DOS_GET_CONSOLE_TASK);
    install_lvo(DOS_BASE, LVO_DOS_SET_CONSOLE_TASK, LIB_DOS, DOS_SET_CONSOLE_TASK);
    install_lvo(DOS_BASE, LVO_DOS_FAULT,           LIB_DOS, DOS_FAULT);

    /* bsdsocket.library at BSD_BASE — pre-fill range with MOVEQ #0,D0 + RTS */
    for (int lvo = -6; lvo >= -216; lvo -= 6) {
        uint32_t addr = (uint32_t)((int)BSD_BASE + lvo);
        if (addr < GUEST_RAM_SIZE - 4) {
            g_ram[addr]   = 0x70; g_ram[addr+1] = 0x00;
            g_ram[addr+2] = 0x4E; g_ram[addr+3] = 0x75;
        }
    }
    install_lvo(BSD_BASE, LVO_BSD_SOCKET,        LIB_BSDSOCKET, BSD_FN_SOCKET);
    install_lvo(BSD_BASE, LVO_BSD_BIND,          LIB_BSDSOCKET, BSD_FN_BIND);
    install_lvo(BSD_BASE, LVO_BSD_LISTEN,        LIB_BSDSOCKET, BSD_FN_LISTEN);
    install_lvo(BSD_BASE, LVO_BSD_ACCEPT,        LIB_BSDSOCKET, BSD_FN_ACCEPT);
    install_lvo(BSD_BASE, LVO_BSD_CONNECT,       LIB_BSDSOCKET, BSD_FN_CONNECT);
    install_lvo(BSD_BASE, LVO_BSD_SEND,          LIB_BSDSOCKET, BSD_FN_SEND);
    install_lvo(BSD_BASE, LVO_BSD_SENDTO,        LIB_BSDSOCKET, BSD_FN_SENDTO);
    install_lvo(BSD_BASE, LVO_BSD_RECV,          LIB_BSDSOCKET, BSD_FN_RECV);
    install_lvo(BSD_BASE, LVO_BSD_RECVFROM,      LIB_BSDSOCKET, BSD_FN_RECVFROM);
    install_lvo(BSD_BASE, LVO_BSD_CLOSESOCKET,   LIB_BSDSOCKET, BSD_FN_CLOSESOCKET);
    install_lvo(BSD_BASE, LVO_BSD_SETSOCKOPT,    LIB_BSDSOCKET, BSD_FN_SETSOCKOPT);
    install_lvo(BSD_BASE, LVO_BSD_GETSOCKOPT,    LIB_BSDSOCKET, BSD_FN_GETSOCKOPT);
    install_lvo(BSD_BASE, LVO_BSD_IOCTLSOCKET,   LIB_BSDSOCKET, BSD_FN_IOCTLSOCKET);
    install_lvo(BSD_BASE, LVO_BSD_INET_ADDR,     LIB_BSDSOCKET, BSD_FN_INET_ADDR);
    install_lvo(BSD_BASE, LVO_BSD_INET_NTOA,     LIB_BSDSOCKET, BSD_FN_INET_NTOA);
    install_lvo(BSD_BASE, LVO_BSD_GETHOSTBYNAME, LIB_BSDSOCKET, BSD_FN_GETHOSTBYNAME);

    /* graphics.library at GRAPHICS_BASE — full AmigaOS LVO range -30 .. -1056.
     * Pre-fill every slot with a safe MOVEQ #0,D0 + RTS, then install an
     * ILLEGAL dispatch stub for every slot so unimplemented calls fall back to
     * graphics_Unimplemented() in graphics_lib.c.
     *
     * LVO slot = |LVO| / 6.  Valid slots are 5..176 (LVO -30..-1056). */
    for (int lvo = -30; lvo >= -1056; lvo -= 6) {
        uint32_t addr = (uint32_t)((int)GRAPHICS_BASE + lvo);
        if (addr >= GUEST_RAM_SIZE - 4) continue;
        g_ram[addr]   = 0x70; g_ram[addr+1] = 0x00; /* MOVEQ #0,D0 */
        g_ram[addr+2] = 0x4E; g_ram[addr+3] = 0x75; /* RTS */
    }
    for (int lvo = -30; lvo >= -1056; lvo -= 6) {
        int slot = (-lvo) / 6;
        install_lvo(GRAPHICS_BASE, lvo, LIB_GRAPHICS, slot);
    }

    /* intuition.library at INTUITION_BASE */
    install_lvo(INTUITION_BASE, LVO_INTUITION_OPEN_LIBRARY                              , LIB_INTUITION, INTUITION_OPEN_LIBRARY);
    install_lvo(INTUITION_BASE, LVO_INTUITION_CLOSE_LIBRARY                             , LIB_INTUITION, INTUITION_CLOSE_LIBRARY);
    install_lvo(INTUITION_BASE, LVO_INTUITION_OPEN_WINDOW                               , LIB_INTUITION, INTUITION_OPEN_WINDOW);
    install_lvo(INTUITION_BASE, LVO_INTUITION_CLOSE_WINDOW                              , LIB_INTUITION, INTUITION_CLOSE_WINDOW);
    install_lvo(INTUITION_BASE, LVO_INTUITION_WINDOW_TO_FRONT                           , LIB_INTUITION, INTUITION_WINDOW_TO_FRONT);
    install_lvo(INTUITION_BASE, LVO_INTUITION_WINDOW_TO_BACK                            , LIB_INTUITION, INTUITION_WINDOW_TO_BACK);
    install_lvo(INTUITION_BASE, LVO_INTUITION_ACTIVATE_WINDOW                           , LIB_INTUITION, INTUITION_ACTIVATE_WINDOW);
    install_lvo(INTUITION_BASE, LVO_INTUITION_MOVE_WINDOW                               , LIB_INTUITION, INTUITION_MOVE_WINDOW);
    install_lvo(INTUITION_BASE, LVO_INTUITION_SIZE_WINDOW                               , LIB_INTUITION, INTUITION_SIZE_WINDOW);
    install_lvo(INTUITION_BASE, LVO_INTUITION_REFRESH_WINDOW                            , LIB_INTUITION, INTUITION_REFRESH_WINDOW);
    install_lvo(INTUITION_BASE, LVO_INTUITION_MODIFY_IDCMP                              , LIB_INTUITION, INTUITION_MODIFY_IDCMP);
    install_lvo(INTUITION_BASE, LVO_INTUITION_SET_WINDOW_TITLES                         , LIB_INTUITION, INTUITION_SET_WINDOW_TITLES);
    install_lvo(INTUITION_BASE, LVO_INTUITION_OPEN_WINDOW_TAGS                          , LIB_INTUITION, INTUITION_OPEN_WINDOW_TAGS);
    install_lvo(INTUITION_BASE, LVO_INTUITION_OPEN_WORKBENCH                            , LIB_INTUITION, INTUITION_OPEN_WORKBENCH);
    install_lvo(INTUITION_BASE, LVO_INTUITION_CLOSE_WORKBENCH                           , LIB_INTUITION, INTUITION_CLOSE_WORKBENCH);
    install_lvo(INTUITION_BASE, LVO_INTUITION_DRAW_BORDER                               , LIB_INTUITION, INTUITION_DRAW_BORDER);
    install_lvo(INTUITION_BASE, LVO_INTUITION_DRAW_IMAGE                                , LIB_INTUITION, INTUITION_DRAW_IMAGE);
    install_lvo(INTUITION_BASE, LVO_INTUITION_PRINT_I_TEXT                              , LIB_INTUITION, INTUITION_PRINT_I_TEXT);
    install_lvo(INTUITION_BASE, LVO_INTUITION_AUTO_REQUEST                              , LIB_INTUITION, INTUITION_AUTO_REQUEST);
    install_lvo(INTUITION_BASE, LVO_INTUITION_BUILD_SYS_REQUEST                         , LIB_INTUITION, INTUITION_BUILD_SYS_REQUEST);
    install_lvo(INTUITION_BASE, LVO_INTUITION_FREE_SYS_REQUEST                          , LIB_INTUITION, INTUITION_FREE_SYS_REQUEST);
    install_lvo(INTUITION_BASE, LVO_INTUITION_EASY_REQUEST                              , LIB_INTUITION, INTUITION_EASY_REQUEST);
    install_lvo(INTUITION_BASE, LVO_INTUITION_OPEN_SCREEN                               , LIB_INTUITION, INTUITION_OPEN_SCREEN);
    install_lvo(INTUITION_BASE, LVO_INTUITION_CLOSE_SCREEN                              , LIB_INTUITION, INTUITION_CLOSE_SCREEN);
    install_lvo(INTUITION_BASE, LVO_INTUITION_MOVE_SCREEN                               , LIB_INTUITION, INTUITION_MOVE_SCREEN);
    install_lvo(INTUITION_BASE, LVO_INTUITION_SCREEN_TO_FRONT                           , LIB_INTUITION, INTUITION_SCREEN_TO_FRONT);
    install_lvo(INTUITION_BASE, LVO_INTUITION_SCREEN_TO_BACK                            , LIB_INTUITION, INTUITION_SCREEN_TO_BACK);
    install_lvo(INTUITION_BASE, LVO_INTUITION_SHOW_TITLE                                , LIB_INTUITION, INTUITION_SHOW_TITLE);
    install_lvo(INTUITION_BASE, LVO_INTUITION_OPEN_SCREEN_TAGS                          , LIB_INTUITION, INTUITION_OPEN_SCREEN_TAGS);
    install_lvo(INTUITION_BASE, LVO_INTUITION_SET_MENU_STRIP                            , LIB_INTUITION, INTUITION_SET_MENU_STRIP);
    install_lvo(INTUITION_BASE, LVO_INTUITION_CLEAR_MENU_STRIP                          , LIB_INTUITION, INTUITION_CLEAR_MENU_STRIP);
    install_lvo(INTUITION_BASE, LVO_INTUITION_RESET_MENU_STRIP                          , LIB_INTUITION, INTUITION_RESET_MENU_STRIP);
    install_lvo(INTUITION_BASE, LVO_INTUITION_ITEM_ADDRESS                              , LIB_INTUITION, INTUITION_ITEM_ADDRESS);
    install_lvo(INTUITION_BASE, LVO_INTUITION_LOCK_PUB_SCREEN                           , LIB_INTUITION, INTUITION_LOCK_PUB_SCREEN);
    install_lvo(INTUITION_BASE, LVO_INTUITION_UNLOCK_PUB_SCREEN                         , LIB_INTUITION, INTUITION_UNLOCK_PUB_SCREEN);
    install_lvo(INTUITION_BASE, LVO_INTUITION_LOCK_PUB_SCREEN_LIST                      , LIB_INTUITION, INTUITION_LOCK_PUB_SCREEN_LIST);
    install_lvo(INTUITION_BASE, LVO_INTUITION_UNLOCK_PUB_SCREEN_LIST                    , LIB_INTUITION, INTUITION_UNLOCK_PUB_SCREEN_LIST);
    install_lvo(INTUITION_BASE, LVO_INTUITION_SET_POINTER                               , LIB_INTUITION, INTUITION_SET_POINTER);
    install_lvo(INTUITION_BASE, LVO_INTUITION_CLEAR_POINTER                             , LIB_INTUITION, INTUITION_CLEAR_POINTER);
    install_lvo(INTUITION_BASE, LVO_INTUITION_SET_WINDOW_POINTER_A                      , LIB_INTUITION, INTUITION_SET_WINDOW_POINTER_A);
    install_lvo(INTUITION_BASE, LVO_INTUITION_GET_DEF_PREFS                             , LIB_INTUITION, INTUITION_GET_DEF_PREFS);
    install_lvo(INTUITION_BASE, LVO_INTUITION_GET_PREFS                                 , LIB_INTUITION, INTUITION_GET_PREFS);
    install_lvo(INTUITION_BASE, LVO_INTUITION_SET_PREFS                                 , LIB_INTUITION, INTUITION_SET_PREFS);
    install_lvo(INTUITION_BASE, LVO_INTUITION_LOCK_GUI_PREFS                            , LIB_INTUITION, INTUITION_LOCK_GUI_PREFS);
    install_lvo(INTUITION_BASE, LVO_INTUITION_UNLOCK_GUI_PREFS                          , LIB_INTUITION, INTUITION_UNLOCK_GUI_PREFS);
    install_lvo(INTUITION_BASE, LVO_INTUITION_QUERY_OVERSCAN                            , LIB_INTUITION, INTUITION_QUERY_OVERSCAN);
    install_lvo(INTUITION_BASE, LVO_INTUITION_CURRENT_TIME                              , LIB_INTUITION, INTUITION_CURRENT_TIME);
    install_lvo(INTUITION_BASE, LVO_INTUITION_DOUBLE_CLICK                              , LIB_INTUITION, INTUITION_DOUBLE_CLICK);
    install_lvo(INTUITION_BASE, LVO_INTUITION_REPORT_MOUSE                              , LIB_INTUITION, INTUITION_REPORT_MOUSE);
    install_lvo(INTUITION_BASE, LVO_INTUITION_DISPLAY_BEEP                              , LIB_INTUITION, INTUITION_DISPLAY_BEEP);
    install_lvo(INTUITION_BASE, LVO_INTUITION_INIT_REQUESTER                            , LIB_INTUITION, INTUITION_INIT_REQUESTER);
    install_lvo(INTUITION_BASE, LVO_INTUITION_END_REQUEST                               , LIB_INTUITION, INTUITION_END_REQUEST);
    install_lvo(INTUITION_BASE, LVO_INTUITION_REQUEST                                   , LIB_INTUITION, INTUITION_REQUEST);
    install_lvo(INTUITION_BASE, LVO_INTUITION_VIEW_ADDRESS                              , LIB_INTUITION, INTUITION_VIEW_ADDRESS);
    install_lvo(INTUITION_BASE, LVO_INTUITION_VIEW_PORT_ADDRESS                         , LIB_INTUITION, INTUITION_VIEW_PORT_ADDRESS);
    install_lvo(INTUITION_BASE, LVO_INTUITION_GET_SCREEN_DATA                           , LIB_INTUITION, INTUITION_GET_SCREEN_DATA);
    install_lvo(INTUITION_BASE, LVO_INTUITION_NEXT_PUB_SCREEN                           , LIB_INTUITION, INTUITION_NEXT_PUB_SCREEN);
    install_lvo(INTUITION_BASE, LVO_INTUITION_SET_DEFAULT_PUB_SCREEN                    , LIB_INTUITION, INTUITION_SET_DEFAULT_PUB_SCREEN);
    install_lvo(INTUITION_BASE, LVO_INTUITION_LOCK_IBASE                                , LIB_INTUITION, INTUITION_LOCK_IBASE);
    install_lvo(INTUITION_BASE, LVO_INTUITION_UNLOCK_IBASE                              , LIB_INTUITION, INTUITION_UNLOCK_IBASE);
    install_lvo(INTUITION_BASE, LVO_INTUITION_SHOW_WINDOW                               , LIB_INTUITION, INTUITION_SHOW_WINDOW);
    install_lvo(INTUITION_BASE, LVO_INTUITION_HIDE_WINDOW                               , LIB_INTUITION, INTUITION_HIDE_WINDOW);
    install_lvo(INTUITION_BASE, LVO_INTUITION_WINDOW_LIMITS                             , LIB_INTUITION, INTUITION_WINDOW_LIMITS);
    install_lvo(INTUITION_BASE, LVO_INTUITION_CHANGE_WINDOW_BOX                         , LIB_INTUITION, INTUITION_CHANGE_WINDOW_BOX);
    install_lvo(INTUITION_BASE, LVO_INTUITION_GET_SCREEN_DRAW_INFO                      , LIB_INTUITION, INTUITION_GET_SCREEN_DRAW_INFO);
    install_lvo(INTUITION_BASE, LVO_INTUITION_FREE_SCREEN_DRAW_INFO                     , LIB_INTUITION, INTUITION_FREE_SCREEN_DRAW_INFO);
    install_lvo(INTUITION_BASE, LVO_INTUITION_DISPLAY_ALERT                             , LIB_INTUITION, INTUITION_DISPLAY_ALERT);
    install_lvo(INTUITION_BASE, LVO_INTUITION_TIMED_DISPLAY_ALERT                       , LIB_INTUITION, INTUITION_TIMED_DISPLAY_ALERT);
    install_lvo(INTUITION_BASE, LVO_INTUITION_SCREEN_DEPTH                              , LIB_INTUITION, INTUITION_SCREEN_DEPTH);
    install_lvo(INTUITION_BASE, LVO_INTUITION_SCREEN_POSITION                           , LIB_INTUITION, INTUITION_SCREEN_POSITION);
    install_lvo(INTUITION_BASE, LVO_INTUITION_ADD_GADGET                                , LIB_INTUITION, INTUITION_ADD_GADGET);
    install_lvo(INTUITION_BASE, LVO_INTUITION_ADD_GLIST                                 , LIB_INTUITION, INTUITION_ADD_GLIST);
    install_lvo(INTUITION_BASE, LVO_INTUITION_REMOVE_GADGET                             , LIB_INTUITION, INTUITION_REMOVE_GADGET);
    install_lvo(INTUITION_BASE, LVO_INTUITION_REMOVE_GLIST                              , LIB_INTUITION, INTUITION_REMOVE_GLIST);
    install_lvo(INTUITION_BASE, LVO_INTUITION_REFRESH_GLIST                             , LIB_INTUITION, INTUITION_REFRESH_GLIST);
    install_lvo(INTUITION_BASE, LVO_INTUITION_ON_GADGET                                 , LIB_INTUITION, INTUITION_ON_GADGET);
    install_lvo(INTUITION_BASE, LVO_INTUITION_OFF_GADGET                                , LIB_INTUITION, INTUITION_OFF_GADGET);
    install_lvo(INTUITION_BASE, LVO_INTUITION_MODIFY_PROP                               , LIB_INTUITION, INTUITION_MODIFY_PROP);
    install_lvo(INTUITION_BASE, LVO_INTUITION_NEW_MODIFY_PROP                           , LIB_INTUITION, INTUITION_NEW_MODIFY_PROP);
    install_lvo(INTUITION_BASE, LVO_INTUITION_ACTIVATE_GADGET                           , LIB_INTUITION, INTUITION_ACTIVATE_GADGET);
    install_lvo(INTUITION_BASE, LVO_INTUITION_SET_WINDOW_ATTRS                          , LIB_INTUITION, INTUITION_SET_WINDOW_ATTRS);
    install_lvo(INTUITION_BASE, LVO_INTUITION_GET_WINDOW_ATTRS                          , LIB_INTUITION, INTUITION_GET_WINDOW_ATTRS);
    install_lvo(INTUITION_BASE, LVO_INTUITION_SET_SCREEN_ATTRS                          , LIB_INTUITION, INTUITION_SET_SCREEN_ATTRS);
    install_lvo(INTUITION_BASE, LVO_INTUITION_GET_SCREEN_ATTRS                          , LIB_INTUITION, INTUITION_GET_SCREEN_ATTRS);
    install_lvo(INTUITION_BASE, LVO_INTUITION_BEGIN_REFRESH                             , LIB_INTUITION, INTUITION_BEGIN_REFRESH);
    install_lvo(INTUITION_BASE, LVO_INTUITION_END_REFRESH                               , LIB_INTUITION, INTUITION_END_REFRESH);
    install_lvo(INTUITION_BASE, LVO_INTUITION_REFRESH_GADGETS                           , LIB_INTUITION, INTUITION_REFRESH_GADGETS);
    install_lvo(INTUITION_BASE, LVO_INTUITION_ON_MENU                                   , LIB_INTUITION, INTUITION_ON_MENU);
    install_lvo(INTUITION_BASE, LVO_INTUITION_OFF_MENU                                  , LIB_INTUITION, INTUITION_OFF_MENU);
    install_lvo(INTUITION_BASE, LVO_INTUITION_SYS_REQ_HANDLER                           , LIB_INTUITION, INTUITION_SYS_REQ_HANDLER);
    install_lvo(INTUITION_BASE, LVO_INTUITION_PUB_SCREEN_STATUS                         , LIB_INTUITION, INTUITION_PUB_SCREEN_STATUS);
    install_lvo(INTUITION_BASE, LVO_INTUITION_GET_DEFAULT_PUB_SCREEN                    , LIB_INTUITION, INTUITION_GET_DEFAULT_PUB_SCREEN);
    install_lvo(INTUITION_BASE, LVO_INTUITION_MOVE_WINDOW_IN_FRONT_OF                   , LIB_INTUITION, INTUITION_MOVE_WINDOW_IN_FRONT_OF);
    install_lvo(INTUITION_BASE, LVO_INTUITION_SET_EDIT_HOOK                             , LIB_INTUITION, INTUITION_SET_EDIT_HOOK);
    install_lvo(INTUITION_BASE, LVO_INTUITION_OBTAIN_GIR_PORT                           , LIB_INTUITION, INTUITION_OBTAIN_GIR_PORT);
    install_lvo(INTUITION_BASE, LVO_INTUITION_RELEASE_GIR_PORT                          , LIB_INTUITION, INTUITION_RELEASE_GIR_PORT);
    install_lvo(INTUITION_BASE, LVO_INTUITION_STRIP_INTUI_MESSAGES                      , LIB_INTUITION, INTUITION_STRIP_INTUI_MESSAGES);
    install_lvo(INTUITION_BASE, LVO_INTUITION_NEW_OBJECT_A                              , LIB_INTUITION, INTUITION_NEW_OBJECT_A);
    install_lvo(INTUITION_BASE, LVO_INTUITION_DISPOSE_OBJECT                            , LIB_INTUITION, INTUITION_DISPOSE_OBJECT);
    install_lvo(INTUITION_BASE, LVO_INTUITION_SET_ATTRS_A                               , LIB_INTUITION, INTUITION_SET_ATTRS_A);
    install_lvo(INTUITION_BASE, LVO_INTUITION_GET_ATTR                                  , LIB_INTUITION, INTUITION_GET_ATTR);
    install_lvo(INTUITION_BASE, LVO_INTUITION_MAKE_CLASS                                , LIB_INTUITION, INTUITION_MAKE_CLASS);
    install_lvo(INTUITION_BASE, LVO_INTUITION_FREE_CLASS                                , LIB_INTUITION, INTUITION_FREE_CLASS);
    install_lvo(INTUITION_BASE, LVO_INTUITION_ADD_CLASS                                 , LIB_INTUITION, INTUITION_ADD_CLASS);
    install_lvo(INTUITION_BASE, LVO_INTUITION_REMOVE_CLASS                              , LIB_INTUITION, INTUITION_REMOVE_CLASS);
    install_lvo(INTUITION_BASE, LVO_INTUITION_NEXT_OBJECT                               , LIB_INTUITION, INTUITION_NEXT_OBJECT);
    install_lvo(INTUITION_BASE, LVO_INTUITION_GET_ATTRS_A                               , LIB_INTUITION, INTUITION_GET_ATTRS_A);
    install_lvo(INTUITION_BASE, LVO_INTUITION_DO_GADGET_METHOD_A                        , LIB_INTUITION, INTUITION_DO_GADGET_METHOD_A);
    install_lvo(INTUITION_BASE, LVO_INTUITION_HELP_CONTROL                              , LIB_INTUITION, INTUITION_HELP_CONTROL);
    install_lvo(INTUITION_BASE, LVO_INTUITION_START_SCREEN_NOTIFY                       , LIB_INTUITION, INTUITION_START_SCREEN_NOTIFY);
    install_lvo(INTUITION_BASE, LVO_INTUITION_END_SCREEN_NOTIFY                         , LIB_INTUITION, INTUITION_END_SCREEN_NOTIFY);
    install_lvo(INTUITION_BASE, LVO_INTUITION_GET_WINDOW_ATTR                           , LIB_INTUITION, INTUITION_GET_WINDOW_ATTR);
    install_lvo(INTUITION_BASE, LVO_INTUITION_SET_WINDOW_ATTR                           , LIB_INTUITION, INTUITION_SET_WINDOW_ATTR);
    install_lvo(INTUITION_BASE, LVO_INTUITION_GET_SCREEN_ATTR                           , LIB_INTUITION, INTUITION_GET_SCREEN_ATTR);
    install_lvo(INTUITION_BASE, LVO_INTUITION_SET_SCREEN_ATTR                           , LIB_INTUITION, INTUITION_SET_SCREEN_ATTR);
    install_lvo(INTUITION_BASE, LVO_INTUITION_SET_GADGET_ATTRS_A                        , LIB_INTUITION, INTUITION_SET_GADGET_ATTRS_A);
    install_lvo(INTUITION_BASE, LVO_INTUITION_ALLOC_SCREEN_BUFFER                       , LIB_INTUITION, INTUITION_ALLOC_SCREEN_BUFFER);
    install_lvo(INTUITION_BASE, LVO_INTUITION_FREE_SCREEN_BUFFER                        , LIB_INTUITION, INTUITION_FREE_SCREEN_BUFFER);
    install_lvo(INTUITION_BASE, LVO_INTUITION_CHANGE_SCREEN_BUFFER                      , LIB_INTUITION, INTUITION_CHANGE_SCREEN_BUFFER);
    install_lvo(INTUITION_BASE, LVO_INTUITION_WBENCH_TO_BACK                            , LIB_INTUITION, INTUITION_WBENCH_TO_BACK);
    install_lvo(INTUITION_BASE, LVO_INTUITION_WBENCH_TO_FRONT                           , LIB_INTUITION, INTUITION_WBENCH_TO_FRONT);
    install_lvo(INTUITION_BASE, LVO_INTUITION_MAKE_SCREEN                               , LIB_INTUITION, INTUITION_MAKE_SCREEN);
    install_lvo(INTUITION_BASE, LVO_INTUITION_REMAKE_DISPLAY                            , LIB_INTUITION, INTUITION_REMAKE_DISPLAY);
    install_lvo(INTUITION_BASE, LVO_INTUITION_RETHINK_DISPLAY                           , LIB_INTUITION, INTUITION_RETHINK_DISPLAY);
    install_lvo(INTUITION_BASE, LVO_INTUITION_CLEAR_DMREQUEST                           , LIB_INTUITION, INTUITION_CLEAR_DMREQUEST);
    install_lvo(INTUITION_BASE, LVO_INTUITION_SET_DMREQUEST                             , LIB_INTUITION, INTUITION_SET_DMREQUEST);
    install_lvo(INTUITION_BASE, LVO_INTUITION_SET_MOUSE_QUEUE                           , LIB_INTUITION, INTUITION_SET_MOUSE_QUEUE);
    install_lvo(INTUITION_BASE, LVO_INTUITION_SET_PUB_SCREEN_MODES                      , LIB_INTUITION, INTUITION_SET_PUB_SCREEN_MODES);
    install_lvo(INTUITION_BASE, LVO_INTUITION_LEND_MENUS                                , LIB_INTUITION, INTUITION_LEND_MENUS);
    install_lvo(INTUITION_BASE, LVO_INTUITION_GADGET_MOUSE                              , LIB_INTUITION, INTUITION_GADGET_MOUSE);
    install_lvo(INTUITION_BASE, LVO_INTUITION_INTUITEXT_LENGTH                          , LIB_INTUITION, INTUITION_INTUITEXT_LENGTH);
    install_lvo(INTUITION_BASE, LVO_INTUITION_POINT_IN_IMAGE                            , LIB_INTUITION, INTUITION_POINT_IN_IMAGE);
    install_lvo(INTUITION_BASE, LVO_INTUITION_ERASE_IMAGE                               , LIB_INTUITION, INTUITION_ERASE_IMAGE);
    install_lvo(INTUITION_BASE, LVO_INTUITION_ZIP_WINDOW                                , LIB_INTUITION, INTUITION_ZIP_WINDOW);
    install_lvo(INTUITION_BASE, LVO_INTUITION_REFRESH_SET_GADGET_ATTRS_A                , LIB_INTUITION, INTUITION_REFRESH_SET_GADGET_ATTRS_A);
    install_lvo(INTUITION_BASE, LVO_INTUITION_SCROLL_WINDOW_RASTER                      , LIB_INTUITION, INTUITION_SCROLL_WINDOW_RASTER);
    install_lvo(INTUITION_BASE, LVO_INTUITION_BUILD_EASY_REQUEST_ARGS                   , LIB_INTUITION, INTUITION_BUILD_EASY_REQUEST_ARGS);
    install_lvo(INTUITION_BASE, LVO_INTUITION_DRAW_IMAGE_STATE                          , LIB_INTUITION, INTUITION_DRAW_IMAGE_STATE);
    install_lvo(INTUITION_BASE, LVO_INTUITION_ALLOC_REMEMBER                            , LIB_INTUITION, INTUITION_ALLOC_REMEMBER);
    install_lvo(INTUITION_BASE, LVO_INTUITION_FREE_REMEMBER                             , LIB_INTUITION, INTUITION_FREE_REMEMBER);
    install_lvo(INTUITION_BASE, LVO_INTUITION_NEW_IMAGE_A                               , LIB_INTUITION, INTUITION_NEW_IMAGE_A);
    install_lvo(INTUITION_BASE, LVO_INTUITION_DISPOSE_IMAGE                             , LIB_INTUITION, INTUITION_DISPOSE_IMAGE);
    install_lvo(INTUITION_BASE, LVO_INTUITION_SET_IPREFS                                , LIB_INTUITION, INTUITION_SET_IPREFS);
    install_lvo(INTUITION_BASE, LVO_INTUITION_GET_HALF_PENS                             , LIB_INTUITION, INTUITION_GET_HALF_PENS);
    install_lvo(INTUITION_BASE, LVO_INTUITION_GADGET_BOX                                , LIB_INTUITION, INTUITION_GADGET_BOX);
    install_lvo(INTUITION_BASE, LVO_INTUITION_SET_GUI_ATTRS_A                           , LIB_INTUITION, INTUITION_SET_GUI_ATTRS_A);
    install_lvo(INTUITION_BASE, LVO_INTUITION_GET_GUI_ATTRS_A                           , LIB_INTUITION, INTUITION_GET_GUI_ATTRS_A);
    install_lvo(INTUITION_BASE, LVO_INTUITION_OPEN_CLASS                                , LIB_INTUITION, INTUITION_OPEN_CLASS);
    install_lvo(INTUITION_BASE, LVO_INTUITION_CLOSE_CLASS                               , LIB_INTUITION, INTUITION_CLOSE_CLASS);
    install_lvo(INTUITION_BASE, LVO_INTUITION_IDO_METHOD_A                              , LIB_INTUITION, INTUITION_IDO_METHOD_A);
    install_lvo(INTUITION_BASE, LVO_INTUITION_IDO_SUPER_METHOD_A                        , LIB_INTUITION, INTUITION_IDO_SUPER_METHOD_A);
    install_lvo(INTUITION_BASE, LVO_INTUITION_ICOERCE_METHOD_A                          , LIB_INTUITION, INTUITION_ICOERCE_METHOD_A);
    install_lvo(INTUITION_BASE, LVO_INTUITION_ISET_SUPER_ATTRS_A                        , LIB_INTUITION, INTUITION_ISET_SUPER_ATTRS_A);
    install_lvo(INTUITION_BASE, LVO_INTUITION_LOCK_SCREEN                               , LIB_INTUITION, INTUITION_LOCK_SCREEN);
    install_lvo(INTUITION_BASE, LVO_INTUITION_UNLOCK_SCREEN                             , LIB_INTUITION, INTUITION_UNLOCK_SCREEN);
    install_lvo(INTUITION_BASE, LVO_INTUITION_LOCK_SCREEN_LIST                          , LIB_INTUITION, INTUITION_LOCK_SCREEN_LIST);
    install_lvo(INTUITION_BASE, LVO_INTUITION_UNLOCK_SCREEN_LIST                        , LIB_INTUITION, INTUITION_UNLOCK_SCREEN_LIST);
    install_lvo(INTUITION_BASE, LVO_INTUITION_LOCK_SCREEN_GI                            , LIB_INTUITION, INTUITION_LOCK_SCREEN_GI);
    install_lvo(INTUITION_BASE, LVO_INTUITION_UNLOCK_SCREEN_GI                          , LIB_INTUITION, INTUITION_UNLOCK_SCREEN_GI);
    /* gadtools.library at GADTOOLS_BASE */
    install_lvo(GADTOOLS_BASE, LVO_GADTOOLS_CREATE_GADGET_A,       LIB_GADTOOLS, GADTOOLS_CREATE_GADGET_A);
    install_lvo(GADTOOLS_BASE, LVO_GADTOOLS_FREE_GADGETS,            LIB_GADTOOLS, GADTOOLS_FREE_GADGETS);
    install_lvo(GADTOOLS_BASE, LVO_GADTOOLS_GT_SET_GADGET_ATTRS_A,   LIB_GADTOOLS, GADTOOLS_GT_SET_GADGET_ATTRS_A);
    install_lvo(GADTOOLS_BASE, LVO_GADTOOLS_CREATE_MENUS_A,          LIB_GADTOOLS, GADTOOLS_CREATE_MENUS_A);
    install_lvo(GADTOOLS_BASE, LVO_GADTOOLS_FREE_MENUS,              LIB_GADTOOLS, GADTOOLS_FREE_MENUS);
    install_lvo(GADTOOLS_BASE, LVO_GADTOOLS_LAYOUT_MENU_ITEMS_A,     LIB_GADTOOLS, GADTOOLS_LAYOUT_MENU_ITEMS_A);
    install_lvo(GADTOOLS_BASE, LVO_GADTOOLS_LAYOUT_MENUS_A,         LIB_GADTOOLS, GADTOOLS_LAYOUT_MENUS_A);
    install_lvo(GADTOOLS_BASE, LVO_GADTOOLS_GT_GET_IMSG,             LIB_GADTOOLS, GADTOOLS_GT_GET_IMSG);
    install_lvo(GADTOOLS_BASE, LVO_GADTOOLS_GT_REPLY_IMSG,           LIB_GADTOOLS, GADTOOLS_GT_REPLY_IMSG);
    install_lvo(GADTOOLS_BASE, LVO_GADTOOLS_GT_REFRESH_WINDOW,       LIB_GADTOOLS, GADTOOLS_GT_REFRESH_WINDOW);
    install_lvo(GADTOOLS_BASE, LVO_GADTOOLS_GT_BEGIN_REFRESH,          LIB_GADTOOLS, GADTOOLS_GT_BEGIN_REFRESH);
    install_lvo(GADTOOLS_BASE, LVO_GADTOOLS_GT_END_REFRESH,            LIB_GADTOOLS, GADTOOLS_GT_END_REFRESH);
    install_lvo(GADTOOLS_BASE, LVO_GADTOOLS_GT_FILTER_IMSG,            LIB_GADTOOLS, GADTOOLS_GT_FILTER_IMSG);
    install_lvo(GADTOOLS_BASE, LVO_GADTOOLS_GT_POST_FILTER_IMSG,     LIB_GADTOOLS, GADTOOLS_GT_POST_FILTER_IMSG);
    install_lvo(GADTOOLS_BASE, LVO_GADTOOLS_CREATE_CONTEXT,          LIB_GADTOOLS, GADTOOLS_CREATE_CONTEXT);
    install_lvo(GADTOOLS_BASE, LVO_GADTOOLS_DRAW_BEVEL_BOX_A,        LIB_GADTOOLS, GADTOOLS_DRAW_BEVEL_BOX_A);
    install_lvo(GADTOOLS_BASE, LVO_GADTOOLS_GET_VISUAL_INFO_A,       LIB_GADTOOLS, GADTOOLS_GET_VISUAL_INFO_A);
    install_lvo(GADTOOLS_BASE, LVO_GADTOOLS_FREE_VISUAL_INFO,        LIB_GADTOOLS, GADTOOLS_FREE_VISUAL_INFO);
    install_lvo(GADTOOLS_BASE, LVO_GADTOOLS_GT_GET_GADGET_ATTRS_A,   LIB_GADTOOLS, GADTOOLS_GT_GET_GADGET_ATTRS_A);

    /* audio.device — real jump table so BeginIO/AbortIO dispatch to the
     * channel-arbitration implementation (UAOS-240). */
    install_lvo(AUDIO_DEV_BASE, -6,  LIB_AUDIODEV, AUDEV_LVO_OPEN);
    install_lvo(AUDIO_DEV_BASE, -12, LIB_AUDIODEV, AUDEV_LVO_CLOSE);
    install_lvo(AUDIO_DEV_BASE, -42, LIB_AUDIODEV, AUDEV_LVO_BEGINIO);
    install_lvo(AUDIO_DEV_BASE, -48, LIB_AUDIODEV, AUDEV_LVO_ABORTIO);

    /* Fill FAKE_LIB_BASE area with RTS so any JSR into unknown lib returns cleanly.
     * Each LVO slot is 6 bytes: ILLEGAL(2) + dispatch(2) + RTS(2).
     * For FAKE_LIB_BASE we just put RTS everywhere (D0=0 is the default return). */
    for (int lvo = -6; lvo >= -756; lvo -= 6) {
        uint32_t addr = (uint32_t)((int)FAKE_LIB_BASE + lvo);
        if (addr < GUEST_RAM_SIZE - 2) {
            g_ram[addr]   = 0x70; g_ram[addr+1] = 0x00; /* MOVEQ #0,D0 */
            g_ram[addr+2] = 0x4E; g_ram[addr+3] = 0x75; /* RTS */
        }
    }

    /* Install real M68k binary libraries loaded from disk */
    install_loadable_libs();

    /* The Process/CLI/RDArgs structs are allocated AFTER hunk_load by
     * UAOS_Emu_SetupProcess() — fixed addresses here would sit inside the
     * program image and get stomped for anything > ~60 KB. */

    /* Store exec_base at absolute address 4 (SysBase) */
    g_ram[4] = (EXEC_BASE >> 24) & 0xFF;
    g_ram[5] = (EXEC_BASE >> 16) & 0xFF;
    g_ram[6] = (EXEC_BASE >>  8) & 0xFF;
    g_ram[7] = (EXEC_BASE      ) & 0xFF;

    /* Also store at 0 (reset SSP will be patched separately) */

    libmap_selfcheck();
}

/* =========================================================================
 * Loadable library registry — real M68k binaries loaded from disk
 * ========================================================================= */

#define MAX_GLUE_LOADABLE_LIBS  16
#define MAX_GLUE_LIB_NAME       64

typedef struct {
    char     name[MAX_GLUE_LIB_NAME];
    uint32_t base_addr;
    uint16_t func_count;
    uint8_t  loaded;
    const uint8_t *binary;
    uint32_t bin_size;
} GlueLoadableLib;

static GlueLoadableLib g_glue_libs[MAX_GLUE_LOADABLE_LIBS];
static int             g_glue_lib_count = 0;
static uint32_t        g_next_loadable_base = 0xA000;

void UAOS_Emu_RegisterLoadableLib(const char *name, const uint8_t *data,
                                  uint32_t size, uint32_t *out_base)
{
    if (!name || !data || !size || !out_base) return;
    if (g_glue_lib_count >= MAX_GLUE_LOADABLE_LIBS) return;
    if (size < 64 ||
        data[0] != 'U' || data[1] != 'A' ||
        data[2] != 'O' || data[3] != 'S' || data[4] != 2) {
        return;
    }

    GlueLoadableLib *e = &g_glue_libs[g_glue_lib_count++];
    int i = 0;
    while (i < MAX_GLUE_LIB_NAME - 1 && name[i]) {
        e->name[i] = name[i]; i++;
    }
    e->name[i] = '\0';
    e->func_count = (uint16_t)(((uint16_t)data[6] << 8) | (uint16_t)data[7]);
    e->loaded = 0;
    e->binary = data;
    e->bin_size = size;
    e->base_addr = g_next_loadable_base;
    g_next_loadable_base += 0x1000;
    *out_base = e->base_addr;
}

/* Install all registered loadable libraries into g_ram */
static void install_loadable_libs(void)
{
    for (int i = 0; i < g_glue_lib_count; i++) {
        GlueLoadableLib *e = &g_glue_libs[i];
        if (e->loaded) continue;
        /* Blobs live at 0xA000+; they must end before the dedicated
         * process-environment region at 0x1B000 (UAOS-237). */
        if (e->base_addr + e->bin_size > 0x0001B000u) continue;

        for (uint32_t j = 0; j < e->bin_size; j++)
            g_ram[e->base_addr + j] = e->binary[j];
        e->loaded = 1;
    }
}

/* =========================================================================
 * exec.library implementation
 * ========================================================================= */

/* g_last_err replaced by global g_dos_last_ioerr via SetIoErr()/IoErr() */

/* -------------------------------------------------------------------------
 * Generated library bases (OpenLibrary for modules without a fixed base)
 *
 * Each bound library gets a guest block:  [960B stub area][64B Library
 * struct][64B name/idstring].  Stubs are ILLEGAL dispatch words tagged
 * LIB_GENERIC (fake: log+return 0) or LIB_UTILITY (real ROM dispatch).
 * ------------------------------------------------------------------------- */
#define GENLIB_STUB_AREA  960          /* covers LVO -6 .. -960 */
#define GENLIB_BLK_SIZE   (GENLIB_STUB_AREA + 0x40 + 0x40)

typedef struct {
    uint32_t base;
    char     name[48];
    uint8_t *ram;       /* owning guest-RAM window — entries are per-task */
    int      utility;   /* bound to utility.library ROM funcs */
    int      cia;       /* 0=CIAA, 1=CIAB for cia*.resource, else -1 */
    uint32_t icr_tab;   /* guest addr of 8-entry ICR Interrupt* table */
} GenLib;

static GenLib g_genlibs[16];
static int    g_genlib_count = 0;

/* Genlib blocks are carved from the 64 KB guard band at the top of chip
 * RAM (0x7F0000-0x800000) — deliberately outside every free-list pool so
 * guest heap activity can never recycle or clobber the LVO stubs.  The
 * fast pool flat-covers 0x800000-0xFF0000 and self-decrunching programs
 * (OctaMED unpacks to 0x800000+) write their image into the same range:
 * pool-allocated lib bases there got stomped and jsr@(-60) ran guest
 * data instead of dispatching.  16 libs × ~1.1 KB fits with room to
 * spare. */
#define GENLIB_ARENA_START 0x007F0000u
#define GENLIB_ARENA_END   0x00800000u
static uint32_t g_genlib_heap = GENLIB_ARENA_START;

/* Per-task guest RAM windows are cleared on launch — callers reset this
 * so stale bases are not handed out into a fresh window.  Entries belong
 * to the window that created them: a relaunch drops only this window's
 * entries, so other live M68k tasks keep their generated libraries (and
 * their cia.resource ICR registrations) intact. */
void emu_reset_genlibs(void)
{
    int w = 0;
    for (int i = 0; i < g_genlib_count; i++)
        if (g_genlibs[i].ram != g_ram) g_genlibs[w++] = g_genlibs[i];
    g_genlib_count = w;
    g_genlib_heap  = GENLIB_ARENA_START;
}

/* Name lookup for LIB_GENERIC dispatch logging */
const char *emu_fake_lib_name(uint32_t base)
{
    for (int i = 0; i < g_genlib_count; i++)
        if (g_genlibs[i].base == base && g_genlibs[i].ram == g_ram)
            return g_genlibs[i].name;
    return NULL;
}

/* Canonical utility.library LVOs → util_funcs[] indices (utility_lib.fd).
 * Index 0 in this table means "install a catch-all stub". */
static const struct { int lvo; uint8_t fn; } g_utility_lvo_map[] = {
    {  -36, 12 },   /* GetTagData   */
    {  -48, 11 },   /* NextTagItem  */
    { -138,  9 },   /* SMult32      */
    { -144, 10 },   /* UMult32      */
    { -162,  5 },   /* Stricmp      */
    { -168,  6 },   /* Strnicmp     */
    { -174,  7 },   /* ToUpper      */
    { -180,  8 },   /* ToLower      */
    { -552,  1 },   /* OpenLibrary  */
    { -414,  2 },   /* CloseLibrary */
};

/* Allocate a guest library base for `name`, fill the Library struct, and
 * install stubs: real LVO->fn map for utility.library, LIB_GENERIC
 * catch-alls elsewhere.  `ntype`/`list_off` publish the node: ln_Type and
 * the ExecBase list offset to link into (LibList 0x17A for libraries,
 * ResourceList 0x150 for resources).  Returns the base or 0. */
static uint32_t emu_gen_lib_base(const char *name, uint32_t req_ver,
                                 uint8_t ntype, uint32_t list_off)
{
    if (g_genlib_count >= 16) return 0;

    /* ciaa.resource / ciab.resource get an extra 8-entry ICR Interrupt
     * table at base+0x80 for AddICRVector/RemICRVector (UAOS-241). */
    int cia = -1;
    if (name[0] == 'c' && name[1] == 'i' && name[2] == 'a' &&
        name[4] == '.' && name[5] == 'r') {
        if (name[3] == 'a') cia = 0;
        else if (name[3] == 'b') cia = 1;
    }
    uint32_t blk_size = GENLIB_BLK_SIZE + ((cia >= 0) ? 0x40u : 0u);

    uint32_t blk = g_genlib_heap;
    if (blk + blk_size > GENLIB_ARENA_END) return 0;
    g_genlib_heap = blk + ((blk_size + 3u) & ~3u);
    for (uint32_t i = 0; i < blk_size; i++) g_ram[blk + i] = 0;

    uint32_t base     = blk + GENLIB_STUB_AREA;
    uint32_t name_ptr = blk + GENLIB_STUB_AREA + 0x40;

    GenLib *e = &g_genlibs[g_genlib_count++];
    e->base = base;
    e->ram  = g_ram;
    e->cia  = cia;
    e->icr_tab = (cia >= 0) ? base + 0x80 : 0;
    int k = 0;
    while (name[k] && k < 47) { e->name[k] = name[k]; g_ram[name_ptr + k] = (uint8_t)name[k]; k++; }
    e->name[k] = '\0';
    g_ram[name_ptr + k] = 0;

    int is_utility = 0;
    const char *un = "utility.library";
    int u = 0;
    while (un[u] && name[u] == un[u]) u++;
    is_utility = (un[u] == 0 && name[u] == 0);
    e->utility = is_utility;

    /* struct Library: Node + flags + sizes + version + idstring + opencnt */
    g_ram[base + 8] = ntype;                    /* ln_Type */
    glue_w32(base + 10, name_ptr);              /* ln_Name */
    glue_w16(base + 16, (uint16_t)GENLIB_STUB_AREA); /* lib_NegSize */
    glue_w16(base + 18, 34);                    /* lib_PosSize */
    uint16_t ver = req_ver ? (uint16_t)req_ver : 39;
    glue_w16(base + 20, ver);                   /* lib_Version */
    glue_w16(base + 22, 0);                     /* lib_Revision */
    glue_w32(base + 24, name_ptr);              /* lib_IdString */
    glue_w16(base + 32, 1);                     /* lib_OpenCnt */

    /* Publish on the matching ExecBase list so FindName(&SysBase->LibList
     * or ->ResourceList) resolves it (UAOS-237). */
    if (list_off)
        guest_list_add_tail(EXEC_BASE + list_off, base);

    uint8_t lib_id = is_utility ? LIB_UTILITY : LIB_GENERIC;
    for (int lvo = -6; lvo >= -(int)GENLIB_STUB_AREA; lvo -= 6)
        install_lvo(base, lvo, lib_id, 0xEE);   /* 0xEE = unmapped */

    if (is_utility) {
        for (unsigned i = 0; i < sizeof(g_utility_lvo_map)/sizeof(g_utility_lvo_map[0]); i++)
            install_lvo(base, g_utility_lvo_map[i].lvo, LIB_UTILITY,
                        g_utility_lvo_map[i].fn);
    }
    return base;
}

static uint32_t emu_gen_find(const char *name)
{
    for (int i = 0; i < g_genlib_count; i++) {
        /* Base addresses repeat across windows — only match an entry
         * materialised in *this* task's guest RAM. */
        if (g_genlibs[i].ram != g_ram) continue;
        const char *n = g_genlibs[i].name;
        int j = 0;
        while (n[j] && name[j] == n[j]) j++;
        if (n[j] == 0 && name[j] == 0) return g_genlibs[i].base;
    }
    return 0;
}

static void exec_OpenLibrary(void)
{
    /* A1 = library name string, D0 = version — returns base in D0 */
    uint32_t name_ptr = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t req_ver  = m68k_get_reg(NULL, M68K_REG_D0);
    char name[64];
    int i = 0;
    while (i < 63 && name_ptr + i < GUEST_RAM_SIZE)
        { name[i] = (char)g_ram[name_ptr+i]; if (!name[i]) break; i++; }
    name[i] = '\0';

    /* Match known libraries — tables of {name, base} pairs */
    static const struct { const char *n; uint32_t b; } fixed[] = {
        { "exec.library",      EXEC_BASE },
        { "dos.library",       DOS_BASE },
        { "bsdsocket.library", BSD_BASE },
        { "graphics.library",  GRAPHICS_BASE },
        { "intuition.library", INTUITION_BASE },
        { "gadtools.library",  GADTOOLS_BASE },
    };
    uint32_t result = 0;
    for (unsigned f = 0; f < sizeof(fixed)/sizeof(fixed[0]); f++) {
        const char *fn_ = fixed[f].n;
        int j = 0;
        while (fn_[j] && name[j] == fn_[j]) j++;
        if (fn_[j] == 0 && name[j] == 0) { result = fixed[f].b; break; }
    }

    /* Check loadable libraries (real M68k binaries loaded from disk) */
    if (!result) {
        for (int li = 0; li < g_glue_lib_count; li++) {
            if (!g_glue_libs[li].loaded) continue;
            const char *ln = g_glue_libs[li].name;
            int lmatch = 1;
            for (int j = 0; ln[j]; j++)
                if (name[j] != ln[j]) { lmatch = 0; break; }
            if (lmatch && name[emu_strlen(ln)] == '\0') {
                result = g_glue_libs[li].base_addr;
                break;
            }
        }
    }

    /* Everything else gets a generated versioned base — real ROM-bound
     * stubs for utility.library, traced catch-alls for the rest
     * (keymap/locale/asl/iffparse/icon/amigaguide/…).  This mirrors the
     * host-harness strategy: enumerate the call surface instead of
     * dying at the first missing library. */
    if (!result) result = emu_gen_find(name);
    if (!result) result = emu_gen_lib_base(name, req_ver, NT_LIBRARY_G, 0x17A);

    /* Trace every OpenLibrary name + requested version + outcome.
     * This is the primary tool for discovering what an m68k app needs. */
    {
        const char *tag = result ? "ok" : "MISSING";
        char msg[128];
        int i = 0;
        const char *pfx = "[emu] OpenLibrary(\"";
        while (pfx[i] && i < 100) { msg[i] = pfx[i]; i++; }
        for (int j = 0; name[j] && i < 110; j++) msg[i++] = name[j];
        const char *mid = "\",v";
        for (int j = 0; mid[j] && i < 116; j++) msg[i++] = mid[j];
        char n[12]; u32_dec(req_ver, n, 12);
        for (int j = 0; n[j] && i < 122; j++) msg[i++] = n[j];
        msg[i++] = ')'; msg[i++] = '-'; msg[i++] = '>';
        msg[i++] = ' ';
        for (int j = 0; tag[j] && i < 120; j++) msg[i++] = tag[j];
        /* base address for correlation with "[lib] x lvo=" logs */
        msg[i++] = ' '; msg[i++] = '@'; msg[i++] = '0'; msg[i++] = 'x';
        u32_hex(result, n);
        for (int j = 0; n[j] && i < 126; j++) msg[i++] = n[j];
        msg[i++] = '\n'; msg[i] = '\0';
        emu_print(msg);
    }

    /* bump lib_OpenCnt for realism */
    if (result && result + 33 < GUEST_RAM_SIZE)
        glue_w16(result + 32, glue_r16(result + 32) + 1);

    m68k_set_reg(M68K_REG_D0, result);

    /* Update the Z flag in SR so caller's beq/bne tests work correctly.
     * The ILLEGAL-instruction callback bypasses normal instruction execution,
     * so flags from before the jsr (e.g. moveq #0,d0 setting Z=1) persist.
     * Z flag is bit 2 (0x0004) of the status register. */
    uint16_t sr = (uint16_t)m68k_get_reg(NULL, M68K_REG_SR);
    if (result == 0) sr |= 0x0004;   /* set Z */
    else             sr &= ~0x0004;  /* clear Z */
    m68k_set_reg(M68K_REG_SR, sr);
}

static void exec_OpenResource(void)
{
    /* OpenResource(resName=a1) → D0=resource base.  Same fake-base scheme:
     * cia.resource/potgo.resource/etc get generated bases so callers see
     * a valid pointer; their LVO calls log via LIB_GENERIC. */
    uint32_t name_ptr = m68k_get_reg(NULL, M68K_REG_A1);
    char name[64];
    int i = 0;
    while (i < 63 && name_ptr + i < GUEST_RAM_SIZE)
        { name[i] = (char)g_ram[name_ptr+i]; if (!name[i]) break; i++; }
    name[i] = '\0';
    uint32_t base = emu_gen_find(name);
    if (!base) base = emu_gen_lib_base(name, 0, 8 /*NT_RESOURCE*/, 0x150);
    m68k_set_reg(M68K_REG_D0, base);
}

static void exec_CloseLibrary(void) { /* no-op */ }

/* -------------------------------------------------------------------------
 * Guest interrupt vectors (UAOS-241)
 *
 * ExecBase.IntVects[16] at +0x54: struct IntVector { iv_Data, iv_Code,
 * iv_Node }.  SetIntVector installs a direct handler; AddIntServer converts
 * the slot to a chain (iv_Code = IV_CHAIN sentinel, iv_Node = head) with
 * Interrupt nodes linked via is_Node.ln_Succ.  Interrupt.is_Data / is_Code
 * sit at +14 / +18 (after the 14-byte Node).
 *
 * Handlers are invoked with the Amiga convention — D0 = pending INTREQ bit,
 * A0 = $DFF000 (custom regs in the guest window), A1 = is_Data,
 * A5 = IntVector address, A6 = SysBase — and return via RTS to the shared
 * hook-return trap.  Delivery is polled from the exec_task slice loop and
 * from the blocking-wait paths (Wait/WaitPort/WaitIO); the Musashi
 * m68k_set_irq() path stays unused for per-task contexts because their
 * exception-vector tables are unpopulated. */
#define EB_INTVECTS   (EXEC_BASE + 0x54)
#define IV_DATA        0
#define IV_CODE        4
#define IV_NODE        8
#define IV_CHAIN       0xFFFFFFFFu
#define IS_DATA       14
#define IS_CODE       18

static uint32_t m68k_isr_call(uint32_t entry, uint32_t d0, uint32_t a1,
                              uint32_t a5);
void UAOS_M68k_DeliverInterrupts(void);
extern uint32_t g_blocked_in;

static void exec_SetIntVector(void)
{
    /* SetIntVector(intNumber=d0, interrupt=a1) -> D0 = old iv_Code */
    uint32_t num  = m68k_get_reg(NULL, M68K_REG_D0) & 15u;
    uint32_t intr = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t iv   = EB_INTVECTS + num * 12u;
    uint32_t old  = guest_read_be32(iv + IV_CODE);
    if (intr) {
        guest_write_be32(iv + IV_DATA, guest_read_be32(intr + IS_DATA));
        guest_write_be32(iv + IV_CODE, guest_read_be32(intr + IS_CODE));
        guest_write_be32(iv + IV_NODE, intr);
    }
    {
        char m[56]; int i = 0;
        const char *t = "[irq] SetIntVector vec="; while (t[i]) { m[i]=t[i]; i++; }
        char n8[12]; u32_dec(num, n8, 12); int j = 0;
        while (n8[j] && i < 40) m[i++] = n8[j++];
        const char *t2 = " code=0x"; j = 0; while (t2[j]) m[i++] = t2[j++];
        u32_hex(guest_read_be32(iv + IV_CODE), n8); j = 0;
        while (n8[j] && i < 52) m[i++] = n8[j++];
        m[i++]='\n'; m[i]='\0'; emu_print(m);
    }
    m68k_set_reg(M68K_REG_D0, old);
}

static void exec_AddIntServer(void)
{
    /* AddIntServer(intNumber=d0, interrupt=a1) — chain of Interrupt nodes */
    uint32_t num  = m68k_get_reg(NULL, M68K_REG_D0) & 15u;
    uint32_t intr = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t iv   = EB_INTVECTS + num * 12u;
    if (!intr) return;
    guest_write_be32(intr, 0);              /* ln_Succ = NULL */
    uint32_t code = guest_read_be32(iv + IV_CODE);
    uint32_t node = guest_read_be32(iv + IV_NODE);
    if (code == 0 && node == 0) {
        guest_write_be32(iv + IV_CODE, IV_CHAIN);
        guest_write_be32(iv + IV_NODE, intr);
        return;
    }
    if (code != IV_CHAIN && node) {
        /* A direct SetIntVector handler occupies the slot — make it the
         * chain head (its ln_Succ may be uninitialized, terminate it). */
        guest_write_be32(node, 0);
        guest_write_be32(iv + IV_CODE, IV_CHAIN);
    } else if (!node) {
        guest_write_be32(iv + IV_NODE, intr);
        return;
    }
    /* Append to chain tail (priority ordering skipped). */
    uint32_t n = guest_read_be32(iv + IV_NODE);
    while (guest_read_be32(n)) n = guest_read_be32(n);
    guest_write_be32(n, intr);
}

static void exec_RemIntServer(void)
{
    /* RemIntServer(intNumber=d0, interrupt=a1) */
    uint32_t num  = m68k_get_reg(NULL, M68K_REG_D0) & 15u;
    uint32_t intr = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t iv   = EB_INTVECTS + num * 12u;
    uint32_t node = guest_read_be32(iv + IV_NODE);
    if (!node || !intr) return;
    if (node == intr) {
        uint32_t next = guest_read_be32(intr);
        guest_write_be32(iv + IV_NODE, next);
        if (!next) guest_write_be32(iv + IV_CODE, 0);
        return;
    }
    while (node) {
        uint32_t next = guest_read_be32(node);
        if (next == intr) { guest_write_be32(node, guest_read_be32(intr)); return; }
        node = next;
    }
}

static void exec_Cause(void)
{
    /* Cause(interrupt=a1) — invoke the software interrupt now. */
    uint32_t intr = m68k_get_reg(NULL, M68K_REG_A1);
    if (!intr) return;
    uint32_t code = guest_read_be32(intr + IS_CODE);
    if (code) m68k_isr_call(code, 0x0004u /*SOFTINT*/,
                            guest_read_be32(intr + IS_DATA), intr);
}

/* cia*.resource dispatch: AddICRVector(-6), RemICRVector(-12),
 * AbleICR(-18), SetICR(-24).  The per-resource ICR table lives at
 * base+0x80 inside the generated lib block (see emu_gen_lib_base). */
static void cia_res_dispatch(uint32_t rbase, int32_t lvo)
{
    GenLib *e = NULL;
    for (int i = 0; i < g_genlib_count; i++)
        if (g_genlibs[i].base == rbase && g_genlibs[i].ram == g_ram)
            e = &g_genlibs[i];
    if (!e || e->cia < 0) { m68k_set_reg(M68K_REG_D0, 0); return; }

    uint32_t d0   = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t intr = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t slot = e->icr_tab + (d0 & 7u) * 4u;
    if (lvo == -6) {
        /* AddICRVector: 0 on success, or the blocking Interrupt.
         * Real cia.resource also enables the ICR bit here. */
        uint32_t old = guest_read_be32(slot);
        if (old) { m68k_set_reg(M68K_REG_D0, old); return; }
        guest_write_be32(slot, intr);
        {
            char m[56]; int i = 0;
            const char *t = "[icr] AddICRVector cia"; while (t[i]) { m[i]=t[i]; i++; }
            m[i++] = (char)('a' + e->cia);
            const char *t2 = " bit="; int j = 0; while (t2[j]) m[i++]=t2[j++];
            char n8[12]; u32_dec(d0 & 7u, n8, 12); j = 0;
            while (n8[j] && i < 44) m[i++] = n8[j++];
            const char *t3 = " code=0x"; j = 0; while (t3[j]) m[i++] = t3[j++];
            u32_hex(guest_read_be32(intr + IS_CODE), n8); j = 0;
            while (n8[j] && i < 52) m[i++] = n8[j++];
            m[i++]='\n'; m[i]='\0'; emu_print(m);
        }
        chip_emu_cia_able_icr(e->cia, (uint8_t)(0x80u | (1u << (d0 & 7u))));
        m68k_set_reg(M68K_REG_D0, 0);
    } else if (lvo == -12) {
        /* RemICRVector: detach handler and disable the ICR bit. */
        if (guest_read_be32(slot) == intr) {
            guest_write_be32(slot, 0);
            chip_emu_cia_able_icr(e->cia, (uint8_t)(1u << (d0 & 7u)));
        }
    } else if (lvo == -18) {
        { char m[48]; int i = 0; const char *t = "[icr] AbleICR cia";
          while (t[i]) { m[i] = t[i]; i++; }
          m[i++] = (char)('a' + e->cia); m[i++] = ' '; m[i++] = '=';
          const char *H = "0123456789abcdef";
          m[i++] = H[(d0 >> 4) & 15]; m[i++] = H[d0 & 15];
          m[i++] = '\n'; m[i] = 0; emu_print(m); }
        m68k_set_reg(M68K_REG_D0,
                     chip_emu_cia_able_icr(e->cia, (uint8_t)d0));
    } else if (lvo == -24) {
        { char m[48]; int i = 0; const char *t = "[icr] SetICR cia";
          while (t[i]) { m[i] = t[i]; i++; }
          m[i++] = (char)('a' + e->cia); m[i++] = ' '; m[i++] = '=';
          const char *H = "0123456789abcdef";
          m[i++] = H[(d0 >> 4) & 15]; m[i++] = H[d0 & 15];
          m[i++] = '\n'; m[i] = 0; emu_print(m); }
        m68k_set_reg(M68K_REG_D0,
                     chip_emu_cia_set_icr(e->cia, (uint8_t)d0));
    } else {
        m68k_set_reg(M68K_REG_D0, 0);
    }
}

/* AllocMem / FreeMem — delegate to the dos_lib free-list allocator
 * (externs declared near the top of this file). */
static void exec_AllocMem(void)
{
    uint32_t size = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t reqs = m68k_get_reg(NULL, M68K_REG_D1);
    uint32_t addr = 0;
    if (size > 0x400000u) {
        /* Implausible alloc — dump caller state so we can see what table
         * entry the program misread. */
        uint32_t a3 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A3);
        char b[160]; int i = 0;
        const char *t = "[allocmem] insane size; a3=0x";
        while (t[i]) { b[i] = t[i]; i++; }
        char n[12]; u32_hex(a3, n); int j = 0;
        while (n[j] && i < 150) b[i++] = n[j++];
        t = " mem="; j = 0; while (t[j]) b[i++] = t[j++];
        for (int k = -8; k < 24 && i < 148; k += 4) {
            u32_hex(guest_read_be32(a3 + (uint32_t)k), n); j = 0;
            while (n[j] && i < 152) b[i++] = n[j++];
            b[i++] = ' ';
        }
        b[i++] = '\n'; b[i] = '\0'; emu_print(b);
    }
    dos_AllocMem_glue(size, reqs, &addr);
    m68k_set_reg(M68K_REG_D0, addr);
}

static void exec_FreeMem(void)
{
    uint32_t addr = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t size = m68k_get_reg(NULL, M68K_REG_D0);
    dos_FreeMem_glue(addr, size);
}

static void exec_FindTask(void)
{
    /* Return pointer to the guest Process struct allocated post-load */
    m68k_set_reg(M68K_REG_D0, g_guest_proc_addr);
}

/* -------------------------------------------------------------------------
 * Guest List/MinList helpers for exec message-port functions
 * (glue read/write macros are defined near the top of this file)
 * ------------------------------------------------------------------------- */

/* Guest List/MinList offsets (AmigaOS standard) */
#define LH_HEAD        0
#define LH_TAIL        4
#define LH_TAILPRED    8
#define LH_TYPE       12
#define LN_SUCC        0
#define LN_PRED        4
#define LN_TYPE        8
#define LN_PRI         9
#define LN_NAME       10

/* Guest MsgPort offsets */
#define MP_FLAGS      14
#define MP_SIGBIT     15
#define MP_SIGTASK    16
#define MP_MSGLIST    20
#define MSGPORT_SZ    0x22
#define MLH_HEAD      0
#define MLH_TAIL      4
#define MLH_TAILPRED  8

/* Guest Message offsets */
#define MN_REPLYPORT  14
#define MN_LENGTH     18
#define MN_DATA       20

static int glue_list_empty(uint32_t list)
{
    uint32_t head = glue_r32(list + LH_HEAD);
    uint32_t tail = list + LH_TAIL;
    return head == tail;
}

static uint32_t glue_list_remove_head(uint32_t list)
{
    uint32_t head = glue_r32(list + LH_HEAD);
    uint32_t tail = list + LH_TAIL;
    if (head == tail) return 0;

    uint32_t succ = glue_r32(head + LN_SUCC);
    uint32_t pred = glue_r32(head + LN_PRED);

    glue_w32(pred + LN_SUCC, succ);
    glue_w32(succ + LN_PRED, pred);
    return head;
}

static void glue_list_add_tail(uint32_t list, uint32_t node)
{
    uint32_t tailpred = glue_r32(list + LH_TAILPRED);

    glue_w32(node + LN_SUCC, list + LH_TAIL);
    glue_w32(node + LN_PRED, tailpred);
    glue_w32(tailpred + LN_SUCC, node);
    glue_w32(list + LH_TAILPRED, node);
}

/* -------------------------------------------------------------------------
 * exec.library signal / message primitives (guest-memory compatible)
 * ------------------------------------------------------------------------- */

static void exec_Wait(void)
{
    uint32_t sigmask = m68k_get_reg(NULL, M68K_REG_D0);
    if (!sigmask) {
        UAOS_M68k_DeliverInterrupts();
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }
    /* Wait() blocks the host task, so the m68k slice loop — where guest
     * IRQs are normally dispatched — stops running.  Nap in 1-tick slices
     * and deliver pending guest interrupts between them; handlers may
     * Signal() this task (or PutMsg into a port it watches), which is how
     * OctaMED's player/input engine wakes its main loop. */
    uint32_t got = 0;
    for (;;) {
        g_blocked_in = 1;
        got = Task_WaitTicks(sigmask, 1);
        g_blocked_in = 0;
        UAOS_M68k_DeliverInterrupts();
        if (got) break;
        UaosTask *t = Task_Current();
        if (t && (t->tc_SigRecvd & sigmask)) {
            got = t->tc_SigRecvd & sigmask;
            t->tc_SigRecvd &= ~sigmask;
            break;
        }
    }
    m68k_set_reg(M68K_REG_D0, got);
}

static void exec_Signal(void)
{
    uint32_t task_addr = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t sigmask   = m68k_get_reg(NULL, M68K_REG_D0);
    UaosTask *t = Task_FindByM68kAddr(task_addr);
    if (!t) t = Task_Current();
    if (t) Signal(t, sigmask);
}

static void exec_SetSignal(void)
{
    uint32_t newsigs = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t sigmask = m68k_get_reg(NULL, M68K_REG_D1);
    m68k_set_reg(M68K_REG_D0, SetSignal(newsigs, sigmask));
}

static void exec_AllocSignal(void)
{
    int32_t signal_num = (int32_t)m68k_get_reg(NULL, M68K_REG_D0);
    UaosTask *t = Task_Current();
    if (!t) { m68k_set_reg(M68K_REG_D0, (uint32_t)-1); return; }

    if (signal_num == -1) {
        uint32_t alloc_mask = t->tc_SigAlloc;
        for (int i = 0; i < 32; i++) {
            if ((alloc_mask >> i) & 1) {
                t->tc_SigAlloc &= ~(1U << i);
                m68k_set_reg(M68K_REG_D0, (uint32_t)i);
                return;
            }
        }
        m68k_set_reg(M68K_REG_D0, (uint32_t)-1);
    } else if (signal_num >= 0 && signal_num < 32) {
        if ((t->tc_SigAlloc >> signal_num) & 1) {
            t->tc_SigAlloc &= ~(1U << signal_num);
            m68k_set_reg(M68K_REG_D0, (uint32_t)signal_num);
        } else {
            m68k_set_reg(M68K_REG_D0, (uint32_t)-1);
        }
    } else {
        m68k_set_reg(M68K_REG_D0, (uint32_t)-1);
    }
}

static void exec_FreeSignal(void)
{
    uint32_t signal_num = m68k_get_reg(NULL, M68K_REG_D0);
    UaosTask *t = Task_Current();
    if (!t) { m68k_set_reg(M68K_REG_D0, (uint32_t)-1); return; }

    if (signal_num < 32) {
        t->tc_SigAlloc |= (1U << signal_num);
        m68k_set_reg(M68K_REG_D0, 0);
    } else {
        m68k_set_reg(M68K_REG_D0, (uint32_t)-1);
    }
}

static void exec_PutMsg(void)
{
    /* PutMsg(port, message) — A0 = port, A1 = message */
    uint32_t port = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t msg  = m68k_get_reg(NULL, M68K_REG_A1);
    if (!port || !msg) return;

    glue_list_add_tail(port + MP_MSGLIST, msg);

    uint32_t sigtask = glue_r32(port + MP_SIGTASK);
    UaosTask *t = Task_FindByM68kAddr(sigtask);
    if (t) {
        uint32_t sigbit = glue_r8(port + MP_SIGBIT);
        Signal(t, 1U << sigbit);
    }
}

volatile uint32_t g_getmsg_calls, g_getmsg_hit;
static void exec_GetMsg(void)
{
    /* GetMsg(port) — A0 = port, returns message in D0 */
    uint32_t port = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t msg  = 0;
    g_getmsg_calls++;
    if (port) msg = glue_list_remove_head(port + MP_MSGLIST);
    if (msg) {
        g_getmsg_hit++;
        if (g_getmsg_hit <= 40) {
            char b[80]; int i = 0;
            const char *t = "[getmsg] port=0x"; while (t[i]) { b[i]=t[i]; i++; }
            char n[12]; u32_hex(port, n); int j = 0; while (n[j]) { b[i]=n[j]; i++; j++; }
            t = " msg=0x"; j = 0; while (t[j]) { b[i]=t[j]; i++; j++; }
            u32_hex(msg, n); j = 0; while (n[j]) { b[i]=n[j]; i++; j++; }
            t = " class=0x"; j = 0; while (t[j]) { b[i]=t[j]; i++; j++; }
            u32_hex(glue_r32(msg + 20), n); j = 0; while (n[j]) { b[i]=n[j]; i++; j++; }
            t = " code=0x"; j = 0; while (t[j]) { b[i]=t[j]; i++; j++; }
            u32_hex(glue_r16(msg + 24), n); j = 0; while (n[j]) { b[i]=n[j]; i++; j++; }
            b[i++] = '\n'; b[i] = 0; emu_print(b);
        }
    }
    m68k_set_reg(M68K_REG_D0, msg);
}

static void exec_ReplyMsg(void)
{
    /* ReplyMsg(message) — A1 = message */
    uint32_t msg = m68k_get_reg(NULL, M68K_REG_A1);
    if (!msg) return;
    uint32_t reply_port = glue_r32(msg + MN_REPLYPORT);
    if (reply_port) {
        glue_list_add_tail(reply_port + MP_MSGLIST, msg);
        uint32_t sigtask = glue_r32(reply_port + MP_SIGTASK);
        UaosTask *t = Task_FindByM68kAddr(sigtask);
        if (t) {
            uint32_t sigbit = glue_r8(reply_port + MP_SIGBIT);
            Signal(t, 1U << sigbit);
        }
    }
}

static void m68k_putch_call(uint32_t proc, uint8_t ch, uint32_t data);

/* exec RawDoFmt(fmt=a3, dataStream=a2, putChProc=a1, putChData=a0)
 *
 * Walks the format string and invokes the guest PutChProc once per output
 * character.  DataStream is an array of ULONG slots consumed in order;
 * word-sized conversions (d/u/x without 'l') take the slot's low word.
 * Supports %% and %d %u %x %X %s %c %b with flags '-'/'0', decimal or
 * '*' width and limit, 'l' length, and the 'n$' parameter selector.
 * Returns a pointer to the format string's terminator in D0. */
static void exec_RawDoFmt(void)
{
    uint32_t fmt  = m68k_get_reg(NULL, M68K_REG_A3);
    uint32_t data = m68k_get_reg(NULL, M68K_REG_A2);
    uint32_t proc = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t pd   = m68k_get_reg(NULL, M68K_REG_A0);
    if (!fmt || fmt >= GUEST_RAM_SIZE) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }

    uint32_t pc = fmt;
    uint32_t argidx = 0;
    int guard = 8192;

    while (guard-- > 0) {
        uint8_t c = glue_r8(pc);
        if (!c) break;
        pc++;
        if (c != '%') { m68k_putch_call(proc, c, pd); continue; }

        uint8_t n = glue_r8(pc);
        if (n == '%') { m68k_putch_call(proc, '%', pd); pc++; continue; }
        if (!n) break;

        /* [param$] — 1-based parameter selector for the value argument */
        uint32_t sel = 0;
        {
            uint32_t save = pc;
            uint32_t v = 0;
            while (glue_r8(pc) >= '0' && glue_r8(pc) <= '9') {
                v = v * 10 + (glue_r8(pc) - '0'); pc++;
            }
            if (glue_r8(pc) == '$' && v > 0) { sel = v; pc++; }
            else pc = save;
        }

        /* flags */
        int leftjust = 0, zeropad = 0;
        for (;;) {
            uint8_t f = glue_r8(pc);
            if (f == '-') { leftjust = 1; pc++; }
            else if (f == '0') { zeropad = 1; pc++; }
            else break;
        }

        /* width — decimal or '*' (next arg) */
        int width = 0;
        if (glue_r8(pc) == '*') {
            width = (int)glue_r32(data + argidx * 4); argidx++; pc++;
        } else {
            while (glue_r8(pc) >= '0' && glue_r8(pc) <= '9') {
                width = width * 10 + (glue_r8(pc) - '0'); pc++;
            }
        }

        /* limit — '.' then decimal or '*' */
        int limit = -1;
        if (glue_r8(pc) == '.') {
            pc++;
            if (glue_r8(pc) == '*') {
                limit = (int)glue_r32(data + argidx * 4); argidx++; pc++;
            } else {
                limit = 0;
                while (glue_r8(pc) >= '0' && glue_r8(pc) <= '9') {
                    limit = limit * 10 + (glue_r8(pc) - '0'); pc++;
                }
            }
        }

        /* length */
        int islong = 0;
        if (glue_r8(pc) == 'l') { islong = 1; pc++; }

        uint8_t type = glue_r8(pc);
        if (!type) break;
        pc++;

        uint32_t validx = sel ? (sel - 1) : argidx;
        if (!sel) argidx++;

        char num[24];
        int  numlen = 0;
        const char *strp = NULL;         /* string content (host ptr into RAM) */
        uint32_t strp_addr = 0;          /* guest addr if strp NULL */
        int  str_len = 0;
        int  numeric = 0, neg = 0;
        uint32_t uv = 0;

        switch (type) {
        case 'd': case 'u': {
            numeric = 1;
            int32_t sv;
            if (islong) { sv = (int32_t)glue_r32(data + validx * 4); }
            else        { sv = (int16_t)(glue_r32(data + validx * 4) & 0xFFFF); }
            uv = (uint32_t)sv;
            if (type == 'd' && sv < 0) { neg = 1; uv = (uint32_t)(-sv); }
            char tmp[16]; int tl = 0;
            if (uv == 0) tmp[tl++] = '0';
            while (uv) { tmp[tl++] = (char)('0' + uv % 10); uv /= 10; }
            while (tl) num[numlen++] = tmp[--tl];
            break;
        }
        case 'x': case 'X': {
            numeric = 1;
            if (islong) uv = glue_r32(data + validx * 4);
            else        uv = glue_r32(data + validx * 4) & 0xFFFF;
            char tmp[16]; int tl = 0;
            if (uv == 0) tmp[tl++] = '0';
            while (uv) {
                uint32_t d = uv & 15;
                tmp[tl++] = (char)(d < 10 ? '0' + d
                                  : (type == 'x' ? 'a' + d - 10 : 'A' + d - 10));
                uv >>= 4;
            }
            while (tl) num[numlen++] = tmp[--tl];
            break;
        }
        case 'c': {
            numeric = 1;
            num[numlen++] = (char)(glue_r32(data + validx * 4) & 0xFF);
            break;
        }
        case 's': {
            strp_addr = glue_r32(data + validx * 4);
            if (strp_addr && strp_addr < GUEST_RAM_SIZE) {
                int l = 0;
                while (strp_addr + l < GUEST_RAM_SIZE && glue_r8(strp_addr + l)
                       && (limit < 0 || l < limit)) l++;
                str_len = l;
            }
            break;
        }
        case 'b': {
            /* BSTR: longword BPTR (addr>>2); first byte = length */
            uint32_t bp = glue_r32(data + validx * 4);
            strp_addr = bp << 2;
            if (strp_addr && strp_addr < GUEST_RAM_SIZE) {
                int l = glue_r8(strp_addr);
                if (limit >= 0 && l > limit) l = limit;
                strp_addr++;
                str_len = l;
            }
            break;
        }
        default:
            /* unknown conversion — emit it literally */
            num[numlen++] = '%'; num[numlen++] = (char)type;
            numeric = 1;
            break;
        }

        /* Emit with justification/padding. */
        int content;
        if (str_len || type == 's' || type == 'b') {
            content = str_len;
        } else {
            content = numlen + (neg ? 1 : 0);
        }
        int zerolen = 0;
        if (numeric && limit > numlen) zerolen = limit - numlen;
        if (numeric && zerolen) content += zerolen;
        if (width < content) width = content;
        int pad = width - content;
        char pc2 = (zeropad && !leftjust && numeric && limit < 0) ? '0' : ' ';

        if (!leftjust) {
            if (pc2 == '0' && neg) { m68k_putch_call(proc, '-', pd); neg = 0; }
            for (int i = 0; i < pad; i++) m68k_putch_call(proc, pc2, pd);
        }
        if (neg) m68k_putch_call(proc, '-', pd);
        for (int i = 0; i < zerolen; i++) m68k_putch_call(proc, '0', pd);
        if (str_len || type == 's' || type == 'b') {
            for (int i = 0; i < str_len; i++) {
                uint8_t sc = glue_r8(strp_addr + i);
                m68k_putch_call(proc, sc, pd);
            }
        } else {
            for (int i = 0; i < numlen; i++) m68k_putch_call(proc, (uint8_t)num[i], pd);
        }
        if (leftjust)
            for (int i = 0; i < pad; i++) m68k_putch_call(proc, ' ', pd);
    }

    m68k_set_reg(M68K_REG_D0, pc);
}

static void exec_WaitPort(void)
{
    /* WaitPort(port) — A0 = port, returns port in D0.
     *
     * Amiga semantics: block until a message arrives.  Every poster
     * (PutMsg/ReplyMsg/post_intui_message) queues the node and then
     * Signal()s MP_SIGTASK with MP_SIGBIT, so nap until that signal —
     * or a message — lands.  Blocking unconditionally is wrong here:
     * Wait() has no timeout and some ports legitimately never receive a
     * message in UAOS (console/device reply ports that CLI tools poll),
     * so cap each call at ~100 ms.  A busy IDCMP loop then costs ~10
     * short sleeps per second instead of millions of ILLEGAL traps,
     * and sleeping doesn't burn emulated cycles — which is what used
     * to trip the cycle-budget abort on interactive programs.
     *
     * The INTUITICKS pump normally lives in the LoadAndRun slice loop,
     * which stalls while we're inside a trap; repump it here when in
     * that context (g_chipset_sync_disabled == 0).  Per-task M68k
     * contexts never pumped ticks and mustn't start — the slot table's
     * guest_win pointers belong to other tasks' address spaces. */
    uint32_t port = m68k_get_reg(NULL, M68K_REG_A0);
    if (!port) { m68k_set_reg(M68K_REG_D0, 0); return; }

    UaosTask *self = Task_Current();
    if (!self || !glue_r32(port + MP_SIGTASK)) {
        /* Nobody could ever signal this port — keep the old immediate
         * return so callers polling dead ports can't hang. */
        m68k_set_reg(M68K_REG_D0,
                     glue_list_empty(port + MP_MSGLIST) ? 0 : port);
        return;
    }

    uint32_t sigbit = glue_r8(port + MP_SIGBIT);
    uint32_t mask   = (sigbit < 32) ? (1U << sigbit) : 0;

    extern volatile uint64_t g_pit_ticks;   /* 100 Hz */
    extern void UAOS_Intuition_PostIntuiTicks(void);
    extern int  g_chipset_sync_disabled;
    uint64_t deadline = g_pit_ticks + 10;   /* ~100 ms cap per call */

    while (glue_list_empty(port + MP_MSGLIST) && g_pit_ticks < deadline) {
        if (!g_chipset_sync_disabled)
            UAOS_Intuition_PostIntuiTicks();
        else
            UAOS_M68k_DeliverInterrupts();   /* per-task ctx: keep guest IRQs flowing */
        /* One-tick nap as a real blocking wait (UAOS-169): the task
         * deschedules immediately instead of sti;hlt-ing as g_current,
         * and wakes early when PutMsg signals the port's sigbit. */
        g_blocked_in = 2;
        Task_WaitTicks(mask, 1);
        g_blocked_in = 0;
        /* Consume the port signal — it only means "check the list"; a
         * leftover edge must not skip every future nap. */
        if (mask && (self->tc_SigRecvd & mask))
            Task_ClearSig(mask);
    }

    m68k_set_reg(M68K_REG_D0,
                 glue_list_empty(port + MP_MSGLIST) ? 0 : port);
}

/* -------------------------------------------------------------------------
 * Task nesting counters — Forbid/Permit/Disable/Enable
 * TDNestCnt @ SysBase+0x127, IDNestCnt @ SysBase+0x126, both init -1.
 * ------------------------------------------------------------------------- */

/* Forbid/Permit nest on TDNestCnt (ExecBase+0x127); Disable/Enable on
 * IDNestCnt (ExecBase+0x126).  Both start at -1 (enabled). */
static void exec_Forbid(void)  { glue_w8(EXEC_BASE + 0x127, glue_r8(EXEC_BASE + 0x127) + 1); }
static void exec_Permit(void)  { glue_w8(EXEC_BASE + 0x127, glue_r8(EXEC_BASE + 0x127) - 1); }
static void exec_Disable(void) { glue_w8(EXEC_BASE + 0x126, glue_r8(EXEC_BASE + 0x126) + 1); }
static void exec_Enable(void)  { glue_w8(EXEC_BASE + 0x126, glue_r8(EXEC_BASE + 0x126) - 1); }

static void exec_SuperState(void) { m68k_set_reg(M68K_REG_D0, 0); }
static void exec_UserState(void)  { }

static void exec_InitStruct(void)
{
    /* InitStruct(initTable=a1, memory=a2, size=d0).  Only the NULL-table
     * (zero-fill) form for now — tables are rare enough that the catch-all
     * log will flag if real parsing is needed. */
    uint32_t table = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t mem   = m68k_get_reg(NULL, M68K_REG_A2);
    uint32_t size  = m68k_get_reg(NULL, M68K_REG_D0);
    if (!table && mem + size < GUEST_RAM_SIZE) {
        for (uint32_t i = 0; i < size; i++) g_ram[mem + i] = 0;
    }
}

static void exec_CopyMem(int quick)
{
    /* CopyMem(src=a0, dst=a1, size=d0) — handles overlap; Quick = forward */
    uint32_t src = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t dst = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t len = m68k_get_reg(NULL, M68K_REG_D0);
    if (src >= GUEST_RAM_SIZE || dst >= GUEST_RAM_SIZE) return;
    if (src + len > GUEST_RAM_SIZE) len = GUEST_RAM_SIZE - src;
    if (dst + len > GUEST_RAM_SIZE) len = GUEST_RAM_SIZE - dst;
    if (!quick && dst > src && dst < src + len) {
        for (uint32_t i = len; i > 0; i--) g_ram[dst + i - 1] = g_ram[src + i - 1];
    } else {
        for (uint32_t i = 0; i < len; i++) g_ram[dst + i] = g_ram[src + i];
    }
}

static void exec_AvailMem(void)
{
    uint32_t attrs = m68k_get_reg(NULL, M68K_REG_D1);
    uint32_t total = 0, largest = 0;
    dos_AvailMem_glue(attrs, &total, &largest);
    m68k_set_reg(M68K_REG_D0, total);
}

static void exec_TypeOfMem(void)
{
    uint32_t addr = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t t = MEMF_PUBLIC | MEMF_FAST;
    if (addr < 0x800000u) t = MEMF_PUBLIC | MEMF_CHIP | MEMF_24BITDMA;
    m68k_set_reg(M68K_REG_D0, t);
}

/* AllocEntry(memList=a0) — real MemList layout:
 *   +0  struct Node (14 bytes)
 *   +14 UWORD ml_NumEntries
 *   +16 MemEntry[n]: { me_Un.meu_Reqs|me_Addr, me_Length } 8 bytes each
 * Allocates each entry, fills me_Addr, returns a NEW MemList in D0. */
static void exec_AllocEntry(void)
{
    uint32_t ml  = m68k_get_reg(NULL, M68K_REG_A0);
    if (!ml || ml + 18 >= GUEST_RAM_SIZE) { m68k_set_reg(M68K_REG_D0, 0x80000000u); return; }
    uint32_t n   = glue_r16(ml + 14);
    uint32_t out = 0;

    uint32_t result = 0;
    dos_AllocMem_glue(16 + n * 8, MEMF_PUBLIC, &result);
    if (!result) { m68k_set_reg(M68K_REG_D0, 0x80000000u); return; }
    for (uint32_t i = 0; i < 16 + n * 8; i++) g_ram[result + i] = 0;
    glue_w16(result + 14, (uint16_t)n);

    for (uint32_t i = 0; i < n; i++) {
        uint32_t me   = ml + 16 + i * 8;
        uint32_t reqs = glue_r32(me);
        uint32_t len  = glue_r32(me + 4);
        uint32_t addr = 0;
        dos_AllocMem_glue(len, reqs, &addr);
        if (!addr) { out = 0x80000000u | i; break; }
        glue_w32(result + 16 + i * 8,     addr);
        glue_w32(result + 16 + i * 8 + 4, len);
        /* also patch the caller's list — SAS/C rt reads me_Addr back */
        glue_w32(me, addr);
    }
    m68k_set_reg(M68K_REG_D0, out ? out : result);
}

static void exec_FreeEntry(void)
{
    uint32_t ml = m68k_get_reg(NULL, M68K_REG_A0);
    if (!ml || ml + 18 >= GUEST_RAM_SIZE) return;
    uint32_t n = glue_r16(ml + 14);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t addr = glue_r32(ml + 16 + i * 8);
        uint32_t len  = glue_r32(ml + 16 + i * 8 + 4);
        if (addr && len) dos_FreeMem_glue(addr, len);
    }
    dos_FreeMem_glue(ml, 16 + n * 8);
}

/* ---- guest List operations (exec/lists.h semantics) -------------------- */

static void glue_list_new(uint32_t list)
{
    glue_w32(list + LH_HEAD,     list + LH_TAIL);
    glue_w32(list + LH_TAIL,     0);
    glue_w32(list + LH_TAILPRED, list + LH_HEAD);
}

static void exec_Insert(void)
{
    /* Insert(list=a0, node=a1, listNode=a2) — node after listNode */
    uint32_t node = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t prev = m68k_get_reg(NULL, M68K_REG_A2);
    uint32_t succ = glue_r32(prev + LN_SUCC);
    glue_w32(node + LN_SUCC, succ);
    glue_w32(node + LN_PRED, prev);
    glue_w32(prev + LN_SUCC, node);
    glue_w32(succ + LN_PRED, node);
}

static void exec_AddHead(void)
{
    uint32_t list = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t node = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t head = glue_r32(list + LH_HEAD);
    glue_w32(node + LN_SUCC, head);
    glue_w32(node + LN_PRED, list + LH_HEAD);
    glue_w32(head + LN_PRED, node);
    glue_w32(list + LH_HEAD, node);
}

static void exec_AddTail_glue(void)
{
    glue_list_add_tail(m68k_get_reg(NULL, M68K_REG_A0),
                       m68k_get_reg(NULL, M68K_REG_A1));
}

static void exec_Remove(void)
{
    uint32_t node = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t succ = glue_r32(node + LN_SUCC);
    uint32_t pred = glue_r32(node + LN_PRED);
    glue_w32(pred + LN_SUCC, succ);
    glue_w32(succ + LN_PRED, pred);
}

static void exec_RemHead(void)
{
    uint32_t list = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t node = glue_r32(list + LH_HEAD);
    uint32_t succ = node ? glue_r32(node + LN_SUCC) : 0;
    if (!node || !succ) { m68k_set_reg(M68K_REG_D0, 0); return; }
    glue_w32(list + LH_HEAD, succ);
    glue_w32(succ + LN_PRED, list + LH_HEAD);
    m68k_set_reg(M68K_REG_D0, node);
}

static void exec_RemTail(void)
{
    uint32_t list = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t node = glue_r32(list + LH_TAILPRED);
    uint32_t pred = node ? glue_r32(node + LN_PRED) : 0;
    if (!node || !pred) { m68k_set_reg(M68K_REG_D0, 0); return; }
    glue_w32(list + LH_TAILPRED, pred);
    glue_w32(pred + LN_SUCC, list + LH_TAIL);
    m68k_set_reg(M68K_REG_D0, node);
}

static void exec_Enqueue(void)
{
    /* Enqueue(list=a0, node=a1) — insert by descending ln_Pri */
    uint32_t list = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t node = m68k_get_reg(NULL, M68K_REG_A1);
    int8_t   pri  = (int8_t)glue_r8(node + LN_PRI);
    uint32_t cur  = glue_r32(list + LH_HEAD);
    while (cur && glue_r32(cur + LN_SUCC) &&
           (int8_t)glue_r8(cur + LN_PRI) >= pri)
        cur = glue_r32(cur + LN_SUCC);
    /* insert before cur (after cur's pred) */
    uint32_t pred = glue_r32(cur + LN_PRED);
    glue_w32(node + LN_SUCC, cur);
    glue_w32(node + LN_PRED, pred);
    glue_w32(pred + LN_SUCC, node);
    glue_w32(cur + LN_PRED, node);
}

static void exec_FindName(void)
{
    /* FindName(list=a0, name=a1) → D0=node or 0 */
    uint32_t list = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t name = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t cur  = glue_r32(list + LH_HEAD);
    while (cur && glue_r32(cur + LN_SUCC)) {
        uint32_t nn = glue_r32(cur + LN_NAME);
        int match = 1;
        if (nn && nn < GUEST_RAM_SIZE && name < GUEST_RAM_SIZE) {
            for (int i = 0; i < 128; i++) {
                uint8_t a = g_ram[name + i], b = g_ram[nn + i];
                if (a != b || !a) { if (a != b) match = 0; break; }
            }
        } else match = 0;
        if (match) { m68k_set_reg(M68K_REG_D0, cur); return; }
        cur = glue_r32(cur + LN_SUCC);
    }
    m68k_set_reg(M68K_REG_D0, 0);
}

/* ---- ports ------------------------------------------------------------- */

#define PORTLIST_ADDR (EXEC_BASE + 0x188)   /* SysBase PortList */

static void exec_AddPort(void)
{
    uint32_t port = m68k_get_reg(NULL, M68K_REG_A1);
    glue_list_add_tail(PORTLIST_ADDR, port);
}

static void exec_RemPort(void)
{
    uint32_t port = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t succ = glue_r32(port + LN_SUCC);
    uint32_t pred = glue_r32(port + LN_PRED);
    glue_w32(pred + LN_SUCC, succ);
    glue_w32(succ + LN_PRED, pred);
}

static void exec_FindPort(void)
{
    /* FindPort(name=a1) — walk SysBase PortList */
    m68k_set_reg(M68K_REG_A0, PORTLIST_ADDR);
    uint32_t save_a0 = 0;
    (void)save_a0;
    /* reuse FindName on the port list */
    uint32_t list = PORTLIST_ADDR;
    uint32_t name = m68k_get_reg(NULL, M68K_REG_A1);
    uint32_t cur  = glue_r32(list + LH_HEAD);
    while (cur && glue_r32(cur + LN_SUCC)) {
        uint32_t nn = glue_r32(cur + LN_NAME);
        int match = 0;
        if (nn && nn < GUEST_RAM_SIZE && name < GUEST_RAM_SIZE) {
            match = 1;
            for (int i = 0; i < 128; i++) {
                uint8_t a = g_ram[name + i], b = g_ram[nn + i];
                if (a != b) { match = 0; break; }
                if (!a) break;
            }
        }
        if (match) { m68k_set_reg(M68K_REG_D0, cur); return; }
        cur = glue_r32(cur + LN_SUCC);
    }
    m68k_set_reg(M68K_REG_D0, 0);
}

static int exec_alloc_sigbit(void)
{
    UaosTask *t = Task_Current();
    if (!t) return -1;
    for (int i = 0; i < 32; i++)
        if ((t->tc_SigAlloc >> i) & 1) { t->tc_SigAlloc &= ~(1U << i); return i; }
    return -1;
}

static void exec_CreateMsgPort(void)
{
    uint32_t port = 0;
    dos_AllocMem_glue(MSGPORT_SZ, MEMF_PUBLIC | MEMF_CLEAR_FLAG, &port);
    if (!port) { m68k_set_reg(M68K_REG_D0, 0); return; }
    for (uint32_t i = 0; i < MSGPORT_SZ; i++) g_ram[port + i] = 0;
    g_ram[port + LN_TYPE]  = 4;   /* NT_MSGPORT */
    g_ram[port + MP_FLAGS] = 0;   /* PA_SIGNAL */
    int sb = exec_alloc_sigbit();
    if (sb < 0) { dos_FreeMem_glue(port, MSGPORT_SZ); m68k_set_reg(M68K_REG_D0, 0); return; }
    g_ram[port + MP_SIGBIT] = (uint8_t)sb;
    glue_w32(port + MP_SIGTASK, g_guest_proc_addr);
    glue_list_new(port + MP_MSGLIST);
    m68k_set_reg(M68K_REG_D0, port);
}

static void exec_DeleteMsgPort(void)
{
    uint32_t port = m68k_get_reg(NULL, M68K_REG_A0);
    if (!port) return;
    uint32_t sb = glue_r8(port + MP_SIGBIT);
    UaosTask *t = Task_Current();
    if (t && sb < 32) t->tc_SigAlloc |= (1U << sb);
    dos_FreeMem_glue(port, MSGPORT_SZ);
}

/* ---- IORequest / devices ------------------------------------------------ */

#define IO_DEVICE     20
#define IO_UNIT       24
#define IO_COMMAND    28
#define IO_FLAGS      30
#define IO_ERROR      31
#define IOSTD_SIZE    48

static uint8_t  g_audio_alloc_mask;
static uint8_t  g_audio_alloc_key;
static uint32_t g_audio_open_cnt;
#define IOF_QUICK     0x01
#define NT_REPLYMSG   6

static void exec_CreateIORequest(void)
{
    /* CreateIORequest(ioReplyPort=a0, size=d0) → D0=IORequest */
    uint32_t port = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t size = m68k_get_reg(NULL, M68K_REG_D0);
    if (size < IOSTD_SIZE) size = IOSTD_SIZE;
    uint32_t io = 0;
    dos_AllocMem_glue(size, MEMF_PUBLIC | MEMF_CLEAR_FLAG, &io);
    if (!io) { m68k_set_reg(M68K_REG_D0, 0); return; }
    for (uint32_t i = 0; i < size; i++) g_ram[io + i] = 0;
    g_ram[io + LN_TYPE] = NT_REPLYMSG;
    glue_w32(io + MN_REPLYPORT, port);
    glue_w16(io + MN_LENGTH, size);
    m68k_set_reg(M68K_REG_D0, io);
}

static void exec_DeleteIORequest(void)
{
    uint32_t io = m68k_get_reg(NULL, M68K_REG_A0);
    if (!io) return;
    uint32_t len = glue_r16(io + MN_LENGTH);
    if (!len) len = IOSTD_SIZE;
    dos_FreeMem_glue(io, len);
}

static void exec_OpenDevice(void)
{
    /* OpenDevice(devName=a0, unit=d0, ioRequest=a1, flags=d1)
     * -> D0 = io_Error (0 = success).  Bring-up stub: the ioreq gets a
     * fake device base and calls complete immediately; real device
     * dispatch (timer/console/keyboard/audio) is UAOS-240. */
    uint32_t name_ptr = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t ioreq    = m68k_get_reg(NULL, M68K_REG_A1);
    char name[64];
    int i = 0;
    while (i < 63 && name_ptr + i < GUEST_RAM_SIZE)
        { name[i] = (char)g_ram[name_ptr + i]; if (!name[i]) break; i++; }
    name[i] = '\0';
    {
        char msg[80] = "[exec] OpenDevice '";
        int k = emu_strlen(msg);
        for (int j = 0; name[j] && k < 76; j++) msg[k++] = name[j];
        msg[k++]='\''; msg[k++]='\n'; msg[k]='\0';
        emu_print(msg);
    }
    if (ioreq && ioreq + IOSTD_SIZE < GUEST_RAM_SIZE) {
        uint32_t devbase = FAKE_LIB_BASE;
        static const char AUDNAME[] = "audio.device";
        int eq = 1;
        for (int k = 0; AUDNAME[k]; k++) if (name[k] != AUDNAME[k]) { eq = 0; break; }
        if (eq && !name[12]) {
            devbase = AUDIO_DEV_BASE;
            g_audio_open_cnt++;
        }
        glue_w32(ioreq + IO_DEVICE, devbase);  /* fake or real device base */
        glue_w32(ioreq + IO_UNIT, m68k_get_reg(NULL, M68K_REG_D0));
        g_ram[ioreq + IO_ERROR] = 0;
    }
    m68k_set_reg(M68K_REG_D0, 0);   /* success */
}

static void exec_CloseDevice(void)
{
    uint32_t io = m68k_get_reg(NULL, M68K_REG_A1);
    if (io && io + IOSTD_SIZE < GUEST_RAM_SIZE &&
        glue_r32(io + IO_DEVICE) == AUDIO_DEV_BASE) {
        if (g_audio_open_cnt) g_audio_open_cnt--;
        if (!g_audio_open_cnt) g_audio_alloc_mask = 0;
    }
}

/* Route an IORequest to its device when it's one we emulate (audio.device).
 * Real exec DoIO/SendIO call the device's BeginIO internally.
 * Returns nonzero when the request was handled by a device. */
static void audio_dev_BeginIO(void);
static int io_dispatch_device(uint32_t io)
{
    if (io + IOSTD_SIZE >= GUEST_RAM_SIZE) return 0;
    if (glue_r32(io + IO_DEVICE) != AUDIO_DEV_BASE) return 0;
    uint32_t saved_a1 = m68k_get_reg(NULL, M68K_REG_A1);
    m68k_set_reg(M68K_REG_A1, io);
    audio_dev_BeginIO();
    m68k_set_reg(M68K_REG_A1, saved_a1);
    return 1;
}

static void exec_DoIO(void)
{
    /* DoIO(ioRequest=a1) → D0=io_Error.  Fake devices complete instantly. */
    uint32_t io = m68k_get_reg(NULL, M68K_REG_A1);
    if (!io) { m68k_set_reg(M68K_REG_D0, -20); return; }
    io_dispatch_device(io);
    uint8_t err = g_ram[io + IO_ERROR];
    m68k_set_reg(M68K_REG_D0, err);
}

static void exec_SendIO(void)
{
    /* SendIO(ioRequest=a1): mark in-flight then immediately complete by
     * replying to the reply port — WaitIO/CheckIO then find it. */
    uint32_t io = m68k_get_reg(NULL, M68K_REG_A1);
    if (!io) return;
    g_ram[io + IO_FLAGS] &= ~IOF_QUICK;
    if (io_dispatch_device(io)) return;   /* device handler completed+replied */
    g_ram[io + IO_ERROR]  = 0;
    uint32_t port = glue_r32(io + MN_REPLYPORT);
    if (port) {
        glue_list_add_tail(port + MP_MSGLIST, io);
        uint32_t sigtask = glue_r32(port + MP_SIGTASK);
        UaosTask *t = Task_FindByM68kAddr(sigtask);
        if (!t) t = Task_Current();
        if (t) Signal(t, 1U << glue_r8(port + MP_SIGBIT));
    }
}

static void exec_CheckIO(void)
{
    /* CheckIO(ioRequest=a1) → D0=ioreq if complete else 0.
     * Our SendIO replies instantly, so check the reply port list. */
    uint32_t io = m68k_get_reg(NULL, M68K_REG_A1);
    m68k_set_reg(M68K_REG_D0, io ? io : 0);
}

static void exec_WaitIO(void)
{
    /* WaitIO(ioRequest=a1) → D0=io_Error; wait for the reply port msg. */
    uint32_t io = m68k_get_reg(NULL, M68K_REG_A1);
    if (!io) { m68k_set_reg(M68K_REG_D0, -20); return; }
    uint32_t port = glue_r32(io + MN_REPLYPORT);
    if (port) {
        UaosTask *self = Task_Current();
        if (self && glue_r32(port + MP_SIGTASK)) {
            uint32_t sigbit = glue_r8(port + MP_SIGBIT);
            uint32_t mask   = (sigbit < 32) ? (1U << sigbit) : 0;
            extern volatile uint64_t g_pit_ticks;
            uint64_t deadline = g_pit_ticks + 10;
            while (glue_list_empty(port + MP_MSGLIST) && g_pit_ticks < deadline) {
                g_blocked_in = 3;
                Task_WaitTicks(mask, 1);
                g_blocked_in = 0;
                UAOS_M68k_DeliverInterrupts();
            }
        }
        /* pop our message if queued */
        glue_list_remove_head(port + MP_MSGLIST);
    }
    m68k_set_reg(M68K_REG_D0, g_ram[io + IO_ERROR]);
}

static void audio_dev_AbortIO(void);
static void exec_AbortIO(void)
{
    uint32_t io = m68k_get_reg(NULL, M68K_REG_A1);
    if (io && io + IOSTD_SIZE < GUEST_RAM_SIZE &&
        glue_r32(io + IO_DEVICE) == AUDIO_DEV_BASE) {
        uint32_t saved_a1 = m68k_get_reg(NULL, M68K_REG_A1);
        audio_dev_AbortIO();
        m68k_set_reg(M68K_REG_A1, saved_a1);
        return;
    }
    m68k_set_reg(M68K_REG_D0, 0);
}

/* ---- audio.device (UAOS-240) ---------------------------------------------
 * Trackers (OctaMED) use audio.device only for channel arbitration, then
 * drive Paula registers directly.  Enough of the command set to make
 * ADCMD_ALLOCATE/FREE behave correctly; everything else completes as a
 * successful no-op.  Device base = AUDIO_DEV_BASE with trapped LVOs for
 * BeginIO/AbortIO; OpenDevice/CloseDevice return this base for
 * "audio.device".
 *
 * struct IOAudio (m68k, word-aligned fields):
 *   +0   IORequest ioa_Request (32)
 *   +32  WORD   ioa_AllocKey
 *   +34  UBYTE *ioa_Data   — byte array of channel bitmask combos
 *   +38  ULONG  ioa_Length
 *   +42  UWORD  ioa_Period
 *   +44  UWORD  ioa_Volume
 *   +46  UWORD  ioa_Cycles
 * ------------------------------------------------------------------------- */
#define IOA_ALLOCKEY     32
#define IOA_DATA         34
#define IOA_LENGTH       38
#define IOA_PERIOD       42
#define IOA_VOLUME       44
#define IOA_CYCLES       46

#define CMD_RESET        1
#define CMD_WRITE        3
#define CMD_UPDATE       4
#define CMD_CLEAR        5
#define CMD_STOP         6
#define CMD_START        7
#define CMD_FLUSH        8
#define ADCMD_ALLOCATE   9    /* CMD_NONSTD+0 */
#define ADCMD_FREE       10
#define ADCMD_SETPREC    11
#define ADCMD_FINISH     12
#define ADCMD_PERVOL     13
#define ADCMD_LOCK       14
#define ADCMD_WAITCYCLE  15

#define ADIOERR_ALLOCFAILED  (-11)

static void audio_dev_reply(uint32_t io)
{
    /* Complete a non-QUICK request: reply it to its port like SendIO. */
    if (g_ram[io + IO_FLAGS] & IOF_QUICK) return;
    uint32_t port = glue_r32(io + MN_REPLYPORT);
    if (!port) return;
    glue_list_add_tail(port + MP_MSGLIST, io);
    uint32_t sigtask = glue_r32(port + MP_SIGTASK);
    UaosTask *t = Task_FindByM68kAddr(sigtask);
    if (!t) t = Task_Current();
    if (t) Signal(t, 1U << glue_r8(port + MP_SIGBIT));
}

static void audio_dev_BeginIO(void)
{
    uint32_t io = m68k_get_reg(NULL, M68K_REG_A1);
    if (!io || io + IOSTD_SIZE >= GUEST_RAM_SIZE) return;
    uint16_t cmd = glue_r16(io + IO_COMMAND);
    { char m[48]; const char *P = "[audcmd] "; int n = 0;
      while (*P) m[n++] = *P++;
      const char *H = "0123456789abcdef";
      m[n++] = H[(cmd >> 12) & 15]; m[n++] = H[(cmd >> 8) & 15];
      m[n++] = H[(cmd >> 4) & 15]; m[n++] = H[cmd & 15];
      m[n++] = '\n'; m[n] = 0; emu_print(m); }
    int8_t err = 0;

    switch (cmd) {
    case ADCMD_ALLOCATE: {
        /* ioa_Data = array of channel-bitmask combinations to try in order;
         * ioa_Length = array size.  On success: io_Unit = chosen mask,
         * ioa_AllocKey nonzero.  We never steal — all-or-nothing. */
        uint32_t data = glue_r32(io + IOA_DATA);
        uint32_t len  = glue_r32(io + IOA_LENGTH);
        uint8_t chosen = 0;
        int got = 0;
        if (len == 0) { got = 1; }
        else {
            for (uint32_t i = 0; i < len && i < 64 && data + i < GUEST_RAM_SIZE; i++) {
                uint8_t want = g_ram[data + i] & 0x0F;
                if (want && !(g_audio_alloc_mask & want)) { chosen = want; got = 1; break; }
            }
        }
        if (got) {
            g_audio_alloc_mask |= chosen;
            int16_t key = (int16_t)glue_r16(io + IOA_ALLOCKEY);
            if (!key) key = (int16_t)(++g_audio_alloc_key ? g_audio_alloc_key
                                                         : (g_audio_alloc_key = 1));
            glue_w16(io + IOA_ALLOCKEY, (uint16_t)key);
            glue_w32(io + IO_UNIT, chosen);   /* channel bitmap in io_Unit */
        } else {
            glue_w32(io + IO_UNIT, 0);
            err = ADIOERR_ALLOCFAILED;
        }
        break;
    }
    case ADCMD_FREE: {
        /* io_Unit low nibble = channel mask to release. */
        g_audio_alloc_mask &= (uint8_t)~glue_r32(io + IO_UNIT);
        break;
    }
    /* Everything else (CMD_RESET/WRITE/PERVOL/SETC/LOCK/START/STOP/FLUSH…)
     * succeeds immediately — playback itself goes through the Paula register
     * emulation, which the guest drives directly. */
    default:
        break;
    }
    g_ram[io + IO_ERROR] = (uint8_t)err;
    audio_dev_reply(io);
}

static void audio_dev_AbortIO(void)
{
    uint32_t io = m68k_get_reg(NULL, M68K_REG_A1);
    if (io && io + IOSTD_SIZE < GUEST_RAM_SIZE) {
        uint16_t cmd = glue_r16(io + IO_COMMAND);
        if (cmd == ADCMD_ALLOCATE)
            g_audio_alloc_mask &= (uint8_t)~glue_r32(io + IO_UNIT);
    }
    m68k_set_reg(M68K_REG_D0, 0);
}

static void audio_dev_Open(void)
{
    /* Device Open (d0=unit, a1=ioreq? device Open gets d0=unit): bump open
     * count, return device base in D0. */
    g_audio_open_cnt++;
    m68k_set_reg(M68K_REG_D0, AUDIO_DEV_BASE);
}

static void audio_dev_Close(void)
{
    if (g_audio_open_cnt) g_audio_open_cnt--;
    if (!g_audio_open_cnt) g_audio_alloc_mask = 0;   /* release all channels */
    m68k_set_reg(M68K_REG_D0, 0);
}

/* ---- semaphores (single-context approximation) ---------------------------
 * SignalSemaphore (exec/semaphores.h):
 *   +0  Node ss_Link (14)
 *   +14 SHORT ss_NestCount
 *   +16 MinList ss_WaitQueue (12)
 *   +28 APTR ss_Owner
 *   +32 SHORT ss_QueueCnt
 *   +34 WORD ss_Link wait count pad
 * ------------------------------------------------------------------------- */
#define SS_NESTCOUNT 14
#define SS_WAITQUEUE 16
#define SS_OWNER     28
#define SS_QUEUECNT  32
#define SEM_SIZE     46

static void sem_init(uint32_t sem)
{
    g_ram[sem + LN_TYPE] = 15;              /* NT_SIGNALSEM */
    glue_w16(sem + SS_NESTCOUNT, 0);
    glue_w32(sem + SS_WAITQUEUE + MLH_HEAD,     sem + SS_WAITQUEUE + MLH_TAIL);
    glue_w32(sem + SS_WAITQUEUE + MLH_TAIL,     0);
    glue_w32(sem + SS_WAITQUEUE + MLH_TAILPRED, sem + SS_WAITQUEUE + MLH_HEAD);
    glue_w32(sem + SS_OWNER, 0);
    glue_w16(sem + SS_QUEUECNT, (uint16_t)-1);
}

static void exec_InitSemaphore(void)
{
    uint32_t sem = m68k_get_reg(NULL, M68K_REG_A0);
    if (sem && sem + SEM_SIZE < GUEST_RAM_SIZE) sem_init(sem);
}

static void exec_ObtainSem(void)
{
    uint32_t sem = m68k_get_reg(NULL, M68K_REG_A0);
    if (!sem) return;
    glue_w32(sem + SS_OWNER, g_guest_proc_addr);
    glue_w16(sem + SS_NESTCOUNT, glue_r16(sem + SS_NESTCOUNT) + 1);
}

static void exec_ReleaseSem(void)
{
    uint32_t sem = m68k_get_reg(NULL, M68K_REG_A0);
    if (!sem) return;
    uint32_t n = glue_r16(sem + SS_NESTCOUNT);
    if (n) n--;
    glue_w16(sem + SS_NESTCOUNT, n);
    if (!n) glue_w32(sem + SS_OWNER, 0);
}

static void exec_AttemptSem(void)
{
    uint32_t sem = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t owner = sem ? glue_r32(sem + SS_OWNER) : 0;
    if (!owner || owner == g_guest_proc_addr) {
        if (sem) {
            glue_w32(sem + SS_OWNER, g_guest_proc_addr);
            glue_w16(sem + SS_NESTCOUNT, glue_r16(sem + SS_NESTCOUNT) + 1);
        }
        m68k_set_reg(M68K_REG_D0, 1);
    } else {
        m68k_set_reg(M68K_REG_D0, 0);
    }
}

static void exec_Procure(void)   { m68k_set_reg(M68K_REG_D0, 1); }
static void exec_Vacate(void)    { }
static void exec_ObtainSemList(void)  { }
static void exec_ReleaseSemList(void) { }
static void exec_FindSemaphore(void)  { m68k_set_reg(M68K_REG_D0, 0); }
static void exec_AddSemaphore(void)   { }
static void exec_RemSemaphore(void)   { }

/* ---- misc -------------------------------------------------------------- */

static void exec_GetCC(void)
{
    m68k_set_reg(M68K_REG_D0, m68k_get_reg(NULL, M68K_REG_SR) & 0x1F);
}

static void exec_SetTaskPri(void)
{
    uint32_t task = m68k_get_reg(NULL, M68K_REG_A1);
    uint8_t  old  = task ? g_ram[task + LN_PRI] : 0;
    if (task) g_ram[task + LN_PRI] = (uint8_t)m68k_get_reg(NULL, M68K_REG_D0);
    m68k_set_reg(M68K_REG_D0, old);
}

static void exec_SetExcept(void)
{
    /* SetExcept(newSignals=d0, signalSet=d1) → D0=old & mask */
    uint32_t proc = g_guest_proc_addr;
    uint32_t mask = m68k_get_reg(NULL, M68K_REG_D1);
    uint32_t old  = proc ? glue_r32(proc + 0x1E) : 0;   /* tc_SigExcept */
    if (proc) glue_w32(proc + 0x1E,
                       (old & ~mask) | (m68k_get_reg(NULL, M68K_REG_D0) & mask));
    m68k_set_reg(M68K_REG_D0, old & mask);
}

static void exec_AllocTrap(void)
{
    /* AllocTrap(trapNum=d0) → D0=trap or -1; bitmap in task tc_TrapAlloc */
    uint32_t proc = g_guest_proc_addr;
    int32_t num = (int32_t)m68k_get_reg(NULL, M68K_REG_D0);
    if (!proc) { m68k_set_reg(M68K_REG_D0, -1); return; }
    uint32_t alloc = glue_r16(proc + 0x22);   /* tc_TrapAlloc (bits = avail) */
    if (num == -1) {
        for (int i = 0; i < 16; i++)
            if ((alloc >> i) & 1) { num = i; break; }
    }
    if (num < 0 || num > 15 || !((alloc >> num) & 1)) {
        m68k_set_reg(M68K_REG_D0, -1); return;
    }
    glue_w16(proc + 0x22, alloc & ~(1u << num));
    m68k_set_reg(M68K_REG_D0, (uint32_t)num);
}

static void exec_FreeTrap(void)
{
    uint32_t proc = g_guest_proc_addr;
    uint32_t num  = m68k_get_reg(NULL, M68K_REG_D0);
    if (proc && num < 16)
        glue_w16(proc + 0x22, glue_r16(proc + 0x22) | (1u << num));
}

static void exec_AllocVec(void)
{
    /* AllocVec(size=d0, attrs=d1) → mem with 4-byte size header */
    uint32_t size = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t attrs = m68k_get_reg(NULL, M68K_REG_D1);
    uint32_t blk = 0;
    dos_AllocMem_glue(size + 4, attrs, &blk);
    if (!blk) { m68k_set_reg(M68K_REG_D0, 0); return; }
    glue_w32(blk, size + 4);
    m68k_set_reg(M68K_REG_D0, blk + 4);
}

static void exec_FreeVec(void)
{
    uint32_t mem = m68k_get_reg(NULL, M68K_REG_A1);
    if (!mem) return;
    dos_FreeMem_glue(mem - 4, glue_r32(mem - 4));
}

/* StackSwap(sss=a0) — V37.  Swaps the task's stack bounds and SP with the
 * values in the StackSwapStruct; the struct receives the old values so a
 * second call restores the original stack.  No stack contents are copied.
 *
 * The jsr'd return address sits on the OLD stack, so like the ROM we pop
 * it off and re-push it on the new stack: the stub RTS that follows our
 * dispatch then returns to the caller running on the new stack. */
static void exec_StackSwap(void)
{
    uint32_t sss  = (uint32_t)m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t sp   = (uint32_t)m68k_get_reg(NULL, M68K_REG_SP);
    if (sss + 12 > GUEST_RAM_SIZE) return;

    uint32_t retaddr = (sp + 4 <= GUEST_RAM_SIZE) ? glue_r32(sp) : 0;
    uint32_t nlo = glue_r32(sss + 0);   /* stk_LowerBound */
    uint32_t nhi = glue_r32(sss + 4);   /* stk_UpperBound */
    uint32_t nsp = glue_r32(sss + 8);   /* stk_Pointer    */

    uint32_t task = glue_r32(EXEC_BASE + 0x114);   /* ThisTask */
    uint32_t olo = 0, ohi = 0;
    if (task && task + 0x42 <= GUEST_RAM_SIZE) {
        olo = glue_r32(task + 0x3A);    /* tc_SPLower */
        ohi = glue_r32(task + 0x3E);    /* tc_SPUpper */
        glue_w32(task + 0x3A, nlo);
        glue_w32(task + 0x3E, nhi);
    }
    glue_w32(sss + 0, olo);
    glue_w32(sss + 4, ohi);
    glue_w32(sss + 8, sp + 4);          /* old SP past the jsr'd retaddr */

    if (nsp >= 4 && nsp + 4 <= GUEST_RAM_SIZE) {
        glue_w32(nsp - 4, retaddr);
        m68k_set_reg(M68K_REG_SP, nsp - 4);
    }
}

static void exec_SetFunction(void)
{
    /* SetFunction(library=a1, funcOffset=a0 (negative), newFunc=d0? no:
     * SetFunction(library=a1, funcOffset=a0, newFunc=d0) → D0=old vector.
     * Real proto: SetFunction(a1=lib, a0=lvo, d0=newfunc). */
    uint32_t lib = m68k_get_reg(NULL, M68K_REG_A1);
    int32_t  lvo = (int32_t)m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t fn  = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t addr = (uint32_t)((int)lib + lvo);
    m68k_set_reg(M68K_REG_D0, addr);   /* "old" = the stub addr */
    if (addr + 6 <= GUEST_RAM_SIZE && fn) {
        g_ram[addr+0] = 0x4E; g_ram[addr+1] = 0xF9;  /* JMP abs.l */
        glue_w32(addr + 2, fn);
    }
}

static void exec_CacheControl(void) { m68k_set_reg(M68K_REG_D0, 0); }

/* Public exec dispatcher for use by other host library code (e.g. gadtools). */
void UAOS_Exec_Dispatch(uint32_t fn)
{
    switch (fn) {
        case EXEC_OPEN_LIBRARY:  exec_OpenLibrary();  break;
        case EXEC_CLOSE_LIBRARY: exec_CloseLibrary(); break;
        case EXEC_ALLOC_MEM:     exec_AllocMem();     break;
        case EXEC_FREE_MEM:      exec_FreeMem();      break;
        case EXEC_FIND_TASK:     exec_FindTask();     break;
        case EXEC_WAIT:          exec_Wait();         break;
        case EXEC_SIGNAL:        exec_Signal();       break;
        case EXEC_SETSIGNAL:     exec_SetSignal();    break;
        case EXEC_ALLOC_SIGNAL:  exec_AllocSignal();  break;
        case EXEC_FREE_SIGNAL:   exec_FreeSignal();   break;
        case EXEC_PUT_MSG:       exec_PutMsg();       break;
        case EXEC_GET_MSG:       exec_GetMsg();       break;
        case EXEC_REPLY_MSG:     exec_ReplyMsg();     break;
        case EXEC_WAIT_PORT:     exec_WaitPort();     break;
        case EXEC_CACHE_CLEAR_U: break;               /* no icache in Musashi */
        case EXEC_INIT_STRUCT:   exec_InitStruct();   break;
        case EXEC_DISABLE:       exec_Disable();      break;
        case EXEC_ENABLE:        exec_Enable();       break;
        case EXEC_FORBID:        exec_Forbid();       break;
        case EXEC_PERMIT:        exec_Permit();       break;
        case EXEC_SUPER_STATE:   exec_SuperState();   break;
        case EXEC_USER_STATE:    exec_UserState();    break;
        case EXEC_SET_INT_VECTOR: exec_SetIntVector(); break;
        case EXEC_ADD_INT_SERVER: exec_AddIntServer(); break;
        case EXEC_REM_INT_SERVER: exec_RemIntServer(); break;
        case EXEC_CAUSE:          exec_Cause();        break;
        case EXEC_AVAIL_MEM:     exec_AvailMem();     break;
        case EXEC_ALLOC_ENTRY:   exec_AllocEntry();   break;
        case EXEC_FREE_ENTRY:    exec_FreeEntry();    break;
        case EXEC_INSERT:        exec_Insert();       break;
        case EXEC_ADD_HEAD:      exec_AddHead();      break;
        case EXEC_ADD_TAIL:      exec_AddTail_glue(); break;
        case EXEC_REMOVE:        exec_Remove();       break;
        case EXEC_REM_HEAD:      exec_RemHead();      break;
        case EXEC_REM_TAIL:      exec_RemTail();      break;
        case EXEC_ENQUEUE:       exec_Enqueue();      break;
        case EXEC_FIND_NAME:     exec_FindName();     break;
        case EXEC_SET_TASK_PRI:  exec_SetTaskPri();   break;
        case EXEC_SET_EXCEPT:    exec_SetExcept();    break;
        case EXEC_ALLOC_TRAP:    exec_AllocTrap();    break;
        case EXEC_FREE_TRAP:     exec_FreeTrap();     break;
        case EXEC_ADD_PORT:      exec_AddPort();      break;
        case EXEC_REM_PORT:      exec_RemPort();      break;
        case EXEC_FIND_PORT:     exec_FindPort();     break;
        case EXEC_OLD_OPEN_LIBRARY:
            m68k_set_reg(M68K_REG_D0, 0);             /* any version */
            exec_OpenLibrary(); break;
        case EXEC_SET_FUNCTION:  exec_SetFunction();  break;
        case EXEC_OPEN_DEVICE:   exec_OpenDevice();   break;
        case EXEC_CLOSE_DEVICE:  exec_CloseDevice();  break;
        case EXEC_DO_IO:         exec_DoIO();         break;
        case EXEC_SEND_IO:       exec_SendIO();       break;
        case EXEC_CHECK_IO:      exec_CheckIO();      break;
        case EXEC_WAIT_IO:       exec_WaitIO();       break;
        case EXEC_ABORT_IO:      exec_AbortIO();      break;
        case EXEC_OPEN_RESOURCE: exec_OpenResource(); break;
        case EXEC_GETCC:         exec_GetCC();        break;
        case EXEC_TYPE_OF_MEM:   exec_TypeOfMem();    break;
        case EXEC_PROCURE:       exec_Procure();      break;
        case EXEC_VACATE:        exec_Vacate();       break;
        case EXEC_INIT_SEMAPHORE:    exec_InitSemaphore(); break;
        case EXEC_OBTAIN_SEM:  exec_ObtainSem();     break;
        case EXEC_RELEASE_SEM: exec_ReleaseSem();   break;
        case EXEC_ATTEMPT_SEM: exec_AttemptSem();   break;
        case EXEC_OBTAIN_SEM_SHARED: exec_ObtainSem();    break;
        case EXEC_OBTAIN_SEM_LIST:   exec_ObtainSemList(); break;
        case EXEC_RELEASE_SEM_LIST:  exec_ReleaseSemList(); break;
        case EXEC_FIND_SEMAPHORE:    exec_FindSemaphore(); break;
        case EXEC_ADD_SEMAPHORE:     exec_AddSemaphore();  break;
        case EXEC_REM_SEMAPHORE:     exec_RemSemaphore();  break;
        case EXEC_COPY_MEM:       exec_CopyMem(0);    break;
        case EXEC_COPY_MEM_QUICK: exec_CopyMem(1);    break;
        case EXEC_CACHE_CONTROL:  exec_CacheControl(); break;
        case EXEC_CACHE_CLEAR_E:  break;              /* no icache */
        case EXEC_RAW_DO_FMT:     exec_RawDoFmt();    break;
        case EXEC_CREATE_IOREQUEST: exec_CreateIORequest(); break;
        case EXEC_DELETE_IOREQUEST: exec_DeleteIORequest(); break;
        case EXEC_CREATE_MSGPORT:   exec_CreateMsgPort();  break;
        case EXEC_DELETE_MSGPORT:   exec_DeleteMsgPort();  break;
        case EXEC_ALLOC_VEC:      exec_AllocVec();    break;
        case EXEC_FREE_VEC:       exec_FreeVec();     break;
        case EXEC_STACK_SWAP:     exec_StackSwap();   break;
        default: {
            /* Catch-all stub (EXEC_STUB_LVO) or unmapped fn — log the LVO
             * via the stub address (PC-4) and return 0. */
            uint32_t spc = m68k_get_reg(NULL, M68K_REG_PC);
            uint32_t a6  = m68k_get_reg(NULL, M68K_REG_A6);
            int32_t lvo = (int32_t)spc - 4 - (int32_t)a6;
            static int32_t last_lvo = 0;
            if (lvo != last_lvo) {
                last_lvo = lvo;
                char msg[48] = "[exec] unimpl lvo=-";
                char n[12]; u32_dec((uint32_t)(-lvo), n, 12);
                int i = emu_strlen(msg), j = 0;
                while (n[j] && i < 44) msg[i++] = n[j++];
                msg[i++] = '\n'; msg[i] = '\0';
                emu_print(msg);
            }
            m68k_set_reg(M68K_REG_D0, 0);
            break;
        }
    }
}

/* =========================================================================
 * dos.library implementation
 * ========================================================================= */

/* (BPTR defines moved above install_library_tables — see near FAKE_PROCESS_ADDR) */

static void dos_Output(void)
{
    m68k_set_reg(M68K_REG_D0, DOS_STDOUT_BPTR);
}

static void dos_Input(void)
{
    m68k_set_reg(M68K_REG_D0, DOS_STDIN_BPTR);
}



static void dos_VFPrintf(void)
{
    /* D1=fh (BPTR), D2=format string ptr, D3=arg array ptr
     * For now print the format string raw (no substitution) */
    uint32_t ptr = m68k_get_reg(NULL, M68K_REG_D2);
    if (ptr < GUEST_RAM_SIZE) {
        char buf[512]; uint32_t i;
        for (i = 0; i < 511 && ptr+i < GUEST_RAM_SIZE && g_ram[ptr+i]; i++)
            buf[i] = (char)g_ram[ptr+i];
        buf[i] = '\0';
        emu_print(buf);
    }
    m68k_set_reg(M68K_REG_D0, 0);
}

static void dos_FPuts(void)
{
    /* D1=fh (BPTR), D2=string ptr (C string) */
    uint32_t ptr = m68k_get_reg(NULL, M68K_REG_D2);
    if (ptr < GUEST_RAM_SIZE) {
        char buf[512]; uint32_t i;
        for (i = 0; i < 511 && ptr+i < GUEST_RAM_SIZE && g_ram[ptr+i]; i++)
            buf[i] = (char)g_ram[ptr+i];
        buf[i] = '\0';
        emu_print(buf);
    }
    m68k_set_reg(M68K_REG_D0, 0);
}

static void dos_PutStr(void)
{
    /* D1=string ptr (C string) — output to stdout */
    uint32_t ptr = m68k_get_reg(NULL, M68K_REG_D1);
    if (ptr < GUEST_RAM_SIZE) {
        char buf[512]; uint32_t i;
        for (i = 0; i < 511 && ptr+i < GUEST_RAM_SIZE && g_ram[ptr+i]; i++)
            buf[i] = (char)g_ram[ptr+i];
        buf[i] = '\0';
        emu_print(buf);
    }
    m68k_set_reg(M68K_REG_D0, 0);
}

static void dos_VPrintf(void)
{
    /* D1=format string ptr, D2=arg array ptr — output to stdout */
    uint32_t ptr = m68k_get_reg(NULL, M68K_REG_D1);
    if (ptr < GUEST_RAM_SIZE) {
        char buf[512]; uint32_t i;
        for (i = 0; i < 511 && ptr+i < GUEST_RAM_SIZE && g_ram[ptr+i]; i++)
            buf[i] = (char)g_ram[ptr+i];
        buf[i] = '\0';
        emu_print(buf);
    }
    m68k_set_reg(M68K_REG_D0, 0);
}

/* Fake RDArgs struct returned by ReadArgs */
/* RDArgs buffer address now lives in g_guest_rdargs_addr (allocated post-load) */
#define FAKE_ARGARRAY_ADDR 0x10280  /* array of ULONG results from ReadArgs */

static void dos_VFWritef(void)
{
    /* D1=fh, D2=format BSTR ptr, D3=array ptr — like printf with %lx etc.
     * For now just print the format string raw */
    uint32_t fmt_bptr = m68k_get_reg(NULL, M68K_REG_D2);
    uint32_t fmt_ptr  = fmt_bptr << 2;
    if (fmt_ptr < GUEST_RAM_SIZE) {
        uint8_t blen = g_ram[fmt_ptr];
        char buf[512]; uint32_t i;
        for (i = 0; i < blen && i < 511 && fmt_ptr+1+i < GUEST_RAM_SIZE; i++)
            buf[i] = (char)g_ram[fmt_ptr+1+i];
        buf[i] = '\0';
        emu_print(buf);
    }
    m68k_set_reg(M68K_REG_D0, 0);
}

static void dos_ReadArgs(void)
{
    /* D1=template BSTR, D2=array ptr, D3=rdargs or 0
     * Returns non-NULL RDArgs handle; fills D2 array with parsed args. */
    /* Zero out the arg result array (D2) */
    uint32_t array_ptr = m68k_get_reg(NULL, M68K_REG_D2);
    if (array_ptr && array_ptr + 64 < GUEST_RAM_SIZE) {
        for (int i = 0; i < 64; i++) g_ram[array_ptr + i] = 0;
    }
    /* Return fake RDArgs struct (buffer allocated post-load) */
    if (g_guest_rdargs_addr) {
        for (int i = 0; i < 0x40; i++) g_ram[g_guest_rdargs_addr + i] = 0;
        m68k_set_reg(M68K_REG_D0, g_guest_rdargs_addr >> 2);
    } else {
        m68k_set_reg(M68K_REG_D0, 0);
    }
}

static void dos_GetArgStr(void)
{
    /* Returns BPTR to the CLI argument string (BSTR format) */
    m68k_set_reg(M68K_REG_D0, g_cmdline_bptr);
}

static void dos_IsInteractive(void)
{
    /* D1=fh — returns DOSTRUE (-1) for console handles */
    m68k_set_reg(M68K_REG_D0, (uint32_t)-1);
}

static void dos_Open(void)
{
    /* D1=name BPTR, D2=mode */
    uint32_t bptr = m68k_get_reg(NULL, M68K_REG_D1);
    char name[128];
    int blen = bstr_to_c(bptr, name, sizeof(name));
    if (blen == 0) {
        m68k_set_reg(M68K_REG_D0, DOS_STDOUT_BPTR);
        return;
    }

    /* Accept console-like names */
    if (name[0] == '*' ||
        (name[0]=='C' && name[1]=='O' && name[2]=='N') ||
        (name[0]=='N' && name[1]=='I' && name[2]=='L') ||
        (name[0]=='R' && name[1]=='A' && name[2]=='W') ||
        (name[0]=='A' && name[1]=='U' && name[2]=='X')) {
        m68k_set_reg(M68K_REG_D0, DOS_STDOUT_BPTR);
        return;
    }

    /* Build full path */
    char full_name[128];
    int has_device = 0;
    for (int i = 0; i < blen; i++) if (name[i] == ':') { has_device = 1; break; }
    if (has_device) {
        int i = 0; while (i < blen) { full_name[i] = name[i]; i++; }
        full_name[i] = '\0';
    } else {
        const char *cwd = m68k_cur_cwd();
        int cwd_len = 0; while (cwd[cwd_len] && cwd_len < 63) cwd_len++;
        int i = 0; while (i < cwd_len) { full_name[i] = cwd[i]; i++; }
        if (cwd_len > 0 && cwd[cwd_len-1] != ':' && cwd[cwd_len-1] != '/')
            full_name[i++] = '/';
        int j = 0; while (j < blen && i < 127) { full_name[i++] = name[j++]; }
        full_name[i] = '\0';
    }

    char vol_name[16];
    extract_vol_name(full_name, vol_name, sizeof(vol_name));
    MsgPort *port = VFS_GetHandlerPort(vol_name);
    if (!port) {
        m68k_set_reg(M68K_REG_D0, 0);
        SetIoErr(ERROR_DEVICE_NOT_MOUNTED);
        return;
    }

    uint32_t mode = m68k_get_reg(NULL, M68K_REG_D2);
    int32_t action = (mode == 1006) ? ACTION_FINDOUTPUT : ACTION_FINDINPUT;
    int32_t handle = DoPkt(port, action, (int32_t)(intptr_t)full_name, (int32_t)mode, 0, 0, 0);
    if (handle == 0) {
        m68k_set_reg(M68K_REG_D0, 0);
        SetIoErr(IoErr());
    } else {
        m68k_set_reg(M68K_REG_D0, (uint32_t)handle);
    }
}

static void dos_Close(void)
{
    uint32_t fh = m68k_get_reg(NULL, M68K_REG_D1);

    HandleEntry *ent = HandleTable_Get(fh);
    if (ent && ent->type == HTYPE_FILE && ent->u.file.fh.node) {
        VFS_Close(&ent->u.file.fh);
        ent->u.file.fh.node = NULL;
    }
    HandleTable_Free(fh);
    m68k_set_reg(M68K_REG_D0, (uint32_t)-1);
}

static void dos_Read(void)
{
    uint32_t fh  = m68k_get_reg(NULL, M68K_REG_D1);
    uint32_t buf = m68k_get_reg(NULL, M68K_REG_D2);
    uint32_t len = m68k_get_reg(NULL, M68K_REG_D3);

    HandleEntry *ent = HandleTable_Get(fh);
    if (!ent || ent->type != HTYPE_FILE || !ent->u.file.fh.node) {
        m68k_set_reg(M68K_REG_D0, (uint32_t)-1);
        return;
    }

    if (buf + len >= GUEST_RAM_SIZE) {
        m68k_set_reg(M68K_REG_D0, (uint32_t)-1);
        return;
    }

    uint32_t bytes = VFS_Read(&ent->u.file.fh, g_ram + buf, len);
    m68k_set_reg(M68K_REG_D0, bytes);
}

static void dos_Write(void)
{
    uint32_t fh  = m68k_get_reg(NULL, M68K_REG_D1);
    uint32_t buf = m68k_get_reg(NULL, M68K_REG_D2);
    uint32_t len = m68k_get_reg(NULL, M68K_REG_D3);

    /* Console handles */
    if (fh == DOS_STDOUT_BPTR || fh == DOS_STDIN_BPTR) {
        if (buf + len < GUEST_RAM_SIZE) {
            char tmp[4096];
            uint32_t i;
            for (i = 0; i < len && buf + i < GUEST_RAM_SIZE; i++)
                tmp[i] = (char)g_ram[buf + i];
            tmp[i] = '\0';
            emu_print(tmp);
            m68k_set_reg(M68K_REG_D0, len);
        } else {
            m68k_set_reg(M68K_REG_D0, (uint32_t)-1);
        }
        return;
    }

    HandleEntry *ent = HandleTable_Get(fh);
    if (!ent || ent->type != HTYPE_FILE || !ent->u.file.fh.node) {
        m68k_set_reg(M68K_REG_D0, (uint32_t)-1);
        return;
    }

    if (buf + len >= GUEST_RAM_SIZE) {
        m68k_set_reg(M68K_REG_D0, (uint32_t)-1);
        return;
    }

    uint32_t bytes = VFS_Write(&ent->u.file.fh, g_ram + buf, len);
    m68k_set_reg(M68K_REG_D0, bytes);
}

static void dos_Seek(void)
{
    uint32_t fh    = m68k_get_reg(NULL, M68K_REG_D1);
    int32_t  offset = (int32_t)m68k_get_reg(NULL, M68K_REG_D2);
    int32_t  mode   = (int32_t)m68k_get_reg(NULL, M68K_REG_D3);

    HandleEntry *ent = HandleTable_Get(fh);
    if (!ent || ent->type != HTYPE_FILE || !ent->u.file.fh.node) {
        m68k_set_reg(M68K_REG_D0, (uint32_t)-1);
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    uint32_t old_pos = ent->u.file.fh.pos;
    uint32_t size    = VFS_Size(&ent->u.file.fh);
    uint32_t new_pos = 0;
    if (mode == OFFSET_CURRENT)      new_pos = old_pos + (uint32_t)offset;
    else if (mode == OFFSET_END)     new_pos = size + (uint32_t)offset;
    else if (mode == OFFSET_BEGINNING) new_pos = (uint32_t)offset;
    else                               new_pos = (uint32_t)offset;
    VFS_Seek(&ent->u.file.fh, new_pos);
    m68k_set_reg(M68K_REG_D0, (uint32_t)old_pos);
}

static void dos_DeleteFile(void)
{
    uint32_t bptr = m68k_get_reg(NULL, M68K_REG_D1);
    char name[128];
    int blen = bstr_to_c(bptr, name, sizeof(name));
    if (blen == 0) {
        m68k_set_reg(M68K_REG_D0, 0);
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    char full_name[128];
    int has_device = 0;
    for (int i = 0; i < blen; i++) if (name[i] == ':') { has_device = 1; break; }
    if (has_device) {
        int i = 0; while (i < blen) { full_name[i] = name[i]; i++; }
        full_name[i] = '\0';
    } else {
        const char *cwd = m68k_cur_cwd();
        int cwd_len = 0; while (cwd[cwd_len] && cwd_len < 63) cwd_len++;
        int i = 0; while (i < cwd_len) { full_name[i] = cwd[i]; i++; }
        if (cwd_len > 0 && cwd[cwd_len-1] != ':' && cwd[cwd_len-1] != '/')
            full_name[i++] = '/';
        int j = 0; while (j < blen && i < 127) { full_name[i++] = name[j++]; }
        full_name[i] = '\0';
    }

    char vol_name[16];
    extract_vol_name(full_name, vol_name, sizeof(vol_name));
    MsgPort *port = VFS_GetHandlerPort(vol_name);
    if (!port) {
        m68k_set_reg(M68K_REG_D0, 0);
        SetIoErr(ERROR_DEVICE_NOT_MOUNTED);
        return;
    }

    int32_t res = DoPkt(port, ACTION_DELETE_OBJECT, (int32_t)(intptr_t)full_name, 0, 0, 0, 0);
    m68k_set_reg(M68K_REG_D0, (uint32_t)res);
}

static void dos_Rename(void)
{
    uint32_t old_bptr = m68k_get_reg(NULL, M68K_REG_D1);
    uint32_t new_bptr = m68k_get_reg(NULL, M68K_REG_D2);
    char old_name[128], new_name[128];
    bstr_to_c(old_bptr, old_name, sizeof(old_name));
    bstr_to_c(new_bptr, new_name, sizeof(new_name));

    char vol_name[16];
    extract_vol_name(old_name, vol_name, sizeof(vol_name));
    MsgPort *port = VFS_GetHandlerPort(vol_name);
    if (!port) {
        m68k_set_reg(M68K_REG_D0, 0);
        SetIoErr(ERROR_DEVICE_NOT_MOUNTED);
        return;
    }

    int32_t res = DoPkt(port, ACTION_RENAME_OBJECT,
                        (int32_t)(intptr_t)old_name, (int32_t)(intptr_t)new_name, 0, 0, 0);
    m68k_set_reg(M68K_REG_D0, (uint32_t)res);
}

static void dos_SetProtection(void)
{
    uint32_t bptr = m68k_get_reg(NULL, M68K_REG_D1);
    int32_t mask  = (int32_t)m68k_get_reg(NULL, M68K_REG_D2);
    char name[128];
    bstr_to_c(bptr, name, sizeof(name));

    char full_name[128];
    int has_device = 0;
    for (int i = 0; name[i]; i++) if (name[i] == ':') { has_device = 1; break; }
    if (has_device) {
        int i = 0; while (name[i]) { full_name[i] = name[i]; i++; }
        full_name[i] = '\0';
    } else {
        const char *cwd = m68k_cur_cwd();
        int cwd_len = 0; while (cwd[cwd_len] && cwd_len < 63) cwd_len++;
        int i = 0; while (i < cwd_len) { full_name[i] = cwd[i]; i++; }
        if (cwd_len > 0 && cwd[cwd_len-1] != ':' && cwd[cwd_len-1] != '/')
            full_name[i++] = '/';
        int j = 0; while (name[j] && i < 127) { full_name[i++] = name[j++]; }
        full_name[i] = '\0';
    }

    char vol_name[16];
    extract_vol_name(full_name, vol_name, sizeof(vol_name));
    MsgPort *port = VFS_GetHandlerPort(vol_name);
    if (!port) {
        m68k_set_reg(M68K_REG_D0, 0);
        SetIoErr(ERROR_DEVICE_NOT_MOUNTED);
        return;
    }

    int32_t res = DoPkt(port, ACTION_SET_PROTECT, (int32_t)(intptr_t)full_name, mask, 0, 0, 0);
    m68k_set_reg(M68K_REG_D0, (uint32_t)res);
}

static void dos_GetVar(void)
{
    m68k_set_reg(M68K_REG_D0, (uint32_t)-1);
    SetIoErr(ERROR_ACTION_NOT_KNOWN);
}

static void dos_SetVar(void)
{
    m68k_set_reg(M68K_REG_D0, 0);
    SetIoErr(ERROR_ACTION_NOT_KNOWN);
}

/* Stdin data to feed LHA: "?\n" to quit interactive mode cleanly */
static const char g_stdin_data[] = "?\n";
static int g_stdin_reads = 0;

static void dos_Lock(void)
{
    uint32_t bptr  = m68k_get_reg(NULL, M68K_REG_D1);
    int32_t  mode  = (int32_t)m68k_get_reg(NULL, M68K_REG_D2);
    char name[128];
    bstr_to_c(bptr, name, sizeof(name));

    char full_name[128];
    int has_device = 0;
    for (int i = 0; name[i]; i++) if (name[i] == ':') { has_device = 1; break; }
    if (has_device) {
        int i = 0; while (name[i]) { full_name[i] = name[i]; i++; }
        full_name[i] = '\0';
    } else {
        const char *cwd = m68k_cur_cwd();
        int cwd_len = 0; while (cwd[cwd_len] && cwd_len < 63) cwd_len++;
        int i = 0; while (i < cwd_len) { full_name[i] = cwd[i]; i++; }
        if (cwd_len > 0 && cwd[cwd_len-1] != ':' && cwd[cwd_len-1] != '/')
            full_name[i++] = '/';
        int j = 0; while (name[j] && i < 127) { full_name[i++] = name[j++]; }
        full_name[i] = '\0';
    }

    char vol_name[16];
    extract_vol_name(full_name, vol_name, sizeof(vol_name));
    MsgPort *port = VFS_GetHandlerPort(vol_name);
    if (!port) {
        m68k_set_reg(M68K_REG_D0, 0);
        SetIoErr(ERROR_DEVICE_NOT_MOUNTED);
        return;
    }

    int32_t handle = DoPkt(port, ACTION_LOCATE_OBJECT, (int32_t)(intptr_t)full_name, mode, 0, 0, 0);
    if (handle == 0) {
        m68k_set_reg(M68K_REG_D0, 0);
        SetIoErr(IoErr());
        return;
    }
    uint32_t lock_bptr = guest_alloc_filelock((uint32_t)handle, mode);
    if (lock_bptr == 0) {
        HandleTable_Free((uint32_t)handle);
        m68k_set_reg(M68K_REG_D0, 0);
        SetIoErr(ERROR_NO_FREE_STORE);
        return;
    }
    m68k_set_reg(M68K_REG_D0, lock_bptr);
}

static void dos_Unlock(void)
{
    uint32_t lock = m68k_get_reg(NULL, M68K_REG_D1);
    if (lock == 0) {
        m68k_set_reg(M68K_REG_D0, DOSTRUE);
        return;
    }
    uint32_t handle = 0;
    if (guest_read_filelock(lock, &handle, NULL) && handle != 0)
        HandleTable_Free(handle);
    m68k_set_reg(M68K_REG_D0, DOSTRUE);
}

static void dos_DupLock(void)
{
    uint32_t lock = m68k_get_reg(NULL, M68K_REG_D1);
    if (lock == 0) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }
    uint32_t handle = 0;
    int32_t access = 0;
    if (!guest_read_filelock(lock, &handle, &access) || handle == 0) {
        m68k_set_reg(M68K_REG_D0, 0);
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    HandleEntry *ent = HandleTable_Get(handle);
    if (!ent || ent->type != HTYPE_LOCK) {
        m68k_set_reg(M68K_REG_D0, 0);
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    char vol_name[16];
    extract_vol_name(ent->path, vol_name, sizeof(vol_name));
    MsgPort *port = VFS_GetHandlerPort(vol_name);
    if (!port) {
        m68k_set_reg(M68K_REG_D0, 0);
        SetIoErr(ERROR_DEVICE_NOT_MOUNTED);
        return;
    }

    int32_t dup = DoPkt(port, ACTION_COPY_DIR, (int32_t)handle, 0, 0, 0, 0);
    if (dup == 0) {
        m68k_set_reg(M68K_REG_D0, 0);
        SetIoErr(IoErr());
        return;
    }
    uint32_t dup_bptr = guest_alloc_filelock((uint32_t)dup, access);
    if (dup_bptr == 0) {
        HandleTable_Free((uint32_t)dup);
        m68k_set_reg(M68K_REG_D0, 0);
        SetIoErr(ERROR_NO_FREE_STORE);
        return;
    }
    m68k_set_reg(M68K_REG_D0, dup_bptr);
}

static void dos_Parent(void)
{
    uint32_t lock = m68k_get_reg(NULL, M68K_REG_D1);
    if (lock == 0) {
        m68k_set_reg(M68K_REG_D0, 0);
        return;
    }
    uint32_t handle = 0;
    int32_t access = 0;
    if (!guest_read_filelock(lock, &handle, &access) || handle == 0) {
        m68k_set_reg(M68K_REG_D0, 0);
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    HandleEntry *ent = HandleTable_Get(handle);
    if (!ent || ent->type != HTYPE_LOCK) {
        m68k_set_reg(M68K_REG_D0, 0);
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    char vol_name[16];
    extract_vol_name(ent->path, vol_name, sizeof(vol_name));
    MsgPort *port = VFS_GetHandlerPort(vol_name);
    if (!port) {
        m68k_set_reg(M68K_REG_D0, 0);
        SetIoErr(ERROR_DEVICE_NOT_MOUNTED);
        return;
    }

    int32_t ph = DoPkt(port, ACTION_PARENT, (int32_t)handle, 0, 0, 0, 0);
    if (ph == 0) {
        m68k_set_reg(M68K_REG_D0, 0);
        SetIoErr(IoErr());
        return;
    }
    uint32_t p_bptr = guest_alloc_filelock((uint32_t)ph, access);
    if (p_bptr == 0) {
        HandleTable_Free((uint32_t)ph);
        m68k_set_reg(M68K_REG_D0, 0);
        SetIoErr(ERROR_NO_FREE_STORE);
        return;
    }
    m68k_set_reg(M68K_REG_D0, p_bptr);
}

static void dos_Examine(void)
{
    uint32_t lock = m68k_get_reg(NULL, M68K_REG_D1);
    uint32_t fib_bptr = m68k_get_reg(NULL, M68K_REG_D2);
    uint32_t fib_ptr  = fib_bptr << 2;

    if (fib_ptr >= GUEST_RAM_SIZE) {
        m68k_set_reg(M68K_REG_D0, 0);
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    uint32_t handle = 0;
    if (!guest_read_filelock(lock, &handle, NULL) || handle == 0) {
        m68k_set_reg(M68K_REG_D0, 0);
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    HandleEntry *ent = HandleTable_Get(handle);
    if (!ent || ent->type != HTYPE_LOCK) {
        m68k_set_reg(M68K_REG_D0, 0);
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    char vol_name[16];
    extract_vol_name(ent->path, vol_name, sizeof(vol_name));
    MsgPort *port = VFS_GetHandlerPort(vol_name);
    if (!port) {
        m68k_set_reg(M68K_REG_D0, 0);
        SetIoErr(ERROR_DEVICE_NOT_MOUNTED);
        return;
    }

    int32_t res = DoPkt(port, ACTION_EXAMINE_OBJECT, (int32_t)handle,
                        (int32_t)(intptr_t)(g_ram + fib_ptr), 0, 0, 0);
    m68k_set_reg(M68K_REG_D0, (uint32_t)res);
}

static void dos_ExamineNext(void)
{
    uint32_t lock = m68k_get_reg(NULL, M68K_REG_D1);
    uint32_t fib_bptr = m68k_get_reg(NULL, M68K_REG_D2);
    uint32_t fib_ptr  = fib_bptr << 2;

    if (fib_ptr >= GUEST_RAM_SIZE) {
        m68k_set_reg(M68K_REG_D0, 0);
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    uint32_t handle = 0;
    if (!guest_read_filelock(lock, &handle, NULL) || handle == 0) {
        m68k_set_reg(M68K_REG_D0, 0);
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    HandleEntry *ent = HandleTable_Get(handle);
    if (!ent || ent->type != HTYPE_LOCK) {
        m68k_set_reg(M68K_REG_D0, 0);
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return;
    }

    char vol_name[16];
    extract_vol_name(ent->path, vol_name, sizeof(vol_name));
    MsgPort *port = VFS_GetHandlerPort(vol_name);
    if (!port) {
        m68k_set_reg(M68K_REG_D0, 0);
        SetIoErr(ERROR_DEVICE_NOT_MOUNTED);
        return;
    }

    int32_t res = DoPkt(port, ACTION_EXAMINE_NEXT, (int32_t)handle,
                        (int32_t)(intptr_t)(g_ram + fib_ptr), 0, 0, 0);
    m68k_set_reg(M68K_REG_D0, (uint32_t)res);
}

static void dos_CreateDir(void)
{
    uint32_t bptr = m68k_get_reg(NULL, M68K_REG_D1);
    char name[128];
    bstr_to_c(bptr, name, sizeof(name));

    char full_name[128];
    int has_device = 0;
    for (int i = 0; name[i]; i++) if (name[i] == ':') { has_device = 1; break; }
    if (has_device) {
        int i = 0; while (name[i]) { full_name[i] = name[i]; i++; }
        full_name[i] = '\0';
    } else {
        const char *cwd = m68k_cur_cwd();
        int cwd_len = 0; while (cwd[cwd_len] && cwd_len < 63) cwd_len++;
        int i = 0; while (i < cwd_len) { full_name[i] = cwd[i]; i++; }
        if (cwd_len > 0 && cwd[cwd_len-1] != ':' && cwd[cwd_len-1] != '/')
            full_name[i++] = '/';
        int j = 0; while (name[j] && i < 127) { full_name[i++] = name[j++]; }
        full_name[i] = '\0';
    }

    char vol_name[16];
    extract_vol_name(full_name, vol_name, sizeof(vol_name));
    MsgPort *port = VFS_GetHandlerPort(vol_name);
    if (!port) {
        m68k_set_reg(M68K_REG_D0, 0);
        SetIoErr(ERROR_DEVICE_NOT_MOUNTED);
        return;
    }

    int32_t res = DoPkt(port, ACTION_CREATE_DIR, (int32_t)(intptr_t)full_name, 0, 0, 0, 0);
    m68k_set_reg(M68K_REG_D0, (uint32_t)res);
}

static void dos_Exit(void)
{
    uint32_t rc = m68k_get_reg(NULL, M68K_REG_D1);
    /* Diagnostic: where did the program exit from?  The caller's return
     * address sits at (SP) — for the top-level RTS path it's our stub. */
    uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP);
    uint32_t ra = (sp + 4 <= GUEST_RAM_SIZE) ? guest_read_be32(sp) : 0;
    char msg[64] = "[dos] Exit rc=";
    char n[12]; u32_dec(rc, n, 12);
    int i = emu_strlen(msg), j = 0;
    while (n[j] && i < 40) msg[i++] = n[j++];
    const char *tail = " retpc=0x";
    j = 0; while (tail[j]) msg[i++] = tail[j++];
    u32_hex(ra, n); j = 0;
    while (n[j] && i < 60) msg[i++] = n[j++];
    msg[i++] = '\n'; msg[i] = '\0';
    emu_print(msg);
    /* Stop the execute loop */
    g_emu_halted = 1;
    m68k_end_timeslice();
}

static void dos_IoErr(void)
{
    m68k_set_reg(M68K_REG_D0, (uint32_t)IoErr());
}

/* =========================================================================
 * M68k Hook invocation helper
 * =========================================================================
 *
 * AmigaOS Hook layout (after the MinNode):
 *   +8  h_Entry
 *   +12 h_SubEntry
 *   +16 h_Data
 *
 * The standard calling convention used by Intuition backfill hooks is:
 *   A0 = struct Hook *       (the hook itself)
 *   A2 = APTR                (object: RastPort for backfill)
 *   A1 = APTR                (message: Rectangle for backfill)
 *
 * We support a small nesting depth so a hook may itself call library
 * functions that may eventually trigger another hook. */

#define HOOK_OFF_ENTRY      8
#define HOOK_OFF_SUBENTRY  12
#define HOOK_OFF_DATA      16

#define HOOK_RETURN_TRAP_ADDR 0x1EF000u
#define HOOK_RETURN_TRAP_LIB  0xFF
#define HOOK_RETURN_TRAP_FN   0xFF

#define MAX_NESTED_HOOKS 4

static uint8_t g_hook_saved_context[MAX_NESTED_HOOKS][1024];
static int     g_hook_nest_level = 0;
static uint8_t g_hook_return_detected[MAX_NESTED_HOOKS] = {0};

uint32_t UAOS_InvokeM68kHook(uint32_t hook_ptr, uint32_t a0, uint32_t a1, uint32_t a2)
{
    if (!hook_ptr || hook_ptr + HOOK_OFF_DATA + 4 > GUEST_RAM_SIZE) return 0;
    if (g_hook_nest_level >= MAX_NESTED_HOOKS) return 0;

    uint32_t entry = guest_read_be32(hook_ptr + HOOK_OFF_ENTRY);
    if (!entry) return 0;

    /* Save current CPU context for restoration after the hook returns. */
    unsigned int ctx_size = m68k_context_size();
    if (ctx_size > sizeof(g_hook_saved_context[0])) ctx_size = sizeof(g_hook_saved_context[0]);
    int level = g_hook_nest_level;
    m68k_get_context(g_hook_saved_context[level]);
    g_hook_return_detected[level] = 0;
    g_hook_nest_level++;

    /* Install the hook return trap (ILLEGAL + special dispatch word). */
    if (HOOK_RETURN_TRAP_ADDR + 4 <= GUEST_RAM_SIZE) {
        g_ram[HOOK_RETURN_TRAP_ADDR + 0] = 0x4A;
        g_ram[HOOK_RETURN_TRAP_ADDR + 1] = 0xFC;
        g_ram[HOOK_RETURN_TRAP_ADDR + 2] = HOOK_RETURN_TRAP_LIB;
        g_ram[HOOK_RETURN_TRAP_ADDR + 3] = HOOK_RETURN_TRAP_FN;
    }

    /* Push return address onto the M68k stack. */
    uint32_t sp = m68k_get_reg(NULL, M68K_REG_A7);
    sp -= 4;
    if (sp + 4 <= GUEST_RAM_SIZE) {
        guest_write_be32(sp, HOOK_RETURN_TRAP_ADDR);
    }
    m68k_set_reg(M68K_REG_A7, sp);

    /* Set up the hook arguments and jump to the entry point. */
    m68k_set_reg(M68K_REG_A0, a0);
    m68k_set_reg(M68K_REG_A1, a1);
    m68k_set_reg(M68K_REG_A2, a2);
    m68k_set_reg(M68K_REG_PC, entry);

    g_m68k_wild_abort = 0;
    while (!g_hook_return_detected[level] && !g_m68k_wild_abort) {
        m68k_execute(1000);
        Chiptrace_PcSample();
    }

    uint32_t d0 = m68k_get_reg(NULL, M68K_REG_D0);

    /* Restore the saved CPU context and return the hook's D0. */
    g_hook_nest_level--;
    m68k_set_context(g_hook_saved_context[level]);

    return d0;
}

/* Call a guest interrupt handler: JSR-style entry that returns via RTS to
 * the shared return trap.  Same nest-level machinery as hooks — handlers
 * may themselves invoke library calls (which may nest further). */
static uint32_t m68k_isr_call_a0(uint32_t entry, uint32_t d0, uint32_t a1,
                                 uint32_t a5, uint32_t a0)
{
    if (!entry || entry >= GUEST_RAM_SIZE) return 0;
    if (g_hook_nest_level >= MAX_NESTED_HOOKS) return 0;

    unsigned int ctx_size = m68k_context_size();
    if (ctx_size > sizeof(g_hook_saved_context[0])) ctx_size = sizeof(g_hook_saved_context[0]);
    int level = g_hook_nest_level;
    m68k_get_context(g_hook_saved_context[level]);
    g_hook_return_detected[level] = 0;
    g_hook_nest_level++;

    if (HOOK_RETURN_TRAP_ADDR + 4 <= GUEST_RAM_SIZE) {
        g_ram[HOOK_RETURN_TRAP_ADDR + 0] = 0x4A;
        g_ram[HOOK_RETURN_TRAP_ADDR + 1] = 0xFC;
        g_ram[HOOK_RETURN_TRAP_ADDR + 2] = HOOK_RETURN_TRAP_LIB;
        g_ram[HOOK_RETURN_TRAP_ADDR + 3] = HOOK_RETURN_TRAP_FN;
    }

    uint32_t sp = m68k_get_reg(NULL, M68K_REG_A7);
    sp -= 4;
    if (sp + 4 <= GUEST_RAM_SIZE) {
        guest_write_be32(sp, HOOK_RETURN_TRAP_ADDR);
    }
    m68k_set_reg(M68K_REG_A7, sp);

    /* Amiga interrupt server convention: D0=pending INTREQ bit,
     * A0=$DFF000 custom regs (or CIA base for cia.resource ICR vectors),
     * A1=is_Data, A5=IntVector addr, A6=SysBase. */
    m68k_set_reg(M68K_REG_D0, d0);
    m68k_set_reg(M68K_REG_A0, a0);
    m68k_set_reg(M68K_REG_A1, a1);
    m68k_set_reg(M68K_REG_A5, a5);
    m68k_set_reg(M68K_REG_A6, EXEC_BASE);
    m68k_set_reg(M68K_REG_PC, entry);

    /* Run until RTS hits the return trap — bounded so a wedged handler
     * cannot hang the host task (~4M cycles ≈ 0.5 s of guest time).  The
     * wild-PC breaker (set by the instr hook when the handler branches out
     * of the 16 MB window) aborts far sooner — before the runaway CPU can
     * shred the arena executing data as code. */
    uint32_t guard = 0;
    g_m68k_wild_abort = 0;
    while (!g_hook_return_detected[level] && guard++ < 4000 &&
           !g_m68k_wild_abort)
        m68k_execute(1000);

    uint32_t ret = (uint32_t)m68k_get_reg(NULL, M68K_REG_D0);
    g_hook_nest_level--;
    m68k_set_context(g_hook_saved_context[level]);
    return ret;
}

static uint32_t m68k_isr_call(uint32_t entry, uint32_t d0, uint32_t a1,
                              uint32_t a5)
{
    return m68k_isr_call_a0(entry, d0, a1, a5, 0x00DFF000u);
}

/* Call a guest character-output callback (exec RawDoFmt PutChProc):
 * convention is D0.B = character, A3 = PutChData, A6 = SysBase.
 * Same nest machinery as hooks — the callback may invoke library calls. */
static void m68k_putch_call(uint32_t proc, uint8_t ch, uint32_t data)
{
    if (!proc || proc >= GUEST_RAM_SIZE) return;
    if (g_hook_nest_level >= MAX_NESTED_HOOKS) return;

    unsigned int ctx_size = m68k_context_size();
    if (ctx_size > sizeof(g_hook_saved_context[0])) ctx_size = sizeof(g_hook_saved_context[0]);
    int level = g_hook_nest_level;
    m68k_get_context(g_hook_saved_context[level]);
    g_hook_return_detected[level] = 0;
    g_hook_nest_level++;

    if (HOOK_RETURN_TRAP_ADDR + 4 <= GUEST_RAM_SIZE) {
        g_ram[HOOK_RETURN_TRAP_ADDR + 0] = 0x4A;
        g_ram[HOOK_RETURN_TRAP_ADDR + 1] = 0xFC;
        g_ram[HOOK_RETURN_TRAP_ADDR + 2] = HOOK_RETURN_TRAP_LIB;
        g_ram[HOOK_RETURN_TRAP_ADDR + 3] = HOOK_RETURN_TRAP_FN;
    }

    uint32_t sp = m68k_get_reg(NULL, M68K_REG_A7);
    sp -= 4;
    guest_write_be32(sp, HOOK_RETURN_TRAP_ADDR);
    m68k_set_reg(M68K_REG_A7, sp);

    m68k_set_reg(M68K_REG_D0, ch);
    m68k_set_reg(M68K_REG_A3, data);
    m68k_set_reg(M68K_REG_A6, EXEC_BASE);
    m68k_set_reg(M68K_REG_PC, proc);

    uint32_t guard = 0;
    g_m68k_wild_abort = 0;
    while (!g_hook_return_detected[level] && guard++ < 4000 &&
           !g_m68k_wild_abort)
        m68k_execute(1000);

    g_hook_nest_level--;
    m68k_set_context(g_hook_saved_context[level]);
}

/* Poll chipset interrupt state and invoke the guest's installed exec
 * Interrupt structures.  Called between m68k_execute() slices in
 * exec_task.c and from the blocking-wait paths (exec_Wait et al.) so
 * handlers fire even while the guest task is descheduled; the real
 * m68k_set_irq() autovector path services the same IntVects while the
 * guest is executing — both routes funnel through the helpers below. */
uint32_t g_dlv_calls = 0;   /* UAOS-241 diag: deliveries attempted */
uint32_t g_dlv_pend  = 0;   /* last pending mask seen */
uint32_t g_dlv_isr   = 0;   /* guest handlers actually invoked */
uint32_t g_blocked_in = 0;  /* 0=running 1=Wait 2=WaitPort 3=WaitIO */

/* INTREQ bit -> interrupt level (real Amiga mapping):
 *   TBE/DSKBLK/SOFTINT (0-2) -> L1    PORTS/CIAA (3)     -> L2
 *   COPER/VERTB/BLIT   (4-6) -> L3    AUD0-3     (7-10)  -> L4
 *   RBF/DSKSYN        (11-12)-> L5    EXTER/CIAB (13-14) -> L6 */
static const uint8_t g_irq_bit_level[15] = {
    1, 1, 1,  2,  3, 3, 3,  4, 4, 4, 4,  5, 5,  6, 6
};

/* Enabled-and-requested INTREQ bits, with the CIAB line synthesized into
 * the EXTER bit (real hardware has no EXTER register bit — the CIA pulls
 * the level-6 line directly). */
static uint16_t irq_pending_bits(void)
{
    extern uint16_t g_intreq;   /* chip_emu.c — requests, incl. unmasked */
    uint16_t intena = chip_emu_intena_shadow();
    uint16_t pend = (uint16_t)(g_intreq & intena & 0x7FFFu);
    if (chip_emu_cia_b_pending() && (intena & 0x2000u)) pend |= 0x2000u;
    return pend;
}

static int guest_ipl(void)
{
    return (int)((m68k_get_reg(NULL, M68K_REG_SR) >> 8) & 7);
}

/* Invoke the exec Interrupt structures registered for INTREQ bit n:
 * chain dispatch for AddIntServer slots, direct call for SetIntVector. */
static void deliver_intvect_bit(int n)
{
    static uint32_t s_irq_seen = 0;
    uint16_t bit  = (uint16_t)(1u << n);
    uint32_t iv   = EB_INTVECTS + (uint32_t)n * 12u;
    uint32_t code = guest_read_be32(iv + IV_CODE);
    uint32_t node = guest_read_be32(iv + IV_NODE);
    if (code == IV_CHAIN) {
        /* Server chain: each handler gets a shot until one claims it
         * (returns non-zero in D0), per exec's server dispatch. */
        int hops = 0;
        for (uint32_t in = node; in && in + 22 <= GUEST_RAM_SIZE && hops++ < 16;
             in = guest_read_be32(in)) {
            uint32_t ic = guest_read_be32(in + IS_CODE);
            if (ic) g_dlv_isr++;
            if (ic && m68k_isr_call(ic, bit,
                                    guest_read_be32(in + IS_DATA), iv))
                break;
        }
    } else if (code && code < GUEST_RAM_SIZE) {
        if (!(s_irq_seen & bit)) {
            s_irq_seen |= bit;
            char m[48]; int i2 = 0;
            const char *t2 = "[irq] deliver vec=";
            while (t2[i2]) { m[i2] = t2[i2]; i2++; }
            char n8[12]; u32_dec((uint32_t)n, n8, 12); int j2 = 0;
            while (n8[j2] && i2 < 40) m[i2++] = n8[j2++];
            const char *t3 = " code=0x"; j2 = 0; while (t3[j2]) m[i2++] = t3[j2++];
            u32_hex(code, n8); j2 = 0; while (n8[j2] && i2 < 44) m[i2++] = n8[j2++];
            m[i2++]='\n'; m[i2]='\0'; emu_print(m);
        }
        g_dlv_isr++;
        m68k_isr_call(code, bit, guest_read_be32(iv + IV_DATA), iv);
    }
    /* An unacknowledged bit stays set in g_intreq and is redelivered on
     * the next delivery round — matching level-triggered hardware. */
}

/* Dispatch pending+enabled ICR bits for this CIA to the Interrupt
 * structures installed via AddICRVector on its cia*.resource.  Entries are
 * window-scoped — only this task's guest RAM is consulted.  We ack each
 * delivered bit so it doesn't refire continuously (the real PORTS/EXTER
 * servers do the same by reading ICR). */
static void deliver_cia_icr(int cia)
{
    for (int i = 0; i < g_genlib_count; i++) {
        if (g_genlibs[i].cia != cia || !g_genlibs[i].icr_tab) continue;
        if (g_genlibs[i].ram != g_ram) continue;
        uint8_t icr = chip_emu_cia_icr_pending(cia);
        if (!icr) continue;
        for (int b = 0; b < 8; b++) {
            if (!(icr & (1u << b))) continue;
            uint32_t in = guest_read_be32(g_genlibs[i].icr_tab + (uint32_t)b * 4u);
            if (!in) continue;
            chip_emu_cia_icr_ack(cia, (uint8_t)(1u << b));
            uint32_t ic = guest_read_be32(in + IS_CODE);
            if (ic && in + 22 <= GUEST_RAM_SIZE) {
                static uint16_t s_icr_seen = 0;
                if (!(s_icr_seen & (1u << b))) {
                    s_icr_seen |= (uint16_t)(1u << b);
                    char m[48]; int i2 = 0;
                    const char *t2 = "[icr] cia"; while (t2[i2]) { m[i2]=t2[i2]; i2++; }
                    m[i2++] = (char)('a' + cia);
                    const char *t3 = " bit="; int j2 = 0; while (t3[j2]) m[i2++]=t3[j2++];
                    char n8[8]; u32_dec((uint32_t)b, n8, 8); j2 = 0;
                    while (n8[j2] && i2 < 44) m[i2++] = n8[j2++];
                    m[i2++]='\n'; m[i2]='\0'; emu_print(m);
                }
                /* ICR handler convention: A0 = CIA chip base, A1 = is_Data. */
                g_dlv_isr++;
                m68k_isr_call_a0(ic, (uint32_t)b,
                                 guest_read_be32(in + IS_DATA), 0,
                                 cia ? 0x00BFD000u : 0x00BFE001u);
            }
        }
    }
}

void UAOS_M68k_DeliverInterrupts(void)
{
    g_dlv_calls++;
    if (!g_ram) return;

    uint16_t pend = irq_pending_bits();
    if (pend) g_dlv_pend = pend;

    /* Honour guest IPL masking like the real 68k: a source only preempts
     * while its level is above the SR interrupt mask. */
    int ipl = guest_ipl();
    for (int n = 0; n < 15 && pend; n++) {
        uint16_t bit = (uint16_t)(1u << n);
        if (!(pend & bit)) continue;
        pend = (uint16_t)(pend & ~bit);
        if (g_irq_bit_level[n] <= ipl) continue;
        deliver_intvect_bit(n);
    }

    /* cia*.resource ICR vectors — the resource's servers are called by the
     * level's interrupt chain on real hardware; drive them here too so a
     * guest that never installs an exec server still gets ICR delivery. */
    if (ipl < 2) deliver_cia_icr(0);   /* CIAA -> PORTS level 2 */
    if (ipl < 6) deliver_cia_icr(1);   /* CIAB -> EXTER level 6 */
}

/* Autovector dispatch — entered via the 4-byte (LIB_IRQ,level) ILLEGAL
 * stub after Musashi takes a real interrupt exception: supervisor mode,
 * SR.IPL = level, and a 68020 format-0 frame [SR:w][PC:l][fmt:w] pushed on
 * the supervisor stack.  Runs the level's pending sources, then pops the
 * frame — an emulated RTE back to the interrupted code (UAOS-241). */
uint32_t g_dlv_vec = 0;   /* UAOS-241 diag: real autovector deliveries */

static void irq_vector_entry(int level)
{
    g_dlv_vec++;
    uint16_t pend = irq_pending_bits();
    for (int n = 0; n < 15; n++) {
        if ((pend & (1u << n)) && g_irq_bit_level[n] == level)
            deliver_intvect_bit(n);
    }
    if (level == 2) deliver_cia_icr(0);
    if (level == 6) deliver_cia_icr(1);

    /* Emulated RTE: restore the interrupted SR/PC from the exception frame. */
    uint32_t sp = (uint32_t)m68k_get_reg(NULL, M68K_REG_SP);
    if (sp + 8 <= GUEST_RAM_SIZE) {
        uint16_t osr = guest_read_be16(sp);
        uint32_t opc = guest_read_be32(sp + 2);
        /* guest_read_be16(sp + 6) = format/vector word — format 0 expected */
        m68k_set_reg(M68K_REG_SP, sp + 8);
        m68k_set_reg(M68K_REG_PC, opc);
        m68k_set_reg(M68K_REG_SR, osr);  /* restores pre-exception IPL */
    }
}

/* Stub for 68881 PMMU ops — referenced by generated m68kops.c when
 * 68020 emulation is enabled, but PMMU is disabled in uaos_m68kconf.h.
 * This stub satisfies the linker; it should never be called at runtime. */
void m68881_mmu_ops(void) { }

/* Copy a region of the shared boot-time guest RAM into the current task's
 * guest RAM.  Structures registered at boot (e.g. BOOPSI class objects on
 * the intuition heap) hold guest addresses that are only valid in
 * g_default_ram; per-task M68k tasks resolve the same numeric addresses
 * through their own g_ram, so the image must be mirrored for class lookups
 * and dispatches to work in a task context.  No-op on the shared path. */
void UAOS_Emu_MirrorSharedRegion(uint32_t off, uint32_t len)
{
    if (g_ram == g_shared_ram) return;
    if (off + len <= GUEST_RAM_SIZE)
        emu_memcpy(g_ram + off, g_shared_ram + off, len);
}

/* =========================================================================
 * ILLEGAL opcode callback — dispatches library calls
 * ========================================================================= */

/* A 0x4AFC outside the regions where we (or the guest) install dispatch
 * stubs is data executed by a wandering CPU — decoding its trailing bytes
 * as (lib,fn) can dispatch a REAL host function (FreeMem/CopyMem/...) with
 * garbage registers and shred the guest arena.  Refuse to dispatch; just
 * skip the word (behaves like the old unknown-lib no-op). */
static int is_stub_addr(uint32_t a)
{
    if (a >= EXEC_BASE - 996 && a < EXEC_BASE) return 1; /* exec jump table */
    if (a >= DOS_BASE - 996  && a < DOS_BASE)  return 1; /* dos jump table  */
    if (a >= 0x2000  && a < 0xC000)   return 1;  /* bsd/gfx/intui/gadtools/loadables */
    if (a >= 0x1EF000u && a < 0x1EF080u) return 1; /* hook trap + IRQ stubs */
    if (a >= 0x7F0000u && a < 0x800000u) return 1; /* genlib stub arena      */
    if (a >= 0xE000  && a < 0x10000)  return 1;  /* fake-lib band           */
    return 0;
}

int m68k_illg_instr_callback(int opcode)
{
    /* We only intercept 0x4AFC (ILLEGAL) — opcode is the full 16-bit word */
    if (opcode != 0x4AFC) return 0;

    /* The two bytes after ILLEGAL hold (lib_id, func_idx) */
    uint32_t pc  = m68k_get_reg(NULL, M68K_REG_PC);
    uint8_t  lib = g_ram[pc];     /* pc already advanced past ILLEGAL word */
    uint8_t  fn  = g_ram[pc + 1];

    if (!is_stub_addr(pc - 2)) {
        static uint32_t stray_count = 0;
        if (++stray_count <= 20 || (stray_count % 100000) == 0) {
            extern void kprint(const char *);
            char msg[64] = "[emu] stray ILLEGAL pc=";
            char n[12]; u32_hex(pc - 2, n);
            int i = emu_strlen(msg), j = 0;
            while (n[j] && i < 60) msg[i++] = n[j++];
            msg[i++]='\n'; msg[i]='\0';
            kprint(msg);
        }
        m68k_set_reg(M68K_REG_PC, pc + 2);
        return 1;
    }

    /* Diagnostic: trace library calls to diagnose hangs */
    {
        static uint32_t g_thunk_count = 0;
        g_thunk_count++;
        /* Print first 50 calls, then every 1000th, and always on lib/fn change to unknown */
        if (g_thunk_count <= 600 || (g_thunk_count % 10000) == 0) {
            char buf[80] = "[trace] #";
            char n[12]; u32_dec(g_thunk_count, n, 12);
            int i = emu_strlen(buf), j = 0;
            while (n[j] && i < 76) buf[i++] = n[j++];
            buf[i++]=' '; buf[i++]='l'; buf[i++]='i'; buf[i++]='b';
            buf[i++]='='; u32_dec(lib, n, 12); j=0; while (n[j]&&i<76) buf[i++]=n[j++];
            buf[i++]=' '; buf[i++]='f'; buf[i++]='n'; buf[i++]='=';
            u32_dec(fn, n, 12); j=0; while (n[j]&&i<76) buf[i++]=n[j++];
            /* args d0/d1 — shows what the caller asked for */
            {
                const char *t = " d0=0x"; j = 0; while (t[j]) buf[i++] = t[j++];
                u32_hex((uint32_t)m68k_get_reg(NULL, M68K_REG_D0), n);
                j = 0; while (n[j] && i < 78) buf[i++] = n[j++];
                t = " d1=0x"; j = 0; while (t[j]) buf[i++] = t[j++];
                u32_hex((uint32_t)m68k_get_reg(NULL, M68K_REG_D1), n);
                j = 0; while (n[j] && i < 78) buf[i++] = n[j++];
            }
            /* caller return address = *(SP) — shows who invoked the stub */
            uint32_t tsp = (uint32_t)m68k_get_reg(NULL, M68K_REG_SP);
            uint32_t ra = (tsp + 4 <= GUEST_RAM_SIZE) ? guest_read_be32(tsp) : 0;
            const char *t = " from=0x"; j = 0; while (t[j]) buf[i++] = t[j++];
            u32_hex(ra, n); j = 0; while (n[j] && i < 78) buf[i++] = n[j++];
            buf[i++]='\n'; buf[i]='\0';
            emu_print(buf);
        }
    }

    /* Advance PC past the 2-byte dispatch word */
    m68k_set_reg(M68K_REG_PC, pc + 2);

    int trace_this = 0;
    {
        static uint32_t g_thunk_traced = 0;
        if (g_thunk_traced < 700) { g_thunk_traced++; trace_this = 1; }
    }

    if (lib == HOOK_RETURN_TRAP_LIB && fn == HOOK_RETURN_TRAP_FN) {
        /* Hook return trap: the m68k hook has RTS'd back to the stub we
         * pushed.  Stop execution so UAOS_InvokeM68kHook() can restore the
         * saved caller context and return the hook's D0.  Use the current
         * nest level to index the per-level completion flag. */
        int level = g_hook_nest_level - 1;
        if (level >= 0 && level < MAX_NESTED_HOOKS)
            g_hook_return_detected[level] = 1;
        m68k_end_timeslice();
        return 1;
    }

    if (lib == LIB_IRQ) {
        /* Autovector stub — fn carries the interrupt level (1-7).  Musashi
         * already pushed the 68020 exception frame; dispatch the level's
         * sources and emulate RTE to resume the interrupted code. */
        irq_vector_entry(fn);
        return 1;
    }

    /* strace: emit the entry record (klog/ring+UART) while tracing */
    int strace_on = Strace_IsEnabled();
    M68kCPUState scpu;
    if (strace_on) {
        for (int i = 0; i < 8; i++) {
            scpu.d[i] = (uint32_t)m68k_get_reg(NULL, M68K_REG_D0 + i);
            scpu.a[i] = (uint32_t)m68k_get_reg(NULL, M68K_REG_A0 + i);
        }
        scpu.pc = (uint32_t)m68k_get_reg(NULL, M68K_REG_PC);
        scpu.sr = (uint16_t)m68k_get_reg(NULL, M68K_REG_SR);
        Strace_M68kEntry(lib, fn, &scpu);
    }

    if (lib == LIB_EXEC) {
        switch (fn) {
            case EXEC_OPEN_LIBRARY:  exec_OpenLibrary();  break;
            case EXEC_CLOSE_LIBRARY: exec_CloseLibrary(); break;
            case EXEC_ALLOC_MEM:     exec_AllocMem();     break;
            case EXEC_FREE_MEM:      exec_FreeMem();      break;
            case EXEC_FIND_TASK:     exec_FindTask();     break;
            case EXEC_WAIT:          exec_Wait();         break;
            case EXEC_SIGNAL:        exec_Signal();       break;
            case EXEC_SETSIGNAL:     exec_SetSignal();    break;
            case EXEC_ALLOC_SIGNAL:  exec_AllocSignal();  break;
            case EXEC_FREE_SIGNAL:   exec_FreeSignal();   break;
            case EXEC_PUT_MSG:       exec_PutMsg();       break;
            case EXEC_GET_MSG:       exec_GetMsg();       break;
            case EXEC_REPLY_MSG:     exec_ReplyMsg();     break;
            case EXEC_WAIT_PORT:     exec_WaitPort();     break;
            case EXEC_CACHE_CLEAR_U: break;               /* no icache */
            case EXEC_INIT_STRUCT:   exec_InitStruct();   break;
            case EXEC_DISABLE:       exec_Disable();      break;
            case EXEC_ENABLE:        exec_Enable();       break;
            case EXEC_FORBID:        exec_Forbid();       break;
            case EXEC_PERMIT:        exec_Permit();       break;
            case EXEC_SUPER_STATE:   exec_SuperState();   break;
            case EXEC_USER_STATE:    exec_UserState();    break;
            case EXEC_SET_INT_VECTOR: exec_SetIntVector(); break;
            case EXEC_ADD_INT_SERVER: exec_AddIntServer(); break;
            case EXEC_REM_INT_SERVER: exec_RemIntServer(); break;
            case EXEC_CAUSE:          exec_Cause();        break;
            case EXEC_AVAIL_MEM:     exec_AvailMem();     break;
            case EXEC_ALLOC_ENTRY:   exec_AllocEntry();   break;
            case EXEC_FREE_ENTRY:    exec_FreeEntry();    break;
            case EXEC_INSERT:        exec_Insert();       break;
            case EXEC_ADD_HEAD:      exec_AddHead();      break;
            case EXEC_ADD_TAIL:      exec_AddTail_glue(); break;
            case EXEC_REMOVE:        exec_Remove();       break;
            case EXEC_REM_HEAD:      exec_RemHead();      break;
            case EXEC_REM_TAIL:      exec_RemTail();      break;
            case EXEC_ENQUEUE:       exec_Enqueue();      break;
            case EXEC_FIND_NAME:     exec_FindName();     break;
            case EXEC_SET_TASK_PRI:  exec_SetTaskPri();   break;
            case EXEC_SET_EXCEPT:    exec_SetExcept();    break;
            case EXEC_ALLOC_TRAP:    exec_AllocTrap();    break;
            case EXEC_FREE_TRAP:     exec_FreeTrap();     break;
            case EXEC_ADD_PORT:      exec_AddPort();      break;
            case EXEC_REM_PORT:      exec_RemPort();      break;
            case EXEC_FIND_PORT:     exec_FindPort();     break;
            case EXEC_OLD_OPEN_LIBRARY:
                m68k_set_reg(M68K_REG_D0, 0);
                exec_OpenLibrary(); break;
            case EXEC_SET_FUNCTION:  exec_SetFunction();  break;
            case EXEC_OPEN_DEVICE:   exec_OpenDevice();   break;
            case EXEC_CLOSE_DEVICE:  exec_CloseDevice();  break;
            case EXEC_DO_IO:         exec_DoIO();         break;
            case EXEC_SEND_IO:       exec_SendIO();       break;
            case EXEC_CHECK_IO:      exec_CheckIO();      break;
            case EXEC_WAIT_IO:       exec_WaitIO();       break;
            case EXEC_ABORT_IO:      exec_AbortIO();      break;
            case EXEC_OPEN_RESOURCE: exec_OpenResource(); break;
            case EXEC_GETCC:         exec_GetCC();        break;
            case EXEC_TYPE_OF_MEM:   exec_TypeOfMem();    break;
            case EXEC_PROCURE:       exec_Procure();      break;
            case EXEC_VACATE:        exec_Vacate();       break;
            case EXEC_INIT_SEMAPHORE:    exec_InitSemaphore(); break;
            case EXEC_OBTAIN_SEM:  exec_ObtainSem();     break;
            case EXEC_RELEASE_SEM: exec_ReleaseSem();   break;
            case EXEC_ATTEMPT_SEM: exec_AttemptSem();   break;
            case EXEC_OBTAIN_SEM_SHARED: exec_ObtainSem();    break;
            case EXEC_OBTAIN_SEM_LIST:   exec_ObtainSemList(); break;
            case EXEC_RELEASE_SEM_LIST:  exec_ReleaseSemList(); break;
            case EXEC_FIND_SEMAPHORE:    exec_FindSemaphore(); break;
            case EXEC_ADD_SEMAPHORE:     exec_AddSemaphore();  break;
            case EXEC_REM_SEMAPHORE:     exec_RemSemaphore();  break;
            case EXEC_COPY_MEM:       exec_CopyMem(0);    break;
            case EXEC_COPY_MEM_QUICK: exec_CopyMem(1);    break;
            case EXEC_CACHE_CONTROL:  exec_CacheControl(); break;
            case EXEC_CACHE_CLEAR_E:  break;              /* no icache */
            case EXEC_RAW_DO_FMT:     exec_RawDoFmt();    break;
            case EXEC_CREATE_IOREQUEST: exec_CreateIORequest(); break;
            case EXEC_DELETE_IOREQUEST: exec_DeleteIORequest(); break;
            case EXEC_CREATE_MSGPORT:   exec_CreateMsgPort();  break;
            case EXEC_DELETE_MSGPORT:   exec_DeleteMsgPort();  break;
            case EXEC_ALLOC_VEC:      exec_AllocVec();    break;
            case EXEC_FREE_VEC:       exec_FreeVec();     break;
            case EXEC_STACK_SWAP:     exec_StackSwap();   break;
            default: {
                /* PC was advanced to stub+4 → recover the LVO hit. */
                uint32_t spc = m68k_get_reg(NULL, M68K_REG_PC);
                int32_t lvo = (int32_t)spc - 4 - (int32_t)EXEC_BASE;
                /* Rate-limit: suppress consecutive repeats + global cap so
                 * Forbid/Permit-style loops don't flood the UART. */
                static int32_t last_lvo = 0;
                static int unimpl_prints = 0;
                if (lvo != last_lvo && unimpl_prints < 100) {
                    last_lvo = lvo;
                    unimpl_prints++;
                    char msg[48] = "[exec] unimpl lvo=";
                    char n[12]; u32_dec((uint32_t)(-lvo), n, 12);
                    int i = emu_strlen(msg), j = 0;
                    msg[i++]='-'; while (n[j] && i<40) msg[i++]=n[j++];
                    j = 0; const char *tail = " fn=";
                    while (tail[j]) msg[i++]=tail[j++];
                    u32_dec(fn, n, 12); j=0;
                    while (n[j] && i<44) msg[i++]=n[j++];
                    msg[i++]='\n'; msg[i]='\0';
                    emu_print(msg);
                }
                m68k_set_reg(M68K_REG_D0, 0);   /* safe default */
            }
        }
    } else if (lib == LIB_DOS || lib == LIB_UTILITY) {
        /* Delegate to ROM module dispatcher — marshal all regs */
        M68kCPUState cpu;
        cpu.d[0] = m68k_get_reg(NULL, M68K_REG_D0);
        cpu.d[1] = m68k_get_reg(NULL, M68K_REG_D1);
        cpu.d[2] = m68k_get_reg(NULL, M68K_REG_D2);
        cpu.d[3] = m68k_get_reg(NULL, M68K_REG_D3);
        cpu.d[4] = m68k_get_reg(NULL, M68K_REG_D4);
        cpu.d[5] = m68k_get_reg(NULL, M68K_REG_D5);
        cpu.d[6] = m68k_get_reg(NULL, M68K_REG_D6);
        cpu.d[7] = m68k_get_reg(NULL, M68K_REG_D7);
        cpu.a[0] = m68k_get_reg(NULL, M68K_REG_A0);
        cpu.a[1] = m68k_get_reg(NULL, M68K_REG_A1);
        cpu.a[2] = m68k_get_reg(NULL, M68K_REG_A2);
        cpu.a[3] = m68k_get_reg(NULL, M68K_REG_A3);
        cpu.a[4] = m68k_get_reg(NULL, M68K_REG_A4);
        cpu.a[5] = m68k_get_reg(NULL, M68K_REG_A5);
        cpu.a[6] = m68k_get_reg(NULL, M68K_REG_A6);
        cpu.a[7] = m68k_get_reg(NULL, M68K_REG_A7);
        cpu.pc   = m68k_get_reg(NULL, M68K_REG_PC);
        cpu.sr   = (uint16_t)m68k_get_reg(NULL, M68K_REG_SR);

        void *rom_fn = UAOS_ROM_NativeFunc(
            lib == LIB_DOS ? "dos.library" : "utility.library", (uint16_t)fn);
        if (rom_fn) {
            void (*fn_ptr)(M68kCPUState *) = (void (*)(M68kCPUState *))rom_fn;
            fn_ptr(&cpu);
            /* Write back registers that may have been modified */
            m68k_set_reg(M68K_REG_D0, cpu.d[0]);
            m68k_set_reg(M68K_REG_D1, cpu.d[1]);
            m68k_set_reg(M68K_REG_D2, cpu.d[2]);
            m68k_set_reg(M68K_REG_D3, cpu.d[3]);
            m68k_set_reg(M68K_REG_D4, cpu.d[4]);
            m68k_set_reg(M68K_REG_D5, cpu.d[5]);
            m68k_set_reg(M68K_REG_D6, cpu.d[6]);
            m68k_set_reg(M68K_REG_D7, cpu.d[7]);
            m68k_set_reg(M68K_REG_A0, cpu.a[0]);
            m68k_set_reg(M68K_REG_A1, cpu.a[1]);
            m68k_set_reg(M68K_REG_A2, cpu.a[2]);
            m68k_set_reg(M68K_REG_A3, cpu.a[3]);
            m68k_set_reg(M68K_REG_A4, cpu.a[4]);
            m68k_set_reg(M68K_REG_A5, cpu.a[5]);
            m68k_set_reg(M68K_REG_A6, cpu.a[6]);
            m68k_set_reg(M68K_REG_A7, cpu.a[7]);
            m68k_set_reg(M68K_REG_PC, cpu.pc);
            if (g_emu_halted)
                m68k_end_timeslice();
        } else {
            /* Unimplemented LVO (catch-all stub) or bad fn id.
             * stub_addr = PC-4 at this point; lvo = stub - a6. */
            uint32_t spc = m68k_get_reg(NULL, M68K_REG_PC);
            uint32_t a6  = m68k_get_reg(NULL, M68K_REG_A6);
            int32_t lvo = (int32_t)spc - 4 - (int32_t)a6;
            static int32_t last_lvo = 0;
            static int unimpl_prints = 0;
            if (lvo != last_lvo && unimpl_prints < 100) {
                last_lvo = lvo;
                unimpl_prints++;
                char msg[48];
                const char *pfx = (lib == LIB_DOS) ? "[dos] unimpl lvo=-"
                                                 : "[util] unimpl lvo=-";
                int i = 0; while (pfx[i]) { msg[i] = pfx[i]; i++; }
                char n[12]; u32_dec((uint32_t)(-lvo), n, 12);
                int j = 0;
                while (n[j] && i<44) msg[i++]=n[j++];
                msg[i++]='\n'; msg[i]='\0';
                emu_print(msg);
            }
            m68k_set_reg(M68K_REG_D0, 0);
        }
    } else if (lib == LIB_GENERIC) {
        /* Fake library base — log the call (lvo via a6), return 0. */
        uint32_t spc = m68k_get_reg(NULL, M68K_REG_PC);
        uint32_t a6  = m68k_get_reg(NULL, M68K_REG_A6);
        int32_t lvo = (int32_t)spc - 4 - (int32_t)a6;
        const char *lname = emu_fake_lib_name(a6);
        static int32_t last_lvo = 0;
        if (lvo != last_lvo) {
            last_lvo = lvo;
            char msg[80];
            int i = 0;
            const char *p = "[lib] "; while (p[i]) { msg[i] = p[i]; i++; }
            const char *q = lname ? lname : "?";
            for (int j = 0; q[j] && i < 60; j++) msg[i++] = q[j];
            const char *t = " lvo=-"; for (int j = 0; t[j]; j++) msg[i++] = t[j];
            char n[12]; u32_dec((uint32_t)(-lvo), n, 12);
            for (int j = 0; n[j] && i < 76; j++) msg[i++] = n[j];
            msg[i++]='\n'; msg[i]='\0';
            emu_print(msg);
        }
        /* locale.library GetCatalogStr (LVO -72) must yield the caller's
         * built-in default string (a1) when no catalog was loaded —
         * returning NULL blanks every menu/gadget label. */
        int is_locale = lname && lname[0]=='l' && lname[6]=='.' &&
                        lname[1]=='o' && lname[2]=='c' && lname[3]=='a' &&
                        lname[4]=='l' && lname[5]=='e';
        /* asl.library: requester block lifecycle plus the native WM file
         * requester (UAOS-242).  LVO map (v37+):
         *   -30 AllocFileRequest   -36 FreeFileRequest   -42 RequestFile
         *   -48 AllocAslRequest    -54 FreeAslRequest    -60 AslRequest */
        int is_asl = lname && lname[0]=='a' && lname[1]=='s' &&
                     lname[2]=='l' && lname[3]=='.';
        if (is_asl) {
            /* Trace every ASL call with its return address (UAOS-242 debug). */
            uint32_t tsp2 = (uint32_t)m68k_get_reg(NULL, M68K_REG_SP);
            uint32_t ra2  = (tsp2 + 4 <= GUEST_RAM_SIZE) ? guest_read_be32(tsp2) : 0;
            char msg2[80]; int i2 = 0;
            const char *p2 = "[frq] call lvo="; while (p2[i2]) { msg2[i2]=p2[i2]; i2++; }
            char n2[12]; u32_dec((uint32_t)(-lvo), n2, 12);
            int j2 = 0; while (n2[j2]) msg2[i2++]=n2[j2++];
            const char *t2 = " ra="; j2 = 0; while (t2[j2]) msg2[i2++]=t2[j2++];
            u32_hex(ra2, n2); j2 = 0; while (n2[j2] && i2<76) msg2[i2++]=n2[j2++];
            msg2[i2++]='\n'; msg2[i2]='\0';
            emu_print(msg2);
        }
        /* cia*.resource: AddICRVector(-6)/RemICRVector(-12)/AbleICR(-18)/
         * SetICR(-24) — real implementations so guests can take CIA timer
         * and keyboard interrupts (UAOS-241). */
        int is_cia = lname && lname[0]=='c' && lname[1]=='i' &&
                     lname[2]=='a' && lname[4]=='.';
        if (is_cia && (lvo == -6 || lvo == -12 || lvo == -18 || lvo == -24))
            cia_res_dispatch(a6, lvo);
        else if (is_locale && lvo == -72)
            m68k_set_reg(M68K_REG_D0, m68k_get_reg(NULL, M68K_REG_A1));
        else if (is_asl && (lvo == -30 || lvo == -48)) {
            /* AllocFileRequest / AllocAslRequest — zeroed 512-byte block.
             * Result strings live inside it (drawer at +256, file at +384)
             * so no separate allocation bookkeeping is needed. */
            uint32_t fr = 0;
            dos_AllocMem_glue(512, MEMF_PUBLIC | MEMF_CLEAR_FLAG, &fr);
            m68k_set_reg(M68K_REG_D0, fr);
        }
        else if (is_asl && (lvo == -36 || lvo == -54)) {
            uint32_t fr = (uint32_t)m68k_get_reg(NULL, M68K_REG_A0);
            if (fr) dos_FreeMem_glue(fr, 512);
            m68k_set_reg(M68K_REG_D0, 0);
        }
        else if (is_asl && (lvo == -42 || lvo == -60)) {
            /* RequestFile / AslRequest: a0=requester, a1=taglist */
            extern int UAOS_Intuition_AslFileRequest(uint32_t req, uint32_t tags);
            uint32_t fr   = (uint32_t)m68k_get_reg(NULL, M68K_REG_A0);
            uint32_t tags = (uint32_t)m68k_get_reg(NULL, M68K_REG_A1);
            emu_print("[frq] AslRequest dispatch\n");
            m68k_set_reg(M68K_REG_D0,
                         (uint32_t)UAOS_Intuition_AslFileRequest(fr, tags));
        }
        else
            m68k_set_reg(M68K_REG_D0, 0);
    } else if (lib == LIB_BSDSOCKET) {
        extern void BsdSocket_Dispatch(uint32_t fn, uint32_t *regs);
        BsdSocket_Dispatch((uint32_t)fn, (uint32_t*)0);
    } else if (lib == LIB_GRAPHICS) {
        extern void UAOS_Graphics_Dispatch(uint32_t fn);
        UAOS_Graphics_Dispatch((uint32_t)fn);
    } else if (lib == LIB_INTUITION) {
        extern void UAOS_Intuition_Dispatch(uint32_t fn);
        UAOS_Intuition_Dispatch((uint32_t)fn);
    } else if (lib == LIB_GADTOOLS) {
        extern void UAOS_GADTOOLS_Dispatch(uint32_t fn);
        UAOS_GADTOOLS_Dispatch((uint32_t)fn);
    } else if (lib == LIB_AUDIODEV) {
        switch (fn) {
        case AUDEV_LVO_OPEN:    audio_dev_Open();    break;
        case AUDEV_LVO_CLOSE:   audio_dev_Close();   break;
        case AUDEV_LVO_BEGINIO: audio_dev_BeginIO(); break;
        case AUDEV_LVO_ABORTIO: audio_dev_AbortIO(); break;
        }
    } else {
        char msg[48] = "[emu] ILLEGAL: unknown lib=";
        char n[4]; u32_dec(lib, n, 4);
        int i = emu_strlen(msg), j = 0;
        while (n[j] && i<46) msg[i++]=n[j++];
        msg[i++]='\n'; msg[i]='\0';
        emu_print(msg);
    }

    if (strace_on)
        Strace_M68kExit(lib, fn, (int32_t)m68k_get_reg(NULL, M68K_REG_D0));

    if (trace_this) {
        char buf[48] = "    ->d0=0x";
        char n[12];
        u32_hex((uint32_t)m68k_get_reg(NULL, M68K_REG_D0), n);
        int i = emu_strlen(buf), j = 0;
        while (n[j] && i < 24) buf[i++] = n[j++];
        /* caller PC = *(SP) for jsr'd stubs */
        uint32_t tsp = (uint32_t)m68k_get_reg(NULL, M68K_REG_SP);
        uint32_t ra = (tsp + 4 <= GUEST_RAM_SIZE) ? guest_read_be32(tsp) : 0;
        const char *t = " ra=0x"; j = 0; while (t[j]) buf[i++] = t[j++];
        u32_hex(ra, n); j = 0; while (n[j] && i < 40) buf[i++] = n[j++];
        buf[i++]='\n'; buf[i]='\0';
        emu_print(buf);
    }

    return 1; /* handled — continue execution */
}

/* TRAP callback — not used for library dispatch but required by config */
int m68k_trap_callback(int trap)
{
    (void)trap;
    return 0; /* let CPU handle it normally */
}

/* =========================================================================
 * Amiga Hunk binary loader
 * ========================================================================= */

/* Big-endian 32-bit read from a byte buffer */
static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|
           ((uint32_t)p[2]<<8)|(uint32_t)p[3];
}

/* Big-endian 16-bit read from a byte buffer */
static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0]<<8)|(uint16_t)p[1]);
}

#define HUNK_HEADER   0x3F3
#define HUNK_CODE     0x3E9
#define HUNK_DATA     0x3EA
#define HUNK_BSS      0x3EB
#define HUNK_RELOC32  0x3EC
#define HUNK_END      0x3F2
#define HUNK_SYMBOL   0x3F0
#define HUNK_DEBUG    0x3F1
#define HUNK_DREL32      0x3F7   /* data-relative 16-bit relocations (like RELOC32SHORT) */
#define HUNK_RELOC32SHORT 0x3FE  /* compact 16-bit relocations */

/* g_hunk_base / g_hunk_blk / g_hunk_count / MAX_HUNKS are declared near the
 * top of this file (SetupProcess reads the seglist from them). */

/* Returns start PC (first hunk base) or 0 on error */
uint32_t hunk_load(const uint8_t *bin, uint32_t bin_size)
{
    if (bin_size < 8) { emu_cprint("[hunk] too small\n"); return 0; }

    const uint8_t *p   = bin;
    const uint8_t *end = bin + bin_size;

    /* HUNK_HEADER */
    if (be32(p) != HUNK_HEADER) { emu_cprint("[hunk] bad magic\n"); return 0; }
    p += 4;

    /* Skip resident library names */
    while (p + 4 <= end) {
        uint32_t cnt = be32(p); p += 4;
        if (!cnt) break;
        p += cnt * 4;
    }

    if (p + 12 > end) { emu_cprint("[hunk] truncated header\n"); return 0; }
    uint32_t table_size = be32(p); p += 4;
    uint32_t first_hunk = be32(p); p += 4;
    uint32_t last_hunk  = be32(p); p += 4;

    uint32_t n_hunks = last_hunk - first_hunk + 1;
    if (n_hunks > MAX_HUNKS) { emu_cprint("[hunk] too many hunks\n"); return 0; }
    (void)table_size;

    /* Read hunk sizes and allocate guest RAM.
     * Each hunk gets a real AmigaDOS segment header like LoadSeg produces:
     *   [block+0] total block size in bytes (header + data)
     *   [block+4] BPTR to next segment's link field (0 = last)
     *   [block+8] hunk data — this is what g_hunk_base[] records.
     * Self-decrunching binaries (e.g. OctaMED V5) walk this layout: they read
     * (hunk0_data-4) as the next-segment BPTR and (hunkN_data-8) as the byte
     * size of the block.  Without the headers they decrypt/scan garbage.
     * g_hunk_blk[0]+4 is also the program seglist BPTR published in
     * pr_SegList / cli_Module by UAOS_Emu_SetupProcess (UAOS-237). */
    g_hunk_count = (int)n_hunks;
    for (uint32_t i = 0; i < n_hunks; i++) {
        if (p + 4 > end) { emu_cprint("[hunk] size table truncated\n"); return 0; }
        uint32_t words = be32(p) & 0x3FFFFFFF; p += 4;  /* mask off mem flags */
        uint32_t bytes = words * 4;
        uint32_t blk = heap_alloc(bytes + 8);
        if (!blk) { emu_cprint("[hunk] OOM\n"); return 0; }
        g_hunk_blk[i] = blk;
        g_hunk_base[i] = blk + 8;
        guest_write_be32(blk, bytes + 8);  /* total block size in bytes */
    }
    for (uint32_t i = 0; i < n_hunks; i++) {
        uint32_t next = (i + 1 < n_hunks) ? ((g_hunk_blk[i + 1] + 4) >> 2) : 0;
        guest_write_be32(g_hunk_blk[i] + 4, next);
    }

    /* Load hunk bodies */
    int cur = 0;
    while (p + 4 <= end && cur < (int)n_hunks) {
        uint32_t type = be32(p) & 0x3FFFFFFF; p += 4;

        if (type == HUNK_CODE || type == HUNK_DATA) {
            if (p + 4 > end) break;
            uint32_t words = be32(p); p += 4;
            uint32_t bytes = words * 4;
            if (p + bytes > end) { emu_cprint("[hunk] CODE/DATA overflow\n"); return 0; }
            emu_memcpy(g_ram + g_hunk_base[cur], p, bytes);
            p += bytes;

        } else if (type == HUNK_BSS) {
            if (p + 4 > end) break;
            p += 4; /* size already allocated as zeroed */

        } else if (type == HUNK_RELOC32) {
            /* Apply relocations: for each hunk, a list of offsets to patch */
            while (p + 4 <= end) {
                uint32_t n_offsets = be32(p); p += 4;
                if (!n_offsets) break;
                if (p + 4 > end) break;
                uint32_t ref_hunk = be32(p); p += 4;
                if (ref_hunk >= n_hunks) { p += n_offsets * 4; continue; }
                uint32_t base = g_hunk_base[ref_hunk];
                for (uint32_t r = 0; r < n_offsets; r++) {
                    if (p + 4 > end) break;
                    uint32_t offset = be32(p); p += 4;
                    uint32_t patch_addr = g_hunk_base[cur] + offset;
                    if (patch_addr + 4 <= GUEST_RAM_SIZE) {
                        uint32_t old_val = m68k_read_memory_32(patch_addr);
                        m68k_write_memory_32(patch_addr, old_val + base);
                    }
                }
            }
            continue; /* don't advance cur */

        } else if (type == HUNK_DREL32 || type == HUNK_RELOC32SHORT) {
            /* Compact 16-bit relocations. Format: series of blocks
             *   count (u16)     — 0 terminates
             *   target_hunk (u16)
             *   count offsets (u16 each)
             * Apply relocations the same way as HUNK_RELOC32.
             * After the terminator, align p to the next 4-byte boundary
             * because hunk type words are always 32-bit. */
            while (p + 2 <= end) {
                uint16_t n_offsets = be16(p); p += 2;
                if (!n_offsets) break;
                if (p + 2 > end) break;
                uint16_t ref_hunk = be16(p); p += 2;
                if (ref_hunk >= n_hunks) { p += n_offsets * 2; continue; }
                uint32_t base = g_hunk_base[ref_hunk];
                for (uint16_t r = 0; r < n_offsets; r++) {
                    if (p + 2 > end) break;
                    uint32_t offset = (uint32_t)be16(p); p += 2;
                    uint32_t patch_addr = g_hunk_base[cur] + offset;
                    if (patch_addr + 4 <= GUEST_RAM_SIZE) {
                        uint32_t old_val = m68k_read_memory_32(patch_addr);
                        m68k_write_memory_32(patch_addr, old_val + base);
                    }
                }
            }
            /* Align to 4-byte boundary (16-bit data may leave p unaligned) */
            p = (const uint8_t *)(((uintptr_t)p + 3) & ~3u);
            continue; /* don't advance cur */

        } else if (type == HUNK_SYMBOL || type == HUNK_DEBUG) {
            /* Skip symbol/debug tables */
            while (p + 4 <= end) {
                uint32_t len = be32(p); p += 4;
                if (!len) break;
                p += len * 4 + 4; /* name words + value */
            }
            continue;

        } else if (type == HUNK_END) {
            cur++;
            continue;

        } else {
            char msg[48] = "[hunk] unknown type=0x";
            char hex[9]; u32_hex(type, hex);
            int i = emu_strlen(msg), j = 0;
            while (hex[j] && i<46) msg[i++]=hex[j++];
            msg[i++]='\n'; msg[i]='\0';
            emu_print(msg);
            break;
        }
    }

    return g_hunk_base[0];  /* entry point = first hunk base */
}

/* =========================================================================
 * Push a string onto the M68k stack; returns new SP
 * ========================================================================= */

static uint32_t push_string(uint32_t sp, const char *s)
{
    int len = emu_strlen(s) + 1;
    sp -= (uint32_t)((len + 1) & ~1); /* word-align */
    if (sp < PROG_BASE) return sp;
    emu_memcpy(g_ram + sp, s, (unsigned int)len);
    return sp;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void UAOS_Emu_SetCwd(const char *cwd)
{
    if (cwd) {
        int i = 0;
        while (cwd[i] && i < 63) {
            g_uaos_cwd[i] = cwd[i];
            i++;
        }
        g_uaos_cwd[i] = '\0';
    }
}

/* Initialise the emulator, load a Hunk binary and run it.
 * argv[0] = program name, argv[1..] = arguments, terminated by NULL.
 * print_fn is called for all program output routed to stdout/stderr.
 * Returns the program exit code (or -1 on load error).
 */
int UAOS_Emu_LoadAndRun_Internal(const uint8_t *binary, uint32_t bin_size,
                                  const char **argv, GluePrintFn print_fn)
{
    g_print = print_fn;

    /* Clear RAM */
    emu_memset(g_ram, 0, GUEST_RAM_SIZE);
    g_uaos_heap_ptr = PROG_BASE;
    SetIoErr(0);
    g_hunk_count = 0;
    g_genlib_count = 0;
    g_emu_halted  = 0;
    g_stdin_reads = 0;

    /* Install library jump tables */
    install_library_tables();

    /* Load the binary */
    uint32_t entry = hunk_load(binary, bin_size);
    if (!entry) {
        emu_cprint("[emu] Failed to load binary\n");
        return -1;
    }

    /* ---- Amiga CLI startup convention ----
     * A0 = pointer to command-line string (everything after program name)
     * D0 = command-line string length (byte count, NOT NUL-terminated)
     * SP = stack pointer with return address on top
     * SysBase is read by the program from absolute address 4
     * A6 is NOT set by us — the program loads SysBase itself via MOVEA.L 4.W,A6
     */

    /* Build command line string in guest RAM below the stack */
    uint32_t sp = STACK_TOP;

    /* Concatenate all argv[1..] into one space-separated string */
    char cmdline[256];
    int cmdlen = 0;
    if (argv) {
        for (int i = 1; argv[i] && cmdlen < 254; i++) {
            if (i > 1 && cmdlen < 254) cmdline[cmdlen++] = ' ';
            for (int j = 0; argv[i][j] && cmdlen < 254; j++)
                cmdline[cmdlen++] = argv[i][j];
        }
    }
    cmdline[cmdlen] = '\n'; /* Amiga CLI lines end with newline */
    cmdlen++;
    cmdline[cmdlen] = '\0';

    /* Place cmdline string just below SP.  BPTRs are (addr >> 2): every
     * pointer handed to the guest as a BPTR must be 4-byte aligned — keep
     * sp aligned after each reservation (UAOS-237). */
    sp -= (uint32_t)((cmdlen + 2) & ~1u);
    sp &= ~3u;
    uint32_t cmdline_ptr = sp;
    emu_memcpy(g_ram + cmdline_ptr, cmdline, (unsigned int)cmdlen);

    /* Build a BSTR version for GetArgStr (byte[0]=len, byte[1..len]=chars) */
    sp -= (uint32_t)((cmdlen + 2 + 4) & ~3u);
    sp &= ~3u;
    uint32_t bstr_ptr = sp;
    g_ram[bstr_ptr] = (uint8_t)(cmdlen < 255 ? cmdlen : 255);
    emu_memcpy(g_ram + bstr_ptr + 1, cmdline, (unsigned int)cmdlen);
    g_cmdline_bptr = bstr_ptr >> 2;  /* BPTR = addr >> 2 */

    /* Build a separate BSTR for cli_CommandName (up to 1+15 bytes). */
    sp -= 16;
    uint32_t cmdname_bstr_ptr = sp;
    const char *cmdname = (argv && argv[0]) ? argv[0] : "m68k";
    UAOS_Emu_SetTarCompat(cmdname);
    uint8_t cmdname_len = 0;
    while (cmdname_len < 15 && cmdname[cmdname_len]) cmdname_len++;
    g_ram[cmdname_bstr_ptr] = cmdname_len;
    emu_memcpy(g_ram + cmdname_bstr_ptr + 1, cmdname, cmdname_len);
    uint32_t cmdname_bptr = cmdname_bstr_ptr >> 2;

    /* Allocate + populate Process/CLI/RDArgs past the loaded hunks */
    UAOS_Emu_SetupProcess(cmdname_bptr);

    /* Push return address — our DOS Exit stub so RTS ends execution */
    uint32_t exit_stub = stub_addr(LIB_DOS, DOS_EXIT);
    sp -= 4;
    m68k_write_memory_32(sp, exit_stub);

    /* Initialise CPU — MUST call pulse_reset before setting registers */
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68020);
    m68k_set_illg_instr_callback(m68k_illg_instr_callback);
    m68k_set_instr_hook_callback(uaos_m68k_instr_hook);

    /* Patch reset vectors so pulse_reset loads our entry/stack:
     * Address 0 = initial SSP, Address 4 = initial PC
     * We save/restore SysBase (stored at addr 4) around this. */
    m68k_write_memory_32(0, sp);     /* SSP */
    m68k_write_memory_32(4, entry);  /* PC  */
    m68k_pulse_reset();              /* CPU loads SSP from 0, PC from 4 */

    /* Restore SysBase at address 4 (pulse_reset consumed it as PC) */
    m68k_write_memory_32(4, EXEC_BASE);

    /* pulse_reset starts at IPL 7 — drop to supervisor/IPL 0 like a real
     * exec task so chipset autovectors can preempt the guest. */
    m68k_set_reg(M68K_REG_SR, 0x2000);

    /* Set entry registers per Amiga CLI convention */
    m68k_set_reg(M68K_REG_A0, cmdline_ptr);       /* command line ptr */
    m68k_set_reg(M68K_REG_D0, (uint32_t)cmdlen);  /* command line length */


    extern void UAOS_Intuition_PostIntuiTicks(void);

    /* Run in 1M-cycle slices until the program calls Exit or we time out */
    int slices = 0;
    while (!g_emu_halted && slices < 200) {  /* max 200M cycles total */
        m68k_execute(1000000);
        Chiptrace_PcSample();
        g_m68k_cycles += (uint64_t)m68k_cycles_run();
        chip_emu_run_to_cycle(g_m68k_cycles);
        UAOS_Intuition_PostIntuiTicks();
        slices++;
    }
    (void)slices;
    return 0;
}
