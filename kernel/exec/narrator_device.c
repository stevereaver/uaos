/*
 * narrator_device.c — UAOS narrator.device Implementation (UAOS-358)
 *
 * AmigaOS narrator.device turns text (or phonemes) into synthesized
 * speech on the audio channels.  The synthesized output path is a
 * no-op for now — the device accepts speech requests with full
 * narrator_rb field validation and completes them io_Error=0, which
 * is enough for programs that OpenDevice/BeginIO to check for speech
 * capability without wedging on IOERR_NOCMD.
 *
 * Commands: the standard device command set only — CMD_WRITE carries
 * a narrator_rb (speech request), CMD_READ carries a mouth_rb (mouth
 * sync shapes; requires a prior write per ND_NoWrite).  Everything
 * else reports ND_Unimpl.
 *
 * Guest memory is big-endian — nr_r/nr_w helpers only.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include "rom_modules.h"
#include "task.h"
#include "../../emulation/uaos_emu.h"

/* =========================================================================
 * Guest structure offsets (big-endian)
 * ========================================================================= */

#define IO_COMMAND       28
#define IO_FLAGS         30
#define IO_ERROR         31
#define IO_ACTUAL        32
#define IO_LENGTH        36
#define IO_DATA          40
#define IOSTD_SIZE       48

/* struct narrator_rb (devices/narrator.h) — fields after IOStdReq */
#define NR_RATE          (IOSTD_SIZE +  0)   /* UWORD rate          */
#define NR_PITCH         (IOSTD_SIZE +  2)   /* UWORD pitch         */
#define NR_MODE          (IOSTD_SIZE +  4)   /* UWORD mode          */
#define NR_SEX           (IOSTD_SIZE +  6)   /* UWORD sex           */
#define NR_CHMASKS       (IOSTD_SIZE +  8)   /* UBYTE *ch_masks     */
#define NR_NMMASKS       (IOSTD_SIZE + 12)   /* UWORD nm_masks      */
#define NR_VOLUME        (IOSTD_SIZE + 14)   /* UWORD volume        */
#define NR_SAMPFREQ      (IOSTD_SIZE + 16)   /* UWORD sampfreq      */
#define NR_MOUTHS        (IOSTD_SIZE + 18)   /* UBYTE mouths        */
#define NR_RB_SIZE       (IOSTD_SIZE + 40)   /* narrator_rb = 88    */

/* struct mouth_rb (devices/narrator.h) — after narrator_rb */
#define MOUTH_WIDTH      (NR_RB_SIZE + 0)    /* UBYTE width         */
#define MOUTH_HEIGHT     (NR_RB_SIZE + 1)    /* UBYTE height        */
#define MOUTH_SHAPE      (NR_RB_SIZE + 2)    /* UBYTE shape         */
#define MOUTH_SYNC       (NR_RB_SIZE + 3)    /* UBYTE sync          */

/* =========================================================================
 * narrator.device commands + error codes
 * ========================================================================= */

#define CMD_RESET              1
#define CMD_READ               2
#define CMD_WRITE              3
#define CMD_UPDATE             4
#define CMD_CLEAR              5
#define CMD_STOP               6
#define CMD_START              7
#define CMD_FLUSH              8

#define ND_NoWrite      (-8)    /* read for mouth without write first  */
#define ND_UnitErr      (-5)    /* unit other than 0                   */
#define ND_Unimpl       (-7)    /* unimplemented command               */
#define ND_RateErr     (-21)
#define ND_PitchErr    (-22)
#define ND_SexErr      (-23)
#define ND_ModeErr     (-24)
#define ND_FreqErr     (-25)
#define ND_VolErr      (-26)

#define IOERR_OPENFAIL (-1)

/* Parameter bounds (devices/narrator.h) */
#define MINRATE     40
#define MAXRATE     400
#define MINPITCH    65
#define MAXPITCH    320
#define MINFREQ     5000
#define MAXFREQ     28000
#define MINVOL      0
#define MAXVOL      64

