/*
 * PROJECT:     ReactOS Cellular Data Adapter
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     NDIS 6 miniport entry points
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "rmnet.h"

#define NDEBUG
#include <debug.h>

NDIS_HANDLE RmNetDriverHandle;
PRMNET_ADAPTER RmNetAdapter;

/* Locally administered addresses, ours and the one the modem answers ARP with */
static const UCHAR RmNetLocalAddress[RMNET_ETH_ADDRESS] = { 0x02, 0x52, 0x4D, 0x4E, 0x45, 0x54 };
static const UCHAR RmNetPeerAddress[RMNET_ETH_ADDRESS] = { 0x02, 0x52, 0x4D, 0x4E, 0x45, 0x01 };

static const NDIS_OID RmNetSupportedOids[] =
{
    OID_GEN_SUPPORTED_LIST,
    OID_GEN_HARDWARE_STATUS,
    OID_GEN_MEDIA_SUPPORTED,
    OID_GEN_MEDIA_IN_USE,
    OID_GEN_PHYSICAL_MEDIUM,
    OID_GEN_MAXIMUM_LOOKAHEAD,
    OID_GEN_CURRENT_LOOKAHEAD,
    OID_GEN_MAXIMUM_FRAME_SIZE,
    OID_GEN_MAXIMUM_TOTAL_SIZE,
    OID_GEN_LINK_SPEED,
    OID_GEN_TRANSMIT_BUFFER_SPACE,
    OID_GEN_RECEIVE_BUFFER_SPACE,
    OID_GEN_TRANSMIT_BLOCK_SIZE,
    OID_GEN_RECEIVE_BLOCK_SIZE,
    OID_GEN_VENDOR_ID,
    OID_GEN_VENDOR_DESCRIPTION,
    OID_GEN_VENDOR_DRIVER_VERSION,
    OID_GEN_DRIVER_VERSION,
    OID_GEN_CURRENT_PACKET_FILTER,
    OID_GEN_MAC_OPTIONS,
    OID_GEN_MEDIA_CONNECT_STATUS,
    OID_GEN_MAXIMUM_SEND_PACKETS,
    OID_GEN_XMIT_OK,
    OID_GEN_RCV_OK,
    OID_GEN_XMIT_ERROR,
    OID_GEN_RCV_ERROR,
    OID_GEN_RCV_NO_BUFFER,
    OID_802_3_PERMANENT_ADDRESS,
    OID_802_3_CURRENT_ADDRESS,
    OID_802_3_MULTICAST_LIST,
    OID_802_3_MAXIMUM_LIST_SIZE,
    OID_802_3_RCV_ERROR_ALIGNMENT,
    OID_802_3_XMIT_ONE_COLLISION,
    OID_802_3_XMIT_MORE_COLLISIONS
};

