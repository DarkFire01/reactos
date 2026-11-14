/*
 * PROJECT:     ReactOS Qualcomm platform drivers
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     HID minidriver requests and touch reports
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "qcomtouch.h"

#include <debug.h>

/* Physical panel size in 0.1 mm, close enough for both OnePlus 6 and 6T */
#define QCTOUCH_PHYSICAL_X      683
#define QCTOUCH_PHYSICAL_Y      1441

#define QCTOUCH_HID_VENDOR      0x06CB
#define QCTOUCH_HID_PRODUCT     0x3706

#define QCTOUCH_FINGER_COLLECTION \
    0x05, 0x0D,                     /* Usage Page (Digitizer) */ \
    0x09, 0x22,                     /* Usage (Finger) */ \
    0xA1, 0x02,                     /* Collection (Logical) */ \
    0x09, 0x42,                     /*   Usage (Tip Switch) */ \
    0x09, 0x32,                     /*   Usage (In Range) */ \
    0x15, 0x00,                     /*   Logical Minimum (0) */ \
    0x25, 0x01,                     /*   Logical Maximum (1) */ \
    0x75, 0x01,                     /*   Report Size (1) */ \
    0x95, 0x02,                     /*   Report Count (2) */ \
    0x81, 0x02,                     /*   Input (Data, Variable, Absolute) */ \
    0x95, 0x06,                     /*   Report Count (6) */ \
    0x81, 0x03,                     /*   Input (Constant, Variable, Absolute) */ \
    0x09, 0x51,                     /*   Usage (Contact Identifier) */ \
    0x25, QCTOUCH_MAX_CONTACTS - 1, /*   Logical Maximum */ \
    0x75, 0x08,                     /*   Report Size (8) */ \
    0x95, 0x01,                     /*   Report Count (1) */ \
    0x81, 0x02,                     /*   Input (Data, Variable, Absolute) */ \
    0x05, 0x01,                     /*   Usage Page (Generic Desktop) */ \
    0x26, 0xFF, 0x7F,               /*   Logical Maximum (32767) */ \
    0x75, 0x10,                     /*   Report Size (16) */ \
    0x55, 0x0E,                     /*   Unit Exponent (-2) */ \
    0x65, 0x11,                     /*   Unit (Centimeter) */ \
    0x35, 0x00,                     /*   Physical Minimum (0) */ \
    0x46, QCTOUCH_PHYSICAL_X & 0xFF, QCTOUCH_PHYSICAL_X >> 8, \
    0x09, 0x30,                     /*   Usage (X) */ \
    0x81, 0x02,                     /*   Input (Data, Variable, Absolute) */ \
    0x46, QCTOUCH_PHYSICAL_Y & 0xFF, QCTOUCH_PHYSICAL_Y >> 8, \
    0x09, 0x31,                     /*   Usage (Y) */ \
    0x81, 0x02,                     /*   Input (Data, Variable, Absolute) */ \
    0x45, 0x00,                     /*   Physical Maximum (0) */ \
    0x55, 0x00,                     /*   Unit Exponent (0) */ \
    0x65, 0x00,                     /*   Unit (None) */ \
    0xC0                            /* End Collection */

