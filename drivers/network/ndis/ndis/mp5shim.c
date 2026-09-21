/*
 * PROJECT:     ReactOS NDIS library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NDIS 5 miniports under the NET_BUFFER_LIST core
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "ndissys.h"
#include "xlate.h"

/*
 * The core drives every miniport through NDIS 6 entry points. For an NDIS 5
 * miniport those are the routines in this file, which turn NET_BUFFER_LISTs
 * into packets on the way down and packets and lookahead indications into
 * NET_BUFFER_LISTs on the way up. The context they get is the adapter.
 */

/* Media offered to an NDIS 5 MiniportInitialize */
#define MP5_MEDIA_COUNT 15

static NDIS_MEDIUM Mp5MediaArray[MP5_MEDIA_COUNT] =
{
    NdisMedium802_3,
    NdisMedium802_5,
    NdisMediumFddi,
    NdisMediumWan,
    NdisMediumLocalTalk,
    NdisMediumDix,
    NdisMediumArcnetRaw,
    NdisMediumArcnet878_2,
    NdisMediumAtm,
    NdisMediumWirelessWan,
    NdisMediumIrda,
    NdisMediumBpc,
    NdisMediumCoWan,
    NdisMedium1394,
    NdisMediumMax
};

/* Where a packet indicated up by the miniport is while protocols look at it */
#define MP5_RECEIVE_INDICATING  0
#define MP5_RECEIVE_RETURNED    1
#define MP5_RECEIVE_DEFERRED    2

typedef struct _MP5_DMA_CONTEXT
{
    PLOGICAL_ADAPTER Adapter;
    PNDIS_PACKET Packet;
} MP5_DMA_CONTEXT, *PMP5_DMA_CONTEXT;

static
VOID
Mp5InstallHandlers(
    _In_ PLOGICAL_ADAPTER Adapter);

static
BOOLEAN
Mp5IsDeserialized(
    _In_ PLOGICAL_ADAPTER Adapter)
{
    return (Adapter->NdisMiniportBlock.Flags & NDIS_ATTRIBUTE_DESERIALIZE) != 0;
}

/* Initialize and halt */

static
NDIS_STATUS
Mp5QueryUlong(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ NDIS_OID Oid,
    _Out_ PULONG Value)
{
    ULONG BytesWritten;

    *Value = 0;
    return CoreQueryInformation(Adapter, Oid, Value, sizeof(*Value), &BytesWritten);
}

/*
 * An NDIS 5 miniport never describes itself, so the general attributes the
 * core runs on are gathered by asking it, the way the old start path did.
 */
static
NDIS_STATUS
Mp5DescribeAdapter(
    _In_ PLOGICAL_ADAPTER Adapter)
{
    NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES General;
    ULONG CurrentLookahead;
    ULONG MaxSendPackets;
    ULONG ConnectStatus;
    ULONG LinkSpeed;
    ULONG BytesWritten;
    NDIS_STATUS Status;

    RtlZeroMemory(&General, sizeof(General));
    General.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES;
    General.Header.Revision = NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_1;
    General.Header.Size = NDIS_SIZEOF_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_1;
    General.MediaType = Adapter->NdisMiniportBlock.MediaType;
    General.PhysicalMediumType = NdisPhysicalMediumUnspecified;
    General.MediaDuplexState = MediaDuplexStateUnknown;
    General.SupportedPacketFilters = NDIS_PACKET_TYPE_DIRECTED | NDIS_PACKET_TYPE_MULTICAST |
                                     NDIS_PACKET_TYPE_ALL_MULTICAST | NDIS_PACKET_TYPE_BROADCAST |
                                     NDIS_PACKET_TYPE_PROMISCUOUS;
    General.AccessType = NET_IF_ACCESS_BROADCAST;
    General.DirectionType = NET_IF_DIRECTION_SENDRECEIVE;
    General.ConnectionType = NET_IF_CONNECTION_DEDICATED;
    General.IfType = IF_TYPE_ETHERNET_CSMACD;
    General.IfConnectorPresent = TRUE;

    Status = Mp5QueryUlong(Adapter, OID_GEN_MAC_OPTIONS, &General.MacOptions);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        NDIS_DbgPrint(MIN_TRACE, ("OID_GEN_MAC_OPTIONS failed (0x%x).\n", Status));
        return Status;
    }

    Status = CoreQueryInformation(Adapter,
                                  OID_802_3_CURRENT_ADDRESS,
                                  General.CurrentMacAddress,
                                  ETH_LENGTH_OF_ADDRESS,
                                  &BytesWritten);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        NDIS_DbgPrint(MIN_TRACE, ("OID_802_3_CURRENT_ADDRESS failed (0x%x).\n", Status));
        return Status;
    }
    General.MacAddressLength = ETH_LENGTH_OF_ADDRESS;

    if (CoreQueryInformation(Adapter,
                             OID_802_3_PERMANENT_ADDRESS,
                             General.PermanentMacAddress,
                             ETH_LENGTH_OF_ADDRESS,
                             &BytesWritten) != NDIS_STATUS_SUCCESS)
    {
        RtlCopyMemory(General.PermanentMacAddress, General.CurrentMacAddress, ETH_LENGTH_OF_ADDRESS);
    }

    Status = Mp5QueryUlong(Adapter, OID_GEN_MAXIMUM_LOOKAHEAD, &General.LookaheadSize);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        NDIS_DbgPrint(MIN_TRACE, ("OID_GEN_MAXIMUM_LOOKAHEAD failed (0x%x).\n", Status));
        return Status;
    }

    Status = Mp5QueryUlong(Adapter, OID_GEN_CURRENT_LOOKAHEAD, &CurrentLookahead);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        NDIS_DbgPrint(MIN_TRACE, ("OID_GEN_CURRENT_LOOKAHEAD failed (0x%x).\n", Status));
        return Status;
    }

    /* Some miniports do not answer this one */
    if (Mp5QueryUlong(Adapter, OID_GEN_MAXIMUM_SEND_PACKETS, &MaxSendPackets) != NDIS_STATUS_SUCCESS)
        MaxSendPackets = 1;

    Status = Mp5QueryUlong(Adapter, OID_802_3_MAXIMUM_LIST_SIZE, &General.MaxMulticastListSize);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        NDIS_DbgPrint(MIN_TRACE, ("OID_802_3_MAXIMUM_LIST_SIZE failed (0x%x).\n", Status));
        return Status;
    }

    if (Mp5QueryUlong(Adapter, OID_GEN_MAXIMUM_FRAME_SIZE, &General.MtuSize) != NDIS_STATUS_SUCCESS)
        General.MtuSize = 1500;

    /* In units of 100 bps */
    if (Mp5QueryUlong(Adapter, OID_GEN_LINK_SPEED, &LinkSpeed) == NDIS_STATUS_SUCCESS)
    {
        General.MaxXmitLinkSpeed = (ULONG64)LinkSpeed * 100;
        General.MaxRcvLinkSpeed = General.MaxXmitLinkSpeed;
        General.XmitLinkSpeed = General.MaxXmitLinkSpeed;
        General.RcvLinkSpeed = General.MaxXmitLinkSpeed;
    }

    if (Mp5QueryUlong(Adapter, OID_GEN_MEDIA_CONNECT_STATUS, &ConnectStatus) == NDIS_STATUS_SUCCESS)
    {
        General.MediaConnectState = (ConnectStatus == NdisMediaStateConnected) ?
                                    MediaConnectStateConnected : MediaConnectStateDisconnected;
    }
    else
    {
        General.MediaConnectState = MediaConnectStateConnected;
    }

    Status = CoreSetGeneralAttributes(Adapter, &General);
    if (Status != NDIS_STATUS_SUCCESS)
        return Status;

    Adapter->Core.CurrentLookahead = CurrentLookahead;
    Adapter->NdisMiniportBlock.MaxSendPackets = MaxSendPackets;

    return NDIS_STATUS_SUCCESS;
}