/* =========================================================================
 * Guest memory helpers — big-endian
 * ========================================================================= */

extern uint8_t *g_ram;

static uint16_t nr_r16(uint8_t *ram, uint32_t a)
{
    return (uint16_t)(((uint16_t)ram[a] << 8) | ram[a + 1]);
}

/* =========================================================================
 * Device vectors (M68kCPUState marshalled by the glue's emu_rom_call)
 * ========================================================================= */

static uint32_t g_nar_open_cnt;
static int      g_nar_spoke;    /* a CMD_WRITE landed (for mouth reads) */

static void narrator_OpenDevice(M68kCPUState *cpu)
{
    /* Open (-6): A1 = IORequest, D0 = unit, D1 = flags — unit 0 only. */
    uint32_t io = cpu->a[1];
    if (cpu->d[0] != 0) {
        if (io && io + IOSTD_SIZE <= GUEST_RAM_SIZE)
            g_ram[io + IO_ERROR] = (uint8_t)ND_UnitErr;
        cpu->d[0] = (uint32_t)IOERR_OPENFAIL;
        return;
    }
    g_nar_open_cnt++;
    g_nar_spoke = 0;
    if (io && io + IOSTD_SIZE <= GUEST_RAM_SIZE)
        g_ram[io + IO_ERROR] = 0;
    cpu->d[0] = 0;
}

static void narrator_CloseDevice(M68kCPUState *cpu)
{
    /* Close (-12): A1 = IORequest */
    if (g_nar_open_cnt) g_nar_open_cnt--;
    cpu->d[0] = 0;
}

/* Validate the narrator_rb fields that callers actually vary; a zero
 * field means "use the default" and always validates.  Returns the
 * NDK error code (negative) or 0. */
static int nar_validate(uint32_t io)
{
    uint16_t rate  = nr_r16(g_ram, io + NR_RATE);
    uint16_t pitch = nr_r16(g_ram, io + NR_PITCH);
    uint16_t mode  = nr_r16(g_ram, io + NR_MODE);
    uint16_t sex   = nr_r16(g_ram, io + NR_SEX);
    uint16_t vol   = nr_r16(g_ram, io + NR_VOLUME);
    uint16_t freq  = nr_r16(g_ram, io + NR_SAMPFREQ);
    if (rate  && (rate  < MINRATE  || rate  > MAXRATE))  return ND_RateErr;
    if (pitch && (pitch < MINPITCH || pitch > MAXPITCH)) return ND_PitchErr;
    if (sex   && sex > 1)                                return ND_SexErr;
    if (mode  && mode > 2)                               return ND_ModeErr;
    if (freq  && (freq < MINFREQ || freq > MAXFREQ))     return ND_FreqErr;
    if (vol > MAXVOL)                                    return ND_VolErr;
    return 0;
}

static void narrator_BeginIO(M68kCPUState *cpu)
{
    /* BeginIO (-30): A1 = IORequest (extended per command) */
    uint32_t io = cpu->a[1];
    if (!io || io + IOSTD_SIZE > GUEST_RAM_SIZE) {
        cpu->d[0] = (uint32_t)-1;
        return;
    }

    uint16_t cmd = nr_r16(g_ram, io + IO_COMMAND);
    if ((cmd == CMD_WRITE || cmd == CMD_READ) &&
        io + NR_RB_SIZE > GUEST_RAM_SIZE) {
        cpu->d[0] = (uint32_t)-1;
        return;
    }

    switch (cmd) {
    case CMD_WRITE: {
        /* Speech request — validate the narrator_rb then accept.
         * Synthesis itself is a no-op: the request completes io_Error=0
         * as if the text were spoken. */
        int err = nar_validate(io);
        if (!err) g_nar_spoke = 1;
        g_ram[io + IO_ERROR] = (uint8_t)err;
        cpu->d[0] = (uint32_t)err;
        break;
    }

    case CMD_READ: {
        /* mouth_rb — mouth sync requires a prior speech write. */
        if (!g_nar_spoke) {
            g_ram[io + IO_ERROR] = (uint8_t)ND_NoWrite;
            cpu->d[0] = (uint32_t)ND_NoWrite;
            break;
        }
        /* Report a neutral mouth (closed) — sync events aren't
         * generated without synthesis. */
        g_ram[io + MOUTH_WIDTH]  = 0;
        g_ram[io + MOUTH_HEIGHT] = 0;
        g_ram[io + MOUTH_SHAPE]   = 0;
        g_ram[io + MOUTH_SYNC]   = 0;
        g_ram[io + IO_ERROR]     = 0;
        cpu->d[0] = 0;
        break;
    }

    case CMD_RESET:
        g_nar_spoke = 0;
        g_ram[io + IO_ERROR] = 0;
        cpu->d[0] = 0;
        break;

    case CMD_STOP:
    case CMD_START:
    case CMD_FLUSH:
    case CMD_UPDATE:
    case CMD_CLEAR:
        g_ram[io + IO_ERROR] = 0;
        cpu->d[0] = 0;
        break;

    default:
        g_ram[io + IO_ERROR] = (uint8_t)ND_Unimpl;
        cpu->d[0] = (uint32_t)ND_Unimpl;
        break;
    }
}

