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
#include "dos/handler.h"
#include "dos/handle_table.h"
#include "dos/dospacket.h"
#include "dos/amiga_dos_types.h"
#include "exec/rom_modules.h"

/* =========================================================================
 * Shell output callback — set by UAOS_Emu_LoadAndRun_Internal
 * ========================================================================= */

typedef void (*GluePrintFn)(const char *s);
static GluePrintFn g_print = (void*)0;

/* Current working directory for resolving relative paths */
char g_uaos_cwd[64] = "RAM:";

static void emu_print(const char *s)
{
    if (g_print) g_print(s);
}

/* Guest memory base for BPTR-to-native conversion */
extern uint8_t g_ram[];
#define GUEST_RAM_SIZE (2 * 1024 * 1024)

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

#define GUEST_RAM_SIZE  (2 * 1024 * 1024)   /* 2 MB */
#define STACK_TOP       0x1F0000  /* top of guest stack — grows downward */
#define PROG_BASE       0x001000  /* program hunks load here */

uint8_t g_ram[GUEST_RAM_SIZE];
int      g_emu_halted   = 0;  /* set by dos_Exit to break the execute loop */
static uint32_t g_cmdline_bptr = 0;  /* BPTR to CLI arg BSTR, set at startup */

/* Bump allocator — starts after program load area.
 * Will be set to first free address after hunk loading. */
uint32_t g_uaos_heap_ptr = PROG_BASE;

