/*
 * PROJECT:     ReactOS GPIO framework extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     GpioIo connections opened through the resource hub
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * A peripheral driver opens \Device\RESOURCE_HUB\<id>, the hub sends the open
 * here with the id as the file name, and the GpioIo descriptor behind it says
 * which pins the file reads or drives.
 */

#define NDEBUG
#include "gpioclxp.h"

/* Field offsets inside a GpioIo descriptor */
#define GCX_GPIO_TYPE           4
#define GCX_GPIO_IO_FLAGS       7
#define GCX_GPIO_PIN_CONFIG     9
#define GCX_GPIO_DRIVE          10
#define GCX_GPIO_DEBOUNCE       12
#define GCX_GPIO_PIN_TABLE      14
#define GCX_GPIO_SOURCE         17
#define GCX_GPIO_VENDOR         19
#define GCX_GPIO_VENDOR_LENGTH  21

/* GpioIo IoRestriction values */
#define GCX_IO_RESTRICT_INPUT   1
#define GCX_IO_RESTRICT_OUTPUT  2

static
USHORT
GcxReadUshort(
    _In_ PUCHAR Descriptor,
    _In_ ULONG Offset)
{
    return (USHORT)(Descriptor[Offset] | (Descriptor[Offset + 1] << 8));
}

/**
 * @brief
 * Reads the connection id from the 16 hex digits of a reparsed open.
 */
