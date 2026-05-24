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

/* =========================================================================
 * Shell output callback — set by UAOS_Emu_LoadAndRun_Internal
 * ========================================================================= */

typedef void (*GluePrintFn)(const char *s);
static GluePrintFn g_print = (void*)0;

static void emu_print(const char *s)
{
    if (g_print) g_print(s);
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
#define STACK_TOP       0x001000
#define PROG_BASE       0x001000

static uint8_t g_ram[GUEST_RAM_SIZE];

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
#define JMPTAB_LIB_SZ   64      /* 16 slots × 4 bytes per lib */

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

/* Build the stub: ILLEGAL word followed by (lib<<8|func) word */
static void install_stub(int lib_id, int func_idx)
{
    uint32_t addr = JMPTAB_BASE + (uint32_t)(lib_id-1) * JMPTAB_LIB_SZ
                                + (uint32_t)(func_idx-1) * 4;
    g_ram[addr]   = 0x4A; g_ram[addr+1] = 0xFC; /* ILLEGAL */
    g_ram[addr+2] = (uint8_t)lib_id;
    g_ram[addr+3] = (uint8_t)func_idx;
}

static uint32_t stub_addr(int lib_id, int func_idx)
{
    return JMPTAB_BASE + (uint32_t)(lib_id-1) * JMPTAB_LIB_SZ
                       + (uint32_t)(func_idx-1) * 4;
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

#define EXEC_BASE   0x0200

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
#define DOS_BASE    0x0280

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
    /* exec.library at EXEC_BASE */
    install_lvo(EXEC_BASE, LVO_OPEN_LIBRARY,  LIB_EXEC, EXEC_OPEN_LIBRARY);
    install_lvo(EXEC_BASE, LVO_CLOSE_LIBRARY, LIB_EXEC, EXEC_CLOSE_LIBRARY);
    install_lvo(EXEC_BASE, LVO_ALLOC_MEM,     LIB_EXEC, EXEC_ALLOC_MEM);
    install_lvo(EXEC_BASE, LVO_FREE_MEM,      LIB_EXEC, EXEC_FREE_MEM);
    install_lvo(EXEC_BASE, LVO_FIND_TASK,     LIB_EXEC, EXEC_FIND_TASK);

    /* dos.library at DOS_BASE */
    install_lvo(DOS_BASE, LVO_DOS_OUTPUT, LIB_DOS, DOS_OUTPUT);
    install_lvo(DOS_BASE, LVO_DOS_WRITE,  LIB_DOS, DOS_WRITE);
    install_lvo(DOS_BASE, LVO_DOS_OPEN,   LIB_DOS, DOS_OPEN);
    install_lvo(DOS_BASE, LVO_DOS_CLOSE,  LIB_DOS, DOS_CLOSE);
    install_lvo(DOS_BASE, LVO_DOS_READ,   LIB_DOS, DOS_READ);
    install_lvo(DOS_BASE, LVO_DOS_EXIT,   LIB_DOS, DOS_EXIT);
    install_lvo(DOS_BASE, LVO_DOS_IO_ERR, LIB_DOS, DOS_IO_ERR);

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
    int match = 0;
    for (int j = 0; name[j] && "dos.library"[j]; j++)
        if (name[j] == "dos.library"[j]) match++;
    uint32_t result = 0;
    if (match == 11) result = DOS_BASE;   /* "dos.library" */

    m68k_set_reg(M68K_REG_D0, result);
    if (!result) {
        char msg[80];
        int mi = 0;
        const char *pre = "[exec] OpenLibrary: unknown: ";
        while (*pre && mi < 79) msg[mi++] = *pre++;
        for (int j = 0; name[j] && mi < 79; j++) msg[mi++] = name[j];
        msg[mi++] = '\n'; msg[mi] = '\0';
        emu_print(msg);
    }
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
    /* Return a fake task pointer */
    m68k_set_reg(M68K_REG_D0, 0x300);
}

/* =========================================================================
 * dos.library implementation
 * ========================================================================= */

/* File handle table — maps dos handle numbers to output (1=stdout) */
#define DOS_STDOUT  1
#define DOS_STDERR  2

static void dos_Output(void)
{
    /* Returns the stdout file handle (BPTR) */
    m68k_set_reg(M68K_REG_D0, DOS_STDOUT);
}

static void dos_Write(void)
{
    /* D1=fh (BPTR), D2=buffer ptr, D3=length */
    uint32_t fh  = m68k_get_reg(NULL, M68K_REG_D1);
    uint32_t ptr = m68k_get_reg(NULL, M68K_REG_D2);
    uint32_t len = m68k_get_reg(NULL, M68K_REG_D3);

    (void)fh; /* route all output to shell */

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

static void dos_Open(void)
{
    /* Stub: return 0 (failure) — file I/O not yet supported */
    m68k_set_reg(M68K_REG_D0, 0);
    g_last_err = 205; /* ERROR_OBJECT_NOT_FOUND */
}

static void dos_Close(void) { m68k_set_reg(M68K_REG_D0, (uint32_t)-1); }

static void dos_Read(void) { m68k_set_reg(M68K_REG_D0, (uint32_t)-1); }

static void dos_Exit(void)
{
    uint32_t rc = m68k_get_reg(NULL, M68K_REG_D1);
    char buf[32] = "[emu] Process exited, rc=";
    char num[12]; u32_dec(rc, num, 12);
    int i = emu_strlen(buf), j = 0;
    while (num[j] && i < 30) buf[i++] = num[j++];
    buf[i++] = '\n'; buf[i] = '\0';
    emu_print(buf);
    /* Stop the CPU */
    m68k_pulse_halt();
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
            case DOS_OUTPUT: dos_Output(); break;
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
    g_heap_ptr = PROG_BASE;
    g_last_err = 0;
    g_hunk_count = 0;

    /* Install library jump tables */
    install_library_tables();

    /* Load the binary */
    uint32_t entry = hunk_load(binary, bin_size);
    if (!entry) {
        emu_print("[emu] Failed to load binary\n");
        return -1;
    }

    {
        char msg[64] = "[emu] Loaded, entry=0x";
        char hex[9]; u32_hex(entry, hex);
        int i = emu_strlen(msg), j = 0;
        while (hex[j] && i<62) msg[i++]=hex[j++];
        msg[i++]='\n'; msg[i]='\0';
        emu_print(msg);
    }

    /* ---- Build Amiga-style process startup stack ----
     * The Amiga startup convention (as seen by a DOS program):
     *   SP at entry:
     *     +0: return address (to DOS Exit stub)
     *     +4: argc  (number of args including name)
     *     +8: argv  (pointer to array of pointers)
     *
     * We build this on the guest stack.
     */
    uint32_t sp = STACK_TOP;

    /* Count args */
    int argc = 0;
    if (argv) while (argv[argc]) argc++;

    /* Push arg strings onto stack, record their addresses */
    uint32_t arg_ptrs[16];
    int narg = (argc > 15) ? 15 : argc;
    for (int i = narg - 1; i >= 0; i--) {
        sp = push_string(sp, argv[i]);
        arg_ptrs[i] = sp;
    }
    arg_ptrs[narg] = 0;

    /* Push null terminator then arg pointer array */
    sp -= 4; m68k_write_memory_32(sp, 0);  /* NULL terminator */
    for (int i = narg - 1; i >= 0; i--) {
        sp -= 4; m68k_write_memory_32(sp, arg_ptrs[i]);
    }
    uint32_t argv_ptr = sp;

    /* Push argc and argv */
    sp -= 4; m68k_write_memory_32(sp, argv_ptr);
    sp -= 4; m68k_write_memory_32(sp, (uint32_t)narg);

    /* Push return address — a DOS Exit stub */
    uint32_t exit_stub = stub_addr(LIB_DOS, DOS_EXIT);
    sp -= 4; m68k_write_memory_32(sp, exit_stub);

    /* Set up CPU */
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_pulse_reset();

    /* Set registers */
    m68k_set_reg(M68K_REG_PC, entry);
    m68k_set_reg(M68K_REG_SP, sp);
    m68k_set_reg(M68K_REG_A6, EXEC_BASE);  /* ExecBase in A6 */

    emu_print("[emu] Starting execution...\n");

    /* Run for up to 50M cycles (should be plenty for a CLI tool) */
    m68k_execute(50000000);

    emu_print("[emu] Execution complete\n");
    return 0;
}
