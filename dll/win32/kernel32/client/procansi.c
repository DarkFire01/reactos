/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS system libraries
 * FILE:            dll/win32/kernel32/client/procansi.c
 * PURPOSE:         Process functions
 * PROGRAMMERS:     Ariadne (ariadne@xs4all.nl)
 * UPDATE HISTORY:
 *                  Created 01/11/98
 */

/* INCLUDES ****************************************************************/

#include <k32.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

LPSTARTUPINFOA BaseAnsiStartupInfo = NULL;
WaitForInputIdleType UserWaitForInputIdleRoutine;

/* FUNCTIONS ******************************************************************/

/*
 * @implemented
 */
VOID
WINAPI
RegisterWaitForInputIdle(IN WaitForInputIdleType lpfnRegisterWaitForInputIdle)
{
    /* Write the global function pointer */
    UserWaitForInputIdleRoutine = lpfnRegisterWaitForInputIdle;
}

/*
 * @implemented
 */
VOID
WINAPI
GetStartupInfoA(IN LPSTARTUPINFOA lpStartupInfo)
{
    PRTL_USER_PROCESS_PARAMETERS Params;
    ANSI_STRING TitleString, ShellString, DesktopString;
    LPSTARTUPINFOA StartupInfo;
    NTSTATUS Status;

    /* Get the cached information as well as the PEB parameters */
    StartupInfo = BaseAnsiStartupInfo;
    Params = NtCurrentPeb()->ProcessParameters;

    /* Check if this is the first time we have to get the cached version */
    while (!StartupInfo)
    {
        /* Create new ANSI startup info */
        StartupInfo = RtlAllocateHeap(RtlGetProcessHeap(),
                                      0,
                                      sizeof(*StartupInfo));
        if (StartupInfo)
        {
            /* Zero out string pointers in case we fail to create them */
            StartupInfo->lpReserved = NULL;
            StartupInfo->lpDesktop = NULL;
            StartupInfo->lpTitle = NULL;

            /* Set the size */
            StartupInfo->cb = sizeof(*StartupInfo);

            /* Copy what's already stored in the PEB */
            StartupInfo->dwX = Params->StartingX;
            StartupInfo->dwY = Params->StartingY;
            StartupInfo->dwXSize = Params->CountX;
            StartupInfo->dwYSize = Params->CountY;
            StartupInfo->dwXCountChars = Params->CountCharsX;
            StartupInfo->dwYCountChars = Params->CountCharsY;
            StartupInfo->dwFillAttribute = Params->FillAttribute;
            StartupInfo->dwFlags = Params->WindowFlags;
            StartupInfo->wShowWindow = (WORD)Params->ShowWindowFlags;
            StartupInfo->cbReserved2 = Params->RuntimeData.Length;
            StartupInfo->lpReserved2 = (LPBYTE)Params->RuntimeData.Buffer;
            StartupInfo->hStdInput = Params->StandardInput;
            StartupInfo->hStdOutput = Params->StandardOutput;
            StartupInfo->hStdError = Params->StandardError;

            /* Copy shell info string */
            Status = RtlUnicodeStringToAnsiString(&ShellString,
                                                  &Params->ShellInfo,
                                                  TRUE);
            if (NT_SUCCESS(Status))
            {
                /* Save it */
                StartupInfo->lpReserved = ShellString.Buffer;

                /* Copy desktop info string */
                Status = RtlUnicodeStringToAnsiString(&DesktopString,
                                                      &Params->DesktopInfo,
                                                      TRUE);
                if (NT_SUCCESS(Status))
                {
                    /* Save it */
                    StartupInfo->lpDesktop = DesktopString.Buffer;

                    /* Copy window title string */
                    Status = RtlUnicodeStringToAnsiString(&TitleString,
                                                          &Params->WindowTitle,
                                                          TRUE);
                    if (NT_SUCCESS(Status))
                    {
                        /* Save it */
                        StartupInfo->lpTitle = TitleString.Buffer;

                        /* We finished with the ANSI version, try to cache it */
                        if (!InterlockedCompareExchangePointer((PVOID*)&BaseAnsiStartupInfo,
                                                               StartupInfo,
                                                               NULL))
                        {
                            /* We were the first thread through, use the data */
                            break;
                        }

                        /* Someone beat us to it, we will use their data instead */
                        /* We're going to free our own stuff, but not raise */
                        RtlFreeAnsiString(&TitleString);
                    }
                    RtlFreeAnsiString(&DesktopString);
                }
                RtlFreeAnsiString(&ShellString);
            }
            RtlFreeHeap(RtlGetProcessHeap(), 0, StartupInfo);
        }
        else
        {
            /* No memory, fail */
            Status = STATUS_NO_MEMORY;
        }

        /* Raise an error if there is no cached information */
        if (!BaseAnsiStartupInfo)
            RtlRaiseStatus(Status);

        /* Get the cached information again: set by another thread */
        StartupInfo = BaseAnsiStartupInfo;
    }

    /* Now copy from the cached ANSI version */
    lpStartupInfo->cb = StartupInfo->cb;
    lpStartupInfo->lpReserved = StartupInfo->lpReserved;
    lpStartupInfo->lpDesktop = StartupInfo->lpDesktop;
    lpStartupInfo->lpTitle = StartupInfo->lpTitle;
    lpStartupInfo->dwX = StartupInfo->dwX;
    lpStartupInfo->dwY = StartupInfo->dwY;
    lpStartupInfo->dwXSize = StartupInfo->dwXSize;
    lpStartupInfo->dwYSize = StartupInfo->dwYSize;
    lpStartupInfo->dwXCountChars = StartupInfo->dwXCountChars;
    lpStartupInfo->dwYCountChars = StartupInfo->dwYCountChars;
    lpStartupInfo->dwFillAttribute = StartupInfo->dwFillAttribute;
    lpStartupInfo->dwFlags = StartupInfo->dwFlags;
    lpStartupInfo->wShowWindow = StartupInfo->wShowWindow;
    lpStartupInfo->cbReserved2 = StartupInfo->cbReserved2;
    lpStartupInfo->lpReserved2 = StartupInfo->lpReserved2;

    /* Check if the shell is hijacking the handles for other features */
    if (lpStartupInfo->dwFlags &
        (STARTF_USESTDHANDLES | STARTF_USEHOTKEY | STARTF_SHELLPRIVATE))
    {
        /* It isn't, so we can return the raw values */
        lpStartupInfo->hStdInput = StartupInfo->hStdInput;
        lpStartupInfo->hStdOutput = StartupInfo->hStdOutput;
        lpStartupInfo->hStdError = StartupInfo->hStdError;
    }
    else
    {
        /* It is, so make sure nobody uses these as console handles */
        lpStartupInfo->hStdInput = INVALID_HANDLE_VALUE;
        lpStartupInfo->hStdOutput = INVALID_HANDLE_VALUE;
        lpStartupInfo->hStdError = INVALID_HANDLE_VALUE;
    }
}

