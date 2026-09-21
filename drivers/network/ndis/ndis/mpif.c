/*
 * PROJECT:     ReactOS NDIS library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The network interface identity of adapters
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "ndissys.h"
#include <ipifcons.h>

/* Every registered interface, in ascending NET_LUID order */
static LIST_ENTRY CoreInterfaceList = { &CoreInterfaceList, &CoreInterfaceList };
static KSPIN_LOCK CoreInterfaceLock;
static LONG CoreLastIfIndex;
static ULONG CoreLastNetLuidIndex;

ULONG
NTAPI
CoreReadKeyUlong(
    _In_ HANDLE Key,
    _In_ PCWSTR Name,
    _In_ ULONG Default)
{
    UCHAR Buffer[FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION Value = (PKEY_VALUE_PARTIAL_INFORMATION)Buffer;
    UNICODE_STRING ValueName;
    ULONG Length;
    NTSTATUS Status;

    RtlInitUnicodeString(&ValueName, Name);
    Status = ZwQueryValueKey(Key, &ValueName, KeyValuePartialInformation, Value, sizeof(Buffer), &Length);
    if (!NT_SUCCESS(Status) || Value->Type != REG_DWORD || Value->DataLength != sizeof(ULONG))
        return Default;

    return *(PULONG)Value->Data;
}

static
VOID
CoreReadInterfaceGuid(
    _In_ HANDLE Key,
    _Out_ LPGUID InterfaceGuid)
{
    UCHAR Buffer[FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) + 40 * sizeof(WCHAR)];
    PKEY_VALUE_PARTIAL_INFORMATION Value = (PKEY_VALUE_PARTIAL_INFORMATION)Buffer;
    UNICODE_STRING ValueName;
    UNICODE_STRING GuidString;
    ULONG Length;
    NTSTATUS Status;

    RtlZeroMemory(InterfaceGuid, sizeof(*InterfaceGuid));

    RtlInitUnicodeString(&ValueName, L"NetCfgInstanceId");
    Status = ZwQueryValueKey(Key, &ValueName, KeyValuePartialInformation, Value, sizeof(Buffer), &Length);
    if (!NT_SUCCESS(Status) || Value->Type != REG_SZ || Value->DataLength < sizeof(WCHAR))
        return;

    /* The stored length counts the terminator */
    GuidString.Buffer = (PWCH)Value->Data;
    GuidString.Length = (USHORT)(Value->DataLength - sizeof(WCHAR));
    GuidString.MaximumLength = (USHORT)Value->DataLength;
    if (GuidString.Buffer[GuidString.Length / sizeof(WCHAR)] != UNICODE_NULL)
        GuidString.Length = (USHORT)Value->DataLength;

    if (!NT_SUCCESS(RtlGUIDFromString(&GuidString, InterfaceGuid)))
        RtlZeroMemory(InterfaceGuid, sizeof(*InterfaceGuid));
}

/* Caller holds CoreInterfaceLock */
static
BOOLEAN
CoreNetLuidInUse(
    _In_ ULONG64 Value)
{
    PLIST_ENTRY Entry;

    for (Entry = CoreInterfaceList.Flink; Entry != &CoreInterfaceList; Entry = Entry->Flink)
    {
        if (CONTAINING_RECORD(Entry, CORE_INTERFACE, ListEntry)->NetLuid.Value == Value)
            return TRUE;
    }

    return FALSE;
}

/**
 * @brief
 * Gives an adapter its interface GUID, NET_LUID and interface index.
 *
 * The interface type, a LUID index already assigned to the device and the
 * interface GUID come from the device's driver key when it has them.
 *
 * @param[in] Adapter
 * The adapter, with its physical device object set.
 *
 * @return
 * STATUS_SUCCESS, or why the driver key could not be read.
 */
