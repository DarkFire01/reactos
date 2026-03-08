/*
* PROJECT:     ReactOS Universal Video Class Driver
* LICENSE:     GPL - See COPYING in the top level directory
* FILE:        drivers/usb/usbvideo/usbvideo.c
* PURPOSE:     USB Video device driver.
* PROGRAMMERS:
*              Johannes Anderwald (johannes.anderwald@reactos.org)
*/

#include "usbvideo.h"

static KSDEVICE_DISPATCH KsDeviceDispatch = {
    USBVideoAddDevice,
    USBVideoPnPStart,
    NULL,
    USBVideoPnPQueryStop,
    USBVideoPnPCancelStop,
    USBVideoPnPStop,
    USBVideoPnPQueryRemove,
    USBVideoPnPCancelRemove,
    USBVideoPnPRemove,
    USBVideoPnPQueryCapabilities,
    USBVideoPnPSurpriseRemoval,
    USBVideoPnPQueryPower,
    USBVideoPnPSetPower
};

static KSDEVICE_DESCRIPTOR KsDeviceDescriptor = {
    &KsDeviceDispatch,
    0,
    NULL,
    0x100, //KSDEVICE_DESCRIPTOR_VERSION,
    0
};

NTSTATUS
NTAPI
USBVideoSelectConfiguration(
    IN PKSDEVICE Device,
    IN PUSB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor)
{
    PUSB_VIDEO_DEVICE_EXTENSION DeviceExtension;
    PUSB_INTERFACE_DESCRIPTOR InterfaceDescriptor;
    PUSBD_INTERFACE_LIST_ENTRY InterfaceList;
    PURB Urb;
    NTSTATUS Status;
    ULONG InterfaceNumber;
    ULONG InterfaceDescriptorCount;

    /* alloc item for configuration request */
    InterfaceList = AllocFunction(sizeof(USBD_INTERFACE_LIST_ENTRY) * (ConfigurationDescriptor->bNumInterfaces + 1));
    if (!InterfaceList)
    {
        /* insufficient resources*/
        return USBD_STATUS_INSUFFICIENT_RESOURCES;
    }

    /* grab interface descriptor */
    InterfaceNumber = 0;
    InterfaceDescriptor = USBD_ParseConfigurationDescriptorEx(ConfigurationDescriptor, ConfigurationDescriptor, InterfaceNumber, 0, USB_DEVICE_CLASS_VIDEO, -1, -1);
    if (!InterfaceDescriptor)
    {
        /* no such interface */
        return STATUS_INVALID_PARAMETER;
    }

    /* lets enumerate the interfaces */
    InterfaceDescriptorCount = 0;
    while (InterfaceDescriptor != NULL)
    {
        if (InterfaceDescriptor->bInterfaceSubClass == 0x01) /* VIDEO_CONTROL*/
        {
            InterfaceList[InterfaceDescriptorCount++].InterfaceDescriptor = InterfaceDescriptor;
        }
        else if (InterfaceDescriptor->bInterfaceSubClass == 0x02) /* VIDEO_STREAMING*/
        {
            InterfaceList[InterfaceDescriptorCount++].InterfaceDescriptor = InterfaceDescriptor;
        }
        InterfaceNumber++;
        InterfaceDescriptor = USBD_ParseConfigurationDescriptorEx(ConfigurationDescriptor, (PVOID)((ULONG_PTR)InterfaceDescriptor + InterfaceDescriptor->bLength), InterfaceNumber, 0, USB_DEVICE_CLASS_VIDEO, -1, -1);
    }

    /* build urb */
    Urb = USBD_CreateConfigurationRequestEx(ConfigurationDescriptor, InterfaceList);
    if (!Urb)
    {
        /* no memory */
        FreeFunction(InterfaceList);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* device extension */
    DeviceExtension = Device->Context;

    /* submit configuration urb */
    Status = SubmitUrbSync(DeviceExtension->LowerDevice, Urb);
    if (!NT_SUCCESS(Status))
    {
        /* free resources */
        ExFreePool(Urb);
        FreeFunction(InterfaceList);
        return Status;
    }

    /* store configuration handle */
    DeviceExtension->ConfigurationHandle = Urb->UrbSelectConfiguration.ConfigurationHandle;

    /* alloc interface info */
    DeviceExtension->InterfaceInfo = AllocFunction(Urb->UrbSelectConfiguration.Interface.Length);
    if (DeviceExtension->InterfaceInfo)
    {
        /* copy interface info */
        RtlCopyMemory(DeviceExtension->InterfaceInfo, &Urb->UrbSelectConfiguration.Interface, Urb->UrbSelectConfiguration.Interface.Length);
    }
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
USBVideoStartDevice(
    IN PKSDEVICE Device)
{
    PURB Urb;
    PUSB_DEVICE_DESCRIPTOR DeviceDescriptor;
    PUSB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor;
    PUSB_VIDEO_DEVICE_EXTENSION DeviceExtension;
    NTSTATUS Status;
    ULONG Length;

    /* get device extension */
    DeviceExtension = Device->Context;

    /* allocate urb */
    Urb = AllocFunction(sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST));
    if (!Urb)
    {
        /* no memory */
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* alloc buffer for device descriptor */
    DeviceDescriptor = AllocFunction(sizeof(USB_DEVICE_DESCRIPTOR));
    if (!DeviceDescriptor)
    {
        /* insufficient resources */
        FreeFunction(Urb);
       return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* build descriptor request */
    UsbBuildGetDescriptorRequest(Urb, sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST), USB_DEVICE_DESCRIPTOR_TYPE, 0, 0, DeviceDescriptor, NULL, sizeof(USB_DEVICE_DESCRIPTOR), NULL);

    /* submit urb */
    Status = SubmitUrbSync(DeviceExtension->LowerDevice, Urb);
    if (!NT_SUCCESS(Status))
    {
        /* free resources */
        FreeFunction(Urb);
        FreeFunction(DeviceDescriptor);
        return Status;
    }

    /* now allocate some space for partial configuration descriptor */
    ConfigurationDescriptor = AllocFunction(sizeof(USB_CONFIGURATION_DESCRIPTOR));
    if (!ConfigurationDescriptor)
    {
        /* free resources */
        FreeFunction(Urb);
        FreeFunction(DeviceDescriptor);
        return Status;
    }

    /* build descriptor request */
    UsbBuildGetDescriptorRequest(Urb, sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST), USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, 0, ConfigurationDescriptor, NULL, sizeof(USB_CONFIGURATION_DESCRIPTOR), NULL);

    /* submit urb */
    Status = SubmitUrbSync(DeviceExtension->LowerDevice, Urb);
    if (!NT_SUCCESS(Status))
    {
        /* free resources */
        FreeFunction(Urb);
        FreeFunction(DeviceDescriptor);
        FreeFunction(ConfigurationDescriptor);
        return Status;
    }

    /* backup length */
    Length = ConfigurationDescriptor->wTotalLength;

    /* free old descriptor */
    FreeFunction(ConfigurationDescriptor);

    /* now allocate some space for full configuration descriptor */
    ConfigurationDescriptor = AllocFunction(Length);
    if (!ConfigurationDescriptor)
    {
        /* free resources */
        FreeFunction(Urb);
        FreeFunction(DeviceDescriptor);
        return Status;
    }

    /* build descriptor request */
    UsbBuildGetDescriptorRequest(Urb, sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST), USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, 0, ConfigurationDescriptor, NULL, Length, NULL);

    /* submit urb */
    Status = SubmitUrbSync(DeviceExtension->LowerDevice, Urb);

    /* free urb */
    FreeFunction(Urb);
    if (!NT_SUCCESS(Status))
    {
        /* free resources */
        FreeFunction(DeviceDescriptor);
        FreeFunction(ConfigurationDescriptor);
        return Status;
    }

    /* check UVC version */
    Status = USBVideoCheckUVCVersion(Device, ConfigurationDescriptor);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("[USBVIDEO] Failed to retrieve UVC version");
        FreeFunction(DeviceDescriptor);
        FreeFunction(ConfigurationDescriptor);
        return Status;
    }

    /* lets add to object bag */
    KsAddItemToObjectBag(Device->Bag, DeviceDescriptor, ExFreePool);
    KsAddItemToObjectBag(Device->Bag, ConfigurationDescriptor, ExFreePool);

    Status = USBVideoSelectConfiguration(Device, ConfigurationDescriptor);
    if (NT_SUCCESS(Status))
    {
        DeviceExtension->ConfigurationDescriptor = ConfigurationDescriptor;
        DeviceExtension->DeviceDescriptor = DeviceDescriptor;
        DPRINT1("UsbVideoSelectConfiguration success %x\n", Status);
    }
    else
    {
        DPRINT1("UsbVideoSelectConfiguration failed with %x\n", Status);
    }
    return Status;
}

