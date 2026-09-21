/*
 * PROJECT:     ReactOS NDIS library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NDIS 5 protocols on top of the NET_BUFFER_LIST core
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "ndissys.h"
#include "xlate.h"

/* Packets handed to NDIS 5 protocols per call */
#define PRO5_MAX_PACKETS 32

/* An NDIS_REQUEST from an NDIS 5 protocol, carried as an OID request */
typedef struct _PRO5_REQUEST
{
    CORE_OID_REQUEST Core;
    PADAPTER_BINDING Binding;
    PNDIS_REQUEST LegacyRequest;
} PRO5_REQUEST, *PPRO5_REQUEST;

/* Send */

static
VOID
Pro5CompletePacket(
    _In_ PADAPTER_BINDING Binding,
    _In_ PNDIS_PACKET Packet,
    _In_ NDIS_STATUS Status)
{
    KIRQL OldIrql;

    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    Binding->ProtocolBinding->Chars.SendCompleteHandler(Binding->NdisOpenBlock.ProtocolBindingContext,
                                                        Packet,
                                                        Status);
    KeLowerIrql(OldIrql);
}

static
VOID
Pro5SendPacketArray(
    _In_ PADAPTER_BINDING Binding,
    _In_reads_(PacketCount) PPNDIS_PACKET Packets,
    _In_ UINT PacketCount)
{
    PLOGICAL_ADAPTER Adapter = Binding->Adapter;
    NDIS_SEND_XLATE Xlate;
    BOOLEAN More;
    UINT i;

    RtlZeroMemory(&Xlate, sizeof(Xlate));
    Xlate.NblPool = Adapter->Core.SendNblPool;
    Xlate.Owner = Binding;
    Xlate.Packets = Packets;
    Xlate.PacketCount = PacketCount;

    for (i = 0; i < PacketCount; i++)
        Packets[i]->Reserved[1] = (ULONG_PTR)Binding;

    do
    {
        More = NdisXlatePacketArray(&Xlate);

        if (Xlate.NetBufferLists == NULL)
        {
            /* Out of NET_BUFFER_LISTs: whatever is left goes back failed */
            for (i = Xlate.Translated; i < PacketCount; i++)
                Pro5CompletePacket(Binding, Packets[i], NDIS_STATUS_RESOURCES);
            return;
        }

        CoreSendNetBufferLists(Adapter, Xlate.NetBufferLists, NDIS_DEFAULT_PORT_NUMBER, 0);
    }
    while (More);
}

/**
 * @brief
 * NdisSend from an NDIS 5 protocol.
 *
 * @param[in] MacBindingHandle
 * The binding.
 *
 * @param[in] Packet
 * The packet to send.
 *
 * @return
 * NDIS_STATUS_PENDING. The protocol always hears back through its send
 * complete handler.
 */
NDIS_STATUS
NTAPI
ProSend(
    _In_ NDIS_HANDLE MacBindingHandle,
    _In_ PNDIS_PACKET Packet)
{
    Pro5SendPacketArray(GET_ADAPTER_BINDING(MacBindingHandle), &Packet, 1);
    return NDIS_STATUS_PENDING;
}

/**
 * @brief
 * NdisSendPackets from an NDIS 5 protocol.
 *
 * @param[in] NdisBindingHandle
 * The binding.
 *
 * @param[in] PacketArray
 * The packets to send.
 *
 * @param[in] NumberOfPackets
 * How many there are.
 */
VOID
NTAPI
ProSendPackets(
    _In_ NDIS_HANDLE NdisBindingHandle,
    _In_reads_(NumberOfPackets) PPNDIS_PACKET PacketArray,
    _In_ UINT NumberOfPackets)
{
    if (NumberOfPackets != 0)
        Pro5SendPacketArray(GET_ADAPTER_BINDING(NdisBindingHandle), PacketArray, NumberOfPackets);
}