NTSTATUS
NTAPI
CoreRegisterInterface(
    _In_ PLOGICAL_ADAPTER Adapter)
{
    PCORE_INTERFACE Interface = &Adapter->Interface;
    PLIST_ENTRY Entry;
    HANDLE Key;
    ULONG IfType;
    ULONG LuidIndex;
    NET_LUID Luid;
    NTSTATUS Status;
    KIRQL OldIrql;

    ASSERT(!Interface->Registered);

    Status = IoOpenDeviceRegistryKey(Adapter->NdisMiniportBlock.PhysicalDeviceObject,
                                     PLUGPLAY_REGKEY_DRIVER,
                                     KEY_READ,
                                     &Key);
    if (!NT_SUCCESS(Status))
        return Status;

    IfType = CoreReadKeyUlong(Key, L"*IfType", IF_TYPE_ETHERNET_CSMACD);
    LuidIndex = CoreReadKeyUlong(Key, L"NetLuidIndex", 0);
    CoreReadInterfaceGuid(Key, &Interface->InterfaceGuid);
    ZwClose(Key);

    Luid.Value = 0;
    Luid.Info.IfType = IfType;

    KeAcquireSpinLock(&CoreInterfaceLock, &OldIrql);

    /* A stored index is kept unless another interface of the type took it already */
    Luid.Info.NetLuidIndex = LuidIndex;
    while (Luid.Info.NetLuidIndex == 0 || CoreNetLuidInUse(Luid.Value))
    {
        CoreLastNetLuidIndex = (CoreLastNetLuidIndex + 1) & 0xFFFFFF;
        Luid.Info.NetLuidIndex = CoreLastNetLuidIndex;
    }

    Interface->NetLuid = Luid;
    Interface->IfIndex = (NET_IFINDEX)InterlockedIncrement(&CoreLastIfIndex);

    for (Entry = CoreInterfaceList.Flink; Entry != &CoreInterfaceList; Entry = Entry->Flink)
    {
        if (CONTAINING_RECORD(Entry, CORE_INTERFACE, ListEntry)->NetLuid.Value > Luid.Value)
            break;
    }

    /* Ahead of the first larger LUID, or last */
    InsertTailList(Entry, &Interface->ListEntry);
    Interface->Registered = TRUE;

    KeReleaseSpinLock(&CoreInterfaceLock, OldIrql);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Retires the interface an adapter was, so its NET_LUID stops resolving.
 *
 * @param[in] Adapter
 * The adapter going away.
 */
VOID
NTAPI
CoreDeregisterInterface(
    _In_ PLOGICAL_ADAPTER Adapter)
{
    PCORE_INTERFACE Interface = &Adapter->Interface;
    KIRQL OldIrql;

    KeAcquireSpinLock(&CoreInterfaceLock, &OldIrql);
    if (Interface->Registered)
    {
        RemoveEntryList(&Interface->ListEntry);
        Interface->Registered = FALSE;
    }
    KeReleaseSpinLock(&CoreInterfaceLock, OldIrql);
}

/**
 * @brief
 * Looks up the interface index of the interface with a NET_LUID.
 *
 * @param[in] NetLuid
 * The interface.
 *
 * @param[out] pIfIndex
 * Its index, or zero when there is no such interface.
 *
 * @return
 * NDIS_STATUS_SUCCESS, or NDIS_STATUS_INTERFACE_NOT_FOUND.
 */
_Use_decl_annotations_
NDIS_STATUS
NTAPI
NdisIfGetInterfaceIndexFromNetLuid(
    NET_LUID NetLuid,
    PNET_IFINDEX pIfIndex)
{
    NDIS_STATUS Status = NDIS_STATUS_INTERFACE_NOT_FOUND;
    PCORE_INTERFACE Interface;
    PLIST_ENTRY Entry;
    KIRQL OldIrql;

    *pIfIndex = 0;

    KeAcquireSpinLock(&CoreInterfaceLock, &OldIrql);
    for (Entry = CoreInterfaceList.Flink; Entry != &CoreInterfaceList; Entry = Entry->Flink)
    {
        Interface = CONTAINING_RECORD(Entry, CORE_INTERFACE, ListEntry);

        /* Sorted, so a larger LUID ends the search */
        if (Interface->NetLuid.Value > NetLuid.Value)
            break;

        if (Interface->NetLuid.Value == NetLuid.Value)
        {
            *pIfIndex = Interface->IfIndex;
            Status = NDIS_STATUS_SUCCESS;
            break;
        }
    }
    KeReleaseSpinLock(&CoreInterfaceLock, OldIrql);

    return Status;
}