static
NDIS_STATUS
NTAPI
Mp5Initialize(
    _In_ NDIS_HANDLE NdisMiniportHandle,
    _In_ NDIS_HANDLE MiniportDriverContext,
    _In_ PNDIS_MINIPORT_INIT_PARAMETERS MiniportInitParameters)
{
    PLOGICAL_ADAPTER Adapter = (PLOGICAL_ADAPTER)NdisMiniportHandle;
    PNDIS_M_DRIVER_BLOCK Driver = (PNDIS_M_DRIVER_BLOCK)MiniportDriverContext;
    UINT SelectedMediumIndex = 0;
    NDIS_STATUS OpenErrorStatus;
    NDIS_STATUS Status;

    UNREFERENCED_PARAMETER(MiniportInitParameters);

    Mp5InstallHandlers(Adapter);

    Status = Driver->MiniportCharacteristics.InitializeHandler(&OpenErrorStatus,
                                                               &SelectedMediumIndex,
                                                               Mp5MediaArray,
                                                               MP5_MEDIA_COUNT,
                                                               Adapter,
                                                               (NDIS_HANDLE)Adapter->Core.WrapperContext);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        /* A failing NDIS 5 miniport must leave nothing registered behind */
        if (Adapter->NdisMiniportBlock.Interrupt != NULL || Adapter->NdisMiniportBlock.TimerQueue != NULL)
        {
            KeBugCheckEx(BUGCODE_ID_DRIVER,
                         (ULONG_PTR)Adapter,
                         (ULONG_PTR)Adapter->NdisMiniportBlock.Interrupt,
                         (ULONG_PTR)Adapter->NdisMiniportBlock.TimerQueue,
                         1);
        }
        return Status;
    }

    if (SelectedMediumIndex >= MP5_MEDIA_COUNT)
    {
        NDIS_DbgPrint(MIN_TRACE, ("MiniportInitialize selected a bad medium index.\n"));
        Driver->MiniportCharacteristics.HaltHandler(Adapter->NdisMiniportBlock.MiniportAdapterContext);
        return NDIS_STATUS_UNSUPPORTED_MEDIA;
    }

    Adapter->NdisMiniportBlock.MediaType = Mp5MediaArray[SelectedMediumIndex];

    Status = Mp5DescribeAdapter(Adapter);
    if (Status != NDIS_STATUS_SUCCESS)
        Driver->MiniportCharacteristics.HaltHandler(Adapter->NdisMiniportBlock.MiniportAdapterContext);

    return Status;
}

static
VOID
NTAPI
Mp5Halt(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ NDIS_HALT_ACTION HaltAction)
{
    PLOGICAL_ADAPTER Adapter = (PLOGICAL_ADAPTER)MiniportAdapterContext;

    UNREFERENCED_PARAMETER(HaltAction);

    Adapter->NdisMiniportBlock.DriverHandle->MiniportCharacteristics.HaltHandler(
        Adapter->NdisMiniportBlock.MiniportAdapterContext);
}

/* An NDIS 5 miniport has no pause state: the core already drained its sends */
static
NDIS_STATUS
NTAPI
Mp5Pause(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNDIS_MINIPORT_PAUSE_PARAMETERS PauseParameters)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(PauseParameters);

    return NDIS_STATUS_SUCCESS;
}

static
NDIS_STATUS
NTAPI
Mp5Restart(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNDIS_MINIPORT_RESTART_PARAMETERS RestartParameters)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(RestartParameters);

    return NDIS_STATUS_SUCCESS;
}