/**
 * @brief
 * Hands finished sends back to the NDIS 5 protocols that made them.
 *
 * @param[in] Adapter
 * The adapter they went out on.
 *
 * @param[in] NetBufferLists
 * Lists built from protocol packets, each carrying its status.
 */
VOID
NTAPI
Pro5SendComplete(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PNET_BUFFER_LIST NetBufferLists)
{
    PNET_BUFFER_LIST NetBufferList;
    PNET_BUFFER_LIST Next;
    PADAPTER_BINDING Binding;
    PNDIS_PACKET Packet;
    NDIS_STATUS Status;

    UNREFERENCED_PARAMETER(Adapter);

    for (NetBufferList = NetBufferLists; NetBufferList != NULL; NetBufferList = Next)
    {
        Next = NET_BUFFER_LIST_NEXT_NBL(NetBufferList);
        NET_BUFFER_LIST_NEXT_NBL(NetBufferList) = NULL;

        Packet = NDIS_XLATE_PACKET(NetBufferList);
        Binding = (PADAPTER_BINDING)NetBufferList->SourceHandle;
        Status = NET_BUFFER_LIST_STATUS(NetBufferList);

        NdisXlateFreeNetBufferLists(NetBufferList);

        Pro5CompletePacket(Binding, Packet, Status);
    }
}

/* Receive */

static
UINT
Pro5CountBindings(
    _In_ PLOGICAL_ADAPTER Adapter)
{
    PLIST_ENTRY Entry;
    UINT Count = 0;

    KeAcquireSpinLockAtDpcLevel(&Adapter->NdisMiniportBlock.Lock);
    for (Entry = Adapter->ProtocolListHead.Flink; Entry != &Adapter->ProtocolListHead; Entry = Entry->Flink)
        Count++;
    KeReleaseSpinLockFromDpcLevel(&Adapter->NdisMiniportBlock.Lock);

    return Count;
}

/*
 * A list the NDIS 5 miniport shim built around one of the miniport's own
 * packets goes up as that packet. Anything else gets a packet laid over it.
 */
static
PNDIS_PACKET
Pro5PacketFromNetBufferList(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PNET_BUFFER_LIST NetBufferList)
{
    PNDIS_PACKET Packet;

    if (NetBufferList->NdisPoolHandle == Adapter->Core.ReceiveNblPool)
    {
        Packet = CORE_NBL_FROM_PACKET(NetBufferList);
        if (Packet != NULL)
            return Packet;
    }

    Packet = CorePacketFromNetBuffer(Adapter->Core.ReceivePacketPool,
                                     NetBufferList,
                                     NET_BUFFER_LIST_FIRST_NB(NetBufferList));
    if (Packet != NULL)
        NDIS_SET_PACKET_HEADER_SIZE(Packet, Adapter->MediumHeaderSize);

    return Packet;
}

static
BOOLEAN
Pro5IsCorePacket(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PNDIS_PACKET Packet)
{
    return Packet->Private.Pool == (PNDIS_PACKET_POOL)Adapter->Core.ReceivePacketPool;
}

/*
 * A protocol that cannot take a packet, or a packet it must not keep, gets the
 * frame as a header and a lookahead buffer holding the rest of it.
 */
