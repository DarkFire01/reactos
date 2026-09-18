/*++

    usbdexinternal.h - Reconstructed private UCX<->USBD interface.

    USBD_INTERFACE_V1 is the function table the USB controller extension (UCX)
    publishes via WDF_QUERY_INTERFACE for usbd.sys to consume.  The original
    header is Microsoft-internal.  The layout below was reconstructed from a
    public reverse-engineering reference (offsets verified, total size 0x98)
    and matches the field names used by the UCX sources:

        +0x00 Size                 USHORT
        +0x02 Version              USHORT
        +0x08 Context              PVOID
        +0x10 InterfaceReference   PINTERFACE_REFERENCE
        +0x18 InterfaceDereference PINTERFACE_DEREFERENCE
        +0x20 USBDClientContractVersion ULONG
        +0x28 USBDInterfaceHandle  USBDI_HANDLE
        +0x30 DeviceObject         PDEVICE_OBJECT
        +0x38 PoolTag              ULONG
        +0x40 ContextFromClient    PVOID
        +0x48 UsbVerifierEnabled .. UsbVerifierFailEnableStaticStreams  (6x ULONG)
        +0x60 UsbVerifierTrackXrbs ULONG
        +0x68 Unregister .. XrbFree  (6x function pointer)

    With /Zp8 and natural alignment this ordering reproduces the documented
    offsets exactly.  The function-pointer fields are typed PVOID (the driver
    only stores function addresses into them; usbd.sys is what invokes them).

--*/

#pragma once

#include <usb.h>       // URB / descriptor types that usbdlib.h declares against
#include <usbdlib.h>   // USBDI_HANDLE, USBD_INTERFACE_VERSION_602

typedef struct _USBD_INTERFACE_V1 {
    USHORT                  Size;
    USHORT                  Version;
    PVOID                   Context;
    PINTERFACE_REFERENCE    InterfaceReference;
    PINTERFACE_DEREFERENCE  InterfaceDereference;
    ULONG                   USBDClientContractVersion;
    USBDI_HANDLE            USBDInterfaceHandle;
    PDEVICE_OBJECT          DeviceObject;
    ULONG                   PoolTag;
    PVOID                   ContextFromClient;
    ULONG                   UsbVerifierEnabled;
    ULONG                   UsbVerifierFailRegistration;
    ULONG                   UsbVerifierFailChainedMdlSupport;
    ULONG                   UsbVerifierFailStaticStreamSupport;
    ULONG                   UsbVerifierStaticStreamCountOverride;
    ULONG                   UsbVerifierFailEnableStaticStreams;
    ULONG                   UsbVerifierTrackXrbs;
    PVOID                   Unregister;
    PVOID                   XrbAllocate;
    PVOID                   IsochXrbAllocate;
    PVOID                   SelectConfigXrbAllocateAndBuild;
    PVOID                   SelectInterfaceXrbAllocateAndBuild;
    PVOID                   XrbFree;
} USBD_INTERFACE_V1, *PUSBD_INTERFACE_V1;

//
// Interface GUID the controller extension exposes.  RECONSTRUCTED placeholder:
// the real value (needed for ABI interop with the shipped usbd.sys) is not
// known here.  selectany lets every TU that includes this header share one
// definition without a multiple-definition link error.
//
__declspec(selectany) const GUID GUID_USBD_INTERFACE_V1 =
    { 0x9c9f0808, 0x0d6b, 0x4f6e, { 0x9a, 0x11, 0x00, 0x55, 0x44, 0x42, 0x56, 0x31 } };

//
// The controller extension hands the routines below out through the interface
// above.  usbdex calls them on behalf of a client driver, so the prototypes
// belong with the interface rather than with either side of it.
//
typedef NTSTATUS (NTAPI *PUSBDEX_XRB_ALLOCATE)(
    _In_ USBDI_HANDLE USBDInterfaceHandle,
    _Out_ PURB *Urb);

typedef NTSTATUS (NTAPI *PUSBDEX_ISOCH_XRB_ALLOCATE)(
    _In_ USBDI_HANDLE USBDInterfaceHandle,
    _In_ ULONG NumberOfIsochPackets,
    _Out_ PURB *Urb);

typedef NTSTATUS (NTAPI *PUSBDEX_SELECT_CONFIG_XRB_ALLOCATE_AND_BUILD)(
    _In_ USBDI_HANDLE USBDInterfaceHandle,
    _In_ PUSB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor,
    _In_ PUSBD_INTERFACE_LIST_ENTRY InterfaceList,
    _Out_ PURB *Urb);

typedef NTSTATUS (NTAPI *PUSBDEX_SELECT_INTERFACE_XRB_ALLOCATE_AND_BUILD)(
    _In_ USBDI_HANDLE USBDInterfaceHandle,
    _In_ USBD_CONFIGURATION_HANDLE ConfigurationHandle,
    _In_ PUSBD_INTERFACE_LIST_ENTRY InterfaceListEntry,
    _Out_ PURB *Urb);

typedef VOID (NTAPI *PUSBDEX_XRB_FREE)(
    _In_ PURB Urb);

typedef VOID (NTAPI *PUSBDEX_UNREGISTER)(
    _In_ USBDI_HANDLE USBDInterfaceHandle);

//
// IOCTL_INTERNAL_USB_QUERY_USB_CAPABILITY, and the payload a client passes
// through IO_STACK_LOCATION::Parameters.Others.Argument1 with it.  The output
// buffer rides along in Irp->AssociatedIrp.SystemBuffer.
//
#ifndef USB_QUERY_USB_CAPABILITY
#define USB_QUERY_USB_CAPABILITY 0x412
#endif

#ifndef IOCTL_INTERNAL_USB_QUERY_USB_CAPABILITY
#define IOCTL_INTERNAL_USB_QUERY_USB_CAPABILITY \
    CTL_CODE(FILE_DEVICE_USBEX, USB_QUERY_USB_CAPABILITY, METHOD_NEITHER, FILE_ANY_ACCESS)
#endif

#ifndef QUERY_USB_CAPABILITY_LATEST
#define QUERY_USB_CAPABILITY_LATEST 1
#endif

typedef struct _QUERY_USB_CAPABILITY {
    PVOID   USBDIHandle;
    GUID    CapabilityType;
    ULONG   OutputBufferLength;
    ULONG   Version;
    ULONG   Size;
    ULONG   ResultLength;
} QUERY_USB_CAPABILITY, *PQUERY_USB_CAPABILITY;
