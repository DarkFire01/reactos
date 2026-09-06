/*
 * PROJECT:     ReactOS Universal Serial Bus Bulk Storage Driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     USB block storage device driver.
 * COPYRIGHT:   2005-2006 James Tabor
 *              2011-2012 Michael Martin (michael.martin@reactos.org)
 *              2011-2013 Johannes Anderwald (johannes.anderwald@reactos.org)
 */

#include "usbstor.h"

#define NDEBUG
#include <debug.h>


VOID
USBSTOR_QueueInitialize(
    PFDO_DEVICE_EXTENSION FDODeviceExtension)
{
    ASSERT(FDODeviceExtension->Common.IsFDO);
    KeInitializeSpinLock(&FDODeviceExtension->IrpListLock);
    InitializeListHead(&FDODeviceExtension->IrpListHead);
    KeInitializeEvent(&FDODeviceExtension->NoPendingRequests, NotificationEvent, TRUE);
    FDODeviceExtension->QueueBusy = FALSE;
}

VOID
NTAPI
USBSTOR_CancelIo(
    IN  PDEVICE_OBJECT DeviceObject,
    IN  PIRP Irp)
{
    PFDO_DEVICE_EXTENSION FDODeviceExtension;

    FDODeviceExtension = (PFDO_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    ASSERT_IRQL_EQUAL(DISPATCH_LEVEL);
    ASSERT(FDODeviceExtension->Common.IsFDO);

    // this IRP is not in our list here
    // now release the cancel lock
    IoReleaseCancelSpinLock(Irp->CancelIrql);
    Irp->IoStatus.Status = STATUS_CANCELLED;

    USBSTOR_QueueTerminateRequest(DeviceObject, Irp);
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    // this irp owned the queue, hand it over to the next one
    USBSTOR_QueueNextRequest(DeviceObject);
}

VOID
NTAPI
USBSTOR_Cancel(
    IN  PDEVICE_OBJECT DeviceObject,
    IN  PIRP Irp)
{
    PFDO_DEVICE_EXTENSION FDODeviceExtension;

    FDODeviceExtension = (PFDO_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    ASSERT_IRQL_EQUAL(DISPATCH_LEVEL);
    ASSERT(FDODeviceExtension->Common.IsFDO);

    KeAcquireSpinLockAtDpcLevel(&FDODeviceExtension->IrpListLock);
    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
    KeReleaseSpinLockFromDpcLevel(&FDODeviceExtension->IrpListLock);

    IoReleaseCancelSpinLock(Irp->CancelIrql);
    Irp->IoStatus.Status = STATUS_CANCELLED;

    USBSTOR_QueueTerminateRequest(DeviceObject, Irp);
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    // this irp was only waiting in the list and never owned the queue, so it is
    // not ours to hand over - the request which does own it also owns the device
    // queue entry and calls USBSTOR_QueueNextRequest when it completes
}

BOOLEAN
USBSTOR_QueueAddIrp(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PDRIVER_CANCEL CancelRoutine, OldDriverCancel;
    PFDO_DEVICE_EXTENSION FDODeviceExtension;
    BOOLEAN QueueRequest;
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    PSCSI_REQUEST_BLOCK Request = (PSCSI_REQUEST_BLOCK)IoStack->Parameters.Others.Argument1;

    FDODeviceExtension = (PFDO_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    ASSERT(FDODeviceExtension->Common.IsFDO);

    IoMarkIrpPending(Irp);

    // the cancel lock is taken before the list lock, the same order USBSTOR_Cancel
    // uses, so that the queue decision and the cancel routine set for this irp
    // cannot disagree
    IoAcquireCancelSpinLock(&Irp->CancelIrql);
    KeAcquireSpinLockAtDpcLevel(&FDODeviceExtension->IrpListLock);

    // the request can only be started right away when no other request owns the
    // queue and there is nothing waiting in front of it
    QueueRequest = FDODeviceExtension->QueueBusy ||
                   !IsListEmpty(&FDODeviceExtension->IrpListHead) ||
                   BooleanFlagOn(FDODeviceExtension->Flags, USBSTOR_FDO_FLAGS_IRP_LIST_FREEZE);

    if (QueueRequest)
    {
        // add irp to queue
        InsertTailList(&FDODeviceExtension->IrpListHead, &Irp->Tail.Overlay.ListEntry);
        CancelRoutine = USBSTOR_Cancel;
    }
    else
    {
        ASSERT(FDODeviceExtension->ActiveSrb == NULL);

        FDODeviceExtension->ActiveSrb = Request;
        FDODeviceExtension->QueueBusy = TRUE;
        CancelRoutine = USBSTOR_CancelIo;
    }

    FDODeviceExtension->IrpPendingCount++;
    KeClearEvent(&FDODeviceExtension->NoPendingRequests);

    OldDriverCancel = IoSetCancelRoutine(Irp, CancelRoutine);

    KeReleaseSpinLockFromDpcLevel(&FDODeviceExtension->IrpListLock);

    // check if the irp has already been cancelled
    if (Irp->Cancel && OldDriverCancel == NULL)
    {
        // it was cancelled before it had a cancel routine, so nobody ran one for
        // it - cancel it here. this releases the cancel lock and completes the
        // irp, so the caller must not start it
        IoSetCancelRoutine(Irp, NULL);
        CancelRoutine(DeviceObject, Irp);
        return TRUE;
    }

    IoReleaseCancelSpinLock(Irp->CancelIrql);

    DPRINT("QueueRequest: %lu IrpPendingCount %lu\n", QueueRequest, FDODeviceExtension->IrpPendingCount);

    return QueueRequest;
}

//
// Hands the queue to the oldest irp in the list and returns it, or leaves the
// queue idle when there is nothing to start. The queue is busy from the moment
// a request is handed to USBSTOR_StartIo until it is handed over here, which
// happens after that request has been completed - a request arriving from
// within that completion waits in the list instead of starting on top of the
// one which is still being completed. Only the owner of the queue may hand it
// over, everybody else has to leave a busy queue alone.
//
static
PIRP
USBSTOR_QueueTakeIrp(
    IN PFDO_DEVICE_EXTENSION FDODeviceExtension,
    IN BOOLEAN OwnsQueue)
{
    KIRQL OldLevel;
    PLIST_ENTRY Entry;
    PIO_STACK_LOCATION IoStack;
    PIRP Irp = NULL;

    KeAcquireSpinLock(&FDODeviceExtension->IrpListLock, &OldLevel);

    if (OwnsQueue || !FDODeviceExtension->QueueBusy)
    {
        if (!BooleanFlagOn(FDODeviceExtension->Flags, USBSTOR_FDO_FLAGS_IRP_LIST_FREEZE) &&
            !IsListEmpty(&FDODeviceExtension->IrpListHead))
        {
            Entry = RemoveHeadList(&FDODeviceExtension->IrpListHead);

            // get offset to start of irp
            Irp = (PIRP)CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);

            IoStack = IoGetCurrentIrpStackLocation(Irp);
            FDODeviceExtension->ActiveSrb = (PSCSI_REQUEST_BLOCK)IoStack->Parameters.Others.Argument1;
            ASSERT(FDODeviceExtension->ActiveSrb);
        }

        FDODeviceExtension->QueueBusy = (Irp != NULL);
    }

    KeReleaseSpinLock(&FDODeviceExtension->IrpListLock, OldLevel);

    return Irp;
}

VOID
USBSTOR_QueueWaitForPendingRequests(
    IN PDEVICE_OBJECT DeviceObject)
{
    PFDO_DEVICE_EXTENSION FDODeviceExtension;

    FDODeviceExtension = (PFDO_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    KeWaitForSingleObject(&FDODeviceExtension->NoPendingRequests,
                          Executive,
                          KernelMode,
                          FALSE,
                          NULL);
}

VOID
USBSTOR_QueueTerminateRequest(
    IN PDEVICE_OBJECT FDODeviceObject,
    IN PIRP Irp)
{
    KIRQL OldLevel;
    PFDO_DEVICE_EXTENSION FDODeviceExtension;
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    PSCSI_REQUEST_BLOCK Request = (PSCSI_REQUEST_BLOCK)IoStack->Parameters.Others.Argument1;

    FDODeviceExtension = (PFDO_DEVICE_EXTENSION)FDODeviceObject->DeviceExtension;
    ASSERT(FDODeviceExtension->Common.IsFDO);

    KeAcquireSpinLock(&FDODeviceExtension->IrpListLock, &OldLevel);

    FDODeviceExtension->IrpPendingCount--;

    // check if this was our current active SRB
    if (FDODeviceExtension->ActiveSrb == Request)
    {
        // indicate processing is completed. the queue itself stays busy until
        // USBSTOR_QueueNextRequest hands it over, this request is not done yet
        FDODeviceExtension->ActiveSrb = NULL;
    }

    // Set the event if nothing else is pending
    if (FDODeviceExtension->IrpPendingCount == 0 &&
        FDODeviceExtension->ActiveSrb == NULL)
    {
        KeSetEvent(&FDODeviceExtension->NoPendingRequests, IO_NO_INCREMENT, FALSE);
    }

    KeReleaseSpinLock(&FDODeviceExtension->IrpListLock, OldLevel);
}

VOID
USBSTOR_QueueNextRequest(
    IN PDEVICE_OBJECT DeviceObject)
{
    PFDO_DEVICE_EXTENSION FDODeviceExtension;
    PIRP Irp;
    PIO_STACK_LOCATION IoStack;
    PSCSI_REQUEST_BLOCK Request;

    FDODeviceExtension = (PFDO_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    ASSERT(FDODeviceExtension->Common.IsFDO);

    // release the device queue entry the finished request was started with.
    // only one packet is in flight at a time, so the device queue is empty here
    // and this just marks the device idle again
    IoStartNextPacket(DeviceObject, TRUE);

    // hand the queue over to the next request
    Irp = USBSTOR_QueueTakeIrp(FDODeviceExtension, TRUE);

    // is there an irp pending
    if (!Irp)
    {
        // no work to do
        return;
    }

    IoStack = IoGetCurrentIrpStackLocation(Irp);
    Request = (PSCSI_REQUEST_BLOCK)IoStack->Parameters.Others.Argument1;

    // start next packet
    IoStartPacket(DeviceObject, Irp, &Request->QueueSortKey, USBSTOR_CancelIo);
}

VOID
USBSTOR_QueueRelease(
    IN PDEVICE_OBJECT DeviceObject)
{
    PFDO_DEVICE_EXTENSION FDODeviceExtension;
    PIRP Irp;
    KIRQL OldLevel;
    PIO_STACK_LOCATION IoStack;
    PSCSI_REQUEST_BLOCK Request;

    FDODeviceExtension = (PFDO_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    ASSERT(FDODeviceExtension->Common.IsFDO);

    KeAcquireSpinLock(&FDODeviceExtension->IrpListLock, &OldLevel);

    // clear freezed status
    FDODeviceExtension->Flags &= ~USBSTOR_FDO_FLAGS_IRP_LIST_FREEZE;

    KeReleaseSpinLock(&FDODeviceExtension->IrpListLock, OldLevel);

    // grab the oldest irp, unless a request already owns the queue - that one
    // picks the list up itself when it completes
    Irp = USBSTOR_QueueTakeIrp(FDODeviceExtension, FALSE);

    if (!Irp)
    {
        return;
    }

    IoStack = IoGetCurrentIrpStackLocation(Irp);
    Request = (PSCSI_REQUEST_BLOCK)IoStack->Parameters.Others.Argument1;

    IoStartPacket(DeviceObject,
                  Irp,
                  &Request->QueueSortKey,
                  USBSTOR_CancelIo);
}

VOID
NTAPI
USBSTOR_StartIo(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp)
{
    PIO_STACK_LOCATION IoStack;
    PSCSI_REQUEST_BLOCK Request;
    PFDO_DEVICE_EXTENSION FDODeviceExtension;
    PPDO_DEVICE_EXTENSION PDODeviceExtension;
    KIRQL OldLevel;
    BOOLEAN ResetInProgress;

    DPRINT("USBSTOR_StartIo\n");

    FDODeviceExtension = (PFDO_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    ASSERT(FDODeviceExtension->Common.IsFDO);

    IoAcquireCancelSpinLock(&OldLevel);

    IoSetCancelRoutine(Irp, NULL);

    // check if the irp has been cancelled
    if (Irp->Cancel)
    {
        IoReleaseCancelSpinLock(OldLevel);

        Irp->IoStatus.Status = STATUS_CANCELLED;
        Irp->IoStatus.Information = 0;

        USBSTOR_QueueTerminateRequest(DeviceObject, Irp);
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        USBSTOR_QueueNextRequest(DeviceObject);
        return;
    }

    IoReleaseCancelSpinLock(OldLevel);

    KeAcquireSpinLock(&FDODeviceExtension->CommonLock, &OldLevel);
    ResetInProgress = BooleanFlagOn(FDODeviceExtension->Flags, USBSTOR_FDO_FLAGS_DEVICE_RESETTING);
    KeReleaseSpinLock(&FDODeviceExtension->CommonLock, OldLevel);

    IoStack = IoGetCurrentIrpStackLocation(Irp);

    PDODeviceExtension = (PPDO_DEVICE_EXTENSION)IoStack->DeviceObject->DeviceExtension;
    Request = IoStack->Parameters.Scsi.Srb;
    ASSERT(PDODeviceExtension->Common.IsFDO == FALSE);

    if (ResetInProgress)
    {
        // hard reset is in progress
        Request->SrbStatus = SRB_STATUS_NO_DEVICE;
        Request->DataTransferLength = 0;
        Irp->IoStatus.Information = 0;
        Irp->IoStatus.Status = STATUS_DEVICE_DOES_NOT_EXIST;
        USBSTOR_QueueTerminateRequest(DeviceObject, Irp);
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        USBSTOR_QueueNextRequest(DeviceObject);
        return;
    }

    USBSTOR_HandleExecuteSCSI(IoStack->DeviceObject, Irp);

    // FIXME: handle error
}