static
VOID
NTAPI
Mp5Shutdown(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ NDIS_SHUTDOWN_ACTION ShutdownAction)
{
    PLOGICAL_ADAPTER Adapter = (PLOGICAL_ADAPTER)MiniportAdapterContext;
    PMINIPORT_BUGCHECK_CONTEXT Context = Adapter->BugcheckContext;

    UNREFERENCED_PARAMETER(ShutdownAction);

    if (Context != NULL && Context->ShutdownHandler != NULL)
        Context->ShutdownHandler(Context->DriverContext);
}

static
VOID
NTAPI
Mp5DevicePnPEventNotify(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNET_DEVICE_PNP_EVENT NetDevicePnPEvent)
{
    PLOGICAL_ADAPTER Adapter = (PLOGICAL_ADAPTER)MiniportAdapterContext;
    PNDIS_M_DRIVER_BLOCK Driver = Adapter->NdisMiniportBlock.DriverHandle;

    if (Driver->MiniportCharacteristics.PnPEventNotifyHandler != NULL)
    {
        Driver->MiniportCharacteristics.PnPEventNotifyHandler(Adapter->NdisMiniportBlock.MiniportAdapterContext,
                                                              NetDevicePnPEvent->DevicePnPEvent,
                                                              NetDevicePnPEvent->InformationBuffer,
                                                              NetDevicePnPEvent->InformationBufferLength);
    }
}

/* Requests */

static
NDIS_STATUS
NTAPI
Mp5OidRequest(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNDIS_OID_REQUEST OidRequest)
{
    PLOGICAL_ADAPTER Adapter = (PLOGICAL_ADAPTER)MiniportAdapterContext;
    PNDIS_M_DRIVER_BLOCK Driver = Adapter->NdisMiniportBlock.DriverHandle;
    NDIS_STATUS Status;
    KIRQL OldIrql;

    /* NDIS 5 calls these at DISPATCH_LEVEL */
    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);

    switch (OidRequest->RequestType)
    {
        case NdisRequestQueryInformation:
        case NdisRequestQueryStatistics:
            Status = Driver->MiniportCharacteristics.QueryInformationHandler(
                Adapter->NdisMiniportBlock.MiniportAdapterContext,
                OidRequest->DATA.QUERY_INFORMATION.Oid,
                OidRequest->DATA.QUERY_INFORMATION.InformationBuffer,
                OidRequest->DATA.QUERY_INFORMATION.InformationBufferLength,
                (PULONG)&OidRequest->DATA.QUERY_INFORMATION.BytesWritten,
                (PULONG)&OidRequest->DATA.QUERY_INFORMATION.BytesNeeded);
            break;

        case NdisRequestSetInformation:
            Status = Driver->MiniportCharacteristics.SetInformationHandler(
                Adapter->NdisMiniportBlock.MiniportAdapterContext,
                OidRequest->DATA.SET_INFORMATION.Oid,
                OidRequest->DATA.SET_INFORMATION.InformationBuffer,
                OidRequest->DATA.SET_INFORMATION.InformationBufferLength,
                (PULONG)&OidRequest->DATA.SET_INFORMATION.BytesRead,
                (PULONG)&OidRequest->DATA.SET_INFORMATION.BytesNeeded);
            break;

        default:
            Status = NDIS_STATUS_NOT_SUPPORTED;
            break;
    }

    KeLowerIrql(OldIrql);
    return Status;
}

/* NdisMQueryInformationComplete and NdisMSetInformationComplete land here */
static
VOID
NTAPI
Mp5RequestComplete(
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ NDIS_STATUS Status)
{
    PLOGICAL_ADAPTER Adapter = (PLOGICAL_ADAPTER)MiniportAdapterHandle;
    PCORE_OID_REQUEST Active = Adapter->Core.ActiveOidRequest;

    if (Active == NULL)
    {
        NDIS_DbgPrint(MIN_TRACE, ("Request completion with no request outstanding.\n"));
        return;
    }

    CoreOidRequestComplete(Adapter, &Active->Request, Status);
}

static
VOID
NTAPI
Mp5CancelOidRequest(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PVOID RequestId)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(RequestId);
}

/* Send */

/*
 * Serialized miniports are called at DISPATCH_LEVEL and may push back with
 * NDIS_STATUS_RESOURCES, which parks the packet on the work queue.
 */
static
NDIS_STATUS
Mp5CallSendHandler(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PNDIS_PACKET Packet)
{
    PNDIS_M_DRIVER_BLOCK Driver = Adapter->NdisMiniportBlock.DriverHandle;
    NDIS_STATUS Status;
    KIRQL OldIrql;

    if (Mp5IsDeserialized(Adapter))
    {
        if (Driver->MiniportCharacteristics.SendPacketsHandler != NULL)
        {
            Driver->MiniportCharacteristics.SendPacketsHandler(Adapter->NdisMiniportBlock.MiniportAdapterContext,
                                                               &Packet,
                                                               1);
            return NDIS_STATUS_PENDING;
        }

        return Driver->MiniportCharacteristics.SendHandler(Adapter->NdisMiniportBlock.MiniportAdapterContext,
                                                           Packet,
                                                           Packet->Private.Flags);
    }

    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);

    if (Driver->MiniportCharacteristics.SendPacketsHandler != NULL)
    {
        Driver->MiniportCharacteristics.SendPacketsHandler(Adapter->NdisMiniportBlock.MiniportAdapterContext,
                                                           &Packet,
                                                           1);
        Status = NDIS_GET_PACKET_STATUS(Packet);
    }
    else
    {
        Status = Driver->MiniportCharacteristics.SendHandler(Adapter->NdisMiniportBlock.MiniportAdapterContext,
                                                             Packet,
                                                             Packet->Private.Flags);
    }

    KeLowerIrql(OldIrql);

    if (Status == NDIS_STATUS_RESOURCES)
    {
        MiniQueueWorkItem(Adapter, NdisWorkItemSend, Packet, TRUE);
        Status = NDIS_STATUS_PENDING;
    }

    return Status;
}

