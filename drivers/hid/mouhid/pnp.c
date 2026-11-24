/*
 * PROJECT:     ReactOS HID Mouse Mapper Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Device creation, PnP, power and HID collection setup
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "mouhid.h"

#define NDEBUG
#include <debug.h>

NTSYSAPI
NTSTATUS
NTAPI
ZwQueryLicenseValue(
    _In_ PUNICODE_STRING ValueName,
    _Out_opt_ PULONG Type,
    _Out_writes_bytes_to_opt_(DataSize, *ResultDataSize) PVOID Data,
    _In_ ULONG DataSize,
    _Out_ PULONG ResultDataSize);

static IO_COMPLETION_ROUTINE MouHidStartComplete;
static IO_COMPLETION_ROUTINE MouHidPowerComplete;

/**
 * @brief
 * Sends a synchronous IOCTL to the HID collection on the read handle.
 */
NTSTATUS
NTAPI
MouHidCallHidClass(
    _In_ PMOUHID_EXTENSION Extension,
    _In_ ULONG IoControlCode,
    _In_reads_bytes_opt_(InputLength) PVOID InputBuffer,
    _In_ ULONG InputLength,
    _Out_writes_bytes_opt_(OutputLength) PVOID OutputBuffer,
    _In_ ULONG OutputLength)
{
    IO_STATUS_BLOCK IoStatus;
    KEVENT Event;
    PIRP Irp;
    NTSTATUS Status;

    RtlZeroMemory(&IoStatus, sizeof(IoStatus));
    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    Irp = IoBuildDeviceIoControlRequest(IoControlCode,
                                        Extension->TopOfStack,
                                        InputBuffer,
                                        InputLength,
                                        OutputBuffer,
                                        OutputLength,
                                        FALSE,
                                        &Event,
                                        &IoStatus);
    if (Irp == NULL)
        return STATUS_UNSUCCESSFUL;

    IoGetNextIrpStackLocation(Irp)->FileObject = Extension->ReadFile;

    Status = IoCallDriver(Extension->TopOfStack, Irp);
    if (Status == STATUS_PENDING)
        Status = KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);

    if (NT_SUCCESS(Status))
        Status = IoStatus.Status;

    return Status;
}

/**
 * @brief
 * Reads a value of at most four bytes from the device hardware key.
 *
 * @param[in,out] Value
 * Receives the data. Bytes past the stored data length are left as they were.
 */
static
NTSTATUS
NTAPI
MouHidQueryDeviceValue(
    _In_ HANDLE Key,
    _In_ PCWSTR Name,
    _Inout_ PULONG Value)
{
    PKEY_VALUE_FULL_INFORMATION Information;
    UNICODE_STRING ValueName;
    ULONG Length;
    NTSTATUS Status;

    RtlInitUnicodeString(&ValueName, Name);

    Length = FIELD_OFFSET(KEY_VALUE_FULL_INFORMATION, Name) + ValueName.MaximumLength + 2 * sizeof(ULONG);
    Information = ExAllocatePool2(POOL_FLAG_PAGED, Length, MOUHID_TAG);
    if (Information == NULL)
        return STATUS_NO_MEMORY;

    Status = ZwQueryValueKey(Key, &ValueName, KeyValueFullInformation, Information, Length, &Length);
    if (NT_SUCCESS(Status))
    {
        if (Information->DataLength > sizeof(*Value))
            Status = STATUS_BUFFER_TOO_SMALL;
        else
            RtlCopyMemory(Value, (PUCHAR)Information + Information->DataOffset, Information->DataLength);
    }

    ExFreePoolWithTag(Information, MOUHID_TAG);
    return Status;
}

/**
 * @brief
 * Loads the per device wheel options from the device hardware key.
 */
