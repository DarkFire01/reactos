/*
 * PROJECT:     ReactOS HID Stack
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Pass-through HID to KMDF filter driver
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

/*
 * mshidkmdf is the shim that lets a *KMDF* driver act as a HID minidriver under
 * the *WDM* HIDCLASS. It registers with hidclass, so hidclass builds the HID FDO
 * and owns its dispatch table; mshidkmdf then forwards every IRP that reaches
 * that FDO straight down to the KMDF driver below, which answers the IOCTL_HID_*
 * set itself. For HID-over-I2C the stack ends up as:
 *
 *     ACPI PDO -> hidi2c (KMDF FDO) -> mshidkmdf (hidclass FDO) -> hidclass PDOs
 *
 * The driver owns no state of its own. Its whole job is the pass-through plus one
 * handshake: at AddDevice it asks the stack below for
 * GUID_WDF_HID_INTERFACE_STANDARD, handing over hidclass's HidNotifyPresence so
 * the KMDF driver can report presence changes back up.
 *
 * Every function below is 1:1 with the reference. There is deliberately no error
 * path around the query, see HidKmdfAddDevice.
 */

#include <ntddk.h>
#include <initguid.h>

/*
 * This is the driver's only TU, so pulling <initguid.h> in first is what makes
 * the header's DEFINE_GUID emit storage for GUID_WDF_HID_INTERFACE_STANDARD
 * rather than just declare it. Same mechanism as acpi_new's guid.c, which needs
 * a TU of its own only because that driver has more than one.
 */
#include "mshidkmdf.h"

static DRIVER_ADD_DEVICE HidKmdfAddDevice;
static DRIVER_DISPATCH HidKmdfPassThrough;
static DRIVER_DISPATCH HidKmdfPowerPassThrough;
static DRIVER_DISPATCH HidKmdfPnp;
static DRIVER_UNLOAD HidKmdfUnload;

/*
 * The callback we publish downwards. It exists only to give
 * the KMDF driver a plain function pointer for hidclass's export.
 */
static
NTSTATUS
NTAPI
HidKmdfNotifyPresence(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ BOOLEAN IsPresent)
{
    return HidNotifyPresence(DeviceObject, IsPresent);
}

static
NTSTATUS
NTAPI
HidKmdfPassThrough(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PHID_DEVICE_EXTENSION HidDeviceExtension = DeviceObject->DeviceExtension;

    IoCopyCurrentIrpStackLocationToNext(Irp);
    return IoCallDriver(HidDeviceExtension->NextDeviceObject, Irp);
}

static
NTSTATUS
NTAPI
HidKmdfPowerPassThrough(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PHID_DEVICE_EXTENSION HidDeviceExtension = DeviceObject->DeviceExtension;

    PoStartNextPowerIrp(Irp);
    IoCopyCurrentIrpStackLocationToNext(Irp);
    return PoCallDriver(HidDeviceExtension->NextDeviceObject, Irp);
}

/*
 * hidclass calls this with the FDO it just built, so DeviceObject->DeviceExtension
 * is a HID_DEVICE_EXTENSION and its MiniDeviceExtension is the DeviceExtensionSize
 * bytes we asked for in DriverEntry, which is exactly the interface block.
 *
 * A stack with no responder is not a failure: the reference ignores the status,
 * wipes the block and returns STATUS_SUCCESS regardless, leaving the presence
 * notification path simply unavailable. Do not "fix" this into an error path --
 * ReactOS's KMDF answers no such interface today, so every device would then fail
 * to start. HidKmdfPnp keys off Size/Version to tell a live block from a wiped one.
 */
