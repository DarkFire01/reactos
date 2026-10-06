/*
 * PROJECT:     ReactOS HID Stack
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     HID over I2C - the wire protocol and the hidclass IOCTLs
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */


#define NDEBUG
#include "hidi2cp.h"

/*
 * One write-read against the register _DSM gave us. Everything else this driver
 * knows about the device is in the thirty bytes that come back.
 */
static
NTSTATUS
HidI2cGetDescriptor(
    _In_ PHIDI2C_CONTEXT Context)
{
    USHORT Register = Context->DescriptorRegister;
    NTSTATUS Status;

    PAGED_CODE();

    Status = HidI2cSpbWriteRead(Context,
                                &Register,
                                sizeof(Register),
                                &Context->Descriptor,
                                HIDI2C_DESCRIPTOR_LENGTH);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    /*
     * A device that is not ready answers a read with all ones or all zeroes, so
     * the length field is the cheapest way to tell a real descriptor from a bus
     * that acknowledged and said nothing.
     */
    if (Context->Descriptor.DescLength != HIDI2C_DESCRIPTOR_LENGTH)
    {
        DPRINT1("hidi2c: descriptor length is %u, expected %u\n",
                Context->Descriptor.DescLength, HIDI2C_DESCRIPTOR_LENGTH);
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }

    if (Context->Descriptor.ReportDescLength == 0 ||
        Context->Descriptor.MaxInputLength == 0)
    {
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }

    DPRINT("hidi2c: VID %04X PID %04X, report desc %u bytes, input %u bytes\n",
           Context->Descriptor.VendorId, Context->Descriptor.ProductId,
           Context->Descriptor.ReportDescLength,
           Context->Descriptor.MaxInputLength);

    return STATUS_SUCCESS;
}

static
NTSTATUS
HidI2cGetReportDescriptor(
    _In_ PHIDI2C_CONTEXT Context)
{
    USHORT Register = Context->Descriptor.ReportDescRegister;
    NTSTATUS Status;

    PAGED_CODE();

    Context->ReportDescriptorLength = Context->Descriptor.ReportDescLength;
    Context->ReportDescriptor = ExAllocatePoolWithTag(NonPagedPool,
                                                      Context->ReportDescriptorLength,
                                                      HIDI2C_POOL_TAG);
    if (Context->ReportDescriptor == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = HidI2cSpbWriteRead(Context,
                                &Register,
                                sizeof(Register),
                                Context->ReportDescriptor,
                                Context->ReportDescriptorLength);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Context->ReportDescriptor, HIDI2C_POOL_TAG);
        Context->ReportDescriptor = NULL;
        Context->ReportDescriptorLength = 0;
    }

    return Status;
}

/*
 * SET_POWER is a bare command with no data phase, so it is a single write of the
 * command register address followed by the two opcode bytes.
 */
NTSTATUS
HidI2cSetPower(
    _In_ PHIDI2C_CONTEXT Context,
    _In_ UCHAR PowerState)
{
    UCHAR Command[4];

    RtlCopyMemory(&Command[0], &Context->Descriptor.CommandRegister, sizeof(USHORT));
    Command[2] = PowerState;
    Command[3] = HIDI2C_OPCODE_SET_POWER;

    return HidI2cSpbWrite(Context, Command, sizeof(Command));
}

/*
 * RESET is the one command whose completion is signalled out of band: the device
 * raises its interrupt and returns a zero-length input report once it is ready.
 * The reference arms a timer for that and fails the device if it never comes.
 */
NTSTATUS
HidI2cReset(
    _In_ PHIDI2C_CONTEXT Context)
{
    UCHAR Command[4];

    RtlCopyMemory(&Command[0], &Context->Descriptor.CommandRegister, sizeof(USHORT));
    Command[2] = 0;
    Command[3] = HIDI2C_OPCODE_RESET;

    return HidI2cSpbWrite(Context, Command, sizeof(Command));
}

/*
 * The device holds its interrupt asserted until the input register is read, so
 * reading it is also what deasserts the line. The first two bytes are the length
 * of the whole report including themselves; a length of zero is the device
 * announcing a completed reset rather than an input report.
 */
