/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS system libraries
 * FILE:            dll/win32/kernel32/client/utils.c
 * PURPOSE:         Utility and Support Functions
 * PROGRAMMER:      Alex Ionescu (alex@relsoft.net)
 *                  Pierre Schweitzer (pierre.schweitzer@reactos.org)
 */

/* INCLUDES ******************************************************************/

#include <k32.h>
#ifdef _M_IX86
#include "i386/ketypes.h"
#elif defined _M_AMD64
#include "amd64/ketypes.h"
#endif

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

UNICODE_STRING Restricted = RTL_CONSTANT_STRING(L"Restricted");
BOOL bIsFileApiAnsi = TRUE; // set the file api to ansi or oem

PRTL_CONVERT_STRING Basep8BitStringToUnicodeString = RtlAnsiStringToUnicodeString;
PRTL_CONVERT_STRINGA BasepUnicodeStringTo8BitString = RtlUnicodeStringToAnsiString;
PRTL_COUNT_STRING BasepUnicodeStringTo8BitSize = BasepUnicodeStringToAnsiSize;
PRTL_COUNT_STRINGA Basep8BitStringToUnicodeSize = BasepAnsiStringToUnicodeSize;

/* FUNCTIONS ******************************************************************/

ULONG
NTAPI
BasepUnicodeStringToOemSize(IN PUNICODE_STRING String)
{
    return RtlUnicodeStringToOemSize(String);
}

ULONG
NTAPI
BasepOemStringToUnicodeSize(IN PANSI_STRING String)
{
    return RtlOemStringToUnicodeSize(String);
}

ULONG
NTAPI
BasepUnicodeStringToAnsiSize(IN PUNICODE_STRING String)
{
    return RtlUnicodeStringToAnsiSize(String);
}

ULONG
NTAPI
BasepAnsiStringToUnicodeSize(IN PANSI_STRING String)
{
    return RtlAnsiStringToUnicodeSize(String);
}

// TODO: Mikhail Tyukin move to PSDK
extern NTSTATUS
WINAPI
BaseGetNamedObjectDirectory(
    _Out_ PHANDLE DirectoryHandle);

HANDLE
WINAPI
BasepGetNamedObjectDirectory(VOID)
{
    NTSTATUS Status;
    HANDLE DirectoryHandle;

    Status = BaseGetNamedObjectDirectory(&DirectoryHandle);
    if (!NT_SUCCESS(Status))
    {
        return NULL;
    }

    return DirectoryHandle;
}

/*
 * Converts an ANSI or OEM String to the TEB StaticUnicodeString
 */
PUNICODE_STRING
WINAPI
Basep8BitStringToStaticUnicodeString(IN LPCSTR String)
{
    PUNICODE_STRING StaticString = &(NtCurrentTeb()->StaticUnicodeString);
    ANSI_STRING AnsiString;
    NTSTATUS Status;

    /* Initialize an ANSI String */
    Status = RtlInitAnsiStringEx(&AnsiString, String);
    if (!NT_SUCCESS(Status))
    {
        Status = STATUS_BUFFER_OVERFLOW;
    }
    else
    {
        /* Convert it */
        Status = Basep8BitStringToUnicodeString(StaticString, &AnsiString, FALSE);
    }

    if (NT_SUCCESS(Status)) return StaticString;

    if (Status == STATUS_BUFFER_OVERFLOW)
    {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
    }
    else
    {
        BaseSetLastNTError(Status);
    }

    return NULL;
}

/*
 * Allocates space from the Heap and converts an Unicode String into it
 */
BOOLEAN
WINAPI
Basep8BitStringToDynamicUnicodeString(OUT PUNICODE_STRING UnicodeString,
                                      IN LPCSTR String)
{
    ANSI_STRING AnsiString;
    NTSTATUS Status;

    /* Initialize an ANSI String */
    Status = RtlInitAnsiStringEx(&AnsiString, String);
    if (!NT_SUCCESS(Status))
    {
        Status = STATUS_BUFFER_OVERFLOW;
    }
    else
    {
        /* Convert it */
        Status = Basep8BitStringToUnicodeString(UnicodeString, &AnsiString, TRUE);
    }

    if (NT_SUCCESS(Status)) return TRUE;

    if (Status == STATUS_BUFFER_OVERFLOW)
    {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
    }
    else
    {
        BaseSetLastNTError(Status);
    }

    return FALSE;
}

