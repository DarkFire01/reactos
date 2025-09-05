/*
 * PROJECT:         ReactOS PCI Bus Driver
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            drivers/bus/pci/arb/ar_memiono.c
 * PURPOSE:         Memory and I/O Port Resource Arbitration
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

/* INCLUDES *******************************************************************/

#include <ntddk.h>
#include <pci.h>
#include <limits.h>


#define NDEBUG
#include <debug.h>

NTSTATUS
NTAPI
ArbCommitAllocation(
    _In_ PARBITER_INSTANCE Arbiter);
/* LOCAL ARBITER HELPERS (cannot rely on ntoskrnl IopGeneric* exports) */
NTSTATUS NTAPI PciArbUnpackRequirement(
    PIO_RESOURCE_DESCRIPTOR Requirement,
    PULONGLONG MinimumAddress,
    PULONGLONG MaximumAddress,
    PULONG Length,
    PULONG Alignment);
NTSTATUS NTAPI PciArbPackResource(
    PIO_RESOURCE_DESCRIPTOR Requirement,
    ULONGLONG Start,
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor);
NTSTATUS NTAPI PciArbUnpackResource(
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor,
    PULONGLONG Start,
    PULONG Length);
LONG NTAPI PciArbScoreRequirement(PIO_RESOURCE_DESCRIPTOR Requirement);
/* Local replacements for kernel-only helpers */
BOOLEAN NTAPI PciArbFindSuitableRange(PARBITER_INSTANCE Arbiter, PARBITER_ALLOCATION_STATE State);
NTSTATUS NTAPI PciArbTranslateOrdering(PIO_RESOURCE_DESCRIPTOR OutDesc, PIO_RESOURCE_DESCRIPTOR InDesc);

/* Transaction (allocation lifecycle) callbacks – thin wrappers over generic library */
static NTSTATUS NTAPI PciArbTestAllocation(PARBITER_INSTANCE Arbiter, PLIST_ENTRY List)
{ return ArbTestAllocation(Arbiter, List); }
static NTSTATUS NTAPI PciArbRetestAllocation(PARBITER_INSTANCE Arbiter, PLIST_ENTRY List)
{
    /* Discard previous possible allocation (if any) then re-run test */
    if (Arbiter->PossibleAllocation)
    {
        RtlFreeRangeList(Arbiter->PossibleAllocation);
        Arbiter->PossibleAllocation = ExAllocatePoolWithTag(PagedPool, sizeof(RTL_RANGE_LIST), TAG_ARB_RANGE);
        if (!Arbiter->PossibleAllocation) return STATUS_INSUFFICIENT_RESOURCES;
        RtlInitializeRangeList(Arbiter->PossibleAllocation);
        /* We intentionally don't copy Allocation yet; ArbTestAllocation will */
    }
    /* Ensure allocation stack present (may have been freed on rollback) */
    if (!Arbiter->AllocationStack)
    {
        Arbiter->AllocationStack = ExAllocatePoolWithTag(PagedPool, PAGE_SIZE, TAG_ARB_ALLOCATION);
        if (!Arbiter->AllocationStack) return STATUS_INSUFFICIENT_RESOURCES;
        Arbiter->AllocationStackMaxSize = PAGE_SIZE;
    }
    return ArbTestAllocation(Arbiter, List);
}
static NTSTATUS NTAPI PciArbCommitAllocation(PARBITER_INSTANCE Arbiter)
{ return ArbCommitAllocation(Arbiter); }
static NTSTATUS NTAPI PciArbRollbackAllocation(PARBITER_INSTANCE Arbiter)
{
    /* Abandon any tentative allocation state */
    if (Arbiter->PossibleAllocation)
    {
        RtlFreeRangeList(Arbiter->PossibleAllocation);
        RtlInitializeRangeList(Arbiter->PossibleAllocation); /* leave reusable */
    }
    if (Arbiter->AllocationStack)
    {
        ExFreePoolWithTag(Arbiter->AllocationStack, TAG_ARB_ALLOCATION);
        Arbiter->AllocationStack = ExAllocatePoolWithTag(PagedPool, PAGE_SIZE, TAG_ARB_ALLOCATION);
        if (Arbiter->AllocationStack)
            Arbiter->AllocationStackMaxSize = PAGE_SIZE;
        else
            Arbiter->AllocationStackMaxSize = 0; /* next retest tries again */
    }
    return STATUS_SUCCESS;
}
static NTSTATUS NTAPI PciArbBootAllocation(PARBITER_INSTANCE Arbiter, PLIST_ENTRY List)
{ return ArbBootAllocation(Arbiter, List); }
/* Optional phase wrappers (currently trivial, allow future specialization) */
static NTSTATUS NTAPI PciArbPreprocessEntry(PARBITER_INSTANCE Arbiter, PARBITER_ALLOCATION_STATE State)
{ UNREFERENCED_PARAMETER(Arbiter); UNREFERENCED_PARAMETER(State); return STATUS_SUCCESS; }
static BOOLEAN NTAPI PciArbGetNextRange(PARBITER_INSTANCE Arbiter, PARBITER_ALLOCATION_STATE State)
{ return ArbGetNextAllocationRange(Arbiter, State); }
static VOID NTAPI PciArbAddAllocation(PARBITER_INSTANCE Arbiter, PARBITER_ALLOCATION_STATE State)
{ ArbAddAllocation(Arbiter, State); }
static VOID NTAPI PciArbBacktrackAllocation(PARBITER_INSTANCE Arbiter, PARBITER_ALLOCATION_STATE State)
{ ArbBacktrackAllocation(Arbiter, State); }

