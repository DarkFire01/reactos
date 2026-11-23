/*
 * PROJECT:     ReactOS C runtime library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Shared body of _ultoa_s and _ultow_s
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <tchar.h>
#include <stdlib.h>
#include <errno.h>

/**
 * @brief
 * Writes an unsigned long in the given radix to a buffer of known size.
 *
 * @param[in] Value
 * The number.
 *
 * @param[out] Buffer
 * The buffer.
 *
 * @param[in] BufferSize
 * Its size, in characters.
 *
 * @param[in] Radix
 * The base, from 2 to 36, with lowercase letters past 9.
 *
 * @return
 * Zero on success. EINVAL for a missing buffer or a bad radix and ERANGE when
 * the digits do not fit, with an empty buffer left behind whenever there is one.
 */
errno_t
__cdecl
_tultoa_x(
    _In_ unsigned long Value,
    _Out_writes_z_(BufferSize) _TCHAR *Buffer,
    _In_ size_t BufferSize,
    _In_ int Radix)
{
    _TCHAR *Front, *Back, Swap;
    unsigned long Digit;
    size_t Length;

    if (Buffer == NULL || BufferSize == 0)
        return EINVAL;

    Buffer[0] = _T('\0');
    if (BufferSize == 1)
        return ERANGE;

    if (Radix < 2 || Radix > 36)
        return EINVAL;

    /* Digits come out lowest first and get reversed once they all fit */
    Length = 0;
    do
    {
        Digit = Value % (unsigned long)Radix;
        Value /= (unsigned long)Radix;
        Buffer[Length++] = (_TCHAR)((Digit < 10) ? (_T('0') + Digit) : (_T('a') + Digit - 10));
    } while (Value != 0 && Length < BufferSize);

    if (Length == BufferSize)
    {
        Buffer[0] = _T('\0');
        return ERANGE;
    }

    Buffer[Length] = _T('\0');
    for (Front = Buffer, Back = Buffer + Length - 1; Front < Back; Front++, Back--)
    {
        Swap = *Front;
        *Front = *Back;
        *Back = Swap;
    }

    return 0;
}
