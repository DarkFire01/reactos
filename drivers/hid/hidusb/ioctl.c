/*
 * PROJECT:     ReactOS USB HID Miniport Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Internal HID IOCTLs sent by hidclass
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "hidusb.h"

#define NDEBUG
#include <debug.h>

/* Room left for devices that return more than their HID descriptor announces */
#define HIDUSB_REPORT_DESCRIPTOR_SLACK  64

static IO_COMPLETION_ROUTINE HidUsbReadCompletion;
static IO_COMPLETION_ROUTINE HidUsbGetSetReportCompletion;
static DRIVER_CANCEL HidUsbResetNotificationCancel;

static
PVOID
NTAPI
HidUsbMapIrpMdl(
    _In_ PIRP Irp)
{
    MM_PAGE_PRIORITY Priority;

    if (Irp->MdlAddress == NULL)
        return NULL;

    Priority = (MM_PAGE_PRIORITY)(NormalPagePriority | MdlMappingNoExecute);
    return MmGetSystemAddressForMdlSafe(Irp->MdlAddress, Priority);
}

static
NTSTATUS
NTAPI
HidUsbReadCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_opt_ PVOID Context)
{
    PHIDUSB_EXTENSION Extension;
    NTSTATUS Status;
    PURB Urb;

    Extension = HidUsbGetExtension(DeviceObject);
    Urb = (PURB)Context;
    Status = STATUS_SUCCESS;

    if (NT_SUCCESS(Irp->IoStatus.Status))
    {
        Irp->IoStatus.Information = Urb->UrbBulkOrInterruptTransfer.TransferBufferLength;
    }
    else if ((Irp->IoStatus.Status != STATUS_CANCELLED) &&
             (Irp->IoStatus.Status != STATUS_DEVICE_NOT_CONNECTED))
    {
        /* Stalls and other transfer errors get the port reset sequence */
        DPRINT1("Read failed 0x%lx, queueing a reset\n", Irp->IoStatus.Status);
        Status = HidUsbQueueResetWorkItem(DeviceObject, Irp);
    }

    if ((Status != STATUS_MORE_PROCESSING_REQUIRED) && Irp->PendingReturned)
        IoMarkIrpPending(Irp);

    ExFreePoolWithTag(Urb, HIDUSB_TAG);
    HidUsbDecrementPendingRequests(Extension);
    IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
    return Status;
}

