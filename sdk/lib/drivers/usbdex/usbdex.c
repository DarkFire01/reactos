/*
 * PROJECT:     ReactOS USB Client Support Library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Client side of the USBD_INTERFACE_V1 contract
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

/* INCLUDES *******************************************************************/

#include <ntddk.h>
#include <usb.h>
#include <usbdlib.h>
#include <usbioctl.h>
#include <drivers/usb/usbdexinternal.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

#define USBDEX_SIGNATURE 'XDBU'

/*
 * What a USBD_HANDLE points at. The interface is the function table the core
 * stack filled in, and everything a later call needs is kept beside it so the
 * caller only ever carries the one handle.
 */
typedef struct _USBDEX_HANDLE_DATA
{
    ULONG Signature;
    ULONG ClientContractVersion;
    ULONG PoolTag;
    PDEVICE_OBJECT DeviceObject;
    PDEVICE_OBJECT TargetDeviceObject;
    BOOLEAN Registered;
    USBD_INTERFACE_V1 Interface;
} USBDEX_HANDLE_DATA, *PUSBDEX_HANDLE_DATA;

/* FUNCTIONS ******************************************************************/

static
PUSBDEX_HANDLE_DATA
UsbdexGetHandleData(
    _In_opt_ USBD_HANDLE USBDHandle)
{
    PUSBDEX_HANDLE_DATA HandleData = (PUSBDEX_HANDLE_DATA)USBDHandle;

    if (HandleData == NULL || HandleData->Signature != USBDEX_SIGNATURE)
        return NULL;

    return HandleData;
}

static
NTSTATUS
NTAPI
UsbdexSyncCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

/**
 * @brief Runs one synthesized IRP down the target stack and waits for it.
 *
 * The caller has already filled in the stack location, so all that is left is
 * to hang a completion routine off it and block until the stack is done. The
 * IRP is ours, so it is freed here either way.
 */
static
NTSTATUS
UsbdexCallTargetSynchronously(
    _In_ PDEVICE_OBJECT TargetDeviceObject,
    _In_ PIRP Irp)
{
    KEVENT Event;
    PIO_STACK_LOCATION IoStack;
    NTSTATUS Status;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    IoStack = IoGetNextIrpStackLocation(Irp);
    IoStack->CompletionRoutine = UsbdexSyncCompletion;
    IoStack->Context = &Event;
    IoStack->Control = SL_INVOKE_ON_SUCCESS | SL_INVOKE_ON_ERROR | SL_INVOKE_ON_CANCEL;

    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;

    Status = IoCallDriver(TargetDeviceObject, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = Irp->IoStatus.Status;
    }

    IoFreeIrp(Irp);

    return Status;
}