/**
 * @brief
 * Hands one packet to an NDIS 5 miniport, queueing it while the miniport is
 * busy.
 *
 * @param[in] Adapter
 * The adapter.
 *
 * @param[in] Packet
 * The packet.
 *
 * @return
 * The send status, or NDIS_STATUS_PENDING.
 */
NDIS_STATUS
Mp5SendPacket(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PNDIS_PACKET Packet)
{
    NDIS_STATUS Status;

    if (MiniIsBusy(Adapter, NdisWorkItemSend))
    {
        MiniQueueWorkItem(Adapter, NdisWorkItemSend, Packet, FALSE);
        return NDIS_STATUS_PENDING;
    }

#if DBG
    MiniDisplayPacket(Packet, "SEND");
#endif

    Status = Mp5CallSendHandler(Adapter, Packet);

    if (Status != NDIS_STATUS_PENDING)
        MiniWorkItemComplete(Adapter, NdisWorkItemSend);

    return Status;
}

/**
 * @brief
 * Sends a packet the work queue held back while the miniport was busy.
 *
 * @param[in] Adapter
 * The adapter.
 *
 * @param[in] Packet
 * The packet.
 */
VOID
Mp5SendQueuedPacket(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PNDIS_PACKET Packet)
{
    NDIS_STATUS Status = Mp5CallSendHandler(Adapter, Packet);

    if (Status != NDIS_STATUS_PENDING)
        MiniSendComplete(Adapter, Packet, Status);
}

static
VOID
NTAPI
Mp5ScatterGatherReady(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PSCATTER_GATHER_LIST ScatterGather,
    _In_ PVOID Context)
{
    PMP5_DMA_CONTEXT DmaContext = Context;
    PLOGICAL_ADAPTER Adapter = DmaContext->Adapter;
    PNDIS_PACKET Packet = DmaContext->Packet;
    NDIS_STATUS Status;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    ExFreePoolWithTag(DmaContext, NDIS_TAG);

    NDIS_PER_PACKET_INFO_FROM_PACKET(Packet, ScatterGatherListPacketInfo) = ScatterGather;

    Status = Mp5SendPacket(Adapter, Packet);
    if (Status != NDIS_STATUS_PENDING)
        MiniSendComplete(Adapter, Packet, Status);
}

/* Miniports that asked for scatter gather DMA get the list built first */
static
NDIS_STATUS
Mp5SendPacketWithDma(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PNDIS_PACKET Packet)
{
    PMP5_DMA_CONTEXT DmaContext;
    PNDIS_BUFFER Buffer;
    UINT PacketLength;
    NTSTATUS Status;
    KIRQL OldIrql;

    NdisQueryPacket(Packet, NULL, NULL, &Buffer, &PacketLength);

    DmaContext = ExAllocatePoolWithTag(NonPagedPool, sizeof(*DmaContext), NDIS_TAG);
    if (DmaContext == NULL)
        return NDIS_STATUS_RESOURCES;

    DmaContext->Adapter = Adapter;
    DmaContext->Packet = Packet;

    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);

    KeFlushIoBuffers(Buffer, FALSE, TRUE);

    Status = Adapter->NdisMiniportBlock.SystemAdapterObject->DmaOperations->GetScatterGatherList(
        Adapter->NdisMiniportBlock.SystemAdapterObject,
        Adapter->NdisMiniportBlock.PhysicalDeviceObject,
        Buffer,
        MmGetMdlVirtualAddress(Buffer),
        PacketLength,
        Mp5ScatterGatherReady,
        DmaContext,
        TRUE);

    KeLowerIrql(OldIrql);

    if (!NT_SUCCESS(Status))
    {
        NDIS_DbgPrint(MIN_TRACE, ("GetScatterGatherList failed (0x%x).\n", Status));
        ExFreePoolWithTag(DmaContext, NDIS_TAG);
        return Status;
    }

    return NDIS_STATUS_PENDING;
}

/*
 * A list made from a protocol's packet goes down as that packet. Anything
 * else gets a packet laid over each of its NET_BUFFERs.
 */
static
PNDIS_PACKET
Mp5PacketForSend(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PNET_BUFFER_LIST NetBufferList,
    _In_ PNET_BUFFER NetBuffer)
{
    PNDIS_PACKET Packet;

    if (NetBufferList->NdisPoolHandle == Adapter->Core.SendNblPool)
        return CORE_NBL_FROM_PACKET(NetBufferList);

    Packet = CorePacketFromNetBuffer(Adapter->Core.SendPacketPool, NetBufferList, NetBuffer);
    if (Packet != NULL)
        NDIS_SET_PACKET_HEADER_SIZE(Packet, Adapter->MediumHeaderSize);

    return Packet;
}

static
VOID
Mp5NetBufferSent(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PNET_BUFFER_LIST NetBufferList,
    _In_ NDIS_STATUS Status)
{
    if (Status != NDIS_STATUS_SUCCESS)
        NET_BUFFER_LIST_STATUS(NetBufferList) = Status;

    /* The list goes back once every NET_BUFFER in it did */
    if (InterlockedDecrement(&CORE_NBL_PENDING_COUNT(NetBufferList)) == 0)
    {
        NdisMSendNetBufferListsComplete(Adapter,
                                        NetBufferList,
                                        (KeGetCurrentIrql() == DISPATCH_LEVEL) ?
                                        NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL : 0);
    }
}

