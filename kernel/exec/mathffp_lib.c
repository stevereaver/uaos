/*
 * mathffp_lib.c — UAOS mathffp.library Implementation
 *
 * AmigaOS mathffp.library provides software floating-point operations
 * for Motorola 68000 CPUs without FPU. This is a native implementation
 * for UAOS using IEEE 754 single-precision float.
 */

#include "rom_modules.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
/* Note: <math.h> is not available in freestanding kernel environment */

/* =========================================================================
 * mathffp.library function indices (must match AmigaOS LVO offsets)
 * ========================================================================= */

#define MATHFFP_OPEN_LIBRARY   1
#define MATHFFP_CLOSE_LIBRARY  2
#define MATHFFP_SP_ADD         3
#define MATHFFP_SP_SUB         4
#define MATHFFP_SP_MUL         5
#define MATHFFP_SP_DIV         6
#define MATHFFP_SP_CMP         7
#define MATHFFP_SP_NEG         8
#define MATHFFP_SP_ABS         9
#define MATHFFP_SP_FIX         10
#define MATHFFP_SP_FLT         11
#define MATHFFP_SP_SQRT        12
#define MATHFFP_SP_LOG         13
#define MATHFFP_SP_EXP         14
#define MATHFFP_SP_SIN         15
#define MATHFFP_SP_COS         16
#define MATHFFP_SP_TAN         17
#define MATHFFP_SP_ATAN        18
#define MATHFFP_SP_ASIN        19
#define MATHFFP_SP_ACOS        20

/* =========================================================================
 * IEEE 754 single-precision conversion helpers
 * ========================================================================= */

/* Convert IEEE 754 float to raw bits */
static inline uint32_t float_to_bits(float f)
{
    union { float f; uint32_t u; } conv;
    conv.f = f;
    return conv.u;
}

/* Convert raw bits to IEEE 754 float */
static inline float bits_to_float(uint32_t u)
{
    union { float f; uint32_t u; } conv;
    conv.u = u;
    return conv.f;
}

/* =========================================================================
 * mathffp.library function implementations
 * ========================================================================= */

static void mathffp_OpenLibrary(M68kCPUState *cpu)
{
    cpu->d[0] = 0x00000060;  /* mathffp.library base address */
}

static void mathffp_CloseLibrary(M68kCPUState *cpu)
{
    (void)cpu;
}

static void mathffp_SPAdd(M68kCPUState *cpu)
{
    /* SPAdd - single precision addition
     * D0 = operand1, D1 = operand2
     * Returns: D0 = result */
    float a = bits_to_float(cpu->d[0]);
    float b = bits_to_float(cpu->d[1]);
    cpu->d[0] = float_to_bits(a + b);
}

static void mathffp_SPSub(M68kCPUState *cpu)
{
    /* SPSub - single precision subtraction
     * D0 = operand1, D1 = operand2
     * Returns: D0 = result */
    float a = bits_to_float(cpu->d[0]);
    float b = bits_to_float(cpu->d[1]);
    cpu->d[0] = float_to_bits(a - b);
}

static void mathffp_SPMul(M68kCPUState *cpu)
{
    /* SPMul - single precision multiplication
     * D0 = operand1, D1 = operand2
     * Returns: D0 = result */
    float a = bits_to_float(cpu->d[0]);
    float b = bits_to_float(cpu->d[1]);
    cpu->d[0] = float_to_bits(a * b);
}

static void mathffp_SPDiv(M68kCPUState *cpu)
{
    /* SPDiv - single precision division
     * D0 = dividend, D1 = divisor
     * Returns: D0 = result */
    float a = bits_to_float(cpu->d[0]);
    float b = bits_to_float(cpu->d[1]);
    if (b == 0.0f) {
        /* Division by zero - return infinity or max value */
        cpu->d[0] = (cpu->d[0] & 0x80000000) | 0x7F800000;
        return;
    }
    cpu->d[0] = float_to_bits(a / b);
}

static void mathffp_SPCmp(M68kCPUState *cpu)
{
    /* SPCmp - single precision comparison
     * D0 = operand1, D1 = operand2
     * Returns: D0 = -1 if D0 < D1, 0 if equal, 1 if D0 > D1 */
    float a = bits_to_float(cpu->d[0]);
    float b = bits_to_float(cpu->d[1]);

    if (a < b) cpu->d[0] = (uint32_t)-1;
    else if (a > b) cpu->d[0] = 1;
    else cpu->d[0] = 0;
}

static void mathffp_SPNeg(M68kCPUState *cpu)
{
    /* SPNeg - single precision negation
     * D0 = operand
     * Returns: D0 = -operand */
    cpu->d[0] ^= 0x80000000;  /* Flip sign bit */
}