static
NDIS_STATUS
RmNetSetRegistrationAttributes(
    _In_ PRMNET_ADAPTER Adapter)
{
    NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES Attributes;

    NdisZeroMemory(&Attributes, sizeof(Attributes));
    Attributes.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES;
    Attributes.Header.Revision = NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1;
    Attributes.Header.Size = NDIS_SIZEOF_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1;
    Attributes.MiniportAdapterContext = Adapter;
    Attributes.AttributeFlags = NDIS_MINIPORT_ATTRIBUTES_NO_HALT_ON_SUSPEND;
    Attributes.CheckForHangTimeInSeconds = 0;
    Attributes.InterfaceType = NdisInterfaceInternal;

    return NdisMSetMiniportAttributes(Adapter->MiniportHandle,
                                      (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&Attributes);
}

static
NDIS_STATUS
RmNetSetGeneralAttributes(
    _In_ PRMNET_ADAPTER Adapter)
{
    NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES Attributes;

    NdisZeroMemory(&Attributes, sizeof(Attributes));
    Attributes.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES;
    Attributes.Header.Revision = NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_2;
    Attributes.Header.Size = NDIS_SIZEOF_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_2;
    Attributes.MediaType = NdisMedium802_3;
    Attributes.PhysicalMediumType = NdisPhysicalMediumUnspecified;
    Attributes.MtuSize = RMNET_DEFAULT_MTU;
    Attributes.MaxXmitLinkSpeed = RMNET_LINK_SPEED;
    Attributes.MaxRcvLinkSpeed = RMNET_LINK_SPEED;
    Attributes.XmitLinkSpeed = NDIS_LINK_SPEED_UNKNOWN;
    Attributes.RcvLinkSpeed = NDIS_LINK_SPEED_UNKNOWN;
    Attributes.MediaConnectState = MediaConnectStateDisconnected;
    Attributes.MediaDuplexState = MediaDuplexStateFull;
    Attributes.LookaheadSize = RMNET_DEFAULT_MTU;
    Attributes.MacOptions = NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA |
                            NDIS_MAC_OPTION_TRANSFERS_NOT_PEND |
                            NDIS_MAC_OPTION_NO_LOOPBACK;
    Attributes.SupportedPacketFilters = NDIS_PACKET_TYPE_DIRECTED | NDIS_PACKET_TYPE_MULTICAST |
                                        NDIS_PACKET_TYPE_ALL_MULTICAST | NDIS_PACKET_TYPE_BROADCAST;
    Attributes.MaxMulticastListSize = 32;
    Attributes.MacAddressLength = RMNET_ETH_ADDRESS;
    NdisMoveMemory(Attributes.PermanentMacAddress, Adapter->Address, RMNET_ETH_ADDRESS);
    NdisMoveMemory(Attributes.CurrentMacAddress, Adapter->Address, RMNET_ETH_ADDRESS);
    Attributes.RecvScaleCapabilities = NULL;
    Attributes.AccessType = NET_IF_ACCESS_BROADCAST;
    Attributes.DirectionType = NET_IF_DIRECTION_SENDRECEIVE;
    Attributes.ConnectionType = NET_IF_CONNECTION_DEDICATED;
    Attributes.IfType = IF_TYPE_ETHERNET_CSMACD;
    Attributes.IfConnectorPresent = FALSE;
    Attributes.SupportedStatistics = NDIS_STATISTICS_XMIT_OK_SUPPORTED |
                                     NDIS_STATISTICS_RCV_OK_SUPPORTED |
                                     NDIS_STATISTICS_XMIT_ERROR_SUPPORTED |
                                     NDIS_STATISTICS_RCV_ERROR_SUPPORTED |
                                     NDIS_STATISTICS_RCV_NO_BUFFER_SUPPORTED;
    Attributes.SupportedOidList = (PNDIS_OID)RmNetSupportedOids;
    Attributes.SupportedOidListLength = sizeof(RmNetSupportedOids);

    return NdisMSetMiniportAttributes(Adapter->MiniportHandle,
                                      (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&Attributes);
}

static
NDIS_HANDLE
RmNetCreatePool(
    _In_ PRMNET_ADAPTER Adapter)
{
    NET_BUFFER_LIST_POOL_PARAMETERS Parameters;

    NdisZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    Parameters.Header.Revision = NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
    Parameters.Header.Size = NDIS_SIZEOF_NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
    Parameters.ProtocolId = NDIS_PROTOCOL_ID_DEFAULT;
    Parameters.fAllocateNetBuffer = TRUE;
    Parameters.PoolTag = RMNET_TAG;

    return NdisAllocateNetBufferListPool(Adapter->MiniportHandle, &Parameters);
}

static
NDIS_STATUS
NTAPI
RmNetInitialize(
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ NDIS_HANDLE MiniportDriverContext,
    _In_ PNDIS_MINIPORT_INIT_PARAMETERS MiniportInitParameters)
{
    PRMNET_ADAPTER Adapter;
    NDIS_STATUS Status;

    UNREFERENCED_PARAMETER(MiniportDriverContext);
    UNREFERENCED_PARAMETER(MiniportInitParameters);

    /* The control device and the IPA client are for one adapter */
    if (RmNetAdapter)
        return NDIS_STATUS_FAILURE;

    Adapter = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Adapter), RMNET_TAG);
    if (!Adapter)
        return NDIS_STATUS_RESOURCES;

    NdisZeroMemory(Adapter, sizeof(*Adapter));
    Adapter->MiniportHandle = MiniportAdapterHandle;
    NdisAllocateSpinLock(&Adapter->Lock);
    NdisMoveMemory(Adapter->Address, RmNetLocalAddress, RMNET_ETH_ADDRESS);
    NdisMoveMemory(Adapter->PeerAddress, RmNetPeerAddress, RMNET_ETH_ADDRESS);
    Adapter->Link.Mtu = RMNET_DEFAULT_MTU;

    Status = RmNetSetRegistrationAttributes(Adapter);
    if (Status == NDIS_STATUS_SUCCESS)
        Status = RmNetSetGeneralAttributes(Adapter);
    if (Status != NDIS_STATUS_SUCCESS)
        goto Fail;

    Adapter->FramePool = RmNetCreatePool(Adapter);
    Adapter->PacketPool = RmNetCreatePool(Adapter);
    if (!Adapter->FramePool || !Adapter->PacketPool)
    {
        Status = NDIS_STATUS_RESOURCES;
        goto Fail;
    }

    RmNetAdapter = Adapter;
    RmNetControlCreate(Adapter);
    RmNetIpaConnect(Adapter);

    DPRINT1("rmnet: adapter up, waiting for the RIL to connect a data session\n");
    return NDIS_STATUS_SUCCESS;

Fail:
    DPRINT1("rmnet: initialization failed 0x%lx\n", Status);
    if (Adapter->FramePool)
        NdisFreeNetBufferListPool(Adapter->FramePool);
    if (Adapter->PacketPool)
        NdisFreeNetBufferListPool(Adapter->PacketPool);
    NdisFreeSpinLock(&Adapter->Lock);
    ExFreePoolWithTag(Adapter, RMNET_TAG);
    return Status;
}