static void narrator_AbortIO(M68kCPUState *cpu)
{
    /* AbortIO (-36): requests complete inside BeginIO — nothing pending. */
    (void)cpu->a[1];
    cpu->d[0] = (uint32_t)-1;
}

/* =========================================================================
 * Function table + registration
 * ========================================================================= */

static void *narrator_funcs[] = {
    narrator_OpenDevice,   /* index 1  */
    narrator_CloseDevice,  /* index 2  */
    narrator_BeginIO,      /* index 3  */
    narrator_AbortIO,      /* index 4  */
};

/* Device vectors -> narrator_funcs[] indices (canonical exec/io.h
 * device vectors; -42/-48 aliases kept for the old UAOS map). */
static const UaosRomLvo narrator_lvo_map[] = {
    {  -6, 1 },   /* Open     */
    { -12, 2 },   /* Close    */
    { -30, 3 },   /* BeginIO  */
    { -36, 4 },   /* AbortIO  */
    { -42, 3 },   /* legacy BeginIO alias */
    { -48, 4 },   /* legacy AbortIO alias */
};

void UAOS_NARRATOR_Register(void)
{
    UAOS_ROM_Register("narrator.device", 37, 0,
                      (uint16_t)(sizeof(narrator_funcs) / sizeof(narrator_funcs[0])),
                      narrator_funcs);
    UAOS_ROM_BindLvoMap("narrator.device", narrator_lvo_map,
                        (uint16_t)(sizeof(narrator_lvo_map) / sizeof(narrator_lvo_map[0])));
}

/* =========================================================================
 * Self-test — run from the boot battery (selftest cmdline)
 * ========================================================================= */

extern void kprint(const char *s);
extern void dos_AllocMem_glue(uint32_t size, uint32_t reqs, uint32_t *out_addr);

#define NAR_ALLOCF (0x00000001u | 0x00010000u)   /* MEMF_PUBLIC | CLEAR */

static void nar_w16(uint8_t *ram, uint32_t a, uint16_t v)
{
    ram[a] = (uint8_t)(v >> 8); ram[a + 1] = (uint8_t)v;
}

