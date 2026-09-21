/*
 * PROJECT:     ReactOS NDIS library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     OID requests through the miniport core
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "ndissys.h"

/* A request NDIS makes for itself and waits on */
typedef struct _CORE_SYNC_REQUEST
{
    CORE_OID_REQUEST Core;
    KEVENT Event;
    NDIS_STATUS Status;
} CORE_SYNC_REQUEST, *PCORE_SYNC_REQUEST;

/* OIDs answered from what a 6.x miniport already described */

static
NDIS_STATUS
CoreAnswerFromBuffer(
    _Inout_ PNDIS_OID_REQUEST Request,
    _In_reads_bytes_(Length) const VOID *Data,
    _In_ ULONG Length)
{
    if (Request->DATA.QUERY_INFORMATION.InformationBufferLength < Length)
    {
        Request->DATA.QUERY_INFORMATION.BytesNeeded = Length;
        return NDIS_STATUS_INVALID_LENGTH;
    }

    RtlCopyMemory(Request->DATA.QUERY_INFORMATION.InformationBuffer, Data, Length);
    Request->DATA.QUERY_INFORMATION.BytesWritten = Length;
    return NDIS_STATUS_SUCCESS;
}

static
NDIS_STATUS
CoreAnswerUlong(
    _Inout_ PNDIS_OID_REQUEST Request,
    _In_ ULONG Value)
{
    return CoreAnswerFromBuffer(Request, &Value, sizeof(Value));
}

static
BOOLEAN
CoreIsQuery(
    _In_ PNDIS_OID_REQUEST Request)
{
    return Request->RequestType == NdisRequestQueryInformation ||
           Request->RequestType == NdisRequestQueryStatistics;
}

/*
 * Returns TRUE when NDIS answered the request itself. Queries of what the
 * miniport described are answered from the cache, and sets of them fail.
 */
