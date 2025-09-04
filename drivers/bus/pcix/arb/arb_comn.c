/*
 * PROJECT:         ReactOS PCI Bus Driver
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            drivers/bus/pci/arb/arb_comn.c
 * PURPOSE:         Common Arbitration Code
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

/* INCLUDES *******************************************************************/

#include <pci.h>

/* Forward (header may not have parsed yet for lint) */
typedef struct _PCI_FDO_EXTENSION *PPCI_FDO_EXTENSION;

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

PCHAR PciArbiterNames[] =
{
    "I/O Port",
    "Memory",
    "Interrupt",
    "Bus Number"
};

/* FUNCTIONS ******************************************************************/


VOID
NTAPI
PciArbiterDestructor(IN PPCI_ARBITER_INSTANCE Arbiter)
{
    UNREFERENCED_PARAMETER(Arbiter);
    /* This function is not yet implemented */
    UNIMPLEMENTED;
    while (TRUE);
}

NTSTATUS
NTAPI
PciInitializeArbiters(IN PPCI_FDO_EXTENSION FdoExtension)
{
    PPCI_INTERFACE CurrentInterface, *Interfaces;
    PPCI_PDO_EXTENSION PdoExtension;
    PPCI_ARBITER_INSTANCE ArbiterInterface;
    NTSTATUS Status;
    PCI_SIGNATURE ArbiterType;
    ASSERT_FDO(FdoExtension);

    /* Loop all the arbiters */
    for (ArbiterType = PciArb_Io; ArbiterType <= PciArb_BusNumber; ArbiterType++)
    {
        /* Check if this is the extension for the Root PCI Bus */
        if (!PCI_IS_ROOT_FDO(FdoExtension))
        {
            /* Get the PDO extension */
            PdoExtension = FdoExtension->PhysicalDeviceObject->DeviceExtension;
            ASSERT_PDO(PdoExtension);

            /* Skip this bus if it does subtractive decode */
            if (PdoExtension->Dependent.type1.SubtractiveDecode)
            {
                DPRINT1("PCI Not creating arbiters for subtractive bus %u\n",
                        PdoExtension->Dependent.type1.SubtractiveDecode);
                continue;
            }
        }

        /* Query all the registered arbiter interfaces */
        Interfaces = PciInterfaces;
        while (*Interfaces)
        {
            /* Find the one that matches the arbiter currently being setup */
            CurrentInterface = *Interfaces;
            if (CurrentInterface->Signature == ArbiterType) break;
            Interfaces++;
        }

        /* Check if the required arbiter was not found in the list */
        if (!*Interfaces)
        {
            /* Skip this arbiter and try the next one */
            DPRINT1("PCI - FDO ext 0x%p no %s arbiter.\n",
                    FdoExtension,
                    PciArbiterNames[ArbiterType - PciArb_Io]);
            continue;
        }

        /* An arbiter was found, allocate an instance for it */
        Status = STATUS_INSUFFICIENT_RESOURCES;
        ArbiterInterface = ExAllocatePoolWithTag(PagedPool,
                                                 sizeof(PCI_ARBITER_INSTANCE),
                                                 PCI_POOL_TAG);
        if (!ArbiterInterface) break;

        /* Setup the instance */
        ArbiterInterface->BusFdoExtension = FdoExtension;
        ArbiterInterface->Interface = CurrentInterface;
        swprintf(ArbiterInterface->InstanceName,
                 L"PCI %S (b=%02x)",
                 PciArbiterNames[ArbiterType - PciArb_Io],
                 FdoExtension->BaseBus);

        /* Call the interface initializer for it */
        Status = CurrentInterface->Initializer(ArbiterInterface);
        if (!NT_SUCCESS(Status)) break;

        /* Link it with this FDO */
        PcipLinkSecondaryExtension(&FdoExtension->SecondaryExtension,
                                   &FdoExtension->SecondaryExtLock,
                                   &ArbiterInterface->Header,
                                   ArbiterType,
                                   PciArbiterDestructor);

        /* This arbiter is now initialized, move to the next one */
        DPRINT1("PCI - FDO ext 0x%p %S arbiter initialized (context 0x%p).\n",
                FdoExtension,
                L"ARBITER HEADER MISSING", //ArbiterInterface->CommonInstance.Name,
                ArbiterInterface);
        Status = STATUS_SUCCESS;
    }

    /* Return to caller */
    return Status;
}

