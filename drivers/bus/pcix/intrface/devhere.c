/*
 * PROJECT:         ReactOS PCI Bus Driver
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            drivers/bus/pci/intrface/devhere.c
 * PURPOSE:         Device Presence Interface
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

/*
 * Some drivers have to behave differently depending on what else is in the
 * machine - a chipset erratum that only bites next to a particular bridge, a
 * feature that only works with a matching companion part. This interface is how
 * they ask, without having to walk configuration space themselves.
 */

/* INCLUDES *******************************************************************/

#include <pci.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

PCI_INTERFACE PciDevicePresentInterface =
{
    &GUID_PCI_DEVICE_PRESENT_INTERFACE,
    sizeof(PCI_DEVICE_PRESENT_INTERFACE),
    PCI_DEVICE_PRESENT_INTERFACE_VERSION,
    PCI_DEVICE_PRESENT_INTERFACE_VERSION,
    PCI_INTERFACE_PDO,
    0,
    PciInterface_DevicePresent,
    devpresent_Constructor,
    devpresent_Initializer
};

/* FUNCTIONS ******************************************************************/

NTSTATUS
NTAPI
devpresent_Initializer(IN PVOID Instance)
{
    UNREFERENCED_PARAMETER(Instance);
    /* PnP Interfaces don't get Initialized */
    ASSERTMSG("PCI devpresent_Initializer, unexpected call.\n", FALSE);
    return STATUS_UNSUCCESSFUL;
}

VOID
NTAPI
PciDevicePresentInterface_Reference(IN PVOID Context)
{
    PPCI_PDO_EXTENSION PdoExtension = (PPCI_PDO_EXTENSION)Context;

    InterlockedIncrement(&PdoExtension->BusInterfaceReferenceCount);
}

VOID
NTAPI
PciDevicePresentInterface_Dereference(IN PVOID Context)
{
    PPCI_PDO_EXTENSION PdoExtension = (PPCI_PDO_EXTENSION)Context;

    InterlockedDecrement(&PdoExtension->BusInterfaceReferenceCount);
}

/*
 * Decide whether one enumerated device is the one being asked about. Only the
 * fields the caller said to compare are compared, so a question as broad as
 * "is there anything of this class" and one as narrow as a single subsystem
 * revision both work.
 */
static
BOOLEAN
NTAPI
PciIsDeviceMatch(IN PPCI_PDO_EXTENSION PdoExtension,
                 IN PPCI_DEVICE_PRESENCE_PARAMETERS Parameters)
{
    /* A device that has gone away does not count as present */
    if (PdoExtension->ReportedMissing) return FALSE;

    if (Parameters->Flags & PCI_USE_VENDEV_IDS)
    {
        if (PdoExtension->VendorId != Parameters->VendorID) return FALSE;
        if (PdoExtension->DeviceId != Parameters->DeviceID) return FALSE;
    }

    if (Parameters->Flags & PCI_USE_CLASS_SUBCLASS)
    {
        if (PdoExtension->BaseClass != Parameters->BaseClass) return FALSE;
        if (PdoExtension->SubClass != Parameters->SubClass) return FALSE;
    }

    if (Parameters->Flags & PCI_USE_PROGIF)
    {
        if (PdoExtension->ProgIf != Parameters->ProgIf) return FALSE;
    }

    if (Parameters->Flags & PCI_USE_SUBSYSTEM_IDS)
    {
        if (PdoExtension->SubsystemVendorId != Parameters->SubVendorID) return FALSE;
        if (PdoExtension->SubsystemId != Parameters->SubSystemID) return FALSE;
    }

    if (Parameters->Flags & PCI_USE_REVISION)
    {
        if (PdoExtension->RevisionId != Parameters->RevisionID) return FALSE;
    }

    return TRUE;
}

/*
 * Look for a device answering the description. The caller may narrow the
 * search to the bus the asking device is on, or to that device alone.
 */
