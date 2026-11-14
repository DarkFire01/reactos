/*
 * PROJECT:     ReactOS HID Stack
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Pass-through HID to KMDF filter driver
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

/*
 * Registers with hidclass as a HID minidriver and forwards every IRP to the
 * KMDF driver below, which handles the IOCTL_HID_* set itself.
 */

#include <ntddk.h>
#include <initguid.h>
#include "mshidkmdf.h"

DRIVER_INITIALIZE DriverEntry;
static DRIVER_ADD_DEVICE MsHidKmdfAddDevice;
static DRIVER_DISPATCH MsHidKmdfDispatchForward;
static DRIVER_DISPATCH MsHidKmdfDispatchPower;
static DRIVER_DISPATCH MsHidKmdfDispatchPnp;
static DRIVER_UNLOAD MsHidKmdfUnload;
static HIDKMDF_NOTIFY_PRESENCE MsHidKmdfNotifyPresence;

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#endif

static
FORCEINLINE
PDEVICE_OBJECT
MsHidKmdfLowerDevice(
    _In_ PDEVICE_OBJECT DeviceObject)
{
    return ((PHID_DEVICE_EXTENSION)DeviceObject->DeviceExtension)->NextDeviceObject;
}

static
FORCEINLINE
PHIDKMDF_PRESENCE_INTERFACE
MsHidKmdfInterfaceBlock(
    _In_ PDEVICE_OBJECT DeviceObject)
{
    return ((PHID_DEVICE_EXTENSION)DeviceObject->DeviceExtension)->MiniDeviceExtension;
}

static
NTSTATUS
NTAPI
MsHidKmdfNotifyPresence(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ BOOLEAN IsPresent)
{
    return HidNotifyPresence(DeviceObject, IsPresent);
}

/**
 * @brief Sends IRP_MN_QUERY_INTERFACE for the presence interface down the stack.
 */
static
NTSTATUS
NTAPI
MsHidKmdfQueryPresenceInterface(
    _In_ PDEVICE_OBJECT LowerDevice,
    _Inout_ PHIDKMDF_PRESENCE_INTERFACE Block)
{
    PIO_STACK_LOCATION Stack;
    NTSTATUS Status;
    PIRP Irp;

    /* One extra location so IoForwardIrpSynchronously has a current one to copy */
    Irp = IoAllocateIrp(LowerDevice->StackSize + 1, FALSE);
    if (Irp == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    IoSetNextIrpStackLocation(Irp);
    Stack = IoGetCurrentIrpStackLocation(Irp);
    RtlZeroMemory(Stack, sizeof(*Stack));

    Stack->MajorFunction = IRP_MJ_PNP;
    Stack->MinorFunction = IRP_MN_QUERY_INTERFACE;
    Stack->Parameters.QueryInterface.InterfaceType = &GUID_HIDKMDF_PRESENCE_INTERFACE;
    Stack->Parameters.QueryInterface.Size = sizeof(*Block);
    Stack->Parameters.QueryInterface.Version = HIDKMDF_PRESENCE_INTERFACE_VERSION;
    Stack->Parameters.QueryInterface.Interface = &Block->Interface;
    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;

    IoForwardIrpSynchronously(LowerDevice, Irp);
    Status = Irp->IoStatus.Status;
    IoFreeIrp(Irp);

    return Status;
}

/**
 * @brief Publishes our presence callback and binds the interface from below.
 *
 * A stack with no responder still starts; the block is left zeroed.
 */
static
NTSTATUS
NTAPI
MsHidKmdfAddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT DeviceObject)
{
    PHIDKMDF_PRESENCE_INTERFACE Block = MsHidKmdfInterfaceBlock(DeviceObject);
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(DriverObject);

    RtlZeroMemory(Block, sizeof(*Block));
    Block->HidClassDevice = DeviceObject;
    Block->NotifyPresence = MsHidKmdfNotifyPresence;
    Block->Interface.Version = HIDKMDF_PRESENCE_INTERFACE_VERSION;
    Block->Interface.Size = sizeof(*Block);

    Status = MsHidKmdfQueryPresenceInterface(MsHidKmdfLowerDevice(DeviceObject), Block);
    if (!NT_SUCCESS(Status))
        RtlZeroMemory(Block, sizeof(*Block));

    DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
MsHidKmdfDispatchForward(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    IoCopyCurrentIrpStackLocationToNext(Irp);
    return IoCallDriver(MsHidKmdfLowerDevice(DeviceObject), Irp);
}

static
NTSTATUS
NTAPI
MsHidKmdfDispatchPower(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PoStartNextPowerIrp(Irp);
    IoCopyCurrentIrpStackLocationToNext(Irp);
    return PoCallDriver(MsHidKmdfLowerDevice(DeviceObject), Irp);
}

/**
 * @brief Releases a bound presence interface before removal goes down the stack.
 */
static
NTSTATUS
NTAPI
MsHidKmdfDispatchPnp(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PHIDKMDF_PRESENCE_INTERFACE Block;

    if (IoGetCurrentIrpStackLocation(Irp)->MinorFunction != IRP_MN_REMOVE_DEVICE)
        return MsHidKmdfDispatchForward(DeviceObject, Irp);

    Block = MsHidKmdfInterfaceBlock(DeviceObject);
    if (Block->Interface.Version == HIDKMDF_PRESENCE_INTERFACE_VERSION &&
        Block->Interface.Size == sizeof(*Block) &&
        Block->Interface.InterfaceDereference != NULL)
    {
        Block->Interface.InterfaceDereference(Block->Interface.Context);
        RtlZeroMemory(Block, sizeof(*Block));
    }

    return MsHidKmdfDispatchForward(DeviceObject, Irp);
}

/* hidclass chains to the unload routine it saw at registration */
static
VOID
NTAPI
MsHidKmdfUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    HID_MINIDRIVER_REGISTRATION Registration;
    ULONG Major;

    DriverObject->DriverUnload = MsHidKmdfUnload;
    DriverObject->DriverExtension->AddDevice = MsHidKmdfAddDevice;

    for (Major = 0; Major <= IRP_MJ_MAXIMUM_FUNCTION; Major++)
    {
        if (Major == IRP_MJ_PNP)
            DriverObject->MajorFunction[Major] = MsHidKmdfDispatchPnp;
        else if (Major == IRP_MJ_POWER)
            DriverObject->MajorFunction[Major] = MsHidKmdfDispatchPower;
        else
            DriverObject->MajorFunction[Major] = MsHidKmdfDispatchForward;
    }

    RtlZeroMemory(&Registration, sizeof(Registration));
    Registration.Revision = HID_REVISION;
    Registration.DriverObject = DriverObject;
    Registration.RegistryPath = RegistryPath;
    Registration.DeviceExtensionSize = sizeof(HIDKMDF_PRESENCE_INTERFACE);
    Registration.DevicesArePolled = FALSE;

    return HidRegisterMinidriver(&Registration);
}
