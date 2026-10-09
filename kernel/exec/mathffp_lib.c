/*
 * mathffp_lib.c — UAOS mathffp.library Implementation
 *
 * AmigaOS mathffp.library provides software floating-point operations
 * in the Motorola Fast Floating Point (FFP) format: the top 24 bits are
 * a normalized mantissa and the low byte packs the sign (bit 7) with an
 * excess-64 exponent.  Transcendental functions convert to IEEE 754 and
 * use the shared freestanding helpers in float_math.c.
 */

#include "rom_modules.h"
#include "float_math.h"
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
#define MATHFFP_SP_TST         21
#define MATHFFP_SP_FLOOR       22
#define MATHFFP_SP_CEIL        23
#define MATHFFP_SP_SINCOS      24
#define MATHFFP_SP_SINH        25
#define MATHFFP_SP_COSH        26
#define MATHFFP_SP_TANH        27
#define MATHFFP_SP_POW         28
#define MATHFFP_SP_TIEEE       29
#define MATHFFP_SP_FIEEE       30

/* =========================================================================
 * FFP format helpers
 *
 * Layout: mantissa occupies bits 31..8 (normalized so bit 23 of that
 * field is set), the low byte is sign(bit 7) | exponent(bits 6..0).
 * value = mantissa/2^24 * 2^(exp-64), so 1.0 encodes as 0x80000041 and
 * zero is the all-zero pattern (a zero low byte means +0.0 regardless
 * of the mantissa bits).
 * ========================================================================= */

static uint32_t ffp_abs(uint32_t v)
{
    return v & ~0x80u;
}

static uint32_t ffp_neg(uint32_t v)
{
    return (uint8_t)v ? v ^ 0x80u : v;
}

static int32_t ffp_tst(uint32_t v)
{
    uint8_t es = (uint8_t)v;
    return (es & 0x80) ? -1 : (es != 0);
}

static int32_t ffp_cmp(uint32_t left, uint32_t right)
{
    int32_t lc = (int32_t)(left & 0xff);
    int32_t rc = (int32_t)(right & 0xff);
    int32_t result;
    if (lc & 0x80) lc -= 256;
    if (rc & 0x80) rc -= 256;
    if (lc != rc)
        result = lc > rc ? 1 : -1;
    else
        result = left > right ? 1 : left < right ? -1 : 0;
    return (lc < 0 && rc < 0) ? -result : result;
}

static uint32_t ffp_fix(uint32_t v)
{
    uint32_t exponent = v & 0x7f;
    uint32_t magnitude = v & 0xffffff00u;
    int negative = (v & 0x80) != 0;
    if (!(uint8_t)v)
        return magnitude;
    if (exponent < 65)
        return 0;
    if (exponent >= 96)
        return negative ? 0x80000000u : 0x7fffffffu;
    magnitude >>= 96 - exponent;
    return negative ? (uint32_t)(0u - magnitude) : magnitude;
}

static uint32_t ffp_flt(int32_t i)
{
    uint32_t mantissa = (uint32_t)i;
    uint32_t code = 96;
    if (!i)
        return 0;
    if (i < 0) {
        mantissa = 0u - mantissa;
        code |= 0x80;
    }
    while (!(mantissa & 0x80000000u)) {
        mantissa <<= 1;
        --code;
    }
    if (mantissa & 0x80) {
        uint32_t prev = mantissa;
        mantissa += 0x100;
        if (mantissa < prev) {
            mantissa = (mantissa >> 1) | 0x80000000u;
            ++code;
        }
    }
    return (mantissa & 0xffffff00u) | code;
}

static uint32_t ffp_normalize_difference(uint32_t mantissa, uint32_t code)
{
    uint32_t sign = code & 0x80;
    int32_t exponent = (int32_t)(code & 0x7f);
    mantissa &= 0xffffff00u;
    if (!mantissa)
        return 0;
    while (!(mantissa & 0x80000000u)) {
        mantissa <<= 1;
        --exponent;
    }
    if (exponent < 0 || (!sign && !exponent))
        return 0;
    return mantissa | sign | (uint32_t)exponent;
}

