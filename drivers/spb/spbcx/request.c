/*
 * PROJECT:     ReactOS Simple Peripheral Bus
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SpbCx request object
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

/*
 * A request carries the caller's transfer list, which points at the caller's
 * buffers. Those are only mapped while still in the caller's context, so the
 * list is captured and its buffers locked in EvtIoInCallerContext before the
 * request is ever queued; by the time a controller driver sees it, everything it
 * describes is an MDL that is safe at DISPATCH_LEVEL.
 */

#define NDEBUG
#include "spbcxp.h"

/* Moved to capture.c, where the transfer-list walk it needs also lives. */

SPBTARGET
NTAPI
ScxRequestGetTarget(
    _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
    _In_ SPBREQUEST SpbRequest)
{
    PSCX_REQUEST Request;

    if (ScxGlobalsFromClient(DriverGlobals) == NULL || SpbRequest == NULL)
    {
        return NULL;
    }

    Request = ScxGetRequestContext((WDFREQUEST)SpbRequest);
    if (Request == NULL || Request->Target == NULL)
    {
        return NULL;
    }

    return (SPBTARGET)Request->Target->FileObject;
}

WDFDEVICE
NTAPI
ScxRequestGetController(
    _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
    _In_ SPBREQUEST SpbRequest)
{
    PSCX_REQUEST Request;

    if (ScxGlobalsFromClient(DriverGlobals) == NULL || SpbRequest == NULL)
    {
        return NULL;
    }

    Request = ScxGetRequestContext((WDFREQUEST)SpbRequest);
    if (Request == NULL || Request->Controller == NULL)
    {
        return NULL;
    }

    return Request->Controller->Device;
}

VOID
NTAPI
ScxRequestGetParameters(
    _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
    _In_ SPBREQUEST SpbRequest,
    _Out_ SPB_REQUEST_PARAMETERS *Parameters)
{
    PSCX_REQUEST Request;

    if (ScxGlobalsFromClient(DriverGlobals) == NULL ||
        SpbRequest == NULL || Parameters == NULL)
    {
        return;
    }

    if (Parameters->Size != sizeof(SPB_REQUEST_PARAMETERS))
    {
        return;
    }

    Request = ScxGetRequestContext((WDFREQUEST)SpbRequest);
    if (Request == NULL)
    {
        return;
    }

    *Parameters = Request->Parameters;
}

/*
 * How a controller driver walks a sequence: it is handed the transfer count in
 * EvtSpbIoSequence and asks for each one in turn. The MDL is the captured,
 * page-locked description of the caller's buffer, so it is safe to touch at
 * DISPATCH_LEVEL; an index past the end yields a zeroed descriptor and a NULL
 * MDL rather than a failure, which is how a driver detects the end.
 */
VOID
NTAPI
ScxRequestGetTransferParameters(
    _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
    _In_ SPBREQUEST SpbRequest,
    _In_ ULONG Index,
    _Out_opt_ SPB_TRANSFER_DESCRIPTOR *TransferDescriptor,
    _Out_opt_ PMDL *TransferBuffer)
{
    PSPB_TRANSFER_LIST_ENTRY Entry;
    PSCX_REQUEST Request;

    if (ScxGlobalsFromClient(DriverGlobals) == NULL || SpbRequest == NULL)
    {
        return;
    }

    Request = ScxGetRequestContext((WDFREQUEST)SpbRequest);
    if (Request == NULL)
    {
        return;
    }

    if (TransferBuffer != NULL)
    {
        *TransferBuffer = NULL;
    }

    if (TransferDescriptor != NULL)
    {
        if (TransferDescriptor->Size != sizeof(SPB_TRANSFER_DESCRIPTOR))
        {
            return;
        }

        SPB_TRANSFER_DESCRIPTOR_INIT(TransferDescriptor);
    }

    if (Request->TransferList == NULL || Index >= Request->TransferCount)
    {
        return;
    }

    Entry = &Request->TransferList->Transfers[Index];

    if (TransferDescriptor != NULL)
    {
        TransferDescriptor->Direction = Entry->Direction;
        TransferDescriptor->DelayInUs = Entry->DelayInUs;

        switch (Entry->Buffer.Format)
        {
            case SpbTransferBufferFormatSimple:
            case SpbTransferBufferFormatSimpleNonPaged:
                TransferDescriptor->TransferLength = Entry->Buffer.Simple.BufferCb;
                break;

            case SpbTransferBufferFormatMdl:
                TransferDescriptor->TransferLength =
                    MmGetMdlByteCount(Entry->Buffer.Mdl);
                break;

            default:
                break;
        }
    }

    if (TransferBuffer != NULL)
    {
        /*
         * A caller-supplied MDL was never copied, so hand back the original;
         * everything else is the one capture.c built and locked.
         */
        *TransferBuffer = (Entry->Buffer.Format == SpbTransferBufferFormatMdl)
                              ? Entry->Buffer.Mdl
                              : Request->TransferMdls[Index];
    }
}


/*
 * The controller driver calls this when its transfer is done. Information is the
 * byte count the request accumulated, which for a sequence is the sum across
 * every transfer, not just the last.
 */
VOID
NTAPI
ScxRequestComplete(
    _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
    _In_ SPBREQUEST SpbRequest,
    _In_ NTSTATUS CompletionStatus)
{
    if (ScxGlobalsFromClient(DriverGlobals) == NULL || SpbRequest == NULL)
    {
        return;
    }

    WdfRequestComplete((WDFREQUEST)SpbRequest, CompletionStatus);
}

/*
 * NOT YET BUILT, and it lands with the capture path it is named after: a client
 * handling a private IOCTL calls this to have us capture a transfer list it
 * found in its own buffer.
 */
NTSTATUS
NTAPI
ScxRequestCaptureIoOtherTransferList(
    _In_ PSPB_DRIVER_GLOBALS DriverGlobals,
    _In_ SPBREQUEST SpbRequest)
{
    if (ScxGlobalsFromClient(DriverGlobals) == NULL || SpbRequest == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_NOT_IMPLEMENTED;
}
