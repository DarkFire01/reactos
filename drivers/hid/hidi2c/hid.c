/*
 * PROJECT:     ReactOS HID Stack
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     HID over I2C protocol and the hidclass requests
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "hidi2c.h"
#include "hidi2c_msg.h"

#define NDEBUG
#include <debug.h>

static DECLARE_CONST_UNICODE_STRING(HidI2cNoResetWaitValue, L"DoNotWaitForResetResponse");

static EVT_WDF_WORKITEM HidI2cEvtIdleWork;

/**
 * @brief
 * Reads a DWORD under the device's hardware key, leaving Value alone if it is absent.
 */
static
VOID
NTAPI
HidI2cQueryDeviceValue(
    _In_ PHIDI2C_DEVICE Context,
    _In_ PCUNICODE_STRING Name,
    _Inout_ PULONG Value)
{
    WDFKEY Key;

    PAGED_CODE();

    if (!NT_SUCCESS(WdfDeviceOpenRegistryKey(Context->Device,
                                             PLUGPLAY_REGKEY_DEVICE,
                                             KEY_READ,
                                             WDF_NO_OBJECT_ATTRIBUTES,
                                             &Key)))
    {
        return;
    }

    (VOID)WdfRegistryQueryULong(Key, Name, Value);
    WdfRegistryClose(Key);
}

static
VOID
NTAPI
HidI2cSetDeviceValue(
    _In_ PHIDI2C_DEVICE Context,
    _In_ PCUNICODE_STRING Name,
    _In_ ULONG Value)
{
    WDFKEY Key;

    PAGED_CODE();

    if (!NT_SUCCESS(WdfDeviceOpenRegistryKey(Context->Device,
                                             PLUGPLAY_REGKEY_DEVICE,
                                             KEY_WRITE,
                                             WDF_NO_OBJECT_ATTRIBUTES,
                                             &Key)))
    {
        return;
    }

    (VOID)WdfRegistryAssignULong(Key, Name, Value);
    WdfRegistryClose(Key);
}

/**
 * @brief
 * Writes one command word, with its argument, to the command register.
 */
static
NTSTATUS
NTAPI
HidI2cSendCommand(
    _In_ PHIDI2C_DEVICE Context,
    _In_ UCHAR Opcode,
    _In_ UCHAR Argument,
    _In_ ULONG TimeoutSeconds)
{
    UCHAR Command[2];

    Command[0] = Argument;
    Command[1] = Opcode;

    return HidI2cWriteRegister(Context,
                               Context->Descriptor.CommandRegister,
                               Command,
                               sizeof(Command),
                               TimeoutSeconds);
}

static
NTSTATUS
NTAPI
HidI2cSetPower(
    _In_ PHIDI2C_DEVICE Context,
    _In_ UCHAR State)
{
    NTSTATUS Status;

    Status = HidI2cSendCommand(Context, HIDI2C_OP_SET_POWER, State, HIDI2C_POWER_TIMEOUT);
    if (!NT_SUCCESS(Status))
        DPRINT1("hidi2c: SET_POWER %u failed 0x%08lx\n", State, Status);

    return Status;
}

/**
 * @brief
 * Reads the HID descriptor and checks every field a working device has to fill in.
 */
