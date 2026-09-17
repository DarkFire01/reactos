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

/**
 * @brief
 * Copies bytes between buffers, checking the room in the destination first.
 *
 * @param[out] Destination
 * The destination buffer.
 *
 * @param[in] DestinationSize
 * Size of @p Destination, in bytes.
 *
 * @param[in] Source
 * The source buffer.
 *
 * @param[in] Count
 * Number of bytes to copy.
 *
 * @return
 * Zero on success, EINVAL or ERANGE otherwise.
 */
errno_t
__cdecl
memcpy_s(
    void *Destination,
    rsize_t DestinationSize,
    const void *Source,
    rsize_t Count)
{
    if (Count == 0)
        return 0;

    if (Destination == NULL)
        return EINVAL;

    /* A failed copy must not leave stale bytes behind */
    if (Source == NULL || DestinationSize < Count)
    {
        RtlZeroMemory(Destination, DestinationSize);
        return (Source == NULL) ? EINVAL : ERANGE;
    }

    RtlCopyMemory(Destination, Source, Count);
    return 0;
}

/**
 * @brief
 * Copies at most @p Count characters of a string and always terminates the
 * result.
 *
 * @param[out] Destination
 * The destination buffer.
 *
 * @param[in] DestinationSize
 * Size of @p Destination, in characters.
 *
 * @param[in] Source
 * The source string.
 *
 * @param[in] Count
 * Maximum number of characters to copy, or _TRUNCATE.
 *
 * @return
 * Zero on success, STRUNCATE when the source was cut short on request, EINVAL
 * or ERANGE otherwise.
 */
errno_t
__cdecl
strncpy_s(
    char *Destination,
    rsize_t DestinationSize,
    const char *Source,
    rsize_t Count)
{
    rsize_t Index = 0;
    rsize_t Limit;

    if (Count == 0 && Destination == NULL && DestinationSize == 0)
        return 0;

    if (Destination == NULL || DestinationSize == 0)
        return EINVAL;

    if (Source == NULL)
    {
        Destination[0] = '\0';
        return EINVAL;
    }

    /* Truncation stops one short so the terminator always has room */
    Limit = (Count == _TRUNCATE) ? DestinationSize - 1 : Count;

    while (Index < Limit && Index < DestinationSize && Source[Index] != '\0')
    {
        Destination[Index] = Source[Index];
        Index++;
    }

    if (Index < DestinationSize)
    {
        Destination[Index] = '\0';
        return (Count == _TRUNCATE && Source[Index] != '\0') ? STRUNCATE : 0;
    }

    /* The string did not fit, so hand back an empty destination */
    Destination[0] = '\0';
    return ERANGE;
}

/**
 * @brief
 * Splits a string into tokens, keeping the scan state in the caller.
 *
 * @param[in] String
 * The string to tokenize, or NULL to carry on from @p Context.
 *
 * @param[in] Delimiters
 * The delimiter characters.
 *
 * @param[in,out] Context
 * Caller owned storage holding the position of the next scan.
 *
 * @return
 * The next token, or NULL once the string is exhausted.
 */
char *
__cdecl
strtok_s(
    char *String,
    const char *Delimiters,
    char **Context)
{
    const char *Delimiter;
    char *Token;

    if (Delimiters == NULL || Context == NULL)
        return NULL;

    if (String == NULL)
        String = *Context;

    if (String == NULL)
        return NULL;

    /* Step over the delimiters in front of the token */
    while (*String != '\0')
    {
        for (Delimiter = Delimiters; *Delimiter != '\0'; Delimiter++)
        {
            if (*String == *Delimiter)
                break;
        }

        if (*Delimiter == '\0')
            break;

        String++;
    }

    if (*String == '\0')
    {
        *Context = String;
        return NULL;
    }

    /* Terminate the token at the delimiter that closes it */
    Token = String;
    while (*String != '\0')
    {
        for (Delimiter = Delimiters; *Delimiter != '\0'; Delimiter++)
        {
            if (*String == *Delimiter)
            {
                *String = '\0';
                *Context = String + 1;
                return Token;
            }
        }

        String++;
    }

    *Context = String;
    return Token;
}

/* EOF */