static
NTSTATUS
GcxParseConnectionName(
    _In_ PCUNICODE_STRING Name,
    _Out_ PLARGE_INTEGER Id)
{
    ULONG Digits = sizeof(LARGE_INTEGER) * 2;
    PCWSTR Buffer = Name->Buffer;
    ULONG Count = Name->Length / sizeof(WCHAR);
    ULONGLONG Value = 0;
    ULONG Index;
    WCHAR Digit;

    if (Count && Buffer[0] == L'\\')
    {
        Buffer++;
        Count--;
    }

    if (Count < Digits)
        return STATUS_OBJECT_NAME_INVALID;

    for (Index = 0; Index < Digits; Index++)
    {
        Digit = Buffer[Index];
        if (Digit >= L'0' && Digit <= L'9')
            Value = (Value << 4) | (ULONG)(Digit - L'0');
        else if (Digit >= L'a' && Digit <= L'f')
            Value = (Value << 4) | (ULONG)(Digit - L'a' + 10);
        else if (Digit >= L'A' && Digit <= L'F')
            Value = (Value << 4) | (ULONG)(Digit - L'A' + 10);
        else
            return STATUS_OBJECT_NAME_INVALID;
    }

    Id->QuadPart = (LONGLONG)Value;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Fills a connection from the GpioIo descriptor the hub returned.
 */
static
NTSTATUS
GcxParseIoDescriptor(
    _In_ PGCX_CONTROLLER Controller,
    _In_ PRH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER Output,
    _Inout_ PGCX_IO_CONNECTION Connection)
{
    PUCHAR Descriptor = Output->ConnectionProperties;
    USHORT TableOffset, SourceOffset, Count, Index;
    BANK_ID BankId;
    PIN_NUMBER Offset;

    if (Output->PropertiesLength < GCX_GPIO_VENDOR_LENGTH + 2 ||
        Descriptor[0] != GPIO_INTERRUPT_IO_DESCRIPTOR ||
        Descriptor[GCX_GPIO_TYPE] != PNP_GPIO_IRQ_DESCRIPTOR_TYPE_IO)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* The pin table ends where the controller name starts */
    TableOffset = GcxReadUshort(Descriptor, GCX_GPIO_PIN_TABLE);
    SourceOffset = GcxReadUshort(Descriptor, GCX_GPIO_SOURCE);
    if (TableOffset == 0 || SourceOffset <= TableOffset || SourceOffset > Output->PropertiesLength)
        return STATUS_INVALID_PARAMETER;

    Count = (SourceOffset - TableOffset) / sizeof(USHORT);
    if (Count == 0 || Count > 64)
        return STATUS_INVALID_PARAMETER;

    Connection->Pins = ExAllocatePoolZero(NonPagedPoolNx, Count * sizeof(PIN_NUMBER), GCX_POOL_TAG);
    if (Connection->Pins == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    /* Firmware numbers pins across the controller, one connection stays in one bank */
    for (Index = 0; Index < Count; Index++)
    {
        if (!GcxPinToBank(Controller, GcxReadUshort(Descriptor, TableOffset + Index * sizeof(USHORT)),
                          &BankId, &Offset) ||
            (Index > 0 && BankId != Connection->BankId))
        {
            return STATUS_NOT_SUPPORTED;
        }

        Connection->BankId = BankId;
        Connection->Pins[Index] = Offset;
    }

    Connection->PinCount = Count;
    Connection->Restriction = (UCHAR)(GcxReadUshort(Descriptor, GCX_GPIO_IO_FLAGS) & 3);
    Connection->PullConfiguration = Descriptor[GCX_GPIO_PIN_CONFIG];
    Connection->DriveStrength = GcxReadUshort(Descriptor, GCX_GPIO_DRIVE);
    Connection->DebounceTimeout = GcxReadUshort(Descriptor, GCX_GPIO_DEBOUNCE);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID
NTAPI
GcxEvtDeviceFileCreate(
    WDFDEVICE Device,
    WDFREQUEST Request,
    WDFFILEOBJECT FileObject)
{
    PRH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER Output = NULL;
    PGCX_CONTROLLER Controller = GcxGetController(Device);
    PGCX_IO_CONNECTION Connection = GcxGetIoConnection(FileObject);
    GPIO_CONNECT_IO_PINS_PARAMETERS Parameters;
    WDF_REQUEST_PARAMETERS RequestParameters;
    PUNICODE_STRING Name;
    ACCESS_MASK Access;
    LARGE_INTEGER Id;
    NTSTATUS Status;

    PAGED_CODE();

    RtlZeroMemory(Connection, sizeof(*Connection));
    Connection->Controller = Controller;

    /* Opening the controller itself carries no connection */
    Name = WdfFileObjectGetFileName(FileObject);
    if (Name == NULL || Name->Length == 0)
    {
        WdfRequestComplete(Request, STATUS_SUCCESS);
        return;
    }

    Status = GcxParseConnectionName(Name, &Id);
    if (NT_SUCCESS(Status))
        Status = GcxQueryHubConnection(Id, &Output);
    if (NT_SUCCESS(Status))
        Status = GcxParseIoDescriptor(Controller, Output, Connection);
    if (!NT_SUCCESS(Status))
        goto Exit;

    /* An unrestricted connection is driven when the open asked to write */
    WDF_REQUEST_PARAMETERS_INIT(&RequestParameters);
    WdfRequestGetParameters(Request, &RequestParameters);
    Access = RequestParameters.Parameters.Create.SecurityContext ?
             RequestParameters.Parameters.Create.SecurityContext->DesiredAccess : 0;

    if (Connection->Restriction == GCX_IO_RESTRICT_INPUT)
        Connection->Mode = ConnectModeInput;
    else if (Connection->Restriction == GCX_IO_RESTRICT_OUTPUT)
        Connection->Mode = ConnectModeOutput;
    else
        Connection->Mode = (Access & (FILE_WRITE_DATA | GENERIC_WRITE)) ? ConnectModeOutput : ConnectModeInput;

    if (Controller->Registration.CLIENT_ConnectIoPins == NULL)
    {
        Status = STATUS_NOT_SUPPORTED;
        goto Exit;
    }

    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.BankId = Connection->BankId;
    Parameters.PinNumberTable = Connection->Pins;
    Parameters.PinCount = Connection->PinCount;
    Parameters.ConnectMode = Connection->Mode;
    Parameters.PullConfiguration = Connection->PullConfiguration;
    Parameters.DebounceTimeout = Connection->DebounceTimeout;
    Parameters.DriveStrength = Connection->DriveStrength;
    if (GcxReadUshort(Output->ConnectionProperties, GCX_GPIO_VENDOR_LENGTH) != 0)
    {
        Parameters.VendorData = Output->ConnectionProperties +
                                GcxReadUshort(Output->ConnectionProperties, GCX_GPIO_VENDOR);
        Parameters.VendorDataLength = GcxReadUshort(Output->ConnectionProperties, GCX_GPIO_VENDOR_LENGTH);
    }

    Status = Controller->Registration.CLIENT_ConnectIoPins(GcxClientContext(Controller), &Parameters);
    if (NT_SUCCESS(Status))
        Connection->Connected = TRUE;

Exit:
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("GpioClx: connection %wZ not opened, 0x%08lx\n", Name, Status);
        if (Connection->Pins != NULL)
        {
            ExFreePoolWithTag(Connection->Pins, GCX_POOL_TAG);
            Connection->Pins = NULL;
        }
    }

    if (Output != NULL)
        ExFreePoolWithTag(Output, GCX_POOL_TAG);

    WdfRequestComplete(Request, Status);
}

_Use_decl_annotations_
VOID
NTAPI
GcxEvtFileClose(
    WDFFILEOBJECT FileObject)
{
    PGCX_IO_CONNECTION Connection = GcxGetIoConnection(FileObject);

    PAGED_CODE();

    if (Connection->Connected)
    {
        GcxDisconnectIoPins(Connection->Controller,
                            Connection->BankId,
                            Connection->Pins,
                            Connection->PinCount,
                            Connection->Mode);
        Connection->Connected = FALSE;
    }

    if (Connection->Pins != NULL)
    {
        ExFreePoolWithTag(Connection->Pins, GCX_POOL_TAG);
        Connection->Pins = NULL;
    }
}

/**
 * @brief
 * Reads a connection's pins, bit n for pin table entry n.
 */
static
NTSTATUS
GcxReadConnection(
    _In_ PGCX_IO_CONNECTION Connection,
    _Out_ PULONG64 Values)
{
    PGCX_CONTROLLER Controller = Connection->Controller;
    GPIO_READ_PINS_MASK_PARAMETERS MaskParameters;
    ULONG64 BankValues = 0;
    NTSTATUS Status;
    USHORT Index;

    *Values = 0;

    if (!Controller->Information.Flags.FormatIoRequestsAsMasks)
    {
        GcxLockBank(Controller, Connection->BankId);
        Status = GcxReadGpioPins(Controller, Connection->BankId, Connection->Pins, Connection->PinCount, Values);
        GcxUnlockBank(Controller, Connection->BankId);
        return Status;
    }

    /* The client reports the whole bank, the pin table picks the bits */
    if (Controller->Registration.CLIENT_ReadGpioPinsUsingMask == NULL)
        return STATUS_NOT_SUPPORTED;

    RtlZeroMemory(&MaskParameters, sizeof(MaskParameters));
    MaskParameters.BankId = Connection->BankId;
    MaskParameters.PinValues = &BankValues;

    GcxLockBank(Controller, Connection->BankId);
    Status = Controller->Registration.CLIENT_ReadGpioPinsUsingMask(GcxClientContext(Controller), &MaskParameters);
    GcxUnlockBank(Controller, Connection->BankId);
    if (!NT_SUCCESS(Status))
        return Status;

    for (Index = 0; Index < Connection->PinCount; Index++)
    {
        if (BankValues & (1ULL << Connection->Pins[Index]))
            *Values |= 1ULL << Index;
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Drives a connection's pins, bit n for pin table entry n.
 */
static
NTSTATUS
GcxWriteConnection(
    _In_ PGCX_IO_CONNECTION Connection,
    _In_ ULONG64 Values)
{
    PGCX_CONTROLLER Controller = Connection->Controller;
    GPIO_WRITE_PINS_MASK_PARAMETERS MaskParameters;
    NTSTATUS Status;
    USHORT Index;

    if (!Controller->Information.Flags.FormatIoRequestsAsMasks)
    {
        GcxLockBank(Controller, Connection->BankId);
        Status = GcxWriteGpioPins(Controller, Connection->BankId, Connection->Pins, Connection->PinCount, &Values);
        GcxUnlockBank(Controller, Connection->BankId);
        return Status;
    }

    if (Controller->Registration.CLIENT_WriteGpioPinsUsingMask == NULL)
        return STATUS_NOT_SUPPORTED;

    RtlZeroMemory(&MaskParameters, sizeof(MaskParameters));
    MaskParameters.BankId = Connection->BankId;
    for (Index = 0; Index < Connection->PinCount; Index++)
    {
        if (Values & (1ULL << Index))
            MaskParameters.SetMask |= 1ULL << Connection->Pins[Index];
        else
            MaskParameters.ClearMask |= 1ULL << Connection->Pins[Index];
    }

    GcxLockBank(Controller, Connection->BankId);
    Status = Controller->Registration.CLIENT_WriteGpioPinsUsingMask(GcxClientContext(Controller), &MaskParameters);
    GcxUnlockBank(Controller, Connection->BankId);
    return Status;
}

_Use_decl_annotations_
VOID
NTAPI
GcxEvtIoDeviceControl(
    WDFQUEUE Queue,
    WDFREQUEST Request,
    size_t OutputBufferLength,
    size_t InputBufferLength,
    ULONG IoControlCode)
{
    GPIO_CLIENT_CONTROLLER_SPECIFIC_FUNCTION_PARAMETERS Specific;
    PGCX_CONTROLLER Controller = GcxGetController(WdfIoQueueGetDevice(Queue));
    PGCX_IO_CONNECTION Connection = GcxGetIoConnection(WdfRequestGetFileObject(Request));
    PVOID InBuffer = NULL, OutBuffer = NULL;
    ULONG_PTR Information = 0;
    ULONG64 Values = 0;
    ULONG Bytes;
    NTSTATUS Status;

    switch (IoControlCode)
    {
        case IOCTL_GPIO_READ_PINS:
            if (!Connection->Connected)
            {
                Status = STATUS_INVALID_DEVICE_REQUEST;
                break;
            }

            Bytes = (Connection->PinCount + 7) / 8;
            Status = WdfRequestRetrieveOutputBuffer(Request, Bytes, &OutBuffer, NULL);
            if (!NT_SUCCESS(Status))
                break;

            Status = GcxReadConnection(Connection, &Values);
            if (NT_SUCCESS(Status))
            {
                RtlCopyMemory(OutBuffer, &Values, Bytes);
                Information = Bytes;
            }
            break;

        case IOCTL_GPIO_WRITE_PINS:
            if (!Connection->Connected || Connection->Mode != ConnectModeOutput)
            {
                Status = STATUS_INVALID_DEVICE_REQUEST;
                break;
            }

            Bytes = (Connection->PinCount + 7) / 8;
            Status = WdfRequestRetrieveInputBuffer(Request, Bytes, &InBuffer, NULL);
            if (!NT_SUCCESS(Status))
                break;

            RtlCopyMemory(&Values, InBuffer, Bytes);
            Status = GcxWriteConnection(Connection, Values);
            if (NT_SUCCESS(Status))
                Information = Bytes;
            break;

        case IOCTL_GPIO_CONTROLLER_SPECIFIC_FUNCTION:
            if (Controller->Registration.CLIENT_ControllerSpecificFunction == NULL)
            {
                Status = STATUS_NOT_SUPPORTED;
                break;
            }

            if (InputBufferLength)
                WdfRequestRetrieveInputBuffer(Request, 1, &InBuffer, NULL);
            if (OutputBufferLength)
                WdfRequestRetrieveOutputBuffer(Request, 1, &OutBuffer, NULL);

            RtlZeroMemory(&Specific, sizeof(Specific));
            Specific.InputBuffer = InBuffer;
            Specific.InputBufferLength = InputBufferLength;
            Specific.OutputBuffer = OutBuffer;
            Specific.OutputBufferLength = OutputBufferLength;
            Status = Controller->Registration.CLIENT_ControllerSpecificFunction(GcxClientContext(Controller),
                                                                               &Specific);
            if (NT_SUCCESS(Status))
                Information = Specific.BytesWritten;
            break;

        default:
            Status = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }

    WdfRequestCompleteWithInformation(Request, Status, Information);
}
