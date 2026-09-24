/*
 * float_math.c — Freestanding single-precision math helpers
 *
 * Shared software implementations of IEEE 754 single-precision
 * transcendental functions used by mathffp.library and
 * mathtrans.library.  Implemented in freestanding C (no <math.h>)
 * using Taylor series and Newton-Raphson iterations.  The kernel
 * GCC on x86_64 supports native float arithmetic even in a
 * freestanding build, so we can use float constants and operators.
 *
 * These implementations were extracted from mathtrans_lib.c.
 */

#include "float_math.h"
#include <stdint.h>

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/* Reduce angle to [-PI, PI] */
static float reduce_angle(float x)
{
    /* Fast modular reduction for reasonable input ranges */
    while (x > UAOS_FLT_PI)  x -= UAOS_FLT_TWO_PI;
    while (x < -UAOS_FLT_PI) x += UAOS_FLT_TWO_PI;
    return x;
}

/* =========================================================================
 * Transcendental function implementations
 * ========================================================================= */

float uaos_sinf(float x)
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

float uaos_cosf(float x)
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

float uaos_tanf(float x)
{
    float c = uaos_cosf(x);
    if (c == 0.0f) return 1e30f;
    return uaos_sinf(x) / c;
}

float uaos_sqrtf(float x)
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

float uaos_expf(float x)
{
    /* Range reduction: e^x = 2^(x / ln2) = 2^k * e^r
     * where k = round(x / ln2), r = x - k * ln2 */
    if (x > 88.0f)  return 3.4e38f;   /* overflow to max float */
    if (x < -87.0f) return 0.0f;      /* underflow to 0 */

    int k = (int)(x / UAOS_FLT_LN2 + (x >= 0 ? 0.5f : -0.5f));
    float r = x - (float)k * UAOS_FLT_LN2;

    /* Taylor series for e^r: 1 + r + r^2/2! + r^3/3! + ... */
    float term = 1.0f;
    float sum = 1.0f;
    for (int i = 1; i <= 12; i++) {
        term *= r / (float)i;
        sum += term;
    }

    /* Multiply by 2^k using bit manipulation */
    union { float f; uint32_t u; } c;
    c.f = sum;
    int exp = (c.u >> 23) & 0xFF;
    exp += k;
    if (k >= 0) {
        /* ldexpf(sum, k) — shift exponent */
        if (exp > 254) return 3.4e38f;
    } else {
        if (exp < 1) return 0.0f;
    }
    c.u = (c.u & 0x807FFFFF) | (exp << 23);
    return c.f;
}

float uaos_logf(float x)
{
    /* Natural log via artanh series:
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

    return (float)e * UAOS_FLT_LN2 + lnm;
}

float uaos_asinf(float x)
{
    if (x < -1.0f || x > 1.0f) return 0.0f;
    if (x == 1.0f)  return UAOS_FLT_HALF_PI;
    if (x == -1.0f) return -UAOS_FLT_HALF_PI;
    /* asin(x) = x + x^3/6 + 3x^5/40 + 15x^7/336 + ...
     * Better convergence for small x; for |x| > 0.5 use:
     * asin(x) = atan(x / sqrt(1 - x^2)) */
    if (x > 0.5f || x < -0.5f) {
        float c = uaos_sqrtf(1.0f - x * x);
        return uaos_atanf(x / c);
    }
    float x2 = x * x;
    float x3 = x2 * x;
    float x5 = x3 * x2;
    float x7 = x5 * x2;
    return x + x3/6.0f + 3.0f*x5/40.0f + 15.0f*x7/336.0f;
}

float uaos_acosf(float x)
{
    if (x < -1.0f || x > 1.0f) return 0.0f;
    return UAOS_FLT_HALF_PI - uaos_asinf(x);
}

float uaos_atanf(float x)
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

    if (recip)  result = UAOS_FLT_HALF_PI - result;
    if (negate) result = -result;
    return result;
}

float uaos_floorf(float x)
{
    int32_t i = (int32_t)x;
    if (x < 0.0f && (float)i != x) i--;
    return (float)i;
}

float uaos_ceilf(float x)
{
    int32_t i = (int32_t)x;
    if (x > 0.0f && (float)i != x) i++;
    return (float)i;
}

float uaos_powf(float base, float exp)
{
    if (base == 0.0f) return (exp > 0.0f) ? 0.0f : 1e30f;
    if (base < 0.0f) {
        /* Integer exponent: ok; otherwise NaN */
        int ie = (int)exp;
        if ((float)ie != exp) return 0.0f;  /* NaN */
        float r = uaos_expf(exp * uaos_logf(-base));
        return (ie & 1) ? -r : r;
    }
    return uaos_expf(exp * uaos_logf(base));
}
