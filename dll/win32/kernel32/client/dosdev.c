/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS system libraries
 * FILE:            dll/win32/kernel32/client/dosdev.c
 * PURPOSE:         Dos device functions
 * PROGRAMMER:      Ariadne (ariadne@xs4all.nl)
 *                  Pierre Schweitzer
 * UPDATE HISTORY:
 *                  Created 01/11/98
 */

/* INCLUDES ******************************************************************/

#include <k32.h>

#define NDEBUG
#include <debug.h>
#include <dbt.h>
DEBUG_CHANNEL(kernel32file);

/* FUNCTIONS *****************************************************************/
#define MAX_DOS_DRIVES 26

/*
 * @implemented
 */
/* Synced to Wine-2008/12/28 */
DWORD
WINAPI
GetLogicalDriveStringsA(IN DWORD nBufferLength,
                        IN LPSTR lpBuffer)
{
    DWORD drive, count;
    DWORD dwDriveMap;
    LPSTR p;

    dwDriveMap = GetLogicalDrives();

    for (drive = count = 0; drive < MAX_DOS_DRIVES; drive++)
    {
        if (dwDriveMap & (1<<drive))
            count++;
    }


    if ((count * 4) + 1 > nBufferLength) return ((count * 4) + 1);

    p = lpBuffer;

    for (drive = 0; drive < MAX_DOS_DRIVES; drive++)
        if (dwDriveMap & (1<<drive))
        {
            *p++ = 'A' + (UCHAR)drive;
            *p++ = ':';
            *p++ = '\\';
            *p++ = '\0';
        }
    *p = '\0';

    return (count * 4);
}

/*
 * @implemented
 */