static
VOID
Pro5IndicateLookahead(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PADAPTER_BINDING Binding,
    _In_ PNDIS_PACKET Packet)
{
    UINT HeaderSize = NDIS_GET_PACKET_HEADER_SIZE(Packet);
    UINT TotalLength;
    PUCHAR Frame;

    if (HeaderSize == 0)
        HeaderSize = Adapter->MediumHeaderSize;

    NdisQueryPacket(Packet, NULL, NULL, NULL, &TotalLength);
    if (TotalLength < HeaderSize)
        return;

    Frame = ExAllocatePoolWithTag(NonPagedPool, TotalLength, NDIS_TAG);
    if (Frame == NULL)
    {
        NDIS_DbgPrint(MIN_TRACE, ("No memory for a lookahead copy.\n"));
        return;
    }

    CopyPacketToBuffer(Frame, Packet, 0, TotalLength);

    /* NdisTransferData copies from here while the indication is in progress */
    Adapter->NdisMiniportBlock.IndicatedPacket[KeGetCurrentProcessorIndex()] = Packet;

    Binding->ProtocolBinding->Chars.ReceiveHandler(Binding->NdisOpenBlock.ProtocolBindingContext,
                                                   (NDIS_HANDLE)Packet,
                                                   Frame,
                                                   HeaderSize,
                                                   Frame + HeaderSize,
                                                   TotalLength - HeaderSize,
                                                   TotalLength - HeaderSize);

    Adapter->NdisMiniportBlock.IndicatedPacket[KeGetCurrentProcessorIndex()] = NULL;

    ExFreePoolWithTag(Frame, NDIS_TAG);
}

static
VOID
Pro5DeliverPackets(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_reads_(Count) PPNDIS_PACKET Packets,
    _In_ UINT Count)
{
    PADAPTER_BINDING Binding;
    PLIST_ENTRY Entry;
    PNDIS_PACKET Packet;
    UINT i;

    KeAcquireSpinLockAtDpcLevel(&Adapter->NdisMiniportBlock.Lock);

    for (Entry = Adapter->ProtocolListHead.Flink; Entry != &Adapter->ProtocolListHead; Entry = Entry->Flink)
    {
        Binding = CONTAINING_RECORD(Entry, ADAPTER_BINDING, AdapterListEntry);

        for (i = 0; i < Count; i++)
        {
            Packet = Packets[i];

            if (Binding->ProtocolBinding->Chars.ReceivePacketHandler != NULL &&
                NDIS_GET_PACKET_STATUS(Packet) != NDIS_STATUS_RESOURCES)
            {
                Packet->WrapperReserved[0] += Binding->ProtocolBinding->Chars.ReceivePacketHandler(
                    Binding->NdisOpenBlock.ProtocolBindingContext,
                    Packet);
            }
            else if (Binding->ProtocolBinding->Chars.ReceiveHandler != NULL)
            {
                Pro5IndicateLookahead(Adapter, Binding, Packet);
            }
        }

        if (Binding->ProtocolBinding->Chars.ReceiveCompleteHandler != NULL)
            Binding->ProtocolBinding->Chars.ReceiveCompleteHandler(Binding->NdisOpenBlock.ProtocolBindingContext);
    }

    KeReleaseSpinLockFromDpcLevel(&Adapter->NdisMiniportBlock.Lock);
}

/**
 * @brief
 * Indicates received NET_BUFFER_LISTs to every NDIS 5 protocol bound to an
 * adapter, as packets.
 *
 * @param[in] Adapter
 * The receiving adapter.
 *
 * @param[in] NetBufferLists
 * The received chain.
 *
 * @param[in] ReceiveFlags
 * NDIS_RECEIVE_FLAGS_*. The caller is at DISPATCH_LEVEL.
 *
 * @param[out] Unheld
 * The lists no protocol kept. The rest come back through NdisReturnPackets.
 */
