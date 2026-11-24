/*
 * PROJECT:     ReactOS HID Stack
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     HID keyboard mapper plug and play and power handling
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "kbdhid.h"

#define NDEBUG
#include <debug.h>

/* Room for a value name and a REG_SZ of four hex digits */
#define KBDHID_MAPPING_TEXT_CHARS       4
#define KBDHID_MAPPING_DATA_LENGTH      ((KBDHID_MAPPING_TEXT_CHARS + 1) * sizeof(WCHAR))
#define KBDHID_MAPPING_BUFFER_LENGTH    (FIELD_OFFSET(KEY_VALUE_FULL_INFORMATION, Name) + 22)

#define KBDHID_ALIGN_UP(Value)          (((Value) + 7) & ~(SIZE_T)7)

NTSTATUS
NTAPI
KbdHidCallHidClass(
    _In_ PKBDHID_DEVICE_EXTENSION DeviceExtension,
    _In_ ULONG IoControlCode,
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ ULONG OutputBufferLength)
{
    IO_STATUS_BLOCK IoStatus;
    KEVENT Event;
    PIRP Irp;
    NTSTATUS Status;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    Irp = IoBuildDeviceIoControlRequest(IoControlCode,
                                        DeviceExtension->NextDeviceObject,
                                        NULL,
                                        0,
                                        OutputBuffer,
                                        OutputBufferLength,
                                        FALSE,
                                        &Event,
                                        &IoStatus);
    if (Irp == NULL)
        return STATUS_UNSUCCESSFUL;

    /* Requests that need an open collection ride on the read loop's file object */
    IoGetNextIrpStackLocation(Irp)->FileObject = DeviceExtension->ReadFileObject;

    Status = IoCallDriver(DeviceExtension->NextDeviceObject, Irp);
    if (Status == STATUS_PENDING)
        Status = KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);

    if (NT_SUCCESS(Status))
        Status = IoStatus.Status;

    return Status;
}

/**
 * @brief
 * Parses leading hex digits, stopping at the first other character.
 */
static
ULONG
KbdHidParseHex(
    _In_z_ PCWSTR Text)
{
    ULONG Value = 0;

    for (; *Text != UNICODE_NULL; Text++)
    {
        if ((*Text >= L'0') && (*Text <= L'9'))
            Value = (Value << 4) + (*Text - L'0');
        else if ((*Text >= L'a') && (*Text <= L'f'))
            Value = (Value << 4) + (*Text - L'a' + 10);
        else if ((*Text >= L'A') && (*Text <= L'F'))
            Value = (Value << 4) + (*Text - L'A' + 10);
        else
            break;
    }

    return Value;
}

/**
 * @brief
 * Loads the UsageMappings remap list from the driver key of the collection.
 * Each REG_SZ value maps the usage in its name to the usage in its data.
 */
static
VOID
KbdHidLoadUsageMappings(
    _In_ PKBDHID_DEVICE_EXTENSION DeviceExtension)
{
    UNICODE_STRING SubkeyName = RTL_CONSTANT_STRING(L"UsageMappings");
    OBJECT_ATTRIBUTES ObjectAttributes;
    ULONGLONG Buffer[KBDHID_ALIGN_UP(KBDHID_MAPPING_BUFFER_LENGTH) / sizeof(ULONGLONG)];
    PKEY_VALUE_FULL_INFORMATION Info = (PKEY_VALUE_FULL_INFORMATION)Buffer;
    PKBDHID_USAGE_MAPPING Head = NULL;
    PKBDHID_USAGE_MAPPING Tail = NULL;
    PKBDHID_USAGE_MAPPING Entry;
    WCHAR FromText[KBDHID_MAPPING_TEXT_CHARS + 1];
    WCHAR ToText[KBDHID_MAPPING_TEXT_CHARS + 1];
    HANDLE DriverKey;
    HANDLE MappingKey;
    ULONG ResultLength;
    ULONG Index;
    KIRQL OldIrql;

    if (NT_SUCCESS(IoOpenDeviceRegistryKey(DeviceExtension->PhysicalDeviceObject,
                                           PLUGPLAY_REGKEY_DRIVER,
                                           KEY_READ,
                                           &DriverKey)))
    {
        InitializeObjectAttributes(&ObjectAttributes,
                                   &SubkeyName,
                                   OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                                   DriverKey,
                                   NULL);

        if (NT_SUCCESS(ZwOpenKey(&MappingKey, KEY_READ, &ObjectAttributes)))
        {
            /* An entry too large for the buffer ends the list */
            for (Index = 0;
                 NT_SUCCESS(ZwEnumerateValueKey(MappingKey,
                                                Index,
                                                KeyValueFullInformation,
                                                Info,
                                                KBDHID_MAPPING_BUFFER_LENGTH,
                                                &ResultLength));
                 Index++)
            {
                if ((Info->Type != REG_SZ) ||
                    (Info->DataLength != KBDHID_MAPPING_DATA_LENGTH) ||
                    (Info->NameLength > KBDHID_MAPPING_DATA_LENGTH))
                {
                    continue;
                }

                RtlCopyMemory(FromText, Info->Name, KBDHID_MAPPING_TEXT_CHARS * sizeof(WCHAR));
                FromText[KBDHID_MAPPING_TEXT_CHARS] = UNICODE_NULL;
                RtlCopyMemory(ToText,
                              (PUCHAR)Info + Info->DataOffset,
                              KBDHID_MAPPING_TEXT_CHARS * sizeof(WCHAR));
                ToText[KBDHID_MAPPING_TEXT_CHARS] = UNICODE_NULL;

                Entry = ExAllocatePoolZero(NonPagedPool, sizeof(*Entry), KBDHID_TAG);
                if (Entry == NULL)
                    break;

                Entry->From = (USAGE)KbdHidParseHex(FromText);
                Entry->To = (USAGE)KbdHidParseHex(ToText);
                DPRINT("Usage mapping 0x%x to 0x%x\n", Entry->From, Entry->To);

                if (Tail != NULL)
                    Tail->Next = Entry;
                else
                    Head = Entry;
                Tail = Entry;
            }

            ZwClose(MappingKey);
        }

        ZwClose(DriverKey);
    }

    KeAcquireSpinLock(&DeviceExtension->MappingLock, &OldIrql);
    DeviceExtension->Mappings = Head;
    KeReleaseSpinLock(&DeviceExtension->MappingLock, OldIrql);
}