static
VOID
NTAPI
MouHidReadDeviceParameters(
    _In_ PMOUHID_EXTENSION Extension)
{
    HANDLE Key;
    ULONG Value = 0;
    NTSTATUS Status;

    Extension->FlipFlopWheel = FALSE;
    Extension->FlipFlopHScroll = FALSE;
    Extension->VScrollHighResolutionDisable = FALSE;
    Extension->VScrollPageOverride = 0;
    Extension->VScrollUsageOverride = 0;
    Extension->HScrollHighResolutionDisable = FALSE;
    Extension->HScrollPageOverride = 0;
    Extension->HScrollUsageOverride = 0;
    Extension->VResolutionMultiplier = 1;
    Extension->HResolutionMultiplier = 1;

    /* Without the key the wheel scale stays zero, as on Windows */
    Status = IoOpenDeviceRegistryKey(Extension->Pdo, PLUGPLAY_REGKEY_DEVICE, STANDARD_RIGHTS_ALL, &Key);
    if (NT_SUCCESS(Status))
    {
        if (NT_SUCCESS(MouHidQueryDeviceValue(Key, L"FlipFlopWheel", &Value)))
            Extension->FlipFlopWheel = (BOOLEAN)Value;

        if (NT_SUCCESS(MouHidQueryDeviceValue(Key, L"FlipFlopHScroll", &Value)))
            Extension->FlipFlopHScroll = (BOOLEAN)Value;

        if (NT_SUCCESS(MouHidQueryDeviceValue(Key, L"WheelScalingFactor", &Value)))
            Extension->WheelScalingFactor = (LONG)Value;
        else
            Extension->WheelScalingFactor = MOUHID_DEFAULT_WHEEL_SCALE;
        Extension->WheelScale = Extension->WheelScalingFactor;

        if (NT_SUCCESS(MouHidQueryDeviceValue(Key, L"HScrollScalingFactor", &Value)))
            Extension->HScrollScalingFactor = (LONG)Value;
        else
            Extension->HScrollScalingFactor = MOUHID_DEFAULT_WHEEL_SCALE;
        Extension->HScrollScale = Extension->HScrollScalingFactor;

        if (NT_SUCCESS(MouHidQueryDeviceValue(Key, L"VScrollHighResolutionDisable", &Value)))
            Extension->VScrollHighResolutionDisable = (BOOLEAN)Value;

        if (NT_SUCCESS(MouHidQueryDeviceValue(Key, L"VScrollPageOverride", &Value)))
            Extension->VScrollPageOverride = (USAGE)Value;

        if (NT_SUCCESS(MouHidQueryDeviceValue(Key, L"VScrollUsageOverride", &Value)))
            Extension->VScrollUsageOverride = (USAGE)Value;

        if (NT_SUCCESS(MouHidQueryDeviceValue(Key, L"HScrollHighResolutionDisable", &Value)))
            Extension->HScrollHighResolutionDisable = (BOOLEAN)Value;

        if (NT_SUCCESS(MouHidQueryDeviceValue(Key, L"HScrollPageOverride", &Value)))
            Extension->HScrollPageOverride = (USAGE)Value;

        if (NT_SUCCESS(MouHidQueryDeviceValue(Key, L"HScrollUsageOverride", &Value)))
            Extension->HScrollUsageOverride = (USAGE)Value;

        ZwClose(Key);
    }

    /* An override needs both a page and a usage */
    if ((Extension->VScrollPageOverride == 0) != (Extension->VScrollUsageOverride == 0))
    {
        DPRINT1("MouHid: ignoring partial vertical scroll override\n");
        Extension->VScrollPageOverride = 0;
        Extension->VScrollUsageOverride = 0;
    }

    if ((Extension->HScrollPageOverride == 0) != (Extension->HScrollUsageOverride == 0))
    {
        DPRINT1("MouHid: ignoring partial horizontal scroll override\n");
        Extension->HScrollPageOverride = 0;
        Extension->HScrollUsageOverride = 0;
    }
}

