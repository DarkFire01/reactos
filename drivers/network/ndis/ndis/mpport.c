/*
 * PROJECT:     ReactOS NDIS library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NDIS ports and miniport initiated PnP events
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "ndissys.h"

typedef enum _CORE_PORT_STATE
{
    CorePortAllocated,
    CorePortActivated
} CORE_PORT_STATE;

typedef struct _CORE_PORT
{
    LIST_ENTRY Link;
    CORE_PORT_STATE State;
    NDIS_PORT_CHARACTERISTICS Characteristics;
} CORE_PORT, *PCORE_PORT;

/* Ports */

/* The caller holds the core lock */
static
PCORE_PORT
CoreFindPort(
    _In_ PMINIPORT_CORE Core,
    _In_ NDIS_PORT_NUMBER PortNumber)
{
    PLIST_ENTRY Entry;
    PCORE_PORT Port;

    for (Entry = Core->PortList.Flink; Entry != &Core->PortList; Entry = Entry->Flink)
    {
        Port = CONTAINING_RECORD(Entry, CORE_PORT, Link);
        if (Port->Characteristics.PortNumber == PortNumber)
            return Port;
    }

    return NULL;
}

/*
 * Takes the lowest free port number from the adapter's bitmap, growing it a
 * byte at a time. Number 0 is the default port and is never handed out.
 * The caller holds the core lock.
 */
static
NDIS_STATUS
CoreClaimPortNumber(
    _In_ PMINIPORT_CORE Core,
    _Out_ PNDIS_PORT_NUMBER PortNumber)
{
    PUCHAR Indices;
    ULONG Index;
    ULONG Bit;

    for (Index = 0; Index < Core->PortIndicesLength; Index++)
    {
        if (Core->PortIndices[Index] == 0xFF)
            continue;

        for (Bit = 0; Core->PortIndices[Index] & (1 << Bit); Bit++)
            ;

        Core->PortIndices[Index] |= (UCHAR)(1 << Bit);
        *PortNumber = Index * 8 + Bit;
        return NDIS_STATUS_SUCCESS;
    }

    Indices = ExAllocatePoolWithTag(NonPagedPool, Core->PortIndicesLength + 1, NDIS_TAG);
    if (Indices == NULL)
        return NDIS_STATUS_RESOURCES;

    if (Core->PortIndices != NULL)
    {
        RtlCopyMemory(Indices, Core->PortIndices, Core->PortIndicesLength);
        Indices[Core->PortIndicesLength] = 1;
        *PortNumber = Core->PortIndicesLength * 8;
        ExFreePoolWithTag(Core->PortIndices, NDIS_TAG);
    }
    else
    {
        /* The default port and the one being allocated */
        Indices[0] = 0x3;
        *PortNumber = 1;
    }

    Core->PortIndices = Indices;
    Core->PortIndicesLength++;
    return NDIS_STATUS_SUCCESS;
}

/**
 * @brief
 * Creates a port on an adapter. It stays inactive until the miniport
 * activates it with NdisMNetPnPEvent.
 *
 * @param[in] NdisMiniportHandle
 * The adapter.
 *
 * @param[in,out] PortCharacteristics
 * What the port is. PortNumber is filled in.
 *
 * @return
 * NDIS_STATUS_SUCCESS or NDIS_STATUS_RESOURCES.
 */
_Use_decl_annotations_
NDIS_STATUS
NTAPI
NdisMAllocatePort(
    NDIS_HANDLE NdisMiniportHandle,
    PNDIS_PORT_CHARACTERISTICS PortCharacteristics)
{
    PLOGICAL_ADAPTER Adapter = (PLOGICAL_ADAPTER)NdisMiniportHandle;
    PMINIPORT_CORE Core = &Adapter->Core;
    NDIS_PORT_NUMBER PortNumber;
    NDIS_STATUS Status;
    PCORE_PORT Port;
    KIRQL OldIrql;

    Port = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Port), NDIS_TAG);
    if (Port == NULL)
        return NDIS_STATUS_RESOURCES;

    RtlZeroMemory(Port, sizeof(*Port));

    KeAcquireSpinLock(&Core->Lock, &OldIrql);

    Status = CoreClaimPortNumber(Core, &PortNumber);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        KeReleaseSpinLock(&Core->Lock, OldIrql);
        ExFreePoolWithTag(Port, NDIS_TAG);
        return Status;
    }

    PortCharacteristics->PortNumber = PortNumber;
    RtlCopyMemory(&Port->Characteristics, PortCharacteristics, sizeof(Port->Characteristics));

    /* No 802.1X authentication runs here, so the default is an open port */
    if (PortCharacteristics->Flags & NDIS_PORT_CHAR_USE_DEFAULT_AUTH_SETTINGS)
    {
        Port->Characteristics.SendControlState = NdisPortControlStateUncontrolled;
        Port->Characteristics.RcvControlState = NdisPortControlStateUncontrolled;
        Port->Characteristics.SendAuthorizationState = NdisPortAuthorizationUnknown;
        Port->Characteristics.RcvAuthorizationState = NdisPortAuthorizationUnknown;
    }

    Port->State = CorePortAllocated;
    InsertTailList(&Core->PortList, &Port->Link);
    Core->PortCount++;

    KeReleaseSpinLock(&Core->Lock, OldIrql);
    return NDIS_STATUS_SUCCESS;
}

