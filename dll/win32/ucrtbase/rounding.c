/*
 * PROJECT:     ReactOS CRT
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     C99 rounding, sign and min/max math functions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <fenv.h>
#include <math.h>

#ifdef _MSC_VER
#pragma warning(disable:4163) // not available as an intrinsic function
#pragma function(copysign, copysignf, rint, rintf, nearbyint, nearbyintf)
#pragma function(lrint, lrintf, llrint, llrintf, lround, lroundf, llround, llroundf)
#pragma function(fmax, fmaxf, fmin, fminf, fdim, fdimf)
#endif

/* Doubles at or above 2^52 have no fraction bits left */
#define ROUND_INTEGRAL_LIMIT    4503599627370496.0
#define ROUND_INT64_LIMIT       9223372036854775808.0
#define ROUND_LONG_MIN          -2147483648.0
#define ROUND_LONG_MAX          2147483647.0
#define ROUND_SIGN_BIT          0x8000000000000000ULL
#define ROUND_FRACTION_BITS     0x000FFFFFFFFFFFFFULL
#define ROUND_EXPONENT_BIAS     1023
#define ROUND_SIGN_BIT_FLOAT    0x80000000U

typedef union _ROUND_DOUBLE
{
    double Value;
    unsigned long long Bits;
} ROUND_DOUBLE;

typedef union _ROUND_FLOAT
{
    float Value;
    unsigned int Bits;
} ROUND_FLOAT;

/*
 * Out of range conversions report invalid and return zero, as on Windows.
 * Exactly 2^63 is let through to the conversion, which gives INT64_MIN.
 */
static
void
RoundRaiseInvalid(void)
{
    volatile double Zero = 0.0;
#ifdef _M_IX86
    fexcept_t AllUnits = ~0UL;
#endif

    Zero = Zero / Zero;
#ifdef _M_IX86
    /* x86 reports it in both the x87 and SSE status */
    fesetexceptflag(&AllUnits, FE_INVALID);
#endif
}

static
int
RoundIsNegative(
    _In_ double Value)
{
    ROUND_DOUBLE Number;

    Number.Value = Value;
    return (Number.Bits & ROUND_SIGN_BIT) != 0;
}

double
__cdecl
copysign(
    _In_ double x,
    _In_ double y)
{
    ROUND_DOUBLE Magnitude, Sign;

    Magnitude.Value = x;
    Sign.Value = y;
    Magnitude.Bits = (Magnitude.Bits & ~ROUND_SIGN_BIT) | (Sign.Bits & ROUND_SIGN_BIT);
    return Magnitude.Value;
}

float
__cdecl
copysignf(
    _In_ float x,
    _In_ float y)
{
    ROUND_FLOAT Magnitude, Sign;

    Magnitude.Value = x;
    Sign.Value = y;
    Magnitude.Bits = (Magnitude.Bits & ~ROUND_SIGN_BIT_FLOAT) | (Sign.Bits & ROUND_SIGN_BIT_FLOAT);
    return Magnitude.Value;
}

/* Adding and removing 2^52 drops the fraction using the current rounding mode */
double
__cdecl
rint(
    _In_ double x)
{
    volatile double Shifted;

    /* Ordered compares signal on a quiet NaN, so it is weeded out first */
    if (x != x || !(x < ROUND_INTEGRAL_LIMIT && x > -ROUND_INTEGRAL_LIMIT))
        return x;

    if (x >= 0.0)
    {
        Shifted = x + ROUND_INTEGRAL_LIMIT;
        Shifted = Shifted - ROUND_INTEGRAL_LIMIT;
    }
    else
    {
        Shifted = x - ROUND_INTEGRAL_LIMIT;
        Shifted = Shifted + ROUND_INTEGRAL_LIMIT;
    }

    return copysign(Shifted, x);
}

float
__cdecl
rintf(
    _In_ float x)
{
    return (float)rint(x);
}

