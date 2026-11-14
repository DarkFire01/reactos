/*
 * PROJECT:     ReactOS GPIO framework extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Recovering a pin from the vector its interrupt was given
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * A GpioInt reaches its consumer as an ordinary interrupt vector. The resource
 * hub still holds the firmware descriptor for that vector, which names the
 * controller by ACPI path and gives the pin number within it.
 */

#define NDEBUG
#include "gpioclxp.h"

#include <reshub.h>

/* ACPI control code 9, absent from the public acpiioct.h */
#define GCX_IOCTL_ACPI_BIOS_NAME \
    CTL_CODE(FILE_DEVICE_ACPI, 9, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

#define GCX_BIOS_NAME_FIRST_GUESS 128
#define GCX_BIOS_NAME_LIMIT       0x8000

#define GCX_HUB_FIRST_GUESS 95

static PDEVICE_OBJECT GcxHubDevice;
static PFILE_OBJECT GcxHubFile;

/**
 * @brief
 * Sends one buffered control request and waits for it.
 *
 * @param[out] Information
 * Receives how much of the output was written.
 *
 * @return
 * The request's status.
 */
static
NTSTATUS
NTAPI
GcxSendIoctl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PFILE_OBJECT FileObject,
    _In_ ULONG IoControlCode,
    _In_reads_bytes_opt_(InputLength) PVOID InputBuffer,
    _In_ ULONG InputLength,
    _Out_writes_bytes_(OutputLength) PVOID OutputBuffer,
    _In_ ULONG OutputLength,
    _Out_ PULONG_PTR Information)
{
    IO_STATUS_BLOCK IoStatus;
    NTSTATUS Status;
    KEVENT Done;
    PIRP Irp;

    PAGED_CODE();

    *Information = 0;
    KeInitializeEvent(&Done, NotificationEvent, FALSE);

    Irp = IoBuildDeviceIoControlRequest(IoControlCode,
                                        DeviceObject,
                                        InputBuffer,
                                        InputLength,
                                        OutputBuffer,
                                        OutputLength,
                                        FALSE,
                                        &Done,
                                        &IoStatus);
    if (Irp == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    IoGetNextIrpStackLocation(Irp)->FileObject = FileObject;

    Status = IoCallDriver(DeviceObject, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Done, Executive, KernelMode, FALSE, NULL);
        Status = IoStatus.Status;
    }

    *Information = IoStatus.Information;
    return Status;
}

/**
 * @brief
 * Asks ACPI for the namespace path of a controller.
 *
 * @param[out] BiosName
 * Receives the name. The buffer is GCX_POOL_TAG pool the caller frees.
 *
 * @return
 * STATUS_SUCCESS, or the failure from ACPI.
 */