static uint32_t ffp_addsub(uint32_t left, uint32_t right, int subtract)
{
    uint32_t left_code = left & 0xff;
    uint32_t right_code = (right & 0xff) ^ (subtract ? 0x80 : 0);
    uint32_t left_mantissa = left & 0xffffff00u;
    uint32_t right_mantissa = right & 0xffffff00u;
    int32_t difference;
    uint32_t mantissa;
    if (!(uint8_t)right)
        return left;
    if (!left_code)
        return right_mantissa | right_code;
    difference = (int32_t)(left_code & 0x7f) - (int32_t)(right_code & 0x7f);
    if (difference < 0) {
        uint32_t swap = left_mantissa;
        left_mantissa = right_mantissa;
        right_mantissa = swap;
        swap = left_code;
        left_code = right_code;
        right_code = swap;
        difference = -difference;
    }
    if (difference >= 24)
        return left_mantissa | left_code;
    if (!((left_code ^ right_code) & 0x80)) {
        uint32_t rounded = left_mantissa | 0x80;
        mantissa = rounded + (right_mantissa >> difference);
        if (mantissa < rounded) {
            mantissa = (mantissa >> 1) | 0x80000000u;
            if ((left_code & 0x7f) == 0x7f)
                return 0xffffff00u | left_code;
            ++left_code;
        }
        return (mantissa & 0xffffff00u) | left_code;
    }
    if (!difference) {
        if (left_mantissa >= right_mantissa)
            return ffp_normalize_difference(left_mantissa - right_mantissa,
                                            left_code);
        return ffp_normalize_difference(right_mantissa - left_mantissa,
                                        right_code);
    }
    mantissa = (left_mantissa | 0x80) - (right_mantissa >> difference);
    if (mantissa & 0x80000000u)
        return (mantissa & 0xffffff00u) | left_code;
    return ffp_normalize_difference(mantissa, left_code);
}

static uint32_t ffp_mul(uint32_t a, uint32_t b)
{
    uint32_t sign = (a ^ b) & 0x80;
    int32_t exponent = (int32_t)(a & 0x7f) + (int32_t)(b & 0x7f) - 64;
    uint64_t product;
    uint32_t mantissa;
    if (!(a & 0xff) || !(b & 0xff))
        return 0;
    /* 24x24 -> 48-bit product in [2^46, 2^48); normalize to [2^47, 2^48) */
    product = (uint64_t)(a >> 8) * (b >> 8);
    if (!(product & (1ULL << 47))) {
        product <<= 1;
        --exponent;
    }
    mantissa = (uint32_t)(product >> 24);
    if (product & (1ULL << 23) && ++mantissa >> 24) {
        mantissa >>= 1;
        ++exponent;
    }
    if (exponent <= 0)
        return 0;
    if (exponent > 0x7f)
        return 0xffffff00u | 0x7fu | sign;
    return (mantissa << 8) | (uint32_t)exponent | sign;
}

static uint32_t ffp_div(uint32_t a, uint32_t b)
{
    uint32_t sign = (a ^ b) & 0x80;
    int32_t exponent = (int32_t)(a & 0x7f) - (int32_t)(b & 0x7f) + 64;
    uint64_t quotient;
    uint32_t mantissa;
    if (!(a & 0xff))
        return 0;
    if (!(b & 0xff))
        return 0xffffff00u | 0x7fu | sign;
    /* quotient of two [0.5,1) mantissas is in (0.5, 2); compute at 25-bit
     * precision then normalize to [2^24, 2^25) before the final shift. */
    quotient = ((uint64_t)(a >> 8) << 25) / (b >> 8);
    if (quotient >= (1ULL << 25)) {
        quotient >>= 1;
        ++exponent;
    }
    mantissa = (uint32_t)(quotient >> 1);
    if ((quotient & 1) && ++mantissa >> 24) {
        mantissa >>= 1;
        ++exponent;
    }
    if (exponent <= 0)
        return 0;
    if (exponent > 0x7f)
        return 0xffffff00u | 0x7fu | sign;
    return (mantissa << 8) | (uint32_t)exponent | sign;
}

