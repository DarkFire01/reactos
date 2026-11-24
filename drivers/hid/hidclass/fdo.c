/*
 * PROJECT:     ReactOS HID Class Driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Plug and Play handling of the device FDO
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

#define NDEBUG
#include <debug.h>

/* The report descriptor is asked for this many times before start gives up */
#define HIDCLASS_REPORT_DESCRIPTOR_TRIES    3

PHIDP_COLLECTION_DESC
NTAPI
HidClassFindCollectionDesc(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ ULONG CollectionNumber)
{
    ULONG Index;

    if (!Fdo->DeviceDesc.CollectionDesc)
        return NULL;

    for (Index = 0; Index < Fdo->DeviceDesc.CollectionDescLength; Index++)
    {
        if (Fdo->DeviceDesc.CollectionDesc[Index].CollectionNumber == CollectionNumber)
            return &Fdo->DeviceDesc.CollectionDesc[Index];
    }

    return NULL;
}

PHIDCLASS_COLLECTION
NTAPI
HidClassFindCollection(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ ULONG CollectionNumber)
{
    ULONG Index;

    if (!Fdo->Collections)
        return NULL;

    for (Index = 0; Index < Fdo->DeviceDesc.CollectionDescLength; Index++)
    {
        if (Fdo->Collections[Index].CollectionNumber == CollectionNumber)
            return &Fdo->Collections[Index];
    }

    return NULL;
}

PHIDP_REPORT_IDS
NTAPI
HidClassFindReportId(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ UCHAR ReportId)
{
    ULONG Index;

    if (!Fdo->DeviceDesc.ReportIDs)
        return NULL;

    for (Index = 0; Index < Fdo->DeviceDesc.ReportIDsLength; Index++)
    {
        if (Fdo->DeviceDesc.ReportIDs[Index].ReportID == ReportId)
            return &Fdo->DeviceDesc.ReportIDs[Index];
    }

    return NULL;
}

/**
 * @brief
 * Tells whether reports of this device start with a report ID byte. A device
 * without report IDs has a single report ID entry of zero.
 */
BOOLEAN
NTAPI
HidClassDeviceUsesReportIds(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo)
{
    return (Fdo->DeviceDesc.ReportIDs[0].ReportID != 0);
}

NTSTATUS
NTAPI
HidClassCopyCollectionInformation(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ ULONG CollectionNumber,
    _Out_writes_bytes_(*Length) PVOID Buffer,
    _Inout_ PULONG Length)
{
    HID_COLLECTION_INFORMATION Information;
    PHIDP_COLLECTION_DESC CollectionDesc;
    ULONG CopyLength;

    CollectionDesc = HidClassFindCollectionDesc(Fdo, CollectionNumber);
    if (!CollectionDesc)
        return STATUS_DATA_ERROR;

    RtlZeroMemory(&Information, sizeof(Information));
    Information.DescriptorSize = CollectionDesc->PreparsedDataLength;
    Information.Polled = Fdo->DriverExtension->DevicesArePolled;
    Information.VendorID = Fdo->Attributes.VendorID;
    Information.ProductID = Fdo->Attributes.ProductID;
    Information.VersionNumber = Fdo->Attributes.VersionNumber;

    CopyLength = min(*Length, sizeof(Information));
    RtlCopyMemory(Buffer, &Information, CopyLength);

    /* The caller learns the full size even when its buffer is short */
    *Length = sizeof(Information);
    return (CopyLength < sizeof(Information)) ? STATUS_INVALID_BUFFER_SIZE : STATUS_SUCCESS;
}

/**
 * @brief
 * Copies the preparsed data of a collection. A short buffer gets the part
 * that fits and STATUS_INVALID_BUFFER_SIZE.
 *
 * @param[in] UserBuffer
 * TRUE when Buffer is a user mode address, the caller handles exceptions.
 */