_Use_decl_annotations_
NTSTATUS
NTAPI
GcxQueryDeviceBiosName(
    WDFDEVICE Device,
    PUNICODE_STRING BiosName)
{
    ULONG_PTR Information;
    PDEVICE_OBJECT Target;
    NTSTATUS Status;
    PWCHAR Buffer;
    ULONG Length;

    PAGED_CODE();

    RtlZeroMemory(BiosName, sizeof(*BiosName));

    Target = WdfDeviceWdmGetAttachedDevice(Device);
    if (Target == NULL)
        return STATUS_NO_SUCH_DEVICE;

    /* ACPI does not report the size it needs, so grow until it fits */
    Length = GCX_BIOS_NAME_FIRST_GUESS;
    do
    {
        Buffer = ExAllocatePoolZero(NonPagedPoolNx, Length, GCX_POOL_TAG);
        if (Buffer == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;

        Status = GcxSendIoctl(Target,
                              NULL,
                              GCX_IOCTL_ACPI_BIOS_NAME,
                              NULL,
                              0,
                              Buffer,
                              Length,
                              &Information);
        if (NT_SUCCESS(Status) && Information >= sizeof(WCHAR))
        {
            /* The returned length includes the terminator */
            BiosName->Buffer = Buffer;
            BiosName->MaximumLength = (USHORT)Length;
            BiosName->Length = (USHORT)(Information - sizeof(WCHAR));
            return STATUS_SUCCESS;
        }

        ExFreePoolWithTag(Buffer, GCX_POOL_TAG);
        if (NT_SUCCESS(Status))
            return STATUS_INVALID_DEVICE_REQUEST;

        Length *= 2;
    } while (Status == STATUS_BUFFER_TOO_SMALL && Length <= GCX_BIOS_NAME_LIMIT);

    return (Status == STATUS_BUFFER_TOO_SMALL) ? STATUS_NAME_TOO_LONG : Status;
}

/**
 * @brief
 * Opens the resource hub once and keeps it referenced.
 */
static
NTSTATUS
NTAPI
GcxOpenResourceHub(VOID)
{
    UNICODE_STRING HubName = RTL_CONSTANT_STRING(RESOURCE_HUB_DEVICE_NAME);
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatus;
    HANDLE Handle;
    NTSTATUS Status;

    PAGED_CODE();

    if (GcxHubDevice != NULL)
        return STATUS_SUCCESS;

    InitializeObjectAttributes(&ObjectAttributes,
                               &HubName,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                               NULL,
                               NULL);

    Status = ZwOpenFile(&Handle,
                        GENERIC_READ | GENERIC_WRITE,
                        &ObjectAttributes,
                        &IoStatus,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        FILE_NON_DIRECTORY_FILE);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("GpioClx: cannot open %wZ 0x%08lx\n", &HubName, Status);
        return Status;
    }

    Status = ObReferenceObjectByHandle(Handle,
                                       GENERIC_READ | GENERIC_WRITE,
                                       *IoFileObjectType,
                                       KernelMode,
                                       (PVOID *)&GcxHubFile,
                                       NULL);
    ZwClose(Handle);
    if (!NT_SUCCESS(Status))
        return Status;

    GcxHubDevice = IoGetAttachedDeviceReference(GcxHubFile->DeviceObject);
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Fetches the firmware descriptor behind an interrupt vector.
 *
 * @param[out] Output
 * Receives the hub's answer in GCX_POOL_TAG pool the caller frees.
 */
static
NTSTATUS
NTAPI
GcxQueryHubDescriptor(
    _In_ ULONG Gsiv,
    _Outptr_ PRH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER *Output)
{
    RH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER Header;
    PRH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER Buffer;
    RH_QUERY_CONNECTION_PROPERTIES_INPUT_BUFFER Input;
    ULONG_PTR Information;
    ULONG Length;
    NTSTATUS Status;

    PAGED_CODE();

    *Output = NULL;

    Status = GcxOpenResourceHub();
    if (!NT_SUCCESS(Status))
        return Status;

    RtlZeroMemory(&Input, sizeof(Input));
    Input.Version = RH_QUERY_CONNECTION_PROPERTIES_INPUT_VERSION;
    Input.QueryType = InterruptVectorType;
    Input.u.InterruptVector = Gsiv;

    Length = GCX_HUB_FIRST_GUESS;
    Buffer = ExAllocatePoolZero(NonPagedPoolNx, Length, GCX_POOL_TAG);
    if (Buffer == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = GcxSendIoctl(GcxHubDevice,
                          GcxHubFile,
                          IOCTL_RH_QUERY_CONNECTION_PROPERTIES,
                          &Input,
                          sizeof(Input),
                          Buffer,
                          Length,
                          &Information);
    if (Status == STATUS_BUFFER_TOO_SMALL)
    {
        /* The header carries the full length even when the body did not fit */
        Header = *Buffer;
        ExFreePoolWithTag(Buffer, GCX_POOL_TAG);

        Length = FIELD_OFFSET(RH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER, ConnectionProperties) +
                 Header.PropertiesLength;
        if (Length <= GCX_HUB_FIRST_GUESS)
            return STATUS_INVALID_BUFFER_SIZE;

        Buffer = ExAllocatePoolZero(NonPagedPoolNx, Length, GCX_POOL_TAG);
        if (Buffer == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;

        Status = GcxSendIoctl(GcxHubDevice,
                              GcxHubFile,
                              IOCTL_RH_QUERY_CONNECTION_PROPERTIES,
                              &Input,
                              sizeof(Input),
                              Buffer,
                              Length,
                              &Information);
    }

    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Buffer, GCX_POOL_TAG);
        return Status;
    }

    *Output = Buffer;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * The GpioInt descriptor inside a hub answer, or NULL if it is not one.
 */
static
PPNP_GPIO_INTERRUPT_IO_DESCRIPTOR
NTAPI
GcxGpioDescriptor(
    _In_ PRH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER Output)
{
    PPNP_GPIO_INTERRUPT_IO_DESCRIPTOR Descriptor;

    if (Output->PropertiesLength < sizeof(*Descriptor))
        return NULL;

    Descriptor = (PPNP_GPIO_INTERRUPT_IO_DESCRIPTOR)Output->ConnectionProperties;
    if (Descriptor->Tag != GPIO_INTERRUPT_IO_DESCRIPTOR)
        return NULL;

    return Descriptor;
}

/**
 * @brief
 * Extracts the controller path a GpioInt descriptor names.
 *
 * @param[out] Name
 * Receives the name, allocated by RtlAnsiStringToUnicodeString.
 */
static
NTSTATUS
NTAPI
GcxDescriptorController(
    _In_ PRH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER Output,
    _Out_ PUNICODE_STRING Name)
{
    PPNP_GPIO_INTERRUPT_IO_DESCRIPTOR Descriptor;
    ANSI_STRING Source;
    USHORT First;
    USHORT Last;

    RtlZeroMemory(Name, sizeof(*Name));

    Descriptor = GcxGpioDescriptor(Output);
    if (Descriptor == NULL)
        return STATUS_NOT_SUPPORTED;

    /* The string runs from its offset up to the vendor data */
    First = Descriptor->ResourceSourceOffset;
    Last = Descriptor->VendorDataOffset;
    if (First == 0 || Last <= First || Last > Output->PropertiesLength)
        return STATUS_INVALID_PARAMETER;

    Source.Buffer = (PCHAR)Descriptor + First;
    Source.MaximumLength = (USHORT)(Last - First);
    Source.Length = (USHORT)strnlen(Source.Buffer, Source.MaximumLength);

    return RtlAnsiStringToUnicodeString(Name, &Source, TRUE);
}

/**
 * @brief
 * Extracts the single pin a GpioInt descriptor names.
 */
static
NTSTATUS
NTAPI
GcxDescriptorPin(
    _In_ PRH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER Output,
    _Out_ PULONG PinNumber)
{
    PPNP_GPIO_INTERRUPT_IO_DESCRIPTOR Descriptor;
    USHORT TableOffset;
    USHORT Pin;

    *PinNumber = 0;

    Descriptor = GcxGpioDescriptor(Output);
    if (Descriptor == NULL)
        return STATUS_INVALID_PARAMETER;

    /* An interrupt vector stands for one line, so the table must hold exactly one pin */
    TableOffset = Descriptor->PinTableOffset;
    if (TableOffset == 0 ||
        (ULONG)TableOffset + sizeof(USHORT) > Output->PropertiesLength ||
        Descriptor->ResourceSourceOffset != TableOffset + sizeof(USHORT))
    {
        return STATUS_INVALID_PARAMETER;
    }

    RtlCopyMemory(&Pin, (PUCHAR)Descriptor + TableOffset, sizeof(Pin));
    if (Pin == INVALID_PIN_NUMBER)
        return STATUS_INVALID_PARAMETER;

    *PinNumber = Pin;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Works out which controller pin an interrupt vector stands for.
 *
 * @return
 * STATUS_SUCCESS, or the reason the vector could not be resolved.
 */
_Use_decl_annotations_
NTSTATUS
NTAPI
GcxResolveGsivToPin(
    ULONG Gsiv,
    PGCX_CONTROLLER *Controller,
    PBANK_ID BankId,
    PPIN_NUMBER PinNumber)
{
    PRH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER Output;
    PGCX_CONTROLLER Owner = NULL;
    UNICODE_STRING Name;
    ULONG ControllerPin;
    NTSTATUS Status;

    PAGED_CODE();

    *Controller = NULL;

    Status = GcxQueryHubDescriptor(Gsiv, &Output);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = GcxDescriptorPin(Output, &ControllerPin);
    if (NT_SUCCESS(Status))
        Status = GcxDescriptorController(Output, &Name);

    ExFreePoolWithTag(Output, GCX_POOL_TAG);
    if (!NT_SUCCESS(Status))
        return Status;

    Owner = GcxFindControllerByBiosName(&Name);
    if (Owner == NULL)
    {
        DPRINT1("GpioClx: GSIV %lu belongs to %wZ, not to us\n", Gsiv, &Name);
        RtlFreeUnicodeString(&Name);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    RtlFreeUnicodeString(&Name);

    /* Firmware numbers pins across the controller, the client by bank */
    if (!GcxPinToBank(Owner, ControllerPin, BankId, PinNumber))
    {
        DPRINT1("GpioClx: GSIV %lu names pin %lu, out of range\n", Gsiv, ControllerPin);
        return STATUS_INVALID_PARAMETER;
    }

    *Controller = Owner;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID
NTAPI
GcxCloseResourceHub(VOID)
{
    if (GcxHubDevice != NULL)
    {
        ObDereferenceObject(GcxHubDevice);
        GcxHubDevice = NULL;
    }

    if (GcxHubFile != NULL)
    {
        ObDereferenceObject(GcxHubFile);
        GcxHubFile = NULL;
    }
}
