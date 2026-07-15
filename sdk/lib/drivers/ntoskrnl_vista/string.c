/*
 * PROJECT:     ReactOS Kernel - Vista+ APIs
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     C runtime string helpers exported by the kernel
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "ntoskrnl_vista.h"

#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef ERANGE
#define ERANGE 34
#endif

#define UI64_MAX 0xFFFFFFFFFFFFFFFFULL

static
int
IsSpaceChar(int Character)
{
    return (Character == ' ' || (Character >= '\t' && Character <= '\r'));
}

/**
 * @brief
 * Converts the initial portion of a string to a 64-bit unsigned integer.
 *
 * @param[in] String
 * The null-terminated string to convert.
 *
 * @param[out] EndPointer
 * Optionally receives a pointer to the first unconverted character.
 *
 * @param[in] Base
 * The numeric base (0, or 2 through 36). A base of 0 auto-detects the base from
 * an optional "0x" or leading "0" prefix.
 *
 * @return
 * The converted value, or UI64_MAX (with EndPointer set to @p String) on
 * overflow or when no digits are found.
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
    int IsNegative = 0;
    int DidConvert = 0;
    int Overflow = 0;

    if (EndPointer != NULL)
        *EndPointer = (char *)String;

    if (String == NULL || Base == 1 || Base < 0 || Base > 36)
        return 0;

    while (IsSpaceChar((unsigned char)*Current))
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
        char Char = *Current;
        int Digit;

        if (Char >= '0' && Char <= '9')
            Digit = Char - '0';
        else if (Char >= 'a' && Char <= 'z')
            Digit = Char - 'a' + 10;
        else if (Char >= 'A' && Char <= 'Z')
            Digit = Char - 'A' + 10;
        else
            break;

        if (Digit >= Base)
            break;

        if (Value > (UI64_MAX - Digit) / Base)
            Overflow = 1;

        Value = Value * Base + Digit;
        DidConvert = 1;
        Current++;
    }

    if (!DidConvert)
        return 0;

    if (EndPointer != NULL)
        *EndPointer = (char *)Current;

    if (Overflow)
        return UI64_MAX;

    return IsNegative ? (unsigned __int64)(-(__int64)Value) : Value;
}

/**
 * @brief
 * Copies bytes between buffers with destination-size validation.
 *
 * @param[out] Destination
 * The destination buffer.
 *
 * @param[in] DestinationSize
 * The size, in bytes, of @p Destination.
 *
 * @param[in] Source
 * The source buffer.
 *
 * @param[in] Count
 * The number of bytes to copy.
 *
 * @return
 * Zero on success, or EINVAL/ERANGE on a validation failure.
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
 * Copies at most @p Count characters of a string, guaranteeing null
 * termination and validating the destination size.
 *
 * @param[out] Destination
 * The destination buffer.
 *
 * @param[in] DestinationSize
 * The size, in characters, of @p Destination.
 *
 * @param[in] Source
 * The source string.
 *
 * @param[in] Count
 * The maximum number of characters to copy, or _TRUNCATE.
 *
 * @return
 * Zero on success, or EINVAL/ERANGE on a validation failure.
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

    if (Count == 0 && Destination == NULL && DestinationSize == 0)
        return 0;

    if (Destination == NULL || DestinationSize == 0)
        return EINVAL;

    if (Source == NULL)
    {
        Destination[0] = '\0';
        return EINVAL;
    }

    while (Index < Count && Index < DestinationSize && Source[Index] != '\0')
    {
        Destination[Index] = Source[Index];
        Index++;
    }

    if (Index < DestinationSize)
    {
        Destination[Index] = '\0';
        return 0;
    }

    /* The source did not fit; report the error and empty the destination. */
    Destination[0] = '\0';
    return ERANGE;
}

/**
 * @brief
 * Reentrant string tokenizer.
 *
 * @param[in] String
 * The string to tokenize, or NULL to continue tokenizing from @p Context.
 *
 * @param[in] Delimiters
 * The set of delimiter characters.
 *
 * @param[in,out] Context
 * Caller-provided storage that preserves state between calls.
 *
 * @return
 * A pointer to the next token, or NULL when no more tokens remain.
 */
char *
__cdecl
strtok_s(
    char *String,
    const char *Delimiters,
    char **Context)
{
    char *Token;
    const char *Delimiter;

    if (Delimiters == NULL || Context == NULL)
        return NULL;

    if (String == NULL)
        String = *Context;

    if (String == NULL)
        return NULL;

    /* Skip leading delimiters. */
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

    /* Scan the token body. */
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
