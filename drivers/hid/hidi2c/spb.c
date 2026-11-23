/*
 * PROJECT:     ReactOS HID Stack
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     HID over I2C bus transactions through SpbCx
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "hidi2c.h"

#define NDEBUG
#include <debug.h>

/* A register address on the wire, little endian */
#define HIDI2C_REGISTER_BYTES sizeof(USHORT)

static
VOID
NTAPI
HidI2cSetTimeout(
    _Out_ PWDF_REQUEST_SEND_OPTIONS Options,
    _In_ ULONG TimeoutSeconds)
{
    WDF_REQUEST_SEND_OPTIONS_INIT(Options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    Options->Timeout = WDF_REL_TIMEOUT_IN_SEC(TimeoutSeconds);
}

static
VOID
NTAPI
HidI2cPutRegister(
    _Out_writes_bytes_(HIDI2C_REGISTER_BYTES) PUCHAR Bytes,
    _In_ USHORT Register)
{
    Bytes[0] = (UCHAR)(Register & 0xFF);
    Bytes[1] = (UCHAR)(Register >> 8);
}

/**
 * @brief
 * Opens the resource hub path of our I2C connection.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
HidI2cOpenTarget(
    _Inout_ PHIDI2C_DEVICE Context)
{
    WCHAR PathBuffer[RESOURCE_HUB_PATH_CHARS];
    WDF_IO_TARGET_OPEN_PARAMS OpenParams;
    WDF_OBJECT_ATTRIBUTES Attributes;
    UNICODE_STRING Path;
    NTSTATUS Status;

    PAGED_CODE();

    RtlInitEmptyUnicodeString(&Path, PathBuffer, sizeof(PathBuffer));
    Status = RESOURCE_HUB_CREATE_PATH_FROM_ID(&Path,
                                              Context->ConnectionId.LowPart,
                                              Context->ConnectionId.HighPart);
    if (!NT_SUCCESS(Status))
        return Status;

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Context->Device;

    Status = WdfIoTargetCreate(Context->Device, &Attributes, &Context->Target);
    if (!NT_SUCCESS(Status))
        return Status;

    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&OpenParams, &Path, GENERIC_READ | GENERIC_WRITE);

    Status = WdfIoTargetOpen(Context->Target, &OpenParams);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("hidi2c: cannot open %wZ 0x%08lx\n", &Path, Status);
        WdfObjectDelete(Context->Target);
        Context->Target = NULL;
    }

    return Status;
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID
NTAPI
HidI2cCloseTarget(
    _Inout_ PHIDI2C_DEVICE Context)
{
    PAGED_CODE();

    if (Context->InputRequest != NULL)
    {
        WdfObjectDelete(Context->InputRequest);
        Context->InputRequest = NULL;
    }

    if (Context->Target == NULL)
        return;

    WdfIoTargetClose(Context->Target);
    WdfObjectDelete(Context->Target);
    Context->Target = NULL;
}

/**
 * @brief
 * Runs a write followed by a read with a repeated start between them.
 *
 * @param[out] Transferred
 * Receives the bytes both transfers moved.
 */
static
NTSTATUS
NTAPI
HidI2cWriteThenRead(
    _In_ PHIDI2C_DEVICE Context,
    _In_reads_bytes_(WriteLength) PVOID WriteBuffer,
    _In_ ULONG WriteLength,
    _Out_writes_bytes_(ReadLength) PVOID ReadBuffer,
    _In_ ULONG ReadLength,
    _In_ ULONG TimeoutSeconds,
    _Out_ PULONG_PTR Transferred)
{
    SPB_TRANSFER_LIST_AND_ENTRIES(2) Sequence;
    WDF_REQUEST_SEND_OPTIONS Options;
    WDF_MEMORY_DESCRIPTOR Memory;

    *Transferred = 0;

    SPB_TRANSFER_LIST_INIT(&Sequence.List, 2);
    Sequence.List.Transfers[0] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionToDevice,
                                                                      0,
                                                                      WriteBuffer,
                                                                      WriteLength);
    Sequence.List.Transfers[1] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionFromDevice,
                                                                      0,
                                                                      ReadBuffer,
                                                                      ReadLength);

    HidI2cSetTimeout(&Options, TimeoutSeconds);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Memory, &Sequence, sizeof(Sequence));

    return WdfIoTargetSendIoctlSynchronously(Context->Target,
                                             NULL,
                                             IOCTL_SPB_EXECUTE_SEQUENCE,
                                             &Memory,
                                             NULL,
                                             &Options,
                                             Transferred);
}

