/*
 * PROJECT:     ReactOS Universal Serial Bus Human Interface Device Driver
 * LICENSE:     GPL - See COPYING in the top level directory
 * FILE:        drivers/hid/hidclass/hidclass.c
 * PURPOSE:     HID Class Driver
 * PROGRAMMERS:
 *              Michael Martin (michael.martin@reactos.org)
 *              Johannes Anderwald (johannes.anderwald@reactos.org)
 */

#include "precomp.h"

#define NDEBUG
#include <debug.h>

static LPWSTR ClientIdentificationAddress = L"HIDCLASS";
static ULONG HidClassDeviceNumber = 0;

NTSTATUS
NTAPI
DllInitialize(
    IN PUNICODE_STRING RegistryPath)
{
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
DllUnload(VOID)
{
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
HidClassAddDevice(
    IN PDRIVER_OBJECT DriverObject,
    IN PDEVICE_OBJECT PhysicalDeviceObject)
{
    WCHAR CharDeviceName[64];
    NTSTATUS Status;
    UNICODE_STRING DeviceName;
    PDEVICE_OBJECT NewDeviceObject;
    PHIDCLASS_FDO_EXTENSION FDODeviceExtension;
    ULONG DeviceExtensionSize;
    PHIDCLASS_DRIVER_EXTENSION DriverExtension;

    /* increment device number */
    InterlockedIncrement((PLONG)&HidClassDeviceNumber);

    /* construct device name */
    _swprintf(CharDeviceName, L"\\Device\\_HID%08x", HidClassDeviceNumber);

    /* initialize device name */
    RtlInitUnicodeString(&DeviceName, CharDeviceName);

    /* get driver object extension */
    DriverExtension = IoGetDriverObjectExtension(DriverObject, ClientIdentificationAddress);
    if (!DriverExtension)
    {
        /* device removed */
        ASSERT(FALSE);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    /* calculate device extension size */
    DeviceExtensionSize = sizeof(HIDCLASS_FDO_EXTENSION) + DriverExtension->DeviceExtensionSize;

    /* now create the device */
    Status = IoCreateDevice(DriverObject, DeviceExtensionSize, &DeviceName, FILE_DEVICE_UNKNOWN, 0, FALSE, &NewDeviceObject);
    if (!NT_SUCCESS(Status))
    {
        /* failed to create device object */
        ASSERT(FALSE);
        return Status;
    }

    /* get device extension */
    FDODeviceExtension = NewDeviceObject->DeviceExtension;

    /* zero device extension */
    RtlZeroMemory(FDODeviceExtension, sizeof(HIDCLASS_FDO_EXTENSION));

    /* initialize device extension */
    FDODeviceExtension->Common.IsFDO = TRUE;
    FDODeviceExtension->Common.DriverExtension = DriverExtension;
    FDODeviceExtension->SelfDeviceObject = NewDeviceObject;
    KeInitializeSpinLock(&FDODeviceExtension->ReadLock);
    KeInitializeEvent(&FDODeviceExtension->ReadsDrained, NotificationEvent, FALSE);
    FDODeviceExtension->Common.HidDeviceExtension.PhysicalDeviceObject = PhysicalDeviceObject;
    FDODeviceExtension->Common.HidDeviceExtension.MiniDeviceExtension = (PVOID)((ULONG_PTR)FDODeviceExtension + sizeof(HIDCLASS_FDO_EXTENSION));
    FDODeviceExtension->Common.HidDeviceExtension.NextDeviceObject = IoAttachDeviceToDeviceStack(NewDeviceObject, PhysicalDeviceObject);
    if (FDODeviceExtension->Common.HidDeviceExtension.NextDeviceObject == NULL)
    {
        /* no PDO */
        IoDeleteDevice(NewDeviceObject);
        DPRINT1("[HIDCLASS] failed to attach to device stack\n");
        return STATUS_DEVICE_REMOVED;
    }

    /* sanity check */
    ASSERT(FDODeviceExtension->Common.HidDeviceExtension.NextDeviceObject);

    /* increment stack size */
    NewDeviceObject->StackSize++;

    /* init device object */
    NewDeviceObject->Flags |= DO_BUFFERED_IO | DO_POWER_PAGABLE;
    NewDeviceObject->Flags  &= ~DO_DEVICE_INITIALIZING;

    /* now call driver provided add device routine */
    ASSERT(DriverExtension->AddDevice != 0);
    Status = DriverExtension->AddDevice(DriverObject, NewDeviceObject);
    if (!NT_SUCCESS(Status))
    {
        /* failed */
        DPRINT1("HIDCLASS: AddDevice failed with %x\n", Status);
        IoDetachDevice(FDODeviceExtension->Common.HidDeviceExtension.NextDeviceObject);
        IoDeleteDevice(NewDeviceObject);
        return Status;
    }

    /* succeeded */
    return Status;
}

VOID
NTAPI
HidClassDriverUnload(
    IN PDRIVER_OBJECT DriverObject)
{
    UNIMPLEMENTED;
}

NTSTATUS
NTAPI
HidClass_Create(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PIO_STACK_LOCATION IoStack;
    PHIDCLASS_COMMON_DEVICE_EXTENSION CommonDeviceExtension;
    PHIDCLASS_PDO_DEVICE_EXTENSION PDODeviceExtension;
    PHIDCLASS_FILEOP_CONTEXT Context;
    PHIDP_COLLECTION_DESC CollectionDescription;
    NTSTATUS Status;
    KIRQL OldLevel;

    //
    // get device extension
    //
    CommonDeviceExtension = DeviceObject->DeviceExtension;
    if (CommonDeviceExtension->IsFDO)
    {
         //
         // only supported for PDO
         //
         Irp->IoStatus.Status = STATUS_UNSUCCESSFUL;
         IoCompleteRequest(Irp, IO_NO_INCREMENT);
         return STATUS_UNSUCCESSFUL;
    }

    //
    // must be a PDO
    //
    ASSERT(CommonDeviceExtension->IsFDO == FALSE);

    //
    // get device extension
    //
    PDODeviceExtension = DeviceObject->DeviceExtension;

    //
    // get stack location
    //
    IoStack = IoGetCurrentIrpStackLocation(Irp);

    DPRINT("ShareAccess %x\n", IoStack->Parameters.Create.ShareAccess);
    DPRINT("Options %x\n", IoStack->Parameters.Create.Options);
    DPRINT("DesiredAccess %x\n", IoStack->Parameters.Create.SecurityContext->DesiredAccess);

    //
    // allocate context
    //
    Context = ExAllocatePoolWithTag(NonPagedPool, sizeof(HIDCLASS_FILEOP_CONTEXT), HIDCLASS_TAG);
    if (!Context)
    {
        //
        // no memory
        //
        Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    //
    // init context
    //
    RtlZeroMemory(Context, sizeof(HIDCLASS_FILEOP_CONTEXT));
    Context->DeviceExtension = PDODeviceExtension;
    KeInitializeSpinLock(&Context->Lock);
    InitializeListHead(&Context->PendingReadListHead);

    //
    // give this file object somewhere to keep the reports that arrive while
    // it is not reading
    //
    CollectionDescription = HidClassPDO_GetCollectionDescription(&PDODeviceExtension->Common.DeviceDescription,
                                                                 PDODeviceExtension->CollectionNumber);
    if (CollectionDescription == NULL || CollectionDescription->InputLength == 0)
    {
        DPRINT1("[HIDCLASS] Collection %lu reports no input\n", PDODeviceExtension->CollectionNumber);
        ExFreePoolWithTag(Context, HIDCLASS_TAG);
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    Status = HidClass_RingInitialize(&Context->ReportRing,
                                     CollectionDescription->InputLength,
                                     HIDCLASS_RING_REPORT_COUNT);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Context, HIDCLASS_TAG);
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    //
    // reports are handed out to every file object on the collection
    //
    KeAcquireSpinLock(&PDODeviceExtension->FileOpLock, &OldLevel);
    InsertTailList(&PDODeviceExtension->FileOpListHead, &Context->FileOpLink);
    KeReleaseSpinLock(&PDODeviceExtension->FileOpLock, OldLevel);

    //
    // store context
    //
    ASSERT(IoStack->FileObject);
    IoStack->FileObject->FsContext = Context;

    //
    // done
    //
    Irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
HidClass_Close(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PIO_STACK_LOCATION IoStack;
    PHIDCLASS_COMMON_DEVICE_EXTENSION CommonDeviceExtension;
    PHIDCLASS_FILEOP_CONTEXT IrpContext;
    KIRQL OldLevel;
    PLIST_ENTRY Entry;
    PIRP ListIrp;
    LIST_ENTRY CancelledReads;

    //
    // get device extension
    //
    CommonDeviceExtension = DeviceObject->DeviceExtension;

    //
    // is it a FDO request
    //
    if (CommonDeviceExtension->IsFDO)
    {
        //
        // how did the request get there
        //
        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER_1;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_PARAMETER_1;
    }

    //
    // get stack location
    //
    IoStack = IoGetCurrentIrpStackLocation(Irp);

    //
    // sanity checks
    //
    ASSERT(IoStack->FileObject);
    ASSERT(IoStack->FileObject->FsContext);

    //
    // get irp context
    //
    IrpContext = IoStack->FileObject->FsContext;
    ASSERT(IrpContext);

    //
    // stop taking reports for this file object and finish the reads that are
    // still waiting for one
    //
    KeAcquireSpinLock(&IrpContext->DeviceExtension->FileOpLock, &OldLevel);

    IrpContext->StopInProgress = TRUE;
    RemoveEntryList(&IrpContext->FileOpLink);
    InitializeListHead(&IrpContext->FileOpLink);

    InitializeListHead(&CancelledReads);
    while (!IsListEmpty(&IrpContext->PendingReadListHead))
    {
        Entry = RemoveHeadList(&IrpContext->PendingReadListHead);
        InitializeListHead(Entry);
        ListIrp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);

        /* Losing the cancel routine means the cancel path has the request */
        if (IoSetCancelRoutine(ListIrp, NULL) != NULL)
        {
            InsertTailList(&CancelledReads, &ListIrp->Tail.Overlay.ListEntry);
        }
    }

    HidClass_RingFree(&IrpContext->ReportRing);

    KeReleaseSpinLock(&IrpContext->DeviceExtension->FileOpLock, OldLevel);

    while (!IsListEmpty(&CancelledReads))
    {
        ListIrp = CONTAINING_RECORD(RemoveHeadList(&CancelledReads),
                                    IRP,
                                    Tail.Overlay.ListEntry);
        ListIrp->IoStatus.Status = STATUS_CANCELLED;
        ListIrp->IoStatus.Information = 0;
        IoCompleteRequest(ListIrp, IO_NO_INCREMENT);
    }



    //
    // remove context
    //
    IoStack->FileObject->FsContext = NULL;

    //
    // free context
    //
    ExFreePoolWithTag(IrpContext, HIDCLASS_TAG);

    //
    // complete request
    //
    Irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Takes a read off the pending list when it is cancelled.
 *
 * @param[in] DeviceObject
 * The collection the read was issued against.
 *
 * @param[in,out] Irp
 * The read being cancelled.
 *
 * @remarks
 * A read that was handed a report in the meantime has already been unlinked
 * and its entry made to point at itself, so unlinking it again does nothing.
 */
static
VOID
NTAPI
HidClass_CancelRead(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PHIDCLASS_PDO_DEVICE_EXTENSION PDODeviceExtension;
    KIRQL OldLevel;

    PDODeviceExtension = DeviceObject->DeviceExtension;

    IoReleaseCancelSpinLock(Irp->CancelIrql);

    KeAcquireSpinLock(&PDODeviceExtension->FileOpLock, &OldLevel);
    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
    InitializeListHead(&Irp->Tail.Overlay.ListEntry);
    KeReleaseSpinLock(&PDODeviceExtension->FileOpLock, OldLevel);

    Irp->IoStatus.Status = STATUS_CANCELLED;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

/**
 * @brief
 * Hands the caller the next input report of the collection.
 *
 * @param[in] DeviceObject
 * The collection being read.
 *
 * @param[in,out] Irp
 * The read request.
 *
 * @return
 * STATUS_SUCCESS when a report was waiting, STATUS_PENDING when the request
 * has been queued until one arrives, or an error.
 *
 * @remarks
 * The reports come from the reads the class driver keeps running on the
 * minidriver, so one is not started here. That is what stops a report going
 * missing in the gap between two reads.
 */
NTSTATUS
NTAPI
HidClass_Read(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PHIDCLASS_PDO_DEVICE_EXTENSION PDODeviceExtension;
    PHIDCLASS_COMMON_DEVICE_EXTENSION CommonDeviceExtension;
    PHIDCLASS_FILEOP_CONTEXT Context;
    PIO_STACK_LOCATION IoStack;
    KIRQL OldLevel;
    PUCHAR Address;
    ULONG Length;

    IoStack = IoGetCurrentIrpStackLocation(Irp);

    CommonDeviceExtension = DeviceObject->DeviceExtension;
    ASSERT(CommonDeviceExtension->IsFDO == FALSE);
    PDODeviceExtension = DeviceObject->DeviceExtension;

    ASSERT(IoStack->FileObject);
    Context = IoStack->FileObject->FsContext;
    ASSERT(Context);

    if (IoStack->Parameters.Read.Length == 0)
    {
        Irp->IoStatus.Status = STATUS_INVALID_BUFFER_SIZE;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_BUFFER_SIZE;
    }

    Address = MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);
    if (Address == NULL)
    {
        Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Length = min(IoStack->Parameters.Read.Length, Context->ReportRing.ReportSize);

    KeAcquireSpinLock(&PDODeviceExtension->FileOpLock, &OldLevel);

    if (Context->StopInProgress)
    {
        KeReleaseSpinLock(&PDODeviceExtension->FileOpLock, OldLevel);
        DPRINT1("[HIDCLASS] Stop In Progress\n");
        Irp->IoStatus.Status = STATUS_CANCELLED;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_CANCELLED;
    }

    /* A report that already came in is handed over without waiting */
    if (HidClass_RingGet(&Context->ReportRing, Address, Length))
    {
        KeReleaseSpinLock(&PDODeviceExtension->FileOpLock, OldLevel);
        Irp->IoStatus.Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = Length;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_SUCCESS;
    }

    /*
     * Nothing to hand over, so wait for the next report. The request goes on
     * the list before the cancel routine is set, so that the routine always
     * finds it there.
     */
    IoMarkIrpPending(Irp);
    InsertTailList(&Context->PendingReadListHead, &Irp->Tail.Overlay.ListEntry);
    IoSetCancelRoutine(Irp, HidClass_CancelRead);

    /* A request cancelled before the routine was in place is ours to finish */
    if (Irp->Cancel && IoSetCancelRoutine(Irp, NULL) != NULL)
    {
        RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
        InitializeListHead(&Irp->Tail.Overlay.ListEntry);
        KeReleaseSpinLock(&PDODeviceExtension->FileOpLock, OldLevel);
        Irp->IoStatus.Status = STATUS_CANCELLED;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_CANCELLED;
    }

    KeReleaseSpinLock(&PDODeviceExtension->FileOpLock, OldLevel);
    return STATUS_PENDING;
}

NTSTATUS
NTAPI
HidClass_Write(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PIO_STACK_LOCATION IoStack;
    PHIDCLASS_COMMON_DEVICE_EXTENSION CommonDeviceExtension;
    PIRP SubIrp;
    KEVENT Event;
    IO_STATUS_BLOCK IoStatusBlock;
    HID_XFER_PACKET XferPacket;
    NTSTATUS Status;
    ULONG Length;

    IoStack = IoGetCurrentIrpStackLocation(Irp);
    Length = IoStack->Parameters.Write.Length;
    if (Length < 1)
    {
        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(&XferPacket, sizeof(XferPacket));
    XferPacket.reportBufferLen = Length;
    XferPacket.reportBuffer = Irp->UserBuffer;
    XferPacket.reportId = XferPacket.reportBuffer[0];

    CommonDeviceExtension = DeviceObject->DeviceExtension;
    SubIrp = IoBuildDeviceIoControlRequest(
        IOCTL_HID_WRITE_REPORT,
        CommonDeviceExtension->HidDeviceExtension.NextDeviceObject,
        NULL, 0,
        NULL, 0,
        TRUE,
        &Event,
        &IoStatusBlock);
    if (!SubIrp)
    {
        Irp->IoStatus.Status = STATUS_NO_MEMORY;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_NOT_IMPLEMENTED;
    }
    SubIrp->UserBuffer = &XferPacket;
    KeInitializeEvent(&Event, SynchronizationEvent, FALSE);
    Status = IoCallDriver(CommonDeviceExtension->HidDeviceExtension.NextDeviceObject, SubIrp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = IoStatusBlock.Status;
    }
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

NTSTATUS
NTAPI
HidClass_DeviceControl(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PIO_STACK_LOCATION IoStack;
    PHIDCLASS_COMMON_DEVICE_EXTENSION CommonDeviceExtension;
    PHID_COLLECTION_INFORMATION CollectionInformation;
    PHIDP_COLLECTION_DESC CollectionDescription;
    PHIDCLASS_PDO_DEVICE_EXTENSION PDODeviceExtension;

    //
    // get device extension
    //
    CommonDeviceExtension = DeviceObject->DeviceExtension;

    //
    // only PDO are supported
    //
    if (CommonDeviceExtension->IsFDO)
    {
        //
        // invalid request
        //
        DPRINT1("[HIDCLASS] DeviceControl Irp for FDO arrived\n");
        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER_1;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_PARAMETER_1;
    }

    ASSERT(CommonDeviceExtension->IsFDO == FALSE);

    //
    // get pdo device extension
    //
    PDODeviceExtension = DeviceObject->DeviceExtension;

    //
    // get stack location
    //
    IoStack = IoGetCurrentIrpStackLocation(Irp);

    switch (IoStack->Parameters.DeviceIoControl.IoControlCode)
    {
        case IOCTL_HID_GET_COLLECTION_INFORMATION:
        {
            //
            // check if output buffer is big enough
            //
            if (IoStack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(HID_COLLECTION_INFORMATION))
            {
                //
                // invalid buffer size
                //
                Irp->IoStatus.Status = STATUS_INVALID_BUFFER_SIZE;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
                return STATUS_INVALID_BUFFER_SIZE;
            }

            //
            // get output buffer
            //
            CollectionInformation = Irp->AssociatedIrp.SystemBuffer;
            ASSERT(CollectionInformation);

            //
            // get collection description
            //
            CollectionDescription = HidClassPDO_GetCollectionDescription(&CommonDeviceExtension->DeviceDescription,
                                                                         PDODeviceExtension->CollectionNumber);
            ASSERT(CollectionDescription);

            //
            // init result buffer
            //
            CollectionInformation->DescriptorSize = CollectionDescription->PreparsedDataLength;
            CollectionInformation->Polled = CommonDeviceExtension->DriverExtension->DevicesArePolled;
            CollectionInformation->VendorID = CommonDeviceExtension->Attributes.VendorID;
            CollectionInformation->ProductID = CommonDeviceExtension->Attributes.ProductID;
            CollectionInformation->VersionNumber = CommonDeviceExtension->Attributes.VersionNumber;

            //
            // complete request
            //
            Irp->IoStatus.Information = sizeof(HID_COLLECTION_INFORMATION);
            Irp->IoStatus.Status = STATUS_SUCCESS;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return STATUS_SUCCESS;
        }
        case IOCTL_HID_GET_COLLECTION_DESCRIPTOR:
        {
            //
            // get collection description
            //
            CollectionDescription = HidClassPDO_GetCollectionDescription(&CommonDeviceExtension->DeviceDescription,
                                                                         PDODeviceExtension->CollectionNumber);
            ASSERT(CollectionDescription);

            //
            // check if output buffer is big enough
            //
            if (IoStack->Parameters.DeviceIoControl.OutputBufferLength < CollectionDescription->PreparsedDataLength)
            {
                //
                // invalid buffer size
                //
                Irp->IoStatus.Status = STATUS_INVALID_BUFFER_SIZE;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
                return STATUS_INVALID_BUFFER_SIZE;
            }

            //
            // copy result
            //
            ASSERT(Irp->UserBuffer);
            RtlCopyMemory(Irp->UserBuffer, CollectionDescription->PreparsedData, CollectionDescription->PreparsedDataLength);

            //
            // complete request
            //
            Irp->IoStatus.Information = CollectionDescription->PreparsedDataLength;
            Irp->IoStatus.Status = STATUS_SUCCESS;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return STATUS_SUCCESS;
        }
        case IOCTL_HID_GET_FEATURE:
        {
            PIRP SubIrp;
            KEVENT Event;
            IO_STATUS_BLOCK IoStatusBlock;
            HID_XFER_PACKET XferPacket;
            NTSTATUS Status;
            PHIDP_REPORT_IDS ReportDescription;

            if (IoStack->Parameters.DeviceIoControl.InputBufferLength < 1)
            {
                Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
                return STATUS_INVALID_PARAMETER;
            }
            ReportDescription = HidClassPDO_GetReportDescriptionByReportID(&PDODeviceExtension->Common.DeviceDescription, ((PUCHAR)Irp->AssociatedIrp.SystemBuffer)[0]);
            if (!ReportDescription || IoStack->Parameters.DeviceIoControl.OutputBufferLength < ReportDescription->FeatureLength)
            {
                Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
                return STATUS_INVALID_PARAMETER;
            }

            RtlZeroMemory(&XferPacket, sizeof(XferPacket));
            XferPacket.reportBufferLen = ReportDescription->FeatureLength;
            XferPacket.reportBuffer = MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);
            XferPacket.reportId = ((PUCHAR)Irp->AssociatedIrp.SystemBuffer)[0];
            if (!XferPacket.reportBuffer)
            {
                Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            SubIrp = IoBuildDeviceIoControlRequest(
                IOCTL_HID_GET_FEATURE,
                CommonDeviceExtension->HidDeviceExtension.NextDeviceObject,
                NULL, 0,
                NULL, 0,
                TRUE,
                &Event,
                &IoStatusBlock);
            if (!SubIrp)
            {
                Irp->IoStatus.Status = STATUS_NO_MEMORY;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
                return STATUS_NOT_IMPLEMENTED;
            }
            SubIrp->UserBuffer = &XferPacket;
            KeInitializeEvent(&Event, SynchronizationEvent, FALSE);
            Status = IoCallDriver(CommonDeviceExtension->HidDeviceExtension.NextDeviceObject, SubIrp);
            if (Status == STATUS_PENDING)
            {
                KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
                Status = IoStatusBlock.Status;
            }
            Irp->IoStatus.Status = Status;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return Status;
        }
        case IOCTL_HID_SET_FEATURE:
        {
            PIRP SubIrp;
            KEVENT Event;
            IO_STATUS_BLOCK IoStatusBlock;
            HID_XFER_PACKET XferPacket;
            NTSTATUS Status;
            PHIDP_REPORT_IDS ReportDescription;

            if (IoStack->Parameters.DeviceIoControl.InputBufferLength < 1)
            {
                Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
                return STATUS_INVALID_PARAMETER;
            }
            ReportDescription = HidClassPDO_GetReportDescriptionByReportID(&PDODeviceExtension->Common.DeviceDescription, ((PUCHAR)Irp->AssociatedIrp.SystemBuffer)[0]);
            if (!ReportDescription || IoStack->Parameters.DeviceIoControl.InputBufferLength < ReportDescription->FeatureLength)
            {
                Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
                return STATUS_INVALID_PARAMETER;
            }

            RtlZeroMemory(&XferPacket, sizeof(XferPacket));
            XferPacket.reportBufferLen = ReportDescription->FeatureLength;
            XferPacket.reportBuffer = Irp->AssociatedIrp.SystemBuffer;
            XferPacket.reportId = XferPacket.reportBuffer[0];

            SubIrp = IoBuildDeviceIoControlRequest(
                IOCTL_HID_SET_FEATURE,
                CommonDeviceExtension->HidDeviceExtension.NextDeviceObject,
                NULL, 0,
                NULL, 0,
                TRUE,
                &Event,
                &IoStatusBlock);
            if (!SubIrp)
            {
                Irp->IoStatus.Status = STATUS_NO_MEMORY;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
                return STATUS_NOT_IMPLEMENTED;
            }
            SubIrp->UserBuffer = &XferPacket;
            KeInitializeEvent(&Event, SynchronizationEvent, FALSE);
            Status = IoCallDriver(CommonDeviceExtension->HidDeviceExtension.NextDeviceObject, SubIrp);
            if (Status == STATUS_PENDING)
            {
                KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
                Status = IoStatusBlock.Status;
            }
            Irp->IoStatus.Status = Status;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return Status;
        }
        default:
        {
            DPRINT1("[HIDCLASS] DeviceControl IoControlCode 0x%x not implemented\n", IoStack->Parameters.DeviceIoControl.IoControlCode);
            Irp->IoStatus.Status = STATUS_NOT_IMPLEMENTED;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return STATUS_NOT_IMPLEMENTED;
        }
    }
}

NTSTATUS
NTAPI
HidClass_InternalDeviceControl(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    UNIMPLEMENTED;
    ASSERT(FALSE);
    Irp->IoStatus.Status = STATUS_NOT_IMPLEMENTED;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
NTAPI
HidClass_Power(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PHIDCLASS_COMMON_DEVICE_EXTENSION CommonDeviceExtension;
    CommonDeviceExtension = DeviceObject->DeviceExtension;

    if (CommonDeviceExtension->IsFDO)
    {
        IoCopyCurrentIrpStackLocationToNext(Irp);
        return HidClassFDO_DispatchRequest(DeviceObject, Irp);
    }
    else
    {
        Irp->IoStatus.Status = STATUS_SUCCESS;
        PoStartNextPowerIrp(Irp);
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_SUCCESS;
    }
}

NTSTATUS
NTAPI
HidClass_PnP(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PHIDCLASS_COMMON_DEVICE_EXTENSION CommonDeviceExtension;

    //
    // get common device extension
    //
    CommonDeviceExtension = DeviceObject->DeviceExtension;

    //
    // check type of device object
    //
    if (CommonDeviceExtension->IsFDO)
    {
        //
        // handle request
        //
        return HidClassFDO_PnP(DeviceObject, Irp);
    }
    else
    {
        //
        // handle request
        //
        return HidClassPDO_PnP(DeviceObject, Irp);
    }
}

NTSTATUS
NTAPI
HidClass_DispatchDefault(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PHIDCLASS_COMMON_DEVICE_EXTENSION CommonDeviceExtension;

    //
    // get common device extension
    //
    CommonDeviceExtension = DeviceObject->DeviceExtension;

    //
    // FIXME: support PDO
    //
    ASSERT(CommonDeviceExtension->IsFDO == TRUE);

    //
    // skip current irp stack location
    //
    IoSkipCurrentIrpStackLocation(Irp);

    //
    // dispatch to lower device object
    //
    return IoCallDriver(CommonDeviceExtension->HidDeviceExtension.NextDeviceObject, Irp);
}

NTSTATUS
NTAPI
HidClassDispatch(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PIO_STACK_LOCATION IoStack;

    //
    // get current stack location
    //
    IoStack = IoGetCurrentIrpStackLocation(Irp);
    DPRINT("[HIDCLASS] Dispatch Major %x Minor %x\n", IoStack->MajorFunction, IoStack->MinorFunction);

    //
    // dispatch request based on major function
    //
    switch (IoStack->MajorFunction)
    {
        case IRP_MJ_CREATE:
            return HidClass_Create(DeviceObject, Irp);
        case IRP_MJ_CLOSE:
            return HidClass_Close(DeviceObject, Irp);
        case IRP_MJ_READ:
            return HidClass_Read(DeviceObject, Irp);
        case IRP_MJ_WRITE:
            return HidClass_Write(DeviceObject, Irp);
        case IRP_MJ_DEVICE_CONTROL:
            return HidClass_DeviceControl(DeviceObject, Irp);
        case IRP_MJ_INTERNAL_DEVICE_CONTROL:
           return HidClass_InternalDeviceControl(DeviceObject, Irp);
        case IRP_MJ_POWER:
            return HidClass_Power(DeviceObject, Irp);
        case IRP_MJ_PNP:
            return HidClass_PnP(DeviceObject, Irp);
        default:
            return HidClass_DispatchDefault(DeviceObject, Irp);
    }
}

NTSTATUS
NTAPI
HidRegisterMinidriver(
    IN PHID_MINIDRIVER_REGISTRATION MinidriverRegistration)
{
    NTSTATUS Status;
    PHIDCLASS_DRIVER_EXTENSION DriverExtension;

    /* check if the version matches */
    if (MinidriverRegistration->Revision > HID_REVISION)
    {
        /* revision mismatch */
        ASSERT(FALSE);
        return STATUS_REVISION_MISMATCH;
    }

    /* now allocate the driver object extension */
    Status = IoAllocateDriverObjectExtension(MinidriverRegistration->DriverObject,
                                             ClientIdentificationAddress,
                                             sizeof(HIDCLASS_DRIVER_EXTENSION),
                                             (PVOID *)&DriverExtension);
    if (!NT_SUCCESS(Status))
    {
        /* failed to allocate driver extension */
        ASSERT(FALSE);
        return Status;
    }

    /* zero driver extension */
    RtlZeroMemory(DriverExtension, sizeof(HIDCLASS_DRIVER_EXTENSION));

    /* init driver extension */
    DriverExtension->DriverObject = MinidriverRegistration->DriverObject;
    DriverExtension->DeviceExtensionSize = MinidriverRegistration->DeviceExtensionSize;
    DriverExtension->DevicesArePolled = MinidriverRegistration->DevicesArePolled;
    DriverExtension->AddDevice = MinidriverRegistration->DriverObject->DriverExtension->AddDevice;
    DriverExtension->DriverUnload = MinidriverRegistration->DriverObject->DriverUnload;

    /* copy driver dispatch routines */
    RtlCopyMemory(DriverExtension->MajorFunction,
                  MinidriverRegistration->DriverObject->MajorFunction,
                  sizeof(PDRIVER_DISPATCH) * (IRP_MJ_MAXIMUM_FUNCTION + 1));

    /* initialize lock */
    KeInitializeSpinLock(&DriverExtension->Lock);

    /* now replace dispatch routines */
    DriverExtension->DriverObject->DriverExtension->AddDevice = HidClassAddDevice;
    DriverExtension->DriverObject->DriverUnload = HidClassDriverUnload;
    DriverExtension->DriverObject->MajorFunction[IRP_MJ_CREATE] = HidClassDispatch;
    DriverExtension->DriverObject->MajorFunction[IRP_MJ_CLOSE] = HidClassDispatch;
    DriverExtension->DriverObject->MajorFunction[IRP_MJ_READ] = HidClassDispatch;
    DriverExtension->DriverObject->MajorFunction[IRP_MJ_WRITE] = HidClassDispatch;
    DriverExtension->DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = HidClassDispatch;
    DriverExtension->DriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = HidClassDispatch;
    DriverExtension->DriverObject->MajorFunction[IRP_MJ_POWER] = HidClassDispatch;
    DriverExtension->DriverObject->MajorFunction[IRP_MJ_PNP] = HidClassDispatch;

    /* done */
    return STATUS_SUCCESS;
}