static
VOID
NTAPI
RmNetHalt(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ NDIS_HALT_ACTION HaltAction)
{
    PRMNET_ADAPTER Adapter = MiniportAdapterContext;

    UNREFERENCED_PARAMETER(HaltAction);

    Adapter->Halting = TRUE;
    RmNetControlDelete();
    RmNetIpaDisconnect(Adapter);

    NdisFreeNetBufferListPool(Adapter->FramePool);
    NdisFreeNetBufferListPool(Adapter->PacketPool);
    NdisFreeSpinLock(&Adapter->Lock);
    RmNetAdapter = NULL;
    ExFreePoolWithTag(Adapter, RMNET_TAG);
}

static
NDIS_STATUS
NTAPI
RmNetPause(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNDIS_MINIPORT_PAUSE_PARAMETERS PauseParameters)
{
    PRMNET_ADAPTER Adapter = MiniportAdapterContext;

    UNREFERENCED_PARAMETER(PauseParameters);

    Adapter->Paused = TRUE;
    return NDIS_STATUS_SUCCESS;
}

static
NDIS_STATUS
NTAPI
RmNetRestart(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNDIS_MINIPORT_RESTART_PARAMETERS RestartParameters)
{
    PRMNET_ADAPTER Adapter = MiniportAdapterContext;

    UNREFERENCED_PARAMETER(RestartParameters);

    Adapter->Paused = FALSE;
    RmNetIndicateLinkState(Adapter);
    return NDIS_STATUS_SUCCESS;
}

static
NDIS_STATUS
RmNetQueryUlong(
    _Inout_ PNDIS_OID_REQUEST Request,
    _In_ ULONG Value)
{
    ULONG Length = Request->DATA.QUERY_INFORMATION.InformationBufferLength;

    if (Length < sizeof(ULONG))
    {
        Request->DATA.QUERY_INFORMATION.BytesNeeded = sizeof(ULONG);
        return NDIS_STATUS_BUFFER_TOO_SHORT;
    }

    *(PULONG)Request->DATA.QUERY_INFORMATION.InformationBuffer = Value;
    Request->DATA.QUERY_INFORMATION.BytesWritten = sizeof(ULONG);
    return NDIS_STATUS_SUCCESS;
}

static
NDIS_STATUS
RmNetQueryCounter(
    _Inout_ PNDIS_OID_REQUEST Request,
    _In_ ULONG64 Value)
{
    ULONG Length = Request->DATA.QUERY_INFORMATION.InformationBufferLength;

    /* Counters go out as 64 bits when the caller left room, 32 bits otherwise */
    if (Length >= sizeof(ULONG64))
    {
        *(PULONG64)Request->DATA.QUERY_INFORMATION.InformationBuffer = Value;
        Request->DATA.QUERY_INFORMATION.BytesWritten = sizeof(ULONG64);
        return NDIS_STATUS_SUCCESS;
    }

    return RmNetQueryUlong(Request, (ULONG)Value);
}