VOID
NTAPI
Pro5IndicateReceive(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PNET_BUFFER_LIST NetBufferLists,
    _In_ ULONG ReceiveFlags,
    _Out_ PNET_BUFFER_LIST *Unheld)
{
    PNDIS_PACKET Packets[PRO5_MAX_PACKETS];
    PNET_BUFFER_LIST Lists[PRO5_MAX_PACKETS];
    PNET_BUFFER_LIST NetBufferList = NetBufferLists;
    PNET_BUFFER_LIST Next;
    PNET_BUFFER_LIST Head = NULL;
    PNET_BUFFER_LIST Tail = NULL;
    PNDIS_PACKET Packet;
    NDIS_STATUS PacketStatus;
    UINT Count;
    UINT i;

    /* With more than one protocol listening, or a borrowed frame, each one copies */
    if ((ReceiveFlags & NDIS_RECEIVE_FLAGS_RESOURCES) || Pro5CountBindings(Adapter) != 1)
        PacketStatus = NDIS_STATUS_RESOURCES;
    else
        PacketStatus = NDIS_STATUS_SUCCESS;

    while (NetBufferList != NULL)
    {
        Count = 0;

        while (NetBufferList != NULL && Count < PRO5_MAX_PACKETS)
        {
            Next = NET_BUFFER_LIST_NEXT_NBL(NetBufferList);
            NET_BUFFER_LIST_NEXT_NBL(NetBufferList) = NULL;

            Packet = Pro5PacketFromNetBufferList(Adapter, NetBufferList);
            if (Packet == NULL)
            {
                if (Tail == NULL)
                    Head = NetBufferList;
                else
                    NET_BUFFER_LIST_NEXT_NBL(Tail) = NetBufferList;
                Tail = NetBufferList;
            }
            else
            {
                Packet->Reserved[1] = (ULONG_PTR)Adapter;
                Packet->WrapperReserved[0] = 0;
                NDIS_SET_PACKET_STATUS(Packet, PacketStatus);

                Packets[Count] = Packet;
                Lists[Count] = NetBufferList;
                Count++;
            }

            NetBufferList = Next;
        }

        if (Count == 0)
            continue;

        Pro5DeliverPackets(Adapter, Packets, Count);

        for (i = 0; i < Count; i++)
        {
            Packet = Packets[i];

            if (Packet->WrapperReserved[0] != 0)
            {
                /* Held: NdisReturnPackets finds the list again from the packet */
                if (!Pro5IsCorePacket(Adapter, Packet))
                    CORE_PACKET_OWNER_NBL(Packet) = Lists[i];
                continue;
            }

            if (Pro5IsCorePacket(Adapter, Packet))
                CoreFreeNetBufferPacket(Packet);

            if (Tail == NULL)
                Head = Lists[i];
            else
                NET_BUFFER_LIST_NEXT_NBL(Tail) = Lists[i];
            Tail = Lists[i];
        }
    }

    *Unheld = Head;
}

/**
 * @brief
 * An NDIS 5 protocol is done with packets it kept from a receive indication.
 *
 * @param[in] PacketsToReturn
 * The packets.
 *
 * @param[in] NumberOfPackets
 * How many there are.
 */
VOID
EXPORT
NdisReturnPackets(
    IN  PNDIS_PACKET    *PacketsToReturn,
    IN  UINT            NumberOfPackets)
{
    PLOGICAL_ADAPTER Adapter;
    PNET_BUFFER_LIST NetBufferList;
    PNDIS_PACKET Packet;
    UINT i;

    for (i = 0; i < NumberOfPackets; i++)
    {
        Packet = PacketsToReturn[i];

        if (--Packet->WrapperReserved[0] != 0)
            continue;

        Adapter = (PLOGICAL_ADAPTER)Packet->Reserved[1];

        if (Pro5IsCorePacket(Adapter, Packet))
        {
            NetBufferList = CORE_PACKET_STATE(Packet)->NetBufferList;
            CoreFreeNetBufferPacket(Packet);
        }
        else
        {
            NetBufferList = CORE_PACKET_OWNER_NBL(Packet);
        }

        CoreReturnNetBufferList(Adapter, NetBufferList);
    }
}

/**
 * @brief
 * NdisTransferData from an NDIS 5 protocol. The whole frame is always in the
 * packet being indicated, so this copies from it rather than asking the
 * miniport.
 *
 * @param[in] MacBindingHandle
 * The binding.
 *
 * @param[in] MacReceiveContext
 * The indication's receive context.
 *
 * @param[in] ByteOffset
 * Offset past the header to start from.
 *
 * @param[in] BytesToTransfer
 * How much to copy.
 *
 * @param[in,out] Packet
 * The protocol's packet to copy into.
 *
 * @param[out] BytesTransferred
 * How much was copied.
 *
 * @return
 * NDIS_STATUS_SUCCESS, or NDIS_STATUS_FAILURE outside an indication.
 */
