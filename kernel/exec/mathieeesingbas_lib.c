/*
 * mathieeesingbas_lib.c — UAOS mathieeesingbas.library Implementation
 *
 * AmigaOS mathieeesingbas.library provides IEEE 754 single-precision
 * basic floating-point operations.  ACE Basic opens this library
 * unconditionally at startup, so a working implementation is required
 * for ACE programs to compile and run.
 *
 * LVO layout matches the AmigaOS 3.x mathieeesingbas.library.
 */

#include "rom_modules.h"
#include <stdint.h>

/* =========================================================================
 * mathieeesingbas.library function indices (must match AmigaOS LVO offsets)
 * ========================================================================= */

#define MIEEESB_OPEN   1
#define MIEEESB_CLOSE  2
#define MIEEESB_SPFIX  3   /* float -> int  (round toward zero) */
#define MIEEESB_SPFLT  4   /* int   -> float */
#define MIEEESB_SPCMP  5   /* compare: D0 vs D1, returns -1/0/+1 in D0 */
#define MIEEESB_SPTST  6   /* test: returns -1/0/+1 for D0 vs 0.0 */
#define MIEEESB_SPABS  7
#define MIEEESB_SPNEG  8
#define MIEEESB_SPADD  9
#define MIEEESB_SPSUB  10
#define MIEEESB_SPMUL  11
#define MIEEESB_SPDIV  12

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
 * Function implementations
 * ========================================================================= */

static void misb_Open(M68kCPUState *cpu)
{
    cpu->d[0] = 0x00000070;  /* library base address */
}

static void misb_Close(M68kCPUState *cpu)
{
    (void)cpu;
}

static void misb_SPFix(M68kCPUState *cpu)
{
    float f = b2f(cpu->d[0]);
    cpu->d[0] = (uint32_t)(int32_t)f;  /* truncate toward zero */
}

static void misb_SPFlt(M68kCPUState *cpu)
{
    int32_t i = (int32_t)cpu->d[0];
    cpu->d[0] = f2b((float)i);
}

static void misb_SPCmp(M68kCPUState *cpu)
{
    float a = b2f(cpu->d[0]);
    float b = b2f(cpu->d[1]);
    if (a < b)      cpu->d[0] = (uint32_t)-1;
    else if (a > b) cpu->d[0] = 1;
    else            cpu->d[0] = 0;
}

static void misb_SPTst(M68kCPUState *cpu)
{
    float a = b2f(cpu->d[0]);
    if (a < 0.0f)      cpu->d[0] = (uint32_t)-1;
    else if (a > 0.0f) cpu->d[0] = 1;
    else               cpu->d[0] = 0;
}

static void misb_SPAbs(M68kCPUState *cpu)
{
    cpu->d[0] &= 0x7FFFFFFF;  /* clear sign bit */
}

static void misb_SPNeg(M68kCPUState *cpu)
{
    cpu->d[0] ^= 0x80000000;  /* flip sign bit */
}

static void misb_SPAdd(M68kCPUState *cpu)
{
    float a = b2f(cpu->d[0]);
    float b = b2f(cpu->d[1]);
    cpu->d[0] = f2b(a + b);
}

static void misb_SPSub(M68kCPUState *cpu)
{
    float a = b2f(cpu->d[0]);
    float b = b2f(cpu->d[1]);
    cpu->d[0] = f2b(a - b);
}

static void misb_SPMul(M68kCPUState *cpu)
{
    float a = b2f(cpu->d[0]);
    float b = b2f(cpu->d[1]);
    cpu->d[0] = f2b(a * b);
}

static void misb_SPDiv(M68kCPUState *cpu)
{
    float a = b2f(cpu->d[0]);
    float b = b2f(cpu->d[1]);
    if (b == 0.0f) {
        cpu->d[0] = (cpu->d[0] & 0x80000000) | 0x7F800000;  /* signed inf */
        return;
    }
    cpu->d[0] = f2b(a / b);
}

/* =========================================================================
 * Function table
 * ========================================================================= */

static void *misb_funcs[] = {
    misb_Open,    /* index 1  */
    misb_Close,   /* index 2  */
    misb_SPFix,   /* index 3  */
    misb_SPFlt,   /* index 4  */
    misb_SPCmp,   /* index 5  */
    misb_SPTst,   /* index 6  */
    misb_SPAbs,   /* index 7  */
    misb_SPNeg,   /* index 8  */
    misb_SPAdd,   /* index 9  */
    misb_SPSub,   /* index 10 */
    misb_SPMul,   /* index 11 */
    misb_SPDiv,   /* index 12 */
};

/* =========================================================================
 * Registration
 * ========================================================================= */

void UAOS_MATHIEEESINGBAS_Register(void)
{
    UAOS_ROM_Register("mathieeesingbas.library", 40, 0x00000070,
                      (uint16_t)(sizeof(misb_funcs) / sizeof(misb_funcs[0])),
                      misb_funcs);
}
