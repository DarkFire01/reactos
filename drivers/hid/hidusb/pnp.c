/*
 * PROJECT:     ReactOS USB HID Miniport Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Plug and Play handling and device configuration
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "hidusb.h"

#define NDEBUG
#include <debug.h>

/* Size of the HID descriptor with one class descriptor, the only form accepted */
#define HIDUSB_HID_DESCRIPTOR_LENGTH    9

static IO_COMPLETION_ROUTINE HidUsbPnpCompletion;

/**
 * @brief
 * Sends SET_IDLE with a duration of zero, so the device only reports changes.
 */
NTSTATUS
NTAPI
HidUsbSetIdle(
    _In_ PDEVICE_OBJECT DeviceObject)
{
    struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST *Request;
    PHIDUSB_EXTENSION Extension;
    NTSTATUS Status;

    Extension = HidUsbGetExtension(DeviceObject);
    if ((Extension == NULL) || (Extension->InterfaceInfo == NULL))
        return STATUS_NOT_FOUND;

    Request = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*Request), HIDUSB_TAG);
    if (Request == NULL)
        return STATUS_NO_MEMORY;

    Request->Hdr.Length = sizeof(*Request);
    if (Extension->Flags & HIDUSB_FLAG_ENDPOINT_REQUESTS)
    {
        Request->Hdr.Function = URB_FUNCTION_CLASS_ENDPOINT;
        Request->Index = 0;
    }
    else
    {
        Request->Hdr.Function = URB_FUNCTION_CLASS_INTERFACE;
        Request->Index = Extension->InterfaceInfo->InterfaceNumber;
    }

    Request->RequestTypeReservedBits = HIDUSB_CLASS_REQUEST_BITS;
    Request->Request = HIDUSB_REQUEST_SET_IDLE;
    Request->Value = 0;
    Request->TransferFlags = 0;
    Request->TransferBufferLength = 0;
    Request->TransferBuffer = NULL;

    Status = HidUsbCallUsb(DeviceObject, (PURB)Request);
    ExFreePoolWithTag(Request, HIDUSB_TAG);
    return Status;
}

static
NTSTATUS
NTAPI
HidUsbGetDeviceDescriptor(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PHIDUSB_EXTENSION Extension)
{
    ULONG Length;

    Length = sizeof(USB_DEVICE_DESCRIPTOR);
    return HidUsbGetDescriptor(DeviceObject,
                               URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE,
                               USB_DEVICE_DESCRIPTOR_TYPE,
                               (PVOID *)&Extension->DeviceDescriptor,
                               &Length,
                               0,
                               0);
}

/**
 * @brief
 * Reads the whole first configuration descriptor.
 *
 * @param[out] ConfigDescriptor
 * Receives the descriptor, which the caller frees.
 *
 * @param[out] ConfigLength
 * Receives the number of bytes read.
 *
 * @return
 * STATUS_DEVICE_DATA_ERROR for a descriptor that is too short or reports no length.
 */