static uint32_t ffp_floor(uint32_t v)
{
    uint32_t code = v & 0xff;
    uint32_t exponent = code & 0x7f;
    uint32_t magnitude, integral;
    if (code & 0x80) {
        magnitude = ffp_neg(v);
        integral = ffp_floor(magnitude);
        if ((uint8_t)ffp_addsub(magnitude, integral, 1))
            integral = ffp_addsub(integral, 0x80000041u, 0);
        return ffp_neg(integral);
    }
    if (!code || exponent >= 96)
        return v;
    if (exponent < 65)
        return 0;
    magnitude = v & 0xffffff00u;
    magnitude = (magnitude >> (96 - exponent)) << (96 - exponent);
    return magnitude | code;
}

static uint32_t ffp_ceil(uint32_t v)
{
    return ffp_neg(ffp_floor(ffp_neg(v)));
}

/* =========================================================================
 * FFP <-> IEEE 754 conversion (for the transcendental helpers)
 * ========================================================================= */

static float ffp_to_float(uint32_t v)
{
    uint8_t code = (uint8_t)v;
    int32_t e;
    float f;
    if (!code)
        return 0.0f;
    e = (int32_t)(code & 0x7f) - 64;
    f = (float)(v >> 8) / 16777216.0f;
    while (e > 0) { f *= 2.0f; --e; }
    while (e < 0) { f *= 0.5f; ++e; }
    return (code & 0x80) ? -f : f;
}

static uint32_t float_to_ffp(float f)
{
    union { float f; uint32_t u; } conv;
    uint32_t sign;
    int32_t e;
    conv.f = f;
    if (!(conv.u & 0x7fffffffu))
        return 0;
    e = (int32_t)((conv.u >> 23) & 0xff);
    sign = (conv.u >> 24) & 0x80;
    if (e == 255)
        return 0xffffff00u | 0x7fu | sign;
    if (!e)
        return 0;
    e -= 62;    /* IEEE biased exponent -> FFP excess-64 exponent */
    if (e <= 0)
        return 0;
    if (e > 0x7f)
        return 0xffffff00u | 0x7fu | sign;
    return ((conv.u & 0x007fffffu) | 0x00800000u) << 8 | (uint32_t)e | sign;
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
    /* SPAdd - FFP addition
     * D0 = operand1, D1 = operand2
     * Returns: D0 = result */
    cpu->d[0] = ffp_addsub(cpu->d[0], cpu->d[1], 0);
}

static void mathffp_SPSub(M68kCPUState *cpu)
{
    /* SPSub - FFP subtraction
     * D0 = operand1, D1 = operand2
     * Returns: D0 = result */
    cpu->d[0] = ffp_addsub(cpu->d[0], cpu->d[1], 1);
}

static void mathffp_SPMul(M68kCPUState *cpu)
{
    /* SPMul - FFP multiplication
     * D0 = operand1, D1 = operand2
     * Returns: D0 = result */
    cpu->d[0] = ffp_mul(cpu->d[0], cpu->d[1]);
}

static void mathffp_SPDiv(M68kCPUState *cpu)
{
    /* SPDiv - FFP division
     * D0 = dividend, D1 = divisor
     * Returns: D0 = result */
    cpu->d[0] = ffp_div(cpu->d[0], cpu->d[1]);
}

static void mathffp_SPCmp(M68kCPUState *cpu)
{
    /* SPCmp - FFP comparison
     * D1 = operand1, D0 = operand2
     * Returns: D0 = -1 if operand1 < operand2, 0 if equal, 1 if greater */
    cpu->d[0] = (uint32_t)ffp_cmp(cpu->d[1], cpu->d[0]);
}

static void mathffp_SPTst(M68kCPUState *cpu)
{
    /* SPTst - test an FFP value against zero
     * D1 = operand
     * Returns: D0 = -1 if negative, 0 if zero, 1 if positive */
    cpu->d[0] = (uint32_t)ffp_tst(cpu->d[1]);
}

static void mathffp_SPNeg(M68kCPUState *cpu)
{
    /* SPNeg - FFP negation (sign lives in low byte bit 7)
     * D0 = operand
     * Returns: D0 = -operand */
    cpu->d[0] = ffp_neg(cpu->d[0]);
}

static void mathffp_SPAbs(M68kCPUState *cpu)
{
    /* SPAbs - FFP absolute value
     * D0 = operand
     * Returns: D0 = |operand| */
    cpu->d[0] = ffp_abs(cpu->d[0]);
}

