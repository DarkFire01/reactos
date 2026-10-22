/*
* PROJECT:     ReactOS Universal Audio Class Driver
* LICENSE:     GPL - See COPYING in the top level directory
* FILE:        drivers/usb/usbaudio/usbaudio.c
* PURPOSE:     USB Audio device driver.
* PROGRAMMERS:
*              Johannes Anderwald (johannes.anderwald@reactos.org)
*/

#include "usbaudio.h"

static KSDEVICE_DISPATCH KsDeviceDispatch = {
    USBAudioAddDevice,
    USBAudioPnPStart,
    NULL,
    USBAudioPnPQueryStop,
    USBAudioPnPCancelStop,
    USBAudioPnPStop,
    USBAudioPnPQueryRemove,
    USBAudioPnPCancelRemove,
    USBAudioPnPRemove,
    USBAudioPnPQueryCapabilities,
    USBAudioPnPSurpriseRemoval,
    USBAudioPnPQueryPower,
    USBAudioPnPSetPower
};

static KSDEVICE_DESCRIPTOR KsDeviceDescriptor = {
    &KsDeviceDispatch,
    0,
    NULL,
    0x100, //KSDEVICE_DESCRIPTOR_VERSION,
    0
};

NTSTATUS
SubmitUrbSync(
    IN PDEVICE_OBJECT DeviceObject,
    IN PURB Urb)
{
    PIRP Irp;
    KEVENT Event;
    IO_STATUS_BLOCK IoStatus;
    PIO_STACK_LOCATION IoStack;
    NTSTATUS Status;

    // init event
    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    // build irp
    Irp = IoBuildDeviceIoControlRequest(IOCTL_INTERNAL_USB_SUBMIT_URB,
        DeviceObject,
        NULL,
        0,
        NULL,
        0,
        TRUE,
        &Event,
        &IoStatus);

    if (!Irp)
    {
        //
        // no memory
        //
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // get next stack location
    IoStack = IoGetNextIrpStackLocation(Irp);

    // store urb
    IoStack->Parameters.Others.Argument1 = Urb;

    // call driver
    Status = IoCallDriver(DeviceObject, Irp);

    // wait for the request to finish
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = IoStatus.Status;
    }

    // done
    return Status;
}

/**
 * @brief
 * Whether the device runs at high speed, as the bus driver says. High speed packets are
 * microframes, so streams pace themselves by it.
 */
static
BOOLEAN
UsbAudioIsHighSpeed(
    _In_ PDEVICE_EXTENSION DeviceExtension)
{
    USB_BUS_INTERFACE_USBDI_V1 BusInterface;
    IO_STATUS_BLOCK IoStatus;
    PIO_STACK_LOCATION IoStack;
    BOOLEAN HighSpeed = FALSE;
    KEVENT Event;
    NTSTATUS Status;
    PIRP Irp;

    RtlZeroMemory(&BusInterface, sizeof(BusInterface));
    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP, DeviceExtension->LowerDevice, NULL, 0, NULL, &Event, &IoStatus);
    if (!Irp)
        return FALSE;

    /* A PnP request starts out as not supported, as the bus driver expects */
    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    IoStack = IoGetNextIrpStackLocation(Irp);
    IoStack->MinorFunction = IRP_MN_QUERY_INTERFACE;
    IoStack->Parameters.QueryInterface.InterfaceType = &USB_BUS_INTERFACE_USBDI_GUID;
    IoStack->Parameters.QueryInterface.Size = sizeof(BusInterface);
    IoStack->Parameters.QueryInterface.Version = USB_BUSIF_USBDI_VERSION_1;
    IoStack->Parameters.QueryInterface.Interface = (PINTERFACE)&BusInterface;
    IoStack->Parameters.QueryInterface.InterfaceSpecificData = NULL;

    Status = IoCallDriver(DeviceExtension->LowerDevice, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = IoStatus.Status;
    }

    if (NT_SUCCESS(Status))
    {
        if (BusInterface.IsDeviceHighSpeed)
            HighSpeed = BusInterface.IsDeviceHighSpeed(BusInterface.BusContext);
        if (BusInterface.InterfaceDereference)
            BusInterface.InterfaceDereference(BusInterface.BusContext);
    }
    return HighSpeed;
}