static
NDIS_STATUS
RmNetQueryBytes(
    _Inout_ PNDIS_OID_REQUEST Request,
    _In_reads_bytes_(Size) const VOID *Data,
    _In_ ULONG Size)
{
    if (Request->DATA.QUERY_INFORMATION.InformationBufferLength < Size)
    {
        Request->DATA.QUERY_INFORMATION.BytesNeeded = Size;
        return NDIS_STATUS_BUFFER_TOO_SHORT;
    }

    NdisMoveMemory(Request->DATA.QUERY_INFORMATION.InformationBuffer, Data, Size);
    Request->DATA.QUERY_INFORMATION.BytesWritten = Size;
    return NDIS_STATUS_SUCCESS;
}

static
NDIS_STATUS
RmNetQuery(
    _In_ PRMNET_ADAPTER Adapter,
    _Inout_ PNDIS_OID_REQUEST Request)
{
    static const CHAR Description[] = "ReactOS Cellular Data Adapter";
    NDIS_MEDIUM Medium = NdisMedium802_3;

    switch (Request->DATA.QUERY_INFORMATION.Oid)
    {
        case OID_GEN_SUPPORTED_LIST:
            return RmNetQueryBytes(Request, RmNetSupportedOids, sizeof(RmNetSupportedOids));

        case OID_GEN_HARDWARE_STATUS:
            return RmNetQueryUlong(Request, NdisHardwareStatusReady);

        case OID_GEN_MEDIA_SUPPORTED:
        case OID_GEN_MEDIA_IN_USE:
            return RmNetQueryBytes(Request, &Medium, sizeof(Medium));

        case OID_GEN_PHYSICAL_MEDIUM:
            return RmNetQueryUlong(Request, NdisPhysicalMediumUnspecified);

        case OID_GEN_MAXIMUM_LOOKAHEAD:
        case OID_GEN_CURRENT_LOOKAHEAD:
        case OID_GEN_MAXIMUM_FRAME_SIZE:
            return RmNetQueryUlong(Request, Adapter->Link.Mtu);

        case OID_GEN_MAXIMUM_TOTAL_SIZE:
        case OID_GEN_TRANSMIT_BLOCK_SIZE:
        case OID_GEN_RECEIVE_BLOCK_SIZE:
            return RmNetQueryUlong(Request, Adapter->Link.Mtu + RMNET_ETH_HEADER);

        case OID_GEN_TRANSMIT_BUFFER_SPACE:
        case OID_GEN_RECEIVE_BUFFER_SPACE:
            return RmNetQueryUlong(Request, 64 * RMNET_FRAME_MAX);

        case OID_GEN_LINK_SPEED:
            return RmNetQueryUlong(Request, (ULONG)(RMNET_LINK_SPEED / 100));

        case OID_GEN_VENDOR_ID:
            return RmNetQueryUlong(Request, 0x00FFFFFF);

        case OID_GEN_VENDOR_DESCRIPTION:
            return RmNetQueryBytes(Request, Description, sizeof(Description));

        case OID_GEN_VENDOR_DRIVER_VERSION:
            return RmNetQueryUlong(Request, 0x00010000);

        case OID_GEN_DRIVER_VERSION:
            return RmNetQueryUlong(Request, 0x0600);

        case OID_GEN_CURRENT_PACKET_FILTER:
            return RmNetQueryUlong(Request, Adapter->PacketFilter);

        case OID_GEN_MAC_OPTIONS:
            return RmNetQueryUlong(Request, NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA |
                                            NDIS_MAC_OPTION_TRANSFERS_NOT_PEND |
                                            NDIS_MAC_OPTION_NO_LOOPBACK);

        case OID_GEN_MEDIA_CONNECT_STATUS:
            return RmNetQueryUlong(Request, Adapter->Connected ? NdisMediaStateConnected
                                                               : NdisMediaStateDisconnected);

        case OID_GEN_MAXIMUM_SEND_PACKETS:
            return RmNetQueryUlong(Request, 16);

        case OID_GEN_XMIT_OK:
            return RmNetQueryCounter(Request, Adapter->SentPackets);

        case OID_GEN_RCV_OK:
            return RmNetQueryCounter(Request, Adapter->ReceivedPackets);

        case OID_GEN_RCV_NO_BUFFER:
            return RmNetQueryCounter(Request, Adapter->DroppedPackets);

        case OID_GEN_XMIT_ERROR:
        case OID_GEN_RCV_ERROR:
        case OID_802_3_RCV_ERROR_ALIGNMENT:
        case OID_802_3_XMIT_ONE_COLLISION:
        case OID_802_3_XMIT_MORE_COLLISIONS:
            return RmNetQueryUlong(Request, 0);

        case OID_802_3_PERMANENT_ADDRESS:
        case OID_802_3_CURRENT_ADDRESS:
            return RmNetQueryBytes(Request, Adapter->Address, RMNET_ETH_ADDRESS);

        case OID_802_3_MAXIMUM_LIST_SIZE:
            return RmNetQueryUlong(Request, 32);

        default:
            return NDIS_STATUS_NOT_SUPPORTED;
    }
}