static uint32_t heap_alloc(uint32_t size)
{
    /* Align to 4 bytes */
    size = (size + 3) & ~3u;
    if (g_uaos_heap_ptr + size > GUEST_RAM_SIZE) return 0;
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
#define LIB_BSDSOCKET   3
#define LIB_GRAPHICS    4
#define LIB_INTUITION   5

/* exec.library function indices */
#define EXEC_OPEN_LIBRARY   1
#define EXEC_CLOSE_LIBRARY  2
#define EXEC_ALLOC_MEM      3
#define EXEC_FREE_MEM       4
#define EXEC_FIND_TASK      5

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

/* graphics.library function indices (must match graphics_lib.c) */
#define GFX_OPEN_LIBRARY      1
#define GFX_CLOSE_LIBRARY     2
#define GFX_INIT_RASTPORT     3
#define GFX_INIT_VIEW         4
#define GFX_LOAD_VIEW         5
#define GFX_WAITTOF           6
#define GFX_RASTPORT          7
#define GFX_TEXT              8
#define GFX_TEXTFIT           9
#define GFX_TEXT_LENGTH       10
#define GFX_MOVE              11
#define GFX_DRAW              12
#define GFX_RECTFILL          13
#define GFX_POLYGON           14
#define GFX_ELLIPSE           15
#define GFX_SET_RAST          16
#define GFX_SET_APEN          17
#define GFX_SET_BPEN          18
#define GFX_SET_DRMD          19
#define GFX_SET_OPEN          20
#define GFX_SET_WRITE_MASK    21
#define GFX_BLIT              22
#define GFX_READ_PIXEL        23
#define GFX_WRITE_PIXEL       24
#define GFX_GET_BITMAP        25
#define GFX_ALLOC_BITMAP      26
#define GFX_FREE_BITMAP       27
#define GFX_LOAD_RGB4         28
#define GFX_LOAD_RGB32        29
#define GFX_GET_COLOR_MAP     30

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
#define DOS_BASE       0x0800  /* moved to 0x0800 so VFPrintf@-354 = 0x69E, clear of EXEC */
#define BSD_BASE       0x3000  /* bsdsocket.library base — clear of DOS range */
#define GRAPHICS_BASE  0x4000  /* graphics.library base — clear of BSD range */
#define INTUITION_BASE 0x5000  /* intuition.library base — clear of graphics range */

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
#define LVO_DOS_DELETEFILE (-78)
#define LVO_DOS_RENAME     (-84)
#define LVO_DOS_SETPROTECTION (-90)
#define LVO_DOS_GETVAR     (-132)
#define LVO_DOS_SETVAR     (-138)
#define LVO_DOS_SEEK       (-66)
#define LVO_DOS_LOCK       (-72)
#define LVO_DOS_UNLOCK     (-78)
#define LVO_DOS_EXAMINE    (-84)
#define LVO_DOS_EXAMINE_NEXT (-90)
#define LVO_DOS_CREATE_DIR (-96)
#define LVO_DOS_DUPLOCK    (-102)
#define LVO_DOS_PARENT     (-108)
#define LVO_DOS_DATE_STAMP (-192)
#define LVO_DOS_DELAY      (-198)
#define LVO_DOS_DATE_TO_STR (-678)

/* graphics.library LVO offsets (AmigaOS standard) */
#define LVO_GFX_INIT_RASTPORT  (-30)
#define LVO_GFX_INIT_VIEW     (-36)
#define LVO_GFX_LOAD_VIEW     (-42)
#define LVO_GFX_WAITTOF       (-48)
#define LVO_GFX_SET_RAST      (-54)
#define LVO_GFX_MOVE          (-60)
#define LVO_GFX_DRAW          (-66)
#define LVO_GFX_SET_OPEN      (-126)
#define LVO_GFX_SET_APEN      (-132)
#define LVO_GFX_SET_BPEN      (-138)
#define LVO_GFX_SET_DRMD      (-144)
#define LVO_GFX_TEXT          (-150)
#define LVO_GFX_TEXT_LENGTH   (-156)
#define LVO_GFX_TEXTFIT       (-162)
#define LVO_GFX_RECTFILL      (-216)
#define LVO_GFX_BLIT          (-228)
#define LVO_GFX_READ_PIXEL    (-234)
#define LVO_GFX_WRITE_PIXEL   (-240)

/* intuition.library LVO offsets */
#define LVO_INTUITION_OPEN_LIBRARY       (-30)
#define LVO_INTUITION_CLOSE_LIBRARY      (-36)
#define LVO_INTUITION_OPEN_WINDOW        (-204)
#define LVO_INTUITION_CLOSE_WINDOW       (-72)
#define LVO_INTUITION_WINDOW_TO_FRONT  (-126)
#define LVO_INTUITION_WINDOW_TO_BACK   (-132)
#define LVO_INTUITION_ACTIVATE_WINDOW  (-138)
#define LVO_INTUITION_MOVE_WINDOW      (-150)
#define LVO_INTUITION_SIZE_WINDOW      (-156)
#define LVO_INTUITION_REFRESH_WINDOW   (-162)
#define LVO_INTUITION_MODIFY_IDCMP     (-174)
#define LVO_INTUITION_SET_WINDOW_TITLES (-276)
#define LVO_INTUITION_OPEN_WINDOW_TAGS (-294)

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
            case DOS_WRITE:  return (uint32_t)((int)DOS_BASE + LVO_DOS_WRITE);
            case DOS_OPEN:   return (uint32_t)((int)DOS_BASE + LVO_DOS_OPEN);
            case DOS_CLOSE:  return (uint32_t)((int)DOS_BASE + LVO_DOS_CLOSE);
            case DOS_READ:   return (uint32_t)((int)DOS_BASE + LVO_DOS_READ);
            case DOS_EXIT:   return (uint32_t)((int)DOS_BASE + LVO_DOS_EXIT);
            case DOS_IO_ERR: return (uint32_t)((int)DOS_BASE + LVO_DOS_IO_ERR);
        }
    } else if (lib_id == LIB_GRAPHICS) {
        switch (func_idx) {
            case GFX_INIT_RASTPORT: return (uint32_t)((int)GRAPHICS_BASE + LVO_GFX_INIT_RASTPORT);
            case GFX_INIT_VIEW:     return (uint32_t)((int)GRAPHICS_BASE + LVO_GFX_INIT_VIEW);
            case GFX_LOAD_VIEW:     return (uint32_t)((int)GRAPHICS_BASE + LVO_GFX_LOAD_VIEW);
            case GFX_WAITTOF:       return (uint32_t)((int)GRAPHICS_BASE + LVO_GFX_WAITTOF);
            case GFX_SET_RAST:      return (uint32_t)((int)GRAPHICS_BASE + LVO_GFX_SET_RAST);
            case GFX_MOVE:          return (uint32_t)((int)GRAPHICS_BASE + LVO_GFX_MOVE);
            case GFX_DRAW:          return (uint32_t)((int)GRAPHICS_BASE + LVO_GFX_DRAW);
            case GFX_SET_OPEN:      return (uint32_t)((int)GRAPHICS_BASE + LVO_GFX_SET_OPEN);
            case GFX_SET_APEN:      return (uint32_t)((int)GRAPHICS_BASE + LVO_GFX_SET_APEN);
            case GFX_SET_BPEN:      return (uint32_t)((int)GRAPHICS_BASE + LVO_GFX_SET_BPEN);
            case GFX_SET_DRMD:      return (uint32_t)((int)GRAPHICS_BASE + LVO_GFX_SET_DRMD);
            case GFX_TEXT:          return (uint32_t)((int)GRAPHICS_BASE + LVO_GFX_TEXT);
            case GFX_TEXT_LENGTH:   return (uint32_t)((int)GRAPHICS_BASE + LVO_GFX_TEXT_LENGTH);
            case GFX_TEXTFIT:       return (uint32_t)((int)GRAPHICS_BASE + LVO_GFX_TEXTFIT);
            case GFX_RECTFILL:      return (uint32_t)((int)GRAPHICS_BASE + LVO_GFX_RECTFILL);
            case GFX_BLIT:          return (uint32_t)((int)GRAPHICS_BASE + LVO_GFX_BLIT);
            case GFX_READ_PIXEL:    return (uint32_t)((int)GRAPHICS_BASE + LVO_GFX_READ_PIXEL);
            case GFX_WRITE_PIXEL:   return (uint32_t)((int)GRAPHICS_BASE + LVO_GFX_WRITE_PIXEL);
        }
    } else if (lib_id == LIB_INTUITION) {
        switch (func_idx) {
            case INTUITION_OPEN_LIBRARY:      return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_OPEN_LIBRARY);
            case INTUITION_CLOSE_LIBRARY:     return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_CLOSE_LIBRARY);
            case INTUITION_OPEN_WINDOW:       return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_OPEN_WINDOW);
            case INTUITION_CLOSE_WINDOW:      return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_CLOSE_WINDOW);
            case INTUITION_WINDOW_TO_FRONT:   return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_WINDOW_TO_FRONT);
            case INTUITION_WINDOW_TO_BACK:    return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_WINDOW_TO_BACK);
            case INTUITION_ACTIVATE_WINDOW:   return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_ACTIVATE_WINDOW);
            case INTUITION_MOVE_WINDOW:       return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_MOVE_WINDOW);
            case INTUITION_SIZE_WINDOW:       return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_SIZE_WINDOW);
            case INTUITION_REFRESH_WINDOW:    return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_REFRESH_WINDOW);
            case INTUITION_MODIFY_IDCMP:      return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_MODIFY_IDCMP);
            case INTUITION_SET_WINDOW_TITLES: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_SET_WINDOW_TITLES);
            case INTUITION_OPEN_WINDOW_TAGS:  return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_OPEN_WINDOW_TAGS);
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