/**
 * @brief
 * Sends a class request to the audio function and waits for it.
 */
NTSTATUS
UsbAudioClassRequest(
    _In_ PDEVICE_EXTENSION DeviceExtension,
    _In_ USHORT Function,
    _In_ UCHAR Request,
    _In_ USHORT Value,
    _In_ USHORT Index,
    _Inout_updates_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length,
    _In_ BOOLEAN In)
{
    PURB Urb;
    PVOID Transfer;
    NTSTATUS Status;

    /* 2.0 has one CUR request whose direction is in bmRequestType */
    if (DeviceExtension->Uac2 && Request == USB_AUDIO_GET_CUR)
        Request = USB_AUDIO2_CUR;

    Urb = AllocFunction(sizeof(struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST));
    Transfer = AllocFunction(max(Length, 1));
    if (!Urb || !Transfer)
    {
        if (Urb)
            FreeFunction(Urb);
        if (Transfer)
            FreeFunction(Transfer);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* The transfer goes through nonpaged memory; callers often pass their stack */
    if (!In)
        RtlCopyMemory(Transfer, Buffer, Length);

    UsbBuildVendorRequest(Urb,
                          Function,
                          sizeof(struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST),
                          In ? (USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK) : USBD_TRANSFER_DIRECTION_OUT,
                          0,
                          Request,
                          Value,
                          Index,
                          Transfer,
                          NULL,
                          Length,
                          NULL);

    /* An event, not a fast mutex: the wait for the transfer needs its completion APC */
    KeWaitForSingleObject(&DeviceExtension->ControlLock, Executive, KernelMode, FALSE, NULL);
    Status = SubmitUrbSync(DeviceExtension->LowerDevice, Urb);
    KeSetEvent(&DeviceExtension->ControlLock, IO_NO_INCREMENT, FALSE);

    if (NT_SUCCESS(Status) && In)
        RtlCopyMemory(Buffer, Transfer, Length);

    FreeFunction(Transfer);
    FreeFunction(Urb);
    return Status;
}

NTSTATUS
NTAPI
USBAudioSelectConfiguration(
    IN PKSDEVICE Device,
    IN PUSB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor)
{
    PDEVICE_EXTENSION DeviceExtension;
    PUSB_INTERFACE_DESCRIPTOR InterfaceDescriptor;
    PUSBD_INTERFACE_LIST_ENTRY InterfaceList;
    PURB Urb;
    NTSTATUS Status;
    ULONG InterfaceDescriptorCount;
    ULONG Index;

    /* alloc item for configuration request */
    InterfaceList = AllocFunction(sizeof(USBD_INTERFACE_LIST_ENTRY) * (ConfigurationDescriptor->bNumInterfaces + 1));
    if (!InterfaceList)
    {
        /* insufficient resources*/
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Every audio interface is selected once, at its zero bandwidth setting; streams pick their own later */
    InterfaceDescriptorCount = 0;
    InterfaceDescriptor = USBD_ParseConfigurationDescriptorEx(ConfigurationDescriptor, ConfigurationDescriptor, -1, 0, USB_DEVICE_CLASS_AUDIO, -1, -1);
    while (InterfaceDescriptor != NULL && InterfaceDescriptorCount < ConfigurationDescriptor->bNumInterfaces)
    {
        InterfaceList[InterfaceDescriptorCount++].InterfaceDescriptor = InterfaceDescriptor;
        InterfaceDescriptor = USBD_ParseConfigurationDescriptorEx(ConfigurationDescriptor, (PVOID)((ULONG_PTR)InterfaceDescriptor + InterfaceDescriptor->bLength), -1, 0, USB_DEVICE_CLASS_AUDIO, -1, -1);
    }

    if (!InterfaceDescriptorCount)
    {
        /* no such interface */
        FreeFunction(InterfaceList);
        return STATUS_INVALID_PARAMETER;
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

    /* The MIDI streaming interface's bulk pipes are known once it is selected */
    for (Index = 0; Index < InterfaceDescriptorCount; Index++)
    {
        if (InterfaceList[Index].InterfaceDescriptor->bInterfaceSubClass == USB_AUDIO_SUBCLASS_MIDI_STREAMING &&
            InterfaceList[Index].Interface)
        {
            UsbMidiSetupInterface(DeviceExtension, InterfaceList[Index].Interface);
        }
    }

    /* alloc interface info */
    DeviceExtension->InterfaceInfo = AllocFunction(Urb->UrbSelectConfiguration.Interface.Length);
    if (DeviceExtension->InterfaceInfo)
    {
        /* copy interface info */
        RtlCopyMemory(DeviceExtension->InterfaceInfo, &Urb->UrbSelectConfiguration.Interface, Urb->UrbSelectConfiguration.Interface.Length);
    }

    ExFreePool(Urb);
    FreeFunction(InterfaceList);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
USBAudioStartDevice(
    IN PKSDEVICE Device)
{
    PURB Urb;
    PUSB_DEVICE_DESCRIPTOR DeviceDescriptor;
    PUSB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor;
    PUSB_AUDIO_CONTROL_INTERFACE_HEADER_DESCRIPTOR Header;
    PDEVICE_EXTENSION DeviceExtension;
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
        return STATUS_INSUFFICIENT_RESOURCES;
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
    ConfigurationDescriptor = (Length >= sizeof(USB_CONFIGURATION_DESCRIPTOR)) ? AllocFunction(Length) : NULL;
    if (!ConfigurationDescriptor)
    {
        /* free resources */
        FreeFunction(Urb);
        FreeFunction(DeviceDescriptor);
        return STATUS_INSUFFICIENT_RESOURCES;
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

    /* every walk trusts wTotalLength, so it may not claim more than was read */
    ConfigurationDescriptor->wTotalLength = (USHORT)min((ULONG)ConfigurationDescriptor->wTotalLength, Length);

    /* lets add to object bag */
    KsAddItemToObjectBag(Device->Bag, DeviceDescriptor, ExFreePool);
    KsAddItemToObjectBag(Device->Bag, ConfigurationDescriptor, ExFreePool);

    DeviceExtension->ConfigurationDescriptor = ConfigurationDescriptor;
    DeviceExtension->DeviceDescriptor = DeviceDescriptor;

    Header = UsbAudioGetControlHeader(ConfigurationDescriptor, &DeviceExtension->ControlInterface);
    if (!Header)
    {
        DPRINT1("USBAudioStartDevice no audio control interface\n");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    DeviceExtension->Uac2 = UsbAudioIsVersion2Header(Header);
    DeviceExtension->HighSpeed = UsbAudioIsHighSpeed(DeviceExtension);

    Status = USBAudioSelectConfiguration(Device, ConfigurationDescriptor);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = UsbAudioParseFormats(DeviceExtension);
    DPRINT1("USBAudioStartDevice control interface %u, audio class %s, %s speed, %lu formats\n",
            DeviceExtension->ControlInterface, DeviceExtension->Uac2 ? "2.0" : "1.0",
            DeviceExtension->HighSpeed ? "high" : "full", DeviceExtension->FormatCount);
    return Status;
}


NTSTATUS
NTAPI
USBAudioAddDevice(
  _In_ PKSDEVICE Device)
{
    /* no op */
    DPRINT1("USBAudioAddDevice\n");
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
USBAudioPnPStart(
  _In_     PKSDEVICE         Device,
  _In_     PIRP              Irp,
  _In_opt_ PCM_RESOURCE_LIST TranslatedResourceList,
  _In_opt_ PCM_RESOURCE_LIST UntranslatedResourceList)
{
    NTSTATUS Status = STATUS_SUCCESS;
    PDEVICE_EXTENSION DeviceExtension;

    if (!Device->Started)
    {
        /* alloc context  */
        DeviceExtension = AllocFunction(sizeof(DEVICE_EXTENSION));
        if (DeviceExtension == NULL)
        {
             /* insufficient resources */
             return STATUS_INSUFFICIENT_RESOURCES;
        }

        /* init context */
        Device->Context = DeviceExtension;
        DeviceExtension->LowerDevice = Device->NextDeviceObject;
        KeInitializeEvent(&DeviceExtension->ControlLock, SynchronizationEvent, TRUE);
        KeInitializeSpinLock(&DeviceExtension->MidiInLock);
        KeInitializeEvent(&DeviceExtension->MidiInIdle, NotificationEvent, TRUE);

        /* add to object bag*/
        KsAddItemToObjectBag(Device->Bag, Device->Context, ExFreePool);

        /* init device*/
        Status = USBAudioStartDevice(Device);
        if (NT_SUCCESS(Status))
        {
            /* TODO retrieve interface */
            Status = USBAudioCreateFilterContext(Device);
        }
    }

    return Status;
}

NTSTATUS
NTAPI
USBAudioPnPQueryStop(
  _In_ PKSDEVICE Device,
  _In_ PIRP      Irp)
{
    /* no op */
    return STATUS_SUCCESS;
}

VOID
NTAPI
USBAudioPnPCancelStop(
  _In_ PKSDEVICE Device,
  _In_ PIRP      Irp)
{
    /* no op */
}

VOID
NTAPI
USBAudioPnPStop(
  _In_ PKSDEVICE Device,
  _In_ PIRP      Irp)
{
    /* Pins are closed, and their streams stopped, before the device goes */
}

NTSTATUS
NTAPI
USBAudioPnPQueryRemove(
  _In_ PKSDEVICE Device,
  _In_ PIRP      Irp)
{
    /* no op */
    return STATUS_SUCCESS;
}


VOID
NTAPI
USBAudioPnPCancelRemove(
  _In_ PKSDEVICE Device,
  _In_ PIRP      Irp)
{
    /* no op */
}

VOID
NTAPI
USBAudioPnPRemove(
  _In_ PKSDEVICE Device,
  _In_ PIRP      Irp)
{
    /* Pins are closed, and their streams stopped, before the device goes */
    if (Device->Context)
        UsbMidiFreeDevice(Device->Context);
}

NTSTATUS
NTAPI
USBAudioPnPQueryCapabilities(
  _In_    PKSDEVICE            Device,
  _In_    PIRP                 Irp,
  _Inout_ PDEVICE_CAPABILITIES Capabilities)
{
    /* The bus driver filled in the capabilities */
    return STATUS_SUCCESS;
}

VOID
NTAPI
USBAudioPnPSurpriseRemoval(
  _In_ PKSDEVICE Device,
  _In_ PIRP      Irp)
{
    /* Transfers to a removed device fail, and the pins stop when they are closed */
}

NTSTATUS
NTAPI
USBAudioPnPQueryPower(
  _In_ PKSDEVICE          Device,
  _In_ PIRP               Irp,
  _In_ DEVICE_POWER_STATE DeviceTo,
  _In_ DEVICE_POWER_STATE DeviceFrom,
  _In_ SYSTEM_POWER_STATE SystemTo,
  _In_ SYSTEM_POWER_STATE SystemFrom,
  _In_ POWER_ACTION       Action)
{
    /* no op */
    return STATUS_SUCCESS;
}

VOID
NTAPI
USBAudioPnPSetPower(
  _In_ PKSDEVICE          Device,
  _In_ PIRP               Irp,
  _In_ DEVICE_POWER_STATE To,
  _In_ DEVICE_POWER_STATE From)
{
    /* Streams are stopped by their clients before the system sleeps */
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
