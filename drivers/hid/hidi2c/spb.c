/*
 * PROJECT:     ReactOS HID Stack
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     HID over I2C resources and bus transactions through SpbCx
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "hidi2c.h"
#include "hidi2c_msg.h"

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

/**
 * @brief
 * Finds the I2C connection and the interrupt, opens the connection and creates
 * the interrupt object with the read request the ISR reuses.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
HidI2cSpbInitialize(
    _Inout_ PHIDI2C_DEVICE Context,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    WCHAR PathBuffer[RESOURCE_HUB_PATH_CHARS];
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Resource;
    WDF_IO_TARGET_OPEN_PARAMS OpenParams;
    WDF_INTERRUPT_CONFIG InterruptConfig;
    WDF_OBJECT_ATTRIBUTES Attributes;
    BOOLEAN HaveConnection = FALSE;
    BOOLEAN HaveInterrupt = FALSE;
    ULONG InterruptIndex = 0;
    WDFWAITLOCK InterruptLock;
    UNICODE_STRING Path;
    ULONG Count;
    ULONG Index;
    NTSTATUS Status;

    PAGED_CODE();

    Count = WdfCmResourceListGetCount(ResourcesTranslated);
    for (Index = 0; Index < Count && !(HaveConnection && HaveInterrupt); Index++)
    {
        Resource = WdfCmResourceListGetDescriptor(ResourcesTranslated, Index);

        if (Resource->Type == CmResourceTypeInterrupt)
        {
            InterruptIndex = Index;
            HaveInterrupt = TRUE;
        }
        else if (Resource->Type == CmResourceTypeConnection &&
                 Resource->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL &&
                 Resource->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C)
        {
            Context->ConnectionId.LowPart = Resource->u.Connection.IdLowPart;
            Context->ConnectionId.HighPart = Resource->u.Connection.IdHighPart;
            HaveConnection = TRUE;
        }
    }

    if (!HaveConnection || !HaveInterrupt)
    {
        DPRINT1("hidi2c: connection %u, interrupt %u\n", HaveConnection, HaveInterrupt);
        HidI2cReportProblem(Context, HIDI2C_PROBLEM_RESOURCES);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = WdfIoTargetCreate(Context->Device, WDF_NO_OBJECT_ATTRIBUTES, &Context->Target);
    if (!NT_SUCCESS(Status))
    {
        Context->Target = NULL;
        return Status;
    }

    RtlInitEmptyUnicodeString(&Path, PathBuffer, sizeof(PathBuffer));
    Status = RESOURCE_HUB_CREATE_PATH_FROM_ID(&Path,
                                              Context->ConnectionId.LowPart,
                                              Context->ConnectionId.HighPart);
    if (!NT_SUCCESS(Status))
        return Status;

    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&OpenParams, &Path, GENERIC_READ | GENERIC_WRITE);
    Status = WdfIoTargetOpen(Context->Target, &OpenParams);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("hidi2c: cannot open %wZ 0x%08lx\n", &Path, Status);
        return Status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Context->Target;
    Status = WdfRequestCreate(&Attributes, Context->Target, &Context->InputRequest);
    if (!NT_SUCCESS(Status))
    {
        Context->InputRequest = NULL;
        return Status;
    }

    /* The ISR talks to the bus, so it runs at passive level under this lock */
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Context->Device;
    Status = WdfWaitLockCreate(&Attributes, &InterruptLock);
    if (!NT_SUCCESS(Status))
        return Status;

    WDF_INTERRUPT_CONFIG_INIT(&InterruptConfig, HidI2cEvtInterruptIsr, HidI2cEvtInterruptDpc);
    InterruptConfig.InterruptRaw = WdfCmResourceListGetDescriptor(ResourcesRaw, InterruptIndex);
    InterruptConfig.InterruptTranslated = WdfCmResourceListGetDescriptor(ResourcesTranslated,
                                                                         InterruptIndex);
    InterruptConfig.WaitLock = InterruptLock;
    InterruptConfig.PassiveHandling = TRUE;

    Status = WdfInterruptCreate(Context->Device,
                                &InterruptConfig,
                                WDF_NO_OBJECT_ATTRIBUTES,
                                &Context->Interrupt);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("hidi2c: WdfInterruptCreate failed 0x%08lx\n", Status);
        return Status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Context->Device;
    return WdfSpinLockCreate(&Attributes, &Context->CompletionLock);
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID
NTAPI
HidI2cSpbRelease(
    _Inout_ PHIDI2C_DEVICE Context)
{
    PAGED_CODE();

    if (Context->InputRequest != NULL)
    {
        WdfObjectDelete(Context->InputRequest);
        Context->InputRequest = NULL;
    }

    if (Context->Target != NULL)
    {
        WdfObjectDelete(Context->Target);
        Context->Target = NULL;
    }
}

