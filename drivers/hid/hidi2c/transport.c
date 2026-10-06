/*
 * PROJECT:     ReactOS HID Stack
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     HID over I2C - SPB transport and the ACPI _DSM
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */


#define NDEBUG
#include "hidi2cp.h"
#include <acpiioct.h>
#include <initguid.h>

/*
 * The _DSM this driver evaluates. Verified byte-for-byte against
 * the GUID in the reference binary's .rdata; it is the one the HID over I2C
 * specification assigns, and function 1 returns the HID descriptor's register
 * address.
 */
DEFINE_GUID(GUID_HIDI2C_DSM,
            0x3CDFF6F7, 0x4267, 0x4555, 0xAD, 0x05, 0xB3, 0x0A, 0x3D, 0x89, 0x38, 0xDE);

/*
 * The connection id is turned back into a name and opened. What answers is the
 * resource hub, which reparses to whichever controller owns the connection, so
 * this driver never learns, and never needs to learn, which I2C controller
 * it is actually talking to.
 */
NTSTATUS
HidI2cOpenSpbTarget(
    _In_ PHIDI2C_CONTEXT Context)
{
    WDF_IO_TARGET_OPEN_PARAMS OpenParams;
    WDF_OBJECT_ATTRIBUTES Attributes;
    UNICODE_STRING TargetPath;
    WCHAR PathBuffer[RESOURCE_HUB_PATH_CHARS];
    NTSTATUS Status;

    PAGED_CODE();

    RtlInitEmptyUnicodeString(&TargetPath, PathBuffer, sizeof(PathBuffer));

    Status = RESOURCE_HUB_CREATE_PATH_FROM_ID(&TargetPath,
                                              Context->ConnectionId.LowPart,
                                              Context->ConnectionId.HighPart);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Context->Device;

    Status = WdfIoTargetCreate(Context->Device, &Attributes, &Context->SpbTarget);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&OpenParams,
                                                &TargetPath,
                                                GENERIC_READ | GENERIC_WRITE);

    Status = WdfIoTargetOpen(Context->SpbTarget, &OpenParams);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("hidi2c: cannot open %wZ (0x%08lX)\n", &TargetPath, Status);
        WdfObjectDelete(Context->SpbTarget);
        Context->SpbTarget = NULL;
        return Status;
    }

    DPRINT("hidi2c: opened connection %I64x\n", Context->ConnectionId.QuadPart);
    return STATUS_SUCCESS;
}

VOID
HidI2cCloseSpbTarget(
    _In_ PHIDI2C_CONTEXT Context)
{
    if (Context->SpbTarget != NULL)
    {
        WdfIoTargetClose(Context->SpbTarget);
        WdfObjectDelete(Context->SpbTarget);
        Context->SpbTarget = NULL;
    }
}

/* A plain write: one transfer, no restart, stop at the end */
NTSTATUS
HidI2cSpbWrite(
    _In_ PHIDI2C_CONTEXT Context,
    _In_reads_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length)
{
    WDF_MEMORY_DESCRIPTOR Descriptor;

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Descriptor, Buffer, Length);

    return WdfIoTargetSendWriteSynchronously(Context->SpbTarget,
                                             NULL,
                                             &Descriptor,
                                             NULL,
                                             NULL,
                                             NULL);
}

/* A plain read, for a device that is already pointed at the right register */
NTSTATUS
HidI2cSpbRead(
    _In_ PHIDI2C_CONTEXT Context,
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length)
{
    WDF_MEMORY_DESCRIPTOR Descriptor;

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Descriptor, Buffer, Length);

    return WdfIoTargetSendReadSynchronously(Context->SpbTarget,
                                            NULL,
                                            &Descriptor,
                                            NULL,
                                            NULL,
                                            NULL);
}

/*
 * The shape every register access takes: write the register address, repeated
 * start, read the answer. It has to be one sequence rather than a write followed
 * by a read, because releasing the bus in between would let another peripheral
 * move the device's internal pointer.
 */