static
BOOLEAN
CoreAnswerOid(
    _In_ PLOGICAL_ADAPTER Adapter,
    _Inout_ PNDIS_OID_REQUEST Request,
    _Out_ PNDIS_STATUS Status)
{
    PMINIPORT_CORE Core = &Adapter->Core;
    NDIS_OID Oid = Request->DATA.QUERY_INFORMATION.Oid;
    BOOLEAN Connected = (Core->LinkState.MediaConnectState == MediaConnectStateConnected);
    NDIS_LINK_SPEED Speed;
    ULONG Value;

    switch (Oid)
    {
        case OID_GEN_CURRENT_LOOKAHEAD:
            if (CoreIsQuery(Request))
            {
                *Status = CoreAnswerUlong(Request, Core->CurrentLookahead);
                return TRUE;
            }

            if (Request->RequestType != NdisRequestSetInformation)
                return FALSE;

            if (Request->DATA.SET_INFORMATION.InformationBufferLength < sizeof(ULONG))
            {
                Request->DATA.SET_INFORMATION.BytesNeeded = sizeof(ULONG);
                *Status = NDIS_STATUS_INVALID_LENGTH;
                return TRUE;
            }

            /* Every frame reaches the protocols whole, so any lookahead up to the maximum holds */
            Value = *(PULONG)Request->DATA.SET_INFORMATION.InformationBuffer;
            if (Value > Core->LookaheadSize)
            {
                *Status = NDIS_STATUS_INVALID_LENGTH;
                return TRUE;
            }

            Core->CurrentLookahead = Value;
            Request->DATA.SET_INFORMATION.BytesRead = sizeof(ULONG);
            *Status = NDIS_STATUS_SUCCESS;
            return TRUE;

        case OID_GEN_PROTOCOL_OPTIONS:
            if (Request->RequestType == NdisRequestSetInformation)
            {
                Request->DATA.SET_INFORMATION.BytesRead = Request->DATA.SET_INFORMATION.InformationBufferLength;
                *Status = NDIS_STATUS_SUCCESS;
                return TRUE;
            }
            break;

        case OID_GEN_CURRENT_PACKET_FILTER:
            if (CoreIsQuery(Request))
            {
                *Status = CoreAnswerUlong(Request, Core->CurrentPacketFilter);
                return TRUE;
            }
            /* A set goes on to the miniport */
            return FALSE;

        case OID_802_3_MULTICAST_LIST:
            return FALSE;

        default:
            break;
    }

    if (!CoreIsQuery(Request))
    {
        switch (Oid)
        {
            case OID_GEN_SUPPORTED_LIST:
            case OID_GEN_MEDIA_SUPPORTED:
            case OID_GEN_MEDIA_IN_USE:
            case OID_GEN_MAXIMUM_LOOKAHEAD:
            case OID_GEN_MAXIMUM_FRAME_SIZE:
            case OID_GEN_MAXIMUM_TOTAL_SIZE:
            case OID_GEN_LINK_SPEED:
            case OID_GEN_DRIVER_VERSION:
            case OID_GEN_MAC_OPTIONS:
            case OID_GEN_MEDIA_CONNECT_STATUS:
            case OID_GEN_PHYSICAL_MEDIUM:
            case OID_GEN_MAX_LINK_SPEED:
            case OID_GEN_LINK_STATE:
            case OID_802_3_PERMANENT_ADDRESS:
            case OID_802_3_CURRENT_ADDRESS:
            case OID_802_3_MAXIMUM_LIST_SIZE:
                *Status = NDIS_STATUS_NOT_SUPPORTED;
                return TRUE;

            default:
                return FALSE;
        }
    }

    switch (Oid)
    {
        case OID_GEN_SUPPORTED_LIST:
            if (Core->SupportedOidList == NULL)
                return FALSE;
            *Status = CoreAnswerFromBuffer(Request, Core->SupportedOidList, Core->SupportedOidListLength);
            return TRUE;

        case OID_GEN_MEDIA_SUPPORTED:
        case OID_GEN_MEDIA_IN_USE:
            *Status = CoreAnswerUlong(Request, Core->MediaType);
            return TRUE;

        case OID_GEN_MAXIMUM_LOOKAHEAD:
            *Status = CoreAnswerUlong(Request, Core->LookaheadSize);
            return TRUE;

        case OID_GEN_MAXIMUM_FRAME_SIZE:
            *Status = CoreAnswerUlong(Request, Core->MtuSize);
            return TRUE;

        case OID_GEN_MAXIMUM_TOTAL_SIZE:
            *Status = CoreAnswerUlong(Request, Core->MtuSize + Adapter->MediumHeaderSize);
            return TRUE;

        case OID_GEN_LINK_SPEED:
            /* In units of 100 bps, the current speed once linked */
            *Status = CoreAnswerUlong(Request,
                                      (ULONG)((Connected ? Core->LinkState.XmitLinkSpeed :
                                                           Core->MaxXmitLinkSpeed) / 100));
            return TRUE;

        case OID_GEN_MEDIA_CONNECT_STATUS:
            *Status = CoreAnswerUlong(Request,
                                      Connected ? NdisMediaStateConnected : NdisMediaStateDisconnected);
            return TRUE;

        case OID_GEN_DRIVER_VERSION:
        {
            PNDIS_MINIPORT_DRIVER_CHARACTERISTICS Chars =
                &Adapter->NdisMiniportBlock.DriverHandle->Characteristics6;
            USHORT Version = (USHORT)((Chars->MajorNdisVersion << 8) | Chars->MinorNdisVersion);

            *Status = CoreAnswerFromBuffer(Request, &Version, sizeof(Version));
            return TRUE;
        }

        case OID_GEN_MAC_OPTIONS:
            *Status = CoreAnswerUlong(Request, Core->MacOptions);
            return TRUE;

        case OID_GEN_PHYSICAL_MEDIUM:
            *Status = CoreAnswerUlong(Request, Core->PhysicalMediumType);
            return TRUE;

        case OID_GEN_MAX_LINK_SPEED:
            Speed.XmitLinkSpeed = Core->MaxXmitLinkSpeed;
            Speed.RcvLinkSpeed = Core->MaxRcvLinkSpeed;
            *Status = CoreAnswerFromBuffer(Request, &Speed, sizeof(Speed));
            return TRUE;

        case OID_GEN_LINK_STATE:
            *Status = CoreAnswerFromBuffer(Request, &Core->LinkState, sizeof(Core->LinkState));
            return TRUE;

        case OID_802_3_PERMANENT_ADDRESS:
            *Status = CoreAnswerFromBuffer(Request, Core->PermanentMacAddress, Core->MacAddressLength);
            return TRUE;

        case OID_802_3_CURRENT_ADDRESS:
            *Status = CoreAnswerFromBuffer(Request, Core->CurrentMacAddress, Core->MacAddressLength);
            return TRUE;

        case OID_802_3_MAXIMUM_LIST_SIZE:
            *Status = CoreAnswerUlong(Request, Core->MaxMulticastListSize);
            return TRUE;

        default:
            return FALSE;
    }
}

