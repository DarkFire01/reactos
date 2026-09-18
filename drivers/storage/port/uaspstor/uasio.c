/*
 * PROJECT:     ReactOS USB Attached SCSI Miniport Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Issuing commands and reading what comes back
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "uaspstor.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ******************************************************************/

static
VOID
UaspCommandTransferDone(
    _In_ PUASP_REQUEST Request,
    _In_ NTSTATUS Status);

static
VOID
UaspDataTransferDone(
    _In_ PUASP_REQUEST Request,
    _In_ NTSTATUS Status);

static
VOID
UaspStatusTransferDone(
    _In_ PUASP_REQUEST Request,
    _In_ NTSTATUS Status);

static
VOID
UaspReadyTransferDone(
    _In_ PUASP_REQUEST Request,
    _In_ NTSTATUS Status);

/**
 * @brief Hands a finished request back to the class layer.
 */
VOID
UaspCompleteSrb(
    _In_ PUASP_ADAPTER_EXTENSION Adapter,
    _In_ PVOID Srb,
    _In_ UCHAR SrbStatus)
{
    SrbSetSrbStatus(Srb, SrbStatus);

    StorPortNotification(RequestComplete, Adapter, Srb);
}

/**
 * @brief Records an outcome, unless something already went wrong.
 *
 * The first thing to go wrong is the one worth reporting: everything after it
 * is fallout.
 */
static
VOID
UaspFailRequest(
    _In_ PUASP_REQUEST Request,
    _In_ UCHAR SrbStatus)
{
    if (Request->SrbStatus == SRB_STATUS_SUCCESS || Request->SrbStatus == SRB_STATUS_ERROR)
        Request->SrbStatus = SrbStatus;
}

/**
 * @brief Retires one transfer, and the request with it when it was the last.
 */
static
VOID
UaspFinishTransfer(
    _In_ PUASP_REQUEST Request)
{
    PUASP_ADAPTER_EXTENSION Adapter = Request->Adapter;
    UCHAR SrbStatus;
    PVOID Srb;

    if (InterlockedDecrement(&Request->Outstanding) != 0)
        return;

    Srb = Request->Srb;
    SrbStatus = Request->SrbStatus;

    if (Request->HasData)
        SrbSetDataTransferLength(Srb, Request->DataTransferred);

    UaspReleaseRequest(Request);

    UaspCompleteSrb(Adapter, Srb, SrbStatus);
}

static
NTSTATUS
NTAPI
UaspTransferCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PVOID Context)
{
    PUASP_TRANSFER Transfer = Context;
    NTSTATUS Status = Irp->IoStatus.Status;

    UNREFERENCED_PARAMETER(DeviceObject);

    /*
     * The stack reports what went wrong on the bus through the request block,
     * which says more than the status of the packet that carried it.
     */
    if (NT_SUCCESS(Status) && !USBD_SUCCESS(Transfer->Urb->UrbHeader.Status))
        Status = STATUS_UNSUCCESSFUL;

    if (!NT_SUCCESS(Status) && UaspIsPipeStalled(Transfer->Urb->UrbHeader.Status))
        UaspQueuePipeReset(Transfer->Request->Adapter, Transfer->ResetPipe);

    Transfer->Completion(Transfer->Request, Status);

    /* The packet is ours and gets used again, so it must not be completed */
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/**
 * @brief Starts one transfer of a command on the pipe or stream given.
 *
 * Every call ends in exactly one completion, so a caller never has to undo
 * a transfer it started.
 */
static
VOID
UaspSubmitTransfer(
    _In_ PUASP_REQUEST Request,
    _In_ UASP_TRANSFER_KIND Kind,
    _In_ USBD_PIPE_HANDLE Pipe,
    _In_ USBD_PIPE_HANDLE ResetPipe,
    _In_opt_ PVOID Buffer,
    _In_opt_ PMDL Mdl,
    _In_ ULONG Length,
    _In_ ULONG TransferFlags,
    _In_ PUASP_TRANSFER_COMPLETION Completion)
{
    PUASP_ADAPTER_EXTENSION Adapter = Request->Adapter;
    PUASP_TRANSFER Transfer = &Request->Transfer[Kind];
    PIO_STACK_LOCATION IoStack;
    PURB Urb = Transfer->Urb;

    Transfer->ResetPipe = ResetPipe;
    Transfer->Completion = Completion;

    RtlZeroMemory(Urb, sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER));

    Urb->UrbBulkOrInterruptTransfer.Hdr.Length =
        sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER);
    Urb->UrbBulkOrInterruptTransfer.Hdr.Function = URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
    Urb->UrbBulkOrInterruptTransfer.PipeHandle = Pipe;
    Urb->UrbBulkOrInterruptTransfer.TransferFlags = TransferFlags;
    Urb->UrbBulkOrInterruptTransfer.TransferBufferLength = Length;
    Urb->UrbBulkOrInterruptTransfer.TransferBuffer = Buffer;
    Urb->UrbBulkOrInterruptTransfer.TransferBufferMDL = Mdl;

    IoReuseIrp(Transfer->Irp, STATUS_SUCCESS);

    IoStack = IoGetNextIrpStackLocation(Transfer->Irp);
    IoStack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    IoStack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;

    USBD_AssignUrbToIoStackLocation(Adapter->UsbdHandle, IoStack, Urb);

    IoStack->CompletionRoutine = UaspTransferCompletion;
    IoStack->Context = Transfer;
    IoStack->Control = SL_INVOKE_ON_SUCCESS | SL_INVOKE_ON_ERROR | SL_INVOKE_ON_CANCEL;

    IoCallDriver(Adapter->LowerDeviceObject, Transfer->Irp);
}

