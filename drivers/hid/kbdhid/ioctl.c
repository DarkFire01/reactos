/*
 * PROJECT:     ReactOS HID Stack
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     HID keyboard mapper internal keyboard requests
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "kbdhid.h"

#define NDEBUG
#include <debug.h>

/* Keys whose make code toggles an indicator */
static const INDICATOR_LIST KbdHidIndicatorList[] =
{
    { 0x3A, KEYBOARD_CAPS_LOCK_ON },
    { 0x45, KEYBOARD_NUM_LOCK_ON },
    { 0x46, KEYBOARD_SCROLL_LOCK_ON }
};

#define KBDHID_INDICATOR_KEY_COUNT  ARRAYSIZE(KbdHidIndicatorList)
#define KBDHID_INDICATOR_TRANSLATION_SIZE \
    ((ULONG)FIELD_OFFSET(KEYBOARD_INDICATOR_TRANSLATION, IndicatorList[KBDHID_INDICATOR_KEY_COUNT]))

/* Output report state carried across the write to the collection */
typedef struct _KBDHID_LED_CONTEXT
{
    PCHAR Report;
    PMDL SavedMdl;
} KBDHID_LED_CONTEXT, *PKBDHID_LED_CONTEXT;

static
NTSTATUS
KbdHidWaitForExtendedAttributes(
    _In_ PKBDHID_DEVICE_EXTENSION DeviceExtension)
{
    switch (InterlockedCompareExchange(&DeviceExtension->ExtendedAttributesState, 0, 0))
    {
        case KbdHidQueryDone:
            return STATUS_SUCCESS;

        case KbdHidQueryInProgress:
            if (KeGetCurrentIrql() != PASSIVE_LEVEL)
                return STATUS_INVALID_DEVICE_REQUEST;

            KeWaitForSingleObject(&DeviceExtension->ExtendedAttributesEvent,
                                  Executive,
                                  KernelMode,
                                  FALSE,
                                  NULL);
            return STATUS_SUCCESS;

        default:
            /* Not known until the first open with read access */
            return STATUS_INVALID_DEVICE_REQUEST;
    }
}