static
NTSTATUS
NTAPI
HidUsbGetConfigDescriptor(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Out_ PUSB_CONFIGURATION_DESCRIPTOR *ConfigDescriptor,
    _Out_ PULONG ConfigLength)
{
    PUSB_CONFIGURATION_DESCRIPTOR Descriptor;
    USHORT TotalLength;
    NTSTATUS Status;
    ULONG Length;

    *ConfigDescriptor = NULL;
    *ConfigLength = 0;

    /* The header first, to learn the total length */
    Descriptor = NULL;
    Length = sizeof(USB_CONFIGURATION_DESCRIPTOR);
    Status = HidUsbGetDescriptor(DeviceObject,
                                 URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE,
                                 USB_CONFIGURATION_DESCRIPTOR_TYPE,
                                 (PVOID *)&Descriptor,
                                 &Length,
                                 0,
                                 0);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Configuration header request failed 0x%lx\n", Status);
        return Status;
    }

    if (Length < sizeof(USB_CONFIGURATION_DESCRIPTOR))
    {
        DPRINT1("Configuration header too short, %lu bytes\n", Length);
        ExFreePoolWithTag(Descriptor, HIDUSB_TAG);
        return STATUS_DEVICE_DATA_ERROR;
    }

    TotalLength = Descriptor->wTotalLength;
    ExFreePoolWithTag(Descriptor, HIDUSB_TAG);

    if (TotalLength == 0)
    {
        DPRINT1("Configuration descriptor reports no length\n");
        return STATUS_DEVICE_DATA_ERROR;
    }

    Descriptor = NULL;
    Length = TotalLength;
    Status = HidUsbGetDescriptor(DeviceObject,
                                 URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE,
                                 USB_CONFIGURATION_DESCRIPTOR_TYPE,
                                 (PVOID *)&Descriptor,
                                 &Length,
                                 0,
                                 0);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Configuration descriptor request failed 0x%lx\n", Status);
        return Status;
    }

    if (Length < sizeof(USB_CONFIGURATION_DESCRIPTOR))
    {
        DPRINT1("Configuration descriptor too short, %lu bytes\n", Length);
        ExFreePoolWithTag(Descriptor, HIDUSB_TAG);
        return STATUS_DEVICE_DATA_ERROR;
    }

    /* Some devices report a different total length the second time */
    if (Descriptor->wTotalLength > TotalLength)
        Descriptor->wTotalLength = TotalLength;

    if (Descriptor->bLength < sizeof(USB_CONFIGURATION_DESCRIPTOR))
        Descriptor->bLength = sizeof(USB_CONFIGURATION_DESCRIPTOR);

    *ConfigDescriptor = Descriptor;
    *ConfigLength = Length;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Locates the HID descriptor of a HID interface.
 *
 * The descriptor normally follows the interface descriptor. Some devices put it
 * after the endpoint descriptors instead, which also sets HIDUSB_FLAG_ENDPOINT_REQUESTS.
 *
 * @param[in] Interface
 * The HID interface descriptor inside the configuration descriptor.
 *
 * @param[in] Remaining
 * Bytes left in the configuration descriptor from the interface on.
 *
 * @return
 * The HID descriptor, or NULL if none was found.
 */
static
PHID_DESCRIPTOR
NTAPI
HidUsbFindHidDescriptor(
    _Inout_ PHIDUSB_EXTENSION Extension,
    _In_ PUSB_INTERFACE_DESCRIPTOR Interface,
    _In_ ULONG Remaining)
{
    PUSB_COMMON_DESCRIPTOR Descriptor;
    PHID_DESCRIPTOR HidDescriptor;
    ULONG EndpointCount;
    ULONG Left;

    if (Remaining < sizeof(USB_INTERFACE_DESCRIPTOR))
        return NULL;

    if ((Interface->bLength < sizeof(USB_INTERFACE_DESCRIPTOR)) || (Interface->bLength > Remaining))
        return NULL;

    Left = Remaining - Interface->bLength;
    if (Left < sizeof(USB_COMMON_DESCRIPTOR))
        return NULL;

    Extension->Flags &= ~HIDUSB_FLAG_ENDPOINT_REQUESTS;
    HidDescriptor = NULL;

    Descriptor = (PUSB_COMMON_DESCRIPTOR)((PUCHAR)Interface + Interface->bLength);
    if (Descriptor->bLength < sizeof(USB_COMMON_DESCRIPTOR))
        return NULL;

    if (Descriptor->bDescriptorType == HID_HID_DESCRIPTOR_TYPE)
    {
        if ((Descriptor->bLength != HIDUSB_HID_DESCRIPTOR_LENGTH) ||
            (Left < HIDUSB_HID_DESCRIPTOR_LENGTH))
        {
            return NULL;
        }

        HidDescriptor = (PHID_DESCRIPTOR)Descriptor;
        Left -= HIDUSB_HID_DESCRIPTOR_LENGTH;
        Descriptor = (PUSB_COMMON_DESCRIPTOR)((PUCHAR)Descriptor + HIDUSB_HID_DESCRIPTOR_LENGTH);
    }
    else
    {
        Extension->Flags |= HIDUSB_FLAG_ENDPOINT_REQUESTS;
    }

    /* A malformed endpoint list keeps a HID descriptor already found */
    EndpointCount = 0;
    while (EndpointCount < Interface->bNumEndpoints)
    {
        if (Left < sizeof(USB_COMMON_DESCRIPTOR))
            return HidDescriptor;

        if (Descriptor->bDescriptorType == USB_ENDPOINT_DESCRIPTOR_TYPE)
        {
            if (Descriptor->bLength != sizeof(USB_ENDPOINT_DESCRIPTOR))
                return HidDescriptor;

            EndpointCount++;
        }
        else if ((Descriptor->bDescriptorType == USB_INTERFACE_DESCRIPTOR_TYPE) ||
                 (Descriptor->bLength == 0))
        {
            return HidDescriptor;
        }

        if (Descriptor->bLength > Left)
            return HidDescriptor;

        Left -= Descriptor->bLength;
        Descriptor = (PUSB_COMMON_DESCRIPTOR)((PUCHAR)Descriptor + Descriptor->bLength);
    }

    if (!(Extension->Flags & HIDUSB_FLAG_ENDPOINT_REQUESTS))
        return HidDescriptor;

    if (Left < sizeof(USB_COMMON_DESCRIPTOR))
        return NULL;

    /* Anything of the right size after the endpoints is taken as the HID descriptor */
    if ((Descriptor->bDescriptorType == HID_HID_DESCRIPTOR_TYPE) ||
        (Descriptor->bLength == HIDUSB_HID_DESCRIPTOR_LENGTH))
    {
        HidDescriptor = (PHID_DESCRIPTOR)Descriptor;
    }

    if ((Descriptor->bLength > Left) && (Left < HIDUSB_HID_DESCRIPTOR_LENGTH))
        return NULL;

    return HidDescriptor;
}

