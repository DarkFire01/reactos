/*
 * PROJECT:     ReactOS Kernel32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Setting and querying thread information by class
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
SetThreadInformation(
    _In_ HANDLE hThread,
    _In_ THREAD_INFORMATION_CLASS ThreadInformationClass,
    _In_reads_bytes_(ThreadInformationSize) LPVOID ThreadInformation,
    _In_ DWORD ThreadInformationSize)
{
    NTSTATUS Status;

    if (ThreadInformation == NULL && ThreadInformationSize != 0)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    switch (ThreadInformationClass)
    {
        case ThreadMemoryPriority:
        {
            if (ThreadInformationSize != sizeof(ULONG))
            {
                SetLastError(ERROR_BAD_LENGTH);
                return FALSE;
            }

            Status = NtSetInformationThread(hThread,
                                            ThreadPagePriority,
                                            ThreadInformation,
                                            ThreadInformationSize);
            break;
        }

        case ThreadAbsoluteCpuPriority:
        {
            if (ThreadInformationSize != sizeof(ULONG))
            {
                SetLastError(ERROR_BAD_LENGTH);
                return FALSE;
            }

            Status = NtSetInformationThread(hThread,
                                            ThreadActualBasePriority,
                                            ThreadInformation,
                                            ThreadInformationSize);
            break;
        }

        case ThreadPowerThrottling:
        {
            /* A hint about how eagerly the scheduler may throttle this thread.
               There is nothing here to throttle it with, and Windows accepts
               this on hardware that cannot do it either, so take it and let
               it have no effect rather than failing a caller that only ever
               asks for it as an optimisation. */
            if (ThreadInformationSize != sizeof(THREAD_POWER_THROTTLING_STATE))
            {
                SetLastError(ERROR_BAD_LENGTH);
                return FALSE;
            }

            Status = STATUS_SUCCESS;
            break;
        }

        default:
        {
            DPRINT1("Unsupported class %d\n", ThreadInformationClass);
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
GetThreadInformation(
    _In_ HANDLE hThread,
    _In_ THREAD_INFORMATION_CLASS ThreadInformationClass,
    _Out_writes_bytes_(ThreadInformationSize) LPVOID ThreadInformation,
    _In_ DWORD ThreadInformationSize)
{
    NTSTATUS Status;

    if (ThreadInformation == NULL)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    switch (ThreadInformationClass)
    {
        case ThreadMemoryPriority:
        {
            if (ThreadInformationSize != sizeof(ULONG))
            {
                SetLastError(ERROR_BAD_LENGTH);
                return FALSE;
            }

            Status = NtQueryInformationThread(hThread,
                                              ThreadPagePriority,
                                              ThreadInformation,
                                              ThreadInformationSize,
                                              NULL);
            break;
        }

        case ThreadAbsoluteCpuPriority:
        {
            if (ThreadInformationSize != sizeof(ULONG))
            {
                SetLastError(ERROR_BAD_LENGTH);
                return FALSE;
            }

            Status = NtQueryInformationThread(hThread,
                                              ThreadActualBasePriority,
                                              ThreadInformation,
                                              ThreadInformationSize,
                                              NULL);
            break;
        }

        default:
        {
            /* Windows does not answer the throttling state either */
            DPRINT1("Unsupported class %d\n", ThreadInformationClass);
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
