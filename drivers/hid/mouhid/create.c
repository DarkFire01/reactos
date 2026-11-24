/*
 * PROJECT:     ReactOS HID Mouse Mapper Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Create and close handling and wheel resolution setup
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "mouhid.h"

#define NDEBUG
#include <debug.h>

static IO_COMPLETION_ROUTINE MouHidCreateComplete;

static
NTSTATUS
NTAPI
MouHidCreateComplete(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/**
 * @brief
 * Programs the resolution multiplier feature that belongs to a wheel.
 *
 * @param[out] Multiplier
 * Receives the number of reports the device now sends per detent, or 1.
 *
 * @return
 * STATUS_NOT_SUPPORTED when the wheel has no usable multiplier.
 */
static
NTSTATUS
NTAPI
MouHidSetResolutionFeature(
    _In_ PMOUHID_EXTENSION Extension,
    _In_ USAGE UsagePage,
    _In_ USAGE Usage,
    _Out_ PLONG Multiplier)
{
    PMOUHID_COLLECTION Collection = Extension->Collection;
    HIDP_VALUE_CAPS ValueCaps;
    USHORT CapsLength = 1;
    USHORT FeatureLength = Collection->Caps.FeatureReportByteLength;
    USHORT LinkCollection;
    PCHAR Feature;
    LONG Scale;
    ULONG Exponent;
    NTSTATUS Status;

    RtlZeroMemory(&ValueCaps, sizeof(ValueCaps));
    *Multiplier = 1;

    if (FeatureLength == 0)
        return STATUS_NOT_SUPPORTED;

    Status = HidP_GetSpecificValueCaps(HidP_Input,
                                       UsagePage,
                                       0,
                                       Usage,
                                       &ValueCaps,
                                       &CapsLength,
                                       Collection->PreparsedData);
    if (!NT_SUCCESS(Status) || (CapsLength != 1) || (ValueCaps.LinkCollection == 0))
        return STATUS_NOT_SUPPORTED;

    /* The multiplier must share the logical collection of the wheel */
    LinkCollection = ValueCaps.LinkCollection;
    Status = HidP_GetSpecificValueCaps(HidP_Feature,
                                       HID_USAGE_PAGE_GENERIC,
                                       LinkCollection,
                                       MOUHID_USAGE_RESOLUTION_MULTIPLIER,
                                       &ValueCaps,
                                       &CapsLength,
                                       Collection->PreparsedData);
    if (!NT_SUCCESS(Status) || (CapsLength != 1) || (ValueCaps.LogicalMax <= ValueCaps.LogicalMin))
        return STATUS_NOT_SUPPORTED;

    Feature = ExAllocatePool2(POOL_FLAG_NON_PAGED, FeatureLength, MOUHID_TAG);
    if (Feature == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Feature[0] = (CHAR)ValueCaps.ReportID;
    Status = MouHidCallHidClass(Extension, IOCTL_HID_GET_FEATURE, NULL, 0, Feature, FeatureLength);
    if (NT_SUCCESS(Status))
    {
        Status = HidP_SetUsageValue(HidP_Feature,
                                    HID_USAGE_PAGE_GENERIC,
                                    LinkCollection,
                                    MOUHID_USAGE_RESOLUTION_MULTIPLIER,
                                    (ULONG)ValueCaps.LogicalMax,
                                    Collection->PreparsedData,
                                    Feature,
                                    FeatureLength);
    }

    if (NT_SUCCESS(Status))
        Status = MouHidCallHidClass(Extension, IOCTL_HID_SET_FEATURE, Feature, FeatureLength, NULL, 0);

    if (NT_SUCCESS(Status))
    {
        Scale = 1;
        for (Exponent = 0; Exponent < ValueCaps.UnitsExp; Exponent++)
            Scale *= 10;

        *Multiplier = ValueCaps.PhysicalMax * Scale;
    }
    else
    {
        DPRINT1("MouHid: resolution multiplier setup failed 0x%08lx\n", Status);
    }

    ExFreePoolWithTag(Feature, MOUHID_TAG);
    return Status;
}

/**
 * @brief
 * Switches the vertical and horizontal wheels to high resolution when the
 * device and the registry allow it, then records the multipliers.
 */
static
NTSTATUS
NTAPI
MouHidSetResolutionMultipliers(
    _In_ PMOUHID_EXTENSION Extension)
{
    PMOUHID_COLLECTION Collection = Extension->Collection;
    LONG Vertical = 1;
    LONG Horizontal = 1;
    LONG Value;
    KIRQL OldIrql;
    NTSTATUS Status = STATUS_SUCCESS;

    if ((Collection->BitSizeWheel != 0) && !Extension->VScrollHighResolutionDisable)
    {
        Status = MouHidSetResolutionFeature(Extension,
                                            Collection->WheelUsagePage,
                                            Collection->WheelUsage,
                                            &Value);
        if (NT_SUCCESS(Status))
            Vertical = Value;
    }

    /* A failed vertical wheel leaves the horizontal one alone */
    if (NT_SUCCESS(Status) &&
        (Collection->BitSizeHWheel != 0) &&
        !Extension->HScrollHighResolutionDisable)
    {
        Status = MouHidSetResolutionFeature(Extension,
                                            Collection->HWheelUsagePage,
                                            Collection->HWheelUsage,
                                            &Value);
        if (NT_SUCCESS(Status))
            Horizontal = Value;
    }

    KeAcquireSpinLock(&Extension->WheelLock, &OldIrql);
    Extension->VResolutionMultiplier = Vertical;
    Extension->HResolutionMultiplier = Horizontal;
    KeReleaseSpinLock(&Extension->WheelLock, OldIrql);

    return Status;
}

/**
 * @brief
 * Runs the resolution multiplier setup unless a close is in progress.
 */
VOID
NTAPI
MouHidApplyResolutionMultipliers(
    _In_ PMOUHID_EXTENSION Extension)
{
    NTSTATUS Status;

    KeClearEvent(&Extension->MultiplierEvent);

    if (InterlockedCompareExchange(&Extension->FileState,
                                   MOUHID_FILE_MULTIPLIER_BUSY,
                                   MOUHID_FILE_OPEN) == MOUHID_FILE_OPEN)
    {
        Status = MouHidSetResolutionMultipliers(Extension);
        InterlockedExchange(&Extension->MultiplierState,
                            (Status == STATUS_NOT_SUPPORTED) ? MOUHID_MULTIPLIER_UNSUPPORTED :
                                                               MOUHID_MULTIPLIER_APPLIED);
        InterlockedCompareExchange(&Extension->FileState,
                                   MOUHID_FILE_OPEN,
                                   MOUHID_FILE_MULTIPLIER_BUSY);
    }

    KeSetEvent(&Extension->MultiplierEvent, IO_NO_INCREMENT, FALSE);
}

/**
 * @brief
 * Forwards opens to the HID collection once mouclass has connected. The first
 * successful open that asks for read access owns the read loop.
 */
NTSTATUS
NTAPI
MouHidCreate(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    PMOUHID_EXTENSION Extension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT ReadFile = NULL;
    KEVENT Event;
    NTSTATUS Status;

    /* Nothing lives below the device name */
    if (Stack->FileObject->FileName.Length != 0)
    {
        Status = STATUS_ACCESS_DENIED;
        Irp->IoStatus.Status = Status;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    Status = IoAcquireRemoveLock(&Extension->RemoveLock, Irp);
    if (!NT_SUCCESS(Status))
    {
        Irp->IoStatus.Status = Status;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    ExAcquireFastMutex(&Extension->CreateMutex);

    if (Extension->ConnectData.ClassService == NULL)
    {
        DPRINT1("MouHid: open before IOCTL_INTERNAL_MOUSE_CONNECT\n");
        Status = STATUS_UNSUCCESSFUL;
    }
    else
    {
        IoCopyCurrentIrpStackLocationToNext(Irp);
        KeInitializeEvent(&Event, NotificationEvent, FALSE);
        IoSetCompletionRoutine(Irp, MouHidCreateComplete, &Event, TRUE, TRUE, TRUE);

        Status = IoCallDriver(Extension->TopOfStack, Irp);
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        if (NT_SUCCESS(Status))
            Status = Irp->IoStatus.Status;

        if (NT_SUCCESS(Status) &&
            (Extension->ReadFile == NULL) &&
            (Stack->Parameters.Create.SecurityContext->DesiredAccess & FILE_READ_DATA))
        {
            InterlockedIncrement(&Extension->EnableCount);

            ReadFile = Stack->FileObject;
            Extension->ReadFile = ReadFile;
            InterlockedExchange(&Extension->FileState, MOUHID_FILE_OPEN);
            KeClearEvent(&Extension->ReadStoppedEvent);
            InterlockedExchange(&Extension->ReadState, MOUHID_READ_REARM);

            /* The read loop holds its own remove lock reference until it stops */
            (VOID)IoAcquireRemoveLock(&Extension->RemoveLock, Extension->ReadIrp);
            Extension->ReadIrp->IoStatus.Status = STATUS_SUCCESS;

            Status = MouHidStartRead(Extension);
            if (Status == STATUS_PENDING)
                Status = STATUS_SUCCESS;
            else if (!NT_SUCCESS(Status))
                Extension->ReadFile = NULL;
        }
    }

    ExReleaseFastMutex(&Extension->CreateMutex);

    if (NT_SUCCESS(Status) &&
        (ReadFile != NULL) &&
        (Extension->MultiplierState == MOUHID_MULTIPLIER_PENDING))
    {
        MouHidApplyResolutionMultipliers(Extension);
    }

    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
    return Status;
}

/**
 * @brief
 * Stops the read loop when its owning handle closes, then forwards the close.
 */
NTSTATUS
NTAPI
MouHidClose(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    PMOUHID_EXTENSION Extension = DeviceObject->DeviceExtension;
    PFILE_OBJECT FileObject = IoGetCurrentIrpStackLocation(Irp)->FileObject;

    ExAcquireFastMutex(&Extension->CreateMutex);

    if ((FileObject != NULL) && (FileObject == Extension->ReadFile))
    {
        InterlockedDecrement(&Extension->EnableCount);

        /* The read has to reach the HID collection before it can be cancelled */
        KeWaitForSingleObject(&Extension->ReadSentEvent, Executive, KernelMode, FALSE, NULL);
        IoCancelIrp(Extension->ReadIrp);
        KeWaitForSingleObject(&Extension->ReadStoppedEvent, Executive, KernelMode, FALSE, NULL);

        if (InterlockedExchange(&Extension->FileState, MOUHID_FILE_CLOSING) == MOUHID_FILE_MULTIPLIER_BUSY)
            KeWaitForSingleObject(&Extension->MultiplierEvent, Executive, KernelMode, FALSE, NULL);

        Extension->ReadFile = NULL;
    }

    ExReleaseFastMutex(&Extension->CreateMutex);

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(Extension->TopOfStack, Irp);
}