static
NTSTATUS
NTAPI
HidKmdfAddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT DeviceObject)
{
    PHID_DEVICE_EXTENSION HidDeviceExtension = DeviceObject->DeviceExtension;
    PWDF_HID_INTERFACE HidInterface = HidDeviceExtension->MiniDeviceExtension;
    PIO_STACK_LOCATION IoStack;
    PIRP Irp;

    UNREFERENCED_PARAMETER(DriverObject);

    /* Publish our half of the block before asking for the responder's. */
    RtlZeroMemory(HidInterface, sizeof(WDF_HID_INTERFACE));
    HidInterface->Interface.Size = sizeof(WDF_HID_INTERFACE);
    HidInterface->Interface.Version = WDF_HID_INTERFACE_VERSION;
    HidInterface->NotifyPresence = HidKmdfNotifyPresence;
    HidInterface->HidClassDeviceObject = DeviceObject;

    Irp = IoAllocateIrp(HidDeviceExtension->NextDeviceObject->StackSize + 1, FALSE);
    if (Irp != NULL)
    {
        /*
         * IoAllocateIrp leaves the current location one past the top; step onto
         * the location we over-allocated, so IoForwardIrpSynchronously has one to
         * copy down from.
         */
        IoSetNextIrpStackLocation(Irp);
        Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;

        IoStack = IoGetCurrentIrpStackLocation(Irp);
        RtlZeroMemory(IoStack, sizeof(IO_STACK_LOCATION));
        IoStack->MajorFunction = IRP_MJ_PNP;
        IoStack->MinorFunction = IRP_MN_QUERY_INTERFACE;
        IoStack->Parameters.QueryInterface.InterfaceType = &GUID_WDF_HID_INTERFACE_STANDARD;
        IoStack->Parameters.QueryInterface.Size = sizeof(WDF_HID_INTERFACE);
        IoStack->Parameters.QueryInterface.Version = WDF_HID_INTERFACE_VERSION;
        IoStack->Parameters.QueryInterface.Interface = &HidInterface->Interface;
        IoStack->Parameters.QueryInterface.InterfaceSpecificData = NULL;

        IoForwardIrpSynchronously(HidDeviceExtension->NextDeviceObject, Irp);

        if (!NT_SUCCESS(Irp->IoStatus.Status))
            RtlZeroMemory(HidInterface, sizeof(WDF_HID_INTERFACE));

        IoFreeIrp(Irp);
    }

    DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

/*
 * Identical to the plain pass-through except that IRP_MN_REMOVE_DEVICE first
 * releases the interface, and only if AddDevice actually acquired one: a wiped
 * block reads back Size == 0, so the Size/Version pair is the liveness test.
 */
static
NTSTATUS
NTAPI
HidKmdfPnp(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PHID_DEVICE_EXTENSION HidDeviceExtension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    PWDF_HID_INTERFACE HidInterface;

    IoCopyCurrentIrpStackLocationToNext(Irp);

    if (IoStack->MinorFunction == IRP_MN_REMOVE_DEVICE)
    {
        HidInterface = HidDeviceExtension->MiniDeviceExtension;

        if (HidInterface->Interface.Size == sizeof(WDF_HID_INTERFACE) &&
            HidInterface->Interface.Version == WDF_HID_INTERFACE_VERSION)
        {
            HidInterface->Interface.InterfaceDereference(HidInterface->Interface.Context);
            RtlZeroMemory(HidInterface, sizeof(WDF_HID_INTERFACE));
        }
    }

    return IoCallDriver(HidDeviceExtension->NextDeviceObject, Irp);
}

/*
 * Empty on purpose. It only has to exist because hidclass saves DriverUnload at
 * registration time and chains to it.
 */
static
VOID
NTAPI
HidKmdfUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
}

/*
 * The dispatch table set up here is what hidclass copies aside inside
 * HidRegisterMinidriver before overwriting the live one with its own; hidclass
 * calls back into these once it has handled its own layer.
 */
CODE_SEG("INIT")
NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    HID_MINIDRIVER_REGISTRATION Registration;
    ULONG Index;

    for (Index = 0; Index <= IRP_MJ_MAXIMUM_FUNCTION; Index++)
        DriverObject->MajorFunction[Index] = HidKmdfPassThrough;

    DriverObject->MajorFunction[IRP_MJ_POWER] = HidKmdfPowerPassThrough;
    DriverObject->MajorFunction[IRP_MJ_PNP] = HidKmdfPnp;
    DriverObject->DriverExtension->AddDevice = HidKmdfAddDevice;
    DriverObject->DriverUnload = HidKmdfUnload;

    RtlZeroMemory(&Registration, sizeof(Registration));
    Registration.Revision = HID_REVISION;
    Registration.DriverObject = DriverObject;
    Registration.RegistryPath = RegistryPath;
    /* The reference hardcodes 48, its x64 sizeof of the interface block. */
    Registration.DeviceExtensionSize = sizeof(WDF_HID_INTERFACE);
    Registration.DevicesArePolled = FALSE;

    return HidRegisterMinidriver(&Registration);
}
