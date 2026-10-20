/*
 * printer_device.c — UAOS printer.device Implementation (UAOS-358)
 *
 * AmigaOS printer.device writes text/escape-stream output and raster
 * dumps to the configured port device (parallel or serial).  Here the
 * byte stream forwards to the host print handler — PRT:'s LPT1 strober
 * when the hardware exists, its ring-buffer sink otherwise — so guest
 * prints appear exactly where PRT: output goes.
 *
 * Commands (devices/printer.h):
 *   CMD_WRITE / PRD_RAWWRITE — raw byte stream to the printer port.
 *   PRD_PRTCOMMAND          — emit the ISO/DEC escape sequence for a
 *                             printer command index + four parm bytes.
 *   PRD_DUMPRPORT           — RastPort dump request; accepted and
 *                             completed (no raster pipeline yet).
 *   PRD_QUERY               — printer attributes query; reports a
 *                             generic 80-column text printer.
 *   PDCMD_QUERY/SETPARAMS are parallel.device commands on the same
 *   numbers as PRD_RAWWRITE/PRD_PRTCOMMAND — disambiguated by device
 *   node: "parallel.device" registers alongside on the same backend.
 *
 * Guest memory is big-endian — pr_r/pr_w helpers only.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include "rom_modules.h"
#include "task.h"
#include "dos/print_handler.h"
#include "../../emulation/uaos_emu.h"

/* =========================================================================
 * Guest structure offsets (big-endian)
 * ========================================================================= */

#define IO_COMMAND       28
#define IO_FLAGS         30
#define IO_ERROR         31
#define IO_ACTUAL        32    /* IOStdReq.io_Actual   */
#define IO_LENGTH        36    /* IOStdReq.io_Length   */
#define IO_DATA          40    /* IOStdReq.io_Data     */
#define IOSTD_SIZE       48

/* IOPrtCmdReq (devices/printer.h) */
#define IOPC_COMMAND     32    /* UWORD io_PrtCommand  */
#define IOPC_PARM0       34    /* UBYTE io_Parm0..3    */
#define IOPC_SIZE        38

/* IODRPReq */
#define IODRP_RASTPORT   32
#define IODRP_SPECIAL    60
#define IODRP_SIZE       62

/* IOExtPar (devices/parallel.h) */
#define IOPAR_PEXTFLAGS  48
#define IOPAR_STATUS     52    /* UBYTE io_Status      */
#define IOPAR_PARFLAGS   53    /* UBYTE io_ParFlags    */
#define IOPAR_PTERM      54    /* IOPArray (2 ULONGs)  */
#define IOPAR_SIZE       62

/* io_Status bits */
#define IOPTF_PARBUSY    0x01
#define IOPTF_PAPEROUT   0x02
#define IOPTF_PARSEL     0x04
#define IOPTF_RWDIR      0x08  /* 0=read 1=write       */

/* io_ParFlags bits */
#define PARF_SHARED      0x20

/* =========================================================================
 * printer.device commands (exec/io.h + devices/printer.h)
 * ========================================================================= */

#define CMD_RESET              1
#define CMD_READ               2
#define CMD_WRITE              3
#define CMD_UPDATE             4
#define CMD_CLEAR              5
#define CMD_STOP               6
#define CMD_START              7
#define CMD_FLUSH              8
#define CMD_NONSTD             9

#define PRD_RAWWRITE        (CMD_NONSTD + 0)   /* 9  */
#define PRD_PRTCOMMAND      (CMD_NONSTD + 1)   /* 10 */
#define PRD_DUMPRPORT       (CMD_NONSTD + 2)   /* 11 */
#define PRD_QUERY           (CMD_NONSTD + 3)   /* 12 */

/* parallel.device commands (devices/parallel.h) — same numbers as
 * PRD_RAWWRITE/PRD_PRTCOMMAND; disambiguated by device node. */
#define PDCMD_QUERY         (CMD_NONSTD + 0)   /* 9  */
#define PDCMD_SETPARAMS     (CMD_NONSTD + 1)   /* 10 */

#define IOERR_OPENFAIL     (-1)
#define IOERR_NOCMD        (-3)

/* =========================================================================
 * Guest memory helpers — big-endian
 * ========================================================================= */

