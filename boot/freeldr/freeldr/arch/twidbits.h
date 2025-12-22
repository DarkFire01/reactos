/*
 * PROJECT:     FreeLoader
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 *              or MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Bit twiddling helpers
 * COPYRIGHT:   Copyright 2025 Hermès Bélusca-Maïto <hermes.belusca-maito@reactos.org>
 *
 * Based on http://www.graphics.stanford.edu/~seander/bithacks.html
 * and other sources.
 */

// HAVE___BUILTIN_POPCOUNT
// HAVE___BUILTIN_PEXT_PDEP

#pragma once

/**
 * @brief   Return the number of bits set in a 32-bit integer.
 * @note    Equivalent to __popcnt().
 **/
FORCEINLINE
ULONG
CountNumberOfBits(
    _In_ UINT32 n)
{
#ifdef HAVE___BUILTIN_POPCOUNT
    return __popcnt(n);
#else
    n -= ((n >> 1) & 0x55555555);
    n =  (((n >> 2) & 0x33333333) + (n & 0x33333333));
#if 0
    n =  (((n >> 4) + n) & 0x0f0f0f0f);
    n += (n >> 8);
    n += (n >> 16);
    return (n & 0x3f);
#else
    return (((n >> 4) + n) & 0x0f0f0f0f) * 0x01010101 >> 24;
#endif
#endif /* HAVE___BUILTIN_POPCOUNT */
}

// NOTE: See also https://git.reactos.org/?p=reactos.git;a=blob;f=sdk/tools/mkhive/rtl.c;hb=054b8e00d9d9bd631a789a9198f61acd81b8c609#l176
// for slightly simpler loop versions.

/**
 * @brief   Find the 1-based index of the lowest bit set in a 32-bit integer.
 **/
FORCEINLINE
ULONG
FindLowestSetBit(
    _In_ UINT32 n)
{
#if 1
    ULONG ret = 0;
    return (_BitScanForward(&ret, n) ? ++ret : 0);
#elif 0
    n |= (n << 1);
    n |= (n << 2);
    n |= (n << 4);
    n |= (n << 8);
    n |= (n << 16);
    n = CountNumberOfBits(~n) + 1;
    return (n <= 32 ? n : 0);
#elif 0
    // Adapted from: https://graphics.stanford.edu/~seander/bithacks.html#ZerosOnRightParallel
    ULONG c = 32; // RTL_BITS_OF(ULONG); // c will be the number of zero bits on the right
    n &= -n; // -signed(n);
    if (n) c--;
    if (n & 0x0000FFFF) c -= 16;
    if (n & 0x00FF00FF) c -= 8;
    if (n & 0x0F0F0F0F) c -= 4;
    if (n & 0x33333333) c -= 2;
    if (n & 0x55555555) c -= 1;
    return (c < 32 ? ++c : 0);
#else
    ULONG ret = 1;
    while ((ret <= 32) && (n & 1) == 0) // 32 == RTL_BITS_OF(ULONG);
    {
        ++ret;
        n >>= 1;
    }
    return (ret <= 32 ? ret : 0);
#endif
}

/**
 * @brief   Find the 1-based index of the highest bit set in a 32-bit integer.
 **/
FORCEINLINE
ULONG
FindHighestSetBit(
    _In_ UINT32 n)
{
#if 1
    ULONG ret = 0;
    return (_BitScanReverse(&ret, n) ? ++ret : 0);
#elif 0
    n |= (n >> 1);
    n |= (n >> 2);
    n |= (n >> 4);
    n |= (n >> 8);
    n |= (n >> 16);
    return CountNumberOfBits(n); // For 0-based index, use "(n >> 1)".
#else
    ULONG ret = 32; // RTL_BITS_OF(ULONG);
    while (ret && (n & (1 << 31)) == 0)
    {
        --ret;
        n <<= 1;
    }
    return ret;
#endif
}

/**
 * @brief
 * Selects the bits from @p Value corresponding to the bits set in @ Mask,
 * and gathers ("compress") them in the least significant part of the result
 * (aka. "compress_right").
 *
 * @note
 * This corresponds to the "pext" BMI2 (Haswell+) Intel instruction.
 *   "Extract bits from unsigned 32-bit integer a at the corresponding bit
 *   locations specified by mask to contiguous low bits in dst; the remaining
 *   upper bits in dst are set to zero."
 * https://www.felixcloutier.com/x86/pext
 * https://programming.sirrida.de/bit_perm.html#bmi2
 * https://geoff.space/2024/06/pext-and-pdep/
 * https://web.archive.org/web/20190108215807/http://www.hackersdelight.org/hdcodetxt/compress.c.txt
 **/