static const UCHAR QcTouchReportDescriptor[] =
{
    /* Multi-touch screen, every contact in one report */
    0x05, 0x0D,                     /* Usage Page (Digitizer) */
    0x09, 0x04,                     /* Usage (Touch Screen) */
    0xA1, 0x01,                     /* Collection (Application) */
    0x85, QCTOUCH_REPORT_ID_TOUCH,  /*   Report ID */
    QCTOUCH_FINGER_COLLECTION,
    QCTOUCH_FINGER_COLLECTION,
    QCTOUCH_FINGER_COLLECTION,
    QCTOUCH_FINGER_COLLECTION,
    QCTOUCH_FINGER_COLLECTION,
    QCTOUCH_FINGER_COLLECTION,
    QCTOUCH_FINGER_COLLECTION,
    QCTOUCH_FINGER_COLLECTION,
    QCTOUCH_FINGER_COLLECTION,
    QCTOUCH_FINGER_COLLECTION,
    0x05, 0x0D,                     /*   Usage Page (Digitizer) */
    0x09, 0x54,                     /*   Usage (Contact Count) */
    0x25, 0x7F,                     /*   Logical Maximum (127) */
    0x75, 0x08,                     /*   Report Size (8) */
    0x95, 0x01,                     /*   Report Count (1) */
    0x81, 0x02,                     /*   Input (Data, Variable, Absolute) */
    0x85, QCTOUCH_REPORT_ID_MAXCOUNT, /* Report ID */
    0x09, 0x55,                     /*   Usage (Contact Count Maximum) */
    0x25, QCTOUCH_MAX_CONTACTS,     /*   Logical Maximum */
    0xB1, 0x02,                     /*   Feature (Data, Variable, Absolute) */
    0xC0,                           /* End Collection */

    /* Device configuration, the input mode the host selects */
    0x05, 0x0D,                     /* Usage Page (Digitizer) */
    0x09, 0x0E,                     /* Usage (Device Configuration) */
    0xA1, 0x01,                     /* Collection (Application) */
    0x85, QCTOUCH_REPORT_ID_CONFIG, /*   Report ID */
    0x09, 0x23,                     /*   Usage (Device Settings) */
    0xA1, 0x02,                     /*   Collection (Logical) */
    0x09, 0x52,                     /*     Usage (Device Mode) */
    0x09, 0x53,                     /*     Usage (Device Identifier) */
    0x15, 0x00,                     /*     Logical Minimum (0) */
    0x25, 0x0A,                     /*     Logical Maximum (10) */
    0x75, 0x08,                     /*     Report Size (8) */
    0x95, 0x02,                     /*     Report Count (2) */
    0xB1, 0x02,                     /*     Feature (Data, Variable, Absolute) */
    0xC0,                           /*   End Collection */
    0xC0,                           /* End Collection */

    /* Absolute pointer for consumers that do not understand touch */
    0x05, 0x01,                     /* Usage Page (Generic Desktop) */
    0x09, 0x02,                     /* Usage (Mouse) */
    0xA1, 0x01,                     /* Collection (Application) */
    0x85, QCTOUCH_REPORT_ID_MOUSE,  /*   Report ID */
    0x09, 0x01,                     /*   Usage (Pointer) */
    0xA1, 0x00,                     /*   Collection (Physical) */
    0x05, 0x09,                     /*     Usage Page (Button) */
    0x19, 0x01,                     /*     Usage Minimum (1) */
    0x29, 0x01,                     /*     Usage Maximum (1) */
    0x15, 0x00,                     /*     Logical Minimum (0) */
    0x25, 0x01,                     /*     Logical Maximum (1) */
    0x75, 0x01,                     /*     Report Size (1) */
    0x95, 0x01,                     /*     Report Count (1) */
    0x81, 0x02,                     /*     Input (Data, Variable, Absolute) */
    0x95, 0x07,                     /*     Report Count (7) */
    0x81, 0x03,                     /*     Input (Constant, Variable, Absolute) */
    0x05, 0x01,                     /*     Usage Page (Generic Desktop) */
    0x09, 0x30,                     /*     Usage (X) */
    0x09, 0x31,                     /*     Usage (Y) */
    0x26, 0xFF, 0x7F,               /*     Logical Maximum (32767) */
    0x75, 0x10,                     /*     Report Size (16) */
    0x95, 0x02,                     /*     Report Count (2) */
    0x81, 0x02,                     /*     Input (Data, Variable, Absolute) */
    0xC0,                           /*   End Collection */
    0xC0,                           /* End Collection */
};