static
NTSTATUS
NTAPI
HidUsbGetHidInfo(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PUSB_CONFIGURATION_DESCRIPTOR ConfigDescriptor,
    _In_ ULONG ConfigLength)
{
    PUSB_INTERFACE_DESCRIPTOR Interface;
    PHID_DESCRIPTOR HidDescriptor;
    PHIDUSB_EXTENSION Extension;
    ULONG Offset;

    Extension = HidUsbGetExtension(DeviceObject);
    RtlZeroMemory(&Extension->HidDescriptor, sizeof(Extension->HidDescriptor));

    Interface = USBD_ParseConfigurationDescriptorEx(ConfigDescriptor,
                                                    ConfigDescriptor,
                                                    -1,
                                                    -1,
                                                    USB_DEVICE_CLASS_HUMAN_INTERFACE,
                                                    -1,
                                                    -1);
    if (Interface == NULL)
    {
        DPRINT1("No HID interface in the configuration\n");
        return STATUS_UNSUCCESSFUL;
    }

    Offset = (ULONG)((PUCHAR)Interface - (PUCHAR)ConfigDescriptor);
    HidDescriptor = HidUsbFindHidDescriptor(Extension, Interface, ConfigLength - Offset);
    if (HidDescriptor == NULL)
    {
        DPRINT1("No HID descriptor for interface %u\n", Interface->bInterfaceNumber);
        return STATUS_UNSUCCESSFUL;
    }

    RtlCopyMemory(&Extension->HidDescriptor, HidDescriptor, sizeof(Extension->HidDescriptor));
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Selects the configuration with only the first HID interface in it and keeps
 * a copy of the resulting interface and pipe information.
 */
static
NTSTATUS
NTAPI
HidUsbSelectConfiguration(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PUSB_CONFIGURATION_DESCRIPTOR ConfigDescriptor)
{
    USBD_INTERFACE_LIST_ENTRY InterfaceList[2];
    PUSBD_INTERFACE_INFORMATION Interface;
    PHIDUSB_EXTENSION Extension;
    NTSTATUS Status;
    PURB Urb;

    Extension = HidUsbGetExtension(DeviceObject);

    RtlZeroMemory(InterfaceList, sizeof(InterfaceList));
    InterfaceList[0].InterfaceDescriptor =
        USBD_ParseConfigurationDescriptorEx(ConfigDescriptor,
                                            ConfigDescriptor,
                                            -1,
                                            -1,
                                            USB_DEVICE_CLASS_HUMAN_INTERFACE,
                                            -1,
                                            -1);
    if (InterfaceList[0].InterfaceDescriptor == NULL)
        return STATUS_INVALID_PARAMETER;

    Urb = USBD_CreateConfigurationRequestEx(ConfigDescriptor, InterfaceList);
    if (Urb == NULL)
        return STATUS_NO_MEMORY;

    Status = HidUsbCallUsb(DeviceObject, Urb);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Select configuration failed 0x%lx\n", Status);
        Extension->ConfigurationHandle = NULL;
        ExFreePool(Urb);
        return Status;
    }

    Extension->ConfigurationHandle = Urb->UrbSelectConfiguration.ConfigurationHandle;

    Interface = &Urb->UrbSelectConfiguration.Interface;
    Extension->InterfaceInfo = ExAllocatePool2(POOL_FLAG_NON_PAGED, Interface->Length, HIDUSB_TAG);
    if (Extension->InterfaceInfo != NULL)
        RtlCopyMemory(Extension->InterfaceInfo, Interface, Interface->Length);
    else
        Status = STATUS_NO_MEMORY;

    ExFreePool(Urb);
    return Status;
}