/* GLOBALS ********************************************************************/

PCI_INTERFACE ArbiterInterfaceMemory =
{
    &GUID_ARBITER_INTERFACE_STANDARD,
    sizeof(ARBITER_INTERFACE),
    0,
    0,
    PCI_INTERFACE_FDO,
    0,
    PciArb_Memory,
    armem_Constructor,
    armem_Initializer
};

PCI_INTERFACE ArbiterInterfaceIo =
{
    &GUID_ARBITER_INTERFACE_STANDARD,
    sizeof(ARBITER_INTERFACE),
    0,
    0,
    PCI_INTERFACE_FDO,
    0,
    PciArb_Io,
    ario_Constructor,
    ario_Initializer
};

/* FUNCTIONS ******************************************************************/

NTSTATUS
NTAPI
ario_Initializer(IN PVOID Instance)
{
    PPCI_ARBITER_INSTANCE PciArb = (PPCI_ARBITER_INSTANCE)Instance;
    PARBITER_INSTANCE Arb;
    if (!PciArb) return STATUS_INVALID_PARAMETER;
    Arb = &PciArb->CommonInstance;
    RtlZeroMemory(Arb, sizeof(*Arb));
    /* Use local PCI helpers instead of kernel generic routines */
    Arb->UnpackRequirement = PciArbUnpackRequirement;
    Arb->PackResource = PciArbPackResource;
    Arb->UnpackResource = PciArbUnpackResource;
    Arb->ScoreRequirement = PciArbScoreRequirement;
    Arb->FindSuitableRange = PciArbFindSuitableRange;
    Arb->TestAllocation = PciArbTestAllocation;
    Arb->RetestAllocation = PciArbRetestAllocation;
    Arb->CommitAllocation = PciArbCommitAllocation;
    Arb->RollbackAllocation = PciArbRollbackAllocation;
    Arb->BootAllocation = PciArbBootAllocation;
    Arb->PreprocessEntry = PciArbPreprocessEntry;
    Arb->GetNextAllocationRange = PciArbGetNextRange;
    Arb->AddAllocation = PciArbAddAllocation;
    Arb->BacktrackAllocation = PciArbBacktrackAllocation;
    return ArbInitializeArbiterInstance(Arb,
                                        PciArb->BusFdoExtension->FunctionalDeviceObject,
                                        CmResourceTypePort,
                                        PciArb->InstanceName,
                                        L"Pci",
                                        PciArbTranslateOrdering);
}

NTSTATUS
NTAPI
ario_Constructor(IN PVOID DeviceExtension,
                 IN PVOID PciInterface,
                 IN PVOID InterfaceData,
                 IN USHORT Version,
                 IN USHORT Size,
                 IN PINTERFACE Interface)
{
    PPCI_FDO_EXTENSION FdoExtension = (PPCI_FDO_EXTENSION)DeviceExtension;
    NTSTATUS Status;
    PAGED_CODE();

    UNREFERENCED_PARAMETER(PciInterface);
    UNREFERENCED_PARAMETER(Version);
    UNREFERENCED_PARAMETER(Size);
    UNREFERENCED_PARAMETER(Interface);

    /* Make sure it's the expected interface */
    if ((ULONG_PTR)InterfaceData == CmResourceTypePort)
    {
        /* Caller wants the port arbiter interface: we currently rely on PnP's internal path.
           Returning STATUS_NOT_SUPPORTED lets the caller retry (e.g. via root/last chance). */
        Status = STATUS_NOT_SUPPORTED;
    }
    else
    {
        Status = STATUS_INVALID_PARAMETER_5; /* InterfaceData not a port resource type */
    }

    /* Return the status */
    return Status;
}

