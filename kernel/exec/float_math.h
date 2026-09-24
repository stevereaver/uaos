/*
 * float_math.h — Freestanding single-precision math helpers
 *
 * Shared software implementations of IEEE 754 single-precision
 * transcendental functions.  The freestanding kernel has no
 * <math.h>, so these are implemented in plain C using Taylor
 * series and Newton-Raphson iterations.  Used by mathffp.library
 * and mathtrans.library.
 */

#ifndef UAOS_FLOAT_MATH_H
#define UAOS_FLOAT_MATH_H

#define UAOS_FLT_PI       3.14159265358979323846f
#define UAOS_FLT_TWO_PI   6.28318530717958647692f
#define UAOS_FLT_HALF_PI  1.57079632679489661923f
#define UAOS_FLT_LN2      0.69314718055994530942f
#define UAOS_FLT_LN10     2.30258509299404568402f
#define UAOS_FLT_E        2.71828182845904523536f

float uaos_sinf(float x);
float uaos_cosf(float x);
float uaos_tanf(float x);
float uaos_sqrtf(float x);
float uaos_expf(float x);
float uaos_logf(float x);
float uaos_asinf(float x);
float uaos_acosf(float x);
float uaos_atanf(float x);
float uaos_floorf(float x);
float uaos_ceilf(float x);
float uaos_powf(float base, float exp);

#endif /* UAOS_FLOAT_MATH_H */
