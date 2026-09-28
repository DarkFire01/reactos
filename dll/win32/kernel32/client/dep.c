/*
 * PROJECT:     ReactOS Kernel32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Data Execution Prevention (DEP) policy functions
 * COPYRIGHT:   Copyright 2010 Detlef Riekenberg
 *              Copyright 2011 Austin English
 *              Copyright 2026 Mark Jansen <mark.jansen@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <k32.h>

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief
 * Reads back one of the ways a process can be hardened.
 *
 * @remarks
 * None of them is applied here, so every policy reads as the default, which is
 * what a caller sees for a policy it never set.
 *
 * @implemented
 */
BOOL
WINAPI
GetProcessMitigationPolicy(
    _In_ HANDLE hProcess,
    _In_ PROCESS_MITIGATION_POLICY MitigationPolicy,
    _Out_writes_bytes_(dwLength) PVOID lpBuffer,
    _In_ SIZE_T dwLength)
{
    UNREFERENCED_PARAMETER(hProcess);

    if ((MitigationPolicy >= MaxProcessMitigationPolicy) ||
        (lpBuffer == NULL) || (dwLength == 0))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    RtlZeroMemory(lpBuffer, dwLength);
    return TRUE;
}

/**
 * @brief
 * Asks for the calling process to be hardened one of the ways it can be.
 *
 * @remarks
 * Nothing is applied, and the caller is told it was: a process that asks to be
 * hardened runs the same either way, and treating the request as a failure
 * would stop callers that harden themselves on the way up.
 */
BOOL
WINAPI
SetProcessMitigationPolicy(
    _In_ PROCESS_MITIGATION_POLICY MitigationPolicy,
    _In_reads_bytes_(dwLength) PVOID lpBuffer,
    _In_ SIZE_T dwLength)
{
    if ((MitigationPolicy >= MaxProcessMitigationPolicy) ||
        (lpBuffer == NULL) || (dwLength == 0))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    return TRUE;
}

/*
 * @implemented
 */
DEP_SYSTEM_POLICY_TYPE
WINAPI
GetSystemDEPPolicy(VOID)
{
    return (DEP_SYSTEM_POLICY_TYPE)SharedUserData->NXSupportPolicy;
}

/*
 * @implemented
 */
BOOL
WINAPI
GetProcessDEPPolicy(
    _In_ HANDLE hProcess,
    _Out_opt_ LPDWORD lpFlags,
    _Out_opt_ PBOOL lpPermanent)
{
    ULONG ExecuteFlags;
    NTSTATUS Status;

    /* ProcessExecuteFlags is only valid for the current process */
    UNREFERENCED_PARAMETER(hProcess);

    Status = NtQueryInformationProcess(hProcess,
                                       ProcessExecuteFlags,
                                       &ExecuteFlags,
                                       sizeof(ExecuteFlags),
                                       NULL);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    if (lpFlags)
    {
        *lpFlags = 0;
        if (ExecuteFlags & MEM_EXECUTE_OPTION_DISABLE)
            *lpFlags |= PROCESS_DEP_ENABLE;
        if (ExecuteFlags & MEM_EXECUTE_OPTION_DISABLE_THUNK_EMULATION)
            *lpFlags |= PROCESS_DEP_DISABLE_ATL_THUNK_EMULATION;
    }

    if (lpPermanent)
        *lpPermanent = (ExecuteFlags & MEM_EXECUTE_OPTION_PERMANENT) != 0;

    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
SetProcessDEPPolicy(
    _In_ DWORD dwFlags)
{
    ULONG ExecuteFlags = 0;
    NTSTATUS Status;

    if (dwFlags & PROCESS_DEP_ENABLE)
        ExecuteFlags |= MEM_EXECUTE_OPTION_DISABLE | MEM_EXECUTE_OPTION_PERMANENT;
    if (dwFlags & PROCESS_DEP_DISABLE_ATL_THUNK_EMULATION)
        ExecuteFlags |= MEM_EXECUTE_OPTION_DISABLE_THUNK_EMULATION;

    Status = NtSetInformationProcess(NtCurrentProcess(),
                                     ProcessExecuteFlags,
                                     &ExecuteFlags,
                                     sizeof(ExecuteFlags));
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    return TRUE;
}