extern uint8_t *g_ram;

static uint16_t pr_r16(uint8_t *ram, uint32_t a)
{
    return (uint16_t)(((uint16_t)ram[a] << 8) | ram[a + 1]);
}

static uint32_t pr_r32(uint8_t *ram, uint32_t a)
{
    return ((uint32_t)ram[a] << 24) | ((uint32_t)ram[a + 1] << 16) |
           ((uint32_t)ram[a + 2] <<  8) |  (uint32_t)ram[a + 3];
}

static void pr_w16(uint8_t *ram, uint32_t a, uint16_t v)
{
    ram[a] = (uint8_t)(v >> 8); ram[a + 1] = (uint8_t)v;
}

/* =========================================================================
 * Output path — bytes go where PRT: output goes
 * ========================================================================= */

static uint32_t pr_emit_bytes(uint32_t data, uint32_t len)
{
    if (!data || data >= GUEST_RAM_SIZE) return 0;
    if (data + len > GUEST_RAM_SIZE) len = GUEST_RAM_SIZE - data;
    uint32_t sent = 0;
    for (; sent < len; sent++)
        PrintHandler_SendByte(g_ram[data + sent]);
    return sent;   /* when LPT1 is absent the handler swallows into its
                    * ring — report the full length either way, like a
                    * spooler would */
}

/* =========================================================================
 * PRD_PRTCOMMAND — ISO/DEC escape translation
 * =========================================================================
 * The guest sends a printer.h command index (aRIS..aRAW) plus up to four
 * parm bytes.  Common sequences are emitted verbatim; parametrized
 * sequences interpolate io_Parm0; unimplemented commands are accepted
 * silently so programs that probe capabilities keep running. */

static void pr_emit_esc(uint8_t b1, uint8_t b2, int two)
{
    PrintHandler_SendByte(0x1B);
    PrintHandler_SendByte(b1);
    if (two) PrintHandler_SendByte(b2);
}

static void pr_command(uint16_t cmd, const uint8_t *parm)
{
    switch (cmd) {
    case  0: pr_emit_esc('c', 0, 0); break;            /* aRIS    reset   */
    case  1: pr_emit_esc('#', '1', 1); break;          /* aRIN    init    */
    case  2: pr_emit_esc('D', 0, 0); break;            /* aIND    lf      */
    case  3: pr_emit_esc('E', 0, 0); break;            /* aNEL    cr+lf   */
    case  4: pr_emit_esc('M', 0, 0); break;            /* aRI     rev lf  */

    case  5: { /* aSGR0  normal char set — ESC[0m */
        PrintHandler_SendByte(0x1B); PrintHandler_SendByte('[');
        PrintHandler_SendByte('0');  PrintHandler_SendByte('m');
        break;
    }
    case  6: case  7: case  8: case  9:
    case 10: case 11: { /* aSGR3/23/4/24/1/22 — ESC[<n>m style toggles */
        static const uint8_t code[] = { '3', '2', '4', '2', '1', '2' };
        static const uint8_t tens[] = {  0,   2,   0,   2,   0,   2  };
        int i = cmd - 6;
        PrintHandler_SendByte(0x1B); PrintHandler_SendByte('[');
        if (tens[i]) PrintHandler_SendByte((uint8_t)('0' + tens[i]));
        PrintHandler_SendByte(code[i]);
        PrintHandler_SendByte('m');
        break;
    }
    case 67: pr_emit_esc('H', 0, 0); break;            /* aHTS    htab    */
    case 68: pr_emit_esc('J', 0, 0); break;            /* aVTS    vtab    */
    case 57: { /* aSLPP — ESC[<parm0>t set form length */
        PrintHandler_SendByte(0x1B); PrintHandler_SendByte('[');
        PrintHandler_SendByte((uint8_t)('0' + (parm[0] / 10)));
        PrintHandler_SendByte((uint8_t)('0' + (parm[0] % 10)));
        PrintHandler_SendByte('t');
        break;
    }
    default:
        /* Unimplemented printer command — accept silently (most index
         * slots are driver-specific anyway). */
        break;
    }
    (void)parm;
}