static
VOID
NTAPI
Mp5SendNetBufferLists(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNET_BUFFER_LIST NetBufferLists,
    _In_ NDIS_PORT_NUMBER PortNumber,
    _In_ ULONG SendFlags)
{
    PLOGICAL_ADAPTER Adapter = (PLOGICAL_ADAPTER)MiniportAdapterContext;
    PNET_BUFFER_LIST NetBufferList;
    PNET_BUFFER_LIST Next;
    PNET_BUFFER NetBuffer;
    PNDIS_PACKET Packet;
    NDIS_STATUS Status;
    LONG Count;

    UNREFERENCED_PARAMETER(PortNumber);
    UNREFERENCED_PARAMETER(SendFlags);

    for (NetBufferList = NetBufferLists; NetBufferList != NULL; NetBufferList = Next)
    {
        Next = NET_BUFFER_LIST_NEXT_NBL(NetBufferList);
        NET_BUFFER_LIST_NEXT_NBL(NetBufferList) = NULL;
        NET_BUFFER_LIST_STATUS(NetBufferList) = NDIS_STATUS_SUCCESS;

        /* One extra count keeps the list from completing before the loop is done */
        Count = 1;
        for (NetBuffer = NET_BUFFER_LIST_FIRST_NB(NetBufferList); NetBuffer != NULL; NetBuffer = NET_BUFFER_NEXT_NB(NetBuffer))
            Count++;
        CORE_NBL_PENDING_COUNT(NetBufferList) = Count;

        for (NetBuffer = NET_BUFFER_LIST_FIRST_NB(NetBufferList); NetBuffer != NULL; NetBuffer = NET_BUFFER_NEXT_NB(NetBuffer))
        {
            Packet = Mp5PacketForSend(Adapter, NetBufferList, NetBuffer);
            if (Packet == NULL)
            {
                Mp5NetBufferSent(Adapter, NetBufferList, NDIS_STATUS_RESOURCES);
                continue;
            }

            CORE_PACKET_OWNER_NBL(Packet) = NetBufferList;

            if (Adapter->NdisMiniportBlock.ScatterGatherListSize != 0)
                Status = Mp5SendPacketWithDma(Adapter, Packet);
            else
                Status = Mp5SendPacket(Adapter, Packet);

            if (Status != NDIS_STATUS_PENDING)
                MiniSendComplete(Adapter, Packet, Status);
        }

        Mp5NetBufferSent(Adapter, NetBufferList, NDIS_STATUS_SUCCESS);
    }
}

/**
 * @brief
 * An NDIS 5 miniport finished sending a packet. NdisMSendComplete and the
 * work queue both land here.
 *
 * @param[in] MiniportAdapterHandle
 * The adapter.
 *
 * @param[in] Packet
 * The packet that was sent.
 *
 * @param[in] Status
 * How it went.
 */
VOID
NTAPI
MiniSendComplete(
    IN  NDIS_HANDLE     MiniportAdapterHandle,
    IN  PNDIS_PACKET    Packet,
    IN  NDIS_STATUS     Status)
{
    PLOGICAL_ADAPTER Adapter = (PLOGICAL_ADAPTER)MiniportAdapterHandle;
    PNET_BUFFER_LIST NetBufferList = CORE_PACKET_OWNER_NBL(Packet);
    PSCATTER_GATHER_LIST ScatterGather;
    KIRQL OldIrql;

    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);

    if (Adapter->NdisMiniportBlock.ScatterGatherListSize != 0)
    {
        ScatterGather = NDIS_PER_PACKET_INFO_FROM_PACKET(Packet, ScatterGatherListPacketInfo);
        if (ScatterGather != NULL)
        {
            Adapter->NdisMiniportBlock.SystemAdapterObject->DmaOperations->PutScatterGatherList(
                Adapter->NdisMiniportBlock.SystemAdapterObject,
                ScatterGather,
                TRUE);
            NDIS_PER_PACKET_INFO_FROM_PACKET(Packet, ScatterGatherListPacketInfo) = NULL;
        }
    }

    if (Packet->Private.Pool == (PNDIS_PACKET_POOL)Adapter->Core.SendPacketPool)
        CoreFreeNetBufferPacket(Packet);

    Mp5NetBufferSent(Adapter, NetBufferList, Status);

    KeLowerIrql(OldIrql);

    MiniWorkItemComplete(Adapter, NdisWorkItemSend);
}

static
VOID
NTAPI
Mp5SendResourcesAvailable(
    _In_ NDIS_HANDLE MiniportAdapterHandle)
{
    /* Run the work queue if anything is waiting */
    MiniWorkItemComplete((PLOGICAL_ADAPTER)MiniportAdapterHandle, NdisWorkItemSend);
}

static
VOID
NTAPI
Mp5CancelSend(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PVOID CancelId)
{
    PLOGICAL_ADAPTER Adapter = (PLOGICAL_ADAPTER)MiniportAdapterContext;
    PNDIS_M_DRIVER_BLOCK Driver = Adapter->NdisMiniportBlock.DriverHandle;

    if (Driver->MiniportCharacteristics.CancelSendPacketsHandler != NULL)
        Driver->MiniportCharacteristics.CancelSendPacketsHandler(Adapter->NdisMiniportBlock.MiniportAdapterContext,
                                                                 CancelId);
}

/* Receive */

static
VOID
Mp5FreeReceiveNetBufferList(
    _In_ PNET_BUFFER_LIST NetBufferList)
{
    PMDL Mdl;
    PVOID Buffer;

    /* A lookahead copy owns its buffer; a packet's buffers go back with the packet */
    if (CORE_NBL_FROM_PACKET(NetBufferList) == NULL)
    {
        Mdl = NET_BUFFER_FIRST_MDL(NET_BUFFER_LIST_FIRST_NB(NetBufferList));
        if (Mdl != NULL)
        {
            Buffer = MmGetMdlVirtualAddress(Mdl);
            IoFreeMdl(Mdl);
            ExFreePoolWithTag(Buffer, NDIS_TAG);
        }
    }

    NdisXlateFreeNetBufferLists(NetBufferList);
}