static
NTSTATUS
NTAPI
HidI2cFetchDescriptor(
    _Inout_ PHIDI2C_DEVICE Context)
{
    PHIDI2C_DESCRIPTOR Descriptor = &Context->Descriptor;
    NTSTATUS Status;

    PAGED_CODE();

    RtlZeroMemory(Descriptor, sizeof(*Descriptor));

    Status = HidI2cReadRegister(Context,
                                Context->DescriptorRegister,
                                Descriptor,
                                sizeof(*Descriptor),
                                HIDI2C_DESCRIPTOR_TIMEOUT);
    if (!NT_SUCCESS(Status))
    {
        HidI2cReportProblem(Context, HIDI2C_PROBLEM_DESCRIPTOR_READ);
        return Status;
    }

    if (Descriptor->Length != sizeof(*Descriptor) ||
        Descriptor->Version != 0x0100 ||
        Descriptor->ReportDescriptorRegister == 0 ||
        Descriptor->InputRegister == 0 ||
        Descriptor->MaxInputLength < sizeof(USHORT) ||
        Descriptor->CommandRegister == 0 ||
        Descriptor->DataRegister == 0 ||
        Descriptor->VendorId == 0 ||
        Descriptor->Reserved != 0)
    {
        DPRINT1("hidi2c: HID descriptor rejected, length %u version 0x%04X\n",
                Descriptor->Length,
                Descriptor->Version);
        HidI2cReportProblem(Context, HIDI2C_PROBLEM_DESCRIPTOR_INVALID);
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }

    DPRINT("hidi2c: VID %04X PID %04X, report descriptor %u bytes, input %u bytes\n",
           Descriptor->VendorId,
           Descriptor->ProductId,
           Descriptor->ReportDescriptorLength,
           Descriptor->MaxInputLength);
    return STATUS_SUCCESS;
}

static
VOID
NTAPI
HidI2cFreeInputBuffer(
    _Inout_ PHIDI2C_DEVICE Context)
{
    if (Context->InputBuffer == NULL)
        return;

    ExFreePoolWithTag(Context->InputBuffer, HIDI2C_POOL_TAG);
    Context->InputBuffer = NULL;
}

/**
 * @brief
 * Brings the device to D0. A cold start reads the descriptor and sets up the
 * input buffer first.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
HidI2cInitialize(
    _Inout_ PHIDI2C_DEVICE Context,
    _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    SIZE_T Size;
    NTSTATUS Status;

    PAGED_CODE();

    if (PreviousState == WdfPowerDeviceD3Final)
    {
        Status = HidI2cFetchDescriptor(Context);
        if (!NT_SUCCESS(Status))
            goto Fail;

        /*
         * The ISR accepts a length field up to two bytes past MaxInputLength,
         * so those two bytes exist and read as zero.
         */
        Size = Context->Descriptor.MaxInputLength + sizeof(USHORT);
        Context->InputBuffer = ExAllocatePoolZero(NonPagedPoolNx, Size, HIDI2C_POOL_TAG);
        if (Context->InputBuffer == NULL)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto Fail;
        }
    }

    Status = HidI2cSetPower(Context, HIDI2C_POWER_ON);
    if (NT_SUCCESS(Status))
        return STATUS_SUCCESS;

    HidI2cReportProblem(Context, HIDI2C_PROBLEM_SET_POWER);

Fail:
    HidI2cFreeInputBuffer(Context);
    return Status;
}

/**
 * @brief
 * Puts the device to sleep, and drops the input buffer when it goes away for good.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
HidI2cShutdown(
    _Inout_ PHIDI2C_DEVICE Context,
    _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    NTSTATUS Status;

    PAGED_CODE();

    Status = HidI2cSetPower(Context, HIDI2C_POWER_SLEEP);

    if (!NT_SUCCESS(Status) || TargetState == WdfPowerDeviceD3Final)
        HidI2cFreeInputBuffer(Context);

    return Status;
}

/**
 * @brief
 * Issues RESET. Unless the device is known not to answer, the default queue
 * holds until the empty reset report arrives or the timer gives up.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
HidI2cReset(
    _Inout_ PHIDI2C_DEVICE Context)
{
    ULONG NoWait = 0;
    NTSTATUS Status;

    PAGED_CODE();

    HidI2cQueryDeviceValue(Context, &HidI2cNoResetWaitValue, &NoWait);

    WdfInterruptAcquireLock(Context->Interrupt);

    if (NoWait == 0)
    {
        WdfIoQueueStopSynchronously(Context->DefaultQueue);
        WdfTimerStart(Context->ResetTimer, WDF_REL_TIMEOUT_IN_MS(HIDI2C_RESET_TIMEOUT_MS));
    }

    Status = HidI2cSendCommand(Context, HIDI2C_OP_RESET, 0, HIDI2C_DEFAULT_TIMEOUT);
    if (NT_SUCCESS(Status))
        Context->ResetPending = TRUE;
    else
        DPRINT1("hidi2c: RESET failed 0x%08lx\n", Status);

    WdfInterruptReleaseLock(Context->Interrupt);
    return Status;
}

/**
 * @brief
 * The device never answered the reset: stop waiting for it now and on later starts.
 */
