/*
 * PROJECT:         ReactOS PCI Bus Driver
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            drivers/bus/pci/arb/ar_busno.c
 * PURPOSE:         Bus Number Arbitration
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

/* INCLUDES *******************************************************************/

#include <pci.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

NTSTATUS
NTAPI
ArbCommitAllocation(
    _In_ PARBITER_INSTANCE Arbiter);
PCI_INTERFACE ArbiterInterfaceBusNumber =
{
    &GUID_ARBITER_INTERFACE_STANDARD,
    sizeof(ARBITER_INTERFACE),
    0,
    0,
    PCI_INTERFACE_FDO,
    0,
    PciArb_BusNumber,
    arbusno_Constructor,
    arbusno_Initializer
};

/* FUNCTIONS ******************************************************************/


/* LOCAL BUS NUMBER ARBITER HELPERS ******************************************/

static NTSTATUS NTAPI PciBusArbUnpackRequirement(
    PIO_RESOURCE_DESCRIPTOR IoDesc,
    PULONGLONG Min,
    PULONGLONG Max,
    PULONG Len,
    PULONG Align)
{
    if (!IoDesc || IoDesc->Type != CmResourceTypeBusNumber) return STATUS_INVALID_PARAMETER;
    *Min = IoDesc->u.BusNumber.MinBusNumber;
    *Max = IoDesc->u.BusNumber.MaxBusNumber;
    *Len = IoDesc->u.BusNumber.Length;
    *Align = 1; /* bus numbers increment by 1 */
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI PciBusArbPackResource(
    PIO_RESOURCE_DESCRIPTOR IoDesc,
    ULONGLONG Start,
    PCM_PARTIAL_RESOURCE_DESCRIPTOR CmDesc)
{
    if (!IoDesc || !CmDesc || IoDesc->Type != CmResourceTypeBusNumber) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(CmDesc, sizeof(*CmDesc));
    CmDesc->Type = CmResourceTypeBusNumber;
    CmDesc->ShareDisposition = IoDesc->ShareDisposition;
    CmDesc->u.BusNumber.Start = Start;
    CmDesc->u.BusNumber.Length = IoDesc->u.BusNumber.Length;
    CmDesc->u.BusNumber.Reserved = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI PciBusArbUnpackResource(
    PCM_PARTIAL_RESOURCE_DESCRIPTOR CmDesc,
    PULONGLONG Start,
    PULONG Len)
{
    if (!CmDesc || CmDesc->Type != CmResourceTypeBusNumber) return STATUS_INVALID_PARAMETER;
    *Start = CmDesc->u.BusNumber.Start;
    *Len = CmDesc->u.BusNumber.Length;
    return STATUS_SUCCESS;
}

static LONG NTAPI PciBusArbScoreRequirement(
    PIO_RESOURCE_DESCRIPTOR IoDesc)
{
    if (!IoDesc || IoDesc->Type != CmResourceTypeBusNumber) return -1;
    return (LONG)IoDesc->u.BusNumber.Length; /* simple heuristic */
}

/* Transaction callbacks (simple wrappers around generic arbiter library) */
static NTSTATUS NTAPI PciBusArbTestAllocation(PARBITER_INSTANCE Arbiter, PLIST_ENTRY List)
{ return ArbTestAllocation(Arbiter, List); }
static NTSTATUS NTAPI PciBusArbRetestAllocation(PARBITER_INSTANCE Arbiter, PLIST_ENTRY List)
{
    if (Arbiter->PossibleAllocation)
    {
        RtlFreeRangeList(Arbiter->PossibleAllocation);
        Arbiter->PossibleAllocation = ExAllocatePoolWithTag(PagedPool, sizeof(RTL_RANGE_LIST), TAG_ARB_RANGE);
        if (!Arbiter->PossibleAllocation) return STATUS_INSUFFICIENT_RESOURCES;
        RtlInitializeRangeList(Arbiter->PossibleAllocation);
    }
    if (!Arbiter->AllocationStack)
    {
        Arbiter->AllocationStack = ExAllocatePoolWithTag(PagedPool, PAGE_SIZE, TAG_ARB_ALLOCATION);
        if (!Arbiter->AllocationStack) return STATUS_INSUFFICIENT_RESOURCES;
        Arbiter->AllocationStackMaxSize = PAGE_SIZE;
    }
    return ArbTestAllocation(Arbiter, List);
}
static NTSTATUS NTAPI PciBusArbCommitAllocation(PARBITER_INSTANCE Arbiter)
{ return ArbCommitAllocation(Arbiter); }
static NTSTATUS NTAPI PciBusArbRollbackAllocation(PARBITER_INSTANCE Arbiter)
{
    if (Arbiter->PossibleAllocation)
    {
        RtlFreeRangeList(Arbiter->PossibleAllocation);
        RtlInitializeRangeList(Arbiter->PossibleAllocation);
    }
    if (Arbiter->AllocationStack)
    {
        ExFreePoolWithTag(Arbiter->AllocationStack, TAG_ARB_ALLOCATION);
        Arbiter->AllocationStack = ExAllocatePoolWithTag(PagedPool, PAGE_SIZE, TAG_ARB_ALLOCATION);
        if (Arbiter->AllocationStack)
            Arbiter->AllocationStackMaxSize = PAGE_SIZE;
        else
            Arbiter->AllocationStackMaxSize = 0;
    }
    return STATUS_SUCCESS;
}
static NTSTATUS NTAPI PciBusArbBootAllocation(PARBITER_INSTANCE Arbiter, PLIST_ENTRY List)
{ return ArbBootAllocation(Arbiter, List); }

NTSTATUS
NTAPI
arbusno_Initializer(IN PVOID Instance)
{
    PPCI_ARBITER_INSTANCE Arbiter = Instance;
    PPCI_FDO_EXTENSION FdoExtension;
    NTSTATUS Status;

    PAGED_CODE();

    RtlZeroMemory(&Arbiter->CommonInstance, sizeof(Arbiter->CommonInstance));

    FdoExtension = Arbiter->BusFdoExtension;

    /* Minimal bus number arbiter implementation (local helpers) */
    Arbiter->CommonInstance.UnpackRequirement = PciBusArbUnpackRequirement;
    Arbiter->CommonInstance.PackResource = PciBusArbPackResource;
    Arbiter->CommonInstance.UnpackResource = PciBusArbUnpackResource;
    Arbiter->CommonInstance.ScoreRequirement = PciBusArbScoreRequirement;
    Arbiter->CommonInstance.FindSuitableRange = ArbFindSuitableRange;
    Arbiter->CommonInstance.TestAllocation = PciBusArbTestAllocation;
    Arbiter->CommonInstance.RetestAllocation = PciBusArbRetestAllocation;
    Arbiter->CommonInstance.CommitAllocation = PciBusArbCommitAllocation;
    Arbiter->CommonInstance.RollbackAllocation = PciBusArbRollbackAllocation;
    Arbiter->CommonInstance.BootAllocation = PciBusArbBootAllocation;

    Status = ArbInitializeArbiterInstance(&Arbiter->CommonInstance,
                                          FdoExtension->FunctionalDeviceObject,
                                          CmResourceTypeBusNumber,
                                          Arbiter->InstanceName,
                                          L"Pci",
                                          NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("arbusno_Initializer: init arbiter return %X", Status);
    }

    return Status;
}

NTSTATUS
NTAPI
arbusno_Constructor(IN PVOID DeviceExtension,
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
    if ((ULONG_PTR)InterfaceData != CmResourceTypeBusNumber)
    {
        /* Arbiter support must have been initialized first */
        if (FdoExtension->ArbitersInitialized)
        {
            /* Not yet implemented */
            UNIMPLEMENTED;
        }
        else
        {
            /* No arbiters for this FDO */
            Status = STATUS_NOT_SUPPORTED;
        }
    }
    else
    {
        /* Not the right interface */
        Status = STATUS_INVALID_PARAMETER_5;
    }

    /* Return the status */
    return Status;
}

/* EOF */