static
VOID
Mp5ReturnPacketToMiniport(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PNDIS_PACKET Packet)
{
    Adapter->NdisMiniportBlock.DriverHandle->MiniportCharacteristics.ReturnPacketHandler(
        Adapter->NdisMiniportBlock.MiniportAdapterContext,
        Packet);
}

static
VOID
NTAPI
Mp5ReturnNetBufferLists(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNET_BUFFER_LIST NetBufferLists,
    _In_ ULONG ReturnFlags)
{
    PLOGICAL_ADAPTER Adapter = (PLOGICAL_ADAPTER)MiniportAdapterContext;
    PNET_BUFFER_LIST NetBufferList;
    PNET_BUFFER_LIST Next;
    PNDIS_PACKET Packet;

    UNREFERENCED_PARAMETER(ReturnFlags);

    for (NetBufferList = NetBufferLists; NetBufferList != NULL; NetBufferList = Next)
    {
        Next = NET_BUFFER_LIST_NEXT_NBL(NetBufferList);
        NET_BUFFER_LIST_NEXT_NBL(NetBufferList) = NULL;

        Packet = CORE_NBL_FROM_PACKET(NetBufferList);
        if (Packet == NULL)
        {
            Mp5FreeReceiveNetBufferList(NetBufferList);
            continue;
        }

        /* Back while the indication is still running: the indicator hands the packet over */
        if (InterlockedCompareExchange(&CORE_NBL_PENDING_COUNT(NetBufferList),
                                       MP5_RECEIVE_RETURNED,
                                       MP5_RECEIVE_INDICATING) == MP5_RECEIVE_INDICATING)
        {
            continue;
        }

        Mp5FreeReceiveNetBufferList(NetBufferList);
        Mp5ReturnPacketToMiniport(Adapter, Packet);
    }
}

/*
 * A packet nobody above kept: a deserialized miniport always gets it through
 * MiniportReturnPacket, a serialized one takes it back when the call returns.
 */
static
VOID
Mp5GivePacketBack(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PNDIS_PACKET Packet)
{
    if (Mp5IsDeserialized(Adapter))
    {
        NDIS_SET_PACKET_STATUS(Packet, NDIS_STATUS_PENDING);
        Mp5ReturnPacketToMiniport(Adapter, Packet);
        return;
    }

    NDIS_SET_PACKET_STATUS(Packet, NDIS_STATUS_SUCCESS);
}

static
PNET_BUFFER_LIST
Mp5NetBufferListForPacket(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PNDIS_PACKET Packet)
{
    PNET_BUFFER_LIST NetBufferList;
    UINT TotalLength;

    NdisQueryPacket(Packet, NULL, NULL, NULL, &TotalLength);

    NetBufferList = NdisAllocateNetBufferAndNetBufferList(Adapter->Core.ReceiveNblPool,
                                                          0,
                                                          0,
                                                          Packet->Private.Head,
                                                          0,
                                                          TotalLength);
    if (NetBufferList == NULL)
        return NULL;

    NetBufferList->SourceHandle = (NDIS_HANDLE)Adapter;
    NetBufferList->NdisReserved[0] = Packet;
    CORE_NBL_PENDING_COUNT(NetBufferList) = MP5_RECEIVE_INDICATING;

    return NetBufferList;
}

/*
 * After the core indicated a packet's list: a packet nobody kept goes back to
 * the miniport now, the NDIS 5 way for its serialization, and one still held
 * comes back through Mp5ReturnNetBufferLists later.
 */
static
VOID
Mp5FinishPacketIndication(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PNDIS_PACKET Packet,
    _In_ PNET_BUFFER_LIST NetBufferList,
    _In_ BOOLEAN Resources)
{
    if (Resources)
    {
        /* The miniport takes the packet back as soon as the indication returns */
        Mp5FreeReceiveNetBufferList(NetBufferList);
        NDIS_SET_PACKET_STATUS(Packet, NDIS_STATUS_RESOURCES);
        return;
    }

    if (InterlockedCompareExchange(&CORE_NBL_PENDING_COUNT(NetBufferList),
                                   MP5_RECEIVE_DEFERRED,
                                   MP5_RECEIVE_INDICATING) == MP5_RECEIVE_INDICATING)
    {
        /* Still held above: MiniportReturnPacket comes later */
        NDIS_SET_PACKET_STATUS(Packet, NDIS_STATUS_PENDING);
        return;
    }

    Mp5FreeReceiveNetBufferList(NetBufferList);
    Mp5GivePacketBack(Adapter, Packet);
}

/* NdisMIndicateReceivePacket lands here */
static
VOID
NTAPI
Mp5IndicateReceivePacket(
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_reads_(NumberOfPackets) PPNDIS_PACKET PacketArray,
    _In_ UINT NumberOfPackets)
{
    PLOGICAL_ADAPTER Adapter = (PLOGICAL_ADAPTER)MiniportAdapterHandle;
    PNET_BUFFER_LIST NetBufferList;
    PNDIS_PACKET Packet;
    BOOLEAN Resources;
    ULONG ReceiveFlags;
    UINT i;

    /* One at a time, since each packet can carry its own resources status */
    for (i = 0; i < NumberOfPackets; i++)
    {
        Packet = PacketArray[i];
        Resources = (NDIS_GET_PACKET_STATUS(Packet) == NDIS_STATUS_RESOURCES);

        /* Before the first restart there are no pools and nobody to tell */
        NetBufferList = (Adapter->Core.State == CoreMiniportRunning) ?
                        Mp5NetBufferListForPacket(Adapter, Packet) : NULL;
        if (NetBufferList == NULL)
        {
            /* Nothing to indicate with: the miniport simply has it back */
            if (!Resources)
                Mp5GivePacketBack(Adapter, Packet);
            continue;
        }

        ReceiveFlags = Resources ? NDIS_RECEIVE_FLAGS_RESOURCES : 0;
        if (KeGetCurrentIrql() == DISPATCH_LEVEL)
            ReceiveFlags |= NDIS_RECEIVE_FLAGS_DISPATCH_LEVEL;

        NdisMIndicateReceiveNetBufferLists(Adapter, NetBufferList, NDIS_DEFAULT_PORT_NUMBER, 1, ReceiveFlags);

        Mp5FinishPacketIndication(Adapter, Packet, NetBufferList, Resources);
    }
}