/*
 * Allocates space from the Heap and converts an Ansi String into it
 */
 /*NOTE: API IS A HACK */
VOID
WINAPI
BasepAnsiStringToHeapUnicodeString(IN LPCSTR AnsiString,
                                   OUT LPWSTR* UnicodeString)
{
    ANSI_STRING AnsiTemp;
    UNICODE_STRING UnicodeTemp;

    DPRINT("BasepAnsiStringToHeapUnicodeString\n");

    /* First create the ANSI_STRING */
    RtlInitAnsiString(&AnsiTemp, AnsiString);

    if (NT_SUCCESS(RtlAnsiStringToUnicodeString(&UnicodeTemp,
                                                &AnsiTemp,
                                                TRUE)))
    {
        *UnicodeString = UnicodeTemp.Buffer;
    }
    else
    {
        *UnicodeString = NULL;
    }
}

PLARGE_INTEGER
WINAPI
BaseFormatTimeOut(OUT PLARGE_INTEGER Timeout,
                  IN DWORD dwMilliseconds)
{
    /* Check if this is an infinite wait, which means no timeout argument */
    if (dwMilliseconds == INFINITE) return NULL;

    /* Otherwise, convert the time to NT Format */
    Timeout->QuadPart = dwMilliseconds * -10000LL;
    return Timeout;
}

/*
 * Checks if the privilege for Real-Time Priority is there
 * Beware about this function behavior:
 * - In case Keep is set to FALSE, then the function will only check
 * whether real time is allowed and won't grant the privilege. In that case
 * it will return TRUE if allowed, FALSE otherwise. Not a state!
 * It means you don't have to release privilege when calling with FALSE.
 */
PVOID
WINAPI
BasepIsRealtimeAllowed(IN BOOLEAN Keep)
{
    ULONG Privilege = SE_INC_BASE_PRIORITY_PRIVILEGE;
    PVOID State;
    NTSTATUS Status;

    Status = RtlAcquirePrivilege(&Privilege, 1, 0, &State);
    if (!NT_SUCCESS(Status)) return NULL;

    if (!Keep)
    {
        RtlReleasePrivilege(State);
        State = (PVOID)TRUE;
    }

    return State;
}

/*
 * @implemented
 */
BOOLEAN
WINAPI
Wow64EnableWow64FsRedirection(IN BOOLEAN Wow64EnableWow64FsRedirection)
{
    NTSTATUS Status;
    BOOL Result;

    Status = RtlWow64EnableFsRedirection(Wow64EnableWow64FsRedirection);
    if (NT_SUCCESS(Status))
    {
        Result = TRUE;
    }
    else
    {
        BaseSetLastNTError(Status);
        Result = FALSE;
    }
    return Result;
}

/*
 * @implemented
 */
BOOL
WINAPI
Wow64DisableWow64FsRedirection(IN PVOID *OldValue)
{
    NTSTATUS Status;
    BOOL Result;

    Status = RtlWow64EnableFsRedirectionEx((PVOID)TRUE, OldValue);
    if (NT_SUCCESS(Status))
    {
        Result = TRUE;
    }
    else
    {
        BaseSetLastNTError(Status);
        Result = FALSE;
    }
    return Result;
}

/*
 * @implemented
 */
BOOL
WINAPI
Wow64RevertWow64FsRedirection(IN PVOID OldValue)
{
    NTSTATUS Status;
    BOOL Result;

    Status = RtlWow64EnableFsRedirectionEx(OldValue, &OldValue);
    if (NT_SUCCESS(Status))
    {
        Result = TRUE;
    }
    else
    {
        BaseSetLastNTError(Status);
        Result = FALSE;
    }
    return Result;
}

/*
 * @implemented
 */
VOID
WINAPI
SetFileApisToOEM(VOID)
{
    /* Set the correct Base Api */
    Basep8BitStringToUnicodeString = (PRTL_CONVERT_STRING)RtlOemStringToUnicodeString;
    BasepUnicodeStringTo8BitString = RtlUnicodeStringToOemString;
    BasepUnicodeStringTo8BitSize = BasepUnicodeStringToOemSize;
    Basep8BitStringToUnicodeSize = BasepOemStringToUnicodeSize;

    /* FIXME: Old, deprecated way */
    bIsFileApiAnsi = FALSE;
}