VOID
NTAPI
ario_ApplyBrokenVideoHack(IN PPCI_FDO_EXTENSION FdoExtension)
{
    PPCI_ARBITER_INSTANCE PciArbiter;
    //PARBITER_INSTANCE CommonInstance;
    //NTSTATUS Status;

    /* Only valid for root FDOs who are being applied the hack for the first time */
    ASSERT(!FdoExtension->BrokenVideoHackApplied);
    ASSERT(PCI_IS_ROOT_FDO(FdoExtension));

    /* Find the I/O arbiter */
    PciArbiter = (PVOID)PciFindNextSecondaryExtension(FdoExtension->
                                                      SecondaryExtension.Next,
                                                      PciArb_Io);
    ASSERT(PciArbiter);
#if 0 // when arb exist
    /* Get the Arb instance */
    CommonInstance = &PciArbiter->CommonInstance;

    /* Free the two lists, enabling full VGA access */
    ArbFreeOrderingList(&CommonInstance->OrderingList);
    ArbFreeOrderingList(&CommonInstance->ReservedList);

    /* Build the ordering for broken video PCI access */
    Status = ArbBuildAssignmentOrdering(CommonInstance,
                                        L"Pci",
                                        L"BrokenVideo",
                                        NULL);
    ASSERT(NT_SUCCESS(Status));
#else
    //Status = STATUS_SUCCESS;
    UNIMPLEMENTED;
    while (TRUE);
#endif
    /* Now the hack has been applied */
    FdoExtension->BrokenVideoHackApplied = TRUE;
}

NTSTATUS
NTAPI
armem_Initializer(IN PVOID Instance)
{
    PPCI_ARBITER_INSTANCE PciArb = (PPCI_ARBITER_INSTANCE)Instance;
    PARBITER_INSTANCE Arb;
    if (!PciArb) return STATUS_INVALID_PARAMETER;
    Arb = &PciArb->CommonInstance;
    RtlZeroMemory(Arb, sizeof(*Arb));
    /* Use local PCI helpers instead of kernel generic routines */
    Arb->UnpackRequirement = PciArbUnpackRequirement;
    Arb->PackResource = PciArbPackResource;
    Arb->UnpackResource = PciArbUnpackResource;
    Arb->ScoreRequirement = PciArbScoreRequirement;
    Arb->FindSuitableRange = PciArbFindSuitableRange;
    Arb->TestAllocation = PciArbTestAllocation;
    Arb->RetestAllocation = PciArbRetestAllocation;
    Arb->CommitAllocation = PciArbCommitAllocation;
    Arb->RollbackAllocation = PciArbRollbackAllocation;
    Arb->BootAllocation = PciArbBootAllocation;
    Arb->PreprocessEntry = PciArbPreprocessEntry;
    Arb->GetNextAllocationRange = PciArbGetNextRange;
    Arb->AddAllocation = PciArbAddAllocation;
    Arb->BacktrackAllocation = PciArbBacktrackAllocation;
    return ArbInitializeArbiterInstance(Arb,
                                        PciArb->BusFdoExtension->FunctionalDeviceObject,
                                        CmResourceTypeMemory,
                                        PciArb->InstanceName,
                                        L"Pci",
                                        PciArbTranslateOrdering);
}

NTSTATUS
NTAPI
armem_Constructor(IN PVOID DeviceExtension,
                  IN PVOID PciInterface,
                  IN PVOID InterfaceData,
                  IN USHORT Version,
                  IN USHORT Size,
                  IN PINTERFACE Interface)
{
    PPCI_FDO_EXTENSION FdoExtension = (PPCI_FDO_EXTENSION)DeviceExtension;
    NTSTATUS Status;
    PAGED_CODE();

    UNREFERENCED_PARAMETER(PciInterface);
    UNREFERENCED_PARAMETER(Version);
    UNREFERENCED_PARAMETER(Size);
    UNREFERENCED_PARAMETER(Interface);

    /* Make sure it's the expected interface */
    if ((ULONG_PTR)InterfaceData == CmResourceTypeMemory)
    {
        Status = STATUS_NOT_SUPPORTED; /* Defer to generic/root provider for now */
    }
    else
    {
        Status = STATUS_INVALID_PARAMETER_5;
    }

    /* Return the status */
    return Status;
}

