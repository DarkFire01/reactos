/*
 * PROJECT:         ReactOS PCI Bus Driver
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            drivers/bus/pci/arb/arb_comn.c
 * PURPOSE:         Common Arbitration Code
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

/* INCLUDES *******************************************************************/

#include <ntddk.h>
#include <pci.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

PUCHAR PciArbiterNames[] =
{
    "I/O Port",
    "Memory",
    "Interrupt",
    "Bus Number"
};

/* FUNCTIONS ******************************************************************/
BOOLEAN
PciIsBootAllocatedRange(
    _In_ PPCI_FDO_EXTENSION FdoExt,
    _In_ CM_RESOURCE_TYPE Type,
    _In_ ULONGLONG Start,
    _In_ ULONGLONG Length);

/* Placeholder for future arbitration list construction helper removed for now. */
/*
 * Build simple arbitration list entries for a PDO's resource requirements and
 * invoke Test/Commit on the matching arbiters. This is intentionally minimal:
 * - Only handles port and memory descriptors
 * - Each descriptor becomes a single-alternative ARBITER_LIST_ENTRY
 * - Boot configs (ARBITER_FLAG_BOOT_CONFIG) not distinguished yet
 */
VOID
PciArbBuildAndCommitFromRequirements(
    _In_ PPCI_PDO_EXTENSION PdoExt,
    _In_ PIO_RESOURCE_REQUIREMENTS_LIST ReqList)
{
    ULONG AltCount, iAlt, ReqIndex, ReqPerList;
    PIO_RESOURCE_LIST FirstList;
    PIO_RESOURCE_LIST *AltArray = NULL;
    PDEVICE_OBJECT Pdo;
    PPCI_FDO_EXTENSION Parent;
    PPCI_ARBITER_INSTANCE IoArb = NULL, MemArb = NULL;

    if (!PdoExt || !ReqList) return;
    Parent = PdoExt->ParentFdoExtension;
    if (!Parent) return;
    Pdo = PdoExt->PhysicalDeviceObject;

    AltCount = ReqList->AlternativeLists;
    if (!AltCount) return;

    /* Collect pointers to each IO_RESOURCE_LIST (alternative list) */
    AltArray = ExAllocatePoolWithTag(PagedPool, sizeof(PIO_RESOURCE_LIST) * AltCount, PCI_POOL_TAG);
    if (!AltArray) return;

    FirstList = ReqList->List;
    AltArray[0] = FirstList;
    for (iAlt = 1; iAlt < AltCount; iAlt++)
    {
        /* Advance from previous list using documented layout */
        PIO_RESOURCE_LIST Prev = AltArray[iAlt - 1];
        AltArray[iAlt] = (PIO_RESOURCE_LIST)((PUCHAR)Prev +
                            FIELD_OFFSET(IO_RESOURCE_LIST, Descriptors) +
                            (Prev->Count * sizeof(IO_RESOURCE_DESCRIPTOR)));
    }

    ReqPerList = FirstList->Count; /* assume all lists have same Count as is typical */
    if (!ReqPerList) goto Cleanup;

    /* Locate arbiters once */
    IoArb = (PPCI_ARBITER_INSTANCE)PciFindNextSecondaryExtension(&Parent->SecondaryExtension, PciArb_Io);
    MemArb = (PPCI_ARBITER_INSTANCE)PciFindNextSecondaryExtension(&Parent->SecondaryExtension, PciArb_Memory);
    if (!IoArb && !MemArb) goto Cleanup;

    /* Iterate each requirement index; build alternatives over all alt lists */
    for (ReqIndex = 0; ReqIndex < ReqPerList; ReqIndex++)
    {
        CM_RESOURCE_TYPE Type;
        PIO_RESOURCE_DESCRIPTOR BaseDesc;
        PPCI_ARBITER_INSTANCE TargetArb;
        PARBITER_INSTANCE AInst;
        PIO_RESOURCE_DESCRIPTOR AltBuffer = NULL;
        ULONG ValidAlt = 0;
        LIST_ENTRY ArbHead;
        PARBITER_LIST_ENTRY ArbEntry;
        ULONG Size;

        BaseDesc = &FirstList->Descriptors[ReqIndex];
        Type = BaseDesc->Type;
        if (Type == CmResourceTypeDevicePrivate)
            continue; /* tagging descriptor, skip */
        if (Type != CmResourceTypePort && Type != CmResourceTypeMemory)
            continue; /* not handled */

        TargetArb = (Type == CmResourceTypePort) ? IoArb : MemArb;
        if (!TargetArb) continue;
        AInst = &TargetArb->CommonInstance;

        /* Count how many alternative lists provide a descriptor of same type at this index */
        for (iAlt = 0; iAlt < AltCount; iAlt++)
        {
            PIO_RESOURCE_LIST RL = AltArray[iAlt];
            if (ReqIndex >= RL->Count) continue; /* inconsistent list, skip */
            if (RL->Descriptors[ReqIndex].Type == Type)
                ValidAlt++;
        }
        if (!ValidAlt) continue;

        /* Allocate a contiguous copy of those alternatives */
        Size = ValidAlt * sizeof(IO_RESOURCE_DESCRIPTOR);
        AltBuffer = ExAllocatePoolWithTag(PagedPool, Size, PCI_POOL_TAG);
        if (!AltBuffer) continue;

        ValidAlt = 0;
        for (iAlt = 0; iAlt < AltCount; iAlt++)
        {
            PIO_RESOURCE_LIST RL = AltArray[iAlt];
            if (ReqIndex >= RL->Count) continue;
            if (RL->Descriptors[ReqIndex].Type != Type) continue; /* mismatch */
            RtlCopyMemory(&AltBuffer[ValidAlt], &RL->Descriptors[ReqIndex], sizeof(IO_RESOURCE_DESCRIPTOR));
            ValidAlt++;
        }
        if (!ValidAlt) { ExFreePoolWithTag(AltBuffer, PCI_POOL_TAG); continue; }

        InitializeListHead(&ArbHead);
        ArbEntry = ExAllocatePoolWithTag(PagedPool, sizeof(ARBITER_LIST_ENTRY), PCI_POOL_TAG);
        if (!ArbEntry)
        {
            ExFreePoolWithTag(AltBuffer, PCI_POOL_TAG);
            continue;
        }
        RtlZeroMemory(ArbEntry, sizeof(*ArbEntry));
        InsertTailList(&ArbHead, &ArbEntry->ListEntry);
        ArbEntry->AlternativeCount = ValidAlt;
        ArbEntry->Alternatives = AltBuffer; /* our contiguous copy */
        ArbEntry->PhysicalDeviceObject = Pdo;
        ArbEntry->RequestSource = ArbiterRequestPnpEnumerated;
        ArbEntry->Flags = 0; /* default */
        /* Mark boot-config if descriptor already programmed and in boot ranges */
        if (PdoExt->Resources && BaseDesc->Type != CmResourceTypeNull)
        {
            ULONGLONG start = BaseDesc->u.Generic.MinimumAddress.QuadPart;
            ULONGLONG len = BaseDesc->u.Generic.Length;
            if (PciIsBootAllocatedRange(Parent, BaseDesc->Type, start, len))
            {
                ArbEntry->Flags |= ARBITER_FLAG_BOOT_CONFIG;
            }
        }
        ArbEntry->InterfaceType = ReqList->InterfaceType;
        ArbEntry->SlotNumber = ReqList->SlotNumber;
        ArbEntry->BusNumber = ReqList->BusNumber;
        ArbEntry->Result = ArbiterResultUndefined;

        if (AInst->TestAllocation)
        {
            NTSTATUS St = AInst->TestAllocation(AInst, &ArbHead);
            if (NT_SUCCESS(St))
            {
                if (AInst->CommitAllocation)
                    (VOID)AInst->CommitAllocation(AInst);
            }
            else if (AInst->RollbackAllocation)
            {
                (VOID)AInst->RollbackAllocation(AInst);
            }
        }

        /* Free temp structures; arbiter copied selected ranges internally */
        RemoveEntryList(&ArbEntry->ListEntry);
        ExFreePoolWithTag(ArbEntry, PCI_POOL_TAG);
        ExFreePoolWithTag(AltBuffer, PCI_POOL_TAG);
    }

Cleanup:
    if (AltArray) ExFreePoolWithTag(AltArray, PCI_POOL_TAG);
}