NTSTATUS
IsGlobalDeviceMap(
    HANDLE DirectoryHandle,
    PBOOLEAN IsGlobal)
{
    NTSTATUS Status;
    DWORD ReturnLength;
    UNICODE_STRING GlobalString;
    OBJECT_NAME_INFORMATION NameInfo, *PNameInfo;

    /* We need both parameters */
    if (DirectoryHandle == 0 || IsGlobal == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    PNameInfo = NULL;
    _SEH2_TRY
    {
        /* Query handle information */
        Status = NtQueryObject(DirectoryHandle,
                               ObjectNameInformation,
                               &NameInfo,
                               0,
                               &ReturnLength);
        /* Only failure we tolerate is length mismatch */
        if (NT_SUCCESS(Status) || Status == STATUS_INFO_LENGTH_MISMATCH)
        {
            /* Allocate big enough buffer */
            PNameInfo = RtlAllocateHeap(RtlGetProcessHeap(), 0, ReturnLength);
            if (PNameInfo == NULL)
            {
                Status = STATUS_NO_MEMORY;
                _SEH2_LEAVE;
            }

            /* Query again handle information */
            Status = NtQueryObject(DirectoryHandle,
                                   ObjectNameInformation,
                                   PNameInfo,
                                   ReturnLength,
                                   &ReturnLength);

            /*
             * If it succeed, check we have Global??
             * If so, return success
             */
            if (NT_SUCCESS(Status))
            {
                RtlInitUnicodeString(&GlobalString, L"\\GLOBAL??");
                *IsGlobal = RtlEqualUnicodeString(&GlobalString, &PNameInfo->Name, FALSE);
                Status = STATUS_SUCCESS;
            }
        }
    }
    _SEH2_FINALLY
    {
        if (PNameInfo != NULL)
        {
            RtlFreeHeap(RtlGetProcessHeap(), 0, PNameInfo);
        }
    }
    _SEH2_END;

    return Status;
}

/*
 * @implemented
 */
DWORD
FindSymbolicLinkEntry(
    PWSTR NameToFind,
    PWSTR NamesList,
    DWORD TotalEntries,
    PBOOLEAN Found)
{
    WCHAR Current;
    DWORD Entries;
    PWSTR PartialNamesList;

    /* We need all parameters to be set */
    if (NameToFind == NULL || NamesList == NULL || Found == NULL)
    {
        return ERROR_INVALID_PARAMETER;
    }

    /* Assume failure */
    *Found = FALSE;

    /* If no entries, job done, nothing found */
    if (TotalEntries == 0)
    {
        return ERROR_SUCCESS;
    }

    /* Start browsing the names list */
    Entries = 0;
    PartialNamesList = NamesList;
    /* As long as we didn't find the name... */
    while (wcscmp(NameToFind, PartialNamesList) != 0)
    {
        /* We chomped an entry! */
        ++Entries;

        /* We're out of entries, bail out not to overrun */
        if (Entries > TotalEntries)
        {
            /*
             * Even though we found nothing,
             * the function ran fine
             */
            return ERROR_SUCCESS;
        }

        /* Jump to the next string */
        do
        {
            Current = *PartialNamesList;
            ++PartialNamesList;
        } while (Current != UNICODE_NULL);
    }

    /*
     * We're here because the loop stopped:
     * it means we found the name in the list
     */
    *Found = TRUE;
    return ERROR_SUCCESS;
}

/*
 * @implemented
 */
BOOL
WINAPI
DefineDosDeviceA(
    DWORD dwFlags,
    LPCSTR lpDeviceName,
    LPCSTR lpTargetPath
    )
{
    BOOL Result;
    NTSTATUS Status;
    ANSI_STRING AnsiString;
    PWSTR TargetPathBuffer;
    UNICODE_STRING TargetPathU;
    PUNICODE_STRING DeviceNameU;

    /* Convert DeviceName using static unicode string */
    RtlInitAnsiString(&AnsiString, lpDeviceName);
    DeviceNameU = &NtCurrentTeb()->StaticUnicodeString;
    Status = RtlAnsiStringToUnicodeString(DeviceNameU, &AnsiString, FALSE);
    if (!NT_SUCCESS(Status))
    {
        /*
         * If the static unicode string is too small,
         * it's because the name is too long...
         * so, return appropriate status!
         */
        if (Status == STATUS_BUFFER_OVERFLOW)
        {
            SetLastError(ERROR_FILENAME_EXCED_RANGE);
            return FALSE;
        }

        BaseSetLastNTError(Status);
        return FALSE;
    }

    /* Convert target path if existing */
    if (lpTargetPath != NULL)
    {
        RtlInitAnsiString(&AnsiString, lpTargetPath);
        Status = RtlAnsiStringToUnicodeString(&TargetPathU, &AnsiString, TRUE);
        if (!NT_SUCCESS(Status))
        {
            BaseSetLastNTError(Status);
            return FALSE;
        }

        TargetPathBuffer = TargetPathU.Buffer;
    }
    else
    {
        TargetPathBuffer = NULL;
    }

    /* Call W */
    Result = DefineDosDeviceW(dwFlags, DeviceNameU->Buffer, TargetPathBuffer);

    /* Free target path if allocated */
    if (TargetPathBuffer != NULL)
    {
        RtlFreeUnicodeString(&TargetPathU);
    }

    return Result;
}


/*
 * @implemented
 */
DWORD
WINAPI
QueryDosDeviceA(
    LPCSTR lpDeviceName,
    LPSTR lpTargetPath,
    DWORD ucchMax
    )
{
    NTSTATUS Status;
    USHORT CurrentPosition;
    ANSI_STRING AnsiString;
    UNICODE_STRING TargetPathU;
    PUNICODE_STRING DeviceNameU;
    DWORD RetLength, CurrentLength, Length;
    PWSTR DeviceNameBuffer, TargetPathBuffer;

    /* If we want a specific device name, convert it */
    if (lpDeviceName != NULL)
    {
        /* Convert DeviceName using static unicode string */
        RtlInitAnsiString(&AnsiString, lpDeviceName);
        DeviceNameU = &NtCurrentTeb()->StaticUnicodeString;
        Status = RtlAnsiStringToUnicodeString(DeviceNameU, &AnsiString, FALSE);
        if (!NT_SUCCESS(Status))
        {
            /*
             * If the static unicode string is too small,
             * it's because the name is too long...
             * so, return appropriate status!
             */
            if (Status == STATUS_BUFFER_OVERFLOW)
            {
                SetLastError(ERROR_FILENAME_EXCED_RANGE);
                return FALSE;
            }

            BaseSetLastNTError(Status);
            return FALSE;
        }

        DeviceNameBuffer = DeviceNameU->Buffer;
    }
    else
    {
        DeviceNameBuffer = NULL;
    }

    /* Allocate the output buffer for W call */
    TargetPathBuffer = RtlAllocateHeap(RtlGetProcessHeap(), 0, ucchMax * sizeof(WCHAR));
    if (TargetPathBuffer == NULL)
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }

    /* Call W */
    Length = QueryDosDeviceW(DeviceNameBuffer, TargetPathBuffer, ucchMax);
    /* We'll return that length in case of a success */
    RetLength = Length;

    /* Handle the case where we would fill output buffer completly */
    if (Length != 0 && Length == ucchMax)
    {
        /* This will be our work length (but not the one we return) */
        --Length;
        /* Already 0 the last char */
        lpTargetPath[Length] = ANSI_NULL;
    }

    /* If we had an output, start the convert loop */
    if (Length != 0)
    {
        /*
         * We'll have to loop because TargetPathBuffer may contain
         * several strings (NULL separated)
         * We'll start at position 0
         */
        CurrentPosition = 0;
        while (CurrentPosition < Length)
        {
            /* Get the maximum length */
            CurrentLength = min(Length - CurrentPosition, MAXUSHORT / 2);

            /* Initialize our output string */
            AnsiString.Length = 0;
            AnsiString.MaximumLength = CurrentLength + sizeof(ANSI_NULL);
            AnsiString.Buffer = &lpTargetPath[CurrentPosition];

            /* Initialize input string that will be converted */
            TargetPathU.Length = CurrentLength * sizeof(WCHAR);
            TargetPathU.MaximumLength = CurrentLength * sizeof(WCHAR) + sizeof(UNICODE_NULL);
            TargetPathU.Buffer = &TargetPathBuffer[CurrentPosition];

            /* Convert to ANSI */
            Status = RtlUnicodeStringToAnsiString(&AnsiString, &TargetPathU, FALSE);
            if (!NT_SUCCESS(Status))
            {
                BaseSetLastNTError(Status);
                /* In case of a failure, forget about everything */
                RetLength = 0;

                goto Leave;
            }

            /* Move to the next string */
            CurrentPosition += CurrentLength;
        }
    }

Leave:
    /* Free our intermediate buffer and leave */
    RtlFreeHeap(RtlGetProcessHeap(), 0, TargetPathBuffer);

    return RetLength;
}