/**
 * @brief Posts the data stage of a command.
 */
static
VOID
UaspSubmitData(
    _In_ PUASP_REQUEST Request)
{
    PUASP_ADAPTER_EXTENSION Adapter = Request->Adapter;
    USBD_PIPE_HANDLE Pipe;
    USBD_PIPE_HANDLE ResetPipe;
    ULONG Flags = 0;

    if (Request->DataIn)
    {
        ResetPipe = Adapter->DataInPipe;
        Pipe = Adapter->StreamsOpen ? Request->DataInStream : Adapter->DataInPipe;
        Flags = USBD_SHORT_TRANSFER_OK;
    }
    else
    {
        ResetPipe = Adapter->DataOutPipe;
        Pipe = Adapter->StreamsOpen ? Request->DataOutStream : Adapter->DataOutPipe;
    }

    UaspSubmitTransfer(Request,
                       UaspTransferData,
                       Pipe,
                       ResetPipe,
                       Request->DataBuffer,
                       Request->DataMdl,
                       Request->DataLength,
                       Flags,
                       UaspDataTransferDone);
}

/**
 * @brief Posts a read on the status pipe, which is where every answer arrives.
 */
static
VOID
UaspSubmitStatusRead(
    _In_ PUASP_REQUEST Request,
    _In_ UASP_TRANSFER_KIND Kind,
    _In_ PVOID Buffer,
    _In_ PUASP_TRANSFER_COMPLETION Completion)
{
    PUASP_ADAPTER_EXTENSION Adapter = Request->Adapter;
    USBD_PIPE_HANDLE Pipe;

    Pipe = Adapter->StreamsOpen ? Request->StatusStream : Adapter->StatusPipe;

    UaspSubmitTransfer(Request,
                       Kind,
                       Pipe,
                       Adapter->StatusPipe,
                       Buffer,
                       NULL,
                       UASP_STATUS_IU_LENGTH,
                       USBD_SHORT_TRANSFER_OK,
                       Completion);
}

/**
 * @brief Fills in the command information unit and sends it.
 */