/**
 * @brief
 * Sends a prepared transfer list to the controller.
 *
 * @param[out] Transferred
 * Receives the bytes all transfers of the sequence moved.
 */
static
NTSTATUS
NTAPI
HidI2cExecuteSequence(
    _In_ PHIDI2C_DEVICE Context,
    _In_reads_bytes_(Size) PSPB_TRANSFER_LIST List,
    _In_ ULONG Size,
    _In_ ULONG TimeoutSeconds,
    _Out_ PULONG Transferred)
{
    WDF_REQUEST_SEND_OPTIONS Options;
    WDF_MEMORY_DESCRIPTOR Memory;
    ULONG_PTR Bytes = 0;
    NTSTATUS Status;

    HidI2cSetTimeout(&Options, TimeoutSeconds);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Memory, List, Size);

    Status = WdfIoTargetSendIoctlSynchronously(Context->Target,
                                               NULL,
                                               IOCTL_SPB_EXECUTE_SEQUENCE,
                                               &Memory,
                                               NULL,
                                               &Options,
                                               &Bytes);

    *Transferred = (ULONG)Bytes;
    return Status;
}

/**
 * @brief
 * Reads Length bytes from a register: the address goes out, the data comes back
 * after a repeated start.
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
    SPB_TRANSFER_LIST_AND_ENTRIES(2) Sequence;
    USHORT Address = Register;
    ULONG Transferred;
    NTSTATUS Status;

    PAGED_CODE();

    if (Buffer == NULL || Length == 0)
        return STATUS_INVALID_PARAMETER;

    SPB_TRANSFER_LIST_INIT(&Sequence.List, 2);
    Sequence.List.Transfers[0] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionToDevice,
                                                                      0,
                                                                      &Address,
                                                                      sizeof(Address));
    Sequence.List.Transfers[1] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionFromDevice,
                                                                      0,
                                                                      Buffer,
                                                                      Length);

    Status = HidI2cExecuteSequence(Context,
                                   &Sequence.List,
                                   sizeof(Sequence),
                                   TimeoutSeconds,
                                   &Transferred);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("hidi2c: read of register 0x%04X failed 0x%08lx\n", Register, Status);
        return Status;
    }

    /* At least the address has to have gone out */
    if (Transferred < sizeof(Address))
        return STATUS_DEVICE_PROTOCOL_ERROR;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Writes a register address followed by data, as one plain write.
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
    ULONG_PTR Written = 0;
    PUCHAR Packet;
    ULONG Size;
    NTSTATUS Status;

    PAGED_CODE();

    if (Buffer == NULL || Length == 0)
        return STATUS_INVALID_PARAMETER;

    Size = HIDI2C_REGISTER_BYTES + Length;
    Packet = ExAllocatePoolZero(NonPagedPoolNx, Size, HIDI2C_POOL_TAG);
    if (Packet == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Packet[0] = (UCHAR)(Register & 0xFF);
    Packet[1] = (UCHAR)(Register >> 8);
    RtlCopyMemory(Packet + HIDI2C_REGISTER_BYTES, Buffer, Length);

    HidI2cSetTimeout(&Options, TimeoutSeconds);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Memory, Packet, Size);

    Status = WdfIoTargetSendWriteSynchronously(Context->Target,
                                               NULL,
                                               &Memory,
                                               NULL,
                                               &Options,
                                               &Written);
    if (!NT_SUCCESS(Status))
        DPRINT1("hidi2c: write to register 0x%04X failed 0x%08lx\n", Register, Status);
    else if (Written != Size)
        Status = STATUS_DEVICE_PROTOCOL_ERROR;

    ExFreePoolWithTag(Packet, HIDI2C_POOL_TAG);
    return Status;
}