/* EOF */

/* Helper implementations placed at end to mirror style of other arbiter files */
NTSTATUS NTAPI PciArbUnpackRequirement(PIO_RESOURCE_DESCRIPTOR Requirement,
                                       PULONGLONG MinimumAddress,
                                       PULONGLONG MaximumAddress,
                                       PULONG Length,
                                       PULONG Alignment)
{
    if (!Requirement) return STATUS_INVALID_PARAMETER;
    switch (Requirement->Type)
    {
        case CmResourceTypePort:
            *MinimumAddress = Requirement->u.Port.MinimumAddress.QuadPart;
            *MaximumAddress = Requirement->u.Port.MaximumAddress.QuadPart;
            *Length = Requirement->u.Port.Length;
            *Alignment = Requirement->u.Port.Alignment ? Requirement->u.Port.Alignment : 1;
            return STATUS_SUCCESS;
        case CmResourceTypeMemory:
            *MinimumAddress = Requirement->u.Memory.MinimumAddress.QuadPart;
            *MaximumAddress = Requirement->u.Memory.MaximumAddress.QuadPart;
            *Length = Requirement->u.Memory.Length;
            *Alignment = Requirement->u.Memory.Alignment ? Requirement->u.Memory.Alignment : 1;
            return STATUS_SUCCESS;
        default:
            return STATUS_INVALID_PARAMETER;
    }
}

NTSTATUS NTAPI PciArbPackResource(PIO_RESOURCE_DESCRIPTOR Requirement,
                                  ULONGLONG Start,
                                  PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor)
{
    if (!Requirement || !Descriptor) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(Descriptor, sizeof(*Descriptor));
    if (Requirement->Type == CmResourceTypePort)
    {
        Descriptor->Type = CmResourceTypePort;
        Descriptor->ShareDisposition = Requirement->ShareDisposition;
        Descriptor->Flags = CM_RESOURCE_PORT_IO;
        Descriptor->u.Port.Start.QuadPart = Start;
        Descriptor->u.Port.Length = Requirement->u.Port.Length;
    }
    else if (Requirement->Type == CmResourceTypeMemory)
    {
        Descriptor->Type = CmResourceTypeMemory;
        Descriptor->ShareDisposition = Requirement->ShareDisposition;
        Descriptor->Flags = Requirement->Flags & (CM_RESOURCE_MEMORY_PREFETCHABLE | CM_RESOURCE_MEMORY_READ_ONLY);
        Descriptor->u.Memory.Start.QuadPart = Start;
        Descriptor->u.Memory.Length = Requirement->u.Memory.Length;
    }
    else
    {
        return STATUS_INVALID_PARAMETER;
    }
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI PciArbUnpackResource(PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor,
                                    PULONGLONG Start,
                                    PULONG Length)
{
    if (!Descriptor || !Start || !Length) return STATUS_INVALID_PARAMETER;
    *Start = Descriptor->u.Generic.Start.QuadPart;
    *Length = Descriptor->u.Generic.Length;
    return STATUS_SUCCESS;
}

LONG NTAPI PciArbScoreRequirement(PIO_RESOURCE_DESCRIPTOR Requirement)
{
    ULONGLONG span;
    if (Requirement->Type == CmResourceTypePort)
    {
        span = Requirement->u.Port.MaximumAddress.QuadPart - Requirement->u.Port.MinimumAddress.QuadPart + 1;
    }
    else if (Requirement->Type == CmResourceTypeMemory)
    {
        span = Requirement->u.Memory.MaximumAddress.QuadPart - Requirement->u.Memory.MinimumAddress.QuadPart + 1;
    }
    else
    {
        return -1;
    }
    if (span > LONG_MAX) span = LONG_MAX;
    return (LONG)span;
}

BOOLEAN NTAPI PciArbFindSuitableRange(PARBITER_INSTANCE Arbiter, PARBITER_ALLOCATION_STATE State)
{
    return ArbFindSuitableRange(Arbiter, State);
}

NTSTATUS NTAPI PciArbTranslateOrdering(PIO_RESOURCE_DESCRIPTOR OutDesc, PIO_RESOURCE_DESCRIPTOR InDesc)
{
    if (!OutDesc || !InDesc) return STATUS_INVALID_PARAMETER;
    RtlCopyMemory(OutDesc, InDesc, sizeof(IO_RESOURCE_DESCRIPTOR));
    return STATUS_SUCCESS;
}