NDIS_STATUS
NTAPI
ProTransferData(
    _In_ NDIS_HANDLE MacBindingHandle,
    _In_ NDIS_HANDLE MacReceiveContext,
    _In_ UINT ByteOffset,
    _In_ UINT BytesToTransfer,
    _Inout_ PNDIS_PACKET Packet,
    _Out_ PUINT BytesTransferred)
{
    PLOGICAL_ADAPTER Adapter = GET_ADAPTER_BINDING(MacBindingHandle)->Adapter;
    PNDIS_PACKET Indicated;
    UINT HeaderSize;

    UNREFERENCED_PARAMETER(MacReceiveContext);

    *BytesTransferred = 0;

    Indicated = Adapter->NdisMiniportBlock.IndicatedPacket[KeGetCurrentProcessorIndex()];
    if (Indicated == NULL)
        return NDIS_STATUS_FAILURE;

    HeaderSize = NDIS_GET_PACKET_HEADER_SIZE(Indicated);
    if (HeaderSize == 0)
        HeaderSize = Adapter->MediumHeaderSize;

    NdisCopyFromPacketToPacket(Packet,
                               0,
                               BytesToTransfer,
                               Indicated,
                               HeaderSize + ByteOffset,
                               BytesTransferred);
    return NDIS_STATUS_SUCCESS;
}

/* Requests */

static
VOID
Pro5CopyRequestBack(
    _In_ PPRO5_REQUEST Request)
{
    PNDIS_OID_REQUEST OidRequest = &Request->Core.Request;
    PNDIS_REQUEST LegacyRequest = Request->LegacyRequest;

    if (OidRequest->RequestType == NdisRequestSetInformation)
    {
        LegacyRequest->DATA.SET_INFORMATION.BytesRead = OidRequest->DATA.SET_INFORMATION.BytesRead;
        LegacyRequest->DATA.SET_INFORMATION.BytesNeeded = OidRequest->DATA.SET_INFORMATION.BytesNeeded;
    }
    else
    {
        LegacyRequest->DATA.QUERY_INFORMATION.BytesWritten = OidRequest->DATA.QUERY_INFORMATION.BytesWritten;
        LegacyRequest->DATA.QUERY_INFORMATION.BytesNeeded = OidRequest->DATA.QUERY_INFORMATION.BytesNeeded;
    }
}

static
VOID
NTAPI
Pro5RequestComplete(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PCORE_OID_REQUEST CoreRequest,
    _In_ NDIS_STATUS Status)
{
    PPRO5_REQUEST Request = CONTAINING_RECORD(CoreRequest, PRO5_REQUEST, Core);
    PADAPTER_BINDING Binding = Request->Binding;
    PNDIS_REQUEST LegacyRequest = Request->LegacyRequest;

    UNREFERENCED_PARAMETER(Adapter);

    Pro5CopyRequestBack(Request);
    ExFreePoolWithTag(Request, NDIS_TAG);

    if (Binding->ProtocolBinding->Chars.RequestCompleteHandler != NULL)
    {
        Binding->ProtocolBinding->Chars.RequestCompleteHandler(Binding->NdisOpenBlock.ProtocolBindingContext,
                                                               LegacyRequest,
                                                               Status);
    }
}

/**
 * @brief
 * NdisRequest from an NDIS 5 protocol, carried to the miniport as an OID
 * request.
 *
 * @param[in] MacBindingHandle
 * The binding.
 *
 * @param[in] NdisRequest
 * The query or set.
 *
 * @return
 * The request's status, or NDIS_STATUS_PENDING.
 */
