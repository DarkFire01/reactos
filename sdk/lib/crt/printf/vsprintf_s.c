/*
 * PROJECT:     ReactOS C runtime library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     vsprintf_s for the kernel runtime
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <stdio.h>
#include <stdarg.h>
#include <limits.h>

/**
 * @brief
 * Formats a string into a buffer of known size.
 *
 * @param[out] Buffer
 * The buffer.
 *
 * @param[in] BufferSize
 * Its size, in characters. (size_t)-1 means the size is not limited.
 *
 * @param[in] Format
 * The format string.
 *
 * @param[in] Arguments
 * The arguments for the format string.
 *
 * @return
 * The number of characters written without the terminator, or -1 when an
 * argument is missing or the output does not fit. An output that does not
 * fit leaves an empty string in the buffer.
 */
int
__cdecl
vsprintf_s(
    _Out_writes_z_(BufferSize) char *Buffer,
    _In_ size_t BufferSize,
    _In_z_ _Printf_format_string_ const char *Format,
    va_list Arguments)
{
    int Length;

    if (Buffer == NULL || BufferSize == 0 || Format == NULL)
        return -1;

    if (BufferSize > INT_MAX && BufferSize != (size_t)-1)
    {
        Buffer[0] = '\0';
        return -1;
    }

    Length = _vsnprintf(Buffer, BufferSize, Format, Arguments);
    if (Length < 0 || (size_t)Length >= BufferSize)
    {
        Buffer[0] = '\0';
        return -1;
    }

    return Length;
}