/**
 * @brief
 * Frees an inactive port.
 *
 * @param[in] NdisMiniportHandle
 * The adapter.
 *
 * @param[in] PortNumber
 * The port.
 *
 * @return
 * NDIS_STATUS_SUCCESS, NDIS_STATUS_INVALID_PORT for a port that does not
 * exist, or NDIS_STATUS_INVALID_PORT_STATE for one still active.
 */
_Use_decl_annotations_
NDIS_STATUS
NTAPI
NdisMFreePort(
    NDIS_HANDLE NdisMiniportHandle,
    NDIS_PORT_NUMBER PortNumber)
{
    PLOGICAL_ADAPTER Adapter = (PLOGICAL_ADAPTER)NdisMiniportHandle;
    PMINIPORT_CORE Core = &Adapter->Core;
    ULONG Index = PortNumber / 8;
    UCHAR Mask = (UCHAR)(1 << (PortNumber % 8));
    NDIS_STATUS Status = NDIS_STATUS_SUCCESS;
    PCORE_PORT Port;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Core->Lock, &OldIrql);

    Port = CoreFindPort(Core, PortNumber);
    if (Port == NULL)
    {
        Status = NDIS_STATUS_INVALID_PORT;
    }
    else if (Port->State != CorePortAllocated)
    {
        Status = NDIS_STATUS_INVALID_PORT_STATE;
    }
    else if (Index >= Core->PortIndicesLength || !(Core->PortIndices[Index] & Mask))
    {
        Status = NDIS_STATUS_INVALID_PARAMETER;
    }
    else
    {
        Core->PortIndices[Index] &= ~Mask;
        RemoveEntryList(&Port->Link);
        Core->PortCount--;
    }

    KeReleaseSpinLock(&Core->Lock, OldIrql);

    if (Status == NDIS_STATUS_SUCCESS)
        ExFreePoolWithTag(Port, NDIS_TAG);

    return Status;
}

/**
 * @brief
 * Releases what is left of the port bookkeeping once the miniport halted.
 *
 * @param[in] Adapter
 * The adapter.
 */
VOID
NTAPI
CoreFreePorts(
    _In_ PLOGICAL_ADAPTER Adapter)
{
    PMINIPORT_CORE Core = &Adapter->Core;
    PCORE_PORT Port;

    /* A miniport that halted without freeing its ports leaves them here */
    while (!IsListEmpty(&Core->PortList))
    {
        Port = CONTAINING_RECORD(RemoveHeadList(&Core->PortList), CORE_PORT, Link);
        ExFreePoolWithTag(Port, NDIS_TAG);
    }

    if (Core->PortIndices != NULL)
        ExFreePoolWithTag(Core->PortIndices, NDIS_TAG);

    Core->PortIndices = NULL;
    Core->PortIndicesLength = 0;
    Core->PortCount = 0;
}

/* Every port in the chain must exist and be inactive before any is activated */
static
NDIS_STATUS
CoreActivatePorts(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_opt_ PNDIS_PORT Ports)
{
    PMINIPORT_CORE Core = &Adapter->Core;
    NDIS_STATUS Status = NDIS_STATUS_SUCCESS;
    PCORE_PORT Port;
    PNDIS_PORT Current;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Core->Lock, &OldIrql);

    for (Current = Ports; Current != NULL && Current->PortCharacteristics.PortNumber != 0; Current = Current->Next)
    {
        Port = CoreFindPort(Core, Current->PortCharacteristics.PortNumber);
        if (Port == NULL)
        {
            Status = NDIS_STATUS_INVALID_PORT;
            break;
        }

        if (Port->State != CorePortAllocated)
        {
            Status = NDIS_STATUS_INVALID_PORT_STATE;
            break;
        }
    }

    if (Status == NDIS_STATUS_SUCCESS)
    {
        for (Current = Ports; Current != NULL && Current->PortCharacteristics.PortNumber != 0; Current = Current->Next)
            CoreFindPort(Core, Current->PortCharacteristics.PortNumber)->State = CorePortActivated;
    }

    KeReleaseSpinLock(&Core->Lock, OldIrql);
    return Status;
}

