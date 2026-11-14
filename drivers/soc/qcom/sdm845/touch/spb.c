/*
 * PROJECT:     ReactOS Qualcomm platform drivers
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SPB and GPIO connections opened through the resource hub
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "qcomtouch.h"

#include <debug.h>

#define QCTOUCH_IO_TIMEOUT_MS   500

NTSTATUS
NTAPI
QcTouchOpenConnection(
    _In_ PQCTOUCH_DEVICE Device,
    _In_ LARGE_INTEGER Id,
    _Out_ WDFIOTARGET *Target)
{
    DECLARE_UNICODE_STRING_SIZE(Path, RESOURCE_HUB_CONNECTION_PATH_CHARS);
    WDF_IO_TARGET_OPEN_PARAMS OpenParams;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDFIOTARGET NewTarget;
    NTSTATUS Status;

    *Target = NULL;

    Status = RESOURCE_HUB_CREATE_PATH_FROM_ID(&Path, Id.LowPart, Id.HighPart);
    if (!NT_SUCCESS(Status))
        return Status;

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Device->Device;

    Status = WdfIoTargetCreate(Device->Device, &Attributes, &NewTarget);
    if (!NT_SUCCESS(Status))
        return Status;

    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&OpenParams, &Path, GENERIC_READ | GENERIC_WRITE);
    Status = WdfIoTargetOpen(NewTarget, &OpenParams);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("open of %wZ failed, 0x%08lx\n", &Path, Status);
        WdfObjectDelete(NewTarget);
        return Status;
    }

    *Target = NewTarget;
    return STATUS_SUCCESS;
}

static
VOID
QcTouchSetTimeout(
    _Out_ PWDF_REQUEST_SEND_OPTIONS Options)
{
    WDF_REQUEST_SEND_OPTIONS_INIT(Options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(Options, WDF_REL_TIMEOUT_IN_MS(QCTOUCH_IO_TIMEOUT_MS));
}

NTSTATUS
NTAPI
QcTouchI2cWrite(
    _In_ PQCTOUCH_DEVICE Device,
    _In_reads_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length)
{
    WDF_MEMORY_DESCRIPTOR Memory;
    WDF_REQUEST_SEND_OPTIONS Options;
    ULONG_PTR Written = 0;
    NTSTATUS Status;

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Memory, Buffer, Length);
    QcTouchSetTimeout(&Options);

    Status = WdfIoTargetSendWriteSynchronously(Device->I2c, NULL, &Memory, NULL, &Options, &Written);
    if (NT_SUCCESS(Status) && Written != 0 && Written != Length)
        Status = STATUS_DEVICE_PROTOCOL_ERROR;

    return Status;
}

/**
 * @brief Writes the register address and reads back in one sequence, so no
 * other target can take the bus between the two halves.
 */
NTSTATUS
NTAPI
QcTouchI2cWriteRead(
    _In_ PQCTOUCH_DEVICE Device,
    _In_reads_bytes_(WriteLength) PVOID WriteBuffer,
    _In_ ULONG WriteLength,
    _Out_writes_bytes_(ReadLength) PVOID ReadBuffer,
    _In_ ULONG ReadLength)
{
    SPB_TRANSFER_LIST_AND_ENTRIES(2) Sequence;
    WDF_MEMORY_DESCRIPTOR Memory;
    WDF_REQUEST_SEND_OPTIONS Options;
    ULONG_PTR Transferred = 0;
    NTSTATUS Status;

    SPB_TRANSFER_LIST_INIT(&Sequence.List, 2);
    Sequence.List.Transfers[0] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionToDevice,
                                                                     0,
                                                                     WriteBuffer,
                                                                     WriteLength);
    Sequence.List.Transfers[1] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionFromDevice,
                                                                     0,
                                                                     ReadBuffer,
                                                                     ReadLength);

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Memory, &Sequence, sizeof(Sequence));
    QcTouchSetTimeout(&Options);

    Status = WdfIoTargetSendIoctlSynchronously(Device->I2c,
                                               NULL,
                                               IOCTL_SPB_EXECUTE_SEQUENCE,
                                               &Memory,
                                               NULL,
                                               &Options,
                                               &Transferred);
    /* Controllers differ on what they count, so only a short transfer fails */
    if (NT_SUCCESS(Status) && Transferred != 0 && Transferred < ReadLength)
        Status = STATUS_DEVICE_PROTOCOL_ERROR;

    return Status;
}

NTSTATUS
NTAPI
QcTouchGpioWrite(
    _In_ WDFIOTARGET Target,
    _In_ BOOLEAN High)
{
    WDF_MEMORY_DESCRIPTOR Memory;
    WDF_REQUEST_SEND_OPTIONS Options;
    UCHAR Value = High ? 1 : 0;

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Memory, &Value, sizeof(Value));
    QcTouchSetTimeout(&Options);

    return WdfIoTargetSendIoctlSynchronously(Target,
                                             NULL,
                                             IOCTL_GPIO_WRITE_PINS,
                                             &Memory,
                                             NULL,
                                             &Options,
                                             NULL);
}
