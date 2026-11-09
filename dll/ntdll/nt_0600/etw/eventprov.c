/*
 * PROJECT:     ReactOS NT Layer/System API
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Manifest based event provider registration and writing
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* The per thread activity id lives in the Vista TEB layout */
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x600
#undef NTDDI_VERSION
#define NTDDI_VERSION 0x06000000

#include "../ntdll_vista.h"
#include <ndk/kefuncs.h>
#include <evntprov.h>

/* There are no trace sessions, so providers register and write but are never enabled */

#define ETWP_MAX_REGISTRATIONS      2048

typedef struct _ETWP_REGISTRATION
{
    LIST_ENTRY Link;
    GUID ProviderId;
    PENABLECALLBACK EnableCallback;
    PVOID CallbackContext;
} ETWP_REGISTRATION, *PETWP_REGISTRATION;

static RTL_SRWLOCK EtwpRegistrationLock = RTL_SRWLOCK_INIT;
static LIST_ENTRY EtwpRegistrations = { &EtwpRegistrations, &EtwpRegistrations };
static ULONG EtwpRegistrationCount;
static ULONG EtwpActivitySeed;

static
BOOLEAN
EtwpIsRegistered(
    _In_ REGHANDLE RegHandle)
{
    PLIST_ENTRY Entry;
    BOOLEAN Found = FALSE;

    if (RegHandle == 0)
        return FALSE;

    RtlAcquireSRWLockShared(&EtwpRegistrationLock);
    for (Entry = EtwpRegistrations.Flink; Entry != &EtwpRegistrations; Entry = Entry->Flink)
    {
        if ((REGHANDLE)(ULONG_PTR)Entry == RegHandle)
        {
            Found = TRUE;
            break;
        }
    }
    RtlReleaseSRWLockShared(&EtwpRegistrationLock);

    return Found;
}

ULONG
NTAPI
EtwEventRegister(
    _In_ LPCGUID ProviderId,
    _In_opt_ PENABLECALLBACK EnableCallback,
    _In_opt_ PVOID CallbackContext,
    _Out_ PREGHANDLE RegHandle)
{
    PETWP_REGISTRATION Registration;

    if (!ProviderId || !RegHandle)
        return ERROR_INVALID_PARAMETER;

    *RegHandle = 0;

    Registration = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Registration));
    if (!Registration)
        return ERROR_NOT_ENOUGH_MEMORY;

    Registration->ProviderId = *ProviderId;
    Registration->EnableCallback = EnableCallback;
    Registration->CallbackContext = CallbackContext;

    RtlAcquireSRWLockExclusive(&EtwpRegistrationLock);
    if (EtwpRegistrationCount >= ETWP_MAX_REGISTRATIONS)
    {
        RtlReleaseSRWLockExclusive(&EtwpRegistrationLock);
        RtlFreeHeap(RtlGetProcessHeap(), 0, Registration);
        return ERROR_NO_SYSTEM_RESOURCES;
    }

    InsertTailList(&EtwpRegistrations, &Registration->Link);
    EtwpRegistrationCount++;
    RtlReleaseSRWLockExclusive(&EtwpRegistrationLock);

    *RegHandle = (REGHANDLE)(ULONG_PTR)&Registration->Link;
    return ERROR_SUCCESS;
}

