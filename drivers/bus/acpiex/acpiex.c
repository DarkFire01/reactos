/*
 * PROJECT:     ReactOS ACPI extension driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     The resource hub device
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * ACPI hands every GPIO and serial bus descriptor of a device to the hub, which
 * turns it into a secondary interrupt or a connection id and keeps the raw bytes.
 * Peripheral drivers open \Device\RESOURCE_HUB\<id> and the hub sends the open
 * on to the controller the descriptor names; controllers ask the hub for the
 * bytes behind an id or a GSIV.
 */

#include "acpiexp.h"

#define NDEBUG
#include <debug.h>

DRIVER_INITIALIZE DriverEntry;

static DRIVER_DISPATCH RhDispatchCreateClose;
static DRIVER_DISPATCH RhDispatchDeviceControl;

static PDEVICE_OBJECT RhDevice;

/* FUNCTIONS ******************************************************************/

/**
 * @brief Reads the connection id from the 16 hex digits that start a hub path.
 *
 * @return STATUS_OBJECT_NAME_INVALID unless the digits end the name or a backslash follows.
 */
static
NTSTATUS
RhParseConnectionId(
    _In_reads_bytes_(Length) PCWSTR Name,
    _In_ ULONG Length,
    _Out_ PLARGE_INTEGER Id)
{
    ULONG Digits = sizeof(LARGE_INTEGER) * 2;
    ULONGLONG Value = 0;
    ULONG Index;
    WCHAR Digit;

    if (Length < Digits * sizeof(WCHAR))
        return STATUS_OBJECT_NAME_INVALID;

    for (Index = 0; Index < Digits; Index++)
    {
        Digit = Name[Index];
        if (Digit >= L'0' && Digit <= L'9')
            Value = (Value << 4) | (ULONG)(Digit - L'0');
        else if (Digit >= L'a' && Digit <= L'f')
            Value = (Value << 4) | (ULONG)(Digit - L'a' + 10);
        else if (Digit >= L'A' && Digit <= L'F')
            Value = (Value << 4) | (ULONG)(Digit - L'A' + 10);
        else
            return STATUS_OBJECT_NAME_INVALID;
    }

    if (Length > Digits * sizeof(WCHAR) && Name[Digits] != L'\\')
        return STATUS_OBJECT_NAME_INVALID;

    Id->QuadPart = (LONGLONG)Value;
    return STATUS_SUCCESS;
}

/**
 * @brief Sends an open of a connection on to the controller that serves it.
 *
 * The id stays in the name, so the controller can ask the hub what was opened.
 */
