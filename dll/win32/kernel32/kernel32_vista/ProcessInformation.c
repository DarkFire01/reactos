/*
 * PROJECT:     ReactOS Kernel32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Setting and querying process information by class
 * COPYRIGHT:   Copyright 2026 ReactOS Contributors
 */

#include "k32_vista.h"

#include <ndk/rtlfuncs.h>
#include <ndk/psfuncs.h>

#define NDEBUG
#include <debug.h>

/*
 * @implemented
 */
BOOL
WINAPI
DECLSPEC_HOTPATCH
SetProcessInformation(
    _In_ HANDLE hProcess,
    _In_ PROCESS_INFORMATION_CLASS ProcessInformationClass,
    _In_reads_bytes_(ProcessInformationSize) LPVOID ProcessInformation,
    _In_ DWORD ProcessInformationSize)
{
    NTSTATUS Status;

    if (ProcessInformation == NULL)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    switch (ProcessInformationClass)
    {
        case ProcessMemoryPriority:
        {
            if (ProcessInformationSize != sizeof(MEMORY_PRIORITY_INFORMATION))
            {
                SetLastError(ERROR_BAD_LENGTH);
                return FALSE;
            }

            Status = NtSetInformationProcess(hProcess,
                                             ProcessPagePriority,
                                             ProcessInformation,
                                             ProcessInformationSize);
            break;
        }

        case ProcessPowerThrottling:
        {
            /* A hint about how eagerly the scheduler may throttle this
               process. There is nothing here to throttle it with, and
               Windows accepts this on hardware that cannot do it either, so
               take it and let it have no effect rather than failing a caller
               that only ever asks for it as an optimisation. */
            PPROCESS_POWER_THROTTLING_STATE State = ProcessInformation;

            if (ProcessInformationSize != sizeof(PROCESS_POWER_THROTTLING_STATE))
            {
                SetLastError(ERROR_BAD_LENGTH);
                return FALSE;
            }

            if (State->Version != PROCESS_POWER_THROTTLING_CURRENT_VERSION)
            {
                SetLastError(ERROR_INVALID_PARAMETER);
                return FALSE;
            }

            Status = STATUS_SUCCESS;
            break;
        }

        default:
        {
            DPRINT1("Unsupported class %d\n", ProcessInformationClass);
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
    }

    if (!NT_SUCCESS(Status))
    {
        SetLastError(RtlNtStatusToDosError(Status));
        return FALSE;
    }

    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
DECLSPEC_HOTPATCH
GetProcessInformation(
    _In_ HANDLE hProcess,
    _In_ PROCESS_INFORMATION_CLASS ProcessInformationClass,
    _Out_writes_bytes_(ProcessInformationSize) LPVOID ProcessInformation,
    _In_ DWORD ProcessInformationSize)
{
    NTSTATUS Status;

    if (ProcessInformation == NULL)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    switch (ProcessInformationClass)
    {
        case ProcessMemoryPriority:
        {
            if (ProcessInformationSize != sizeof(MEMORY_PRIORITY_INFORMATION))
            {
                SetLastError(ERROR_BAD_LENGTH);
                return FALSE;
            }

            Status = NtQueryInformationProcess(hProcess,
                                               ProcessPagePriority,
                                               ProcessInformation,
                                               ProcessInformationSize,
                                               NULL);
            break;
        }

        default:
        {
            /* Windows does not answer the throttling state either */
            DPRINT1("Unsupported class %d\n", ProcessInformationClass);
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
    }

    if (!NT_SUCCESS(Status))
    {
        SetLastError(RtlNtStatusToDosError(Status));
        return FALSE;
    }

    return TRUE;
}
