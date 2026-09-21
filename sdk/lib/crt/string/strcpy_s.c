/*
 * PROJECT:     ReactOS C runtime library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     strcpy_s for the kernel runtime
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <string.h>
#include <errno.h>

/**
 * @brief
 * Copies a string into a buffer of known size.
 *
 * @param[out] Destination
 * The buffer.
 *
 * @param[in] DestinationSize
 * Its size, in characters.
 *
 * @param[in] Source
 * The string to copy.
 *
 * @return
 * Zero on success. EINVAL for a missing buffer or string and ERANGE when the
 * string does not fit, in both cases with an empty destination where there is
 * one.
 */
errno_t
__cdecl
strcpy_s(
    char *Destination,
    size_t DestinationSize,
    const char *Source)
{
    size_t Index;

    if (Destination == NULL || DestinationSize == 0)
        return EINVAL;

    if (Source == NULL)
    {
        Destination[0] = '\0';
        return EINVAL;
    }

    for (Index = 0; Index < DestinationSize; Index++)
    {
        Destination[Index] = Source[Index];
        if (Source[Index] == '\0')
            return 0;
    }

    Destination[0] = '\0';
    return ERANGE;
}