int uaos_narrdev_test(void)
{
    int errors = 0;
    M68kCPUState cpu;
    memset(&cpu, 0, sizeof(cpu));

    uint32_t io = 0, txt = 0;
    dos_AllocMem_glue(NR_RB_SIZE + 8, NAR_ALLOCF, &io);
    dos_AllocMem_glue(32, NAR_ALLOCF, &txt);
    if (!io || !txt) {
        kprint("[narrdev] FAIL: alloc\n");
        return 0;
    }

    /* Open: unit 0 ok, unit 1 is ND_UnitErr. */
    cpu.d[0] = 0; cpu.a[1] = io;
    narrator_OpenDevice(&cpu);
    if (cpu.d[0] != 0 || g_ram[io + IO_ERROR] != 0) {
        errors++; kprint("[narrdev] FAIL: open unit0\n");
    }
    cpu.d[0] = 1;
    narrator_OpenDevice(&cpu);
    if ((int32_t)cpu.d[0] != IOERR_OPENFAIL ||
        g_ram[io + IO_ERROR] != (uint8_t)ND_UnitErr) {
        errors++; kprint("[narrdev] FAIL: open unit1\n");
    }
    cpu.d[0] = 0;

    /* A mouth read before any speech write is ND_NoWrite. */
    g_ram[io + IO_FLAGS] = UIOF_QUICK;
    nar_w16(g_ram, io + IO_COMMAND, CMD_READ);
    narrator_BeginIO(&cpu);
    if (g_ram[io + IO_ERROR] != (uint8_t)ND_NoWrite) {
        errors++; kprint("[narrdev] FAIL: read-before-write\n");
    }

    /* A well-formed speech request completes io_Error=0. */
    memcpy(g_ram + txt, "hello", 5);
    nar_w16(g_ram, io + IO_COMMAND, CMD_WRITE);
    nar_w16(g_ram, io + NR_RATE, 150);      /* DEFRATE   */
    nar_w16(g_ram, io + NR_PITCH, 110);     /* DEFPITCH  */
    nar_w16(g_ram, io + NR_MODE, 0);        /* NATURALF0 */
    nar_w16(g_ram, io + NR_SEX, 0);         /* MALE      */
    nar_w16(g_ram, io + NR_VOLUME, 64);     /* DEFVOL    */
    nar_w16(g_ram, io + NR_SAMPFREQ, 22200);/* DEFFREQ   */
    g_ram[io + IO_DATA + 3] = (uint8_t)txt;
    g_ram[io + IO_DATA + 2] = (uint8_t)(txt >> 8);
    g_ram[io + IO_LENGTH + 3] = 5;
    narrator_BeginIO(&cpu);
    if (g_ram[io + IO_ERROR] != 0) {
        errors++; kprint("[narrdev] FAIL: speech write\n");
    }

    /* Now the mouth read completes with a neutral shape. */
    nar_w16(g_ram, io + IO_COMMAND, CMD_READ);
    narrator_BeginIO(&cpu);
    if (g_ram[io + IO_ERROR] != 0 ||
        g_ram[io + MOUTH_WIDTH] != 0 || g_ram[io + MOUTH_HEIGHT] != 0) {
        errors++; kprint("[narrdev] FAIL: mouth read\n");
    }

    /* Out-of-bounds fields produce the NDK errors. */
    nar_w16(g_ram, io + IO_COMMAND, CMD_WRITE);
    nar_w16(g_ram, io + NR_RATE, 999);
    narrator_BeginIO(&cpu);
    if (g_ram[io + IO_ERROR] != (uint8_t)ND_RateErr) {
        errors++; kprint("[narrdev] FAIL: rate err\n");
    }
    nar_w16(g_ram, io + NR_RATE, 150);
    nar_w16(g_ram, io + NR_PITCH, 999);
    narrator_BeginIO(&cpu);
    if (g_ram[io + IO_ERROR] != (uint8_t)ND_PitchErr) {
        errors++; kprint("[narrdev] FAIL: pitch err\n");
    }
    nar_w16(g_ram, io + NR_PITCH, 110);

    /* Unknown command -> ND_Unimpl. */
    nar_w16(g_ram, io + IO_COMMAND, 99);
    narrator_BeginIO(&cpu);
    if (g_ram[io + IO_ERROR] != (uint8_t)ND_Unimpl) {
        errors++; kprint("[narrdev] FAIL: unimpl\n");
    }

    /* AbortIO — nothing pending, reports not-found. */
    narrator_AbortIO(&cpu);
    if ((int32_t)cpu.d[0] != -1) {
        errors++; kprint("[narrdev] FAIL: abort\n");
    }

    return errors == 0;
}