/**
 * @brief
 * Reads Length bytes from a register: the address goes out, the data comes back.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
HidI2cReadRegister(
    _In_ PHIDI2C_DEVICE Context,
    _In_ USHORT Register,
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length,
    _In_ ULONG TimeoutSeconds)
{
    UCHAR Address[HIDI2C_REGISTER_BYTES];
    ULONG_PTR Transferred;
    NTSTATUS Status;

    PAGED_CODE();

    HidI2cPutRegister(Address, Register);

    Status = HidI2cWriteThenRead(Context,
                                 Address,
                                 sizeof(Address),
                                 Buffer,
                                 Length,
                                 TimeoutSeconds,
                                 &Transferred);
    if (!NT_SUCCESS(Status))
        DPRINT1("hidi2c: read of register 0x%04X failed 0x%08lx\n", Register, Status);

    return Status;
}

/**
 * @brief
 * Writes a register address followed by data, in one transfer.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
HidI2cWriteRegister(
    _In_ PHIDI2C_DEVICE Context,
    _In_ USHORT Register,
    _In_reads_bytes_(Length) const VOID *Buffer,
    _In_ ULONG Length,
    _In_ ULONG TimeoutSeconds)
{
    WDF_REQUEST_SEND_OPTIONS Options;
    WDF_MEMORY_DESCRIPTOR Memory;
    PUCHAR Packet;
    ULONG Size = HIDI2C_REGISTER_BYTES + Length;
    NTSTATUS Status;

    PAGED_CODE();

    Packet = ExAllocatePoolZero(NonPagedPoolNx, Size, HIDI2C_POOL_TAG);
    if (Packet == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    HidI2cPutRegister(Packet, Register);
    RtlCopyMemory(Packet + HIDI2C_REGISTER_BYTES, Buffer, Length);

    HidI2cSetTimeout(&Options, TimeoutSeconds);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Memory, Packet, Size);

    Status = WdfIoTargetSendWriteSynchronously(Context->Target, NULL, &Memory, NULL, &Options, NULL);
    if (!NT_SUCCESS(Status))
        DPRINT1("hidi2c: write to register 0x%04X failed 0x%08lx\n", Register, Status);

    ExFreePoolWithTag(Packet, HIDI2C_POOL_TAG);
    return Status;
}

/**
 * @brief
 * Builds command register, command bytes and data register as one write prefix.
 */
static
PUCHAR
NTAPI
HidI2cBuildCommand(
    _In_ PHIDI2C_DEVICE Context,
    _In_reads_bytes_(CommandLength) const UCHAR *Command,
    _In_ ULONG CommandLength,
    _In_ ULONG TrailerLength,
    _Out_ PULONG Size)
{
    PUCHAR Packet;

    *Size = 2 * HIDI2C_REGISTER_BYTES + CommandLength + TrailerLength;

    Packet = ExAllocatePoolZero(NonPagedPoolNx, *Size, HIDI2C_POOL_TAG);
    if (Packet == NULL)
        return NULL;

    HidI2cPutRegister(Packet, Context->Descriptor.CommandRegister);
    RtlCopyMemory(Packet + HIDI2C_REGISTER_BYTES, Command, CommandLength);
    HidI2cPutRegister(Packet + HIDI2C_REGISTER_BYTES + CommandLength, Context->Descriptor.DataRegister);
    return Packet;
}