static
VOID
Mp5IndicateCopy(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PNET_BUFFER_LIST NetBufferList)
{
    NdisMIndicateReceiveNetBufferLists(Adapter,
                                       NetBufferList,
                                       NDIS_DEFAULT_PORT_NUMBER,
                                       1,
                                       (KeGetCurrentIrql() == DISPATCH_LEVEL) ?
                                       NDIS_RECEIVE_FLAGS_DISPATCH_LEVEL : 0);
}

/**
 * @brief
 * A lookahead indication from an NDIS 5 miniport. The frame is copied whole,
 * asking the miniport for whatever the lookahead did not hold, and goes up as
 * a NET_BUFFER_LIST the core owns.
 *
 * @param[in] Adapter
 * The indicating adapter.
 *
 * @param[in] MacReceiveContext
 * The miniport's context for MiniportTransferData.
 *
 * @param[in] HeaderBuffer
 * The medium header.
 *
 * @param[in] HeaderBufferSize
 * Its size.
 *
 * @param[in] LookaheadBuffer
 * The start of the data.
 *
 * @param[in] LookaheadBufferSize
 * How much of it is there.
 *
 * @param[in] PacketSize
 * The size of the data in full.
 */
VOID
NTAPI
Mp5IndicateLookahead(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ NDIS_HANDLE MacReceiveContext,
    _In_reads_bytes_(HeaderBufferSize) PVOID HeaderBuffer,
    _In_ UINT HeaderBufferSize,
    _In_reads_bytes_(LookaheadBufferSize) PVOID LookaheadBuffer,
    _In_ UINT LookaheadBufferSize,
    _In_ UINT PacketSize)
{
    PNDIS_M_DRIVER_BLOCK Driver = Adapter->NdisMiniportBlock.DriverHandle;
    PNET_BUFFER_LIST NetBufferList;
    PCORE_PACKET_STATE State;
    PNDIS_PACKET TransferPacket;
    PMDL TransferMdl;
    PUCHAR Frame;
    PMDL Mdl;
    UINT Total = HeaderBufferSize + PacketSize;
    UINT BytesTransferred;
    NDIS_STATUS Status;

    if (Adapter->Core.State != CoreMiniportRunning || Total < PacketSize || Total == 0)
        return;

    Frame = ExAllocatePoolWithTag(NonPagedPool, Total, NDIS_TAG);
    if (Frame == NULL)
        return;

    Mdl = IoAllocateMdl(Frame, Total, FALSE, FALSE, NULL);
    if (Mdl == NULL)
    {
        ExFreePoolWithTag(Frame, NDIS_TAG);
        return;
    }
    MmBuildMdlForNonPagedPool(Mdl);

    NetBufferList = NdisAllocateNetBufferAndNetBufferList(Adapter->Core.ReceiveNblPool, 0, 0, Mdl, 0, Total);
    if (NetBufferList == NULL)
    {
        IoFreeMdl(Mdl);
        ExFreePoolWithTag(Frame, NDIS_TAG);
        return;
    }
    NetBufferList->SourceHandle = (NDIS_HANDLE)Adapter;

    RtlCopyMemory(Frame, HeaderBuffer, HeaderBufferSize);

    if (PacketSize <= LookaheadBufferSize)
    {
        RtlCopyMemory(Frame + HeaderBufferSize, LookaheadBuffer, PacketSize);
        Mp5IndicateCopy(Adapter, NetBufferList);
        return;
    }

    /* The rest has to come from the miniport */
    if (Driver->MiniportCharacteristics.TransferDataHandler == NULL)
    {
        Mp5FreeReceiveNetBufferList(NetBufferList);
        return;
    }

    NdisAllocatePacket(&Status, &TransferPacket, Adapter->Core.ReceivePacketPool);
    TransferMdl = (Status == NDIS_STATUS_SUCCESS) ?
                  IoAllocateMdl(Frame + HeaderBufferSize, PacketSize, FALSE, FALSE, NULL) : NULL;
    if (TransferMdl == NULL)
    {
        if (Status == NDIS_STATUS_SUCCESS)
            NdisFreePacket(TransferPacket);
        Mp5FreeReceiveNetBufferList(NetBufferList);
        return;
    }
    MmBuildMdlForNonPagedPool(TransferMdl);

    TransferPacket->Private.Head = TransferMdl;
    TransferPacket->Private.Tail = TransferMdl;
    TransferPacket->Private.ValidCounts = FALSE;

    State = CORE_PACKET_STATE(TransferPacket);
    State->NetBufferList = NetBufferList;
    State->LastMdl = TransferMdl;

    Status = Driver->MiniportCharacteristics.TransferDataHandler(TransferPacket,
                                                                 &BytesTransferred,
                                                                 Adapter->NdisMiniportBlock.MiniportAdapterContext,
                                                                 MacReceiveContext,
                                                                 0,
                                                                 PacketSize);
    if (Status == NDIS_STATUS_PENDING)
        return;

    TransferPacket->Private.Head = NULL;
    IoFreeMdl(TransferMdl);
    NdisFreePacket(TransferPacket);

    if (Status == NDIS_STATUS_SUCCESS)
        Mp5IndicateCopy(Adapter, NetBufferList);
    else
        Mp5FreeReceiveNetBufferList(NetBufferList);
}