NTSTATUS
NTAPI
HidClassCopyCollectionDescriptor(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ ULONG CollectionNumber,
    _Out_writes_bytes_(*Length) PVOID Buffer,
    _Inout_ PULONG Length,
    _In_ BOOLEAN UserBuffer)
{
    PHIDP_COLLECTION_DESC CollectionDesc;
    ULONG CopyLength;
    NTSTATUS Status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(UserBuffer);

    CollectionDesc = HidClassFindCollectionDesc(Fdo, CollectionNumber);
    if (!CollectionDesc)
        return STATUS_DATA_ERROR;

    CopyLength = CollectionDesc->PreparsedDataLength;
    if (*Length < CopyLength)
    {
        CopyLength = *Length;
        Status = STATUS_INVALID_BUFFER_SIZE;
    }

    RtlCopyMemory(Buffer, CollectionDesc->PreparsedData, CopyLength);
    *Length = CollectionDesc->PreparsedDataLength;
    return Status;
}

VOID
NTAPI
HidClassFreeUnreadReports(
    _In_ PHIDCLASS_COLLECTION Collection)
{
    while (!IsListEmpty(&Collection->UnreadReports))
    {
        PLIST_ENTRY Entry = RemoveHeadList(&Collection->UnreadReports);

        ExFreePoolWithTag(CONTAINING_RECORD(Entry, HIDCLASS_REPORT, Link), HIDCLASS_TAG);
    }

    Collection->UnreadReportCount = 0;
}

/**
 * @brief
 * Adjusts the open counts of a collection, its PDO and the FDO.
 *
 * @return
 * The FDO count after the change. It drops to zero only once the device was
 * removed and the last handle closed.
 */
LONG
NTAPI
HidClassUpdateOpenCount(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_opt_ PHIDCLASS_COLLECTION Collection,
    _In_opt_ PHIDCLASS_PDO_EXTENSION Pdo,
    _In_ BOOLEAN Open)
{
    BOOLEAN PolicyChanged;
    LONG Delta = Open ? 1 : -1;
    LONG Count;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Fdo->OpenCountLock, &OldIrql);

    if (Collection)
        Collection->OpenCount += Delta;

    if (Pdo)
        Pdo->OpenCount += Delta;

    Fdo->OpenCount += Delta;
    Count = Fdo->OpenCount;

    /* The idle policy differs between no handles and at least one */
    PolicyChanged = Open ? (Count == 2) : (Count == 1);

    KeReleaseSpinLock(&Fdo->OpenCountLock, OldIrql);

    if (PolicyChanged)
        HidClassUpdateIdlePolicy(Fdo);

    return Count;
}