static
VOID
KbdHidFreeUsageMappings(
    _In_ PKBDHID_DEVICE_EXTENSION DeviceExtension)
{
    PKBDHID_USAGE_MAPPING Entry;
    PKBDHID_USAGE_MAPPING Next;
    KIRQL OldIrql;

    KeAcquireSpinLock(&DeviceExtension->MappingLock, &OldIrql);
    Entry = DeviceExtension->Mappings;
    DeviceExtension->Mappings = NULL;
    KeReleaseSpinLock(&DeviceExtension->MappingLock, OldIrql);

    for (; Entry != NULL; Entry = Next)
    {
        Next = Entry->Next;
        ExFreePoolWithTag(Entry, KBDHID_TAG);
    }
}

/**
 * @brief
 * Reads an override of at most four bytes from the device key.
 * Bytes past the stored data length keep whatever Value held.
 */
static
NTSTATUS
KbdHidQueryOverride(
    _In_ HANDLE KeyHandle,
    _In_z_ PCWSTR Name,
    _Inout_ PULONG Value)
{
    ULONG Buffer[(FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) / sizeof(ULONG)) + 1];
    PKEY_VALUE_PARTIAL_INFORMATION Info = (PKEY_VALUE_PARTIAL_INFORMATION)Buffer;
    UNICODE_STRING ValueName;
    ULONG ResultLength;
    NTSTATUS Status;

    RtlInitUnicodeString(&ValueName, Name);

    Status = ZwQueryValueKey(KeyHandle,
                             &ValueName,
                             KeyValuePartialInformation,
                             Info,
                             sizeof(Buffer),
                             &ResultLength);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Info->DataLength > sizeof(*Value))
        return STATUS_BUFFER_TOO_SMALL;

    RtlCopyMemory(Value, Info->Data, Info->DataLength);
    return STATUS_SUCCESS;
}

static
VOID
KbdHidReadOverrides(
    _In_ PKBDHID_DEVICE_EXTENSION DeviceExtension)
{
    PKEYBOARD_ATTRIBUTES Attributes = &DeviceExtension->Attributes;
    HANDLE DeviceKey;
    ULONG Value = 0;

    if (!NT_SUCCESS(IoOpenDeviceRegistryKey(DeviceExtension->PhysicalDeviceObject,
                                            PLUGPLAY_REGKEY_DEVICE,
                                            KEY_READ,
                                            &DeviceKey)))
    {
        return;
    }

    if (NT_SUCCESS(KbdHidQueryOverride(DeviceKey, L"KeyboardTypeOverride", &Value)))
    {
        Attributes->KeyboardIdentifier.Type = (UCHAR)Value;
        DeviceExtension->IdEx.Type = Value;
    }

    if (NT_SUCCESS(KbdHidQueryOverride(DeviceKey, L"KeyboardSubtypeOverride", &Value)))
    {
        Attributes->KeyboardIdentifier.Subtype = (UCHAR)Value;
        DeviceExtension->IdEx.Subtype = Value;
    }

    if (NT_SUCCESS(KbdHidQueryOverride(DeviceKey, L"KeyboardNumberTotalKeysOverride", &Value)))
        Attributes->NumberOfKeysTotal = (USHORT)Value;

    if (NT_SUCCESS(KbdHidQueryOverride(DeviceKey, L"KeyboardNumberFunctionKeysOverride", &Value)))
        Attributes->NumberOfFunctionKeys = (USHORT)Value;

    if (NT_SUCCESS(KbdHidQueryOverride(DeviceKey, L"KeyboardNumberIndicatorsOverride", &Value)))
        Attributes->NumberOfIndicators = (USHORT)Value;

    ZwClose(DeviceKey);
}