static
NTSTATUS
QcTouchCopyOut(
    _In_ WDFREQUEST Request,
    _In_reads_bytes_(Length) const VOID *Data,
    _In_ ULONG Length)
{
    PVOID Buffer;
    NTSTATUS Status;

    Status = WdfRequestRetrieveOutputBuffer(Request, Length, &Buffer, NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlCopyMemory(Buffer, Data, Length);
    WdfRequestSetInformation(Request, Length);
    return STATUS_SUCCESS;
}

/**
 * @brief Hands queued reports to pending reads until one side runs dry.
 */
static
VOID
QcTouchDrainReports(
    _In_ PQCTOUCH_DEVICE Device)
{
    QCTOUCH_PENDING_REPORT Report;
    WDFREQUEST Request;
    NTSTATUS Status;

    for (;;)
    {
        WdfSpinLockAcquire(Device->ReportLock);
        if (Device->ReportCount == 0)
        {
            WdfSpinLockRelease(Device->ReportLock);
            return;
        }

        Status = WdfIoQueueRetrieveNextRequest(Device->ReadQueue, &Request);
        if (!NT_SUCCESS(Status))
        {
            WdfSpinLockRelease(Device->ReportLock);
            return;
        }

        Report = Device->Reports[Device->ReportHead];
        Device->ReportHead = (Device->ReportHead + 1) % QCTOUCH_REPORT_RING;
        Device->ReportCount--;
        WdfSpinLockRelease(Device->ReportLock);

        Status = QcTouchCopyOut(Request, Report.Data, Report.Length);
        WdfRequestComplete(Request, Status);
    }
}

static
VOID
QcTouchQueueReport(
    _In_ PQCTOUCH_DEVICE Device,
    _In_reads_bytes_(Length) const VOID *Data,
    _In_ ULONG Length)
{
    PQCTOUCH_PENDING_REPORT Slot;

    WdfSpinLockAcquire(Device->ReportLock);

    /* Nobody is reading, so the oldest report is the one to lose */
    if (Device->ReportCount == QCTOUCH_REPORT_RING)
    {
        Device->ReportHead = (Device->ReportHead + 1) % QCTOUCH_REPORT_RING;
        Device->ReportCount--;
    }

    Slot = &Device->Reports[(Device->ReportHead + Device->ReportCount) % QCTOUCH_REPORT_RING];
    RtlCopyMemory(Slot->Data, Data, Length);
    Slot->Length = Length;
    Device->ReportCount++;

    WdfSpinLockRelease(Device->ReportLock);

    QcTouchDrainReports(Device);
}

static
USHORT
QcTouchScale(
    _In_ ULONG Value,
    _In_ ULONG Maximum)
{
    if (Value >= Maximum)
        return QCTOUCH_LOGICAL_MAX;

    return (USHORT)((Value * QCTOUCH_LOGICAL_MAX) / Maximum);
}

static
VOID
QcTouchSendMouse(
    _In_ PQCTOUCH_DEVICE Device,
    _In_ BOOLEAN Down)
{
    QCTOUCH_MOUSE_REPORT Mouse;

    Mouse.ReportId = QCTOUCH_REPORT_ID_MOUSE;
    Mouse.Buttons = Down ? 1 : 0;
    Mouse.X = Device->PrimaryX;
    Mouse.Y = Device->PrimaryY;
    QcTouchQueueReport(Device, &Mouse, sizeof(Mouse));
}

/**
 * @brief Turns one F12 object list into a touch report and a pointer report.
 *
 * Contacts that lifted since the last list go out once more without the tip
 * bit, so the host sees every contact end.
 */
VOID
NTAPI
QcTouchReportObjects(
    _In_ PQCTOUCH_DEVICE Device,
    _In_reads_bytes_(RMI_READ_MAX) const UCHAR *Packet)
{
    QCTOUCH_TOUCH_REPORT Touch;
    PQCTOUCH_CONTACT_REPORT Contact;
    ULONG Mask = 0;
    ULONG Slot;
    ULONG Primary = MAXULONG;
    BOOLEAN WasDown;

    RtlZeroMemory(&Touch, sizeof(Touch));
    Touch.ReportId = QCTOUCH_REPORT_ID_TOUCH;

    for (Slot = 0; Slot < Device->FingerCount; Slot++)
    {
        const UCHAR *Object = &Packet[Device->Data1Offset + Slot * RMI_F12_FINGER_BYTES];
        ULONG X;
        ULONG Y;

        if (Object[0] != RMI_F12_OBJECT_FINGER && Object[0] != RMI_F12_OBJECT_GLOVED)
        {
            if (!(Device->ContactMask & (1 << Slot)))
                continue;

            /* Lifted: repeat the last spot without the tip */
            Contact = &Touch.Contacts[Touch.ContactCount++];
            Contact->ContactId = (UCHAR)Slot;
            Contact->X = Device->ContactX[Slot];
            Contact->Y = Device->ContactY[Slot];
            continue;
        }

        X = Object[1] | ((ULONG)Object[2] << 8);
        Y = Object[3] | ((ULONG)Object[4] << 8);

        Contact = &Touch.Contacts[Touch.ContactCount++];
        Contact->Flags = QCTOUCH_CONTACT_TIP | QCTOUCH_CONTACT_IN_RANGE;
        Contact->ContactId = (UCHAR)Slot;
        Contact->X = QcTouchScale(X, Device->MaxX);
        Contact->Y = QcTouchScale(Y, Device->MaxY);
        Device->ContactX[Slot] = Contact->X;
        Device->ContactY[Slot] = Contact->Y;

        Mask |= 1 << Slot;
        if (Primary == MAXULONG)
            Primary = Slot;
        if (Slot == Device->PrimaryContact && (Device->ContactMask & (1 << Slot)))
            Primary = Slot;
    }

    if (Touch.ContactCount != 0)
        QcTouchQueueReport(Device, &Touch, sizeof(Touch));

    WasDown = Device->ContactMask != 0;
    Device->ContactMask = Mask;

    /* The pointer follows the first contact down until it lifts */
    if (Primary != MAXULONG)
    {
        for (Slot = 0; Slot < Touch.ContactCount; Slot++)
        {
            if (Touch.Contacts[Slot].ContactId == Primary)
            {
                Device->PrimaryX = Touch.Contacts[Slot].X;
                Device->PrimaryY = Touch.Contacts[Slot].Y;
            }
        }

        Device->PrimaryContact = Primary;
        QcTouchSendMouse(Device, TRUE);
    }
    else if (WasDown)
    {
        QcTouchSendMouse(Device, FALSE);
    }
}

/**
 * @brief Ends every contact still down, used when the sensor stops reporting.
 */
VOID
NTAPI
QcTouchReleaseAll(
    _In_ PQCTOUCH_DEVICE Device)
{
    QCTOUCH_TOUCH_REPORT Touch;
    ULONG Slot;

    if (Device->ContactMask == 0)
        return;

    RtlZeroMemory(&Touch, sizeof(Touch));
    Touch.ReportId = QCTOUCH_REPORT_ID_TOUCH;

    for (Slot = 0; Slot < QCTOUCH_MAX_CONTACTS; Slot++)
    {
        if (!(Device->ContactMask & (1 << Slot)))
            continue;

        Touch.Contacts[Touch.ContactCount].ContactId = (UCHAR)Slot;
        Touch.Contacts[Touch.ContactCount].X = Device->ContactX[Slot];
        Touch.Contacts[Touch.ContactCount].Y = Device->ContactY[Slot];
        Touch.ContactCount++;
    }

    Device->ContactMask = 0;
    QcTouchQueueReport(Device, &Touch, sizeof(Touch));
    QcTouchSendMouse(Device, FALSE);
}

static
NTSTATUS
QcTouchGetFeature(
    _In_ PQCTOUCH_DEVICE Device,
    _In_ WDFREQUEST Request)
{
    PHID_XFER_PACKET Packet = WdfRequestWdmGetIrp(Request)->UserBuffer;

    if (Packet == NULL || Packet->reportBuffer == NULL || Packet->reportBufferLen < 2)
        return STATUS_INVALID_PARAMETER;

    switch (Packet->reportId)
    {
        case QCTOUCH_REPORT_ID_MAXCOUNT:
            Packet->reportBuffer[0] = QCTOUCH_REPORT_ID_MAXCOUNT;
            Packet->reportBuffer[1] = (UCHAR)(Device->FingerCount ? Device->FingerCount : QCTOUCH_MAX_CONTACTS);
            WdfRequestSetInformation(Request, 2);
            return STATUS_SUCCESS;

        case QCTOUCH_REPORT_ID_CONFIG:
            if (Packet->reportBufferLen < 3)
                return STATUS_BUFFER_TOO_SMALL;

            Packet->reportBuffer[0] = QCTOUCH_REPORT_ID_CONFIG;
            Packet->reportBuffer[1] = Device->InputMode;
            Packet->reportBuffer[2] = 0;
            WdfRequestSetInformation(Request, 3);
            return STATUS_SUCCESS;
    }

    return STATUS_INVALID_PARAMETER;
}

static
NTSTATUS
QcTouchSetFeature(
    _In_ PQCTOUCH_DEVICE Device,
    _In_ WDFREQUEST Request)
{
    PHID_XFER_PACKET Packet = WdfRequestWdmGetIrp(Request)->UserBuffer;

    if (Packet == NULL || Packet->reportBuffer == NULL || Packet->reportBufferLen < 2)
        return STATUS_INVALID_PARAMETER;

    if (Packet->reportId != QCTOUCH_REPORT_ID_CONFIG)
        return STATUS_INVALID_PARAMETER;

    Device->InputMode = Packet->reportBuffer[1];
    WdfRequestSetInformation(Request, Packet->reportBufferLen);
    return STATUS_SUCCESS;
}

VOID
NTAPI
QcTouchHidInternalIoctl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    WDFDEVICE WdfDevice = WdfIoQueueGetDevice(Queue);
    PQCTOUCH_DEVICE Device = QcTouchGetDevice(WdfDevice);
    WDF_REQUEST_SEND_OPTIONS Options;
    HID_DEVICE_ATTRIBUTES Attributes;
    HID_DESCRIPTOR Descriptor;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode)
    {
        case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
            RtlZeroMemory(&Descriptor, sizeof(Descriptor));
            Descriptor.bLength = sizeof(Descriptor);
            Descriptor.bDescriptorType = HID_HID_DESCRIPTOR_TYPE;
            Descriptor.bcdHID = HID_REVISION;
            Descriptor.bNumDescriptors = 1;
            Descriptor.DescriptorList[0].bReportType = HID_REPORT_DESCRIPTOR_TYPE;
            Descriptor.DescriptorList[0].wReportLength = sizeof(QcTouchReportDescriptor);
            Status = QcTouchCopyOut(Request, &Descriptor, sizeof(Descriptor));
            break;

        case IOCTL_HID_GET_REPORT_DESCRIPTOR:
            Status = QcTouchCopyOut(Request, QcTouchReportDescriptor, sizeof(QcTouchReportDescriptor));
            break;

        case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
            RtlZeroMemory(&Attributes, sizeof(Attributes));
            Attributes.Size = sizeof(Attributes);
            Attributes.VendorID = QCTOUCH_HID_VENDOR;
            Attributes.ProductID = QCTOUCH_HID_PRODUCT;
            Attributes.VersionNumber = 1;
            Status = QcTouchCopyOut(Request, &Attributes, sizeof(Attributes));
            break;

        case IOCTL_HID_READ_REPORT:
            Status = WdfRequestForwardToIoQueue(Request, Device->ReadQueue);
            if (!NT_SUCCESS(Status))
                break;

            QcTouchDrainReports(Device);
            return;

        case IOCTL_HID_GET_FEATURE:
            Status = QcTouchGetFeature(Device, Request);
            break;

        case IOCTL_HID_SET_FEATURE:
            Status = QcTouchSetFeature(Device, Request);
            break;

        case IOCTL_HID_ACTIVATE_DEVICE:
        case IOCTL_HID_DEACTIVATE_DEVICE:
            Status = STATUS_SUCCESS;
            break;

        case IOCTL_HID_GET_STRING:
        case IOCTL_HID_WRITE_REPORT:
        case IOCTL_HID_GET_INPUT_REPORT:
        case IOCTL_HID_SET_OUTPUT_REPORT:
            Status = STATUS_NOT_SUPPORTED;
            break;

        default:
            /* Not a HID request, it belongs to the bus below */
            WdfRequestFormatRequestUsingCurrentType(Request);
            WDF_REQUEST_SEND_OPTIONS_INIT(&Options, WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);
            if (WdfRequestSend(Request, WdfDeviceGetIoTarget(WdfDevice), &Options))
                return;

            Status = WdfRequestGetStatus(Request);
            break;
    }

    WdfRequestComplete(Request, Status);
}