static
NTSTATUS
NTAPI
HidClassQueryCapabilitiesCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent(Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/**
 * @brief
 * Asks the bus driver below the minidriver for the device capabilities.
 */
static
NTSTATUS
NTAPI
HidClassQueryCapabilities(
    _In_ PDEVICE_OBJECT LowerDevice,
    _Out_ PDEVICE_CAPABILITIES Capabilities)
{
    PIO_STACK_LOCATION IoStack;
    NTSTATUS Status;
    KEVENT Event;
    PIRP Irp;

    Irp = IoAllocateIrp(LowerDevice->StackSize, FALSE);
    if (!Irp)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(Capabilities, sizeof(*Capabilities));
    Capabilities->Size = sizeof(*Capabilities);
    Capabilities->Version = 1;
    Capabilities->Address = MAXULONG;
    Capabilities->UINumber = MAXULONG;

    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    IoStack = IoGetNextIrpStackLocation(Irp);
    IoStack->MajorFunction = IRP_MJ_PNP;
    IoStack->MinorFunction = IRP_MN_QUERY_CAPABILITIES;
    IoStack->Parameters.DeviceCapabilities.Capabilities = Capabilities;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    IoSetCompletionRoutine(Irp, HidClassQueryCapabilitiesCompletion, &Event, TRUE, TRUE, TRUE);

    Status = IoCallDriver(LowerDevice, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = Irp->IoStatus.Status;
    }

    IoFreeIrp(Irp);
    return Status;
}

/**
 * @brief
 * Reads the report descriptor the HID descriptor announced. The minidriver
 * has to return exactly that many bytes.
 */
static
NTSTATUS
NTAPI
HidClassGetReportDescriptor(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ PIRP Irp,
    _In_ ULONG Length)
{
    PIO_STACK_LOCATION IoStack;
    PUCHAR Descriptor;
    ULONG Attempt;
    NTSTATUS Status = STATUS_DEVICE_DATA_ERROR;

    if (Length == 0)
        return STATUS_DEVICE_DATA_ERROR;

    Descriptor = ExAllocatePoolZero(NonPagedPool, Length, HIDCLASS_TAG);
    if (!Descriptor)
        return STATUS_INSUFFICIENT_RESOURCES;

    for (Attempt = 0; Attempt < HIDCLASS_REPORT_DESCRIPTOR_TRIES; Attempt++)
    {
        IoStack = IoGetNextIrpStackLocation(Irp);
        RtlZeroMemory(IoStack, sizeof(*IoStack));
        IoStack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
        IoStack->Parameters.DeviceIoControl.IoControlCode = IOCTL_HID_GET_REPORT_DESCRIPTOR;
        IoStack->Parameters.DeviceIoControl.OutputBufferLength = Length;
        Irp->UserBuffer = Descriptor;
        Irp->IoStatus.Information = 0;

        Status = HidClassCallMinidriverSynchronous(Fdo, Irp);
        if (NT_SUCCESS(Status))
        {
            if (Irp->IoStatus.Information == Length)
            {
                Fdo->ReportDescriptor = Descriptor;
                Fdo->ReportDescriptorLength = Length;
                return Status;
            }

            DPRINT1("HIDCLASS: report descriptor came back with %Iu of %lu bytes\n",
                    Irp->IoStatus.Information, Length);
            Status = STATUS_DEVICE_DATA_ERROR;
        }
    }

    ExFreePoolWithTag(Descriptor, HIDCLASS_TAG);
    return Status;
}

/**
 * @brief
 * Collects the HID descriptor, the device attributes and the report
 * descriptor from the minidriver.
 */
static
NTSTATUS
NTAPI
HidClassGetDeviceDescriptors(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo)
{
    PIO_STACK_LOCATION IoStack;
    NTSTATUS Status;
    PIRP Irp;

    Irp = IoAllocateIrp(Fdo->DeviceObject->StackSize, FALSE);
    if (!Irp)
        return STATUS_INSUFFICIENT_RESOURCES;

    IoStack = IoGetNextIrpStackLocation(Irp);
    IoStack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    IoStack->Parameters.DeviceIoControl.IoControlCode = IOCTL_HID_GET_DEVICE_DESCRIPTOR;
    IoStack->Parameters.DeviceIoControl.OutputBufferLength = sizeof(Fdo->HidDescriptor);
    IoStack->Parameters.DeviceIoControl.Type3InputBuffer = NULL;
    Irp->UserBuffer = &Fdo->HidDescriptor;

    Status = HidClassCallMinidriverSynchronous(Fdo, Irp);
    if (Status != STATUS_SUCCESS)
    {
        Status = STATUS_DEVICE_DATA_ERROR;
        goto Exit;
    }

    if (Irp->IoStatus.Information != sizeof(Fdo->HidDescriptor))
    {
        DPRINT1("HIDCLASS: HID descriptor has a bad size %Iu\n", Irp->IoStatus.Information);
        Status = STATUS_DEVICE_DATA_ERROR;
        goto Exit;
    }

    IoStack = IoGetNextIrpStackLocation(Irp);
    IoStack->Parameters.DeviceIoControl.IoControlCode = IOCTL_HID_GET_DEVICE_ATTRIBUTES;
    IoStack->Parameters.DeviceIoControl.OutputBufferLength = sizeof(Fdo->Attributes);
    Irp->UserBuffer = &Fdo->Attributes;

    Status = HidClassCallMinidriverSynchronous(Fdo, Irp);
    if (!NT_SUCCESS(Status))
    {
        Status = STATUS_DEVICE_DATA_ERROR;
        goto Exit;
    }

    if (Fdo->HidDescriptor.DescriptorList[0].bReportType != HID_REPORT_DESCRIPTOR_TYPE)
    {
        DPRINT1("HIDCLASS: first class descriptor is not a report descriptor\n");
        Status = STATUS_DEVICE_DATA_ERROR;
        goto Exit;
    }

    Status = HidClassGetReportDescriptor(Fdo, Irp, Fdo->HidDescriptor.DescriptorList[0].wReportLength);

Exit:
    IoFreeIrp(Irp);
    return Status;
}

static
NTSTATUS
NTAPI
HidClassInitializeCollection(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ ULONG Index)
{
    PHIDP_COLLECTION_DESC CollectionDesc = &Fdo->DeviceDesc.CollectionDesc[Index];
    PHIDCLASS_COLLECTION Collection = &Fdo->Collections[Index];
    ULONG Length;
    NTSTATUS Status;

    Collection->CollectionNumber = CollectionDesc->CollectionNumber;
    Collection->CollectionIndex = Index;
    Collection->UsagePage = CollectionDesc->UsagePage;
    Collection->Usage = CollectionDesc->Usage;
    Collection->Kind = HidClassCollectionOther;

    if (CollectionDesc->UsagePage == HID_USAGE_PAGE_GENERIC)
    {
        if (CollectionDesc->Usage == HID_USAGE_GENERIC_POINTER || CollectionDesc->Usage == HID_USAGE_GENERIC_MOUSE)
            Collection->Kind = HidClassCollectionMouse;
        else if (CollectionDesc->Usage == HID_USAGE_GENERIC_KEYBOARD || CollectionDesc->Usage == HID_USAGE_GENERIC_KEYPAD)
            Collection->Kind = HidClassCollectionKeyboard;
    }

    InitializeListHead(&Collection->FileList);
    InitializeListHead(&Collection->UnreadReports);
    InitializeListHead(&Collection->PolledReads);
    KeInitializeSpinLock(&Collection->FileLock);
    KeInitializeSpinLock(&Collection->PowerEventLock);
    KeInitializeSpinLock(&Collection->SecureReadLock);
    KeInitializeSpinLock(&Collection->PolledLock);
    KeInitializeTimer(&Collection->PollTimer);

    Length = sizeof(Collection->Information);
    Status = HidClassCopyCollectionInformation(Fdo,
                                               Collection->CollectionNumber,
                                               &Collection->Information,
                                               &Length);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Keep a private copy of the preparsed data for power events */
    Length = Collection->Information.DescriptorSize;
    if (Length == 0)
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    Collection->PreparsedData = ExAllocatePoolZero(NonPagedPool, Length, HIDCLASS_TAG);
    if (!Collection->PreparsedData)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = HidClassCopyCollectionDescriptor(Fdo,
                                              Collection->CollectionNumber,
                                              Collection->PreparsedData,
                                              &Length,
                                              FALSE);
    if (!NT_SUCCESS(Status))
        return Status;

    if (CollectionDesc->InputLength)
        Fdo->NoInputCollections = FALSE;

    return STATUS_SUCCESS;
}

static
VOID
NTAPI
HidClassFreeDeviceResources(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo)
{
    ULONG Index;

    if (Fdo->Collections)
    {
        for (Index = 0; Index < Fdo->DeviceDesc.CollectionDescLength; Index++)
        {
            PHIDCLASS_COLLECTION Collection = &Fdo->Collections[Index];

            if (Collection->PolledReport)
                ExFreePoolWithTag(Collection->PolledReport, HIDCLASS_TAG);

            if (Collection->PreparsedData)
                ExFreePoolWithTag(Collection->PreparsedData, HIDCLASS_TAG);

            HidClassFreeUnreadReports(Collection);
        }

        ExFreePoolWithTag(Fdo->Collections, HIDCLASS_TAG);
        Fdo->Collections = NULL;
    }

    if (Fdo->DeviceDescValid)
    {
        HidP_FreeCollectionDescription(&Fdo->DeviceDesc);
        Fdo->DeviceDescValid = FALSE;
    }
    RtlZeroMemory(&Fdo->DeviceDesc, sizeof(Fdo->DeviceDesc));

    if (Fdo->ReportDescriptor)
    {
        ExFreePoolWithTag(Fdo->ReportDescriptor, HIDCLASS_TAG);
        Fdo->ReportDescriptor = NULL;
    }
}

/**
 * @brief
 * First start of the FDO. Reads the descriptors, splits the device into its
 * top level collections and asks PnP for the collection PDOs.
 */
static
NTSTATUS
NTAPI
HidClassInitializeDevice(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo)
{
    ULONG Index;
    NTSTATUS Status;

    Status = HidClassGetDeviceDescriptors(Fdo);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("HIDCLASS: reading the descriptors failed %lx\n", Status);
        return Status;
    }

    Status = HidP_GetCollectionDescription((PHIDP_REPORT_DESCRIPTOR)Fdo->ReportDescriptor,
                                           Fdo->ReportDescriptorLength,
                                           NonPagedPool,
                                           &Fdo->DeviceDesc);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("HIDCLASS: report descriptor did not parse, error %lu at offset %lu\n",
                Fdo->DeviceDesc.Dbg.ErrorCode, Fdo->DeviceDesc.Dbg.BreakOffset);
        return Status;
    }
    Fdo->DeviceDescValid = TRUE;

    if (Fdo->DeviceDesc.CollectionDescLength == 0 || Fdo->DeviceDesc.ReportIDsLength == 0)
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    Fdo->Collections = ExAllocatePoolZero(NonPagedPool,
                                          Fdo->DeviceDesc.CollectionDescLength * sizeof(*Fdo->Collections),
                                          HIDCLASS_TAG);
    if (!Fdo->Collections)
        return STATUS_INSUFFICIENT_RESOURCES;

    HidClassInitializeFdoWmi(Fdo);

    Fdo->NoInputCollections = TRUE;
    for (Index = 0; Index < Fdo->DeviceDesc.CollectionDescLength; Index++)
    {
        Status = HidClassInitializeCollection(Fdo, Index);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("HIDCLASS: collection %lu failed to initialize %lx\n", Index, Status);
            return Status;
        }
    }

    if (!Fdo->NoInputCollections && !Fdo->DriverExtension->DevicesArePolled)
    {
        Status = HidClassAllocatePingPongs(Fdo);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    HidClassConfigureIdle(Fdo);

    IoInvalidateDeviceRelations(HidClassExtensionFromFdo(Fdo)->Hid.PhysicalDeviceObject, BusRelations);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
HidClassFdoStartDevice(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ PIRP Irp)
{
    PHIDCLASS_DEVICE_EXTENSION DeviceExtension = HidClassExtensionFromFdo(Fdo);
    BOOLEAN Restart = Fdo->Initialized;
    NTSTATUS Status;

    Status = HidClassQueryCapabilities(DeviceExtension->Hid.NextDeviceObject, &Fdo->Capabilities);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("HIDCLASS: capabilities query failed %lx\n", Status);
        goto Exit;
    }

    IoCopyCurrentIrpStackLocationToNext(Irp);
    Status = HidClassCallMinidriverSynchronous(Fdo, Irp);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("HIDCLASS: minidriver failed start %lx\n", Status);
        goto Exit;
    }

    if (!Fdo->Initialized)
    {
        Status = HidClassInitializeDevice(Fdo);
        if (!NT_SUCCESS(Status))
        {
            HidClassFreePingPongs(Fdo);
            HidClassFreeDeviceResources(Fdo);
            goto Exit;
        }
        Fdo->Initialized = TRUE;
    }

    HidClassSetIoState(Fdo, HidClassIoPass);

    Status = HidClassStartReads(Fdo);
    if (!NT_SUCCESS(Status))
        goto Exit;

    if (Restart)
        IoInvalidateDeviceRelations(DeviceExtension->Hid.PhysicalDeviceObject, BusRelations);

