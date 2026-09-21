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

/* Transmit; the upper edge queues nothing, so there is nothing to hand out */

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
    UNREFERENCED_PARAMETER(NdisMiniportDataPathHandle);
    UNREFERENCED_PARAMETER(Quantum);
    UNREFERENCED_PARAMETER(MaxNumFrames);
    UNREFERENCED_PARAMETER(Credit);

    *NetBufferList = NULL;
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

    DPRINT1("Transfer complete (%d) for %p, none was sent\n", WifiTxFrameStatus, NetBufferList);
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
    UNREFERENCED_PARAMETER(NdisMiniportDataPathHandle);
    UNREFERENCED_PARAMETER(WifiTxFrameIdList);
    UNREFERENCED_PARAMETER(WifiTxCompleteList);

    DPRINT1("Send complete (%d) for %u frames, none was sent\n", WifiTxFrameStatus, NumCompletedSends);
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

    UNREFERENCED_PARAMETER(IndicationLevel);
    UNREFERENCED_PARAMETER(RxThrottleParams);

    *WifiStatus = NDIS_STATUS_SUCCESS;

    if (Adapter->DataHandlers.RxGetMpdusHandler == NULL)
        return;

    Adapter->DataHandlers.RxGetMpdusHandler(Adapter->TalTxRx, PeerId, ExTid, &Frames);
    if (Frames != NULL && Adapter->DataHandlers.RxReturnFramesHandler != NULL)
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
