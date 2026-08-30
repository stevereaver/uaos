/*
 * mathtrans_lib.c — UAOS mathtrans.library Implementation
 *
 * AmigaOS mathtrans.library provides IEEE 754 single-precision
 * transcendental functions (sin, cos, tan, sqrt, exp, log, etc.).
 * ACE Basic opens this library unconditionally at startup.
 *
 * All functions are implemented with freestanding C (no <math.h>)
 * using Taylor series and Newton-Raphson iterations.  The kernel
 * GCC on x86_64 supports native float arithmetic even in a
 * freestanding build, so we can use float constants and operators.
 *
 * LVO layout matches the AmigaOS 3.x mathtrans.library.
 */

#include "rom_modules.h"
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
 * Math constants
 * ========================================================================= */

#define PI       3.14159265358979323846f
#define TWO_PI   6.28318530717958647692f
#define HALF_PI  1.57079632679489661923f
#define LN2      0.69314718055994530942f
#define LN10     2.30258509299404568402f
#define E_VAL    2.71828182845904523536f

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/* Reduce angle to [-PI, PI] */
static float reduce_angle(float x)
{
    /* Fast modular reduction for reasonable input ranges */
    while (x > PI)  x -= TWO_PI;
    while (x < -PI) x += TWO_PI;
    return x;
}

/* Forward declarations for inter-dependencies */
static float my_atanf(float x);

/* =========================================================================
 * Transcendental function implementations
 * ========================================================================= */

static float my_sinf(float x)
{
    x = reduce_angle(x);
    /* Taylor series: sin(x) = x - x^3/3! + x^5/5! - x^7/7! + ... */
    float x2 = x * x;
    float x3 = x2 * x;
    float x5 = x3 * x2;
    float x7 = x5 * x2;
    float x9 = x7 * x2;
    float x11 = x9 * x2;
    return x - x3/6.0f + x5/120.0f - x7/5040.0f + x9/362880.0f - x11/39916800.0f;
}

static float my_cosf(float x)
{
    x = reduce_angle(x);
    /* Taylor series: cos(x) = 1 - x^2/2! + x^4/4! - x^6/6! + ... */
    float x2 = x * x;
    float x4 = x2 * x2;
    float x6 = x4 * x2;
    float x8 = x6 * x2;
    float x10 = x8 * x2;
    return 1.0f - x2/2.0f + x4/24.0f - x6/720.0f + x8/40320.0f - x10/3628800.0f;
}

static float my_tanf(float x)
{
    float c = my_cosf(x);
    if (c == 0.0f) return f2b(0) & 0x80000000 ? -1e30f : 1e30f;
    return my_sinf(x) / c;
}

static float my_sqrtf(float x)
{
    if (x < 0.0f) return 0.0f;  /* NaN would be ideal, but 0 is safe */
    if (x == 0.0f) return 0.0f;
    /* Newton-Raphson: x_{n+1} = 0.5 * (x_n + S / x_n) */
    float guess = x * 0.5f;
    if (guess == 0.0f) guess = 1.0f;
    for (int i = 0; i < 20; i++) {
        float next = 0.5f * (guess + x / guess);
        if (next == guess) break;
        guess = next;
    }
    return guess;
}

static float my_expf(float x)
{
    /* Range reduction: e^x = 2^(x / ln2) = 2^k * e^r
     * where k = round(x / ln2), r = x - k * ln2 */
    if (x > 88.0f)  return 3.4e38f;   /* overflow to max float */
    if (x < -87.0f) return 0.0f;      /* underflow to 0 */

    int k = (int)(x / LN2 + (x >= 0 ? 0.5f : -0.5f));
    float r = x - (float)k * LN2;

    /* Taylor series for e^r: 1 + r + r^2/2! + r^3/3! + ... */
    float term = 1.0f;
    float sum = 1.0f;
    for (int i = 1; i <= 12; i++) {
        term *= r / (float)i;
        sum += term;
    }

    /* Multiply by 2^k using bit manipulation */
    if (k >= 0) {
        /* ldexpf(sum, k) — shift exponent */
        union { float f; uint32_t u; } c;
        c.f = sum;
        int exp = (c.u >> 23) & 0xFF;
        exp += k;
        if (exp > 254) return 3.4e38f;
        c.u = (c.u & 0x807FFFFF) | (exp << 23);
        return c.f;
    } else {
        union { float f; uint32_t u; } c;
        c.f = sum;
        int exp = (c.u >> 23) & 0xFF;
        exp += k;
        if (exp < 1) return 0.0f;
        c.u = (c.u & 0x807FFFFF) | (exp << 23);
        return c.f;
    }
}

static float my_logf(float x)
{
    /* Natural log via Newton-Raphson on e^y = x
     * ln(x) = 2 * artanh((x-1)/(x+1))
     * artanh(z) = z + z^3/3 + z^5/5 + ... */
    if (x <= 0.0f) return -1e30f;

    /* Range reduction: x = m * 2^e, where 1 <= m < 2
     * ln(x) = e * ln2 + ln(m) */
    union { float f; uint32_t u; } c;
    c.f = x;
    int e = ((c.u >> 23) & 0xFF) - 127;
    /* Normalize mantissa to [1, 2) */
    c.u = (c.u & 0x807FFFFF) | (127 << 23);
    float m = c.f;

    /* ln(m) for m in [1, 2) using artanh series */
    float z = (m - 1.0f) / (m + 1.0f);
    float z2 = z * z;
    float z3 = z2 * z;
    float z5 = z3 * z2;
    float z7 = z5 * z2;
    float z9 = z7 * z2;
    float lnm = 2.0f * (z + z3/3.0f + z5/5.0f + z7/7.0f + z9/9.0f);

    return (float)e * LN2 + lnm;
}