/* Every port in the zero terminated array must exist and be active before any is deactivated */
static
NDIS_STATUS
CoreDeactivatePorts(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_reads_opt_(Count) PNDIS_PORT_NUMBER PortNumbers,
    _In_ ULONG Count)
{
    PMINIPORT_CORE Core = &Adapter->Core;
    NDIS_STATUS Status = NDIS_STATUS_SUCCESS;
    PCORE_PORT Port;
    KIRQL OldIrql;
    ULONG i;

    if (PortNumbers == NULL || Count == 0)
        return NDIS_STATUS_SUCCESS;

    KeAcquireSpinLock(&Core->Lock, &OldIrql);

    for (i = 0; i < Count && PortNumbers[i] != 0; i++)
    {
        Port = CoreFindPort(Core, PortNumbers[i]);
        if (Port == NULL)
        {
            Status = NDIS_STATUS_INVALID_PORT;
            break;
        }

        if (Port->State != CorePortActivated)
        {
            Status = NDIS_STATUS_INVALID_PORT_STATE;
            break;
        }
    }

    if (Status == NDIS_STATUS_SUCCESS)
    {
        for (i = 0; i < Count && PortNumbers[i] != 0; i++)
            CoreFindPort(Core, PortNumbers[i])->State = CorePortAllocated;
    }

    KeReleaseSpinLock(&Core->Lock, OldIrql);
    return Status;
}

/* PnP events */

/*
 * Hands a PnP event to every protocol bound to the adapter, waiting out the
 * ones that pend. The first failure ends it.
 */
NDIS_STATUS
NTAPI
CoreNotifyProtocols(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PNET_PNP_EVENT Template)
{
    PADAPTER_BINDING Binding;
    NET_PNP_EVENT PnPEvent;
    PLIST_ENTRY Entry;
    NDIS_STATUS Status = NDIS_STATUS_SUCCESS;
    KEVENT Completed;

    for (Entry = Adapter->ProtocolListHead.Flink; Entry != &Adapter->ProtocolListHead; Entry = Entry->Flink)
    {
        Binding = CONTAINING_RECORD(Entry, ADAPTER_BINDING, AdapterListEntry);
        if (Binding->ProtocolBinding->Chars.PnPEventHandler == NULL)
            continue;

        /* No IRP rides with this one: NdisCompletePnPEvent signals the event instead */
        RtlCopyMemory(&PnPEvent, Template, sizeof(PnPEvent));
        RtlZeroMemory(PnPEvent.NdisReserved, sizeof(PnPEvent.NdisReserved));
        KeInitializeEvent(&Completed, NotificationEvent, FALSE);
        PnPEvent.NdisReserved[2] = (ULONG_PTR)&Completed;

        Status = Binding->ProtocolBinding->Chars.PnPEventHandler(Binding->NdisOpenBlock.ProtocolBindingContext,
                                                                 &PnPEvent);
        if (Status == NDIS_STATUS_PENDING)
        {
            KeWaitForSingleObject(&Completed, Executive, KernelMode, FALSE, NULL);
            Status = (NDIS_STATUS)PnPEvent.NdisReserved[3];
        }

        if (Status != NDIS_STATUS_SUCCESS)
            break;
    }

    return Status;
}

/**
 * @brief
 * A miniport reports a PnP event of its own: port activation and
 * deactivation, or an event every protocol above has to hear about.
 *
 * @param[in] MiniportAdapterHandle
 * The adapter.
 *
 * @param[in] NetPnPEventNotification
 * The event.
 *
 * @return
 * The outcome. Events nothing here acts on succeed.
 */
_Use_decl_annotations_
NDIS_STATUS
NTAPI
NdisMNetPnPEvent(
    NDIS_HANDLE MiniportAdapterHandle,
    PNET_PNP_EVENT_NOTIFICATION NetPnPEventNotification)
{
    PLOGICAL_ADAPTER Adapter = (PLOGICAL_ADAPTER)MiniportAdapterHandle;
    PNET_PNP_EVENT PnPEvent = &NetPnPEventNotification->NetPnPEvent;

    switch (PnPEvent->NetEvent)
    {
        case NetEventQueryPower:
        case NetEventQueryRemoveDevice:
        case NetEventCancelRemoveDevice:
        case NetEventPnPCapabilities:
        case NetEventNDKEnable:
        case NetEventNDKDisable:
        case NetEventSwitchActivate:
            return CoreNotifyProtocols(Adapter, PnPEvent);

        case NetEventPortActivation:
            return CoreActivatePorts(Adapter, PnPEvent->Buffer);

        case NetEventPortDeactivation:
            return CoreDeactivatePorts(Adapter,
                                       PnPEvent->Buffer,
                                       PnPEvent->BufferLength / sizeof(NDIS_PORT_NUMBER));

        default:
            return NDIS_STATUS_SUCCESS;
    }
}