/* =========================================================================
 * Device vectors (M68kCPUState marshalled by the glue's emu_rom_call)
 * ========================================================================= */

static uint32_t g_prt_open_cnt;

static void printer_OpenDevice(M68kCPUState *cpu)
{
    /* Open (-6): A1 = IORequest, D0 = unit, D1 = flags — unit 0 only. */
    uint32_t io = cpu->a[1];
    if (cpu->d[0] != 0) {
        if (io && io + IOSTD_SIZE <= GUEST_RAM_SIZE)
            g_ram[io + IO_ERROR] = (uint8_t)IOERR_OPENFAIL;
        cpu->d[0] = (uint32_t)IOERR_OPENFAIL;
        return;
    }
    g_prt_open_cnt++;
    if (io && io + IOSTD_SIZE <= GUEST_RAM_SIZE)
        g_ram[io + IO_ERROR] = 0;
    cpu->d[0] = 0;
}

static void printer_CloseDevice(M68kCPUState *cpu)
{
    /* Close (-12): A1 = IORequest */
    if (g_prt_open_cnt) g_prt_open_cnt--;
    cpu->d[0] = 0;
}

static void printer_BeginIO(M68kCPUState *cpu)
{
    /* BeginIO (-30): A1 = IORequest (extended per command) */
    uint32_t io = cpu->a[1];
    if (!io || io + IOSTD_SIZE > GUEST_RAM_SIZE) {
        cpu->d[0] = (uint32_t)-1;
        return;
    }

    uint16_t cmd = pr_r16(g_ram, io + IO_COMMAND);

    switch (cmd) {
    case CMD_WRITE:
    case PRD_RAWWRITE: {
        /* Byte stream to the printer port. */
        uint32_t data = pr_r32(g_ram, io + IO_DATA);
        uint32_t len  = pr_r32(g_ram, io + IO_LENGTH);
        uint32_t n    = pr_emit_bytes(data, len);
        g_ram[io + IO_ACTUAL]     = (uint8_t)(n >> 24);
        g_ram[io + IO_ACTUAL + 1] = (uint8_t)(n >> 16);
        g_ram[io + IO_ACTUAL + 2] = (uint8_t)(n >> 8);
        g_ram[io + IO_ACTUAL + 3] = (uint8_t)n;
        g_ram[io + IO_ERROR] = 0;
        cpu->d[0] = 0;
        break;
    }

    case PRD_PRTCOMMAND: {
        /* IOPrtCmdReq — translate a printer.h command index to ESCs. */
        uint16_t pc = pr_r16(g_ram, io + IOPC_COMMAND);
        uint8_t parm[4];
        for (int i = 0; i < 4; i++)
            parm[i] = g_ram[io + IOPC_PARM0 + (uint32_t)i];
        pr_command(pc, parm);
        g_ram[io + IO_ERROR] = 0;
        cpu->d[0] = 0;
        break;
    }

    case PRD_DUMPRPORT: {
        /* IODRPReq — raster dump.  No raster-to-printer pipeline yet;
         * SPECIAL_NOPRINT(0x2000) callers take this path too.  Accept
         * and complete cleanly. */
        g_ram[io + IO_ERROR] = 0;
        cpu->d[0] = 0;
        break;
    }

    case PRD_QUERY: {
        /* io_Data -> printer info.  Report a conservative status word:
         * printer present (PRT: exists), 80 cols, no colour. */
        uint32_t data = pr_r32(g_ram, io + IO_DATA);
        if (data && data + 4 <= GUEST_RAM_SIZE) {
            g_ram[data + 0] = 0;
            g_ram[data + 1] = 0;
            g_ram[data + 2] = 0;
            g_ram[data + 3] = (uint8_t)PrintHandler_IsPresent();
        }
        g_ram[io + IO_ACTUAL] = 0; g_ram[io + IO_ACTUAL + 1] = 0;
        g_ram[io + IO_ACTUAL + 2] = 0; g_ram[io + IO_ACTUAL + 3] = 4;
        g_ram[io + IO_ERROR] = 0;
        cpu->d[0] = 0;
        break;
    }

    case CMD_READ:
        /* Printers don't read back. */
        g_ram[io + IO_ERROR] = (uint8_t)IOERR_NOCMD;
        cpu->d[0] = (uint32_t)IOERR_NOCMD;
        break;

    case CMD_RESET:
    case CMD_FLUSH:
    case CMD_STOP:
    case CMD_START:
    case CMD_UPDATE:
    case CMD_CLEAR:
        g_ram[io + IO_ERROR] = 0;
        cpu->d[0] = 0;
        break;

    default:
        g_ram[io + IO_ERROR] = (uint8_t)IOERR_NOCMD;
        cpu->d[0] = (uint32_t)IOERR_NOCMD;
        break;
    }
}

