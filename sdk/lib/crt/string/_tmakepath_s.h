/*
 * PROJECT:     ReactOS C runtime library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Shared body of _makepath_s and _wmakepath_s
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <tchar.h>
#include <stdlib.h>
#include <errno.h>

/**
 * @brief
 * Builds a path from its drive, directory, file name and extension.
 *
 * @param[out] PathResult
 * The buffer.
 *
 * @param[in] PathSize
 * Its size, in characters.
 *
 * @param[in] Drive
 * The drive, of which only the letter is used.
 *
 * @param[in] Dir
 * The directory, which gets a backslash unless it ends in a separator.
 *
 * @param[in] Filename
 * The file name.
 *
 * @param[in] Ext
 * The extension, which gets a dot unless it starts with one.
 *
 * @return
 * Zero on success. EINVAL for a missing buffer and ERANGE when the path does
 * not fit, with an empty buffer left behind.
 */
errno_t
__cdecl
_tmakepath_x(
    _Out_writes_z_(PathSize) _TCHAR *PathResult,
    _In_ size_t PathSize,
    _In_opt_z_ const _TCHAR *Drive,
    _In_opt_z_ const _TCHAR *Dir,
    _In_opt_z_ const _TCHAR *Filename,
    _In_opt_z_ const _TCHAR *Ext)
{
    _TCHAR *Out, *Last;
    const _TCHAR *Part;

    if (PathResult == NULL || PathSize == 0)
        return EINVAL;

    /* Last is kept for the terminator, so every other character must land before it */
    Out = PathResult;
    Last = PathResult + PathSize - 1;

    if (Drive != NULL && Drive[0] != _T('\0'))
    {
        if (Last - Out < 2)
            goto TooSmall;

        *Out++ = Drive[0];
        *Out++ = _T(':');
    }

    if (Dir != NULL && Dir[0] != _T('\0'))
    {
        for (Part = Dir; *Part != _T('\0'); Part++)
        {
            if (Out == Last)
                goto TooSmall;
            *Out++ = *Part;
        }

        if (Part[-1] != _T('\\') && Part[-1] != _T('/'))
        {
            if (Out == Last)
                goto TooSmall;
            *Out++ = _T('\\');
        }
    }

    if (Filename != NULL)
    {
        for (Part = Filename; *Part != _T('\0'); Part++)
        {
            if (Out == Last)
                goto TooSmall;
            *Out++ = *Part;
        }
    }

    if (Ext != NULL && Ext[0] != _T('\0'))
    {
        if (Ext[0] != _T('.'))
        {
            if (Out == Last)
                goto TooSmall;
            *Out++ = _T('.');
        }

        for (Part = Ext; *Part != _T('\0'); Part++)
        {
            if (Out == Last)
                goto TooSmall;
            *Out++ = *Part;
        }
    }

    *Out = _T('\0');
    return 0;

TooSmall:
    PathResult[0] = _T('\0');
    return ERANGE;
}