static
NTSTATUS
UsbdexQueryInterface(
    _In_ PDEVICE_OBJECT TargetDeviceObject,
    _In_ const GUID *InterfaceType,
    _Inout_ PINTERFACE Interface)
{
    PIO_STACK_LOCATION IoStack;
    PIRP Irp;

    Irp = IoAllocateIrp(TargetDeviceObject->StackSize, FALSE);
    if (Irp == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    IoStack = IoGetNextIrpStackLocation(Irp);
    IoStack->MajorFunction = IRP_MJ_PNP;
    IoStack->MinorFunction = IRP_MN_QUERY_INTERFACE;
    IoStack->Parameters.QueryInterface.InterfaceType = InterfaceType;
    IoStack->Parameters.QueryInterface.Interface = Interface;
    IoStack->Parameters.QueryInterface.Size = Interface->Size;
    IoStack->Parameters.QueryInterface.Version = Interface->Version;
    IoStack->Parameters.QueryInterface.InterfaceSpecificData = NULL;

    return UsbdexCallTargetSynchronously(TargetDeviceObject, Irp);
}

_Must_inspect_result_
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
USBD_CreateHandle(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PDEVICE_OBJECT TargetDeviceObject,
    _In_ ULONG USBDClientContractVersion,
    _In_ ULONG PoolTag,
    _Out_ USBD_HANDLE *USBDHandle)
{
    PUSBDEX_HANDLE_DATA HandleData;
    NTSTATUS Status;

    if (USBDHandle == NULL)
        return STATUS_INVALID_PARAMETER;

    *USBDHandle = NULL;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return STATUS_INVALID_DEVICE_STATE;

    if (DeviceObject == NULL || TargetDeviceObject == NULL || PoolTag == 0)
        return STATUS_INVALID_PARAMETER;

    /* Nothing older than this ever asked for a handle */
    if (USBDClientContractVersion != USBD_CLIENT_CONTRACT_VERSION_602)
    {
        DPRINT1("Unsupported client contract version 0x%lx\n", USBDClientContractVersion);
        return STATUS_NOT_SUPPORTED;
    }

    HandleData = ExAllocatePoolWithTag(NonPagedPool,
                                       sizeof(*HandleData),
                                       PoolTag);
    if (HandleData == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(HandleData, sizeof(*HandleData));

    HandleData->Signature = USBDEX_SIGNATURE;
    HandleData->ClientContractVersion = USBDClientContractVersion;
    HandleData->PoolTag = PoolTag;
    HandleData->DeviceObject = DeviceObject;
    HandleData->TargetDeviceObject = TargetDeviceObject;

    /*
     * The interface doubles as the registration request: the stack reads the
     * client half out of it before filling in the routines it hands back.
     */
    HandleData->Interface.Size = sizeof(USBD_INTERFACE_V1);
    HandleData->Interface.Version = USBD_INTERFACE_VERSION_602;
    HandleData->Interface.USBDClientContractVersion = USBDClientContractVersion;
    HandleData->Interface.DeviceObject = DeviceObject;
    HandleData->Interface.PoolTag = PoolTag;

    Status = UsbdexQueryInterface(TargetDeviceObject,
                                  &GUID_USBD_INTERFACE_V1,
                                  (PINTERFACE)&HandleData->Interface);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("The stack below %p does not speak USBD_INTERFACE_V1 (Status 0x%08lx)\n",
                TargetDeviceObject, Status);
        ExFreePoolWithTag(HandleData, PoolTag);
        return Status;
    }

    if (HandleData->Interface.USBDInterfaceHandle == NULL ||
        HandleData->Interface.XrbAllocate == NULL ||
        HandleData->Interface.XrbFree == NULL)
    {
        DPRINT1("The stack below %p returned an incomplete interface\n", TargetDeviceObject);
        ExFreePoolWithTag(HandleData, PoolTag);
        return STATUS_NOT_SUPPORTED;
    }

    HandleData->Registered = TRUE;

    *USBDHandle = (USBD_HANDLE)HandleData;

    return STATUS_SUCCESS;
}

VOID
USBD_CloseHandle(
    _In_ USBD_HANDLE USBDHandle)
{
    PUSBDEX_HANDLE_DATA HandleData;
    ULONG PoolTag;

    HandleData = UsbdexGetHandleData(USBDHandle);
    if (HandleData == NULL)
        return;

    if (HandleData->Registered)
    {
        if (HandleData->Interface.Unregister != NULL)
        {
            ((PUSBDEX_UNREGISTER)HandleData->Interface.Unregister)(
                HandleData->Interface.USBDInterfaceHandle);
        }

        if (HandleData->Interface.InterfaceDereference != NULL)
            HandleData->Interface.InterfaceDereference(HandleData->Interface.Context);
    }

    PoolTag = HandleData->PoolTag;
    HandleData->Signature = 0;
    ExFreePoolWithTag(HandleData, PoolTag);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Must_inspect_result_
NTSTATUS
USBD_UrbAllocate(
    _In_ USBD_HANDLE USBDHandle,
    _Outptr_result_bytebuffer_(sizeof(URB)) PURB *Urb)
{
    PUSBDEX_HANDLE_DATA HandleData;

    if (Urb == NULL)
        return STATUS_INVALID_PARAMETER;

    *Urb = NULL;

    HandleData = UsbdexGetHandleData(USBDHandle);
    if (HandleData == NULL)
        return STATUS_INVALID_PARAMETER;

    return ((PUSBDEX_XRB_ALLOCATE)HandleData->Interface.XrbAllocate)(
               HandleData->Interface.USBDInterfaceHandle, Urb);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Must_inspect_result_
NTSTATUS
USBD_IsochUrbAllocate(
    _In_ USBD_HANDLE USBDHandle,
    _In_ ULONG NumberOfIsochPackets,
    _Outptr_ PURB *Urb)
{
    PUSBDEX_HANDLE_DATA HandleData;

    if (Urb == NULL)
        return STATUS_INVALID_PARAMETER;

    *Urb = NULL;

    HandleData = UsbdexGetHandleData(USBDHandle);
    if (HandleData == NULL || HandleData->Interface.IsochXrbAllocate == NULL)
        return STATUS_INVALID_PARAMETER;

    return ((PUSBDEX_ISOCH_XRB_ALLOCATE)HandleData->Interface.IsochXrbAllocate)(
               HandleData->Interface.USBDInterfaceHandle, NumberOfIsochPackets, Urb);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Must_inspect_result_
NTSTATUS
USBD_SelectConfigUrbAllocateAndBuild(
    _In_ USBD_HANDLE USBDHandle,
    _In_ PUSB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor,
    _In_ PUSBD_INTERFACE_LIST_ENTRY InterfaceList,
    _Outptr_ PURB *Urb)
{
    PUSBDEX_HANDLE_DATA HandleData;

    if (Urb == NULL)
        return STATUS_INVALID_PARAMETER;

    *Urb = NULL;

    HandleData = UsbdexGetHandleData(USBDHandle);
    if (HandleData == NULL || HandleData->Interface.SelectConfigXrbAllocateAndBuild == NULL)
        return STATUS_INVALID_PARAMETER;

    return ((PUSBDEX_SELECT_CONFIG_XRB_ALLOCATE_AND_BUILD)
                HandleData->Interface.SelectConfigXrbAllocateAndBuild)(
               HandleData->Interface.USBDInterfaceHandle,
               ConfigurationDescriptor,
               InterfaceList,
               Urb);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Must_inspect_result_
NTSTATUS
USBD_SelectInterfaceUrbAllocateAndBuild(
    _In_ USBD_HANDLE USBDHandle,
    _In_ USBD_CONFIGURATION_HANDLE ConfigurationHandle,
    _Inout_ PUSBD_INTERFACE_LIST_ENTRY InterfaceList,
    _Outptr_ PURB *Urb)
{
    PUSBDEX_HANDLE_DATA HandleData;

    if (Urb == NULL)
        return STATUS_INVALID_PARAMETER;

    *Urb = NULL;

    HandleData = UsbdexGetHandleData(USBDHandle);
    if (HandleData == NULL || HandleData->Interface.SelectInterfaceXrbAllocateAndBuild == NULL)
        return STATUS_INVALID_PARAMETER;

    return ((PUSBDEX_SELECT_INTERFACE_XRB_ALLOCATE_AND_BUILD)
                HandleData->Interface.SelectInterfaceXrbAllocateAndBuild)(
               HandleData->Interface.USBDInterfaceHandle,
               ConfigurationHandle,
               InterfaceList,
               Urb);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
USBD_UrbFree(
    _In_ USBD_HANDLE USBDHandle,
    _In_ PURB Urb)
{
    PUSBDEX_HANDLE_DATA HandleData;

    HandleData = UsbdexGetHandleData(USBDHandle);
    if (HandleData == NULL || Urb == NULL)
        return;

    ((PUSBDEX_XRB_FREE)HandleData->Interface.XrbFree)(Urb);
}

/**
 * @brief Points an IRP stack location at an URB this library allocated.
 *
 * Argument1 is where every USB stack looks for the URB. A client on the 602
 * contract also leaves it in FileObject, which is how the core stack tells an
 * URB that came from USBD_UrbAllocate from a plain one the caller built.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
USBD_AssignUrbToIoStackLocation(
    _In_ USBD_HANDLE USBDHandle,
    _In_ PIO_STACK_LOCATION IoStackLocation,
    _In_ PURB Urb)
{
    PUSBDEX_HANDLE_DATA HandleData;

    HandleData = UsbdexGetHandleData(USBDHandle);
    if (HandleData == NULL || IoStackLocation == NULL || Urb == NULL)
        return;

    IoStackLocation->Parameters.Others.Argument1 = Urb;

    if (HandleData->ClientContractVersion == USBD_CLIENT_CONTRACT_VERSION_602)
        IoStackLocation->FileObject = (PFILE_OBJECT)Urb;
}

_Must_inspect_result_
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
USBD_QueryUsbCapability(
    _In_ USBD_HANDLE USBDHandle,
    _In_ const GUID *CapabilityType,
    _In_ ULONG OutputBufferLength,
    _Out_writes_bytes_opt_(OutputBufferLength) PUCHAR OutputBuffer,
    _Out_opt_ PULONG ResultLength)
{
    QUERY_USB_CAPABILITY Capability;
    PUSBDEX_HANDLE_DATA HandleData;
    PIO_STACK_LOCATION IoStack;
    PIRP Irp;
    NTSTATUS Status;

    if (ResultLength != NULL)
        *ResultLength = 0;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return STATUS_INVALID_PARAMETER;

    HandleData = UsbdexGetHandleData(USBDHandle);
    if (HandleData == NULL || CapabilityType == NULL)
        return STATUS_INVALID_PARAMETER;

    /* The two have to agree, in both directions */
    if ((OutputBufferLength == 0) != (OutputBuffer == NULL))
        return STATUS_INVALID_PARAMETER;

    RtlZeroMemory(&Capability, sizeof(Capability));
    Capability.Version = QUERY_USB_CAPABILITY_LATEST;
    Capability.Size = sizeof(Capability);
    Capability.USBDIHandle = HandleData->Interface.USBDInterfaceHandle;
    Capability.CapabilityType = *CapabilityType;
    Capability.OutputBufferLength = OutputBufferLength;

    Irp = IoAllocateIrp(HandleData->TargetDeviceObject->StackSize, FALSE);
    if (Irp == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Irp->AssociatedIrp.SystemBuffer = OutputBuffer;

    IoStack = IoGetNextIrpStackLocation(Irp);
    IoStack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    IoStack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_QUERY_USB_CAPABILITY;
    IoStack->Parameters.Others.Argument1 = &Capability;

    Status = UsbdexCallTargetSynchronously(HandleData->TargetDeviceObject, Irp);

    if (NT_SUCCESS(Status) && ResultLength != NULL)
        *ResultLength = Capability.ResultLength;

    return Status;
}