static void mathffp_SPAbs(M68kCPUState *cpu)
{
    /* SPAbs - single precision absolute value
     * D0 = operand
     * Returns: D0 = |operand| */
    cpu->d[0] &= 0x7FFFFFFF;  /* Clear sign bit */
}

static void mathffp_SPFix(M68kCPUState *cpu)
{
    /* SPFix - convert float to integer (round toward zero)
     * D0 = float operand
     * Returns: D0 = integer result */
    float f = bits_to_float(cpu->d[0]);
    int32_t result = (int32_t)f;  /* C cast truncates toward zero */
    cpu->d[0] = (uint32_t)result;
}

static void mathffp_SPFlt(M68kCPUState *cpu)
{
    /* SPFlt - convert integer to float
     * D0 = integer operand
     * Returns: D0 = float result */
    int32_t i = (int32_t)cpu->d[0];
    cpu->d[0] = float_to_bits((float)i);
}

/* Advanced math functions (sqrt, log, exp, trig) are not implemented
 * in this freestanding kernel environment. The math library provides
 * only basic arithmetic: Add, Sub, Mul, Div, Neg, Abs, Fix, Flt.
 * Applications requiring advanced math should use software implementations.
 */

static void mathffp_SPSqrt(M68kCPUState *cpu)
{
    /* SPSqrt - not implemented in freestanding environment */
    cpu->d[0] = 0x7FC00000;  /* NaN */
}

static void mathffp_SPLog(M68kCPUState *cpu)
{
    /* SPLog - not implemented in freestanding environment */
    cpu->d[0] = 0xFF800000;  /* -Infinity */
}

static void mathffp_SPExp(M68kCPUState *cpu)
{
    /* SPExp - not implemented in freestanding environment */
    cpu->d[0] = 0x7F800000;  /* +Infinity */
}

static void mathffp_SPSin(M68kCPUState *cpu)
{
    /* SPSin - not implemented in freestanding environment */
    cpu->d[0] = 0x7FC00000;  /* NaN */
}

static void mathffp_SPCos(M68kCPUState *cpu)
{
    /* SPCos - not implemented in freestanding environment */
    cpu->d[0] = 0x7FC00000;  /* NaN */
}

static void mathffp_SPTan(M68kCPUState *cpu)
{
    /* SPTan - not implemented in freestanding environment */
    cpu->d[0] = 0x7FC00000;  /* NaN */
}

static void mathffp_SPAtan(M68kCPUState *cpu)
{
    /* SPAtan - not implemented in freestanding environment */
    cpu->d[0] = 0x7FC00000;  /* NaN */
}

static void mathffp_SPAsin(M68kCPUState *cpu)
{
    /* SPAsin - not implemented in freestanding environment */
    cpu->d[0] = 0x7FC00000;  /* NaN */
}

static void mathffp_SPAcos(M68kCPUState *cpu)
{
    /* SPAcos - not implemented in freestanding environment */
    cpu->d[0] = 0x7FC00000;  /* NaN */
}

/* =========================================================================
 * Function table
 * ========================================================================= */

static void *mathffp_funcs[] = {
    mathffp_OpenLibrary,   /* index 1  */
    mathffp_CloseLibrary,  /* index 2  */
    mathffp_SPAdd,         /* index 3  */
    mathffp_SPSub,         /* index 4  */
    mathffp_SPMul,         /* index 5  */
    mathffp_SPDiv,         /* index 6  */
    mathffp_SPCmp,         /* index 7  */
    mathffp_SPNeg,         /* index 8  */
    mathffp_SPAbs,         /* index 9  */
    mathffp_SPFix,         /* index 10 */
    mathffp_SPFlt,         /* index 11 */
    mathffp_SPSqrt,        /* index 12 */
    mathffp_SPLog,         /* index 13 */
    mathffp_SPExp,         /* index 14 */
    mathffp_SPSin,         /* index 15 */
    mathffp_SPCos,         /* index 16 */
    mathffp_SPTan,         /* index 17 */
    mathffp_SPAtan,        /* index 18 */
    mathffp_SPAsin,        /* index 19 */
    mathffp_SPAcos,        /* index 20 */
};

/* =========================================================================
 * Registration function
 * ========================================================================= */

void UAOS_MATHFFP_Register(void)
{
    UAOS_ROM_Register("mathffp.library", 40, 0x00000060,
                      (uint16_t)(sizeof(mathffp_funcs) / sizeof(mathffp_funcs[0])),
                      mathffp_funcs);
}
