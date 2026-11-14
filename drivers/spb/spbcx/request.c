/*
 * PROJECT:     ReactOS Simple Peripheral Bus
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SpbCx request object
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "spbcxp.h"

#define NDEBUG
#include <debug.h>

/**
 * @brief Validates the caller's globals and returns the context of an SPBREQUEST.
 */
static
PSCX_REQUEST
NTAPI
ScxRequestFromClient(
    _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
    _In_ SPBREQUEST SpbRequest)
{
    if (ScxGlobalsFromClient(DriverGlobals) == NULL || SpbRequest == NULL)
        return NULL;

    return ScxGetRequestContext((WDFREQUEST)SpbRequest);
}

SPBTARGET
NTAPI
ScxRequestGetTarget(
    _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
    _In_ SPBREQUEST SpbRequest)
{
    PSCX_REQUEST Request = ScxRequestFromClient(DriverGlobals, SpbRequest);

    if (Request == NULL || Request->Target == NULL)
        return NULL;

    return (SPBTARGET)Request->Target->FileObject;
}

WDFDEVICE
NTAPI
ScxRequestGetController(
    _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
    _In_ SPBREQUEST SpbRequest)
{
    PSCX_REQUEST Request = ScxRequestFromClient(DriverGlobals, SpbRequest);

    if (Request == NULL || Request->Controller == NULL)
        return NULL;

    return Request->Controller->Device;
}

VOID
NTAPI
ScxRequestGetParameters(
    _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
    _In_ SPBREQUEST SpbRequest,
    _Inout_ SPB_REQUEST_PARAMETERS *Parameters)
{
    PSCX_REQUEST Request = ScxRequestFromClient(DriverGlobals, SpbRequest);

    if (Request == NULL || Parameters == NULL || Parameters->Size != sizeof(*Parameters))
        return;

    if (Request->Parameters.Size == 0)
        return;

    *Parameters = Request->Parameters;
}

/**
 * @brief Describes one transfer of a request.
 *
 * A read or write is transfer 0 over the request buffer. An index past the
 * end leaves a zeroed descriptor and a NULL MDL.
 */
VOID
NTAPI
ScxRequestGetTransferParameters(
    _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
    _In_ SPBREQUEST SpbRequest,
    _In_ ULONG Index,
    _Inout_opt_ SPB_TRANSFER_DESCRIPTOR *TransferDescriptor,
    _Out_opt_ PMDL *TransferBuffer)
{
    PSCX_REQUEST Request = ScxRequestFromClient(DriverGlobals, SpbRequest);
    PSPB_TRANSFER_LIST_ENTRY Entry;
    PMDL Mdl = NULL;

    if (Request == NULL)
        return;

    if (TransferDescriptor != NULL)
    {
        if (TransferDescriptor->Size != sizeof(*TransferDescriptor))
            return;

        SPB_TRANSFER_DESCRIPTOR_INIT(TransferDescriptor);
    }

    if (Request->TransferList != NULL && Index < Request->TransferCount)
    {
        Entry = &Request->TransferList->Transfers[Index];

        /* A caller supplied MDL was never copied */
        if (Entry->Buffer.Format == SpbTransferBufferFormatMdl)
            Mdl = Entry->Buffer.Mdl;
        else
            Mdl = Request->TransferMdls[Index];

        if (TransferDescriptor != NULL)
        {
            TransferDescriptor->Direction = Entry->Direction;
            TransferDescriptor->DelayInUs = Entry->DelayInUs;
            TransferDescriptor->TransferLength = (Mdl != NULL) ? MmGetMdlByteCount(Mdl) : 0;
        }
    }
    else if (Request->BufferMdl != NULL && Index == 0)
    {
        Mdl = Request->BufferMdl;

        if (TransferDescriptor != NULL)
        {
            TransferDescriptor->Direction = Request->BufferDirection;
            TransferDescriptor->TransferLength = Request->Parameters.Length;
        }
    }

    if (TransferBuffer != NULL)
        *TransferBuffer = Mdl;
}

VOID
NTAPI
ScxRequestComplete(
    _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
    _In_ SPBREQUEST SpbRequest,
    _In_ NTSTATUS CompletionStatus)
{
    if (ScxGlobalsFromClient(DriverGlobals) == NULL || SpbRequest == NULL)
        return;

    WdfRequestComplete((WDFREQUEST)SpbRequest, CompletionStatus);
}

/**
 * @brief Captures a transfer list carried by a client private IOCTL.
 *
 * Called from the client's in caller context callback, so the buffers are
 * still mapped. The standard SPB IOCTLs are not eligible.
 */
NTSTATUS
NTAPI
ScxRequestCaptureIoOtherTransferList(
    _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
    _In_ SPBREQUEST SpbRequest)
{
    PSCX_REQUEST Request = ScxRequestFromClient(DriverGlobals, SpbRequest);
    WDF_REQUEST_PARAMETERS Parameters;
    NTSTATUS Status;

    if (Request == NULL || Request->TransferList != NULL)
        return STATUS_INVALID_PARAMETER;

    WDF_REQUEST_PARAMETERS_INIT(&Parameters);
    WdfRequestGetParameters((WDFREQUEST)SpbRequest, &Parameters);

    if (Parameters.Type != WdfRequestTypeDeviceControl &&
        Parameters.Type != WdfRequestTypeDeviceControlInternal)
    {
        return STATUS_INVALID_PARAMETER;
    }

    switch (Parameters.Parameters.DeviceIoControl.IoControlCode)
    {
        case IOCTL_SPB_EXECUTE_SEQUENCE:
        case IOCTL_SPB_LOCK_CONTROLLER:
        case IOCTL_SPB_UNLOCK_CONTROLLER:
        case IOCTL_SPB_LOCK_CONNECTION:
        case IOCTL_SPB_UNLOCK_CONNECTION:
            return STATUS_INVALID_PARAMETER;

        default:
            break;
    }

    Status = ScxCaptureRequestTransferList(Request);
    if (!NT_SUCCESS(Status))
        return Status;

    SPB_REQUEST_PARAMETERS_INIT(&Request->Parameters);
    Request->Parameters.Type = SpbRequestTypeOther;
    Request->Parameters.Position = SpbRequestSequencePositionSingle;
    Request->Parameters.SequenceTransferCount = Request->TransferCount;

    return STATUS_SUCCESS;
}