static
BOOLEAN
NTAPI
PciIsDevicePresentWorker(IN PPCI_PDO_EXTENSION Caller,
                         IN PPCI_DEVICE_PRESENCE_PARAMETERS Parameters)
{
    PPCI_FDO_EXTENSION FdoExtension;
    PPCI_PDO_EXTENSION PdoExtension;
    BOOLEAN Found = FALSE;
    PAGED_CODE();

    /* Asking only about the device that asked needs no search at all */
    if (Parameters->Flags & PCI_USE_LOCAL_DEVICE)
    {
        return (Caller) ? PciIsDeviceMatch(Caller, Parameters) : FALSE;
    }

    /* Take the lock, since this walks the buses and their children */
    KeEnterCriticalRegion();
    KeWaitForSingleObject(&PciGlobalLock, Executive, KernelMode, FALSE, NULL);

    for (FdoExtension = (PPCI_FDO_EXTENSION)PciFdoExtensionListHead.Next;
         FdoExtension;
         FdoExtension = (PPCI_FDO_EXTENSION)FdoExtension->List.Next)
    {
        /* A search kept to one bus skips every other one */
        if ((Parameters->Flags & PCI_USE_LOCAL_BUS) &&
            ((!Caller) || (Caller->ParentFdoExtension != FdoExtension)))
        {
            continue;
        }

        for (PdoExtension = FdoExtension->ChildPdoList;
             PdoExtension;
             PdoExtension = PdoExtension->Next)
        {
            if (PciIsDeviceMatch(PdoExtension, Parameters))
            {
                Found = TRUE;
                break;
            }
        }

        if (Found) break;
    }

    KeSetEvent(&PciGlobalLock, IO_NO_INCREMENT, FALSE);
    KeLeaveCriticalRegion();
    return Found;
}

BOOLEAN
NTAPI
PciIsDevicePresent(IN USHORT VendorID,
                   IN USHORT DeviceID,
                   IN UCHAR RevisionID,
                   IN USHORT SubVendorID,
                   IN USHORT SubSystemID,
                   IN ULONG Flags)
{
    PCI_DEVICE_PRESENCE_PARAMETERS Parameters;
    PAGED_CODE();

    /*
     * This form of the question names its fields directly rather than through
     * a parameter block, and the vendor and device are always part of it.
     */
    RtlZeroMemory(&Parameters, sizeof(Parameters));
    Parameters.Size = sizeof(Parameters);
    Parameters.Flags = Flags | PCI_USE_VENDEV_IDS;
    Parameters.VendorID = VendorID;
    Parameters.DeviceID = DeviceID;
    Parameters.RevisionID = RevisionID;
    Parameters.SubVendorID = SubVendorID;
    Parameters.SubSystemID = SubSystemID;

    return PciIsDevicePresentWorker(NULL, &Parameters);
}

BOOLEAN
NTAPI
PciIsDevicePresentEx(IN PVOID Context,
                     IN PPCI_DEVICE_PRESENCE_PARAMETERS Parameters)
{
    PAGED_CODE();

    /* A parameter block from a newer caller than this driver knows about */
    if (Parameters->Size < sizeof(PCI_DEVICE_PRESENCE_PARAMETERS)) return FALSE;

    return PciIsDevicePresentWorker((PPCI_PDO_EXTENSION)Context, Parameters);
}

NTSTATUS
NTAPI
devpresent_Constructor(IN PVOID DeviceExtension,
                       IN PVOID Instance,
                       IN PVOID InterfaceData,
                       IN USHORT Version,
                       IN USHORT Size,
                       IN PINTERFACE Interface)
{
    PPCI_DEVICE_PRESENT_INTERFACE PresentInterface =
        (PPCI_DEVICE_PRESENT_INTERFACE)Interface;
    PAGED_CODE();

    UNREFERENCED_PARAMETER(Instance);
    UNREFERENCED_PARAMETER(InterfaceData);
    UNREFERENCED_PARAMETER(Version);
    UNREFERENCED_PARAMETER(Size);

    PresentInterface->Size = sizeof(PCI_DEVICE_PRESENT_INTERFACE);
    PresentInterface->Version = PCI_DEVICE_PRESENT_INTERFACE_VERSION;
    PresentInterface->Context = DeviceExtension;
    PresentInterface->InterfaceReference = PciDevicePresentInterface_Reference;
    PresentInterface->InterfaceDereference = PciDevicePresentInterface_Dereference;
    PresentInterface->IsDevicePresent = PciIsDevicePresent;
    PresentInterface->IsDevicePresentEx = PciIsDevicePresentEx;
    return STATUS_SUCCESS;
}

/* EOF */