NTSTATUS
HidI2cSpbWriteRead(
    _In_ PHIDI2C_CONTEXT Context,
    _In_reads_bytes_(WriteLength) PVOID WriteBuffer,
    _In_ ULONG WriteLength,
    _Out_writes_bytes_(ReadLength) PVOID ReadBuffer,
    _In_ ULONG ReadLength)
{
    SPB_TRANSFER_LIST_AND_ENTRIES(2) Sequence;
    WDF_MEMORY_DESCRIPTOR Descriptor;
    ULONG_PTR BytesTransferred = 0;
    NTSTATUS Status;

    SPB_TRANSFER_LIST_INIT(&Sequence.List, 2);

    Sequence.List.Transfers[0] =
        SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionToDevice,
                                            0, WriteBuffer, WriteLength);
    Sequence.List.Transfers[1] =
        SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionFromDevice,
                                            0, ReadBuffer, ReadLength);

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Descriptor, &Sequence, sizeof(Sequence));

    Status = WdfIoTargetSendIoctlSynchronously(Context->SpbTarget,
                                               NULL,
                                               IOCTL_SPB_EXECUTE_SEQUENCE,
                                               &Descriptor,
                                               NULL,
                                               NULL,
                                               &BytesTransferred);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("hidi2c: write-read failed 0x%08lX\n", Status);
    }

    return Status;
}

/*
 * _DSM takes four arguments, always in this order: the UUID as a 16-byte buffer,
 * an integer revision, an integer function index, and a package of
 * function-specific arguments. The reference lays them out exactly like this and
 * sends the result with plain IOCTL_ACPI_EVAL_METHOD, not the _EX form, which
 * exists to name a method by path rather than by its four characters.
 *
 * The package is empty for both functions used here; _DSM still requires the
 * argument to be present.
 *
 * This works out to 60 bytes where the reference declares 64; it rounds up and
 * leaves four bytes of slack, which nothing reads. Size is what ACPI validates
 * the buffer against, so the exact figure is the safer one, but expect the
 * difference when diffing against AcpiPrepareInputParametersForDsm.
 */
#define HIDI2C_DSM_ARG_SIZE                                         \
    (FIELD_OFFSET(ACPI_EVAL_INPUT_BUFFER_COMPLEX, Argument) +       \
     (FIELD_OFFSET(ACPI_METHOD_ARGUMENT, Data) + sizeof(GUID)) +    \
     (FIELD_OFFSET(ACPI_METHOD_ARGUMENT, Data) + sizeof(ULONG)) * 3)

static
VOID
HidI2cBuildDsmInput(
    _Out_writes_bytes_(HIDI2C_DSM_ARG_SIZE) PACPI_EVAL_INPUT_BUFFER_COMPLEX Input,
    _In_ ULONG FunctionIndex)
{
    PACPI_METHOD_ARGUMENT Argument;

    RtlZeroMemory(Input, HIDI2C_DSM_ARG_SIZE);

    Input->Signature = ACPI_EVAL_INPUT_BUFFER_COMPLEX_SIGNATURE;
    RtlCopyMemory(Input->MethodName, "_DSM", 4);
    Input->Size = HIDI2C_DSM_ARG_SIZE;
    Input->ArgumentCount = 4;

    /* Arg0: the UUID, as a buffer */
    Argument = Input->Argument;
    Argument->Type = ACPI_METHOD_ARGUMENT_BUFFER;
    Argument->DataLength = sizeof(GUID);
    RtlCopyMemory(Argument->Data, &GUID_HIDI2C_DSM, sizeof(GUID));
    Argument = (PACPI_METHOD_ARGUMENT)((PUCHAR)Argument +
                   FIELD_OFFSET(ACPI_METHOD_ARGUMENT, Data) + sizeof(GUID));

    /* Arg1: revision */
    Argument->Type = ACPI_METHOD_ARGUMENT_INTEGER;
    Argument->DataLength = sizeof(ULONG);
    Argument->Argument = HIDI2C_DSM_REVISION;
    Argument = (PACPI_METHOD_ARGUMENT)((PUCHAR)Argument +
                   FIELD_OFFSET(ACPI_METHOD_ARGUMENT, Data) + sizeof(ULONG));

    /* Arg2: which function */
    Argument->Type = ACPI_METHOD_ARGUMENT_INTEGER;
    Argument->DataLength = sizeof(ULONG);
    Argument->Argument = FunctionIndex;
    Argument = (PACPI_METHOD_ARGUMENT)((PUCHAR)Argument +
                   FIELD_OFFSET(ACPI_METHOD_ARGUMENT, Data) + sizeof(ULONG));

    /* Arg3: the empty argument package */
    Argument->Type = ACPI_METHOD_ARGUMENT_PACKAGE;
    Argument->DataLength = sizeof(ULONG);
    Argument->Argument = 0;
}