/**
 * @brief
 * Reads the extended keyboard attributes feature report, if the collection
 * has one, and folds the physical layout into the keyboard type.
 */
NTSTATUS
NTAPI
KbdHidQueryExtendedAttributes(
    _In_ PKBDHID_DEVICE_EXTENSION DeviceExtension)
{
    PKBDHID_HID_DATA HidData = DeviceExtension->HidData;
    PKEYBOARD_ID Identifier = &DeviceExtension->Attributes.KeyboardIdentifier;
    PKEYBOARD_EXTENDED_ATTRIBUTES Extended = &DeviceExtension->ExtendedAttributes;
    PHIDP_VALUE_CAPS ValueCaps = NULL;
    PHIDP_VALUE_CAPS Found = NULL;
    PCHAR Report = NULL;
    ULONG ReportLength = HidData->Caps.FeatureReportByteLength;
    USHORT CapsCount = HidData->Caps.NumberFeatureValueCaps;
    USHORT Index;
    ULONG FormFactor = 0;
    ULONG KeyType = 0;
    ULONG Layout = 0;
    ULONG VendorLayout = 0;
    ULONG LanguageTag = 0;
    ULONG InputAssist = 0;
    NTSTATUS Status;

    if ((ReportLength == 0) || (CapsCount == 0))
    {
        Status = STATUS_NOT_SUPPORTED;
        goto Store;
    }

    ValueCaps = ExAllocatePoolZero(NonPagedPool, CapsCount * sizeof(*ValueCaps), KBDHID_TAG);
    if (ValueCaps == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Store;
    }

    Status = HidP_GetValueCaps(HidP_Feature, ValueCaps, &CapsCount, HidData->PreparsedData);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    for (Index = 0; Index < CapsCount; Index++)
    {
        if ((ValueCaps[Index].LinkUsagePage == HID_USAGE_PAGE_CONSUMER) &&
            (ValueCaps[Index].LinkUsage == KBDHID_USAGE_EXTENDED_ATTRIBUTES) &&
            (ValueCaps[Index].UsagePage == HID_USAGE_PAGE_CONSUMER) &&
            ValueCaps[Index].IsRange &&
            (ValueCaps[Index].Range.UsageMin <= KBDHID_USAGE_INPUT_ASSIST) &&
            (ValueCaps[Index].Range.UsageMax >= KBDHID_USAGE_FORM_FACTOR))
        {
            Found = &ValueCaps[Index];
            break;
        }
    }

    if (Found == NULL)
    {
        Status = STATUS_NOT_FOUND;
        goto Cleanup;
    }

    Report = ExAllocatePoolZero(NonPagedPool, ReportLength, KBDHID_TAG);
    if (Report == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }

    Report[0] = (CHAR)Found->ReportID;
    Status = KbdHidCallHidClass(DeviceExtension, IOCTL_HID_GET_FEATURE, Report, ReportLength);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Extended attributes feature report failed 0x%lx\n", Status);
        goto Cleanup;
    }

    if (NT_SUCCESS(HidP_GetUsageValue(HidP_Feature,
                                      HID_USAGE_PAGE_CONSUMER,
                                      Found->LinkCollection,
                                      KBDHID_USAGE_PHYSICAL_LAYOUT,
                                      &Layout,
                                      HidData->PreparsedData,
                                      Report,
                                      ReportLength)))
    {
        if (Layout > 6)
        {
            Layout = 0;
        }
        else if (Layout == 6)
        {
            /* A vendor specific layout needs the vendor value to mean anything */
            if (NT_SUCCESS(HidP_GetUsageValue(HidP_Feature,
                                              HID_USAGE_PAGE_CONSUMER,
                                              Found->LinkCollection,
                                              KBDHID_USAGE_VENDOR_LAYOUT,
                                              &VendorLayout,
                                              HidData->PreparsedData,
                                              Report,
                                              ReportLength)) &&
                (VendorLayout == 0))
            {
                Layout = 0;
            }
        }
    }

    if (NT_SUCCESS(HidP_GetUsageValue(HidP_Feature,
                                      HID_USAGE_PAGE_CONSUMER,
                                      Found->LinkCollection,
                                      KBDHID_USAGE_FORM_FACTOR,
                                      &FormFactor,
                                      HidData->PreparsedData,
                                      Report,
                                      ReportLength)) &&
        (FormFactor > 2))
    {
        FormFactor = 0;
    }

    if (NT_SUCCESS(HidP_GetUsageValue(HidP_Feature,
                                      HID_USAGE_PAGE_CONSUMER,
                                      Found->LinkCollection,
                                      KBDHID_USAGE_KEY_TYPE,
                                      &KeyType,
                                      HidData->PreparsedData,
                                      Report,
                                      ReportLength)) &&
        (KeyType > 3))
    {
        KeyType = 0;
    }

    HidP_GetUsageValue(HidP_Feature,
                       HID_USAGE_PAGE_CONSUMER,
                       Found->LinkCollection,
                       KBDHID_USAGE_LANGUAGE_TAG,
                       &LanguageTag,
                       HidData->PreparsedData,
                       Report,
                       ReportLength);

    HidP_GetUsageValue(HidP_Feature,
                       HID_USAGE_PAGE_CONSUMER,
                       Found->LinkCollection,
                       KBDHID_USAGE_INPUT_ASSIST,
                       &InputAssist,
                       HidData->PreparsedData,
                       Report,
                       ReportLength);

    /* A keyboard type set by override wins over the reported layout */
    switch (Identifier->Type)
    {
        case KEYBOARD_TYPE_GENERIC_101:
            Layout = 1;
            break;

        case KEYBOARD_TYPE_JAPAN:
            Layout = 5;
            break;

        case KEYBOARD_TYPE_KOREA:
            Layout = 2;
            break;

        default:
            if (Layout == 1)
            {
                Identifier->Type = KEYBOARD_TYPE_GENERIC_101;
                Identifier->Subtype = 0;
            }
            else if (Layout == 2)
            {
                Identifier->Type = KEYBOARD_TYPE_KOREA;
                Identifier->Subtype = 6;
            }
            else if (Layout == 5)
            {
                Identifier->Type = KEYBOARD_TYPE_JAPAN;
                Identifier->Subtype = 2;
            }
            else
            {
                break;
            }

            DeviceExtension->IdEx.Type = Identifier->Type;
            DeviceExtension->IdEx.Subtype = Identifier->Subtype;
            break;
    }

    Status = STATUS_SUCCESS;

