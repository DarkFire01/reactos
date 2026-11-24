/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS system libraries
 * FILE:            dll/win32/kernel32/client/file/volumeansi.c
 * PURPOSE:         Volume functions (ANSI)
 */

/* INCLUDES *****************************************************************/

#include <k32.h>

/* FUNCTIONS ****************************************************************/

/*
 * @implemented
 */
BOOL
WINAPI
GetVolumePathNameA(IN LPCSTR lpszFileName,
                   IN LPSTR lpszVolumePathName,
                   IN DWORD cchBufferLength)
{
    BOOL Ret;
    PUNICODE_STRING FileNameU;
    ANSI_STRING VolumePathName;
    UNICODE_STRING VolumePathNameU;

    /* Convert file name to unicode */
    FileNameU = Basep8BitStringToStaticUnicodeString(lpszFileName);
    if (FileNameU == NULL)
    {
        return FALSE;
    }

    /* Initialize all the strings we'll need */
    VolumePathName.Buffer = lpszVolumePathName;
    VolumePathName.Length = 0;
    VolumePathName.MaximumLength = cchBufferLength - 1;

    VolumePathNameU.Length = 0;
    VolumePathNameU.MaximumLength = (cchBufferLength - 1) * sizeof(WCHAR) + sizeof(UNICODE_NULL);
    /* Allocate a buffer for calling the -W */
    VolumePathNameU.Buffer = RtlAllocateHeap(RtlGetProcessHeap(), 0, VolumePathNameU.MaximumLength);
    if (VolumePathNameU.Buffer == NULL)
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    /* Call the -W implementation */
    Ret = GetVolumePathNameW(FileNameU->Buffer, VolumePathNameU.Buffer, cchBufferLength);
    /* If it succeed */
    if (Ret)
    {
        NTSTATUS Status;

        /* Convert back to ANSI */
        RtlInitUnicodeString(&VolumePathNameU, VolumePathNameU.Buffer);
        Status = RtlUnicodeStringToAnsiString(&VolumePathName, &VolumePathNameU, FALSE);
        /* If conversion failed, just set error code and fail the rest */
        if (!NT_SUCCESS(Status))
        {
            BaseSetLastNTError(Status);
            Ret = FALSE;
        }
        /* Otherwise, null terminate the string (it's OK, we computed -1) */
        else
        {
            VolumePathName.Buffer[VolumePathName.Length] = ANSI_NULL;
        }
    }

    /* Free the buffer allocated for -W call */
    RtlFreeHeap(RtlGetProcessHeap(), 0, VolumePathNameU.Buffer);
    return Ret;
}


/*
 * @implemented
 */
BOOL
WINAPI
FindNextVolumeA(IN HANDLE handle,
                IN LPSTR volume,
                IN DWORD len)
{
    WCHAR *buffer = RtlAllocateHeap(RtlGetProcessHeap(), 0, len * sizeof(WCHAR));
    BOOL ret;

    if (!buffer)
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    if ((ret = FindNextVolumeW( handle, buffer, len )))
    {
        if (!WideCharToMultiByte( CP_ACP, 0, buffer, -1, volume, len, NULL, NULL )) ret = FALSE;
    }

    RtlFreeHeap(RtlGetProcessHeap(), 0, buffer);
    return ret;
}


/*
 * @implemented
 */
BOOL
WINAPI
GetVolumePathNamesForVolumeNameA(IN LPCSTR lpszVolumeName,
                                 IN LPSTR lpszVolumePathNames,
                                 IN DWORD cchBufferLength,
                                 OUT PDWORD lpcchReturnLength)
{
    BOOL Ret;
    NTSTATUS Status;
    DWORD cchReturnLength;
    ANSI_STRING VolumePathName;
    PUNICODE_STRING VolumeNameU;
    UNICODE_STRING VolumePathNamesU;

    /* Convert volume name to unicode */
    VolumeNameU = Basep8BitStringToStaticUnicodeString(lpszVolumeName);
    if (VolumeNameU == NULL)
    {
        return FALSE;
    }

    /* Initialize the strings we'll use later on */
    VolumePathName.Length = 0;
    VolumePathName.MaximumLength = cchBufferLength;
    VolumePathName.Buffer = lpszVolumePathNames;

    VolumePathNamesU.Length = 0;
    VolumePathNamesU.MaximumLength = sizeof(WCHAR) * cchBufferLength;
    /* If caller provided a non 0 sized string, allocate a buffer for our unicode string */
    if (VolumePathNamesU.MaximumLength != 0)
    {
        VolumePathNamesU.Buffer = RtlAllocateHeap(RtlGetProcessHeap(), 0, VolumePathNamesU.MaximumLength);
        if (VolumePathNamesU.Buffer == NULL)
        {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return FALSE;
        }
    }
    else
    {
        VolumePathNamesU.Buffer = NULL;
    }

    /* Call the -W implementation */
    Ret = GetVolumePathNamesForVolumeNameW(VolumeNameU->Buffer, VolumePathNamesU.Buffer,
                                           cchBufferLength, &cchReturnLength);
    /* Call succeed, we'll return the total length */
    if (Ret)
    {
        VolumePathNamesU.Length = sizeof(WCHAR) * cchReturnLength;
    }
    else
    {
        /* Else, if we fail for anything else than too small buffer, quit */
        if (GetLastError() != ERROR_MORE_DATA)
        {
            if (VolumePathNamesU.Buffer != NULL)
            {
                RtlFreeHeap(RtlGetProcessHeap(), 0, VolumePathNamesU.Buffer);
            }

            return FALSE;
        }

        /* Otherwise, we'll just copy as much as we can */
        VolumePathNamesU.Length = sizeof(WCHAR) * cchBufferLength;
    }

    /* Convert our output string back to ANSI */
    Status = RtlUnicodeStringToAnsiString(&VolumePathName, &VolumePathNamesU, FALSE);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);

        if (VolumePathNamesU.Buffer != NULL)
        {
            RtlFreeHeap(RtlGetProcessHeap(), 0, VolumePathNamesU.Buffer);
        }

        return FALSE;
    }

    /* If caller wants return length, two cases... */
    if (lpcchReturnLength != NULL)
    {
        /* We succeed: return the copied length */
        if (Ret)
        {
            *lpcchReturnLength = VolumePathName.Length;
        }
        /* We failed, return the size we would have loved having! */
        else
        {
            *lpcchReturnLength = sizeof(WCHAR) * cchReturnLength;
        }
    }

    /* Release our buffer if allocated */
    if (VolumePathNamesU.Buffer != NULL)
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, VolumePathNamesU.Buffer);
    }

    return Ret;
}

/* EOF */