NDIS_STATUS
NTAPI
ProRequest(
    _In_ NDIS_HANDLE MacBindingHandle,
    _In_ PNDIS_REQUEST NdisRequest)
{
    PADAPTER_BINDING Binding = GET_ADAPTER_BINDING(MacBindingHandle);
    PNDIS_OID_REQUEST OidRequest;
    PPRO5_REQUEST Request;
    NDIS_STATUS Status;

    if (NdisRequest->RequestType != NdisRequestQueryInformation &&
        NdisRequest->RequestType != NdisRequestSetInformation &&
        NdisRequest->RequestType != NdisRequestQueryStatistics)
    {
        return NDIS_STATUS_NOT_SUPPORTED;
    }

    Request = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Request), NDIS_TAG);
    if (Request == NULL)
        return NDIS_STATUS_RESOURCES;

    RtlZeroMemory(Request, sizeof(*Request));
    Request->Binding = Binding;
    Request->LegacyRequest = NdisRequest;
    Request->Core.Completion = Pro5RequestComplete;

    OidRequest = &Request->Core.Request;
    OidRequest->Header.Type = NDIS_OBJECT_TYPE_OID_REQUEST;
    OidRequest->Header.Revision = NDIS_OID_REQUEST_REVISION_1;
    OidRequest->Header.Size = NDIS_SIZEOF_OID_REQUEST_REVISION_1;
    OidRequest->RequestType = NdisRequest->RequestType;
    OidRequest->PortNumber = NDIS_DEFAULT_PORT_NUMBER;

    /* Query and set share their first three fields */
    OidRequest->DATA.QUERY_INFORMATION.Oid = NdisRequest->DATA.QUERY_INFORMATION.Oid;
    OidRequest->DATA.QUERY_INFORMATION.InformationBuffer = NdisRequest->DATA.QUERY_INFORMATION.InformationBuffer;
    OidRequest->DATA.QUERY_INFORMATION.InformationBufferLength =
        NdisRequest->DATA.QUERY_INFORMATION.InformationBufferLength;

    Status = CoreOidRequest(Binding->Adapter, &Request->Core);
    if (Status != NDIS_STATUS_PENDING)
    {
        Pro5CopyRequestBack(Request);
        ExFreePoolWithTag(Request, NDIS_TAG);
    }

    return Status;
}

/* Status */

/**
 * @brief
 * Indicates an NDIS 5 status to every protocol bound to an adapter.
 *
 * @param[in] Adapter
 * The adapter the status is about.
 *
 * @param[in] GeneralStatus
 * The NDIS 5 status code.
 *
 * @param[in] StatusBuffer
 * Status specific data.
 *
 * @param[in] StatusBufferSize
 * Its size.
 */
VOID
NTAPI
Pro5IndicateStatus(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ NDIS_STATUS GeneralStatus,
    _In_reads_bytes_opt_(StatusBufferSize) PVOID StatusBuffer,
    _In_ UINT StatusBufferSize)
{
    PADAPTER_BINDING Binding;
    PLIST_ENTRY Entry;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Adapter->NdisMiniportBlock.Lock, &OldIrql);

    for (Entry = Adapter->ProtocolListHead.Flink; Entry != &Adapter->ProtocolListHead; Entry = Entry->Flink)
    {
        Binding = CONTAINING_RECORD(Entry, ADAPTER_BINDING, AdapterListEntry);

        Binding->ProtocolBinding->Chars.StatusHandler(Binding->NdisOpenBlock.ProtocolBindingContext,
                                                      GeneralStatus,
                                                      StatusBuffer,
                                                      StatusBufferSize);

        Binding->ProtocolBinding->Chars.StatusCompleteHandler(Binding->NdisOpenBlock.ProtocolBindingContext);
    }

    KeReleaseSpinLock(&Adapter->NdisMiniportBlock.Lock, OldIrql);
}