/*
 * @implemented
 */
VOID
WINAPI
SetFileApisToANSI(VOID)
{
    /* Set the correct Base Api */
    Basep8BitStringToUnicodeString = RtlAnsiStringToUnicodeString;
    BasepUnicodeStringTo8BitString = RtlUnicodeStringToAnsiString;
    BasepUnicodeStringTo8BitSize = BasepUnicodeStringToAnsiSize;
    Basep8BitStringToUnicodeSize = BasepAnsiStringToUnicodeSize;

    /* FIXME: Old, deprecated way */
    bIsFileApiAnsi = TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
AreFileApisANSI(VOID)
{
   return Basep8BitStringToUnicodeString == RtlAnsiStringToUnicodeString;
}

/*
 * @implemented
 */
VOID
WINAPI
BaseMarkFileForDelete(IN HANDLE FileHandle,
                      IN ULONG FileAttributes)
{
    IO_STATUS_BLOCK IoStatusBlock;
    FILE_BASIC_INFORMATION FileBasicInfo;
    FILE_DISPOSITION_INFORMATION FileDispositionInfo;

    /* If no attributes were given, get them */
    if (!FileAttributes)
    {
        FileBasicInfo.FileAttributes = 0;
        NtQueryInformationFile(FileHandle,
                               &IoStatusBlock,
                               &FileBasicInfo,
                               sizeof(FileBasicInfo),
                               FileBasicInformation);
        FileAttributes = FileBasicInfo.FileAttributes;
    }

    /* If file is marked as RO, reset its attributes */
    if (FileAttributes & FILE_ATTRIBUTE_READONLY)
    {
        RtlZeroMemory(&FileBasicInfo, sizeof(FileBasicInfo));
        FileBasicInfo.FileAttributes = FILE_ATTRIBUTE_NORMAL;
        NtSetInformationFile(FileHandle,
                             &IoStatusBlock,
                             &FileBasicInfo,
                             sizeof(FileBasicInfo),
                             FileBasicInformation);
    }

    /* Finally, mark the file for deletion */
    FileDispositionInfo.DeleteFile = TRUE;
    NtSetInformationFile(FileHandle,
                         &IoStatusBlock,
                         &FileDispositionInfo,
                         sizeof(FileDispositionInfo),
                         FileDispositionInformation);
}

/*
 * @unimplemented
 */
NTSTATUS
WINAPI
BasepCheckWinSaferRestrictions(IN HANDLE UserToken,
                               IN LPWSTR ApplicationName,
                               IN HANDLE FileHandle,
                               OUT PBOOLEAN InJob,
                               OUT PHANDLE NewToken,
                               OUT PHANDLE JobHandle)
{
    NTSTATUS Status;

    /* Validate that there's a name */
    if ((ApplicationName) && *(ApplicationName))
    {
        /* Validate that the required output parameters are there */
        if ((InJob) && (NewToken) && (JobHandle))
        {
            /* Do the work (one day...) */
            DPRINT("BasepCheckWinSaferRestrictions is UNIMPLEMENTED\n");
            Status = STATUS_SUCCESS;
        }
        else
        {
            /* Act as if SEH hit this */
            Status = STATUS_ACCESS_VIOLATION;
        }
    }
    else
    {
        /* Input is invalid */
        Status = STATUS_INVALID_PARAMETER;
    }

    /* Return the status */
    return Status;
}

/*
 * @implemented
 */
VOID
WINAPI
BasepFreeAppCompatData(IN PVOID AppCompatData,
                       IN PVOID AppCompatSxsData)
{
    /* Free the input pointers if present */
    if (AppCompatData) RtlFreeHeap(RtlGetProcessHeap(), 0, AppCompatData);
    if (AppCompatSxsData) RtlFreeHeap(RtlGetProcessHeap(), 0, AppCompatSxsData);
}

// Note for myself, me and I Mikhail Tyukin: Move this to a header
NTSYSAPI NTSTATUS  WINAPI NtSetInformationDebugObject(HANDLE,DEBUGOBJECTINFOCLASS,PVOID,ULONG,ULONG*);
/*
 * @implemented
 */
BOOL
WINAPI
DebugSetProcessKillOnExit(IN BOOL KillOnExit)
{
    HANDLE Handle;
    NTSTATUS Status;
    ULONG State;

    /* Get the debug object */
    Handle = DbgUiGetThreadDebugObject();
    if (!Handle)
    {
        /* Fail */
        BaseSetLastNTError(STATUS_INVALID_HANDLE);
        return FALSE;
    }

    /* Now set the kill-on-exit state */
    State = KillOnExit != 0;
    Status = NtSetInformationDebugObject(Handle,
                                         DebugObjectKillProcessOnExitInformation,
                                         &State,
                                         sizeof(State),
                                         NULL);
    if (!NT_SUCCESS(Status))
    {
        /* Fail */
        BaseSetLastNTError(Status);
        return FALSE;
    }

    /* Success */
    return TRUE;
}

/*
 * @implemented
 */
DWORD
WINAPI
WTSGetActiveConsoleSessionId(VOID)
{
    return SharedUserData->ActiveConsoleId;
}

/*
 * @implemented
 */
BOOL
WINAPI
SetComPlusPackageInstallStatus(IN ULONG ComPlusPackage)
{
    NTSTATUS Status;

    DPRINT("(0x%X)\n", ComPlusPackage);

    if (ComPlusPackage & ~1)
    {
        DPRINT1("0x%lX\n", ComPlusPackage);
        BaseSetLastNTError(STATUS_INVALID_PARAMETER);
        return FALSE;
    }

    Status = NtSetSystemInformation(SystemComPlusPackage, &ComPlusPackage, sizeof(ComPlusPackage));
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("0x%lX\n", Status);
        BaseSetLastNTError(Status);
        return FALSE;
    }

    return TRUE;
}