/*
 * @implemented
 */
BOOL
WINAPI
DosDateTimeToFileTime(IN WORD wFatDate,
                      IN WORD wFatTime,
                      OUT LPFILETIME lpFileTime)
{
    TIME_FIELDS TimeFields;
    LARGE_INTEGER SystemTime;

    TimeFields.Year = (wFatDate >> 9) + 1980;
    TimeFields.Month = (wFatDate >> 5) & 0xF;
    TimeFields.Day = (wFatDate & 0x1F);
    TimeFields.Hour = (wFatTime >> 11);
    TimeFields.Minute = (wFatTime >> 5) & 0x3F;
    TimeFields.Second = (wFatTime & 0x1F) << 1;
    TimeFields.Milliseconds = 0;

    if (RtlTimeFieldsToTime(&TimeFields, &SystemTime))
    {
        lpFileTime->dwLowDateTime = SystemTime.LowPart;
        lpFileTime->dwHighDateTime = SystemTime.HighPart;
        return TRUE;
    }

    BaseSetLastNTError(STATUS_INVALID_PARAMETER);
    return FALSE;
}

/*
 * @implemented
 */
BOOL
WINAPI
FileTimeToDosDateTime(IN CONST FILETIME *lpFileTime,
                      OUT LPWORD lpFatDate,
                      OUT LPWORD lpFatTime)
{
    LARGE_INTEGER FileTime;
    TIME_FIELDS TimeFields;

    FileTime.HighPart = lpFileTime->dwHighDateTime;
    FileTime.LowPart = lpFileTime->dwLowDateTime;

    if (FileTime.QuadPart < 0)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    RtlTimeToTimeFields(&FileTime, &TimeFields);
    if ((TimeFields.Year < 1980) || (TimeFields.Year > 2107))
    {
        BaseSetLastNTError(STATUS_INVALID_PARAMETER);
        return FALSE;
    }

    *lpFatDate = (TimeFields.Day) |
                 (TimeFields.Month << 5) |
                 ((TimeFields.Year - 1980) << 9);
    *lpFatTime = (TimeFields.Second >> 1) |
                 (TimeFields.Minute << 5) |
                 (TimeFields.Hour << 11);

    return TRUE;
}


/* EOF */