static
NDIS_STATUS
RmNetSet(
    _In_ PRMNET_ADAPTER Adapter,
    _Inout_ PNDIS_OID_REQUEST Request)
{
    PVOID Buffer = Request->DATA.SET_INFORMATION.InformationBuffer;
    ULONG Length = Request->DATA.SET_INFORMATION.InformationBufferLength;

    switch (Request->DATA.SET_INFORMATION.Oid)
    {
        case OID_GEN_CURRENT_PACKET_FILTER:
            if (Length < sizeof(ULONG))
                return NDIS_STATUS_INVALID_LENGTH;
            Adapter->PacketFilter = *(PULONG)Buffer;
            Request->DATA.SET_INFORMATION.BytesRead = sizeof(ULONG);
            return NDIS_STATUS_SUCCESS;

        case OID_GEN_CURRENT_LOOKAHEAD:
            if (Length < sizeof(ULONG))
                return NDIS_STATUS_INVALID_LENGTH;
            Request->DATA.SET_INFORMATION.BytesRead = sizeof(ULONG);
            return NDIS_STATUS_SUCCESS;

        /* The modem link carries no multicast, the list is only accepted */
        case OID_802_3_MULTICAST_LIST:
            Request->DATA.SET_INFORMATION.BytesRead = Length;
            return NDIS_STATUS_SUCCESS;

        default:
            return NDIS_STATUS_NOT_SUPPORTED;
    }
}

static
NDIS_STATUS
NTAPI
RmNetOidRequest(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _Inout_ PNDIS_OID_REQUEST OidRequest)
{
    PRMNET_ADAPTER Adapter = MiniportAdapterContext;

    switch (OidRequest->RequestType)
    {
        case NdisRequestQueryInformation:
        case NdisRequestQueryStatistics:
            return RmNetQuery(Adapter, OidRequest);

        case NdisRequestSetInformation:
            return RmNetSet(Adapter, OidRequest);

        default:
            return NDIS_STATUS_NOT_SUPPORTED;
    }
}

static
VOID
NTAPI
RmNetCancelOidRequest(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PVOID RequestId)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(RequestId);
}

/* Every frame is copied out before the list goes back, so sends complete at once */
static
VOID
NTAPI
RmNetSendNetBufferLists(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNET_BUFFER_LIST NetBufferLists,
    _In_ NDIS_PORT_NUMBER PortNumber,
    _In_ ULONG SendFlags)
{
    PRMNET_ADAPTER Adapter = MiniportAdapterContext;
    PNET_BUFFER_LIST List;
    PNET_BUFFER Buffer;
    UCHAR Storage[RMNET_FRAME_MAX];
    PUCHAR Frame;
    ULONG Length;
    NDIS_STATUS Status;

    UNREFERENCED_PARAMETER(PortNumber);

    Status = Adapter->Paused ? NDIS_STATUS_PAUSED : NDIS_STATUS_SUCCESS;

    for (List = NetBufferLists; List; List = NET_BUFFER_LIST_NEXT_NBL(List))
    {
        NET_BUFFER_LIST_STATUS(List) = Status;
        if (Status != NDIS_STATUS_SUCCESS)
            continue;

        for (Buffer = NET_BUFFER_LIST_FIRST_NB(List); Buffer; Buffer = NET_BUFFER_NEXT_NB(Buffer))
        {
            Length = NET_BUFFER_DATA_LENGTH(Buffer);
            if (Length < RMNET_ETH_HEADER || Length > sizeof(Storage))
            {
                Adapter->DroppedPackets++;
                continue;
            }

            Frame = NdisGetDataBuffer(Buffer, Length, Storage, 1, 0);
            if (Frame)
                RmNetSendFrame(Adapter, Frame, Length);
        }
    }

    NdisMSendNetBufferListsComplete(Adapter->MiniportHandle,
                                    NetBufferLists,
                                    (SendFlags & NDIS_SEND_FLAGS_DISPATCH_LEVEL) ?
                                        NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL : 0);
}

