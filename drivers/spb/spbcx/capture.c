/*
 * PROJECT:     ReactOS Simple Peripheral Bus
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Capturing a caller's transfer list before it is queued
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

/*
 * The buffers an SPB_TRANSFER_LIST points at are only mapped in the caller's
 * context, so the list is copied and every buffer locked behind an MDL before
 * the request is queued.
 */

#include "spbcxp.h"
#include <pseh/pseh2.h>

#define NDEBUG
#include <debug.h>

VOID
NTAPI
ScxReleaseTransfers(
    _Inout_ PSCX_REQUEST Request)
{
    ULONG Index;
    PMDL Mdl;

    if (Request->TransferMdls != NULL)
    {
        for (Index = 0; Index < Request->TransferCount; Index++)
        {
            Mdl = Request->TransferMdls[Index];
            if (Mdl == NULL)
                continue;

            MmUnlockPages(Mdl);
            IoFreeMdl(Mdl);
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

/* Runs for completion and cancellation alike */
VOID
NTAPI
ScxEvtRequestCleanup(
    _In_ WDFOBJECT Object)
{
    PSCX_REQUEST Request = ScxGetRequestContext((WDFREQUEST)Object);

    if (Request != NULL)
        ScxReleaseTransfers(Request);
}

/**
 * @brief Describes a caller buffer with an MDL and locks it.
 *
 * A transfer from the device writes into the buffer, so it is probed for write.
 */
static
NTSTATUS
NTAPI
ScxLockTransferBuffer(
    _In_ PVOID Buffer,
    _In_ ULONG Length,
    _In_ SPB_TRANSFER_DIRECTION Direction,
    _In_ KPROCESSOR_MODE AccessMode,
    _Outptr_ PMDL *LockedMdl)
{
    LOCK_OPERATION Operation;
    NTSTATUS Status = STATUS_SUCCESS;
    PMDL Mdl;

    *LockedMdl = NULL;

    Mdl = IoAllocateMdl(Buffer, Length, FALSE, FALSE, NULL);
    if (Mdl == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    if (Direction == SpbTransferDirectionFromDevice)
        Operation = IoWriteAccess;
    else
        Operation = IoReadAccess;

    _SEH2_TRY
    {
        MmProbeAndLockPages(Mdl, AccessMode, Operation);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    if (!NT_SUCCESS(Status))
    {
        IoFreeMdl(Mdl);
        return Status;
    }

    *LockedMdl = Mdl;
    return STATUS_SUCCESS;
}

/**
 * @brief Validates one entry and locks its buffer.
 *
 * A caller supplied MDL is used as is and not recorded, since it stays the
 * caller's. Only kernel callers may pass an MDL or claim a non-paged buffer.
 */
static
NTSTATUS
NTAPI
ScxCaptureTransferEntry(
    _In_ PSPB_TRANSFER_LIST_ENTRY Entry,
    _In_ KPROCESSOR_MODE AccessMode,
    _Outptr_result_maybenull_ PMDL *LockedMdl)
{
    *LockedMdl = NULL;

    if (Entry->Direction != SpbTransferDirectionFromDevice &&
        Entry->Direction != SpbTransferDirectionToDevice)
    {
        return STATUS_INVALID_PARAMETER;
    }

    switch (Entry->Buffer.Format)
    {
        case SpbTransferBufferFormatSimpleNonPaged:
            if (AccessMode != KernelMode)
                return STATUS_INVALID_PARAMETER;

            /* Fall through */
        case SpbTransferBufferFormatSimple:
            if (Entry->Buffer.Simple.Buffer == NULL || Entry->Buffer.Simple.BufferCb == 0)
                return STATUS_INVALID_PARAMETER;

            return ScxLockTransferBuffer(Entry->Buffer.Simple.Buffer,
                                         Entry->Buffer.Simple.BufferCb,
                                         Entry->Direction,
                                         AccessMode,
                                         LockedMdl);

        case SpbTransferBufferFormatMdl:
            if (AccessMode != KernelMode || Entry->Buffer.Mdl == NULL)
                return STATUS_INVALID_PARAMETER;

            return STATUS_SUCCESS;

        case SpbTransferBufferFormatList:
            /* Scatter lists are not supported yet */
            return STATUS_NOT_SUPPORTED;

        default:
            return STATUS_INVALID_PARAMETER;
    }
}

/**
 * @brief Copies the request's input transfer list and locks every buffer in it.
 *
 * The copy is validated, not the caller's buffer, so nothing can change
 * between the check and the use. A failure part way unwinds what was locked.
 */
NTSTATUS
NTAPI
ScxCaptureRequestTransferList(
    _In_ PSCX_REQUEST Request)
{
    PSPB_TRANSFER_LIST CallerList;
    PSPB_TRANSFER_LIST List;
    size_t ListLength;
    size_t Required;
    KPROCESSOR_MODE AccessMode;
    ULONG Count;
    ULONG Index;
    NTSTATUS Status;

    Status = WdfRequestRetrieveInputBuffer(Request->FxRequest,
                                           sizeof(*CallerList),
                                           (PVOID *)&CallerList,
                                           &ListLength);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Internal device control always comes from kernel mode */
    AccessMode = WdfRequestGetRequestorMode(Request->FxRequest);

    if (CallerList->Size != sizeof(*CallerList))
        return STATUS_INVALID_PARAMETER;

    Count = CallerList->TransferCount;
    if (Count == 0 || Count > (MAXULONG / sizeof(SPB_TRANSFER_LIST_ENTRY)))
        return STATUS_INVALID_PARAMETER;

    Required = FIELD_OFFSET(SPB_TRANSFER_LIST, Transfers) + ((size_t)Count * sizeof(SPB_TRANSFER_LIST_ENTRY));
    if (ListLength < Required)
        return STATUS_BUFFER_TOO_SMALL;

    List = ExAllocatePoolZero(NonPagedPoolNx, Required, SCX_POOL_TAG);
    if (List == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlCopyMemory(List, CallerList, Required);

    Request->TransferMdls = ExAllocatePoolZero(NonPagedPoolNx, Count * sizeof(PMDL), SCX_POOL_TAG);
    if (Request->TransferMdls == NULL)
    {
        ExFreePoolWithTag(List, SCX_POOL_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Request->TransferList = List;

    for (Index = 0; Index < Count; Index++)
    {
        /* Counting as we go leaves exactly the locked entries for the unwind */
        Request->TransferCount = Index;

        Status = ScxCaptureTransferEntry(&List->Transfers[Index], AccessMode, &Request->TransferMdls[Index]);
        if (!NT_SUCCESS(Status))
        {
            ScxReleaseTransfers(Request);
            return Status;
        }
    }

    Request->TransferCount = Count;
    return STATUS_SUCCESS;
}

/**
 * @brief Attaches the SpbCx context, and the client's if it asked for one.
 */
static
NTSTATUS
NTAPI
ScxAttachRequestContext(
    _In_ PSCX_CONTROLLER Controller,
    _In_ WDFREQUEST FxRequest,
    _Outptr_ PSCX_REQUEST *Context)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    PSCX_REQUEST Request;
    NTSTATUS Status;

    *Context = NULL;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SCX_REQUEST);
    Attributes.EvtCleanupCallback = ScxEvtRequestCleanup;

    Status = WdfObjectAllocateContext(FxRequest, &Attributes, (PVOID *)&Request);
    if (!NT_SUCCESS(Status))
        return Status;

    Request->Controller = Controller;
    Request->FxRequest = FxRequest;

    if (Controller->RequestAttributesSet)
    {
        Status = WdfObjectAllocateContext(FxRequest, &Controller->RequestAttributes, NULL);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    *Context = Request;
    return STATUS_SUCCESS;
}

VOID
NTAPI
ScxEvtIoInCallerContext(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST FxRequest)
{
    PSCX_CONTROLLER Controller = ScxGetControllerContext(Device);
    WDF_REQUEST_PARAMETERS Parameters;
    PSCX_REQUEST Request;
    NTSTATUS Status;

    if (Controller == NULL)
    {
        WdfRequestComplete(FxRequest, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }

    Status = ScxAttachRequestContext(Controller, FxRequest, &Request);
    if (!NT_SUCCESS(Status))
    {
        WdfRequestComplete(FxRequest, Status);
        return;
    }

    WDF_REQUEST_PARAMETERS_INIT(&Parameters);
    WdfRequestGetParameters(FxRequest, &Parameters);

    if (Parameters.Type == WdfRequestTypeDeviceControl ||
        Parameters.Type == WdfRequestTypeDeviceControlInternal)
    {
        switch (Parameters.Parameters.DeviceIoControl.IoControlCode)
        {
            case IOCTL_SPB_EXECUTE_SEQUENCE:
                Status = ScxCaptureRequestTransferList(Request);
                if (!NT_SUCCESS(Status))
                {
                    DPRINT1("SpbCx: transfer list capture failed 0x%lx\n", Status);
                    WdfRequestComplete(FxRequest, Status);
                    return;
                }

                SPB_REQUEST_PARAMETERS_INIT(&Request->Parameters);
                Request->Parameters.Type = SpbRequestTypeSequence;
                Request->Parameters.Position = SpbRequestSequencePositionSingle;
                Request->Parameters.SequenceTransferCount = Request->TransferCount;
                break;

            case IOCTL_SPB_LOCK_CONTROLLER:
            case IOCTL_SPB_UNLOCK_CONTROLLER:
            case IOCTL_SPB_LOCK_CONNECTION:
            case IOCTL_SPB_UNLOCK_CONNECTION:
                break;

            default:
                /* A client that took the IoOther callback handles its own buffers */
                if (Controller->EvtIoInCallerContext != NULL)
                {
                    Controller->EvtIoInCallerContext(Device, FxRequest);
                    return;
                }
                break;
        }
    }

    Status = WdfDeviceEnqueueRequest(Device, FxRequest);
    if (!NT_SUCCESS(Status))
        WdfRequestComplete(FxRequest, Status);
}
