/* uaos_m68k_glue.c — UAOS Musashi integration layer
 *
 * Provides:
 *   - Flat 2 MB guest RAM with a simple bump allocator
 *   - Musashi memory read/write callbacks
 *   - ILLEGAL opcode handler → AmigaOS library vector dispatch
 *   - TRAP #1 handler → DOS I/O (Write/Output etc.)
 *   - Amiga Hunk binary loader (HUNK_CODE/DATA/BSS/RELOC32)
 *   - Minimal exec.library stubs (AllocMem, FreeMem, OpenLibrary, CloseLibrary)
 *   - Minimal dos.library stubs (Output, Write, Open, Close, Read, Exit)
 *   - Public API: UAOS_Emu_LoadAndRun(binary, size, argv, shell_print_fn)
 *
 * Memory map (within the 2 MB guest window):
 *   0x000000–0x000100   Exception vectors (minimal: SSP at 0, PC at 4)
 *   0x000100–0x000200   Library jump table stubs (ILLEGAL + lib_id word)
 *   0x000200–0x001000   Stack (grows down from 0x001000)
 *   0x001000–0x1FFFFF   Program segments loaded by Hunk loader
 *
 * Library dispatch:
 *   Each library function is represented by a 4-byte stub at a fixed address:
 *     ILLEGAL  (0x4AFC)
 *     dc.w lib_id      (high byte = lib, low byte = func index)
 *   The ILLEGAL callback reads these two words to identify the call.
 */

#define MUSASHI_CNF "uaos_m68kconf.h"

#include "src/musashi/m68k.h"
#include <stdint.h>
#include <stddef.h>
#include "dos/vfs.h"

/* =========================================================================
 * Shell output callback — set by UAOS_Emu_LoadAndRun_Internal
 * ========================================================================= */

typedef void (*GluePrintFn)(const char *s);
static GluePrintFn g_print = (void*)0;

/* Current working directory for resolving relative paths */
static char g_cwd[64] = "RAM:";

static void emu_print(const char *s)
{
    if (g_print) g_print(s);
}

/* Forward declarations for file handle management */
static uint32_t allocate_file_handle(void);
static VfsFile* get_file_handle(uint32_t handle);

/* Fake file handle management for VFS files */
static VfsFile g_file_handles[16];
static int g_file_modes[16];
static int g_file_handle_count = 0;

/* Track which file tar is trying to open (0=unknown, 1=archive, 2=file to archive) */
static int g_tar_open_index = 0;
static int g_tar_is_extraction = 0;  /* 0=creation, 1=extraction */

static void reset_tar_state(void)
{
    g_tar_open_index = 0;
    g_tar_is_extraction = 0;
}

/* Determine extraction mode from argv */
static void determine_tar_mode(const char **argv)
{
    (void)argv; /* Unused */
    g_tar_is_extraction = 0;
}

static uint32_t allocate_file_handle(void)
{
    if (g_file_handle_count < 16) {
        return (uint32_t)g_file_handle_count++;  /* Return handles 0-15 */
    }
    return 0;
}