static void install_loadable_libs(void);

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

    /* graphics.library at GRAPHICS_BASE — pre-fill range with MOVEQ #0,D0 + RTS */
    for (int lvo = -6; lvo >= -240; lvo -= 6) {
        uint32_t addr = (uint32_t)((int)GRAPHICS_BASE + lvo);
        if (addr < GUEST_RAM_SIZE - 4) {
            g_ram[addr]   = 0x70; g_ram[addr+1] = 0x00; /* MOVEQ #0,D0 */
            g_ram[addr+2] = 0x4E; g_ram[addr+3] = 0x75; /* RTS */
        }
    }
    install_lvo(GRAPHICS_BASE, LVO_GFX_INIT_RASTPORT, LIB_GRAPHICS, GFX_INIT_RASTPORT);
    install_lvo(GRAPHICS_BASE, LVO_GFX_INIT_VIEW,     LIB_GRAPHICS, GFX_INIT_VIEW);
    install_lvo(GRAPHICS_BASE, LVO_GFX_LOAD_VIEW,     LIB_GRAPHICS, GFX_LOAD_VIEW);
    install_lvo(GRAPHICS_BASE, LVO_GFX_WAITTOF,       LIB_GRAPHICS, GFX_WAITTOF);
    install_lvo(GRAPHICS_BASE, LVO_GFX_SET_RAST,      LIB_GRAPHICS, GFX_SET_RAST);
    install_lvo(GRAPHICS_BASE, LVO_GFX_MOVE,          LIB_GRAPHICS, GFX_MOVE);
    install_lvo(GRAPHICS_BASE, LVO_GFX_DRAW,          LIB_GRAPHICS, GFX_DRAW);
    install_lvo(GRAPHICS_BASE, LVO_GFX_SET_OPEN,      LIB_GRAPHICS, GFX_SET_OPEN);
    install_lvo(GRAPHICS_BASE, LVO_GFX_SET_APEN,      LIB_GRAPHICS, GFX_SET_APEN);
    install_lvo(GRAPHICS_BASE, LVO_GFX_SET_BPEN,      LIB_GRAPHICS, GFX_SET_BPEN);
    install_lvo(GRAPHICS_BASE, LVO_GFX_SET_DRMD,      LIB_GRAPHICS, GFX_SET_DRMD);
    install_lvo(GRAPHICS_BASE, LVO_GFX_TEXT,          LIB_GRAPHICS, GFX_TEXT);
    install_lvo(GRAPHICS_BASE, LVO_GFX_TEXT_LENGTH,   LIB_GRAPHICS, GFX_TEXT_LENGTH);
    install_lvo(GRAPHICS_BASE, LVO_GFX_TEXTFIT,       LIB_GRAPHICS, GFX_TEXTFIT);
    install_lvo(GRAPHICS_BASE, LVO_GFX_RECTFILL,      LIB_GRAPHICS, GFX_RECTFILL);
    install_lvo(GRAPHICS_BASE, LVO_GFX_BLIT,          LIB_GRAPHICS, GFX_BLIT);
    install_lvo(GRAPHICS_BASE, LVO_GFX_READ_PIXEL,    LIB_GRAPHICS, GFX_READ_PIXEL);
    install_lvo(GRAPHICS_BASE, LVO_GFX_WRITE_PIXEL,   LIB_GRAPHICS, GFX_WRITE_PIXEL);

    /* intuition.library at INTUITION_BASE — pre-fill range with MOVEQ #0,D0 + RTS */
    for (int lvo = -6; lvo >= -300; lvo -= 6) {
        uint32_t addr = (uint32_t)((int)INTUITION_BASE + lvo);
        if (addr < GUEST_RAM_SIZE - 4) {
            g_ram[addr]   = 0x70; g_ram[addr+1] = 0x00; /* MOVEQ #0,D0 */
            g_ram[addr+2] = 0x4E; g_ram[addr+3] = 0x75; /* RTS */
        }
    }
    install_lvo(INTUITION_BASE, LVO_INTUITION_OPEN_LIBRARY,       LIB_INTUITION, INTUITION_OPEN_LIBRARY);
    install_lvo(INTUITION_BASE, LVO_INTUITION_CLOSE_LIBRARY,      LIB_INTUITION, INTUITION_CLOSE_LIBRARY);
    install_lvo(INTUITION_BASE, LVO_INTUITION_OPEN_WINDOW,        LIB_INTUITION, INTUITION_OPEN_WINDOW);
    install_lvo(INTUITION_BASE, LVO_INTUITION_CLOSE_WINDOW,       LIB_INTUITION, INTUITION_CLOSE_WINDOW);
    install_lvo(INTUITION_BASE, LVO_INTUITION_WINDOW_TO_FRONT,    LIB_INTUITION, INTUITION_WINDOW_TO_FRONT);
    install_lvo(INTUITION_BASE, LVO_INTUITION_WINDOW_TO_BACK,     LIB_INTUITION, INTUITION_WINDOW_TO_BACK);
    install_lvo(INTUITION_BASE, LVO_INTUITION_ACTIVATE_WINDOW,    LIB_INTUITION, INTUITION_ACTIVATE_WINDOW);
    install_lvo(INTUITION_BASE, LVO_INTUITION_MOVE_WINDOW,        LIB_INTUITION, INTUITION_MOVE_WINDOW);
    install_lvo(INTUITION_BASE, LVO_INTUITION_SIZE_WINDOW,        LIB_INTUITION, INTUITION_SIZE_WINDOW);
    install_lvo(INTUITION_BASE, LVO_INTUITION_REFRESH_WINDOW,     LIB_INTUITION, INTUITION_REFRESH_WINDOW);
    install_lvo(INTUITION_BASE, LVO_INTUITION_MODIFY_IDCMP,       LIB_INTUITION, INTUITION_MODIFY_IDCMP);
    install_lvo(INTUITION_BASE, LVO_INTUITION_SET_WINDOW_TITLES,  LIB_INTUITION, INTUITION_SET_WINDOW_TITLES);
    install_lvo(INTUITION_BASE, LVO_INTUITION_OPEN_WINDOW_TAGS,   LIB_INTUITION, INTUITION_OPEN_WINDOW_TAGS);

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
static uint32_t        g_next_loadable_base = 0x6000;

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
        if (e->base_addr + e->bin_size > GUEST_RAM_SIZE) continue;

        for (uint32_t j = 0; j < e->bin_size; j++)
            g_ram[e->base_addr + j] = e->binary[j];
        e->loaded = 1;
    }
}