static void printer_AbortIO(M68kCPUState *cpu)
{
    /* AbortIO (-36): all requests complete inside BeginIO — nothing to
     * abort; report not-found like keyboard.device does. */
    (void)cpu->a[1];
    cpu->d[0] = (uint32_t)-1;
}

/* =========================================================================
 * Function table + registration
 * ========================================================================= */

static void *printer_funcs[] = {
    printer_OpenDevice,   /* index 1  */
    printer_CloseDevice,  /* index 2  */
    printer_BeginIO,      /* index 3  */
    printer_AbortIO,      /* index 4  */
};

/* Device vectors -> printer_funcs[] indices (canonical exec/io.h
 * device vectors; -42/-48 aliases kept for the old UAOS map). */
static const UaosRomLvo printer_lvo_map[] = {
    {  -6, 1 },   /* Open     */
    { -12, 2 },   /* Close    */
    { -30, 3 },   /* BeginIO  */
    { -36, 4 },   /* AbortIO  */
    { -42, 3 },   /* legacy BeginIO alias */
    { -48, 4 },   /* legacy AbortIO alias */
};

void UAOS_PRINTER_Register(void)
{
    UAOS_ROM_Register("printer.device", 40, 0,
                      (uint16_t)(sizeof(printer_funcs) / sizeof(printer_funcs[0])),
                      printer_funcs);
    UAOS_ROM_BindLvoMap("printer.device", printer_lvo_map,
                        (uint16_t)(sizeof(printer_lvo_map) / sizeof(printer_lvo_map[0])));
}

/* =========================================================================
 * parallel.device — the port layer underneath printer.device
 * =========================================================================
 * Same LPT1/ring backend, parallel.h command semantics: CMD_WRITE is
 * the byte stream (like a real port write), PDCMD_QUERY fills
 * IOExtPar.io_Status with the port status bits, PDCMD_SETPARAMS latches
 * io_ParFlags/io_PTermArray.  Reads return io_Actual=0 (no read-side
 * hardware behind LPT1's strober).
 */

static uint8_t  g_par_flags;        /* latched io_ParFlags             */
static uint32_t g_par_term[2];      /* latched io_PTermArray           */
static uint8_t  g_par_last_dir = IOPTF_RWDIR;   /* we only ever write  */
static uint32_t g_par_open_cnt;

static void parallel_OpenDevice(M68kCPUState *cpu)
{
    /* Open (-6): A1 = IOExtPar, D0 = unit, D1 = flags (PARF_*).
     * Exclusive unless PARF_SHARED — the printer port is single-owner,
     * but with only one emulated port be permissive: accept shared and
     * exclusive opens alike, refusing only non-zero units. */
    uint32_t io = cpu->a[1];
    if (cpu->d[0] != 0) {
        if (io && io + IOPAR_SIZE <= GUEST_RAM_SIZE)
            g_ram[io + IO_ERROR] = (uint8_t)IOERR_OPENFAIL;
        cpu->d[0] = (uint32_t)IOERR_OPENFAIL;
        return;
    }
    g_par_open_cnt++;
    if (io && io + IOPAR_SIZE <= GUEST_RAM_SIZE) {
        g_ram[io + IO_ERROR] = 0;
        g_ram[io + IOPAR_PARFLAGS] = g_par_flags |
                                     (uint8_t)(cpu->d[1] & PARF_SHARED);
    }
    cpu->d[0] = 0;
}

static void parallel_CloseDevice(M68kCPUState *cpu)
{
    /* Close (-12): A1 = IORequest */
    if (g_par_open_cnt) g_par_open_cnt--;
    cpu->d[0] = 0;
}

