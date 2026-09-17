/*
 * PROJECT:     ReactOS Kernel - Vista+ APIs
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Device interface property forwarders for drivers
 * COPYRIGHT:   2026 Justin Miller (justin.miller@reactos.org)
 */

/* The kernel implements these itself, so keep them out of the objects it links */

#include "ntoskrnl_vista.h"

typedef
NTSTATUS
(NTAPI *PSET_INTERFACE_PROPERTY_ROUTINE)(
    _In_ PUNICODE_STRING SymbolicLinkName,
    _In_ CONST DEVPROPKEY *PropertyKey,
    _In_ LCID Lcid,
    _In_ ULONG Flags,
    _In_ DEVPROPTYPE Type,
    _In_ ULONG Size,
    _In_reads_bytes_opt_(Size) PVOID Data);

typedef
NTSTATUS
(NTAPI *PGET_INTERFACE_PROPERTY_ROUTINE)(
    _In_ PUNICODE_STRING SymbolicLinkName,
    _In_ CONST DEVPROPKEY *PropertyKey,
    _In_ LCID Lcid,
    _Reserved_ ULONG Flags,
    _In_ ULONG Size,
    _Out_writes_bytes_to_opt_(Size, *RequiredSize) PVOID Data,
    _Out_ PULONG RequiredSize,
    _Out_ PDEVPROPTYPE Type);

/* Forward to the kernel export when present, it only exists on NT 6.2+ */
_IRQL_requires_max_(PASSIVE_LEVEL)
_Must_inspect_result_
NTKRNLVISTAAPI
NTSTATUS
NTAPI
IoSetDeviceInterfacePropertyData(
    _In_ PUNICODE_STRING SymbolicLinkName,
    _In_ CONST DEVPROPKEY *PropertyKey,
    _In_ LCID Lcid,
    _In_ ULONG Flags,
    _In_ DEVPROPTYPE Type,
    _In_ ULONG Size,
    _In_reads_bytes_opt_(Size) PVOID Data)
{
    static PSET_INTERFACE_PROPERTY_ROUTINE SetRoutine;
    UNICODE_STRING RoutineName;

    if (!SetRoutine)
    {
        RtlInitUnicodeString(&RoutineName, L"IoSetDeviceInterfacePropertyData");
        SetRoutine = (PSET_INTERFACE_PROPERTY_ROUTINE)MmGetSystemRoutineAddress(&RoutineName);
        if (!SetRoutine)
            return STATUS_NOT_IMPLEMENTED;
    }

    return SetRoutine(SymbolicLinkName, PropertyKey, Lcid, Flags, Type, Size, Data);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
_Must_inspect_result_
NTKRNLVISTAAPI
NTSTATUS
NTAPI
IoGetDeviceInterfacePropertyData(
    _In_ PUNICODE_STRING SymbolicLinkName,
    _In_ CONST DEVPROPKEY *PropertyKey,
    _In_ LCID Lcid,
    _Reserved_ ULONG Flags,
    _In_ ULONG Size,
    _Out_writes_bytes_to_opt_(Size, *RequiredSize) PVOID Data,
    _Out_ PULONG RequiredSize,
    _Out_ PDEVPROPTYPE Type)
{
    static PGET_INTERFACE_PROPERTY_ROUTINE GetRoutine;
    UNICODE_STRING RoutineName;

    if (!GetRoutine)
    {
        RtlInitUnicodeString(&RoutineName, L"IoGetDeviceInterfacePropertyData");
        GetRoutine = (PGET_INTERFACE_PROPERTY_ROUTINE)MmGetSystemRoutineAddress(&RoutineName);
        if (!GetRoutine)
            return STATUS_NOT_IMPLEMENTED;
    }

    return GetRoutine(SymbolicLinkName, PropertyKey, Lcid, Flags, Size, Data, RequiredSize, Type);
}

/* EOF */
