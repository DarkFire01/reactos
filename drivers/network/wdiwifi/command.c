/*
 * PROJECT:     ReactOS WDI upper edge
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     WDI commands down to the miniport and indications up from it
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "wdiwifi.h"

#define NDEBUG
#include <debug.h>

/* A response too big for its buffer is retried with what the miniport asked for */
#define WDI_MAX_RESIZES                     2

_Use_decl_annotations_
VOID
NTAPI
WdiInitializeCommands(
    PWDI_ADAPTER Adapter)
{
    KeInitializeEvent(&Adapter->CommandLock, SynchronizationEvent, TRUE);
    KeInitializeSpinLock(&Adapter->TaskLock);
    KeInitializeEvent(&Adapter->TaskDone, NotificationEvent, FALSE);
    Adapter->NextTransactionId = 0;
}

_Use_decl_annotations_
VOID
NTAPI
WdiFreeMessage(
    PWDI_MESSAGE Message)
{
    if (Message->Buffer != NULL)
        ExFreePoolWithTag(Message->Buffer, WDI_TAG);

    RtlZeroMemory(Message, sizeof(*Message));
}

/* These go down as direct OIDs */
static
BOOLEAN
WdiIsDirectMessage(
    _In_ UINT16 MessageId)
{
    switch (MessageId)
    {
        case WDI_TASK_P2P_SEND_RESPONSE_ACTION_FRAME:
        case WDI_TASK_SEND_AP_ASSOCIATION_RESPONSE:
        case WDI_SET_FAST_BSS_TRANSITION_PARAMETERS:
        case WDI_SET_SAE_AUTH_PARAMS:
            return TRUE;

        default:
            return FALSE;
    }
}

static
UINT32
WdiNextTransactionId(
    _In_ PWDI_ADAPTER Adapter)
{
    LONG Id;

    /* Zero marks unsolicited indications */
    do
    {
        Id = InterlockedIncrement(&Adapter->NextTransactionId);
    } while (Id == 0);

    return (UINT32)Id;
}

static
PWDI_REQUEST
WdiBuildRequest(
    _In_ PWDI_ADAPTER Adapter,
    _In_ UINT16 MessageId,
    _In_ WDI_PORT_ID PortId,
    _In_ UINT32 TransactionId,
    _In_reads_bytes_(TlvLength) const UCHAR *Tlvs,
    _In_ ULONG TlvLength,
    _In_ ULONG OutputLength)
{
    PNDIS_MINIPORT_DRIVER_CHARACTERISTICS Ihv = &Adapter->Miniport->Ndis;
    PWDI_MESSAGE_HEADER Header;
    PWDI_REQUEST Request;

    Request = ExAllocatePoolWithTag(NonPagedPool, FIELD_OFFSET(WDI_REQUEST, Buffer[OutputLength]), WDI_TAG);
    if (Request == NULL)
        return NULL;

    RtlZeroMemory(Request, FIELD_OFFSET(WDI_REQUEST, Buffer[sizeof(*Header)]));
    Request->Adapter = Adapter;
    Request->State = WDI_REQUEST_PENDING;
    Request->BufferLength = OutputLength;
    KeInitializeEvent(&Request->Done, NotificationEvent, FALSE);

    Header = (PWDI_MESSAGE_HEADER)Request->Buffer;
    Header->PortId = PortId;
    Header->TransactionId = TransactionId;
    if (TlvLength != 0)
        RtlCopyMemory(Header + 1, Tlvs, TlvLength);

    /* The request revision follows what the miniport was built for */
    Request->Oid.Header.Type = NDIS_OBJECT_TYPE_OID_REQUEST;
    if (Ihv->MajorNdisVersion > 6 || (Ihv->MajorNdisVersion == 6 && Ihv->MinorNdisVersion >= 50))
    {
        Request->Oid.Header.Revision = NDIS_OID_REQUEST_REVISION_2;
        Request->Oid.Header.Size = NDIS_SIZEOF_OID_REQUEST_REVISION_2;
    }
    else
    {
        Request->Oid.Header.Revision = NDIS_OID_REQUEST_REVISION_1;
        Request->Oid.Header.Size = NDIS_SIZEOF_OID_REQUEST_REVISION_1;
    }
    Request->Oid.RequestType = NdisRequestMethod;
    Request->Oid.RequestId = &Request->Oid;
    Request->Oid.DATA.METHOD_INFORMATION.Oid = WDI_DEFINE_OID(MessageId);
    Request->Oid.DATA.METHOD_INFORMATION.InformationBuffer = Request->Buffer;
    Request->Oid.DATA.METHOD_INFORMATION.InputBufferLength = sizeof(*Header) + TlvLength;
    Request->Oid.DATA.METHOD_INFORMATION.OutputBufferLength = OutputLength;

    return Request;
}