static
VOID
UaspSubmitCommand(
    _In_ PUASP_REQUEST Request)
{
    PUASP_ADAPTER_EXTENSION Adapter = Request->Adapter;
    PUAS_COMMAND_IU Command = Request->CommandIu;
    PVOID Srb = Request->Srb;
    PCDB Cdb = SrbGetCdb(Srb);
    UCHAR CdbLength = SrbGetCdbLength(Srb);

    RtlZeroMemory(Command, sizeof(*Command));

    Command->Header.Id = UAS_IU_COMMAND;
    UaspStoreBigEndian16(Command->Header.Tag, Request->Tag);
    Command->Attributes = UAS_TASK_ATTRIBUTE_SIMPLE;

    /*
     * The logical unit travels in the architecture model format: a single
     * level address with the unit in the second byte.
     */
    Command->Lun[1] = SrbGetLun(Srb);

    if (CdbLength > sizeof(Command->Cdb))
        CdbLength = sizeof(Command->Cdb);

    if (Cdb != NULL && CdbLength != 0)
        RtlCopyMemory(Command->Cdb, Cdb, CdbLength);

    UaspSubmitTransfer(Request,
                       UaspTransferCommand,
                       Adapter->CommandPipe,
                       Adapter->CommandPipe,
                       Command,
                       NULL,
                       sizeof(*Command),
                       0,
                       UaspCommandTransferDone);
}

/**
 * @brief Works out what the device said about a command it has finished.
 *
 * Two layouts of the sense information unit are in the field: the one this
 * protocol shipped with, and the draft that came before it. They are told
 * apart by which of the two length fields agrees with the bytes received.
 */
static
UCHAR
UaspProcessStatusIu(
    _In_ PUASP_REQUEST Request,
    _In_ PVOID Iu,
    _In_ ULONG Received)
{
    PUAS_SENSE_IU_DRAFT Draft = Iu;
    PUAS_SENSE_IU Sense = Iu;
    PUAS_RESPONSE_IU Response = Iu;
    PUCHAR SenseData;
    PVOID SenseBuffer;
    USHORT SenseLength;
    UCHAR SenseBufferLength;
    UCHAR ScsiStatus;
    PVOID Srb = Request->Srb;
    UCHAR Id = *(PUCHAR)Iu;

    if (Id == UAS_IU_RESPONSE)
    {
        if (Received < sizeof(UAS_RESPONSE_IU))
            return SRB_STATUS_ERROR;

        switch (Response->ResponseCode)
        {
            case UAS_RESPONSE_TMF_COMPLETE:
            case UAS_RESPONSE_TMF_SUCCEEDED:
                return SRB_STATUS_SUCCESS;

            case UAS_RESPONSE_INCORRECT_LUN:
                return SRB_STATUS_INVALID_LUN;

            case UAS_RESPONSE_INVALID_IU:
            case UAS_RESPONSE_TMF_NOT_SUPPORTED:
            case UAS_RESPONSE_TMF_FAILED:
                return SRB_STATUS_ABORTED;

            default:
                DPRINT1("Response code %u for tag %u\n", Response->ResponseCode, Request->Tag);
                return SRB_STATUS_ERROR;
        }
    }

    if (Id != UAS_IU_SENSE)
    {
        DPRINT1("Information unit %u on the status pipe for tag %u\n", Id, Request->Tag);
        return SRB_STATUS_ERROR;
    }

    if (Received < FIELD_OFFSET(UAS_SENSE_IU, SenseData))
    {
        /* Short of even the header, so there is nothing to read out of it */
        if (Received < FIELD_OFFSET(UAS_SENSE_IU_DRAFT, SenseData))
            return SRB_STATUS_ERROR;

        ScsiStatus = Draft->Status;
        SenseLength = UaspReadBigEndian16(Draft->Length);
        SenseData = Draft->SenseData;
    }
    else if (UaspReadBigEndian16(Sense->Length) ==
             Received - FIELD_OFFSET(UAS_SENSE_IU, SenseData))
    {
        ScsiStatus = Sense->Status;
        SenseLength = UaspReadBigEndian16(Sense->Length);
        SenseData = Sense->SenseData;
    }
    else if (UaspReadBigEndian16(Draft->Length) ==
             Received - FIELD_OFFSET(UAS_SENSE_IU_DRAFT, SenseData))
    {
        ScsiStatus = Draft->Status;
        SenseLength = UaspReadBigEndian16(Draft->Length);
        SenseData = Draft->SenseData;
    }
    else
    {
        /* Neither layout accounts for what arrived, so trust none of it */
        DPRINT1("Sense unit of %lu bytes matches no layout, tag %u\n", Received, Request->Tag);
        return SRB_STATUS_ERROR;
    }

    SrbSetScsiStatus(Srb, ScsiStatus);

    if (ScsiStatus == SCSISTAT_GOOD)
        return SRB_STATUS_SUCCESS;

    if (ScsiStatus == SCSISTAT_BUSY || ScsiStatus == SCSISTAT_QUEUE_FULL)
        return SRB_STATUS_BUSY;

    if (ScsiStatus != SCSISTAT_CHECK_CONDITION || SenseLength == 0)
        return SRB_STATUS_ERROR;

    SenseBuffer = SrbGetSenseInfoBuffer(Srb);
    SenseBufferLength = SrbGetSenseInfoBufferLength(Srb);

    if (SenseBuffer == NULL || SenseBufferLength == 0)
        return SRB_STATUS_ERROR;

    if (SenseLength > SenseBufferLength)
        SenseLength = SenseBufferLength;

    RtlCopyMemory(SenseBuffer, SenseData, SenseLength);

    return SRB_STATUS_ERROR | SRB_STATUS_AUTOSENSE_VALID;
}