/* =========================================================================
 * exec.library implementation
 * ========================================================================= */

/* g_last_err replaced by global g_dos_last_ioerr via SetIoErr()/IoErr() */

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

    const char *bsd_name = "bsdsocket.library";
    int bsd_match = 1;
    for (int j = 0; bsd_name[j]; j++)
        if (name[j] != bsd_name[j]) { bsd_match = 0; break; }
    if (bsd_match) result = BSD_BASE;

    const char *gfx_name = "graphics.library";
    int gfx_match = 1;
    for (int j = 0; gfx_name[j]; j++)
        if (name[j] != gfx_name[j]) { gfx_match = 0; break; }
    if (gfx_match) result = GRAPHICS_BASE;

    const char *intuition_name = "intuition.library";
    int intuition_match = 1;
    for (int j = 0; intuition_name[j]; j++)
        if (name[j] != intuition_name[j]) { intuition_match = 0; break; }
    if (intuition_match) result = INTUITION_BASE;

    /* Check loadable libraries (real M68k binaries loaded from disk) */
    if (result == FAKE_LIB_BASE) {
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
        int cwd_len = 0; while (g_uaos_cwd[cwd_len] && cwd_len < 63) cwd_len++;
        int i = 0; while (i < cwd_len) { full_name[i] = g_uaos_cwd[i]; i++; }
        if (cwd_len > 0 && g_uaos_cwd[cwd_len-1] != ':' && g_uaos_cwd[cwd_len-1] != '/')
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
    int32_t handle = DoPkt(port, action, (int32_t)full_name, (int32_t)mode, 0, 0, 0);
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
        int cwd_len = 0; while (g_uaos_cwd[cwd_len] && cwd_len < 63) cwd_len++;
        int i = 0; while (i < cwd_len) { full_name[i] = g_uaos_cwd[i]; i++; }
        if (cwd_len > 0 && g_uaos_cwd[cwd_len-1] != ':' && g_uaos_cwd[cwd_len-1] != '/')
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

    int32_t res = DoPkt(port, ACTION_DELETE_OBJECT, (int32_t)full_name, 0, 0, 0, 0);
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
                        (int32_t)old_name, (int32_t)new_name, 0, 0, 0);
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
        int cwd_len = 0; while (g_uaos_cwd[cwd_len] && cwd_len < 63) cwd_len++;
        int i = 0; while (i < cwd_len) { full_name[i] = g_uaos_cwd[i]; i++; }
        if (cwd_len > 0 && g_uaos_cwd[cwd_len-1] != ':' && g_uaos_cwd[cwd_len-1] != '/')
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

    int32_t res = DoPkt(port, ACTION_SET_PROTECT, (int32_t)full_name, mask, 0, 0, 0);
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
        int cwd_len = 0; while (g_uaos_cwd[cwd_len] && cwd_len < 63) cwd_len++;
        int i = 0; while (i < cwd_len) { full_name[i] = g_uaos_cwd[i]; i++; }
        if (cwd_len > 0 && g_uaos_cwd[cwd_len-1] != ':' && g_uaos_cwd[cwd_len-1] != '/')
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

    int32_t handle = DoPkt(port, ACTION_LOCATE_OBJECT, (int32_t)full_name, mode, 0, 0, 0);
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
                        (int32_t)(g_ram + fib_ptr), 0, 0, 0);
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
                        (int32_t)(g_ram + fib_ptr), 0, 0, 0);
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
        int cwd_len = 0; while (g_uaos_cwd[cwd_len] && cwd_len < 63) cwd_len++;
        int i = 0; while (i < cwd_len) { full_name[i] = g_uaos_cwd[i]; i++; }
        if (cwd_len > 0 && g_uaos_cwd[cwd_len-1] != ':' && g_uaos_cwd[cwd_len-1] != '/')
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

    int32_t res = DoPkt(port, ACTION_CREATE_DIR, (int32_t)full_name, 0, 0, 0, 0);
    m68k_set_reg(M68K_REG_D0, (uint32_t)res);
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
    m68k_set_reg(M68K_REG_D0, (uint32_t)IoErr());
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
        /* Delegate dos.library to ROM module dispatcher */
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

        void *rom_fn = UAOS_ROM_NativeFunc("dos.library", (uint16_t)fn);
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
            char msg[40] = "[dos] unknown fn=";
            char n[4]; u32_dec(fn, n, 4);
            int i = emu_strlen(msg), j = 0;
            while (n[j] && i<38) msg[i++]=n[j++];
            msg[i++]='\n'; msg[i]='\0';
            emu_print(msg);
        }
    } else if (lib == LIB_BSDSOCKET) {
        extern void BsdSocket_Dispatch(uint32_t fn, uint32_t *regs);
        BsdSocket_Dispatch((uint32_t)fn, (uint32_t*)0);
    } else if (lib == LIB_GRAPHICS) {
        extern void UAOS_Graphics_Dispatch(uint32_t fn);
        UAOS_Graphics_Dispatch((uint32_t)fn);
    } else if (lib == LIB_INTUITION) {
        extern void UAOS_Intuition_Dispatch(uint32_t fn);
        UAOS_Intuition_Dispatch((uint32_t)fn);
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
