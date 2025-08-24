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

/* Arbiter library helpers (not declared in public header) */
extern NTSTATUS NTAPI ArbRetestAllocation(_In_ PARBITER_INSTANCE Arbiter,
                                          _In_ PLIST_ENTRY ArbitrationList);
extern NTSTATUS NTAPI ArbCommitAllocation(_In_ PARBITER_INSTANCE Arbiter);
extern NTSTATUS NTAPI ArbRollbackAllocation(_In_ PARBITER_INSTANCE Arbiter);
extern NTSTATUS NTAPI ArbPreprocessEntry(_In_ PARBITER_INSTANCE Arbiter,
                                         _Inout_ PARBITER_ALLOCATION_STATE ArbState);
extern NTSTATUS NTAPI ArbAllocateEntry(_In_ PARBITER_INSTANCE Arbiter,
                                       _Inout_ PARBITER_ALLOCATION_STATE ArbState);

/* Forward declarations for local handlers */
static NTSTATUS NTAPI arbusno_UnpackRequirement(
    _In_ PIO_RESOURCE_DESCRIPTOR IoDesc,
    _Out_ PULONGLONG Min,
    _Out_ PULONGLONG Max,
    _Out_ PULONG Length,
    _Out_ PULONG Alignment);

static NTSTATUS NTAPI arbusno_PackResource(
    _In_ PIO_RESOURCE_DESCRIPTOR IoDesc,
    _In_ ULONGLONG Start,
    _Out_ PCM_PARTIAL_RESOURCE_DESCRIPTOR CmDesc);

static NTSTATUS NTAPI arbusno_UnpackResource(
    _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR CmDesc,
    _Out_ PULONGLONG Start,
    _Out_ PULONG Length);

static LONG NTAPI arbusno_ScoreRequirement(_In_ PIO_RESOURCE_DESCRIPTOR IoDesc);

NTSTATUS
NTAPI
arbusno_Initializer(IN PVOID Instance)
{
    PPCI_ARBITER_INSTANCE Arbiter = Instance;
    PPCI_FDO_EXTENSION FdoExtension = Arbiter->BusFdoExtension;

    PAGED_CODE();

    RtlZeroMemory(&Arbiter->CommonInstance, sizeof(Arbiter->CommonInstance));

    /* Minimal bus-number arbiter: unpack/pack treat values as 32-bit */
    Arbiter->CommonInstance.UnpackRequirement = arbusno_UnpackRequirement;
    Arbiter->CommonInstance.PackResource = arbusno_PackResource;
    Arbiter->CommonInstance.UnpackResource = arbusno_UnpackResource;
    Arbiter->CommonInstance.ScoreRequirement = arbusno_ScoreRequirement;
    Arbiter->CommonInstance.TestAllocation = ArbTestAllocation;
    Arbiter->CommonInstance.RetestAllocation = ArbRetestAllocation;
    Arbiter->CommonInstance.CommitAllocation = ArbCommitAllocation;
    Arbiter->CommonInstance.RollbackAllocation = ArbRollbackAllocation;
    Arbiter->CommonInstance.BootAllocation = ArbBootAllocation;
    Arbiter->CommonInstance.StartArbiter = NULL;
    Arbiter->CommonInstance.PreprocessEntry = ArbPreprocessEntry;
    Arbiter->CommonInstance.AllocateEntry = ArbAllocateEntry;
    Arbiter->CommonInstance.GetNextAllocationRange = ArbGetNextAllocationRange;
    Arbiter->CommonInstance.FindSuitableRange = ArbFindSuitableRange;
    Arbiter->CommonInstance.AddAllocation = ArbAddAllocation;
    Arbiter->CommonInstance.BacktrackAllocation = ArbBacktrackAllocation;
    Arbiter->CommonInstance.OverrideConflict = NULL;

    return ArbInitializeArbiterInstance(&Arbiter->CommonInstance,
                                        FdoExtension->FunctionalDeviceObject,
                                        CmResourceTypeBusNumber,
                                        Arbiter->InstanceName,
                                        L"Pci",
                                        NULL);
}

static NTSTATUS NTAPI arbusno_UnpackRequirement(
    _In_ PIO_RESOURCE_DESCRIPTOR IoDesc,
    _Out_ PULONGLONG Min,
    _Out_ PULONGLONG Max,
    _Out_ PULONG Length,
    _Out_ PULONG Alignment)
{
    *Length = IoDesc->u.BusNumber.Length;
    *Alignment = 1;
    *Min = IoDesc->u.BusNumber.MinBusNumber;
    *Max = IoDesc->u.BusNumber.MaxBusNumber;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI arbusno_PackResource(
    _In_ PIO_RESOURCE_DESCRIPTOR IoDesc,
    _In_ ULONGLONG Start,
    _Out_ PCM_PARTIAL_RESOURCE_DESCRIPTOR CmDesc)
{
    RtlZeroMemory(CmDesc, sizeof(*CmDesc));
    CmDesc->Type = CmResourceTypeBusNumber;
    CmDesc->ShareDisposition = IoDesc->ShareDisposition;
    CmDesc->u.BusNumber.Start = (ULONG)Start;
    CmDesc->u.BusNumber.Length = IoDesc->u.BusNumber.Length;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI arbusno_UnpackResource(
    _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR CmDesc,
    _Out_ PULONGLONG Start,
    _Out_ PULONG Length)
{
    *Start = CmDesc->u.BusNumber.Start;
    *Length = CmDesc->u.BusNumber.Length;
    return STATUS_SUCCESS;
}

static LONG NTAPI arbusno_ScoreRequirement(_In_ PIO_RESOURCE_DESCRIPTOR IoDesc)
{
    UNREFERENCED_PARAMETER(IoDesc);
    return 0;
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
            while (TRUE);
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