/* What NDIS keeps track of once the miniport accepted a set */
static
VOID
CoreAfterOid(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PNDIS_OID_REQUEST Request,
    _In_ NDIS_STATUS Status)
{
    if (Status != NDIS_STATUS_SUCCESS || Request->RequestType != NdisRequestSetInformation)
        return;

    if (Request->DATA.SET_INFORMATION.InformationBufferLength < sizeof(ULONG))
        return;

    switch (Request->DATA.SET_INFORMATION.Oid)
    {
        case OID_GEN_CURRENT_PACKET_FILTER:
            Adapter->Core.CurrentPacketFilter = *(PULONG)Request->DATA.SET_INFORMATION.InformationBuffer;
            break;

        case OID_GEN_CURRENT_LOOKAHEAD:
            Adapter->Core.CurrentLookahead = *(PULONG)Request->DATA.SET_INFORMATION.InformationBuffer;
            break;

        default:
            break;
    }
}

/* The queue */

static IO_WORKITEM_ROUTINE CoreRunQueuedOid;

/* The device extension is not NDIS's when a class extension owns the device */
typedef struct _CORE_OID_WORK
{
    PIO_WORKITEM WorkItem;
    PLOGICAL_ADAPTER Adapter;
} CORE_OID_WORK, *PCORE_OID_WORK;

static
VOID
CoreScheduleQueuedOid(
    _In_ PLOGICAL_ADAPTER Adapter)
{
    PCORE_OID_WORK Work;

    Work = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Work), NDIS_TAG);
    if (Work != NULL)
    {
        Work->Adapter = Adapter;
        Work->WorkItem = IoAllocateWorkItem(Adapter->NdisMiniportBlock.DeviceObject);
        if (Work->WorkItem != NULL)
        {
            IoQueueWorkItem(Work->WorkItem, CoreRunQueuedOid, DelayedWorkQueue, Work);
            return;
        }

        ExFreePoolWithTag(Work, NDIS_TAG);
    }

    NDIS_DbgPrint(MIN_TRACE, ("No work item for a queued OID request.\n"));
}

/*
 * Finish the active request, then start the next one. Completion is only
 * called for requests whose originator was told they pended.
 */
static
VOID
CoreFinishOid(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PCORE_OID_REQUEST CoreRequest,
    _In_ NDIS_STATUS Status,
    _In_ BOOLEAN CallCompletion)
{
    PMINIPORT_CORE Core = &Adapter->Core;
    BOOLEAN More;
    KIRQL OldIrql;

    CoreAfterOid(Adapter, &CoreRequest->Request, Status);

    KeAcquireSpinLock(&Core->Lock, &OldIrql);
    Core->ActiveOidRequest = NULL;
    More = !IsListEmpty(&Core->OidQueue);
    KeReleaseSpinLock(&Core->Lock, OldIrql);

    if (CallCompletion)
        CoreRequest->Completion(Adapter, CoreRequest, Status);

    if (More)
        CoreScheduleQueuedOid(Adapter);
}

static
NDIS_STATUS
CoreDispatchOid(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PCORE_OID_REQUEST CoreRequest)
{
    return Adapter->Core.Dispatch->OidRequestHandler(CORE_DISPATCH_CONTEXT(Adapter),
                                                     &CoreRequest->Request);
}

static
VOID
NTAPI
CoreRunQueuedOid(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID Context)
{
    PCORE_OID_WORK Work = Context;
    PLOGICAL_ADAPTER Adapter = Work->Adapter;
    PMINIPORT_CORE Core = &Adapter->Core;
    PCORE_OID_REQUEST CoreRequest = NULL;
    NDIS_STATUS Status;
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(DeviceObject);

    IoFreeWorkItem(Work->WorkItem);
    ExFreePoolWithTag(Work, NDIS_TAG);

    KeAcquireSpinLock(&Core->Lock, &OldIrql);
    if (Core->ActiveOidRequest == NULL && !IsListEmpty(&Core->OidQueue))
    {
        CoreRequest = CONTAINING_RECORD(RemoveHeadList(&Core->OidQueue), CORE_OID_REQUEST, QueueEntry);
        Core->ActiveOidRequest = CoreRequest;
    }
    KeReleaseSpinLock(&Core->Lock, OldIrql);

    if (CoreRequest == NULL)
        return;

    /* Its originator was already told NDIS_STATUS_PENDING */
    Status = CoreDispatchOid(Adapter, CoreRequest);
    if (Status != NDIS_STATUS_PENDING)
        CoreFinishOid(Adapter, CoreRequest, Status, TRUE);
}

