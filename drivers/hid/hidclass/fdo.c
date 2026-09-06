/*
 * PROJECT:     ReactOS Universal Serial Bus Human Interface Device Driver
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     HID Class Driver
 * COPYRIGHT:   Copyright  Michael Martin <michael.martin@reactos.org>
 *              Copyright  Johannes Anderwald <johannes.anderwald@reactos.org>
 *              Copyright 2022 Roman Masanin <36927roma@gmail.com>
 */

#include "precomp.h"

#define NDEBUG
#include <debug.h>

// driver verifier
IO_COMPLETION_ROUTINE HidClassFDO_QueryCapabilitiesCompletionRoutine;
IO_COMPLETION_ROUTINE HidClassFDO_DispatchRequestSynchronousCompletion;
IO_COMPLETION_ROUTINE HidClassFDO_ReadCompletion;

NTSTATUS
NTAPI
HidClassFDO_QueryCapabilitiesCompletionRoutine(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);
    ASSERT(Context != NULL);

    KeSetEvent(Context, 0, FALSE);

    //
    // completion is done in the HidClassFDO_QueryCapabilities routine
    //
    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS
HidClassFDO_QueryCapabilities(
    IN PDEVICE_OBJECT DeviceObject,
    IN OUT PDEVICE_CAPABILITIES Capabilities)
{
    PIRP Irp;
    KEVENT Event;
    NTSTATUS Status;
    PIO_STACK_LOCATION IoStack;
    PHIDCLASS_FDO_EXTENSION FDODeviceExtension;

    //
    // get device extension
    //
    FDODeviceExtension = DeviceObject->DeviceExtension;
    ASSERT(FDODeviceExtension->Common.IsFDO);

    //
    // init event
    //
    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    //
    // now allocate the irp
    //
    Irp = IoAllocateIrp(DeviceObject->StackSize, FALSE);
    if (!Irp)
    {
        //
        // no memory
        //
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    //
    // get next stack location
    //
    IoStack = IoGetNextIrpStackLocation(Irp);

    //
    // init stack location
    //
    IoStack->MajorFunction = IRP_MJ_PNP;
    IoStack->MinorFunction = IRP_MN_QUERY_CAPABILITIES;
    IoStack->Parameters.DeviceCapabilities.Capabilities = Capabilities;

    //
    // set completion routine
    //
    IoSetCompletionRoutine(Irp, HidClassFDO_QueryCapabilitiesCompletionRoutine, &Event, TRUE, TRUE, TRUE);

    //
    // init capabilities
    //
    RtlZeroMemory(Capabilities, sizeof(DEVICE_CAPABILITIES));
    Capabilities->Size = sizeof(DEVICE_CAPABILITIES);
    Capabilities->Version = 1; // FIXME hardcoded constant
    Capabilities->Address = MAXULONG;
    Capabilities->UINumber = MAXULONG;

    //
    // pnp irps have default completion code
    //
    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;

    //
    // call lower  device
    //
    Status = IoCallDriver(FDODeviceExtension->Common.HidDeviceExtension.NextDeviceObject, Irp);
    if (Status == STATUS_PENDING)
    {
        //
        // wait for completion
        //
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
    }

    //
    // get status
    //
    Status = Irp->IoStatus.Status;

    //
    // complete request
    //
    IoFreeIrp(Irp);

    //
    // done
    //
    return Status;
}

NTSTATUS
NTAPI
HidClassFDO_DispatchRequestSynchronousCompletion(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);
    ASSERT(Context != NULL);

    KeSetEvent(Context, 0, FALSE);

    return STATUS_MORE_PROCESSING_REQUIRED;
}