/**
 * @brief
 * Sends command register, command and data register, then reads the answer
 * from the data register after a repeated start.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
HidI2cCommandRead(
    _In_ PHIDI2C_DEVICE Context,
    _In_reads_bytes_(CommandLength) PUCHAR Command,
    _In_ USHORT CommandLength,
    _Out_writes_bytes_(Length) PUCHAR Buffer,
    _In_ USHORT Length)
{
    SPB_TRANSFER_LIST_AND_ENTRIES(2) Sequence;
    SPB_TRANSFER_BUFFER_LIST_ENTRY Pieces[3];
    USHORT CommandRegister = Context->Descriptor.CommandRegister;
    USHORT DataRegister = Context->Descriptor.DataRegister;
    ULONG Transferred;
    NTSTATUS Status;

    PAGED_CODE();

    if (Command == NULL || CommandLength == 0 || Buffer == NULL || Length == 0)
        return STATUS_INVALID_PARAMETER;

    Pieces[0].Buffer = &CommandRegister;
    Pieces[0].BufferCb = sizeof(CommandRegister);
    Pieces[1].Buffer = Command;
    Pieces[1].BufferCb = CommandLength;
    Pieces[2].Buffer = &DataRegister;
    Pieces[2].BufferCb = sizeof(DataRegister);

    SPB_TRANSFER_LIST_INIT(&Sequence.List, 2);
    Sequence.List.Transfers[0] =
        SPB_TRANSFER_LIST_ENTRY_INIT_BUFFER_LIST(SpbTransferDirectionToDevice,
                                                 0,
                                                 Pieces,
                                                 RTL_NUMBER_OF(Pieces));
    Sequence.List.Transfers[1] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionFromDevice,
                                                                      0,
                                                                      Buffer,
                                                                      Length);

    Status = HidI2cExecuteSequence(Context,
                                   &Sequence.List,
                                   sizeof(Sequence),
                                   HIDI2C_DEFAULT_TIMEOUT,
                                   &Transferred);
    if (!NT_SUCCESS(Status))
        return Status;

    /* The whole command has to have gone out before the answer means anything */
    if (Transferred < CommandLength + 2 * HIDI2C_REGISTER_BYTES)
        return STATUS_DEVICE_PROTOCOL_ERROR;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Sends command register, command, data register and data as one transfer.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
HidI2cCommandWrite(
    _In_ PHIDI2C_DEVICE Context,
    _In_reads_bytes_(CommandLength) PUCHAR Command,
    _In_ USHORT CommandLength,
    _In_reads_bytes_(Length) PUCHAR Data,
    _In_ USHORT Length)
{
    SPB_TRANSFER_LIST_AND_ENTRIES(1) Sequence;
    SPB_TRANSFER_BUFFER_LIST_ENTRY Pieces[4];
    USHORT CommandRegister = Context->Descriptor.CommandRegister;
    USHORT DataRegister = Context->Descriptor.DataRegister;
    ULONG Transferred;
    NTSTATUS Status;

    PAGED_CODE();

    if (Command == NULL || CommandLength == 0 || Data == NULL || Length == 0)
        return STATUS_INVALID_PARAMETER;

    Pieces[0].Buffer = &CommandRegister;
    Pieces[0].BufferCb = sizeof(CommandRegister);
    Pieces[1].Buffer = Command;
    Pieces[1].BufferCb = CommandLength;
    Pieces[2].Buffer = &DataRegister;
    Pieces[2].BufferCb = sizeof(DataRegister);
    Pieces[3].Buffer = Data;
    Pieces[3].BufferCb = Length;

    SPB_TRANSFER_LIST_INIT(&Sequence.List, 1);
    Sequence.List.Transfers[0] =
        SPB_TRANSFER_LIST_ENTRY_INIT_BUFFER_LIST(SpbTransferDirectionToDevice,
                                                 0,
                                                 Pieces,
                                                 RTL_NUMBER_OF(Pieces));

    Status = HidI2cExecuteSequence(Context,
                                   &Sequence.List,
                                   sizeof(Sequence),
                                   HIDI2C_DEFAULT_TIMEOUT,
                                   &Transferred);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Transferred != (ULONG)CommandLength + Length + 2 * HIDI2C_REGISTER_BYTES)
        return STATUS_DEVICE_PROTOCOL_ERROR;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Reads from the device without addressing a register first, which is how
 * input reports and the reset answer arrive.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
HidI2cReadInput(
    _In_ PHIDI2C_DEVICE Context,
    _Out_writes_bytes_(Length) PUCHAR Buffer,
    _In_ ULONG Length)
{
    WDF_REQUEST_REUSE_PARAMS Reuse;
    WDF_REQUEST_SEND_OPTIONS Options;
    WDF_MEMORY_DESCRIPTOR Memory;

    PAGED_CODE();

    if (Buffer == NULL || Length == 0)
        return STATUS_INVALID_PARAMETER;

    WDF_REQUEST_REUSE_PARAMS_INIT(&Reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_NOT_SUPPORTED);
    WdfRequestReuse(Context->InputRequest, &Reuse);

    HidI2cSetTimeout(&Options, HIDI2C_DEFAULT_TIMEOUT);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Memory, Buffer, Length);

    return WdfIoTargetSendReadSynchronously(Context->Target,
                                            Context->InputRequest,
                                            &Memory,
                                            NULL,
                                            &Options,
                                            NULL);
}