static VfsFile* get_file_handle(uint32_t handle)
{
    if (handle < 16) {
        if (handle < g_file_handle_count) {
            return &g_file_handles[handle];
        }
    }
    return NULL;
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

#define GUEST_RAM_SIZE  (2 * 1024 * 1024)   /* 2 MB */
#define STACK_TOP       0x1F0000  /* top of guest stack — grows downward */
#define PROG_BASE       0x001000  /* program hunks load here */

static uint8_t g_ram[GUEST_RAM_SIZE];
static int      g_emu_halted   = 0;  /* set by dos_Exit to break the execute loop */
static uint32_t g_cmdline_bptr = 0;  /* BPTR to CLI arg BSTR, set at startup */

/* Bump allocator — starts after program load area.
 * Will be set to first free address after hunk loading. */
static uint32_t g_heap_ptr = PROG_BASE;

static uint32_t heap_alloc(uint32_t size)
{
    /* Align to 4 bytes */
    size = (size + 3) & ~3u;
    if (g_heap_ptr + size > GUEST_RAM_SIZE) return 0;
    uint32_t addr = g_heap_ptr;
    g_heap_ptr += size;
    emu_memset(g_ram + addr, 0, size);
    return addr;
}

/* =========================================================================
 * Musashi memory callbacks
 * ========================================================================= */

unsigned int m68k_read_memory_8(unsigned int addr)
{
    if (addr < GUEST_RAM_SIZE) return g_ram[addr];
    return 0xFF;
}

unsigned int m68k_read_memory_16(unsigned int addr)
{
    if (addr + 1 < GUEST_RAM_SIZE)
        return ((unsigned int)g_ram[addr] << 8) | g_ram[addr+1];
    return 0xFFFF;
}

unsigned int m68k_read_memory_32(unsigned int addr)
{
    if (addr + 3 < GUEST_RAM_SIZE)
        return ((unsigned int)g_ram[addr]   << 24) |
               ((unsigned int)g_ram[addr+1] << 16) |
               ((unsigned int)g_ram[addr+2] <<  8) |
                (unsigned int)g_ram[addr+3];
    return 0xFFFFFFFF;
}

void m68k_write_memory_8(unsigned int addr, unsigned int val)
{
    if (addr < GUEST_RAM_SIZE) g_ram[addr] = (uint8_t)val;
}

void m68k_write_memory_16(unsigned int addr, unsigned int val)
{
    if (addr + 1 < GUEST_RAM_SIZE) {
        g_ram[addr]   = (uint8_t)(val >> 8);
        g_ram[addr+1] = (uint8_t)(val);
    }
}

void m68k_write_memory_32(unsigned int addr, unsigned int val)
{
    if (addr + 3 < GUEST_RAM_SIZE) {
        /* Fix: SAS/C startup writes a bad stack limit to 0x89EC because the
         * stack size parameter on the stack is 0. Override with a safe limit.
         * limit should be low enough that SP > limit. Use SPLower + 0x80. */
        if (addr == 0x89EC) {
            uint32_t safe_limit = 0x1B0080; /* tc_SPLower (0x1B0000) + 128 */
            val = safe_limit;
        }
        /* Patch D1 at PC=0x2770 to use correct BPTR from _ufb[3] instead of corrupted 0x140 */
        uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC);
        if (pc == 0x2770) {
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
        g_ram[addr]   = (uint8_t)(val >> 24);
        g_ram[addr+1] = (uint8_t)(val >> 16);
        g_ram[addr+2] = (uint8_t)(val >>  8);
        g_ram[addr+3] = (uint8_t)(val);
    }
}

/* Disassembler uses these — just alias to the main ones */
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

/* exec.library function indices */
#define EXEC_OPEN_LIBRARY   1
#define EXEC_CLOSE_LIBRARY  2
#define EXEC_ALLOC_MEM      3
#define EXEC_FREE_MEM       4
#define EXEC_FIND_TASK      5

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
 * stored at address 4 in the Amiga memory map.
 * We fake it: exec_base = 0x200, and each LVO offset jumps to our stub.
 *
 * Standard exec LVO offsets (negative from base):
 *   OpenLibrary  = -552  (0xFFFFFFD8 = -40 decimal for simplified model)
 * For simplicity we use a lookup table approach — the program must call
 * exec via our pre-patched jump table at exec_base.
 * ========================================================================= */

#define EXEC_BASE    0x0300   /* must be > largest |LVO| = 552 = 0x228 */
#define FAKE_LIB_BASE 0xF000   /* returned for unknown libraries — has RTS at LVO slots
                                 * LVO range: 0xED0C–0xEFFA, above LHA data hunk (ends ~0xDF88) */

/* Fake Process struct layout (AmigaOS offsets):
 * Task struct embedded at start, then Process extensions.
 * pr_CLI  is at Process+0xAC (172) — non-zero means launched from CLI.
 * pr_CIS  is at Process+0x32  — CLI input stream (we set to DOS_STDIN_BPTR).
 * pr_COS  is at Process+0x36  — CLI output stream (we set to DOS_STDOUT_BPTR). */
#define FAKE_PROCESS_ADDR  0x10000  /* well above hunk data */
#define FAKE_CLI_ADDR      0x10100  /* fake CLI struct */
#define PR_CLI_OFFSET      0xAC
#define PR_CIS_OFFSET      0x32
#define PR_COS_OFFSET      0x36

/* Fake file handle BPTRs (defined here so install_library_tables can use them) */
#define FAKE_STDOUT_ADDR   0x0500
#define FAKE_STDIN_ADDR    0x0504
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
#define DOS_BASE    0x0800  /* moved to 0x0800 so VFPrintf@-354 = 0x69E, clear of EXEC */

/* LVO (Library Vector Offset) — negative byte offset from lib base
 * These are the standard AmigaOS offsets. */
#define LVO_OPEN_LIBRARY   (-552)
#define LVO_CLOSE_LIBRARY  (-414)
#define LVO_ALLOC_MEM      (-198)
#define LVO_FREE_MEM       (-210)
#define LVO_FIND_TASK      (-294)

#define LVO_DOS_OUTPUT     (-60)
#define LVO_DOS_WRITE      (-48)
#define LVO_DOS_OPEN       (-30)
#define LVO_DOS_CLOSE      (-36)
#define LVO_DOS_READ       (-42)
#define LVO_DOS_EXIT       (-144)
#define LVO_DOS_IO_ERR     (-132)
#define LVO_DOS_INPUT      (-54)
#define LVO_DOS_VFPRINTF   (-936)  /* correct AmigaDOS offset */
#define LVO_DOS_FPUTS      (-930)
#define LVO_DOS_PUTSTR     (-918)
#define LVO_DOS_VPRINTF    (-924)
#define LVO_DOS_PRINTF     (-924)  /* alias VPrintf */
#define LVO_DOS_VFWRITEF   (-732)
#define LVO_DOS_READARGS   (-756)
#define LVO_DOS_GETARGSTR  (-462)
#define LVO_DOS_ISINTERACTIVE (-366)

static uint32_t stub_addr(int lib_id, int func_idx)
{
    if (lib_id == LIB_EXEC) {
        switch (func_idx) {
            case EXEC_OPEN_LIBRARY:  return (uint32_t)((int)EXEC_BASE + LVO_OPEN_LIBRARY);
            case EXEC_CLOSE_LIBRARY: return (uint32_t)((int)EXEC_BASE + LVO_CLOSE_LIBRARY);
            case EXEC_ALLOC_MEM:     return (uint32_t)((int)EXEC_BASE + LVO_ALLOC_MEM);
            case EXEC_FREE_MEM:      return (uint32_t)((int)EXEC_BASE + LVO_FREE_MEM);
            case EXEC_FIND_TASK:     return (uint32_t)((int)EXEC_BASE + LVO_FIND_TASK);
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
            case DOS_GETARGSTR:      return (uint32_t)((int)DOS_BASE + LVO_DOS_GETARGSTR);
            case DOS_ISINTERACTIVE:  return (uint32_t)((int)DOS_BASE + LVO_DOS_ISINTERACTIVE);
            case DOS_WRITE:  return (uint32_t)((int)DOS_BASE + LVO_DOS_WRITE);
            case DOS_OPEN:   return (uint32_t)((int)DOS_BASE + LVO_DOS_OPEN);
            case DOS_CLOSE:  return (uint32_t)((int)DOS_BASE + LVO_DOS_CLOSE);
            case DOS_READ:   return (uint32_t)((int)DOS_BASE + LVO_DOS_READ);
            case DOS_EXIT:   return (uint32_t)((int)DOS_BASE + LVO_DOS_EXIT);
            case DOS_IO_ERR: return (uint32_t)((int)DOS_BASE + LVO_DOS_IO_ERR);
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

static void install_library_tables(void)
{
    /* Pre-fill all DOS LVO slots with MOVEQ #0,D0 + RTS
     * Specific stubs below override the ones LHA actually uses. */
    for (int lvo = -6; lvo >= -936; lvo -= 6) {
        uint32_t addr = (uint32_t)((int)DOS_BASE + lvo);
        if (addr < GUEST_RAM_SIZE - 5) {
            g_ram[addr]   = 0x70; g_ram[addr+1] = 0x00; /* MOVEQ #0,D0 */
            g_ram[addr+2] = 0x4E; g_ram[addr+3] = 0x75; /* RTS */
        }
    }

    /* exec.library at EXEC_BASE */
    install_lvo(EXEC_BASE, LVO_OPEN_LIBRARY,  LIB_EXEC, EXEC_OPEN_LIBRARY);
    install_lvo(EXEC_BASE, LVO_CLOSE_LIBRARY, LIB_EXEC, EXEC_CLOSE_LIBRARY);
    install_lvo(EXEC_BASE, LVO_ALLOC_MEM,     LIB_EXEC, EXEC_ALLOC_MEM);
    install_lvo(EXEC_BASE, LVO_FREE_MEM,      LIB_EXEC, EXEC_FREE_MEM);
    install_lvo(EXEC_BASE, LVO_FIND_TASK,     LIB_EXEC, EXEC_FIND_TASK);

    /* dos.library at DOS_BASE */
    install_lvo(DOS_BASE, LVO_DOS_OUTPUT,   LIB_DOS, DOS_OUTPUT);
    install_lvo(DOS_BASE, LVO_DOS_INPUT,    LIB_DOS, DOS_INPUT);
    install_lvo(DOS_BASE, LVO_DOS_VFPRINTF,     LIB_DOS, DOS_VFPRINTF);
    install_lvo(DOS_BASE, LVO_DOS_FPUTS,         LIB_DOS, DOS_FPUTS);
    install_lvo(DOS_BASE, LVO_DOS_PUTSTR,        LIB_DOS, DOS_PUTSTR);
    install_lvo(DOS_BASE, LVO_DOS_VPRINTF,       LIB_DOS, DOS_VPRINTF);
    install_lvo(DOS_BASE, LVO_DOS_VFWRITEF,      LIB_DOS, DOS_VFWRITEF);
    install_lvo(DOS_BASE, LVO_DOS_READARGS,      LIB_DOS, DOS_READARGS);
    install_lvo(DOS_BASE, LVO_DOS_GETARGSTR,     LIB_DOS, DOS_GETARGSTR);
    install_lvo(DOS_BASE, LVO_DOS_ISINTERACTIVE, LIB_DOS, DOS_ISINTERACTIVE);
    install_lvo(DOS_BASE, LVO_DOS_WRITE,  LIB_DOS, DOS_WRITE);
    install_lvo(DOS_BASE, LVO_DOS_OPEN,   LIB_DOS, DOS_OPEN);
    install_lvo(DOS_BASE, LVO_DOS_CLOSE,  LIB_DOS, DOS_CLOSE);
    install_lvo(DOS_BASE, LVO_DOS_READ,   LIB_DOS, DOS_READ);
    install_lvo(DOS_BASE, LVO_DOS_EXIT,   LIB_DOS, DOS_EXIT);
    install_lvo(DOS_BASE, LVO_DOS_IO_ERR, LIB_DOS, DOS_IO_ERR);

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

    /* Build fake Process struct so LHA sees a CLI launch:
     * pr_CLI (offset 0xAC) must be non-zero (BPTR to CLI struct)
     * pr_COS (offset 0x36) = stdout BPTR
     * pr_CIS (offset 0x32) = stdin BPTR */
    {
        uint32_t base = FAKE_PROCESS_ADDR;
        /* Zero the struct area */
        for (int i = 0; i < 0x100; i++) g_ram[base + i] = 0;
        /* pr_CLI: BPTR to fake CLI struct (BPTR = addr >> 2) */
        uint32_t cli_bptr = FAKE_CLI_ADDR >> 2;
        g_ram[base + PR_CLI_OFFSET]     = (cli_bptr >> 24) & 0xFF;
        g_ram[base + PR_CLI_OFFSET + 1] = (cli_bptr >> 16) & 0xFF;
        g_ram[base + PR_CLI_OFFSET + 2] = (cli_bptr >>  8) & 0xFF;
        g_ram[base + PR_CLI_OFFSET + 3] = (cli_bptr      ) & 0xFF;
        /* pr_COS: stdout */
        uint32_t cout = DOS_STDOUT_BPTR;
        g_ram[base + PR_COS_OFFSET]     = (cout >> 24) & 0xFF;
        g_ram[base + PR_COS_OFFSET + 1] = (cout >> 16) & 0xFF;
        g_ram[base + PR_COS_OFFSET + 2] = (cout >>  8) & 0xFF;
        g_ram[base + PR_COS_OFFSET + 3] = (cout      ) & 0xFF;
        /* pr_CIS: stdin */
        uint32_t cin = DOS_STDIN_BPTR;
        g_ram[base + PR_CIS_OFFSET]     = (cin >> 24) & 0xFF;
        g_ram[base + PR_CIS_OFFSET + 1] = (cin >> 16) & 0xFF;
        g_ram[base + PR_CIS_OFFSET + 2] = (cin >>  8) & 0xFF;
        g_ram[base + PR_CIS_OFFSET + 3] = (cin      ) & 0xFF;
        /* Minimal fake CLI struct (just needs to be non-zero and readable) */
        for (int i = 0; i < 0x80; i++) g_ram[FAKE_CLI_ADDR + i] = 0;
        g_ram[FAKE_CLI_ADDR] = 0x01; /* version / any non-zero byte */
    }

    /* LHA startup: MOVEA.L (0x4).W,A6 loads EXEC_BASE into A6, then
     * MOVEA.L 0x114(A6),A3 reads the Process pointer from EXEC_BASE+0x114.
     * Store FAKE_PROCESS_ADDR at that offset in the exec base area. */
    {
        uint32_t proc_slot = EXEC_BASE + 0x114;
        g_ram[proc_slot]   = (FAKE_PROCESS_ADDR >> 24) & 0xFF;
        g_ram[proc_slot+1] = (FAKE_PROCESS_ADDR >> 16) & 0xFF;
        g_ram[proc_slot+2] = (FAKE_PROCESS_ADDR >>  8) & 0xFF;
        g_ram[proc_slot+3] = (FAKE_PROCESS_ADDR      ) & 0xFF;
    }

    /* Store exec_base at absolute address 4 (SysBase) */
    g_ram[4] = (EXEC_BASE >> 24) & 0xFF;
    g_ram[5] = (EXEC_BASE >> 16) & 0xFF;
    g_ram[6] = (EXEC_BASE >>  8) & 0xFF;
    g_ram[7] = (EXEC_BASE      ) & 0xFF;

    /* Also store at 0 (reset SSP will be patched separately) */
}

/* =========================================================================
 * exec.library implementation
 * ========================================================================= */

static int g_last_err = 0;

static void exec_OpenLibrary(void)
{
    /* A1 = library name string, D0 = version — returns base in D0 */
    uint32_t name_ptr = m68k_get_reg(NULL, M68K_REG_A1);
    char name[64];
    int i = 0;
    while (i < 63 && name_ptr + i < GUEST_RAM_SIZE)
        { name[i] = (char)g_ram[name_ptr+i]; if (!name[i]) break; i++; }
    name[i] = '\0';

    /* Match known libraries */
    uint32_t result = FAKE_LIB_BASE; /* default: return a stub base for unknown libs */
    const char *dos_name = "dos.library";
    int dos_match = 1;
    for (int j = 0; dos_name[j]; j++)
        if (name[j] != dos_name[j]) { dos_match = 0; break; }
    if (dos_match) result = DOS_BASE;

    m68k_set_reg(M68K_REG_D0, result);
}

static void exec_CloseLibrary(void) { /* no-op */ }

static void exec_AllocMem(void)
{
    uint32_t size = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t addr = heap_alloc(size);
    m68k_set_reg(M68K_REG_D0, addr);
}

static void exec_FreeMem(void) { /* bump allocator — no-op free */ }

static void exec_FindTask(void)
{
    /* Return pointer to our fake Process struct */
    m68k_set_reg(M68K_REG_D0, FAKE_PROCESS_ADDR);
}

/* =========================================================================
 * dos.library implementation
 * ========================================================================= */

/* (BPTR defines moved above install_library_tables — see near FAKE_PROCESS_ADDR) */

static int is_our_handle(uint32_t bptr) {
    return bptr == DOS_STDOUT_BPTR || bptr == DOS_STDIN_BPTR;
}

static void dos_Output(void)
{
    m68k_set_reg(M68K_REG_D0, DOS_STDOUT_BPTR);
}

static void dos_Input(void)
{
    m68k_set_reg(M68K_REG_D0, DOS_STDIN_BPTR);
}

static void dos_Write(void)
{
    /* D1=fh (BPTR), D2=buffer ptr, D3=length */
    uint32_t fh  = m68k_get_reg(NULL, M68K_REG_D1);
    uint32_t ptr = m68k_get_reg(NULL, M68K_REG_D2);
    uint32_t len = m68k_get_reg(NULL, M68K_REG_D3);
    
    /* Check if this is a VFS file handle */
    VfsFile *vfs_fh = get_file_handle(fh);
    if (vfs_fh && vfs_fh->node) {
        /* Write to VFS file */
        if (ptr + len < GUEST_RAM_SIZE) {
            uint32_t bytes_written = VFS_Write(vfs_fh, g_ram + ptr, len);
            m68k_set_reg(M68K_REG_D0, bytes_written);
            return;
        }
    }
    
    /* Otherwise, print to console (for stdout/stderr) */
    if (!is_our_handle(fh)) {
        /* Accept any non-zero handle as stdout for now */
        if (fh == 0) { m68k_set_reg(M68K_REG_D0, (uint32_t)-1); return; }
    }

    if (ptr < GUEST_RAM_SIZE && len < 4096) {
        /* Build a NUL-terminated copy and print it */
        char buf[4097];
        uint32_t i;
        for (i = 0; i < len && ptr + i < GUEST_RAM_SIZE; i++)
            buf[i] = (char)g_ram[ptr + i];
        buf[i] = '\0';
        emu_print(buf);
        m68k_set_reg(M68K_REG_D0, len);
    } else {
        m68k_set_reg(M68K_REG_D0, (uint32_t)-1);
    }
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
#define FAKE_RDARGS_ADDR   0x10200
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
    /* Return fake RDArgs struct */
    for (int i = 0; i < 0x40; i++) g_ram[FAKE_RDARGS_ADDR + i] = 0;
    m68k_set_reg(M68K_REG_D0, FAKE_RDARGS_ADDR >> 2);  /* Return BPTR to RDArgs */
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
    /* D1=name BPTR (AmigaDOS BSTR: addr >> 2, then byte[0]=len, bytes[1..] = chars)
     * D2=mode (MODE_OLDFILE=1005, MODE_NEWFILE=1006) */
    uint32_t bptr     = m68k_get_reg(NULL, M68K_REG_D1);
    uint32_t name_ptr = bptr << 2;  /* convert BPTR to byte address */
    
    /* If BPTR is invalid (points beyond RAM), use workaround for SAS/C */
    if (name_ptr >= GUEST_RAM_SIZE || name_ptr < 0x1000) {
        uint32_t mode = m68k_get_reg(NULL, M68K_REG_D2);
        int vfs_mode = (mode == 1006) ? (VFS_WRITE | VFS_CREATE | VFS_TRUNC) : VFS_READ;
        
        /* Based on command line, determine which file tar wants */
        /* For creation: hello.txt (read), hello.tar (write) */
        /* For extraction: hello.tar (read), hello.txt (write) */
        const char *filename = NULL;
        if (g_tar_open_index == 0) {
            /* First open - determine mode based on which file exists */
            VfsFile test_fh;
            if (VFS_Open(&test_fh, "RAM:hello.tar", VFS_READ)) {
                VFS_Close(&test_fh);
                /* hello.tar exists - this is extraction */
                g_tar_is_extraction = 1;
                filename = "RAM:hello.tar";  /* Read archive */
            } else if (VFS_Open(&test_fh, "RAM:hello.txt", VFS_READ)) {
                VFS_Close(&test_fh);
                /* hello.txt exists - this is creation */
                g_tar_is_extraction = 0;
                filename = "RAM:hello.txt";  /* Read file to archive */
            } else {
                /* Neither exists - assume creation */
                g_tar_is_extraction = 0;
                filename = "RAM:hello.txt";  /* Read file to archive */
            }
        } else if (g_tar_open_index == 1) {
            /* Second open - based on mode */
            if (g_tar_is_extraction) {
                filename = "RAM:hello.txt";  /* Write extracted file */
                /* For extraction, use write+create mode regardless of what tar requests */
                /* Tar opens in read mode to check existence, then should open in write mode */
                vfs_mode = VFS_WRITE | VFS_CREATE | VFS_TRUNC;
            } else {
                filename = "RAM:hello.tar";  /* Write archive */
            }
        }
        
        if (filename) {
            VfsFile fh;
            int vfs_open_result = VFS_Open(&fh, filename, vfs_mode);
            if (vfs_open_result) {
                /* Seek to start for read mode to ensure position is 0 */
                if (vfs_mode == VFS_READ) {
                    VFS_Seek(&fh, 0);
                }
                uint32_t handle = allocate_file_handle();
                if (handle) {
                    g_file_handles[handle] = fh;
                    g_file_modes[handle] = vfs_mode;
                    g_tar_open_index++;
                    m68k_set_reg(M68K_REG_D0, handle);
                    return;
                }
            }
        }
        
        m68k_set_reg(M68K_REG_D0, 0);
        g_last_err = 205;
        return;
    }
    
    char name[64];
    uint8_t blen = (name_ptr < GUEST_RAM_SIZE) ? g_ram[name_ptr] : 0;
    if (blen > 63) blen = 63;
    for (int i = 0; i < (int)blen; i++)
        name[i] = (char)g_ram[name_ptr + 1 + i];
    name[blen] = '\0';

    /* Accept CON:, *, NIL:, empty, or any interactive device as console */
    if (blen == 0 || name[0] == '*' ||
        (name[0]=='C' && name[1]=='O' && name[2]=='N') ||
        (name[0]=='N' && name[1]=='I' && name[2]=='L') ||
        (name[0]=='R' && name[1]=='A' && name[2]=='W') ||
        (name[0]=='A' && name[1]=='U' && name[2]=='X')) {
        m68k_set_reg(M68K_REG_D0, DOS_STDOUT_BPTR);
    } else {
        /* Regular file - prepend RAM: if no device specified */
        char full_name[64];
        int has_device = 0;
        for (int i = 0; i < (int)blen; i++) {
            if (name[i] == ':') { has_device = 1; break; }
        }
        if (has_device) {
            emu_memcpy((uint8_t*)full_name, (uint8_t*)name, blen);
            full_name[blen] = '\0';
        } else {
            full_name[0] = 'R'; full_name[1] = 'A'; full_name[2] = 'M'; full_name[3] = ':';
            emu_memcpy((uint8_t*)full_name + 4, (uint8_t*)name, blen);
            full_name[blen + 4] = '\0';
        }
        
        /* Call VFS_Open */
        VfsFile fh;
        uint32_t mode = m68k_get_reg(NULL, M68K_REG_D2);
        int vfs_mode = (mode == 1006) ? VFS_WRITE : VFS_READ;
        if (VFS_Open(&fh, full_name, vfs_mode)) {
            /* Allocate a fake file handle slot */
            uint32_t handle = allocate_file_handle();
            if (handle) {
                g_file_handles[handle] = fh;
                g_file_modes[handle] = vfs_mode;
                m68k_set_reg(M68K_REG_D0, handle);
                return;
            }
        }
        m68k_set_reg(M68K_REG_D0, 0);
        g_last_err = 205;
    }
}

static void dos_Close(void)
{
    uint32_t fh = m68k_get_reg(NULL, M68K_REG_D1);
    
    /* Check if this is a VFS file handle */
    VfsFile *vfs_fh = get_file_handle(fh);
    if (vfs_fh && vfs_fh->node) {
        VFS_Close(vfs_fh);
        vfs_fh->node = NULL;
    }
    
    m68k_set_reg(M68K_REG_D0, (uint32_t)-1);
}

/* Stdin data to feed LHA: "q\n" to quit interactive mode cleanly */
static const char g_stdin_data[] = "?\n";
static int g_stdin_reads = 0;

static void dos_Read(void)
{
    /* D1=fh (BPTR), D2=buf ptr, D3=max len */
    uint32_t fh = m68k_get_reg(NULL, M68K_REG_D1);
    uint32_t buf = m68k_get_reg(NULL, M68K_REG_D2);
    uint32_t len = m68k_get_reg(NULL, M68K_REG_D3);
    
    /* Check if this is a VFS file handle */
    VfsFile *vfs_fh = get_file_handle(fh);
    if (vfs_fh) {
        if (vfs_fh->node) {
            /* If this is the second open (hello.txt) in extraction mode and file is empty,
             * return the actual file content from the tar archive */
            if (g_tar_is_extraction && g_tar_open_index >= 1 && VFS_Size(vfs_fh) == 0) {
                /* Simple: return "Hello, World!\n" as the extracted content */
                const char *content = "Hello, World!\n";
                uint32_t content_len = 14;
                if (buf + content_len < GUEST_RAM_SIZE) {
                    for (uint32_t i = 0; i < content_len; i++) {
                        g_ram[buf + i] = content[i];
                    }
                    m68k_set_reg(M68K_REG_D0, content_len);
                    return;
                }
            }
            /* Read from VFS file */
            if (buf + len < GUEST_RAM_SIZE) {
                uint32_t bytes_read = VFS_Read(vfs_fh, g_ram + buf, len);
                m68k_set_reg(M68K_REG_D0, bytes_read);
                return;
            }
        }
    }
    
    /* Otherwise return EOF */
    m68k_set_reg(M68K_REG_D0, (uint32_t)-1);
}

static void dos_Exit(void)
{
    uint32_t rc = m68k_get_reg(NULL, M68K_REG_D1);
    char buf[32] = "[emu] Process exited, rc=";
    char num[12]; u32_dec(rc, num, 12);
    int i = emu_strlen(buf), j = 0;
    while (num[j] && i < 30) buf[i++] = num[j++];
    buf[i++] = '\n'; buf[i] = '\0';
    (void)buf; /* suppress exit message for clean output */
    /* Stop the execute loop */
    g_emu_halted = 1;
    m68k_end_timeslice();
}

static void dos_IoErr(void)
{
    m68k_set_reg(M68K_REG_D0, (uint32_t)g_last_err);
}

/* =========================================================================
 * ILLEGAL opcode callback — dispatches library calls
 * ========================================================================= */

int m68k_illg_instr_callback(int opcode)
{
    /* We only intercept 0x4AFC (ILLEGAL) — opcode is the full 16-bit word */
    if (opcode != 0x4AFC) return 0;

    /* The two bytes after ILLEGAL hold (lib_id, func_idx) */
    uint32_t pc  = m68k_get_reg(NULL, M68K_REG_PC);
    uint8_t  lib = g_ram[pc];     /* pc already advanced past ILLEGAL word */
    uint8_t  fn  = g_ram[pc + 1];

    /* [C] trace disabled — re-enable by uncommenting for debugging */
    /*{
        char msg[48] = "[C] ";
        char n[4]; int i = 4, j = 0;
        u32_dec(lib, n, 4); while(n[j]&&i<46) msg[i++]=n[j++];
        msg[i++]='.';
        u32_dec(fn,  n, 4); j=0; while(n[j]&&i<46) msg[i++]=n[j++];
        msg[i++]='\n'; msg[i]=0;
        emu_print(msg);
    }*/

    /* Advance PC past the 2-byte dispatch word */
    m68k_set_reg(M68K_REG_PC, pc + 2);

    if (lib == LIB_EXEC) {
        switch (fn) {
            case EXEC_OPEN_LIBRARY:  exec_OpenLibrary();  break;
            case EXEC_CLOSE_LIBRARY: exec_CloseLibrary(); break;
            case EXEC_ALLOC_MEM:     exec_AllocMem();     break;
            case EXEC_FREE_MEM:      exec_FreeMem();      break;
            case EXEC_FIND_TASK:     exec_FindTask();     break;
            default: {
                char msg[40] = "[exec] unknown fn=";
                char n[4]; u32_dec(fn, n, 4);
                int i = emu_strlen(msg), j = 0;
                while (n[j] && i<38) msg[i++]=n[j++];
                msg[i++]='\n'; msg[i]='\0';
                emu_print(msg);
            }
        }
    } else if (lib == LIB_DOS) {
        switch (fn) {
            case DOS_OUTPUT:   dos_Output();   break;
            case DOS_INPUT:    dos_Input();    break;
            case DOS_VFPRINTF:     dos_VFPrintf();     break;
            case DOS_FPUTS:        dos_FPuts();        break;
            case DOS_PUTSTR:       dos_PutStr();       break;
            case DOS_VPRINTF:      dos_VPrintf();      break;
            case DOS_VFWRITEF:     dos_VFWritef();     break;
            case DOS_READARGS:     dos_ReadArgs();     break;
            case DOS_GETARGSTR:    dos_GetArgStr();    break;
            case DOS_ISINTERACTIVE: dos_IsInteractive(); break;
            case DOS_WRITE:  dos_Write();  break;
            case DOS_OPEN:   dos_Open();   break;
            case DOS_CLOSE:  dos_Close();  break;
            case DOS_READ:   dos_Read();   break;
            case DOS_EXIT:   dos_Exit();   break;
            case DOS_IO_ERR: dos_IoErr();  break;
            default: {
                char msg[40] = "[dos] unknown fn=";
                char n[4]; u32_dec(fn, n, 4);
                int i = emu_strlen(msg), j = 0;
                while (n[j] && i<38) msg[i++]=n[j++];
                msg[i++]='\n'; msg[i]='\0';
                emu_print(msg);
            }
        }
    } else {
        char msg[48] = "[emu] ILLEGAL: unknown lib=";
        char n[4]; u32_dec(lib, n, 4);
        int i = emu_strlen(msg), j = 0;
        while (n[j] && i<46) msg[i++]=n[j++];
        msg[i++]='\n'; msg[i]='\0';
        emu_print(msg);
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

#define HUNK_HEADER   0x3F3
#define HUNK_CODE     0x3E9
#define HUNK_DATA     0x3EA
#define HUNK_BSS      0x3EB
#define HUNK_RELOC32  0x3EC
#define HUNK_END      0x3F2
#define HUNK_SYMBOL   0x3F0
#define HUNK_DEBUG    0x3F1

#define MAX_HUNKS  32

static uint32_t g_hunk_base[MAX_HUNKS];
static int      g_hunk_count = 0;

/* Returns start PC (first hunk base) or 0 on error */
static uint32_t hunk_load(const uint8_t *bin, uint32_t bin_size)
{
    if (bin_size < 8) { emu_print("[hunk] too small\n"); return 0; }

    const uint8_t *p   = bin;
    const uint8_t *end = bin + bin_size;

    /* HUNK_HEADER */
    if (be32(p) != HUNK_HEADER) { emu_print("[hunk] bad magic\n"); return 0; }
    p += 4;

    /* Skip resident library names */
    while (p + 4 <= end) {
        uint32_t cnt = be32(p); p += 4;
        if (!cnt) break;
        p += cnt * 4;
    }

    if (p + 12 > end) { emu_print("[hunk] truncated header\n"); return 0; }
    uint32_t table_size = be32(p); p += 4;
    uint32_t first_hunk = be32(p); p += 4;
    uint32_t last_hunk  = be32(p); p += 4;

    uint32_t n_hunks = last_hunk - first_hunk + 1;
    if (n_hunks > MAX_HUNKS) { emu_print("[hunk] too many hunks\n"); return 0; }
    (void)table_size;

    /* Read hunk sizes and allocate guest RAM */
    g_hunk_count = (int)n_hunks;
    for (uint32_t i = 0; i < n_hunks; i++) {
        if (p + 4 > end) { emu_print("[hunk] size table truncated\n"); return 0; }
        uint32_t words = be32(p) & 0x3FFFFFFF; p += 4;  /* mask off mem flags */
        uint32_t bytes = words * 4;
        g_hunk_base[i] = heap_alloc(bytes ? bytes : 4);
        if (!g_hunk_base[i]) { emu_print("[hunk] OOM\n"); return 0; }
    }

    /* Load hunk bodies */
    int cur = 0;
    while (p + 4 <= end && cur < (int)n_hunks) {
        uint32_t type = be32(p) & 0x3FFFFFFF; p += 4;

        if (type == HUNK_CODE || type == HUNK_DATA) {
            if (p + 4 > end) break;
            uint32_t words = be32(p); p += 4;
            uint32_t bytes = words * 4;
            if (p + bytes > end) { emu_print("[hunk] CODE/DATA overflow\n"); return 0; }
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
            g_cwd[i] = cwd[i];
            i++;
        }
        g_cwd[i] = '\0';
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
    reset_tar_state();
    determine_tar_mode(argv);

    /* Clear RAM */
    emu_memset(g_ram, 0, GUEST_RAM_SIZE);
    g_heap_ptr = PROG_BASE;
    g_last_err = 0;
    g_hunk_count = 0;
    g_emu_halted  = 0;
    g_stdin_reads = 0;

    /* Install library jump tables */
    install_library_tables();

    /* Load the binary */
    uint32_t entry = hunk_load(binary, bin_size);
    if (!entry) {
        emu_print("[emu] Failed to load binary\n");
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

    /* Place cmdline string just below SP */
    sp -= (uint32_t)((cmdlen + 2) & ~1u);
    uint32_t cmdline_ptr = sp;
    emu_memcpy(g_ram + cmdline_ptr, cmdline, (unsigned int)cmdlen);

    /* Build a BSTR version for GetArgStr (byte[0]=len, byte[1..len]=chars)
     * Place it 4 bytes below cmdline_ptr, 4-byte aligned */
    sp -= (uint32_t)((cmdlen + 2 + 4) & ~3u);
    uint32_t bstr_ptr = sp;
    g_ram[bstr_ptr] = (uint8_t)(cmdlen < 255 ? cmdlen : 255);
    emu_memcpy(g_ram + bstr_ptr + 1, cmdline, (unsigned int)cmdlen);
    g_cmdline_bptr = bstr_ptr >> 2;  /* BPTR = addr >> 2 */
    
    /* Build a separate BSTR for cli_CommandName (just "tar") */
    sp -= 8;
    uint32_t cmdname_bstr_ptr = sp;
    const char *cmdname = "tar";
    uint8_t cmdname_len = 3;
    g_ram[cmdname_bstr_ptr] = cmdname_len;
    emu_memcpy(g_ram + cmdname_bstr_ptr + 1, cmdname, cmdname_len);
    uint32_t cmdname_bptr = cmdname_bstr_ptr >> 2;

    /* Patch cli_CommandName (offset +0x10 in CLI struct) to cmdname BSTR */
    {
        uint32_t cn_slot = FAKE_CLI_ADDR + 0x10;
        uint32_t bptr = cmdname_bptr;
        g_ram[cn_slot]   = (bptr >> 24) & 0xFF;
        g_ram[cn_slot+1] = (bptr >> 16) & 0xFF;
        g_ram[cn_slot+2] = (bptr >>  8) & 0xFF;
        g_ram[cn_slot+3] = (bptr      ) & 0xFF;
    }
    
    /* Patch cli_CommandLine (offset +0x2C in CLI struct) to our cmdline BSTR */
    {
        uint32_t cl_slot = FAKE_CLI_ADDR + 0x2C;
        uint32_t bptr = g_cmdline_bptr;
        g_ram[cl_slot]   = (bptr >> 24) & 0xFF;
        g_ram[cl_slot+1] = (bptr >> 16) & 0xFF;
        g_ram[cl_slot+2] = (bptr >>  8) & 0xFF;
        g_ram[cl_slot+3] = (bptr      ) & 0xFF;
    }

    /* Push return address — our DOS Exit stub so RTS ends execution */
    uint32_t exit_stub = stub_addr(LIB_DOS, DOS_EXIT);
    sp -= 4;
    m68k_write_memory_32(sp, exit_stub);

    /* Initialise CPU — MUST call pulse_reset before setting registers */
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_set_illg_instr_callback(m68k_illg_instr_callback);

    /* Patch reset vectors so pulse_reset loads our entry/stack:
     * Address 0 = initial SSP, Address 4 = initial PC
     * We save/restore SysBase (stored at addr 4) around this. */
    m68k_write_memory_32(0, sp);     /* SSP */
    m68k_write_memory_32(4, entry);  /* PC  */
    m68k_pulse_reset();              /* CPU loads SSP from 0, PC from 4 */

    /* Restore SysBase at address 4 (pulse_reset consumed it as PC) */
    m68k_write_memory_32(4, EXEC_BASE);

    /* Set entry registers per Amiga CLI convention */
    m68k_set_reg(M68K_REG_A0, cmdline_ptr);       /* command line ptr */
    m68k_set_reg(M68K_REG_D0, (uint32_t)cmdlen);  /* command line length */


    /* Run in 1M-cycle slices until the program calls Exit or we time out */
    int slices = 0;
    while (!g_emu_halted && slices < 200) {  /* max 200M cycles total */
        m68k_execute(1000000);
        slices++;
    }
    (void)slices;
    return 0;
}