static
NTSTATUS
NTAPI
KbdHidSetIndicatorsComplete(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PVOID Context)
{
    PKBDHID_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
    PKBDHID_LED_CONTEXT LedContext = Context;

    if (Irp->PendingReturned)
        IoMarkIrpPending(Irp);

    IoFreeMdl(Irp->MdlAddress);
    Irp->MdlAddress = LedContext->SavedMdl;
    ExFreePoolWithTag(LedContext->Report, KBDHID_TAG);
    ExFreePoolWithTag(LedContext, KBDHID_TAG);

    IoReleaseRemoveLock(&DeviceExtension->RemoveLock, Irp);
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Sends the indicator state to the device as an LED output report.
 */
static
NTSTATUS
KbdHidSetIndicators(
    _In_ PKBDHID_DEVICE_EXTENSION DeviceExtension,
    _In_ PKEYBOARD_INDICATOR_PARAMETERS Parameters,
    _In_ PIRP Irp)
{
    PKBDHID_HID_DATA HidData = DeviceExtension->HidData;
    PKBDHID_LED_CONTEXT LedContext = NULL;
    PIO_STACK_LOCATION NextStack;
    USAGE Usages[4];
    ULONG UsageCount = 0;
    ULONG ReportLength;
    PMDL Mdl = NULL;
    NTSTATUS Status;

    Status = IoAcquireRemoveLock(&DeviceExtension->RemoveLock, Irp);
    if (!NT_SUCCESS(Status))
    {
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    if (!DeviceExtension->Started)
    {
        Status = STATUS_INVALID_DEVICE_STATE;
        goto Done;
    }

    ReportLength = HidData->Caps.OutputReportByteLength;
    if (ReportLength == 0)
    {
        /* Nothing to light up */
        Status = STATUS_SUCCESS;
        goto Done;
    }

    LedContext = ExAllocatePoolZero(NonPagedPool, sizeof(*LedContext), KBDHID_TAG);
    if (LedContext != NULL)
        LedContext->Report = ExAllocatePoolZero(NonPagedPool, ReportLength, KBDHID_TAG);
    if ((LedContext == NULL) || (LedContext->Report == NULL))
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Done;
    }

    Mdl = IoAllocateMdl(LedContext->Report, ReportLength, FALSE, FALSE, NULL);
    if (Mdl == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Done;
    }
    MmBuildMdlForNonPagedPool(Mdl);

    if (Parameters->LedFlags & KEYBOARD_CAPS_LOCK_ON)
        Usages[UsageCount++] = HID_USAGE_LED_CAPS_LOCK;
    if (Parameters->LedFlags & KEYBOARD_NUM_LOCK_ON)
        Usages[UsageCount++] = HID_USAGE_LED_NUM_LOCK;
    if (Parameters->LedFlags & KEYBOARD_SCROLL_LOCK_ON)
        Usages[UsageCount++] = HID_USAGE_LED_SCROLL_LOCK;
    if (Parameters->LedFlags & KEYBOARD_KANA_LOCK_ON)
        Usages[UsageCount++] = HID_USAGE_LED_KANA;

    if (UsageCount != 0)
    {
        Status = HidP_SetUsages(HidP_Output,
                                HID_USAGE_PAGE_LED,
                                0,
                                Usages,
                                &UsageCount,
                                HidData->PreparsedData,
                                LedContext->Report,
                                ReportLength);
    }
    else
    {
        /* Clearing one LED still stamps the report id into an all off report */
        Usages[0] = HID_USAGE_LED_SCROLL_LOCK;
        UsageCount = 1;
        Status = HidP_UnsetUsages(HidP_Output,
                                  HID_USAGE_PAGE_LED,
                                  0,
                                  Usages,
                                  &UsageCount,
                                  HidData->PreparsedData,
                                  LedContext->Report,
                                  ReportLength);
    }

    if (!NT_SUCCESS(Status) && (Status != HIDP_STATUS_BUTTON_NOT_PRESSED))
        goto Done;

    LedContext->SavedMdl = Irp->MdlAddress;
    Irp->MdlAddress = Mdl;

    IoCopyCurrentIrpStackLocationToNext(Irp);
    NextStack = IoGetNextIrpStackLocation(Irp);
    NextStack->MajorFunction = IRP_MJ_WRITE;
    NextStack->MinorFunction = 0;
    NextStack->Parameters.Write.Length = ReportLength;
    NextStack->Parameters.Write.Key = 0;
    NextStack->Parameters.Write.ByteOffset.QuadPart = 0;

    IoSetCompletionRoutine(Irp, KbdHidSetIndicatorsComplete, LedContext, TRUE, TRUE, TRUE);
    return IoCallDriver(DeviceExtension->NextDeviceObject, Irp);

Done:
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    if (LedContext != NULL)
    {
        if (LedContext->Report != NULL)
            ExFreePoolWithTag(LedContext->Report, KBDHID_TAG);
        ExFreePoolWithTag(LedContext, KBDHID_TAG);
    }

    if (Mdl != NULL)
        IoFreeMdl(Mdl);

    IoReleaseRemoveLock(&DeviceExtension->RemoveLock, Irp);
    return Status;
}

static
NTSTATUS
KbdHidSetTypematic(
    _In_ PKBDHID_DEVICE_EXTENSION DeviceExtension,
    _In_ PKEYBOARD_TYPEMATIC_PARAMETERS Parameters)
{
    PKEYBOARD_ATTRIBUTES Attributes = &DeviceExtension->Attributes;

    /* Zero rate and delay leaves the current settings alone */
    if ((Parameters->Rate == 0) && (Parameters->Delay == 0))
        return STATUS_SUCCESS;

    if ((Parameters->Rate < Attributes->KeyRepeatMinimum.Rate) ||
        (Parameters->Rate > Attributes->KeyRepeatMaximum.Rate) ||
        (Parameters->Delay < Attributes->KeyRepeatMinimum.Delay) ||
        (Parameters->Delay > Attributes->KeyRepeatMaximum.Delay))
    {
        return STATUS_INVALID_PARAMETER;
    }

    DPRINT("Typematic rate %u delay %u\n", Parameters->Rate, Parameters->Delay);

    DeviceExtension->Typematic = *Parameters;
    DeviceExtension->RepeatPeriod = 1000 / Parameters->Rate;
    DeviceExtension->RepeatDelay.QuadPart = -10000LL * Parameters->Delay;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Handles the keyboard port requests kbdclass sends down the stack.
 */
NTSTATUS
NTAPI
KbdHidInternalDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    PKBDHID_DEVICE_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    ULONG InputLength = IoStack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG OutputLength = IoStack->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID Buffer = Irp->AssociatedIrp.SystemBuffer;
    PKEYBOARD_INDICATOR_TRANSLATION Translation;
    PCONNECT_DATA ConnectData;
    NTSTATUS Status;

    Irp->IoStatus.Information = 0;

    Status = IoAcquireRemoveLock(&DeviceExtension->RemoveLock, Irp);
    if (!NT_SUCCESS(Status))
    {
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    switch (IoStack->Parameters.DeviceIoControl.IoControlCode)
    {
        case IOCTL_INTERNAL_KEYBOARD_CONNECT:
            if (DeviceExtension->ConnectData.ClassService != NULL)
            {
                Status = STATUS_SHARING_VIOLATION;
                break;
            }

            if (InputLength < sizeof(CONNECT_DATA))
            {
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            ConnectData = IoStack->Parameters.DeviceIoControl.Type3InputBuffer;
            DeviceExtension->ConnectData = *ConnectData;
            Status = STATUS_SUCCESS;
            break;

        case IOCTL_INTERNAL_KEYBOARD_DISCONNECT:
            Status = STATUS_NOT_IMPLEMENTED;
            break;

        case IOCTL_INTERNAL_KEYBOARD_ENABLE:
        case IOCTL_INTERNAL_KEYBOARD_DISABLE:
            Status = STATUS_NOT_SUPPORTED;
            break;

        case IOCTL_KEYBOARD_QUERY_ATTRIBUTES:
            if (OutputLength < sizeof(KEYBOARD_ATTRIBUTES))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            /* The attributes are valid without the extended query */
            KbdHidWaitForExtendedAttributes(DeviceExtension);
            Status = STATUS_SUCCESS;

            RtlCopyMemory(Buffer,
                          &DeviceExtension->Attributes,
                          sizeof(DeviceExtension->Attributes));
            Irp->IoStatus.Information = sizeof(DeviceExtension->Attributes);
            break;

        case IOCTL_KEYBOARD_QUERY_EXTENDED_ATTRIBUTES:
            if (OutputLength < sizeof(KEYBOARD_EXTENDED_ATTRIBUTES))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            Status = KbdHidWaitForExtendedAttributes(DeviceExtension);
            if (!NT_SUCCESS(Status))
                break;

            RtlCopyMemory(Buffer,
                          &DeviceExtension->ExtendedAttributes,
                          sizeof(DeviceExtension->ExtendedAttributes));
            Irp->IoStatus.Information = sizeof(DeviceExtension->ExtendedAttributes);
            break;

        case IOCTL_KEYBOARD_QUERY_INDICATOR_TRANSLATION:
            if (OutputLength < KBDHID_INDICATOR_TRANSLATION_SIZE)
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            Translation = Buffer;
            Translation->NumberOfIndicatorKeys = KBDHID_INDICATOR_KEY_COUNT;
            RtlCopyMemory(Translation->IndicatorList,
                          KbdHidIndicatorList,
                          sizeof(KbdHidIndicatorList));
            Irp->IoStatus.Information = KBDHID_INDICATOR_TRANSLATION_SIZE;
            Status = STATUS_SUCCESS;
            break;

        case IOCTL_KEYBOARD_QUERY_INDICATORS:
            if (OutputLength < sizeof(KEYBOARD_INDICATOR_PARAMETERS))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            RtlCopyMemory(Buffer,
                          &DeviceExtension->Indicators,
                          sizeof(DeviceExtension->Indicators));
            Irp->IoStatus.Information = sizeof(DeviceExtension->Indicators);
            Status = STATUS_SUCCESS;
            break;

        case IOCTL_KEYBOARD_SET_INDICATORS:
            if (InputLength < sizeof(KEYBOARD_INDICATOR_PARAMETERS))
            {
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            /* Completion of the request is up to the output report path */
            Status = KbdHidSetIndicators(DeviceExtension, Buffer, Irp);
            IoReleaseRemoveLock(&DeviceExtension->RemoveLock, Irp);
            return Status;

        case IOCTL_KEYBOARD_QUERY_TYPEMATIC:
            if (OutputLength < sizeof(KEYBOARD_TYPEMATIC_PARAMETERS))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            RtlCopyMemory(Buffer, &DeviceExtension->Typematic, sizeof(DeviceExtension->Typematic));
            Irp->IoStatus.Information = sizeof(DeviceExtension->Typematic);
            Status = STATUS_SUCCESS;
            break;

        case IOCTL_KEYBOARD_SET_TYPEMATIC:
            if (InputLength < sizeof(KEYBOARD_TYPEMATIC_PARAMETERS))
            {
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            Status = KbdHidSetTypematic(DeviceExtension, Buffer);
            break;

        default:
            DPRINT1("Unknown internal request 0x%lx\n",
                    IoStack->Parameters.DeviceIoControl.IoControlCode);
            Status = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }

    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    IoReleaseRemoveLock(&DeviceExtension->RemoveLock, Irp);
    return Status;
}