double
__cdecl
nearbyint(
    _In_ double x)
{
    fexcept_t Inexact;
    double Result;

    fegetexceptflag(&Inexact, FE_INEXACT);
    Result = rint(x);
    fesetexceptflag(&Inexact, FE_INEXACT);
    return Result;
}

float
__cdecl
nearbyintf(
    _In_ float x)
{
    return (float)nearbyint(x);
}

long
__cdecl
lrint(
    _In_ double x)
{
    double Result = rint(x);

    if (Result >= ROUND_LONG_MIN && Result <= ROUND_LONG_MAX)
        return (long)Result;

    RoundRaiseInvalid();
    return 0;
}

long
__cdecl
lrintf(
    _In_ float x)
{
    return lrint(x);
}

long long
__cdecl
llrint(
    _In_ double x)
{
    double Result = rint(x);

    if (Result >= -ROUND_INT64_LIMIT && Result <= ROUND_INT64_LIMIT)
        return (long long)Result;

    RoundRaiseInvalid();
    return 0;
}

long long
__cdecl
llrintf(
    _In_ float x)
{
    return llrint(x);
}

/* Clears the fraction bits directly so no inexact flag is raised */
static
double
RoundTruncate(
    _In_ double x)
{
    ROUND_DOUBLE Number;
    int Exponent;

    Number.Value = x;
    Exponent = (int)((Number.Bits >> 52) & 0x7FF) - ROUND_EXPONENT_BIAS;
    if (Exponent < 0)
        Number.Bits &= ROUND_SIGN_BIT;
    else if (Exponent < 52)
        Number.Bits &= ~(ROUND_FRACTION_BITS >> Exponent);

    return Number.Value;
}

/* Halfway cases go away from zero no matter the rounding mode */
static
double
RoundHalfAway(
    _In_ double x)
{
    double Whole;

    if (x != x || !(x < ROUND_INTEGRAL_LIMIT && x > -ROUND_INTEGRAL_LIMIT))
        return x;

    Whole = RoundTruncate(x);
    if (x - Whole >= 0.5)
        Whole += 1.0;
    else if (x - Whole <= -0.5)
        Whole -= 1.0;

    return Whole;
}

long
__cdecl
lround(
    _In_ double x)
{
    double Result = RoundHalfAway(x);

    if (Result >= ROUND_LONG_MIN && Result <= ROUND_LONG_MAX)
        return (long)Result;

    RoundRaiseInvalid();
    return 0;
}

long
__cdecl
lroundf(
    _In_ float x)
{
    return lround(x);
}

long long
__cdecl
llround(
    _In_ double x)
{
    double Result = RoundHalfAway(x);

    if (Result >= -ROUND_INT64_LIMIT && Result <= ROUND_INT64_LIMIT)
        return (long long)Result;

    RoundRaiseInvalid();
    return 0;
}

long long
__cdecl
llroundf(
    _In_ float x)
{
    return llround(x);
}

/* A NaN argument loses to any number */
double
__cdecl
fmax(
    _In_ double x,
    _In_ double y)
{
    if (x != x)
        return y;
    if (y != y)
        return x;
    if (x == y)
        return RoundIsNegative(x) ? y : x;

    return (x > y) ? x : y;
}

float
__cdecl
fmaxf(
    _In_ float x,
    _In_ float y)
{
    return (float)fmax(x, y);
}

double
__cdecl
fmin(
    _In_ double x,
    _In_ double y)
{
    if (x != x)
        return y;
    if (y != y)
        return x;
    if (x == y)
        return RoundIsNegative(x) ? x : y;

    return (x < y) ? x : y;
}

float
__cdecl
fminf(
    _In_ float x,
    _In_ float y)
{
    return (float)fmin(x, y);
}

double
__cdecl
fdim(
    _In_ double x,
    _In_ double y)
{
    if (x != x || y != y)
        return x + y;

    return (x > y) ? x - y : 0.0;
}

float
__cdecl
fdimf(
    _In_ float x,
    _In_ float y)
{
    if (x != x || y != y)
        return x + y;

    return (x > y) ? x - y : 0.0f;
}