NTSTATUS
HidClassFDO_DispatchRequestSynchronous(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    KEVENT Event;
    PHIDCLASS_COMMON_DEVICE_EXTENSION CommonDeviceExtension;
    NTSTATUS Status;
    PIO_STACK_LOCATION IoStack;

    //
    // init event
    //
    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    //
    // get device extension
    //
    CommonDeviceExtension = DeviceObject->DeviceExtension;

    //
    // set completion routine
    //
    IoSetCompletionRoutine(Irp, HidClassFDO_DispatchRequestSynchronousCompletion, &Event, TRUE, TRUE, TRUE);

    ASSERT(Irp->CurrentLocation > 0);
    //
    // create stack location
    //
    IoSetNextIrpStackLocation(Irp);

    //
    // get next stack location
    //
    IoStack = IoGetCurrentIrpStackLocation(Irp);

    //
    // store device object
    //
    IoStack->DeviceObject = DeviceObject;

    //
    // sanity check
    //
    ASSERT(CommonDeviceExtension->DriverExtension->MajorFunction[IoStack->MajorFunction] != NULL);

    //
    // call minidriver (hidusb)
    //
    Status = CommonDeviceExtension->DriverExtension->MajorFunction[IoStack->MajorFunction](DeviceObject, Irp);

    //
    // wait for the request to finish
    //
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);

        //
        // update status
        //
        Status = Irp->IoStatus.Status;
    }

    //
    // done
    //
    return Status;
}

NTSTATUS
HidClassFDO_DispatchRequest(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PHIDCLASS_COMMON_DEVICE_EXTENSION CommonDeviceExtension;
    NTSTATUS Status;
    PIO_STACK_LOCATION IoStack;

    //
    // get device extension
    //
    CommonDeviceExtension = DeviceObject->DeviceExtension;

    ASSERT(Irp->CurrentLocation > 0);

    //
    // create stack location
    //
    IoSetNextIrpStackLocation(Irp);

    //
    // get next stack location
    //
    IoStack = IoGetCurrentIrpStackLocation(Irp);

    //
    // store device object
    //
    IoStack->DeviceObject = DeviceObject;

    //
    // sanity check
    //
    ASSERT(CommonDeviceExtension->DriverExtension->MajorFunction[IoStack->MajorFunction] != NULL);

    //
    // call driver
    //
    Status = CommonDeviceExtension->DriverExtension->MajorFunction[IoStack->MajorFunction](DeviceObject, Irp);

    //
    // done
    //
    return Status;
}

