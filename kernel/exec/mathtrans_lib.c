/*
 * mathtrans_lib.c — UAOS mathtrans.library Implementation
 *
 * AmigaOS mathtrans.library provides IEEE 754 single-precision
 * transcendental functions (sin, cos, tan, sqrt, exp, log, etc.).
 * ACE Basic opens this library unconditionally at startup.
 *
 * All functions are implemented with freestanding C (no <math.h>)
 * using the shared helpers in float_math.c (Taylor series and
 * Newton-Raphson iterations).
 *
 * LVO layout matches the AmigaOS 3.x mathtrans.library.
 */

#include "rom_modules.h"
#include "float_math.h"
#include <stdint.h>

/* =========================================================================
 * mathtrans.library function indices (must match AmigaOS LVO offsets)
 * ========================================================================= */

#define MT_OPEN     1
#define MT_CLOSE    2
#define MT_SPSIN    3
#define MT_SPCOS    4
#define MT_SPTAN    5
#define MT_SPSINCOS 6   /* returns sin; stores cos at A0 */
#define MT_SPASIN   7
#define MT_SPACOS   8
#define MT_SPATAN   9
#define MT_SPEXP    10
#define MT_SPLOG    11   /* natural log (ln) */
#define MT_SPLOG10  12
#define MT_SPSQRT   13
#define MT_SPFLOOR  14
#define MT_SPCEIL   15
#define MT_SPPOW    16

/* =========================================================================
 * IEEE 754 single-precision conversion helpers
 * ========================================================================= */

static inline uint32_t f2b(float f)
{
    union { float f; uint32_t u; } c;
    c.f = f;
    return c.u;
}

static inline float b2f(uint32_t u)
{
    union { float f; uint32_t u; } c;
    c.u = u;
    return c.f;
}

/* =========================================================================
 * Library function wrappers (M68k register interface)
 * ========================================================================= */

static void mt_Open(M68kCPUState *cpu)
{
    cpu->d[0] = 0x00000080;
}

static void mt_Close(M68kCPUState *cpu)
{
    (void)cpu;
}

static void mt_SPSin(M68kCPUState *cpu)
{
    cpu->d[0] = f2b(uaos_sinf(b2f(cpu->d[0])));
}

static void mt_SPCos(M68kCPUState *cpu)
{
    cpu->d[0] = f2b(uaos_cosf(b2f(cpu->d[0])));
}

static void mt_SPTan(M68kCPUState *cpu)
{
    cpu->d[0] = f2b(uaos_tanf(b2f(cpu->d[0])));
}

static void mt_SPSincos(M68kCPUState *cpu)
{
    float x = b2f(cpu->d[0]);
    /* Returns sin in D0, stores cos at the address in A0 */
    float c = uaos_cosf(x);
    uint32_t addr = cpu->a[0];
    /* Write cos as a 32-bit IEEE float to guest memory */
    extern uint8_t *uaos_ram_base;
    if (uaos_ram_base && addr < 0x200000) {
        uint32_t bits = f2b(c);
        uaos_ram_base[addr+0] = (bits >> 24) & 0xFF;
        uaos_ram_base[addr+1] = (bits >> 16) & 0xFF;
        uaos_ram_base[addr+2] = (bits >> 8)  & 0xFF;
        uaos_ram_base[addr+3] =  bits        & 0xFF;
    }
    cpu->d[0] = f2b(uaos_sinf(x));
}

static void mt_SPAsin(M68kCPUState *cpu)
{
    cpu->d[0] = f2b(uaos_asinf(b2f(cpu->d[0])));
}

static void mt_SPAcos(M68kCPUState *cpu)
{
    cpu->d[0] = f2b(uaos_acosf(b2f(cpu->d[0])));
}

static void mt_SPAtan(M68kCPUState *cpu)
{
    cpu->d[0] = f2b(uaos_atanf(b2f(cpu->d[0])));
}

static void mt_SPExp(M68kCPUState *cpu)
{
    cpu->d[0] = f2b(uaos_expf(b2f(cpu->d[0])));
}

static void mt_SPLog(M68kCPUState *cpu)
{
    cpu->d[0] = f2b(uaos_logf(b2f(cpu->d[0])));
}

static void mt_SPLog10(M68kCPUState *cpu)
{
    float ln = uaos_logf(b2f(cpu->d[0]));
    cpu->d[0] = f2b(ln / UAOS_FLT_LN10);
}

static void mt_SPSqrt(M68kCPUState *cpu)
{
    cpu->d[0] = f2b(uaos_sqrtf(b2f(cpu->d[0])));
}

static void mt_SPFloor(M68kCPUState *cpu)
{
    cpu->d[0] = f2b(uaos_floorf(b2f(cpu->d[0])));
}

static void mt_SPCeil(M68kCPUState *cpu)
{
    cpu->d[0] = f2b(uaos_ceilf(b2f(cpu->d[0])));
}

static void mt_SPPow(M68kCPUState *cpu)
{
    float base = b2f(cpu->d[0]);
    float exp  = b2f(cpu->d[1]);
    cpu->d[0] = f2b(uaos_powf(base, exp));
}

/* =========================================================================
 * Function table
 * ========================================================================= */

static void *mt_funcs[] = {
    mt_Open,     /* index 1  */
    mt_Close,    /* index 2  */
    mt_SPSin,    /* index 3  */
    mt_SPCos,    /* index 4  */
    mt_SPTan,    /* index 5  */
    mt_SPSincos, /* index 6  */
    mt_SPAsin,   /* index 7  */
    mt_SPAcos,   /* index 8  */
    mt_SPAtan,   /* index 9  */
    mt_SPExp,    /* index 10 */
    mt_SPLog,    /* index 11 */
    mt_SPLog10,  /* index 12 */
    mt_SPSqrt,   /* index 13 */
    mt_SPFloor,  /* index 14 */
    mt_SPCeil,   /* index 15 */
    mt_SPPow,    /* index 16 */
};

/* =========================================================================
 * Registration
 * ========================================================================= */

void UAOS_MATHTRANS_Register(void)
{
    UAOS_ROM_Register("mathtrans.library", 40, 0x00000080,
                      (uint16_t)(sizeof(mt_funcs) / sizeof(mt_funcs[0])),
                      mt_funcs);
}