static
NTSTATUS
NTAPI
HidUsbInitDevice(
    _In_ PDEVICE_OBJECT DeviceObject)
{
    PUSB_CONFIGURATION_DESCRIPTOR ConfigDescriptor;
    PHIDUSB_EXTENSION Extension;
    ULONG ConfigLength;
    NTSTATUS Status;

    Extension = HidUsbGetExtension(DeviceObject);

    Status = HidUsbGetDeviceDescriptor(DeviceObject, Extension);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Device descriptor request failed 0x%lx\n", Status);
        return Status;
    }

    Status = HidUsbGetConfigDescriptor(DeviceObject, &ConfigDescriptor, &ConfigLength);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = HidUsbGetHidInfo(DeviceObject, ConfigDescriptor, ConfigLength);
    if (NT_SUCCESS(Status))
    {
        Status = HidUsbSelectConfiguration(DeviceObject, ConfigDescriptor);

        /* SET_IDLE is optional for most devices, so its result does not matter */
        if (NT_SUCCESS(Status))
            HidUsbSetIdle(DeviceObject);
    }

    ExFreePoolWithTag(ConfigDescriptor, HIDUSB_TAG);
    return Status;
}

/* Puts the device back in the unconfigured state */
static
VOID
NTAPI
HidUsbDeconfigure(
    _In_ PDEVICE_OBJECT DeviceObject)
{
    struct _URB_SELECT_CONFIGURATION *Request;
    NTSTATUS Status;

    Request = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*Request), HIDUSB_TAG);
    if (Request == NULL)
    {
        DPRINT1("No memory to deconfigure the device\n");
        return;
    }

    Request->Hdr.Length = sizeof(*Request);
    Request->Hdr.Function = URB_FUNCTION_SELECT_CONFIGURATION;
    Request->ConfigurationDescriptor = NULL;

    Status = HidUsbCallUsb(DeviceObject, (PURB)Request);
    if (!NT_SUCCESS(Status))
        DPRINT1("Deconfigure failed 0x%lx\n", Status);

    ExFreePoolWithTag(Request, HIDUSB_TAG);
}

static
VOID
NTAPI
HidUsbFreeResources(
    _Inout_ PHIDUSB_EXTENSION Extension)
{
    if (Extension->InterfaceInfo != NULL)
    {
        ExFreePoolWithTag(Extension->InterfaceInfo, HIDUSB_TAG);
        Extension->InterfaceInfo = NULL;
    }

    if (Extension->DeviceDescriptor != NULL)
    {
        ExFreePoolWithTag(Extension->DeviceDescriptor, HIDUSB_TAG);
        Extension->DeviceDescriptor = NULL;
    }
}

