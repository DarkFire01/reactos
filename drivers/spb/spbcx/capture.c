/*
 * PROJECT:     ReactOS Simple Peripheral Bus
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Capturing a caller's transfer list before it is queued
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

/*
 * An SPB_TRANSFER_LIST points at the caller's buffers. Those are only mapped
 * while we are still in the caller's context and only stay put while their pages
 * are locked, so the list is copied and every buffer in it is described by an MDL
 * here, before the request is ever queued. By the time a controller driver sees
 * the request it is holding MDLs it can touch at DISPATCH_LEVEL.
 *
 * Only IOCTL_SPB_EXECUTE_SEQUENCE carries a transfer list. Reads and writes hand
 * WDF a buffer directly, and the lock/unlock IOCTLs carry nothing at all, so the
 * reference lets all of those through untouched, as does this.
 */

#define NDEBUG
#include "spbcxp.h"

/*
 * Runs from the request's cleanup callback, so it covers completion and
 * cancellation alike; there is no path where a locked page outlives the request.
 */
VOID
ScxReleaseTransfers(
    _In_ PSCX_REQUEST Request)
{
    ULONG Index;

    if (Request->TransferMdls != NULL)
    {
        for (Index = 0; Index < Request->TransferCount; Index++)
        {
            PMDL Mdl = Request->TransferMdls[Index];

            if (Mdl != NULL)
            {
                /* Only the ones we built and locked; a caller-supplied MDL is theirs */
                if ((Mdl->MdlFlags & MDL_PAGES_LOCKED) != 0)
                {
                    MmUnlockPages(Mdl);
                }

                IoFreeMdl(Mdl);
                Request->TransferMdls[Index] = NULL;
            }
        }

        ExFreePoolWithTag(Request->TransferMdls, SCX_POOL_TAG);
        Request->TransferMdls = NULL;
    }

    if (Request->TransferList != NULL)
    {
        ExFreePoolWithTag(Request->TransferList, SCX_POOL_TAG);
        Request->TransferList = NULL;
    }

    Request->TransferCount = 0;
}

VOID
NTAPI
ScxEvtRequestCleanup(
    _In_ WDFOBJECT Object)
{
    PSCX_REQUEST Request = ScxGetRequestContext((WDFREQUEST)Object);

    if (Request != NULL)
    {
        ScxReleaseTransfers(Request);
    }
}

/*
 * A caller-supplied MDL is taken as-is; anything else gets one built and its
 * pages locked. The direction decides how: a read has the device writing into
 * the buffer, so the pages must be probed for write access.
 */