NTSTATUS
HidClassFDO_GetDescriptors(
    IN PDEVICE_OBJECT DeviceObject)
{
    PHIDCLASS_FDO_EXTENSION FDODeviceExtension;
    PIRP Irp;
    PIO_STACK_LOCATION IoStack;
    NTSTATUS Status;

    //
    // get device extension
    //
    FDODeviceExtension = DeviceObject->DeviceExtension;
    ASSERT(FDODeviceExtension->Common.IsFDO);

    //
    // let's allocate irp
    //
    Irp = IoAllocateIrp(DeviceObject->StackSize, FALSE);
    if (!Irp)
    {
        //
        // no memory
        //
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    //
    // get stack location
    //
    IoStack = IoGetNextIrpStackLocation(Irp);

    //
    // init stack location
    //
    IoStack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    IoStack->Parameters.DeviceIoControl.IoControlCode = IOCTL_HID_GET_DEVICE_DESCRIPTOR;
    IoStack->Parameters.DeviceIoControl.OutputBufferLength = sizeof(HID_DESCRIPTOR);
    IoStack->Parameters.DeviceIoControl.InputBufferLength = 0;
    IoStack->Parameters.DeviceIoControl.Type3InputBuffer = NULL;
    Irp->UserBuffer = &FDODeviceExtension->HidDescriptor;

    //
    // send request
    //
    Status = HidClassFDO_DispatchRequestSynchronous(DeviceObject, Irp);
    if (!NT_SUCCESS(Status))
    {
        //
        // failed to get device descriptor
        //
        DPRINT1("[HIDCLASS] IOCTL_HID_GET_DEVICE_DESCRIPTOR failed with %x\n", Status);
        IoFreeIrp(Irp);
        return Status;
    }

    //
    // let's get device attributes
    //
    IoStack->Parameters.DeviceIoControl.IoControlCode = IOCTL_HID_GET_DEVICE_ATTRIBUTES;
    IoStack->Parameters.DeviceIoControl.OutputBufferLength = sizeof(HID_DEVICE_ATTRIBUTES);
    Irp->UserBuffer = &FDODeviceExtension->Common.Attributes;

    //
    // send request
    //
    Status = HidClassFDO_DispatchRequestSynchronous(DeviceObject, Irp);
    if (!NT_SUCCESS(Status))
    {
        //
        // failed to get device descriptor
        //
        DPRINT1("[HIDCLASS] IOCTL_HID_GET_DEVICE_ATTRIBUTES failed with %x\n", Status);
        IoFreeIrp(Irp);
        return Status;
    }

    //
    // sanity checks
    //
    ASSERT(FDODeviceExtension->HidDescriptor.bLength == sizeof(HID_DESCRIPTOR));
    ASSERT(FDODeviceExtension->HidDescriptor.bNumDescriptors > 0);
    ASSERT(FDODeviceExtension->HidDescriptor.DescriptorList[0].wReportLength > 0);
    ASSERT(FDODeviceExtension->HidDescriptor.DescriptorList[0].bReportType == HID_REPORT_DESCRIPTOR_TYPE);

    //
    // now allocate space for the report descriptor
    //
    FDODeviceExtension->ReportDescriptor = ExAllocatePoolWithTag(NonPagedPool,
                                                                 FDODeviceExtension->HidDescriptor.DescriptorList[0].wReportLength,
                                                                 HIDCLASS_TAG);
    if (!FDODeviceExtension->ReportDescriptor)
    {
        //
        // not enough memory
        //
        IoFreeIrp(Irp);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    //
    // init stack location
    //
    IoStack->Parameters.DeviceIoControl.IoControlCode = IOCTL_HID_GET_REPORT_DESCRIPTOR;
    IoStack->Parameters.DeviceIoControl.OutputBufferLength = FDODeviceExtension->HidDescriptor.DescriptorList[0].wReportLength;
    Irp->UserBuffer = FDODeviceExtension->ReportDescriptor;

    //
    // send request
    //
    Status = HidClassFDO_DispatchRequestSynchronous(DeviceObject, Irp);
    if (!NT_SUCCESS(Status))
    {
        //
        // failed to get device descriptor
        //
        DPRINT1("[HIDCLASS] IOCTL_HID_GET_REPORT_DESCRIPTOR failed with %x\n", Status);
        IoFreeIrp(Irp);
        return Status;
    }

    //
    // completed successfully
    //
    IoFreeIrp(Irp);
    return STATUS_SUCCESS;
}

PIRP HidClass_DequeueIrp(PHIDCLASS_PDO_DEVICE_EXTENSION deviceContext)
{
    PIRP nextIrp = NULL;
    PDRIVER_CANCEL oldCancelRoutine;
    PLIST_ENTRY listEntry;

    while (nextIrp == NULL && IsListEmpty(&deviceContext->PendingIRPList) == FALSE)
    {
        listEntry = RemoveHeadList(&deviceContext->PendingIRPList);
        nextIrp = CONTAINING_RECORD(listEntry, IRP, Tail.Overlay.ListEntry);

        oldCancelRoutine = IoSetCancelRoutine(nextIrp, NULL);
        if (oldCancelRoutine != NULL)
        {
            // Cancel routine not called for this IRP. Return this IRP.
            ASSERT(oldCancelRoutine == HidClassPDO_IrpCancelRoutine);
        }
        else
        {
            // IRP is canceled
            ASSERT(nextIrp->Cancel);
            InitializeListHead(&nextIrp->Tail.Overlay.ListEntry);
            nextIrp = NULL;
        }
    }

    return nextIrp;
}

NTSTATUS
NTAPI
HidClassFDO_ReadCompletion(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PVOID Context)
{
    PHIDCLASS_FDO_EXTENSION DeviceExtension;
    PHIDCLASS_PDO_DEVICE_EXTENSION PDODeviceExtension = NULL;
    ULONG CollectionNumber;
    KIRQL CurrentIRQL;
    LIST_ENTRY completeIRP;
    PUCHAR address = NULL;
    PIRP tempIRP = NULL;

    UNREFERENCED_PARAMETER(DeviceObject);

    /* get device extension */
    DeviceExtension = Context;
    ASSERT(DeviceExtension != NULL);

    if (Irp->IoStatus.Status == STATUS_PRIVILEGE_NOT_HELD ||
        Irp->IoStatus.Status == STATUS_DEVICE_NOT_CONNECTED ||
        Irp->IoStatus.Status == STATUS_CANCELLED)
    {
        /* failed to read or should be stopped*/
        DPRINT1("[HIDCLASS] ReadCompletion terminating read Status %x\n", Irp->IoStatus.Status);

        return STATUS_MORE_PROCESSING_REQUIRED;
    }

    if (Irp->IoStatus.Information > 0)
    {
        //
        // The minidriver only wrote IoStatus.Information bytes, but everything
        // downstream hands out a whole report.  Clear the rest so a device that
        // returns a short report cannot leak whatever the previous one left in
        // this buffer.
        //
        if (Irp->IoStatus.Information < DeviceExtension->InputBufferSize)
        {
            RtlZeroMemory(DeviceExtension->InputBuffer + Irp->IoStatus.Information,
                          DeviceExtension->InputBufferSize - Irp->IoStatus.Information);
        }

        if (DeviceExtension->DeviceRelations->Count > 0)
        {
            // find PDO related to the reportID
            {
                PHIDP_REPORT_IDS reportDescription;

                //
                // Only read byte 0 as a report id if this device actually uses them.
                //
                // A device that does not is described by a single entry whose
                // ReportID is zero, and its input reports carry no id byte at all -
                // byte 0 is the first byte of data.  Feeding that to the lookup asked
                // for a report id that happened to be whatever the device last
                // reported, which for a mouse is its X movement.  The lookup missed,
                // the report was dropped by the "nowhere to deliver it" path below,
                // and the device did nothing at all.
                //
                if (DeviceExtension->Common.DeviceDescription.ReportIDsLength == 1 &&
                    DeviceExtension->Common.DeviceDescription.ReportIDs[0].ReportID == 0)
                {
                    reportDescription = &DeviceExtension->Common.DeviceDescription.ReportIDs[0];
                }
                else
                {
                    reportDescription = HidClassPDO_GetReportDescriptionByReportID(
                        &DeviceExtension->Common.DeviceDescription,
                        DeviceExtension->InputBuffer[0]);
                }
                if (reportDescription != NULL)
                {
                    CollectionNumber = reportDescription->CollectionNumber;

                    for (int i = 0; i < DeviceExtension->DeviceRelations->Count; i++)
                    {
                        PDODeviceExtension = DeviceExtension->DeviceRelations->Objects[i]->DeviceExtension;
                        ASSERT(PDODeviceExtension->Common.IsFDO == FALSE);

                        if (PDODeviceExtension->CollectionNumber == CollectionNumber)
                        {
                            break;
                        }
                        else
                        {
                            PDODeviceExtension = NULL;
                        }
                    }
                }
            }

            /*
             * Either the report named an ID this device never described, or
             * no child owns the collection it belongs to. There is nowhere to
             * deliver it, so drop it and re-arm the read - the lookup result
             * used to be dereferenced unconditionally, which faulted here.
             */
            if (PDODeviceExtension == NULL)
            {
                HidClassFDO_SubmitRead(DeviceExtension);
                return STATUS_MORE_PROCESSING_REQUIRED;
            }

            InitializeListHead(&completeIRP);

            KeAcquireSpinLock(&PDODeviceExtension->ReadLock, &CurrentIRQL);

            // check if any IRP waiting data
            while (!IsListEmpty(&PDODeviceExtension->PendingIRPList))
            {
                tempIRP = HidClass_DequeueIrp(PDODeviceExtension);
                if (tempIRP != NULL)
                {
                    if (tempIRP->MdlAddress != NULL)
                    {
                        //
                        // NormalPagePriority: this mapping is what the request
                        // is for, so failing it under pressure only turns a
                        // good read into STATUS_INVALID_USER_BUFFER.
                        //
                        address = MmGetSystemAddressForMdlSafe(tempIRP->MdlAddress, NormalPagePriority);
                        if (address != NULL)
                        {
                            PHIDP_COLLECTION_DESC desc;
                            ULONG copyLength;

                            desc = HidClassPDO_GetCollectionDescription(&PDODeviceExtension->Common.DeviceDescription,
                                                                       PDODeviceExtension->CollectionNumber);

                            //
                            // Bound by the request as well as by the report.
                            // HidClass_Read refuses anything shorter before it
                            // ever reaches this queue, so this is belt and
                            // braces - but it is a copy into a caller's buffer
                            // and it is worth being unable to get it wrong.
                            //
                            copyLength = (desc != NULL) ? desc->InputLength : 0;
                            if (copyLength > IoGetCurrentIrpStackLocation(tempIRP)->Parameters.Read.Length)
                            {
                                copyLength = IoGetCurrentIrpStackLocation(tempIRP)->Parameters.Read.Length;
                            }
                            if (copyLength > DeviceExtension->InputBufferSize)
                            {
                                copyLength = DeviceExtension->InputBufferSize;
                            }

                            RtlCopyMemory(address, DeviceExtension->InputBuffer, copyLength);
                            tempIRP->IoStatus.Information = copyLength;
                            tempIRP->IoStatus.Status = STATUS_SUCCESS;
                        }
                        else
                        {
                            tempIRP->IoStatus.Status = STATUS_INVALID_USER_BUFFER;
                            tempIRP->IoStatus.Information = 0;
                        }
                    }

                    // Mark this IRP to be cenceled after exiting spinLock
                    InsertHeadList(&completeIRP, &tempIRP->Tail.Overlay.ListEntry);
                }
            }

            // No IRP waiting data, put it into the buffer
            if (IsListEmpty(&completeIRP) == TRUE)
            {
                HidClass_CyclicBufferPut(&PDODeviceExtension->InputBuffer, DeviceExtension->InputBuffer);
            }

            KeReleaseSpinLock(&PDODeviceExtension->ReadLock, CurrentIRQL);

            // complete waiting IRPs
            {
                PIRP nextIrp;
                PLIST_ENTRY listEntry;
                while (IsListEmpty(&completeIRP) == FALSE)
                {
                    listEntry = RemoveHeadList(&completeIRP);
                    nextIrp = CONTAINING_RECORD(listEntry, IRP, Tail.Overlay.ListEntry);

                    IoCompleteRequest(nextIrp, IO_NO_INCREMENT);
                }
            }
        }
    }

    // re-init read
    HidClassFDO_SubmitRead(DeviceExtension);

    /* stop completion */
    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS
HidClassFDO_SubmitRead(IN PHIDCLASS_FDO_EXTENSION DeviceExtension)
{
    PIO_STACK_LOCATION IoStack;
    NTSTATUS Status;

    /*
     * HidClassFDO_ReadCompletion re-arms the read by calling straight back in
     * here, and this dispatches into the minidriver on the caller's own stack
     * rather than through IoCallDriver. A read the minidriver fails outright -
     * anything outside the three statuses the completion routine gives up on -
     * therefore completes before this returns, re-enters the completion routine
     * on this stack and submits again, without bound. Count the submissions
     * instead: the first caller owns the loop below, and a caller nested inside
     * it only leaves its request behind and returns.
     */
    if (InterlockedIncrement(&DeviceExtension->ReadSubmitCount) > 1)
    {
        return STATUS_PENDING;
    }

    do
    {
        /* re-use irp */
        IoReuseIrp(DeviceExtension->InputIRP, STATUS_SUCCESS);

        /* init irp */
        DeviceExtension->InputIRP->UserBuffer = DeviceExtension->InputWriteAddress;

        /* get next stack location */
        IoStack = IoGetNextIrpStackLocation(DeviceExtension->InputIRP);

        /* init stack location */
        IoStack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
        IoStack->Parameters.DeviceIoControl.IoControlCode = IOCTL_HID_READ_REPORT;
        IoStack->Parameters.DeviceIoControl.OutputBufferLength = DeviceExtension->InputBufferSize;
        IoStack->Parameters.DeviceIoControl.InputBufferLength = 0;
        IoStack->Parameters.DeviceIoControl.Type3InputBuffer = NULL;
        IoStack->DeviceObject = ((PHIDCLASS_PDO_DEVICE_EXTENSION)DeviceExtension->DeviceRelations->Objects[0]->DeviceExtension)->FDODeviceObject;
        //IoStack->DeviceObject = DeviceExtension->DeviceRelations->Objects[0];

        /* set completion routine */
        IoSetCompletionRoutine(DeviceExtension->InputIRP, HidClassFDO_ReadCompletion, DeviceExtension, TRUE, TRUE, TRUE);

        // TODO: create function for this.
        // let's dispatch the request
        IoSetNextIrpStackLocation(DeviceExtension->InputIRP);
        Status = DeviceExtension->Common.DriverExtension->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL](((PHIDCLASS_PDO_DEVICE_EXTENSION)DeviceExtension->DeviceRelations->Objects[0]->DeviceExtension)->FDODeviceObject, DeviceExtension->InputIRP);

    } while (InterlockedDecrement(&DeviceExtension->ReadSubmitCount) > 0);

    /* done */
    return Status;
}

NTSTATUS
HidClassFDO_InitiateRead(IN PHIDCLASS_FDO_EXTENSION DeviceExtension)
{
    if (DeviceExtension->IsReadLoopStarted == FALSE)
    {
        DeviceExtension->IsReadLoopStarted = TRUE;

        DeviceExtension->InputWriteAddress = DeviceExtension->InputBuffer;
        if (DeviceExtension->Common.DeviceDescription.ReportIDsLength == 1 && DeviceExtension->Common.DeviceDescription.ReportIDs[0].ReportID == 0)
        {
            DeviceExtension->InputWriteAddress[0] = 0;
            DeviceExtension->InputWriteAddress++;
            DeviceExtension->InputBufferSize--;
        }

        DPRINT1("[HIDCLASS] HidClassFDO_InitiateRead for address %p and size %x\n", DeviceExtension->InputWriteAddress, DeviceExtension->InputBufferSize);

        HidClassFDO_SubmitRead(DeviceExtension);
    }

    return STATUS_SUCCESS;
}

NTSTATUS
HidClassFDO_StartDevice(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    NTSTATUS Status;
    PHIDCLASS_FDO_EXTENSION FDODeviceExtension;

    //
    // get device extension
    //
    FDODeviceExtension = DeviceObject->DeviceExtension;
    ASSERT(FDODeviceExtension->Common.IsFDO);

    //
    // query capabilities
    //
    Status = HidClassFDO_QueryCapabilities(DeviceObject, &FDODeviceExtension->Capabilities);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("[HIDCLASS] Failed to retrieve capabilities %x\n", Status);
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    //
    // let's start the lower device too
    //
    IoSkipCurrentIrpStackLocation(Irp);
    Status = HidClassFDO_DispatchRequestSynchronous(DeviceObject, Irp);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("[HIDCLASS] Failed to start lower device with %x\n", Status);
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    //
    // let's get the descriptors
    //
    Status = HidClassFDO_GetDescriptors(DeviceObject);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("[HIDCLASS] Failed to retrieve the descriptors %x\n", Status);
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    //
    // Dump the report descriptor the device gave us, 16 bytes to a line.
    //
    // Everything the parser decides about a device - which usages exist, how
    // wide each field is, where it sits in the report - comes out of these
    // bytes, and nothing else in the stack ever shows them. Without it a field
    // that reads back wrong cannot be told apart from a field the parser placed
    // wrong, which is the difference between a device quirk and a bug here.
    //
    {
        ULONG Length = FDODeviceExtension->HidDescriptor.DescriptorList[0].wReportLength;
        ULONG Offset;

        DPRINT1("[HIDCLASS] report descriptor for %p, %lu byte(s):\n",
                DeviceObject, Length);

        for (Offset = 0; Offset < Length; Offset += 16)
        {
            CHAR  Line[16 * 3 + 1];
            ULONG Count = min(16, Length - Offset);
            ULONG i;

            for (i = 0; i < Count; i++)
            {
                CHAR *p = &Line[i * 3];
                UCHAR b = FDODeviceExtension->ReportDescriptor[Offset + i];

                p[0] = "0123456789abcdef"[b >> 4];
                p[1] = "0123456789abcdef"[b & 0xF];
                p[2] = ' ';
            }
            Line[Count * 3] = '\0';

            DPRINT1("[HIDCLASS]   %04lx: %s\n", Offset, Line);
        }
    }

    //
    // now get the the collection description
    //
    Status = HidP_GetCollectionDescription(FDODeviceExtension->ReportDescriptor, FDODeviceExtension->HidDescriptor.DescriptorList[0].wReportLength, NonPagedPool, &FDODeviceExtension->Common.DeviceDescription);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("[HIDCLASS] Failed to retrieve the collection description %x\n", Status);
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    USHORT maxInputLen = 0;
    for (int i = 0; i < FDODeviceExtension->Common.DeviceDescription.CollectionDescLength; i++)
    {
        if (FDODeviceExtension->Common.DeviceDescription.CollectionDesc[i].InputLength > maxInputLen)
        {
            maxInputLen = FDODeviceExtension->Common.DeviceDescription.CollectionDesc[i].InputLength;
        }
    }
    FDODeviceExtension->InputBufferSize = maxInputLen;
    FDODeviceExtension->InputBuffer = ExAllocatePoolWithTag(NonPagedPool, FDODeviceExtension->InputBufferSize + 1, HIDCLASS_TAG);

    FDODeviceExtension->InputIRP = IoAllocateIrp(DeviceObject->StackSize, FALSE);

    // trigger the IRP_MN_QUERY_DEVICE_RELATIONS
    IoInvalidateDeviceRelations(FDODeviceExtension->Common.HidDeviceExtension.PhysicalDeviceObject, BusRelations);

    //
    // complete request
    //
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

NTSTATUS
HidClassFDO_RemoveDevice(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    NTSTATUS Status;
    PHIDCLASS_FDO_EXTENSION FDODeviceExtension;

    //
    // get device extension
    //
    FDODeviceExtension = DeviceObject->DeviceExtension;
    ASSERT(FDODeviceExtension->Common.IsFDO);

    /* FIXME cleanup */

    //
    // dispatch to minidriver
    //
    IoSkipCurrentIrpStackLocation(Irp);
    Status = HidClassFDO_DispatchRequestSynchronous(DeviceObject, Irp);

    //
    // complete request
    //
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    //
    // detach and delete device
    //
    IoDetachDevice(FDODeviceExtension->Common.HidDeviceExtension.NextDeviceObject);
    IoDeleteDevice(DeviceObject);

    return Status;
}

NTSTATUS
HidClassFDO_CopyDeviceRelations(
    IN PDEVICE_OBJECT DeviceObject,
    OUT PDEVICE_RELATIONS *OutRelations)
{
    PDEVICE_RELATIONS DeviceRelations;
    PHIDCLASS_FDO_EXTENSION FDODeviceExtension;
    ULONG Index;

    //
    // get device extension
    //
    FDODeviceExtension = DeviceObject->DeviceExtension;
    ASSERT(FDODeviceExtension->Common.IsFDO);

    //
    // allocate result
    //
    DeviceRelations = ExAllocatePoolWithTag(NonPagedPool,
                                            sizeof(DEVICE_RELATIONS) + (FDODeviceExtension->DeviceRelations->Count - 1) * sizeof(PDEVICE_OBJECT),
                                            HIDCLASS_TAG);
    if (!DeviceRelations)
    {
        //
        // no memory
        //
        *OutRelations = NULL;
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    //
    // copy device objects
    //
    for (Index = 0; Index < FDODeviceExtension->DeviceRelations->Count; Index++)
    {
        //
        // reference pdo
        //
        ObReferenceObject(FDODeviceExtension->DeviceRelations->Objects[Index]);

        //
        // store object
        //
        DeviceRelations->Objects[Index] = FDODeviceExtension->DeviceRelations->Objects[Index];
    }

    //
    // set object count
    //
    DeviceRelations->Count = FDODeviceExtension->DeviceRelations->Count;

    //
    // store result
    //
    *OutRelations = DeviceRelations;
    return STATUS_SUCCESS;
}

NTSTATUS
HidClassFDO_DeviceRelations(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PHIDCLASS_FDO_EXTENSION FDODeviceExtension;
    PIO_STACK_LOCATION IoStack;
    NTSTATUS Status;
    PDEVICE_RELATIONS DeviceRelations;

    //
    // get device extension
    //
    FDODeviceExtension = DeviceObject->DeviceExtension;
    ASSERT(FDODeviceExtension->Common.IsFDO);

    //
    // get current irp stack location
    //
    IoStack = IoGetCurrentIrpStackLocation(Irp);

    //
    // check relations type
    //
    if (IoStack->Parameters.QueryDeviceRelations.Type != BusRelations)
    {
        //
        // only bus relations are handled
        //
        IoSkipCurrentIrpStackLocation(Irp);
        return IoCallDriver(FDODeviceExtension->Common.HidDeviceExtension.NextDeviceObject, Irp);
    }

    if (FDODeviceExtension->DeviceRelations == NULL)
    {
        //
        // time to create the pdos
        //
        Status = HidClassPDO_CreatePDO(DeviceObject, &FDODeviceExtension->DeviceRelations);
        if (!NT_SUCCESS(Status))
        {
            //
            // failed
            //
            DPRINT1("[HIDCLASS] HidClassPDO_CreatePDO failed with %x\n", Status);
            Irp->IoStatus.Status = Status;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return STATUS_SUCCESS;
        }
        //
        // sanity check
        //
        ASSERT(FDODeviceExtension->DeviceRelations->Count > 0);
    }

    //
    // now copy device relations
    //
    Status = HidClassFDO_CopyDeviceRelations(DeviceObject, &DeviceRelations);
    //
    // store result
    //
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = (ULONG_PTR)DeviceRelations;

    //
    // complete request
    //
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

NTSTATUS
HidClassFDO_PnP(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PIO_STACK_LOCATION IoStack;
    PHIDCLASS_FDO_EXTENSION FDODeviceExtension;
    NTSTATUS Status;

    //
    // get device extension
    //
    FDODeviceExtension = DeviceObject->DeviceExtension;
    ASSERT(FDODeviceExtension->Common.IsFDO);

    //
    // get current irp stack location
    //
    IoStack = IoGetCurrentIrpStackLocation(Irp);
    switch (IoStack->MinorFunction)
    {
        case IRP_MN_START_DEVICE:
        {
             return HidClassFDO_StartDevice(DeviceObject, Irp);
        }
        case IRP_MN_REMOVE_DEVICE:
        {
             return HidClassFDO_RemoveDevice(DeviceObject, Irp);
        }
        case IRP_MN_QUERY_DEVICE_RELATIONS:
        {
             return HidClassFDO_DeviceRelations(DeviceObject, Irp);
        }
        case IRP_MN_QUERY_REMOVE_DEVICE:
        case IRP_MN_QUERY_STOP_DEVICE:
        case IRP_MN_CANCEL_REMOVE_DEVICE:
        case IRP_MN_CANCEL_STOP_DEVICE:
        {
            //
            // set status to success and fall through
            //
            Irp->IoStatus.Status = STATUS_SUCCESS;
        }
        default:
        {
            //
            // dispatch to mini driver
            //
           IoCopyCurrentIrpStackLocationToNext(Irp);
           Status = HidClassFDO_DispatchRequest(DeviceObject, Irp);
           return Status;
        }
    }
}