_Use_decl_annotations_
VOID
NTAPI
WdiOidRequestComplete(
    PWDI_ADAPTER Adapter,
    PNDIS_OID_REQUEST OidRequest,
    NDIS_STATUS Status)
{
    PWDI_REQUEST Request = CONTAINING_RECORD(OidRequest, WDI_REQUEST, Oid);

    UNREFERENCED_PARAMETER(Adapter);

    Request->Status = Status;
    if (InterlockedCompareExchange(&Request->State, WDI_REQUEST_COMPLETED, WDI_REQUEST_PENDING) == WDI_REQUEST_ABANDONED)
    {
        /* Whoever sent it stopped waiting, so nobody else frees it */
        ExFreePoolWithTag(Request, WDI_TAG);
        return;
    }

    KeSetEvent(&Request->Done, IO_NO_INCREMENT, FALSE);
}

static
VOID
WdiArmTask(
    _In_ PWDI_ADAPTER Adapter,
    _In_ BOOLEAN Arm,
    _In_ UINT32 TransactionId,
    _In_ WDI_PORT_ID PortId)
{
    KIRQL OldIrql;

    KeAcquireSpinLock(&Adapter->TaskLock, &OldIrql);
    Adapter->TaskArmed = Arm;
    Adapter->TaskTransactionId = TransactionId;
    Adapter->TaskPortId = PortId;
    if (Arm)
        KeClearEvent(&Adapter->TaskDone);
    KeReleaseSpinLock(&Adapter->TaskLock, OldIrql);

    if (!Arm)
        WdiFreeMessage(&Adapter->TaskResult);
}

static
BOOLEAN
WdiWait(
    _In_ PKEVENT Event,
    _In_ ULONG Milliseconds)
{
    LARGE_INTEGER Timeout;

    Timeout.QuadPart = -10000LL * Milliseconds;
    return KeWaitForSingleObject(Event, Executive, KernelMode, FALSE, &Timeout) == STATUS_SUCCESS;
}

static
NDIS_STATUS
WdiCopyMessage(
    _Out_ PWDI_MESSAGE Message,
    _In_ UINT16 MessageId,
    _In_reads_bytes_(Length) const VOID *Buffer,
    _In_ ULONG Length)
{
    Message->Buffer = ExAllocatePoolWithTag(NonPagedPool, Length, WDI_TAG);
    if (Message->Buffer == NULL)
        return NDIS_STATUS_RESOURCES;

    RtlCopyMemory(Message->Buffer, Buffer, Length);
    Message->MessageId = MessageId;
    Message->Length = Length;
    return NDIS_STATUS_SUCCESS;
}

/**
 * @brief
 * Sends one WDI message as a method OID and waits for its response. A task
 * also waits for the indication with its transaction id, which ends it.
 *
 * @param[in] Adapter
 * The adapter.
 *
 * @param[in] MessageId
 * The WDI message.
 *
 * @param[in] PortId
 * The port it is for, or WDI_PORT_ID_ADAPTER.
 *
 * @param[in] Tlvs
 * The message's TLVs, following the header the channel fills in.
 *
 * @param[in] TlvLength
 * Their length.
 *
 * @param[in] Task
 * Whether the message is a task.
 *
 * @param[out] Result
 * The response, or for a task its completion indication, header first.
 * The caller frees it with WdiFreeMessage.
 *
 * @return
 * How the message went: the OID status when that failed, otherwise the
 * status in the response or completion header.
 */
