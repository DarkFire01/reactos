/*
 * PROJECT:     ReactOS Kernel - Vista+ APIs
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     C runtime string helpers exported by the kernel
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "ntoskrnl_vista.h"

/* DEFINES ********************************************************************/

#ifndef EINVAL
#define EINVAL 22
#endif

#ifndef ERANGE
#define ERANGE 34
#endif

#ifndef STRUNCATE
#define STRUNCATE 80
#endif

#ifndef _TRUNCATE
#define _TRUNCATE ((rsize_t)-1)
#endif

#define UI64_MAX 0xFFFFFFFFFFFFFFFFULL

/* PRIVATE FUNCTIONS **********************************************************/

static
BOOLEAN
IsBlank(
    _In_ int Character)
{
    return (Character == ' ' || (Character >= '\t' && Character <= '\r'));
}

/* FUNCTIONS ******************************************************************/

/**
 * @brief
 * Converts the leading part of a string to a 64 bit unsigned integer.
 *
 * @param[in] String
 * The null terminated string to convert.
 *
 * @param[out] EndPointer
 * Optionally receives the first character that was not converted.
 *
 * @param[in] Base
 * The numeric base, either zero or 2 through 36. Zero picks the base from an
 * optional "0x" or "0" prefix.
 *
 * @return
 * The converted value, or UI64_MAX when the value does not fit.
 */
unsigned __int64
__cdecl
_strtoui64(
    const char *String,
    char **EndPointer,
    int Base)
{
    const char *Current = String;
    unsigned __int64 Value = 0;
    BOOLEAN IsNegative = FALSE;
    BOOLEAN Converted = FALSE;
    BOOLEAN Overflow = FALSE;
    char Character;
    int Digit;

    if (EndPointer != NULL)
        *EndPointer = (char *)String;

    if (String == NULL || Base == 1 || Base < 0 || Base > 36)
        return 0;

    while (IsBlank((unsigned char)*Current))
        Current++;

    if (*Current == '+' || *Current == '-')
    {
        IsNegative = (*Current == '-');
        Current++;
    }

    if ((Base == 0 || Base == 16) &&
        Current[0] == '0' && (Current[1] == 'x' || Current[1] == 'X'))
    {
        Current += 2;
        Base = 16;
    }
    else if (Base == 0)
    {
        Base = (Current[0] == '0') ? 8 : 10;
    }

    for (;;)
    {
        Character = *Current;

        if (Character >= '0' && Character <= '9')
            Digit = Character - '0';
        else if (Character >= 'a' && Character <= 'z')
            Digit = Character - 'a' + 10;
        else if (Character >= 'A' && Character <= 'Z')
            Digit = Character - 'A' + 10;
        else
            break;

        if (Digit >= Base)
            break;

        if (Value > (UI64_MAX - Digit) / Base)
            Overflow = TRUE;

        Value = Value * Base + Digit;
        Converted = TRUE;
        Current++;
    }

    if (!Converted)
        return 0;

    if (EndPointer != NULL)
        *EndPointer = (char *)Current;

    if (Overflow)
        return UI64_MAX;

    /* A negated value wraps around, which is what the CRT hands back */
    return IsNegative ? (0ULL - Value) : Value;
}

/* EOF */