Exit:
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

/**
 * @brief
 * Reports the collection PDOs, or none while the minidriver says the device
 * is not present.
 */
static
NTSTATUS
NTAPI
HidClassFdoQueryBusRelations(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ PIRP Irp)
{
    PDEVICE_RELATIONS Relations;
    ULONG Size;
    ULONG Index;
    NTSTATUS Status;

    if (!InterlockedCompareExchange(&Fdo->IsPresent, 0, 0))
    {
        Fdo->RelationsReported = FALSE;
        HidClassStopReads(Fdo);

        Relations = ExAllocatePoolZero(PagedPool, sizeof(*Relations), HIDCLASS_TAG);
        if (Relations)
        {
            if (Fdo->ClientPdoRelations)
            {
                ExFreePoolWithTag(Fdo->ClientPdoRelations, HIDCLASS_TAG);
                Fdo->ClientPdoRelations = NULL;
            }
            Relations->Count = 0;
        }

        if (Fdo->Collections)
        {
            for (Index = 0; Index < Fdo->DeviceDesc.CollectionDescLength; Index++)
            {
                PHIDCLASS_COLLECTION Collection = &Fdo->Collections[Index];
                KIRQL OldIrql;

                if (Collection->Kind != HidClassCollectionKeyboard)
                    continue;

                KeAcquireSpinLock(&Collection->FileLock, &OldIrql);
                HidClassFreeUnreadReports(Collection);
                KeReleaseSpinLock(&Collection->FileLock, OldIrql);
            }
        }

        Irp->IoStatus.Information = (ULONG_PTR)Relations;
        return Relations ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
    }

    if (!Fdo->ClientPdoRelations)
    {
        Status = HidClassCreateCollectionPdos(Fdo);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    Fdo->RelationsReported = TRUE;
    HidClassStartReads(Fdo);

    Size = FIELD_OFFSET(DEVICE_RELATIONS, Objects[Fdo->ClientPdoRelations->Count]);
    Relations = ExAllocatePoolWithTag(PagedPool, Size, HIDCLASS_TAG);
    if (Relations)
    {
        RtlCopyMemory(Relations, Fdo->ClientPdoRelations, Size);
        for (Index = 0; Index < Relations->Count; Index++)
        {
            ObReferenceObject(Relations->Objects[Index]);
            Relations->Objects[Index]->Flags &= ~DO_DEVICE_INITIALIZING;
        }
    }

    Irp->IoStatus.Information = (ULONG_PTR)Relations;
    return Relations ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}

static
VOID
NTAPI
HidClassDeleteDeviceObjects(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo)
{
    PDEVICE_OBJECT DeviceObject = Fdo->DeviceObject;
    ULONG Index;

    if (Fdo->ClientPdoRelations)
    {
        for (Index = 0; Index < Fdo->ClientPdoRelations->Count; Index++)
        {
            PDEVICE_OBJECT Pdo = Fdo->ClientPdoRelations->Objects[Index];

            ObDereferenceObject(Pdo);
            IoDeleteDevice(Pdo);
        }

        ExFreePoolWithTag(Fdo->ClientPdoRelations, HIDCLASS_TAG);
        Fdo->ClientPdoRelations = NULL;
    }

    if (Fdo->ClientPdos)
    {
        ExFreePoolWithTag(Fdo->ClientPdos, HIDCLASS_TAG);
        Fdo->ClientPdos = NULL;
    }

    ObDereferenceObject(DeviceObject);
    IoDeleteDevice(DeviceObject);
}

/**
 * @brief
 * Final teardown, run by remove or by the last close after remove.
 */
VOID
NTAPI
HidClassCleanUpFdo(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo)
{
    HidClassFreeDeviceResources(Fdo);

    if (Fdo->DeviceName.Buffer)
    {
        ExFreePoolWithTag(Fdo->DeviceName.Buffer, HIDCLASS_TAG);
        Fdo->DeviceName.Buffer = NULL;
    }

    if (Fdo->WmiRegistered)
    {
        IoWMIRegistrationControl(Fdo->DeviceObject, WMIREG_ACTION_DEREGISTER);
        Fdo->WmiRegistered = FALSE;
    }

    HidClassDeleteDeviceObjects(Fdo);
}

static
NTSTATUS
NTAPI
HidClassFdoRemoveDevice(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ PIRP Irp)
{
    PHIDCLASS_DEVICE_EXTENSION DeviceExtension = HidClassExtensionFromFdo(Fdo);
    NTSTATUS Status;

    IoAcquireRemoveLock(&Fdo->RemoveLock, Irp);

    HidClassSetIoState(Fdo, HidClassIoFail);
    HidClassCompleteCollectionWaitWakes(Fdo, STATUS_NO_SUCH_DEVICE);
    if (Fdo->WaitWakeIrp)
        IoCancelIrp(Fdo->WaitWakeIrp);

    HidClassFreePingPongs(Fdo);
    HidClassDrainResetNotifications(Fdo);

    IoReleaseRemoveLockAndWait(&Fdo->RemoveLock, Irp);

    HidClassUnregisterPoFx(Fdo);
    if (Fdo->IdleIrp)
    {
        IoCancelIrp(Fdo->IdleIrp);
        KeWaitForSingleObject(&Fdo->IdleIrpDone, Executive, KernelMode, FALSE, NULL);
        IoFreeIrp(Fdo->IdleIrp);
        Fdo->IdleIrp = NULL;
    }

    Irp->IoStatus.Status = STATUS_SUCCESS;
    IoSkipCurrentIrpStackLocation(Irp);
    Status = HidClassCallMinidriver(Fdo, Irp);

    HidClassDereferenceDriver(Fdo->DriverExtension->DriverObject);
    IoDetachDevice(DeviceExtension->Hid.NextDeviceObject);

    /* Open handles keep the FDO until the last of them closes */
    if (HidClassUpdateOpenCount(Fdo, NULL, NULL, FALSE) == 0)
        HidClassCleanUpFdo(Fdo);
    else
        DPRINT1("HIDCLASS: device removed with handles open, cleanup deferred\n");

    return Status;
}

NTSTATUS
NTAPI
HidClassFdoPnp(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ PIRP Irp)
{
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS Status;

    switch (IoStack->MinorFunction)
    {
        case IRP_MN_START_DEVICE:
            return HidClassFdoStartDevice(Fdo, Irp);

        case IRP_MN_REMOVE_DEVICE:
            return HidClassFdoRemoveDevice(Fdo, Irp);

        case IRP_MN_STOP_DEVICE:
            HidClassSetIoState(Fdo, HidClassIoQueue);
            HidClassStopReads(Fdo);

            IoCopyCurrentIrpStackLocationToNext(Irp);
            Status = HidClassCallMinidriverSynchronous(Fdo, Irp);
            Irp->IoStatus.Status = Status;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return Status;

        case IRP_MN_QUERY_STOP_DEVICE:
        case IRP_MN_CANCEL_STOP_DEVICE:
            Irp->IoStatus.Status = STATUS_SUCCESS;
            break;

        case IRP_MN_QUERY_DEVICE_RELATIONS:
            if (IoStack->Parameters.QueryDeviceRelations.Type != BusRelations)
                break;

            Status = HidClassFdoQueryBusRelations(Fdo, Irp);
            if (!NT_SUCCESS(Status))
            {
                Irp->IoStatus.Status = Status;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
                return Status;
            }
            Irp->IoStatus.Status = Status;
            break;

        case IRP_MN_SURPRISE_REMOVAL:
            HidClassSetIoState(Fdo, HidClassIoFail);
            HidClassCompleteCollectionWaitWakes(Fdo, STATUS_NO_SUCH_DEVICE);
            HidClassFreePingPongs(Fdo);
            HidClassDrainResetNotifications(Fdo);
            Irp->IoStatus.Status = STATUS_SUCCESS;
            break;

        default:
            break;
    }

    return HidClassForwardToMinidriver(Fdo, Irp);
}