ULONG
NTAPI
EtwEventUnregister(
    _In_ REGHANDLE RegHandle)
{
    PLIST_ENTRY Entry;

    if (RegHandle == 0)
        return ERROR_INVALID_PARAMETER;

    RtlAcquireSRWLockExclusive(&EtwpRegistrationLock);
    for (Entry = EtwpRegistrations.Flink; Entry != &EtwpRegistrations; Entry = Entry->Flink)
    {
        if ((REGHANDLE)(ULONG_PTR)Entry != RegHandle)
            continue;

        RemoveEntryList(Entry);
        EtwpRegistrationCount--;
        RtlReleaseSRWLockExclusive(&EtwpRegistrationLock);

        RtlFreeHeap(RtlGetProcessHeap(), 0, CONTAINING_RECORD(Entry, ETWP_REGISTRATION, Link));
        return ERROR_SUCCESS;
    }
    RtlReleaseSRWLockExclusive(&EtwpRegistrationLock);

    return ERROR_INVALID_HANDLE;
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
EtwEventProviderEnabled(
    _In_ REGHANDLE RegHandle,
    _In_ UCHAR Level,
    _In_ ULONGLONG Keyword)
{
    UNREFERENCED_PARAMETER(RegHandle);
    UNREFERENCED_PARAMETER(Level);
    UNREFERENCED_PARAMETER(Keyword);
    return FALSE;
}

ULONG
NTAPI
EtwEventWriteTransfer(
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
        return ERROR_INVALID_PARAMETER;

    if (!EtwpIsRegistered(RegHandle))
        return ERROR_INVALID_HANDLE;

    /* Nobody is listening */
    return ERROR_SUCCESS;
}

ULONG
NTAPI
EtwEventWrite(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor,
    _In_ ULONG UserDataCount,
    _In_reads_opt_(UserDataCount) PEVENT_DATA_DESCRIPTOR UserData)
{
    return EtwEventWriteTransfer(RegHandle, EventDescriptor, NULL, NULL, UserDataCount, UserData);
}

ULONG
NTAPI
EtwEventWriteString(
    _In_ REGHANDLE RegHandle,
    _In_ UCHAR Level,
    _In_ ULONGLONG Keyword,
    _In_ PCWSTR String)
{
    UNREFERENCED_PARAMETER(Level);
    UNREFERENCED_PARAMETER(Keyword);

    if (!String)
        return ERROR_INVALID_PARAMETER;

    if (!EtwpIsRegistered(RegHandle))
        return ERROR_INVALID_HANDLE;

    return ERROR_SUCCESS;
}

ULONG
NTAPI
EtwEventSetInformation(
    _In_ REGHANDLE RegHandle,
    _In_ ULONG InformationClass,
    _In_reads_bytes_opt_(InformationLength) PVOID EventInformation,
    _In_ ULONG InformationLength)
{
    UNREFERENCED_PARAMETER(EventInformation);
    UNREFERENCED_PARAMETER(InformationLength);

    if (!EtwpIsRegistered(RegHandle))
        return ERROR_INVALID_HANDLE;

    /* Binary tracking, provider traits and descriptor type only affect consumers */
    if (InformationClass == 0 || InformationClass == 2 || InformationClass == 3)
        return ERROR_SUCCESS;

    return ERROR_NOT_SUPPORTED;
}

/* Activity ids only need to be unique within the machine for correlation */
static
VOID
EtwpCreateActivityId(
    _Out_ LPGUID ActivityId)
{
    LARGE_INTEGER Time;
    ULONG Seed;

    NtQuerySystemTime(&Time);
    Seed = InterlockedIncrement((PLONG)&EtwpActivitySeed) ^ Time.LowPart ^ HandleToUlong(NtCurrentTeb()->ClientId.UniqueThread);

    ActivityId->Data1 = RtlRandom(&Seed);
    ActivityId->Data2 = (USHORT)RtlRandom(&Seed);
    ActivityId->Data3 = (USHORT)((RtlRandom(&Seed) & 0x0FFF) | 0x4000);
    *(PULONG)&ActivityId->Data4[0] = RtlRandom(&Seed);
    *(PULONG)&ActivityId->Data4[4] = Time.HighPart ^ RtlRandom(&Seed);
    ActivityId->Data4[0] = (ActivityId->Data4[0] & 0x3F) | 0x80;
}

ULONG
NTAPI
EtwEventActivityIdControl(
    _In_ ULONG ControlCode,
    _Inout_ LPGUID ActivityId)
{
    PTEB Teb = NtCurrentTeb();
    GUID Previous;

    if (!ActivityId)
        return ERROR_INVALID_PARAMETER;

    switch (ControlCode)
    {
        case EVENT_ACTIVITY_CTRL_GET_ID:
            *ActivityId = Teb->ActivityId;
            break;

        case EVENT_ACTIVITY_CTRL_SET_ID:
            Teb->ActivityId = *ActivityId;
            break;

        case EVENT_ACTIVITY_CTRL_CREATE_ID:
            EtwpCreateActivityId(ActivityId);
            break;

        case EVENT_ACTIVITY_CTRL_GET_SET_ID:
            Previous = Teb->ActivityId;
            Teb->ActivityId = *ActivityId;
            *ActivityId = Previous;
            break;

        case EVENT_ACTIVITY_CTRL_CREATE_SET_ID:
            *ActivityId = Teb->ActivityId;
            EtwpCreateActivityId(&Teb->ActivityId);
            break;

        default:
            return ERROR_INVALID_PARAMETER;
    }

    return ERROR_SUCCESS;
}

ULONG
NTAPI
EtwEventWriteEx(
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

    return EtwEventWriteTransfer(RegHandle, EventDescriptor, ActivityId, RelatedActivityId, UserDataCount, UserData);
}