/**
 * @brief
 * Sends a command and reads the answer back from the data register.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
HidI2cCommandRead(
    _In_ PHIDI2C_DEVICE Context,
    _In_reads_bytes_(CommandLength) const UCHAR *Command,
    _In_ ULONG CommandLength,
    _Out_writes_bytes_(Length) PUCHAR Buffer,
    _In_ ULONG Length)
{
    ULONG_PTR Transferred;
    PUCHAR Packet;
    ULONG Size;
    NTSTATUS Status;

    PAGED_CODE();

    if (Command == NULL || CommandLength == 0 || Buffer == NULL || Length == 0)
        return STATUS_INVALID_PARAMETER;

    Packet = HidI2cBuildCommand(Context, Command, CommandLength, 0, &Size);
    if (Packet == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = HidI2cWriteThenRead(Context,
                                 Packet,
                                 Size,
                                 Buffer,
                                 Length,
                                 HIDI2C_DEFAULT_TIMEOUT,
                                 &Transferred);

    /* The whole command has to have gone out before the answer means anything */
    if (NT_SUCCESS(Status) && Transferred < Size)
        Status = STATUS_DEVICE_PROTOCOL_ERROR;

    ExFreePoolWithTag(Packet, HIDI2C_POOL_TAG);
    return Status;
}

/**
 * @brief
 * Sends a command followed by data for the data register, in one transfer.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
HidI2cCommandWrite(
    _In_ PHIDI2C_DEVICE Context,
    _In_reads_bytes_(CommandLength) const UCHAR *Command,
    _In_ ULONG CommandLength,
    _In_reads_bytes_(Length) const UCHAR *Data,
    _In_ ULONG Length)
{
    WDF_REQUEST_SEND_OPTIONS Options;
    WDF_MEMORY_DESCRIPTOR Memory;
    ULONG_PTR Transferred = 0;
    PUCHAR Packet;
    ULONG Size;
    NTSTATUS Status;

    PAGED_CODE();

    if (Command == NULL || CommandLength == 0 || Data == NULL || Length == 0)
        return STATUS_INVALID_PARAMETER;

    Packet = HidI2cBuildCommand(Context, Command, CommandLength, Length, &Size);
    if (Packet == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlCopyMemory(Packet + Size - Length, Data, Length);

    HidI2cSetTimeout(&Options, HIDI2C_DEFAULT_TIMEOUT);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Memory, Packet, Size);

    Status = WdfIoTargetSendWriteSynchronously(Context->Target, NULL, &Memory, NULL, &Options, &Transferred);
    if (NT_SUCCESS(Status) && Transferred != Size)
        Status = STATUS_DEVICE_PROTOCOL_ERROR;

    ExFreePoolWithTag(Packet, HIDI2C_POOL_TAG);
    return Status;
}

/**
 * @brief
 * Reads one input report into InputBuffer without addressing a register first.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
HidI2cReadInput(
    _In_ PHIDI2C_DEVICE Context)
{
    WDF_REQUEST_REUSE_PARAMS Reuse;
    WDF_REQUEST_SEND_OPTIONS Options;
    WDF_MEMORY_DESCRIPTOR Memory;
    NTSTATUS Status;

    PAGED_CODE();

    if (Context->InputRequest != NULL)
    {
        WDF_REQUEST_REUSE_PARAMS_INIT(&Reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_NOT_SUPPORTED);
        Status = WdfRequestReuse(Context->InputRequest, &Reuse);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    HidI2cSetTimeout(&Options, HIDI2C_DEFAULT_TIMEOUT);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Memory, Context->InputBuffer, Context->Descriptor.MaxInputLength);

    return WdfIoTargetSendReadSynchronously(Context->Target,
                                            Context->InputRequest,
                                            &Memory,
                                            NULL,
                                            &Options,
                                            NULL);
}