/* NdisMTransferDataComplete lands here, for the transfers started above */
static
VOID
NTAPI
Mp5TransferDataComplete(
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ PNDIS_PACKET Packet,
    _In_ NDIS_STATUS Status,
    _In_ UINT BytesTransferred)
{
    PLOGICAL_ADAPTER Adapter = (PLOGICAL_ADAPTER)MiniportAdapterHandle;
    PCORE_PACKET_STATE State;
    PNET_BUFFER_LIST NetBufferList;
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(BytesTransferred);

    if (Packet->Private.Pool != (PNDIS_PACKET_POOL)Adapter->Core.ReceivePacketPool)
    {
        NDIS_DbgPrint(MIN_TRACE, ("Transfer completion for a packet NDIS did not start.\n"));
        return;
    }

    State = CORE_PACKET_STATE(Packet);
    NetBufferList = State->NetBufferList;

    Packet->Private.Head = NULL;
    IoFreeMdl(State->LastMdl);
    NdisFreePacket(Packet);

    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    if (Status == NDIS_STATUS_SUCCESS)
        Mp5IndicateCopy(Adapter, NetBufferList);
    else
        Mp5FreeReceiveNetBufferList(NetBufferList);
    KeLowerIrql(OldIrql);
}

/* Status */

/* NdisMIndicateStatus lands here */
static
VOID
NTAPI
Mp5IndicateStatus(
    _In_ NDIS_HANDLE MiniportHandle,
    _In_ NDIS_STATUS GeneralStatus,
    _In_ PVOID StatusBuffer,
    _In_ UINT StatusBufferSize)
{
    PLOGICAL_ADAPTER Adapter = (PLOGICAL_ADAPTER)MiniportHandle;
    NDIS_STATUS_INDICATION Indication;
    NDIS_LINK_STATE LinkState;

    RtlZeroMemory(&Indication, sizeof(Indication));
    Indication.Header.Type = NDIS_OBJECT_TYPE_STATUS_INDICATION;
    Indication.Header.Revision = NDIS_STATUS_INDICATION_REVISION_1;
    Indication.Header.Size = NDIS_SIZEOF_STATUS_INDICATION_REVISION_1;
    Indication.SourceHandle = Adapter;
    Indication.PortNumber = NDIS_DEFAULT_PORT_NUMBER;

    if (GeneralStatus == NDIS_STATUS_MEDIA_CONNECT || GeneralStatus == NDIS_STATUS_MEDIA_DISCONNECT)
    {
        /* Media state is link state in the core */
        LinkState = Adapter->Core.LinkState;
        LinkState.MediaConnectState = (GeneralStatus == NDIS_STATUS_MEDIA_CONNECT) ?
                                      MediaConnectStateConnected : MediaConnectStateDisconnected;

        Indication.StatusCode = NDIS_STATUS_LINK_STATE;
        Indication.StatusBuffer = &LinkState;
        Indication.StatusBufferSize = sizeof(LinkState);
    }
    else
    {
        Indication.StatusCode = GeneralStatus;
        Indication.StatusBuffer = StatusBuffer;
        Indication.StatusBufferSize = StatusBufferSize;
    }

    CoreIndicateStatus(Adapter, &Indication);
}

/* Every status already went up complete */
static
VOID
NTAPI
Mp5IndicateStatusComplete(
    _In_ NDIS_HANDLE MiniportAdapterHandle)
{
    UNREFERENCED_PARAMETER(MiniportAdapterHandle);
}

/* The NDIS 5 macros call through these */
static
VOID
Mp5InstallHandlers(
    _In_ PLOGICAL_ADAPTER Adapter)
{
    PNDIS_MINIPORT_BLOCK Block = &Adapter->NdisMiniportBlock;

    Block->EthRxIndicateHandler = EthFilterDprIndicateReceive;
    Block->EthRxCompleteHandler = EthFilterDprIndicateReceiveComplete;
    Block->SendCompleteHandler = MiniSendComplete;
    Block->SendResourcesHandler = Mp5SendResourcesAvailable;
    Block->ResetCompleteHandler = MiniResetComplete;
    Block->TDCompleteHandler = Mp5TransferDataComplete;
    Block->PacketIndicateHandler = Mp5IndicateReceivePacket;
    Block->StatusHandler = Mp5IndicateStatus;
    Block->StatusCompleteHandler = Mp5IndicateStatusComplete;
    Block->SendPacketsHandler = ProSendPackets;
    Block->QueryCompleteHandler = Mp5RequestComplete;
    Block->SetCompleteHandler = Mp5RequestComplete;
}

/* The NDIS 6 entry points an NDIS 5 miniport is driven through */
NDIS_MINIPORT_DRIVER_CHARACTERISTICS Mp5ShimCharacteristics =
{
    { NDIS_OBJECT_TYPE_MINIPORT_DRIVER_CHARACTERISTICS,
      NDIS_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_1,
      NDIS_SIZEOF_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_1 },
    5,                          /* MajorNdisVersion */
    1,                          /* MinorNdisVersion */
    0,                          /* MajorDriverVersion */
    0,                          /* MinorDriverVersion */
    0,                          /* Flags */
    NULL,                       /* SetOptionsHandler */
    Mp5Initialize,
    Mp5Halt,
    NULL,                       /* UnloadHandler */
    Mp5Pause,
    Mp5Restart,
    Mp5OidRequest,
    Mp5SendNetBufferLists,
    Mp5ReturnNetBufferLists,
    Mp5CancelSend,
    NULL,                       /* CheckForHangHandlerEx, see MiniCallCheckForHangHandler */
    NULL,                       /* ResetHandlerEx, see MiniCallResetHandler */
    Mp5DevicePnPEventNotify,
    Mp5Shutdown,
    Mp5CancelOidRequest
};
