/*
 * PROJECT:     ReactOS C runtime library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     strncat_s for the kernel runtime
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <string.h>
#include <errno.h>

#ifndef _TRUNCATE
#define _TRUNCATE ((size_t)-1)
#endif

#ifndef STRUNCATE
#define STRUNCATE 80
#endif

/**
 * @brief
 * Appends at most Count characters of a string to one in a buffer of known size.
 *
 * @param[in,out] Destination
 * The buffer, holding a terminated string.
 *
 * @param[in] DestinationSize
 * Its size, in characters.
 *
 * @param[in] Source
 * The string to append, may be NULL when Count is zero.
 *
 * @param[in] Count
 * How many characters of Source to append at most, _TRUNCATE for as many as fit.
 *
 * @return
 * Zero on success, STRUNCATE when _TRUNCATE cut the string short. EINVAL for a
 * missing buffer or string or an unterminated destination and ERANGE when the
 * result does not fit, in both cases with an empty destination where there is one.
 */
errno_t
__cdecl
strncat_s(
    char *Destination,
    size_t DestinationSize,
    const char *Source,
    size_t Count)
{
    size_t End, Index;

    if (Count == 0 && Destination == NULL && DestinationSize == 0)
        return 0;

    if (Destination == NULL || DestinationSize == 0)
        return EINVAL;

    if (Count == 0)
        return 0;

    if (Source == NULL)
    {
        Destination[0] = '\0';
        return EINVAL;
    }

    for (End = 0; End < DestinationSize && Destination[End] != '\0'; End++)
        ;

    if (End == DestinationSize)
    {
        Destination[0] = '\0';
        return EINVAL;
    }

    for (Index = 0; (Count == _TRUNCATE || Index < Count) && Source[Index] != '\0'; Index++)
    {
        if (End + Index + 1 >= DestinationSize)
        {
            if (Count == _TRUNCATE)
            {
                Destination[DestinationSize - 1] = '\0';
                return STRUNCATE;
            }

            Destination[0] = '\0';
            return ERANGE;
        }

        Destination[End + Index] = Source[Index];
    }

    Destination[End + Index] = '\0';
    return 0;
}