/**
 * @brief
 * Sends an OID request to a miniport, or answers it when NDIS can.
 *
 * @param[in] Adapter
 * The adapter the request is for.
 *
 * @param[in] CoreRequest
 * The request. Completion is called only if this returns NDIS_STATUS_PENDING.
 *
 * @return
 * The request's status, or NDIS_STATUS_PENDING.
 */
NDIS_STATUS
NTAPI
CoreOidRequest(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PCORE_OID_REQUEST CoreRequest)
{
    PMINIPORT_CORE Core = &Adapter->Core;
    NDIS_STATUS Status;
    BOOLEAN Queue;
    KIRQL OldIrql;

    if (Adapter->NdisMiniportBlock.DriverHandle->Ndis6Driver &&
        CoreAnswerOid(Adapter, &CoreRequest->Request, &Status))
    {
        return Status;
    }

    /* MiniportOidRequest runs at PASSIVE_LEVEL, and only one at a time */
    KeAcquireSpinLock(&Core->Lock, &OldIrql);
    Queue = (Core->ActiveOidRequest != NULL || OldIrql != PASSIVE_LEVEL);
    if (Queue)
        InsertTailList(&Core->OidQueue, &CoreRequest->QueueEntry);
    else
        Core->ActiveOidRequest = CoreRequest;
    KeReleaseSpinLock(&Core->Lock, OldIrql);

    if (Queue)
    {
        if (OldIrql != PASSIVE_LEVEL)
            CoreScheduleQueuedOid(Adapter);
        return NDIS_STATUS_PENDING;
    }

    Status = CoreDispatchOid(Adapter, CoreRequest);
    if (Status != NDIS_STATUS_PENDING)
        CoreFinishOid(Adapter, CoreRequest, Status, FALSE);

    return Status;
}

/**
 * @brief
 * Finishes the OID request a miniport pended.
 *
 * @param[in] Adapter
 * The adapter.
 *
 * @param[in] OidRequest
 * The request the miniport was given.
 *
 * @param[in] Status
 * How it went.
 */
VOID
NTAPI
CoreOidRequestComplete(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PNDIS_OID_REQUEST OidRequest,
    _In_ NDIS_STATUS Status)
{
    CoreFinishOid(Adapter,
                  CONTAINING_RECORD(OidRequest, CORE_OID_REQUEST, Request),
                  Status,
                  TRUE);
}

/**
 * @brief
 * A 6.x miniport finishes an OID request it returned NDIS_STATUS_PENDING for.
 *
 * @param[in] MiniportAdapterHandle
 * The adapter.
 *
 * @param[in] OidRequest
 * The request.
 *
 * @param[in] Status
 * How it went.
 */
_Use_decl_annotations_
VOID
NTAPI
NdisMOidRequestComplete(
    NDIS_HANDLE MiniportAdapterHandle,
    PNDIS_OID_REQUEST OidRequest,
    NDIS_STATUS Status)
{
    CoreOidRequestComplete((PLOGICAL_ADAPTER)MiniportAdapterHandle, OidRequest, Status);
}

/* Requests NDIS makes for itself */

static
VOID
NTAPI
CoreSyncRequestComplete(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PCORE_OID_REQUEST CoreRequest,
    _In_ NDIS_STATUS Status)
{
    PCORE_SYNC_REQUEST Request = CONTAINING_RECORD(CoreRequest, CORE_SYNC_REQUEST, Core);

    UNREFERENCED_PARAMETER(Adapter);

    Request->Status = Status;
    KeSetEvent(&Request->Event, IO_NO_INCREMENT, FALSE);
}

