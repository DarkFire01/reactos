/*
 * PROJECT:     ReactOS Win32 Base API
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Locale and geography names
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "k32_vista.h"

#define NDEBUG
#include <debug.h>

/* What a machine with no geography set says it is, per the two letter codes */
#define K32_DEFAULT_GEO_NAME L"US"

/**
 * @brief
 * Names the closest locale this machine has to the one asked for.
 *
 * @param[in] NameToResolve
 * The locale wanted. NULL asks for the user's own.
 *
 * @return
 * How many characters the answer has, counting its terminator, or zero when no
 * locale here is close enough to answer with.
 *
 * @remarks
 * A locale this machine carries resolves to itself. One it does not carry is
 * not narrowed down to a parent, because nothing here keeps which locale is a
 * parent of which.
 */
INT
WINAPI
ResolveLocaleName(
    _In_opt_ LPCWSTR NameToResolve,
    _Out_writes_opt_(Length) LPWSTR LocaleName,
    _In_ INT Length)
{
    SIZE_T Needed;

    if ((Length < 0) || ((LocaleName == NULL) && (Length != 0)))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    if (NameToResolve == NULL)
        return GetUserDefaultLocaleName(LocaleName, Length) + 1;

    if (!IsValidLocaleName(NameToResolve))
    {
        SetLastError(ERROR_SUCCESS);
        return 0;
    }

    Needed = wcslen(NameToResolve) + 1;

    if (Length == 0)
        return (INT)Needed;

    if (Needed > (SIZE_T)Length)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }

    RtlCopyMemory(LocaleName, NameToResolve, Needed * sizeof(WCHAR));

    return (INT)Needed;
}

/**
 * @brief
 * Names the geography this machine is set to, as its two letter code.
 *
 * @return
 * How many characters the name has, counting its terminator, or zero on
 * failure.
 *
 * @remarks
 * Nothing here records a geography, so the name is the one a machine that was
 * never told reports.
 */
INT
WINAPI
GetUserDefaultGeoName(
    _Out_writes_opt_(Length) LPWSTR GeoName,
    _In_ INT Length)
{
    SIZE_T Needed = RTL_NUMBER_OF(K32_DEFAULT_GEO_NAME);

    if ((Length < 0) || ((GeoName == NULL) && (Length != 0)))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    if (Length == 0)
        return (INT)Needed;

    if (Needed > (SIZE_T)Length)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }

    RtlCopyMemory(GeoName, K32_DEFAULT_GEO_NAME, Needed * sizeof(WCHAR));

    return (INT)Needed;
}

/* EOF */