static
VOID
UaspCommandTransferDone(
    _In_ PUASP_REQUEST Request,
    _In_ NTSTATUS Status)
{
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Command for tag %u never went out (Status 0x%08lx)\n", Request->Tag, Status);

        UaspFailRequest(Request, UaspTranslateStatus(Status));

        /*
         * Nothing is coming back for a command the device never saw, so the
         * reads waiting on it are taken away rather than left to time out.
         */
        IoCancelIrp(Request->Transfer[UaspTransferStatus].Irp);

        if (Request->HasData)
        {
            IoCancelIrp(Request->Transfer[UaspTransferReady].Irp);
            IoCancelIrp(Request->Transfer[UaspTransferData].Irp);
        }
    }

    UaspFinishTransfer(Request);
}

static
VOID
UaspDataTransferDone(
    _In_ PUASP_REQUEST Request,
    _In_ NTSTATUS Status)
{
    PURB Urb = Request->Transfer[UaspTransferData].Urb;

    if (NT_SUCCESS(Status))
        Request->DataTransferred = Urb->UrbBulkOrInterruptTransfer.TransferBufferLength;
    else
        DPRINT1("Data stage of tag %u failed (Status 0x%08lx)\n", Request->Tag, Status);

    UaspFinishTransfer(Request);
}

static
VOID
UaspStatusTransferDone(
    _In_ PUASP_REQUEST Request,
    _In_ NTSTATUS Status)
{
    PURB Urb = Request->Transfer[UaspTransferStatus].Urb;
    UCHAR SrbStatus;
    ULONG Received;

    if (!NT_SUCCESS(Status))
    {
        UaspFailRequest(Request, UaspTranslateStatus(Status));
        UaspFinishTransfer(Request);
        return;
    }

    Received = Urb->UrbBulkOrInterruptTransfer.TransferBufferLength;

    SrbStatus = UaspProcessStatusIu(Request, Request->StatusIu, Received);

    if (SrbStatus == SRB_STATUS_SUCCESS && Request->HasData &&
        Request->DataTransferred != Request->DataLength)
    {
        SrbStatus = SRB_STATUS_DATA_OVERRUN;
    }

    Request->SrbStatus = SrbStatus;

    UaspFinishTransfer(Request);
}

/**
 * @brief Reads the unit that says the device is ready for the data stage.
 *
 * Only a device without streams sends one. It may also answer the command
 * outright, without ever asking for data, and that lands here because it is
 * the first read queued on the pipe.
 */
