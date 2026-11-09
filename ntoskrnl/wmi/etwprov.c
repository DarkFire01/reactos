/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Kernel mode manifest based event providers
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <ntoskrnl.h>
#include <evntprov.h>

#define NDEBUG
#include <debug.h>

/* There are no trace sessions, providers register and write but are never enabled */

#define TAG_ETW_PROVIDER            'PwtE'
#define ETWP_PROVIDER_SIGNATURE     0x50777445

typedef struct _ETWP_KERNEL_PROVIDER
{
    ULONG Signature;
    GUID ProviderId;
    PETWENABLECALLBACK EnableCallback;
    PVOID CallbackContext;
} ETWP_KERNEL_PROVIDER, *PETWP_KERNEL_PROVIDER;

/* Callable at any IRQL, so this only looks at the nonpaged registration */
static
BOOLEAN
EtwpValidProvider(
    _In_ REGHANDLE RegHandle)
{
    PETWP_KERNEL_PROVIDER Provider = (PETWP_KERNEL_PROVIDER)(ULONG_PTR)RegHandle;

    return Provider && Provider->Signature == ETWP_PROVIDER_SIGNATURE;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
EtwRegister(
    _In_ LPCGUID ProviderId,
    _In_opt_ PETWENABLECALLBACK EnableCallback,
    _In_opt_ PVOID CallbackContext,
    _Out_ PREGHANDLE RegHandle)
{
    PETWP_KERNEL_PROVIDER Provider;

    PAGED_CODE();

    if (!ProviderId || !RegHandle)
        return STATUS_INVALID_PARAMETER;

    *RegHandle = 0;

    Provider = ExAllocatePoolZero(NonPagedPool, sizeof(*Provider), TAG_ETW_PROVIDER);
    if (!Provider)
        return STATUS_INSUFFICIENT_RESOURCES;

    Provider->Signature = ETWP_PROVIDER_SIGNATURE;
    Provider->ProviderId = *ProviderId;
    Provider->EnableCallback = EnableCallback;
    Provider->CallbackContext = CallbackContext;

    *RegHandle = (REGHANDLE)(ULONG_PTR)Provider;
    return STATUS_SUCCESS;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
EtwUnregister(
    _In_ REGHANDLE RegHandle)
{
    PETWP_KERNEL_PROVIDER Provider = (PETWP_KERNEL_PROVIDER)(ULONG_PTR)RegHandle;

    PAGED_CODE();

    if (!EtwpValidProvider(RegHandle))
        return STATUS_INVALID_HANDLE;

    Provider->Signature = 0;
    ExFreePoolWithTag(Provider, TAG_ETW_PROVIDER);
    return STATUS_SUCCESS;
}

BOOLEAN
NTAPI
EtwEventEnabled(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor)
{
    UNREFERENCED_PARAMETER(RegHandle);
    UNREFERENCED_PARAMETER(EventDescriptor);
    return FALSE;
}

BOOLEAN
NTAPI
EtwProviderEnabled(
    _In_ REGHANDLE RegHandle,
    _In_ UCHAR Level,
    _In_ ULONGLONG Keyword)
{
    UNREFERENCED_PARAMETER(RegHandle);
    UNREFERENCED_PARAMETER(Level);
    UNREFERENCED_PARAMETER(Keyword);
    return FALSE;
}

_IRQL_requires_max_(HIGH_LEVEL)
NTSTATUS
NTAPI
EtwWriteTransfer(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor,
    _In_opt_ LPCGUID ActivityId,
    _In_opt_ LPCGUID RelatedActivityId,
    _In_ ULONG UserDataCount,
    _In_reads_opt_(UserDataCount) PEVENT_DATA_DESCRIPTOR UserData)
{
    UNREFERENCED_PARAMETER(ActivityId);
    UNREFERENCED_PARAMETER(RelatedActivityId);
    UNREFERENCED_PARAMETER(UserData);

    if (!EventDescriptor || UserDataCount > MAX_EVENT_DATA_DESCRIPTORS)
        return STATUS_INVALID_PARAMETER;

    if (!EtwpValidProvider(RegHandle))
        return STATUS_INVALID_HANDLE;

    return STATUS_SUCCESS;
}

_IRQL_requires_max_(HIGH_LEVEL)
NTSTATUS
NTAPI
EtwWrite(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor,
    _In_opt_ LPCGUID ActivityId,
    _In_ ULONG UserDataCount,
    _In_reads_opt_(UserDataCount) PEVENT_DATA_DESCRIPTOR UserData)
{
    return EtwWriteTransfer(RegHandle, EventDescriptor, ActivityId, NULL, UserDataCount, UserData);
}

_IRQL_requires_max_(HIGH_LEVEL)
NTSTATUS
NTAPI
EtwWriteEx(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor,
    _In_ ULONG64 Filter,
    _In_ ULONG Flags,
    _In_opt_ LPCGUID ActivityId,
    _In_opt_ LPCGUID RelatedActivityId,
    _In_ ULONG UserDataCount,
    _In_reads_opt_(UserDataCount) PEVENT_DATA_DESCRIPTOR UserData)
{
    UNREFERENCED_PARAMETER(Filter);
    UNREFERENCED_PARAMETER(Flags);

    return EtwWriteTransfer(RegHandle, EventDescriptor, ActivityId, RelatedActivityId, UserDataCount, UserData);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
EtwWriteString(
    _In_ REGHANDLE RegHandle,
    _In_ UCHAR Level,
    _In_ ULONGLONG Keyword,
    _In_opt_ LPCGUID ActivityId,
    _In_ PCWSTR String)
{
    UNREFERENCED_PARAMETER(Level);
    UNREFERENCED_PARAMETER(Keyword);
    UNREFERENCED_PARAMETER(ActivityId);

    if (!String)
        return STATUS_INVALID_PARAMETER;

    return EtwpValidProvider(RegHandle) ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
}

NTSTATUS
NTAPI
EtwSetInformation(
    _In_ REGHANDLE RegHandle,
    _In_ ULONG InformationClass,
    _In_reads_bytes_opt_(InformationLength) PVOID EventInformation,
    _In_ ULONG InformationLength)
{
    UNREFERENCED_PARAMETER(EventInformation);
    UNREFERENCED_PARAMETER(InformationLength);

    if (!EtwpValidProvider(RegHandle))
        return STATUS_INVALID_HANDLE;

    /* Binary tracking, provider traits and descriptor type only matter to consumers */
    if (InformationClass == 0 || InformationClass == 2 || InformationClass == 3)
        return STATUS_SUCCESS;

    return STATUS_NOT_SUPPORTED;
}

/* Kernel callers keep activity ids themselves, only creation is meaningful here */
_IRQL_requires_max_(APC_LEVEL)
NTSTATUS
NTAPI
EtwActivityIdControl(
    _In_ ULONG ControlCode,
    _Inout_updates_bytes_(sizeof(GUID)) LPGUID ActivityId)
{
    static LONG Sequence;
    LARGE_INTEGER Time;
    ULONG Seed;

    if (!ActivityId)
        return STATUS_INVALID_PARAMETER;

    switch (ControlCode)
    {
        case EVENT_ACTIVITY_CTRL_GET_ID:
            RtlZeroMemory(ActivityId, sizeof(*ActivityId));
            return STATUS_SUCCESS;

        case EVENT_ACTIVITY_CTRL_SET_ID:
            return STATUS_SUCCESS;

        case EVENT_ACTIVITY_CTRL_CREATE_ID:
        case EVENT_ACTIVITY_CTRL_CREATE_SET_ID:
            KeQuerySystemTime(&Time);
            Seed = Time.LowPart ^ (ULONG)InterlockedIncrement(&Sequence);
            ActivityId->Data1 = RtlRandomEx(&Seed);
            ActivityId->Data2 = (USHORT)RtlRandomEx(&Seed);
            ActivityId->Data3 = (USHORT)((RtlRandomEx(&Seed) & 0x0FFF) | 0x4000);
            *(PULONG)&ActivityId->Data4[0] = RtlRandomEx(&Seed);
            *(PULONG)&ActivityId->Data4[4] = Time.HighPart ^ RtlRandomEx(&Seed);
            ActivityId->Data4[0] = (ActivityId->Data4[0] & 0x3F) | 0x80;
            return STATUS_SUCCESS;

        case EVENT_ACTIVITY_CTRL_GET_SET_ID:
            RtlZeroMemory(ActivityId, sizeof(*ActivityId));
            return STATUS_SUCCESS;

        default:
            return STATUS_INVALID_PARAMETER;
    }
}