static void mathffp_SPFix(M68kCPUState *cpu)
{
    /* SPFix - convert FFP to integer (truncate)
     * D0 = FFP operand
     * Returns: D0 = integer result */
    cpu->d[0] = ffp_fix(cpu->d[0]);
}

static void mathffp_SPFlt(M68kCPUState *cpu)
{
    /* SPFlt - convert integer to FFP
     * D0 = integer operand
     * Returns: D0 = FFP result */
    cpu->d[0] = ffp_flt((int32_t)cpu->d[0]);
}

static void mathffp_SPFloor(M68kCPUState *cpu)
{
    /* SPFloor - largest integer not greater than the operand
     * D0 = FFP operand
     * Returns: D0 = FFP result */
    cpu->d[0] = ffp_floor(cpu->d[0]);
}

static void mathffp_SPCeil(M68kCPUState *cpu)
{
    /* SPCeil - smallest integer not less than the operand
     * D0 = FFP operand
     * Returns: D0 = FFP result */
    cpu->d[0] = ffp_ceil(cpu->d[0]);
}

/* Transcendental functions convert the FFP operand to IEEE 754, run the
 * shared freestanding helpers (also used by mathtrans.library), and
 * convert the result back.  All take their operand in D0 and return in
 * D0. */

static void mathffp_SPSqrt(M68kCPUState *cpu)
{
    cpu->d[0] = float_to_ffp(uaos_sqrtf(ffp_to_float(cpu->d[0])));
}

static void mathffp_SPLog(M68kCPUState *cpu)
{
    /* SPLog - natural logarithm (ln) */
    cpu->d[0] = float_to_ffp(uaos_logf(ffp_to_float(cpu->d[0])));
}

static void mathffp_SPExp(M68kCPUState *cpu)
{
    cpu->d[0] = float_to_ffp(uaos_expf(ffp_to_float(cpu->d[0])));
}

static void mathffp_SPSin(M68kCPUState *cpu)
{
    cpu->d[0] = float_to_ffp(uaos_sinf(ffp_to_float(cpu->d[0])));
}

static void mathffp_SPCos(M68kCPUState *cpu)
{
    cpu->d[0] = float_to_ffp(uaos_cosf(ffp_to_float(cpu->d[0])));
}

static void mathffp_SPTan(M68kCPUState *cpu)
{
    cpu->d[0] = float_to_ffp(uaos_tanf(ffp_to_float(cpu->d[0])));
}

static void mathffp_SPAtan(M68kCPUState *cpu)
{
    cpu->d[0] = float_to_ffp(uaos_atanf(ffp_to_float(cpu->d[0])));
}

static void mathffp_SPAsin(M68kCPUState *cpu)
{
    cpu->d[0] = float_to_ffp(uaos_asinf(ffp_to_float(cpu->d[0])));
}

static void mathffp_SPAcos(M68kCPUState *cpu)
{
    cpu->d[0] = float_to_ffp(uaos_acosf(ffp_to_float(cpu->d[0])));
}

static void mathffp_SPSinCos(M68kCPUState *cpu)
{
    /* SPSinCos - sine and cosine in one call
     * D0 = FFP angle (radians)
     * Returns: D0 = sin(angle), D1 = cos(angle) */
    float x = ffp_to_float(cpu->d[0]);
    cpu->d[0] = float_to_ffp(uaos_sinf(x));
    cpu->d[1] = float_to_ffp(uaos_cosf(x));
}

static void mathffp_SPSinh(M68kCPUState *cpu)
{
    float e = uaos_expf(ffp_to_float(cpu->d[0]));
    cpu->d[0] = float_to_ffp((e - 1.0f / e) * 0.5f);
}

static void mathffp_SPCosh(M68kCPUState *cpu)
{
    float e = uaos_expf(ffp_to_float(cpu->d[0]));
    cpu->d[0] = float_to_ffp((e + 1.0f / e) * 0.5f);
}

static void mathffp_SPTanh(M68kCPUState *cpu)
{
    float e = uaos_expf(ffp_to_float(cpu->d[0]));
    cpu->d[0] = float_to_ffp((e - 1.0f / e) / (e + 1.0f / e));
}

static void mathffp_SPPow(M68kCPUState *cpu)
{
    /* SPPow - raise to a power
     * D1 = power, D0 = base
     * Returns: D0 = base ** power */
    cpu->d[0] = float_to_ffp(uaos_powf(ffp_to_float(cpu->d[0]),
                                       ffp_to_float(cpu->d[1])));
}