static
NTSTATUS
ScxBuildMdlForBuffer(
    _In_ PVOID Buffer,
    _In_ ULONG Length,
    _In_ SPB_TRANSFER_DIRECTION Direction,
    _In_ KPROCESSOR_MODE AccessMode,
    _Outptr_ PMDL *OutMdl)
{
    LOCK_OPERATION Operation;
    PMDL Mdl;

    *OutMdl = NULL;

    Mdl = IoAllocateMdl(Buffer, Length, FALSE, FALSE, NULL);
    if (Mdl == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Operation = (Direction == SpbTransferDirectionFromDevice) ? IoWriteAccess
                                                              : IoReadAccess;

    _SEH2_TRY
    {
        MmProbeAndLockPages(Mdl, AccessMode, Operation);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        IoFreeMdl(Mdl);
        _SEH2_YIELD(return _SEH2_GetExceptionCode());
    }
    _SEH2_END;

    *OutMdl = Mdl;
    return STATUS_SUCCESS;
}

/*
 * Every field is validated before anything is locked, because a bad entry seen
 * halfway through a list would leave the earlier ones locked with nothing owning
 * them yet.
 */
static
NTSTATUS
ScxCaptureTransferEntry(
    _In_ PSPB_TRANSFER_LIST_ENTRY Entry,
    _In_ KPROCESSOR_MODE AccessMode,
    _Outptr_ PMDL *OutMdl)
{
    *OutMdl = NULL;

    /* SpbTransferDirectionNone is not a transfer */
    if (Entry->Direction != SpbTransferDirectionFromDevice &&
        Entry->Direction != SpbTransferDirectionToDevice)
    {
        return STATUS_INVALID_PARAMETER;
    }

    switch (Entry->Buffer.Format)
    {
        case SpbTransferBufferFormatSimple:
            if (Entry->Buffer.Simple.Buffer == NULL ||
                Entry->Buffer.Simple.BufferCb == 0)
            {
                return STATUS_INVALID_PARAMETER;
            }

            return ScxBuildMdlForBuffer(Entry->Buffer.Simple.Buffer,
                                        Entry->Buffer.Simple.BufferCb,
                                        Entry->Direction,
                                        AccessMode,
                                        OutMdl);

        case SpbTransferBufferFormatSimpleNonPaged:
            /*
             * A user-mode caller cannot promise a buffer is non-paged, and
             * cannot hand over an MDL at all, because both would have us trust an
             * address it does not own. The reference refuses these the same way.
             */
            if (AccessMode != KernelMode)
            {
                return STATUS_INVALID_PARAMETER;
            }

            if (Entry->Buffer.Simple.Buffer == NULL ||
                Entry->Buffer.Simple.BufferCb == 0)
            {
                return STATUS_INVALID_PARAMETER;
            }

            return ScxBuildMdlForBuffer(Entry->Buffer.Simple.Buffer,
                                        Entry->Buffer.Simple.BufferCb,
                                        Entry->Direction,
                                        AccessMode,
                                        OutMdl);

        case SpbTransferBufferFormatMdl:
            if (AccessMode != KernelMode || Entry->Buffer.Mdl == NULL)
            {
                return STATUS_INVALID_PARAMETER;
            }

            /*
             * Taken as-is and deliberately not recorded for cleanup: it belongs
             * to the caller, who unmaps it when the request completes.
             */
            *OutMdl = NULL;
            return STATUS_SUCCESS;

        case SpbTransferBufferFormatList:
            /*
             * NOT YET BUILT: a scatter list means chaining one MDL per element,
             * which is BuildMdlForBufferList in the reference. Refused rather
             * than silently transferring only the first element.
             */
            return STATUS_NOT_SUPPORTED;

        default:
            return STATUS_INVALID_PARAMETER;
    }
}

/*
 * The list itself is copied out of the caller's buffer first. Validating in
 * place would leave every field re-readable by the caller between the check and
 * the use.
 */
static
NTSTATUS
ScxCaptureTransferList(
    _In_ PSCX_REQUEST Request,
    _In_reads_bytes_(ListLength) PSPB_TRANSFER_LIST CallerList,
    _In_ size_t ListLength,
    _In_ KPROCESSOR_MODE AccessMode)
{
    PSPB_TRANSFER_LIST List;
    size_t Required;
    ULONG Count;
    ULONG Index;
    NTSTATUS Status;

    if (ListLength < sizeof(SPB_TRANSFER_LIST))
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    if (CallerList->Size != sizeof(SPB_TRANSFER_LIST))
    {
        return STATUS_INVALID_PARAMETER;
    }

    Count = CallerList->TransferCount;
    if (Count == 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Guard the multiply before it can wrap the length check */
    if (Count > (MAXULONG / sizeof(SPB_TRANSFER_LIST_ENTRY)))
    {
        return STATUS_INVALID_PARAMETER;
    }

    Required = FIELD_OFFSET(SPB_TRANSFER_LIST, Transfers) +
               ((size_t)Count * sizeof(SPB_TRANSFER_LIST_ENTRY));
    if (ListLength < Required)
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    List = ExAllocatePoolWithTag(NonPagedPool, Required, SCX_POOL_TAG);
    if (List == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyMemory(List, CallerList, Required);

    Request->TransferMdls = ExAllocatePoolWithTag(NonPagedPool,
                                                  Count * sizeof(PMDL),
                                                  SCX_POOL_TAG);
    if (Request->TransferMdls == NULL)
    {
        ExFreePoolWithTag(List, SCX_POOL_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(Request->TransferMdls, Count * sizeof(PMDL));
    Request->TransferList = List;

    /*
     * TransferCount goes up as each entry succeeds, so a failure part-way leaves
     * exactly the locked entries recorded and ScxReleaseTransfers unwinds them.
     */
    for (Index = 0; Index < Count; Index++)
    {
        Status = ScxCaptureTransferEntry(&List->Transfers[Index],
                                         AccessMode,
                                         &Request->TransferMdls[Index]);
        if (!NT_SUCCESS(Status))
        {
            Request->TransferCount = Index;
            ScxReleaseTransfers(Request);
            return Status;
        }
    }

    Request->TransferCount = Count;
    return STATUS_SUCCESS;
}

/*
 * The reference routes on request type and then on IOCTL, and only
 * IOCTL_SPB_EXECUTE_SEQUENCE gets captured. Everything else either carries no
 * caller buffers or is the client's own business.
 */
VOID
NTAPI
ScxEvtIoInCallerContext(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST FxRequest)
{
    PSCX_CONTROLLER Controller = ScxGetControllerContext(Device);
    WDF_REQUEST_PARAMETERS Parameters;
    WDF_OBJECT_ATTRIBUTES Attributes;
    PSCX_REQUEST Request;
    PSPB_TRANSFER_LIST CallerList;
    KPROCESSOR_MODE AccessMode;
    size_t ListLength;
    NTSTATUS Status;

    WDF_REQUEST_PARAMETERS_INIT(&Parameters);
    WdfRequestGetParameters(FxRequest, &Parameters);

    if (Controller == NULL)
    {
        WdfRequestComplete(FxRequest, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }

    if (Parameters.Type != WdfRequestTypeDeviceControl &&
        Parameters.Type != WdfRequestTypeDeviceControlInternal)
    {
        goto Enqueue;
    }

    switch (Parameters.Parameters.DeviceIoControl.IoControlCode)
    {
        case IOCTL_SPB_EXECUTE_SEQUENCE:
            break;

        case IOCTL_SPB_LOCK_CONTROLLER:
        case IOCTL_SPB_UNLOCK_CONTROLLER:
        case IOCTL_SPB_LOCK_CONNECTION:
        case IOCTL_SPB_UNLOCK_CONNECTION:
            /* No buffers to capture */
            goto Enqueue;

        default:
            /*
             * A client that took the IoOther callback also took responsibility
             * for its own IOCTLs' buffers.
             */
            if (Controller->EvtIoInCallerContext != NULL)
            {
                Controller->EvtIoInCallerContext(Device, FxRequest);
                return;
            }
            goto Enqueue;
    }

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SCX_REQUEST);
    Attributes.EvtCleanupCallback = ScxEvtRequestCleanup;

    Status = WdfObjectAllocateContext(FxRequest, &Attributes, (PVOID *)&Request);
    if (!NT_SUCCESS(Status))
    {
        WdfRequestComplete(FxRequest, Status);
        return;
    }

    RtlZeroMemory(Request, sizeof(SCX_REQUEST));
    Request->Controller = Controller;
    Request->FxRequest = FxRequest;

    Status = WdfRequestRetrieveInputBuffer(FxRequest,
                                           sizeof(SPB_TRANSFER_LIST),
                                           (PVOID *)&CallerList,
                                           &ListLength);
    if (!NT_SUCCESS(Status))
    {
        WdfRequestComplete(FxRequest, Status);
        return;
    }

    /*
     * WdfRequestGetRequestorMode says whether the buffers can be trusted. An
     * internal-device-control request always originates in the kernel, which is
     * how a driver like hidi2c is allowed to pass non-paged buffers.
     */
    AccessMode = WdfRequestGetRequestorMode(FxRequest);

    Status = ScxCaptureTransferList(Request, CallerList, ListLength, AccessMode);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("SpbCx: transfer list capture failed 0x%08lX\n", Status);
        WdfRequestComplete(FxRequest, Status);
        return;
    }

    /* What the client will read back through SpbRequestGetParameters */
    SPB_REQUEST_PARAMETERS_INIT(&Request->Parameters);
    Request->Parameters.Type = SpbRequestTypeSequence;
    Request->Parameters.Position = SpbRequestSequencePositionSingle;
    Request->Parameters.SequenceTransferCount = Request->TransferCount;

Enqueue:
    Status = WdfDeviceEnqueueRequest(Device, FxRequest);
    if (!NT_SUCCESS(Status))
    {
        WdfRequestComplete(FxRequest, Status);
    }
}
