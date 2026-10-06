/*
 * PROJECT:     ReactOS GPIO framework extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Recovering a pin from the vector its interrupt was given
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * A GPIO interrupt is the one connection class that does not reach its consumer
 * as a connection id. ACPI mints a synthetic vector for the pin and the device
 * ends up holding an ordinary interrupt resource, so when the HAL later asks
 * this driver to arm "the pin behind vector N", the vector is all there is to
 * go on.
 *
 * Both halves of the answer come from the resource hub, which still has the
 * firmware descriptor keyed by that vector: the descriptor names the controller
 * by its ACPI path and gives the pin number within it. Matching the path to one
 * of this driver's controllers needs each controller's own path, which ACPI
 * hands over on request.
 */

#define NDEBUG
#include "gpioclxp.h"

#include <reshub.h>
#include <acpiioct.h>

/* GLOBALS *******************************************************************/

/*
 * ACPI serves this behind control code 9. It is not in the public
 * acpiioct.h, which stops at IOCTL_ACPI_GET_DEVICE_SPECIFIC_DATA.
 */
#ifndef IOCTL_ACPI_QUERY_DEVICE_BIOS_NAME
#define IOCTL_ACPI_QUERY_DEVICE_BIOS_NAME \
    CTL_CODE(FILE_DEVICE_ACPI, 9, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#endif

/* Where the reference starts sizing the name, doubling until it fits (:21747) */
#define GCX_BIOS_NAME_INITIAL_LENGTH 128
#define GCX_BIOS_NAME_MAXIMUM_LENGTH 0x8000

/* Its first guess at a descriptor, and how it grows (:27380) */
#define GCX_HUB_DESCRIPTOR_INITIAL_LENGTH 95

static PDEVICE_OBJECT GcxResourceHub = NULL;
static PFILE_OBJECT GcxResourceHubFile = NULL;

/* FUNCTIONS *****************************************************************/

/**
 * @brief
 * Sends one buffered control request and waits for it.
 *
 * @param[in] DeviceObject
 * The device to send it to.
 *
 * @param[in] FileObject
 * The file object it was opened by, or NULL for a device reached directly.
 *
 * @param[in] IoControlCode
 * The control code.
 *
 * @param[in] InputBuffer
 * The input buffer, or NULL.
 *
 * @param[in] InputLength
 * Its length.
 *
 * @param[out] OutputBuffer
 * The output buffer.
 *
 * @param[in] OutputLength
 * Its length.
 *
 * @param[out] Information
 * Receives how much of the output was written.
 *
 * @return
 * The request's status.
 */
static
NTSTATUS
GcxSendIoctl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PFILE_OBJECT FileObject,
    _In_ ULONG IoControlCode,
    _In_opt_ PVOID InputBuffer,
    _In_ ULONG InputLength,
    _Out_ PVOID OutputBuffer,
    _In_ ULONG OutputLength,
    _Out_ PULONG_PTR Information)
{
    IO_STATUS_BLOCK IoStatusBlock;
    KEVENT Event;
    PIRP Irp;
    NTSTATUS Status;

    PAGED_CODE();

    *Information = 0;

    KeInitializeEvent(&Event, SynchronizationEvent, FALSE);

    Irp = IoBuildDeviceIoControlRequest(IoControlCode,
                                        DeviceObject,
                                        InputBuffer,
                                        InputLength,
                                        OutputBuffer,
                                        OutputLength,
                                        FALSE,
                                        &Event,
                                        &IoStatusBlock);
    if (Irp == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (FileObject != NULL)
    {
        IoGetNextIrpStackLocation(Irp)->FileObject = FileObject;
    }

    Status = IoCallDriver(DeviceObject, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = IoStatusBlock.Status;
    }

    *Information = IoStatusBlock.Information;

    return Status;
}

/**
 * @brief
 * Asks ACPI what this controller is called in the firmware.
 *
 * That name is what a Connection() descriptor elsewhere in the namespace refers
 * to the controller by, so it is the only thing that ties a pin described on
 * some other device back to this one.
 *
 * @param[in] Device
 * The controller device.
 *
 * @param[out] BiosName
 * Receives the name. The caller frees the buffer.
 *
 * @return
 * STATUS_SUCCESS, or the failure from ACPI.
 */
NTSTATUS
GcxQueryDeviceBiosName(
    _In_ WDFDEVICE Device,
    _Out_ PUNICODE_STRING BiosName)
{
    PDEVICE_OBJECT Lower;
    ULONG_PTR Information;
    NTSTATUS Status;
    USHORT Length;
    PVOID Buffer;

    PAGED_CODE();

    RtlZeroMemory(BiosName, sizeof(*BiosName));

    Lower = WdfDeviceWdmGetAttachedDevice(Device);
    if (Lower == NULL)
    {
        return STATUS_NO_SUCH_DEVICE;
    }

    /*
     * ACPI answers STATUS_BUFFER_TOO_SMALL without saying how short the buffer
     * was, so the only way to size it is to keep doubling - which is what the
     * reference does from 128 bytes (:21747).
     */
    for (Length = GCX_BIOS_NAME_INITIAL_LENGTH;
         Length <= GCX_BIOS_NAME_MAXIMUM_LENGTH;
         Length = (USHORT)(Length * 2))
    {
        Buffer = ExAllocatePoolZero(NonPagedPool, Length, GCX_POOL_TAG);
        if (Buffer == NULL)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        Status = GcxSendIoctl(Lower,
                              NULL,
                              IOCTL_ACPI_QUERY_DEVICE_BIOS_NAME,
                              NULL,
                              0,
                              Buffer,
                              Length,
                              &Information);
        if (Status != STATUS_BUFFER_TOO_SMALL)
        {
            if (NT_SUCCESS(Status))
            {
                /* The reported length counts the terminator; the string does not */
                BiosName->Buffer = Buffer;
                BiosName->MaximumLength = Length;
                BiosName->Length = (USHORT)(Information - sizeof(WCHAR));
                return STATUS_SUCCESS;
            }

            ExFreePoolWithTag(Buffer, GCX_POOL_TAG);
            return Status;
        }

        ExFreePoolWithTag(Buffer, GCX_POOL_TAG);

        if (Length == GCX_BIOS_NAME_MAXIMUM_LENGTH)
        {
            break;
        }
    }

    return STATUS_NAME_TOO_LONG;
}

/**
 * @brief
 * Opens the resource hub, loading it if nothing has needed it yet.
 *
 * @return
 * STATUS_SUCCESS, or the reason the hub could not be reached.
 */
static
NTSTATUS
GcxOpenResourceHub(
    VOID)
{
    DECLARE_CONST_UNICODE_STRING(HubDeviceName, RESOURCE_HUB_DEVICE_NAME);
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatusBlock;
    HANDLE HubHandle = NULL;
    NTSTATUS Status;

    PAGED_CODE();

    if (GcxResourceHub != NULL)
    {
        return STATUS_SUCCESS;
    }

    InitializeObjectAttributes(&ObjectAttributes,
                               (PUNICODE_STRING)&HubDeviceName,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                               NULL,
                               NULL);

    Status = ZwOpenFile(&HubHandle,
                        GENERIC_READ | GENERIC_WRITE,
                        &ObjectAttributes,
                        &IoStatusBlock,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        FILE_NON_DIRECTORY_FILE);
    if (!NT_SUCCESS(Status))
    {
        /*
         * ACPI loads the hub when it first meets a Connection() descriptor, and
         * a GPIO interrupt is one, so by the time a pin is being armed the hub
         * is up. Not finding it means no descriptor was ever translated.
         */
        DPRINT1("GpioClx: cannot open %wZ: 0x%08lX\n", &HubDeviceName, Status);
        return Status;
    }

    Status = ObReferenceObjectByHandle(HubHandle,
                                       GENERIC_READ | GENERIC_WRITE,
                                       *IoFileObjectType,
                                       KernelMode,
                                       (PVOID *)&GcxResourceHubFile,
                                       NULL);
    ZwClose(HubHandle);

    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    GcxResourceHub = IoGetAttachedDeviceReference(GcxResourceHubFile->DeviceObject);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Recovers the firmware descriptor behind a synthetic interrupt vector.
 *
 * @param[in] Gsiv
 * The vector.
 *
 * @param[out] Output
 * Receives the hub's answer. The caller frees it.
 *
 * @return
 * STATUS_SUCCESS, or the hub's failure.
 */
static
NTSTATUS
GcxQueryResourceHubBiosDescriptor(
    _In_ ULONG Gsiv,
    _Outptr_ PRH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER *Output)
{
    RH_QUERY_CONNECTION_PROPERTIES_INPUT_BUFFER Input;
    PRH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER Buffer;
    ULONG_PTR Information;
    ULONG Length;
    NTSTATUS Status;

    PAGED_CODE();

    *Output = NULL;

    Status = GcxOpenResourceHub();
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    RtlZeroMemory(&Input, sizeof(Input));
    Input.Version = RH_QUERY_CONNECTION_PROPERTIES_INPUT_VERSION;
    Input.QueryType = InterruptVectorType;
    Input.u.InterruptVector = Gsiv;

    /*
     * The hub reports the descriptor's length in the header even when it could
     * not fit the body, so one retry is always enough.
     */
    Length = GCX_HUB_DESCRIPTOR_INITIAL_LENGTH;
    for (;;)
    {
        Buffer = ExAllocatePoolZero(NonPagedPool, Length, GCX_POOL_TAG);
        if (Buffer == NULL)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        Status = GcxSendIoctl(GcxResourceHub,
                              GcxResourceHubFile,
                              IOCTL_RH_QUERY_CONNECTION_PROPERTIES,
                              &Input,
                              sizeof(Input),
                              Buffer,
                              Length,
                              &Information);
        if (Status != STATUS_BUFFER_TOO_SMALL)
        {
            break;
        }

        Length = Buffer->PropertiesLength +
                 FIELD_OFFSET(RH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER,
                              ConnectionProperties);
        ExFreePoolWithTag(Buffer, GCX_POOL_TAG);

        if (Length <= GCX_HUB_DESCRIPTOR_INITIAL_LENGTH)
        {
            /* It did not ask for more, so retrying would loop */
            return STATUS_INVALID_BUFFER_SIZE;
        }
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
 * Pulls the controller's firmware name out of a GpioInt descriptor.
 *
 * The name is an ANSI string sitting between the pin table and the vendor data,
 * which is how the reference finds it: the offsets at +17 and +19 bound it
 * (GpioUtilGetResourceName).
 *
 * @param[in] Output
 * The hub's answer.
 *
 * @param[out] Name
 * Receives the name. The caller frees the buffer.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_INVALID_PARAMETER for a descriptor that is not one.
 */
static
NTSTATUS
GcxGetResourceName(
    _In_ PRH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER Output,
    _Out_ PUNICODE_STRING Name)
{
    PPNP_GPIO_INTERRUPT_IO_DESCRIPTOR Descriptor;
    ANSI_STRING Ansi;
    USHORT Start;
    USHORT End;

    PAGED_CODE();

    RtlZeroMemory(Name, sizeof(*Name));

    if (Output->PropertiesLength < sizeof(PNP_GPIO_INTERRUPT_IO_DESCRIPTOR))
    {
        return STATUS_INVALID_PARAMETER;
    }

    Descriptor = (PPNP_GPIO_INTERRUPT_IO_DESCRIPTOR)Output->ConnectionProperties;
    if (Descriptor->Tag != GPIO_INTERRUPT_IO_DESCRIPTOR)
    {
        return STATUS_NOT_SUPPORTED;
    }

    Start = Descriptor->ResourceSourceOffset;
    End = Descriptor->VendorDataOffset;

    if ((Start == 0) || (End <= Start) || (End > Output->PropertiesLength))
    {
        return STATUS_INVALID_PARAMETER;
    }

    Ansi.Buffer = (PCHAR)Descriptor + Start;
    Ansi.MaximumLength = (USHORT)(End - Start);

    /* The string is padded to the vendor data rather than sized, so measure it */
    for (Ansi.Length = 0;
         Ansi.Length < Ansi.MaximumLength && Ansi.Buffer[Ansi.Length] != '\0';
         Ansi.Length++)
    {
        NOTHING;
    }

    return RtlAnsiStringToUnicodeString(Name, &Ansi, TRUE);
}

/**
 * @brief
 * The pin a GpioInt descriptor names.
 *
 * Exactly one is allowed: a vector stands for a single line, so a multi-pin
 * descriptor has no single pin to be.
 *
 * @param[in] Output
 * The hub's answer.
 *
 * @param[out] PinNumber
 * Receives the pin, as a number across the whole controller.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_INVALID_PARAMETER.
 */
static
NTSTATUS
GcxGetDescriptorPin(
    _In_ PRH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER Output,
    _Out_ PULONG PinNumber)
{
    PPNP_GPIO_INTERRUPT_IO_DESCRIPTOR Descriptor;
    PUSHORT PinTable;
    USHORT Start;

    PAGED_CODE();

    *PinNumber = 0;

    if (Output->PropertiesLength < sizeof(PNP_GPIO_INTERRUPT_IO_DESCRIPTOR))
    {
        return STATUS_INVALID_PARAMETER;
    }

    Descriptor = (PPNP_GPIO_INTERRUPT_IO_DESCRIPTOR)Output->ConnectionProperties;

    Start = Descriptor->PinTableOffset;
    if ((Start == 0) ||
        (Start + sizeof(USHORT) > Output->PropertiesLength) ||
        (Descriptor->ResourceSourceOffset <= Start) ||
        ((Descriptor->ResourceSourceOffset - Start) != sizeof(USHORT)))
    {
        return STATUS_INVALID_PARAMETER;
    }

    PinTable = (PUSHORT)((PCHAR)Descriptor + Start);
    if (*PinTable == INVALID_PIN_NUMBER)
    {
        return STATUS_INVALID_PARAMETER;
    }

    *PinNumber = *PinTable;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Works out which of this driver's pins a vector stands for.
 *
 * @param[in] Gsiv
 * The vector.
 *
 * @param[out] Controller
 * Receives the controller the pin belongs to.
 *
 * @param[out] BankId
 * Receives the bank.
 *
 * @param[out] PinNumber
 * Receives the pin within that bank.
 *
 * @return
 * STATUS_SUCCESS, or the reason the vector could not be resolved.
 */
NTSTATUS
GcxResolveGsivToPin(
    _In_ ULONG Gsiv,
    _Outptr_ PGCX_CONTROLLER *Controller,
    _Out_ PBANK_ID BankId,
    _Out_ PPIN_NUMBER PinNumber)
{
    PRH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER Output = NULL;
    PGCX_CONTROLLER Found;
    UNICODE_STRING Name;
    ULONG AbsolutePin;
    NTSTATUS Status;

    PAGED_CODE();

    *Controller = NULL;

    Status = GcxQueryResourceHubBiosDescriptor(Gsiv, &Output);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    Status = GcxGetResourceName(Output, &Name);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Output, GCX_POOL_TAG);
        return Status;
    }

    Found = GcxFindControllerByBiosName(&Name);
    if (Found == NULL)
    {
        DPRINT1("GpioClx: GSIV %lu names %wZ, which is not one of ours\n",
                Gsiv, &Name);
        RtlFreeUnicodeString(&Name);
        ExFreePoolWithTag(Output, GCX_POOL_TAG);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    RtlFreeUnicodeString(&Name);

    Status = GcxGetDescriptorPin(Output, &AbsolutePin);
    ExFreePoolWithTag(Output, GCX_POOL_TAG);

    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    /* The firmware numbers pins across the controller; GpioClx numbers by bank */
    if (!GcxPinToBank(Found, AbsolutePin, BankId, PinNumber))
    {
        DPRINT1("GpioClx: GSIV %lu names pin %lu, past the end of the controller\n",
                Gsiv, AbsolutePin);
        return STATUS_INVALID_PARAMETER;
    }

    *Controller = Found;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Lets go of the resource hub.
 */
VOID
GcxCloseResourceHub(
    VOID)
{
    if (GcxResourceHub != NULL)
    {
        ObDereferenceObject(GcxResourceHub);
        GcxResourceHub = NULL;
    }

    if (GcxResourceHubFile != NULL)
    {
        ObDereferenceObject(GcxResourceHubFile);
        GcxResourceHubFile = NULL;
    }
}

/* EOF */