static
VOID
NTAPI
RmNetCancelSend(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PVOID CancelId)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(CancelId);
}

/* Received frames are ours, each with its own copy of the data */
static
VOID
NTAPI
RmNetReturnNetBufferLists(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNET_BUFFER_LIST NetBufferLists,
    _In_ ULONG ReturnFlags)
{
    PNET_BUFFER_LIST List, Next;
    PMDL Mdl;
    PVOID Data;

    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(ReturnFlags);

    for (List = NetBufferLists; List; List = Next)
    {
        Next = NET_BUFFER_LIST_NEXT_NBL(List);
        NET_BUFFER_LIST_NEXT_NBL(List) = NULL;

        Mdl = NET_BUFFER_FIRST_MDL(NET_BUFFER_LIST_FIRST_NB(List));
        Data = MmGetSystemAddressForMdlSafe(Mdl, NormalPagePriority);
        NdisFreeNetBufferList(List);
        NdisFreeMdl(Mdl);
        if (Data)
            ExFreePoolWithTag(Data, RMNET_TAG);
    }
}

static
VOID
NTAPI
RmNetDevicePnPEvent(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNET_DEVICE_PNP_EVENT NetDevicePnPEvent)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(NetDevicePnPEvent);
}

static
VOID
NTAPI
RmNetShutdown(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ NDIS_SHUTDOWN_ACTION ShutdownAction)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(ShutdownAction);
}

static
VOID
NTAPI
RmNetUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);

    if (RmNetDriverHandle)
        NdisMDeregisterMiniportDriver(RmNetDriverHandle);
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    NDIS_MINIPORT_DRIVER_CHARACTERISTICS Characteristics;
    NDIS_STATUS Status;

    NdisZeroMemory(&Characteristics, sizeof(Characteristics));
    Characteristics.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_DRIVER_CHARACTERISTICS;
    Characteristics.Header.Revision = NDIS_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_2;
    Characteristics.Header.Size = NDIS_SIZEOF_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_2;
    Characteristics.MajorNdisVersion = 6;
    Characteristics.MinorNdisVersion = 20;
    Characteristics.MajorDriverVersion = 1;
    Characteristics.MinorDriverVersion = 0;
    Characteristics.InitializeHandlerEx = RmNetInitialize;
    Characteristics.HaltHandlerEx = RmNetHalt;
    Characteristics.UnloadHandler = RmNetUnload;
    Characteristics.PauseHandler = RmNetPause;
    Characteristics.RestartHandler = RmNetRestart;
    Characteristics.OidRequestHandler = RmNetOidRequest;
    Characteristics.SendNetBufferListsHandler = RmNetSendNetBufferLists;
    Characteristics.ReturnNetBufferListsHandler = RmNetReturnNetBufferLists;
    Characteristics.CancelSendHandler = RmNetCancelSend;
    Characteristics.DevicePnPEventNotifyHandler = RmNetDevicePnPEvent;
    Characteristics.ShutdownHandlerEx = RmNetShutdown;
    Characteristics.CancelOidRequestHandler = RmNetCancelOidRequest;

    Status = NdisMRegisterMiniportDriver(DriverObject,
                                         RegistryPath,
                                         NULL,
                                         &Characteristics,
                                         &RmNetDriverHandle);
    if (Status != NDIS_STATUS_SUCCESS)
        DPRINT1("rmnet: NdisMRegisterMiniportDriver failed 0x%lx\n", Status);

    return (Status == NDIS_STATUS_SUCCESS) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}