/*
 * @implemented
 */
UINT
WINAPI
DECLSPEC_HOTPATCH
WinExec(LPCSTR lpCmdLine,
        UINT uCmdShow)
{
    STARTUPINFOA StartupInfo;
    PROCESS_INFORMATION  ProcessInformation;
    DWORD dosErr;

    RtlZeroMemory(&StartupInfo, sizeof(StartupInfo));
    StartupInfo.cb = sizeof(STARTUPINFOA);
    StartupInfo.wShowWindow = (WORD)uCmdShow;
    StartupInfo.dwFlags = 0;

    if (!CreateProcessA(NULL,
                        (PVOID)lpCmdLine,
                        NULL,
                        NULL,
                        FALSE,
                        0,
                        NULL,
                        NULL,
                        &StartupInfo,
                        &ProcessInformation))
    {
        dosErr = GetLastError();
        return dosErr < 32 ? dosErr : ERROR_BAD_FORMAT;
    }

    if (NULL != UserWaitForInputIdleRoutine)
    {
        UserWaitForInputIdleRoutine(ProcessInformation.hProcess,
                                           10000);
    }

    NtClose(ProcessInformation.hProcess);
    NtClose(ProcessInformation.hThread);

    return 33; /* Something bigger than 31 means success. */
}

/*
 * @implemented
 */
