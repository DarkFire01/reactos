/*
 * PROJECT:     ReactOS WDI upper edge
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The data path API the WLAN miniport calls up into
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * Nothing is sent or received above the upper edge yet. The miniport can
 * still allocate frame metadata and indicate frames and peers once its data
 * path starts, so frames are pulled and handed straight back.
 */

#include "wdiwifi.h"

#define NDEBUG
#include <debug.h>

/* Frame metadata */

_Use_decl_annotations_
NDIS_STATUS
NTAPI
WdiCreateFrameLookaside(
    PWDI_ADAPTER Adapter)
{
    ULONG Size;

    Size = FIELD_OFFSET(WDI_FRAME, Metadata) + sizeof(WDI_FRAME_METADATA) + Adapter->FrameExtraSpace;
    Size = ALIGN_UP_BY(Size, 16);
    if (Adapter->FrameExtraSpace > MAXUSHORT || Size > MAXUSHORT)
    {
        DPRINT1("Frame metadata needs %lu extra bytes\n", Adapter->FrameExtraSpace);
        return STATUS_UNSUCCESSFUL;
    }

    Adapter->FrameSize = (USHORT)Size;
    ExInitializeNPagedLookasideList(&Adapter->FrameLookaside, NULL, NULL, 0, Size, WDI_TAG, 0);
    Adapter->FrameLookasideReady = TRUE;
    return NDIS_STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID
NTAPI
WdiDeleteFrameLookaside(
    PWDI_ADAPTER Adapter)
{
    if (!Adapter->FrameLookasideReady)
        return;

    ExDeleteNPagedLookasideList(&Adapter->FrameLookaside);
    Adapter->FrameLookasideReady = FALSE;
}

static
PWDI_FRAME_METADATA
NTAPI
WdiAllocateFrameMetadata(
    _In_ NDIS_HANDLE NdisMiniportDataPathHandle)
{
    PWDI_ADAPTER Adapter = NdisMiniportDataPathHandle;
    PWDI_FRAME Frame;

    Frame = ExAllocateFromNPagedLookasideList(&Adapter->FrameLookaside);
    if (Frame == NULL)
    {
        DPRINT1("Out of frame metadata\n");
        return NULL;
    }

    RtlZeroMemory(Frame, Adapter->FrameSize);
    Frame->Size = Adapter->FrameSize;
    return &Frame->Metadata;
}

static
VOID
NTAPI
WdiFreeFrameMetadata(
    _In_ NDIS_HANDLE NdisMiniportDataPathHandle,
    _In_ PWDI_FRAME_METADATA WiFiFrameMetaData)
{
    PWDI_ADAPTER Adapter = NdisMiniportDataPathHandle;

    if (WiFiFrameMetaData == NULL)
        return;

    ExFreeToNPagedLookasideList(&Adapter->FrameLookaside,
                                CONTAINING_RECORD(WiFiFrameMetaData, WDI_FRAME, Metadata));
}

/* Send queue; frames wait here until the miniport pulls them with TxDequeue */

VOID
NTAPI
WdiInitializeSendQueue(
    _In_ PWDI_ADAPTER Adapter)
{
    KeInitializeSpinLock(&Adapter->TxLock);
    InitializeListHead(&Adapter->TxQueue);
    Adapter->TxQueued = 0;
    Adapter->TxNextId = 0;
    RtlZeroMemory(Adapter->TxOutstanding, sizeof(Adapter->TxOutstanding));
}

static
PWDI_FRAME_METADATA
NTAPI
WdiAllocateSendFrame(
    _In_ PWDI_ADAPTER Adapter,
    _In_ PNET_BUFFER_LIST NetBufferList)
{
    PWDI_FRAME Frame;
    PWDI_FRAME_METADATA Metadata;

    Frame = ExAllocateFromNPagedLookasideList(&Adapter->FrameLookaside);
    if (Frame == NULL)
        return NULL;

    RtlZeroMemory(Frame, Adapter->FrameSize);
    Frame->Size = Adapter->FrameSize;
    Metadata = &Frame->Metadata;
    Metadata->pNBL = NetBufferList;
    return Metadata;
}

static
VOID
NTAPI
WdiFreeSendFrame(
    _In_ PWDI_ADAPTER Adapter,
    _In_ PWDI_FRAME_METADATA Metadata)
{
    ExFreeToNPagedLookasideList(&Adapter->FrameLookaside,
                                CONTAINING_RECORD(Metadata, WDI_FRAME, Metadata));
}

/* The EtherType a converted frame carries sits in the SNAP right after the
   24 byte 802.11 header this edge builds */
static
UINT16
NTAPI
WdiFrameEthertype(
    _In_ PNET_BUFFER_LIST NetBufferList)
{
    PNET_BUFFER NetBuffer = NET_BUFFER_LIST_FIRST_NB(NetBufferList);
    UCHAR Storage[32];
    PUCHAR Data;

    if (NetBuffer == NULL)
        return 0;

    Data = NdisGetDataBuffer(NetBuffer, sizeof(Storage), Storage, 1, 0);
    if (Data == NULL)
        return 0;

    return (UINT16)((Data[30] << 8) | Data[31]);
}

static
VOID
NTAPI
WdiCompleteSend(
    _In_ PWDI_ADAPTER Adapter,
    _In_ PNET_BUFFER_LIST NetBufferList,
    _In_ NDIS_STATUS Status)
{
    NET_BUFFER_LIST_STATUS(NetBufferList) = Status;
    NET_BUFFER_LIST_NEXT_NBL(NetBufferList) = NULL;
    NdisMSendNetBufferListsComplete(Adapter->MiniportAdapterHandle,
                                    NetBufferList,
                                    0);
}

/**
 * @brief
 * Takes the sends NDIS handed the miniport, gives each one a frame id and
 * queues it, then tells the miniport frames are waiting.
 */
VOID
NTAPI
WdiQueueSend(
    _In_ PWDI_ADAPTER Adapter,
    _In_ PNET_BUFFER_LIST NetBufferLists,
    _In_ ULONG SendFlags)
{
    PNET_BUFFER_LIST NetBufferList;
    PNET_BUFFER_LIST Next;
    PWDI_FRAME_METADATA Metadata;
    KIRQL OldIrql;
    ULONG Queued = 0;
    ULONG Id;

    UNREFERENCED_PARAMETER(SendFlags);

    for (NetBufferList = NetBufferLists; NetBufferList != NULL; NetBufferList = Next)
    {
        Next = NET_BUFFER_LIST_NEXT_NBL(NetBufferList);
        NET_BUFFER_LIST_NEXT_NBL(NetBufferList) = NULL;

        Metadata = WdiAllocateSendFrame(Adapter, NetBufferList);
        if (Metadata == NULL)
        {
            WdiCompleteSend(Adapter, NetBufferList, NDIS_STATUS_RESOURCES);
            continue;
        }

        Metadata->u.txMetaData.PortID = Adapter->Ports[0].PortId;
        Metadata->u.txMetaData.PeerID = 0;
        Metadata->u.txMetaData.ExTID = 0;
        Metadata->u.txMetaData.IsUnicast = TRUE;
        Metadata->u.txMetaData.Ethertype = WdiFrameEthertype(NetBufferList);
        Metadata->u.txMetaData.bTxCompleteRequired = TRUE;
        NET_BUFFER_LIST_MINIPORT_RESERVED(NetBufferList)[0] = Metadata;

        KeAcquireSpinLock(&Adapter->TxLock, &OldIrql);

        for (Id = 0; Id < WDI_MAX_TX_FRAMES; Id++)
        {
            ULONG Slot = (Adapter->TxNextId + Id) % WDI_MAX_TX_FRAMES;
            if (Adapter->TxOutstanding[Slot] == NULL)
            {
                Adapter->TxOutstanding[Slot] = Metadata;
                Metadata->FrameID = (WDI_FRAME_ID)Slot;
                Adapter->TxNextId = (Slot + 1) % WDI_MAX_TX_FRAMES;
                break;
            }
        }

        if (Id == WDI_MAX_TX_FRAMES)
        {
            KeReleaseSpinLock(&Adapter->TxLock, OldIrql);
            WdiFreeSendFrame(Adapter, Metadata);
            WdiCompleteSend(Adapter, NetBufferList, NDIS_STATUS_RESOURCES);
            continue;
        }

        InsertTailList(&Adapter->TxQueue, &Metadata->Linkage);
        Adapter->TxQueued++;
        Queued++;

        KeReleaseSpinLock(&Adapter->TxLock, OldIrql);
    }

    if (Queued != 0 && Adapter->DataHandlers.TxDataSendHandler != NULL)
    {
        Adapter->DataHandlers.TxDataSendHandler(Adapter->TalTxRx,
                                                Adapter->Ports[0].PortId,
                                                0,
                                                0,
                                                (UINT16)Queued,
                                                Adapter->TxQueued,
                                                FALSE);
    }
}

/**
 * @brief
 * Fails and completes every queued and outstanding send. Used when the data
 * path stops.
 */
VOID
NTAPI
WdiFlushSends(
    _In_ PWDI_ADAPTER Adapter,
    _In_ NDIS_STATUS Status)
{
    PWDI_FRAME_METADATA Metadata;
    PLIST_ENTRY Entry;
    KIRQL OldIrql;
    ULONG Id;

    KeAcquireSpinLock(&Adapter->TxLock, &OldIrql);

    while (!IsListEmpty(&Adapter->TxQueue))
    {
        Entry = RemoveHeadList(&Adapter->TxQueue);
        Metadata = CONTAINING_RECORD(Entry, WDI_FRAME_METADATA, Linkage);
        Adapter->TxOutstanding[Metadata->FrameID] = NULL;
        Adapter->TxQueued--;

        KeReleaseSpinLock(&Adapter->TxLock, OldIrql);
        WdiCompleteSend(Adapter, Metadata->pNBL, Status);
        WdiFreeSendFrame(Adapter, Metadata);
        KeAcquireSpinLock(&Adapter->TxLock, &OldIrql);
    }

    for (Id = 0; Id < WDI_MAX_TX_FRAMES; Id++)
    {
        Metadata = Adapter->TxOutstanding[Id];
        if (Metadata == NULL)
            continue;

        Adapter->TxOutstanding[Id] = NULL;
        KeReleaseSpinLock(&Adapter->TxLock, OldIrql);
        WdiCompleteSend(Adapter, Metadata->pNBL, Status);
        WdiFreeSendFrame(Adapter, Metadata);
        KeAcquireSpinLock(&Adapter->TxLock, &OldIrql);
    }

    KeReleaseSpinLock(&Adapter->TxLock, OldIrql);
}

/* Transmit indications from the miniport pulling and finishing frames */

static
VOID
NTAPI
WdiTxDequeue(
    _In_ NDIS_HANDLE NdisMiniportDataPathHandle,
    _In_ UINT32 Quantum,
    _In_ UINT8 MaxNumFrames,
    _In_ UINT16 Credit,
    _Out_ PNET_BUFFER_LIST *NetBufferList)
{
    PWDI_ADAPTER Adapter = NdisMiniportDataPathHandle;
    PWDI_FRAME_METADATA Metadata;
    PNET_BUFFER_LIST Head = NULL;
    PNET_BUFFER_LIST Tail = NULL;
    PLIST_ENTRY Entry;
    KIRQL OldIrql;
    UINT8 Taken = 0;

    UNREFERENCED_PARAMETER(Quantum);
    UNREFERENCED_PARAMETER(Credit);

    KeAcquireSpinLock(&Adapter->TxLock, &OldIrql);

    while (Taken < MaxNumFrames && !IsListEmpty(&Adapter->TxQueue))
    {
        Entry = RemoveHeadList(&Adapter->TxQueue);
        Metadata = CONTAINING_RECORD(Entry, WDI_FRAME_METADATA, Linkage);
        Adapter->TxQueued--;

        NET_BUFFER_LIST_NEXT_NBL(Metadata->pNBL) = NULL;
        if (Tail == NULL)
            Head = Metadata->pNBL;
        else
            NET_BUFFER_LIST_NEXT_NBL(Tail) = Metadata->pNBL;
        Tail = Metadata->pNBL;
        Taken++;
    }

    KeReleaseSpinLock(&Adapter->TxLock, OldIrql);

    *NetBufferList = Head;
}

static
VOID
NTAPI
WdiTxTransferComplete(
    _In_ NDIS_HANDLE NdisMiniportDataPathHandle,
    _In_ WDI_TX_FRAME_STATUS WifiTxFrameStatus,
    _In_ PNET_BUFFER_LIST NetBufferList)
{
    UNREFERENCED_PARAMETER(NdisMiniportDataPathHandle);
    UNREFERENCED_PARAMETER(WifiTxFrameStatus);
    UNREFERENCED_PARAMETER(NetBufferList);

    /* The payload has been taken by the miniport; the send is completed to
       NDIS only once its frame id comes back through TxSendComplete */
}

static
VOID
NTAPI
WdiTxSendComplete(
    _In_ NDIS_HANDLE NdisMiniportDataPathHandle,
    _In_ WDI_TX_FRAME_STATUS WifiTxFrameStatus,
    _In_ UINT16 NumCompletedSends,
    _In_reads_(NumCompletedSends) WDI_FRAME_ID *WifiTxFrameIdList,
    _In_reads_opt_(NumCompletedSends) WDI_TX_COMPLETE_DATA *WifiTxCompleteList)
{
    PWDI_ADAPTER Adapter = NdisMiniportDataPathHandle;
    PWDI_FRAME_METADATA Metadata;
    NDIS_STATUS Status;
    KIRQL OldIrql;
    UINT16 i;

    UNREFERENCED_PARAMETER(WifiTxCompleteList);

    Status = (WifiTxFrameStatus == WDI_TxFrameStatus_Ok) ?
             NDIS_STATUS_SUCCESS : NDIS_STATUS_FAILURE;

    for (i = 0; i < NumCompletedSends; i++)
    {
        WDI_FRAME_ID FrameId = WifiTxFrameIdList[i];

        if (FrameId >= WDI_MAX_TX_FRAMES)
            continue;

        KeAcquireSpinLock(&Adapter->TxLock, &OldIrql);
        Metadata = Adapter->TxOutstanding[FrameId];
        Adapter->TxOutstanding[FrameId] = NULL;
        KeReleaseSpinLock(&Adapter->TxLock, OldIrql);

        if (Metadata == NULL)
            continue;

        WdiCompleteSend(Adapter, Metadata->pNBL, Status);
        WdiFreeSendFrame(Adapter, Metadata);
    }
}

static
VOID
NTAPI
WdiTxQueryRaTidState(
    _In_ NDIS_HANDLE NdisMiniportDataPathHandle,
    _In_ WDI_PORT_ID PortId,
    _In_ WDI_PEER_ID PeerId,
    _In_ WDI_EXTENDED_TID ExTid,
    _Out_ NDIS_STATUS *WifiStatus,
    _Out_ PUINT16 QueueLength)
{
    UNREFERENCED_PARAMETER(NdisMiniportDataPathHandle);
    UNREFERENCED_PARAMETER(PortId);
    UNREFERENCED_PARAMETER(PeerId);
    UNREFERENCED_PARAMETER(ExTid);

    *WifiStatus = NDIS_STATUS_SUCCESS;
    *QueueLength = 0;
}

static
VOID
NTAPI
WdiTxSendPause(
    _In_ NDIS_HANDLE NdisMiniportDataPathHandle,
    _In_ WDI_PORT_ID PortId,
    _In_ WDI_PEER_ID PeerId,
    _In_ UINT32 ExTidBitmask,
    _In_ WDI_TX_PAUSE_REASON TxPauseReason)
{
    UNREFERENCED_PARAMETER(NdisMiniportDataPathHandle);
    UNREFERENCED_PARAMETER(ExTidBitmask);

    DPRINT("Transmit paused, port %u peer %u reason 0x%x\n", PortId, PeerId, TxPauseReason);
}

static
VOID
NTAPI
WdiTxSendRestart(
    _In_ NDIS_HANDLE NdisMiniportDataPathHandle,
    _In_ WDI_PORT_ID PortId,
    _In_ WDI_PEER_ID PeerId,
    _In_ UINT32 ExTidBitmask,
    _In_ WDI_TX_PAUSE_REASON TxRestartReason)
{
    UNREFERENCED_PARAMETER(NdisMiniportDataPathHandle);
    UNREFERENCED_PARAMETER(ExTidBitmask);

    DPRINT("Transmit restarted, port %u peer %u reason 0x%x\n", PortId, PeerId, TxRestartReason);
}

static
VOID
NTAPI
WdiTxReleaseFrames(
    _In_ NDIS_HANDLE NdisMiniportDataPathHandle,
    _In_ WDI_PORT_ID PortId,
    _In_ WDI_PEER_ID PeerId,
    _In_ UINT32 ExTidBitmask,
    _In_ UINT8 MaxNumFrames,
    _In_ UINT16 Credit,
    _Out_ PNET_BUFFER_LIST *NetBufferList)
{
    UNREFERENCED_PARAMETER(NdisMiniportDataPathHandle);
    UNREFERENCED_PARAMETER(PortId);
    UNREFERENCED_PARAMETER(PeerId);
    UNREFERENCED_PARAMETER(ExTidBitmask);
    UNREFERENCED_PARAMETER(MaxNumFrames);
    UNREFERENCED_PARAMETER(Credit);

    *NetBufferList = NULL;
}

static
VOID
NTAPI
WdiTxInjectFrame(
    _In_ NDIS_HANDLE NdisMiniportDataPathHandle,
    _In_ WDI_PORT_ID PortId,
    _In_ WDI_PEER_ID PeerId,
    _In_ WDI_EXTENDED_TID ExTid,
    _In_ PNET_BUFFER_LIST NetBufferList,
    _In_ BOOLEAN bIsUnicast,
    _In_ BOOLEAN bUseLegacyRates,
    _In_ UINT16 Ethertype,
    _In_ WDI_EXEMPTION_ACTION_TYPE ExemptionAction)
{
    UNREFERENCED_PARAMETER(NdisMiniportDataPathHandle);
    UNREFERENCED_PARAMETER(ExTid);
    UNREFERENCED_PARAMETER(NetBufferList);
    UNREFERENCED_PARAMETER(bIsUnicast);
    UNREFERENCED_PARAMETER(bUseLegacyRates);
    UNREFERENCED_PARAMETER(ExemptionAction);

    DPRINT1("Injected frame, port %u peer %u ethertype 0x%04x, not sent\n", PortId, PeerId, Ethertype);
}

static
VOID
NTAPI
WdiTxAbortConfirm(
    _In_ NDIS_HANDLE NdisMiniportDataPathHandle)
{
    UNREFERENCED_PARAMETER(NdisMiniportDataPathHandle);
}

static
VOID
NTAPI
WdiTxQuerySuspectFrameCompleteStatus(
    _In_ NDIS_HANDLE NdisMiniportDataPathHandle,
    _In_ UINT64 SuspectFrameContext,
    _In_ PNET_BUFFER_LIST NetBufferList,
    _Out_ BOOLEAN *IsTransferCompleteNeeded,
    _Out_ BOOLEAN *IsSendCompleteNeeded,
    _Out_ NDIS_STATUS *WifiStatus)
{
    UNREFERENCED_PARAMETER(NdisMiniportDataPathHandle);
    UNREFERENCED_PARAMETER(SuspectFrameContext);
    UNREFERENCED_PARAMETER(NetBufferList);

    *IsTransferCompleteNeeded = FALSE;
    *IsSendCompleteNeeded = FALSE;
    *WifiStatus = NDIS_STATUS_SUCCESS;
}

/* Receive; frames are pulled and returned since nothing above takes them yet */

static
VOID
NTAPI
WdiRxInorderData(
    _In_ NDIS_HANDLE NdisMiniportDataPathHandle,
    _In_ WDI_RX_INDICATION_LEVEL IndicationLevel,
    _In_ WDI_PEER_ID PeerId,
    _In_ WDI_EXTENDED_TID ExTid,
    _In_ PNDIS_RECEIVE_THROTTLE_PARAMETERS RxThrottleParams,
    _Out_ NDIS_STATUS *WifiStatus)
{
    PWDI_ADAPTER Adapter = NdisMiniportDataPathHandle;
    PNET_BUFFER_LIST Frames = NULL;
    PNET_BUFFER_LIST Frame;
    ULONG Count;

    UNREFERENCED_PARAMETER(IndicationLevel);
    UNREFERENCED_PARAMETER(RxThrottleParams);

    *WifiStatus = NDIS_STATUS_SUCCESS;

    if (Adapter->DataHandlers.RxGetMpdusHandler == NULL ||
        Adapter->DataHandlers.RxReturnFramesHandler == NULL)
    {
        return;
    }

    Adapter->DataHandlers.RxGetMpdusHandler(Adapter->TalTxRx, PeerId, ExTid, &Frames);
    if (Frames == NULL)
        return;

    Count = 0;
    for (Frame = Frames; Frame != NULL; Frame = NET_BUFFER_LIST_NEXT_NBL(Frame))
        Count++;

    /* NDIS turns the 802.11 frames into Ethernet and copies them, since the
       resources flag keeps ownership here so they go straight back below */
    NdisMIndicateReceiveNetBufferLists(Adapter->MiniportAdapterHandle,
                                       Frames,
                                       NDIS_DEFAULT_PORT_NUMBER,
                                       Count,
                                       NDIS_RECEIVE_FLAGS_RESOURCES |
                                       NDIS_RECEIVE_FLAGS_DISPATCH_LEVEL);

    Adapter->DataHandlers.RxReturnFramesHandler(Adapter->TalTxRx, Frames);
}

static
VOID
NTAPI
WdiRxStopConfirm(
    _In_ NDIS_HANDLE NdisMiniportDataPathHandle)
{
    UNREFERENCED_PARAMETER(NdisMiniportDataPathHandle);
}

static
VOID
NTAPI
WdiRxFlushConfirm(
    _In_ NDIS_HANDLE NdisMiniportDataPathHandle)
{
    UNREFERENCED_PARAMETER(NdisMiniportDataPathHandle);
}

/* Peers */

static
VOID
NTAPI
WdiPeerCreate(
    _In_ NDIS_HANDLE NdisMiniportDataPathHandle,
    _In_ WDI_PORT_ID PortId,
    _In_ WDI_PEER_ID PeerId,
    _In_ WDI_MAC_ADDRESS PeerAddr)
{
    UNREFERENCED_PARAMETER(NdisMiniportDataPathHandle);

    DPRINT1("Peer %u on port %u is %02x:%02x:%02x:%02x:%02x:%02x\n",
            PeerId, PortId,
            PeerAddr.Address[0], PeerAddr.Address[1], PeerAddr.Address[2],
            PeerAddr.Address[3], PeerAddr.Address[4], PeerAddr.Address[5]);
}

static
VOID
NTAPI
WdiPeerDelete(
    _In_ NDIS_HANDLE NdisMiniportDataPathHandle,
    _In_ WDI_PORT_ID PortId,
    _In_ WDI_PEER_ID PeerId,
    _Out_ NDIS_STATUS *WifiStatus)
{
    UNREFERENCED_PARAMETER(NdisMiniportDataPathHandle);

    /* No frames are queued for any peer, so it can go right away */
    DPRINT1("Peer %u on port %u deleted\n", PeerId, PortId);
    *WifiStatus = NDIS_STATUS_SUCCESS;
}

/**
 * @brief
 * Fills in the data path API handed to the miniport's TalTxRxInitialize.
 */
_Use_decl_annotations_
VOID
NTAPI
WdiSetDataApi(
    PNDIS_WDI_DATA_API DataApi)
{
    RtlZeroMemory(DataApi, sizeof(*DataApi));
    DataApi->Header.Type = NDIS_OBJECT_TYPE_WDI_DATA_API;
    DataApi->Header.Revision = NDIS_OBJECT_TYPE_WDI_DATA_API_REVISION_2;
    DataApi->Header.Size = NDIS_SIZEOF_WDI_DATA_API_REVISION_2;

    DataApi->TxDequeueIndication = WdiTxDequeue;
    DataApi->TxTransferCompleteIndication = WdiTxTransferComplete;
    DataApi->TxSendCompleteIndication = WdiTxSendComplete;
    DataApi->TxQueryRATIDState = WdiTxQueryRaTidState;
    DataApi->TxSendPauseIndication = WdiTxSendPause;
    DataApi->TxSendRestartIndication = WdiTxSendRestart;
    DataApi->TxReleaseFrameIndication = WdiTxReleaseFrames;
    DataApi->TxInjectFrameIndication = WdiTxInjectFrame;
    DataApi->TxAbortConfirm = WdiTxAbortConfirm;
    DataApi->RxInorderDataIndication = WdiRxInorderData;
    DataApi->RxStopConfirm = WdiRxStopConfirm;
    DataApi->RxFlushConfirm = WdiRxFlushConfirm;
    DataApi->PeerCreateIndication = WdiPeerCreate;
    DataApi->PeerDeleteIndication = WdiPeerDelete;
    DataApi->AllocateWiFiFrameMetaData = WdiAllocateFrameMetadata;
    DataApi->FreeWiFiFrameMetaData = WdiFreeFrameMetadata;
    DataApi->TxQuerySuspectFrameCompleteStatus = WdiTxQuerySuspectFrameCompleteStatus;
}
