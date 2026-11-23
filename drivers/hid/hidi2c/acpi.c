/*
 * PROJECT:     ReactOS HID Stack
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     HID over I2C _DSM, which names the HID descriptor register
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "hidi2c.h"
#include <acpiioct.h>
#include <initguid.h>

#define NDEBUG
#include <debug.h>

/* {3CDFF6F7-4267-4555-AD05-B30A3D8938DE}, from the HID over I2C specification */
DEFINE_GUID(HIDI2C_DSM_GUID,
            0x3CDFF6F7, 0x4267, 0x4555, 0xAD, 0x05, 0xB3, 0x0A, 0x3D, 0x89, 0x38, 0xDE);

#define HIDI2C_DSM_REVISION          1
#define HIDI2C_DSM_QUERY             0
#define HIDI2C_DSM_DESCRIPTOR        1

/* Header, the UUID buffer and three integer sized arguments */
#define HIDI2C_DSM_INPUT_SIZE 64

typedef union _HIDI2C_DSM_INPUT
{
    ACPI_EVAL_INPUT_BUFFER_COMPLEX Header;
    UCHAR Raw[HIDI2C_DSM_INPUT_SIZE];
} HIDI2C_DSM_INPUT, *PHIDI2C_DSM_INPUT;

/**
 * @brief
 * Evaluates one function of the HID over I2C _DSM on our own ACPI device.
 */
static
NTSTATUS
NTAPI
HidI2cEvaluateDsm(
    _In_ PHIDI2C_DEVICE Context,
    _In_ ULONG Function,
    _Out_ PACPI_EVAL_OUTPUT_BUFFER Output)
{
    WDF_MEMORY_DESCRIPTOR InputMemory;
    WDF_MEMORY_DESCRIPTOR OutputMemory;
    PACPI_METHOD_ARGUMENT Argument;
    PHIDI2C_DSM_INPUT Input;
    NTSTATUS Status;

    PAGED_CODE();

    Input = ExAllocatePoolZero(PagedPool, sizeof(*Input), HIDI2C_POOL_TAG);
    if (Input == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Input->Header.Signature = ACPI_EVAL_INPUT_BUFFER_COMPLEX_SIGNATURE;
    Input->Header.MethodNameAsUlong = (ULONG)'MSD_';
    Input->Header.Size = sizeof(*Input);
    Input->Header.ArgumentCount = 4;

    Argument = Input->Header.Argument;
    ACPI_METHOD_SET_ARGUMENT_BUFFER(Argument, &HIDI2C_DSM_GUID, sizeof(GUID));

    Argument = ACPI_METHOD_NEXT_ARGUMENT(Argument);
    ACPI_METHOD_SET_ARGUMENT_INTEGER(Argument, HIDI2C_DSM_REVISION);

    Argument = ACPI_METHOD_NEXT_ARGUMENT(Argument);
    ACPI_METHOD_SET_ARGUMENT_INTEGER(Argument, Function);

    /* Neither function takes data, but the fourth argument is an empty package */
    Argument = ACPI_METHOD_NEXT_ARGUMENT(Argument);
    ACPI_METHOD_SET_ARGUMENT_INTEGER(Argument, 0);
    Argument->Type = ACPI_METHOD_ARGUMENT_PACKAGE;

    RtlZeroMemory(Output, sizeof(*Output));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&InputMemory, Input, sizeof(*Input));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&OutputMemory, Output, sizeof(*Output));

    Status = WdfIoTargetSendIoctlSynchronously(WdfDeviceGetIoTarget(Context->Device),
                                               NULL,
                                               IOCTL_ACPI_EVAL_METHOD,
                                               &InputMemory,
                                               &OutputMemory,
                                               NULL,
                                               NULL);

    ExFreePoolWithTag(Input, HIDI2C_POOL_TAG);
    return Status;
}

/**
 * @brief
 * Finds the HID descriptor register: function 0 must list function 1, which returns it.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
HidI2cQueryDescriptorRegister(
    _Inout_ PHIDI2C_DEVICE Context)
{
    ACPI_EVAL_OUTPUT_BUFFER Output;
    PACPI_METHOD_ARGUMENT Result = Output.Argument;
    NTSTATUS Status;

    PAGED_CODE();

    Status = HidI2cEvaluateDsm(Context, HIDI2C_DSM_QUERY, &Output);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("hidi2c: _DSM query failed 0x%08lx\n", Status);
        return Status;
    }

    if (Result->Type != ACPI_METHOD_ARGUMENT_BUFFER || Result->DataLength == 0)
        return STATUS_INVALID_PARAMETER;

    if ((Result->Data[0] & (1 << HIDI2C_DSM_DESCRIPTOR)) == 0)
    {
        DPRINT1("hidi2c: _DSM lacks the descriptor function\n");
        return STATUS_NOT_SUPPORTED;
    }

    Status = HidI2cEvaluateDsm(Context, HIDI2C_DSM_DESCRIPTOR, &Output);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Result->Type != ACPI_METHOD_ARGUMENT_INTEGER || Result->DataLength == 0)
        return STATUS_UNSUCCESSFUL;

    Context->DescriptorRegister = (USHORT)Result->Argument;
    DPRINT("hidi2c: HID descriptor at register 0x%04X\n", Context->DescriptorRegister);
    return STATUS_SUCCESS;
}