/*
 * @implemented
 */
DWORD
WINAPI
GetHandleContext(IN HANDLE Handle)
{
    /* This is Windows behavior, not a ReactOS Stub */
    DbgPrintEx(0, 0, "Unsupported API - kernel32!GetHandleContext() called\n");
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

/*
 * @implemented
 */
HANDLE
WINAPI
CreateSocketHandle(VOID)
{
    /* This is Windows behavior, not a ReactOS Stub */
    DbgPrintEx(0, 0, "Unsupported API - kernel32!CreateSocketHandle() called\n");
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

/*
 * @implemented
 */
BOOL
WINAPI
SetHandleContext(IN HANDLE Handle,
                 IN DWORD Context)
{
    /* This is Windows behavior, not a ReactOS Stub */
    DbgPrintEx(0, 0, "Unsupported API - kernel32!SetHandleContext() called\n");
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

#define THEONLYGROUP 0 // Fake support for a single group

static DWORD
GetProcessorCountInfo(IN WORD GroupNumber, IN BOOL Maximum)
{
    SYSTEM_INFO info;

    // TODO: SharedUserData->ActiveProcessorCount for ALL_PROCESSOR_GROUPS?
    if (GroupNumber != ALL_PROCESSOR_GROUPS && GroupNumber != THEONLYGROUP)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    GetNativeSystemInfo(&info);
    return info.dwNumberOfProcessors;
}

/*
 * @implemented
 */
WORD
WINAPI
GetActiveProcessorGroupCount(VOID)
{
    STUB; // FIXME: Real group support
    return 1; // TODO: SharedUserData->ActiveGroupCount?
}

/*
 * @implemented
 */
DWORD
WINAPI
GetActiveProcessorCount(IN WORD GroupNumber)
{
    STUB; // FIXME: Real group support
    return GetProcessorCountInfo(GroupNumber, FALSE);
}

/*
 * @implemented
 */
DWORD
WINAPI
GetMaximumProcessorCount(IN WORD GroupNumber)
{
    STUB; // FIXME: Real group support
    return GetProcessorCountInfo(GroupNumber, TRUE);
}

WORD
WINAPI
GetMaximumProcessorGroupCount()
{
    STUB;  // FIXME: Real group support
    return 1;
}

#define FSCTL_PIPE_GET_CONNECTION_ATTRIBUTE CTL_CODE(FILE_DEVICE_NAMED_PIPE, 12, METHOD_BUFFERED, FILE_ANY_ACCESS)

static inline BOOL set_ntstatus( NTSTATUS status )
{
    if (status) SetLastError( RtlNtStatusToDosError( status ));
    return !status;
}

/***********************************************************************
 *           GetNamedPipeClientProcessId  (KERNEL32.@)
 */
BOOL WINAPI GetNamedPipeClientProcessId( HANDLE pipe, ULONG *id )
{
    IO_STATUS_BLOCK iosb;

    return set_ntstatus( NtFsControlFile( pipe, NULL, NULL, NULL, &iosb,
                                          FSCTL_PIPE_GET_CONNECTION_ATTRIBUTE, (void *)"ClientProcessId",
                                          sizeof("ClientProcessId"), id, sizeof(*id) ));
}

/*
 * @implemented
 */
BOOL
WINAPI
MoveFileW(IN LPCWSTR lpExistingFileName,
          IN LPCWSTR lpNewFileName)
{
    return MoveFileWithProgressW(lpExistingFileName,
                                 lpNewFileName,
                                 NULL,
                                 NULL,
                                 MOVEFILE_COPY_ALLOWED);
}



/*
 * @implemented
 */
HANDLE
NTAPI
OpenFileMappingA(IN DWORD dwDesiredAccess,
                 IN BOOL bInheritHandle,
                 IN LPCSTR lpName)
{
    ConvertOpenWin32AnsiObjectApiToUnicodeApi(FileMapping, dwDesiredAccess, bInheritHandle, lpName);
}


/*
 * @implemented
 */
BOOL
WINAPI
DebugBreakProcess(IN HANDLE Process)
{
    NTSTATUS Status;

    /* Send the breakin request */
    Status = DbgUiIssueRemoteBreakin(Process);
    if (!NT_SUCCESS(Status))
    {
        /* Failure */
        BaseSetLastNTError(Status);
        return FALSE;
    }

    /* Success */
    return TRUE;
}

extern BOOLEAN
WINAPI
CreateSymbolicLinkW(IN LPCWSTR lpSymlinkFileName,
                    IN LPCWSTR lpTargetFileName,
                    IN DWORD dwFlags); // Mikhail Tyukin move to PSDK

/*
 * @implemented
 */
BOOLEAN
NTAPI
CreateSymbolicLinkA(IN LPCSTR lpSymlinkFileName,
                    IN LPCSTR lpTargetFileName,
                    IN DWORD dwFlags)
{
    PWCHAR SymlinkW, TargetW;
    BOOLEAN Ret;

    if(!lpSymlinkFileName || !lpTargetFileName)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (!(SymlinkW = FilenameA2W(lpSymlinkFileName, FALSE)))
        return FALSE;

    if (!(TargetW = FilenameA2W(lpTargetFileName, TRUE)))
        return FALSE;

    Ret = CreateSymbolicLinkW(SymlinkW,
                              TargetW,
                              dwFlags);

    RtlFreeHeap(RtlGetProcessHeap(), 0, SymlinkW);
    RtlFreeHeap(RtlGetProcessHeap(), 0, TargetW);

    return Ret;
}

/*
 * @implemented
 */
DWORD_PTR
WINAPI
SetThreadAffinityMask(IN HANDLE hThread,
                      IN DWORD_PTR dwThreadAffinityMask)
{
    THREAD_BASIC_INFORMATION ThreadBasic;
    KAFFINITY AffinityMask;
    NTSTATUS Status;

    AffinityMask = (KAFFINITY)dwThreadAffinityMask;

    Status = NtQueryInformationThread(hThread,
                                      ThreadBasicInformation,
                                      &ThreadBasic,
                                      sizeof(THREAD_BASIC_INFORMATION),
                                      NULL);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return 0;
    }

    Status = NtSetInformationThread(hThread,
                                    ThreadAffinityMask,
                                    &AffinityMask,
                                    sizeof(KAFFINITY));
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        ThreadBasic.AffinityMask = 0;
    }

    return ThreadBasic.AffinityMask;
}