NTSTATUS
HidI2cReadInputReport(
    _In_ PHIDI2C_CONTEXT Context,
    _Out_ PULONG BytesRead)
{
    USHORT Length;
    NTSTATUS Status;

    *BytesRead = 0;

    Status = HidI2cSpbRead(Context, Context->InputBuffer, Context->InputBufferLength);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    RtlCopyMemory(&Length, Context->InputBuffer, sizeof(Length));

    if (Length == 0)
    {
        /* Reset complete, not a report */
        return STATUS_SUCCESS;
    }

    if (Length > Context->InputBufferLength || Length < sizeof(USHORT))
    {
        DPRINT1("hidi2c: input report claims %u bytes, buffer is %lu\n",
                Length, Context->InputBufferLength);
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }

    *BytesRead = Length;
    return STATUS_SUCCESS;
}

/*
 * The bring-up order matters: power on first, because a device in sleep NAKs
 * everything; then the descriptor, which names every other register; then the
 * report descriptor, whose length only the descriptor knows.
 */
NTSTATUS
HidI2cInitialize(
    _In_ PHIDI2C_CONTEXT Context)
{
    NTSTATUS Status;

    PAGED_CODE();

    Status = HidI2cAcpiGetDescriptorRegister(Context);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    Status = HidI2cGetDescriptor(Context);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    Status = HidI2cSetPower(Context, HIDI2C_POWER_ON);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    Status = HidI2cGetReportDescriptor(Context);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    Context->InputBufferLength = Context->Descriptor.MaxInputLength;
    Context->InputBuffer = ExAllocatePoolWithTag(NonPagedPool,
                                                 Context->InputBufferLength,
                                                 HIDI2C_POOL_TAG);
    if (Context->InputBuffer == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    return STATUS_SUCCESS;
}

VOID
HidI2cDestroy(
    _In_ PHIDI2C_CONTEXT Context)
{
    if (Context->InputBuffer != NULL)
    {
        ExFreePoolWithTag(Context->InputBuffer, HIDI2C_POOL_TAG);
        Context->InputBuffer = NULL;
    }

    if (Context->ReportDescriptor != NULL)
    {
        ExFreePoolWithTag(Context->ReportDescriptor, HIDI2C_POOL_TAG);
        Context->ReportDescriptor = NULL;
    }
}

/*
 * A GPIO-backed interrupt has nothing to acknowledge in this device's registers;
 * the line is level-triggered and clears when the input report is read, which
 * happens in the DPC.
 */
BOOLEAN
NTAPI
HidI2cEvtInterruptIsr(
    _In_ WDFINTERRUPT Interrupt,
    _In_ ULONG MessageId)
{
    UNREFERENCED_PARAMETER(MessageId);

    WdfInterruptQueueDpcForIsr(Interrupt);
    return TRUE;
}

VOID
NTAPI
HidI2cEvtInterruptDpc(
    _In_ WDFINTERRUPT Interrupt,
    _In_ WDFOBJECT AssociatedObject)
{
    PHIDI2C_CONTEXT Context = HidI2cGetContext(WdfInterruptGetDevice(Interrupt));
    WDFREQUEST Request;
    PVOID Buffer;
    size_t BufferLength;
    ULONG BytesRead = 0;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(AssociatedObject);

    Status = HidI2cReadInputReport(Context, &BytesRead);
    if (!NT_SUCCESS(Status) || BytesRead == 0)
    {
        return;
    }

    Status = WdfIoQueueRetrieveNextRequest(Context->ReadQueue, &Request);
    if (!NT_SUCCESS(Status))
    {
        /* Nobody is reading; the report is dropped, as the reference does */
        return;
    }

    Status = WdfRequestRetrieveOutputBuffer(Request, 1, &Buffer, &BufferLength);
    if (NT_SUCCESS(Status))
    {
        /*
         * hidclass wants the report without the two-byte length prefix the wire
         * format carries.
         */
        ULONG ReportLength = BytesRead - sizeof(USHORT);

        if (ReportLength > BufferLength)
        {
            ReportLength = (ULONG)BufferLength;
        }

        RtlCopyMemory(Buffer, Context->InputBuffer + sizeof(USHORT), ReportLength);
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, ReportLength);
    }
    else
    {
        WdfRequestComplete(Request, Status);
    }
}

