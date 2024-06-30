/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Power Manager Framework API (PoFx) support routines
 * COPYRIGHT:   Copyright 2023 George Bișoc <george.bisoc@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* PUBLIC FUNCTIONS ***********************************************************/

NTKERNELAPI
NTSTATUS
NTAPI
PoFxRegisterDevice(
    _In_ PDEVICE_OBJECT Pdo,
    _In_ PPO_FX_DEVICE Device,
    _Out_ POHANDLE *Handle)
{
    UNIMPLEMENTED;

    /* Fake success for storport */
    return STATUS_SUCCESS;
}

NTKERNELAPI
VOID
NTAPI
PoFxUnregisterDevice(
    _In_ POHANDLE Handle)
{
    UNIMPLEMENTED;
}

NTKERNELAPI
VOID
NTAPI
PoFxStartDevicePowerManagement(
    _In_ POHANDLE Handle)
{
    UNIMPLEMENTED;
}

NTKERNELAPI
VOID
NTAPI
PoFxActivateComponent(
    _In_ POHANDLE Handle,
    _In_ ULONG Component,
    _In_ ULONG Flags)
{
    UNIMPLEMENTED;
}

NTKERNELAPI
VOID
NTAPI
PoFxCompleteDevicePowerNotRequired(
    _In_ POHANDLE Handle)
{
    UNIMPLEMENTED;
}

NTKERNELAPI
VOID
NTAPI
PoFxIdleComponent(
    _In_ POHANDLE Handle,
    _In_ ULONG Component,
    _In_ ULONG Flags)
{
    UNIMPLEMENTED;
}

NTKERNELAPI
VOID
NTAPI
PoFxCompleteIdleCondition(
    _In_ POHANDLE Handle,
    _In_ ULONG Component)
{
    UNIMPLEMENTED;
}

NTKERNELAPI
VOID
NTAPI
PoFxCompleteIdleState(
    _In_ POHANDLE Handle,
    _In_ ULONG Component)
{
    UNIMPLEMENTED;
}

NTKERNELAPI
VOID
NTAPI
PoFxSetDeviceIdleTimeout(
    _In_ POHANDLE Handle,
    _In_ ULONGLONG IdleTimeout)
{
    UNIMPLEMENTED;
}

NTKERNELAPI
VOID
NTAPI
PoFxReportDevicePoweredOn(
    _In_ POHANDLE Handle)
{
    UNIMPLEMENTED;
}

NTKRNLVISTAAPI
VOID
NTAPI
PoFxSetComponentLatency(
    _In_ POHANDLE  Handle,
    _In_ ULONG     Component,
    _In_ ULONGLONG Latency
)
{
    UNIMPLEMENTED;
}

NTKRNLVISTAAPI
VOID
NTAPI
PoFxSetComponentResidency(
    _In_ POHANDLE  Handle,
    _In_ ULONG     Component,
    _In_ ULONGLONG Residency
)
{
    UNIMPLEMENTED;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
NTKRNLVISTAAPI
NTSTATUS
NTAPI
PoFxPowerControl(
    _In_ POHANDLE Handle,
    _In_ LPCGUID  PowerControlCode,
    _In_opt_ PVOID    InBuffer,
    _In_ SIZE_T   InBufferSize,
    _Out_opt_ PVOID    OutBuffer,
    _In_ SIZE_T   OutBufferSize,
    _Out_opt_ PSIZE_T  BytesReturned
)
{
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}