static
NTSTATUS
HidI2cEvaluateDsm(
    _In_ PHIDI2C_CONTEXT Context,
    _In_ ULONG FunctionIndex,
    _Out_writes_bytes_(OutputLength) PACPI_EVAL_OUTPUT_BUFFER Output,
    _In_ ULONG OutputLength)
{
    UCHAR InputBuffer[HIDI2C_DSM_ARG_SIZE];
    WDF_MEMORY_DESCRIPTOR InputDescriptor;
    WDF_MEMORY_DESCRIPTOR OutputDescriptor;
    WDFIOTARGET AcpiTarget;

    PAGED_CODE();

    HidI2cBuildDsmInput((PACPI_EVAL_INPUT_BUFFER_COMPLEX)InputBuffer, FunctionIndex);

    RtlZeroMemory(Output, OutputLength);

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&InputDescriptor, InputBuffer, sizeof(InputBuffer));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&OutputDescriptor, Output, OutputLength);

    /*
     * Down our own stack: ACPI is the bus driver that enumerated this device, so
     * the default I/O target reaches it.
     */
    AcpiTarget = WdfDeviceGetIoTarget(Context->Device);

    return WdfIoTargetSendIoctlSynchronously(AcpiTarget,
                                             NULL,
                                             IOCTL_ACPI_EVAL_METHOD,
                                             &InputDescriptor,
                                             &OutputDescriptor,
                                             NULL,
                                             NULL);
}

/*
 * Function 0 of any _DSM returns a bitmap of the functions it implements, one
 * bit per index. Asking before calling is what the reference does, and it is the
 * difference between "this firmware has no HID descriptor for us" and a method
 * that faults halfway through.
 */
static
BOOLEAN
HidI2cDsmSupportsFunction(
    _In_ PHIDI2C_CONTEXT Context,
    _In_ ULONG FunctionIndex)
{
    UCHAR OutputBuffer[sizeof(ACPI_EVAL_OUTPUT_BUFFER) + 32];
    PACPI_EVAL_OUTPUT_BUFFER Output = (PACPI_EVAL_OUTPUT_BUFFER)OutputBuffer;
    PACPI_METHOD_ARGUMENT Argument;
    NTSTATUS Status;

    PAGED_CODE();

    Status = HidI2cEvaluateDsm(Context, 0, Output, sizeof(OutputBuffer));
    if (!NT_SUCCESS(Status) || Output->Count == 0)
    {
        return FALSE;
    }

    Argument = Output->Argument;

    /*
     * The bitmap comes back as a buffer, one bit per function index; a
     * single-byte buffer therefore describes functions 0 through 7, which covers
     * every index this specification defines.
     */
    if (Argument->Type != ACPI_METHOD_ARGUMENT_BUFFER || Argument->DataLength == 0)
    {
        return FALSE;
    }

    if ((FunctionIndex / 8) >= Argument->DataLength)
    {
        return FALSE;
    }

    return (Argument->Data[FunctionIndex / 8] & (1 << (FunctionIndex % 8))) != 0;
}

/*
 * AcpiInitialize, through AcpiGetDeviceMethod (:11623).
 *
 * The HID descriptor's address is the one thing about this device not
 * discoverable over the bus, so the firmware supplies it: _DSM function 1
 * returns it as an integer.
 */
NTSTATUS
HidI2cAcpiGetDescriptorRegister(
    _In_ PHIDI2C_CONTEXT Context)
{
    UCHAR OutputBuffer[sizeof(ACPI_EVAL_OUTPUT_BUFFER) + 32];
    PACPI_EVAL_OUTPUT_BUFFER Output = (PACPI_EVAL_OUTPUT_BUFFER)OutputBuffer;
    PACPI_METHOD_ARGUMENT Argument;
    NTSTATUS Status;

    PAGED_CODE();

    if (!HidI2cDsmSupportsFunction(Context, HIDI2C_DSM_FUNCTION_HID_DESC))
    {
        DPRINT1("hidi2c: _DSM does not implement function %u\n",
                HIDI2C_DSM_FUNCTION_HID_DESC);
        return STATUS_NOT_SUPPORTED;
    }

    Status = HidI2cEvaluateDsm(Context, HIDI2C_DSM_FUNCTION_HID_DESC,
                               Output, sizeof(OutputBuffer));
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("hidi2c: _DSM function %u failed 0x%08lX\n",
                HIDI2C_DSM_FUNCTION_HID_DESC, Status);
        return Status;
    }

    if (Output->Count == 0)
    {
        return STATUS_ACPI_INVALID_DATA;
    }

    Argument = Output->Argument;
    if (Argument->Type != ACPI_METHOD_ARGUMENT_INTEGER)
    {
        DPRINT1("hidi2c: _DSM returned type %u, expected an integer\n",
                Argument->Type);
        return STATUS_ACPI_INVALID_DATA;
    }

    Context->DescriptorRegister = (USHORT)Argument->Argument;

    DPRINT("hidi2c: HID descriptor register is 0x%04X\n", Context->DescriptorRegister);
    return STATUS_SUCCESS;
}