/*
 * What mshidkmdf forwards down from hidclass. Only the descriptor and attribute
 * queries plus the read loop are built; the rest are refused so a caller gets a
 * defined answer instead of a request that never completes.
 */
VOID
NTAPI
HidI2cEvtInternalDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    PHIDI2C_CONTEXT Context = HidI2cGetContext(WdfIoQueueGetDevice(Queue));
    ULONG_PTR Information = 0;
    NTSTATUS Status;
    PVOID Buffer;
    size_t Length;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode)
    {
        case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
        {
            HID_DESCRIPTOR Descriptor;

            Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(Descriptor),
                                                    &Buffer, &Length);
            if (!NT_SUCCESS(Status))
                break;

            /*
             * hidclass wants a USB-shaped HID descriptor, so the I2C one is
             * translated: the only field it really consumes is the report
             * descriptor's length.
             */
            RtlZeroMemory(&Descriptor, sizeof(Descriptor));
            Descriptor.bLength = sizeof(HID_DESCRIPTOR);
            Descriptor.bDescriptorType = HID_HID_DESCRIPTOR_TYPE;
            Descriptor.bcdHID = Context->Descriptor.BcdVersion;
            Descriptor.bCountry = 0;
            Descriptor.bNumDescriptors = 1;
            Descriptor.DescriptorList[0].bReportType = HID_REPORT_DESCRIPTOR_TYPE;
            Descriptor.DescriptorList[0].wReportLength =
                Context->Descriptor.ReportDescLength;

            RtlCopyMemory(Buffer, &Descriptor, sizeof(Descriptor));
            Information = sizeof(Descriptor);
            Status = STATUS_SUCCESS;
            break;
        }

        case IOCTL_HID_GET_REPORT_DESCRIPTOR:
            if (Context->ReportDescriptor == NULL)
            {
                Status = STATUS_DEVICE_NOT_READY;
                break;
            }

            Status = WdfRequestRetrieveOutputBuffer(Request,
                                                    Context->ReportDescriptorLength,
                                                    &Buffer, &Length);
            if (!NT_SUCCESS(Status))
                break;

            RtlCopyMemory(Buffer, Context->ReportDescriptor,
                          Context->ReportDescriptorLength);
            Information = Context->ReportDescriptorLength;
            Status = STATUS_SUCCESS;
            break;

        case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
        {
            PHID_DEVICE_ATTRIBUTES Attributes;

            Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*Attributes),
                                                    (PVOID *)&Attributes, &Length);
            if (!NT_SUCCESS(Status))
                break;

            RtlZeroMemory(Attributes, sizeof(*Attributes));
            Attributes->Size = sizeof(*Attributes);
            Attributes->VendorID = Context->Descriptor.VendorId;
            Attributes->ProductID = Context->Descriptor.ProductId;
            Attributes->VersionNumber = Context->Descriptor.VersionId;

            Information = sizeof(*Attributes);
            Status = STATUS_SUCCESS;
            break;
        }

        case IOCTL_HID_READ_REPORT:
            /*
             * Parked until the device interrupts. The DPC completes it, which is
             * what makes the read loop interrupt-driven rather than polled.
             */
            Status = WdfRequestForwardToIoQueue(Request, Context->ReadQueue);
            if (NT_SUCCESS(Status))
            {
                return;
            }
            break;

        default:
            /*
             * NOT YET BUILT: GET_REPORT, SET_REPORT, GET/SET_FEATURE and the
             * idle notification. Each is a command-register write followed by a
             * data-register transfer, and none is needed to enumerate a device
             * and read from it.
             */
            Status = STATUS_NOT_SUPPORTED;
            break;
    }

    WdfRequestCompleteWithInformation(Request, Status, Information);
}
