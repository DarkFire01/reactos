/*
 * PROJECT:         ReactOS PCI Bus Driver
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            drivers/bus/pci/intrface/devhere.c
 * PURPOSE:         Device Presence Interface
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

/* INCLUDES *******************************************************************/

#include <pci.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

static BOOLEAN
NTAPI
PciIsDevicePresent(
	IN USHORT VendorID,
	IN USHORT DeviceID,
	IN UCHAR RevisionID,
	IN USHORT SubVendorID,
	IN USHORT SubSystemID,
	IN ULONG Flags);

static BOOLEAN
NTAPI
PciIsDevicePresentEx(
	IN PVOID Context,
	IN PPCI_DEVICE_PRESENCE_PARAMETERS Parameters);

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

NTSTATUS
NTAPI
devpresent_Constructor(IN PVOID DeviceExtension,
                       IN PVOID Instance,
                       IN PVOID InterfaceData,
                       IN USHORT Version,
                       IN USHORT Size,
                       IN PINTERFACE Interface)
{
    PAGED_CODE();

    PPCI_PDO_EXTENSION PdoExtension;
    PPCI_DEVICE_PRESENT_INTERFACE Iface;

    UNREFERENCED_PARAMETER(Instance);
    UNREFERENCED_PARAMETER(InterfaceData);
    UNREFERENCED_PARAMETER(Version);

    /* This interface is only valid for PDOs */
    PdoExtension = (PPCI_PDO_EXTENSION)DeviceExtension;
    ASSERT_PDO(PdoExtension);

    /* Validate size */
    if (Size < sizeof(PCI_DEVICE_PRESENT_INTERFACE)) return STATUS_INVALID_PARAMETER;

    /* Initialize the interface */
    Iface = (PPCI_DEVICE_PRESENT_INTERFACE)Interface;
    RtlZeroMemory(Iface, sizeof(*Iface));
    Iface->Size = sizeof(PCI_DEVICE_PRESENT_INTERFACE);
    Iface->Version = PCI_DEVICE_PRESENT_INTERFACE_VERSION;
    Iface->Context = PdoExtension;
    Iface->InterfaceReference = (PINTERFACE_REFERENCE)PciInterface_RefDereference_NoOp;
    Iface->InterfaceDereference = (PINTERFACE_DEREFERENCE)PciInterface_RefDereference_NoOp;

    /* Hook presence query routines. We support the Ex path; the legacy one can delegate. */
    Iface->IsDevicePresent = (PPCI_IS_DEVICE_PRESENT)PciIsDevicePresent;
    Iface->IsDevicePresentEx = (PPCI_IS_DEVICE_PRESENT_EX)PciIsDevicePresentEx;

    return STATUS_SUCCESS;
}

static __forceinline BOOLEAN
PciMatchPresenceByParams(
	IN PPCI_PDO_EXTENSION Pdo,
	IN PPCI_DEVICE_PRESENCE_PARAMETERS Params)
{
	/* Ignore PDOs that are currently not present */
	if (Pdo->NotPresent) return FALSE;

	/* Match by Vendor/Device IDs */
	if (Params->Flags & PCI_USE_VENDEV_IDS)
	{
		if ((Pdo->VendorId != Params->VendorID) ||
		    (Pdo->DeviceId != Params->DeviceID))
			return FALSE;
	}

	/* Match by Revision */
	if (Params->Flags & PCI_USE_REVISION)
	{
		if (Pdo->RevisionId != Params->RevisionID)
			return FALSE;
	}

	/* Match by Subsystem IDs (only meaningful for type-0) */
	if (Params->Flags & PCI_USE_SUBSYSTEM_IDS)
	{
		if ((Pdo->SubsystemVendorId != Params->SubVendorID) ||
		    (Pdo->SubsystemId != Params->SubSystemID))
			return FALSE;
	}

	/* Match by Class/SubClass */
	if (Params->Flags & PCI_USE_CLASS_SUBCLASS)
	{
		if ((Pdo->BaseClass != Params->BaseClass) ||
		    (Pdo->SubClass != Params->SubClass))
			return FALSE;
	}

	/* Match by Programming Interface */
	if (Params->Flags & PCI_USE_PROGIF)
	{
		if (Pdo->ProgIf != Params->ProgIf)
			return FALSE;
	}

	return TRUE;
}

static BOOLEAN
NTAPI
PciIsDevicePresentEx(
	IN PVOID Context,
	IN PPCI_DEVICE_PRESENCE_PARAMETERS Parameters)
{
	PPCI_PDO_EXTENSION ContextPdo = (PPCI_PDO_EXTENSION)Context;
	PPCI_FDO_EXTENSION FdoToScan;
	PPCI_PDO_EXTENSION Pdo;
	PSINGLE_LIST_ENTRY Entry;

	/* Basic parameter validation */
	if (!Parameters) return FALSE;

	/* If the caller requests a local search and provided a PDO context, honor it */
	if ((Parameters->Flags & (PCI_USE_LOCAL_BUS | PCI_USE_LOCAL_DEVICE)) &&
	    (ContextPdo != NULL))
	{
		FdoToScan = ContextPdo->ParentFdoExtension;
		if (!FdoToScan) return FALSE;

		/* Iterate PDOS on this bus */
		for (Pdo = FdoToScan->ChildPdoList; Pdo; Pdo = Pdo->Next)
		{
			/* If limited to the same device, filter by device number */
			if ((Parameters->Flags & PCI_USE_LOCAL_DEVICE) &&
			    (Pdo->Slot.u.bits.DeviceNumber != ContextPdo->Slot.u.bits.DeviceNumber))
				continue;

			if (PciMatchPresenceByParams(Pdo, Parameters)) return TRUE;
		}

		return FALSE;
	}

	/* Global search across all PCI buses */
	for (Entry = PciFdoExtensionListHead.Next; Entry; Entry = Entry->Next)
	{
		FdoToScan = CONTAINING_RECORD(Entry, PCI_FDO_EXTENSION, List);
		for (Pdo = FdoToScan->ChildPdoList; Pdo; Pdo = Pdo->Next)
		{
			if (PciMatchPresenceByParams(Pdo, Parameters)) return TRUE;
		}
	}

	return FALSE;
}

static BOOLEAN
NTAPI
PciIsDevicePresent(
	IN USHORT VendorID,
	IN USHORT DeviceID,
	IN UCHAR RevisionID,
	IN USHORT SubVendorID,
	IN USHORT SubSystemID,
	IN ULONG Flags)
{
	PCI_DEVICE_PRESENCE_PARAMETERS Params;

	RtlZeroMemory(&Params, sizeof(Params));
	Params.Size = sizeof(Params);
	Params.Flags = Flags;
	Params.VendorID = VendorID;
	Params.DeviceID = DeviceID;
	Params.RevisionID = RevisionID;
	Params.SubVendorID = SubVendorID;
	Params.SubSystemID = SubSystemID;

	/* Class/Subclass/ProgIf are only compared if corresponding flags are set */
	return PciIsDevicePresentEx(NULL, &Params);
}

/* EOF */