static void parallel_BeginIO(M68kCPUState *cpu)
{
    /* BeginIO (-30): A1 = IOExtPar */
    uint32_t io = cpu->a[1];
    if (!io || io + IOPAR_SIZE > GUEST_RAM_SIZE) {
        cpu->d[0] = (uint32_t)-1;
        return;
    }

    uint16_t cmd = pr_r16(g_ram, io + IO_COMMAND);

    switch (cmd) {
    case CMD_WRITE: {
        /* Byte stream out the parallel port. */
        uint32_t data = pr_r32(g_ram, io + IO_DATA);
        uint32_t len  = pr_r32(g_ram, io + IO_LENGTH);
        uint32_t n    = pr_emit_bytes(data, len);
        g_ram[io + IO_ACTUAL]     = (uint8_t)(n >> 24);
        g_ram[io + IO_ACTUAL + 1] = (uint8_t)(n >> 16);
        g_ram[io + IO_ACTUAL + 2] = (uint8_t)(n >> 8);
        g_ram[io + IO_ACTUAL + 3] = (uint8_t)n;
        g_par_last_dir = IOPTF_RWDIR;
        g_ram[io + IO_ERROR] = 0;
        cpu->d[0] = 0;
        break;
    }

    case CMD_READ:
        /* No read hardware behind the strober — a completed zero-length
         * read like a real port with nothing driving the data lines. */
        g_ram[io + IO_ACTUAL] = g_ram[io + IO_ACTUAL + 1] =
        g_ram[io + IO_ACTUAL + 2] = g_ram[io + IO_ACTUAL + 3] = 0;
        g_par_last_dir = 0;
        g_ram[io + IO_ERROR] = 0;
        cpu->d[0] = 0;
        break;

    case PDCMD_QUERY: {
        /* Fill io_Status: printer selected when LPT1 answered the
         * probe, PAPEROUT when it didn't; the port is never busy since
         * the ring always accepts. */
        uint8_t st = g_par_last_dir;
        if (PrintHandler_IsPresent()) st |= IOPTF_PARSEL;
        else                          st |= IOPTF_PAPEROUT;
        g_ram[io + IOPAR_STATUS] = st;
        g_ram[io + IO_ERROR] = 0;
        cpu->d[0] = 0;
        break;
    }

    case PDCMD_SETPARAMS: {
        /* Latch io_ParFlags + io_PTermArray — they only matter for
         * read termination, which never produces bytes here. */
        g_par_flags  = g_ram[io + IOPAR_PARFLAGS];
        g_par_term[0] = pr_r32(g_ram, io + IOPAR_PTERM);
        g_par_term[1] = pr_r32(g_ram, io + IOPAR_PTERM + 4);
        g_ram[io + IO_ERROR] = 0;
        cpu->d[0] = 0;
        break;
    }

    case CMD_RESET:
    case CMD_FLUSH:
    case CMD_STOP:
    case CMD_START:
    case CMD_UPDATE:
    case CMD_CLEAR:
        g_ram[io + IO_ERROR] = 0;
        cpu->d[0] = 0;
        break;

    default:
        g_ram[io + IO_ERROR] = (uint8_t)IOERR_NOCMD;
        cpu->d[0] = (uint32_t)IOERR_NOCMD;
        break;
    }
}

static void parallel_AbortIO(M68kCPUState *cpu)
{
    (void)cpu->a[1];
    cpu->d[0] = (uint32_t)-1;
}

static void *parallel_funcs[] = {
    parallel_OpenDevice,   /* index 1  */
    parallel_CloseDevice,  /* index 2  */
    parallel_BeginIO,      /* index 3  */
    parallel_AbortIO,      /* index 4  */
};

static const UaosRomLvo parallel_lvo_map[] = {
    {  -6, 1 },   /* Open     */
    { -12, 2 },   /* Close    */
    { -30, 3 },   /* BeginIO  */
    { -36, 4 },   /* AbortIO  */
    { -42, 3 },   /* legacy BeginIO alias */
    { -48, 4 },   /* legacy AbortIO alias */
};