FORCEINLINE
UINT32
CompressBits(
    _In_ UINT32 Value,
    _In_ UINT32 Mask)
{
#if defined(_INCLUDED_IMM) && defined(HAVE___BUILTIN_PEXT_PDEP)
    return _pext_u32(Value, Mask);
#else
    UINT32 dst = 0;
    ULONG k = 0;
    while (Mask)
    {
        if (Mask & 1)
        {
            dst |= ((Value & 1) << k);
            ++k;
        }
        Value >>= 1;
        Mask >>= 1;
    }
    return dst;
#endif
}

/**
 * @brief
 * Selects the bits from @p Value and scatters ("expand") them in the result
 * to the positions indicated by the bits set in @ Mask.
 *
 * @note
 * This corresponds to the "pdep" BMI2 (Haswell+) Intel instruction.
 *   "Deposit contiguous low bits from unsigned 32-bit integer a to dst at
 *   the corresponding bit locations specified by mask; all other bits in dst
 *   are set to zero."
 * https://www.felixcloutier.com/x86/pdep
 * https://programming.sirrida.de/bit_perm.html#bmi2
 * https://geoff.space/2024/06/pext-and-pdep/
 **/
FORCEINLINE
UINT32
ExpandBits(
    _In_ UINT32 Value,
    _In_ UINT32 Mask)
{
#if defined(_INCLUDED_IMM) && defined(HAVE___BUILTIN_PEXT_PDEP)
    return _pdep_u32(Value, Mask);
#else
    UINT32 dst = 0;
    ULONG k = 0;
    while (Mask)
    {
        if (Mask & 1)
        {
            dst |= ((Value & 1) << k);
            Value >>= 1;
        }
        ++k;
        Mask >>= 1;
    }
    return dst;
#endif
}

#ifdef UNIT_TEST
/**
 * @brief
 * Unit-test adapted from:
 * https://web.archive.org/web/20190108215807/http://www.hackersdelight.org/hdcodetxt/compress.c.txt
 **/

#include <stdio.h>

unsigned errors;
void error(unsigned x, unsigned m, unsigned got, unsigned shdbe)
{
    ++errors;
    printf("Error for x = %08X, m = %08X, got %08X, should be %08X\n",
           x, m, got, shdbe);
}

int main(void)
{
    static UINT32 test[] = {
//      Data        Mask        Result
        0xFFFFFFFF, 0x80000000, 0x00000001,
        0xFFFFFFFF, 0x0010084A, 0x0000001F,
        0xFFFFFFFF, 0x55555555, 0x0000FFFF,
        0xFFFFFFFF, 0x88E00F55, 0x00001FFF,
        0x01234567, 0x0000FFFF, 0x00004567,
        0x01234567, 0xFFFF0000, 0x00000123,
        0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
        0,          0,          0,
        0,          0xFFFFFFFF, 0,
        0xFFFFFFFF, 0,          0,
        0x80000000, 0x80000000, 1,
        0x55555555, 0x55555555, 0x0000FFFF,
        0x55555555, 0xAAAAAAAA, 0,
        0x789ABCDE, 0x0F0F0F0F, 0x00008ACE,
        0x789ABCDE, 0xF0F0F0F0, 0x000079BD,
        0x92345678, 0x80000000, 0x00000001,
        0x12345678, 0xF0035555, 0x000004ec,
        0x80000000, 0xF0035555, 0x00002000,
    };
    size_t i, n;

    n = sizeof(test)/sizeof(test[0]);
    for (i = 0; i < n; i += 3)
    {
        UINT32 r, r2;
        r = CompressBits(test[i], test[i+1]);
        if (r != test[i+2])
            error(test[i], test[i+1], r, test[i+2]);

        r2 = ExpandBits(r, test[i+1]);
        if (r2 != (test[i] & test[i+1]))
            error(r, test[i+1], r2, (test[i] & test[i+1]));
    }

    if (errors == 0)
        printf("Passed all %Iu cases.\n", n/3);
    return errors;
}
#endif /* UNIT_TEST */