NTSTATUS
NTAPI
USBVideoAddDevice(
  _In_ PKSDEVICE Device)
{
    UNIMPLEMENTED;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
USBVideoPnPStart(
  _In_     PKSDEVICE         Device,
  _In_     PIRP              Irp,
  _In_opt_ PCM_RESOURCE_LIST TranslatedResourceList,
  _In_opt_ PCM_RESOURCE_LIST UntranslatedResourceList
)
{
    NTSTATUS Status = STATUS_SUCCESS;
    PUSB_VIDEO_DEVICE_EXTENSION DeviceExtension;

    if (!Device->Started)
    {
        /* alloc context  */
        DeviceExtension = AllocFunction(sizeof(USB_VIDEO_DEVICE_EXTENSION));
        if (DeviceExtension == NULL)
        {
             /* insufficient resources */
             return STATUS_INSUFFICIENT_RESOURCES;
        }

        /* init context */
        Device->Context = DeviceExtension;
        DeviceExtension->LowerDevice = Device->NextDeviceObject;
        KeInitializeEvent(&DeviceExtension->StoppedStreamingEvent, NotificationEvent, FALSE);

        /* add to object bag*/
        KsAddItemToObjectBag(Device->Bag, Device->Context, ExFreePool);

        /* init device*/
        Status = USBVideoStartDevice(Device);
        if (NT_SUCCESS(Status))
        {
            Status = USBVideoCreateFilterContext(Device);
            if (!NT_SUCCESS(Status))
            {
                DPRINT1("USBVideoCreateFilterContext failed with %x\n", Status);
                return Status;
            }
        }

        /* Set FDO flags */
        Device->FunctionalDeviceObject->Flags |= (DO_POWER_PAGABLE | DO_BUFFERED_IO);
        Device->FunctionalDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
    }
    return Status;
}

NTSTATUS
NTAPI
USBVideoPnPQueryStop(
  _In_ PKSDEVICE Device,
  _In_ PIRP      Irp
)
{
    UNIMPLEMENTED;
    return STATUS_SUCCESS;
}

VOID
NTAPI
USBVideoPnPCancelStop(
  _In_ PKSDEVICE Device,
  _In_ PIRP      Irp
)
{
    UNIMPLEMENTED;
}

VOID
NTAPI
USBVideoPnPStop(
  _In_ PKSDEVICE Device,
  _In_ PIRP      Irp
)
{
    UNIMPLEMENTED;
}

NTSTATUS
NTAPI
USBVideoPnPQueryRemove(
  _In_ PKSDEVICE Device,
  _In_ PIRP      Irp
)
{
    UNIMPLEMENTED;
    return STATUS_SUCCESS;
}

VOID
NTAPI
USBVideoPnPCancelRemove(
  _In_ PKSDEVICE Device,
  _In_ PIRP      Irp
)
{
    UNIMPLEMENTED;
}

VOID
NTAPI
USBVideoPnPRemove(
  _In_ PKSDEVICE Device,
  _In_ PIRP      Irp
)
{
    UNIMPLEMENTED;
}

NTSTATUS
NTAPI
USBVideoPnPQueryCapabilities(
  _In_    PKSDEVICE            Device,
  _In_    PIRP                 Irp,
  _Inout_ PDEVICE_CAPABILITIES Capabilities
)
{
    UNIMPLEMENTED;
    return STATUS_SUCCESS;
}

VOID
NTAPI
USBVideoPnPSurpriseRemoval(
  _In_ PKSDEVICE Device,
  _In_ PIRP      Irp
)
{
    UNIMPLEMENTED;
}

NTSTATUS
NTAPI
USBVideoPnPQueryPower(
  _In_ PKSDEVICE          Device,
  _In_ PIRP               Irp,
  _In_ DEVICE_POWER_STATE DeviceTo,
  _In_ DEVICE_POWER_STATE DeviceFrom,
  _In_ SYSTEM_POWER_STATE SystemTo,
  _In_ SYSTEM_POWER_STATE SystemFrom,
  _In_ POWER_ACTION       Action
)
{
    UNIMPLEMENTED;
    return STATUS_SUCCESS;
}

VOID
NTAPI
USBVideoPnPSetPower(
  _In_ PKSDEVICE          Device,
  _In_ PIRP               Irp,
  _In_ DEVICE_POWER_STATE To,
  _In_ DEVICE_POWER_STATE From
)
{
    UNIMPLEMENTED;
}

NTSTATUS
NTAPI
DriverEntry(
    IN PDRIVER_OBJECT DriverObject,
    IN PUNICODE_STRING RegistryPath)
{
    NTSTATUS Status;

    // initialize driver
    Status = KsInitializeDriver(DriverObject, RegistryPath, &KsDeviceDescriptor);
    if (!NT_SUCCESS(Status))
    {
        // failed to initialize driver
        DPRINT1("Failed to initialize driver with %x\n", Status);
        return Status;
    }
    return Status;
}