BOOL
WINAPI
GetProcessAffinityMask(IN HANDLE hProcess,
                       OUT PDWORD_PTR lpProcessAffinityMask,
                       OUT PDWORD_PTR lpSystemAffinityMask)
{
    PROCESS_BASIC_INFORMATION ProcessInfo;
    NTSTATUS Status;
    SYSTEM_INFO SystemInfo = {};

    /* Query information on the process from the kernel */
    Status = NtQueryInformationProcess(hProcess,
                                       ProcessBasicInformation,
                                       &ProcessInfo,
                                       sizeof(ProcessInfo),
                                       NULL);
    if (!NT_SUCCESS(Status))
    {
        /* Fail */
        BaseSetLastNTError(Status);
        return FALSE;
    }

    GetSystemInfo(&SystemInfo);

    /* Copy the affinity mask, and get the system one from our shared data */
    *lpProcessAffinityMask = ProcessInfo.AffinityMask;
    *lpSystemAffinityMask = SystemInfo.dwActiveProcessorMask;
    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
SetProcessAffinityMask(IN HANDLE hProcess,
                       IN DWORD_PTR dwProcessAffinityMask)
{
    NTSTATUS Status;

    /* Directly set the affinity mask */
    Status = NtSetInformationProcess(hProcess,
                                     ProcessAffinityMask,
                                     (PVOID)&dwProcessAffinityMask,
                                     sizeof(dwProcessAffinityMask));
    if (!NT_SUCCESS(Status))
    {
        /* Handle failure */
        BaseSetLastNTError(Status);
        return FALSE;
    }

    /* Everything was ok */
    return TRUE;
}

/*
 * @implemented
 */
VOID
WINAPI
FatalExit(IN int ExitCode)
{
#if DBG
    /* On Checked builds, Windows gives the user a nice little debugger UI */
    CHAR Action[2];
    DbgPrint("FatalExit...\n\n");

    /* Check for reactos specific flag (set by rosautotest) */
    if (RtlGetNtGlobalFlags() & FLG_DISABLE_DEBUG_PROMPTS)
    {
        RtlRaiseStatus(STATUS_FATAL_APP_EXIT);
    }

    while (TRUE)
    {
        DbgPrompt("A (Abort), B (Break), I (Ignore)? ", Action, sizeof(Action));
        switch (Action[0])
        {
            case 'B': case 'b':
                 DbgBreakPoint();
                 break;

            case 'A': case 'a':
                ExitProcess(ExitCode);

            case 'I': case 'i':
                return;
        }
    }
#endif
    /* On other builds, just kill the process */
    ExitProcess(ExitCode);
}

// mikhail tyukin todo move to psdk
extern BOOL
WINAPI
SetProcessWorkingSetSizeEx(IN HANDLE hProcess,
                           IN SIZE_T dwMinimumWorkingSetSize,
                           IN SIZE_T dwMaximumWorkingSetSize,
                           IN DWORD Flags);
extern BOOL
WINAPI
GetProcessWorkingSetSizeEx(IN HANDLE hProcess,
                           OUT PSIZE_T lpMinimumWorkingSetSize,
                           OUT PSIZE_T lpMaximumWorkingSetSize,
                           OUT PDWORD Flags);

/*
 * @implemented
 */
BOOL
WINAPI
SetProcessWorkingSetSize(IN HANDLE hProcess,
                         IN SIZE_T dwMinimumWorkingSetSize,
                         IN SIZE_T dwMaximumWorkingSetSize)
{
    /* Call the newer API */
    return SetProcessWorkingSetSizeEx(hProcess,
                                      dwMinimumWorkingSetSize,
                                      dwMaximumWorkingSetSize,
                                      0);
}

/*
 * @implemented
 */
BOOL
WINAPI
GetProcessIoCounters(IN HANDLE hProcess,
                     OUT PIO_COUNTERS lpIoCounters)
{
    NTSTATUS Status;

    /* Query the kernel. Structures are identical, so let it do the copy too. */
    Status = NtQueryInformationProcess(hProcess,
                                       ProcessIoCounters,
                                       lpIoCounters,
                                       sizeof(IO_COUNTERS),
                                       NULL);
    if (!NT_SUCCESS(Status))
    {
        /* Handle error path */
        BaseSetLastNTError(Status);
        return FALSE;
    }

    /* All done */
    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
GetProcessWorkingSetSize(IN HANDLE hProcess,
                         OUT PSIZE_T lpMinimumWorkingSetSize,
                         OUT PSIZE_T lpMaximumWorkingSetSize)
{
    DWORD Dummy;
    return GetProcessWorkingSetSizeEx(hProcess,
                                      lpMinimumWorkingSetSize,
                                      lpMaximumWorkingSetSize,
                                      &Dummy);
}

/*
 * @implemented
 */
BOOL
WINAPI
GetThreadSelectorEntry(IN HANDLE hThread,
                       IN DWORD dwSelector,
                       OUT LPLDT_ENTRY lpSelectorEntry)
{
#ifdef _M_IX86
    DESCRIPTOR_TABLE_ENTRY DescriptionTableEntry;
    NTSTATUS Status;

    /* Set the selector and do the query */
    DescriptionTableEntry.Selector = dwSelector;
    Status = NtQueryInformationThread(hThread,
                                      ThreadDescriptorTableEntry,
                                      &DescriptionTableEntry,
                                      sizeof(DESCRIPTOR_TABLE_ENTRY),
                                      NULL);
    if (!NT_SUCCESS(Status))
    {
        /* Fail */
        BaseSetLastNTError(Status);
        return FALSE;
    }

    /* Success, return the selector */
    *lpSelectorEntry = DescriptionTableEntry.Descriptor;
    return TRUE;
#else
    DPRINT1("Calling GetThreadSelectorEntry!\n");
    return FALSE;
#endif
}

/*
 * @implemented
 */
DWORD
WINAPI
LoadModule(LPCSTR lpModuleName,
           LPVOID lpParameterBlock)
{
    STARTUPINFOA StartupInfo;
    PROCESS_INFORMATION ProcessInformation;
    LOADPARMS32 *LoadParams;
    char FileName[MAX_PATH];
    LPSTR CommandLine;
    DWORD Length, Error;
    BOOL ProcessStatus;
    ANSI_STRING AnsiStr;
    UNICODE_STRING UnicStr;
    RTL_PATH_TYPE PathType;
    HANDLE Handle;

    LoadParams = (LOADPARMS32*)lpParameterBlock;

    /* Check load parameters */
    if (LoadParams->dwReserved || LoadParams->wMagicValue != 2)
    {
        /* Fail with invalid param error */
        BaseSetLastNTError(STATUS_INVALID_PARAMETER);
        return 0;
    }

    /* Search path */
    Length = SearchPathA(NULL, lpModuleName, ".exe", MAX_PATH, FileName, NULL);

    /* Check if path was found */
    if (Length && Length < MAX_PATH)
    {
        /* Build StartupInfo */
        RtlZeroMemory(&StartupInfo, sizeof(StartupInfo));

        StartupInfo.cb = sizeof(STARTUPINFOA);
        StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
        StartupInfo.wShowWindow = LoadParams->wCmdShow;

        /* Allocate command line buffer */
        CommandLine = RtlAllocateHeap(RtlGetProcessHeap(),
                                      HEAP_ZERO_MEMORY,
                                      (ULONG)LoadParams->lpCmdLine[0] + Length + 2);

        /* Put module name there, then a space, and then copy provided command line,
           and null-terminate it */
        RtlCopyMemory(CommandLine, FileName, Length);
        CommandLine[Length] = ' ';
        RtlCopyMemory(&CommandLine[Length + 1], &LoadParams->lpCmdLine[1], (ULONG)LoadParams->lpCmdLine[0]);
        CommandLine[Length + 1 + (ULONG)LoadParams->lpCmdLine[0]] = 0;

        /* Create the process */
        ProcessStatus = CreateProcessA(FileName,
                                       CommandLine,
                                       NULL,
                                       NULL,
                                       FALSE,
                                       0,
                                       LoadParams->lpEnvAddress,
                                       NULL,
                                       &StartupInfo,
                                       &ProcessInformation);

        /* Free the command line buffer */
        RtlFreeHeap(RtlGetProcessHeap(), 0, CommandLine);

        if (!ProcessStatus)
        {
            /* Creating process failed, return right error code */
            Error = GetLastError();
            switch(Error)
            {
            case ERROR_BAD_EXE_FORMAT:
               return ERROR_BAD_FORMAT;

            case ERROR_FILE_NOT_FOUND:
            case ERROR_PATH_NOT_FOUND:
                return Error;
            }

            /* Return 0 otherwise */
            return 0;
        }

        /* Wait up to 30 seconds for the process to become idle */
        if (UserWaitForInputIdleRoutine)
        {
            UserWaitForInputIdleRoutine(ProcessInformation.hProcess, 30000);
        }

        /* Close handles */
        NtClose(ProcessInformation.hThread);
        NtClose(ProcessInformation.hProcess);

        /* Return magic success value (33) */
        return 33;
    }

    /* The path was not found, create an ansi string from
        the module name and convert it to unicode */
    RtlInitAnsiString(&AnsiStr, lpModuleName);
    if (!NT_SUCCESS(RtlAnsiStringToUnicodeString(&UnicStr,&AnsiStr,TRUE)))
        return ERROR_FILE_NOT_FOUND;

    /* Determine path type */
    PathType = RtlDetermineDosPathNameType_U(UnicStr.Buffer);

    /* Free the unicode module name */
    RtlFreeUnicodeString(&UnicStr);

    /* If it's a relative path, return file not found */
    if (PathType == RtlPathTypeRelative)
        return ERROR_FILE_NOT_FOUND;

    /* If not, try to open it */
    Handle = CreateFile(lpModuleName,
                        GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL,
                        OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL,
                        NULL);

    if (Handle != INVALID_HANDLE_VALUE)
    {
        /* Opening file succeeded for some reason, close the handle and return file not found anyway */
        CloseHandle(Handle);
        return ERROR_FILE_NOT_FOUND;
    }

    /* Return last error which CreateFile set during an attempt to open it */
    return GetLastError();
}


/*
 * @implemented
 */
BOOL
WINAPI
MapUserPhysicalPagesScatter(IN PVOID *VirtualAddresses,
                            IN ULONG_PTR NumberOfPages,
                            OUT PULONG_PTR PageArray OPTIONAL)
{
    NTSTATUS Status;

    Status = NtMapUserPhysicalPagesScatter(VirtualAddresses,
                                           NumberOfPages,
                                           PageArray);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    return TRUE;
}

/* EOF */
