/*
* PROJECT:     ReactOS Universal Video Class Driver
* LICENSE:     GPL - See COPYING in the top level directory
* FILE:        drivers/usb/usbvideo/filter.c
* PURPOSE:     USB Video device driver.
* PROGRAMMERS:
*              Johannes Anderwald (johannes.anderwald@reactos.org)
*/
#include "usbvideo.h"

static LPWSTR ReferenceString = L"global";
GUID GUID_KSCATEGORY_VIDEO = { STATIC_KSCATEGORY_VIDEO };

static KSFILTER_DISPATCH USBVideoFilterDispatch =
{
    USBVideoFilterCreate,
    NULL,
    NULL,
    NULL
};

static KSPIN_DISPATCH UsbVideoPinDispatch =
{
    USBVideoPinCreate,
    USBVideoPinClose,
    USBVideoPinProcess,
    USBVideoPinReset,
    USBVideoPinSetDataFormat,
    USBVideoPinSetDeviceState,
    NULL,
    NULL,
    NULL,
    NULL
};


NTSTATUS
NTAPI
USBVideoFilterCreate(
    PKSFILTER Filter,
    PIRP Irp)
{
    PKSFILTERFACTORY FilterFactory;
    PKSDEVICE Device;
    PFILTER_CONTEXT FilterContext;

    FilterFactory = KsGetParent(Filter);
    if (FilterFactory == NULL)
    {
        /* invalid parameter */
        return STATUS_INVALID_PARAMETER;
    }

    Device = KsGetParent(FilterFactory);
    if (Device == NULL)
    {
        /* invalid parameter */
        return STATUS_INVALID_PARAMETER;
    }

    /* alloc filter context */
    FilterContext = AllocFunction(sizeof(FILTER_CONTEXT));
    if (FilterContext == NULL)
    {
        /* no memory */
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* init context */
    FilterContext->DeviceExtension = Device->Context;
    FilterContext->LowerDevice = Device->NextDeviceObject;
    Filter->Context = FilterContext;

    DPRINT("USBVideoFilterCreate FilterContext %p LowerDevice %p DeviceExtension %p\n", FilterContext, FilterContext->LowerDevice, FilterContext->DeviceExtension);
    KsAddItemToObjectBag(Filter->Bag, FilterContext, ExFreePool);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
USBVideoCreateFilterContext(
    PKSDEVICE Device)
{
    PKSFILTER_DESCRIPTOR FilterDescriptor;
    NTSTATUS Status;

    /* allocate descriptor */
    FilterDescriptor = AllocFunction(sizeof(KSFILTER_DESCRIPTOR));
    if (!FilterDescriptor)
    {
        /* no memory */
        return USBD_STATUS_INSUFFICIENT_RESOURCES;
    }

    /* init filter descriptor*/
    FilterDescriptor->Version = KSFILTER_DESCRIPTOR_VERSION;
    FilterDescriptor->Flags = 0;
    FilterDescriptor->ReferenceGuid = &KSNAME_Filter;
    FilterDescriptor->Dispatch = &USBVideoFilterDispatch;
    FilterDescriptor->CategoriesCount = 1;
    FilterDescriptor->Categories = &GUID_KSCATEGORY_VIDEO;

    /* build topology */
    Status = BuildUSBVideoFilterTopology(Device, FilterDescriptor);
    if (!NT_SUCCESS(Status))
    {
        /* failed*/
        FreeFunction(FilterDescriptor);
        return Status;
    }

    /* build pin descriptors */
    Status = USBVideoPinBuildDescriptors(Device, (PKSPIN_DESCRIPTOR_EX *)&FilterDescriptor->PinDescriptors, &FilterDescriptor->PinDescriptorsCount, &FilterDescriptor->PinDescriptorSize);
    if (!NT_SUCCESS(Status))
    {
        /* failed*/
        FreeFunction(FilterDescriptor);
        return Status;
    }

    /* lets create the filter */
    Status = KsCreateFilterFactory(Device->FunctionalDeviceObject, FilterDescriptor, ReferenceString, NULL, KSCREATE_ITEM_FREEONSTOP, NULL, NULL, NULL);
    DPRINT("KsCreateFilterFactory: %x\n", Status);
    return Status;
}
