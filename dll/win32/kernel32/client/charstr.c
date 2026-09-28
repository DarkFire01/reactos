/*
 * PROJECT:     ReactOS Kernel32
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Walking a string a character at a time
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <k32.h>

#define NDEBUG
#include <debug.h>

/*
 * User32 offers these as well. They are here because a module built against the
 * legacy string contract asks for them there, alongside the lstr family, and
 * because walking a string needs nothing a window station provides.
 */

/**
 * @brief
 * Returns the character after this one, or this one if it is the last.
 */
LPWSTR
WINAPI
BaseCharNextW(
    _In_ LPCWSTR Current)
{
    if (*Current == UNICODE_NULL)
        return (LPWSTR)Current;

    return (LPWSTR)(Current + 1);
}

/**
 * @brief
 * Returns the character before this one, or this one if it is the first.
 */
LPWSTR
WINAPI
BaseCharPrevW(
    _In_ LPCWSTR Start,
    _In_ LPCWSTR Current)
{
    if (Current <= Start)
        return (LPWSTR)Start;

    return (LPWSTR)(Current - 1);
}

/**
 * @brief
 * Returns the character after this one, over a string in the ANSI code page.
 *
 * @remarks
 * A lead byte and the byte that follows it are the one character, so both are
 * stepped over together.
 */
LPSTR
WINAPI
BaseCharNextA(
    _In_ LPCSTR Current)
{
    if (*Current == ANSI_NULL)
        return (LPSTR)Current;

    if (IsDBCSLeadByte(*Current) && (Current[1] != ANSI_NULL))
        return (LPSTR)(Current + 2);

    return (LPSTR)(Current + 1);
}

/**
 * @brief
 * Returns the character before this one, over a string in the ANSI code page.
 *
 * @remarks
 * Whether the byte before this one is the tail of a two byte character is only
 * knowable by walking forward from the start, which is what this does.
 */
LPSTR
WINAPI
BaseCharPrevA(
    _In_ LPCSTR Start,
    _In_ LPCSTR Current)
{
    LPCSTR Walk = Start;

    while (Walk < Current)
    {
        LPCSTR Next = BaseCharNextA(Walk);

        if (Next >= Current)
            break;

        Walk = Next;
    }

    return (LPSTR)Walk;
}

/**
 * @brief
 * Compares two strings, or looks for one inside the other, by code unit rather
 * than by any locale's idea of order.
 *
 * @param[in] IgnoreCase
 * Whether to fold the two cases together first.
 *
 * @return
 * For a search, where the string was found, or -1 when it was not. For a
 * comparison, whether the strings were the same as a 0, or -1 when not.
 */
INT
WINAPI
FindStringOrdinal(
    _In_ DWORD FindStringOrdinalFlags,
    _In_ LPCWSTR StringSource,
    _In_ INT SourceLength,
    _In_ LPCWSTR StringValue,
    _In_ INT ValueLength,
    _In_ BOOL IgnoreCase)
{
    INT Start;
    INT Step;
    INT Index;

    if ((StringSource == NULL) || (StringValue == NULL))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    if (SourceLength < 0)
        SourceLength = (INT)wcslen(StringSource);

    if (ValueLength < 0)
        ValueLength = (INT)wcslen(StringValue);

    if (FindStringOrdinalFlags == FIND_FROMSTART)
    {
        Start = 0;
        Step = 1;
    }
    else if (FindStringOrdinalFlags == FIND_FROMEND)
    {
        Start = SourceLength - ValueLength;
        Step = -1;
    }
    else if (FindStringOrdinalFlags == FIND_STARTSWITH)
    {
        Start = 0;
        Step = 0;
    }
    else if (FindStringOrdinalFlags == FIND_ENDSWITH)
    {
        Start = SourceLength - ValueLength;
        Step = 0;
    }
    else
    {
        SetLastError(ERROR_INVALID_FLAGS);
        return -1;
    }

    if (ValueLength > SourceLength)
    {
        SetLastError(ERROR_SUCCESS);
        return -1;
    }

    for (Index = Start;
         (Index >= 0) && (Index <= (SourceLength - ValueLength));
         Index += Step)
    {
        INT Same = IgnoreCase
                   ? CompareStringOrdinal(StringSource + Index, ValueLength,
                                          StringValue, ValueLength, TRUE)
                   : CompareStringOrdinal(StringSource + Index, ValueLength,
                                          StringValue, ValueLength, FALSE);

        if (Same == CSTR_EQUAL)
        {
            SetLastError(ERROR_SUCCESS);
            return Index;
        }

        if (Step == 0)
            break;
    }

    SetLastError(ERROR_SUCCESS);

    return -1;
}

/* EOF */