static float my_asinf(float x)
{
    if (x < -1.0f || x > 1.0f) return 0.0f;
    if (x == 1.0f)  return HALF_PI;
    if (x == -1.0f) return -HALF_PI;
    /* asin(x) = x + x^3/6 + 3x^5/40 + 15x^7/336 + ...
     * Better convergence for small x; for |x| > 0.5 use:
     * asin(x) = atan(x / sqrt(1 - x^2)) */
    if (x > 0.5f || x < -0.5f) {
        float c = my_sqrtf(1.0f - x * x);
        return my_atanf(x / c);
    }
    float x2 = x * x;
    float x3 = x2 * x;
    float x5 = x3 * x2;
    float x7 = x5 * x2;
    return x + x3/6.0f + 3.0f*x5/40.0f + 15.0f*x7/336.0f;
}

static float my_acosf(float x)
{
    if (x < -1.0f || x > 1.0f) return 0.0f;
    return HALF_PI - my_asinf(x);
}

static float my_atanf(float x)
{
    /* For |x| > 1, use atan(x) = pi/2 - atan(1/x) for x > 0
     *                atan(x) = -pi/2 - atan(1/x) for x < 0 */
    int negate = 0;
    int recip = 0;
    if (x < 0.0f) { x = -x; negate = 1; }
    if (x > 1.0f) { x = 1.0f / x; recip = 1; }

    /* Taylor series: atan(x) = x - x^3/3 + x^5/5 - x^7/7 + ...
     * Converges well for |x| <= 1 */
    float x2 = x * x;
    float x3 = x2 * x;
    float x5 = x3 * x2;
    float x7 = x5 * x2;
    float x9 = x7 * x2;
    float x11 = x9 * x2;
    float x13 = x11 * x2;
    float x15 = x13 * x2;
    float result = x - x3/3.0f + x5/5.0f - x7/7.0f + x9/9.0f
                 - x11/11.0f + x13/13.0f - x15/15.0f;

    if (recip)  result = HALF_PI - result;
    if (negate) result = -result;
    return result;
}

static float my_floorf(float x)
{
    int32_t i = (int32_t)x;
    if (x < 0.0f && (float)i != x) i--;
    return (float)i;
}

static float my_ceilf(float x)
{
    int32_t i = (int32_t)x;
    if (x > 0.0f && (float)i != x) i++;
    return (float)i;
}

static float my_powf(float base, float exp)
{
    if (base == 0.0f) return (exp > 0.0f) ? 0.0f : 1e30f;
    if (base < 0.0f) {
        /* Integer exponent: ok; otherwise NaN */
        int ie = (int)exp;
        if ((float)ie != exp) return 0.0f;  /* NaN */
        float r = my_expf(exp * my_logf(-base));
        return (ie & 1) ? -r : r;
    }
    return my_expf(exp * my_logf(base));
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
    cpu->d[0] = f2b(my_sinf(b2f(cpu->d[0])));
}

static void mt_SPCos(M68kCPUState *cpu)
{
    cpu->d[0] = f2b(my_cosf(b2f(cpu->d[0])));
}

static void mt_SPTan(M68kCPUState *cpu)
{
    cpu->d[0] = f2b(my_tanf(b2f(cpu->d[0])));
}

static void mt_SPSincos(M68kCPUState *cpu)
{
    float x = b2f(cpu->d[0]);
    /* Returns sin in D0, stores cos at the address in A0 */
    float c = my_cosf(x);
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
    cpu->d[0] = f2b(my_sinf(x));
}

static void mt_SPAsin(M68kCPUState *cpu)
{
    cpu->d[0] = f2b(my_asinf(b2f(cpu->d[0])));
}

static void mt_SPAcos(M68kCPUState *cpu)
{
    cpu->d[0] = f2b(my_acosf(b2f(cpu->d[0])));
}

static void mt_SPAtan(M68kCPUState *cpu)
{
    cpu->d[0] = f2b(my_atanf(b2f(cpu->d[0])));
}

static void mt_SPExp(M68kCPUState *cpu)
{
    cpu->d[0] = f2b(my_expf(b2f(cpu->d[0])));
}

static void mt_SPLog(M68kCPUState *cpu)
{
    cpu->d[0] = f2b(my_logf(b2f(cpu->d[0])));
}

static void mt_SPLog10(M68kCPUState *cpu)
{
    float ln = my_logf(b2f(cpu->d[0]));
    cpu->d[0] = f2b(ln / LN10);
}

static void mt_SPSqrt(M68kCPUState *cpu)
{
    cpu->d[0] = f2b(my_sqrtf(b2f(cpu->d[0])));
}

static void mt_SPFloor(M68kCPUState *cpu)
{
    cpu->d[0] = f2b(my_floorf(b2f(cpu->d[0])));
}

static void mt_SPCeil(M68kCPUState *cpu)
{
    cpu->d[0] = f2b(my_ceilf(b2f(cpu->d[0])));
}

static void mt_SPPow(M68kCPUState *cpu)
{
    float base = b2f(cpu->d[0]);
    float exp  = b2f(cpu->d[1]);
    cpu->d[0] = f2b(my_powf(base, exp));
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