NTSTATUS
NTAPI
PciInitializeArbiterRanges(IN PPCI_FDO_EXTENSION DeviceExtension,
                           IN PCM_RESOURCE_LIST Resources)
{
    PPCI_PDO_EXTENSION PdoExtension;
    //CM_RESOURCE_TYPE DesiredType;
    PVOID Instance;
    PCI_SIGNATURE ArbiterType;

    UNREFERENCED_PARAMETER(Resources);

    /* If this is the root and we have boot resources not yet parsed, do a simple parse now.
       Later these will be used to add fixed allocations into the arbiter once implemented. */
    if (PCI_IS_ROOT_FDO(DeviceExtension) &&
        DeviceExtension->BootResources &&
        IsListEmpty(&DeviceExtension->BootRangeList))
    {
        PCM_RESOURCE_LIST Boot = DeviceExtension->BootResources;
        ULONG iFull; PCM_FULL_RESOURCE_DESCRIPTOR Full;
        Full = &Boot->List[0];
        for (iFull = 0; iFull < Boot->Count; iFull++)
        {
            ULONG iPart; PCM_PARTIAL_RESOURCE_DESCRIPTOR Part;
            Part = &Full->PartialResourceList.PartialDescriptors[0];
            for (iPart = 0; iPart < Full->PartialResourceList.Count; iPart++, Part++)
            {
                if ((Part->Type == CmResourceTypePort) || (Part->Type == CmResourceTypeMemory))
                {
                    PPCI_BOOT_RANGE Range = ExAllocatePoolWithTag(PagedPool, sizeof(PCI_BOOT_RANGE), PCI_POOL_TAG);
                    if (!Range) continue;
                    Range->Type = Part->Type;
                    if (Part->Type == CmResourceTypePort)
                    {
                        Range->Start = Part->u.Port.Start.QuadPart;
                        Range->Length = Part->u.Port.Length;
                        Range->Prefetchable = FALSE;
                    }
                    else
                    {
                        Range->Start = Part->u.Memory.Start.QuadPart;
                        Range->Length = Part->u.Memory.Length;
                        Range->Prefetchable = (Part->Flags & CM_RESOURCE_MEMORY_PREFETCHABLE) ? TRUE : FALSE;
                    }
                    InsertTailList(&DeviceExtension->BootRangeList, &Range->ListEntry);
                }
            }
            /* Advance to next full descriptor */
            Full = (PCM_FULL_RESOURCE_DESCRIPTOR)((PUCHAR)Full +
                    FIELD_OFFSET(CM_FULL_RESOURCE_DESCRIPTOR, PartialResourceList.PartialDescriptors) +
                    (Full->PartialResourceList.Count * sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR)));
        }
        if (!IsListEmpty(&DeviceExtension->BootRangeList) && !DeviceExtension->BootResourcesPersisted)
        {
            /* Standard persistence via IoReportResourceUsage (raw == translated for now) */
            NTSTATUS st = IoReportResourceUsage(NULL, NULL, DeviceExtension->BootResources, DeviceExtension->BootResourcesSize,
                                                NULL, DeviceExtension->BootResources, DeviceExtension->BootResourcesSize, FALSE, NULL);
            if (NT_SUCCESS(st)) DeviceExtension->BootResourcesPersisted = TRUE;
            DPRINT1("PCI Root Boot resource summary: persisted=%d st=0x%lx\n", DeviceExtension->BootResourcesPersisted, st);
        }
    }

    /* Arbiters should not already be initialized */
    if (DeviceExtension->ArbitersInitialized)
    {
        /* Duplicated start request, fail initialization */
        DPRINT1("PCI Warning hot start FDOx %p, resource ranges not checked.\n", DeviceExtension);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    /* Check for non-root FDO */
    if (!PCI_IS_ROOT_FDO(DeviceExtension))
    {
        /* Grab the PDO */
        PdoExtension = (PPCI_PDO_EXTENSION)DeviceExtension->PhysicalDeviceObject->DeviceExtension;
        ASSERT_PDO(PdoExtension);

        /* Check if this is a subtractive bus */
        if (PdoExtension->Dependent.type1.SubtractiveDecode)
        {
            /* There is nothing to do regarding arbitration of resources */
            DPRINT1("PCI Skipping arbiter initialization for subtractive bridge FDOX %p\n", DeviceExtension);
            return STATUS_SUCCESS;
        }
    }

    /* Loop all arbiters */
    for (ArbiterType = PciArb_Io; ArbiterType <= PciArb_Memory; ArbiterType++)
    {
        /* Pick correct resource type for each arbiter */
        if (ArbiterType == PciArb_Io)
        {
            /* I/O Port */
            //DesiredType = CmResourceTypePort;
        }
        else if (ArbiterType == PciArb_Memory)
        {
            /* Device RAM */
            //DesiredType = CmResourceTypeMemory;
        }
        else
        {
            /* Ignore anything else */
            continue;
        }

        /* Find an arbiter of this type */
        Instance = PciFindNextSecondaryExtension(&DeviceExtension->SecondaryExtension,
                                                 ArbiterType);
        if (Instance)
        {
            /*
             * Now we should initialize it, not yet implemented because Arb
             * library isn't yet implemented, not even the headers.
             */
            /* Seed boot reservations after arbiter init (memory & port only, once) */
            if (PCI_IS_ROOT_FDO(DeviceExtension) && !DeviceExtension->BootRangesSeeded)
            {
                PARBITER_INSTANCE Arb = &((PPCI_ARBITER_INSTANCE)Instance)->CommonInstance;
                PLIST_ENTRY le;
                for (le = DeviceExtension->BootRangeList.Flink; le != &DeviceExtension->BootRangeList; le = le->Flink)
                {
                    PPCI_BOOT_RANGE br = CONTAINING_RECORD(le, PCI_BOOT_RANGE, ListEntry);
                    if ((ArbiterType == PciArb_Io && br->Type == CmResourceTypePort) ||
                        (ArbiterType == PciArb_Memory && br->Type == CmResourceTypeMemory))
                    {
                        (VOID)RtlAddRange(Arb->Allocation,
                                          br->Start,
                                          br->Start + br->Length - 1,
                                          0,
                                          RTL_RANGE_LIST_ADD_IF_CONFLICT,
                                          NULL,
                                          NULL);
                    }
                }
                if (ArbiterType == PciArb_Memory || ArbiterType == PciArb_Io)
                    DeviceExtension->BootRangesSeeded = TRUE; /* After both loops first pass */
            }
        }
        else
        {
            /* The arbiter was not found, this is an error! */
            DPRINT1("PCI - FDO ext 0x%p %s arbiter (REQUIRED) is missing.\n",
                    DeviceExtension,
                    PciArbiterNames[ArbiterType - PciArb_Io]);
        }
    }

    /* Arbiters are now initialized */
    DeviceExtension->ArbitersInitialized = TRUE;
    return STATUS_SUCCESS;
}

/* EOF */