static void mathffp_SPTieee(M68kCPUState *cpu)
{
    /* SPTieee - convert IEEE 754 single bits to FFP
     * D0 = IEEE 754 operand
     * Returns: D0 = FFP result */
    union { float f; uint32_t u; } conv;
    conv.u = cpu->d[0];
    cpu->d[0] = float_to_ffp(conv.f);
}

static void mathffp_SPFieee(M68kCPUState *cpu)
{
    /* SPFieee - convert FFP to IEEE 754 single bits
     * D0 = FFP operand
     * Returns: D0 = IEEE 754 result */
    union { float f; uint32_t u; } conv;
    conv.f = ffp_to_float(cpu->d[0]);
    cpu->d[0] = conv.u;
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
    mathffp_SPTst,         /* index 21 */
    mathffp_SPFloor,       /* index 22 */
    mathffp_SPCeil,        /* index 23 */
    mathffp_SPSinCos,      /* index 24 */
    mathffp_SPSinh,        /* index 25 */
    mathffp_SPCosh,        /* index 26 */
    mathffp_SPTanh,        /* index 27 */
    mathffp_SPPow,         /* index 28 */
    mathffp_SPTieee,       /* index 29 */
    mathffp_SPFieee,       /* index 30 */
};

/* Canonical mathffp.library LVOs -> mathffp_funcs[] indices. */
static const UaosRomLvo mathffp_lvo_map[] = {
    {  -30, MATHFFP_SP_FIX },    /* SPFix   */
    {  -36, MATHFFP_SP_FLT },    /* SPFlt   */
    {  -42, MATHFFP_SP_CMP },    /* SPCmp   */
    {  -48, MATHFFP_SP_TST },    /* SPTst   */
    {  -54, MATHFFP_SP_ABS },    /* SPAbs   */
    {  -60, MATHFFP_SP_NEG },    /* SPNeg   */
    {  -66, MATHFFP_SP_ADD },    /* SPAdd   */
    {  -72, MATHFFP_SP_SUB },    /* SPSub   */
    {  -78, MATHFFP_SP_MUL },    /* SPMul   */
    {  -84, MATHFFP_SP_DIV },    /* SPDiv   */
    {  -90, MATHFFP_SP_FLOOR },  /* SPFloor */
    {  -96, MATHFFP_SP_CEIL },   /* SPCeil  */
    { -102, MATHFFP_SP_SIN },    /* SPSin   */
    { -108, MATHFFP_SP_COS },    /* SPCos   */
    { -114, MATHFFP_SP_TAN },    /* SPTan   */
    { -120, MATHFFP_SP_SINCOS }, /* SPSinCos */
    { -126, MATHFFP_SP_SINH },   /* SPSinh  */
    { -132, MATHFFP_SP_COSH },   /* SPCosh  */
    { -138, MATHFFP_SP_TANH },   /* SPTanh  */
    { -144, MATHFFP_SP_EXP },    /* SPExp   */
    { -150, MATHFFP_SP_LOG },    /* SPLog   */
    { -156, MATHFFP_SP_POW },    /* SPPow   */
    { -162, MATHFFP_SP_SQRT },   /* SPSqrt  */
    { -168, MATHFFP_SP_TIEEE },  /* SPTieee */
    { -174, MATHFFP_SP_FIEEE },  /* SPFieee */
    { -180, MATHFFP_SP_ASIN },   /* SPAsin  */
    { -186, MATHFFP_SP_ACOS },   /* SPAcos  */
    { -192, MATHFFP_SP_ATAN },   /* SPAtan  */
};

/* =========================================================================
 * Registration function
 * ========================================================================= */

void UAOS_MATHFFP_Register(void)
{
    UAOS_ROM_Register("mathffp.library", 40, 0x00000060,
                      (uint16_t)(sizeof(mathffp_funcs) / sizeof(mathffp_funcs[0])),
                      mathffp_funcs);
    UAOS_ROM_BindLvoMap("mathffp.library", mathffp_lvo_map,
                        (uint16_t)(sizeof(mathffp_lvo_map) / sizeof(mathffp_lvo_map[0])));
}