VOID
NTAPI
PciArbiterDestructor(IN PPCI_ARBITER_INSTANCE Arbiter)
{
    PARBITER_INSTANCE A;
    if (!Arbiter) return;
    A = &Arbiter->CommonInstance;
    /* Free ordering lists */
    ArbFreeOrderingList(&A->OrderingList);
    ArbFreeOrderingList(&A->ReservedList);
    /* Free range lists */
    if (A->PossibleAllocation)
    {
        RtlFreeRangeList(A->PossibleAllocation);
        ExFreePoolWithTag(A->PossibleAllocation, TAG_ARB_RANGE);
        A->PossibleAllocation = NULL;
    }
    if (A->Allocation)
    {
        RtlFreeRangeList(A->Allocation);
        ExFreePoolWithTag(A->Allocation, TAG_ARB_RANGE);
        A->Allocation = NULL;
    }
    /* Free allocation stack */
    if (A->AllocationStack)
    {
        ExFreePoolWithTag(A->AllocationStack, TAG_ARB_ALLOCATION);
        A->AllocationStack = NULL;
        A->AllocationStackMaxSize = 0;
    }
    /* Free mutex */
    if (A->MutexEvent)
    {
        ExFreePoolWithTag(A->MutexEvent, TAG_ARBITER);
        A->MutexEvent = NULL;
    }
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
        /* Skip Interrupt arbiter: IRQ arbitration is provided globally by HAL (Hal IRQ Arbiter) */
        if (ArbiterType == PciArb_Interrupt)
        {
            DPRINT("PCI - Skipping per-bus Interrupt arbiter (HAL provides global IRQ arbiter)\n");
            continue;
        }
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
        _swprintf(ArbiterInterface->InstanceName,
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
    /* Log successful arbiter initialization with instance name */
    DPRINT1("PCI - FDO ext 0x%p Arbiter '%S' initialized (ctx 0x%p).\n",
        FdoExtension,
        ArbiterInterface->InstanceName,
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
                if (Arb->Allocation)
                {
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
                }
                if (ArbiterType == PciArb_Io)
                    DeviceExtension->BootRangeSeedMask |= 0x1;
                else if (ArbiterType == PciArb_Memory)
                    DeviceExtension->BootRangeSeedMask |= 0x2;
                if (DeviceExtension->BootRangeSeedMask == 0x3)
                    DeviceExtension->BootRangesSeeded = TRUE; /* both types seeded */
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

VOID
NTAPI
PciArbitersCommitPending(IN PPCI_FDO_EXTENSION FdoExtension)
{
    PSINGLE_LIST_ENTRY link;
    for (link = FdoExtension->SecondaryExtension.Next; link; link = link->Next)
    {
        PPCI_SECONDARY_EXTENSION sec = CONTAINING_RECORD(link, PCI_SECONDARY_EXTENSION, List);
        if (sec->ExtensionType == PciArb_Io || sec->ExtensionType == PciArb_Memory || sec->ExtensionType == PciArb_BusNumber)
        {
            PPCI_ARBITER_INSTANCE inst = (PPCI_ARBITER_INSTANCE)sec;
            PARBITER_INSTANCE a = &inst->CommonInstance;
            if (a->PossibleAllocation && a->PossibleAllocation != a->Allocation)
            {
                /* Commit any pending tested allocation */
                if (a->CommitAllocation)
                {
                    (VOID)a->CommitAllocation(a);
                    DPRINT1("PCI Arbiter '%S' committed pending allocation\n", a->Name);
                }
            }
        }
    }
}

/* EOF */
