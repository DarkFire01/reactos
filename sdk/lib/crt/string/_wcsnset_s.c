/*
 * PROJECT:     ReactOS C runtime library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     _wcsnset_s, _wcsset_s and _wcslwr_s for the kernel runtime
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <string.h>
#include <errno.h>

/**
 * @brief
 * Sets at most Count characters of a wide string in a buffer of known size to one value.
 *
 * @param[in,out] String
 * The buffer, holding a terminated string.
 *
 * @param[in] StringSize
 * Its size, in characters.
 *
 * @param[in] Value
 * The character to store.
 *
 * @param[in] Count
 * How many characters to set at most.
 *
 * @return
 * Zero on success. EINVAL for a missing buffer or one the string does not end
 * in, the latter with an empty string left behind.
 */
errno_t
__cdecl
_wcsnset_s(
    _Inout_updates_z_(StringSize) wchar_t *String,
    _In_ size_t StringSize,
    _In_ wchar_t Value,
    _In_ size_t Count)
{
    size_t Index;

    if (String == NULL)
        return (Count == 0 && StringSize == 0) ? 0 : EINVAL;

    if (StringSize == 0)
        return EINVAL;

    /* The terminator has to sit inside the buffer, even past the characters that change */
    for (Index = 0; String[Index] != L'\0'; Index++)
    {
        if (Index == StringSize - 1)
        {
            String[0] = L'\0';
            return EINVAL;
        }

        if (Index < Count)
            String[Index] = Value;
    }

    return 0;
}

/**
 * @brief
 * Sets every character of a wide string in a buffer of known size to one value.
 *
 * @param[in,out] String
 * The buffer, holding a terminated string.
 *
 * @param[in] StringSize
 * Its size, in characters.
 *
 * @param[in] Value
 * The character to store.
 *
 * @return
 * Zero on success. EINVAL for a missing buffer or one the string does not end
 * in, the latter with an empty string left behind.
 */
errno_t
__cdecl
_wcsset_s(
    _Inout_updates_z_(StringSize) wchar_t *String,
    _In_ size_t StringSize,
    _In_ wchar_t Value)
{
    return _wcsnset_s(String, StringSize, Value, (size_t)-1);
}

/**
 * @brief
 * Lowers the case of the ASCII letters of a wide string in a buffer of known size.
 *
 * @param[in,out] String
 * The buffer, holding a terminated string.
 *
 * @param[in] StringSize
 * Its size, in characters.
 *
 * @return
 * Zero on success. EINVAL for a missing buffer or one the string does not end
 * in, the latter with an empty string left behind.
 */
errno_t
__cdecl
_wcslwr_s(
    _Inout_updates_z_(StringSize) wchar_t *String,
    _In_ size_t StringSize)
{
    wchar_t *Cursor;

    if (String == NULL)
        return EINVAL;

    /* Windows clears the first character here even when the size is zero */
    if (wcsnlen(String, StringSize) == StringSize)
    {
        String[0] = L'\0';
        return EINVAL;
    }

    for (Cursor = String; *Cursor != L'\0'; Cursor++)
    {
        if (*Cursor >= L'A' && *Cursor <= L'Z')
            *Cursor += L'a' - L'A';
    }

    return 0;
}