void UAOS_PARALLEL_Register(void)
{
    UAOS_ROM_Register("parallel.device", 40, 0,
                      (uint16_t)(sizeof(parallel_funcs) / sizeof(parallel_funcs[0])),
                      parallel_funcs);
    UAOS_ROM_BindLvoMap("parallel.device", parallel_lvo_map,
                        (uint16_t)(sizeof(parallel_lvo_map) / sizeof(parallel_lvo_map[0])));
}

/* =========================================================================
 * Self-test — run from the boot battery (selftest cmdline)
 * ========================================================================= */

extern void kprint(const char *s);
extern void dos_AllocMem_glue(uint32_t size, uint32_t reqs, uint32_t *out_addr);

#define PRT_ALLOCF (0x00000001u | 0x00010000u)   /* MEMF_PUBLIC | CLEAR */

int uaos_printdev_test(void)
{
    int errors = 0;
    M68kCPUState cpu;
    memset(&cpu, 0, sizeof(cpu));

    uint32_t io = 0, buf = 0, q = 0, par = 0;
    dos_AllocMem_glue(IOPAR_SIZE, PRT_ALLOCF, &io);
    dos_AllocMem_glue(64, PRT_ALLOCF, &buf);
    dos_AllocMem_glue(16, PRT_ALLOCF, &q);
    dos_AllocMem_glue(IOPAR_SIZE, PRT_ALLOCF, &par);
    if (!io || !buf || !q || !par) {
        kprint("[printdev] FAIL: alloc\n");
        return 0;
    }

    /* --- printer.device --------------------------------------------- */
    cpu.d[0] = 0; cpu.a[1] = io;
    printer_OpenDevice(&cpu);
    if (cpu.d[0] != 0 || g_ram[io + IO_ERROR] != 0) {
        errors++; kprint("[printdev] FAIL: prt open\n");
    }
    cpu.d[0] = 1;
    printer_OpenDevice(&cpu);
    if ((int32_t)cpu.d[0] != IOERR_OPENFAIL) {
        errors++; kprint("[printdev] FAIL: prt open unit1\n");
    }
    cpu.d[0] = 0;

    /* CMD_WRITE + PRD_RAWWRITE byte streams. */
    memcpy(g_ram + buf, "PRT", 3);
    g_ram[io + IO_FLAGS] = UIOF_QUICK;
    pr_w16(g_ram, io + IO_COMMAND, CMD_WRITE);
    /* io_Data/io_Length land at the IOStdReq fields */
    g_ram[io + IO_DATA + 0] = (uint8_t)(buf >> 24);
    g_ram[io + IO_DATA + 1] = (uint8_t)(buf >> 16);
    g_ram[io + IO_DATA + 2] = (uint8_t)(buf >> 8);
    g_ram[io + IO_DATA + 3] = (uint8_t)buf;
    g_ram[io + IO_LENGTH + 3] = 3;
    printer_BeginIO(&cpu);
    if (g_ram[io + IO_ERROR] != 0 ||
        g_ram[io + IO_ACTUAL + 3] != 3) {
        errors++; kprint("[printdev] FAIL: CMD_WRITE\n");
    }
    pr_w16(g_ram, io + IO_COMMAND, PRD_RAWWRITE);
    g_ram[io + IO_LENGTH + 3] = 0;
    g_ram[io + IO_LENGTH + 2] = 0;
    g_ram[io + IO_LENGTH + 3] = 3;
    printer_BeginIO(&cpu);
    if (g_ram[io + IO_ERROR] != 0) {
        errors++; kprint("[printdev] FAIL: RAWWRITE\n");
    }

    /* PRD_PRTCOMMAND — aRIS translates to ESC'c'. */
    pr_w16(g_ram, io + IO_COMMAND, PRD_PRTCOMMAND);
    pr_w16(g_ram, io + IOPC_COMMAND, 0);   /* aRIS */
    printer_BeginIO(&cpu);
    if (g_ram[io + IO_ERROR] != 0) {
        errors++; kprint("[printdev] FAIL: PRTCOMMAND\n");
    }

    /* PRD_DUMPRPORT — accepted, completed. */
    pr_w16(g_ram, io + IO_COMMAND, PRD_DUMPRPORT);
    printer_BeginIO(&cpu);
    if (g_ram[io + IO_ERROR] != 0) {
        errors++; kprint("[printdev] FAIL: DUMPRPORT\n");
    }

    /* PRD_QUERY fills a 4-byte status word. */
    g_ram[io + IO_DATA + 3] = (uint8_t)q;
    g_ram[io + IO_DATA + 2] = (uint8_t)(q >> 8);
    pr_w16(g_ram, io + IO_COMMAND, PRD_QUERY);
    printer_BeginIO(&cpu);
    if (g_ram[io + IO_ERROR] != 0 ||
        g_ram[io + IO_ACTUAL + 3] != 4) {
        errors++; kprint("[printdev] FAIL: PRD_QUERY\n");
    }

    /* Printers don't read. */
    pr_w16(g_ram, io + IO_COMMAND, CMD_READ);
    printer_BeginIO(&cpu);
    if (g_ram[io + IO_ERROR] != (uint8_t)IOERR_NOCMD) {
        errors++; kprint("[printdev] FAIL: read nocmd\n");
    }

    /* --- parallel.device -------------------------------------------- */
    cpu.d[0] = 0; cpu.a[1] = par;
    parallel_OpenDevice(&cpu);
    if (cpu.d[0] != 0 || g_ram[par + IO_ERROR] != 0) {
        errors++; kprint("[printdev] FAIL: par open\n");
    }

    /* PDCMD_QUERY — status byte reflects the LPT1 probe. */
    pr_w16(g_ram, par + IO_COMMAND, PDCMD_QUERY);
    parallel_BeginIO(&cpu);
    {
        uint8_t st = g_ram[par + IOPAR_STATUS];
        uint8_t expect = PrintHandler_IsPresent() ? IOPTF_PARSEL
                                                  : IOPTF_PAPEROUT;
        if (g_ram[par + IO_ERROR] != 0 || !(st & expect)) {
            errors++; kprint("[printdev] FAIL: PDCMD_QUERY\n");
        }
    }

    /* PDCMD_SETPARAMS latches flags; a fresh open echoes them. */
    g_ram[par + IOPAR_PARFLAGS] = 0x02;   /* PARF_ACKMODE */
    pr_w16(g_ram, par + IO_COMMAND, PDCMD_SETPARAMS);
    parallel_BeginIO(&cpu);
    if (g_ram[par + IO_ERROR] != 0) {
        errors++; kprint("[printdev] FAIL: SETPARAMS\n");
    }
    uint32_t par2 = 0;
    dos_AllocMem_glue(IOPAR_SIZE, PRT_ALLOCF, &par2);
    if (par2) {
        cpu.d[0] = 0; cpu.d[1] = PARF_SHARED; cpu.a[1] = par2;
        parallel_OpenDevice(&cpu);
        if (g_ram[par2 + IOPAR_PARFLAGS] != (0x02 | PARF_SHARED)) {
            errors++; kprint("[printdev] FAIL: params not latched\n");
        }
        cpu.d[1] = 0;
    }
    cpu.a[1] = par;

    /* Port write + zero-length read. */
    memcpy(g_ram + buf, "PAR", 3);
    g_ram[par + IO_DATA + 3] = (uint8_t)buf;
    g_ram[par + IO_DATA + 2] = (uint8_t)(buf >> 8);
    g_ram[par + IO_LENGTH + 3] = 3;
    pr_w16(g_ram, par + IO_COMMAND, CMD_WRITE);
    parallel_BeginIO(&cpu);
    if (g_ram[par + IO_ERROR] != 0 ||
        g_ram[par + IO_ACTUAL + 3] != 3) {
        errors++; kprint("[printdev] FAIL: par write\n");
    }
    pr_w16(g_ram, par + IO_COMMAND, CMD_READ);
    parallel_BeginIO(&cpu);
    if (g_ram[par + IO_ERROR] != 0 ||
        g_ram[par + IO_ACTUAL + 3] != 0) {
        errors++; kprint("[printdev] FAIL: par read\n");
    }

    /* AbortIO on an idle device reports not-found. */
    parallel_AbortIO(&cpu);
    if ((int32_t)cpu.d[0] != -1) {
        errors++; kprint("[printdev] FAIL: par abort\n");
    }

    return errors == 0;
}