_Use_decl_annotations_
NDIS_STATUS
NTAPI
WdiSendCommand(
    PWDI_ADAPTER Adapter,
    UINT16 MessageId,
    WDI_PORT_ID PortId,
    const UCHAR *Tlvs,
    ULONG TlvLength,
    BOOLEAN Task,
    PWDI_MESSAGE Result)
{
    PWDI_MESSAGE_HEADER Header;
    PWDI_REQUEST Request;
    NDIS_STATUS Status;
    UINT32 TransactionId;
    ULONG OutputLength;
    ULONG Written;
    ULONG Resizes = 0;

    PAGED_CODE();

    if (Result != NULL)
        RtlZeroMemory(Result, sizeof(*Result));

    if (Adapter->ShutDown != 0)
        return STATUS_UNSUCCESSFUL;

    KeWaitForSingleObject(&Adapter->CommandLock, Executive, KernelMode, FALSE, NULL);

    OutputLength = max((ULONG)sizeof(WDI_MESSAGE_HEADER) + TlvLength, WDI_MIN_RESPONSE_LENGTH);

    for (;;)
    {
        TransactionId = WdiNextTransactionId(Adapter);
        Request = WdiBuildRequest(Adapter, MessageId, PortId, TransactionId, Tlvs, TlvLength, OutputLength);
        if (Request == NULL)
        {
            Status = NDIS_STATUS_RESOURCES;
            goto Done;
        }

        /* The ending indication can come before the OID completes */
        if (Task)
            WdiArmTask(Adapter, TRUE, TransactionId, PortId);

        DPRINT("Sending WDI message %u, transaction %lu\n", MessageId, TransactionId);

        if (WdiIsDirectMessage(MessageId))
            Status = WdiGlobals.Ndis.InvokeDirectOidRequest(Adapter->MiniportAdapterHandle, &Request->Oid);
        else
            Status = WdiGlobals.Ndis.InvokeOidRequest(Adapter->MiniportAdapterHandle, &Request->Oid);

        if (Status != NDIS_STATUS_PENDING)
            WdiOidRequestComplete(Adapter, &Request->Oid, Status);

        if (!WdiWait(&Request->Done, Adapter->CommandTimeout) &&
            InterlockedCompareExchange(&Request->State, WDI_REQUEST_ABANDONED, WDI_REQUEST_PENDING) == WDI_REQUEST_PENDING)
        {
            DPRINT1("WDI message %u timed out after %lu ms\n", MessageId, Adapter->CommandTimeout);
            NdisWriteErrorLogEntry(Adapter->MiniportAdapterHandle, NDIS_ERROR_CODE_HARDWARE_FAILURE, 2, MessageId, TransactionId);
            WdiArmTask(Adapter, FALSE, 0, 0);
            Status = NDIS_STATUS_DEVICE_FAILED;
            goto Done;
        }

        Status = Request->Status;
        if ((Status == NDIS_STATUS_BUFFER_OVERFLOW ||
             Status == NDIS_STATUS_INVALID_LENGTH ||
             Status == NDIS_STATUS_BUFFER_TOO_SHORT) &&
            Request->Oid.DATA.METHOD_INFORMATION.BytesNeeded > OutputLength &&
            Resizes++ < WDI_MAX_RESIZES)
        {
            OutputLength = Request->Oid.DATA.METHOD_INFORMATION.BytesNeeded;
            ExFreePoolWithTag(Request, WDI_TAG);
            WdiArmTask(Adapter, FALSE, 0, 0);
            continue;
        }

        break;
    }

    /* A failed OID leaves just the header, with the OID status in it */
    Header = (PWDI_MESSAGE_HEADER)Request->Buffer;
    if (Status != NDIS_STATUS_SUCCESS)
    {
        Header->Status = Status;
        Written = sizeof(*Header);
    }
    else
    {
        Written = min(Request->Oid.DATA.METHOD_INFORMATION.BytesWritten, Request->BufferLength);
        if (Written < sizeof(*Header))
        {
            DPRINT1("WDI message %u answered with %lu bytes\n", MessageId, Written);
            Status = NDIS_STATUS_INVALID_LENGTH;
        }
        else
        {
            Status = Header->Status;
        }
    }

    if (Status != NDIS_STATUS_SUCCESS || !Task)
    {
        if (Task)
            WdiArmTask(Adapter, FALSE, 0, 0);

        if (Result != NULL && Written >= sizeof(*Header))
        {
            NDIS_STATUS CopyStatus = WdiCopyMessage(Result, MessageId, Request->Buffer, Written);
            if (Status == NDIS_STATUS_SUCCESS)
                Status = CopyStatus;
        }

        ExFreePoolWithTag(Request, WDI_TAG);
        goto Done;
    }

    ExFreePoolWithTag(Request, WDI_TAG);

    if (!WdiWait(&Adapter->TaskDone, Adapter->TaskTimeout))
    {
        DPRINT1("WDI task %u did not finish in %lu ms\n", MessageId, Adapter->TaskTimeout);
        NdisWriteErrorLogEntry(Adapter->MiniportAdapterHandle, NDIS_ERROR_CODE_HARDWARE_FAILURE, 2, MessageId, TransactionId);
        WdiArmTask(Adapter, FALSE, 0, 0);
        Status = NDIS_STATUS_DEVICE_FAILED;
        goto Done;
    }

    /* The indication handler disarmed the task and left its copy behind */
    Header = (PWDI_MESSAGE_HEADER)Adapter->TaskResult.Buffer;
    if (Header == NULL)
    {
        Status = NDIS_STATUS_RESOURCES;
        goto Done;
    }

    Status = Header->Status;
    if (Result != NULL)
        *Result = Adapter->TaskResult;
    else
        WdiFreeMessage(&Adapter->TaskResult);
    RtlZeroMemory(&Adapter->TaskResult, sizeof(Adapter->TaskResult));

Done:
    if (Status != NDIS_STATUS_SUCCESS)
        DPRINT1("WDI message %u failed (0x%x)\n", MessageId, Status);

    KeSetEvent(&Adapter->CommandLock, IO_NO_INCREMENT, FALSE);
    return Status;
}