NTSTATUS
NTAPI
MouHidAddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject)
{
    PDEVICE_OBJECT DeviceObject = NULL;
    PMOUHID_EXTENSION Extension;
    UNICODE_STRING FamilyName;
    POWER_STATE PowerState;
    ULONG ResultLength = 0;
    NTSTATUS Status;

    Status = IoCreateDevice(DriverObject,
                            sizeof(*Extension),
                            NULL,
                            FILE_DEVICE_MOUSE,
                            0,
                            FALSE,
                            &DeviceObject);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("MouHid: IoCreateDevice failed 0x%08lx\n", Status);
        return Status;
    }

    MouHidGlobals.DeviceFamily = MAXULONG;
    RtlInitUnicodeString(&FamilyName, L"Kernel-OneCore-DeviceFamilyID");
    (VOID)ZwQueryLicenseValue(&FamilyName,
                              NULL,
                              &MouHidGlobals.DeviceFamily,
                              sizeof(MouHidGlobals.DeviceFamily),
                              &ResultLength);

    Extension = DeviceObject->DeviceExtension;
    Extension->TopOfStack = IoAttachDeviceToDeviceStack(DeviceObject, PhysicalDeviceObject);
    if (Extension->TopOfStack == NULL)
    {
        MouHidLogError(DriverObject, MOUHID_LOG_ATTACH_FAILED, STATUS_DEVICE_NOT_CONNECTED);
        IoDeleteDevice(DeviceObject);
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    Extension->Self = DeviceObject;
    Extension->Started = FALSE;
    Extension->Initialized = FALSE;
    Extension->MultiplierState = MOUHID_MULTIPLIER_PENDING;
    Extension->FileState = MOUHID_FILE_NONE;
    Extension->UnitId = (USHORT)InterlockedIncrement(&MouHidGlobals.UnitCount);
    Extension->Pdo = PhysicalDeviceObject;

    /* One IRP is reused for every report read */
    Extension->ReadIrp = IoAllocateIrp(Extension->TopOfStack->StackSize, FALSE);
    if (Extension->ReadIrp == NULL)
    {
        IoDetachDevice(Extension->TopOfStack);
        IoDeleteDevice(DeviceObject);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    KeInitializeEvent(&Extension->ReadStoppedEvent, SynchronizationEvent, FALSE);
    KeInitializeEvent(&Extension->ReadSentEvent, NotificationEvent, TRUE);
    KeInitializeEvent(&Extension->MultiplierEvent, NotificationEvent, TRUE);
    IoInitializeRemoveLock(&Extension->RemoveLock, MOUHID_TAG, 1, 0);
    Extension->ReadFile = NULL;
    ExInitializeFastMutex(&Extension->CreateMutex);
    KeInitializeSpinLock(&Extension->WheelLock);

    Extension->InputData.UnitId = Extension->UnitId;
    Extension->Attributes.MouseIdentifier = MOUSE_HID_HARDWARE;
    Extension->Attributes.SampleRate = 0;
    Extension->Attributes.InputDataQueueLength = 2;

    MouHidReadDeviceParameters(Extension);

    PowerState.DeviceState = PowerDeviceD0;
    PoSetPowerState(DeviceObject, DevicePowerState, PowerState);

    MouHidInitializeWmi(Extension);

    DeviceObject->Flags |= DO_POWER_PAGABLE;
    DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Looks up an input value usage and returns its bit size.
 */
static
BOOLEAN
NTAPI
MouHidFindInputValue(
    _In_ PHIDP_PREPARSED_DATA PreparsedData,
    _In_ USAGE UsagePage,
    _In_ USAGE Usage,
    _Out_ PUSHORT BitSize)
{
    HIDP_VALUE_CAPS ValueCaps;
    USHORT CapsLength = 1;
    NTSTATUS Status;

    Status = HidP_GetSpecificValueCaps(HidP_Input,
                                       UsagePage,
                                       0,
                                       Usage,
                                       &ValueCaps,
                                       &CapsLength,
                                       PreparsedData);
    if (!NT_SUCCESS(Status) || (CapsLength != 1))
        return FALSE;

    *BitSize = ValueCaps.BitSize;
    return TRUE;
}

/**
 * @brief
 * Reads the collection descriptor and builds the report layout.
 *
 * @return
 * STATUS_UNSUCCESSFUL when the collection is not a mouse or pointer.
 */
static
NTSTATUS
NTAPI
MouHidStartDevice(
    _In_ PMOUHID_EXTENSION Extension)
{
    HID_COLLECTION_INFORMATION CollectionInfo;
    PHIDP_PREPARSED_DATA PreparsedData;
    PMOUHID_COLLECTION Collection = NULL;
    HIDP_CAPS Caps;
    HIDP_VALUE_CAPS ValueCaps;
    USHORT CapsLength;
    USHORT MaxUsages;
    USHORT BitSize;
    ULONG ReportSize;
    ULONG ListSize;
    PUCHAR Buffer;
    BOOLEAN Found;
    NTSTATUS Status;

    RtlZeroMemory(&CollectionInfo, sizeof(CollectionInfo));
    RtlZeroMemory(&Caps, sizeof(Caps));
    RtlZeroMemory(&ValueCaps, sizeof(ValueCaps));

    Status = MouHidCallHidClass(Extension,
                                IOCTL_HID_GET_COLLECTION_INFORMATION,
                                NULL,
                                0,
                                &CollectionInfo,
                                sizeof(CollectionInfo));
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("MouHid: no collection information 0x%08lx\n", Status);
        return Status;
    }

    PreparsedData = ExAllocatePool2(POOL_FLAG_NON_PAGED, CollectionInfo.DescriptorSize, MOUHID_TAG);
    if (PreparsedData == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = MouHidCallHidClass(Extension,
                                IOCTL_HID_GET_COLLECTION_DESCRIPTOR,
                                NULL,
                                0,
                                PreparsedData,
                                CollectionInfo.DescriptorSize);
    if (!NT_SUCCESS(Status))
        goto Fail;

    Status = HidP_GetCaps(PreparsedData, &Caps);
    if (!NT_SUCCESS(Status))
        goto Fail;

    /* Mice always, pointers unless UseOnlyMice is set */
    if ((Caps.UsagePage != HID_USAGE_PAGE_GENERIC) ||
        ((Caps.Usage != HID_USAGE_GENERIC_MOUSE) &&
         ((Caps.Usage != HID_USAGE_GENERIC_POINTER) || MouHidGlobals.UseOnlyMice)))
    {
        DPRINT1("MouHid: collection %x:%x is not a mouse\n", Caps.UsagePage, Caps.Usage);
        Status = STATUS_UNSUCCESSFUL;
        goto Fail;
    }

    MaxUsages = (USHORT)HidP_MaxUsageListLength(HidP_Input, HID_USAGE_PAGE_BUTTON, PreparsedData);
    ReportSize = ALIGN_UP_BY(Caps.InputReportByteLength, 8);
    ListSize = ALIGN_UP_BY(MaxUsages * sizeof(USAGE), 8);

    /* The report buffer and the four usage lists follow the layout */
    Collection = ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                 sizeof(*Collection) + ReportSize + 4 * ListSize,
                                 MOUHID_TAG);
    Extension->Collection = Collection;
    if (Collection == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Fail;
    }

    Collection->PreparsedData = PreparsedData;
    Collection->Caps = Caps;
    Collection->MaxUsageListLength = MaxUsages;
    Extension->Attributes.NumberOfButtons = MaxUsages;

    Buffer = (PUCHAR)(Collection + 1);
    Collection->Report = (PCHAR)Buffer;
    Collection->PreviousUsages = (PUSAGE)(Buffer + ReportSize);
    Collection->CurrentUsages = (PUSAGE)(Buffer + ReportSize + ListSize);
    Collection->BreakUsages = (PUSAGE)(Buffer + ReportSize + 2 * ListSize);
    Collection->MakeUsages = (PUSAGE)(Buffer + ReportSize + 3 * ListSize);

    Collection->ReportMdl = IoAllocateMdl(Collection->Report, Caps.InputReportByteLength, FALSE, FALSE, NULL);
    if (Collection->ReportMdl == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Fail;
    }
    MmBuildMdlForNonPagedPool(Collection->ReportMdl);

    CapsLength = 1;
    Status = HidP_GetSpecificValueCaps(HidP_Input,
                                       HID_USAGE_PAGE_GENERIC,
                                       0,
                                       HID_USAGE_GENERIC_X,
                                       &ValueCaps,
                                       &CapsLength,
                                       PreparsedData);
    if (!NT_SUCCESS(Status))
        goto Fail;

    Collection->IsAbsolute = FALSE;
    Extension->InputData.Flags = MOUSE_MOVE_RELATIVE;
    if (ValueCaps.IsAbsolute)
    {
        if (((Caps.Usage == HID_USAGE_GENERIC_POINTER) && MouHidGlobals.TreatAbsolutePointerAsAbsolute) ||
            !MouHidGlobals.TreatAbsoluteAsRelative)
        {
            Collection->IsAbsolute = TRUE;
            Extension->InputData.Flags = MOUSE_MOVE_ABSOLUTE;
        }
        else
        {
            Extension->ProblemFlags |= MOUHID_PROBLEM_ABSOLUTE_IGNORED;
            MouHidLogError(Extension->Self->DriverObject,
                           MOUHID_LOG_ABSOLUTE_IGNORED,
                           MOUHID_LOG_ABSOLUTE_IGNORED);
        }
    }

    /* Without a physical range the largest positive logical value is used */
    Collection->BitSizeX = ValueCaps.BitSize;
    Collection->MaxX = ValueCaps.PhysicalMax;
    if (Collection->MaxX == 0)
        Collection->MaxX = (LONG)((1UL << (ValueCaps.BitSize - 1)) - 1);

    CapsLength = 1;
    Status = HidP_GetSpecificValueCaps(HidP_Input,
                                       HID_USAGE_PAGE_GENERIC,
                                       0,
                                       HID_USAGE_GENERIC_Y,
                                       &ValueCaps,
                                       &CapsLength,
                                       PreparsedData);
    if (!NT_SUCCESS(Status))
        goto Fail;

    Collection->BitSizeY = ValueCaps.BitSize;
    Collection->MaxY = ValueCaps.PhysicalMax;
    if (Collection->MaxY == 0)
        Collection->MaxY = (LONG)((1UL << (ValueCaps.BitSize - 1)) - 1);

    /* Vertical wheel: the registry override, else Wheel, else Z */
    if (Extension->VScrollUsageOverride != 0)
    {
        Found = MouHidFindInputValue(PreparsedData,
                                     Extension->VScrollPageOverride,
                                     Extension->VScrollUsageOverride,
                                     &BitSize);
        Collection->WheelUsagePage = Extension->VScrollPageOverride;
        Collection->WheelUsage = Extension->VScrollUsageOverride;
    }
    else
    {
        Collection->WheelUsagePage = HID_USAGE_PAGE_GENERIC;
        Collection->WheelUsage = HID_USAGE_GENERIC_WHEEL;
        Found = MouHidFindInputValue(PreparsedData, HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_WHEEL, &BitSize);
        if (!Found)
        {
            Collection->WheelUsage = HID_USAGE_GENERIC_Z;
            Found = MouHidFindInputValue(PreparsedData, HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_Z, &BitSize);
        }
    }

    if (Found)
    {
        Collection->BitSizeWheel = BitSize;
        Collection->WheelMissing = FALSE;
        Extension->Attributes.MouseIdentifier = WHEELMOUSE_HID_HARDWARE;
    }
    else
    {
        Collection->BitSizeWheel = 0;
        Collection->WheelMissing = TRUE;
        Collection->WheelUsagePage = 0;
        Collection->WheelUsage = 0;
    }

    /* Horizontal wheel: the registry override, else AC Pan */
    if (Extension->HScrollUsageOverride != 0)
    {
        Collection->HWheelUsagePage = Extension->HScrollPageOverride;
        Collection->HWheelUsage = Extension->HScrollUsageOverride;
    }
    else
    {
        Collection->HWheelUsagePage = HID_USAGE_PAGE_CONSUMER;
        Collection->HWheelUsage = MOUHID_USAGE_CONSUMER_AC_PAN;
    }

    Found = MouHidFindInputValue(PreparsedData,
                                 Collection->HWheelUsagePage,
                                 Collection->HWheelUsage,
                                 &BitSize);
    if (Found)
    {
        Collection->BitSizeHWheel = BitSize;
        Collection->HWheelMissing = FALSE;
        if (MouHidGlobals.DeviceFamily == MOUHID_FAMILY_XBOX)
            Extension->Attributes.MouseIdentifier |= HORIZONTAL_WHEEL_PRESENT;
    }
    else
    {
        Collection->BitSizeHWheel = 0;
        Collection->HWheelMissing = TRUE;
        Collection->HWheelUsagePage = 0;
        Collection->HWheelUsage = 0;
    }

    /* Windows hands back the hidparse status of the Y lookup */
    return Status;

Fail:
    ExFreePoolWithTag(PreparsedData, MOUHID_TAG);
    if (Collection != NULL)
    {
        if (Collection->ReportMdl != NULL)
            IoFreeMdl(Collection->ReportMdl);

        ExFreePoolWithTag(Collection, MOUHID_TAG);
        Extension->Collection = NULL;
    }

    return Status;
}

static
NTSTATUS
NTAPI
MouHidStartComplete(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_opt_ PVOID Context)
{
    PMOUHID_EXTENSION Extension = Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent(&Extension->StartEvent, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS
NTAPI
MouHidPnp(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    PMOUHID_EXTENSION Extension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PMOUHID_COLLECTION Collection = Extension->Collection;
    NTSTATUS Status;

    Status = IoAcquireRemoveLock(&Extension->RemoveLock, Irp);
    if (!NT_SUCCESS(Status))
    {
        Irp->IoStatus.Information = 0;
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    switch (Stack->MinorFunction)
    {
        case IRP_MN_START_DEVICE:
        {
            if (Extension->Started)
            {
                Status = STATUS_SUCCESS;
                Irp->IoStatus.Status = Status;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
                break;
            }

            IoCopyCurrentIrpStackLocationToNext(Irp);
            KeInitializeEvent(&Extension->StartEvent, NotificationEvent, FALSE);
            IoSetCompletionRoutine(Irp, MouHidStartComplete, Extension, TRUE, TRUE, TRUE);
            Irp->IoStatus.Status = STATUS_SUCCESS;

            Status = IoCallDriver(Extension->TopOfStack, Irp);
            if (Status == STATUS_PENDING)
            {
                KeWaitForSingleObject(&Extension->StartEvent, Executive, KernelMode, FALSE, NULL);
                Status = Irp->IoStatus.Status;
            }

            if (NT_SUCCESS(Status) && NT_SUCCESS(Irp->IoStatus.Status))
            {
                /* The collection layout is read once per device object */
                if (!Extension->Initialized)
                {
                    Status = MouHidStartDevice(Extension);
                    if (NT_SUCCESS(Status))
                    {
                        IoWMIRegistrationControl(DeviceObject, WMIREG_ACTION_REGISTER);
                        Extension->Initialized = TRUE;
                    }
                }

                if (NT_SUCCESS(Status))
                    Extension->Started = TRUE;
            }

            Irp->IoStatus.Status = Status;
            Irp->IoStatus.Information = 0;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            break;
        }

        case IRP_MN_REMOVE_DEVICE:
        {
            IoWMIRegistrationControl(Extension->Self, WMIREG_ACTION_DEREGISTER);
            Extension->Removed = TRUE;

            KeWaitForSingleObject(&Extension->ReadSentEvent, Executive, KernelMode, FALSE, NULL);
            IoCancelIrp(Extension->ReadIrp);

            IoSkipCurrentIrpStackLocation(Irp);
            Irp->IoStatus.Status = STATUS_SUCCESS;
            Status = IoCallDriver(Extension->TopOfStack, Irp);

            /* Also waits for the read loop to drop its reference */
            IoReleaseRemoveLockAndWait(&Extension->RemoveLock, Irp);

            IoFreeIrp(Extension->ReadIrp);
            if (Collection != NULL)
            {
                ExFreePoolWithTag(Collection->PreparsedData, MOUHID_TAG);
                IoFreeMdl(Collection->ReportMdl);
                ExFreePoolWithTag(Collection, MOUHID_TAG);
            }

            IoDetachDevice(Extension->TopOfStack);
            IoDeleteDevice(Extension->Self);
            return Status;
        }

        case IRP_MN_STOP_DEVICE:
            Extension->Started = FALSE;
            Irp->IoStatus.Status = STATUS_SUCCESS;
            IoSkipCurrentIrpStackLocation(Irp);
            Status = IoCallDriver(Extension->TopOfStack, Irp);
            break;

        case IRP_MN_QUERY_REMOVE_DEVICE:
        case IRP_MN_CANCEL_REMOVE_DEVICE:
        case IRP_MN_QUERY_STOP_DEVICE:
        case IRP_MN_CANCEL_STOP_DEVICE:
        case IRP_MN_SURPRISE_REMOVAL:
            Irp->IoStatus.Status = STATUS_SUCCESS;
            IoSkipCurrentIrpStackLocation(Irp);
            Status = IoCallDriver(Extension->TopOfStack, Irp);
            break;

        default:
            IoSkipCurrentIrpStackLocation(Irp);
            Status = IoCallDriver(Extension->TopOfStack, Irp);
            break;
    }

    IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
    return Status;
}

static
NTSTATUS
NTAPI
MouHidPowerComplete(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_opt_ PVOID Context)
{
    PMOUHID_EXTENSION Extension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);

    UNREFERENCED_PARAMETER(Context);

    /* Back in D0 with an open handle, restore the high resolution wheels */
    if ((Stack->MinorFunction == IRP_MN_SET_POWER) &&
        (Stack->Parameters.Power.Type == DevicePowerState) &&
        (Extension->DevicePowerState == PowerDeviceD0) &&
        NT_SUCCESS(Irp->IoStatus.Status) &&
        (Extension->ReadFile != NULL) &&
        (Extension->MultiplierState == MOUHID_MULTIPLIER_REAPPLY))
    {
        /* The feature requests are synchronous */
        if (KeGetCurrentIrql() == PASSIVE_LEVEL)
            MouHidApplyResolutionMultipliers(Extension);
        else
            DPRINT1("MouHid: D0 completed at raised IRQL, multipliers not restored\n");
    }

    IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
    return STATUS_CONTINUE_COMPLETION;
}

NTSTATUS
NTAPI
MouHidPower(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    PMOUHID_EXTENSION Extension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    DEVICE_POWER_STATE NewState;
    KIRQL OldIrql;
    NTSTATUS Status;

    Status = IoAcquireRemoveLock(&Extension->RemoveLock, Irp);
    if (!NT_SUCCESS(Status))
    {
        PoStartNextPowerIrp(Irp);
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    if ((Stack->MinorFunction == IRP_MN_SET_POWER) &&
        (Stack->Parameters.Power.Type == DevicePowerState))
    {
        NewState = Stack->Parameters.Power.State.DeviceState;
        Extension->PreviousDevicePowerState = Extension->DevicePowerState;
        Extension->DevicePowerState = NewState;

        if (NewState == PowerDeviceD0)
        {
            IoCopyCurrentIrpStackLocationToNext(Irp);
            IoSetCompletionRoutine(Irp, MouHidPowerComplete, NULL, TRUE, TRUE, TRUE);
            IoMarkIrpPending(Irp);
            PoCallDriver(Extension->TopOfStack, Irp);
            return STATUS_PENDING;
        }

        /* The device forgets its multiplier setting while powered down */
        InterlockedCompareExchange(&Extension->MultiplierState,
                                   MOUHID_MULTIPLIER_REAPPLY,
                                   MOUHID_MULTIPLIER_APPLIED);

        KeAcquireSpinLock(&Extension->WheelLock, &OldIrql);
        Extension->WheelScale = Extension->WheelScalingFactor;
        Extension->HScrollScale = Extension->HScrollScalingFactor;
        KeReleaseSpinLock(&Extension->WheelLock, OldIrql);
    }

    PoStartNextPowerIrp(Irp);
    IoSkipCurrentIrpStackLocation(Irp);
    Status = PoCallDriver(Extension->TopOfStack, Irp);
    IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
    return Status;
}