Cleanup:
    if (Report != NULL)
        ExFreePoolWithTag(Report, KBDHID_TAG);
    ExFreePoolWithTag(ValueCaps, KBDHID_TAG);

Store:
    Extended->Version = KEYBOARD_EXTENDED_ATTRIBUTES_STRUCT_VERSION_1;
    Extended->FormFactor = (UCHAR)FormFactor;
    Extended->KeyType = (UCHAR)KeyType;
    Extended->PhysicalLayout = (UCHAR)Layout;
    Extended->VendorSpecificPhysicalLayout = (UCHAR)VendorLayout;
    Extended->IETFLanguageTagIndex = (UCHAR)LanguageTag;
    Extended->ImplementedInputAssistControls = (UCHAR)InputAssist;

    DPRINT("Extended attributes 0x%lx: form %u key %u layout %u vendor %u language %u assist %u\n",
           Status, FormFactor, KeyType, Layout, VendorLayout, LanguageTag, InputAssist);
    return Status;
}

/**
 * @brief
 * Reads the collection description and sets up the report buffer and the
 * usage lists the read loop works with.
 */
static
NTSTATUS
KbdHidStartDevice(
    _In_ PKBDHID_DEVICE_EXTENSION DeviceExtension)
{
    HID_COLLECTION_INFORMATION Information;
    PHIDP_PREPARSED_DATA PreparsedData;
    PKBDHID_HID_DATA HidData;
    HIDP_CAPS Caps;
    ULONG MaxUsages;
    SIZE_T ReportSize;
    SIZE_T ListSize;
    PUCHAR Cursor;
    NTSTATUS Status;

    KbdHidLoadUsageMappings(DeviceExtension);

    RtlZeroMemory(&Information, sizeof(Information));
    Status = KbdHidCallHidClass(DeviceExtension,
                                IOCTL_HID_GET_COLLECTION_INFORMATION,
                                &Information,
                                sizeof(Information));
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Collection information failed 0x%lx\n", Status);
        return Status;
    }

    PreparsedData = ExAllocatePoolZero(NonPagedPool, Information.DescriptorSize, KBDHID_TAG);
    if (PreparsedData == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = KbdHidCallHidClass(DeviceExtension,
                                IOCTL_HID_GET_COLLECTION_DESCRIPTOR,
                                PreparsedData,
                                Information.DescriptorSize);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Collection descriptor failed 0x%lx\n", Status);
        goto Failure;
    }

    RtlZeroMemory(&Caps, sizeof(Caps));
    Status = HidP_GetCaps(PreparsedData, &Caps);
    if (!NT_SUCCESS(Status))
        goto Failure;

    DeviceExtension->Attributes.NumberOfKeysTotal = Caps.NumberInputDataIndices;
    KbdHidReadOverrides(DeviceExtension);

    MaxUsages = HidP_MaxUsageListLength(HidP_Input, HID_USAGE_PAGE_KEYBOARD, PreparsedData);
    DPRINT("Input report %u bytes, %lu usages\n", Caps.InputReportByteLength, MaxUsages);

    ReportSize = KBDHID_ALIGN_UP(Caps.InputReportByteLength);
    ListSize = KBDHID_ALIGN_UP((SIZE_T)MaxUsages * sizeof(*HidData->CurrentUsages));
    if ((MaxUsages > (MAXULONG / sizeof(USAGE_AND_PAGE))) ||
        (ListSize > (MAXULONG - sizeof(*HidData) - ReportSize) / 6))
    {
        Status = STATUS_UNSUCCESSFUL;
        goto Failure;
    }

    HidData = ExAllocatePoolZero(NonPagedPool,
                                 sizeof(*HidData) + ReportSize + 6 * ListSize,
                                 KBDHID_TAG);
    if (HidData == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Failure;
    }

    HidData->PreparsedData = PreparsedData;
    HidData->Caps = Caps;
    HidData->MaxUsages = MaxUsages;
    KeInitializeSpinLock(&HidData->Lock);

    Cursor = (PUCHAR)(HidData + 1);
    HidData->InputReport = (PCHAR)Cursor;
    Cursor += ReportSize;
    HidData->PreviousUsages = (PUSAGE_AND_PAGE)Cursor;
    Cursor += ListSize;
    HidData->CurrentUsages = (PUSAGE_AND_PAGE)Cursor;
    Cursor += ListSize;
    HidData->BreakUsages = (PUSAGE_AND_PAGE)Cursor;
    Cursor += ListSize;
    HidData->RepeatUsages = (PUSAGE_AND_PAGE)Cursor;
    Cursor += ListSize;
    HidData->ScratchMake = (PUSAGE_AND_PAGE)Cursor;
    Cursor += ListSize;
    HidData->ScratchBreak = (PUSAGE_AND_PAGE)Cursor;

    HidData->InputMdl = IoAllocateMdl(HidData->InputReport,
                                      Caps.InputReportByteLength,
                                      FALSE,
                                      FALSE,
                                      NULL);
    if (HidData->InputMdl == NULL)
    {
        ExFreePoolWithTag(HidData, KBDHID_TAG);
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Failure;
    }
    MmBuildMdlForNonPagedPool(HidData->InputMdl);

    DeviceExtension->HidData = HidData;
    return STATUS_SUCCESS;