static
VOID
UaspReadyTransferDone(
    _In_ PUASP_REQUEST Request,
    _In_ NTSTATUS Status)
{
    PURB Urb = Request->Transfer[UaspTransferReady].Urb;
    ULONG Received;
    UCHAR Id;

    if (!NT_SUCCESS(Status))
    {
        UaspFailRequest(Request, UaspTranslateStatus(Status));

        IoCancelIrp(Request->Transfer[UaspTransferStatus].Irp);
        UaspFinishTransfer(Request);
        return;
    }

    Received = Urb->UrbBulkOrInterruptTransfer.TransferBufferLength;
    Id = *(PUCHAR)Request->ReadyIu;

    if (Id == UAS_IU_READ_READY || Id == UAS_IU_WRITE_READY)
    {
        /*
         * The data stage only becomes one of the answers to wait for once it
         * has been asked for. Counting it before this transfer is retired
         * keeps the request from looking finished in between.
         */
        InterlockedIncrement(&Request->Outstanding);

        UaspSubmitData(Request);
        UaspFinishTransfer(Request);
        return;
    }

    /*
     * The command is over before the data stage, so this is the answer to it
     * and the read still queued behind it will never be satisfied.
     */
    Request->SrbStatus = UaspProcessStatusIu(Request, Request->ReadyIu, Received);

    IoCancelIrp(Request->Transfer[UaspTransferStatus].Irp);

    UaspFinishTransfer(Request);
}

/**
 * @brief Works out where the buffer of a command lives.
 */
static
BOOLEAN
UaspResolveDataBuffer(
    _In_ PUASP_ADAPTER_EXTENSION Adapter,
    _In_ PUASP_REQUEST Request)
{
    PSCSI_REQUEST_BLOCK Srb = Request->Srb;
    PVOID Address;
    PVOID Mdl;

    Request->DataBuffer = NULL;
    Request->DataMdl = NULL;

    /*
     * The packet the request came in on describes the buffer, which is what
     * the controller wants, and saves mapping it.
     */
    if (StorPortGetOriginalMdl(Adapter, Srb, &Mdl) == STOR_STATUS_SUCCESS)
    {
        Request->DataMdl = Mdl;
        return TRUE;
    }

    if (StorPortGetSystemAddress(Adapter, Srb, &Address) == STOR_STATUS_SUCCESS)
    {
        Request->DataBuffer = Address;
        return TRUE;
    }

    DPRINT1("No buffer behind a request of %lu bytes\n", Request->DataLength);

    return FALSE;
}

/**
 * @brief Sends one command from the class layer to the device.
 *
 * With streams every transfer of the command rides on the stream its tag
 * names, so the data stage can be posted before the command itself. Without
 * them the device asks for the data when it wants it, and the request waits
 * for that.
 */
VOID
UaspIssueCommand(
    _In_ PUASP_ADAPTER_EXTENSION Adapter,
    _In_ PVOID Srb)
{
    PUASP_REQUEST Request;
    ULONG SrbFlags;

    Request = UaspAcquireRequest(Adapter);
    if (Request == NULL)
    {
        UaspCompleteSrb(Adapter, Srb, SRB_STATUS_BUSY);
        return;
    }

    SrbFlags = SrbGetSrbFlags(Srb);

    Request->Srb = Srb;
    Request->SrbStatus = SRB_STATUS_ERROR;
    Request->DataTransferred = 0;
    Request->DataLength = SrbGetDataTransferLength(Srb);
    Request->DataIn = (SrbFlags & SRB_FLAGS_DATA_IN) != 0;
    Request->HasData = ((SrbFlags & (SRB_FLAGS_DATA_IN | SRB_FLAGS_DATA_OUT)) != 0) &&
                       (Request->DataLength != 0);

    if (Request->HasData && !UaspResolveDataBuffer(Adapter, Request))
    {
        UaspReleaseRequest(Request);
        UaspCompleteSrb(Adapter, Srb, SRB_STATUS_ERROR);
        return;
    }

    /*
     * The command itself and the answer to it. With streams the data stage
     * is posted here too, so it counts from the start; without them it only
     * counts once the device has asked for it.
     */
    Request->Outstanding = 2;

    if (Request->HasData)
        Request->Outstanding++;

    /*
     * Order matters on a device without streams: both reads share the one
     * pipe, and the device sends the request for data before the answer.
     */
    if (Request->HasData)
    {
        if (Adapter->StreamsOpen)
        {
            UaspSubmitData(Request);
        }
        else
        {
            UaspSubmitStatusRead(Request,
                                 UaspTransferReady,
                                 Request->ReadyIu,
                                 UaspReadyTransferDone);
        }
    }

    UaspSubmitStatusRead(Request,
                         UaspTransferStatus,
                         Request->StatusIu,
                         UaspStatusTransferDone);

    UaspSubmitCommand(Request);
}
