/*
 * PROJECT:     ReactOS Kernel32
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Reading strings out of a module's resources
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <k32.h>

#define NDEBUG
#include <debug.h>

/* A string table holds sixteen strings, each one a length and then its text */
#define K32_STRINGS_PER_TABLE 16

/**
 * @brief
 * Reads one string out of a module's string tables.
 *
 * @param[in] Length
 * How many characters the buffer holds. Zero asks for the string where it lies
 * instead of a copy, and the buffer then receives a pointer to it.
 *
 * @return
 * How many characters the string has, without its terminator, or zero if the
 * module has no such string.
 *
 * @remarks
 * User32 offers this as well. This one is here because a module built against
 * the library loader contract asks for it there, and because nothing in the
 * answer needs a window station.
 */
INT
WINAPI
BaseLoadStringW(
    _In_opt_ HINSTANCE Instance,
    _In_ UINT Id,
    _Out_writes_to_opt_(Length, return + 1) LPWSTR Buffer,
    _In_ INT Length)
{
    HRSRC Found;
    HGLOBAL Loaded;
    PWCHAR Walk;
    UINT Index;
    UINT Count;

    if ((Length < 0) || ((Buffer == NULL) && (Length != 0)))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    /* The tables are numbered from one, sixteen strings to each */
    Found = FindResourceExW(Instance,
                            (LPCWSTR)RT_STRING,
                            (LPCWSTR)(ULONG_PTR)((Id / K32_STRINGS_PER_TABLE) + 1),
                            0);
    if (Found == NULL)
        return 0;

    Loaded = LoadResource(Instance, Found);
    if (Loaded == NULL)
        return 0;

    Walk = LockResource(Loaded);
    if (Walk == NULL)
        return 0;

    /* Walk past the strings ahead of the wanted one in its table */
    for (Index = Id % K32_STRINGS_PER_TABLE; Index > 0; Index--)
        Walk += *Walk + 1;

    Count = *Walk++;

    if (Length == 0)
    {
        *(PWCHAR *)Buffer = Walk;
        return Count;
    }

    if (Count > (UINT)Length - 1)
        Count = Length - 1;

    RtlCopyMemory(Buffer, Walk, Count * sizeof(WCHAR));
    Buffer[Count] = UNICODE_NULL;

    return Count;
}

/* EOF */