static
NTSTATUS
NTAPI
HidUsbPnpCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent((PKEVENT)Context, EVENT_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static
NTSTATUS
NTAPI
HidUsbRemoveDevice(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    HIDUSB_DEVICE_STATE OldState;
    PHIDUSB_EXTENSION Extension;
    NTSTATUS Status;

    Extension = HidUsbGetExtension(DeviceObject);

    OldState = Extension->State;
    Extension->State = HidUsbStateRemoved;

    /* A stop already dropped the base reference */
    if ((OldState != HidUsbStateStopping) && (OldState != HidUsbStateStopped))
        HidUsbDecrementPendingRequests(Extension);

    HidUsbCompleteResetNotification(DeviceObject, STATUS_NO_SUCH_DEVICE);

    if (OldState == HidUsbStateRunning)
        HidUsbAbortPendingRequests(DeviceObject);

    KeWaitForSingleObject(&Extension->NoPendingRequests, Executive, KernelMode, FALSE, NULL);

    IoSkipCurrentIrpStackLocation(Irp);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Status = IoCallDriver(HidUsbGetNextDevice(DeviceObject), Irp);

    IoReleaseRemoveLockAndWait(&Extension->RemoveLock, Irp);
    HidUsbFreeResources(Extension);
    return Status;
}

/**
 * @brief
 * Handles PnP IRPs. Everything but a remove goes down first and is finished here.
 */
NTSTATUS
NTAPI
HidUsbPnp(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PDEVICE_CAPABILITIES Capabilities;
    HIDUSB_DEVICE_STATE OldState;
    PHIDUSB_EXTENSION Extension;
    PIO_STACK_LOCATION IoStack;
    UCHAR MinorFunction;
    NTSTATUS Status;
    KEVENT Event;

    Extension = HidUsbGetExtension(DeviceObject);
    IoStack = IoGetCurrentIrpStackLocation(Irp);
    MinorFunction = IoStack->MinorFunction;

    Status = IoAcquireRemoveLock(&Extension->RemoveLock, Irp);
    if (!NT_SUCCESS(Status))
    {
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    switch (MinorFunction)
    {
        case IRP_MN_START_DEVICE:
        {
            OldState = Extension->State;
            Extension->State = HidUsbStateStarting;
            KeResetEvent(&Extension->NoPendingRequests);

            /* Coming back from a stop or remove takes the base reference again */
            if ((OldState == HidUsbStateStopping) ||
                (OldState == HidUsbStateStopped) ||
                (OldState == HidUsbStateRemoved))
            {
                HidUsbIncrementPendingRequests(Extension);
            }

            Extension->InterfaceInfo = NULL;
            break;
        }

        case IRP_MN_REMOVE_DEVICE:
            return HidUsbRemoveDevice(DeviceObject, Irp);

        case IRP_MN_STOP_DEVICE:
        {
            if (Extension->State != HidUsbStateRunning)
                break;

            Extension->State = HidUsbStateStopping;
            HidUsbAbortPendingRequests(DeviceObject);
            HidUsbDecrementPendingRequests(Extension);
            KeWaitForSingleObject(&Extension->NoPendingRequests,
                                  Executive,
                                  KernelMode,
                                  FALSE,
                                  NULL);
            HidUsbDeconfigure(DeviceObject);
            break;
        }

        case IRP_MN_QUERY_PNP_DEVICE_STATE:
        {
            if (Extension->State == HidUsbStateFailed)
                Irp->IoStatus.Information |= PNP_DEVICE_FAILED;
            break;
        }

        default:
            break;
    }

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    IoCopyCurrentIrpStackLocationToNext(Irp);

    Status = IoSetCompletionRoutineEx(DeviceObject,
                                      Irp,
                                      HidUsbPnpCompletion,
                                      &Event,
                                      TRUE,
                                      TRUE,
                                      TRUE);
    if (NT_SUCCESS(Status))
    {
        if (IoCallDriver(HidUsbGetNextDevice(DeviceObject), Irp) == STATUS_PENDING)
            KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);

        Status = Irp->IoStatus.Status;

        switch (MinorFunction)
        {
            case IRP_MN_START_DEVICE:
            {
                if (!NT_SUCCESS(Status))
                {
                    DPRINT1("Lower start failed 0x%lx\n", Status);
                    Extension->State = HidUsbStateFailed;
                    break;
                }

                Extension->State = HidUsbStateRunning;
                Status = HidUsbInitDevice(DeviceObject);
                if (!NT_SUCCESS(Status))
                {
                    DPRINT1("Device initialization failed 0x%lx\n", Status);
                    Extension->State = HidUsbStateFailed;
                }
                break;
            }

            case IRP_MN_STOP_DEVICE:
            {
                Extension->State = HidUsbStateStopped;
                HidUsbFreeResources(Extension);
                break;
            }

            case IRP_MN_QUERY_CAPABILITIES:
            {
                if (NT_SUCCESS(Status))
                {
                    Capabilities = IoStack->Parameters.DeviceCapabilities.Capabilities;
                    Capabilities->SurpriseRemovalOK = TRUE;
                }
                break;
            }

            default:
                break;
        }
    }
    else
    {
        DPRINT1("No completion context for PnP IRP 0x%x\n", MinorFunction);
    }

    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    IoReleaseRemoveLock(&Extension->RemoveLock, Irp);
    return Status;
}
