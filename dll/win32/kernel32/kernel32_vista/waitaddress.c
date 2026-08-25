/*
 * PROJECT:     ReactOS Kernel32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Waiting on and waking an address
 * COPYRIGHT:   Copyright 2026 ReactOS Contributors
 */

/*
 * The Win32 face of RtlWaitOnAddress and friends, which ntdll already
 * implements. Only the shape differs: this takes a timeout in milliseconds
 * rather than as an NT relative time, and answers with a BOOL and a last
 * error rather than an NTSTATUS.
 *
 * These matter more than their obscurity suggests. They are how a lock built
 * on a single word blocks without a kernel object per lock, so a caller that
 * has them uses them everywhere - Chromium's own lock and its allocator both
 * do - and a caller that resolves them by name and does not find them can
 * lose the process outright, because a failed delay-load is fatal there.
 */

#include "k32_vista.h"

#include <ndk/rtlfuncs.h>

#define NDEBUG
#include <debug.h>

NTSYSAPI
NTSTATUS
NTAPI
RtlWaitOnAddress(
    _In_ PVOID Address,
    _In_ PVOID CompareAddress,
    _In_ SIZE_T AddressSize,
    _In_opt_ PLARGE_INTEGER Timeout);

NTSYSAPI VOID NTAPI RtlWakeAddressAll(_In_ PVOID Address);
NTSYSAPI VOID NTAPI RtlWakeAddressSingle(_In_ PVOID Address);

/*
 * @implemented
 */
BOOL
WINAPI
DECLSPEC_HOTPATCH
WaitOnAddress(
    _In_ volatile VOID *Address,
    _In_ PVOID CompareAddress,
    _In_ SIZE_T AddressSize,
    _In_ DWORD dwMilliseconds)
{
    LARGE_INTEGER Timeout;
    PLARGE_INTEGER TimeoutPtr = NULL;
    NTSTATUS Status;

    if (dwMilliseconds != INFINITE)
    {
        /* A relative timeout, which NT counts negative in 100ns units */
        Timeout.QuadPart = (LONGLONG)dwMilliseconds * -10000LL;
        TimeoutPtr = &Timeout;
    }

    Status = RtlWaitOnAddress((PVOID)Address,
                              CompareAddress,
                              AddressSize,
                              TimeoutPtr);

    /* A wait that ran out of time is reported as a failure with
       ERROR_TIMEOUT, not as an error in its own right, and a caller loops on
       exactly that. STATUS_TIMEOUT is a success code, so it has to be picked
       out before NT_SUCCESS() decides. */
    if (Status == STATUS_TIMEOUT)
    {
        SetLastError(ERROR_TIMEOUT);
        return FALSE;
    }

    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    return TRUE;
}

/*
 * @implemented
 */
VOID
WINAPI
DECLSPEC_HOTPATCH
WakeByAddressAll(
    _In_ PVOID Address)
{
    RtlWakeAddressAll(Address);
}

/*
 * @implemented
 */
VOID
WINAPI
DECLSPEC_HOTPATCH
WakeByAddressSingle(
    _In_ PVOID Address)
{
    RtlWakeAddressSingle(Address);
}