static
NTSTATUS
RhReparseConnection(
    _Inout_ PFILE_OBJECT FileObject)
{
    UNICODE_STRING Target, NewName;
    LARGE_INTEGER Id;
    PCWSTR Relative;
    ULONG RelativeLength;
    NTSTATUS Status;

    Relative = FileObject->FileName.Buffer;
    RelativeLength = FileObject->FileName.Length;
    if (RelativeLength >= sizeof(WCHAR) && Relative[0] == L'\\')
    {
        Relative++;
        RelativeLength -= sizeof(WCHAR);
    }

    /* The name is not terminated, the parser stays inside its length */
    Status = RhParseConnectionId(Relative, RelativeLength, &Id);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = RhGetConnectionTarget(Id, &Target);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ACPIEX: connection %I64u has no target, 0x%lx\n", Id.QuadPart, Status);
        return Status;
    }

    /* <controller device>\<id>[\rest] */
    NewName.MaximumLength = Target.Length + sizeof(WCHAR) + (USHORT)RelativeLength;
    NewName.Length = 0;
    NewName.Buffer = ExAllocatePoolWithTag(PagedPool, NewName.MaximumLength, RH_POOL_TAG);
    if (!NewName.Buffer)
    {
        ExFreePoolWithTag(Target.Buffer, RH_POOL_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyUnicodeString(&NewName, &Target);
    RtlAppendUnicodeToString(&NewName, L"\\");
    RtlCopyMemory((PUCHAR)NewName.Buffer + NewName.Length,
                  Relative,
                  NewName.MaximumLength - NewName.Length);
    NewName.Length = NewName.MaximumLength;

    Status = IoReplaceFileObjectName(FileObject, NewName.Buffer, NewName.Length);

    ExFreePoolWithTag(NewName.Buffer, RH_POOL_TAG);
    ExFreePoolWithTag(Target.Buffer, RH_POOL_TAG);
    return NT_SUCCESS(Status) ? STATUS_REPARSE : Status;
}

static
NTSTATUS
NTAPI
RhDispatchCreateClose(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS Status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(DeviceObject);

    Irp->IoStatus.Information = 0;

    /* The hub itself opens with no name, a connection with its id */
    if (Stack->MajorFunction == IRP_MJ_CREATE && Stack->FileObject && Stack->FileObject->FileName.Length)
    {
        Status = RhReparseConnection(Stack->FileObject);
        if (Status == STATUS_REPARSE)
            Irp->IoStatus.Information = IO_REPARSE;
    }

    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static
NTSTATUS
NTAPI
RhDispatchDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    ULONG InputLength = Stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG OutputLength = Stack->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID Buffer = Irp->AssociatedIrp.SystemBuffer;
    ULONG_PTR Information = 0;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(DeviceObject);

    switch (Stack->Parameters.DeviceIoControl.IoControlCode)
    {
        case IOCTL_UACPINT_RH_QUERY_TRANSLATION:
            /* Only the ACPI driver in the kernel may bind the translation interface */
            if (Irp->RequestorMode != KernelMode)
            {
                Status = STATUS_ACCESS_DENIED;
                break;
            }
            Status = RhExchangeTranslationInterface(Buffer, InputLength, OutputLength, &Information);
            break;

        case IOCTL_RH_QUERY_CONNECTION_PROPERTIES:
            if (InputLength < sizeof(RH_QUERY_CONNECTION_PROPERTIES_INPUT_BUFFER))
            {
                Status = STATUS_INVALID_PARAMETER;
                break;
            }
            Status = RhQueryConnectionProperties(Buffer, Buffer, OutputLength, &Information);
            break;

        case IOCTL_RH_QUERY_ACTIVE_BOTH_INITIAL_POLARITY:
        {
            PRH_QUERY_ACTIVE_BOTH_INITIAL_POLARITY_INPUT_BUFFER Input = Buffer;
            PRH_QUERY_ACTIVE_BOTH_INITIAL_POLARITY_OUTPUT_BUFFER Output = Buffer;
            UCHAR Polarity = 0;

            if (InputLength < sizeof(*Input) || OutputLength < sizeof(*Output) ||
                Input->Version != RH_QUERY_ACTIVE_BOTH_INITIAL_POLARITY_INPUT_VERSION)
            {
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            Status = RhQueryInitialPolarity(Input->InterruptVector, &Polarity);
            if (NT_SUCCESS(Status))
            {
                Output->Version = RH_QUERY_ACTIVE_BOTH_INITIAL_POLARITY_OUTPUT_VERSION;
                Output->InitialPolarity = Polarity;
                Information = sizeof(*Output);
            }
            break;
        }

        case IOCTL_RH_UPDATE_ACTIVE_BOTH_INITIAL_POLARITY:
        {
            PRH_UPDATE_ACTIVE_BOTH_INITIAL_POLARITY_INPUT_BUFFER Input = Buffer;

            if (InputLength < sizeof(*Input))
            {
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            Status = RhUpdateInitialPolarity(Input->InterruptVector, Input->InitialPolarity);
            if (NT_SUCCESS(Status) && OutputLength >= sizeof(ULONG))
            {
                *(PULONG)Buffer = 1;
                Information = sizeof(ULONG);
            }
            break;
        }

        default:
            Status = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }

    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    UNICODE_STRING DeviceName = RTL_CONSTANT_STRING(RESOURCE_HUB_DEVICE_NAME);
    UNICODE_STRING LinkName = RTL_CONSTANT_STRING(RESOURCE_HUB_SYMBOLIC_NAME);
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(RegistryPath);

    RhInitializeTranslation();

    Status = IoCreateDevice(DriverObject,
                            0,
                            &DeviceName,
                            FILE_DEVICE_RESOURCE_HUB,
                            FILE_DEVICE_SECURE_OPEN,
                            FALSE,
                            &RhDevice);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ACPIEX: hub device creation failed 0x%lx\n", Status);
        return Status;
    }

    Status = IoCreateSymbolicLink(&LinkName, &DeviceName);
    if (!NT_SUCCESS(Status))
        DPRINT1("ACPIEX: hub link creation failed 0x%lx\n", Status);

    DriverObject->MajorFunction[IRP_MJ_CREATE] = RhDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP] = RhDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = RhDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = RhDispatchDeviceControl;

    RhDevice->Flags |= DO_BUFFERED_IO;
    RhDevice->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}