_Use_decl_annotations_
VOID
NTAPI
HidI2cEvtResetTimer(
    WDFTIMER Timer)
{
    PHIDI2C_DEVICE Context = HidI2cGetDevice(WdfTimerGetParentObject(Timer));

    DPRINT1("hidi2c: no reset response, releasing the queue\n");

    WdfIoQueueStart(Context->DefaultQueue);
    HidI2cSetDeviceValue(Context, &HidI2cNoResetWaitValue, 1);
}

static
NTSTATUS
NTAPI
HidI2cGetDeviceDescriptor(
    _In_ PHIDI2C_DEVICE Context,
    _In_ WDFREQUEST Request)
{
    PHID_DESCRIPTOR Descriptor;
    NTSTATUS Status;

    Status = WdfRequestRetrieveOutputBuffer(Request,
                                            sizeof(*Descriptor),
                                            (PVOID *)&Descriptor,
                                            NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    Descriptor->bLength = sizeof(*Descriptor);
    Descriptor->bDescriptorType = HID_HID_DESCRIPTOR_TYPE;
    Descriptor->bcdHID = 0x0001;
    Descriptor->bCountry = 0;
    Descriptor->bNumDescriptors = 1;
    Descriptor->DescriptorList[0].bReportType = HID_REPORT_DESCRIPTOR_TYPE;
    Descriptor->DescriptorList[0].wReportLength = Context->Descriptor.ReportDescriptorLength;

    WdfRequestSetInformation(Request, sizeof(*Descriptor));
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Reads the report descriptor straight into the request's buffer.
 */
static
NTSTATUS
NTAPI
HidI2cGetReportDescriptor(
    _In_ PHIDI2C_DEVICE Context,
    _In_ WDFREQUEST Request)
{
    USHORT Length = Context->Descriptor.ReportDescriptorLength;
    WDFMEMORY Memory;
    size_t Capacity;
    PVOID Buffer;
    NTSTATUS Status;

    Status = WdfRequestRetrieveOutputMemory(Request, &Memory);
    if (!NT_SUCCESS(Status))
        return Status;

    Buffer = WdfMemoryGetBuffer(Memory, &Capacity);
    if (Buffer == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    if (Capacity < Length)
        return STATUS_INVALID_BUFFER_SIZE;

    Status = HidI2cReadRegister(Context,
                                Context->Descriptor.ReportDescriptorRegister,
                                Buffer,
                                Length,
                                HIDI2C_DEFAULT_TIMEOUT);
    if (!NT_SUCCESS(Status))
        return Status;

    WdfRequestSetInformation(Request, Length);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
HidI2cGetDeviceAttributes(
    _In_ PHIDI2C_DEVICE Context,
    _In_ WDFREQUEST Request)
{
    PHID_DEVICE_ATTRIBUTES Attributes;
    NTSTATUS Status;

    Status = WdfRequestRetrieveOutputBuffer(Request,
                                            sizeof(*Attributes),
                                            (PVOID *)&Attributes,
                                            NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    Attributes->Size = sizeof(*Attributes);
    Attributes->VendorID = Context->Descriptor.VendorId;
    Attributes->ProductID = Context->Descriptor.ProductId;
    Attributes->VersionNumber = Context->Descriptor.VersionId;

    WdfRequestSetInformation(Request, sizeof(*Attributes));
    return STATUS_SUCCESS;
}

/**
 * @brief
 * The fixed strings every HID over I2C device reports.
 */
static
NTSTATUS
NTAPI
HidI2cGetString(
    _In_ WDFREQUEST Request)
{
    PIRP Irp = WdfRequestWdmGetIrp(Request);
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PCWSTR Text;
    SIZE_T Size;

    switch ((USHORT)(ULONG_PTR)Stack->Parameters.DeviceIoControl.Type3InputBuffer)
    {
        case HID_STRING_ID_IMANUFACTURER:
            Text = L"Microsoft";
            break;

        case HID_STRING_ID_IPRODUCT:
            Text = L"HIDI2C Device";
            break;

        case HID_STRING_ID_ISERIALNUMBER:
            Text = L"9999";
            break;

        default:
            return STATUS_INVALID_PARAMETER;
    }

    Size = (wcslen(Text) + 1) * sizeof(WCHAR);
    if (Stack->Parameters.DeviceIoControl.OutputBufferLength < Size || Irp->UserBuffer == NULL)
        return STATUS_INVALID_BUFFER_SIZE;

    RtlCopyMemory(Irp->UserBuffer, Text, Size);
    WdfRequestSetInformation(Request, Size);
    return STATUS_SUCCESS;
}

/**
 * @brief
 * The transfer packet hidclass hangs off the IRP for report requests.
 *
 * The buffer length hidclass passes must cover the packet itself: the output
 * length for GET requests, the input length for SET and WRITE.
 */
static
PHID_XFER_PACKET
NTAPI
HidI2cGetPacket(
    _In_ WDFREQUEST Request,
    _In_ BOOLEAN FromDevice,
    _Out_ PNTSTATUS Status)
{
    WDF_REQUEST_PARAMETERS Parameters;
    PHID_XFER_PACKET Packet;
    size_t Length;

    WDF_REQUEST_PARAMETERS_INIT(&Parameters);
    WdfRequestGetParameters(Request, &Parameters);

    if (FromDevice)
        Length = Parameters.Parameters.DeviceIoControl.OutputBufferLength;
    else
        Length = Parameters.Parameters.DeviceIoControl.InputBufferLength;

    if (Length < sizeof(*Packet))
    {
        *Status = STATUS_INVALID_BUFFER_SIZE;
        return NULL;
    }

    Packet = WdfRequestWdmGetIrp(Request)->UserBuffer;
    if (Packet == NULL)
    {
        *Status = STATUS_INVALID_PARAMETER;
        return NULL;
    }

    if (Packet->reportBufferLen == 0 || Packet->reportBuffer == NULL)
    {
        *Status = STATUS_BUFFER_TOO_SMALL;
        return NULL;
    }

    *Status = STATUS_SUCCESS;
    return Packet;
}

/**
 * @brief
 * Fills the command bytes for a GET_REPORT or SET_REPORT of one report.
 *
 * @return
 * How many of the three command bytes are used.
 */
static
USHORT
NTAPI
HidI2cReportCommand(
    _In_ UCHAR Opcode,
    _In_ UCHAR Type,
    _In_ UCHAR ReportId,
    _Out_writes_(3) PUCHAR Command)
{
    Command[1] = Opcode;

    if (ReportId < HIDI2C_REPORT_ID_ESCAPE)
    {
        Command[0] = (UCHAR)((Type << 4) | ReportId);
        return 2;
    }

    Command[0] = (UCHAR)((Type << 4) | HIDI2C_REPORT_ID_ESCAPE);
    Command[2] = ReportId;
    return 3;
}

/**
 * @brief
 * GET_REPORT for an input or feature report, answered through the data register.
 */
static
NTSTATUS
NTAPI
HidI2cGetReport(
    _In_ PHIDI2C_DEVICE Context,
    _In_ WDFREQUEST Request,
    _In_ UCHAR Type)
{
    PHID_XFER_PACKET Packet;
    USHORT CommandLength;
    UCHAR Command[3];
    USHORT Returned;
    PUCHAR Answer;
    USHORT Size;
    NTSTATUS Status;

    Packet = HidI2cGetPacket(Request, TRUE, &Status);
    if (Packet == NULL)
        return Status;

    if (Type != HIDI2C_REPORT_INPUT && Type != HIDI2C_REPORT_FEATURE)
        return STATUS_INVALID_PARAMETER;

    CommandLength = HidI2cReportCommand(HIDI2C_OP_GET_REPORT, Type, Packet->reportId, Command);

    /* The answer leads with its own length, and the whole of it has to fit a USHORT */
    if (Packet->reportBufferLen >= MAXUSHORT - sizeof(USHORT))
        Size = MAXUSHORT;
    else
        Size = (USHORT)(Packet->reportBufferLen + sizeof(USHORT));

    Answer = ExAllocatePoolZero(NonPagedPoolNx, Size, HIDI2C_POOL_TAG);
    if (Answer == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = HidI2cCommandRead(Context, Command, CommandLength, Answer, Size);
    if (!NT_SUCCESS(Status))
        goto Exit;

    /* A bare length field is an empty report */
    Returned = (USHORT)(Answer[0] | (Answer[1] << 8));
    if (Returned == sizeof(USHORT))
        goto Exit;

    Returned -= sizeof(USHORT);
    if (Returned > Packet->reportBufferLen || Returned > Size - sizeof(USHORT))
    {
        Status = STATUS_BUFFER_TOO_SMALL;
        goto Exit;
    }

    RtlCopyMemory(Packet->reportBuffer, Answer + sizeof(USHORT), Returned);
    WdfRequestSetInformation(Request, Returned);

Exit:
    ExFreePoolWithTag(Answer, HIDI2C_POOL_TAG);
    return Status;
}

/**
 * @brief
 * SET_REPORT for an output or feature report, the data going to the data register.
 */
static
NTSTATUS
NTAPI
HidI2cSetReport(
    _In_ PHIDI2C_DEVICE Context,
    _In_ WDFREQUEST Request,
    _In_ UCHAR Type)
{
    PHID_XFER_PACKET Packet;
    USHORT CommandLength;
    UCHAR Command[3];
    PUCHAR Data;
    USHORT Size;
    NTSTATUS Status;

    Packet = HidI2cGetPacket(Request, FALSE, &Status);
    if (Packet == NULL)
        return Status;

    if (Type != HIDI2C_REPORT_OUTPUT && Type != HIDI2C_REPORT_FEATURE)
        return STATUS_INVALID_PARAMETER;

    /* The length field covers itself and has to fit a USHORT */
    if (Packet->reportBufferLen > MAXUSHORT - sizeof(USHORT))
        return STATUS_INVALID_PARAMETER;

    CommandLength = HidI2cReportCommand(HIDI2C_OP_SET_REPORT, Type, Packet->reportId, Command);

    Size = (USHORT)(Packet->reportBufferLen + sizeof(USHORT));
    Data = ExAllocatePoolZero(NonPagedPoolNx, Size, HIDI2C_POOL_TAG);
    if (Data == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Data[0] = (UCHAR)(Size & 0xFF);
    Data[1] = (UCHAR)(Size >> 8);
    RtlCopyMemory(Data + sizeof(USHORT), Packet->reportBuffer, Packet->reportBufferLen);

    Status = HidI2cCommandWrite(Context, Command, CommandLength, Data, Size);
    if (NT_SUCCESS(Status))
        WdfRequestSetInformation(Request, Packet->reportBufferLen);

    ExFreePoolWithTag(Data, HIDI2C_POOL_TAG);
    return Status;
}

/**
 * @brief
 * Sends an output report straight to the output register.
 */
static
NTSTATUS
NTAPI
HidI2cWriteReport(
    _In_ PHIDI2C_DEVICE Context,
    _In_ WDFREQUEST Request)
{
    PHID_XFER_PACKET Packet;
    PUCHAR Data;
    USHORT Size;
    NTSTATUS Status;

    Packet = HidI2cGetPacket(Request, FALSE, &Status);
    if (Packet == NULL)
        return Status;

    if (Packet->reportBufferLen > MAXUSHORT - sizeof(USHORT))
        return STATUS_INVALID_PARAMETER;

    Size = (USHORT)(Packet->reportBufferLen + sizeof(USHORT));
    if (Size > Context->Descriptor.MaxOutputLength)
        return STATUS_INVALID_PARAMETER;

    Data = ExAllocatePoolZero(NonPagedPoolNx, Size, HIDI2C_POOL_TAG);
    if (Data == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Data[0] = (UCHAR)(Size & 0xFF);
    Data[1] = (UCHAR)(Size >> 8);
    RtlCopyMemory(Data + sizeof(USHORT), Packet->reportBuffer, Packet->reportBufferLen);

    Status = HidI2cWriteRegister(Context,
                                 Context->Descriptor.OutputRegister,
                                 Data,
                                 Size,
                                 HIDI2C_DEFAULT_TIMEOUT);
    if (NT_SUCCESS(Status))
        WdfRequestSetInformation(Request, Packet->reportBufferLen);

    ExFreePoolWithTag(Data, HIDI2C_POOL_TAG);
    return Status;
}

/**
 * @brief
 * Calls hidclass back from a work item, then parks the request until the next D0 entry.
 */
static
VOID
NTAPI
HidI2cEvtIdleWork(
    _In_ WDFWORKITEM WorkItem)
{
    PHIDI2C_IDLE_WORK Work = HidI2cGetIdleWork(WorkItem);
    PHIDI2C_DEVICE Context = HidI2cGetDevice(Work->Device);
    PHID_SUBMIT_IDLE_NOTIFICATION_CALLBACK_INFO Info;
    PIO_STACK_LOCATION Stack;
    NTSTATUS Status;

    Stack = IoGetCurrentIrpStackLocation(WdfRequestWdmGetIrp(Work->Request));
    Info = Stack->Parameters.DeviceIoControl.Type3InputBuffer;
    Info->IdleCallback(Info->IdleContext);

    Status = WdfRequestForwardToIoQueue(Work->Request, Context->IdleQueue);
    if (!NT_SUCCESS(Status))
        WdfRequestComplete(Work->Request, Status);

    WdfObjectDelete(WorkItem);
}

/**
 * @brief
 * Accepts an idle notification request if it carries a callback.
 *
 * @return
 * STATUS_PENDING when the request now belongs to the work item.
 */
static
NTSTATUS
NTAPI
HidI2cSendIdleNotification(
    _In_ PHIDI2C_DEVICE Context,
    _In_ WDFREQUEST Request)
{
    PHID_SUBMIT_IDLE_NOTIFICATION_CALLBACK_INFO Info;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_WORKITEM_CONFIG Config;
    PIO_STACK_LOCATION Stack;
    WDFWORKITEM WorkItem;
    PHIDI2C_IDLE_WORK Work;
    NTSTATUS Status;

    Stack = IoGetCurrentIrpStackLocation(WdfRequestWdmGetIrp(Request));
    Info = Stack->Parameters.DeviceIoControl.Type3InputBuffer;
    if (Info == NULL || Info->IdleCallback == NULL)
        return STATUS_NO_CALLBACK_ACTIVE;

    WDF_WORKITEM_CONFIG_INIT(&Config, HidI2cEvtIdleWork);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, HIDI2C_IDLE_WORK);
    Attributes.ParentObject = Context->Device;

    Status = WdfWorkItemCreate(&Config, &Attributes, &WorkItem);
    if (!NT_SUCCESS(Status))
        return Status;

    Work = HidI2cGetIdleWork(WorkItem);
    Work->Device = Context->Device;
    Work->Request = Request;

    WdfWorkItemEnqueue(WorkItem);
    return STATUS_PENDING;
}

/**
 * @brief
 * Completes the idle notification parked by the work item, the device is back in D0.
 */
VOID
NTAPI
HidI2cCompleteIdleRequest(
    _In_ PHIDI2C_DEVICE Context)
{
    WDFREQUEST Request;

    if (NT_SUCCESS(WdfIoQueueRetrieveNextRequest(Context->IdleQueue, &Request)))
        WdfRequestComplete(Request, STATUS_SUCCESS);
}

/**
 * @brief
 * Lets a power down go ahead while an idle notification is still with the work item.
 */
_Use_decl_annotations_
VOID
NTAPI
HidI2cEvtIoStop(
    WDFQUEUE Queue,
    WDFREQUEST Request,
    ULONG ActionFlags)
{
    WDF_REQUEST_PARAMETERS Parameters;
    ULONG IoControlCode;

    UNREFERENCED_PARAMETER(Queue);

    if (ActionFlags & WdfRequestStopActionPurge)
        return;

    WDF_REQUEST_PARAMETERS_INIT(&Parameters);
    WdfRequestGetParameters(Request, &Parameters);

    IoControlCode = Parameters.Parameters.DeviceIoControl.IoControlCode;

    if (Parameters.Type == WdfRequestTypeDeviceControlInternal &&
        IoControlCode == IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST &&
        (ActionFlags & WdfRequestStopActionSuspend))
    {
        WdfRequestStopAcknowledge(Request, FALSE);
    }
}

/**
 * @brief
 * Handles the IOCTL_HID_* requests mshidkmdf forwards from hidclass.
 */
_Use_decl_annotations_
VOID
NTAPI
HidI2cEvtInternalDeviceControl(
    WDFQUEUE Queue,
    WDFREQUEST Request,
    size_t OutputBufferLength,
    size_t InputBufferLength,
    ULONG IoControlCode)
{
    PHIDI2C_DEVICE Context = HidI2cGetDevice(WdfIoQueueGetDevice(Queue));
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode)
    {
        case IOCTL_HID_READ_REPORT:
            /* Parked until the device interrupts with a report */
            Status = WdfRequestForwardToIoQueue(Request, Context->ReadQueue);
            if (NT_SUCCESS(Status))
                return;
            break;

        case IOCTL_HID_DEVICERESET_NOTIFICATION:
            Status = WdfRequestForwardToIoQueue(Request, Context->ResetNotifyQueue);
            if (NT_SUCCESS(Status))
                return;
            break;

        case IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST:
            Status = HidI2cSendIdleNotification(Context, Request);
            if (Status == STATUS_PENDING)
                return;
            break;

        case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
            Status = HidI2cGetDeviceDescriptor(Context, Request);
            break;

        case IOCTL_HID_GET_REPORT_DESCRIPTOR:
            Status = HidI2cGetReportDescriptor(Context, Request);
            break;

        case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
            Status = HidI2cGetDeviceAttributes(Context, Request);
            break;

        case IOCTL_HID_WRITE_REPORT:
            Status = HidI2cWriteReport(Context, Request);
            break;

        case IOCTL_HID_GET_STRING:
            Status = HidI2cGetString(Request);
            break;

        case IOCTL_HID_GET_INPUT_REPORT:
            Status = HidI2cGetReport(Context, Request, HIDI2C_REPORT_INPUT);
            break;

        case IOCTL_HID_GET_FEATURE:
            Status = HidI2cGetReport(Context, Request, HIDI2C_REPORT_FEATURE);
            break;

        case IOCTL_HID_SET_FEATURE:
            Status = HidI2cSetReport(Context, Request, HIDI2C_REPORT_FEATURE);
            break;

        case IOCTL_HID_SET_OUTPUT_REPORT:
            Status = HidI2cSetReport(Context, Request, HIDI2C_REPORT_OUTPUT);
            break;

        default:
            Status = STATUS_NOT_SUPPORTED;
            break;
    }

    WdfRequestComplete(Request, Status);
}