/**
 * @brief
 * Takes a WDI indication from the miniport. The one ending the pending
 * task is copied for its sender; the rest are not acted on yet.
 *
 * @param[in] Adapter
 * The adapter.
 *
 * @param[in] StatusIndication
 * The indication, a WDI header and TLVs in its status buffer.
 */
_Use_decl_annotations_
VOID
NTAPI
WdiIndication(
    PWDI_ADAPTER Adapter,
    PNDIS_STATUS_INDICATION StatusIndication)
{
    PWDI_MESSAGE_HEADER Header = StatusIndication->StatusBuffer;
    UINT16 MessageId = (UINT16)StatusIndication->StatusCode;
    BOOLEAN Ends = FALSE;
    KIRQL OldIrql;

    if (StatusIndication->StatusBufferSize < sizeof(*Header) || Header == NULL)
    {
        DPRINT1("WDI indication %u with %lu bytes dropped\n", MessageId, StatusIndication->StatusBufferSize);
        return;
    }

    if (MessageId == WDI_INDICATION_BSS_ENTRY_LIST)
    {
        WdiRecordBssList(Adapter,
                         (const UCHAR *)(Header + 1),
                         StatusIndication->StatusBufferSize - sizeof(*Header));
    }

    KeAcquireSpinLock(&Adapter->TaskLock, &OldIrql);
    if (Adapter->TaskArmed &&
        Header->TransactionId == Adapter->TaskTransactionId &&
        Header->PortId == Adapter->TaskPortId)
    {
        /* Without a copy the sender still hears the task ended, and fails it */
        Adapter->TaskArmed = FALSE;
        Ends = TRUE;
        WdiCopyMessage(&Adapter->TaskResult,
                       MessageId,
                       StatusIndication->StatusBuffer,
                       StatusIndication->StatusBufferSize);
        KeSetEvent(&Adapter->TaskDone, IO_NO_INCREMENT, FALSE);
    }
    KeReleaseSpinLock(&Adapter->TaskLock, OldIrql);

    if (!Ends && MessageId != WDI_INDICATION_BSS_ENTRY_LIST)
    {
        DPRINT1("Unsolicited WDI indication %u, port %u, transaction %lu\n",
                MessageId, Header->PortId, Header->TransactionId);
    }
}