Failure:
    ExFreePoolWithTag(PreparsedData, KBDHID_TAG);
    return Status;
}

static
NTSTATUS
NTAPI
KbdHidStartComplete(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PVOID Context)
{
    PKBDHID_DEVICE_EXTENSION DeviceExtension = Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent(&DeviceExtension->StartEvent, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static
NTSTATUS
KbdHidRemoveDevice(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    PKBDHID_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
    PKBDHID_HID_DATA HidData = DeviceExtension->HidData;
    LARGE_INTEGER Interval;
    NTSTATUS Status;

    IoWMIRegistrationControl(DeviceObject, WMIREG_ACTION_DEREGISTER);

    KbdHidCancelAutoRepeat(DeviceExtension);
    KeCancelTimer(&DeviceExtension->RetryTimer);

    /* Give a timer DPC that already fired the longer of the two periods to finish */
    Interval = DeviceExtension->RepeatDelay;
    if (DeviceExtension->RetryDelay.QuadPart < Interval.QuadPart)
        Interval = DeviceExtension->RetryDelay;
    KeDelayExecutionThread(KernelMode, FALSE, &Interval);

    DeviceExtension->Removed = TRUE;
    KeWaitForSingleObject(&DeviceExtension->ReadIssueDoneEvent, Executive, KernelMode, FALSE, NULL);
    IoCancelIrp(DeviceExtension->ReadIrp);

    Irp->IoStatus.Status = STATUS_SUCCESS;
    IoSkipCurrentIrpStackLocation(Irp);
    Status = IoCallDriver(DeviceExtension->NextDeviceObject, Irp);

    /* Waits out the read loop too, it holds the lock while it runs */
    IoReleaseRemoveLockAndWait(&DeviceExtension->RemoveLock, Irp);

    IoFreeIrp(DeviceExtension->ReadIrp);
    if (HidData != NULL)
    {
        ExFreePoolWithTag(HidData->PreparsedData, KBDHID_TAG);
        IoFreeMdl(HidData->InputMdl);
        ExFreePoolWithTag(HidData, KBDHID_TAG);
        DeviceExtension->HidData = NULL;
    }
    KbdHidFreeUsageMappings(DeviceExtension);

    IoDetachDevice(DeviceExtension->NextDeviceObject);
    IoDeleteDevice(DeviceObject);
    return Status;
}

NTSTATUS
NTAPI
KbdHidPnp(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    PKBDHID_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS Status;

    Status = IoAcquireRemoveLock(&DeviceExtension->RemoveLock, Irp);
    if (!NT_SUCCESS(Status))
    {
        Irp->IoStatus.Information = 0;
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    DPRINT("PnP minor 0x%x\n", IoStack->MinorFunction);

    switch (IoStack->MinorFunction)
    {
        case IRP_MN_START_DEVICE:
            if (DeviceExtension->Started)
            {
                Status = STATUS_SUCCESS;
                Irp->IoStatus.Status = Status;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
                break;
            }

            KeInitializeEvent(&DeviceExtension->StartEvent, NotificationEvent, FALSE);
            IoCopyCurrentIrpStackLocationToNext(Irp);
            IoSetCompletionRoutine(Irp, KbdHidStartComplete, DeviceExtension, TRUE, TRUE, TRUE);
            Irp->IoStatus.Status = STATUS_SUCCESS;

            Status = IoCallDriver(DeviceExtension->NextDeviceObject, Irp);
            if (Status == STATUS_PENDING)
            {
                KeWaitForSingleObject(&DeviceExtension->StartEvent,
                                      Executive,
                                      KernelMode,
                                      FALSE,
                                      NULL);
                Status = Irp->IoStatus.Status;
            }

            if (NT_SUCCESS(Status) && NT_SUCCESS(Irp->IoStatus.Status))
            {
                /* A restart after a stop keeps what the first start set up */
                if (!DeviceExtension->Initialized)
                {
                    Status = KbdHidStartDevice(DeviceExtension);
                    if (NT_SUCCESS(Status))
                    {
                        IoWMIRegistrationControl(DeviceObject, WMIREG_ACTION_REGISTER);
                        DeviceExtension->Initialized = TRUE;
                    }
                }

                if (NT_SUCCESS(Status))
                    DeviceExtension->Started = TRUE;
            }

            Irp->IoStatus.Status = Status;
            Irp->IoStatus.Information = 0;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            break;

        case IRP_MN_REMOVE_DEVICE:
            /* Releases the remove lock itself */
            return KbdHidRemoveDevice(DeviceObject, Irp);

        case IRP_MN_STOP_DEVICE:
            DeviceExtension->Started = FALSE;
            Irp->IoStatus.Status = STATUS_SUCCESS;
            IoSkipCurrentIrpStackLocation(Irp);
            Status = IoCallDriver(DeviceExtension->NextDeviceObject, Irp);
            break;

        case IRP_MN_QUERY_REMOVE_DEVICE:
        case IRP_MN_CANCEL_REMOVE_DEVICE:
        case IRP_MN_QUERY_STOP_DEVICE:
        case IRP_MN_CANCEL_STOP_DEVICE:
        case IRP_MN_SURPRISE_REMOVAL:
            Irp->IoStatus.Status = STATUS_SUCCESS;
            IoSkipCurrentIrpStackLocation(Irp);
            Status = IoCallDriver(DeviceExtension->NextDeviceObject, Irp);
            break;

        default:
            IoSkipCurrentIrpStackLocation(Irp);
            Status = IoCallDriver(DeviceExtension->NextDeviceObject, Irp);
            break;
    }

    IoReleaseRemoveLock(&DeviceExtension->RemoveLock, Irp);
    return Status;
}

static
NTSTATUS
NTAPI
KbdHidPowerComplete(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PVOID Context)
{
    PKBDHID_DEVICE_EXTENSION DeviceExtension = Context;
    PKBDHID_HID_DATA HidData = DeviceExtension->HidData;
    SIZE_T ListLength;
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (Irp->PendingReturned)
        IoMarkIrpPending(Irp);

    if (NT_SUCCESS(Irp->IoStatus.Status))
    {
        /* Keys held across the power down come back as released without a break */
        KbdHidCancelAutoRepeat(DeviceExtension);

        KeAcquireSpinLock(&HidData->Lock, &OldIrql);
        ListLength = HidData->MaxUsages * sizeof(*HidData->CurrentUsages);
        RtlZeroMemory(HidData->PreviousUsages, ListLength);
        RtlZeroMemory(HidData->RepeatUsages, ListLength);
        KeReleaseSpinLock(&HidData->Lock, OldIrql);
    }

    IoReleaseRemoveLock(&DeviceExtension->RemoveLock, Irp);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
KbdHidPower(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    PKBDHID_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    DEVICE_POWER_STATE NewState;
    BOOLEAN IsPoweringDown = FALSE;
    NTSTATUS Status;

    Status = IoAcquireRemoveLock(&DeviceExtension->RemoveLock, Irp);
    if (!NT_SUCCESS(Status))
    {
        PoStartNextPowerIrp(Irp);
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    if ((IoStack->MinorFunction == IRP_MN_SET_POWER) &&
        (IoStack->Parameters.Power.Type == DevicePowerState))
    {
        NewState = IoStack->Parameters.Power.State.DeviceState;
        DPRINT("Set device power D%d\n", NewState - PowerDeviceD0);

        if (DeviceExtension->Initialized &&
            (DeviceExtension->DevicePowerState == PowerDeviceD0) &&
            (NewState > PowerDeviceD0))
        {
            IsPoweringDown = TRUE;
        }

        DeviceExtension->DevicePowerState = NewState;
    }

    PoStartNextPowerIrp(Irp);

    if (IsPoweringDown)
    {
        IoCopyCurrentIrpStackLocationToNext(Irp);
        IoSetCompletionRoutine(Irp, KbdHidPowerComplete, DeviceExtension, TRUE, TRUE, TRUE);
        return PoCallDriver(DeviceExtension->NextDeviceObject, Irp);
    }

    IoSkipCurrentIrpStackLocation(Irp);
    Status = PoCallDriver(DeviceExtension->NextDeviceObject, Irp);
    IoReleaseRemoveLock(&DeviceExtension->RemoveLock, Irp);
    return Status;
}

static
VOID
KbdHidLogAttachFailure(
    _In_ PDRIVER_OBJECT DriverObject)
{
    PIO_ERROR_LOG_PACKET Entry;

    Entry = IoAllocateErrorLogEntry(DriverObject, sizeof(*Entry));
    if (Entry == NULL)
        return;

    RtlZeroMemory(Entry, sizeof(*Entry));
    Entry->ErrorCode = KBDHID_LOG_ATTACH_FAILED;
    Entry->FinalStatus = STATUS_DEVICE_NOT_CONNECTED;
    IoWriteErrorLogEntry(Entry);
}

static
VOID
KbdHidInitializeKeyboardState(
    _In_ PKBDHID_DEVICE_EXTENSION DeviceExtension)
{
    PKEYBOARD_ATTRIBUTES Attributes = &DeviceExtension->Attributes;
    USHORT UnitId = DeviceExtension->UnitId;

    DeviceExtension->InputData.UnitId = UnitId;
    DeviceExtension->ScanPrefix = KbdHidPrefixNone;

    Attributes->KeyboardIdentifier.Type = KEYBOARD_TYPE_UNKNOWN;
    Attributes->KeyboardIdentifier.Subtype = 0;
    Attributes->KeyboardMode = 1;
    Attributes->NumberOfFunctionKeys = 12;
    Attributes->NumberOfIndicators = 3;
    Attributes->NumberOfKeysTotal = 101;
    Attributes->InputDataQueueLength = 1;
    Attributes->KeyRepeatMinimum.UnitId = UnitId;
    Attributes->KeyRepeatMinimum.Rate = 2;
    Attributes->KeyRepeatMinimum.Delay = 250;
    Attributes->KeyRepeatMaximum.UnitId = UnitId;
    Attributes->KeyRepeatMaximum.Rate = 30;
    Attributes->KeyRepeatMaximum.Delay = 1000;

    DeviceExtension->IdEx.Type = KEYBOARD_TYPE_UNKNOWN;
    DeviceExtension->IdEx.Subtype = 0;

    DeviceExtension->ExtendedAttributes.Version = KEYBOARD_EXTENDED_ATTRIBUTES_STRUCT_VERSION_1;
    DeviceExtension->ExtendedAttributesState = KbdHidQueryNotStarted;
    KeInitializeEvent(&DeviceExtension->ExtendedAttributesEvent, NotificationEvent, FALSE);

    DeviceExtension->Indicators.UnitId = UnitId;
    DeviceExtension->Indicators.LedFlags = 0;

    /* 30 characters per second after a quarter second */
    DeviceExtension->Typematic.UnitId = UnitId;
    DeviceExtension->Typematic.Rate = 30;
    DeviceExtension->Typematic.Delay = 250;
    DeviceExtension->RepeatPeriod = 1000 / 30;
    DeviceExtension->RepeatDelay.QuadPart = -250LL * 10000;
    KeInitializeDpc(&DeviceExtension->RepeatDpc, KbdHidAutoRepeatDpc, DeviceExtension);
    KeInitializeTimer(&DeviceExtension->RepeatTimer);

    /* Back off from a keyboard flooding identical reports */
    DeviceExtension->RetryDelay.QuadPart = -50LL * 10000;
    KeInitializeDpc(&DeviceExtension->RetryDpc, KbdHidRetryReadDpc, DeviceExtension);
    KeInitializeTimer(&DeviceExtension->RetryTimer);
}

NTSTATUS
NTAPI
KbdHidAddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject)
{
    PKBDHID_DEVICE_EXTENSION DeviceExtension;
    PDEVICE_OBJECT DeviceObject;
    POWER_STATE PowerState;
    NTSTATUS Status;

    Status = IoCreateDevice(DriverObject,
                            sizeof(KBDHID_DEVICE_EXTENSION),
                            NULL,
                            FILE_DEVICE_KEYBOARD,
                            0,
                            FALSE,
                            &DeviceObject);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("IoCreateDevice failed 0x%lx\n", Status);
        return Status;
    }

    DeviceExtension = DeviceObject->DeviceExtension;
    RtlZeroMemory(DeviceExtension, sizeof(*DeviceExtension));

    DeviceExtension->NextDeviceObject = IoAttachDeviceToDeviceStack(DeviceObject,
                                                                    PhysicalDeviceObject);
    if (DeviceExtension->NextDeviceObject == NULL)
    {
        KbdHidLogAttachFailure(DriverObject);
        IoDeleteDevice(DeviceObject);
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    DeviceExtension->Self = DeviceObject;
    DeviceExtension->PhysicalDeviceObject = PhysicalDeviceObject;
    DeviceExtension->UnitId = (USHORT)InterlockedIncrement(&KbdHidGlobals.DeviceCount);
    KeInitializeSpinLock(&DeviceExtension->MappingLock);

    DeviceExtension->ReadIrp = IoAllocateIrp(DeviceExtension->NextDeviceObject->StackSize, FALSE);
    if (DeviceExtension->ReadIrp == NULL)
    {
        IoDetachDevice(DeviceExtension->NextDeviceObject);
        IoDeleteDevice(DeviceObject);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    KeInitializeEvent(&DeviceExtension->ReadStoppedEvent, SynchronizationEvent, FALSE);
    KeInitializeEvent(&DeviceExtension->ReadIssueDoneEvent, NotificationEvent, TRUE);
    KeInitializeEvent(&DeviceExtension->StartEvent, NotificationEvent, FALSE);
    IoInitializeRemoveLock(&DeviceExtension->RemoveLock, KBDHID_TAG, 1, 0);
    ExInitializeFastMutex(&DeviceExtension->OpenMutex);

    KbdHidInitializeKeyboardState(DeviceExtension);

    DeviceExtension->DevicePowerState = PowerDeviceD0;
    PowerState.DeviceState = PowerDeviceD0;
    PoSetPowerState(DeviceObject, DevicePowerState, PowerState);

    DeviceExtension->WmiLibInfo.GuidCount = ARRAYSIZE(KbdHidWmiGuidList);
    DeviceExtension->WmiLibInfo.GuidList = KbdHidWmiGuidList;
    DeviceExtension->WmiLibInfo.QueryWmiRegInfo = KbdHidQueryWmiRegInfo;
    DeviceExtension->WmiLibInfo.QueryWmiDataBlock = KbdHidQueryWmiDataBlock;
    DeviceExtension->WmiLibInfo.SetWmiDataBlock = KbdHidSetWmiDataBlock;
    DeviceExtension->WmiLibInfo.SetWmiDataItem = KbdHidSetWmiDataItem;

    DeviceObject->Flags |= DO_POWER_PAGABLE;
    DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    return STATUS_SUCCESS;
}