static
NTSTATUS
NTAPI
HidUsbGetSetReportCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_opt_ PVOID Context)
{
    PHIDUSB_EXTENSION Extension;
    PURB Urb;

    Extension = HidUsbGetExtension(DeviceObject);
    Urb = (PURB)Context;

    /* Every transfer type used here keeps the length at the same offset */
    if (NT_SUCCESS(Irp->IoStatus.Status))
        Irp->IoStatus.Information = Urb->UrbControlTransfer.TransferBufferLength;

    ExFreePoolWithTag(Urb, HIDUSB_TAG);

    if (Irp->PendingReturned)
        IoMarkIrpPending(Irp);

    HidUsbDecrementPendingRequests(Extension);
    IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Passes the IRP down with the URB attached. The completion routine frees the URB
 * and drops the remove lock.
 *
 * @param[out] Complete
 * Cleared once the IRP belongs to the lower driver.
 */
static
NTSTATUS
NTAPI
HidUsbSubmitUrbIrp(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp,
    _In_ PURB Urb,
    _In_ PIO_COMPLETION_ROUTINE CompletionRoutine,
    _Out_ PBOOLEAN Complete)
{
    PIO_STACK_LOCATION NextStack;
    PHIDUSB_EXTENSION Extension;

    Extension = HidUsbGetExtension(DeviceObject);

    NextStack = IoGetNextIrpStackLocation(Irp);
    NextStack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    NextStack->Parameters.Others.Argument1 = Urb;
    NextStack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    IoSetCompletionRoutine(Irp, CompletionRoutine, Urb, TRUE, TRUE, TRUE);

    if (!NT_SUCCESS(HidUsbIncrementPendingRequests(Extension)))
    {
        ExFreePoolWithTag(Urb, HIDUSB_TAG);
        return STATUS_NO_SUCH_DEVICE;
    }

    *Complete = FALSE;
    return IoCallDriver(HidUsbGetNextDevice(DeviceObject), Irp);
}

/* Fills in a HID class request to the interface, or to an endpoint for devices that need that */
static
VOID
NTAPI
HidUsbBuildClassRequest(
    _In_ PHIDUSB_EXTENSION Extension,
    _Out_ struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST *Request,
    _In_ USHORT EndpointIndex,
    _In_ UCHAR RequestCode,
    _In_ USHORT Value,
    _In_ ULONG TransferFlags,
    _In_ PHID_XFER_PACKET Packet)
{
    Request->Hdr.Length = sizeof(*Request);

    if (Extension->Flags & HIDUSB_FLAG_ENDPOINT_REQUESTS)
    {
        Request->Hdr.Function = URB_FUNCTION_CLASS_ENDPOINT;
        Request->Index = EndpointIndex;
    }
    else
    {
        Request->Hdr.Function = URB_FUNCTION_CLASS_INTERFACE;
        Request->Index = Extension->InterfaceInfo->InterfaceNumber;
    }

    Request->RequestTypeReservedBits = HIDUSB_CLASS_REQUEST_BITS;
    Request->Request = RequestCode;
    Request->Value = Value;
    Request->TransferFlags = TransferFlags;
    Request->TransferBuffer = Packet->reportBuffer;
    Request->TransferBufferLength = Packet->reportBufferLen;
}

static
NTSTATUS
NTAPI
HidUsbGetHidDescriptor(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PHIDUSB_EXTENSION Extension;
    PIO_STACK_LOCATION IoStack;
    ULONG Length;

    Extension = HidUsbGetExtension(DeviceObject);
    IoStack = IoGetCurrentIrpStackLocation(Irp);

    if (Extension->HidDescriptor.bLength == 0)
    {
        Irp->IoStatus.Information = 0;
        return STATUS_UNSUCCESSFUL;
    }

    Length = min(IoStack->Parameters.DeviceIoControl.OutputBufferLength,
                 Extension->HidDescriptor.bLength);
    Length = min(Length, (ULONG)sizeof(Extension->HidDescriptor));

    RtlCopyMemory(Irp->UserBuffer, &Extension->HidDescriptor, Length);
    Irp->IoStatus.Information = Length;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Reads the report descriptor. A failure other than a disconnect starts the
 * reset sequence, which then completes the IRP.
 */
static
NTSTATUS
NTAPI
HidUsbGetReportDescriptor(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp,
    _Inout_ PBOOLEAN Complete)
{
    PUSBD_PIPE_INFORMATION Pipe;
    PHIDUSB_EXTENSION Extension;
    PIO_STACK_LOCATION IoStack;
    USHORT ReportLength;
    PVOID Descriptor;
    USHORT Function;
    USHORT Recipient;
    NTSTATUS Status;
    ULONG Length;

    Extension = HidUsbGetExtension(DeviceObject);
    IoStack = IoGetCurrentIrpStackLocation(Irp);
    ReportLength = Extension->HidDescriptor.DescriptorList[0].wReportLength;

    Descriptor = NULL;
    Length = ReportLength + HIDUSB_REPORT_DESCRIPTOR_SLACK;
    Function = 0;
    Recipient = 0;
    Status = STATUS_SUCCESS;

    if (Extension->Flags & HIDUSB_FLAG_ENDPOINT_REQUESTS)
    {
        Pipe = HidUsbFindInterruptPipe(Extension, TRUE);
        if (Pipe == NULL)
        {
            Status = STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        else
        {
            Function = URB_FUNCTION_GET_DESCRIPTOR_FROM_ENDPOINT;
            Recipient = (USHORT)(Pipe->EndpointAddress & USB_ENDPOINT_ADDRESS_MASK);
        }
    }
    else
    {
        Function = URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE;
        Recipient = Extension->InterfaceInfo->InterfaceNumber;
    }

    if (NT_SUCCESS(Status))
    {
        Status = HidUsbGetDescriptor(DeviceObject,
                                     Function,
                                     Extension->HidDescriptor.DescriptorList[0].bReportType,
                                     &Descriptor,
                                     &Length,
                                     0,
                                     Recipient);
        if (NT_SUCCESS(Status))
        {
            Length = min(Length, ReportLength);
            Length = min(Length, IoStack->Parameters.DeviceIoControl.OutputBufferLength);

            RtlCopyMemory(Irp->UserBuffer, Descriptor, Length);
            Irp->IoStatus.Information = Length;
            ExFreePoolWithTag(Descriptor, HIDUSB_TAG);
            return Status;
        }

        if (Status == STATUS_DEVICE_NOT_CONNECTED)
            return Status;
    }

    DPRINT1("Report descriptor request failed 0x%lx\n", Status);

    Irp->IoStatus.Status = Status;
    if (HidUsbQueueResetWorkItem(DeviceObject, Irp) == STATUS_MORE_PROCESSING_REQUIRED)
    {
        *Complete = FALSE;
        IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
        return STATUS_PENDING;
    }

    return Status;
}

static
NTSTATUS
NTAPI
HidUsbReadReport(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp,
    _Inout_ PBOOLEAN Complete)
{
    struct _URB_BULK_OR_INTERRUPT_TRANSFER *Transfer;
    PUSBD_PIPE_INFORMATION Pipe;
    PIO_STACK_LOCATION IoStack;
    ULONG Length;

    IoStack = IoGetCurrentIrpStackLocation(Irp);
    Length = IoStack->Parameters.DeviceIoControl.OutputBufferLength;

    if ((Length == 0) || (Irp->UserBuffer == NULL))
        return STATUS_INVALID_PARAMETER;

    Pipe = HidUsbFindInterruptPipe(HidUsbGetExtension(DeviceObject), TRUE);
    if (Pipe == NULL)
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    Transfer = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(URB), HIDUSB_TAG);
    if (Transfer == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Transfer->Hdr.Length = sizeof(*Transfer);
    Transfer->Hdr.Function = URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
    Transfer->PipeHandle = Pipe->PipeHandle;
    Transfer->TransferFlags = USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK;
    Transfer->TransferBuffer = Irp->UserBuffer;
    Transfer->TransferBufferLength = Length;
    Transfer->TransferBufferMDL = NULL;
    Transfer->UrbLink = NULL;

    return HidUsbSubmitUrbIrp(DeviceObject, Irp, (PURB)Transfer, HidUsbReadCompletion, Complete);
}

/**
 * @brief
 * Sends an output report on the interrupt OUT pipe, or as SET_REPORT when the
 * interface has none.
 *
 * @param[in] Timed
 * The packet is a HIDUSB_TIMED_XFER_PACKET and a control transfer honors its timeout.
 */
static
NTSTATUS
NTAPI
HidUsbWriteReport(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp,
    _In_ BOOLEAN Timed,
    _Inout_ PBOOLEAN Complete)
{
    struct _URB_BULK_OR_INTERRUPT_TRANSFER *Transfer;
    struct _URB_CONTROL_TRANSFER_EX *Control;
    PUSB_DEFAULT_PIPE_SETUP_PACKET Setup;
    PHIDUSB_TIMED_XFER_PACKET TimedPacket;
    PUSBD_PIPE_INFORMATION Pipe;
    PHIDUSB_EXTENSION Extension;
    PHID_XFER_PACKET Packet;
    USHORT Value;
    PURB Urb;

    Extension = HidUsbGetExtension(DeviceObject);
    Packet = (PHID_XFER_PACKET)Irp->UserBuffer;

    if ((Packet == NULL) || (Packet->reportBuffer == NULL) || (Packet->reportBufferLen == 0))
        return STATUS_DATA_ERROR;

    Urb = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*Urb), HIDUSB_TAG);
    if (Urb == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Value = (USHORT)(HIDUSB_REPORT_TYPE_OUTPUT + Packet->reportId);

    if (Extension->Flags & HIDUSB_FLAG_ENDPOINT_REQUESTS)
    {
        Pipe = HidUsbFindInterruptPipe(Extension, TRUE);
        if (Pipe == NULL)
        {
            ExFreePoolWithTag(Urb, HIDUSB_TAG);
            return STATUS_DATA_ERROR;
        }

        HidUsbBuildClassRequest(Extension,
                                &Urb->UrbControlVendorClassRequest,
                                (USHORT)(Pipe->EndpointAddress & USB_ENDPOINT_ADDRESS_MASK),
                                HIDUSB_REQUEST_SET_REPORT,
                                Value,
                                0,
                                Packet);
    }
    else
    {
        Pipe = HidUsbFindInterruptPipe(Extension, FALSE);
        if (Pipe != NULL)
        {
            Transfer = &Urb->UrbBulkOrInterruptTransfer;
            Transfer->Hdr.Length = sizeof(*Transfer);
            Transfer->Hdr.Function = URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
            Transfer->PipeHandle = Pipe->PipeHandle;
            Transfer->TransferFlags = USBD_SHORT_TRANSFER_OK;
            Transfer->TransferBuffer = Packet->reportBuffer;
            Transfer->TransferBufferLength = Packet->reportBufferLen;
            Transfer->TransferBufferMDL = NULL;
            Transfer->UrbLink = NULL;
        }
        else if (Timed)
        {
            TimedPacket = (PHIDUSB_TIMED_XFER_PACKET)Packet;

            Control = &Urb->UrbControlTransferEx;
            Control->Hdr.Length = sizeof(*Control);
            Control->Hdr.Function = URB_FUNCTION_CONTROL_TRANSFER_EX;
            Control->TransferFlags = USBD_DEFAULT_PIPE_TRANSFER;
            Control->TransferBuffer = Packet->reportBuffer;
            Control->TransferBufferLength = Packet->reportBufferLen;
            Control->Timeout = TimedPacket->Timeout;
            if (Control->Timeout == 0)
                Control->Timeout = HIDUSB_CONTROL_WRITE_TIMEOUT;

            Setup = (PUSB_DEFAULT_PIPE_SETUP_PACKET)Control->SetupPacket;
            Setup->bmRequestType.B = HIDUSB_CLASS_INTERFACE_OUT;
            Setup->bRequest = HIDUSB_REQUEST_SET_REPORT;
            Setup->wValue.W = Value;
            Setup->wIndex.W = Extension->InterfaceInfo->InterfaceNumber;
            Setup->wLength = (USHORT)Packet->reportBufferLen;
        }
        else
        {
            HidUsbBuildClassRequest(Extension,
                                    &Urb->UrbControlVendorClassRequest,
                                    0,
                                    HIDUSB_REQUEST_SET_REPORT,
                                    Value,
                                    0,
                                    Packet);
        }
    }

    return HidUsbSubmitUrbIrp(DeviceObject, Irp, Urb, HidUsbGetSetReportCompletion, Complete);
}

/* GET_REPORT and SET_REPORT for feature, input and output reports */
static
NTSTATUS
NTAPI
HidUsbGetSetReport(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp,
    _Inout_ PBOOLEAN Complete)
{
    PHIDUSB_EXTENSION Extension;
    PIO_STACK_LOCATION IoStack;
    PHID_XFER_PACKET Packet;
    ULONG TransferFlags;
    USHORT ReportType;
    UCHAR RequestCode;
    PURB Urb;

    Extension = HidUsbGetExtension(DeviceObject);
    IoStack = IoGetCurrentIrpStackLocation(Irp);

    switch (IoStack->Parameters.DeviceIoControl.IoControlCode)
    {
        case IOCTL_HID_SET_FEATURE:
            ReportType = HIDUSB_REPORT_TYPE_FEATURE;
            RequestCode = HIDUSB_REQUEST_SET_REPORT;
            TransferFlags = 0;
            break;

        case IOCTL_HID_GET_FEATURE:
            ReportType = HIDUSB_REPORT_TYPE_FEATURE;
            RequestCode = HIDUSB_REQUEST_GET_REPORT;
            TransferFlags = USBD_TRANSFER_DIRECTION_IN;
            break;

        case IOCTL_HID_SET_OUTPUT_REPORT:
            ReportType = HIDUSB_REPORT_TYPE_OUTPUT;
            RequestCode = HIDUSB_REQUEST_SET_REPORT;
            TransferFlags = 0;
            break;

        case IOCTL_HID_GET_INPUT_REPORT:
            ReportType = HIDUSB_REPORT_TYPE_INPUT;
            RequestCode = HIDUSB_REQUEST_GET_REPORT;
            TransferFlags = USBD_TRANSFER_DIRECTION_IN;
            break;

        default:
            return STATUS_NOT_SUPPORTED;
    }

    Packet = (PHID_XFER_PACKET)Irp->UserBuffer;
    if ((Packet == NULL) || (Packet->reportBuffer == NULL) || (Packet->reportBufferLen == 0))
        return STATUS_DATA_ERROR;

    Urb = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*Urb), HIDUSB_TAG);
    if (Urb == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    /* Windows addresses endpoint style requests to wIndex 1 here */
    HidUsbBuildClassRequest(Extension,
                            &Urb->UrbControlVendorClassRequest,
                            1,
                            RequestCode,
                            (USHORT)(ReportType + Packet->reportId),
                            TransferFlags,
                            Packet);

    return HidUsbSubmitUrbIrp(DeviceObject, Irp, Urb, HidUsbGetSetReportCompletion, Complete);
}

/**
 * @brief
 * Reads a string descriptor. IOCTL_HID_GET_STRING names one of the device
 * descriptor strings, IOCTL_HID_GET_INDEXED_STRING passes the index itself.
 * The language ID sits in the high word of the request value and goes to the
 * device unchanged.
 */
static
NTSTATUS
NTAPI
HidUsbGetString(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PUSB_STRING_DESCRIPTOR StringDescriptor;
    PHIDUSB_EXTENSION Extension;
    PIO_STACK_LOCATION IoStack;
    ULONG BufferLength;
    ULONG CopyLength;
    USHORT LanguageId;
    BOOLEAN Indexed;
    NTSTATUS Status;
    ULONG Request;
    PWCHAR Buffer;
    ULONG Length;
    UCHAR Index;

    Extension = HidUsbGetExtension(DeviceObject);
    IoStack = IoGetCurrentIrpStackLocation(Irp);

    if (IoStack->Parameters.DeviceIoControl.IoControlCode == IOCTL_HID_GET_STRING)
    {
        Buffer = (PWCHAR)Irp->UserBuffer;
        Indexed = FALSE;
    }
    else if (IoStack->Parameters.DeviceIoControl.IoControlCode == IOCTL_HID_GET_INDEXED_STRING)
    {
        Buffer = (PWCHAR)HidUsbMapIrpMdl(Irp);
        Indexed = TRUE;
    }
    else
    {
        return STATUS_INVALID_USER_BUFFER;
    }

    if (Buffer == NULL)
        return STATUS_INVALID_USER_BUFFER;

    BufferLength = IoStack->Parameters.DeviceIoControl.OutputBufferLength;
    if (BufferLength < sizeof(WCHAR))
        return STATUS_INVALID_USER_BUFFER;

    Request = (ULONG)(ULONG_PTR)IoStack->Parameters.DeviceIoControl.Type3InputBuffer;
    LanguageId = (USHORT)(Request >> 16);

    if (Indexed)
    {
        Index = (UCHAR)Request;
    }
    else
    {
        switch (Request & 0xFFFF)
        {
            case HID_STRING_ID_IMANUFACTURER:
                Index = Extension->DeviceDescriptor->iManufacturer;
                break;

            case HID_STRING_ID_IPRODUCT:
                Index = Extension->DeviceDescriptor->iProduct;
                break;

            case HID_STRING_ID_ISERIALNUMBER:
                Index = Extension->DeviceDescriptor->iSerialNumber;
                break;

            default:
                return STATUS_INVALID_PARAMETER;
        }

        if (Index == 0)
            return STATUS_INVALID_PARAMETER;
    }

    /* Room for the descriptor header in front of the characters */
    Length = BufferLength + 2;
    StringDescriptor = ExAllocatePool2(POOL_FLAG_NON_PAGED, Length, HIDUSB_TAG);
    if (StringDescriptor == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = HidUsbGetDescriptor(DeviceObject,
                                 URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE,
                                 USB_STRING_DESCRIPTOR_TYPE,
                                 (PVOID *)&StringDescriptor,
                                 &Length,
                                 Index,
                                 LanguageId);
    if (NT_SUCCESS(Status))
    {
        /* bLength is taken as signed, like Windows does, then limited by what arrived */
        CopyLength = (ULONG)((LONG)(CHAR)StringDescriptor->bLength - 2);
        CopyLength = min(CopyLength, Length);
        CopyLength &= ~(ULONG)1;

        if (CopyLength > BufferLength - sizeof(WCHAR))
        {
            Status = STATUS_INVALID_BUFFER_SIZE;
        }
        else
        {
            RtlCopyMemory(Buffer, StringDescriptor->bString, CopyLength);
            Buffer[CopyLength / sizeof(WCHAR)] = UNICODE_NULL;
            Irp->IoStatus.Information = CopyLength + sizeof(WCHAR);
        }
    }

    ExFreePoolWithTag(StringDescriptor, HIDUSB_TAG);
    return Status;
}

static
NTSTATUS
NTAPI
HidUsbGetDeviceAttributes(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PHID_DEVICE_ATTRIBUTES Attributes;
    PHIDUSB_EXTENSION Extension;
    PIO_STACK_LOCATION IoStack;

    Extension = HidUsbGetExtension(DeviceObject);
    IoStack = IoGetCurrentIrpStackLocation(Irp);
    Attributes = (PHID_DEVICE_ATTRIBUTES)Irp->UserBuffer;

    if (IoStack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(*Attributes))
        return STATUS_INVALID_BUFFER_SIZE;

    RtlZeroMemory(Attributes, sizeof(*Attributes));
    Attributes->Size = sizeof(*Attributes);
    Attributes->VendorID = Extension->DeviceDescriptor->idVendor;
    Attributes->ProductID = Extension->DeviceDescriptor->idProduct;
    Attributes->VersionNumber = Extension->DeviceDescriptor->bcdDevice;

    Irp->IoStatus.Information = sizeof(*Attributes);
    return STATUS_SUCCESS;
}

/* The hidclass callback info has the same layout as USB_IDLE_CALLBACK_INFO */
static
NTSTATUS
NTAPI
HidUsbSendIdleNotification(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp,
    _Inout_ PBOOLEAN Complete)
{
    PIO_STACK_LOCATION IoStack;
    PIO_STACK_LOCATION NextStack;

    IoStack = IoGetCurrentIrpStackLocation(Irp);

    if (IoStack->Parameters.DeviceIoControl.InputBufferLength <
        sizeof(HID_SUBMIT_IDLE_NOTIFICATION_CALLBACK_INFO))
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    /* The request can stay down for as long as the device is idle */
    IoReleaseRemoveLock(&HidUsbGetExtension(DeviceObject)->RemoveLock, Irp);

    IoCopyCurrentIrpStackLocationToNext(Irp);
    NextStack = IoGetNextIrpStackLocation(Irp);
    NextStack->Parameters.DeviceIoControl.IoControlCode =
        IOCTL_INTERNAL_USB_SUBMIT_IDLE_NOTIFICATION;

    *Complete = FALSE;
    return IoCallDriver(HidUsbGetNextDevice(DeviceObject), Irp);
}

static
NTSTATUS
NTAPI
HidUsbGetPhysicalDescriptor(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION IoStack;
    PVOID Buffer;
    ULONG Length;

    IoStack = IoGetCurrentIrpStackLocation(Irp);
    Length = IoStack->Parameters.DeviceIoControl.OutputBufferLength;
    if (Length == 0)
        return STATUS_INVALID_USER_BUFFER;

    Buffer = HidUsbMapIrpMdl(Irp);
    if (Buffer == NULL)
        return STATUS_INVALID_USER_BUFFER;

    return HidUsbGetDescriptor(DeviceObject,
                               URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE,
                               HID_PHYSICAL_DESCRIPTOR_TYPE,
                               &Buffer,
                               &Length,
                               0,
                               0);
}

static
NTSTATUS
NTAPI
HidUsbGetMsGenreDescriptor(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    struct _URB_OS_FEATURE_DESCRIPTOR_REQUEST *Request;
    PHIDUSB_EXTENSION Extension;
    PIO_STACK_LOCATION IoStack;
    NTSTATUS Status;
    PVOID Buffer;
    ULONG Length;

    Extension = HidUsbGetExtension(DeviceObject);
    IoStack = IoGetCurrentIrpStackLocation(Irp);

    Length = IoStack->Parameters.DeviceIoControl.OutputBufferLength;
    if (Length == 0)
        return STATUS_INVALID_USER_BUFFER;

    Buffer = HidUsbMapIrpMdl(Irp);
    if (Buffer == NULL)
        return STATUS_INVALID_USER_BUFFER;

    Request = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*Request), HIDUSB_TAG);
    if (Request == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(Buffer, Length);

    Request->Hdr.Length = sizeof(*Request);
    Request->Hdr.Function = URB_FUNCTION_GET_MS_FEATURE_DESCRIPTOR;
    Request->TransferBuffer = Buffer;
    Request->TransferBufferLength = Length;
    Request->TransferBufferMDL = NULL;
    Request->UrbLink = NULL;
    Request->Recipient = BMREQUEST_TO_INTERFACE;
    Request->InterfaceNumber = Extension->InterfaceInfo->InterfaceNumber;
    Request->MS_PageIndex = 0;
    Request->MS_FeatureDescriptorIndex = HIDUSB_MS_GENRE_INDEX;

    Status = HidUsbCallUsb(DeviceObject, (PURB)Request);
    if (NT_SUCCESS(Status))
    {
        if (USBD_SUCCESS(Request->Hdr.Status))
        {
            Irp->IoStatus.Information = Request->TransferBufferLength;
        }
        else
        {
            DPRINT1("Genre descriptor URB failed 0x%lx\n", Request->Hdr.Status);
            Status = STATUS_UNSUCCESSFUL;
        }
    }

    ExFreePoolWithTag(Request, HIDUSB_TAG);
    return Status;
}

static
VOID
NTAPI
HidUsbResetNotificationCancel(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _Inout_ _IRQL_uses_cancel_ PIRP Irp)
{
    PHIDUSB_EXTENSION Extension;
    BOOLEAN Owned;
    KIRQL OldIrql;

    IoReleaseCancelSpinLock(Irp->CancelIrql);

    Extension = HidUsbGetExtension(DeviceObject);
    Owned = FALSE;

    KeAcquireSpinLock(&Extension->ResetNotifyLock, &OldIrql);
    if (Extension->ResetNotifyIrp == Irp)
    {
        Extension->ResetNotifyIrp = NULL;
        Owned = TRUE;
    }
    KeReleaseSpinLock(&Extension->ResetNotifyLock, OldIrql);

    if (!Owned)
        return;

    Irp->IoStatus.Information = 0;
    Irp->IoStatus.Status = STATUS_CANCELLED;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
}

/**
 * @brief
 * Parks the device reset notification IRP until a port reset succeeds or the
 * device goes away. Only one may be outstanding.
 */
static
NTSTATUS
NTAPI
HidUsbQueueResetNotification(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PHIDUSB_EXTENSION Extension;
    NTSTATUS Status;
    KIRQL OldIrql;

    Extension = HidUsbGetExtension(DeviceObject);

    KeAcquireSpinLock(&Extension->ResetNotifyLock, &OldIrql);

    if (Extension->ResetNotifyIrp != NULL)
    {
        KeReleaseSpinLock(&Extension->ResetNotifyLock, OldIrql);
        Status = STATUS_INVALID_DEVICE_REQUEST;
    }
    else
    {
        Extension->ResetNotifyIrp = Irp;
        IoSetCancelRoutine(Irp, HidUsbResetNotificationCancel);

        if (!Irp->Cancel || (IoSetCancelRoutine(Irp, NULL) == NULL))
        {
            IoMarkIrpPending(Irp);
            KeReleaseSpinLock(&Extension->ResetNotifyLock, OldIrql);
            return STATUS_PENDING;
        }

        Extension->ResetNotifyIrp = NULL;
        KeReleaseSpinLock(&Extension->ResetNotifyLock, OldIrql);
        Status = STATUS_CANCELLED;
    }

    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
    return Status;
}

/**
 * @brief
 * Dispatches the internal IOCTLs hidclass sends to its minidriver.
 *
 * @return
 * STATUS_PENDING when the request went down the stack or waits for a reset,
 * otherwise the status the IRP was completed with.
 */
NTSTATUS
NTAPI
HidUsbInternalIoctl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PHIDUSB_EXTENSION Extension;
    PIO_STACK_LOCATION IoStack;
    BOOLEAN Complete;
    NTSTATUS Status;

    Extension = HidUsbGetExtension(DeviceObject);

    Status = IoAcquireRemoveLock(&Extension->RemoveLock, Irp);
    if (!NT_SUCCESS(Status))
    {
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    IoStack = IoGetCurrentIrpStackLocation(Irp);
    Complete = TRUE;

    switch (IoStack->Parameters.DeviceIoControl.IoControlCode)
    {
        case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
            Status = HidUsbGetHidDescriptor(DeviceObject, Irp);
            break;

        case IOCTL_HID_GET_REPORT_DESCRIPTOR:
            Status = HidUsbGetReportDescriptor(DeviceObject, Irp, &Complete);
            break;

        case IOCTL_HID_READ_REPORT:
            Status = HidUsbReadReport(DeviceObject, Irp, &Complete);
            break;

        case IOCTL_HID_WRITE_REPORT:
            Status = HidUsbWriteReport(DeviceObject, Irp, FALSE, &Complete);
            break;

        case HIDUSB_IOCTL_WRITE_REPORT_TIMED:
            Status = HidUsbWriteReport(DeviceObject, Irp, TRUE, &Complete);
            break;

        case IOCTL_HID_GET_STRING:
        case IOCTL_HID_GET_INDEXED_STRING:
            Status = HidUsbGetString(DeviceObject, Irp);
            break;

        /* Nothing to do, the interrupt pipe only runs while reads are queued */
        case IOCTL_HID_ACTIVATE_DEVICE:
        case IOCTL_HID_DEACTIVATE_DEVICE:
            Status = STATUS_SUCCESS;
            break;

        case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
            Status = HidUsbGetDeviceAttributes(DeviceObject, Irp);
            break;

        case IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST:
            Status = HidUsbSendIdleNotification(DeviceObject, Irp, &Complete);
            break;

        case IOCTL_HID_SET_FEATURE:
        case IOCTL_HID_GET_FEATURE:
        case IOCTL_HID_SET_OUTPUT_REPORT:
        case IOCTL_HID_GET_INPUT_REPORT:
            Status = HidUsbGetSetReport(DeviceObject, Irp, &Complete);
            break;

        case IOCTL_GET_PHYSICAL_DESCRIPTOR:
            Status = HidUsbGetPhysicalDescriptor(DeviceObject, Irp);
            break;

        case IOCTL_HID_GET_MS_GENRE_DESCRIPTOR:
            Status = HidUsbGetMsGenreDescriptor(DeviceObject, Irp);
            break;

        case IOCTL_HID_DEVICERESET_NOTIFICATION:
            return HidUsbQueueResetNotification(DeviceObject, Irp);

        /* Unknown requests complete with whatever status they arrived with */
        default:
            Status = Irp->IoStatus.Status;
            break;
    }

    if (Complete)
    {
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
    }

    return Status;
}