static
NDIS_STATUS
CoreSyncRequest(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ NDIS_REQUEST_TYPE RequestType,
    _In_ NDIS_OID Oid,
    _In_ PVOID Buffer,
    _In_ ULONG Length,
    _Out_ PULONG BytesDone,
    _Out_opt_ PULONG BytesNeeded)
{
    PCORE_SYNC_REQUEST Request;
    NDIS_STATUS Status;

    *BytesDone = 0;
    if (BytesNeeded != NULL)
        *BytesNeeded = 0;

    Request = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Request), NDIS_TAG);
    if (Request == NULL)
        return NDIS_STATUS_RESOURCES;

    RtlZeroMemory(Request, sizeof(*Request));
    KeInitializeEvent(&Request->Event, NotificationEvent, FALSE);
    Request->Core.Completion = CoreSyncRequestComplete;

    Request->Core.Request.Header.Type = NDIS_OBJECT_TYPE_OID_REQUEST;
    Request->Core.Request.Header.Revision = NDIS_OID_REQUEST_REVISION_1;
    Request->Core.Request.Header.Size = NDIS_SIZEOF_OID_REQUEST_REVISION_1;
    Request->Core.Request.RequestType = RequestType;
    Request->Core.Request.PortNumber = NDIS_DEFAULT_PORT_NUMBER;
    Request->Core.Request.DATA.QUERY_INFORMATION.Oid = Oid;
    Request->Core.Request.DATA.QUERY_INFORMATION.InformationBuffer = Buffer;
    Request->Core.Request.DATA.QUERY_INFORMATION.InformationBufferLength = Length;

    Status = CoreOidRequest(Adapter, &Request->Core);
    if (Status == NDIS_STATUS_PENDING)
    {
        KeWaitForSingleObject(&Request->Event, Executive, KernelMode, FALSE, NULL);
        Status = Request->Status;
    }

    if (RequestType == NdisRequestSetInformation)
        *BytesDone = Request->Core.Request.DATA.SET_INFORMATION.BytesRead;
    else
        *BytesDone = Request->Core.Request.DATA.QUERY_INFORMATION.BytesWritten;

    if (BytesNeeded != NULL)
        *BytesNeeded = Request->Core.Request.DATA.QUERY_INFORMATION.BytesNeeded;

    ExFreePoolWithTag(Request, NDIS_TAG);
    return Status;
}

/**
 * @brief
 * Queries a miniport for NDIS's own use and waits for the answer.
 *
 * @param[in] Adapter
 * The adapter to query.
 *
 * @param[in] Oid
 * What to ask for.
 *
 * @param[out] Buffer
 * Receives the answer.
 *
 * @param[in] Length
 * Size of Buffer.
 *
 * @param[out] BytesWritten
 * How much was written.
 *
 * @return
 * The query's status.
 */
NDIS_STATUS
NTAPI
CoreQueryInformation(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ NDIS_OID Oid,
    _Out_writes_bytes_to_(Length, *BytesWritten) PVOID Buffer,
    _In_ ULONG Length,
    _Out_ PULONG BytesWritten)
{
    return CoreSyncRequest(Adapter, NdisRequestQueryInformation, Oid, Buffer, Length, BytesWritten, NULL);
}

/**
 * @brief
 * Queries a miniport for NDIS's own use and waits for the answer, also
 * reporting how much room a longer answer needs.
 *
 * @param[in] Adapter
 * The adapter to query.
 *
 * @param[in] Oid
 * What to ask for.
 *
 * @param[out] Buffer
 * Receives the answer. Can be NULL when Length is zero.
 *
 * @param[in] Length
 * Size of Buffer.
 *
 * @param[out] BytesWritten
 * How much was written.
 *
 * @param[out] BytesNeeded
 * How much the answer needs, when Buffer was too short.
 *
 * @return
 * The query's status.
 */
NDIS_STATUS
NTAPI
CoreQueryInformationEx(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ NDIS_OID Oid,
    _Out_writes_bytes_to_opt_(Length, *BytesWritten) PVOID Buffer,
    _In_ ULONG Length,
    _Out_ PULONG BytesWritten,
    _Out_ PULONG BytesNeeded)
{
    return CoreSyncRequest(Adapter,
                           NdisRequestQueryInformation,
                           Oid,
                           Buffer,
                           Length,
                           BytesWritten,
                           BytesNeeded);
}

/**
 * @brief
 * Sets a miniport value for NDIS's own use and waits for the result.
 *
 * @param[in] Adapter
 * The adapter.
 *
 * @param[in] Oid
 * What to set.
 *
 * @param[in] Buffer
 * The value.
 *
 * @param[in] Length
 * Size of Buffer.
 *
 * @param[out] BytesRead
 * How much the miniport consumed.
 *
 * @return
 * The set's status.
 */
NDIS_STATUS
NTAPI
CoreSetInformation(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ NDIS_OID Oid,
    _In_reads_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length,
    _Out_ PULONG BytesRead)
{
    return CoreSyncRequest(Adapter, NdisRequestSetInformation, Oid, Buffer, Length, BytesRead, NULL);
}
