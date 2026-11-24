/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS system libraries
 * FILE:            dll/win32/kernel32/client/file/moveansi.c
 * PURPOSE:         Directory functions
 * PROGRAMMER:      Ariadne ( ariadne@xs4all.nl)
 *                  Gerhard W. Gruber (sparhawk_at_gmx.at)
 *                  Dmitry Philippov (shedon@mail.ru)
 *                  Pierre Schweitzer (pierre@reactos.org)
 */

/* INCLUDES *****************************************************************/

#include <k32.h>

/* FUNCTIONS ****************************************************************/

/*
 * @implemented
 */
BOOL
WINAPI
ReplaceFileA(IN LPCSTR lpReplacedFileName,
             IN LPCSTR lpReplacementFileName,
             IN LPCSTR lpBackupFileName OPTIONAL,
             IN DWORD dwReplaceFlags,
             IN LPVOID lpExclude,
             IN LPVOID lpReserved)
{
    BOOL Ret;
    UNICODE_STRING ReplacedFileNameW, ReplacementFileNameW, BackupFileNameW;

    if (!lpReplacedFileName || !lpReplacementFileName || lpExclude || lpReserved ||
        (dwReplaceFlags & ~(REPLACEFILE_WRITE_THROUGH | REPLACEFILE_IGNORE_MERGE_ERRORS)))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (!Basep8BitStringToDynamicUnicodeString(&ReplacedFileNameW, lpReplacedFileName))
    {
        return FALSE;
    }

    if (!Basep8BitStringToDynamicUnicodeString(&ReplacementFileNameW, lpReplacementFileName))
    {
        RtlFreeUnicodeString(&ReplacedFileNameW);
        return FALSE;
    }

    if (lpBackupFileName)
    {
        if (!Basep8BitStringToDynamicUnicodeString(&BackupFileNameW, lpBackupFileName))
        {
            RtlFreeUnicodeString(&ReplacementFileNameW);
            RtlFreeUnicodeString(&ReplacedFileNameW);
            return FALSE;
        }
    }
    else
    {
        BackupFileNameW.Buffer = NULL;
    }

    Ret = ReplaceFileW(ReplacedFileNameW.Buffer,
                       ReplacementFileNameW.Buffer,
                       BackupFileNameW.Buffer,
                       dwReplaceFlags, 0, 0);

    if (lpBackupFileName)
    {
        RtlFreeUnicodeString(&BackupFileNameW);
    }
    RtlFreeUnicodeString(&ReplacementFileNameW);
    RtlFreeUnicodeString(&ReplacedFileNameW);

    return Ret;
}

/* EOF */
